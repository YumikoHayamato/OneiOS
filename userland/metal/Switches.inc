// SPDX-License-Identifier: MIT
//
// OneiOS: the boot arguments of the Metal stack's OneiOS patches (userland/metal/patches, docs/gpu.md section 7.1).
// Appended to the Metal plug-in's NativeAdrenoMetalPlugin.mm by userland/metal/build.sh (stage), as DisplayWide.inc is:
// the plug-in is loaded by every process that has a Metal device, and this constructor runs as it loads, before the
// process opens its device. Each switch sets an environment variable that its patch reads once, at its first use; with
// the patch left out (ONEIOS_METAL_SKIP) the variable is set and nothing reads it.
//
//   oneios-idle-trim=0         ONEIOS_IDLE_TRIM=0: no 2 s idle trim of the runtime's kept memory (NativeMetal 0002 off)
//   oneios-surface-use=MODE    ONEIOS_SURFACE_USE=MODE: which surface textures of the render server count in use only while
//                              a command buffer reads them (NativeMetal 0003): video (default), all, life (as before)
//   oneios-ycbcr-planes=0      backboardd keeps the runtime's converted image of each two-plane YCbCr texture: no
//                              METAL2VULKAN_YCBCR_PLANES_ONLY=1 (metal2vulkan 0001 then changes nothing)
//   oneios-sched-pace=0        ONEIOS_SCHED_PACE=0: command buffers scheduled at submission (NativeMetal 0004's pacing off)
//   oneios-read-wait=0         ONEIOS_READ_WAIT=0: the render server does not wait for other processes' writers of the
//                              surfaces it reads (NativeMetal 0004's awaitWriters off)
//   oneios-region-identity=0   ONEIOS_REGION_IDENTITY=0: no mach_vm_region identity of no-copy pages where the recursion is
//                              refused (NativeMetal 0006: such buffers are imported anew, as before, without the refused calls)
//   oneios-pool-cut=0          METAL2VULKAN_POOL_USE_WINDOW_S=0: spare command pools kept as before (metal2vulkan 0005)
//   oneios-idle-spares=0       METAL2VULKAN_IDLE_SPARES=0: the idle trim leaves parked images and spare blocks (metal2vulkan 0006)
//   oneios-bo-cache-idle=0     no idle trim of Turnip's freed-BO cache (mesa 0003; called from NativeMetal 0002's timer)
//   oneios-compile-here=1      METAL2VULKAN_COMPILE_ON_ASKING_THREAD=1: driver pipelines made on the asking thread where its
//                              stack holds 2 MiB (metal2vulkan 0008, upstream c135133); OFF without it. NativeMetal 0013 then
//                              says the asking thread's QoS class and stack once per process
//   oneios-keep-room=MIB       METAL2VULKAN_KEEP_ROOM_MIB=MIB: the most free room metal2vulkan keeps in one piece in each of its
//                              pools (the board's metal2vulkan 0027); 8 without it (upstream OneiOS e84a46e's KeepRoom.inc),
//                              oneios-keep-room=0 the runtime's own, half the pool's next block (32 MiB once a pool has 64).
//                              Not a patch: this default is the whole change. The A/B file /private/var/ngpu/debug/
//                              keep-room-mib, read when the device opens, and a process's own setting still win
//   (oneios-dl-full=0 is read by NativeMetal 0014's own constructor: every display link keeps the rate it asks)
//
// What it logs (iOS's log, each process that loads the plug-in): "OneiOS metal switches: ..." with the switches the boot
// arguments set, once; nothing when none is set (backboardd says its YCbCr planes either way).
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
        {"oneios-pool-cut=0", "METAL2VULKAN_POOL_USE_WINDOW_S", "0"},
        {"oneios-idle-spares=0", "METAL2VULKAN_IDLE_SPARES", "0"},
        {"oneios-compile-here=1", "METAL2VULKAN_COMPILE_ON_ASKING_THREAD", "1"},
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
    // The pools' room (upstream OneiOS e84a46e, KeepRoom.inc, from its 2026-11-24 session: backboardd's pooled blocks 440 MiB
    // for 264 MiB placed, the room kept in one piece making a 64 MiB block whenever the free bytes were only in pieces): 8 MiB.
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
    // backboardd: a two-plane YCbCr texture whose planes its functions read in place is made over the planes alone, without
    // the runtime's converted image and staging copy (metal2vulkan 0001, Droid-VM/UniXNU d48df9c's 0039; NativeMetal 43f78ae
    // sets it for macOS's WindowServer). A process's own setting wins.
    if (!strcmp(getprogname(), "backboardd")) {
        const bool off = bootWord(args, "oneios-ycbcr-planes=0");
        if (!off) setenv("METAL2VULKAN_YCBCR_PLANES_ONLY", "1", 0);
        os_log(OS_LOG_DEFAULT, "OneiOS metal switches: backboardd's two-plane YCbCr textures %s",
               off ? "keep their converted image (oneios-ycbcr-planes=0)" : "over their planes alone where read in place");
    }
    if (said[0]) os_log(OS_LOG_DEFAULT, "OneiOS metal switches: %{public}s", said);
}
}  // namespace oneios_metal_switches

// Called every 2 s by the frontend's idle trim timer (NativeMetal 0002), in a process with a device: once a minute, the freed
// BOs Turnip's cache kept for 30 s or more go back to the host (mesa 0003, tu_darwin_bo_cache_trim_idle; upstream OneiOS
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
                trim = (uint64_t (*)(uint64_t))dlsym(library, "tu_darwin_bo_cache_trim_idle");   // null without mesa 0003
                looked = true;
            }
    }
    if (trim) (void)trim(30000);
}
