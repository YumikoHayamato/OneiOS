// SPDX-License-Identifier: MIT
// The bridge channel's phone end on a byte stream: bridges/stream.h. The dispatch is devices/bridge_dev.c's for the
// channels a VM of crosvm gets (control, haptics, battery); the transport is frames on a descriptor instead of two
// virtqueues. Messages for the guest are written under one lock: the battery's come from its worker thread.
#include "bridges/stream.h"
#include "oneios_bridge_stream.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  int fd;
  br_stream_config config;
  pthread_mutex_t lock;                   // the writes, and the counters they change
  br_stream_stats stats;
  int failed;                             // a write failed: the stream is given up
  int ended;                              // br_stream_serve returned: a battery report still on its way is dropped
} stream_state;

static void to_guest(stream_state *s, uint16_t channel, uint16_t type, const void *payload, uint32_t length)
{
  uint8_t frame[OBS_MAX_FRAME];
  const size_t size = obs_encode(frame, channel, type, payload, length);
  if (!size) return;
  pthread_mutex_lock(&s->lock);
  for (size_t done = 0; done < size && !s->failed && !s->ended;) {
    ssize_t n = write(s->fd, frame + done, size - done);
    if (n > 0) { done += (size_t)n; s->stats.bytes_out += (uint64_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {      // a full socket: the guest reads it within its poll
      struct pollfd w = { s->fd, POLLOUT, 0 };
      if (poll(&w, 1, 2000) > 0) continue;
    }
    s->failed = 1;
  }
  pthread_mutex_unlock(&s->lock);
}

static void battery_send(void *ctx, const br_battery_sample *b)
{
  stream_state *s = ctx;
  struct ob_battery_state state = { (b->present ? OB_BATTERY_PRESENT : 0) | (b->plugged ? OB_BATTERY_PLUGGED : 0) |
                                        (b->charging ? OB_BATTERY_CHARGING : 0) | (b->full ? OB_BATTERY_FULL : 0),
                                    b->percent, b->temp_deci_c, b->voltage_mv, b->current_ma, b->full_mah, b->design_mah, b->cycles, b->minutes };
  to_guest(s, OB_CH_BATTERY, OB_BATTERY_STATE, &state, sizeof state);
  pthread_mutex_lock(&s->lock);
  s->stats.battery_states++;
  pthread_mutex_unlock(&s->lock);
}

static void from_guest(stream_state *s, const struct ob_header *h, const uint8_t *payload)
{
  switch (h->channel) {
  case OB_CH_CONTROL:
    if (h->type == OB_CTL_HELLO) {
      struct ob_ctl_hello hello = { OB_PROTO_VERSION, 1u << OB_CH_CONTROL | (s->config.haptic ? 1u << OB_CH_HAPTIC : 0) |
                                                       (s->config.battery ? 1u << OB_CH_BATTERY : 0) };
      s->stats.hellos++;
      if (s->stats.hellos == 1)
        br_log(1, "bridge: the guest's hello: channels 0x%x for it (control%s%s)", hello.channels, s->config.haptic ? ", haptics" : "",
               s->config.battery ? ", battery" : "");
      if (br_haptic_impacts_available(s->config.haptic)) {
        struct ob_ctl_hello_ext ext = { hello.version, hello.channels, OB_FEAT_HAPTIC_IMPACT };
        to_guest(s, OB_CH_CONTROL, OB_CTL_HELLO, &ext, sizeof ext);
      } else {
        to_guest(s, OB_CH_CONTROL, OB_CTL_HELLO, &hello, sizeof hello);
      }
    } else s->stats.unknown++;
    break;
  case OB_CH_HAPTIC:
    if (!s->config.haptic) { s->stats.unknown++; break; }
    s->stats.haptic++;
    if (h->type == OB_HAPTIC_VIB && h->length >= sizeof(struct ob_haptic_vib)) {
      struct ob_haptic_vib vib;
      memcpy(&vib, payload, sizeof vib);
      br_haptic_vib(s->config.haptic, vib.on_ms, vib.intensity, vib.frequency_hz);
    } else if (h->type == OB_HAPTIC_STOP) br_haptic_stop(s->config.haptic);
    else if (h->type == OB_HAPTIC_IMPACT && h->length == sizeof(struct ob_haptic_impact) && br_haptic_impacts_available(s->config.haptic)) {
      struct ob_haptic_impact impact;
      memcpy(&impact, payload, sizeof impact);
      if (impact.kind >= OB_IMPACT_LIGHT && impact.kind <= OB_IMPACT_HEAVY) (void)br_haptic_impact(s->config.haptic, impact.kind);
    }
    break;
  case OB_CH_BATTERY:
    if (s->config.battery && h->type == OB_BATTERY_QUERY) {
      br_battery_sample sample;
      s->stats.battery_queries++;
      (void)br_battery_query(s->config.battery, &sample);         // on a failed read: the last good sample
      if (s->stats.battery_queries == 1)
        br_log(1, "bridge: battery: iOS asks: %u%%%s%s (the changes follow)", sample.percent, sample.plugged ? ", on power" : ", on battery",
               sample.charging ? ", charging" : sample.full ? ", full" : "");
      battery_send(s, &sample);
      br_battery_listen(s->config.battery, battery_send, s);      // changes from now on
    } else s->stats.unknown++;
    break;
  default:
    s->stats.unknown++;                   // Bluetooth, brightness, thermal, camera: not offered in the hello
    break;
  }
}

int br_stream_serve(int fd, const br_stream_config *config, const volatile int *stop, br_stream_stats *stats)
{
  // One stream at a time (the lock says so), its state static: the battery's worker may still be in battery_send for
  // a moment after br_battery_listen(NULL) below, and must find the state (ended) and not a returned frame.
  static struct obs_parser parser;
  static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;
  static stream_state s = { .lock = PTHREAD_MUTEX_INITIALIZER };
  int rc = 0;
  pthread_mutex_lock(&one);
  memset(&parser, 0, sizeof parser);
  pthread_mutex_lock(&s.lock);
  s.fd = fd;
  s.config = *config;
  s.failed = s.ended = 0;
  memset(&s.stats, 0, sizeof s.stats);
  pthread_mutex_unlock(&s.lock);
  for (;;) {
    if (stop && *stop) break;
    if (s.failed) { rc = -1; break; }
    struct pollfd p = { fd, POLLIN, 0 };
    int ready = poll(&p, 1, 200);
    if (ready < 0) { if (errno == EINTR) continue; rc = -1; break; }
    if (!ready) continue;
    uint8_t bytes[512];
    ssize_t n = read(fd, bytes, sizeof bytes);
    if (n == 0) break;                                              // the other end closed: the VM ended
    if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; rc = errno == ECONNRESET ? 0 : -1; break; }
    for (ssize_t i = 0; i < n; i++) {
      if (!obs_push(&parser, bytes[i])) continue;
      struct ob_header header;
      memcpy(&header, parser.message, sizeof header);
      pthread_mutex_lock(&s.lock);
      s.stats.frames_in++;
      pthread_mutex_unlock(&s.lock);
      from_guest(&s, &header, parser.message + sizeof header);
    }
    pthread_mutex_lock(&s.lock);
    s.stats.bytes_in += (uint64_t)n;
    pthread_mutex_unlock(&s.lock);
  }
  // A guest that is gone leaves no motor running and no reads for it.
  if (s.config.battery) br_battery_listen(s.config.battery, NULL, NULL);
  if (s.config.haptic) br_haptic_stop(s.config.haptic);
  pthread_mutex_lock(&s.lock);
  s.ended = 1;
  s.stats.frames_bad = parser.dropped;
  if (stats) *stats = s.stats;
  pthread_mutex_unlock(&s.lock);
  pthread_mutex_unlock(&one);
  return rc;
}
