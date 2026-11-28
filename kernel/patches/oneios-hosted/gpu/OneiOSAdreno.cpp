// SPDX-License-Identifier: MIT
// OneiOS, hosted kernel: NativeAdreno's Metal face on the virtio GPU, bound to the iPhone18,2 kernel collection.
//
// What is linked (build-oneios-gpu.py): NativeAdreno's user client, IOKit surface, execbuf, memory and PM4 encoder
// with Droid-VM's edits, Droid-VM's package/DroidVMGpu.cpp (the hardware classes never attached, the publish, the
// accelerator's registration) and the staged AGXAccelerator (NativeAdrenoAccelerator.cpp), a subclass of the
// collection's IOAcceleratorES (com.apple.iokit.IOGPUFamily). Apple's AGXG18P is not in the collection, so the
// class name AGXAccelerator and the user client's name AGXDeviceUserClient are free.
//
// The parent is reached as the software AES reaches IOAESAccelerator (../aes/OneiOSSoftwareAES.cpp): its
// constructor and destructor through generated stubs `bti c; b <name>.original`, which XnuPrelink retargets, and
// its metaclass as an absolute symbol whose value is the link address. Under xnuhost the collection runs slid, so
//   - oneios_adreno_parent_bound: the live IOAcceleratorES must be the bound one (link address + slide, 136 bytes);
//   - oneios_adreno_parent_slide: AGXAccelerator's metaclass, constructed with the link address as its superclass,
//     gets the live one before OSMetaClass::postModLoad reads it.
// DroidVMGpu.cpp calls both from its publishMetal (the build stages those two lines and the start condition).
//
// The start: oneios_adreno_start, from oneios_gpu3d_start (OneiOSGpu.cpp), which the board's late start calls in
// bsd_init. Boot arguments:
//   oneios-gpu-metal=0|1     0: nothing is published. 1: published whatever the host offers. Not given: published
//                            when the host's virtio GPU offers 3D (oneios_gpu3d_state 3). Droid-VM's
//                            droidvm-gpu-metal is read the same way when oneios-gpu-metal is absent.
//   droidvm-gpu-clients=...  the processes besides backboardd that get a Metal device: the grant of the user client
//                            (DroidVMGpu.cpp) and the plug-in's own list (oneios_gpu_client_list below)
//   oneios-adreno-selftest=1 after the publish: IOKit's match of IOAcceleratorES, an in-kernel open of the Metal
//                            plug-in's connection (type 0x4149) and one GETPARAM through its tunnel; one log line
//                            with each result.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSMetaClass.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <machine/machine_routines.h>
#include <mach/task_info.h>
extern "C" uint64_t gPhysBase, gPhysSize;
#include "DroidVMVirtgpuABI.h"

extern vm_offset_t vm_kernel_slide;
extern "C" {
// The generated bindings (build-oneios-gpu.py): the parent metaclass's link address and instance size as numbers.
extern const uint64_t oneios_adreno_parent_meta_link;
extern const uint32_t oneios_adreno_parent_size;
extern char oneios_adreno_accelerator_meta[] __asm__("__ZN14AGXAccelerator10gMetaClassE");
extern volatile uint32_t oneios_gpu3d_state;                   // OneiOSGpu.cpp
extern volatile uint64_t droidvm_metal_publish[3];             // DroidVMGpu.cpp: publish status, accelerator stage, status
extern volatile uint32_t droidvm_gpu_chain;                    // DroidVMGpu.cpp: which class chains were adopted
void sheng_gpu_power_probe(void);                              // DroidVMGpu.cpp: the publish, on the caller's thread (staged)

// 0 not started; 1 not wanted; 2 the publish was started; 0x81 the live IOAcceleratorES is not the bound one.
volatile uint32_t oneios_adreno_stage;
// The self-test: [0] stage (1 waiting, 2 matched, 3 opened, 4 called, 5 closed; 0x80 | n failed at n), [1] the
// open's status, [2] the call's status, [3] its answer (the virtgpu ABI version).
volatile uint64_t oneios_adreno_selftest[4];
bool oneios_adreno_wanted(void);
bool oneios_adreno_parent_bound(void);
void oneios_adreno_parent_slide(void);
void oneios_adreno_start(void);
}

// What NativeAdrenoPublish.cpp calls on sheng and on a DroidVM board, and what stands behind it there: the
// display hand-off (NativeMobileDisplay's presenter: the display archive defines these two when it is linked, and
// its definitions win over these weak ones) and the board's log export. Neither exists here.
extern "C" __attribute__((weak)) IOReturn native_gop_handoff_call(uint32_t, task_t, const void *, size_t, void *, size_t)
{
    return kIOReturnUnsupported;
}
extern "C" __attribute__((weak)) IOMemoryDescriptor *native_gop_handoff_memory(void) {return nullptr;}
extern "C" __attribute__((weak)) int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {return -1;}

// OSMetaClass as laid out in libkern/c++/OSMetaClass.h: the vtable pointer, reserved, superClassLink.
struct MetaClassHead { void *vtable; void *reserved; const OSMetaClass *superClassLink; };

static const OSMetaClass *boundParent()
{
    return reinterpret_cast<const OSMetaClass *>(oneios_adreno_parent_meta_link + vm_kernel_slide);
}

// The processes besides backboardd that get a Metal device: one list for both gates. The kernel's grant
// (DroidVMGpu.cpp grantedProcesses, staged by build-oneios-gpu.py to read it here) decides the open for the names it
// holds; the plug-in makes a device only in a process its NativeAdrenoMetal.clients names, and 0039 writes that file
// beside the bundle from oneios_gpu_plugin_client_list at every boot. Names as the grant and the plug-in take them:
// "a,b", "X*" (a prefix), "*X*" (a part), "@apps" (every process run from an app bundle), "*" (every process).
//   droidvm-gpu-clients=...         the list; empty: backboardd alone. Not given: kDefaultGpuClients ("*").
//   oneios-gpu-plugin-clients=...   the plug-in's list alone, for a test (the processes it names then ask, and the
//                                   grant or the sandbox decides each open). Not given: the list above.
// Every process (the owner's decision, 2026-11-12), as on an iPhone, where every process that asks has a Metal device and
// the iPad board of Droid-VM runs the same since 2026-11-03. Until then the default named WidgetRenderer_Default, *Poster*,
// iconservicesagent and SBRendererService: SpringBoard, the apps, the keyboard and WebKit drew on the CPU. The GPU's
// address room is cut per process for this (drm2kgsl patch 0004): a process the room has no place for gets no device.
static const char kDefaultGpuClients[] = "*";
extern "C" void oneios_gpu_client_list(char *out, size_t size)
{
    if (!out || size < 2) return;
    memset(out, 0, size);
    // The string forms of PE_parse_boot_argn copy up to the length given and terminate after it: one byte is kept.
    if (!PE_parse_boot_argn("droidvm-gpu-clients", out, int(size - 1))) strlcpy(out, kDefaultGpuClients, size);
}
extern "C" void oneios_gpu_plugin_client_list(char *out, size_t size)
{
    if (!out || size < 2) return;
    memset(out, 0, size);
    if (!PE_parse_boot_argn("oneios-gpu-plugin-clients", out, int(size - 1))) oneios_gpu_client_list(out, size);
}

bool oneios_adreno_wanted(void)
{
    uint32_t wanted = oneios_gpu3d_state == 3;
    if (!PE_parse_boot_argn("oneios-gpu-metal", &wanted, sizeof wanted)) PE_parse_boot_argn("droidvm-gpu-metal", &wanted, sizeof wanted);
    return wanted != 0;
}

bool oneios_adreno_parent_bound(void)
{
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *live = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (live && live == boundParent() && live->getClassSize() == oneios_adreno_parent_size) return true;
    oneios_adreno_stage = 0x81;
    IOLog("oneios-adreno: the live IOAcceleratorES (%p, %u bytes) is not the bound one (%p, %u bytes): the bindings are for "
          "another kernel collection; no accelerator\n", live, live ? live->getClassSize() : 0, boundParent(), oneios_adreno_parent_size);
    return false;
}

void oneios_adreno_parent_slide(void)
{
    MetaClassHead *meta = reinterpret_cast<MetaClassHead *>(oneios_adreno_accelerator_meta);
    if (reinterpret_cast<uint64_t>(meta->superClassLink) == oneios_adreno_parent_meta_link) meta->superClassLink = boundParent();
    if (meta->superClassLink != boundParent())
        IOLog("oneios-adreno: AGXAccelerator's superclass is %p, expected %p\n", meta->superClassLink, boundParent());
}

// take20: the writers the render server's read wait gave up on and no longer waits for (staged/gpu.rs GPU3D_READ_WAIT;
// defined with its hooks at the end of this file).
extern "C" volatile uint64_t oneios_gpu_read_wait_stale_count;

namespace {
void selftest(void *, wait_result_t)
{
    volatile uint64_t *st = oneios_adreno_selftest;
    st[0] = 1;
    // The publish has run by now (oneios_adreno_start): stage 5 is "registered", a status is a refusal.
    if (droidvm_metal_publish[1] != 5) {
        st[0] = 0x81;
        IOLog("oneios-adreno: selftest: not published (publish %#llx, accelerator stage %llu status %#llx)\n", droidvm_metal_publish[0],
              droidvm_metal_publish[1], droidvm_metal_publish[2]);
        return;
    }
    // What Metal does first: IOServiceMatching("IOAcceleratorES").
    OSDictionary *matching = IOService::serviceMatching("IOAcceleratorES");
    IOService *service = matching ? IOService::waitForMatchingService(matching, 10ull * NSEC_PER_SEC) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!service) {
        st[0] = 0x82;
        IOLog("oneios-adreno: selftest: IOKit matched no IOAcceleratorES\n");
        return;
    }
    st[0] = 2;
    OSObject *plugin = service->copyProperty("MetalPluginName");
    OSString *pluginName = OSDynamicCast(OSString, plugin);
    IOLog("oneios-adreno: selftest: IOKit matched IOAcceleratorES: class %s (parent %s), %u bytes, MetalPluginName %s, registered %d, "
          "chains %u\n", service->getMetaClass()->getClassName(), service->getMetaClass()->getSuperClass()->getClassName(),
          service->getMetaClass()->getClassSize(), pluginName ? pluginName->getCStringNoCopy() : "(none)",
          (service->getState() & kIOServiceRegisteredState) != 0, droidvm_gpu_chain);
    OSSafeReleaseNULL(plugin);
    // The Metal plug-in's connection: open type 0x4149, then {'NTNL', 0x7200 + op} and the op's record.
    IOUserClient *client = nullptr;
    const IOReturn opened = service->newUserClient(kernel_task, nullptr, DROIDVM_VIRTGPU_OPEN_TYPE, nullptr, &client);
    st[1] = uint32_t(opened);
    if (opened || !client) {
        st[0] = 0x83;
        IOLog("oneios-adreno: selftest: the open with type %#x failed (%#x)\n", DROIDVM_VIRTGPU_OPEN_TYPE, opened);
        service->release();
        return;
    }
    st[0] = 3;
    struct {uint32_t magic, method; droidvm_virtgpu_getparam record;} request = {0x4e544e4c, DROIDVM_VIRTGPU_METHOD + DROIDVM_VIRTGPU_GETPARAM,
                                                                                {DROIDVM_VIRTGPU_PARAM_ABI_VERSION}};
    droidvm_virtgpu_getparam_reply reply = {};
    IOExternalMethodArguments arguments = {};
    arguments.version = kIOExternalMethodArgumentsCurrentVersion;
    arguments.structureInput = &request;
    arguments.structureInputSize = sizeof request;
    arguments.structureOutput = &reply;
    arguments.structureOutputSize = sizeof reply;
    const IOReturn called = client->externalMethod(0, &arguments, nullptr, nullptr, nullptr);
    st[2] = uint32_t(called);
    st[3] = reply.value;
    st[0] = 4;
    IOLog("oneios-adreno: selftest: user client %s opened with type %#x; GETPARAM(ABI_VERSION) through the tunnel: status %#x, value %llu "
          "(the 3D device is %s: gpu state %u)\n", client->getMetaClass()->getClassName(), DROIDVM_VIRTGPU_OPEN_TYPE, called, reply.value,
          oneios_gpu3d_state == 3 ? "there" : "not there", oneios_gpu3d_state);
    client->clientClose();
    client->release();
    service->release();
    st[0] = called ? 0x84 : 5;
}
}  // namespace

// The GPU addon handed over by the loader (xnuhost's <kernel>.gpuaddon sidecar, format NGPUADD2, built by
// userland/metal/build.sh): UniXNU's Data path (kern_exec.c native_gpu_addon_prepare) writes it into
// /private/var/ngpu before its addon mount, so a rebuilt plug-in does not need a new Data seed. This is
// RamBlock's native_ram_block_gpu_addon; storage/ has a weak answer of "none" for a kernel without this archive.
extern "C" int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes)
{
    if (!data || !bytes) return 0;
    *data = nullptr;
    *bytes = 0;
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!map) return 0;
    uint64_t physical = 0, size = 0;
    if (OSData *range = OSDynamicCast(OSData, map->getProperty("GpuAddon"))) {
        if (range->getLength() == 16) {
            memcpy(&physical, range->getBytesNoCopy(), 8);
            memcpy(&size, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
        }
    }
    map->release();
    if (!size || size > (uint64_t(64) << 20) || (physical & 4095) || physical < gPhysBase || size > gPhysSize ||
        physical - gPhysBase > gPhysSize - size) return 0;
    *data = reinterpret_cast<const uint8_t *>(ml_static_ptovirt(physical));
    if (!*data) return 0;
    *bytes = size;
    return 1;
}

// The tunnel's own counters (DroidVMVirtioGpu3D.cpp), printed as they change: whether the processes that hold a Metal
// device give the GPU work, how much, and what GPU memory they hold. Every oneios-gpu-report seconds (default 10, 0: off)
// while any changed, three lines, each short enough for the kernel's log (IOLog cuts a line at 256 bytes, which cut the
// one long line this was before 2026-11-11 just before its memory figures):
//   oneios-gpu-report: 10 s: submits +612 (fenced +600); frames +598 mean 3100 us max ever 41000 us;
//     <1/<2/<4/<8/<16/<32 ms +0/+12/+400/+150/+30/+6 longer +0; fence timeouts +0; refusals +0 (op 0 0x0)
//   oneios-gpu-memory: blobs +14 closed +9 (deferred +2, waiting 3); live 210: own 150 (64 MiB), wired 60 (32 MiB);
//     contexts +0 clients 3
//   oneios-gpu-clients: backboardd 40+20 MiB, iconservicesagent 16+0 MiB
//   oneios-gpu-sizes: backboardd <1 120, <2 30, <4 12, <8 4, <16 2, <32 6, <64 0, 64+ 3 MiB blobs   (one per client)
// "frames" are backboardd's fenced submissions, one or more per frame CoreAnimation draws on the GPU, with the time from
// submission to the GPU being done. "own" blobs are pages this driver allocated (Turnip's buffers, textures): GPU memory
// that exists because of the client. "wired" blobs are the caller's own pages wired for the GPU (CREATE_BLOB_USER: an
// IOSurface or a no-copy buffer imported, often another process's window): those pages are the surface's and are counted
// again wherever the surface is. In the clients line the two are own+wired per process. A leak shows as "own" or the
// live count growing while blobs closed lag behind blobs made, through screens that come back to the same state.
extern "C" {
extern volatile uint64_t droidvm_virtgpu_stats[8], droidvm_virtgpu_rs_fences[3], droidvm_virtgpu_rs_fence_hist[10];
extern volatile uint64_t droidvm_virtgpu_live[4], droidvm_virtgpu_last_refusal[4], droidvm_virtgpu_closes[4];
struct DroidVMClientMemory { char name[16]; uint64_t pageBytes, userBytes; uint32_t sizes[8]; };
extern DroidVMClientMemory droidvm_virtgpu_client_memory[64];
// NativeAdrenoPublish.cpp: the processes that read the accelerator's MetalPluginName (Metal's discovery, so each process
// that asked for a Metal device), the first twelve names with their counts.
struct NativeAdrenoAskerRecord { char name[20]; uint32_t reads; };
extern NativeAdrenoAskerRecord native_adreno_metal_askers[12];
// 0039 (IOUserClient.cpp): prints the plug-in's text rows that have been quiet this long.
void hosted_gpu_text_flush(uint64_t quiet_ns) __attribute__((weak));
}
// Which processes use the CPU, the same interval: the client that keeps a still screen redrawing (SpringBoard committing,
// or only backboardd running an animation of its own), and the heat. The busiest eight, as a percentage of one CPU:
//   oneios-cpu: 10 s: backboardd[53] 62%, SpringBoard[30] 35%, kernel_task[0] 20%, ... (all 140%)
// From each task's recorded user and system time (TASK_POWER_INFO); a process first seen in this walk counts from its
// start (its first seconds are an app's launch), unless its pid's slot was in use in the walk before.
extern "C" {
typedef struct proc *proc_t;
typedef int (*proc_iterate_fn_t)(proc_t, void *);
void proc_iterate(unsigned int flags, proc_iterate_fn_t callout, void *arg, proc_iterate_fn_t filterfn, void *filterarg);
task_t proc_task(proc_t);
int proc_pid(proc_t);
kern_return_t task_info(task_t, task_flavor_t, task_info_t, mach_msg_type_number_t *);
void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
// hosted 0075 (hosted_user.c): the time all processors have idled since boot, up to now, and how many there are
uint64_t hosted_processors_idle_mach(uint32_t *count) __attribute__((weak));
// 0084 (kern_memorystatus.c, pe_serial.c), 0014 (ubc_subr.c) and the disk driver (storage/OneiOSHostedDisk.cpp)
void oneios_memory_report(uint32_t every) __attribute__((weak));
void oneios_disk_report(uint32_t every) __attribute__((weak));
extern uint64_t oneios_console_bytes, oneios_console_ticks;
extern uint64_t cs_native4k_read_ticks, cs_native4k_hash_ticks, cs_native4k_bytes;
}
#include "cpu_window.h"
namespace cpu {
struct Events { uint32_t faults, pageins, cow, calls, machs, csw; };
struct Seen { int pid; uint32_t round; uint64_t mach, sys; Events events; };
Seen seen[1024];
struct Busy { int pid; uint64_t mach, sys; Events events; bool fresh; char name[24]; };
struct Walk { uint32_t round; Busy top[8]; uint64_t all; };
int visit(proc_t p, void *arg)
{
    Walk &w = *static_cast<Walk *>(arg);
    task_t task = proc_task(p);
    const int pid = proc_pid(p);
    if (!task || pid < 0) return 0;
    task_power_info_data_t info = {};
    mach_msg_type_number_t count = TASK_POWER_INFO_COUNT;
    if (task_info(task, TASK_POWER_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    const uint64_t now = info.total_user + info.total_system, sys = info.total_system;
    Seen &e = seen[unsigned(pid) % 1024];
    const bool known = e.pid == pid && e.round + 1 == w.round && now >= e.mach;
    const bool fresh = !known && w.round > 1 && e.round + 1 < w.round;   // the slot was free in the walk before: new
    const uint64_t used = known ? now - e.mach : fresh ? now : 0, used_sys = known && sys >= e.sys ? sys - e.sys : fresh ? sys : 0;
    // What the kernel time was for: faults, page-ins, system calls, context switches (TASK_EVENTS_INFO, since its start).
    task_events_info_data_t ev = {};
    count = TASK_EVENTS_INFO_COUNT;
    Events total = {}, did = {};
    if (task_info(task, TASK_EVENTS_INFO, reinterpret_cast<task_info_t>(&ev), &count) == KERN_SUCCESS) {
        total = { uint32_t(ev.faults), uint32_t(ev.pageins), uint32_t(ev.cow_faults), uint32_t(ev.syscalls_unix), uint32_t(ev.syscalls_mach), uint32_t(ev.csw) };
        const Events was = known ? e.events : Events{};
        did = { total.faults - was.faults, total.pageins - was.pageins, total.cow - was.cow, total.calls - was.calls,
                total.machs - was.machs, total.csw - was.csw };
    }
    e.pid = pid; e.round = w.round; e.mach = now; e.sys = sys; e.events = total;
    w.all += used;
    unsigned at = 8;
    while (at > 0 && w.top[at - 1].mach < used) at--;
    if (at < 8 && used) {
        memmove(&w.top[at + 1], &w.top[at], (7 - at) * sizeof(Busy));
        w.top[at].pid = pid; w.top[at].mach = used; w.top[at].sys = used_sys; w.top[at].events = did; w.top[at].fresh = fresh;
        strlcpy(w.top[at].name, proc_best_name(p), sizeof w.top[at].name);
    }
    return 0;   // PROC_RETURNED
}
// Returns the measured seconds of the window, for the other lines of the same report (round 1: since boot).
uint32_t report(uint32_t every, uint32_t round)
{
    // The span is the time since the last walk, measured: in a stall the report thread wakes late (the phone,
    // 2026-11-13 10:4x: the display's own 10 s line came after 30 s), and dividing by the nominal `every` gave a
    // 10 s line whose processes added up to 1,440% of a CPU on eight vCPUs. The line says the measured seconds.
    // "all" and each share are bounded by the CPUs' busy time (cpu_window.h says why: recount adds a thread's time to its
    // task only when the thread changes state, so a long stretch lands in one window whole).
    static uint64_t last, idle_last;
    const uint64_t walked = mach_absolute_time();
    uint32_t ncpu = 0;
    const uint64_t idle_now = hosted_processors_idle_mach ? hosted_processors_idle_mach(&ncpu) : 0;
    Walk w = {};
    w.round = round;
    proc_iterate(1 /* PROC_ALLPROCLIST */, visit, &w, nullptr, nullptr);
    uint64_t span = 0, idle = 0;
    absolutetime_to_nanoseconds(walked - last, &span);
    absolutetime_to_nanoseconds(idle_now - idle_last, &idle);
    const bool first = round < 2 || !last || (ncpu && !idle_last);
    last = walked;
    idle_last = idle_now;
    span = oneios_cpu_span_ns(span, every);
    every = uint32_t((span + 500000000ull) / 1000000000ull);
    if (first) return every;   // the first walk only records where each process was
    const uint64_t busy = oneios_cpu_busy_ns(span, ncpu, idle);
    char text[220];
    size_t at = 0;
    for (unsigned i = 0; i < 8 && w.top[i].mach; i++) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(w.top[i].mach, &ns);
        at += snprintf(text + at, sizeof text - at, "%s%.20s[%d] %llu%%", at ? ", " : "", w.top[i].name, w.top[i].pid,
                       oneios_cpu_percent(oneios_cpu_capped_ns(ns, busy, ncpu), span));
        if (at >= sizeof text - 1) break;
    }
    uint64_t all = 0;
    absolutetime_to_nanoseconds(w.all, &all);
    // "all": the processors' busy time where the kernel gives it (hosted 0075), else the tasks' sum as before
    IOLog("oneios-cpu: %u s: %s (all %llu%%)\n", every, at ? text : "idle", oneios_cpu_percent(ncpu ? busy : all, span));
    // The same processes' kernel time (TASK_POWER_INFO total_system): their system calls, faults and IOKit calls (a
    // swap's CPU copy or half-float conversion in the framebuffer's user client is backboardd's system time), against
    // their own code in user mode. A line of its own, so that the line above keeps its form for the log tools.
    at = 0;
    for (unsigned i = 0; i < 8 && w.top[i].mach; i++) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(w.top[i].sys, &ns);
        at += snprintf(text + at, sizeof text - at, "%s%.20s[%d] %llu%%", at ? ", " : "", w.top[i].name, w.top[i].pid, ns * 100 / span);
        if (at >= sizeof text - 1) break;
    }
    if (at) IOLog("oneios-cpu-sys: %u s: %s (kernel time of the same processes)\n", every, text);
    // What that kernel time was for, of those with 5% of a CPU of it or more (the four busiest at most): page faults (f, of them
    // page-ins p and copy-on-write c), system calls of unix (u) and mach (m), context switches (x). "+": seen first now. An entry is
    // never cut: one that does not fit whole starts another line (IOLog cuts a line near 255 characters).
    at = 0;
    for (unsigned i = 0, n = 0; i < 8 && n < 4 && w.top[i].mach; i++) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(w.top[i].sys, &ns);
        if (ns * 100 / span < 5) continue;
        n++;
        const Events &d = w.top[i].events;
        char entry[128];   // at most 16 + 13 + 1 + 6 x 12 characters
        const size_t length = size_t(snprintf(entry, sizeof entry, "%.16s[%d]%s f%u p%u c%u u%u m%u x%u", w.top[i].name, w.top[i].pid,
                                              w.top[i].fresh ? "+" : "", d.faults, d.pageins, d.cow, d.calls, d.machs, d.csw));
        if (at && at + 2 + length >= sizeof text) {
            IOLog("oneios-cpu-why: %u s: %s\n", every, text);
            at = 0;
        }
        at += snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", entry);
    }
    if (at) IOLog("oneios-cpu-why: %u s: %s\n", every, text);
    return every;
}
// The console's own cost and the code signatures' 16 KiB to 4 KiB preparation, when they moved.
void costs(uint32_t every)
{
    static uint64_t console[2], signatures[3];
    if (oneios_console_bytes != console[0]) {
        const uint64_t bytes = oneios_console_bytes, ticks = oneios_console_ticks;
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(ticks - console[1], &ns);
        IOLog("oneios-console: %u s: %llu bytes, %llu ms in the port's registers\n", every, bytes - console[0], ns / 1000000);
        console[0] = bytes; console[1] = ticks;
    }
    if (cs_native4k_bytes != signatures[0]) {
        const uint64_t bytes = cs_native4k_bytes, reads = cs_native4k_read_ticks, hashes = cs_native4k_hash_ticks;
        uint64_t read = 0, hash = 0;
        absolutetime_to_nanoseconds(reads - signatures[1], &read);
        absolutetime_to_nanoseconds(hashes - signatures[2], &hash);
        IOLog("oneios-signatures: %u s: %llu KiB prepared, %llu ms reading, %llu ms hashing\n", every, (bytes - signatures[0]) >> 10,
              read / 1000000, hash / 1000000);
        signatures[0] = bytes; signatures[1] = reads; signatures[2] = hashes;
    }
}
}  // namespace cpu

// Each blob's life (hang, 2026-11-13), from DroidVMVirtioGpu3D.cpp's staged hooks (staged/gpu.rs GPU3D): what kind of
// memory the live blobs of each client are, how old they are, and which closed ones still wait for the client's GPU
// work in flight. The phone's report (2026-11-12 23:45) showed backboardd at 926+148 MiB with 1,192 blobs under 1 MiB,
// about 120 more every 10 s (blobs +182 closed +17): the driver closes what it is asked to, so the holder is the client;
// these say which of its memory it is. Two lines per client with blobs, after its oneios-gpu-sizes line:
//   oneios-gpu-kinds: backboardd own task 900 (700 MiB), kernel 2 (1), mappable 650; wired IOSurface 80 (140 MiB),
//     malloc 3 (2), other 1 (1); made +182 gone +17
//   oneios-gpu-ages: backboardd <16K 600, <64K 300, <256K 200, <1M 92; age <10 s 120, <1 min 300, <10 min 500, older 72;
//     held closed 3 (1 MiB, oldest 2 s)
// "own task" blobs are this driver's pages in the client's task (Turnip's BOs: buffers, textures, command streams,
// descriptors, pooled blocks), "kernel" the same in the kernel's map (where the task's could not be had); "mappable" the
// own ones made with the blob flag USE_MAPPABLE (what the CPU writes: command streams, shared buffers, staging).
// "wired" blobs are the caller's own pages (CREATE_BLOB_USER) by the VM tag of the caller's mapping at that address:
// IOSurface (VM_MEMORY_IOSURFACE, a surface imported), malloc (VM_MEMORY_MALLOC*, a no-copy buffer) and other. A blob's
// age is from its creation; "held closed" are blobs closed while the client still had fenced work in flight, which the
// driver frees once that work has finished: a count that only grows, or an oldest age in minutes, is work that never
// finishes. Growth in "own task" small blobs that live long (the age columns) is memory the plug-in or Turnip keeps
// (Turnip's BO cache keeps at most 16 MiB of BOs up to 1 MiB and 96 MiB of larger ones: TU_DARWIN_BO_CACHE_*);
// growth in "wired IOSurface" is surfaces the render server keeps imported.
#include <mach/vm_region.h>
extern "C" kern_return_t mach_vm_region_recurse(vm_map_t, uint64_t *, uint64_t *, uint32_t *, int *, mach_msg_type_number_t *);

namespace blobs {
enum : uint32_t { Capacity = 32768, KindHostVisible = 1, KindWired = 2, KindTaskPages = 4, KindMappable = 1u << 4 };
enum : uint8_t { TagNone, TagIOSurface, TagMalloc, TagOther };
struct Entry { uint32_t id, born, closed; uint8_t slot, kind, tag, pad; uint64_t bytes; };
Entry *table;                    // open addressing by resource id, IOMalloc'd at the first blob
uint32_t used, untracked;
uint64_t made[64], gone[64];     // by client slot, since boot
IOSimpleLock *lock;
uint32_t secondsNow()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return uint32_t(ns / 1000000000ull) + 1;   // never 0: 0 is "not closed"
}
uint32_t hash(uint32_t id) { return (id * 2654435761u) >> 17 & (Capacity - 1); }
Entry *find(uint32_t id)
{
    for (uint32_t i = hash(id), n = 0; table && n < Capacity; i = (i + 1) & (Capacity - 1), n++) {
        if (table[i].id == id) return &table[i];
        if (!table[i].id) return nullptr;
    }
    return nullptr;
}
// The VM tag of the caller's mapping at `address` (a CREATE_BLOB_USER's pages).
uint8_t tagAt(task_t task, uint64_t address)
{
    vm_map_t map = task ? get_task_map(task) : nullptr;
    if (!map) return TagNone;
    uint64_t at = address, size = 0;
    uint32_t depth = 64;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(map, &at, &size, &depth, reinterpret_cast<int *>(&info), &count) != KERN_SUCCESS || at > address)
        return TagOther;
    if (info.user_tag == 88 /* VM_MEMORY_IOSURFACE */) return TagIOSurface;
    if (info.user_tag >= 1 && info.user_tag <= 13 && info.user_tag != 10) return TagMalloc;   // VM_MEMORY_MALLOC .. MALLOC_PROB_GUARD
    return TagOther;
}
}  // namespace blobs

// Where a surface's pixels go (widgets, 2026-11-13). On the phone (2026-11-12 23:45) every widget was blank or white while
// WidgetRenderer_Default had a Metal device (its plug-in loaded, AGXAccelerator opened 4 times, 28+3 MiB of blobs), and
// nothing said where its drawing went: into IOSurfaces or not, done or not, read by the render server or not, and what
// the pages held. The staged driver's hooks (staged/gpu.rs GPU3D) feed this trail; the GPU report prints it while it
// changes, at most these lines each report (docs/gpu.md 5.1, "Where a surface's pixels go"):
//   oneios-gpu-sync: 10 s: tickets +40 ended +40 (closed +0, not kept +0); CPU locks +12 (read-only +10, no wait +0): waited +2,
//     gave up +0, mean 900 us, max ever 4000 us; reads that waited for writers +0, gave up +0, max ever 0 us
//   oneios-gpu-surfaces: WidgetRenderer_ submits +40; writes +24 done +24 (closed +0) on 6 surfaces: 5 since read by
//     another (backboardd), 1 by none, 0 read while written; reads +2: others' +0 (while written +0), with no GPU writer +2
//   oneios-gpu-pixels: WidgetRenderer_ 6 surfaces (5 MiB): zero 0, white 1, flat 0, drawn 5; changed 3; pages of another
//     process's surfaces 0 (-)
//   oneios-gpu-locks: 10 s: SpringBoard 12 (GPU-written 3, read-only 10), WidgetRenderer_Default 2 (GPU-written 2, ...)
// The surfaces lines are by IOSurface ID, from what each command buffer names as it commits (NativeMetal's surfacesync: the
// surfaces it renders into or samples, and when it completed). The pixels lines are by blob: each IOSurface the client
// imported for the GPU (CREATE_BLOB_USER over a mapping tagged VM_MEMORY_IOSURFACE), its guest pages read here, 8 pages
// spread over it and 16 pixels in each: all zero (never written, or cleared to transparent black), white (every byte
// 0xf0 or more), flat (one other value), drawn (anything else); "changed" since the last report. "pages of another
// process's surfaces": the import's first page is the first page of an IOSurface another client imported (one physical
// range seen by both), with the client named that most share with.
extern "C" {
extern volatile uint64_t droidvm_surface_stats[8], droidvm_surface_lock_options[4], droidvm_surface_read_waits[4];
void proc_selfname(char *, int);
uint64_t phystokv(uint64_t pa);                // arm_vm_init.c; the identity on the RAM window under xnuhost
}
namespace trail {
enum : uint32_t { Buckets = 1024, Ways = 4, NoSlot = 0xff, PageCapacity = 2048, Samples = 8, Words = 16, Lockers = 8 };
// An IOSurface the GPU used: its last use, the trail's sequence at its last GPU write done and at its last read by another
// client than its writer, the client that wrote it last and the other that read it last, the writes begun and not ended,
// and whether another client's read began while a write was in flight (bit 0).
struct Surface { uint32_t id, seen; uint64_t wroteSeq, readSeq; uint32_t wroteAt; uint8_t writer, reader, inflight, flags; };
struct Slot { uint64_t submits, writes, done, closedEnds, reads, readOthers, readDuring, readUnwritten; };
struct Locker { char name[16]; uint32_t locks, gpuWritten, readOnly; };
// An IOSurface import's pages: its blob, client, first page, the pages read for the report, and what they held last.
struct Pages { uint32_t id; uint8_t slot, peer, kept, look; uint64_t bytes, first, hash, pa[Samples]; };
Surface *surfaces;                 // Buckets x Ways, 128 KiB, at the first use
Slot slots[64];                    // by client slot, since boot (the report prints the differences)
Locker lockers[Lockers];           // since the last report
uint32_t lockersOther, replaced;
uint64_t sequence;
IOSimpleLock *slock;               // surfaces, slots, lockers
Pages *pages;                      // open addressing by blob id, 192 KiB, at the first import
uint32_t pagesUsed, pagesUntracked;
IOLock *plock;                     // pages: a lock that may be held while the report reads them (thread context only)
bool surfacesReady()
{
    if (!slock) {
        IOSimpleLock *made = IOSimpleLockAlloc();
        if (made && !OSCompareAndSwapPtr(nullptr, made, reinterpret_cast<void *volatile *>(&slock))) IOSimpleLockFree(made);
        if (!slock) return false;
    }
    if (!surfaces) {
        Surface *t = static_cast<Surface *>(IOMallocZero(sizeof(Surface) * Buckets * Ways));
        if (t && !OSCompareAndSwapPtr(nullptr, t, reinterpret_cast<void *volatile *>(&surfaces))) IOFree(t, sizeof(Surface) * Buckets * Ways);
    }
    return surfaces != nullptr;
}
bool pagesReady()
{
    if (!plock) {
        IOLock *made = IOLockAlloc();
        if (made && !OSCompareAndSwapPtr(nullptr, made, reinterpret_cast<void *volatile *>(&plock))) IOLockFree(made);
        if (!plock) return false;
    }
    if (!pages) {
        Pages *t = static_cast<Pages *>(IOMallocZero(sizeof(Pages) * PageCapacity));
        if (t && !OSCompareAndSwapPtr(nullptr, t, reinterpret_cast<void *volatile *>(&pages))) IOFree(t, sizeof(Pages) * PageCapacity);
    }
    return pages != nullptr;
}
// slock held. The surface's entry, or with `make` a new one in place of its bucket's least recently used.
Surface *surface(uint32_t id, uint32_t now, bool make)
{
    Surface *bucket = &surfaces[((id * 2654435761u) >> 22) * Ways], *pick = nullptr;
    for (uint32_t w = 0; w < Ways; w++) {
        if (bucket[w].id == id) return &bucket[w];
        if (!pick || (pick->id && (!bucket[w].id || bucket[w].seen < pick->seen))) pick = &bucket[w];
    }
    if (!make) return nullptr;
    if (pick->id) replaced++;
    *pick = {id, now, 0, 0, 0, uint8_t(NoSlot), uint8_t(NoSlot), 0, 0};
    return pick;
}
uint32_t pageHash(uint32_t id) { return (id * 2654435761u) >> 21 & (PageCapacity - 1); }
// plock held.
Pages *pagesOf(uint32_t id)
{
    for (uint32_t i = pageHash(id), n = 0; pages && n < PageCapacity; i = (i + 1) & (PageCapacity - 1), n++) {
        if (pages[i].id == id) return &pages[i];
        if (!pages[i].id) return nullptr;
    }
    return nullptr;
}
// plock held: the entry goes, the probe chains stay whole (backward shift).
void erase(Pages *e)
{
    uint32_t i = uint32_t(e - pages), j = i;
    for (;;) {
        pages[i].id = 0;
        for (;;) {
            j = (j + 1) & (PageCapacity - 1);
            if (!pages[j].id) { pagesUsed--; return; }
            const uint32_t k = pageHash(pages[j].id);
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
            break;
        }
        pages[i] = pages[j];
        i = j;
    }
}
// From oneios_gpu_blob_made: whether the import is an IOSurface's (only those stay).
void kept(uint32_t id, bool iosurface)
{
    if (!plock || !pages) return;
    IOLockLock(plock);
    if (Pages *e = pagesOf(id)) {
        if (iosurface) e->kept = 1; else erase(e);
    }
    IOLockUnlock(plock);
}
// From oneios_gpu_blob_gone.
void gone(uint32_t id)
{
    if (!plock || !pages) return;
    IOLockLock(plock);
    if (Pages *e = pagesOf(id)) erase(e);
    IOLockUnlock(plock);
}
// What the words read from an import's pages are: 0 all zero, 1 white (every byte 0xf0 or more), 2 one other value, 3 drawn.
uint8_t classify(const uint32_t *w, uint32_t n)
{
    bool zero = true, white = true, flat = true;
    for (uint32_t i = 0; i < n; i++) {
        zero &= w[i] == 0;
        white &= (w[i] & 0xf0f0f0f0u) == 0xf0f0f0f0u;
        flat &= w[i] == w[0];
    }
    return zero ? 0 : white ? 1 : flat ? 2 : 3;
}
const char *slotName(uint32_t s) { return s < 64 && droidvm_virtgpu_client_memory[s].name[0] ? droidvm_virtgpu_client_memory[s].name : "-"; }
// The sync line: the tunnel's own IOSurface counters (DroidVMVirtioGpu3D.cpp surfaceAccess, surfaceTrap, awaitWriters).
void printSync(uint32_t every)
{
    static uint64_t was[8], wasOptions[4], wasReads[4], staleWas;
    uint64_t st[8], op[4], rd[4];
    for (unsigned i = 0; i < 8; i++) st[i] = droidvm_surface_stats[i];
    for (unsigned i = 0; i < 4; i++) { op[i] = droidvm_surface_lock_options[i]; rd[i] = droidvm_surface_read_waits[i]; }
    const uint64_t stale = oneios_gpu_read_wait_stale_count;
    // take20: the writers the read wait gave up on, on a line of their own (the sync line is ~240 bytes already)
    if (stale != staleWas)
        IOLog("oneios-gpu-sync: %u s: writers the render server's read wait gave up on, no longer waited for: +%llu (%llu since the boot)\n", every,
              stale - staleWas, stale);
    staleWas = stale;
    if (!memcmp(st, was, sizeof st) && !memcmp(op, wasOptions, sizeof op) && !memcmp(rd, wasReads, sizeof rd)) return;
    const uint64_t locks = (op[0] + op[1] + op[2] + op[3]) - (wasOptions[0] + wasOptions[1] + wasOptions[2] + wasOptions[3]);
    const uint64_t waited = st[4] - was[4];
    IOLog("oneios-gpu-sync: %u s: tickets +%llu ended +%llu (closed +%llu, not kept +%llu); CPU locks +%llu (read-only +%llu, no wait +%llu): "
          "waited +%llu, gave up +%llu, mean %llu us, max ever %llu us; reads that waited for writers +%llu, gave up +%llu, max ever %llu us\n",
          every, st[0] - was[0], st[1] - was[1], st[2] - was[2], st[3] - was[3], locks, (op[1] + op[3]) - (wasOptions[1] + wasOptions[3]),
          (op[2] + op[3]) - (wasOptions[2] + wasOptions[3]), waited, st[5] - was[5], waited ? (st[6] - was[6]) / waited : 0, st[7],
          rd[0] - wasReads[0], rd[1] - wasReads[1], rd[3]);
    memcpy(was, st, sizeof was);
    memcpy(wasOptions, op, sizeof wasOptions);
    memcpy(wasReads, rd, sizeof wasReads);
}
// The surfaces lines (one per client that submitted or named a surface since the last report) and the CPU lockers.
void printSurfaces(uint32_t every)
{
    static Slot was[64];
    static uint32_t lastAt;
    if (!slock || !surfaces) return;
    const uint32_t now = blobs::secondsNow(), since = lastAt ? lastAt : (now > every ? now - every : 0);
    lastAt = now;
    struct Count { uint32_t written, reachedOther, notRead, during; uint8_t reader; };
    Count *count = static_cast<Count *>(IOMallocZero(sizeof(Count) * 64));
    if (!count) return;
    Slot nowSlots[64];
    Locker seen[Lockers];
    uint32_t other, lost;
    IOSimpleLockLock(slock);
    for (uint32_t i = 0; i < Buckets * Ways; i++) {
        const Surface &s = surfaces[i];
        if (!s.id || s.writer >= 64 || !s.wroteAt || s.wroteAt < since) continue;
        Count &c = count[s.writer];
        c.written++;
        if (s.readSeq > s.wroteSeq) { c.reachedOther++; c.reader = s.reader; } else c.notRead++;
        if (s.flags & 1) c.during++;
    }
    memcpy(nowSlots, slots, sizeof nowSlots);
    memcpy(seen, lockers, sizeof seen);
    other = lockersOther; lost = replaced;
    bzero(lockers, sizeof lockers);
    lockersOther = 0;
    IOSimpleLockUnlock(slock);
    for (uint32_t s = 0; s < 64; s++) {
        const Slot &n = nowSlots[s], &w = was[s];
        if (n.submits == w.submits && n.writes == w.writes && n.reads == w.reads && n.done == w.done) continue;
        const Count &c = count[s];
        IOLog("oneios-gpu-surfaces: %.16s submits +%llu; writes +%llu done +%llu (closed +%llu) on %u surfaces: %u since read by another (%.16s), "
              "%u by none, %u read while written; reads +%llu: others' +%llu (while written +%llu), with no GPU writer +%llu\n",
              slotName(s), n.submits - w.submits, n.writes - w.writes, n.done - w.done, n.closedEnds - w.closedEnds, c.written, c.reachedOther,
              c.reachedOther ? slotName(c.reader) : "-", c.notRead, c.during, n.reads - w.reads, n.readOthers - w.readOthers,
              n.readDuring - w.readDuring, n.readUnwritten - w.readUnwritten);
    }
    memcpy(was, nowSlots, sizeof was);
    IOFree(count, sizeof(Count) * 64);
    char text[200];
    size_t at = 0;
    for (const Locker &l : seen) {
        if (!l.name[0]) continue;
        char one[80];
        const int n = snprintf(one, sizeof one, "%.16s %u (GPU-written %u, read-only %u)", l.name, l.locks, l.gpuWritten, l.readOnly);
        if (at && at + 2 + size_t(n) >= sizeof text) { IOLog("oneios-gpu-locks: %u s: %s\n", every, text); at = 0; }
        at += snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", one);
    }
    if (at || other) IOLog("oneios-gpu-locks: %u s: %s; other processes %u; surfaces forgotten for room %u\n", every, at ? text : "-", other, lost);
}
// The pixels lines: each client's IOSurface imports, what their pages hold. Printed for a client when its line changes.
void printPixels()
{
    static uint64_t printed[64];
    if (!plock || !pages) return;
    struct Count { uint32_t surfaces, cls[4], changed, shared; uint64_t bytes; uint16_t peers[64]; };
    Count *count = static_cast<Count *>(IOMallocZero(sizeof(Count) * 64));
    if (!count) return;
    uint32_t untracked;
    IOLockLock(plock);
    for (uint32_t i = 0; i < PageCapacity; i++) {
        Pages &e = pages[i];
        if (!e.id || !e.kept || e.slot >= 64) continue;
        uint32_t words[Samples * Words], n = 0;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t k = 0; k < Samples; k++) {
            const uint64_t pa = e.pa[k];
            if (!pa || pa < gPhysBase || pa - gPhysBase > gPhysSize - 4096) continue;
            const volatile uint32_t *page = reinterpret_cast<const volatile uint32_t *>(phystokv(pa & ~uint64_t(4095)));
            for (uint32_t j = 0; j < Words; j++) {
                words[n] = page[j * (4096 / 4 / Words) + (k & 3)];
                hash = (hash ^ words[n]) * 1099511628211ull;
                n++;
            }
        }
        Count &c = count[e.slot];
        c.surfaces++;
        c.bytes += e.bytes;
        c.cls[n ? classify(words, n) : 0]++;
        if (e.look && e.hash != hash) c.changed++;
        e.hash = hash;
        e.look = 1;
        if (e.peer < 64) { c.shared++; c.peers[e.peer]++; }
    }
    untracked = pagesUntracked;
    IOLockUnlock(plock);
    for (uint32_t s = 0; s < 64; s++) {
        const Count &c = count[s];
        uint32_t top = NoSlot;
        for (uint32_t p = 0; p < 64; p++) if (c.peers[p] && (top == NoSlot || c.peers[p] > c.peers[top])) top = p;
        uint64_t digest = c.surfaces;
        for (uint32_t k = 0; k < 4; k++) digest = digest * 1000003u + c.cls[k];
        digest = (digest * 1000003u + c.changed) * 1000003u + c.shared;
        if (digest == printed[s]) continue;
        printed[s] = digest;
        if (!c.surfaces && !droidvm_virtgpu_client_memory[s].name[0]) continue;
        IOLog("oneios-gpu-pixels: %.16s %u surfaces (%llu MiB): zero %u, white %u, flat %u, drawn %u; changed %u; pages of another process's "
              "surfaces %u (%.16s)%s\n", slotName(s), c.surfaces, c.bytes >> 20, c.cls[0], c.cls[1], c.cls[2], c.cls[3], c.changed, c.shared,
              top < 64 ? slotName(top) : "-", untracked ? "; table full: some imports not read" : "");
    }
    IOFree(count, sizeof(Count) * 64);
}
void print(uint32_t every)
{
    printSync(every);
    printSurfaces(every);
    printPixels();
}
}  // namespace trail
// A command buffer names an IOSurface it renders into (write) or samples, as it commits (DroidVMVirtioGpu3D.cpp surfaceAccess).
extern "C" void oneios_gpu_surface_use(uint32_t id, uint32_t write, uint32_t slot, uint64_t)
{
    using namespace trail;
    if (!id || slot >= 64 || !surfacesReady()) return;
    const uint32_t now = blobs::secondsNow();
    IOSimpleLockLock(slock);
    Surface *s = surface(id, now, true);
    Slot &me = slots[slot];
    s->seen = now;
    if (write) {
        me.writes++;
        s->writer = uint8_t(slot);
        if (s->inflight < 255) s->inflight++;
    } else {
        me.reads++;
        if (s->writer == NoSlot) {
            me.readUnwritten++;
        } else if (s->writer != slot) {
            me.readOthers++;
            s->reader = uint8_t(slot);
            s->readSeq = ++sequence;
            if (s->inflight) { me.readDuring++; s->flags |= 1; }
        }
    }
    IOSimpleLockUnlock(slock);
}
// The command buffer completed (ticket ended), or its connection closed (ticket 0): a write of the surface is done.
extern "C" void oneios_gpu_surface_done(uint32_t id, uint32_t write, uint32_t slot, uint64_t ticket)
{
    using namespace trail;
    if (!write || slot >= 64 || !slock || !surfaces) return;
    const uint32_t now = blobs::secondsNow();
    IOSimpleLockLock(slock);
    slots[slot].done++;
    if (!ticket) slots[slot].closedEnds++;
    if (Surface *s = surface(id, now, false)) {
        if (s->inflight) s->inflight--;
        s->wroteSeq = ++sequence;
        s->wroteAt = now;
    }
    IOSimpleLockUnlock(slock);
}
// The video decoders' surface locks, for the sampler's oneios-video line (watch:: below; take17).
namespace watch { extern volatile uint64_t decoderLocks[2]; }
// A CPU lock of a surface (IOSurfaceRootUserClient's lock trap), in the locking process; only surfaces the GPU used.
extern "C" void oneios_gpu_surface_lock(uint32_t id, uint32_t options)
{
    using namespace trail;
    if (!slock || !surfaces) return;
    char name[16] = "";
    proc_selfname(name, sizeof name);
    if (!strncmp(name, "videocodecd", sizeof name)) __atomic_add_fetch(&watch::decoderLocks[0], 1, __ATOMIC_RELAXED);
    else if (!strncmp(name, "mediaplaybackd", sizeof name)) __atomic_add_fetch(&watch::decoderLocks[1], 1, __ATOMIC_RELAXED);
    const uint32_t now = blobs::secondsNow();
    IOSimpleLockLock(slock);
    if (const Surface *s = surface(id, now, false)) {
        Locker *l = nullptr;
        for (Locker &k : lockers) {
            if (!strncmp(k.name, name, sizeof k.name)) { l = &k; break; }
            if (!k.name[0] && !l) l = &k;
        }
        if (l && !l->name[0]) strlcpy(l->name, name, sizeof l->name);
        if (l) {
            l->locks++;
            if (s->writer != NoSlot) l->gpuWritten++;
            if (options & 1) l->readOnly++;
        } else {
            lockersOther++;
        }
    }
    IOSimpleLockUnlock(slock);
}
extern "C" void oneios_gpu_submit(uint32_t slot)
{
    if (slot < 64) __atomic_fetch_add(&trail::slots[slot].submits, 1, __ATOMIC_RELAXED);
}
// The GPU's per-process policy on the host (runtime/xnuhost/drm2kgsl/oneios_gpu_policy.h, docs/gpu.md 2.2b): who submits.
// The staged DroidVMVirtioGpu3D.cpp calls this on the client's own thread at its context's creation (CTX_CREATE's
// context_init bits 8-23) and at each fenced execbuffer (the SUBMIT_3D header's three padding bytes):
//   [0] 0x80 (the hint is there) | 0x40 if the thread is DarwinBG | the task's effective role (task_role_t, RunningBoard's);
//   [1] the thread's effective QoS (THREAD_QOS_*);
//   [2] the time to the frame's deadline in 250 us units (0 none, 255 for 63.75 ms or more).
// The role and the deadline are the CPU agent's when its hosted CLPC (fix-host-overhead: thread groups, work intervals,
// RunningBoard roles) defines the two functions below; until then the task's own role and no deadline (the host then
// takes the frame cadence). Their weak defaults here say "not known".
extern "C" int proc_get_effective_task_policy(task_t task, int flavor);
extern "C" int proc_get_effective_thread_policy(thread_t thread, int flavor);
// The thread group's role of `thread` as a task_role_t (0-9), or -1 when not known.
extern "C" __attribute__((weak)) int oneios_clpc_thread_role(thread_t) {return -1;}
// The deadline of the work interval `thread` is in (mach absolute time), or 0 when it is in none.
extern "C" __attribute__((weak)) uint64_t oneios_clpc_frame_deadline(thread_t) {return 0;}
extern "C" void oneios_gpu_hint(uint8_t *out)
{
    enum { PolicyDarwinBG = 0x21, PolicyRole = 0x2A, PolicyQoS = 0x35 };   // TASK_POLICY_DARWIN_BG, _ROLE, _QOS (policy_internal.h)
    thread_t thread = current_thread();
    int role = oneios_clpc_thread_role(thread);
    if (role < 0 || role > 15) role = proc_get_effective_task_policy(current_task(), PolicyRole);
    const int background = proc_get_effective_thread_policy(thread, PolicyDarwinBG);
    const int qos = proc_get_effective_thread_policy(thread, PolicyQoS);
    out[0] = uint8_t(0x80 | (background ? 0x40 : 0) | (role & 0xf));
    out[1] = uint8_t(qos & 7);
    out[2] = 0;
    const uint64_t deadline = oneios_clpc_frame_deadline(thread), now = mach_absolute_time();
    if (deadline > now) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(deadline - now, &ns);
        const uint64_t units = ns / 250000;
        out[2] = uint8_t(units < 1 ? 1 : units > 255 ? 255 : units);
    }
}
// A test switch, off by default: oneios-gpu-read-wait=1 makes the render server's command buffers wait, as they name the
// surfaces they sample, for other processes' GPU work begun before them that writes those surfaces (the driver's
// awaitWriters, at most droidvm_surface_read_wait_ms: 100 ms), which a newer plug-in asks for itself and this pin never
// does. KGSL runs the render server's queue ahead of the apps', so without it a composition can sample a widget's surface
// before the widget's drawing into it has run. The waits are oneios-gpu-sync's "reads that waited for writers".
extern "C" volatile int droidvm_virtgpu_compositor_pid;
extern "C" bool oneios_gpu_reader_waits(int pid)
{
    static int on = -1;
    if (on < 0) {
        uint32_t value = 0;
        PE_parse_boot_argn("oneios-gpu-read-wait", &value, sizeof value);
        on = value != 0;
        IOLog("oneios-gpu-read-wait: %s\n", on ? "on: the render server's reads wait for other processes' writes" : "off");
    }
    return on && pid > 0 && pid == droidvm_virtgpu_compositor_pid;
}
// A blob of the caller's own pages (CREATE_BLOB_USER), its guest physical segments {address, length, padding}: the pages the
// report reads, and whether another client already imported the same first page.
extern "C" void oneios_gpu_blob_segments(uint32_t id, uint32_t slot, uint64_t bytes, const void *list, uint32_t count)
{
    using namespace trail;
    struct Segment { uint64_t address; uint32_t length, padding; };
    if (!id || slot >= 64 || !count || !bytes || !pagesReady()) return;
    const Segment *segment = static_cast<const Segment *>(list);
    Pages p = {};
    p.id = id; p.slot = uint8_t(slot); p.peer = uint8_t(NoSlot); p.bytes = bytes; p.first = segment[0].address;
    // Samples pages spread over the blob, its first and last among them, found in one walk of the segments.
    const uint64_t last = (bytes - 1) & ~uint64_t(4095);
    uint64_t at = 0;
    uint32_t i = 0;
    for (uint32_t k = 0; k < Samples; k++) {
        const uint64_t want = (last / 4096 * k / (Samples - 1)) * 4096;
        while (i < count && at + segment[i].length <= want) at += segment[i++].length;
        p.pa[k] = i < count ? segment[i].address + (want - at) : 0;
    }
    IOLockLock(plock);
    for (uint32_t j = 0; j < PageCapacity; j++) {
        if (pages[j].id && pages[j].kept && pages[j].first == p.first && pages[j].slot != slot) { p.peer = pages[j].slot; break; }
    }
    if (pagesUsed >= PageCapacity / 4 * 3) {
        pagesUntracked++;
    } else {
        uint32_t j = pageHash(id);
        while (pages[j].id && pages[j].id != id) j = (j + 1) & (PageCapacity - 1);
        if (!pages[j].id) pagesUsed++;
        pages[j] = p;
    }
    IOLockUnlock(plock);
}
extern "C" void oneios_gpu_blob_made(uint32_t id, uint32_t slot, uint64_t bytes, uint32_t kind, task_t task, uint64_t user)
{
    using namespace blobs;
    if (!id || slot >= 64) return;
    if (!lock) {
        IOSimpleLock *made_lock = IOSimpleLockAlloc();
        if (made_lock && !OSCompareAndSwapPtr(nullptr, made_lock, reinterpret_cast<void *volatile *>(&lock))) IOSimpleLockFree(made_lock);
        if (!lock) return;
    }
    if (!table) {
        Entry *t = static_cast<Entry *>(IOMallocZero(sizeof(Entry) * Capacity));   // 768 KiB, once
        if (t && !OSCompareAndSwapPtr(nullptr, t, reinterpret_cast<void *volatile *>(&table))) IOFree(t, sizeof(Entry) * Capacity);
        if (!table) return;
    }
    const uint8_t tag = (kind & KindWired) ? tagAt(task, user) : TagNone;
    const uint32_t now = secondsNow();
    IOSimpleLockLock(lock);
    made[slot]++;
    if (used >= Capacity / 4 * 3) {
        untracked++;
    } else {
        uint32_t i = hash(id);
        while (table[i].id && table[i].id != id) i = (i + 1) & (Capacity - 1);
        if (!table[i].id) used++;
        table[i] = {id, now, 0, uint8_t(slot), uint8_t(kind), tag, 0, bytes};
    }
    IOSimpleLockUnlock(lock);
    if (kind & KindWired) trail::kept(id, tag == TagIOSurface);     // the surface trail keeps the pages of IOSurface imports only
}
extern "C" void oneios_gpu_blob_closed(uint32_t id)
{
    using namespace blobs;
    if (!lock) return;
    const uint32_t now = secondsNow();
    IOSimpleLockLock(lock);
    if (Entry *e = find(id)) e->closed = now;
    IOSimpleLockUnlock(lock);
}
extern "C" void oneios_gpu_blob_gone(uint32_t id)
{
    using namespace blobs;
    trail::gone(id);          // before its pages go back (the driver unwires them after this hook)
    if (!lock) return;
    IOSimpleLockLock(lock);
    Entry *e = find(id);
    if (e) {
        if (e->slot < 64) gone[e->slot]++;
        // backward shift: the probe chains stay whole
        uint32_t i = uint32_t(e - table), j = i;
        for (;;) {
            table[i].id = 0;
            for (;;) {
                j = (j + 1) & (Capacity - 1);
                if (!table[j].id) goto done;
                const uint32_t k = hash(table[j].id);
                if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
                break;
            }
            table[i] = table[j];
            i = j;
        }
    done:
        used--;
    }
    IOSimpleLockUnlock(lock);
}
namespace blobs {
struct Census {
    uint32_t task, kernel, mappable, iosurface, malloc, other, small[4], age[4], held;
    uint64_t taskBytes, kernelBytes, iosurfaceBytes, mallocBytes, otherBytes, heldBytes;
    uint32_t heldOldest;
};
// The two lines of each client slot with blobs. `was` keeps made/gone at the last report.
void print(uint64_t (&wasMade)[64], uint64_t (&wasGone)[64])
{
    if (!lock || !table) return;
    Census *all = static_cast<Census *>(IOMallocZero(sizeof(Census) * 64));
    if (!all) return;
    uint64_t madeNow[64], goneNow[64];
    const uint32_t now = secondsNow();
    IOSimpleLockLock(lock);
    for (uint32_t i = 0; i < Capacity; i++) {
        const Entry &e = table[i];
        if (!e.id || e.slot >= 64) continue;
        Census &c = all[e.slot];
        const uint64_t mib = e.bytes;
        if (e.kind & KindWired) {
            if (e.tag == TagIOSurface) { c.iosurface++; c.iosurfaceBytes += mib; }
            else if (e.tag == TagMalloc) { c.malloc++; c.mallocBytes += mib; }
            else { c.other++; c.otherBytes += mib; }
        } else if (!(e.kind & KindHostVisible)) {
            if (e.kind & KindTaskPages) { c.task++; c.taskBytes += mib; } else { c.kernel++; c.kernelBytes += mib; }
            if (e.kind & KindMappable) c.mappable++;
        }
        if (e.bytes < (1u << 20)) c.small[e.bytes < (16u << 10) ? 0 : e.bytes < (64u << 10) ? 1 : e.bytes < (256u << 10) ? 2 : 3]++;
        const uint32_t age = now - e.born;
        c.age[age < 10 ? 0 : age < 60 ? 1 : age < 600 ? 2 : 3]++;
        if (e.closed) {
            c.held++; c.heldBytes += e.bytes;
            if (now - e.closed > c.heldOldest) c.heldOldest = now - e.closed;
        }
    }
    memcpy(madeNow, made, sizeof madeNow);
    memcpy(goneNow, gone, sizeof goneNow);
    const uint32_t lost = untracked;
    IOSimpleLockUnlock(lock);
    for (unsigned s = 0; s < 64; s++) {
        const DroidVMClientMemory &m = droidvm_virtgpu_client_memory[s];
        const Census &c = all[s];
        const uint32_t live = c.task + c.kernel + c.iosurface + c.malloc + c.other;
        if (!m.name[0] || !(live || madeNow[s] != wasMade[s]) || (madeNow[s] == wasMade[s] && goneNow[s] == wasGone[s] && !c.held)) continue;
        IOLog("oneios-gpu-kinds: %.16s own task %u (%llu MiB), kernel %u (%llu), mappable %u; wired IOSurface %u (%llu MiB), malloc %u (%llu), "
              "other %u (%llu); made +%llu gone +%llu\n", m.name, c.task, c.taskBytes >> 20, c.kernel, c.kernelBytes >> 20, c.mappable,
              c.iosurface, c.iosurfaceBytes >> 20, c.malloc, c.mallocBytes >> 20, c.other, c.otherBytes >> 20, madeNow[s] - wasMade[s],
              goneNow[s] - wasGone[s]);
        IOLog("oneios-gpu-ages: %.16s <16K %u, <64K %u, <256K %u, <1M %u; age <10 s %u, <1 min %u, <10 min %u, older %u; held closed %u "
              "(%llu MiB, oldest %u s)%s\n", m.name, c.small[0], c.small[1], c.small[2], c.small[3], c.age[0], c.age[1], c.age[2], c.age[3],
              c.held, c.heldBytes >> 20, c.heldOldest, lost ? "; table full: some blobs not counted" : "");
    }
    memcpy(wasMade, madeNow, sizeof madeNow);
    memcpy(wasGone, goneNow, sizeof goneNow);
    IOFree(all, sizeof(Census) * 64);
}
}  // namespace blobs

// ---- the stages of the render server's fenced submissions (native-perf, 2026-11-26; docs/native.md "Frame pacing")
// oneios-gpu-report's "frames" is one time for three things: the host's part (the doorbell, crosvm's worker, drm2kgsl,
// KGSL, the GPU, the fence coming back), the time until some guest thread looked at the used ring, and the time until
// the client's own waiter took its fence. The staged transport stamps a command as it is taken off the used ring
// (staged/gpu.rs TRANSPORT_STAGES) and the 3D half hands the stamps over here as "frames" is counted (GPU3D_STAGES):
//   oneios-gpu-stages: 10 s: +1132 stamped (+3 not); host <.25/<.5/<1/<2/<4/<8/<16/<32 ms +0/+2/+40/+310/+420/+300/+50/+8
//     longer +2 mean 6100 us
//       "host": from the post (before the doorbell) to the command off the used ring. "not": the command came back
//       between the two looks of one reap, so its stamps were not had (it is in "frames" only).
//   oneios-gpu-look: 10 s: after that, the waiter had it in <.25/<.5/<1/<2/<4 ms +900/+150/+60/+20/+2 longer +0 mean 180 us
//       the guest's own part: what a waiter that polls, or a late wake-up, adds to "frames".
//   oneios-gpu-ahead: 10 s: nothing ahead +800 mean 5200 us (over 8 ms +120, over 16 ms +10); unfenced ahead +332 mean
//     8300 us (over 8 ms +160, over 16 ms +44), 2.1 of them a time; 3.4 in flight at a post
//       the host's time of the submissions posted with no unfenced command unanswered, and of those posted behind some
//       (a blob made or released, a mapping: crosvm's one worker does them first). Much more time behind them is the
//       queue's; the same time is the GPU's.
// And the transport's own counters, which nothing printed (DroidVMVirtioGpu.cpp), when they moved:
//   oneios-gpu-waits: 10 s: waits +5000 (watch +3000, behind it +2000), looks +40000, interrupt sleeps +0 (not needed +0,
//     to the guard +0), interrupts +0, deadlines +12; intid 0 (off), spin 300 us
//       an interrupt sleep that runs to its 20 ms guard is an interrupt that did not come.
//   oneios-gpu-doorbell: 10 s: +3000 <32/<64/<128/<256/<512/<1024 us +0/+2100/+800/+90/+8/+2 longer +0[; interrupt to the
//     look +N <32/... ...]
//   oneios-gpu-legs: 10 s: blobs made +921 in <.5/<1/<2/<4/<8/<16/<33 ms +800/+60/+30/+20/+8/+3/+0 longer +0; released +..
//       the device's time for the unfenced commands, from the doorbell to the answer off the ring (a second line for the
//       ccmds and the rest).
// oneios-gpu-stages=1 turns these lines on (default 0: off, and the transport's stamps are not asked for; oneios-gpu-report=0
// turns every report off). The display-pace and -wait lines do not depend on it.
extern "C" {
extern volatile uint64_t droidvm_virtio_gpu_waits[9];
extern volatile uint32_t droidvm_virtio_gpu_legs[4][2][12], droidvm_virtio_gpu_kick_us[12], droidvm_virtio_gpu_irq_reap_us[12];
extern volatile uint32_t droidvm_virtio_gpu_intid, droidvm_virtio_gpu_irq, droidvm_virtio_gpu_spin_us;
// ../display/OneiOSDisplay.cpp: the swaps' own line (oneios-display-pace). Weak: a kernel without the display has none.
void oneios_display_pace_report(uint32_t every) __attribute__((weak));
}
#include "frame_stages.h"
namespace stages {
oneios_stage_hist hostLeg, lookLeg;
// Submissions whose stamps were not had; then by what was ahead of them at the post, [0] nothing, [1] unfenced commands:
// how many, the host's time (us), of them 8 ms or more, 16 ms or more. Then the unfenced commands ahead and the commands
// in flight at the posts, summed.
uint64_t unstamped, split[2][4], aheadSum, inFlightSum;
void add(uint64_t *to, uint64_t by) { __atomic_add_fetch(to, by, __ATOMIC_RELAXED); }
uint64_t get(const uint64_t *from) { return __atomic_load_n(from, __ATOMIC_RELAXED); }

void text(char *out, size_t size, const uint64_t *d, unsigned n)
{
    size_t at = 0;
    out[0] = 0;
    for (unsigned i = 0; i < n && at + 1 < size; i++) at += size_t(snprintf(out + at, size - at, "%s+%llu", i ? "/" : "", d[i]));
}

// oneios-gpu-stages=1 (default 0: off): the transport's stamps are asked for, per fenced submission, and the stage lines printed.
// Off, nothing here runs on a submission: the 3D half's weak hook below answers 0 and times() is never called.
bool enabled()
{
    static uint32_t on = 2;
    if (on == 2) { uint32_t v = 0; PE_parse_boot_argn("oneios-gpu-stages", &v, sizeof v); on = v ? 1 : 0; }
    return on == 1;
}

void report(uint32_t every)
{
    // The swaps' own lines (display-pace and -wait) read counters the display keeps anyway: they do not depend on the stages.
    if (oneios_display_pace_report) oneios_display_pace_report(every);
    if (!enabled()) return;
    static oneios_stage_hist hostWas, lookWas;
    static uint64_t unstampedWas, splitWas[2][4], aheadWas, inFlightWas;
    oneios_stage_hist h, l;
    oneios_stage_read(&hostLeg, &h);
    oneios_stage_read(&lookLeg, &l);
    const uint64_t un = get(&unstamped);
    if (h.count != hostWas.count || un != unstampedWas) {
        char line[120];
        oneios_stage_text(line, sizeof line, &h, &hostWas, 8);
        IOLog("oneios-gpu-stages: %u s: +%llu stamped (+%llu not); host <.25/<.5/<1/<2/<4/<8/<16/<32 ms %s\n", every,
              h.count - hostWas.count, un - unstampedWas, line);
        oneios_stage_text(line, sizeof line, &l, &lookWas, 5);
        IOLog("oneios-gpu-look: %u s: after that, the waiter had it in <.25/<.5/<1/<2/<4 ms %s\n", every, line);
        uint64_t d[2][4];
        for (unsigned k = 0; k < 2; k++) for (unsigned i = 0; i < 4; i++) { const uint64_t v = get(&split[k][i]); d[k][i] = v - splitWas[k][i]; splitWas[k][i] = v; }
        const uint64_t ahead = get(&aheadSum), flight = get(&inFlightSum), n = d[0][0] + d[1][0];
        const uint64_t aheadTenths = d[1][0] ? (ahead - aheadWas) * 10 / d[1][0] : 0, flightTenths = n ? (flight - inFlightWas) * 10 / n : 0;
        IOLog("oneios-gpu-ahead: %u s: nothing ahead +%llu mean %llu us (over 8 ms +%llu, over 16 ms +%llu); unfenced ahead +%llu mean %llu us "
              "(over 8 ms +%llu, over 16 ms +%llu), %llu.%llu of them a time; %llu.%llu in flight at a post\n", every,
              d[0][0], d[0][0] ? d[0][1] / d[0][0] : 0, d[0][2], d[0][3], d[1][0], d[1][0] ? d[1][1] / d[1][0] : 0, d[1][2], d[1][3],
              aheadTenths / 10, aheadTenths % 10, flightTenths / 10, flightTenths % 10);
        aheadWas = ahead; inFlightWas = flight;
        hostWas = h; lookWas = l; unstampedWas = un;
    }
    // The transport's waits.
    static uint64_t waitsWas[9];
    uint64_t w[9], dw[9];
    bool moved = false;
    for (unsigned i = 0; i < 9; i++) { w[i] = droidvm_virtio_gpu_waits[i]; dw[i] = w[i] - waitsWas[i]; moved |= dw[i] != 0; }
    if (moved) {
        IOLog("oneios-gpu-waits: %u s: waits +%llu (watch +%llu, behind it +%llu), looks +%llu, interrupt sleeps +%llu (not needed +%llu, "
              "to the guard +%llu), interrupts +%llu, deadlines +%llu; intid %u (%s), spin %u us\n", every, dw[0], dw[1], dw[2], dw[3], dw[4],
              dw[5], dw[7], dw[6], dw[8], droidvm_virtio_gpu_intid, droidvm_virtio_gpu_intid && droidvm_virtio_gpu_irq ? "on" : "off",
              droidvm_virtio_gpu_spin_us);
        memcpy(waitsWas, w, sizeof w);
    }
    // The doorbell, and with the interrupt on, from the interrupt to the look that took its answer.
    static uint32_t kickWas[ONEIOS_LEG_BUCKETS], irqWas[ONEIOS_LEG_BUCKETS];
    uint32_t kick[ONEIOS_LEG_BUCKETS], irq[ONEIOS_LEG_BUCKETS];
    for (unsigned i = 0; i < ONEIOS_LEG_BUCKETS; i++) { kick[i] = droidvm_virtio_gpu_kick_us[i]; irq[i] = droidvm_virtio_gpu_irq_reap_us[i]; }
    uint64_t dk[ONEIOS_KICK_SHOWN], di[ONEIOS_KICK_SHOWN];
    const uint64_t kicks = oneios_kick_delta(kick, kickWas, dk), irqs = oneios_kick_delta(irq, irqWas, di);
    if (kicks || irqs) {
        char a[100], b[100], tail[150];
        text(a, sizeof a, dk, ONEIOS_KICK_SHOWN - 1);
        tail[0] = 0;
        if (irqs) {
            text(b, sizeof b, di, ONEIOS_KICK_SHOWN - 1);
            snprintf(tail, sizeof tail, "; interrupt to the look +%llu %s longer +%llu", irqs, b, di[ONEIOS_KICK_SHOWN - 1]);
        }
        IOLog("oneios-gpu-doorbell: %u s: +%llu <32/<64/<128/<256/<512/<1024 us %s longer +%llu%s\n", every, kicks, a, dk[ONEIOS_KICK_SHOWN - 1], tail);
        memcpy(kickWas, kick, sizeof kick);
        memcpy(irqWas, irq, sizeof irq);
    }
    // The device's time for the unfenced commands, by kind (droidvm_virtio_gpu_legs[kind][0]).
    static uint32_t legsWas[4][ONEIOS_LEG_BUCKETS];
    static const char *const kinds[4] = {"ccmds", "blobs made", "released", "the rest"};
    static const unsigned order[4] = {1, 2, 0, 3};
    char line[250];
    size_t at = 0;
    for (unsigned k = 0; k < 4; k++) {
        const unsigned kind = order[k];
        uint32_t now[ONEIOS_LEG_BUCKETS];
        for (unsigned i = 0; i < ONEIOS_LEG_BUCKETS; i++) now[i] = droidvm_virtio_gpu_legs[kind][0][i];
        uint64_t d[ONEIOS_LEG_SHOWN];
        const uint64_t n = oneios_leg_delta(now, legsWas[kind], d);
        memcpy(legsWas[kind], now, sizeof now);
        if (!n) continue;
        char numbers[100], one[160];
        text(numbers, sizeof numbers, d, ONEIOS_LEG_SHOWN - 1);
        const size_t length = size_t(snprintf(one, sizeof one, "%s +%llu in <.5/<1/<2/<4/<8/<16/<33 ms %s longer +%llu", kinds[kind], n, numbers,
                                              d[ONEIOS_LEG_SHOWN - 1]));
        // two kinds to a line at most, and never a cut one (IOLog cuts near 255 characters)
        if (at && at + 2 + length >= 200) { IOLog("oneios-gpu-legs: %u s: %s\n", every, line); at = 0; }
        at += size_t(snprintf(line + at, sizeof line - at, "%s%s", at ? "; " : "", one));
    }
    if (at) IOLog("oneios-gpu-legs: %u s: %s\n", every, line);
}
}  // namespace stages

// The 3D half's reapFences, as it counts one of the render server's fenced submissions into "frames": `stamped` 0 when
// the command's stamps were not had, else t[0] its post and t[1] its answer off the used ring (us, the transport's
// clock, as `now`), t[2] the commands in flight at its post and t[3] the unfenced ones among them.
// The 3D half asks before it takes a command's stamps (times() takes the transport's lock): only with oneios-gpu-stages=1.
extern "C" bool oneios_gpu_stage_wanted(void)
{
    return stages::enabled();
}

// take20: the first frames after a rest (watch.h's oneios-wake). The swap path (oneios_gpu_swap_gap, under the display's
// frame lock) begins a wake and adds its gaps; the fence path below adds the render server's host legs of its fenced frames
// reaped after the wake began, each in a slot of its own (several threads reap); the sampler prints the line once the wake is
// complete and only then lets the next one begin (`active`). At most 300 a boot. Only with oneios-gpu-stages=1 for the legs
// (the fence path's stamps); the gaps come either way.
#include "watch.h"   // its include below (the watch's section) is then a no-op
namespace wakes {
oneios_wake wake;
volatile uint32_t active;      // 1 from a wake's begin until its line is printed
volatile uint32_t lines;
void leg(uint64_t device, uint64_t posted)
{
    if (!__atomic_load_n(&active, __ATOMIC_ACQUIRE) || posted < wake.at_us) return;
    const uint32_t slot = __atomic_fetch_add(&wake.legs, 1, __ATOMIC_RELAXED);
    if (slot < ONEIOS_WAKE_FRAMES) wake.leg_us[slot] = device > 0xffffffffull ? 0xffffffffu : uint32_t(device);
}
}  // namespace wakes

extern "C" void oneios_gpu_stage_fence(uint32_t stamped, const uint64_t *t, uint64_t now)
{
    using namespace stages;
    if (!stamped || !t || t[1] < t[0] || now < t[1]) {
        add(&unstamped, 1);
        return;
    }
    const uint64_t device = t[1] - t[0];
    wakes::leg(device, t[0]);
    oneios_stage_add(&hostLeg, device);
    oneios_stage_add(&lookLeg, now - t[1]);
    uint64_t *s = split[t[3] ? 1 : 0];
    add(&s[0], 1);
    add(&s[1], device);
    if (device >= 8000) add(&s[2], 1);
    if (device >= 16000) add(&s[3], 1);
    add(&aheadSum, t[3]);
    add(&inFlightSum, t[2]);
}

// ---- the lines written as things happen (take17, 2026-11-26; watch.h says why; docs/native.md "What take16's lines say")
// A sampler thread: every 100 ms while swaps come (every second otherwise) it notes which processes had the CPUs and what
// the GPU's clients submitted, and from that and three hooks it writes, each line with the time of day (UTC):
//   oneios-hitch: 12:27:12: 739 ms without a swap (the one before came after 16 ms); one process busy (all 771 ms of CPU
//     in 900 ms of ticks); the GPU: apps +0 submits, the render server +2, blobs +14; hitch 5 (0 not printed)
//   oneios-hitch: 12:27:12: the busiest in it: com.apple.WebKit[287] 702 ms, backboardd[55] 12 ms
//       a gap of oneios-hitch-ms (150) to 5000 ms between two swaps, when the swap before the gap came within 50 ms of
//       its own predecessor (frames were coming). "one process busy": it had a CPU for half the gap or more; "the CPUs
//       busy": several did; "nothing busy": a wait, or one animation's end and the next one's start. The CPU times are
//       the ticks' that cover the gap and the 100 ms after it (a thread's time is added to its task when it next
//       switches: cpu_window.h), so they can be a little more than the gap. At most six a 10 s, 2000 a boot (take19; 400 before).
//   oneios-gpu-room: 12:29:21: backboardd passed 90 % of its GPU addresses: 1422 -> 1979 of 2048 MiB (most 1979)
//   oneios-gpu-room: 12:30:21: backboardd let go of 1309 MiB: 1979 -> 670 of 2048 MiB
//       a client's blobs (own and wired, as oneios-gpu-clients) against a context's part of the GPU addresses: 2048
//       MiB, the slice the phone's crosvm cuts for each context (oneios-gpu-room-mib=N for another; watch::room). The
//       DRM capset names the device's whole range, said once as it is first asked (DroidVMVirtioGpu3D.cpp getCaps,
//       staged/gpu.rs GPU3D_ROOM): `the device has 131072 MiB of GPU addresses from 0x40001e8000 (the DRM capset)`.
//   oneios-pressure: 12:29:22: normal -> warning; available 604 MiB
//       the memory pressure level (0087), as it changes: every process with a memory pressure source is then told.
//   oneios-video: 12:29:41: decoder locks +61 (videocodecd +61, mediaplaybackd +0), swaps +58; backboardd GPU 1682+297 of
//     2048 MiB (96 %); available 573 MiB, pressure warning
//       each second in which a video decoder locked surfaces for the CPU (about one a frame it writes), and five more
//       after the last (a picture that stops is `locks +0`). At most 1500 a boot.
// And in the 10 s report: `oneios-clock:` the time of day of each round with the sampler's own cost in it (its walks of
// the process list, their CPU time and the time they took), and `oneios-gpu-room:` every client that holds a tenth of
// a context's part or more.
// take20 (watch.h says why): the hitch's second line ends "; slow blobs (8 ms or more) +M" when M is not 0 (CREATE_BLOB
// commands in the gap whose device leg took 8 ms or more, the transport's own buckets), and two lines more:
//   oneios-frames: 10:47:13: ... (the 10 s report) `oneios-frames: 10 s: gaps right after frames of 25-50/50-100/100-150 ms
//     +12/+3/+1; the longest: 87 ms at 10:47:03.214, 64 ms at 10:47:03.402, 41 ms at 10:47:09.977`
//   oneios-wake: 10:50:40: after 33.4 s without a swap: the next swaps 8.3/8.4/25.0/... ms apart; the render server's next
//     fenced frames 31.2/12.0/4.1/... ms on the host
//   oneios-frames=0      neither of the two (the hitch lines' blob count stays)
//   oneios-watch=0       no sampler: none of these lines but the report's two
//   oneios-hitch-ms=N    the shortest gap that is a line (default 150; 0: no oneios-hitch line)
//   oneios-gpu-room-mib=N  a context's part of the GPU addresses (default 2048)
#include <kern/clock.h>
#include "watch.h"
extern "C" {
// DroidVMVirtioGpu3D.cpp as staged (staged/gpu.rs GPU3D_ROOM) and DroidVMVirtioGpuDisplay.cpp (staged/display.rs SCANOUT).
void oneios_gpu_capset_seen(uint32_t pid, uint64_t start, uint64_t size);
void oneios_gpu_swap_gap(uint64_t gap_us, uint64_t now_us);
// 0087 (kern_memorystatus.c)
uint32_t oneios_memory_available_mib(void) __attribute__((weak));
uint32_t oneios_memory_pressure_level(void) __attribute__((weak));
// osfmk/kern/thread.h: the calling thread's CPU time, mach units. (recount_task_times, a task's user and system time
// summed over its CPU kinds, is kern/recount.h's, which the kernel's headers above bring in.)
uint64_t thread_get_runtime_self(void);
}
namespace watch {
uint64_t nowUs() { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
// "12:29:41": the time of day `ago_us` before now, UTC (the guest's clock is the phone's, 0078): crosvm.log's own, and an
// hour (or two) behind the phone's thermal.txt and logcat in Italy.
void clockText(char *out, size_t size, uint64_t ago_us)
{
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_calendar_microtime(&secs, &usecs);
    const uint64_t at = uint64_t(secs) - (ago_us + 500000) / 1000000;
    const uint32_t day = uint32_t(at % 86400);
    snprintf(out, size, "%02u:%02u:%02u", day / 3600, day / 60 % 60, day % 60);
}
// take20: the same to the millisecond ("12:29:41.375"), for oneios-frames.
void clockTextMs(char *out, size_t size, uint64_t ago_us)
{
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_calendar_microtime(&secs, &usecs);
    const uint64_t nowUsDay = uint64_t(secs) * 1000000ull + usecs;
    const uint64_t at = nowUsDay > ago_us ? nowUsDay - ago_us : 0;
    const uint32_t day = uint32_t(at / 1000000ull % 86400), ms = uint32_t(at / 1000ull % 1000);
    snprintf(out, size, "%02u:%02u:%02u.%03u", day / 3600, day / 60 % 60, day % 60, ms);
}
const char *levelName(uint32_t level)
{
    static const char *const names[4] = {"normal", "warning", "urgent", "critical"};
    return level < 4 ? names[level] : "?";
}

// The GPU addresses a context has. Droid-VM's renderer gives every context a slice of its one range (crosvm.log: `VA
// slices: 64 x 2GB over VBO 0x2000000000`) and tells the context's Turnip by GET_PARAM in the context's own ring, which
// the kernel does not read: the kernel sees the DRM capset, and that names the whole range (128 GiB on this phone:
// `va_start=0x40001e8000 va_size=0x2000000000`). So a context's part is a figure here: 2048 MiB, the slice of the
// phone's crosvm f7c6ce8, or oneios-gpu-room-mib=N for a renderer that cuts another (its `VA slices` line says it);
// never more than the capset's range. The capset's range is kept as first seen (under the 3D half's clients lock)
// and said once.
uint64_t deviceStart;
volatile uint64_t deviceSize;
uint32_t roomMiB = 2048;
uint64_t room()
{
    const uint64_t figure = uint64_t(roomMiB) << 20, device = deviceSize;
    return device && device < figure ? device : figure;
}

// The gaps the swap path found (the display's frame lock is held there: it stores, the sampler prints).
struct Gap { uint64_t gap, at, before; };
Gap gaps[8];
volatile uint32_t gapHead;
uint64_t beforeUs;                     // the swap path's own: the gap before this one
volatile uint64_t lastSwapUs, swapCount;
uint32_t leastMs = 150;
// take20 (watch.h): the gaps oneios-frames counts (the swap path's: one writer, under the display's frame lock), and the
// switch of the frames and wake lines (oneios-frames=0: neither).
volatile uint64_t framesCount[3], framesHead;
oneios_frame_gap framesRing[ONEIOS_FRAMES_RING];
uint32_t framesOn = 1;

// The surface locks of the video decoders (oneios_gpu_surface_lock, the locking process's thread).
volatile uint64_t decoderLocks[2];     // videocodecd, mediaplaybackd

// The sampler's own cost, for the 10 s report's oneios-clock line: its walks of the process list, the CPU time they
// took (the thread's own, which is the cost), the time from each walk's start to its end (longer when the thread
// waited for a CPU) and the longest of those.
volatile uint64_t walks, walkUs, walkMostUs, walkCpuUs;

// The sampler's walk: each process's CPU time since the tick before (its own table, so that the 10 s report's is not
// disturbed). A process first seen counts from its next tick. The time is the task's own sum (recount_task_times: a
// few words read, no lock): TASK_POWER_INFO, which the 10 s report asks, takes the task's lock and each of its
// threads' at splsched, and this walk is ten a second.
struct Seen { int pid; uint64_t mach; };
Seen seen[4096];
int visit(proc_t p, void *arg)
{
    oneios_watch_tick &t = *static_cast<oneios_watch_tick *>(arg);
    task_t task = proc_task(p);
    const int pid = proc_pid(p);
    if (!task || pid < 0) return 0;
    const struct recount_times_mach times = recount_task_times(task);
    const uint64_t now = times.rtm_user + times.rtm_system;
    Seen &e = seen[unsigned(pid) % 4096];
    const uint64_t used = e.pid == pid && now >= e.mach ? now - e.mach : 0;
    e.pid = pid;
    e.mach = now;
    if (!used) return 0;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(used, &ns);
    const uint32_t us = ns / 1000 > 0xffffffffull ? 0xffffffffu : uint32_t(ns / 1000);
    t.all_us += us;
    if (us >= 500) oneios_watch_put(t.procs, &t.count, pid, proc_best_name(p), us);
    return 0;   // PROC_RETURNED
}

uint32_t hitches, hitchesUnprinted;
void hitch(const oneios_watch_tick *ring, uint64_t head, const Gap &g, uint64_t now)
{
    static uint32_t inWindow;
    static uint64_t windowAt;
    hitches++;
    if (g.at - windowAt > 10000000) { windowAt = g.at; inWindow = 0; }
    // take19: 2000 a boot (take18 used its 400 by 06:51, 25 minutes in: the last closes had no line)
    if (inWindow++ >= 6 || hitches > 2000) { hitchesUnprinted++; return; }
    oneios_watch_sum sum;
    oneios_watch_sum_window(ring, head, g.at - g.gap, g.at + 100000, &sum);
    char when[12], text[160];
    clockText(when, sizeof when, now > g.at ? now - g.at : 0);
    size_t at = 0;
    text[0] = 0;
    for (uint32_t i = 0; i < sum.count && i < ONEIOS_WATCH_SHOWN && sum.procs[i].us >= 1000 && at < 120; i++)
        at += size_t(snprintf(text + at, sizeof text - at, "%s%.16s[%d] %u ms", at ? ", " : "", sum.procs[i].name, sum.procs[i].pid, sum.procs[i].us / 1000));
    const int kind = oneios_hitch_kind(&sum, g.gap);
    // two lines, each made here and under the console's 255 bytes a line (a longer one is cut and loses its end)
    char line[250];
    snprintf(line, sizeof line, "oneios-hitch: %s: %llu ms without a swap (the one before came after %llu ms); %s (all %llu ms of CPU in "
             "%llu ms of ticks); the GPU: apps +%u submits, the render server +%u, blobs +%u; hitch %u (%u not printed)", when, g.gap / 1000,
             g.before / 1000, kind == 2 ? "one process busy" : kind == 1 ? "the CPUs busy" : "nothing busy", sum.all_us / 1000,
             sum.span_us / 1000, sum.app_submits, sum.rs_submits, sum.blobs, hitches, hitchesUnprinted);
    IOLog("%s\n", line);
    // take20: the blobs whose CREATE_BLOB took the device 8 ms or more, on the second line (at most 242 bytes with it): the first
    // line is take19's, whose worst case already fills line[] (249 bytes with 4-digit counts and a 6-digit "came after")
    char slow[48] = "";
    if (sum.slow_blobs) snprintf(slow, sizeof slow, "; slow blobs (8 ms or more) +%u", sum.slow_blobs);
    IOLog("oneios-hitch: %s: the busiest in it: %s%s\n", when, at ? text : "none over 1 ms", slow);
}

// Once a second: each client's room, the pressure level, and the decoders' second.
void second(uint64_t now, uint64_t swaps)
{
    static uint64_t roomWas[64], roomMost[64], locksWas[2], swapsWas;
    static uint32_t roomLines, levelWas, videoTail, videoLines;
    static bool deviceSaid;
    char when[12];
    clockText(when, sizeof when, 0);
    // the device's range, said once, by the sampler rather than under the 3D half's lock
    if (!deviceSaid && deviceSize) {
        deviceSaid = true;
        IOLog("oneios-gpu-room: %s: the device has %llu MiB of GPU addresses from %#llx (the DRM capset); a context's part is taken as "
              "%llu MiB (oneios-gpu-room-mib)\n", when, deviceSize >> 20, deviceStart, room() >> 20);
    }
    uint64_t compositor[3] = {};   // the render server's own bytes, wired bytes, range
    for (unsigned s = 0; s < 64; s++) {
        const DroidVMClientMemory &c = droidvm_virtgpu_client_memory[s];
        const uint64_t used = c.name[0] ? c.pageBytes + c.userBytes : 0, size = used ? room() : 0;
        // take18: backboardd has a slot (a context, its own range) per connection, most of them small or empty, and the last
        // one named it used to win: take17's 219 lines all said "backboardd GPU 0+0 of 0 MiB" while the room lines had its
        // main context at 1844 to 1910 MiB. The render server's largest context is the one against its range.
        if (!strncmp(c.name, "backboardd", sizeof c.name) && used >= compositor[0] + compositor[1]) {
            compositor[0] = c.pageBytes; compositor[1] = c.userBytes; compositor[2] = size;
        }
        if (used > roomMost[s] || !used) roomMost[s] = used;
        const int event = oneios_room_event(roomWas[s], used, size);
        if (event && roomLines < 300) {
            roomLines++;
            if (event == 1)
                IOLog("oneios-gpu-room: %s: %.16s passed 90 %% of its GPU addresses: %llu -> %llu of %llu MiB (most %llu)\n", when, c.name,
                      roomWas[s] >> 20, used >> 20, size >> 20, roomMost[s] >> 20);
            else
                IOLog("oneios-gpu-room: %s: %.16s let go of %llu MiB: %llu -> %llu of %llu MiB\n", when, c.name, (roomWas[s] - used) >> 20,
                      roomWas[s] >> 20, used >> 20, size >> 20);
        }
        roomWas[s] = used;
    }
    const uint32_t level = oneios_memory_pressure_level ? oneios_memory_pressure_level() : 0;
    const uint32_t available = oneios_memory_available_mib ? oneios_memory_available_mib() : 0;
    if (level != levelWas) {
        IOLog("oneios-pressure: %s: %s -> %s; available %u MiB\n", when, levelName(levelWas), levelName(level), available);
        levelWas = level;
    }
    const uint64_t locks[2] = {decoderLocks[0], decoderLocks[1]}, d[2] = {locks[0] - locksWas[0], locks[1] - locksWas[1]};
    locksWas[0] = locks[0];
    locksWas[1] = locks[1];
    if (d[0] + d[1]) videoTail = 6;
    if (videoTail && videoLines < 1500) {
        videoTail--;
        videoLines++;
        IOLog("oneios-video: %s: decoder locks +%llu (videocodecd +%llu, mediaplaybackd +%llu), swaps +%llu; backboardd GPU %llu+%llu of %llu MiB "
              "(%u %%); available %u MiB, pressure %s\n", when, d[0] + d[1], d[0], d[1], swaps - swapsWas, compositor[0] >> 20, compositor[1] >> 20,
              compositor[2] >> 20, oneios_room_percent(compositor[0] + compositor[1], compositor[2]), available, levelName(level));
    }
    swapsWas = swaps;
    (void)now;
}

// The 10 s report's line: every client that holds a tenth of its range or more, in lines of at most ~200 bytes.
void reportRooms()
{
    char text[200];
    size_t at = 0;
    for (unsigned s = 0; s < 64; s++) {
        const DroidVMClientMemory &c = droidvm_virtgpu_client_memory[s];
        const uint64_t used = c.name[0] ? c.pageBytes + c.userBytes : 0, size = used ? room() : 0;
        if (!size || used * 10 < size) continue;
        char one[72];
        const int n = snprintf(one, sizeof one, "%.16s %llu of %llu MiB (%u %%)", c.name, used >> 20, size >> 20, oneios_room_percent(used, size));
        if (at && at + 2 + size_t(n) >= sizeof text) {
            IOLog("oneios-gpu-room: %s\n", text);
            at = 0;
        }
        at += size_t(snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", one));
    }
    if (at) IOLog("oneios-gpu-room: %s\n", text);
}

// take20: the 10 s report's oneios-frames line (watch.h): the round's gaps of 25 ms up to the hitch lines' least right after
// frames, by class, and the longest three with their time of day. Nothing when the round had none.
void reportFrames(uint32_t every)
{
    static uint64_t countsWas[3], taken;
    if (!framesOn) return;
    const uint64_t have = __atomic_load_n(&framesHead, __ATOMIC_ACQUIRE);
    const uint64_t c[3] = {framesCount[0], framesCount[1], framesCount[2]};
    const uint64_t d[3] = {c[0] - countsWas[0], c[1] - countsWas[1], c[2] - countsWas[2]};
    memcpy(countsWas, c, sizeof c);
    if (!d[0] && !d[1] && !d[2]) { taken = have; return; }
    oneios_frame_gap top[ONEIOS_FRAMES_KEPT];
    oneios_frames_longest(framesRing, taken, have, top);
    taken = have;
    const uint64_t now = nowUs();
    char text[3][32];
    for (unsigned i = 0; i < ONEIOS_FRAMES_KEPT; i++) {
        text[i][0] = 0;
        if (!top[i].gap_us) continue;
        char when[16];
        clockTextMs(when, sizeof when, now > top[i].at_us ? now - top[i].at_us : 0);
        snprintf(text[i], sizeof text[i], "%s%llu ms at %s", i ? ", " : "", top[i].gap_us / 1000, when);
    }
    IOLog("oneios-frames: %u s: gaps right after frames of 25-50/50-100/100-%u ms +%llu/+%llu/+%llu; the longest: %s%s%s\n", every,
          leastMs ? leastMs : 150, d[0], d[1], d[2], text[0], text[1], text[2]);
}

void run(void *, wait_result_t)
{
    static oneios_watch_tick ring[ONEIOS_WATCH_TICKS];   // 26 KiB: not a kernel stack's
    static uint64_t submitsWas[64];
    uint64_t head = 0, blobsWas = droidvm_virtgpu_stats[2], secondAt = nowUs();
    // take20: CREATE_BLOB commands whose device leg took 8 ms or more (the transport's buckets <16384 us, <32768 us, more)
    auto slowBlobs = []() -> uint32_t {
        return droidvm_virtio_gpu_legs[1][0][9] + droidvm_virtio_gpu_legs[1][0][10] + droidvm_virtio_gpu_legs[1][0][11];
    };
    uint32_t slowWas = slowBlobs();
    uint32_t taken = gapHead;
    for (;;) {
        // every 100 ms while swaps come or a gap waits for its tick, every second otherwise
        const bool busy = nowUs() - lastSwapUs < 3000000 || taken != gapHead;
        IOSleep(busy ? 100 : 1000);
        oneios_watch_tick &t = ring[head % ONEIOS_WATCH_TICKS];
        memset(&t, 0, sizeof t);
        const uint64_t began = nowUs(), cpuBegan = thread_get_runtime_self();
        // PROC_NOWAITTRANS: without it proc_iterate waits for each process that is in an exec (proc_transwait), and
        // a tick was as late as the exec was long (on the PC's bench a walk took 8.9 s of which 1.4 ms on a CPU). The
        // visit reads the task's time sums and the name only; a process's task is its proc's own memory.
        proc_iterate(1 /* PROC_ALLPROCLIST */ | 4 /* PROC_NOWAITTRANS */, visit, &t, nullptr, nullptr);
        t.at_us = nowUs();
        uint64_t cpuNs = 0;
        absolutetime_to_nanoseconds(thread_get_runtime_self() - cpuBegan, &cpuNs);
        walks = walks + 1;
        walkCpuUs = walkCpuUs + cpuNs / 1000;
        walkUs = walkUs + (t.at_us - began);
        if (t.at_us - began > walkMostUs) walkMostUs = t.at_us - began;
        for (unsigned s = 0; s < 64; s++) {
            const uint64_t n = __atomic_load_n(&trail::slots[s].submits, __ATOMIC_RELAXED), made = n - submitsWas[s];
            submitsWas[s] = n;
            if (!made || !head) continue;   // the first tick's are since boot
            if (!strncmp(droidvm_virtgpu_client_memory[s].name, "backboardd", sizeof droidvm_virtgpu_client_memory[s].name)) t.rs_submits += uint32_t(made);
            else t.app_submits += uint32_t(made);
        }
        const uint64_t blobsNow = droidvm_virtgpu_stats[2];
        t.blobs = uint32_t(blobsNow - blobsWas);
        const uint32_t slowNow = slowBlobs();
        t.slow_blobs = slowNow - slowWas;
        slowWas = slowNow;
        blobsWas = blobsNow;
        head++;
        // the gaps whose end is a tick old: their lines (those overwritten in the ring meanwhile are only counted)
        for (uint32_t have = __atomic_load_n(&gapHead, __ATOMIC_ACQUIRE); taken != have;) {   // pairs with the swap path's release store
            if (have - taken > 8) { hitches += have - taken - 8; hitchesUnprinted += have - taken - 8; taken = have - 8; }
            const Gap g = gaps[taken % 8];
            if (t.at_us < g.at + 100000) break;
            taken++;
            hitch(ring, head, g, t.at_us);
        }
        // take20: a complete wake (watch.h's oneios-wake), once; the next one can begin after it
        if (__atomic_load_n(&wakes::active, __ATOMIC_ACQUIRE) && oneios_wake_complete(&wakes::wake, t.at_us)) {
            const oneios_wake &w = wakes::wake;
            char when[12], gapsText[80], legsText[80];
            clockText(when, sizeof when, t.at_us > w.at_us ? t.at_us - w.at_us : 0);
            const uint32_t legs = w.legs < ONEIOS_WAKE_FRAMES ? w.legs : ONEIOS_WAKE_FRAMES;
            oneios_wake_list(gapsText, sizeof gapsText, w.gap_us, w.gaps);
            oneios_wake_list(legsText, sizeof legsText, w.leg_us, legs);
            IOLog("oneios-wake: %s: after %llu.%llu s without a swap: the next swaps %s ms apart; the render server's next fenced frames "
                  "%s ms on the host\n", when, w.rest_us / 1000000, w.rest_us / 100000 % 10, gapsText, legsText);
            wakes::lines = wakes::lines + 1;
            __atomic_store_n(&wakes::active, 0, __ATOMIC_RELEASE);
        }
        if (t.at_us - secondAt >= 1000000) {
            second(t.at_us, swapCount);
            secondAt = t.at_us;
        }
    }
}

void start()
{
    uint32_t on = 1;
    PE_parse_boot_argn("oneios-watch", &on, sizeof on);
    PE_parse_boot_argn("oneios-frames", &framesOn, sizeof framesOn);
    PE_parse_boot_argn("oneios-hitch-ms", &leastMs, sizeof leastMs);
    PE_parse_boot_argn("oneios-gpu-room-mib", &roomMiB, sizeof roomMiB);
    if (!roomMiB) roomMiB = 2048;
    if (!on) { leastMs = 0; return; }
    thread_t thread = nullptr;
    if (kernel_thread_start(run, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
}  // namespace watch

extern "C" void oneios_gpu_capset_seen(uint32_t, uint64_t start, uint64_t size)
{
    using namespace watch;
    if (!size || deviceSize) return;
    deviceStart = start;
    deviceSize = size;        // last: the sampler takes the start once the size is set
}

extern "C" void oneios_gpu_swap_gap(uint64_t gap, uint64_t now)
{
    using namespace watch;
    const uint64_t before = beforeUs;
    beforeUs = gap;
    lastSwapUs = now;
    swapCount = swapCount + 1;
    if (framesOn) {
        // take20: oneios-frames (the 10 s report prints it) and oneios-wake (the sampler prints it)
        if (oneios_frames_is(gap, before, uint64_t(leastMs) * 1000)) {
            framesCount[oneios_frames_class(gap)] = framesCount[oneios_frames_class(gap)] + 1;
            const uint64_t f = framesHead;
            framesRing[f % ONEIOS_FRAMES_RING] = {gap, now};
            __atomic_store_n(&framesHead, f + 1, __ATOMIC_RELEASE);   // pairs with reportFrames' acquire load
        }
        oneios_wake &w = wakes::wake;
        if (__atomic_load_n(&wakes::active, __ATOMIC_ACQUIRE)) {
            if (w.gaps < ONEIOS_WAKE_FRAMES) {
                w.gap_us[w.gaps] = gap > 0xffffffffull ? 0xffffffffu : uint32_t(gap);
                w.gaps = w.gaps + 1;
            }
        } else if (oneios_wake_is(gap) && wakes::lines < 300) {
            memset(&w, 0, sizeof w);
            w.at_us = now;
            w.rest_us = gap;
            __atomic_store_n(&wakes::active, 1, __ATOMIC_RELEASE);   // the fence path's legs and the sampler from here on
        }
    }
    if (!oneios_hitch_is(gap, before, uint64_t(leastMs) * 1000)) return;
    const uint32_t h = gapHead;
    gaps[h % 8] = {gap, now, before};
    __atomic_store_n(&gapHead, h + 1, __ATOMIC_RELEASE);
}

namespace {
void report(void *, wait_result_t)
{
    uint32_t every = 10;
    PE_parse_boot_argn("oneios-gpu-report", &every, sizeof every);
    if (!every) return;
    uint64_t was[8] = {}, fences[3] = {}, hist[10] = {}, closed[2] = {}, live[4] = {}, askedBefore = 0;
    for (uint32_t round = 1;; round++) {
        IOSleep(every * 1000);
        {   // take17: the round's time of day (UTC) and the seconds since boot, for the phone's own logs, and what the
            // sampler (watch::run) cost in the round: its walks of the process list and the time they took
            static uint64_t walksWas, walkUsWas, walkCpuWas;
            char when[12];
            watch::clockText(when, sizeof when, 0);
            const uint64_t walks = watch::walks, us = watch::walkUs, most = watch::walkMostUs, cpu = watch::walkCpuUs;
            watch::walkMostUs = 0;
            IOLog("oneios-clock: %s UTC, up %llu s, round %u; the sampler: %llu walks, %llu us of CPU, %llu ms from start to end (the "
                  "longest %llu us)\n", when, watch::nowUs() / 1000000, round, walks - walksWas, cpu - walkCpuWas, (us - walkUsWas) / 1000,
                  most);
            walksWas = walks;
            walkUsWas = us;
            walkCpuWas = cpu;
        }
        const uint32_t measured = cpu::report(every, round);   // a stall's window is longer than the nominal one
        if (oneios_memory_report) oneios_memory_report(measured);
        if (oneios_disk_report) oneios_disk_report(measured);
        cpu::costs(measured);
        uint64_t now[8], rs[3], h[10], cl[3], lv[4];
        for (unsigned i = 0; i < 8; i++) now[i] = droidvm_virtgpu_stats[i];
        for (unsigned i = 0; i < 3; i++) rs[i] = droidvm_virtgpu_rs_fences[i];
        for (unsigned i = 0; i < 10; i++) h[i] = droidvm_virtgpu_rs_fence_hist[i];
        for (unsigned i = 0; i < 3; i++) cl[i] = droidvm_virtgpu_closes[i];
        for (unsigned i = 0; i < 4; i++) lv[i] = droidvm_virtgpu_live[i];
        bool work = rs[0] != fences[0], memory = memcmp(lv, live, sizeof lv) != 0 || cl[0] != closed[0];
        for (unsigned i = 1; i < 8; i++) work |= now[i] != was[i];
        if (work) {
            const uint64_t frames = rs[0] - fences[0];
            IOLog("oneios-gpu-report: %u s: submits +%llu (fenced +%llu); frames +%llu mean %llu us max ever %llu us; "
                  "<1/<2/<4/<8/<16/<32 ms +%llu/+%llu/+%llu/+%llu/+%llu/+%llu longer +%llu; fence timeouts +%llu; refusals +%llu (op %llu %#llx)\n",
                  every, now[4] - was[4], now[5] - was[5], frames, frames ? (rs[1] - fences[1]) / frames : 0, rs[2],
                  h[0] - hist[0], h[1] - hist[1], h[2] - hist[2], h[3] - hist[3], h[4] - hist[4], h[5] - hist[5],
                  (h[6] + h[7] + h[8] + h[9]) - (hist[6] + hist[7] + hist[8] + hist[9]), now[6] - was[6], now[7] - was[7],
                  droidvm_virtgpu_last_refusal[0], droidvm_virtgpu_last_refusal[1]);
        }
        if (work || memory) {
            IOLog("oneios-gpu-memory: blobs +%llu closed +%llu (deferred +%llu, waiting %llu); live %llu: own %llu (%llu MiB), "
                  "wired %llu (%llu MiB); contexts +%llu clients %llu\n",
                  now[2] - was[2], cl[0] - closed[0], cl[1] - closed[1], cl[2], lv[2] + lv[3], lv[2], lv[0] >> 20, lv[3], lv[1] >> 20,
                  now[1] - was[1], now[0]);
        }
        if (memory) {
            // The clients holding GPU memory, own+wired MiB, in lines of at most ~200 bytes.
            char text[200];
            size_t at = 0;
            for (unsigned i = 0; i < 64; i++) {
                const DroidVMClientMemory &c = droidvm_virtgpu_client_memory[i];
                if (!c.name[0] || !(c.pageBytes + c.userBytes)) continue;
                char one[64];
                const int n = snprintf(one, sizeof one, "%.16s %llu+%llu MiB", c.name, c.pageBytes >> 20, c.userBytes >> 20);
                if (at && at + 2 + size_t(n) >= sizeof text) {
                    IOLog("oneios-gpu-clients: %s\n", text);
                    at = 0;
                }
                at += snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", one);
            }
            IOLog("oneios-gpu-clients: %s\n", at ? text : "none");
            // Each client's live blobs by size: metal2vulkan's pooled blocks are 64 MiB buffers ("64+"), windows and
            // textures the small sizes, so growth in "64+" is the runtime's pools and growth below is resources.
            for (unsigned i = 0; i < 64; i++) {
                const DroidVMClientMemory &c = droidvm_virtgpu_client_memory[i];
                if (!c.name[0] || !(c.pageBytes + c.userBytes)) continue;
                IOLog("oneios-gpu-sizes: %.16s <1 %u, <2 %u, <4 %u, <8 %u, <16 %u, <32 %u, <64 %u, 64+ %u MiB blobs\n", c.name,
                      c.sizes[0], c.sizes[1], c.sizes[2], c.sizes[3], c.sizes[4], c.sizes[5], c.sizes[6], c.sizes[7]);
            }
            // The kinds and ages of the blobs of each client that made or let go of one in the interval, or still has
            // closed ones held (blobs:: above).
            static uint64_t wasMade[64], wasGone[64];
            blobs::print(wasMade, wasGone);
            // Each client's blobs against the GPU address range its capset named, from a tenth of it on (watch:: above).
            watch::reportRooms();
        }
        // Where the clients' IOSurface drawing goes (trail:: above): only what changed since the last report.
        trail::print(every);
        // take20: the round's stutters under the hitch lines, with the time of the longest (watch:: above, watch.h).
        watch::reportFrames(every);
        // The stages of the render server's submissions, the transport's waits and the swaps' pace (stages:: above).
        stages::report(every);
        // Who asked for a Metal device (read MetalPluginName), when that changed: with the clients line, a process that
        // asked and holds no GPU memory had its device refused or made none (its reason: the oneios-gpu-text lines).
        uint64_t asked = 0;
        for (const NativeAdrenoAskerRecord &a : native_adreno_metal_askers) asked = asked * 31 + a.reads + uint8_t(a.name[0]);
        if (asked != askedBefore) {
            askedBefore = asked;
            char text[200];
            size_t at = 0;
            for (const NativeAdrenoAskerRecord &a : native_adreno_metal_askers) {
                if (!a.name[0]) continue;
                char one[40];
                const int n = snprintf(one, sizeof one, "%.16s %u", a.name, a.reads);
                if (at && at + 2 + size_t(n) >= sizeof text) {
                    IOLog("oneios-gpu-askers: %s\n", text);
                    at = 0;
                }
                at += snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", one);
            }
            if (at) IOLog("oneios-gpu-askers: %s\n", text);
        }
        if (hosted_gpu_text_flush) hosted_gpu_text_flush(1000000000ull);
        memcpy(was, now, sizeof was);
        memcpy(fences, rs, sizeof fences);
        memcpy(hist, h, sizeof hist);
        memcpy(closed, cl, sizeof closed);
        memcpy(live, lv, sizeof live);
    }
}
}  // namespace

void oneios_adreno_start(void)
{
    if (oneios_adreno_stage) return;
    if (!oneios_adreno_wanted()) {
        oneios_adreno_stage = 1;
        IOLog("oneios-adreno: no Metal accelerator (gpu state %u; oneios-gpu-metal=1 publishes it anyway)\n", oneios_gpu3d_state);
        // The report all the same (2026-11-20): its oneios-cpu lines say which processes have the CPUs, which matters most
        // where iOS draws with them (the native kernel on QEMU, a host with no 3D); its GPU lines stay quiet without work.
        thread_t reporter = nullptr;
        if (kernel_thread_start(report, nullptr, &reporter) == KERN_SUCCESS) thread_deallocate(reporter);
        watch::start();   // take17: the event lines (a hitch needs only the display's swaps)
        return;
    }
    oneios_adreno_stage = 2;
    sheng_gpu_power_probe();
    uint32_t test = 0;
    PE_parse_boot_argn("oneios-adreno-selftest", &test, sizeof test);
    thread_t thread = nullptr;
    if (test && kernel_thread_start(selftest, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    thread_t reporter = nullptr;
    if (kernel_thread_start(report, nullptr, &reporter) == KERN_SUCCESS) thread_deallocate(reporter);
    watch::start();   // take17: the event lines (watch:: above)
}

// ---- take20: the render server's read wait and the writers it gave up on (staged/gpu.rs GPU3D_READ_WAIT)
// take19's second session (-loggpu): for 2.5 minutes after the notification panel closed, every frame of backboardd waited the
// whole 100 ms (droidvm_surface_read_wait_ms) for a writer whose ticket never ended, and gave up (oneios-gpu-sync `reads that
// waited for writers +97, gave up +97`): the Home screen drew 10 frames a second while the finger moved. DroidVMVirtioGpu3D.cpp's
// awaitWriters now, once a wait gave up, marks those writers' uses not written (staleWriters), so no read waits for them again;
// their tickets end as before when their command buffers complete. A writer is said once (the first 64 of a boot):
//   oneios-gpu-sync: WidgetRenderer_[pid]'s GPU write of surface 0x2f (ticket 9123) never ended: the render server waited
//     100 ms for it and gave up; no longer waited for
// and counted on a sync line of its own (`oneios-gpu-sync: 10 s: writers the render server's read wait gave up on, no longer waited
// for: +N (M since the boot)`). CPU locks still wait for such a writer. oneios-read-wait-stale=0: as before (every read waits).
extern "C" {
volatile uint64_t oneios_gpu_read_wait_stale_count;
extern volatile uint32_t droidvm_surface_read_wait_ms;   // DroidVMVirtioGpu3D.cpp: the read wait's most (100 ms)
void proc_name(int pid, char *buf, int size);            // sys/proc.h
}

extern "C" bool oneios_gpu_read_wait_stale_on(void)
{
    static int on = -1;
    if (on < 0) {
        uint32_t value = 1;
        PE_parse_boot_argn("oneios-read-wait-stale", &value, sizeof value);
        on = value ? 1 : 0;
    }
    return on == 1;
}

// Under the tunnel's surface lock (an IOLock: IOLog is allowed), from the render server's submitting thread.
extern "C" void oneios_gpu_read_wait_stale(uint32_t surface, int pid, uint64_t ticket)
{
    const uint64_t n = oneios_gpu_read_wait_stale_count;
    oneios_gpu_read_wait_stale_count = n + 1;
    if (n >= 64) return;
    char name[24] = {};
    proc_name(pid, name, sizeof name);
    IOLog("oneios-gpu-sync: %.20s[%d]'s GPU write of surface 0x%x (ticket %llu) never ended: the render server waited %u ms for it and "
          "gave up; no longer waited for\n", name[0] ? name : "?", pid, surface, ticket, droidvm_surface_read_wait_ms);
}
