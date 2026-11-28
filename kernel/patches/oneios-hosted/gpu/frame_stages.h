// SPDX-License-Identifier: MIT
// The arithmetic of the frame-stage lines (oneios-gpu-stages, oneios-gpu-ahead, oneios-gpu-waits, oneios-gpu-legs in
// OneiOSAdreno.cpp; oneios-display-pace in ../display/OneiOSDisplay.cpp), plain C so that runtime/xnuhost's host tests
// (tests/test_xnuhost.c, test_frame_stages) check the same code the kernel runs, as cpu_window.h.
//
// What the lines are for (docs/native.md, "Frame pacing"): oneios-gpu-report's "frames" is one number for three things, the
// host's part of a fenced submission (the doorbell, crosvm's worker, drm2kgsl, KGSL, the GPU, the fence coming back), the
// time until a guest thread looked at the used ring, and the time until the client's own waiter took its fence. The
// transport stamps a command when it is taken off the used ring; with that stamp the first two are told from the third.
// And a submission posted while unfenced commands (a blob made or released, a mapping) were still unanswered waited for
// the device to do those first: the two populations are kept apart.
#ifndef ONEIOS_FRAME_STAGES_H
#define ONEIOS_FRAME_STAGES_H

// uint32_t, uint64_t, size_t and snprintf come from the includer (the kernel's headers, or <stdint.h> and <stdio.h>).

// A time in microseconds by bucket: under 0.25, 0.5, 1, 2, 4, 8, 16 and 32 ms, then longer.
#define ONEIOS_STAGE_BUCKETS 9
static inline unsigned oneios_stage_bucket(uint64_t us)
{
    unsigned b = 0;
    for (uint64_t edge = 250; b < ONEIOS_STAGE_BUCKETS - 1 && us >= edge; edge *= 2) b++;
    return b;
}

// Count, total and largest (us), and the buckets. Added to from several threads: every field is its own atomic add, so
// a reader may see a count one ahead of its bucket, never a lost one.
struct oneios_stage_hist {
    uint64_t count, sum, max, bucket[ONEIOS_STAGE_BUCKETS];
};

static inline void oneios_stage_add(struct oneios_stage_hist *h, uint64_t us)
{
    __atomic_add_fetch(&h->bucket[oneios_stage_bucket(us)], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->sum, us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->count, 1, __ATOMIC_RELAXED);
    uint64_t seen = __atomic_load_n(&h->max, __ATOMIC_RELAXED);
    while (us > seen && !__atomic_compare_exchange_n(&h->max, &seen, us, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static inline void oneios_stage_read(const struct oneios_stage_hist *h, struct oneios_stage_hist *out)
{
    out->count = __atomic_load_n(&h->count, __ATOMIC_RELAXED);
    out->sum = __atomic_load_n(&h->sum, __ATOMIC_RELAXED);
    out->max = __atomic_load_n(&h->max, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < ONEIOS_STAGE_BUCKETS; i++) out->bucket[i] = __atomic_load_n(&h->bucket[i], __ATOMIC_RELAXED);
}

// "+a/+b/.../+h longer +i mean M us" of what was added between two readings (`was` to `now`), buckets 0 to `shown` - 1
// one by one and the rest together as "longer" (shown 1 to ONEIOS_STAGE_BUCKETS - 1). Returns the characters written
// (as snprintf: the text is cut at `size`, never past it).
static inline int oneios_stage_text(char *out, size_t size, const struct oneios_stage_hist *now, const struct oneios_stage_hist *was,
                                    unsigned shown)
{
    size_t at = 0;
    if (!size) return 0;
    out[0] = 0;
    if (shown < 1) shown = 1;
    if (shown > ONEIOS_STAGE_BUCKETS - 1) shown = ONEIOS_STAGE_BUCKETS - 1;
    uint64_t rest = 0;
    for (unsigned i = 0; i < ONEIOS_STAGE_BUCKETS; i++) {
        const uint64_t d = now->bucket[i] - was->bucket[i];
        if (i >= shown) { rest += d; continue; }
        const int n = snprintf(out + at, size - at, "%s+%llu", i ? "/" : "", (unsigned long long)d);
        if (n < 0 || (size_t)n >= size - at) return (int)(size - 1);
        at += (size_t)n;
    }
    const uint64_t count = now->count - was->count;
    const int n = snprintf(out + at, size - at, " longer +%llu mean %llu us", (unsigned long long)rest,
                           (unsigned long long)(count ? (now->sum - was->sum) / count : 0));
    if (n < 0 || (size_t)n >= size - at) return (int)(size - 1);
    return (int)(at + (size_t)n);
}

// The transport's own histograms (droidvm_virtio_gpu_legs, _kick_us, _irq_reap_us) are 12 buckets of microseconds by
// powers of two: under 32, 64 ... 32768, then more. Their change between two readings as 8 numbers: under 0.5 ms
// (buckets 0-4), 1, 2, 4, 8, 16 and 33 ms, then more. Returns the sum.
#define ONEIOS_LEG_BUCKETS 12
#define ONEIOS_LEG_SHOWN 8
static inline uint64_t oneios_leg_delta(const uint32_t now[ONEIOS_LEG_BUCKETS], const uint32_t was[ONEIOS_LEG_BUCKETS],
                                        uint64_t out[ONEIOS_LEG_SHOWN])
{
    uint64_t all = 0;
    for (unsigned i = 0; i < ONEIOS_LEG_SHOWN; i++) out[i] = 0;
    for (unsigned i = 0; i < ONEIOS_LEG_BUCKETS; i++) {
        const uint64_t d = (uint32_t)(now[i] - was[i]);   // the counters are 32 bits and wrap
        out[i < 5 ? 0 : i - 4] += d;
        all += d;
    }
    return all;
}

// The doorbell's and the interrupt's histograms the same way, by their own scale (a trapped write is tens of
// microseconds): under 32, 64, 128, 256, 512 us and 1 ms, then more (7 numbers). Returns the sum.
#define ONEIOS_KICK_SHOWN 7
static inline uint64_t oneios_kick_delta(const uint32_t now[ONEIOS_LEG_BUCKETS], const uint32_t was[ONEIOS_LEG_BUCKETS],
                                         uint64_t out[ONEIOS_KICK_SHOWN])
{
    uint64_t all = 0;
    for (unsigned i = 0; i < ONEIOS_KICK_SHOWN; i++) out[i] = 0;
    for (unsigned i = 0; i < ONEIOS_LEG_BUCKETS; i++) {
        const uint64_t d = (uint32_t)(now[i] - was[i]);
        out[i < ONEIOS_KICK_SHOWN - 1 ? i : ONEIOS_KICK_SHOWN - 1] += d;
        all += d;
    }
    return all;
}

// The gaps between one swap and the next (DroidVMVirtioGpuDisplay.cpp's droidvm_virtio_offer_gap_hist: 1 ms steps, the
// last for 40 ms and more) as what they mean at a refresh rate: [0] sooner than three quarters of a refresh (two swaps in
// one refresh), [1] one refresh apart (up to one and a half), [2] one refresh skipped (up to two and a half), [3] two or
// more skipped (up to 40 ms), [4] a pause (40 ms and more: no animation, or a stall). A step counts where its middle
// lies. hz 0 is taken as 60. Returns the swaps counted.
#define ONEIOS_GAP_STEPS 41
#define ONEIOS_GAP_CLASSES 5
static inline uint64_t oneios_gap_classes(const uint64_t now[ONEIOS_GAP_STEPS], const uint64_t was[ONEIOS_GAP_STEPS], unsigned hz,
                                          uint64_t out[ONEIOS_GAP_CLASSES])
{
    if (!hz) hz = 60;
    const uint64_t refresh_us = 1000000 / hz;
    uint64_t all = 0;
    for (unsigned i = 0; i < ONEIOS_GAP_CLASSES; i++) out[i] = 0;
    for (unsigned ms = 0; ms < ONEIOS_GAP_STEPS; ms++) {
        const uint64_t d = now[ms] - was[ms];
        const uint64_t middle_us = (uint64_t)ms * 1000 + 500;
        unsigned c;
        if (ms == ONEIOS_GAP_STEPS - 1) c = 4;
        else if (middle_us * 4 < refresh_us * 3) c = 0;
        else if (middle_us * 2 < refresh_us * 3) c = 1;
        else if (middle_us * 2 < refresh_us * 5) c = 2;
        else c = 3;
        out[c] += d;
        all += d;
    }
    return all;
}

// Frames a second from a count of gaps and their total (us), in tenths (1198 = 119.8); 0 without gaps.
static inline uint64_t oneios_gap_rate_tenths(uint64_t gaps, uint64_t total_us)
{
    return total_us ? gaps * 10000000ull / total_us : 0;
}

#endif
