// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The video decoder of the native kernel: oneios_codec_start, called once by oneios_drivers_start
// (drivers/bridge/OneiOSBridge.c; a weak default there for a kernel without this archive). It brings up UniXNU-droidvm's
// virtio-media transport (DroidVMVirtioMedia.c, staged with the PCI transport: crosvm's `--virtio-media kind=decoder`
// card on its ECAM bus, /arm-io/virtio-media-0 "oneios,virtio-media-pci" with media-host-pool, xnuloader-crosvm --media)
// and publishes iOS's paravirtualized VideoToolbox driver (DroidVMVTParavirt.cpp + the staged DroidVMVTHost.cpp), so
// VideoToolbox decodes H.264 and HEVC (VP9 and AV1 when the device offers them) on the phone's MediaCodec instead of the
// guest's CPU. See docs/hardware.md 7g and this folder's README.md.
//
// Off unless oneios-codec=1 (the native kernel's default is 0; xnuloader-crosvm --media adds oneios-codec=1 and
// droidvm-vt-paravirt=1): an Image without --media runs nothing of this archive, as the kernels before it.
// droidvm-media=0 leaves the transport out; droidvm-vt-paravirt=1 publishes AppleVideoToolboxParavirtualizationDriver
// (only when a decoder or encoder device is up: without one VideoToolbox keeps its own codecs).
//
// With a device up, a report thread says what the three files count, every 30 s while a figure moves (nothing while
// nobody uses the codec): the device's start-up check, who opened the service, the messages, the sessions and frames,
// the first failed step, the decoder's frames by codec and the time from a frame's dfrm to its Demt, the transport's
// commands and events, and the lines DroidVMVTHost.cpp keeps of each session (its trace ring). oneios-codec-report=0
// leaves it out.
#include <stdbool.h>
#include <stdint.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <pexpert/pexpert.h>

extern void IOSleep(unsigned milliseconds);
extern int printf(const char *, ...);
extern void proc_name(int pid, char *buf, int size);
extern void droidvm_media_start(void);                 // DroidVMVirtioMedia.c
extern int droidvm_vt_paravirt_attach(void);           // DroidVMVTParavirt.cpp
extern volatile uint32_t droidvm_media_found, droidvm_media_up;
extern volatile uint64_t droidvm_media_commands, droidvm_media_timeouts, droidvm_media_events[3];
extern char droidvm_media_status[][192];
extern volatile uint32_t droidvm_vtpv_stage, droidvm_vtpv_opens, droidvm_vtpv_closes, droidvm_vtpv_messages, droidvm_vtpv_refused;
extern volatile uint32_t droidvm_vtpv_sent, droidvm_vtpv_dropped;
extern volatile uint32_t droidvm_vthost_sessions, droidvm_vthost_frames_in, droidvm_vthost_frames_out, droidvm_vthost_errors;
extern volatile uint32_t droidvm_vthost_error_stage, droidvm_vthost_replies, droidvm_vthost_unknown, droidvm_vthost_last_unknown;
extern volatile int32_t droidvm_vthost_error_value;
extern char droidvm_vthost_trace[16][112];
extern volatile uint32_t droidvm_vthost_trace_count;
// The staged DroidVMVTHost.cpp (staged/codec_decode.rs): the decoder's frames by codec (avc1, hvc1, vp09, av01), handed
// over decoded and dropped, and the time from a frame's dfrm to its Demt (sum, count, the largest since the last line).
extern volatile uint32_t oneios_decode_frames[4], oneios_decode_dropped[4];
extern volatile uint64_t oneios_decode_us_sum;
extern volatile uint32_t oneios_decode_us_count, oneios_decode_us_max;
// The staged DroidVMVTHost.cpp (staged/codec_decode.rs CODEC_CROSVM): sessions crosvm's device ended, sessions resumed after a
// mid-stream announce (and of them set up again at a new size), CAPTURE sets smaller than asked, access units larger than
// their OUTPUT buffer, sessions refused because their buffers would not fit the media_host pool. The staged
// DroidVMVirtioMedia.c (staged/codec_transport.rs): commands answered after their 12 s wait (taken off before the next one),
// and commands not sent because such a command was still the device's.
extern volatile uint32_t oneios_decode_lost, oneios_decode_resumed, oneios_decode_resized, oneios_decode_short, oneios_decode_cut,
    oneios_decode_unfit;
extern volatile uint64_t oneios_media_late_taken, oneios_media_late_refused;
// DroidVMVTParavirt.cpp's record of a message from a process (its struct droidvm_vtpv_record at the pin f9121cd, 136
// bytes; the ring keeps the newest 32, indexed by seq).
struct oneios_codec_record {
    uint32_t seq, pid, length, surface_count;
    uint32_t type, flags;
    uint64_t request;
    uint8_t uuid[16];
    uint32_t surfaces[5];
    uint32_t reserved;
    uint8_t payload[64];
};
_Static_assert(sizeof(struct oneios_codec_record) == 136, "struct droidvm_vtpv_record");
extern struct oneios_codec_record droidvm_vtpv_log[32];

static void oneios_codec_report(void *arg, wait_result_t result) {
    (void)arg; (void)result;
    IOSleep(3000);                                     // the transport's own start-up check of the device is done by now
    for (uint32_t i = 0; i < droidvm_media_found && i < 4; ++i)
        printf("ONEIOS CODEC: device %u: %.191s\n", i, droidvm_media_status[i]);
    uint64_t last = 0;
    uint32_t traced = 0, shown = 0;
    uint32_t decoded_last[8] = {0};
    uint64_t us_last = 0;
    uint32_t count_last = 0;
    for (;;) {
        IOSleep(30000);
        const uint64_t moved = (uint64_t)droidvm_vtpv_opens + droidvm_vtpv_closes + droidvm_vtpv_messages + droidvm_vthost_errors +
                               droidvm_vthost_frames_out + droidvm_media_timeouts;
        if (moved == last) continue;
        last = moved;
        // The frames each codec decoded and dropped since the last line, and the time from a frame's dfrm to its Demt (the
        // codec's reordering is in it), once a frame was decoded or dropped since.
        uint32_t decoded[8], any = 0;
        for (int i = 0; i < 4; ++i) {
            decoded[i] = oneios_decode_frames[i] - decoded_last[i];
            decoded[4 + i] = oneios_decode_dropped[i] - decoded_last[4 + i];
            decoded_last[i] = oneios_decode_frames[i];
            decoded_last[4 + i] = oneios_decode_dropped[i];
            any |= decoded[i] | decoded[4 + i];
        }
        if (any) {
            const uint64_t us = oneios_decode_us_sum - us_last;
            const uint32_t timed = oneios_decode_us_count - count_last;
            const uint32_t max = __atomic_exchange_n(&oneios_decode_us_max, 0u, __ATOMIC_RELAXED);
            us_last += us;
            count_last += timed;
            printf("ONEIOS CODEC: decoded: avc1 %u, hvc1 %u, vp09 %u, av01 %u; dropped %u, %u, %u, %u; dfrm to Demt avg %llu us, "
                   "max %u us (%u frames)\n", decoded[0], decoded[1], decoded[2], decoded[3], decoded[4], decoded[5], decoded[6],
                   decoded[7], timed ? us / timed : 0, max, timed);
        }
        // The last type the host has no case for, as its four characters ("none" before the first): a new type of a new
        // iOS shows at once on this line.
        const uint32_t unknown = droidvm_vthost_last_unknown;
        char unknown_type[5] = "none";
        if (unknown) {
            for (int i = 0; i < 4; ++i) {
                const char c = (char)(unknown >> (24 - 8 * i));
                unknown_type[i] = c >= 0x20 && c < 0x7f ? c : '?';
            }
        }
        printf("ONEIOS CODEC: counters: service opened %u closed %u, messages %u (refused %u, unknown %u, the last '%s'), "
               "to the guest %u (dropped %u), replies %u; sessions %u, frames in %u out %u, errors %u (last: step %u value %d); "
               "virtio-media commands %llu (timeouts %llu), events %llu errors %llu buffers %llu other\n",
               droidvm_vtpv_opens, droidvm_vtpv_closes, droidvm_vtpv_messages, droidvm_vtpv_refused, droidvm_vthost_unknown, unknown_type,
               droidvm_vtpv_sent, droidvm_vtpv_dropped, droidvm_vthost_replies,
               droidvm_vthost_sessions, droidvm_vthost_frames_in, droidvm_vthost_frames_out,
               droidvm_vthost_errors, droidvm_vthost_error_stage, droidvm_vthost_error_value,
               droidvm_media_commands, droidvm_media_timeouts, droidvm_media_events[0], droidvm_media_events[1], droidvm_media_events[2]);
        // What crosvm's decoder did to the sessions (CODEC_CROSVM) and the commands it answered late, once any of it happened
        // (a line of its own: the counters line is long enough that the console cuts its end).
        if (oneios_decode_lost | oneios_decode_resumed | oneios_decode_short | oneios_decode_cut | oneios_decode_unfit ||
            oneios_media_late_taken | oneios_media_late_refused)
            printf("ONEIOS CODEC: decoder sessions: ended by the device %u, resumed after a new format %u (set up again %u), "
                   "CAPTURE sets cut short %u, access units cut %u, refused for the pool %u; commands answered late %llu, "
                   "not sent %llu\n", oneios_decode_lost, oneios_decode_resumed, oneios_decode_resized, oneios_decode_short,
                   oneios_decode_cut, oneios_decode_unfit, oneios_media_late_taken, oneios_media_late_refused);
        // The messages since the last report, the newest 32 at most: who sent what (decs: the decoder list; +dec, dsta, dfrm,
        // dfin, dinv, -dec: a decoder session; dcap, dcan: its capabilities and a new format).
        const uint32_t messages = droidvm_vtpv_messages;
        if (messages - shown > 32) shown = messages - 32;
        for (; shown != messages; ++shown) {
            const struct oneios_codec_record *r = &droidvm_vtpv_log[shown % 32];
            char name[33] = "?";
            if (r->seq != shown) continue;             // the slot was taken again meanwhile
            proc_name((int)r->pid, name, sizeof name);
            printf("ONEIOS CODEC: message %u: %s[%u] '%c%c%c%c', %u bytes, %u surface(s)\n", r->seq, name, r->pid,
                   (char)(r->type >> 24), (char)(r->type >> 16), (char)(r->type >> 8), (char)r->type, r->length, r->surface_count);
        }
        // The session lines written since the last report (the ring keeps the newest 16).
        const uint32_t count = droidvm_vthost_trace_count;
        if (count - traced > 16) traced = count - 16;
        for (; traced != count; ++traced) printf("ONEIOS CODEC: session: %.111s\n", droidvm_vthost_trace[traced % 16]);
    }
}

void oneios_codec_start(void) {
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    uint32_t enabled = 0;                              // the native kernel: only an Image made with --media has the decoder
#else
    uint32_t enabled = 1;                              // the hosted kernel (upstream's default; no codec archive in this tree)
#endif
    uint32_t report = 1;
    PE_parse_boot_argn("oneios-codec", &enabled, sizeof enabled);
    if (!enabled) { printf("ONEIOS CODEC: off (oneios-codec=0; xnuloader-crosvm --media gives oneios-codec=1)\n"); return; }

    droidvm_media_start();                             // the virtio-media transport: crosvm's decoder on its PCI bus
    const int paravirt = droidvm_vt_paravirt_attach(); // the VideoToolbox paravirtualization classes + service

    printf("ONEIOS CODEC: %u virtio-media device(s), %u up; VideoToolbox paravirtualization %s (stage %u)\n",
           droidvm_media_found, droidvm_media_up,
           paravirt == 0 && droidvm_vtpv_stage == 2 ? "published" : "not published", droidvm_vtpv_stage);
    PE_parse_boot_argn("oneios-codec-report", &report, sizeof report);
    thread_t thread;
    if (report && droidvm_media_up && kernel_thread_start(oneios_codec_report, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
