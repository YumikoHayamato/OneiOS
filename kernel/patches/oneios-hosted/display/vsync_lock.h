// SPDX-License-Identifier: MIT
// The guest's vsync held to the panel's (native-perf, 2026-11-26; docs/native.md "Frame pacing"). Plain C: the kernel
// (OneiOSDisplay.cpp, the staged NativeLegacyDisplay.cpp's tick), the phone's publisher (runtime/pace/pacectl.c) and the
// host tests (runtime/pace/tests/test_pace.c) read the same layout and run the same arithmetic.
//
// The display's tick is a work-loop timer: next = last + 1/120 s of the guest's counter. The S25's panel is a
// command-mode one that refreshes by its own oscillator, and surfaceflinger latches a layer's buffer on that panel's
// vsync. Two free clocks of nearly the same rate slip past each other: whenever the guest's frames cross the latch, one
// is shown twice or two fall into one refresh, for as long as the crossing lasts, again and again at the difference of
// the rates. Here the tick is put on the panel's own grid.
//
// The page is the one the frame count lives in (boot argument oneios-frame-seq, OneiOSDisplay.cpp: 16 KiB of the VM's
// RAM that the host maps through crosvm's memfd). Android and the VM read one counter (Gunyah writes CNTVOFF_EL2 = 0 on
// every CPU and nowhere else: quic/gunyah-hypervisor hyp/vm/arm_vm_timer; boot 3's guest clock ran at the phone's
// uptime), so a time is the counter's value on both sides.
//
//   +0x100  the panel's record, written by the phone (pacectl vsync), read by the kernel at every tick
//           +0x00  "OVSY" and the version, 1                 written once
//           +0x08  the sequence: odd while the fields are being written, even when they are whole
//           +0x10  one vsync of the panel's grid (counter ticks): surfaceflinger's deadline for a frame, or its vsync
//           +0x18  the panel's period, in 1/256 of a tick
//           +0x20  the counter when this was written
//           +0x28  the phase: what the kernel adds to the grid for its tick (signed ticks; vm-boot --vsync-phase-us)
//   +0x180  the guest's record, written by the kernel at every tick, read by the phone
//           +0x00  "OTCK" and the version, 1
//           +0x08  the sequence, as above
//           +0x10  the counter at the last tick it sent
//           +0x18  ticks sent since boot
//           +0x20  of them, those placed on the panel's grid
//           +0x28  the times the grid was taken up, and (+0x30) given up
//
// Without a record, or with one that is stale (the publisher ended, or was never started: every session without
// vm-boot --vsync-lock), not whole, or not a display's (a period outside 1/240 to 1/24 s), the tick is what it was:
// last + the nominal interval. With one, the next tick is the first point of (vsync + phase + k x period) at least half
// a period away: one period after a tick that fired on time, never nearer than half a period, never farther than one
// and a half, whatever the record says. The kernel reads the page only with the boot argument oneios-vsync-lock=1 (its
// default is 0: an image without it ticks as before and writes no record).
//
// Order (arm64: the writer and the reader are on different CPUs, across the VM boundary): a record is a seqlock. The
// writer stores the sequence odd, a release fence, the fields, and the sequence even with release order; the reader
// loads the sequence with acquire order, the fields, an acquire fence, and the sequence again. oneios_rec_* below are
// those steps, shared by the kernel's record, the phone's record and the phone's reader of the kernel's.
#ifndef ONEIOS_VSYNC_LOCK_H
#define ONEIOS_VSYNC_LOCK_H

// uint32_t, uint64_t and int64_t come from the includer (the kernel's headers, or <stdint.h>).

#define ONEIOS_VSYNC_PAGE_BYTES 16384
#define ONEIOS_VSYNC_PANEL_OFFSET 0x100
#define ONEIOS_VSYNC_GUEST_OFFSET 0x180
#define ONEIOS_FRAME_SEQ_MAGIC 0x000000015153464fULL     // "OFSQ", version 1: the page's first word, written by the guest once it has mapped it
#define ONEIOS_VSYNC_PANEL_MAGIC 0x000000015953564fULL   // "OVSY", version 1
#define ONEIOS_VSYNC_GUEST_MAGIC 0x000000014b43544fULL   // "OTCK", version 1
// Words of the panel's record and of the guest's (8 bytes each, from the record's start).
enum { ONEIOS_VSYNC_MAGIC, ONEIOS_VSYNC_SEQ, ONEIOS_VSYNC_AT, ONEIOS_VSYNC_PERIOD_Q8, ONEIOS_VSYNC_WRITTEN, ONEIOS_VSYNC_PHASE, ONEIOS_VSYNC_WORDS };
enum { ONEIOS_TICK_MAGIC, ONEIOS_TICK_SEQ, ONEIOS_TICK_AT, ONEIOS_TICK_SENT, ONEIOS_TICK_LOCKED, ONEIOS_TICK_GAINED, ONEIOS_TICK_LOST, ONEIOS_TICK_WORDS };

// A record older than this (seconds of the counter) is no longer the panel's: the publisher writes several a second.
#define ONEIOS_VSYNC_STALE_S 2
// A display's period: 1/240 s to 1/24 s.
#define ONEIOS_VSYNC_MAX_HZ 240
#define ONEIOS_VSYNC_MIN_HZ 24


// ---- the seqlock steps of one record (a page of 8-byte words; `seq` is the record's sequence word)
// Writer: oneios_rec_begin, the fields as relaxed 64-bit stores, oneios_rec_end(seq from begin).
static inline uint64_t oneios_rec_begin(volatile uint64_t *seq)
{
    const uint64_t odd = __atomic_load_n(seq, __ATOMIC_RELAXED) | 1;
    __atomic_store_n(seq, odd, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);   // the odd sequence is visible before any field below
    return odd;
}
static inline void oneios_rec_end(volatile uint64_t *seq, uint64_t odd)
{
    __atomic_store_n(seq, odd + 1, __ATOMIC_RELEASE);   // every field is visible before the even sequence
}
// Reader: s = oneios_rec_open (0 when the writer is in it), the fields as relaxed loads, oneios_rec_shut(s): the same
// sequence after the fields, whole.
static inline uint64_t oneios_rec_open(const volatile uint64_t *seq)
{
    const uint64_t s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    return (s & 1) ? 0 : s;
}
static inline int oneios_rec_shut(const volatile uint64_t *seq, uint64_t s)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);   // the fields were loaded before the sequence is loaded again
    return __atomic_load_n(seq, __ATOMIC_RELAXED) == s;
}

// The panel's record as one reader saw it whole.
struct oneios_vsync_panel {
    uint64_t at, period_q8, written;
    int64_t phase;
};

// The record's words read whole: the sequence even and the same before and after. `words` is the mapped record
// (ONEIOS_VSYNC_WORDS of them). 1 and *out, or 0 (no record yet, another version, or the writer was in it three times).
static inline int oneios_vsync_read(const volatile uint64_t *words, struct oneios_vsync_panel *out)
{
    if (__atomic_load_n(&words[ONEIOS_VSYNC_MAGIC], __ATOMIC_ACQUIRE) != ONEIOS_VSYNC_PANEL_MAGIC) return 0;
    for (int tries = 0; tries < 3; tries++) {
        const uint64_t before = oneios_rec_open(&words[ONEIOS_VSYNC_SEQ]);
        if (!before) continue;   // odd (the writer is in it), or never written (0)
        out->at = __atomic_load_n(&words[ONEIOS_VSYNC_AT], __ATOMIC_RELAXED);
        out->period_q8 = __atomic_load_n(&words[ONEIOS_VSYNC_PERIOD_Q8], __ATOMIC_RELAXED);
        out->written = __atomic_load_n(&words[ONEIOS_VSYNC_WRITTEN], __ATOMIC_RELAXED);
        out->phase = (int64_t)__atomic_load_n(&words[ONEIOS_VSYNC_PHASE], __ATOMIC_RELAXED);
        if (oneios_rec_shut(&words[ONEIOS_VSYNC_SEQ], before)) return 1;
    }
    return 0;
}

// The writer's side of the same: the sequence odd, the fields, the sequence even (the magic first, once).
static inline void oneios_vsync_write(volatile uint64_t *words, const struct oneios_vsync_panel *in)
{
    const uint64_t seq = oneios_rec_begin(&words[ONEIOS_VSYNC_SEQ]);
    __atomic_store_n(&words[ONEIOS_VSYNC_AT], in->at, __ATOMIC_RELAXED);
    __atomic_store_n(&words[ONEIOS_VSYNC_PERIOD_Q8], in->period_q8, __ATOMIC_RELAXED);
    __atomic_store_n(&words[ONEIOS_VSYNC_WRITTEN], in->written, __ATOMIC_RELAXED);
    __atomic_store_n(&words[ONEIOS_VSYNC_PHASE], (uint64_t)in->phase, __ATOMIC_RELAXED);
    oneios_rec_end(&words[ONEIOS_VSYNC_SEQ], seq);
    __atomic_store_n(&words[ONEIOS_VSYNC_MAGIC], ONEIOS_VSYNC_PANEL_MAGIC, __ATOMIC_RELEASE);
}

// Whether a record can place a tick now: written within the last ONEIOS_VSYNC_STALE_S seconds and not after now (a
// quarter of a second of slack: the two sides read the counter at different moments), a display's period, a grid point
// within a minute of now. hz is the counter's rate.
static inline int oneios_vsync_usable(const struct oneios_vsync_panel *p, uint64_t now, uint64_t hz)
{
    if (!p || !hz) return 0;
    const uint64_t period = p->period_q8 >> 8;
    if (period < hz / ONEIOS_VSYNC_MAX_HZ || period > hz / ONEIOS_VSYNC_MIN_HZ) return 0;
    // its age, signed: the writer may have written it after this side read `now`
    const int64_t age = (int64_t)(now - p->written);
    if (age < -(int64_t)(hz / 4) || age > (int64_t)((uint64_t)ONEIOS_VSYNC_STALE_S * hz + hz / 4)) return 0;
    const int64_t away = (int64_t)(p->at - now);
    if (away > (int64_t)(60 * hz) || away < -(int64_t)(60 * hz)) return 0;
    const int64_t phase = p->phase;
    return phase < (int64_t)hz && phase > -(int64_t)hz;
}

// The next tick after `now` on the record's grid: the first point of (at + phase + k x period) that is half a period or
// more away. The record must be usable. The result is in (now + period / 2 - 1, now + 3 x period / 2 + 1].
static inline uint64_t oneios_vsync_on_grid(const struct oneios_vsync_panel *p, uint64_t now)
{
    const uint64_t period_q8 = p->period_q8;
    // everything relative to `now`, in 1/256 ticks, signed: the grid's base is within a minute and a second of now
    const int64_t base_q8 = ((int64_t)(p->at - now) + p->phase) * 256;
    const int64_t least_q8 = (int64_t)(period_q8 / 2);
    // the smallest k with base + k x period >= least
    int64_t k = (least_q8 - base_q8) / (int64_t)period_q8;
    if (base_q8 + k * (int64_t)period_q8 < least_q8) k++;
    const int64_t off_q8 = base_q8 + k * (int64_t)period_q8;
    return now + (uint64_t)((off_q8 + 128) >> 8);
}

// What the tick keeps between calls.
struct oneios_vsync_state {
    uint32_t locked;                   // the last tick was placed on the panel's grid
    uint64_t sent, on_grid, gained, lost;
};

// The time of the tick after the one that fired now. `next_free` is where the free-running timer would put it (the
// caller's last + interval, moved past `now` when it fell behind); `panel` is the record read just now, or NULL.
// *interval_out: the period the compositor is told (the panel's when on its grid, else `nominal`).
static inline uint64_t oneios_vsync_next(struct oneios_vsync_state *s, const struct oneios_vsync_panel *panel, uint64_t now, uint64_t next_free,
                                         uint64_t nominal, uint64_t hz, uint64_t *interval_out)
{
    s->sent++;
    if (!oneios_vsync_usable(panel, now, hz)) {
        if (s->locked) { s->locked = 0; s->lost++; }
        if (interval_out) *interval_out = nominal;
        return next_free;
    }
    if (!s->locked) { s->locked = 1; s->gained++; }
    s->on_grid++;
    if (interval_out) *interval_out = (panel->period_q8 + 128) >> 8;
    return oneios_vsync_on_grid(panel, now);
}

#endif
