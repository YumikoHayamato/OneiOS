// SPDX-License-Identifier: MIT
// take17 (2026-11-26): the arithmetic of OneiOSAdreno.cpp's event lines (watch::), plain C so that runtime/xnuhost's
// host tests (tests/test_xnuhost.c, test_watch) check the same code the kernel runs.
//
// The 10 s report could not say what three of take16's faults were:
//   - a web game froze when a tap started an effect or the player hit an obstacle: the 10 s lines only count "pauses"
//     (gaps of 40 ms and more between swaps), and take15's five freezes of the same game were in none of them either;
//   - YouTube at 1080p showed a black picture for seconds while the sound went on: the lines say the render server's GPU
//     blobs were at 1.9 to 2.0 GiB whenever such a video played (take14 1906, take15 1987, take16 2028 MiB), which is the
//     whole GPU address range the phone's crosvm gives a context (crosvm.log: `VA slices: 64 x 2GB`), and that the
//     decoder's share of a core fell from 60-80 % to 7-33 % in some windows, but not what happened in which second;
//   - when the memory pressure level changed (every process is then told to give memory back).
// So three kinds of lines are written as things happen, each with the time of day (UTC) for the phone's own logs:
//   oneios-hitch:     a gap of 150 ms to 5 s between two swaps that came right after frames (the swap before it was
//                     less than 50 ms earlier), with the processes that had the CPUs in it and what the GPU's clients
//                     submitted in it. A freeze with a busy process names it; one with nothing busy is a wait (the
//                     network, the disk, another process) or simply an animation's end and the next one's start.
//   oneios-gpu-room:  a client's GPU blobs against a context's part of the GPU addresses (the slice the host's renderer
//                     cuts for each context, 2 GiB on the phone; Turnip takes it as the context's whole budget): when
//                     it passes 90 %, and when it lets go of an eighth of it or more within a second (a trim, or the
//                     surfaces of a video that ended).
//   oneios-video:     each second in which a video decoder (videocodecd, mediaplaybackd) locked surfaces: how many
//                     locks, the swaps, the render server's GPU blobs against its range, the memory available and the
//                     pressure level. A picture that stops with the sound going on is a second with few or no locks.
#ifndef ONEIOS_WATCH_H
#define ONEIOS_WATCH_H

// uint32_t, uint64_t and memset/strncmp come from the includer (the kernel's headers in OneiOSAdreno.cpp, <stdint.h> and
// <string.h> in the host test).

#define ONEIOS_WATCH_TICKS 64        // the sampler's ring: 6.4 s of its 100 ms ticks
#define ONEIOS_WATCH_PROCS 12        // the busiest processes kept of a tick
#define ONEIOS_WATCH_SHOWN 5         // and of a gap's sum, in its line
#define ONEIOS_HITCH_MOST_US 5000000ull    // a longer gap is the screen at rest, not a freeze
#define ONEIOS_HITCH_FRAMES_US 50000ull    // "right after frames": the swap before came within this

// Whether a gap between two swaps is a candidate: long enough, not the screen at rest, and right after frames.
// least_us 0: the lines are off.
static inline int oneios_hitch_is(uint64_t gap_us, uint64_t before_us, uint64_t least_us)
{
    return least_us && gap_us >= least_us && gap_us <= ONEIOS_HITCH_MOST_US && before_us && before_us < ONEIOS_HITCH_FRAMES_US;
}

// One process's CPU time in a tick, and a tick: when it was taken, the CPU time of every process in it, the busiest
// ones (largest first), and what the GPU's clients did in it (the render server's fenced submissions apart).
struct oneios_watch_proc { int pid; uint32_t us; char name[20]; };
struct oneios_watch_tick {
    uint64_t at_us, all_us;
    uint32_t count, app_submits, rs_submits, blobs;
    uint32_t slow_blobs;                 // take20: blobs whose CREATE_BLOB took the device 8 ms or more
    struct oneios_watch_proc procs[ONEIOS_WATCH_PROCS];
};
// A window's sum: the time the ticks it took cover, every process's CPU time, the busiest, and the GPU's counts.
struct oneios_watch_sum {
    uint64_t span_us, all_us;
    uint32_t count, ticks, app_submits, rs_submits, blobs;
    uint32_t slow_blobs;
    struct oneios_watch_proc procs[ONEIOS_WATCH_PROCS];
};

// A process's time put into a list kept largest first (a tick's as it is walked, a sum's as ticks are added): added to
// its entry when the pid is there, else a new entry, the smallest one going when the list is full and this is larger.
static inline void oneios_watch_put(struct oneios_watch_proc *list, uint32_t *count, int pid, const char *name, uint32_t us)
{
    uint32_t at = *count;
    for (uint32_t i = 0; i < *count; i++) {
        if (list[i].pid == pid) { at = i; break; }
    }
    if (at == *count) {
        if (*count == ONEIOS_WATCH_PROCS) {
            if (list[*count - 1].us >= us) return;
            at = *count - 1;
        } else {
            (*count)++;
        }
        list[at].pid = pid;
        list[at].us = 0;
        unsigned k = 0;
        for (; name && name[k] && k + 1 < sizeof list[at].name; k++) list[at].name[k] = name[k];
        list[at].name[k] = 0;
    }
    list[at].us += us;
    while (at > 0 && list[at - 1].us < list[at].us) {   // one step of an insertion sort: the list was in order
        const struct oneios_watch_proc swap = list[at - 1];
        list[at - 1] = list[at];
        list[at] = swap;
        at--;
    }
}

// The ticks of a ring that cover any of [from_us, to_us], summed. `head` ticks were taken so far (the newest is
// ring[(head - 1) % ONEIOS_WATCH_TICKS]); a tick covers the time since the one before it, so the first of a ring has no
// span and is left out. Returns how many ticks were taken.
static inline uint32_t oneios_watch_sum_window(const struct oneios_watch_tick *ring, uint64_t head, uint64_t from_us, uint64_t to_us,
                                               struct oneios_watch_sum *out)
{
    memset(out, 0, sizeof *out);
    const uint64_t have = head < ONEIOS_WATCH_TICKS ? head : ONEIOS_WATCH_TICKS;
    for (uint64_t back = have; back > 1; back--) {           // oldest first, each with the one before it
        const struct oneios_watch_tick *t = &ring[(head - back + 1) % ONEIOS_WATCH_TICKS], *before = &ring[(head - back) % ONEIOS_WATCH_TICKS];
        if (t->at_us <= from_us || before->at_us >= to_us || t->at_us <= before->at_us) continue;
        out->ticks++;
        out->span_us += t->at_us - before->at_us;
        out->all_us += t->all_us;
        out->app_submits += t->app_submits;
        out->rs_submits += t->rs_submits;
        out->blobs += t->blobs;
        out->slow_blobs += t->slow_blobs;
        for (uint32_t i = 0; i < t->count; i++) oneios_watch_put(out->procs, &out->count, t->procs[i].pid, t->procs[i].name, t->procs[i].us);
    }
    return out->ticks;
}

// What a gap looks like from its sum: 2 a process had a CPU for half of it or more (a freeze with its doer), 1 the
// CPUs did work for half of it or more among several, 0 nothing much ran (a wait, or the screen between two animations).
static inline int oneios_hitch_kind(const struct oneios_watch_sum *sum, uint64_t gap_us)
{
    if (sum->count && (uint64_t)sum->procs[0].us * 2 >= gap_us) return 2;
    return sum->all_us * 2 >= gap_us ? 1 : 0;
}

// A client's GPU blobs against its GPU address range, in percent (0 when the range is not known).
static inline uint32_t oneios_room_percent(uint64_t used, uint64_t size)
{
    return size ? (uint32_t)(used >= size ? 100 : used * 100 / size) : 0;
}

// What a second did to a client's room: 1 it passed 90 % of the range (was below, is at or above), 2 it let go of an
// eighth of the range or more, 0 neither.
static inline int oneios_room_event(uint64_t was, uint64_t is, uint64_t size)
{
    if (!size) return 0;
    if (was * 10 < size * 9 && is * 10 >= size * 9) return 1;
    if (was > is && (was - is) * 8 >= size) return 2;
    return 0;
}

// ---- take20 (2026-11-27): what take19's lines could not place
// take19's owner felt stutters the hitch lines (150 ms and more) did not have, and the 10 s lines count them without a
// time: none of take19's one-skipped refreshes (5,703 in its moving rounds) can be put next to the second the owner named.
// And the idle-then-touch question (a scroll after 10 s and more at rest) needs the first frames after a rest, which the
// 10 s rounds dilute. Two more lines (OneiOSAdreno.cpp prints them; the arithmetic is here, for the host tests):
//   oneios-frames: a 10 s round's gaps of 25 ms up to the hitch lines' least between two swaps that came right after
//                  frames (the swap before came within 50 ms), in three classes, and the three longest with their time of
//                  day to the millisecond (for the phone's own logs, the iOS log's pipeline creations, the Data store's
//                  cache files, whose times are the guest's clock too).
//   oneios-wake:   the first frames after the screen rested 3 s or more: the rest, the next swaps' gaps and the render
//                  server's host legs of its next fenced frames (the GPU's wake and clock floor, the vCPUs' wake).
// And the hitch line says how many of its blobs took the device 8 ms or more ("slow blobs", take19's first Control Center open: 175
// blobs in a 2.7 s freeze, 227 slow ones in its round).
#define ONEIOS_FRAMES_LEAST_US 25000ull    // a stutter under the hitch lines: two refreshes and more missed at 120 Hz
#define ONEIOS_FRAMES_RING 64              // the qualifying gaps kept between two reports (the counts are exact anyway)
#define ONEIOS_FRAMES_KEPT 3               // the longest ones a line names
#define ONEIOS_WAKE_REST_US 3000000ull     // a rest: no swap for 3 s
#define ONEIOS_WAKE_MOST_US 600000000ull   // longer is the screen off, not a rest to wake from (10 minutes)
#define ONEIOS_WAKE_FRAMES 8               // the swaps and the fenced frames a wake line holds

struct oneios_frame_gap { uint64_t gap_us, at_us; };

// Whether a gap between two swaps is one oneios-frames counts: 25 ms or more, less than the hitch lines' least (150 ms
// when the hitch lines are off), right after frames.
static inline int oneios_frames_is(uint64_t gap_us, uint64_t before_us, uint64_t hitch_least_us)
{
    const uint64_t below = hitch_least_us ? hitch_least_us : 150000ull;
    return gap_us >= ONEIOS_FRAMES_LEAST_US && gap_us < below && before_us && before_us < ONEIOS_HITCH_FRAMES_US;
}

// Its class: 0 under 50 ms, 1 under 100 ms, 2 longer.
static inline unsigned oneios_frames_class(uint64_t gap_us)
{
    return gap_us < 50000ull ? 0 : gap_us < 100000ull ? 1 : 2;
}

// The longest ONEIOS_FRAMES_KEPT gaps of the ring entries numbered [from, to) (numbers only grow; the ring keeps the last
// ONEIOS_FRAMES_RING, so an older `from` starts at the oldest kept), longest first, into out (an unused one: gap 0).
// Returns how many entries were looked at.
static inline uint32_t oneios_frames_longest(const struct oneios_frame_gap *ring, uint64_t from, uint64_t to,
                                             struct oneios_frame_gap out[ONEIOS_FRAMES_KEPT])
{
    memset(out, 0, sizeof(struct oneios_frame_gap) * ONEIOS_FRAMES_KEPT);
    if (to > ONEIOS_FRAMES_RING && from < to - ONEIOS_FRAMES_RING) from = to - ONEIOS_FRAMES_RING;
    uint32_t looked = 0;
    for (uint64_t n = from; n < to; n++, looked++) {
        const struct oneios_frame_gap g = ring[n % ONEIOS_FRAMES_RING];
        for (unsigned i = 0; i < ONEIOS_FRAMES_KEPT; i++) {
            if (g.gap_us <= out[i].gap_us) continue;
            for (unsigned j = ONEIOS_FRAMES_KEPT - 1; j > i; j--) out[j] = out[j - 1];
            out[i] = g;
            break;
        }
    }
    return looked;
}

// A wake: the rest that ended at at_us, then up to ONEIOS_WAKE_FRAMES swap gaps and the render server's host legs of up to
// ONEIOS_WAKE_FRAMES fenced frames reaped after it. The swap path begins it and adds the gaps (one writer: the display's
// frame lock); the fence path adds the legs (several threads: the slot counter, which can pass ONEIOS_WAKE_FRAMES).
// Complete when both are full, or a second after its start (an animation of fewer frames).
struct oneios_wake {
    uint64_t at_us, rest_us;
    uint32_t gaps, legs;
    uint32_t gap_us[ONEIOS_WAKE_FRAMES], leg_us[ONEIOS_WAKE_FRAMES];
};

// Whether a gap ends a rest: 3 s to 10 minutes.
static inline int oneios_wake_is(uint64_t gap_us)
{
    return gap_us >= ONEIOS_WAKE_REST_US && gap_us <= ONEIOS_WAKE_MOST_US;
}

static inline int oneios_wake_complete(const struct oneios_wake *w, uint64_t now_us)
{
    if (!w->at_us) return 0;
    return (w->gaps >= ONEIOS_WAKE_FRAMES && w->legs >= ONEIOS_WAKE_FRAMES) || now_us >= w->at_us + 1000000ull;
}

// "9.1/8.3/25.0" for n microsecond values, as milliseconds with one decimal ("-" for none). Returns the characters
// written (as snprintf: the text is cut at size, never past it).
static inline int oneios_wake_list(char *out, size_t size, const uint32_t *us, uint32_t n)
{
    size_t at = 0;
    if (!size) return 0;
    out[0] = 0;
    if (n > ONEIOS_WAKE_FRAMES) n = ONEIOS_WAKE_FRAMES;
    if (!n) return snprintf(out, size, "-");
    for (uint32_t i = 0; i < n; i++) {
        const int k = snprintf(out + at, size - at, "%s%u.%u", i ? "/" : "", us[i] / 1000, us[i] % 1000 / 100);
        if (k < 0 || (size_t)k >= size - at) return (int)(size - 1);
        at += (size_t)k;
    }
    return (int)at;
}

#endif
