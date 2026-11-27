// SPDX-License-Identifier: MIT
//
// OneiOS: the boot arguments of the Metal stack's OneiOS patches for the stack 555b13d (userland/metal/patches-555b13d,
// docs/gpu.md section 7.2). Appended to the Metal plug-in's NativeAdrenoMetalPlugin.mm by userland/metal/build.sh (stage), as
// DisplayWide.inc is: the plug-in is loaded by every process that has a Metal device, and this constructor runs as it loads,
// before the process opens its device. Each switch sets an environment variable that its patch reads once, at its first use;
// with the patch left out (ONEIOS_METAL_SKIP) the variable is set and nothing reads it. Switches.inc is the stack 624bab3's.
//
//   oneios-idle-trim=0         ONEIOS_IDLE_TRIM=0: no 2 s idle trim of the runtime's kept memory (the base's timer; NativeMetal 0002)
//   oneios-surface-use=MODE    ONEIOS_SURFACE_USE=MODE: which surface textures of the render server count in use only while
//                              a command buffer reads them (NativeMetal 0001): video (default), all, life (the base's rule)
//   oneios-sched-pace=0        ONEIOS_SCHED_PACE=0: command buffers scheduled at submission (the base's pacing off; NativeMetal 0002)
//   oneios-read-wait=0         ONEIOS_READ_WAIT=0: the render server does not wait for other processes' writers of the
//                              surfaces it reads (the base's awaitWriters off; NativeMetal 0002)
//   oneios-region-identity=0   ONEIOS_REGION_IDENTITY=0: no mach_vm_region identity of no-copy pages where the recursion is
//                              refused (NativeMetal 0004)
//   oneios-bo-cache-idle=0     no idle trim of Turnip's freed-BO cache (mesa 0001; called from the idle timer, NativeMetal 0002)
//   oneios-keep-room=MIB       METAL2VULKAN_KEEP_ROOM_MIB=MIB: the most free room metal2vulkan keeps in one piece in each of its
//                              pools (the board's metal2vulkan 0027, carried to 555b13d, where it also lowers the threshold of
//                              a block made ahead); 8 without it (take18's default, upstream OneiOS e84a46e's KeepRoom.inc),
//                              oneios-keep-room=0 the runtime's own. Not a patch: this default is the whole change. The A/B
//                              file /private/var/ngpu/debug/keep-room-mib, read when the device opens, and a process's own
//                              setting still win
//   (oneios-dl-full=0 is read by NativeMetal 0005's own constructor: every display link keeps the rate it asks)
//   oneios-device-answers=0    ONEIOS_DEVICE_ANSWERS=0: the five private device queries NativeMetal 0006 answers unanswered again
//   oneios-drawable-release=1  ONEIOS_DRAWABLE_RELEASE=1: backboardd counts an app's CAMetalLayer drawable in use only while a
//                              command buffer reads it (NativeMetal 0007, which needs 0001); OFF without it
//   oneios-client-cache=temp   ONEIOS_CLIENT_CACHE=temp: take20's own-folder cache is not tried (NativeMetal 0008); the base's
//                              same-user shared directory stays
//   oneios-prewarm=1           ONEIOS_PREWARM=1: take20's kept render pipeline states and its boot pass (NativeMetal 0009); OFF
//                              without it (the base's own warm pass, metal-flow bit 4, is on either way). oneios-prewarm-max=N
//   oneios-pipeline-wait-ms=N  backboardd alone, ONEIOS_PIPELINE_WAIT_MS=N (1..4500): a pipeline state's creation and a command
//                              buffer wait N ms at most for a driver pipeline being made, on the runtime's pool; what the
//                              new pipeline draws is left out until it is made (NativeMetal 0010); OFF without it: every
//                              creation waits for its compile, as Metal does. An A/B for the first-use compile stutters
//   oneios-queue-priority=WORD backboardd alone, METAL2VULKAN_QUEUE_PRIORITY=WORD (low, medium, high, realtime): its queues'
//                              global priority fixed as the device opens, take18's way (it set high); without it the base's
//                              way alone, Core Animation's setGPUPriority: forwarded to Turnip (tu_darwin_set_priority)
//   oneios-msaa=1              NATIVE_METAL_SINGLE_SAMPLE=0 (the base's variable): a process's textures, passes and pipelines keep
//                              the sample counts it asks; without it the base makes them all with one sample (no
//                              multisampling in any process: less GPU work, no antialiased edges), which take18's stack did not
// Read by the plug-in itself, in NativeMetal 0002 (said in the line below when set, nothing is set here):
//   oneios-ycbcr-planes=0      the base's gpu-ycbcr-planes=0: two-plane YCbCr surface textures keep their converted image, in
//                              every process
//   oneios-compile-here=0      driver pipelines made on a thread the runtime starts, not on the asking thread (ON by default
//                              in this stack: the base sets METAL2VULKAN_COMPILE_ON_ASKING_THREAD=1 in every process)
// Gone with their patches, which the base replaces with code that has no such knob: oneios-pool-cut=0 (the runtime's own
// spares.rs keeps only recent uses' command buffers) and oneios-idle-spares=0 (the bridge's room thread lets parked objects and
// kept empty blocks go by time); oneios-compile-here=1 is the default now.
// The base's own boot arguments work as upstream documents them (docs/gpu.md 7.2 has the table): metal-flow=N, gpu-purge=0,
// gpu-ycbcr-planes=0, diag=1, gpu-trace=1, log-gpu=1, tu-debug=..., tu-cb=1, metal-tile=1, wallpaper-freeze=1, ca-triple-buffer=1.
//
// What it logs (iOS's log, each process that loads the plug-in): "OneiOS metal switches: ..." with the switches the boot
// arguments set, once; nothing when none is set.
#include <dlfcn.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace oneios_metal_switches {
// Whether the boot arguments hold `word` as a whole word.
static bool bootWord(const char *args, const char *word) {
    const size_t length = strlen(word);
    for (const char *at = strstr(args, word); at; at = strstr(at + 1, word)) {
        const char end = at[length];
        if ((at == args || at[-1] == ' ') && (end == 0 || end == ' ')) return true;
    }
    return false;
}
// The value of `key` (ending in '='), a whole word of the boot arguments, into `out`; false without it.
static bool bootValue(const char *args, const char *key, char *out, size_t size) {
    const size_t length = strlen(key);
    for (const char *at = strstr(args, key); at; at = strstr(at + 1, key)) {
        if (at != args && at[-1] != ' ') continue;
        size_t n = strcspn(at + length, " ");
        if (n >= size) n = size - 1;
        memcpy(out, at + length, n);
        out[n] = 0;
        return true;
    }
    return false;
}
static bool gBoCacheIdle = true;

__attribute__((constructor)) static void apply() {
    char args[1100] = {};
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0)) args[0] = 0;
    char said[256] = {};
    auto note = [&](const char *what) {
        if (said[0]) strlcat(said, ", ", sizeof said);
        strlcat(said, what, sizeof said);
    };
    struct Flag {const char *word, *name, *value;};
    static const Flag flags[] = {
        {"oneios-idle-trim=0", "ONEIOS_IDLE_TRIM", "0"},
        {"oneios-sched-pace=0", "ONEIOS_SCHED_PACE", "0"},
        {"oneios-read-wait=0", "ONEIOS_READ_WAIT", "0"},
        {"oneios-region-identity=0", "ONEIOS_REGION_IDENTITY", "0"},
        {"oneios-device-answers=0", "ONEIOS_DEVICE_ANSWERS", "0"},
        {"oneios-msaa=1", "NATIVE_METAL_SINGLE_SAMPLE", "0"},
        {"oneios-drawable-release=1", "ONEIOS_DRAWABLE_RELEASE", "1"},
        {"oneios-client-cache=temp", "ONEIOS_CLIENT_CACHE", "temp"},
        {"oneios-prewarm=1", "ONEIOS_PREWARM", "1"},
    };
    for (const Flag &flag : flags)
        if (bootWord(args, flag.word)) {
            setenv(flag.name, flag.value, 1);
            note(flag.word);
        }
    if (bootWord(args, "oneios-bo-cache-idle=0")) {
        gBoCacheIdle = false;
        note("oneios-bo-cache-idle=0");
    }
    // The pools' room (upstream OneiOS e84a46e, KeepRoom.inc, from its 2026-11-24 session on the older runtime: backboardd's pooled
    // blocks 440 MiB for 264 MiB placed, the room kept in one piece making a 64 MiB block whenever the free bytes were only in
    // pieces): 8 MiB, kept as take18's default. Not measured on 555b13d, whose pools start at 4 MiB and grow: oneios-keep-room=0.
    // setenv without overwrite: a process's own setting wins, and the plug-in's A/B file, read when the device opens, after.
    {
        char text[16] = "8";
        if (bootValue(args, "oneios-keep-room=", text, sizeof text)) {
            char said_room[40];
            snprintf(said_room, sizeof said_room, "oneios-keep-room=%s", text);
            note(said_room);
        }
        char *end = nullptr;
        const unsigned long mib = strtoul(text, &end, 10);
        if (end != text && !*end && mib > 0 && mib <= 4096) {
            snprintf(text, sizeof text, "%lu", mib);
            setenv("METAL2VULKAN_KEEP_ROOM_MIB", text, 0);
        }
    }
    {   // NativeMetal 0009's bound (take20): a number of kept render pipeline states
        char most[16] = {};
        if (bootValue(args, "oneios-prewarm-max=", most, sizeof most)) {
            char *end = nullptr;
            const unsigned long n = strtoul(most, &end, 10);
            if (end != most && !*end && n > 0 && n < 16384) {
                setenv("ONEIOS_PREWARM_MAX", most, 1);
                char said_most[40];
                snprintf(said_most, sizeof said_most, "oneios-prewarm-max=%lu", n);
                note(said_most);
            }
        }
    }
    char mode[16] = {};
    if (bootValue(args, "oneios-surface-use=", mode, sizeof mode)) {
        if (!strcmp(mode, "video") || !strcmp(mode, "all") || !strcmp(mode, "life")) {
            setenv("ONEIOS_SURFACE_USE", mode, 1);
            char text[48];
            snprintf(text, sizeof text, "oneios-surface-use=%s", mode);
            note(text);
        } else {
            note("oneios-surface-use=? (not video, all or life: the default, video)");
        }
    }
    // The render server alone: the opt-in bounded wait for pipelines being made (NativeMetal 0010), and its queues' global
    // priority fixed as the device opens, as take18's plug-in set it (the board's metal2vulkan 0012 and Turnip's
    // turnip-darwin-queue-priority read METAL2VULKAN_QUEUE_PRIORITY; the base forwards Core Animation's setGPUPriority: instead).
    if (!strcmp(getprogname(), "backboardd")) {
        char wait[16] = {};
        if (bootValue(args, "oneios-pipeline-wait-ms=", wait, sizeof wait)) {
            char *end = nullptr;
            const unsigned long ms = strtoul(wait, &end, 10);
            if (end != wait && !*end && ms >= 1 && ms <= 4500) {
                snprintf(wait, sizeof wait, "%lu", ms);
                setenv("ONEIOS_PIPELINE_WAIT_MS", wait, 0);
                char said_wait[48];
                snprintf(said_wait, sizeof said_wait, "oneios-pipeline-wait-ms=%lu", ms);
                note(said_wait);
            }
        }
        char priority[16] = {};
        if (bootValue(args, "oneios-queue-priority=", priority, sizeof priority) &&
            (!strcmp(priority, "low") || !strcmp(priority, "medium") || !strcmp(priority, "high") || !strcmp(priority, "realtime"))) {
            setenv("METAL2VULKAN_QUEUE_PRIORITY", priority, 0);
            char said_priority[48];
            snprintf(said_priority, sizeof said_priority, "oneios-queue-priority=%s", priority);
            note(said_priority);
        }
    }
    // Read by the plug-in itself as the device opens (NativeMetal 0002); said here so the log has every switch of the boot.
    for (const char *word : {"oneios-ycbcr-planes=0", "oneios-compile-here=0"})
        if (bootWord(args, word)) note(word);
    // Gone in this stack: said, so that an image built for the stack 624bab3 does not look obeyed.
    for (const char *word : {"oneios-pool-cut=0", "oneios-idle-spares=0"})
        if (bootWord(args, word)) note(word[7] == 'p' ? "oneios-pool-cut=0 (no such switch in this stack)" : "oneios-idle-spares=0 (no such switch in this stack)");
    if (said[0]) os_log(OS_LOG_DEFAULT, "OneiOS metal switches: %{public}s", said);
}
}  // namespace oneios_metal_switches

// Called every 2 s by the frontend's idle trim timer (NativeMetal 0002), in a process with a device: once a minute, the freed
// BOs Turnip's cache kept for 30 s or more go back to the host (mesa 0001, tu_darwin_bo_cache_trim_idle; upstream OneiOS
// cb32176's CommandPoolRow.inc called it from its row thread). The cache went back only past its budgets (96 + 16 MiB in the
// render server, 16 + 8 elsewhere) or at a memory pressure rise, so a process that went still kept it full (upstream's
// render server: 78 MiB at a still screen), every BO wired guest memory. A busy process takes its BOs back within seconds.
extern "C" void oneios_idle_tick(void) {
    static unsigned ticks;
    static uint64_t (*trim)(uint64_t);
    static bool looked;   // Turnip's library found (it is loaded by the device's opening, before the first tick)
    if (!oneios_metal_switches::gBoCacheIdle || ++ticks % 30) return;
    if (!looked) {
        char path[1024];
        if (turnip::libraryPath(path, sizeof path))
            if (void *library = dlopen(path, RTLD_NOW | RTLD_NOLOAD)) {
                trim = (uint64_t (*)(uint64_t))dlsym(library, "tu_darwin_bo_cache_trim_idle");   // null without mesa 0001
                looked = true;
            }
    }
    if (trim) (void)trim(30000);
}
