// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The hosted kernel's speaker and microphone: oneios_audio_start, called once by oneios_drivers_start
// (drivers/bridge/OneiOSBridge.c; a weak default there for a kernel without this archive). It starts
// UniXNU-droidvm's virtio-snd transport (DroidVMVirtioSnd.c: xnuhost's /arm-io/virtio-snd, runtime/bridges/devices/
// virtio_snd.c), which publishes the two IOAudio2Devices of DroidVMAudio.cpp, says what attached, and starts a
// report thread: every 30 s while the counters move, one line of each side's counters (what an underrun, a late
// engine period or a refused selector looks like from inside the guest), and while the speaker plays one line of its
// engine's timing (`speaker engine: ...`: how late it took its periods, how far ahead the HAL wrote: iOS's I/O buffer
// and safety offset as used). oneios-audio-report=0 leaves it out.
//
// oneios-audio-selftest=SECONDS (1..10): 3 s after the start, a 1 kHz sine at -30 dBFS (quiet at any media volume)
// straight into the transport, before iOS's audio server runs: the guest kernel -> virtio-snd -> oneios-audio ->
// the phone's speaker path without iOS's audio stack. UniXNU-droidvm's droidvm-snd-selftest plays a full-scale
// sawtooth: not to be used with a speaker.
//
// The microphone (take17, the native VM): `ONEIOS AUDIO: mic: 10 s: ...` whenever the Mic device is touched, started or
// runs (what iOS asked of it, what its engine took, what the device gave and whether that was sound), and
// oneios-audio-mic-selftest=SECONDS (1..10): two capture runs straight off the transport at boot, the phone's
// microphone -> crosvm -> virtio-snd -> this kernel without iOS's audio stack (`ONEIOS AUDIO: mic self-test, run N`).
#include <stdbool.h>
#include <stdint.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern void IOSleep(unsigned milliseconds);
extern int printf(const char *, ...);
extern void droidvm_snd_start(void);
extern uint32_t droidvm_snd_play(const int16_t *frames, uint32_t count);
extern uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count);
extern void droidvm_snd_running(bool output, bool input);
extern uint64_t mach_absolute_time(void);
extern void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
extern void clock_delay_until(uint64_t deadline);
extern char droidvm_snd_status[64];
extern volatile uint32_t droidvm_snd_found, droidvm_snd_stage, droidvm_snd_streams;
extern volatile uint64_t droidvm_snd_tx_periods, droidvm_snd_tx_drops, droidvm_snd_rx_periods, droidvm_snd_rx_empty, droidvm_snd_errors;
extern volatile uint32_t droidvm_audio_stage, droidvm_audio_out_running, droidvm_audio_in_running, droidvm_audio_clients;
extern volatile uint64_t droidvm_audio_periods, droidvm_audio_late, droidvm_audio_played, droidvm_audio_captured;
extern volatile uint64_t droidvm_audio_last_selector, droidvm_audio_selectors, droidvm_audio_bad_selectors, droidvm_audio_maps;
extern volatile int droidvm_audio_family_status, droidvm_audio_hog_owner;
// The speaker engine's timing (DroidVMAudio.cpp as staged, fix-audio-latency).
extern uint32_t oneios_speaker_ring;
extern volatile uint64_t oneios_speaker_periods, oneios_speaker_late_sum, oneios_speaker_late_max, oneios_speaker_leads,
    oneios_speaker_lead_sum, oneios_speaker_lead_min, oneios_speaker_lead_max, oneios_speaker_lead_last, oneios_speaker_tight;

// Since the last report, the speaker's engine: how late it took its periods, and how far ahead of each the HAL had
// written (its write end: iOS's I/O buffer and the safety offset as used, so the guest's part of the latency from
// iOS's mix); a lead within a period of the ring's end is "tight" (the ring too small for the I/O buffer iOS took).
static void oneios_audio_speaker_report(void) {
    static uint64_t periods, late_sum, leads, lead_sum, tight;
    const uint64_t p = oneios_speaker_periods - periods, l = oneios_speaker_leads - leads;
    if (!p) return;
    const uint64_t late = (oneios_speaker_late_sum - late_sum) / p, lead = l ? (oneios_speaker_lead_sum - lead_sum) / l : 0;
    if (l)
        printf("ONEIOS AUDIO: speaker engine: %llu periods, taken late by %llu us on average (max %llu); the HAL wrote %llu..%llu frames "
               "ahead (%llu on average, %llu ms: iOS's I/O buffer and safety offset), %llu periods tight (ring %u frames)\n",
               p, late, oneios_speaker_late_max, oneios_speaker_lead_min, oneios_speaker_lead_max, lead, lead / 48,
               oneios_speaker_tight - tight, oneios_speaker_ring);
    else
        printf("ONEIOS AUDIO: speaker engine: %llu periods, taken late by %llu us on average (max %llu); the HAL's write end not ahead "
               "(last %llu; ring %u frames)\n", p, late, oneios_speaker_late_max, oneios_speaker_lead_last, oneios_speaker_ring);
    periods = oneios_speaker_periods; late_sum = oneios_speaker_late_sum; leads = oneios_speaker_leads;
    lead_sum = oneios_speaker_lead_sum; tight = oneios_speaker_tight;
    oneios_speaker_late_max = 0; oneios_speaker_lead_max = 0; oneios_speaker_lead_min = ~0ULL;
}

// What else the HAL calls (DroidVMAudio.cpp as staged): opened clients, setProperties, notification ports, refused mappings.
extern volatile uint64_t oneios_audio_opens, oneios_audio_properties, oneios_audio_ports, oneios_audio_bad_maps;

// Every 10 s (about oneios-cpu's window) while the HAL's calls, mappings, engine or clients move: the window's own counts. A
// window without this line had no selector, setProperties, notification port, mapping (refused ones included) or client
// opened; reading the devices' registry properties is not a call into the driver and is not counted.
static void oneios_audio_window(void) {
    static uint64_t periods, selectors, refused, maps, errors, opens, properties, ports, bad_maps;
    static uint32_t clients;
    const uint64_t p = droidvm_audio_periods, s = droidvm_audio_selectors, r = droidvm_audio_bad_selectors, m = droidvm_audio_maps,
        e = droidvm_snd_errors, o = oneios_audio_opens, w = oneios_audio_properties, n = oneios_audio_ports, b = oneios_audio_bad_maps;
    const uint32_t c = droidvm_audio_clients;
    if (p == periods && s == selectors && r == refused && m == maps && e == errors && c == clients && o == opens &&
        w == properties && n == ports && b == bad_maps) return;
    printf("ONEIOS AUDIO: 10 s: +%llu periods, +%llu selectors (+%llu refused, last %llu), +%llu maps (+%llu refused), "
           "+%llu properties, +%llu ports, +%llu errors, %u clients (+%llu opened), speaker %s\n",
           p - periods, s - selectors, r - refused, droidvm_audio_last_selector, m - maps, b - bad_maps, w - properties,
           n - ports, e - errors, c, o - opens,
           droidvm_audio_out_running ? "running" : "idle");
    periods = p; selectors = s; refused = r; maps = m; errors = e; clients = c;
    opens = o; properties = w; ports = n; bad_maps = b;
}

// The microphone (take17): its stream on the device (DroidVMVirtioSnd.c as staged for the native kernel: 0 none or
// oneios-audio-mic=0, 1 being set up, 2 ready, 3 refused or not answered; the hosted kernel's transport starts its stream
// itself and leaves 0), the Mic device's StartIOs and StopIOs and its engine's own periods (DroidVMAudio.cpp as staged),
// and what the device gave: periods, of them with a sample over 32 (sound, not silence), the largest sample, periods
// handed back unheard (the engine fell more than four behind), and the engine's periods the device had nothing for.
extern volatile uint32_t oneios_snd_mic_state, oneios_snd_rx_peak;
extern volatile uint64_t oneios_snd_mic_ms, oneios_snd_mic_starts, oneios_snd_mic_refused, oneios_snd_mic_unready, oneios_snd_rx_loud,
    oneios_snd_rx_skipped, oneios_snd_slow_controls;
extern volatile uint64_t oneios_mic_ios[2], oneios_mic_periods, oneios_audio_refused_formats;
// What touched the Mic device at all (DroidVMAudio.cpp as staged): user clients opened [0] and closed [1], selectors [2]
// with a bit for each selector seen in [5], mappings [3], property sets [4].
extern volatile uint64_t oneios_mic_touch[6];
extern int snprintf(char *, __SIZE_TYPE__, const char *, ...);

static const char *oneios_audio_mic_stream(void) {
    switch (oneios_snd_mic_state) {
    case 1: return "being set up";
    case 2: return "ready";
    case 3: return "refused or not answered";
    default: return "none (no stream, or oneios-audio-mic=0)";
    }
}

// Every 10 s in which the Mic device was touched (opened, closed, asked anything, mapped), started, stopped or ran, or
// its stream's state changed: two lines (the console cuts a line past 255 bytes), what iOS asked of the device and what
// the device gave. Voice Memos recording is `engine +2000 periods` with `+2000 periods` from the device and `with sound`
// near that. iOS not starting the device (take16) is a first line without StartIO whose `touched:` part says how far the
// HAL came: no client opened (the HAL never took the device), a client and selectors without 0 or 10 (it took it, set
// it up and did not start it: the reason is in iOS's own log), or nothing after the boot's opening at all (no recording
// was routed to it). A stream that gives nothing is `+0 periods` from the device, a silent one `with sound +0`.
static void oneios_audio_mic_window(void) {
    static uint64_t periods, rx, empty, skipped, loud, ios[2], unready, touch[5];
    static uint32_t state = ~0u;
    const uint64_t p = oneios_mic_periods, r = droidvm_snd_rx_periods, e = droidvm_snd_rx_empty, k = oneios_snd_rx_skipped,
        l = oneios_snd_rx_loud, a = oneios_mic_ios[0], o = oneios_mic_ios[1], u = oneios_snd_mic_unready;
    const uint64_t t[5] = {oneios_mic_touch[0], oneios_mic_touch[1], oneios_mic_touch[2], oneios_mic_touch[3], oneios_mic_touch[4]};
    const uint32_t s = oneios_snd_mic_state;
    bool touched = false;
    for (unsigned i = 0; i < 5; ++i) touched = touched || t[i] != touch[i];
    if (p == periods && a == ios[0] && o == ios[1] && s == state && u == unready && !touched) return;
    const uint32_t peak = oneios_snd_rx_peak;
    oneios_snd_rx_peak = 0;
    char seen[40] = " none";                      // the selectors asked of the Mic since the boot, by number
    const uint64_t mask = oneios_mic_touch[5];
    for (unsigned i = 0, at = 0; i < 64 && at + 4 < sizeof seen; ++i)
        if (mask >> i & 1) at += (unsigned)snprintf(seen + at, sizeof seen - at, " %u", i);
    char line[250];                               // each line whole: made here, under the console's 255 bytes
    snprintf(line, sizeof line, "ONEIOS AUDIO: mic: 10 s: touched: %llu clients opened (+%llu, %llu closed), +%llu selectors (seen since "
             "the boot:%s), +%llu maps, +%llu property sets; StartIO +%llu StopIO +%llu, engine +%llu periods (%s)",
             t[0], t[0] - touch[0], t[1], t[2] - touch[2], seen, t[3] - touch[3], t[4] - touch[4], a - ios[0], o - ios[1], p - periods,
             droidvm_audio_in_running ? "running" : "idle");
    printf("%s\n", line);
    snprintf(line, sizeof line, "ONEIOS AUDIO: mic: 10 s: the device gave +%llu periods, with sound +%llu, largest sample %u of 32767, "
             "nothing for +%llu, unheard +%llu; its stream: %s (set up in %llu ms; STARTs %llu, refused %llu, asked before ready %llu)",
             r - rx, l - loud, peak, e - empty, k - skipped, oneios_audio_mic_stream(), oneios_snd_mic_ms, oneios_snd_mic_starts,
             oneios_snd_mic_refused, u);
    printf("%s\n", line);
    periods = p; rx = r; empty = e; skipped = k; loud = l; ios[0] = a; ios[1] = o; state = s; unready = u;
    for (unsigned i = 0; i < 5; ++i) touch[i] = t[i];
}

static void oneios_audio_report(void *arg, wait_result_t result) {
    (void)arg; (void)result;
    uint64_t last = 0;
    uint32_t last_clients = 0;
    for (;;) {
        for (unsigned tick = 0; tick < 3; ++tick) {
            IOSleep(10000);
            oneios_audio_window();
            oneios_audio_mic_window();
        }
        oneios_audio_speaker_report();
        const uint64_t moved = droidvm_audio_periods + droidvm_audio_selectors + droidvm_snd_errors;
        if (moved == last && droidvm_audio_clients == last_clients) continue;
        last = moved;
        last_clients = droidvm_audio_clients;
        printf("ONEIOS AUDIO: speaker %s, mic %s, %u clients (hog %d), %llu selectors (%llu refused, last %llu), %llu maps; "
               "engine %llu periods, %llu late, %llu frames played, %llu captured; virtio-snd %llu tx periods, %llu dropped "
               "(host behind), %llu rx periods, %llu rx empty, %llu errors\n",
               droidvm_audio_out_running ? "running" : "idle", droidvm_audio_in_running ? "running" : "idle", droidvm_audio_clients,
               droidvm_audio_hog_owner, droidvm_audio_selectors, droidvm_audio_bad_selectors, droidvm_audio_last_selector,
               droidvm_audio_maps, droidvm_audio_periods, droidvm_audio_late, droidvm_audio_played, droidvm_audio_captured,
               droidvm_snd_tx_periods, droidvm_snd_tx_drops, droidvm_snd_rx_periods, droidvm_snd_rx_empty, droidvm_snd_errors);
    }
}

// The tone below is being played (set by oneios_audio_start before its thread starts): the microphone's self-test waits.
static volatile uint32_t oneios_audio_tone_running;

// One period of 1 kHz at 48 kHz, amplitude 1000 (-30 dBFS).
static const int16_t oneios_sine[48] = {
    0, 131, 259, 383, 500, 609, 707, 793, 866, 924, 966, 991, 1000, 991, 966, 924, 866, 793, 707, 609, 500, 383, 259, 131,
    0, -131, -259, -383, -500, -609, -707, -793, -866, -924, -966, -991, -1000, -991, -966, -924, -866, -793, -707, -609,
    -500, -383, -259, -131};

static void oneios_audio_selftest(void *arg, wait_result_t result) {
    (void)result;
    const uint32_t seconds = (uint32_t)(uintptr_t)arg, periods = seconds * 200;
    static int16_t period[240 * 2];
    IOSleep(3000);
    droidvm_snd_running(true, false);
    uint32_t frame = 0, sent = 0, refused = 0;
    uint64_t step, deadline = mach_absolute_time();
    nanoseconds_to_absolutetime(5000000, &step);            // 240 frames at 48 kHz
    for (uint32_t p = 0; p < periods; ++p) {
        for (uint32_t i = 0; i < 240; ++i, ++frame) period[2 * i] = period[2 * i + 1] = oneios_sine[frame % 48];
        if (droidvm_snd_play(period, 240) == 240) sent++;
        else refused++;
        deadline += step;           // on the guest's clock, as DroidVMAudio's engine paces its periods
        clock_delay_until(deadline);
    }
    IOSleep(100);
    droidvm_snd_running(false, false);
    oneios_audio_tone_running = 0;
    printf("ONEIOS AUDIO: self-test: %u of %u periods of 1 kHz at -30 dBFS sent (%u refused: no free slot)\n", sent, periods, refused);
}

// oneios-audio-mic-selftest=SECONDS (1..10; take17): once the microphone's stream is ready (the native kernel sets it up
// in a thread of its own: 10 s on the phone with One UI stopped), and after the tone above when both are asked, the
// transport's input is started, a period is taken every 5 ms for that long, the input is stopped, then started for one
// more second and stopped (a stopped stream's chains: DroidVMVirtioSnd.c as staged): the phone's microphone -> crosvm ->
// virtio-snd -> this kernel, without iOS's audio stack. A line per run: the periods the device gave, how many of them
// had a sample over 32, the largest sample (0 for every period is digital silence: a host that mutes its capture).
// It takes the device's periods as the Mic's engine does: for a boot's first seconds, before anything records.
static void oneios_audio_mic_selftest(void *arg, wait_result_t result) {
    (void)result;
    const uint32_t seconds = (uint32_t)(uintptr_t)arg;
    static int16_t period[240];
    for (unsigned waited = 0; (oneios_snd_mic_state == 1 || oneios_audio_tone_running) && waited < 900; ++waited) IOSleep(100);
    for (unsigned run = 0; run < 2; ++run) {
        const uint32_t periods = (run ? 1 : seconds) * 200;
        const uint64_t rx = droidvm_snd_rx_periods, loud = oneios_snd_rx_loud, empty = droidvm_snd_rx_empty, skipped = oneios_snd_rx_skipped;
        uint32_t got = 0, peak = 0;
        const uint32_t speaker = droidvm_audio_out_running;
        const uint64_t selectors = droidvm_audio_selectors;
        droidvm_snd_running(droidvm_audio_out_running != 0, true);
        uint64_t step, deadline = mach_absolute_time();
        nanoseconds_to_absolutetime(5000000, &step);            // 240 frames at 48 kHz
        for (uint32_t p = 0; p < periods; ++p) {
            if (droidvm_snd_capture(period, 240) == 240) {
                got++;
                for (unsigned i = 0; i < 240; ++i) {
                    const uint32_t size = period[i] < 0 ? (uint32_t)(-(int32_t)period[i]) : (uint32_t)period[i];
                    if (size > peak) peak = size;
                }
            }
            deadline += step;
            clock_delay_until(deadline);
        }
        droidvm_snd_running(droidvm_audio_out_running != 0, droidvm_audio_in_running != 0);
        // An engine that started or stopped in the run set the streams itself (the input off): the run is then no measure.
        const uint64_t asked = droidvm_audio_selectors - selectors;
        printf("ONEIOS AUDIO: mic self-test, run %u: %u of %u periods from the device (%llu taken off its queue, %llu with sound, %llu "
               "unheard, nothing %llu times), largest sample %u of 32767; its stream: %s\n", run + 1, got, periods,
               droidvm_snd_rx_periods - rx, oneios_snd_rx_loud - loud, oneios_snd_rx_skipped - skipped, droidvm_snd_rx_empty - empty, peak,
               oneios_audio_mic_stream());
        if (asked || speaker != droidvm_audio_out_running || droidvm_audio_in_running)
            printf("ONEIOS AUDIO: mic self-test, run %u: iOS's audio server asked %llu selectors of the devices in it (an engine's start "
                   "or stop sets the streams: the run is then not a measure)\n", run + 1, asked);
    }
}

void oneios_audio_start(void) {
    droidvm_snd_start();
    const bool up = droidvm_snd_stage == 3;
    printf("ONEIOS AUDIO: %s: virtio-snd %s (found %u, stage %u, %u streams); IOAudio2 devices: %s (stage %u, family %d); "
           "the microphone's stream: %s\n",
           up && droidvm_audio_stage == 4 ? "on" : "off", droidvm_snd_status[0] ? droidvm_snd_status : "not found",
           droidvm_snd_found, droidvm_snd_stage, droidvm_snd_streams,
           droidvm_audio_stage == 4 ? "Speaker and Mic published" : droidvm_audio_stage == 3 ? "none published" :
           droidvm_audio_stage ? "IOAudio2Family's classes not linked" : "not attached (droidvm-audio=0?)",
           droidvm_audio_stage, droidvm_audio_family_status, oneios_audio_mic_stream());
    uint32_t report = 1;
    PE_parse_boot_argn("oneios-audio-report", &report, sizeof(report));
    thread_t thread;
    if (report && kernel_thread_start(oneios_audio_report, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    uint32_t selftest = 0;
    if (up && PE_parse_boot_argn("oneios-audio-selftest", &selftest, sizeof(selftest)) && selftest) {
        if (selftest > 10) selftest = 10;
        oneios_audio_tone_running = 1;
        if (kernel_thread_start(oneios_audio_selftest, (void *)(uintptr_t)selftest, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        else oneios_audio_tone_running = 0;
    }
    uint32_t mic = 0;
    if (up && PE_parse_boot_argn("oneios-audio-mic-selftest", &mic, sizeof(mic)) && mic) {
        if (mic > 10) mic = 10;
        if (kernel_thread_start(oneios_audio_mic_selftest, (void *)(uintptr_t)mic, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
}
