// SPDX-License-Identifier: MIT
// The bridge channel on a byte stream (bridges/stream.h, drivers/bridge/oneios_bridge_stream.h): the frames, and the
// phone's end against a guest that speaks them over a socket pair as the kernel's bridge driver does over crosvm's
// second serial port: the hello, the battery's first answer and its changes, haptic requests to a counted motor,
// noise and broken frames on the line, the end of the stream. No phone, no Apple files.
#define _GNU_SOURCE
#include "bridges/stream.h"
#include "oneios_bridge_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void write_node(const char *dir, const char *name, const char *text)
{
  char path[600];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  if (!f || fputs(text, f) < 0 || fclose(f)) abort();
}

// ---- the frames ---------------------------------------------------------------------------------
static void test_frames(void)
{
  static struct obs_parser p;
  uint8_t frame[OBS_MAX_FRAME];
  // CRC-16/CCITT-FALSE's check value
  CHECK(obs_crc16(0xffff, (const uint8_t *)"123456789", 9) == 0x29b1);
  struct ob_haptic_vib vib = { 20, 700, 170 };
  size_t n = obs_encode(frame, OB_CH_HAPTIC, OB_HAPTIC_VIB, &vib, sizeof vib);
  CHECK(n == 12 + sizeof vib && frame[0] == 0x4f && frame[1] == 0x42 && frame[2] == OB_CH_HAPTIC && frame[4] == OB_HAPTIC_VIB && frame[6] == 8);
  int got = 0;
  for (size_t i = 0; i < n; i++) got += obs_push(&p, frame[i]);
  struct ob_header h;
  memcpy(&h, p.message, sizeof h);
  CHECK(got == 1 && p.have == 8 + sizeof vib && h.channel == OB_CH_HAPTIC && h.type == OB_HAPTIC_VIB && h.length == sizeof vib &&
        !memcmp(p.message + 8, &vib, sizeof vib));
  // no payload; the longest payload; one byte too long
  n = obs_encode(frame, OB_CH_BATTERY, OB_BATTERY_QUERY, NULL, 0);
  got = 0;
  for (size_t i = 0; i < n; i++) got += obs_push(&p, frame[i]);
  CHECK(n == 12 && got == 1 && p.have == 8);
  static uint8_t big[OB_MAX_PAYLOAD + 1];
  for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + 3);
  n = obs_encode(frame, OB_CH_HCI, 4, big, OB_MAX_PAYLOAD);
  got = 0;
  for (size_t i = 0; i < n; i++) got += obs_push(&p, frame[i]);
  CHECK(n == OBS_MAX_FRAME && got == 1 && p.have == OB_MAX_MESSAGE && !memcmp(p.message + 8, big, OB_MAX_PAYLOAD));
  CHECK(obs_encode(frame, OB_CH_HCI, 4, big, OB_MAX_PAYLOAD + 1) == 0);
  // noise before a frame (with a lone 0x4f in it), a changed byte, a length no message has: dropped, and the next frame is read
  const uint64_t frames = p.frames, dropped = p.dropped;
  const uint8_t noise[] = { 0x00, 0x4f, 0x4f, 0x13, 0x42, 0xff, 0x4f };
  for (size_t i = 0; i < sizeof noise; i++) CHECK(!obs_push(&p, noise[i]));
  n = obs_encode(frame, OB_CH_HAPTIC, OB_HAPTIC_STOP, NULL, 0);
  got = 0;
  for (size_t i = 0; i < n; i++) got += obs_push(&p, frame[i]);
  CHECK(got == 1 && p.frames == frames + 1);
  n = obs_encode(frame, OB_CH_HAPTIC, OB_HAPTIC_VIB, &vib, sizeof vib);
  frame[11] ^= 0x10;                                               // a payload byte
  got = 0;
  for (size_t i = 0; i < n; i++) got += obs_push(&p, frame[i]);
  CHECK(got == 0 && p.dropped == dropped + 1);
  const uint8_t huge[] = { 0x4f, 0x42, 1, 0, 1, 0, 0xff, 0xff, 0xff, 0x7f };      // length 0x7fffffff
  for (size_t i = 0; i < sizeof huge; i++) CHECK(!obs_push(&p, huge[i]));
  CHECK(p.dropped == dropped + 2);
  n = obs_encode(frame, OB_CH_HAPTIC, OB_HAPTIC_VIB, &vib, sizeof vib);
  got = 0;
  for (size_t i = 0; i < n; i++) got += obs_push(&p, frame[i]);
  CHECK(got == 1 && p.frames == frames + 2);
  // a frame cut off by the start of the next one: the cut one is lost, the one after it is read again
  n = obs_encode(frame, OB_CH_HAPTIC, OB_HAPTIC_VIB, &vib, sizeof vib);
  for (size_t i = 0; i < 9; i++) (void)obs_push(&p, frame[i]);
  got = 0;
  for (int round = 0; round < 3; round++) for (size_t i = 0; i < n; i++) got += obs_push(&p, frame[i]);
  CHECK(got >= 2);
}

// ---- the phone's end against a guest -------------------------------------------------------------
typedef struct { int fd; br_stream_config config; volatile int stop; br_stream_stats stats; int rc; } server;
static void *serve_thread(void *arg)
{
  server *s = arg;
  s->rc = br_stream_serve(s->fd, &s->config, &s->stop, &s->stats);
  return NULL;
}

static void guest_send(int fd, uint16_t channel, uint16_t type, const void *payload, uint32_t length)
{
  uint8_t frame[OBS_MAX_FRAME];
  size_t n = obs_encode(frame, channel, type, payload, length);
  // a byte at a time, as the kernel writes the port's transmit register
  for (size_t i = 0; i < n; i++) if (write(fd, frame + i, 1) != 1) abort();
}

// The next whole message from the phone's end within ms; its size, or -1.
static int guest_recv(int fd, struct obs_parser *p, int ms)
{
  for (int waited = 0; waited < ms;) {
    struct pollfd w = { fd, POLLIN, 0 };
    if (poll(&w, 1, 10) <= 0) { waited += 10; continue; }
    uint8_t byte;
    if (read(fd, &byte, 1) != 1) return -1;
    if (obs_push(p, byte)) return (int)p->have;
  }
  return -1;
}

static void test_serve(const char *tmp)
{
  char root[400], dir[440], usb[440];
  snprintf(root, sizeof root, "%s/power_supply", tmp);
  snprintf(dir, sizeof dir, "%s/battery", root);
  snprintf(usb, sizeof usb, "%s/usb", root);
  mkdir(root, 0755); mkdir(dir, 0755); mkdir(usb, 0755);
  write_node(dir, "capacity", "73\n");
  write_node(dir, "status", "Discharging\n");
  write_node(dir, "present", "1\n");
  write_node(dir, "temp", "305\n");
  write_node(dir, "voltage_now", "4012000\n");
  write_node(dir, "current_now", "-400000\n");
  write_node(dir, "charge_full", "5000000\n");
  write_node(usb, "online", "0\n");
  br_battery_options options = { .dir = root, .poll_ms = 10 };
  br_battery *battery = br_battery_open(&options);
  br_haptic *motor = br_haptic_open_sim();
  CHECK(battery != NULL && motor != NULL);
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) abort();
  server s = { .fd = pair[0], .config = { .haptic = motor, .battery = battery } };
  pthread_t thread;
  if (pthread_create(&thread, NULL, serve_thread, &s)) abort();
  const int g = pair[1];
  static struct obs_parser rx;

  // the hello: the channels this end serves, and no other
  struct ob_ctl_hello hello = { OB_PROTO_VERSION, 0 };
  guest_send(g, OB_CH_CONTROL, OB_CTL_HELLO, &hello, sizeof hello);
  CHECK(guest_recv(g, &rx, 2000) == 8 + (int)sizeof hello);
  struct ob_header h;
  memcpy(&h, rx.message, 8); memcpy(&hello, rx.message + 8, sizeof hello);
  CHECK(h.channel == OB_CH_CONTROL && h.type == OB_CTL_HELLO && hello.version == OB_PROTO_VERSION);
  CHECK(hello.channels == (1u << OB_CH_CONTROL | 1u << OB_CH_HAPTIC | 1u << OB_CH_BATTERY));

  // the battery: the real level and state at the query, then each change by itself
  guest_send(g, OB_CH_BATTERY, OB_BATTERY_QUERY, NULL, 0);
  struct ob_battery_state state;
  CHECK(guest_recv(g, &rx, 2000) == 8 + (int)sizeof state);
  memcpy(&h, rx.message, 8); memcpy(&state, rx.message + 8, sizeof state);
  CHECK(h.channel == OB_CH_BATTERY && h.type == OB_BATTERY_STATE && h.length == sizeof state);
  CHECK(state.flags == OB_BATTERY_PRESENT && state.percent == 73 && state.temp_deci_c == 305 && state.voltage_mv == 4012 && state.current_ma == -400 &&
        state.full_mah == 5000);
  write_node(dir, "status", "Charging\n");
  write_node(usb, "online", "1\n");
  CHECK(guest_recv(g, &rx, 4000) == 8 + (int)sizeof state);
  memcpy(&state, rx.message + 8, sizeof state);
  CHECK((state.flags & (OB_BATTERY_PLUGGED | OB_BATTERY_CHARGING)) == (OB_BATTERY_PLUGGED | OB_BATTERY_CHARGING) && state.percent == 73);
  write_node(dir, "capacity", "74\n");
  CHECK(guest_recv(g, &rx, 4000) == 8 + (int)sizeof state);
  memcpy(&state, rx.message + 8, sizeof state);
  CHECK(state.percent == 74);

  // haptics: every request reaches the motor, the second and the tenth as the first (noise and a broken frame between)
  struct ob_haptic_vib vib = { 20, 800, 0 };
  for (int i = 0; i < 10; i++) {
    guest_send(g, OB_CH_HAPTIC, OB_HAPTIC_VIB, &vib, sizeof vib);
    br_haptic_stats st = { 0 };
    for (int waited = 0; waited < 2000; waited += 5) { br_haptic_stats_get(motor, &st); if (st.vib == (uint64_t)i + 1) break; usleep(5000); }
    CHECK(st.vib == (uint64_t)i + 1);
    br_haptic_flush(motor);
    if (i == 4) {
      const uint8_t junk[] = { 0x4f, 0x42, 1, 0, 1, 0, 8, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0xaa, 0x4f };   // a wrong checksum, then noise
      if (write(g, junk, sizeof junk) != (ssize_t)sizeof junk) abort();
    }
  }
  br_haptic_stats st;
  br_haptic_stats_get(motor, &st);
  CHECK(st.vib == 10 && st.played == 10 && st.failed == 0);
  guest_send(g, OB_CH_HAPTIC, OB_HAPTIC_STOP, NULL, 0);
  // a channel this end does not serve is counted and answered by nothing; the next hello is answered again
  guest_send(g, OB_CH_THERMAL, OB_THERMAL_QUERY, NULL, 0);
  hello = (struct ob_ctl_hello){ OB_PROTO_VERSION, 0 };
  guest_send(g, OB_CH_CONTROL, OB_CTL_HELLO, &hello, sizeof hello);
  int size;
  do size = guest_recv(g, &rx, 2000); while (size > 0 && (memcpy(&h, rx.message, 8), h.channel == OB_CH_BATTERY));   // a report on its way
  CHECK(size == 8 + (int)sizeof hello && h.channel == OB_CH_CONTROL);

  // the VM ends: the guest's side closes; the server returns 0, the motor is stopped, the battery is not read any more
  close(g);
  pthread_join(thread, NULL);
  CHECK(s.rc == 0);
  CHECK(s.stats.hellos == 2 && s.stats.haptic == 11 && s.stats.battery_queries == 1 && s.stats.battery_states >= 3 && s.stats.unknown == 1);
  CHECK(s.stats.frames_in == 2 + 11 + 1 + 1 && s.stats.frames_bad == 1 && s.stats.bytes_out > 0 && s.stats.bytes_in > 0);
  br_haptic_flush(motor);
  br_haptic_stats_get(motor, &st);
  CHECK(st.stop >= 1);
  close(pair[0]);

  // the stop flag ends a stream nobody closes; a guest that never says hello is served nothing
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) abort();
  server quiet = { .fd = pair[0], .config = { .battery = battery } };
  if (pthread_create(&thread, NULL, serve_thread, &quiet)) abort();
  usleep(50000);
  quiet.stop = 1;
  pthread_join(thread, NULL);
  CHECK(quiet.rc == 0 && quiet.stats.frames_in == 0 && quiet.stats.bytes_out == 0);
  close(pair[0]); close(pair[1]);
  br_haptic_close(motor);
  br_battery_close(battery);
}

// ---- bridgectl serve, as vm-boot starts it and crosvm reaches it ----------------------------------------------------
// The tool listens on its socket; the test connects as crosvm's serial port does, asks as the kernel does, and closes
// as crosvm does when the VM ends: the tool then says so and ends with 0.
static void test_cli(const char *tool, const char *tmp)
{
  char sock[440], root[400], out[440];
  snprintf(sock, sizeof sock, "%s/bridge.sock", tmp);
  snprintf(root, sizeof root, "%s/power_supply", tmp);
  snprintf(out, sizeof out, "%s/bridge.txt", tmp);
  pid_t pid = fork();
  if (pid < 0) abort();
  if (!pid) {
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) _exit(126);
    setenv("ONEIOS_BATTERY_DIR", root, 1);
    setenv("ASAN_OPTIONS", "detect_leaks=0", 1);
    execl(tool, tool, "serve", sock, "--wait", "20", "--no-haptics", (char *)NULL);
    _exit(127);
  }
  struct stat st;
  for (int waited = 0; waited < 10000 && (stat(sock, &st) || !S_ISSOCK(st.st_mode)); waited += 10) usleep(10000);
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (strlen(sock) >= sizeof addr.sun_path) abort();
  strcpy(addr.sun_path, sock);
  int g = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(g >= 0 && !connect(g, (struct sockaddr *)&addr, sizeof addr));
  static struct obs_parser rx;
  struct ob_ctl_hello hello = { OB_PROTO_VERSION, 0 };
  guest_send(g, OB_CH_CONTROL, OB_CTL_HELLO, &hello, sizeof hello);
  CHECK(guest_recv(g, &rx, 5000) == 8 + (int)sizeof hello);
  memcpy(&hello, rx.message + 8, sizeof hello);
  CHECK(hello.channels == (1u << OB_CH_CONTROL | 1u << OB_CH_BATTERY));       // --no-haptics: the channel is not offered
  guest_send(g, OB_CH_BATTERY, OB_BATTERY_QUERY, NULL, 0);
  struct ob_battery_state state;
  CHECK(guest_recv(g, &rx, 5000) == 8 + (int)sizeof state);
  memcpy(&state, rx.message + 8, sizeof state);
  CHECK(state.percent == 74 && (state.flags & OB_BATTERY_CHARGING));
  close(g);
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  char text[4096] = "";
  FILE *f = fopen(out, "r");
  if (f) { size_t n = fread(text, 1, sizeof text - 1, f); text[n] = 0; fclose(f); }
  CHECK(strstr(text, "serve: listening on ") && strstr(text, "battery served, haptics off (--no-haptics)"));
  CHECK(strstr(text, "serve: crosvm connected") && strstr(text, "serve: crosvm closed the port (the VM ended): 2 messages from the guest (1 hello, 0 haptic, 1 battery queries"));
  CHECK(access(sock, F_OK) != 0);                                           // the socket's name is gone once it is taken
}

int main(int argc, char **argv)
{
  char tmp[] = "/tmp/oneios-stream-XXXXXX";
  if (!mkdtemp(tmp)) return 1;
  test_frames();
  test_serve(tmp);
  if (argc > 1) test_cli(argv[1], tmp);
  char cmd[300];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
  (void)!system(cmd);
  printf("stream: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
