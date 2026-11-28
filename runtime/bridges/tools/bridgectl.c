// SPDX-License-Identifier: MIT
// bridgectl: tries each host bridge on the phone, without xnuhost or a guest. Run as root (su):
//   bridgectl cpu [NVCPU]            the host's cores and the vCPU plan made from them
//   bridgectl haptic-inspect              O_RDONLY evdev queries only: name, sine/periodic/gain bits, max effects
//   bridgectl haptic-hal-inspect          read-only AIDL getCapabilities/version: never on, off or amplitude
//   bridgectl haptic-hal-primitives       read-only AIDL composition/effect support and primitive durations
//   bridgectl haptic-native-inspect       passive, opt-in production Treble impact preflight; never actuate
//   bridgectl haptic-hal-click            ACTIVE one-shot IVibrator.perform(CLICK, MEDIUM); explicit opt-in required
//   bridgectl haptic-hal-heavy-click      ACTIVE one-shot IVibrator.perform(HEAVY_CLICK, STRONG); explicit opt-in required
//   bridgectl haptic [MS [INTENSITY [HZ]]]   one vibration (default 200 ms, 1000, resonance)
//   bridgectl haptic-pattern         three taps and a ramp: what an iOS pattern feels like
//   bridgectl serve /SOCKET [--wait S] [--no-haptics] [--no-battery]
//                                    the bridge channel of a VM of crosvm (the native path): listens on the Unix socket
//                                    crosvm's second serial port connects to (vm-boot) and serves the kernel's bridge
//                                    driver there: the phone's battery to iOS, iOS's haptics to the phone's motor;
//                                    until crosvm closes it, SIGTERM, or S seconds (120) without a connection
// The backend: ONEIOS_HW_BACKEND, ONEIOS_HW_BACKEND_<BRIDGE>, ONEIOS_HW_FALLBACK (bridges/hw_backend.h): haptic's is
// the evdev device (samsung) or the vibrator HAL (treble). Off the phone: ONEIOS_INPUT_DIR, ONEIOS_BINDER.
//   bridgectl bt                     opens the Bluetooth HAL, sends HCI Reset and Read BD_ADDR, prints the events
//   bridgectl net                    opens the tap, answers the first DHCP request it sees, counts frames for 10 s
//   bridgectl audio HELPER [S [record]]  a quiet 440 Hz tone for S s (2) through the helper (path of oneios-audio),
//                                    then 0.5 s later a 660 Hz one for 0.5 s;
//                                    with record, then 2 s of the microphone (frames, peak, RMS); first the helper's
//                                    probe (the speaker's latency xnuhost would publish to iOS)
//   bridgectl audio-loop HELPER [N]  N clicks (10), 0.5 s apart, each 20 ms of 1 kHz at -30 dBFS, while the microphone
//                                    records: the round trip of each (handed to the helper to heard by the microphone:
//                                    the speaker's latency, the air and the microphone's), and their median
// Each prints what it found and ends with "ok" or the reason it failed.
#define _GNU_SOURCE
#include "bridges/bridges.h"
#include "bridges/hw_backend.h"
#include "bridges/stream.h"
#include "../hal/hal.h"
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int cpu(int argc, char **argv)
{
  br_cpu_topology t;
  br_cpu_plan plan;
  unsigned nvcpu = argc > 2 ? (unsigned)atoi(argv[2]) : 8;
  if (br_cpu_read("/sys", &t)) { printf("no CPUs under /sys/devices/system/cpu\n"); return 1; }
  printf("host: %u CPUs, %u P, %u E\n", t.ncpu, t.n_p, t.n_e);
  for (unsigned i = 0; i < t.ncpu; i++) {
    const br_host_cpu *c = &t.cpu[i];
    if (c->present) printf("  cpu%u: %c capacity %u part 0x%03x policy %u %u..%u kHz (%u steps) L1d %u L2 %u L3 %u\n", i, c->type, c->capacity, c->part, c->policy,
                           c->min_khz, c->max_khz, c->nfreq, c->l1d_bytes, c->l2_bytes, c->l3_bytes);
  }
  if (br_cpu_plan_make(&t, nvcpu, false, &plan)) { printf("no plan for %u vCPUs\n", nvcpu); return 1; }
  printf("plan: %u vCPUs, %u E then %u P\n", plan.nvcpu, plan.n_e, plan.n_p);
  for (unsigned i = 0; i < plan.nvcpu; i++)
    printf("  vcpu%u: %c cluster %u host mask 0x%llx capacity %u max %u kHz\n", i, plan.v[i].type, plan.v[i].cluster_id, (unsigned long long)plan.v[i].affinity,
           plan.v[i].capacity, plan.v[i].max_khz);
  int rc = br_cpu_place(0, &plan, plan.nvcpu - 1);
  printf("placing this thread as vcpu%u: %s\n", plan.nvcpu - 1, rc ? strerror(-rc) : "done");
  rc = br_cpu_request(0, &plan, plan.nvcpu - 1, plan.v[plan.nvcpu - 1].max_khz);
  printf("uclamp.min %u for the top frequency: %s\n", br_cpu_util_for(&plan, plan.nvcpu - 1, plan.v[plan.nvcpu - 1].max_khz), rc ? strerror(-rc) : "done");
  return 0;
}

// Strictly passive probe. Unlike br_haptic_open_as(), this does NOT send
// FF_GAIN, upload a waveform, create a worker or play an effect.
static int haptic_inspect(void)
{
  DIR *d = opendir("/dev/input");
  if (!d) { printf("haptic-inspect: cannot open /dev/input: %s\n", strerror(errno)); return 1; }
  int found = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "event", 5)) continue;
    char path[256], name[128] = "";
    snprintf(path, sizeof path, "/dev/input/%.160s", e->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned char bits[FF_MAX / 8 + 1] = { 0 };
    int rc = ioctl(fd, EVIOCGBIT(EV_FF, sizeof bits), bits);
    bool periodic = rc >= 0 && ((bits[FF_PERIODIC / 8] >> (FF_PERIODIC % 8)) & 1);
    bool sine = rc >= 0 && ((bits[FF_SINE / 8] >> (FF_SINE % 8)) & 1);
    if (periodic && sine) {
      found++;
      (void)ioctl(fd, EVIOCGNAME(sizeof name), name);
      int effects = -1;
      int effect_rc = ioctl(fd, EVIOCGEFFECTS, &effects);
      bool custom = ((bits[FF_CUSTOM / 8] >> (FF_CUSTOM % 8)) & 1);
      bool gain = ((bits[FF_GAIN / 8] >> (FF_GAIN % 8)) & 1);
      printf("haptic-inspect: %s \"%s\" FF_PERIODIC=1 FF_SINE=1 FF_CUSTOM=%d FF_GAIN=%d max_effects=%d%s\n",
             path, name, custom, gain, effects, effect_rc < 0 ? " (query unavailable)" : "");
    }
    close(fd);
  }
  closedir(d);
  if (!found) { puts("haptic-inspect: no readable FF_PERIODIC + FF_SINE device"); return 1; }
  puts("haptic-inspect: ok (O_RDONLY; no FF_GAIN, EVIOCSFF, or playback)");
  return 0;
}

// The active Samsung evdev FF_SINE upload fails with EINVAL on SM8850,
// while One UI can play FF_CUSTOM. Before considering HAL playback, verify
// whether the stock AIDL vibrator is accessible with *read-only* calls.
// Opening and closing a HAL handle here does not call IVibrator.off()/on().
static int haptic_hal_take_caps(void *ctx, ab_reader *r)
{
  *(int32_t *)ctx = ab_get_i32(r);
  return r->bad ? -EPROTO : 0;
}

static int haptic_hal_inspect(void)
{
  char why[256] = "";
  br_hal *hal = br_hal_open(NULL, HAL_VIBRATOR_INSTANCE, HAL_VIBRATOR, why, sizeof why);
  if (!hal) {
    printf("haptic-hal-inspect: no usable %s: %s\n", HAL_VIBRATOR_INSTANCE, why);
    return 1;
  }
  const int32_t version = br_hal_version(hal);
  int32_t caps = 0;
  ab_parcel req;
  br_hal_parcel(&req, HAL_VIBRATOR);
  int rc = br_hal_call(hal, HAL_VIB_GET_CAPABILITIES, &req, haptic_hal_take_caps, &caps);
  br_hal_close(hal);
  if (rc) {
    printf("haptic-hal-inspect: getCapabilities failed: %s\n", br_hal_error(rc));
    return 1;
  }
  printf("haptic-hal-inspect: %s version %d capabilities 0x%x (amplitude=%s)\n",
         HAL_VIBRATOR_INSTANCE, version, (unsigned)caps,
         (caps & HAL_VIB_CAP_AMPLITUDE) ? "yes" : "no");
  puts("haptic-hal-inspect: ok (getCapabilities only; no on, off, amplitude or FF events)");
  return 0;
}

// Only AIDL getters. No IVibrator.on/off/compose/perform/setAmplitude,
// no sysfs writes, no opening the evdev node for playback. This query
// establishes vendor-supported primitive IDs before any physical trial.
typedef struct { int32_t n, id[32]; } haptic_int_list;
static int haptic_take_list(void *ctx, ab_reader *r)
{
  haptic_int_list *l = ctx;
  int32_t n = ab_get_i32(r);
  if (r->bad || n < 0 || n > (int32_t)(sizeof l->id / sizeof l->id[0])) return -EPROTO;
  for (int32_t i = 0; i < n; i++) l->id[i] = ab_get_i32(r);
  if (r->bad) return -EPROTO;
  l->n = n;
  return 0;
}
static const char *haptic_primitive_name(int32_t id)
{
  switch (id) {
  case 0: return "NOOP";
  case 1: return "CLICK";
  case 2: return "THUD";
  case 3: return "SPIN";
  case 4: return "QUICK_RISE";
  case 5: return "SLOW_RISE";
  case 6: return "QUICK_FALL";
  case 7: return "LIGHT_TICK";
  case 8: return "LOW_TICK";
  default: return "unknown";
  }
}

static int haptic_hal_primitives(void)
{
  char why[256] = "";
  br_hal *hal = br_hal_open(NULL, HAL_VIBRATOR_INSTANCE, HAL_VIBRATOR, why, sizeof why);
  if (!hal) {
    printf("haptic-hal-primitives: no usable %s: %s\n", HAL_VIBRATOR_INSTANCE, why);
    return 1;
  }
  ab_parcel p;
  int32_t caps = 0;
  br_hal_parcel(&p, HAL_VIBRATOR);
  int rc = br_hal_call(hal, HAL_VIB_GET_CAPABILITIES, &p, haptic_hal_take_caps, &caps);
  if (rc) {
    printf("haptic-hal-primitives: getCapabilities failed: %s\n", br_hal_error(rc));
    br_hal_close(hal);
    return 1;
  }
  printf("haptic-hal-primitives: AIDL version %d capabilities 0x%x compose=%s\n",
         br_hal_version(hal), (unsigned)caps,
         (caps & HAL_VIB_CAP_COMPOSE) ? "yes" : "no");
  if (!(caps & HAL_VIB_CAP_COMPOSE)) {
    puts("haptic-hal-primitives: composition not advertised; no primitive queries");
    br_hal_close(hal);
    return 0;
  }

  int32_t max_delay = -1, max_size = -1;
  br_hal_parcel(&p, HAL_VIBRATOR);
  rc = br_hal_call(hal, HAL_VIB_COMPOSE_DELAY_MAX, &p, haptic_hal_take_caps, &max_delay);
  if (!rc) {
    br_hal_parcel(&p, HAL_VIBRATOR);
    rc = br_hal_call(hal, HAL_VIB_COMPOSE_SIZE_MAX, &p, haptic_hal_take_caps, &max_size);
  }
  if (rc) {
    printf("haptic-hal-primitives: composition limits query failed: %s\n", br_hal_error(rc));
    br_hal_close(hal);
    return 1;
  }
  printf("haptic-hal-primitives: max delay %d ms, max composition entries %d\n", max_delay, max_size);

  haptic_int_list primitives = {0};
  br_hal_parcel(&p, HAL_VIBRATOR);
  rc = br_hal_call(hal, HAL_VIB_SUPPORTED_PRIMITIVES, &p, haptic_take_list, &primitives);
  if (rc) {
    printf("haptic-hal-primitives: getSupportedPrimitives failed: %s\n", br_hal_error(rc));
    br_hal_close(hal);
    return 1;
  }
  printf("haptic-hal-primitives: %d supported primitive(s)\n", primitives.n);
  for (int32_t i = 0; i < primitives.n; i++) {
    int32_t duration = -1;
    br_hal_parcel(&p, HAL_VIBRATOR);
    ab_put_i32(&p, primitives.id[i]);
    rc = br_hal_call(hal, HAL_VIB_PRIMITIVE_DURATION, &p, haptic_hal_take_caps, &duration);
    if (rc) {
      printf("  primitive %d %-12s duration unavailable: %s\n",
             primitives.id[i], haptic_primitive_name(primitives.id[i]), br_hal_error(rc));
    } else {
      printf("  primitive %d %-12s duration %d ms\n",
             primitives.id[i], haptic_primitive_name(primitives.id[i]), duration);
    }
  }

  haptic_int_list effects = {0};
  br_hal_parcel(&p, HAL_VIBRATOR);
  rc = br_hal_call(hal, HAL_VIB_SUPPORTED_EFFECTS, &p, haptic_take_list, &effects);
  if (rc) {
    printf("haptic-hal-primitives: getSupportedEffects unavailable: %s\n", br_hal_error(rc));
  } else {
    printf("haptic-hal-primitives: %d supported preset effect(s):", effects.n);
    for (int32_t i = 0; i < effects.n; i++) printf(" %d", effects.id[i]);
    putchar('\n');
  }
  br_hal_close(hal);
  puts("haptic-hal-primitives: ok (read-only AIDL getters; no on, off, compose, perform, amplitude or FF)");
  return 0;
}

// Passive physical integration probe for the REAL production haptic bridge.
// The native opt-in causes HAL getCapabilities/getSupportedEffects only, and
// the bridge is closed unused. No off()/perform()/on()/setAmplitude(), even
// when the required preset set is unavailable. Missing HAL is fail-closed.
static int haptic_native_inspect(int argc)
{
  if (argc != 2) return 2;
  const char *enabled = getenv("ONEIOS_HAPTIC_NATIVE_IMPACTS");
  if (!enabled || strcmp(enabled, "1")) {
    puts("haptic-native-inspect: blocked; set ONEIOS_HAPTIC_NATIVE_IMPACTS=1 to permit read-only native preflight");
    return 1;
  }
  if (br_hw_fallback_allowed(br_hw_current())) {
    puts("haptic-native-inspect: blocked; disable ONEIOS_HW_FALLBACK to prevent evdev fallback");
    return 1;
  }
  br_haptic *h = br_haptic_open_as(NULL, BR_HW_TREBLE, NULL);
  if (!h) {
    puts("haptic-native-inspect: Treble bridge unavailable; no evdev fallback");
    return 1;
  }
  bool supported = br_haptic_impacts_available(h);
  br_haptic_close(h);                   // no on/off/gain/perform when native was requested
  if (!supported) {
    puts("haptic-native-inspect: native preset preflight FAILED (no actuation)");
    return 1;
  }
  puts("haptic-native-inspect: ready (native impact preflight PASS; no on, off, gain, perform or evdev)");
  return 0;
}

// Bounded, opt-in ACTIVE test of a vendor-advertised AOSP preset.
// Exactly one AIDL perform() request: no FF fallback, manual waveform,
// amplitude/gain setting, on/off, loop, external control or compose.
// Do not use while the Android framework is already vibrating.
static int haptic_hal_preset(int argc, bool heavy)
{
  const char *cmd = heavy ? "haptic-hal-heavy-click" : "haptic-hal-click";
  const char *effect_name = heavy ? "HEAVY_CLICK" : "CLICK";
  const char *strength_name = heavy ? "STRONG" : "MEDIUM";
  const int32_t effect = heavy ? HAL_VIB_EFFECT_HEAVY_CLICK : HAL_VIB_EFFECT_CLICK;
  const int32_t strength = heavy ? HAL_VIB_STRENGTH_STRONG : HAL_VIB_STRENGTH_MEDIUM;
  if (argc != 2) {
    printf("usage: ONEIOS_HAPTIC_ACTIVE_TEST=1 bridgectl %s\n", cmd);
    return 2;
  }
  const char *ack = getenv("ONEIOS_HAPTIC_ACTIVE_TEST");
  if (!ack || strcmp(ack, "1") != 0) {
    printf("%s: blocked; set ONEIOS_HAPTIC_ACTIVE_TEST=1 to authorize one active %s\n", cmd, effect_name);
    return 1;
  }

  char why[256] = "";
  br_hal *hal = br_hal_open(NULL, HAL_VIBRATOR_INSTANCE, HAL_VIBRATOR, why, sizeof why);
  if (!hal) {
    printf("%s: no usable %s: %s\n", cmd, HAL_VIBRATOR_INSTANCE, why);
    return 1;
  }
  haptic_int_list effects = {0};
  ab_parcel p;
  br_hal_parcel(&p, HAL_VIBRATOR);
  int rc = br_hal_call(hal, HAL_VIB_SUPPORTED_EFFECTS, &p, haptic_take_list, &effects);
  if (rc) {
    printf("%s: getSupportedEffects failed: %s\n", cmd, br_hal_error(rc));
    br_hal_close(hal);
    return 1;
  }
  bool advertised = false;
  for (int32_t i = 0; i < effects.n; i++)
    if (effects.id[i] == effect) advertised = true;
  if (!advertised) {
    printf("%s: %s not in supported effects; no actuation attempted\n", cmd, effect_name);
    br_hal_close(hal);
    return 1;
  }

  br_hal_parcel(&p, HAL_VIBRATOR);
  ab_put_i32(&p, effect);
  ab_put_i32(&p, strength);
  ab_put_null_binder(&p);                 // S26 CAP_PERFORM_CALLBACK not advertised
  int32_t duration_ms = -1;
  rc = br_hal_call(hal, HAL_VIB_PERFORM, &p, haptic_hal_take_caps, &duration_ms);
  if (rc) {
    printf("%s: perform(%s, %s) rejected: %s\n", cmd, effect_name, strength_name, br_hal_error(rc));
    br_hal_close(hal);
    return 1;
  }
  printf("%s: HAL accepted one %s %s (reported duration %d ms)\n", cmd, effect_name, strength_name, duration_ms);
  if (duration_ms <= 0 || duration_ms > 1000) {
    printf("%s: HAL returned an unexpected duration; no follow-up effects\n", cmd);
    br_hal_close(hal);
    return 1;
  }
  // Let the HAL finish its own effect; off() could cancel unrelated Android
  // vibration starting after our one-shot preset.
  usleep((useconds_t)(duration_ms + 80) * 1000u);
  br_hal_close(hal);
  printf("%s: done (one perform; no off, gain, compose or fallback; tactile verification required)\n", cmd);
  return 0;
}

static int haptic(int argc, char **argv, bool pattern)
{
  br_haptic *h = br_haptic_open_as(getenv("ONEIOS_INPUT_DIR"), BR_HW_UNSET, NULL);   // the backend: hw-backend-haptics
  if (!h) return 1;
  if (pattern) {
    for (unsigned i = 0; i < 3; i++) { br_haptic_vib(h, 15, 1000, 0); usleep(150000); }
    for (unsigned level = 100; level <= 1000; level += 100) { br_haptic_vib(h, 200, level, 0); usleep(60000); }
    usleep(300000);
  } else {
    unsigned ms = argc > 2 ? (unsigned)atoi(argv[2]) : 200, intensity = argc > 3 ? (unsigned)atoi(argv[3]) : 1000, hz = argc > 4 ? (unsigned)atoi(argv[4]) : 0;
    br_haptic_vib(h, ms, intensity, hz);
    usleep((ms + 100) * 1000);
  }
  // The worker is asynchronous: an earlier "ok" only meant the CLI
  // completed, even if the driver rejected every effect (EINVAL).
  br_haptic_flush(h);
  br_haptic_stats stats;
  br_haptic_stats_get(h, &stats);
  int failed = stats.failed > 0 || stats.played == 0;
  if (failed)
    printf("bridgectl: haptic failed (effects played=%llu, failed=%llu)\n",
           (unsigned long long)stats.played, (unsigned long long)stats.failed);
  br_haptic_close(h);
  return failed ? 1 : 0;
}

static volatile int bt_state = -1;
static volatile unsigned bt_events;
static void bt_rx(void *ctx, uint8_t type, const uint8_t *packet, size_t length)
{
  (void)ctx;
  printf("  <- type %u, %zu bytes:", type, length);
  for (size_t i = 0; i < length && i < 24; i++) printf(" %02x", packet[i]);
  printf("\n");
  bt_events++;
}
static void bt_changed(void *ctx, int state) { (void)ctx; bt_state = state; printf("  state %d\n", state); }

static int bt(void)
{
  static const uint8_t reset[] = { 0x03, 0x0c, 0x00 }, read_address[] = { 0x09, 0x10, 0x00 };
  uint8_t address[6];
  if (!br_bt_address("/mnt/vendor/efs/bluetooth/bt_addr", address)) printf("address file: %02x:%02x:%02x:%02x:%02x:%02x\n", address[0], address[1], address[2], address[3], address[4], address[5]);
  int hal = br_hci_hidl_probe();
  if (hal < 0) { printf("no Bluetooth HAL found (%d)\n", hal); return 1; }
  printf("HAL %s registered\n", hal == 11 ? "1.1" : "1.0");
  br_hci *h = br_hci_open_hidl(bt_rx, bt_changed, NULL);
  if (!h || br_hci_open(h)) return 1;
  for (unsigned i = 0; i < 200 && bt_state < 1; i++) usleep(100000);      // the firmware download takes seconds
  if (bt_state != 1) { printf("the HAL did not become ready (state %d)\n", bt_state); return 1; }
  int rc = br_hci_send(h, 1, reset, sizeof reset);
  usleep(500000);
  if (!rc) rc = br_hci_send(h, 1, read_address, sizeof read_address);
  usleep(500000);
  br_hci_close(h);
  usleep(200000);
  if (rc || bt_events < 2) { printf("send %d, %u events (2 expected)\n", rc, bt_events); return 1; }
  printf("bt: ok: %u events (Command Complete of HCI Reset and Read BD_ADDR expected)\n", bt_events);
  return 0;
}

static volatile unsigned net_frames;
static void net_rx(void *ctx, const uint8_t *frame, size_t length) { (void)ctx; (void)frame; (void)length; net_frames++; }

static int net(void)
{
  br_net_config c;
  br_net_config_defaults(&c);
  br_net *n = br_net_open_tap(&c, net_rx, NULL);
  if (!n) return 1;
  printf("tap %s is open; run `oneios-net.sh up` in another shell, then `ping -I %s 192.168.77.2` shows ARP requests here\n", c.ifname, c.ifname);
  sleep(10);
  printf("%u frames from the tap in 10 s\n", net_frames);
  br_net_close(n);
  return 0;
}

// A quiet tone (-30 dBFS: never loud, whatever the media volume) paced by the clock as the guest's engine paces
// its periods; then, with `record`, two seconds of the microphone: frames, peak and RMS. The helper's own lines
// (its stream's mode, the ring's report) are on stderr.
static int audio(int argc, char **argv)
{
  const char *path = argv[2];
  unsigned seconds = argc > 3 ? (unsigned)atoi(argv[3]) : 2;
  bool record = argc > 4 && !strcmp(argv[4], "record");
  if (seconds < 1 || seconds > 30) seconds = 2;
  void *ctx;
  const br_audio_backend *b = br_audio_helper(path, &ctx);
  if (!b) return 1;
  int probed = br_audio_probe_latency(b, ctx, 2000);
  if (probed > 0) printf("audio: probe: speaker latency %d frames (%.1f ms): oneios-audio-latency=%d in a session\n", probed, probed / 48.0, probed);
  else printf("audio: probe: not measured (%s)\n", strerror(-probed));
  if (b->start(ctx, true, 48000, 2)) return 1;
  int16_t period[480];
  double phase = 0;
  struct timespec at;
  clock_gettime(CLOCK_MONOTONIC, &at);
  unsigned sent = 0;
  for (unsigned p = 0; p < seconds * 200; p++) {                // 5 ms periods
    for (unsigned i = 0; i < 240; i++) { period[2 * i] = period[2 * i + 1] = (int16_t)(1000 * sin(phase)); phase += 2 * M_PI * 440 / 48000; }
    if (b->write(ctx, period, 240) == 240) sent++;
    at.tv_nsec += 5000000;
    if (at.tv_nsec >= 1000000000) { at.tv_nsec -= 1000000000; at.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL);
  }
  printf("audio: %u of %u periods of a 440 Hz tone at -30 dBFS sent\n", sent, seconds * 200);
  b->stop(ctx, true);
  // A second, short tone 0.5 s later, as iOS stops and starts its device around each sound: the helper's stream
  // lingers, so this one starts without the track's stall (its report says so).
  usleep(500000);
  if (b->start(ctx, true, 48000, 2)) return 1;
  clock_gettime(CLOCK_MONOTONIC, &at);
  for (unsigned p = 0; p < 100; p++) {
    for (unsigned i = 0; i < 240; i++) { period[2 * i] = period[2 * i + 1] = (int16_t)(1000 * sin(phase)); phase += 2 * M_PI * 660 / 48000; }
    (void)b->write(ctx, period, 240);
    at.tv_nsec += 5000000;
    if (at.tv_nsec >= 1000000000) { at.tv_nsec -= 1000000000; at.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL);
  }
  b->stop(ctx, true);
  int rc = sent ? 0 : 1;
  if (record) {
    if (b->start(ctx, false, 48000, 1)) { b->close(ctx); return 1; }
    int16_t pcm[240];
    uint64_t frames = 0, counted = 0;
    double sum = 0;
    int peak = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
      int n = b->read(ctx, pcm, 240);
      if (n < 0) break;
      // The first 250 ms are the input's start (a full-scale click on the phone): not counted in the peak and RMS.
      for (int i = 0; i < n && frames + (uint64_t)i >= 12000; i++) { int v = abs(pcm[i]); if (v > peak) peak = v; sum += (double)pcm[i] * pcm[i]; counted++; }
      frames += (uint64_t)(n > 0 ? n : 0);
      clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < 2000);
    b->stop(ctx, false);
    double rms = counted ? sqrt(sum / (double)counted) : 0;
    printf("audio: microphone: %llu frames in 2 s (96000 expected), peak %d (%.1f dBFS), RMS %.1f dBFS%s\n", (unsigned long long)frames, peak,
           peak ? 20 * log10(peak / 32768.0) : -999.0, rms > 0 ? 20 * log10(rms / 32768.0) : -999.0,
           frames && !peak ? " (all zero: silenced?)" : "");
    if (!frames) rc = 1;
  }
  usleep(300000);                                               // the helper's last report
  b->close(ctx);
  usleep(100000);
  return rc;
}

// audio-loop: the microphone's frames, each with the time it came in here (a reader thread), against the clicks,
// each with the time its first period was handed to the helper. A frame's time is its period's arrival less the
// frames after it in that period: it includes the microphone's own path (capture, AAudio, the helper, the socket),
// so a round trip is the speaker's latency plus the air plus the microphone's latency. The helper's speaker report
// (stderr) says the speaker's part from AAudio's timestamps; the difference is the microphone's and the air's.
typedef struct { const br_audio_backend *b; void *ctx; int16_t *pcm; double *at; unsigned frames, cap; bool run; } loop_mic;
static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
static void *loop_reader(void *arg)
{
  loop_mic *m = arg;
  int16_t pcm[240];
  while (__atomic_load_n(&m->run, __ATOMIC_ACQUIRE) && m->frames + 240 <= m->cap) {
    int n = m->b->read(m->ctx, pcm, 240);
    if (n < 0) break;
    if (n == 0) continue;
    double now = mono_s();
    memcpy(m->pcm + m->frames, pcm, (size_t)n * 2);
    for (int i = 0; i < n; i++) m->at[m->frames + (unsigned)i] = now - (double)(n - 1 - i) / 48000;
    m->frames += (unsigned)n;
  }
  return NULL;
}
static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static int audio_loop(int argc, char **argv)
{
  const char *path = argv[2];
  unsigned clicks = argc > 3 ? (unsigned)atoi(argv[3]) : 10;
  if (clicks < 1 || clicks > 20) clicks = 10;
  void *ctx;
  const br_audio_backend *b = br_audio_helper(path, &ctx);
  if (!b) return 1;
  loop_mic m = { .b = b, .ctx = ctx, .cap = 48000 * 14, .run = true };
  m.pcm = calloc(m.cap, sizeof *m.pcm);
  m.at = calloc(m.cap, sizeof *m.at);
  double sent[20] = { 0 }, heard[20];
  pthread_t reader;
  if (!m.pcm || !m.at || b->start(ctx, false, 48000, 1) || b->start(ctx, true, 48000, 2) || pthread_create(&reader, NULL, loop_reader, &m)) {
    printf("audio-loop: the speaker or the microphone did not start\n");
    b->close(ctx);
    return 1;
  }
  // 1 s of silence (the microphone's start click, the noise floor), then a click every 0.5 s: 4 periods of 1 kHz.
  int16_t period[480];
  struct timespec at;
  clock_gettime(CLOCK_MONOTONIC, &at);
  const unsigned periods = 200 + clicks * 100 + 60;
  for (unsigned p = 0, k = 0; p < periods; p++) {
    bool tone = p >= 200 && (p - 200) % 100 < 4 && (p - 200) / 100 < clicks;
    for (unsigned i = 0; i < 240; i++) period[2 * i] = period[2 * i + 1] = tone ? (int16_t)(1000 * sin(2 * M_PI * 1000 * i / 48000.0)) : 0;
    if (tone && (p - 200) % 100 == 0) sent[k++] = mono_s();
    (void)b->write(ctx, period, 240);
    at.tv_nsec += 5000000;
    if (at.tv_nsec >= 1000000000) { at.tv_nsec -= 1000000000; at.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL);
  }
  b->stop(ctx, true);
  usleep(200000);
  __atomic_store_n(&m.run, false, __ATOMIC_RELEASE);
  pthread_join(reader, NULL);
  b->stop(ctx, false);
  // The noise: RMS of 1 ms windows from 0.4 s in to the first click; the threshold 8 times their median (18 dB), at
  // least -56 dBFS.
  double windows[1000];
  unsigned nw = 0;
  for (unsigned f = 0; f + 48 <= m.frames && nw < 1000; f += 48) {
    if (m.at[f] < sent[0] - 0.6 || m.at[f + 47] >= sent[0]) continue;
    double s = 0;
    for (unsigned i = 0; i < 48; i++) s += (double)m.pcm[f + i] * m.pcm[f + i];
    windows[nw++] = sqrt(s / 48);
  }
  qsort(windows, nw, sizeof *windows, cmp_double);
  double noise = nw ? windows[nw / 2] : 0, threshold = noise * 8 > 50 ? noise * 8 : 50;
  unsigned found = 0;
  for (unsigned k = 0; k < clicks; k++) {
    heard[k] = -1;
    for (unsigned f = 0; f + 48 <= m.frames; f += 8) {
      if (m.at[f] < sent[k]) continue;
      if (m.at[f] > sent[k] + 0.4) break;
      double s = 0;
      for (unsigned i = 0; i < 48; i++) s += (double)m.pcm[f + i] * m.pcm[f + i];
      if (sqrt(s / 48) > threshold) { heard[k] = (m.at[f] - sent[k]) * 1e3; break; }
    }
    if (heard[k] >= 0) printf("audio-loop: click %u: heard %.1f ms after it was handed to the helper\n", k + 1, heard[k]);
    else printf("audio-loop: click %u: not heard within 400 ms\n", k + 1);
    if (heard[k] >= 0) heard[found++] = heard[k];
  }
  qsort(heard, found, sizeof *heard, cmp_double);
  printf("audio-loop: %u frames from the microphone, noise %.1f dBFS, threshold %.1f dBFS\n", m.frames,
         noise > 0 ? 20 * log10(noise / 32768.0) : -999.0, 20 * log10(threshold / 32768.0));
  if (found) printf("audio-loop: round trip (the speaker + the air + the microphone) median %.1f ms, %.1f..%.1f, %u of %u clicks\n",
                    heard[found / 2], heard[0], heard[found - 1], found, clicks);
  usleep(300000);                                               // the helper's last report
  b->close(ctx);
  usleep(100000);
  free(m.pcm);
  free(m.at);
  return found * 2 >= clicks ? 0 : 1;
}

// The bridges' own lines (why a HAL was not found, the HAL's state), as xnuhost logs them.
// ---- serve: the bridge channel for a VM of crosvm (the native path; docs/native.md "The bridge in the crosvm VM") ----
// The kernel's bridge driver reaches the phone through crosvm's second serial port, which crosvm connects to this
// socket when it makes its devices: so this listens before crosvm starts (vm-boot's script waits for the socket) and
// serves one connection: the phone's battery to iOS (read only: /sys/class/power_supply, or the health HAL), iOS's
// haptics to the phone's motor. It ends when crosvm closes the socket (the VM ended), at SIGTERM or SIGINT, or when
// nothing connected in --wait seconds; the motor is stopped at the end. Never a side key's device: the haptic
// backend opens the force-feedback device only.
static volatile int serve_stop;
static void serve_signal(int sig) { (void)sig; serve_stop = 1; }

static int serve(int argc, char **argv)
{
  const char *path = argc > 2 ? argv[2] : NULL;
  double wait_s = 120;
  bool haptics = true, battery = true;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--no-haptics")) haptics = false;
    else if (!strcmp(argv[i], "--no-battery")) battery = false;
    else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait_s = atof(argv[++i]);
    else path = NULL;
  }
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (!path || *path != '/' || strlen(path) >= sizeof addr.sun_path || wait_s <= 0 || wait_s > 3600) {
    fprintf(stderr, "usage: bridgectl serve /SOCKET [--wait SECONDS] [--no-haptics] [--no-battery]\n");
    return 2;
  }
  strcpy(addr.sun_path, path);
  struct sigaction act = { .sa_handler = serve_signal };           // no SA_RESTART: poll() ends at the signal
  sigaction(SIGTERM, &act, NULL);
  sigaction(SIGINT, &act, NULL);
  signal(SIGPIPE, SIG_IGN);
  br_stream_config config = { 0 };
  if (battery) {
    // ONEIOS_BATTERY_DIR: the options (as batteryctl); otherwise the phone's paths and variables
    br_battery_options o = { .dir = getenv("ONEIOS_BATTERY_DIR"), .supplies = getenv("ONEIOS_BATTERY_SUPPLIES") };
    config.battery = br_battery_open(o.dir ? &o : NULL);
  }
  if (haptics) config.haptic = br_haptic_open_as(getenv("ONEIOS_INPUT_DIR"), BR_HW_UNSET, NULL);
  unlink(path);
  int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof addr) || chmod(path, 0600) || listen(server, 1)) {
    printf("serve: %s: %s\n", path, strerror(errno));
    if (server >= 0) close(server);
    if (config.haptic) br_haptic_close(config.haptic);
    if (config.battery) br_battery_close(config.battery);
    return 1;
  }
  printf("serve: listening on %s for crosvm's second serial port: battery %s, haptics %s\n", path,
         config.battery ? "served" : battery ? "not served (no battery to read)" : "off (--no-battery)",
         config.haptic ? "served" : haptics ? "not served (no actuator opened)" : "off (--no-haptics)");
  int client = -1;
  struct timespec t0, now;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  while (client < 0 && !serve_stop) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9 >= wait_s) break;
    struct pollfd listening = { server, POLLIN, 0 };
    if (poll(&listening, 1, 200) > 0) client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
  }
  close(server);
  unlink(path);
  int rc = 1;
  if (client < 0) {
    printf("serve: %s: the VM has no bridge\n", serve_stop ? "stopped before crosvm connected" : "crosvm did not connect in time");
  } else {
    printf("serve: crosvm connected: the VM's bridge channel is served here\n");
    br_stream_stats st;
    rc = br_stream_serve(client, &config, &serve_stop, &st) ? 1 : 0;
    close(client);
    printf("serve: %s: %llu messages from the guest (%llu hello, %llu haptic, %llu battery queries, %llu on channels not served; %llu bad "
           "frames), %llu battery states to it; %llu bytes in, %llu out\n",
           rc ? "the stream failed" : serve_stop ? "stopped" : "crosvm closed the port (the VM ended)", (unsigned long long)st.frames_in,
           (unsigned long long)st.hellos, (unsigned long long)st.haptic, (unsigned long long)st.battery_queries, (unsigned long long)st.unknown,
           (unsigned long long)st.frames_bad, (unsigned long long)st.battery_states, (unsigned long long)st.bytes_in, (unsigned long long)st.bytes_out);
  }
  if (config.haptic) br_haptic_close(config.haptic);                // its counter line; the motor stopped
  if (config.battery) br_battery_close(config.battery);
  return rc;
}

static void say(int level, const char *line) { printf("  %s%s\n", level == 0 ? "error: " : "", line); }

int main(int argc, char **argv)
{
  int rc = 2;
  setvbuf(stdout, NULL, _IOLBF, 0);
  br_log_sink = say;
  br_hw_settings hw;
  const char *bad = NULL;
  br_hw_settings_init(&hw);
  if (br_hw_settings_env(&hw, &bad)) { fprintf(stderr, "bridgectl: bad %s (samsung or treble; ONEIOS_HW_FALLBACK 0 or 1)\n", bad); return 2; }
  br_hw_configure(&hw);
  if (argc < 2) { fprintf(stderr, "usage: bridgectl cpu|haptic-inspect|haptic-hal-inspect|haptic-hal-primitives|haptic-native-inspect|haptic-hal-click|haptic-hal-heavy-click|haptic|haptic-pattern|serve|bt|net|audio|audio-loop ... (see tools/bridgectl.c)\n"); return 2; }
  if (!strcmp(argv[1], "cpu")) rc = cpu(argc, argv);
  else if (!strcmp(argv[1], "haptic-inspect")) rc = haptic_inspect();
  else if (!strcmp(argv[1], "haptic-hal-inspect")) rc = haptic_hal_inspect();
  else if (!strcmp(argv[1], "haptic-hal-primitives")) rc = haptic_hal_primitives();
  else if (!strcmp(argv[1], "haptic-native-inspect")) rc = haptic_native_inspect(argc);
  else if (!strcmp(argv[1], "haptic-hal-click")) rc = haptic_hal_preset(argc, false);
  else if (!strcmp(argv[1], "haptic-hal-heavy-click")) rc = haptic_hal_preset(argc, true);
  else if (!strcmp(argv[1], "haptic")) rc = haptic(argc, argv, false);
  else if (!strcmp(argv[1], "haptic-pattern")) rc = haptic(argc, argv, true);
  else if (!strcmp(argv[1], "serve")) rc = serve(argc, argv);
  else if (!strcmp(argv[1], "bt")) rc = bt();
  else if (!strcmp(argv[1], "net")) rc = net();
  else if (!strcmp(argv[1], "audio") && argc > 2) rc = audio(argc, argv);
  else if (!strcmp(argv[1], "audio-loop") && argc > 2) rc = audio_loop(argc, argv);
  printf("%s\n", rc == 0 ? "ok" : rc == 2 ? "unknown command" : "failed");
  return rc;
}
