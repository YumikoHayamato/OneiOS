// SPDX-License-Identifier: MIT
// pacectl: the panel's vsync for the VM (`oneios-tool phone vm-boot --vsync-lock`; README.md; docs/native.md "Frame pacing").
//
//   pacectl vsync --pid PID --page-offset N [--phase-us N] [--phase-file FILE] [--every-ms N] [--say-s N] [--seconds N]
//     PID is crosvm's; N is where the frame sequence's page lies in its guest memfd (the page's address in the VM less
//     the RAM's base: xnuloader-crosvm's `fb ... SEQ` and the pool's place, as vm-boot works it out). The page is found in
//     crosvm's own mapping (/proc/PID/maps, opened through /proc/PID/map_files, as displayctl pool finds the frame) and
//     mapped to write. Several times a second (--every-ms, 250) surfaceflinger is asked for its next vsync (AChoreographer:
//     the frame's deadline and the timeline's period), which goes into the page as counter ticks (vsync_lock.h); the
//     kernel's refresh tick follows it. --phase-us (or the number in --phase-file, read at every turn: a session can
//     change it without a restart) moves the kernel's tick off surfaceflinger's deadline by that much. Every 10 s (--say-s)
//     one line says the panel's period and what the kernel did with it (its own record in the page). It ends when crosvm does, at
//     --seconds, or at SIGTERM; the record it leaves is stale 2 s later and the kernel's tick runs free again.
//   --test-guest FILE --test-period-ns N (the host tests): a plain file stands for crosvm's memfd and a clock of this
//     program's own for surfaceflinger's (a vsync every N ns of CLOCK_MONOTONIC, the deadline three periods ahead).
//
// Nothing here is needed for the VM to run: without it, or when any step fails (said once), the kernel's tick is the
// free-running one it always was.
#define _GNU_SOURCE
#include "../../display/src/scantime.h"
#include "../../../kernel/patches/oneios-hosted/display/vsync_lock.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/choreographer.h>
#include <android/looper.h>
#endif

static volatile sig_atomic_t stopped;
static void on_signal(int n) { (void)n; stopped = 1; }

static int usage(void)
{
  fprintf(stderr, "usage: pacectl vsync --pid PID --page-offset N [--phase-us N] [--phase-file FILE] [--every-ms N] [--say-s N] [--seconds N] [--wait-s N (0: as long as crosvm lives)]\n"
                  "       pacectl vsync --test-guest FILE --test-period-ns N --page-offset N [...]\n");
  return 2;
}

// ---- the page in crosvm's memfd ----------------------------------------------------------------------------------

static char *read_all(const char *path)
{
  FILE *f = fopen(path, "re");
  if (!f) { fprintf(stderr, "pacectl: %s: %s\n", path, strerror(errno)); return NULL; }
  size_t size = 0, cap = 0;
  char *text = NULL;
  for (;;) {
    if (cap - size < 4096) {
      size_t more = cap ? cap * 2 : 1 << 16;
      char *grown = more <= (64u << 20) ? realloc(text, more) : NULL;
      if (!grown) break;
      text = grown;
      cap = more;
    }
    size_t n = fread(text + size, 1, cap - size - 1, f);
    if (!n) break;
    size += n;
  }
  fclose(f);
  if (text) text[size] = 0;
  return text;
}

// The mapping of crosvm that holds the page (a shared-RAM VM: its RAM is one shared mapping of "/memfd:crosvm_guest"),
// opened to write through /proc/PID/map_files and checked to be that mapping's file. The fd, or -1 (said).
static int open_guest(pid_t pid, uint64_t offset)
{
  char path[96];
  snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
  char *maps = read_all(path);
  if (!maps) return -1;
  uint64_t start = 0, end = 0, inode = 0;
  int missing = xhd_pool_mapping(maps, offset, ONEIOS_VSYNC_PAGE_BYTES, 1, &start, &end, &inode);
  free(maps);
  if (missing) {
    fprintf(stderr, "pacectl: process %d maps no shared \"/memfd:crosvm_guest\" over offset 0x%" PRIx64 ": not crosvm's shared-RAM VM with that page\n", (int)pid, offset);
    return -1;
  }
  snprintf(path, sizeof path, "/proc/%d/map_files/%" PRIx64 "-%" PRIx64, (int)pid, start, end);
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) { fprintf(stderr, "pacectl: %s: %s\n", path, strerror(errno)); return -1; }
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_ino != inode || (uint64_t)st.st_size < offset + ONEIOS_VSYNC_PAGE_BYTES) {
    fprintf(stderr, "pacectl: %s is not the memfd of crosvm's mapping, or ends before the page\n", path);
    close(fd);
    return -1;
  }
  return fd;
}

// --test-guest FILE: a plain file of the tests' own, never a device or anything under /proc.
static int open_test_guest(const char *file, uint64_t need)
{
  char real[PATH_MAX];
  if (!realpath(file, real) || !strncmp(real, "/proc/", 6) || !strncmp(real, "/dev/", 5)) {
    fprintf(stderr, "pacectl: --test-guest %s: a plain file of the tests, not under /proc or /dev\n", file);
    return -1;
  }
  int fd = open(real, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size < need) {
    fprintf(stderr, "pacectl: --test-guest %s: not a plain file of %" PRIu64 " bytes or more\n", file, need);
    if (fd >= 0) close(fd);
    return -1;
  }
  return fd;
}

static int alive(pid_t pid)
{
  char path[64], text[512];
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno != ENOENT && errno != ESRCH;
  ssize_t n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n <= 0) return 1;
  text[n] = 0;
  const char *state = strrchr(text, ')');
  return !state || (state[1] != 0 && state[2] != 'Z' && state[2] != 'X');
}

// ---- surfaceflinger's vsync ----------------------------------------------------------------------------------------

// One look: a point of the grid the kernel's tick is to follow (CLOCK_MONOTONIC ns; surfaceflinger's deadline for a frame
// where it gives one, else its vsync) and the period (ns). 0, or -1 when none came.
struct vsync_look { int64_t at_ns, period_ns; int deadline; };

#if defined(__ANDROID__)
struct source { AChoreographer *choreographer; struct vsync_look look; int64_t last_frame_ns; int got; };

static void on_vsync(const AChoreographerFrameCallbackData *data, void *arg)
{
  struct source *s = arg;
  const int64_t frame = AChoreographerFrameCallbackData_getFrameTimeNanos(data);
  const size_t n = AChoreographerFrameCallbackData_getFrameTimelinesLength(data);
  const size_t i = AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(data);
  s->look = (struct vsync_look){ frame, 0, 0 };
  if (n && i < n) {
    const int64_t deadline = AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(data, i);
    if (deadline > 0) { s->look.at_ns = deadline; s->look.deadline = 1; }
    // two timelines of one callback are one period of surfaceflinger's own model apart
    const size_t a = i + 1 < n ? i : i ? i - 1 : i;
    if (a + 1 < n) {
      const int64_t apart = AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(data, a + 1) -
                            AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(data, a);
      if (apart > 0) s->look.period_ns = apart;
    }
  }
  // else, from two callbacks one refresh apart
  if (!s->look.period_ns && s->last_frame_ns && frame > s->last_frame_ns && frame - s->last_frame_ns < 50000000) s->look.period_ns = frame - s->last_frame_ns;
  s->last_frame_ns = frame;
  s->got = 1;
}

static int source_open(struct source *s, int64_t test_period_ns)
{
  (void)test_period_ns;
  memset(s, 0, sizeof *s);
  if (!ALooper_prepare(0)) { fprintf(stderr, "pacectl: no looper for this thread\n"); return -1; }
  s->choreographer = AChoreographer_getInstance();
  if (!s->choreographer) { fprintf(stderr, "pacectl: surfaceflinger gives this process no choreographer\n"); return -1; }
  return 0;
}

static int source_look(struct source *s, struct vsync_look *out)
{
  // up to two callbacks: the second gives the period where the first one's timelines did not
  for (int turn = 0; turn < 2; turn++) {
    s->got = 0;
    AChoreographer_postVsyncCallback(s->choreographer, on_vsync, s);
    const int64_t until = scan_monotonic_ns() + 200000000;
    while (!s->got && !stopped && scan_monotonic_ns() < until) ALooper_pollOnce(50, NULL, NULL, NULL);
    if (!s->got) return -1;
    if (s->look.period_ns) break;
  }
  *out = s->look;
  return s->look.period_ns ? 0 : -1;
}
#else
// The build host: a clock of this program's own (--test-period-ns), the deadline three periods ahead.
struct source { int64_t period_ns, origin_ns; };

static int source_open(struct source *s, int64_t test_period_ns)
{
  if (test_period_ns <= 0) { fprintf(stderr, "pacectl: surfaceflinger's vsync is the phone's: here only --test-guest with --test-period-ns\n"); return -1; }
  s->period_ns = test_period_ns;
  s->origin_ns = 1000;
  return 0;
}

static int source_look(struct source *s, struct vsync_look *out)
{
  const int64_t now = scan_monotonic_ns();
  const int64_t n = (now - s->origin_ns) / s->period_ns + 3;
  *out = (struct vsync_look){ s->origin_ns + n * s->period_ns, s->period_ns, 1 };
  return 0;
}
#endif

// ---- the record --------------------------------------------------------------------------------------------------

// A time of CLOCK_MONOTONIC as the counter's value (it may be ahead of now: a deadline), from one reading of both.
static uint64_t ticks_at(int64_t at_ns, int64_t now_ns, uint64_t now_counter, uint64_t hz)
{
  const int64_t away = at_ns - now_ns;
  const uint64_t ns = (uint64_t)(away < 0 ? -away : away);
  const uint64_t ticks = ns / 1000000000ull * hz + ns % 1000000000ull * hz / 1000000000ull;
  return away < 0 ? now_counter - ticks : now_counter + ticks;
}

static int64_t signed_ticks(int64_t us, uint64_t hz)
{
  const uint64_t a = (uint64_t)(us < 0 ? -us : us), ticks = a / 1000000 * hz + a % 1000000 * hz / 1000000;
  return us < 0 ? -(int64_t)ticks : (int64_t)ticks;
}

// The phase a session wants now: --phase-file's number (microseconds) when the file is there and holds one, else --phase-us.
static int64_t phase_now(const char *file, int64_t fallback_us)
{
  if (!file) return fallback_us;
  FILE *f = fopen(file, "re");
  if (!f) return fallback_us;
  long long us = 0;
  const int got = fscanf(f, "%lld", &us);
  fclose(f);
  return got == 1 && us > -500000 && us < 500000 ? (int64_t)us : fallback_us;
}

static int vsync_main(int argc, char **argv)
{
  long pid = 0, every_ms = 250, seconds = 0, say_s = 10, wait_s = 0;
  long long phase_us = 0, test_period_ns = 0;
  uint64_t offset = ~0ull;
  const char *phase_file = NULL, *test_guest = NULL;
  for (int i = 2; i < argc; i++) {
    if (i + 1 >= argc) return usage();
    const char *o = argv[i], *v = argv[++i];
    char *end = NULL;
    errno = 0;
    if (!strcmp(o, "--phase-file")) { phase_file = v; continue; }
    if (!strcmp(o, "--test-guest")) { test_guest = v; continue; }
    const long long n = strtoll(v, &end, 0);
    if (errno || !end || *end || end == v) return usage();
    if (!strcmp(o, "--pid")) pid = (long)n;
    else if (!strcmp(o, "--page-offset")) offset = (uint64_t)n;
    else if (!strcmp(o, "--phase-us")) phase_us = n;
    else if (!strcmp(o, "--every-ms")) every_ms = (long)n;
    else if (!strcmp(o, "--seconds")) seconds = (long)n;
    else if (!strcmp(o, "--say-s")) say_s = (long)n;
    else if (!strcmp(o, "--wait-s")) wait_s = (long)n;
    else if (!strcmp(o, "--test-period-ns")) test_period_ns = n;
    else return usage();
  }
  if (offset == ~0ull || (offset & (ONEIOS_VSYNC_PAGE_BYTES - 1)) || every_ms < 20 || every_ms > 1000 || seconds < 0 || say_s < 1 || say_s > 3600 || wait_s < 0 || wait_s > 3600 || phase_us <= -500000 || phase_us >= 500000 ||
      (test_guest ? pid != 0 || test_period_ns <= 0 : pid <= 1 || test_period_ns != 0)) return usage();
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const uint64_t hz = scan_counter_hz();
  if (!hz) { fprintf(stderr, "pacectl: the counter has no rate: nothing is published\n"); return 1; }
  int fd = test_guest ? open_test_guest(test_guest, offset + ONEIOS_VSYNC_PAGE_BYTES) : open_guest((pid_t)pid, offset);
  if (fd < 0) return 1;
  volatile uint64_t *page = mmap(NULL, ONEIOS_VSYNC_PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
  close(fd);
  if (page == MAP_FAILED) { fprintf(stderr, "pacectl: the page cannot be mapped to write: %s\n", strerror(errno)); return 1; }
  // Nothing is written until the guest's kernel has made the page its own: "OFSQ" at +0 (OneiOSDisplay.cpp writes it, with a
  // release store, once it has mapped the page). Before that the offset is only a guess at where the page is in the VM's RAM,
  // and the host never writes VM memory the guest has not claimed: an offset that is wrong never gets its magic, and nothing
  // is written. The magic is also checked again before every record (a guest that restarted rewrites the page).
  {
    const int64_t wait_start = scan_monotonic_ns();
    int told_wait = 0;
    while (__atomic_load_n(&page[0], __ATOMIC_ACQUIRE) != ONEIOS_FRAME_SEQ_MAGIC) {
      if (stopped) return 0;
      if (!test_guest && !alive((pid_t)pid)) { printf("pacectl: crosvm is gone before the guest's page was there\n"); return 1; }
      if (wait_s && scan_monotonic_ns() - wait_start >= (int64_t)wait_s * 1000000000) {
        fprintf(stderr, "pacectl: the page at offset 0x%" PRIx64 " has no frame-sequence magic after %ld s (the guest has not mapped it, or the offset is not the page): nothing is written\n", offset, wait_s);
        return 1;
      }
      if (!told_wait++) printf("pacectl: waiting for the guest to claim the page at offset 0x%" PRIx64 " (%s)\n", offset, wait_s ? "for the time asked" : "as long as crosvm lives");
      usleep(100000);
    }
  }
  struct source source;
  if (source_open(&source, test_period_ns)) return 1;
  volatile uint64_t *panel = page + ONEIOS_VSYNC_PANEL_OFFSET / 8, *guest = page + ONEIOS_VSYNC_GUEST_OFFSET / 8;
  printf("pacectl: the panel's vsync goes to the VM's page at offset 0x%" PRIx64 " every %ld ms (the counter at %" PRIu64 " Hz, phase %lld us)\n", offset, every_ms,
         hz, phase_us);
  const int64_t start_ns = scan_monotonic_ns();
  int64_t said_ns = start_ns, last_period_ns = 0, last_phase_us = phase_us;
  uint64_t written = 0, missed = 0, sent_was = 0, grid_was = 0, strange = 0;
  int told_source = 0, told_missing = 0, told_clock = 0, told_gone = 0;
  while (!stopped) {
    const int64_t turn_ns = scan_monotonic_ns();
    if (seconds && turn_ns - start_ns >= (int64_t)seconds * 1000000000) break;
    if (!test_guest && !alive((pid_t)pid)) { printf("pacectl: crosvm is gone\n"); break; }
    struct vsync_look look;
    if (source_look(&source, &look)) {
      missed++;
      if (!told_missing++) printf("pacectl: surfaceflinger gave no vsync this time: the record is left as it is (stale after %d s)\n", ONEIOS_VSYNC_STALE_S);
    } else {
      const int64_t now_ns = scan_monotonic_ns();
      const uint64_t now_counter = scan_counter();
      last_phase_us = phase_now(phase_file, phase_us);
      struct oneios_vsync_panel record = {
        .at = ticks_at(look.at_ns, now_ns, now_counter, hz),
        .period_q8 = (uint64_t)look.period_ns / 1000000000ull * hz * 256 + (uint64_t)look.period_ns % 1000000000ull * hz * 256 / 1000000000ull,
        .written = now_counter,
        .phase = signed_ticks(last_phase_us, hz),
      };
      if (__atomic_load_n(&page[0], __ATOMIC_ACQUIRE) != ONEIOS_FRAME_SEQ_MAGIC) {
        missed++;
        if (!told_gone++) printf("pacectl: the page lost its frame-sequence magic (the guest restarted?): nothing is written until it is back\n");
      } else if (oneios_vsync_usable(&record, now_counter, hz)) {
        oneios_vsync_write(panel, &record);
        written++;
        last_period_ns = look.period_ns;
        if (!told_source++) printf("pacectl: surfaceflinger's %s, %" PRId64 " ns apart (%.3f Hz)\n", look.deadline ? "frame deadlines" : "vsyncs", look.period_ns, 1e9 / (double)look.period_ns);
      } else {
        missed++;
        if (!told_missing++) printf("pacectl: surfaceflinger's vsync (%" PRId64 " ns apart) is no display's: not published\n", look.period_ns);
      }
    }
    // The kernel's own record: what it did with the grid, and whether its counter is this one.
    if (turn_ns - said_ns >= (int64_t)say_s * 1000000000) {
      said_ns = turn_ns;
      uint64_t tick_at = 0, sent = 0, on_grid = 0, gained = 0, lost = 0;
      int whole = 0;
      if (__atomic_load_n(&guest[ONEIOS_TICK_MAGIC], __ATOMIC_ACQUIRE) == ONEIOS_VSYNC_GUEST_MAGIC) {
        for (int tries = 0; tries < 3 && !whole; tries++) {
          const uint64_t before = oneios_rec_open(&guest[ONEIOS_TICK_SEQ]);
          if (!before) continue;
          tick_at = __atomic_load_n(&guest[ONEIOS_TICK_AT], __ATOMIC_RELAXED); sent = __atomic_load_n(&guest[ONEIOS_TICK_SENT], __ATOMIC_RELAXED);
          on_grid = __atomic_load_n(&guest[ONEIOS_TICK_LOCKED], __ATOMIC_RELAXED); gained = __atomic_load_n(&guest[ONEIOS_TICK_GAINED], __ATOMIC_RELAXED);
          lost = __atomic_load_n(&guest[ONEIOS_TICK_LOST], __ATOMIC_RELAXED);
          whole = oneios_rec_shut(&guest[ONEIOS_TICK_SEQ], before);
        }
      }
      if (!whole) {
        printf("pacectl: %" PRIu64 " records written (%" PRIu64 " turns without one); the kernel has written no record of its ticks (no vsync client yet, or a kernel without this)\n", written, missed);
      } else {
        const uint64_t now_counter = scan_counter();
        const int64_t ago = (int64_t)(now_counter - tick_at);
        // a tick the kernel sent in this side's future, or long ago while it keeps sending: its counter is not this one
        if (sent != sent_was && (ago < -(int64_t)(hz / 4) || ago > (int64_t)(5 * hz))) strange++;
        printf("pacectl: panel %" PRId64 " ns (%.3f Hz), phase %lld us; %" PRIu64 " records (%" PRIu64 " turns without); the kernel's ticks +%" PRIu64 ", on the panel's grid +%" PRIu64
               " (taken up %" PRIu64 ", given up %" PRIu64 ")\n", last_period_ns, last_period_ns ? 1e9 / (double)last_period_ns : 0.0, (long long)last_phase_us, written,
               missed, sent - sent_was, on_grid - grid_was, gained, lost);
        if (strange && !told_clock++)
          printf("pacectl: the kernel's last tick is %" PRId64 " ms from this counter's now: its counter is not this one, and its tick stays free (it refuses such a record)\n",
                 ago / (int64_t)(hz / 1000));
        sent_was = sent; grid_was = on_grid;
      }
    }
    for (long slept = 0; slept < every_ms && !stopped; slept += 10) usleep(10000);
  }
  printf("pacectl: ended after %" PRIu64 " records (%" PRIu64 " turns without one): the kernel's tick runs free %d s from now\n", written, missed, ONEIOS_VSYNC_STALE_S);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc >= 2 && !strcmp(argv[1], "vsync")) return vsync_main(argc, argv);
  return usage();
}
