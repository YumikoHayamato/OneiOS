// SPDX-License-Identifier: MIT
// pacectl on the build host (make test): a plain file in the place of crosvm's memfd, pacectl's own clock in
// surfaceflinger's (--test-guest, --test-period-ns), and this program in the kernel's: it reads the panel's record with
// the kernel's own code (vsync_lock.h) and places its ticks by it, as the staged NativeLegacyDisplay.cpp's tick does
// through OneiOSDisplay.cpp. The arithmetic itself is tested with the kernel's other headers (runtime/xnuhost
// tests/test_xnuhost.c, test_vsync_lock).
#define _GNU_SOURCE
#include "../../display/src/scantime.h"
#include "../../../kernel/patches/oneios-hosted/display/vsync_lock.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// xhd.c is the display's (pacectl links it for crosvm's mapping); this test needs none of it.
static void sleep_until(uint64_t counter_ns)
{
  struct timespec t = { (time_t)(counter_ns / 1000000000ull), (long)(counter_ns % 1000000000ull) };
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}

static int run(const char *command) { const int status = system(command); return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }

// How far `t` is from the grid of `period` through `origin`, either way.
static uint64_t off_grid(uint64_t t, uint64_t origin, uint64_t period)
{
  const uint64_t r = (t - origin) % period;
  return r < period - r ? r : period - r;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: test_pace PACECTL\n"); return 2; }
  const char *pacectl = argv[1];
  char dir[] = "/tmp/oneios-pace-test.XXXXXX", guest[64], out[64], phase[64], command[2048];
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  snprintf(guest, sizeof guest, "%s/guest.mem", dir);
  snprintf(out, sizeof out, "%s/out", dir);
  snprintf(phase, sizeof phase, "%s/phase", dir);
  const uint64_t hz = scan_counter_hz(), period = 8358021;              // this host's counter is nanoseconds; a panel at 119.65 Hz
  CHECK(hz == 1000000000ull);

  // What is refused: no page, a page that is not on a 16 KiB boundary, a device in the file's place, a file too short, a
  // crosvm that is not one (this program), a clock on the phone's binary only.
  snprintf(command, sizeof command, "%s vsync --test-guest %s --test-period-ns 8358021 > /dev/null 2>&1", pacectl, guest);
  CHECK(run(command) == 2);
  snprintf(command, sizeof command, "%s vsync --test-guest %s --test-period-ns 8358021 --page-offset 4096 > /dev/null 2>&1", pacectl, guest);
  CHECK(run(command) == 2);
  snprintf(command, sizeof command, "%s vsync --test-guest /dev/zero --test-period-ns 8358021 --page-offset 0 > %s 2>&1", pacectl, out);
  CHECK(run(command) == 1);
  int fd = open(guest, O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0 && ftruncate(fd, 16384) == 0);
  snprintf(command, sizeof command, "%s vsync --test-guest %s --test-period-ns 8358021 --page-offset 16384 > %s 2>&1 || grep -q 'not a plain file of 32768 bytes or more' %s", pacectl, guest, out, out);
  CHECK(run(command) == 0);
  snprintf(command, sizeof command, "%s vsync --pid %d --page-offset 16384 > %s 2>&1; [ $? = 1 ] && grep -q 'maps no shared \"/memfd:crosvm_guest\" over offset 0x4000' %s", pacectl, (int)getpid(), out, out);
  CHECK(run(command) == 0);
  snprintf(command, sizeof command, "%s vsync --pid 1 --page-offset 16384 > /dev/null 2>&1", pacectl);
  CHECK(run(command) == 2);
  snprintf(command, sizeof command, "%s vsync --test-guest %s --test-period-ns 8358021 --page-offset 16384 --phase-us 500000 > /dev/null 2>&1", pacectl, guest);
  CHECK(run(command) == 2);

  // The VM's RAM: 64 KiB, the page at 16 KiB. The kernel has written its frame count there; nothing of the panel yet.
  CHECK(ftruncate(fd, 65536) == 0);
  uint8_t *mem = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(mem != MAP_FAILED);
  if (mem == MAP_FAILED) return 1;
  volatile uint64_t *page = (volatile uint64_t *)(mem + 16384), *panel = page + ONEIOS_VSYNC_PANEL_OFFSET / 8, *mine = page + ONEIOS_VSYNC_GUEST_OFFSET / 8;
  // The guest has not claimed the page (no "OFSQ" at +0): pacectl waits for it, then gives up with exit 1 and writes nothing, not
  // a byte of the page or of the memory around it.
  CHECK(page[0] == 0);
  snprintf(command, sizeof command, "%s vsync --test-guest %s --test-period-ns 8358021 --page-offset 16384 --every-ms 20 --wait-s 1 > %s 2>&1; [ $? = 1 ] && "
           "grep -q 'has no frame-sequence magic after 1 s' %s && grep -q 'waiting for the guest to claim the page' %s", pacectl, guest, out, out, out);
  CHECK(run(command) == 0);
  {
    int clean = 1;
    for (size_t i = 0; i < 65536; i++) clean &= mem[i] == 0;
    CHECK(clean);
  }
  // A page whose first word is something else (another page's data: a wrong offset) is no claim either.
  page[0] = 0x1122334455667788ULL;
  snprintf(command, sizeof command, "%s vsync --test-guest %s --test-period-ns 8358021 --page-offset 16384 --every-ms 20 --wait-s 1 > %s 2>&1; [ $? = 1 ]", pacectl, guest, out);
  CHECK(run(command) == 0);
  CHECK(page[0] == 0x1122334455667788ULL && page[1] == 0 && panel[ONEIOS_VSYNC_MAGIC] == 0 && panel[ONEIOS_VSYNC_SEQ] == 0);
  page[0] = 0;
  // The guest claims it a moment after pacectl started: nothing is written before, records after.
  pid_t waiter = fork();
  if (!waiter) {
    int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(o, 1); dup2(o, 2);
    execl(pacectl, pacectl, "vsync", "--test-guest", guest, "--test-period-ns", "8358021", "--page-offset", "16384", "--every-ms", "20", "--wait-s", "10",
          "--seconds", "1", (char *)NULL);
    _exit(127);
  }
  CHECK(waiter > 0);
  usleep(500000);
  CHECK(page[0] == 0 && panel[ONEIOS_VSYNC_MAGIC] == 0 && panel[ONEIOS_VSYNC_SEQ] == 0 && panel[ONEIOS_VSYNC_AT] == 0);   // half a second in: still nothing
  __atomic_store_n(&page[0], 0x000000015153464fULL, __ATOMIC_RELEASE);
  {
    struct oneios_vsync_panel first;
    int got = 0;
    for (int i = 0; i < 100 && !got; i++) { got = oneios_vsync_read(panel, &first); if (!got) usleep(10000); }
    CHECK(got);
  }
  {
    int status_w = 0;
    CHECK(waitpid(waiter, &status_w, 0) == waiter && WIFEXITED(status_w) && WEXITSTATUS(status_w) == 0);
  }
  // A restarted guest starts from a page of zeros and claims it again
  memset(mem, 0, 65536);
  page[0] = 0x000000015153464fULL;
  page[1] = 41;
  struct oneios_vsync_panel record = { 0 };
  CHECK(!oneios_vsync_read(panel, &record));

  // pacectl for 3 s, writing every 20 ms, saying what the kernel did every second; the phase from a file from the middle on.
  pid_t child = fork();
  if (!child) {
    int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(o, 1); dup2(o, 2);
    execl(pacectl, pacectl, "vsync", "--test-guest", guest, "--test-period-ns", "8358021", "--page-offset", "16384", "--every-ms", "20", "--say-s", "1",
          "--seconds", "3", "--phase-file", phase, (char *)NULL);
    _exit(127);
  }
  CHECK(child > 0);
  for (int i = 0; i < 100 && !oneios_vsync_read(panel, &record); i++) usleep(10000);
  uint64_t now = scan_counter();
  CHECK(oneios_vsync_read(panel, &record) && oneios_vsync_usable(&record, now, hz));
  // The record: the stand-in panel's period to 1/256 of a tick, a point of its grid a few periods ahead (the deadline), no phase.
  // (pacectl's clock: a vsync every 8358021 ns from 1000 ns of CLOCK_MONOTONIC; its two clock reads are a moment apart.)
  CHECK(record.period_q8 == period * 256 && record.phase == 0 && record.at > now && record.at - now < 5 * period);
  CHECK(off_grid(record.at, 1000, period) < 200000);
  CHECK(now - record.written < 500000000);

  // The kernel's side for 1.2 s: every tick is placed by the record read just before it, as OneiOSDisplay.cpp does.
  struct oneios_vsync_state state = { 0 };
  const uint64_t nominal = 8333333;
  uint64_t tick = now + nominal, told = 0, worst = 0, least = ~0ull, most = 0, ticks = 0, phased = 0;
  while (scan_counter() < now + 1200000000ull) {
    sleep_until(tick);
    const uint64_t at = scan_counter();
    struct oneios_vsync_panel seen;
    const int have = oneios_vsync_read(panel, &seen);
    const uint64_t next = oneios_vsync_next(&state, have ? &seen : NULL, at, tick + nominal <= at ? at + nominal : tick + nominal, nominal, hz, &told);
    // the kernel's own record, for pacectl's line
    const uint64_t seq = mine[ONEIOS_TICK_SEQ] | 1;
    mine[ONEIOS_TICK_SEQ] = seq; mine[ONEIOS_TICK_AT] = at; mine[ONEIOS_TICK_SENT] = state.sent; mine[ONEIOS_TICK_LOCKED] = state.on_grid;
    mine[ONEIOS_TICK_GAINED] = state.gained; mine[ONEIOS_TICK_LOST] = state.lost; mine[ONEIOS_TICK_SEQ] = seq + 1; mine[ONEIOS_TICK_MAGIC] = ONEIOS_VSYNC_GUEST_MAGIC;
    if (have && state.locked) {
      // where the next tick is put: on the stand-in panel's grid, moved by the record's phase
      const uint64_t off = off_grid(next - (uint64_t)seen.phase, 1000, period);
      if (off > worst) worst = off;
      if (next - at < least) least = next - at;
      if (next - at > most) most = next - at;
      if (seen.phase) phased++;
    }
    tick = next; ticks++;
    // half way: the session asks for a phase of 2 ms
    if (ticks == 60) { FILE *f = fopen(phase, "w"); fprintf(f, "2000\n"); fclose(f); }
  }
  // on the grid from the first tick, never nearer than half a period nor farther than one and a half, the compositor told the
  // panel's period; about one tick a refresh (1.2 s are 143.6 of them)
  CHECK(state.locked && state.gained == 1 && state.lost == 0 && state.on_grid == ticks && told == period);
  CHECK(worst < 200000 && least >= period / 2 && most <= period * 3 / 2 + 1);
  CHECK(ticks >= 135 && ticks <= 146);
  // the phase from the file reached the record and moved the ticks (2 ms are 2000000 of this counter's ticks)
  CHECK(oneios_vsync_read(panel, &record) && record.phase == 2000000 && phased > 20);
  CHECK(page[0] == 0x000000015153464fULL && page[1] == 41);            // the frame count beside it is untouched

  // pacectl ends by itself; two seconds later its last record is stale and the tick runs free again.
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  usleep(2400000);
  now = scan_counter();
  struct oneios_vsync_panel last;
  CHECK(oneios_vsync_read(panel, &last) && !oneios_vsync_usable(&last, now, hz));
  CHECK(oneios_vsync_next(&state, &last, now, now + nominal, nominal, hz, &told) == now + nominal && told == nominal && !state.locked && state.lost == 1);
  // what it said: its start, the source, a line a second with the kernel's ticks on the grid, its end
  snprintf(command, sizeof command,
           "grep -q \"^pacectl: the panel's vsync goes to the VM's page at offset 0x4000 every 20 ms (the counter at 1000000000 Hz, phase 0 us)\" %s && "
           "grep -q \"^pacectl: surfaceflinger's frame deadlines, 8358021 ns apart (119.646 Hz)\" %s && "
           "grep -q -E \"^pacectl: panel 8358021 ns \\(119.646 Hz\\), phase (0|2000) us; [0-9]+ records \\(0 turns without\\); the kernel's ticks \\+[0-9]+, on the panel's grid \\+[1-9][0-9]* \\(taken up 1, given up 0\\)\" %s && "
           "tail -n 1 %s | grep -q -E \"^pacectl: ended after 1[0-9][0-9] records \\(0 turns without one\\): the kernel's tick runs free 2 s from now\"",
           out, out, out, out);
  CHECK(run(command) == 0);
  if (failed) { snprintf(command, sizeof command, "cat %s", out); run(command); }
  munmap(mem, 65536);
  close(fd);
  unlink(guest); unlink(out); unlink(phase); rmdir(dir);
  printf("%s: %d checks, %d failed\n", failed ? "FAILED" : "ok", checks, failed);
  return failed ? 1 : 0;
}
