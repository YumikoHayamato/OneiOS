// SPDX-License-Identifier: MIT
// Haptics bridge: iOS vibration requests onto the Galaxy S24 Ultra's actuator.
//
// The actuator is an LRA driven by a Cirrus CS40L26A (I2C 0x40) under Samsung's sec_vibrator_inputff:
// /dev/input/event8 "sec_vibrator_inputff", EV_FF bits FF_PERIODIC, FF_SINE, FF_CUSTOM, FF_GAIN and
// no FF_RUMBLE (read on the owner's phone; docs/hardware.md). The driver's FF_SINE path programs the
// chip's buzz generator: period 4..10 ms (100..250 Hz), magnitude 0..255, length in ms
// (cs40l26_sine_upload, drivers/vibrator/cs/cs40l26/cs40l26.c in Samsung's kernel source). FF_GAIN
// takes 0..10000 (Samsung's intensity scale, gain / 100 = percent).
//
// One effect is kept and updated for every request. A worker thread does the ioctls (each is I2C
// traffic), so a request from a vCPU never waits; the newest request wins.
//
// Two backends (bridges/hw_backend.h, hw-backend-haptics): samsung, the evdev device above (unchanged); treble, the
// vibrator HAL android.hardware.vibrator.IVibrator/default through servicemanager: on(ms), setAmplitude (when the HAL
// has amplitude control), off(). The same worker, mapping and counters drive both.
#define _GNU_SOURCE
#include "bridges/bridges.h"
#include "bridges/hw_backend.h"
#include "../hal/hal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

#define PERIOD_MIN_MS 4u        // CS40L26_BUZZGEN_PER_MIN
#define PERIOD_MAX_MS 10u       // CS40L26_BUZZGEN_PER_MAX
#define RESONANCE_MS  6u        // 166 Hz: a guess at the LRA's resonance until f0 is read on the phone
                                // (attribute f0_measured of the class sec_vib_inputff); whole milliseconds only
#define LEVEL_MAX     255u      // CS40L26_BUZZGEN_LEVEL_MAX
#define LEVEL_FLOOR   40u       // below this an LRA does not move enough to feel
#define LENGTH_MAX_MS 10000u

void br_haptic_map(unsigned intensity, unsigned frequency_hz, unsigned *period_ms, unsigned *magnitude)
{
  if (intensity > 1000) intensity = 1000;
  unsigned period = frequency_hz ? (1000 + frequency_hz / 2) / frequency_hz : RESONANCE_MS;
  if (period < PERIOD_MIN_MS) period = PERIOD_MIN_MS;
  if (period > PERIOD_MAX_MS) period = PERIOD_MAX_MS;
  *period_ms = period;
  // Any intensity above zero is felt: the floor up to full scale, linear.
  *magnitude = intensity ? LEVEL_FLOOR + (LEVEL_MAX - LEVEL_FLOOR) * intensity / 1000 : 0;
}

enum { HREQ_VIB = 1, HREQ_STOP = 2, HREQ_IMPACT = 3 };
static uint64_t haptic_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

struct br_haptic {
  const br_haptic_backend *backend;
  void *ctx;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t wake, idle;
  bool quit, pending, busy;
  uint32_t on_ms;
  unsigned intensity, frequency_hz, impact_kind, request, active_kind;
  bool impact_enabled;             // HAL preset inventory + explicit opt-in
  uint64_t impact_until_ms;        // bound off(): never cancel an already-finished native tick
  int effect;                 // -1 until the first upload
  bool playing;
  unsigned failures;
  br_haptic_stats stats;      // the counter line (br_haptic_report)
  char name[300];             // what plays: the device node, or "the stand-in actuator"
};

static void *worker(void *arg)
{
  br_haptic *h = arg;
  pthread_mutex_lock(&h->lock);
  for (;;) {
    while (!h->pending && !h->quit) {
      h->busy = false;
      pthread_cond_broadcast(&h->idle);
      pthread_cond_wait(&h->wake, &h->lock);
    }
    if (h->quit && !h->pending) break;
    uint32_t on_ms = h->on_ms;
    unsigned intensity = h->intensity, frequency = h->frequency_hz;
    unsigned request = h->request, kind = h->impact_kind;
    h->pending = false;
    h->busy = true;
    pthread_mutex_unlock(&h->lock);

    unsigned period = 0, magnitude = 0, duration = 0;
    br_haptic_map(intensity, frequency, &period, &magnitude);
    int rc = 0;
    const char *step = "stop";
    int effect_id = h->effect;
    bool played = false, stopped = false;
    // HAL preset effects finish by themselves. A later guest STOP must never
    // issue off() after the vendor's reported duration, because One UI could
    // already be playing a different vibration.
    if (h->playing && h->active_kind == HREQ_IMPACT &&
        haptic_ms() >= h->impact_until_ms) {
      h->playing = false;
      h->active_kind = 0;
    }
    if (request == HREQ_STOP || (request == HREQ_VIB && (!on_ms || !magnitude))) {
      if (h->playing) {
        rc = h->backend->play(h->ctx, h->effect < 0 ? 0 : h->effect, false);
        stopped = rc == 0;
      }
      h->playing = false;
      h->active_kind = 0;
    } else if (request == HREQ_IMPACT) {
      step = "native impact";
      if (!h->impact_enabled || !h->backend->impact ||
          kind < BR_HAPTIC_IMPACT_LIGHT || kind > BR_HAPTIC_IMPACT_HEAVY) rc = -EOPNOTSUPP;
      // A continuous buzz is explicitly stopped before the new discrete tap.
      if (!rc && h->playing && h->active_kind == HREQ_VIB) {
        rc = h->backend->play(h->ctx, h->effect < 0 ? 0 : h->effect, false);
        stopped = rc == 0;
        if (!rc) { h->playing = false; h->active_kind = 0; }
      }
      if (!rc) {
        rc = h->backend->impact(h->ctx, kind, &duration);
        if (!rc && (duration < 1 || duration > 1000)) rc = -EPROTO;
        if (!rc) {
          h->playing = played = true;
          h->active_kind = HREQ_IMPACT;
          h->impact_until_ms = haptic_ms() + duration;
        }
      }
    } else {
      step = "effect upload";
      // Switching from a currently running preset back to a continuous
      // waveform cancels that preset; otherwise no spurious off() call.
      if (h->playing && h->active_kind == HREQ_IMPACT) {
        rc = h->backend->play(h->ctx, h->effect < 0 ? 0 : h->effect, false);
        stopped = rc == 0;
        if (!rc) { h->playing = false; h->active_kind = 0; }
      }
      if (!rc) {
        int id = h->backend->upload(h->ctx, h->effect,
                                   on_ms > LENGTH_MAX_MS ? LENGTH_MAX_MS : on_ms, period, magnitude);
        if (id < 0) rc = id;
        else {
          h->effect = effect_id = id;
          step = "effect play";
          rc = h->backend->play(h->ctx, id, true);
          h->playing = played = rc == 0;
          h->active_kind = played ? HREQ_VIB : 0;
        }
      }
    }
    if (rc < 0 && h->failures++ < 8)
      br_log(0, "haptics: %s failed: %s (%u ms, intensity %u, impact %u, period %u ms, level %u, effect id %d)",
             step, strerror(-rc), on_ms, intensity, kind, period, magnitude, effect_id);
    pthread_mutex_lock(&h->lock);
    if (played) h->stats.played++;
    if (stopped) h->stats.stopped++;
    if (rc < 0) h->stats.failed++;
    uint64_t n = h->stats.played;
    pthread_mutex_unlock(&h->lock);
    if (played && (n <= 8 || !(n & (n - 1)))) {
      if (request == HREQ_IMPACT)
        br_log(1, "haptics: native impact %u played on %s: HAL duration %u ms (effect %llu)",
               kind, h->name, duration, (unsigned long long)n);
      else
        br_log(1, "haptics: effect %llu played on %s: %u ms, period %u ms, level %u (intensity %u, %u Hz asked)",
               (unsigned long long)n, h->name, on_ms > LENGTH_MAX_MS ? LENGTH_MAX_MS : on_ms,
               period, magnitude, intensity, frequency);
    }
    if (played && n > 8 && !(n & (n - 1))) br_haptic_report(h);
    pthread_mutex_lock(&h->lock);
  }
  h->busy = false;
  pthread_cond_broadcast(&h->idle);
  pthread_mutex_unlock(&h->lock);
  return NULL;
}

br_haptic *br_haptic_open_backend(const br_haptic_backend *backend, void *ctx)
{
  br_haptic *h = calloc(1, sizeof *h);
  if (!h) return NULL;
  h->backend = backend;
  h->ctx = ctx;
  h->effect = -1;
  // Custom/fake backends can opt in explicitly. The Treble backend overrides
  // this after querying getSupportedEffects() on the real vendor HAL.
  const char *opt = getenv("ONEIOS_HAPTIC_NATIVE_IMPACTS");
  h->impact_enabled = backend && backend->impact && opt && !strcmp(opt, "1");
  snprintf(h->name, sizeof h->name, "the back end");
  pthread_mutex_init(&h->lock, NULL);
  pthread_cond_init(&h->wake, NULL);
  pthread_cond_init(&h->idle, NULL);
  if (backend->gain) {
    int rc = backend->gain(ctx, 10000);
    if (rc < 0) br_log(0, "haptics: initial gain setting failed: %s", strerror(-rc));
  }
  if (pthread_create(&h->thread, NULL, worker, h)) { free(h); return NULL; }
  return h;
}

void br_haptic_stats_get(br_haptic *h, br_haptic_stats *out)
{
  memset(out, 0, sizeof *out);
  if (!h) return;
  pthread_mutex_lock(&h->lock);
  *out = h->stats;
  pthread_mutex_unlock(&h->lock);
}

void br_haptic_report(br_haptic *h)
{
  br_haptic_stats s;
  if (!h) return;
  br_haptic_stats_get(h, &s);
  br_log(1, "haptics: counters: %llu requests from the guest (%llu vibrate, %llu stop; %llu replaced before they started), "
         "%llu effects played, %llu stopped, %llu failed on %s (%llu discrete impacts)", (unsigned long long)(s.vib + s.stop + s.impacts), (unsigned long long)s.vib,
         (unsigned long long)s.stop, (unsigned long long)s.replaced, (unsigned long long)s.played, (unsigned long long)s.stopped,
         (unsigned long long)s.failed, h->name, (unsigned long long)s.impacts);
}

// ---- the stand-in actuator (haptics=sim: a PC boot, which has no vibrator) --------------------------------
// It takes every upload and play like the evdev device and keeps nothing but the counters, so the guest's whole
// path (its Actuator device, oneios_haptic.c, the bridge channel) runs and the log shows what would have played.
static int sim_upload(void *ctx, int id, unsigned length_ms, unsigned period_ms, unsigned magnitude)
{
  (void)ctx; (void)length_ms; (void)period_ms; (void)magnitude;
  return id < 0 ? 0 : id;
}
static int sim_play(void *ctx, int id, bool on) { (void)ctx; (void)id; (void)on; return 0; }
static const br_haptic_backend sim_backend = { .upload = sim_upload, .play = sim_play };

br_haptic *br_haptic_open_sim(void)
{
  br_haptic *h = br_haptic_open_backend(&sim_backend, NULL);
  if (!h) return NULL;
  snprintf(h->name, sizeof h->name, "the stand-in actuator");
  br_log(1, "haptics: on: a stand-in actuator (haptics=sim): requests are counted and logged, nothing vibrates");
  return h;
}

// take24: ONEIOS_HAPTIC_MIN_INTENSITY=N (1..1000, unset or 0: every request is played, as before). A vibrate request
// weaker than N is not played. Why: iOS's own UI feedback (UIKit's selection and slider ticks: Control Center's pages, each
// step of the volume keys) reaches the guest's Actuator as samples of about 12 % of full scale, and the map above lifts
// any intensity to the LRA's floor, so a tick an iPhone barely gives is a clear 20 ms buzz here (take23: 66 of them).
static unsigned haptic_min_intensity(void)
{
  static int min = -1;
  if (min < 0) {
    const char *e = getenv("ONEIOS_HAPTIC_MIN_INTENSITY");
    long v = e && *e ? strtol(e, NULL, 10) : 0;
    min = v < 0 ? 0 : v > 1000 ? 1000 : (int)v;
    if (min) br_log(1, "haptics: requests weaker than %d of 1000 are not played (ONEIOS_HAPTIC_MIN_INTENSITY)", min);
  }
  return (unsigned)min;
}

void br_haptic_vib(br_haptic *h, uint32_t on_ms, unsigned intensity, unsigned frequency_hz)
{
  if (!h) return;
  const unsigned min = haptic_min_intensity();
  if (on_ms && intensity && intensity < min) {
    static unsigned long long skipped;                 // guarded by the lock below
    pthread_mutex_lock(&h->lock);
    h->stats.vib++;
    const unsigned long long n = ++skipped;
    pthread_mutex_unlock(&h->lock);
    if ((n & (n - 1)) == 0)
      br_log(1, "haptics: %llu request(s) under the floor not played (the last: %u ms, intensity %u of 1000, floor %u)", n, on_ms, intensity, min);
    return;
  }
  pthread_mutex_lock(&h->lock);
  if (h->pending) h->stats.replaced++;     // the newest wins: the one not started is never played
  if (on_ms) h->stats.vib++;
  else h->stats.stop++;
  h->on_ms = on_ms; h->intensity = intensity; h->frequency_hz = frequency_hz;
  h->request = on_ms && intensity ? HREQ_VIB : HREQ_STOP;
  h->pending = true;
  pthread_cond_signal(&h->wake);
  pthread_mutex_unlock(&h->lock);
}

bool br_haptic_impacts_available(br_haptic *h)
{
  return h && h->impact_enabled && h->backend && h->backend->impact;
}

int br_haptic_impact(br_haptic *h, unsigned kind)
{
  if (!br_haptic_impacts_available(h) || kind < BR_HAPTIC_IMPACT_LIGHT ||
      kind > BR_HAPTIC_IMPACT_HEAVY) return -EOPNOTSUPP;
  pthread_mutex_lock(&h->lock);
  if (h->pending) h->stats.replaced++;
  h->stats.impacts++;
  h->impact_kind = kind;
  h->on_ms = 0;
  h->request = HREQ_IMPACT;
  h->pending = true;
  pthread_cond_signal(&h->wake);
  pthread_mutex_unlock(&h->lock);
  return 0;
}

void br_haptic_stop(br_haptic *h) { br_haptic_vib(h, 0, 0, 0); }

void br_haptic_flush(br_haptic *h)
{
  if (!h) return;
  pthread_mutex_lock(&h->lock);
  while (h->pending || h->busy) pthread_cond_wait(&h->idle, &h->lock);
  pthread_mutex_unlock(&h->lock);
}

void br_haptic_close(br_haptic *h)
{
  if (!h) return;
  br_haptic_report(h);                     // the session's figures, before the closing stop counts as one
  br_haptic_stop(h);                       // the motor never outlives the bridge
  pthread_mutex_lock(&h->lock);
  h->quit = true;
  pthread_cond_signal(&h->wake);
  pthread_mutex_unlock(&h->lock);
  pthread_join(h->thread, NULL);
  if (h->backend->close) h->backend->close(h->ctx);
  free(h);
}

// ---- the evdev force-feedback back end ----------------------------------------------------------
#ifdef __linux__
#define BIT_AT(bits, n) ((bits)[(n) / 8] >> ((n) % 8) & 1)

static bool has_periodic(int fd)
{
  unsigned char bits[FF_MAX / 8 + 1] = { 0 };
  if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof bits), bits) < 0) return false;
  return BIT_AT(bits, FF_PERIODIC) && BIT_AT(bits, FF_SINE);
}

int br_haptic_find(const char *dev_dir, char *path, size_t size)
{
  if (!dev_dir) dev_dir = "/dev/input";
  DIR *d = opendir(dev_dir);
  if (!d) return -1;
  int found = -1;
  char best[300] = "";
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "event", 5)) continue;
    char node[300], name[80] = "";
    snprintf(node, sizeof node, "%s/%s", dev_dir, e->d_name);
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    if (has_periodic(fd)) {
      (void)ioctl(fd, EVIOCGNAME(sizeof name), name);
      // Samsung's wrapper first; any other periodic device otherwise.
      if (!strcmp(name, "sec_vibrator_inputff") || found < 0) { snprintf(best, sizeof best, "%s", node); found = 0; }
      if (!strcmp(name, "sec_vibrator_inputff")) { close(fd); break; }
    }
    close(fd);
  }
  closedir(d);
  if (found == 0) snprintf(path, size, "%s", best);
  return found;
}

static int ff_upload(void *ctx, int id, unsigned length_ms, unsigned period_ms, unsigned magnitude)
{
  int fd = (int)(intptr_t)ctx;
  struct ff_effect effect;
  memset(&effect, 0, sizeof effect);
  effect.type = FF_PERIODIC;
  effect.id = (short)id;
  effect.replay.length = (unsigned short)length_ms;
  effect.u.periodic.waveform = FF_SINE;
  effect.u.periodic.period = (unsigned short)period_ms;
  effect.u.periodic.magnitude = (short)magnitude;
  if (ioctl(fd, EVIOCSFF, &effect) < 0) return -errno;
  return effect.id;
}

static int ff_event(int fd, unsigned code, int value)
{
  struct input_event event;
  memset(&event, 0, sizeof event);
  event.type = EV_FF;
  event.code = (unsigned short)code;
  event.value = value;
  return write(fd, &event, sizeof event) == (ssize_t)sizeof event ? 0 : -errno;
}

static int ff_play(void *ctx, int id, bool on) { return ff_event((int)(intptr_t)ctx, (unsigned)id, on); }
static int ff_gain(void *ctx, unsigned gain) { return ff_event((int)(intptr_t)ctx, FF_GAIN, (int)gain); }
static void ff_close(void *ctx) { close((int)(intptr_t)ctx); }      // closing erases the file's effects

static const br_haptic_backend evdev_backend = { .upload = ff_upload, .play = ff_play, .gain = ff_gain, .close = ff_close };

static br_haptic *open_evdev(const char *dev_dir)
{
  char node[300];
  if (br_haptic_find(dev_dir, node, sizeof node)) { br_log(0, "haptics: off: no force-feedback device with FF_PERIODIC + FF_SINE under %s", dev_dir ? dev_dir : "/dev/input"); return NULL; }
  int fd = open(node, O_RDWR | O_CLOEXEC);
  if (fd < 0) { br_log(0, "haptics: off: %s: %s", node, strerror(errno)); return NULL; }
  br_haptic *h = br_haptic_open_backend(&evdev_backend, (void *)(intptr_t)fd);
  if (!h) { close(fd); return NULL; }
  snprintf(h->name, sizeof h->name, "%s", node);
  br_log(1, "haptics: on: %s (sine effects, period %u..%u ms, level %u..%u)", node, PERIOD_MIN_MS, PERIOD_MAX_MS, LEVEL_FLOOR, LEVEL_MAX);
  return h;
}
#else
int br_haptic_find(const char *dev_dir, char *path, size_t size) { (void)dev_dir; (void)path; (void)size; return -1; }
static br_haptic *open_evdev(const char *dev_dir) { (void)dev_dir; return NULL; }
#endif

// ---- the vibrator HAL back end (treble) ----------------------------------------------------------
// The effect is a length and a level: on(length) then, with amplitude control, setAmplitude(level / 255 x gain), as
// AOSP's vibrator service orders them. The period has no place in IVibrator's on(): the HAL plays at its own resonance.
typedef struct {
  br_hal *hal;
  int32_t caps;
  unsigned length_ms, magnitude, gain;
  uint32_t preset_mask;            // validated at open, only with explicit opt-in
  bool native_requested, native_enabled, continuous; // opt-in inspection must never emit an unused off()
} vib_ctx;

static int vib_simple(vib_ctx *v, uint32_t code)
{
  ab_parcel p;
  br_hal_parcel(&p, HAL_VIBRATOR);
  return br_hal_call(v->hal, code, &p, NULL, NULL);
}
static int vib_upload(void *ctx, int id, unsigned length_ms, unsigned period_ms, unsigned magnitude)
{
  vib_ctx *v = ctx;
  (void)id; (void)period_ms;
  v->length_ms = length_ms;
  v->magnitude = magnitude;
  return 0;
}
static int vib_play(void *ctx, int id, bool on)
{
  vib_ctx *v = ctx;
  (void)id;
  if (!on) {
    int rc = vib_simple(v, HAL_VIB_OFF);
    if (!rc) v->continuous = false;
    return rc;
  }
  ab_parcel p;
  br_hal_parcel(&p, HAL_VIBRATOR);
  ab_put_i32(&p, (int32_t)v->length_ms);
  ab_put_null_binder(&p);                    // no callback: the worker does not wait for the end
  int rc = br_hal_call(v->hal, HAL_VIB_ON, &p, NULL, NULL);
  if (!rc) v->continuous = true;  // even if a later setAmplitude() fails
  if (rc || !(v->caps & HAL_VIB_CAP_AMPLITUDE)) return rc;
  float amplitude = (float)v->magnitude / (float)LEVEL_MAX * (float)v->gain / 10000.0f;
  if (amplitude > 1.0f) amplitude = 1.0f;
  if (amplitude < 0.01f) amplitude = 0.01f;   // the HAL refuses 0; the level floor keeps it above this anyway
  br_hal_parcel(&p, HAL_VIBRATOR);
  ab_put_float(&p, amplitude);
  return br_hal_call(v->hal, HAL_VIB_SET_AMPLITUDE, &p, NULL, NULL);
}
static int vib_take_i32(void *ctx, ab_reader *r)
{
  *(int32_t *)ctx = ab_get_i32(r);
  return r->bad ? -EPROTO : 0;
}

static int vib_take_effects(void *ctx, ab_reader *r)
{
  uint32_t *mask = ctx;
  int32_t n = ab_get_i32(r);
  if (r->bad || n < 0 || n > 32) return -EPROTO;
  for (int32_t i = 0; i < n; ++i) {
    int32_t e = ab_get_i32(r);
    if (e >= 0 && e < 32) *mask |= 1u << e;
  }
  return r->bad ? -EPROTO : 0;
}

// Explicit one-shot impact: the calibrated vendor preset is separate from
// the legacy duration/frequency path. No EVIOCSFF or Samsung trigger indices.
static int vib_impact(void *ctx, unsigned kind, unsigned *duration_ms)
{
  vib_ctx *v = ctx;
  if (!v->native_enabled) return -EOPNOTSUPP;
  int32_t effect, strength;
  switch (kind) {
  case BR_HAPTIC_IMPACT_LIGHT:  effect = HAL_VIB_EFFECT_TICK; strength = HAL_VIB_STRENGTH_LIGHT; break;
  case BR_HAPTIC_IMPACT_MEDIUM: effect = HAL_VIB_EFFECT_CLICK; strength = HAL_VIB_STRENGTH_MEDIUM; break;
  case BR_HAPTIC_IMPACT_HEAVY:  effect = HAL_VIB_EFFECT_HEAVY_CLICK; strength = HAL_VIB_STRENGTH_STRONG; break;
  default: return -EINVAL;
  }
  if (!(v->preset_mask & (1u << effect))) return -EOPNOTSUPP;
  ab_parcel p;
  br_hal_parcel(&p, HAL_VIBRATOR);
  ab_put_i32(&p, effect);
  ab_put_i32(&p, strength);
  ab_put_null_binder(&p);
  int32_t duration = -1;
  int rc = br_hal_call(v->hal, HAL_VIB_PERFORM, &p, vib_take_i32, &duration);
  if (rc) return rc;
  if (duration < 1 || duration > 1000) return -EPROTO;
  *duration_ms = (unsigned)duration;
  return 0;
}
static int vib_gain(void *ctx, unsigned gain) { ((vib_ctx *)ctx)->gain = gain > 10000 ? 10000 : gain; return 0; }
static void vib_close(void *ctx)
{
  vib_ctx *v = ctx;
  // Keep legacy shutdown behavior when native mode was not requested.
  // During explicit native opt-in, closing an unused or completed bridge
  // must NOT send off(): One UI could currently own the same actuator.
  // A continuous on(ms) started by this bridge still needs to be stopped.
  if (!v->native_requested || v->continuous) (void)vib_simple(v, HAL_VIB_OFF);
  br_hal_close(v->hal);
  free(v);
}
static const br_haptic_backend hal_backend = {
  .upload = vib_upload, .play = vib_play, .gain = vib_gain, .impact = vib_impact, .close = vib_close
};

static int take_caps(void *ctx, ab_reader *r) { *(int32_t *)ctx = ab_get_i32(r); return r->bad ? -EPROTO : 0; }

// NULL with why filled when the HAL is not there.
static br_haptic *open_hal(const char *binder, char *why, size_t size)
{
  vib_ctx *v = calloc(1, sizeof *v);
  if (!v) { snprintf(why, size, "no memory"); return NULL; }
  v->gain = 10000;
  v->hal = br_hal_open(binder, HAL_VIBRATOR_INSTANCE, HAL_VIBRATOR, why, size);
  if (!v->hal) { free(v); return NULL; }
  ab_parcel p;
  br_hal_parcel(&p, HAL_VIBRATOR);
  int rc = br_hal_call(v->hal, HAL_VIB_GET_CAPABILITIES, &p, take_caps, &v->caps);
  if (rc) { snprintf(why, size, "%s: getCapabilities: %s", HAL_VIBRATOR_INSTANCE, br_hal_error(rc)); br_hal_close(v->hal); free(v); return NULL; }
  int32_t version = br_hal_version(v->hal);
  const char *opt = getenv("ONEIOS_HAPTIC_NATIVE_IMPACTS");
  v->native_requested = opt && !strcmp(opt, "1");
  if (v->native_requested) {
    uint32_t mask = 0;
    br_hal_parcel(&p, HAL_VIBRATOR);
    rc = br_hal_call(v->hal, HAL_VIB_SUPPORTED_EFFECTS, &p, vib_take_effects, &mask);
    const uint32_t required = (1u << HAL_VIB_EFFECT_TICK) |
                              (1u << HAL_VIB_EFFECT_CLICK) |
                              (1u << HAL_VIB_EFFECT_HEAVY_CLICK);
    if (!rc && (mask & required) == required) {
      v->preset_mask = mask;
      v->native_enabled = true;
    } else {
      br_log(0, "haptics: native impacts unavailable: %s (supported preset mask %#x), continuous path unchanged",
             rc ? br_hal_error(rc) : "not all TICK/CLICK/HEAVY_CLICK presets advertised", mask);
    }
  }
  br_haptic *h = br_haptic_open_backend(&hal_backend, v);
  if (!h) { br_hal_close(v->hal); free(v); snprintf(why, size, "no worker thread"); return NULL; }
  h->impact_enabled = v->native_enabled;
  snprintf(h->name, sizeof h->name, "the vibrator HAL");
  if (h->impact_enabled)
    br_log(1, "haptics: native impacts enabled (explicit opt-in; TICK/CLICK/HEAVY_CLICK preflight PASS)");
  br_log(1, "haptics: on: the vibrator HAL %s (version %d, capabilities 0x%x: %s; level %u..%u as the amplitude)", HAL_VIBRATOR_INSTANCE, version,
         (unsigned)v->caps, v->caps & HAL_VIB_CAP_AMPLITUDE ? "amplitude control" : "no amplitude control, every effect at the HAL's own strength", LEVEL_FLOOR, LEVEL_MAX);
  return h;
}

br_haptic *br_haptic_open_as(const char *dev_dir, int backend, const char *binder)
{
  if (br_hw_select(BR_HW_HAPTICS, backend) == BR_HW_TREBLE) {
    char why[256];
    br_haptic *h = open_hal(binder, why, sizeof why);
    if (h || !br_hw_missing(BR_HW_HAPTICS, why)) return h;
  }
  return open_evdev(dev_dir);
}

br_haptic *br_haptic_open(const char *dev_dir) { return br_haptic_open_as(dev_dir, BR_HW_UNSET, NULL); }
