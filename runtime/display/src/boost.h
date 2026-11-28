// SPDX-License-Identifier: MIT
// The touch boost (displayctl touch --boost, `oneios-tool phone vm-boot --touch-boost`): while fingers move, the lowest
// clocks (devfreq's min_freq, cpufreq's scaling_min_freq) are raised, and put back BOOST hold seconds after the last touch.
// A process of its own does it, told by a pipe, so that a slow write to a clock's file never delays a touch; the pipe's
// end (the forwarder ended or was killed, whatever the signal) puts the clocks back too. Before the first write the
// values to put back go to a state file, which is there only while something is raised: boot.sh writes them back from it
// when both processes were killed.
// What is put back is the lowest value a file had while nothing was raised here, and for the first settle seconds only
// that is read and nothing raised: Samsung's own boosts hold a minimum for some seconds around the start (take12 saved
// 2227200 and 2246400 kHz as the originals; take13's clocks were below both soon after).
#ifndef ONEIOS_BOOST_H
#define ONEIOS_BOOST_H
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BOOST_MAX 8
struct boost_node { char path[128]; long target, low; int raised; };
struct boost {
  struct boost_node node[BOOST_MAX];
  unsigned n;
  const char *state;
  double hold_s, settle_s;
  unsigned long raises;
  int up, told, refused;
  int left;                            // the forwarder's: the boost's process ended and its file is still to be used
  double left_at;
};

static inline double boost_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec / 1e9;
}

// PATH=VALUE,PATH=VALUE...; 0, or -1 for anything else.
static inline int boost_parse(struct boost *b, const char *list)
{
  char copy[1024];
  if (strlen(list) >= sizeof copy) return -1;
  strcpy(copy, list);
  for (char *save = NULL, *one = strtok_r(copy, ",", &save); one; one = strtok_r(NULL, ",", &save)) {
    char *eq = strchr(one, '='), *end;
    if (!eq || b->n == BOOST_MAX || one[0] != '/' || (size_t)(eq - one) >= sizeof b->node[0].path || strpbrk(one, " \n")) return -1;
    *eq = 0;
    struct boost_node *d = &b->node[b->n];
    errno = 0;
    d->target = strtol(eq + 1, &end, 10);
    if (errno || *end || end == eq + 1 || d->target <= 0) return -1;
    strcpy(d->path, one);
    d->low = 0;
    d->raised = 0;
    b->n++;
  }
  return b->n ? 0 : -1;
}

static inline long boost_read(const char *path)
{
  char text[32];
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  ssize_t n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n <= 0) return 0;
  text[n] = 0;
  return strtol(text, NULL, 10);
}

static inline int boost_write(const char *path, long value)
{
  char text[32];
  int n = snprintf(text, sizeof text, "%ld\n", value);
  int fd = open(path, O_WRONLY | O_CLOEXEC | O_TRUNC);
  if (fd < 0) return -1;
  ssize_t w = write(fd, text, (size_t)n);
  if (close(fd) || w != n) return -1;
  return 0;
}

// What nothing here raised: the lowest seen is the original.
static inline void boost_sample(struct boost *b)
{
  for (unsigned i = 0; i < b->n; i++) {
    struct boost_node *d = &b->node[i];
    if (d->raised) continue;
    long v = boost_read(d->path);
    if (v > 0 && (!d->low || v < d->low)) d->low = v;
  }
}

// The kernel's name for this boot, 0 when it cannot be read.
static inline int boost_boot(char id[64])
{
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  ssize_t r = fd < 0 ? -1 : read(fd, id, 63);
  if (fd >= 0) close(fd);
  id[r > 0 ? r : 0] = 0;
  id[strcspn(id, "\n")] = 0;
  return id[0] != 0;
}

// A state file that is there at the start, of this boot: its clocks are still raised (boot.sh could not put them back), and
// what they hold now is no original. They are this process's to put back, those outside its list too.
static inline void boost_adopt(struct boost *b)
{
  char id[64], line[256], path[128];
  long value;
  FILE *f = fopen(b->state, "re");
  if (!f) return;
  if (boost_boot(id) && fgets(line, sizeof line, f) && !strncmp(line, "boot ", 5) && !strncmp(line + 5, id, strlen(id)) && line[5 + strlen(id)] == '\n') {
    while (fgets(line, sizeof line, f)) {
      if (sscanf(line, "%127s %ld", path, &value) != 2 || value <= 0) continue;
      unsigned i = 0;
      while (i < b->n && strcmp(b->node[i].path, path)) i++;
      if (i == b->n) {
        const size_t l = strlen(path);
        if (b->n == BOOST_MAX || strncmp(path, "/sys/", 5) || strstr(path, "..") || l < 8 || strcmp(path + l - 8, "min_freq")) continue;
        b->node[b->n++] = (struct boost_node){ .target = 0 };      // never raised here
        strcpy(b->node[i].path, path);
      }
      b->node[i].low = value;
      b->node[i].raised = 1;
    }
  }
  fclose(f);
}

static inline int boost_any_raised(const struct boost *b)
{
  for (unsigned i = 0; i < b->n; i++) if (b->node[i].raised) return 1;
  return 0;
}

// The state file: `boot ID`, then `PATH VALUE` for every clock about to be raised. Whole or not there.
static inline int boost_save(const struct boost *b)
{
  char tmp[512], id[64];
  if (snprintf(tmp, sizeof tmp, "%s.new", b->state) >= (int)sizeof tmp) return -1;
  if (!boost_boot(id)) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if (!f) { if (fd >= 0) close(fd); return -1; }
  int bad = fprintf(f, "boot %s\n", id) < 0;
  for (unsigned i = 0; i < b->n; i++)
    if (b->node[i].raised || (b->node[i].low && b->node[i].low < b->node[i].target)) bad |= fprintf(f, "%s %ld\n", b->node[i].path, b->node[i].low) < 0;
  if (fclose(f)) bad = 1;
  return bad || rename(tmp, b->state) ? -1 : 0;
}

static inline void boost_raise(struct boost *b)
{
  boost_sample(b);
  if (boost_save(b)) {
    if (!b->told++) { fprintf(stderr, "displayctl touch: boost: %s: %s: the clocks are left alone\n", b->state, strerror(errno)); }
    return;
  }
  for (unsigned i = 0; i < b->n; i++) {
    struct boost_node *d = &b->node[i];
    if (!d->low || d->low >= d->target) continue;
    d->raised = 1;                     // before the write: a write that failed half way is put back too
    if (boost_write(d->path, d->target) && !b->told++)
      fprintf(stderr, "displayctl touch: boost: %s: %s\n", d->path, strerror(errno));
  }
  b->up = 1;
  if (!b->raises++) {
    printf("displayctl touch: boost: the first touch raised");
    for (unsigned i = 0; i < b->n; i++)
      if (b->node[i].raised) printf(" %s %ld (it was %ld, now %ld)", b->node[i].path, b->node[i].target, b->node[i].low, boost_read(b->node[i].path));
    printf("\n");
    fflush(stdout);
  }
}

// 0 when every clock is back and the state file is gone; a refused write keeps the file, and the clock raised: boost_run
// tries again, and boot.sh after it. Said once until they are all back.
static inline int boost_release(struct boost *b)
{
  int bad = 0;
  for (unsigned i = 0; i < b->n; i++) {
    struct boost_node *d = &b->node[i];
    if (!d->raised) continue;
    if (boost_write(d->path, d->low)) {
      if (!b->refused)
        fprintf(stderr, "displayctl touch: boost: %s could not be put back at %ld: %s: tried again every second\n", d->path, d->low, strerror(errno));
      bad = 1;
    } else {
      d->raised = 0;
    }
  }
  b->up = 0;
  if (!bad && b->refused) fprintf(stderr, "displayctl touch: boost: every clock is back after all\n");
  b->refused = bad;
  if (!bad) unlink(b->state);
  return bad;
}

// The boost's process: a byte on the pipe is a touch, the pipe's end is the end. Its exit status.
static inline int boost_run(struct boost *b, int touches)
{
  const double start = boost_now();
  double last = 0;
  boost_adopt(b);
  for (;;) {
    const double t = boost_now();
    int wait_ms = -1;
    if (t < start + b->settle_s) wait_ms = 1000;
    const int again = !b->up && boost_any_raised(b);      // a put-back was refused (a cluster's cores offline)
    if (again) wait_ms = 1000;
    if (b->up) {
      wait_ms = (int)((last + b->hold_s - t) * 1000) + 1;
      if (wait_ms < 1) { boost_release(b); continue; }
    }
    struct pollfd p = { touches, POLLIN, 0 };
    int r = poll(&p, 1, wait_ms);
    if (r < 0 && errno != EINTR) break;
    if (r <= 0) {
      if (again && !r) boost_release(b);
      if (!b->up) boost_sample(b);
      continue;
    }
    char sink[64];
    ssize_t n = read(touches, sink, sizeof sink);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (n <= 0) break;
    last = boost_now();
    if (last < start + b->settle_s) boost_sample(b);
    else if (!b->up) boost_raise(b);
  }
  int bad = boost_release(b);
  for (int k = 0; bad && k < 5; k++) { usleep(200000); bad = boost_release(b); }
  printf("displayctl touch: boost: raised %lu times, %s\n", b->raises, bad ? "a clock could not be put back" : "every clock is back");
  for (unsigned i = 0; i < b->n; i++) printf("displayctl touch: boost: %s is %ld\n", b->node[i].path, boost_read(b->node[i].path));
  fflush(stdout);
  return bad;
}

// The clocks put back from the state file alone (the boost's process was killed); only the paths of the list. 0 when there
// is nothing left to put back.
static inline int boost_restore_state(const struct boost *b)
{
  FILE *f = fopen(b->state, "re");
  if (!f) return errno != ENOENT;
  char line[256], path[200];
  long value;
  int bad = 0;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "%199s %ld", path, &value) != 2 || value <= 0) continue;
    for (unsigned i = 0; i < b->n; i++)
      if (!strcmp(b->node[i].path, path)) bad |= boost_write(path, value) != 0;
  }
  fclose(f);
  if (!bad) unlink(b->state);
  return bad;
}

// The forwarder's look at the boost's process, at every turn of its loop: one that ended, however, has its clocks put back
// at once (with no touch nothing else would tell), and tried again every second while a write is refused.
static inline void boost_watch(struct boost *b, pid_t *child, int *touched, double t)
{
  if (*child > 0) {
    int status;
    const pid_t done = waitpid(*child, &status, WNOHANG);
    if (!done || (done < 0 && errno == EINTR)) return;
    *child = -1;
    if (*touched >= 0) close(*touched);
    *touched = -1;
    b->left = 1;
    b->left_at = t;
  }
  if (!b->left || t < b->left_at) return;
  b->left_at = t + 1;
  if (boost_restore_state(b)) return;
  b->left = 0;
  printf("displayctl touch: boost: its process ended: no clock is left raised\n");
  fflush(stdout);
}
#endif
