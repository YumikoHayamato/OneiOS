// SPDX-License-Identifier: MIT
// Host tests of the display helper's core: the display.sock protocol against a serving thread
// (what xnuhost does), the letterbox fit, the damage rectangles, the row copy; and displayctl
// against a stand-in helper. No phone, no Apple files.
//   test_display [path to a host build of displayctl]
#define _GNU_SOURCE
#include "xhd.h"
#include "boost.h"
#include "scantime.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/un.h>

static int checks, failed;
static int as_helper(const char *session);
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_fit(void)
{
  struct xhd_fit f;
  xhd_fit(1440, 3120, 1440, 3120, &f);                 // the guest at the panel's native mode: 1:1, no bars
  CHECK(f.w == 1440 && f.h == 3120 && f.x == 0 && f.y == 0);
  xhd_fit(1440, 3120, 1080, 2340, &f);                 // the panel left at FHD+ (the native mode was refused): scaled, same shape
  CHECK(f.w == 1080 && f.h == 2340 && f.x == 0 && f.y == 0);
  xhd_fit(1320, 2868, 1440, 3120, &f);                 // a mismatch: the iPhone's own frame on the S24 Ultra's panel
  CHECK(f.w == 1436 && f.h == 3120 && f.x == 2 && f.y == 0);
  xhd_fit(1320, 2868, 1080, 2340, &f);                 // One UI at FHD+
  CHECK(f.w == 1077 && f.h == 2340 && f.x == 1 && f.y == 0);
  xhd_fit(1440, 3120, 1440, 3120, &f);
  CHECK(f.w == 1440 && f.h == 3120 && f.x == 0 && f.y == 0);
  xhd_fit(720, 1560, 1440, 3120, &f);                  // same aspect, scaled up
  CHECK(f.w == 1440 && f.h == 3120 && f.x == 0 && f.y == 0);
  xhd_fit(1920, 1080, 1440, 3120, &f);                 // wide in tall: bars above and below
  CHECK(f.w == 1440 && f.h == 810 && f.x == 0 && f.y == 1155);
  xhd_fit(1320, 2868, 3120, 1440, &f);                 // tall in wide: bars left and right
  CHECK(f.h == 1440 && f.w == 663 && f.x == 1228 && f.y == 0);
  xhd_fit(1, 10000, 100, 100, &f);                     // never thinner than a pixel
  CHECK(f.w == 1 && f.h == 100);
  xhd_fit(16384, 16384, 16384, 16384, &f);             // no overflow at the largest sizes
  CHECK(f.w == 16384 && f.h == 16384);
  xhd_fit(0, 10, 100, 100, &f);
  CHECK(f.w == 0 && f.h == 0);
  for (uint32_t sw = 1; sw < 400; sw += 7) for (uint32_t sh = 1; sh < 400; sh += 11) {
    xhd_fit(sw, sh, 144, 312, &f);
    if (!(f.w >= 1 && f.h >= 1 && f.x + f.w <= 144 && f.y + f.h <= 312 && (f.w == 144 || f.h == 312))) { CHECK(!"fit inside the panel, touching two edges"); return; }
  }
  CHECK(1);
}

static void test_rects(void)
{
  struct xhd_rect r = { 10, 20, 30, 40 };
  CHECK(xhd_rect_clip(&r, 100, 100) && r.x == 10 && r.y == 20 && r.w == 30 && r.h == 40);
  r = (struct xhd_rect){ 90, 95, 30, 40 };
  CHECK(xhd_rect_clip(&r, 100, 100) && r.w == 10 && r.h == 5);
  r = (struct xhd_rect){ 100, 0, 1, 1 };
  CHECK(!xhd_rect_clip(&r, 100, 100) && r.w == 0);
  r = (struct xhd_rect){ 5, 5, 0xffffffffu, 0xffffffffu };              // a width that would wrap
  CHECK(xhd_rect_clip(&r, 100, 100) && r.w == 95 && r.h == 95);
  r = (struct xhd_rect){ 0xfffffff0u, 0, 0x20, 1 };
  CHECK(!xhd_rect_clip(&r, 100, 100));
  r = (struct xhd_rect){ 0, 0, 0, 5 };
  CHECK(!xhd_rect_clip(&r, 100, 100));
  struct xhd_rect u = { 0, 0, 0, 0 }, a = { 10, 10, 5, 5 }, b = { 50, 2, 10, 10 }, none = { 7, 7, 0, 0 };
  xhd_rect_union(&u, &a);
  CHECK(u.x == 10 && u.y == 10 && u.w == 5 && u.h == 5);
  xhd_rect_union(&u, &b);
  CHECK(u.x == 10 && u.y == 2 && u.w == 50 && u.h == 13);
  xhd_rect_union(&u, &none);
  CHECK(u.x == 10 && u.y == 2 && u.w == 50 && u.h == 13);
}

static void test_hello_check(void)
{
  struct xhd_message m = { { 'X', 'H', 'D', '1' }, 1320, 2868, 5280, 1 };
  off_t size = (off_t)5280 * 2868;
  CHECK(xhd_check_hello(&m, sizeof m, size) == 0);
  CHECK(xhd_check_hello(&m, sizeof m, size + 4096) == 0);
  CHECK(xhd_check_hello(&m, sizeof m, size - 1) == -EINVAL);            // a frame larger than its memfd
  CHECK(xhd_check_hello(&m, sizeof m - 4, size) == -EPROTO);
  struct xhd_message bad = m;
  bad.magic[3] = 'F';
  CHECK(xhd_check_hello(&bad, sizeof bad, size) == -EPROTO);
  bad = m; bad.a = 0;
  CHECK(xhd_check_hello(&bad, sizeof bad, size) == -EINVAL);
  bad = m; bad.b = 20000;
  CHECK(xhd_check_hello(&bad, sizeof bad, (off_t)1 << 40) == -EINVAL);
  bad = m; bad.c = 5276;                                                // a stride shorter than a row
  CHECK(xhd_check_hello(&bad, sizeof bad, size) == -EINVAL);
  bad = m; bad.c = 5281;
  CHECK(xhd_check_hello(&bad, sizeof bad, size * 2) == -EINVAL);
  bad = m; bad.d = 3;                                                   // a format this helper does not know
  CHECK(xhd_check_hello(&bad, sizeof bad, size) == -EINVAL);
  bad = m; bad.d = 0;
  CHECK(xhd_check_hello(&bad, sizeof bad, size) == -EINVAL);
  bad = m; bad.a = 0x40000001u;                                         // width * 4 wraps to 4
  CHECK(xhd_check_hello(&bad, sizeof bad, size) == -EINVAL);
  // Half floats: 8 bytes a pixel, the stride a whole number of them.
  struct xhd_message half = { { 'X', 'H', 'D', '1' }, 1440, 3120, 11520, XHD_FORMAT_RGBA16F };
  CHECK(xhd_check_hello(&half, sizeof half, (off_t)11520 * 3120) == 0);
  CHECK(xhd_check_hello(&half, sizeof half, (off_t)11520 * 3120 - 1) == -EINVAL);
  bad = half; bad.c = 5760;                                             // a B,G,R,X stride: a half-float row does not fit
  CHECK(xhd_check_hello(&bad, sizeof bad, (off_t)11520 * 3120) == -EINVAL);
  bad = half; bad.c = 11524;                                            // not a whole number of pixels
  CHECK(xhd_check_hello(&bad, sizeof bad, (off_t)11524 * 3120) == -EINVAL);
}

// The frame formats' buffers and dataspaces, the names of --dataspace, and the half-float conversions the copy uses.
static void test_formats(void)
{
  int ds = -5;
  CHECK(xhd_buffer_for(XHD_FORMAT_BGRX, &ds) == XHD_BUFFER_BGRA_8888 && ds == XHD_DATASPACE_SRGB);
  CHECK(xhd_buffer_for(XHD_FORMAT_RGBA16F, &ds) == XHD_BUFFER_RGBA_FP16 && ds == XHD_DATASPACE_SCRGB_LINEAR);
  CHECK(xhd_buffer_for(7, &ds) == 0 && ds == XHD_DATASPACE_UNKNOWN);
  CHECK(xhd_dataspace_parse("auto") == -1 && xhd_dataspace_parse("p3") == XHD_DATASPACE_DISPLAY_P3 &&
        xhd_dataspace_parse("scrgb-linear") == XHD_DATASPACE_SCRGB_LINEAR && xhd_dataspace_parse("bt709") == -2);
  CHECK(!strcmp(xhd_dataspace_name(XHD_DATASPACE_SRGB), "srgb") && !strcmp(xhd_dataspace_name(1234), "other"));
  // Android's values, from its headers (data_space.h): standard << 16 | transfer << 22 | range << 27.
  CHECK(XHD_DATASPACE_SRGB == (1 << 16 | 2 << 22 | 1 << 27) && XHD_DATASPACE_DISPLAY_P3 == (10 << 16 | 2 << 22 | 1 << 27) &&
        XHD_DATASPACE_SCRGB == (1 << 16 | 2 << 22 | 3 << 27) && XHD_DATASPACE_SCRGB_LINEAR == (1 << 16 | 1 << 22 | 3 << 27));
  // Halves: exact values, rounding, the ends.
  CHECK(xh_half_from_float(1.0f) == 0x3c00 && xh_half_from_float(-2.0f) == 0xc000 && xh_half_from_float(0.5f) == 0x3800);
  CHECK(xh_half_from_float(65504.0f) == 0x7bff && xh_half_from_float(1e6f) == 0x7c00 && xh_half_from_float(0.0f) == 0);
  CHECK(xh_half_from_float(5.9604645e-8f) == 0x0001);                  // the smallest subnormal
  CHECK((xh_half_from_float(__builtin_nanf("")) & 0x7fff) > 0x7c00);   // a NaN stays one
  int round_trip = 1;
  for (unsigned h = 0; h < 0x7c00; h++) if (xh_half_from_float(xh_half_to_float((uint16_t)h)) != h) round_trip = 0;
  CHECK(round_trip);                                                    // every finite positive half
  CHECK(xh_half_to_float(0x3c00) == 1.0f && xh_half_to_float(0xbc00) == -1.0f && xh_half_to_float(0x0400) == 6.1035156e-5f);
  // sRGB: 8-bit -> linear -> 8-bit gives the byte back; out of range is clamped.
  const uint16_t *lin = xh_srgb8_to_half();
  const uint8_t *enc = xh_half_to_srgb8();
  int back = 1;
  for (unsigned i = 0; i < 256; i++) if (enc[lin[i]] != i) back = 0;
  CHECK(back);
  CHECK(lin[0] == 0 && lin[255] == 0x3c00 && enc[0xbc00] == 0 && enc[0x4000] == 255 && enc[0x7c00] == 255 && enc[0x7e00] == 0);
  CHECK(lin[128] == xh_half_from_float(0.21586050f));                  // sRGB's 128 is 21.6% of the light
}

// The copy between frame formats and buffer layouts.
static void test_copy_formats(void)
{
  enum { W = 3, H = 2 };
  uint16_t half[H][W][4], out16[H][W][4];
  const float values[W][3] = { { 1.0f, 0.0f, 0.0f }, { 1.2249f, -0.042f, -0.0197f }, { 2.0f, 0.5f, 0.2158605f } };
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
    for (int c = 0; c < 3; c++) half[y][x][c] = xh_half_from_float(values[x][c]);
    half[y][x][3] = 0x3c00;
  }
  struct xhd_rect all = { 0, 0, W, H };
  memset(out16, 0, sizeof out16);
  CHECK(xhd_copy((uint8_t *)out16, W * 8, XHD_BUFFER_RGBA_FP16, (const uint8_t *)half, W * 8, XHD_FORMAT_RGBA16F, all) == 0);
  CHECK(!memcmp(out16, half, sizeof half));                             // half floats into RGBA_FP16: as they are
  uint8_t out8[H][W][4];
  CHECK(xhd_copy((uint8_t *)out8, W * 4, XHD_BUFFER_BGRA_8888, (const uint8_t *)half, W * 8, XHD_FORMAT_RGBA16F, all) == 0);
  // into 8-bit: encoded and clamped, so sRGB's red and P3's red (outside sRGB) become the same 255,0,0
  CHECK(out8[0][0][0] == 0 && out8[0][0][1] == 0 && out8[0][0][2] == 255 && out8[0][0][3] == 255);
  CHECK(out8[0][1][0] == 0 && out8[0][1][1] == 0 && out8[0][1][2] == 255);
  CHECK(out8[1][2][2] == 255 && out8[1][2][1] == 188 && out8[1][2][0] == 128);   // 2.0 clamps; 0.5 is sRGB 188; 0.2159 is 128
  CHECK(xhd_copy((uint8_t *)out8, W * 4, XHD_BUFFER_RGBX_8888, (const uint8_t *)half, W * 8, XHD_FORMAT_RGBA16F, all) == 0);
  CHECK(out8[1][2][0] == 255 && out8[1][2][1] == 188 && out8[1][2][2] == 128 && out8[1][2][3] == 255);
  // 8-bit into RGBA_FP16: decoded to linear
  uint8_t bgrx[1][2][4] = { { { 0, 128, 255, 7 }, { 255, 255, 255, 0 } } };
  uint16_t got[1][2][4];
  struct xhd_rect two = { 0, 0, 2, 1 };
  CHECK(xhd_copy((uint8_t *)got, 16, XHD_BUFFER_RGBA_FP16, (const uint8_t *)bgrx, 8, XHD_FORMAT_BGRX, two) == 0);
  CHECK(got[0][0][0] == 0x3c00 && got[0][0][1] == xh_half_from_float(0.21586050f) && got[0][0][2] == 0 && got[0][0][3] == 0x3c00);
  CHECK(got[0][1][0] == 0x3c00 && got[0][1][1] == 0x3c00 && got[0][1][2] == 0x3c00);
  CHECK(xhd_copy((uint8_t *)got, 16, 99, (const uint8_t *)bgrx, 8, XHD_FORMAT_BGRX, two) == -EINVAL);
  CHECK(xhd_copy((uint8_t *)got, 16, XHD_BUFFER_RGBA_FP16, (const uint8_t *)bgrx, 8, 9, two) == -EINVAL);
}

static void test_copy(void)
{
  enum { W = 7, H = 5, SS = 40, DS = 32 };                              // strides wider than the rows
  uint8_t src[SS * H], dst[DS * H], want[DS * H];
  for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 7 + 1);
  memset(dst, 0xee, sizeof dst);
  memset(want, 0xee, sizeof want);
  struct xhd_rect r = { 2, 1, 4, 3 };
  CHECK(xhd_copy(dst, DS, XHD_BUFFER_BGRA_8888, src, SS, XHD_FORMAT_BGRX, r) == 0);
  for (uint32_t y = 1; y < 4; y++) memcpy(want + y * DS + 8, src + y * SS + 8, 16);
  CHECK(!memcmp(dst, want, sizeof dst));                                // only the rectangle, bytes as they are
  CHECK(xhd_copy(dst, DS, XHD_BUFFER_RGBX_8888, src, SS, XHD_FORMAT_BGRX, r) == 0);
  int ok = 1;
  for (uint32_t y = 1; y < 4; y++) for (uint32_t x = 2; x < 6; x++) {
    const uint8_t *s = src + y * SS + x * 4, *d = dst + y * DS + x * 4;
    if (d[0] != s[2] || d[1] != s[1] || d[2] != s[0] || d[3] != 0xff) ok = 0;
  }
  CHECK(ok);                                                            // B and R swapped, alpha opaque
  CHECK(dst[0] == 0xee && dst[DS * 4] == 0xee && dst[DS + 7] == 0xee && dst[DS + 24] == 0xee);
}

// ---- the protocol against a serving thread --------------------------------------------------
struct server { char path[200]; int delay_ms; uint32_t width, height; int mode; int done; };

static void *serve(void *arg)
{
  struct server *s = arg;
  usleep((useconds_t)s->delay_ms * 1000);                               // the helper is already waiting for the socket
  int listener = xhd_listen(s->path);
  int fd = accept(listener, NULL, NULL);
  size_t bytes = (size_t)s->width * 4 * s->height;
  int memfd = xhd_memfd("test-frame", s->mode == 1 ? bytes / 2 : bytes);
  if (s->mode != 1) {
    uint8_t *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    for (size_t i = 0; i < bytes; i++) p[i] = (uint8_t)(i % 251);
    munmap(p, bytes);
  }
  if (s->mode == 2) {                                                   // a hello with no descriptor
    struct xhd_message m = { { 'X', 'H', 'D', '1' }, s->width, s->height, s->width * 4, 1 };
    send(fd, &m, sizeof m, 0);
  } else xhd_send_hello(fd, s->width, s->height, s->width * 4, XHD_FORMAT_BGRX, memfd);
  if (s->mode == 0) {
    xhd_send_flush(fd, 0, 0, s->width, s->height);                      // what xnuhost sends at a connection
    char ack;
    recv(fd, &ack, 1, 0);                                               // the test read the full flush
    xhd_send_flush(fd, 10, 10, 20, 20);
    xhd_send_flush(fd, 100, 5, 20, 20);
    xhd_send_flush(fd, s->width - 4, s->height - 4, 50, 50);            // reaches past the frame: clipped
    xhd_send_flush(fd, s->width + 5, 0, 5, 5);                          // outside: dropped
    struct xhd_message junk = { { 'N', 'O', 'P', 'E' }, 0, 0, 9999, 9999 };
    send(fd, &junk, sizeof junk, 0);                                    // unknown: ignored
    send(fd, "short", 5, 0);
    recv(fd, &ack, 1, 0);
  }
  close(memfd);
  close(fd);
  close(listener);
  s->done = 1;
  return NULL;
}

static void test_panel_report(void)
{
  int pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
  CHECK(xhd_send_panel(pair[0], 1440, 3120, 120) == 0);
  struct xhd_message m;
  CHECK(recv(pair[1], &m, sizeof m, 0) == (ssize_t)sizeof m);
  CHECK(!memcmp(m.magic, "XHDP", 4) && m.a == 1440 && m.b == 3120 && m.c == 120 && m.d == 0);
  close(pair[0]);
  close(pair[1]);
}

// The first post waits for SESSION/ready (2026-11-15: the layer covers the OneOS boot screen, and the frames before
// ready are black or the console): held while it is missing, not once it is there, never without a session.
static void test_hold_first(const char *dir)
{
  char ready[300];
  snprintf(ready, sizeof ready, "%s/ready", dir);
  unlink(ready);
  CHECK(xhd_hold_first(ready) == 1);
  CHECK(xhd_hold_first(NULL) == 0 && xhd_hold_first("") == 0);                 // --socket: no hold
  FILE *f = fopen(ready, "w");
  CHECK(f && fputs("ready\n", f) >= 0 && fclose(f) == 0);
  CHECK(xhd_hold_first(ready) == 0);                                           // xnuhost said ready
  unlink(ready);
  CHECK(mkdir(ready, 0700) == 0 && xhd_hold_first(ready) == 0 && rmdir(ready) == 0);   // whatever it is: there
}

static void test_protocol(const char *dir)
{
  struct server s = { .delay_ms = 300, .width = 64, .height = 48 };
  snprintf(s.path, sizeof s.path, "%s/display.sock", dir);
  pthread_t thread;
  pthread_create(&thread, NULL, serve, &s);
  CHECK(xhd_connect(s.path, 0, NULL) == -ENOENT);                       // not there yet, and no wait asked
  int fd = xhd_connect(s.path, 5000, NULL);                             // waits for xnuhost to make it
  CHECK(fd >= 0);
  struct xhd_frame frame;
  CHECK(xhd_recv_hello(fd, &frame, 5000) == 0);
  CHECK(frame.width == 64 && frame.height == 48 && frame.stride == 256 && frame.format == XHD_FORMAT_BGRX && frame.bytes == 256 * 48);
  int same = 1;
  for (size_t i = 0; i < frame.bytes; i++) if (frame.pixels[i] != (uint8_t)(i % 251)) same = 0;
  CHECK(same);                                                          // the shared frame, mapped
  struct xhd_rect damage = { 0, 0, 0, 0 };
  for (int i = 0; i < 200 && !damage.w; i++) { CHECK(xhd_drain(fd, &frame, &damage) == 0); usleep(10000); }
  CHECK(damage.x == 0 && damage.y == 0 && damage.w == 64 && damage.h == 48);
  damage = (struct xhd_rect){ 0, 0, 0, 0 };
  CHECK(xhd_drain(fd, &frame, &damage) == 0 && damage.w == 0);          // nothing pending, and it did not block
  send(fd, "a", 1, 0);
  usleep(200000);
  CHECK(xhd_drain(fd, &frame, &damage) == 0);
  // 10,10 20x20 + 100,5 20x20 (clipped to 64 wide: dropped, x >= width) + the corner 60,44 4x4
  CHECK(damage.x == 10 && damage.y == 10 && damage.w == 54 && damage.h == 38);
  send(fd, "b", 1, 0);
  pthread_join(thread, NULL);
  damage = (struct xhd_rect){ 0, 0, 0, 0 };
  CHECK(xhd_drain(fd, &frame, &damage) == -1);                          // xnuhost closed: the session is over
  xhd_frame_close(&frame);
  CHECK(frame.pixels == NULL && frame.memfd == -1);
  close(fd);

  // Refused hellos: a frame larger than its memfd, and no memfd at all.
  for (int mode = 1; mode <= 2; mode++) {
    s = (struct server){ .delay_ms = 0, .width = 64, .height = 48, .mode = mode };
    snprintf(s.path, sizeof s.path, "%s/display.sock", dir);
    pthread_create(&thread, NULL, serve, &s);
    fd = xhd_connect(s.path, 5000, NULL);
    CHECK(fd >= 0);
    int e = xhd_recv_hello(fd, &frame, 5000);
    CHECK(e == (mode == 1 ? -EINVAL : -EPROTO));
    CHECK(frame.pixels == NULL);
    close(fd);
    pthread_join(thread, NULL);
  }

  // Not a socket: a link (even to a socket) and a plain file are refused; a stop ends the wait.
  char other[256], link_path[256];
  snprintf(other, sizeof other, "%s/real.sock", dir);
  snprintf(link_path, sizeof link_path, "%s/link.sock", dir);
  int listener = xhd_listen(other);
  CHECK(listener >= 0);
  CHECK(symlink(other, link_path) == 0);
  CHECK(xhd_connect(link_path, 0, NULL) == -ENOTSOCK);
  close(listener);
  unlink(link_path);
  unlink(other);
  FILE *plain = fopen(link_path, "w");
  fclose(plain);
  CHECK(xhd_connect(link_path, 1000, NULL) == -ENOTSOCK);
  unlink(link_path);
  volatile int stop = 1;
  CHECK(xhd_connect(link_path, 60000, &stop) == -ENOENT);               // returns at once
  // A socket nobody listens on any more: refused, after the wait.
  listener = xhd_listen(other);
  close(listener);
  CHECK(xhd_connect(other, 200, NULL) == -ECONNREFUSED);
  unlink(other);
  // A hello that never comes.
  int pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
  CHECK(xhd_recv_hello(pair[0], &frame, 100) == -ETIMEDOUT);
  close(pair[1]);
  CHECK(xhd_recv_hello(pair[0], &frame, 100) == -ECONNRESET);
  close(pair[0]);
  unlink(s.path);
}

// The pool bridge's pure parts (displayctl pool): crosvm's pool mapping in a maps text, the row copy, the frame count.
static void test_pool_parts(void)
{
  // crosvm's own lines on the S25 (vm-boot maps1, 2026-11-23: the pool is the memfd's last 8 MiB, mapped on its own),
  // and look-alikes that must not count: a private mapping, another memfd's name, an anonymous one
  static const char maps[] =
    "59b38d8000-59b4507000 r-xp 00000000 07:c0 429696                         /apex/com.android.virt/bin/crosvm\n"
    "6e50600000-6e50e00000 rw-s 7f800000 00:01 2460461                        /memfd:crosvm_guest (deleted)\n"
    "6e50e00000-6ed0600000 rw-s 00000000 00:01 2460461                        /memfd:crosvm_guest (deleted)\n";
  static const char others[] =
    "7000000000-7000800000 rw-p 7f800000 00:01 2460462                        /memfd:crosvm_guest (deleted)\n"
    "7100000000-7100800000 rw-s 7f800000 00:01 2460463                        /memfd:crosvm_guest2\n"
    "7200000000-7200800000 rw-s 7f800000 00:00 0 \n";
  uint64_t start = 0, end = 0, inode = 0;
  CHECK(xhd_pool_mapping(maps, 0x7f800000, 8 << 20, 0, &start, &end, &inode) == 0 && start == 0x6e50600000ull &&
        end == 0x6e50e00000ull && inode == 2460461);
  CHECK(xhd_pool_mapping(maps, 0x7f800000, 16 << 20, 0, &start, &end, &inode) == -ENOENT);   // not the pool's size
  CHECK(xhd_pool_mapping(maps, 0, 8 << 20, 0, &start, &end, &inode) == -ENOENT);            // the lent RAM's is longer
  CHECK(xhd_pool_mapping(others, 0x7f800000, 8 << 20, 0, &start, &end, &inode) == -ENOENT);
  // Droid-VM's shared-RAM VM: its devices' region inside crosvm's mapping of the window (the memfd from 6 MiB), which
  // counts only when the caller says the VM is one: a protected VM's RAM must never
  static const char shared[] =
    "76a8000000-76a8400000 rw-s 00000000 00:01 3000001                        /memfd:crosvm_guest (deleted)\n"
    "76a8600000-7728000000 rw-s 00600000 00:01 3000001                        /memfd:crosvm_guest (deleted)\n";
  CHECK(xhd_pool_mapping(shared, 0x7e000000, 32 << 20, 1, &start, &end, &inode) == 0 && start == 0x76a8600000ull &&
        end == 0x7728000000ull && inode == 3000001);
  CHECK(xhd_pool_mapping(shared, 0x7e000000, 32 << 20, 0, &start, &end, &inode) == -ENOENT);
  CHECK(xhd_pool_mapping(shared, 0x7fe00000, 32 << 20, 1, &start, &end, &inode) == -ENOENT);  // past the window's end
  CHECK(xhd_pool_mapping(shared, 0x200000, 1 << 20, 1, &start, &end, &inode) == 0 && start == 0x76a8000000ull);
  CHECK(xhd_pool_mapping(shared, 0x300000, 2 << 20, 1, &start, &end, &inode) == -ENOENT);     // across two mappings
  CHECK(xhd_pool_mapping(others, 0x7f800000, 8 << 20, 1, &start, &end, &inode) == -ENOENT);

  // The copy: 16x8 pixels, 64 bytes a row, four rows a 256-byte page; page 1 (rows 4-7) not resident.
  uint8_t src[512], dst[512];
  memset(src, 0, sizeof src);
  memset(dst, 0, sizeof dst);
  src[1 * 64 + 5] = 9;
  src[2 * 64] = 7;
  src[6 * 64] = 3;
  unsigned char resident[2] = { 1, 0 };
  struct xhd_rect r;
  CHECK(xhd_pool_copy(dst, src, 16, 8, 64, resident, 256, 0, &r) == 2 && r.x == 0 && r.y == 1 && r.w == 16 && r.h == 2);
  CHECK(dst[1 * 64 + 5] == 9 && dst[2 * 64] == 7 && dst[6 * 64] == 0);                   // the absent page is not read
  CHECK(xhd_pool_copy(dst, src, 16, 8, 64, resident, 256, 0, &r) == 0 && r.w == 0);       // nothing changed since
  resident[1] = 1;
  CHECK(xhd_pool_copy(dst, src, 16, 8, 64, resident, 256, 1, &r) == 1 && r.y == 6 && r.h == 1 && dst[6 * 64] == 3);
  // The count: only with its magic.
  uint64_t page[2] = { 0, 5 }, count = 0;
  CHECK(!xhd_frame_seq((const uint8_t *)page, &count));
  page[0] = XHD_FRAME_SEQ_MAGIC;
  CHECK(xhd_frame_seq((const uint8_t *)page, &count) && count == 5);
  // The touch frames crosvm's multi-touch device takes: positions, slots, ids, contact size, BTN_TOUCH, SYN_REPORT
  // as they are (a lift's tracking id -1 included); the touchscreen's other keys, EV_MSC and SYN_DROPPED not.
  struct xhd_vinput v;
  CHECK(sizeof v == 8);
  CHECK(xhd_touch_event(3, 0x35, 2047, &v) && v.type == 3 && v.code == 0x35 && v.value == 2047);
  CHECK(xhd_touch_event(3, 0x39, -1, &v) && v.value == 0xffffffffu);
  CHECK(xhd_touch_event(3, 0x2f, 9, &v) && xhd_touch_event(3, 0x30, 12, &v) && xhd_touch_event(1, 0x14a, 1, &v));
  CHECK(xhd_touch_event(0, 0, 0, &v) && v.type == 0 && v.code == 0);
  CHECK(!xhd_touch_event(1, 0x8f, 1, &v) && !xhd_touch_event(1, 0x145, 1, &v));     // KEY_WAKEUP, BTN_TOOL_FINGER
  CHECK(!xhd_touch_event(0, 3, 0, &v) && !xhd_touch_event(4, 5, 77, &v) && !xhd_touch_event(5, 0, 1, &v));
  // The side keys of a takeover: power (116), volume up (115) and down (114) go on as they come, autorepeats and other keys
  // not; a click of power switches the panel, a hold does not.
  struct xhd_vinput k[2];
  double down = 0;
  int toggle = 0;
  CHECK(xhd_side_key(1, 115, 1, 10.0, &down, &toggle, k) == 2 && k[0].type == 1 && k[0].code == 115 && k[0].value == 1 &&
        k[1].type == 0 && k[1].code == 0 && !toggle);
  CHECK(!xhd_side_key(1, 115, 2, 10.1, &down, &toggle, k) && xhd_side_key(1, 115, 0, 10.2, &down, &toggle, k) == 2 && !k[0].value);
  CHECK(xhd_side_key(1, 114, 1, 11.0, &down, &toggle, k) == 2 && !toggle && down == 0);
  CHECK(!xhd_side_key(1, 113, 1, 11.0, &down, &toggle, k) && !xhd_side_key(3, 116, 1, 11.0, &down, &toggle, k));
  CHECK(xhd_side_key(1, 116, 1, 12.0, &down, &toggle, k) == 2 && down == 12.0 && !toggle);
  CHECK(xhd_side_key(1, 116, 0, 12.3, &down, &toggle, k) == 2 && toggle == 1 && down == 0 && k[0].code == 116 && !k[0].value);
  CHECK(xhd_power_blanks_panel(1, 0) == 0 && xhd_power_blanks_panel(0, 1) == 0 && xhd_power_blanks_panel(1, 1) == 1);   // a short click: the key goes, the panel stays
  toggle = 0;
  CHECK(xhd_side_key(1, 116, 1, 20.0, &down, &toggle, k) == 2 && xhd_side_key(1, 116, 0, 23.5, &down, &toggle, k) == 2 && !toggle);
}

// The touch boost (displayctl touch --boost) on plain files for the clocks: its process as the forwarder starts it.
static void test_boost(const char *dir)
{
  char gpu[64], cpu[64], state[64], list[256], line[200];
  snprintf(gpu, sizeof gpu, "%s/gpu_min", dir);
  snprintf(cpu, sizeof cpu, "%s/cpu_min", dir);
  snprintf(state, sizeof state, "%s/boost.state", dir);
  snprintf(list, sizeof list, "%s=607000000,%s=1497600", gpu, cpu);
  struct boost b = { .state = state, .hold_s = 0.3, .settle_s = 0.4 };
  CHECK(!boost_parse(&b, list) && b.n == 2 && b.node[1].target == 1497600 && !strcmp(b.node[0].path, gpu));
  struct boost bad = { 0 };
  CHECK(boost_parse(&bad, "") && boost_parse(&bad, "/a") && boost_parse(&bad, "a=1") && boost_parse(&bad, "/a=0") && boost_parse(&bad, "/a=1x") && boost_parse(&bad, "/a b=1"));
  int make = open(gpu, O_CREAT | O_WRONLY, 0600); close(make);
  make = open(cpu, O_CREAT | O_WRONLY, 0600); close(make);
  boost_write(gpu, 160000000);
  boost_write(cpu, 2227200);                  // a minimum someone holds at the start only
  for (int round = 0; round < 3; round++) {
    int ends[2];
    CHECK(!pipe(ends));
    b.node[0].low = b.node[1].low = 0;
    pid_t child = fork();
    if (!child) {
      signal(SIGTERM, SIG_IGN);
      close(ends[1]);
      _exit(boost_run(&b, ends[0]));
    }
    close(ends[0]);
    CHECK(write(ends[1], "t", 1) == 1);
    usleep(150000);
    CHECK(boost_read(gpu) == 160000000 && access(state, F_OK));      // the start: only read
    if (!round) boost_write(cpu, 384000);
    usleep(200000);
    CHECK(write(ends[1], "t", 1) == 1);                               // read again, still at the start
    usleep(250000);
    CHECK(boost_read(gpu) == 160000000);
    CHECK(write(ends[1], "t", 1) == 1);
    usleep(150000);
    CHECK(boost_read(gpu) == 607000000 && boost_read(cpu) == 1497600 && !access(state, F_OK));
    FILE *f = fopen(state, "r");
    int cpu_line = 0, boot_line = 0;
    while (f && fgets(line, sizeof line, f)) {
      if (!strncmp(line, "boot ", 5) && strlen(line) > 20) boot_line = 1;
      if (strstr(line, "cpu_min 384000\n")) cpu_line = 1;
    }
    if (f) fclose(f);
    CHECK(boot_line && cpu_line);                                     // never the start's 2227200
    int status;
    if (!round) {
      // another touch holds it on, then the release, then a raise ended by the pipe's end while SIGTERM is ignored
      CHECK(write(ends[1], "t", 1) == 1);
      usleep(200000);
      CHECK(boost_read(gpu) == 607000000);
      usleep(350000);
      CHECK(boost_read(gpu) == 160000000 && boost_read(cpu) == 384000 && access(state, F_OK));
      CHECK(write(ends[1], "t", 1) == 1);
      usleep(100000);
      kill(child, SIGTERM);
      usleep(50000);
      CHECK(boost_read(gpu) == 607000000);
    }
    if (round == 2) kill(child, SIGKILL);
    close(ends[1]);
    CHECK(waitpid(child, &status, 0) == child);
    if (round == 2) {
      // killed outright: its file is there, and the forwarder puts the clocks back from it
      CHECK(WIFSIGNALED(status) && boost_read(gpu) == 607000000 && !access(state, F_OK));
      boost_restore_state(&b);
    } else {
      CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    CHECK(boost_read(gpu) == 160000000 && boost_read(cpu) == 384000 && access(state, F_OK));
  }
  // a minimum already at the target or above is left alone
  struct boost high = { .state = state };
  CHECK(!boost_parse(&high, list));
  boost_write(cpu, 2227200);
  high.raises = 1;
  boost_raise(&high);
  CHECK(boost_read(cpu) == 2227200 && boost_read(gpu) == 607000000);
  CHECK(!boost_release(&high) && boost_read(cpu) == 2227200 && boost_read(gpu) == 160000000);
  // no place for the state file: nothing is raised
  struct boost nowhere = { .state = "/nonexistent/boost.state", .told = 1 };
  CHECK(!boost_parse(&nowhere, list));
  boost_raise(&nowhere);
  CHECK(boost_read(gpu) == 160000000 && !nowhere.up);
  unlink(gpu);
  unlink(cpu);
}

// The clock's file made unwritable (a directory in its place: root is refused too), as an offline policy's, and back.
static void clock_gone(const char *path, int gone)
{
  char kept[100];
  snprintf(kept, sizeof kept, "%s.kept", path);
  if (gone) CHECK(!rename(path, kept) && !mkdir(path, 0700));
  else CHECK(!rmdir(path) && !rename(kept, path));
}

// The ways a clock could stay raised: a refused put-back, the boost's process killed with no touch after, a file left over.
static void test_boost_stuck(const char *dir)
{
  char clock[64], list[100], state[64], line[200];
  snprintf(clock, sizeof clock, "%s/stuck_min", dir);
  snprintf(state, sizeof state, "%s/stuck.state", dir);
  snprintf(list, sizeof list, "%s=1497600", clock);
  int make = open(clock, O_CREAT | O_WRONLY, 0600); close(make);
  for (int round = 0; round < 4; round++) {
    // 0: refused at the release, 1: refused at the pipe's end, 2: killed, 3: killed and refused at first
    struct boost b = { .state = state, .hold_s = 0.3 };
    CHECK(!boost_parse(&b, list));
    boost_write(clock, 384000);
    int ends[2], status;
    CHECK(!pipe(ends));
    pid_t child = fork();
    if (!child) { close(ends[1]); _exit(boost_run(&b, ends[0])); }
    close(ends[0]);
    usleep(100000);
    CHECK(write(ends[1], "t", 1) == 1);
    usleep(100000);
    CHECK(boost_read(clock) == 1497600 && !access(state, F_OK));
    if (round == 0) {
      clock_gone(clock, 1);
      usleep(500000);
      CHECK(!access(state, F_OK));
      clock_gone(clock, 0);
      usleep(1300000);                                    // no touch, the pipe still open
      CHECK(boost_read(clock) == 384000 && access(state, F_OK));
      CHECK(write(ends[1], "t", 1) == 1);                 // and it works on
      usleep(100000);
      CHECK(boost_read(clock) == 1497600);
    }
    if (round < 2) {
      if (round) clock_gone(clock, 1);
      close(ends[1]);
      usleep(round ? 300000 : 0);
      if (round) clock_gone(clock, 0);
      CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    } else {
      pid_t watched = child;
      int touched = ends[1];
      boost_watch(&b, &watched, &touched, 10);
      CHECK(watched == child && touched == ends[1] && boost_read(clock) == 1497600);      // alive: nothing
      kill(child, SIGKILL);
      usleep(100000);
      if (round == 3) clock_gone(clock, 1);
      boost_watch(&b, &watched, &touched, 11);
      CHECK(watched == -1 && touched == -1);
      if (round == 3) {
        CHECK(b.left && !access(state, F_OK));
        clock_gone(clock, 0);
        boost_watch(&b, &watched, &touched, 11.5);        // once a second
        CHECK(b.left && boost_read(clock) == 1497600);
        boost_watch(&b, &watched, &touched, 12.1);
      }
      CHECK(!b.left);
    }
    CHECK(boost_read(clock) == 384000 && access(state, F_OK));
  }
  // a state file of this boot at the start: its value is the original, not what the raised clock holds; kept with a raise
  for (int other = 0; other < 2; other++) {
    char id[64];
    CHECK(boost_boot(id));
    FILE *f = fopen(state, "w");
    CHECK(f && fprintf(f, "boot %s\n%s 384000\n/etc/min_freq 5\n/sys/../x_min_freq 5\n/sys/oneios_none/scaling_min_freq 7\n", other ? "0" : id, clock) > 0);
    if (f) fclose(f);
    boost_write(clock, 1497600);
    struct boost b = { .state = state };
    CHECK(!boost_parse(&b, list));
    boost_adopt(&b);
    CHECK(b.n == (other ? 1 : 2) && b.node[0].low == (other ? 0 : 384000) && boost_any_raised(&b) == !other);
    if (other) continue;
    CHECK(boost_release(&b) && boost_read(clock) == 384000 && !access(state, F_OK));      // the one that is not there is kept
    CHECK(!boost_save(&b));
    int kept = 0;
    f = fopen(state, "r");
    while (f && fgets(line, sizeof line, f)) kept |= !strcmp(line, "/sys/oneios_none/scaling_min_freq 7\n");
    if (f) fclose(f);
    CHECK(kept);
  }
  unlink(state);
  unlink(clock);
}

// displayctl with a stand-in for the helper: a shell script that takes the session, and leaves.
static const char *self_path;
static void test_displayctl(const char *displayctl, const char *dir)
{
  char helper[256], command[1024];
  snprintf(helper, sizeof helper, "%s/helper.sh", dir);
  FILE *f = fopen(helper, "w");
  fprintf(f, "#!/bin/sh\necho \"$@\" > %s/helper.args\nexit 7\n", dir);
  fclose(f);
  chmod(helper, 0755);
  snprintf(command, sizeof command, "TMPDIR=%s %s test --seconds 1 --size 64x48 --helper %s -- --hz 60 > %s/out 2>&1", dir, displayctl, helper, dir);
  int status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);                  // a helper that never connects is a failure
  snprintf(command, sizeof command, "grep -q 'socket .*displayctl\\..*/display.sock --wait 5 --hz 60' %s/helper.args", dir);
  CHECK(system(command) == 0);                                          // direct diagnostic socket; never an XNU session
  snprintf(command, sizeof command, "grep -q 'ended before it connected' %s/out && ! ls -d %s/displayctl.* 2>/dev/null", dir, dir);
  CHECK(system(command) == 0);                                          // and its folder is gone
  snprintf(command, sizeof command, "rm -f %s/helper.sh %s/helper.args %s/out", dir, dir, dir);
  CHECK(system(command) == 0);
  // With a helper that speaks the protocol (this program, see main): the whole exchange passes.
  snprintf(command, sizeof command, "TMPDIR=%s %s test --seconds 1 --size 64x48 --helper %s > %s/out 2>&1", dir, displayctl, self_path, dir);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  snprintf(command, sizeof command, "grep -q 'removed its layer and exited 0' %s/out && grep -q '^displayctl: ok' %s/out", dir, dir);
  CHECK(system(command) == 0);
  // One initial full-frame hello flush + exactly one union rectangle per
  // animation update. This catches the S26 flicker-causing double flush.
  snprintf(command, sizeof command,
           "awk '/^displayctl: [0-9]+ frames in / {steps=$2+0} /^test-helper: [0-9]+ flush messages$/ {packets=$2+0} END {exit !(steps>0 && packets==steps+1)}' %s/out",
           dir);
  CHECK(system(command) == 0);
  // The frame handshake: every step after the first frame was acked (none drawn while the helper copied).
  snprintf(command, sizeof command,
           "awk '/^displayctl: [0-9]+ frames in / {steps=$2+0; for (i=1;i<=NF;i++) if ($(i+1)==\"acked\") acked=$i+0} END {exit !(steps>0 && acked==steps)}' %s/out", dir);
  CHECK(system(command) == 0);
  // A helper without acks (an older runtime): the test still passes, paced by time, and says so.
  snprintf(command, sizeof command, "TEST_HELPER_NO_ACK=1 TMPDIR=%s %s test --seconds 1 --size 64x48 --helper %s > %s/out 2>&1", dir, displayctl, self_path, dir);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  snprintf(command, sizeof command, "grep -q '0 acked by the helper (no frame handshake' %s/out && grep -q '^displayctl: ok' %s/out", dir, dir);
  CHECK(system(command) == 0);
  // The half-float pattern: the stand-in checks the stride, the white edge and the wide band's P3 red.
  snprintf(command, sizeof command, "TMPDIR=%s %s test --seconds 1 --size 64x48 --format rgba16f --helper %s > %s/out 2>&1", dir, displayctl, self_path, dir);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  snprintf(command, sizeof command, "grep -q 'pattern in R,G,B,A half float' %s/out && grep -q '^displayctl: ok' %s/out", dir, dir);
  CHECK(system(command) == 0);
  snprintf(command, sizeof command,
           "awk '/^displayctl: [0-9]+ frames in / {steps=$2+0} /^test-helper: [0-9]+ flush messages$/ {packets=$2+0} END {exit !(steps>0 && packets==steps+1)}' %s/out",
           dir);
  CHECK(system(command) == 0);
  snprintf(command, sizeof command, "rm -f %s/out", dir);
  CHECK(system(command) == 0);
  // A live One UI diagnostic must reject destructive or endpoint-overriding
  // helper flags before spawning a helper or creating a test session.
  static const char * const denied[] = { "--takeover", "--session", "--socket", "--lib", "--wait", "--logical" };
  for (unsigned n = 0; n < sizeof denied / sizeof *denied; n++) {
    snprintf(command, sizeof command, "TMPDIR=%s %s test --seconds 1 --size 64x48 --helper %s -- %s ignored > %s/out 2>&1",
             dir, displayctl, self_path, denied[n], dir);
    status = system(command);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    snprintf(command, sizeof command, "grep -q 'refusing %s' %s/out && ! ls -d %s/displayctl.* 2>/dev/null && rm -f %s/out",
             denied[n], dir, dir, dir);
    CHECK(system(command) == 0);
  }
  snprintf(command, sizeof command, "%s test --format rgb565 > /dev/null 2>&1", displayctl);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);                  // a format it does not make: usage
}

// displayctl pool against a stand-in for crosvm's memfd (--test-guest: a plain file), with this program as the helper.
// This process plays the guest: the pool at 1 MiB, 1 MiB long, the frame (64x48, 256 bytes a row) at its start, the
// count's page at 16 KiB; every 50 ms a row changes and the count moves.
static void test_displayctl_pool(const char *displayctl, const char *dir)
{
  char guest[256], out[256], command[2048];
  snprintf(guest, sizeof guest, "%s/guest.mem", dir);
  snprintf(out, sizeof out, "%s/out", dir);
  int fd = open(guest, O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0 && ftruncate(fd, 2 << 20) == 0);
  uint8_t *mem = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(mem != MAP_FAILED);
  if (mem == MAP_FAILED) return;
  uint8_t *pool = mem + (1 << 20);
  for (int i = 0; i < 64 * 48; i++) {
    uint8_t *p = pool + (i / 64) * 256 + (i % 64) * 4;
    p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 255;
  }
  uint64_t *seq = (uint64_t *)(pool + 16384);
  seq[1] = 1;
  __atomic_store_n(&seq[0], XHD_FRAME_SEQ_MAGIC, __ATOMIC_RELEASE);
  char pid[32];
  snprintf(pid, sizeof pid, "%d", (int)getpid());
  pid_t child = fork();
  if (child == 0) {
    setenv("TMPDIR", dir, 1);
    int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(o, 1);
    dup2(o, 2);
    execl(displayctl, displayctl, "pool", "--pid", pid, "--test-guest", guest, "--pool-offset", "1048576", "--pool-bytes", "1048576",
          "--size", "64x48", "--stride", "256", "--seq-offset", "16384", "--seconds", "1.5", "--helper", self_path, (char *)NULL);
    _exit(127);
  }
  for (int k = 0; k < 20; k++) {
    usleep(50000);
    pool[(5 + k) * 256] = (uint8_t)(100 + k);
    __atomic_store_n(&seq[1], (uint64_t)(2 + k), __ATOMIC_RELEASE);
  }
  int status = -1;
  waitpid(child, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  // the stand-in helper passed (a whole first frame, then bands, X = 255), and frames came by the count
  snprintf(command, sizeof command, "grep -q 'removed its layer and exited 0' %s && grep -q '^displayctl: ok' %s && "
           "grep -q 'its count at +0x4000: there' %s && awk '/time is up after/ {f=$9+0; s=$16+0} END {exit !(f>=5 && s>=5)}' %s",
           out, out, out, out);
  CHECK(system(command) == 0);
  // refused before a session or a helper: geometry outside the pool, a disruptive helper option, a missing guest
  snprintf(command, sizeof command, "TMPDIR=%s %s pool --pid %s --test-guest %s --pool-offset 1048576 --pool-bytes 1048576 --size 64x48 "
           "--stride 256 --seq-offset 8192 --helper %s > %s 2>&1", dir, displayctl, pid, guest, self_path, out);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);                  // the count inside the frame
  snprintf(command, sizeof command, "TMPDIR=%s %s pool --pid %s --test-guest %s --pool-offset 1048576 --pool-bytes 1048576 --size 64x48 "
           "--stride 256 --seq-offset 16384 --helper %s -- --takeover > %s 2>&1", dir, displayctl, pid, guest, self_path, out);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
  snprintf(command, sizeof command, "grep -q 'refusing --takeover' %s && ! ls -d %s/displayctl.* 2>/dev/null", out, dir);
  CHECK(system(command) == 0);
  snprintf(command, sizeof command, "TMPDIR=%s %s pool --pid %s --test-guest /proc/self/mem --pool-offset 1048576 --pool-bytes 1048576 "
           "--size 64x48 --stride 256 --seq-offset 16384 --helper %s > %s 2>&1", dir, displayctl, pid, self_path, out);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);                  // never /proc as a stand-in
  // touch: the boost's clocks and its file, both or neither
  snprintf(command, sizeof command, "%s touch --evdev /dev/input/event0 --socket %s/t.sock --boost /a=1 > %s 2>&1", displayctl, dir, out);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
  // touch: only an event device's path, and a socket
  snprintf(command, sizeof command, "%s touch --evdev %s --socket %s/t.sock > %s 2>&1", displayctl, guest, dir, out);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
  snprintf(command, sizeof command, "%s touch --evdev /dev/input/event0/../x --socket %s/t.sock > %s 2>&1", displayctl, dir, out);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
  munmap(mem, 2 << 20);
  close(fd);
  unlink(guest);
  unlink(out);
}

// This program as the helper displayctl starts (--socket PATH --wait N): connects, takes the hello,
// reads flushes until the socket closes, and checks it saw the moving square's damage.
static int as_helper(const char *socket_path)
{
  // Diagnostics must be free of the real guest's session/ready requirement.
  if (xhd_hold_first(NULL)) return 9;
  int fd = xhd_connect(socket_path, 5000, NULL);
  struct xhd_frame frame;
  if (fd < 0 || xhd_recv_hello(fd, &frame, 5000)) return 9;
  int full = 0, partial = 0, flushes = 0, wanted = 0;
  for (int i = 0; i < 2000; i++) {
    // Count wire packets, not helper presentations: a double-flush per
    // animation step would pass the image tests yet flicker on the S26.
    struct xhd_message m;
    ssize_t n = recv(fd, &m, sizeof m, MSG_DONTWAIT);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(5000); continue; }
    if (n < 0) return 10;
    if (n > 0) {
      // The frame handshake (xhd.h): ack each flush when asked, unless this stand-in plays a helper without acks.
      if (n == sizeof m && !memcmp(m.magic, "XHDW", 4)) { wanted = m.a && !getenv("TEST_HELPER_NO_ACK"); continue; }
      if (n != sizeof m || memcmp(m.magic, "XHDF", 4)) return 11;
      if (wanted) xhd_send_ack(fd, (uint32_t)flushes + 1, NULL);
      struct xhd_rect r = { m.a, m.b, m.c, m.d };
      if (!xhd_rect_clip(&r, frame.width, frame.height)) return 12;
      flushes++;
      if (r.w == frame.width && r.h == frame.height) full++;
      else partial++;
      continue;
    }
    // Peer closed: one initial full flush plus ONE union flush per
    // completed animation step. These counts must agree with displayctl.
    if (n == 0) {
      printf("test-helper: %d flush messages\n", flushes);
    fflush(stdout);
    if (frame.format == XHD_FORMAT_BGRX) return full && partial && frame.pixels[3] == 255 ? 0 : 8;
      // half floats: the white edge at 1.0, and the wide band's P3 red (second eighth, lowest quarter) above sRGB's 1.0
      // (the bouncing square may cover part of the band: any pixel of the row will do)
      uint16_t edge[4], p3[4];
      memcpy(edge, frame.pixels, 8);
      int wide = 0;
      for (uint32_t x = frame.width / 8; x < frame.width / 4; x++) {
        memcpy(p3, frame.pixels + (size_t)(frame.height - 4) * frame.stride + (size_t)x * 8, 8);
        if (xh_half_to_float(p3[0]) > 1.2f && xh_half_to_float(p3[1]) < 0.0f) wide = 1;
      }
      int ok = frame.format == XHD_FORMAT_RGBA16F && frame.stride == frame.width * 8 && edge[0] == 0x3c00 && edge[3] == 0x3c00 && wide;
      return full && partial && ok ? 0 : 6;
    }
  }
  return 7;
}

// ---- the touch frames' scan time (src/scantime.h, displayctl touch --scan-time) ----
static void test_scan_time(void)
{
  // xnuhost's arithmetic (tests/test_xnuhost.c): a frame read 2 ms after its scan is 38400 ticks of the 19.2 MHz counter back.
  const uint64_t hz = 19200000, counter = 1000000000;
  const int64_t now = 5000000000ll;
  CHECK(scan_ticks(now - 2000000, now, counter, hz) == (uint32_t)(counter - 38400));
  CHECK(scan_ticks(now, now, counter, hz) == (uint32_t)counter);
  CHECK(scan_ticks(now + 5, now, counter, hz) == (uint32_t)counter);                    // a scan "after now": now
  // the low 32 bits only: the counter past 2^32 (223 s at 19.2 MHz), and a scan before the wrap
  CHECK(scan_ticks(now - 1000000, now, 0x100000010ull, hz) == (uint32_t)(0x100000010ull - 19200));
  CHECK(scan_ticks(now - 1000000, now, 0x100000010ull, hz) == 0xffffb510u);
  // seconds and their rest are converted apart: no overflow at days of uptime, no rounding up
  CHECK(scan_ticks(now - 3000000001ll, now, 0x500000000ull, hz) == (uint32_t)(0x500000000ull - 3 * hz - 0));
  CHECK(scan_ticks(now - 999999999, now, counter, hz) == (uint32_t)(counter - 19199999));
  // what is usable: the past second, and a millisecond of the future (the two clocks are read one after the other)
  CHECK(scan_usable(now - 1, now) && scan_usable(now, now) && scan_usable(now + 1000000, now) && scan_usable(now - 999999999, now));
  CHECK(!scan_usable(now + 1000001, now) && !scan_usable(now - 1000000000, now) && !scan_usable(0, now) && !scan_usable(-5, now));
  // a wall-clock time (the device kept CLOCK_REALTIME): decades after the monotonic now, never used
  CHECK(!scan_usable(1795650229000000000ll, now));

  // The frame: the time goes before its SYN_REPORT, and the frame is one event longer.
  struct xhd_vinput frame[5] = { { EV_ABS, ABS_MT_POSITION_X, 100 }, { EV_ABS, ABS_MT_POSITION_Y, 200 }, { EV_SYN, SYN_REPORT, 0 } };
  struct scan_stats stats = { 0 };
  unsigned n = scan_stamp(frame, 3, 5, now - 2000000, now, counter, hz, &stats);
  CHECK(n == 4 && frame[0].code == ABS_MT_POSITION_X && frame[1].code == ABS_MT_POSITION_Y);
  CHECK(frame[2].type == SCAN_EV_MSC && frame[2].code == SCAN_MSC_TIMESTAMP && frame[2].value == (uint32_t)(counter - 38400));
  CHECK(frame[3].type == EV_SYN && frame[3].code == SYN_REPORT && frame[3].value == 0);
  CHECK(stats.stamped == 1 && stats.bare == 0 && stats.sum_us == 2000 && stats.least_us == 2000 && stats.most_us == 2000);
  // the next ones: least and most follow
  struct xhd_vinput lone[2] = { { EV_SYN, SYN_REPORT, 0 } };
  CHECK(scan_stamp(lone, 1, 2, now - 100000, now, counter, hz, &stats) == 2 && lone[0].type == SCAN_EV_MSC && lone[1].type == EV_SYN);
  lone[0] = (struct xhd_vinput){ EV_SYN, SYN_REPORT, 0 };
  CHECK(scan_stamp(lone, 1, 2, now - 7000000, now, counter, hz, &stats) == 2);
  CHECK(stats.stamped == 3 && stats.sum_us == 2000 + 100 + 7000 && stats.least_us == 100 && stats.most_us == 7000);
  // as it is: no room, a time that cannot be the scan's, no rate, an empty frame
  struct xhd_vinput full[3] = { { EV_ABS, ABS_MT_POSITION_X, 1 }, { EV_ABS, ABS_MT_POSITION_Y, 2 }, { EV_SYN, SYN_REPORT, 0 } };
  CHECK(scan_stamp(full, 3, 3, now - 1000, now, counter, hz, &stats) == 3 && full[2].type == EV_SYN && full[1].code == ABS_MT_POSITION_Y);
  CHECK(scan_stamp(frame, 3, 5, now - 2000000000ll, now, counter, hz, &stats) == 3);
  CHECK(scan_stamp(frame, 3, 5, now - 1000, now, counter, 0, &stats) == 3);
  CHECK(scan_stamp(frame, 0, 5, now - 1000, now, counter, hz, &stats) == 0);
  CHECK(stats.stamped == 3 && stats.bare == 4);
  CHECK(scan_stamp(frame, 3, 5, now - 1000, now, counter, hz, NULL) == 4);                 // nobody counts: still stamped
  // this host's counter (the tests' stand-in on another machine than the phone): it moves, at its rate
  const uint64_t a = scan_counter();
  CHECK(scan_counter_hz() > 0 && scan_counter() >= a && scan_monotonic_ns() > 0);
}

// displayctl touch with a FIFO in the touchscreen's place (--test-evdev) and this program in crosvm's: the frames as they
// reach crosvm's socket, with and without --scan-time.
static int touch_events(int fifo, int64_t at_ns, const struct xhd_vinput *list, unsigned n)
{
  struct input_event ev[16];
  memset(ev, 0, sizeof ev);
  for (unsigned i = 0; i < n; i++) {
    ev[i].input_event_sec = at_ns / 1000000000;
    ev[i].input_event_usec = at_ns % 1000000000 / 1000;
    ev[i].type = list[i].type; ev[i].code = list[i].code; ev[i].value = (int32_t)list[i].value;
  }
  return write(fifo, ev, n * sizeof *ev) == (ssize_t)(n * sizeof *ev);
}

static unsigned touch_read(int sock, struct xhd_vinput *out, unsigned want, int wait_ms)
{
  size_t got = 0;
  while (got < want * sizeof *out) {
    struct pollfd p = { sock, POLLIN, 0 };
    if (poll(&p, 1, wait_ms) <= 0) break;
    ssize_t r = read(sock, (char *)out + got, want * sizeof *out - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  return (unsigned)(got / sizeof *out);
}

static int wait_for_text(const char *file, const char *text, int tenths)
{
  char command[768];
  snprintf(command, sizeof command, "grep -q -F \"%s\" %s 2>/dev/null", text, file);
  for (int i = 0; i < tenths; i++) {
    if (system(command) == 0) return 1;
    usleep(100000);
  }
  return 0;
}

static void test_displayctl_touch(const char *displayctl, const char *dir, int scan)
{
  char fifo[256], sock[256], out[256], command[1024];
  snprintf(fifo, sizeof fifo, "%s/events", dir);
  snprintf(sock, sizeof sock, "%s/touch.sock", dir);
  snprintf(out, sizeof out, "%s/touch.out", dir);
  unlink(fifo); unlink(out);
  CHECK(mkfifo(fifo, 0600) == 0);
  pid_t child = fork();
  if (!child) {
    int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(o, 1); dup2(o, 2);
    if (scan) execl(displayctl, displayctl, "touch", "--evdev", fifo, "--test-evdev", "--socket", sock, "--wait", "10", "--scan-time", (char *)NULL);
    else execl(displayctl, displayctl, "touch", "--evdev", fifo, "--socket", sock, "--wait", "10", "--test-evdev", (char *)NULL);
    _exit(127);
  }
  CHECK(child > 0);
  // crosvm's side: connect to the forwarder's socket once it listens
  int client = socket(AF_UNIX, SOCK_STREAM, 0), connected = -1;
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  strncpy(addr.sun_path, sock, sizeof addr.sun_path - 1);
  for (int i = 0; i < 100 && connected; i++) { connected = connect(client, (struct sockaddr *)&addr, sizeof addr); if (connected) usleep(50000); }
  CHECK(connected == 0);
  CHECK(wait_for_text(out, "the touchscreen is the VM's", 50));            // (flushed before its loop: what came earlier was dropped)
  // The counter's rate as the forwarder read it: this host's stand-in counts nanoseconds; the phone's binary run by
  // qemu-aarch64 (argv[1] a script that does so) reads the emulated CNTFRQ_EL0, whatever it is.
  unsigned long long rate = 0;
  if (scan) {
    CHECK(wait_for_text(out, "scan time: each frame goes with its scan time (the counter at ", 50));
    snprintf(command, sizeof command, "sed -n 's/.*scan time: each frame goes with its scan time (the counter at \\([0-9]*\\) Hz).*/\\1/p' %s", out);
    FILE *said = popen(command, "r");
    CHECK(said && fscanf(said, "%llu", &rate) == 1 && rate >= 1000000);
    if (said) pclose(said);
  }
  int touch = open(fifo, O_WRONLY | O_NONBLOCK);
  CHECK(touch >= 0);
  // A touch-down scanned 2 ms ago, with an event the forwarder never passes on (the IC's own EV_MSC).
  const struct xhd_vinput down[7] = { { EV_ABS, ABS_MT_SLOT, 0 }, { EV_ABS, ABS_MT_TRACKING_ID, 7 }, { EV_ABS, ABS_MT_POSITION_X, 100 },
                                      { EV_ABS, ABS_MT_POSITION_Y, 200 }, { EV_KEY, BTN_TOUCH, 1 }, { EV_MSC, MSC_SCAN, 99 }, { EV_SYN, SYN_REPORT, 0 } };
  const int64_t scanned = (scan_monotonic_ns() - 2000000) / 1000 * 1000;     // an event's time is whole microseconds
  CHECK(touch_events(touch, scanned, down, 7));
  struct xhd_vinput got[16];
  memset(got, 0, sizeof got);
  unsigned n = touch_read(client, got, scan ? 7 : 6, 3000);
  CHECK(n == (scan ? 7u : 6u));
  CHECK(got[0].type == EV_ABS && got[0].code == ABS_MT_SLOT && got[1].code == ABS_MT_TRACKING_ID && got[1].value == 7 && got[2].value == 100 &&
        got[3].value == 200 && got[4].type == EV_KEY && got[4].code == BTN_TOUCH && got[4].value == 1);
  if (scan) {
    // the time before the SYN_REPORT. This host's stand-in counter is CLOCK_MONOTONIC's nanoseconds, so the value is the
    // scan's own time (the low 32 bits; the two clock reads are a few hundred ns apart, 50 ms allowed on a busy machine).
    // The phone's binary under qemu-aarch64 (make test-arm64: ONEIOS_TEST_EMULATED) reads the emulator's counter, whose
    // zero is its own: there only the event and the spacing of two frames are checked.
    const uint32_t off = got[5].value - (uint32_t)scanned;
    CHECK(got[5].type == SCAN_EV_MSC && got[5].code == SCAN_MSC_TIMESTAMP && (getenv("ONEIOS_TEST_EMULATED") || (rate == 1000000000ull && off < 50000000u)));
    CHECK(got[6].type == EV_SYN && got[6].code == SYN_REPORT);
  } else {
    CHECK(got[5].type == EV_SYN && got[5].code == SYN_REPORT);
  }
  // A move whose time cannot be its scan's (a second old), then one from the future (another clock): both pass, bare.
  const struct xhd_vinput move[2] = { { EV_ABS, ABS_MT_POSITION_Y, 180 }, { EV_SYN, SYN_REPORT, 0 } };
  CHECK(touch_events(touch, scan_monotonic_ns() - 1500000000ll, move, 2));
  CHECK(touch_read(client, got, 2, 3000) == 2 && got[0].code == ABS_MT_POSITION_Y && got[0].value == 180 && got[1].type == EV_SYN);
  CHECK(touch_events(touch, scan_monotonic_ns() + 5000000000ll, move, 2));
  CHECK(touch_read(client, got, 2, 3000) == 2 && got[0].code == ABS_MT_POSITION_Y && got[1].type == EV_SYN);
  // Two frames in one read: each gets its own time.
  struct input_event two[4];
  memset(two, 0, sizeof two);
  const int64_t first = scan_monotonic_ns() - 9000000, second = first + 8333333;
  for (int i = 0; i < 4; i++) {
    const int64_t at = i < 2 ? first : second;
    two[i].input_event_sec = at / 1000000000; two[i].input_event_usec = at % 1000000000 / 1000;
    two[i].type = i % 2 ? EV_SYN : EV_ABS; two[i].code = i % 2 ? SYN_REPORT : ABS_MT_POSITION_Y; two[i].value = i % 2 ? 0 : 170 - i;
  }
  CHECK(write(touch, two, sizeof two) == (ssize_t)sizeof two);
  n = touch_read(client, got, scan ? 6 : 4, 3000);
  CHECK(n == (scan ? 6u : 4u));
  if (scan) {
    // 8333 us apart in the counter's ticks, as the scans were (the event times are whole microseconds)
    const uint32_t apart = got[4].value - got[1].value;
    CHECK(got[1].type == SCAN_EV_MSC && got[4].type == SCAN_EV_MSC && apart >= 8333 * (rate / 1000) / 1000 && apart <= 8334 * (rate / 1000) / 1000 + 1);
    CHECK(got[0].value == 170 && got[2].type == EV_SYN && got[3].value == 168 && got[5].type == EV_SYN);
  } else {
    CHECK(got[0].value == 170 && got[1].type == EV_SYN && got[2].value == 168 && got[3].type == EV_SYN);
  }
  // crosvm goes: the forwarder ends by itself and says what it did; its last line is still the one boot.sh quotes.
  close(client);
  int status = 0;
  for (int i = 0; i < 100 && waitpid(child, &status, WNOHANG) == 0; i++) usleep(50000);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  close(touch);
  snprintf(command, sizeof command, "tail -n 1 %s | grep -q '^displayctl touch: crosvm went after 5 frames (1[0-9] events read, 0 overflows, 0 test swipes)'", out);
  CHECK(system(command) == 0);
  snprintf(command, sizeof command, "grep -c 'scan time' %s > /dev/null", out);
  CHECK((system(command) == 0) == (scan != 0));
  if (scan) {
    snprintf(command, sizeof command, "grep -q -E '^displayctl touch: scan time: 3 frames went with it \\(the forwarder had each [0-9]+ / [0-9]+ / [0-9]+ us after its scan: least, mean, most\\), 2 without$' %s", out);
    CHECK(system(command) == 0);
  }
  // a device of /dev is never a test's: usage
  snprintf(command, sizeof command, "%s touch --evdev /dev/null --test-evdev --socket %s > /dev/null 2>&1", displayctl, sock);
  status = system(command);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
  unlink(fifo); unlink(out); unlink(sock);
}

int main(int argc, char **argv)
{
  if (argc > 2 && !strcmp(argv[1], "--socket")) return as_helper(argv[2]);
  self_path = argv[0];
  char dir[] = "/tmp/oneios-display-test.XXXXXX";
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  test_fit();
  test_rects();
  test_hello_check();
  test_copy();
  test_formats();
  test_copy_formats();
  test_pool_parts();
  test_panel_report();
  test_scan_time();
  test_hold_first(dir);
  test_boost(dir);
  test_boost_stuck(dir);
  test_protocol(dir);
  if (argc > 1) test_displayctl(argv[1], dir);
  if (argc > 1) test_displayctl_pool(argv[1], dir);
  if (argc > 1) { test_displayctl_touch(argv[1], dir, 1); test_displayctl_touch(argv[1], dir, 0); }
  rmdir(dir);
  printf("%s: %d checks, %d failed\n", failed ? "FAILED" : "ok", checks, failed);
  return failed ? 1 : 0;
}
