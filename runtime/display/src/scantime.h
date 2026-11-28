// SPDX-License-Identifier: MIT
// The scan time of each touch frame for the VM (displayctl touch --scan-time, `oneios-tool phone vm-boot --touch-time`).
//
// The guest's virtio-input is polled every 4 ms (no interrupt on crosvm's PCI) and the S25's touchscreen sends no
// MSC_TIMESTAMP, so the kernel stamped every touch report with the time it made it: 0 to 4 ms and the path's jitter after
// the finger was there, a different delay on each frame. UIKit places a moving finger by those times, so content that
// follows a finger was off by the finger's speed times that error on every frame, although every frame was drawn on time.
// The hosted path has had the cure since 2026-11-12 (runtime/xnuhost/src/input.c, kernel/patches/oneios-hosted/hid/
// OneiOSTouchTime.inc, hid/README.md "Latency"): before a frame's SYN_REPORT the guest gets EV_MSC / MSC_TIMESTAMP, the low
// 32 bits of the time Linux's input core stamped the frame with, in ticks of the counter the guest's mach_absolute_time
// reads, and the HID report goes to iOS with that time (handleReportWithTime). The same kernel code runs in the native
// VM (the staged DroidVMVirtioInput.c of both kernels: `oneios-hid: touch latency: N frames without a usable scan time`
// in every native session so far), and crosvm passes whatever its multi-touch socket is sent (devices/src/virtio/input/
// event_source.rs: every 8-byte event read is queued for the guest, none filtered by the device's own event bits).
//
// The two clocks are one: Gunyah writes CNTVOFF_EL2 = 0 on every CPU and nowhere else (quic/gunyah-hypervisor,
// hyp/vm/arm_vm_timer/aarch64/src/arm_vm_timer.c), so Android and the VM read the same CNTVCT_EL0 (boot 3 of 2026-11-23:
// the guest's 1970 clock ran at the phone's uptime). Should a hypervisor ever give the VM another count, the guest
// refuses a time in its future or more than 1 s old and stamps the report itself, as before (OneiOSTouchTime.inc; its
// log line then keeps counting "frames without a usable scan time").
#ifndef ONEIOS_SCANTIME_H
#define ONEIOS_SCANTIME_H
#include "xhd.h"
#include <stdint.h>
#include <time.h>

#define SCAN_EV_MSC 4
#define SCAN_MSC_TIMESTAMP 5
// A frame's time is used only this close to now: the guest takes none older (ONEIOS_TOUCH_SCAN_MAX_MS), and one in the
// future is another clock's (the device did not take CLOCK_MONOTONIC after all).
#define SCAN_MAX_AGE_NS 1000000000ll
#define SCAN_FUTURE_NS 1000000ll

static inline int64_t scan_monotonic_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

// The counter the guest's mach_absolute_time reads, and its rate.
#if defined(__aarch64__)
static inline uint64_t scan_counter(void) { uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline uint64_t scan_counter_hz(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#else
// the build host's tests: a counter of nanoseconds
static inline uint64_t scan_counter(void) { return (uint64_t)scan_monotonic_ns(); }
static inline uint64_t scan_counter_hz(void) { return 1000000000ull; }
#endif

// The frame's scan time as the guest's counter (xnuhost's scan_ticks): the counter now, less the time since the scan;
// the low 32 bits are what the event carries.
static inline uint32_t scan_ticks(int64_t scan_ns, int64_t now_ns, uint64_t now_counter, uint64_t hz)
{
  const uint64_t ago = now_ns > scan_ns ? (uint64_t)(now_ns - scan_ns) : 0;
  const uint64_t ticks = ago / 1000000000ull * hz + ago % 1000000000ull * hz / 1000000000ull;
  return (uint32_t)(now_counter - ticks);
}

static inline int scan_usable(int64_t scan_ns, int64_t now_ns)
{
  return scan_ns > 0 && scan_ns <= now_ns + SCAN_FUTURE_NS && now_ns - scan_ns < SCAN_MAX_AGE_NS;
}

// What the forwarder says at its end: the frames sent with their scan time and without one, and how long after its scan
// the forwarder had each frame (us: sum, least, most).
struct scan_stats { unsigned long stamped, bare; uint64_t sum_us, least_us, most_us; };

// A whole frame (its last event the SYN_REPORT) with the scan time put before that SYN_REPORT: the new length. A frame
// whose time is not usable, or with no room left, goes as it is.
static inline unsigned scan_stamp(struct xhd_vinput *frame, unsigned n, unsigned room, int64_t scan_ns, int64_t now_ns, uint64_t now_counter,
                                  uint64_t hz, struct scan_stats *stats)
{
  if (!n || n >= room || !hz || !scan_usable(scan_ns, now_ns)) {
    if (stats) stats->bare++;
    return n;
  }
  frame[n] = frame[n - 1];
  frame[n - 1] = (struct xhd_vinput){ SCAN_EV_MSC, SCAN_MSC_TIMESTAMP, scan_ticks(scan_ns, now_ns, now_counter, hz) };
  if (stats) {
    const uint64_t us = now_ns > scan_ns ? (uint64_t)(now_ns - scan_ns) / 1000 : 0;
    stats->stamped++;
    stats->sum_us += us;
    if (stats->stamped == 1 || us < stats->least_us) stats->least_us = us;
    if (us > stats->most_us) stats->most_us = us;
  }
  return n + 1;
}
#endif
