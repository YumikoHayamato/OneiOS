// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "xhd.h"
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int xhd_check_hello(const struct xhd_message *m, size_t length, off_t memfd_size)
{
  if (length != sizeof *m || memcmp(m->magic, "XHD1", 4)) return -EPROTO;
  uint32_t width = m->a, height = m->b, stride = m->c, format = m->d;
  if (!width || !height || width > XHD_MAX_SIDE || height > XHD_MAX_SIDE) return -EINVAL;
  unsigned bpp = xh_pixels_bpp(format);
  if (!bpp) return -EINVAL;
  if (stride < width * bpp || stride > XHD_MAX_SIDE * 8 || stride % bpp) return -EINVAL;
  if (memfd_size < 0 || (uint64_t)stride * height > (uint64_t)memfd_size) return -EINVAL;
  return 0;
}

int xhd_connect(const char *path, int wait_ms, const volatile int *stop)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof addr.sun_path) return -ENAMETOOLONG;
  strcpy(addr.sun_path, path);
  long long end = now_ms() + (wait_ms > 0 ? wait_ms : 0);
  int error = ENOENT;
  for (;;) {
    struct stat st;
    if (lstat(path, &st)) error = errno;
    else if (!S_ISSOCK(st.st_mode)) return -ENOTSOCK;       // a link or a file put there: not ours to follow
    else {
      int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
      if (fd < 0) return -errno;
      if (!connect(fd, (struct sockaddr *)&addr, sizeof addr)) return fd;
      error = errno;
      close(fd);
      if (error != ECONNREFUSED && error != ENOENT && error != EAGAIN && error != EINTR) return -error;
    }
    if ((stop && *stop) || now_ms() >= end) return -error;
    struct timespec nap = { 0, 100 * 1000000 };
    nanosleep(&nap, NULL);
  }
}

int xhd_send_panel(int fd, uint32_t width, uint32_t height, uint32_t hz)
{
  struct xhd_message m = { { 'X', 'H', 'D', 'P' }, width, height, hz, 0 };
  return send(fd, &m, sizeof m, MSG_NOSIGNAL) < 0 ? -errno : 0;
}

int xhd_recv_hello(int fd, struct xhd_frame *frame, int timeout_ms)
{
  memset(frame, 0, sizeof *frame);
  frame->memfd = -1;
  struct pollfd p = { fd, POLLIN, 0 };
  int ready;
  do ready = poll(&p, 1, timeout_ms); while (ready < 0 && errno == EINTR);
  if (ready < 0) return -errno;
  if (!ready) return -ETIMEDOUT;
  struct xhd_message hello;
  char control[CMSG_SPACE(sizeof(int) * 4)];
  struct iovec iov = { &hello, sizeof hello };
  struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
  ssize_t n;
  do n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC); while (n < 0 && errno == EINTR);
  if (n < 0) return -errno;
  // Every descriptor that came is ours to close; the first is the frame.
  int memfd = -1;
  for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
    if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
    size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < count; i++) {
      int got;
      memcpy(&got, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
      if (memfd < 0) memfd = got; else close(got);
    }
  }
  if (n == 0) { if (memfd >= 0) close(memfd); return -ECONNRESET; }
  if (memfd < 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) { if (memfd >= 0) close(memfd); return -EPROTO; }
  struct stat st;
  int bad = fstat(memfd, &st) ? -errno : !S_ISREG(st.st_mode) ? -EINVAL : xhd_check_hello(&hello, (size_t)n, st.st_size);
  if (bad) { close(memfd); return bad; }
  size_t bytes = (size_t)hello.c * hello.b;
  void *map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, memfd, 0);
  if (map == MAP_FAILED) { int e = errno; close(memfd); return -e; }
  frame->width = hello.a; frame->height = hello.b; frame->stride = hello.c; frame->format = hello.d;
  frame->memfd = memfd; frame->pixels = map; frame->bytes = bytes;
  return 0;
}

void xhd_frame_close(struct xhd_frame *frame)
{
  if (frame->pixels) munmap((void *)frame->pixels, frame->bytes);
  if (frame->memfd >= 0) close(frame->memfd);
  frame->pixels = NULL;
  frame->memfd = -1;
}

int xhd_rect_clip(struct xhd_rect *r, uint32_t width, uint32_t height)
{
  if (!r->w || !r->h || r->x >= width || r->y >= height) { r->x = r->y = r->w = r->h = 0; return 0; }
  if (r->w > width - r->x) r->w = width - r->x;
  if (r->h > height - r->y) r->h = height - r->y;
  return 1;
}

void xhd_rect_union(struct xhd_rect *into, const struct xhd_rect *r)
{
  if (!r->w || !r->h) return;
  if (!into->w || !into->h) { *into = *r; return; }
  uint32_t x0 = into->x < r->x ? into->x : r->x, y0 = into->y < r->y ? into->y : r->y;
  uint64_t ax = (uint64_t)into->x + into->w, bx = (uint64_t)r->x + r->w, ay = (uint64_t)into->y + into->h, by = (uint64_t)r->y + r->h;
  uint64_t x1 = ax > bx ? ax : bx, y1 = ay > by ? ay : by;
  into->x = x0; into->y = y0; into->w = (uint32_t)(x1 - x0); into->h = (uint32_t)(y1 - y0);
}

int xhd_hold_first(const char *ready)
{
  struct stat st;
  return ready && *ready && stat(ready, &st) && errno == ENOENT;
}

static int acks_wanted;
int xhd_acks_wanted(void) { return acks_wanted; }

int xhd_send_want_acks(int fd)
{
  struct xhd_message m = { { 'X', 'H', 'D', 'W' }, 1, 0, 0, 0 };
  return send(fd, &m, sizeof m, MSG_NOSIGNAL) < 0 ? -errno : 0;
}

int xhd_send_ack(int fd, uint32_t posts, const struct xhd_rect *copied)
{
  struct xhd_message m = { { 'X', 'H', 'D', 'A' }, posts, copied ? copied->x : 0, copied ? copied->y : 0, copied ? copied->w : 0 };
  return send(fd, &m, sizeof m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 ? -errno : 0;
}

int xhd_wait_ack(int fd, int timeout_ms)
{
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    long left = timeout_ms - ((now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L);
    if (left <= 0) return 0;
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, (int)left);
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    if (r == 0) return 0;
    if (p.revents & (POLLHUP | POLLERR) && !(p.revents & POLLIN)) return -1;
    struct xhd_message m;
    ssize_t n = recv(fd, &m, sizeof m, MSG_DONTWAIT);
    if (n == 0) return -1;
    if (n < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; return -1; }
    if (n == sizeof m && !memcmp(m.magic, "XHDA", 4)) return 1;
  }
}

int xhd_drain(int fd, const struct xhd_frame *frame, struct xhd_rect *damage)
{
  // A bound on one round: a guest that floods flushes must not keep the frame from being posted.
  for (int i = 0; i < 4096; i++) {
    struct xhd_message m;
    ssize_t n = recv(fd, &m, sizeof m, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      fprintf(stderr, "oneios-display: reading xnuhost's socket failed: %s\n", strerror(errno));
      return -1;
    }
    if (n == 0) { fprintf(stderr, "oneios-display: xnuhost closed the socket\n"); return -1; }
    if (n == sizeof m && !memcmp(m.magic, "XHDW", 4)) { acks_wanted = m.a != 0; continue; }
    if (n != sizeof m || memcmp(m.magic, "XHDF", 4)) continue;      // not a flush: ignored
    struct xhd_rect r = { m.a, m.b, m.c, m.d };
    if (xhd_rect_clip(&r, frame->width, frame->height)) xhd_rect_union(damage, &r);
  }
  return 0;
}

void xhd_fit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, struct xhd_fit *fit)
{
  memset(fit, 0, sizeof *fit);
  if (!sw || !sh || !dw || !dh) return;
  uint64_t w, h;
  if ((uint64_t)sw * dh <= (uint64_t)sh * dw) {        // the source is the narrower: full height, bars left and right
    h = dh;
    w = ((uint64_t)sw * dh + sh / 2) / sh;
  } else {                                             // full width, bars above and below
    w = dw;
    h = ((uint64_t)sh * dw + sw / 2) / sw;
  }
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (w > dw) w = dw;
  if (h > dh) h = dh;
  fit->w = (uint32_t)w; fit->h = (uint32_t)h;
  fit->x = (dw - fit->w) / 2; fit->y = (dh - fit->h) / 2;
}

int xhd_copy(uint8_t *dst, size_t dst_stride, int buffer, const uint8_t *src, size_t src_stride, uint32_t format, struct xhd_rect r)
{
  unsigned in = xh_pixels_bpp(format), out = buffer == XHD_BUFFER_RGBA_FP16 ? 8 : buffer == XHD_BUFFER_BGRA_8888 || buffer == XHD_BUFFER_RGBX_8888 ? 4 : 0;
  if (!in || !out) return -EINVAL;
  for (uint32_t row = r.y; row < r.y + r.h; row++) {
    const uint8_t *s = src + (size_t)row * src_stride + (size_t)r.x * in;
    uint8_t *d = dst + (size_t)row * dst_stride + (size_t)r.x * out;
    if (format == XHD_FORMAT_RGBA16F) {
      if (buffer == XHD_BUFFER_RGBA_FP16) memcpy(d, s, (size_t)r.w * 8);
      else xh_pixels_rgba16f_to_8(d, s, r.w, buffer == XHD_BUFFER_RGBX_8888);
    } else if (buffer == XHD_BUFFER_BGRA_8888) memcpy(d, s, (size_t)r.w * 4);
    else if (buffer == XHD_BUFFER_RGBA_FP16) xh_pixels_bgrx8_to_rgba16f(d, s, r.w);
    else for (uint32_t i = 0; i < r.w; i++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 0xff; }
  }
  return 0;
}

int xhd_buffer_for(uint32_t format, int *dataspace)
{
  if (format == XHD_FORMAT_BGRX) { *dataspace = XHD_DATASPACE_SRGB; return XHD_BUFFER_BGRA_8888; }
  if (format == XHD_FORMAT_RGBA16F) { *dataspace = XHD_DATASPACE_SCRGB_LINEAR; return XHD_BUFFER_RGBA_FP16; }
  *dataspace = XHD_DATASPACE_UNKNOWN;
  return 0;
}

static const struct { const char *name; int value; } dataspaces[] = {
  { "unknown", XHD_DATASPACE_UNKNOWN }, { "srgb", XHD_DATASPACE_SRGB }, { "p3", XHD_DATASPACE_DISPLAY_P3 },
  { "scrgb", XHD_DATASPACE_SCRGB }, { "scrgb-linear", XHD_DATASPACE_SCRGB_LINEAR },
};

const char *xhd_dataspace_name(int dataspace)
{
  for (size_t i = 0; i < sizeof dataspaces / sizeof *dataspaces; i++) if (dataspaces[i].value == dataspace) return dataspaces[i].name;
  return "other";
}

int xhd_dataspace_parse(const char *name)
{
  if (!strcmp(name, "auto")) return -1;
  for (size_t i = 0; i < sizeof dataspaces / sizeof *dataspaces; i++) if (!strcmp(dataspaces[i].name, name)) return dataspaces[i].value;
  return -2;
}

int xhd_listen(const char *path)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof addr.sun_path) return -ENAMETOOLONG;
  strcpy(addr.sun_path, path);
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 2)) { int e = errno; close(fd); return -e; }
  return fd;
}

int xhd_send_hello(int fd, uint32_t width, uint32_t height, uint32_t stride, uint32_t format, int memfd)
{
  struct xhd_message hello = { { 'X', 'H', 'D', '1' }, width, height, stride, format };
  char control[CMSG_SPACE(sizeof(int))];
  struct iovec iov = { &hello, sizeof hello };
  struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
  memset(control, 0, sizeof control);
  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &memfd, sizeof(int));
  return sendmsg(fd, &msg, MSG_NOSIGNAL) < 0 ? -errno : 0;
}

int xhd_send_flush(int fd, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
  struct xhd_message m = { { 'X', 'H', 'D', 'F' }, x, y, w, h };
  return send(fd, &m, sizeof m, MSG_NOSIGNAL) < 0 ? -errno : 0;
}

int xhd_memfd(const char *name, size_t bytes)
{
  int fd = (int)syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */);
  if (fd < 0) return -errno;
  if (ftruncate(fd, (off_t)bytes)) { int e = errno; close(fd); return -e; }
  return fd;
}

int xhd_pool_mapping(const char *maps, uint64_t offset, uint64_t bytes, int shared_ram, uint64_t *start, uint64_t *end,
                     uint64_t *inode)
{
  static const char name[] = "/memfd:crosvm_guest";
  for (const char *line = maps; line && *line;) {
    const char *next = strchr(line, '\n');
    size_t length = next ? (size_t)(next - line) : strlen(line);
    char text[512];
    if (length < sizeof text) {
      memcpy(text, line, length);
      text[length] = 0;
      unsigned long long s, e, off, ino;
      unsigned major, minor;
      char perms[8];
      int at = 0;
      if (sscanf(text, "%llx-%llx %7s %llx %x:%x %llu %n", &s, &e, perms, &off, &major, &minor, &ino, &at) == 7 && at > 0 &&
          strlen(perms) == 4 && perms[0] == 'r' && perms[3] == 's' && e > s &&
          (shared_ram ? off <= offset && bytes <= e - s && offset - off <= e - s - bytes : off == offset && e - s == bytes) &&
          !strncmp(text + at, name, sizeof name - 1) && (text[at + sizeof name - 1] == 0 || text[at + sizeof name - 1] == ' ')) {
        *start = s;
        *end = e;
        *inode = ino;
        return 0;
      }
    }
    line = next ? next + 1 : NULL;
  }
  return -ENOENT;
}

static void invalidate_lines(const uint8_t *p, size_t n)
{
#if defined(__aarch64__)
  // EL0 may clean and invalidate by VA on Linux (SCTLR_EL1.UCI); it needs only read permission. DminLine: log2 words.
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 0xf);
  for (uintptr_t a = (uintptr_t)p & ~(line - 1); a < (uintptr_t)p + n; a += line) __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb ish" ::: "memory");
#else
  (void)p;
  (void)n;
#endif
}

uint32_t xhd_pool_copy(uint8_t *dst, const uint8_t *src, uint32_t width, uint32_t height, uint32_t stride,
                       const unsigned char *resident, size_t page_size, int invalidate, struct xhd_rect *changed)
{
  const size_t row = (size_t)width * 4;
  uint32_t first = 0, last = 0, rows = 0;
  for (uint32_t y = 0; y < height; y++) {
    const size_t at = (size_t)y * stride;
    int in = 1;
    for (size_t p = at / page_size; in && p <= (at + row - 1) / page_size; p++) in = resident[p] & 1;
    if (!in) continue;
    if (invalidate) invalidate_lines(src + at, row);
    if (!memcmp(dst + at, src + at, row)) continue;
    memcpy(dst + at, src + at, row);
    if (!rows++) first = y;
    last = y;
  }
  *changed = rows ? (struct xhd_rect){ 0, first, width, last - first + 1 } : (struct xhd_rect){ 0, 0, 0, 0 };
  return rows;
}

int xhd_frame_seq(const uint8_t *page, uint64_t *count)
{
  if (__atomic_load_n((const uint64_t *)page, __ATOMIC_ACQUIRE) != XHD_FRAME_SEQ_MAGIC) return 0;
  *count = __atomic_load_n((const uint64_t *)(page + 8), __ATOMIC_ACQUIRE);
  return 1;
}

int xhd_power_blanks_panel(int toggle, int power_blanks)
{
  return toggle && power_blanks;
}

unsigned xhd_side_key(uint16_t type, uint16_t code, int32_t value, double now, double *power_down, int *toggle,
                      struct xhd_vinput out[2])
{
  if (type != EV_KEY || (code != KEY_POWER && code != KEY_VOLUMEUP && code != KEY_VOLUMEDOWN) || (value != 0 && value != 1)) return 0;
  if (code == KEY_POWER) {
    if (value == 1) *power_down = now;
    else {
      if (*power_down > 0 && now - *power_down < XHD_POWER_CLICK_S) *toggle = 1;
      *power_down = 0;
    }
  }
  out[0] = (struct xhd_vinput){ EV_KEY, code, (uint32_t)value };
  out[1] = (struct xhd_vinput){ EV_SYN, SYN_REPORT, 0 };
  return 2;
}

int xhd_touch_event(uint16_t type, uint16_t code, int32_t value, struct xhd_vinput *out)
{
  int keep = 0;
  if (type == EV_SYN) keep = code == SYN_REPORT;
  else if (type == EV_KEY) keep = code == BTN_TOUCH;
  else if (type == EV_ABS)
    keep = code == ABS_X || code == ABS_Y || code == ABS_MT_SLOT || code == ABS_MT_TRACKING_ID || code == ABS_MT_POSITION_X ||
           code == ABS_MT_POSITION_Y || code == ABS_MT_TOUCH_MAJOR || code == ABS_MT_TOUCH_MINOR;
  if (keep) *out = (struct xhd_vinput){ type, code, (uint32_t)value };
  return keep;
}
