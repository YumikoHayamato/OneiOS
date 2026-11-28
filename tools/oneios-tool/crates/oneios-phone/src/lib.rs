//! oneios-tool phone: the phone on adb (docs/building.md section 6). Status, switch, return, stage, sums, the
//! Data store's copy, and the session's own commands (`session.rs`: the display helper's restart, the touch IC, the
//! panel's level). The rules every command keeps are in `adb.rs`.
//!
//! **Not run on the phone yet** (no phone on the box it was written on): tested against a stand-in phone,
//! `tests/phone/fake-adb.sh`, which runs the fork's own root script under its test root.

pub mod adb;
pub mod app;
pub mod cpu;
pub mod cycle;
pub mod droidvm;
pub mod hostshare;
pub mod hugepool;
pub mod take23;
pub mod hw;
pub mod local;
pub mod log;
pub mod phone;
pub mod pull;
pub mod session;
pub mod sparse;
pub mod udmabuf;
pub mod haltpoll;
pub mod vmboot;
pub mod vmperf;
pub mod vmprobe;

use adb::Adb;

#[derive(clap::Args)]
pub struct Cli {
    /// The phone's adb serial (or ANDROID_SERIAL); needed only when several are attached
    #[arg(long, short = 's', global = true)]
    serial: Option<String>,
    #[command(subcommand)]
    command: Command,
}

#[derive(clap::Subcommand)]
pub enum Command {
    /// Root, the installed app, the session, and what `oneios_root.sh status` says (read only)
    Status {
        /// Again every SECONDS (default 5) until Ctrl-C
        #[arg(long, value_name = "SECONDS", num_args = 0..=1, default_missing_value = "5")]
        watch: Option<u64>,
    },
    /// The session's logs by name: print, filter, follow live, save (runs beside a session: read only)
    Log(LogArgs),
    /// Start a session: `oneios_root.sh switch <the app's versionCode> 1`
    Switch {
        /// Instead of the installed app's versionCode
        #[arg(long)]
        version_code: Option<u64>,
    },
    /// End the session (session/return.request) and wait for One UI's restore line
    Return {
        /// Seconds to wait for the restore line
        #[arg(long, default_value_t = 180)]
        timeout: u64,
        /// The line written into return.request (the return logs it)
        #[arg(long, default_value = "oneios-tool: return")]
        note: String,
    },
    /// Only during a session: end the display helper (control/display.pid: SIGTERM, then SIGKILL) and start it again
    /// with the switch script's own command; waits for its first frame in control/display.log. xnuhost is not touched
    DisplayRestart {
        /// First put this liboneios-display.so into the staged runtime (runtime/bin/), its SHA-256 checked on the phone
        #[arg(long, value_name = "PATH")]
        lib: Option<String>,
        /// First put this oneios-display.dex into the staged runtime (runtime/lib/), its SHA-256 checked on the phone
        #[arg(long, value_name = "PATH")]
        dex: Option<String>,
        /// Seconds to wait for the new helper's `first frame posted`
        #[arg(long, value_name = "SECONDS", default_value_t = 30)]
        timeout: u64,
        /// Seconds between the old helper's SIGTERM and its SIGKILL
        #[arg(long, value_name = "SECONDS", default_value_t = 3)]
        kill_after: u64,
    },
    /// Only during a session: the touch IC's power state (/sys/class/sec/tsp/enabled)
    Touch {
        #[command(subcommand)]
        command: TouchCommand,
    },
    /// Where iOS's CPU work can run, read only: each CPU's core, frequency domain, top and capped clocks, governor and
    /// boost; Android's cpusets; in a session xnuhost's threads (CPU, nice, cpuset, uclamp) and the iOS threads by CPU
    Cpu {
        /// Also record a Perfetto trace (sched, freq, idle) of SECONDS and copy it here
        #[arg(long, value_name = "SECONDS")]
        trace: Option<u64>,
        /// Where the trace goes (default oneios-cpu.pftrace)
        #[arg(long, value_name = "FILE")]
        out: Option<String>,
    },
    /// The hardware as the phone's kernel was given it, read only, for the native path: the device tree
    /// (/sys/firmware/fdt), /proc's cmdline, cpuinfo, meminfo, iomem, partitions, interrupts, the CPUs' MIDR, the
    /// panel's connector and the partitions' names, into DIR. Named files only; refused during a session
    Hw {
        /// The folder for the files (fdt.dtb, iomem.txt, ..., hw.txt)
        #[arg(long, value_name = "DIR", default_value = "phone-hw")]
        out: String,
    },
    /// The kernel's log (root's dmesg), read only: the last N lines, or the last N that match a REGEX
    Dmesg {
        /// Print only lines matching REGEX (repeatable: any of them)
        #[arg(long, value_name = "REGEX")]
        grep: Vec<String>,
        /// How many lines, from the end
        #[arg(long, value_name = "N", default_value_t = 200)]
        tail: usize,
        /// The boot before this one: the log the phone kept through its restart (pstore, by name)
        #[arg(long)]
        previous: bool,
    },
    /// How much free memory comes in 2 MiB blocks (buddyinfo), which a VM's RAM needs; with --free, first a sync, the
    /// file cache dropped and one compaction (one-shot: nothing is lost, no setting changes). No VM. Refused during a
    /// session
    Hugeblocks {
        /// Drop the file cache and compact first, then read again
        #[arg(long)]
        free: bool,
    },
    /// Droid-VM's gh_hugepage_reserve (scripts/hugepage/build.sh), a pool of 2 MiB pages for a VM's RAM: status, load
    /// beside One UI, grow with One UI's framework stopped and started again (no restart of the phone), unload
    Hugepool {
        #[command(subcommand)]
        command: HugepoolCommand,
    },
    /// Droid-VM's gunyah_host_share (scripts/hugepage/build-share.sh): /dev/gunyah_share, through which Droid-VM's
    /// crosvm SHAREs a VM's RAM with it (the shared-RAM VM, vm-probe --shared-ram): status, load after a preflight of
    /// the phone's kernel, unload
    Hostshare {
        #[command(subcommand)]
        command: HostshareCommand,
    },
    /// Droid-VM's udmabuf module (scripts/hugepage/build-udmabuf.sh): large GPU buffers from a fragmented phone (kvmalloc in
    /// place of the built-in's kmalloc_array): status, load, unload
    Udmabuf {
        #[command(subcommand)]
        command: UdmabufCommand,
    },
    /// Droid-VM's gunyah_halt_poll module (their prebuilt of DroidVM-Prebuilt-Root f7c6ce8, SHA-256 pinned): a vCPU waits a moment for its wakeup
    /// before its thread sleeps, so a completion skips the host's interrupt and scheduler round trip: status, load, unload
    Haltpoll {
        #[command(subcommand)]
        command: HaltpollCommand,
    },
    /// Droid-VM's own crosvm (scripts/droidvm/fetch-crosvm.sh) pushed to the phone with its libraries, each checked by
    /// SHA-256 there, then the lines of its `crosvm run --help` that match REGEX
    DroidvmCrosvm {
        /// The package's folder on the PC
        #[arg(long, value_name = "DIR", default_value = "/mnt/tkg-build/oneios/dl/droidvm-prebuilt/82c59c2")]
        dir: String,
        /// What to show of its options
        #[arg(long, value_name = "REGEX", default_value = "pseudo-unprotected|--gpu |--pre-alloc|virtio-mmio")]
        grep: String,
        /// The host renderer crosvm loads: `prebuilt` (the package's own), or a libvirglrenderer.so to push in its place
        /// (scripts/droidvm/build-virgl.sh). crosvm links it, so a renderer the phone cannot load fails here
        #[arg(long, value_name = "FILE|prebuilt", default_value = "prebuilt")]
        virgl: String,
        /// crosvm itself: `prebuilt` (the package's own), or a crosvm to push in its place (take18b: Droid-VM/crosvm
        /// wip/hackintosh built from source, artifacts/crosvm); its own links are checked by the same `run --help`
        #[arg(long, value_name = "FILE|prebuilt", default_value = "prebuilt")]
        crosvm: String,
    },
    /// The native kernel (an Image of runtime/loader's xnuloader-crosvm) booted in a protected VM of AVF's crosvm under
    /// Gunyah, its console followed live; the VM's RAM and vCPUs from the Image's label. --stop ends a running one.
    /// Refused during a session
    VmBoot {
        /// The Image (xnuloader-crosvm KERNEL OUT)
        #[arg(long, value_name = "FILE", default_value = "out/crosvm/kernel.img")]
        image: String,
        /// Seconds before crosvm stop
        #[arg(long, value_name = "SECONDS", default_value_t = 120)]
        seconds: u32,
        /// 2 MiB folios for the VM's RAM for the run (needs gh_hugepage_reserve's pool above ~0.5 GB)
        #[arg(long)]
        thp: bool,
        /// Where serial.txt, crosvm.log, dmesg.txt, run.txt and fdt.dtb go
        #[arg(long, value_name = "DIR", default_value = "phone-vmboot")]
        out: String,
        /// Stop the running VM now (crosvm stop through its socket) instead
        #[arg(long)]
        stop: bool,
        /// The screen's bridge, for an Image made with --display (runtime/display: make)
        #[arg(long, value_name = "FILE", default_value = "runtime/display/build/android/displayctl")]
        bridge: String,
        /// A test of the touch without a finger: the forwarder sends a swipe up 200 s after crosvm started, then every 60 s
        #[arg(long)]
        swipe_test: bool,
        /// vCPU N on the phone's core N (crosvm --cpu-affinity)
        #[arg(long)]
        pin: bool,
        /// Droid-VM's crosvm package (scripts/droidvm/fetch-crosvm.sh), for an Image of xnuloader-crosvm --shared-ram: their
        /// hackintosh line's by default, whose crosvm takes the kernel's scanout (`fetch-crosvm.sh DIR hackintosh`)
        #[arg(long, value_name = "DIR", default_value = "/mnt/tkg-build/oneios/dl/droidvm-prebuilt/f7c6ce8")]
        droidvm: String,
        /// The GPU's lowest clock while an Image with the GPU runs (MHz; the level at or above it), put back at the end.
        /// Default 0: the governor scales freely (saves power; the stutter was the panel, not the clock). Try `607` to A/B.
        #[arg(long, value_name = "MHZ", default_value_t = vmboot::GPU_FLOOR_MHZ)]
        gpu_floor: u32,
        /// The CPUs' lowest clocks while the VM runs, percent of each cluster's top clock (Android's cap stays), put back at
        /// the end. It comes 30 s after crosvm starts: until then the clocks to put back are read (the lowest seen).
        /// Default 0: the governor scales freely (saves power). Try `60` to A/B.
        #[arg(long, value_name = "PERCENT", default_value_t = vmboot::CPU_FLOOR_PERCENT)]
        cpu_floor: u32,
        /// How long the GPU stays awake after its last work while the VM runs (KGSL's idle_timer, ms; Android's is 80), put back
        /// at the end. Default 0: left alone. Apart from the clocks: try `400` alone, or with --touch-boost, to A/B
        #[arg(long, value_name = "MS", default_value_t = 0)]
        gpu_idle_timer: u32,
        /// While fingers move (and 1.5 s after), the GPU's lowest clock is 607 MHz and the CPU clusters' 40% of their top; put
        /// back by the touch forwarder, or by the script when that was killed. Not with --gpu-floor or --cpu-floor
        #[arg(long)]
        touch_boost: bool,
        /// One UI's framework stopped while the VM runs (as hugepool grow stops it: Samsung's watchdog fed, the way back armed
        /// first) and started again when the VM ends, however it ends; the CPUs' caps lifted meanwhile
        #[arg(long)]
        takeover: bool,
        /// With --takeover: Samsung's GPU max cap (/sys/kernel/gpu/gpu_max_clock, KGSL's max_gpuclk: One UI's SSRM sets 500 to
        /// 832 MHz when warm, and with One UI down nothing moves it) lifted to the GPU's top while the VM runs and written back at
        /// the end, as the cpufreq_limit is. The default with --takeover since take18 (take17's whole session ran at a cap of 607
        /// MHz, the lock screen's frames GPU-bound at it); this flag is kept for old command lines
        #[arg(long)]
        gpu_cap_lift: bool,
        /// With --takeover: leave Samsung's GPU max cap as One UI left it (the way before take18; an A/B of the cap)
        #[arg(long)]
        keep_gpu_cap: bool,
        /// With --takeover: leave the media volume as it is (default: set to its maximum before One UI stops)
        #[arg(long)]
        no_max_volume: bool,
        /// With --pin: vCPU i on core N+i, the cores below N left to the host (crosvm's device threads, surfaceflinger)
        #[arg(long, value_name = "N", default_value_t = 0)]
        pin_first: u32,
        /// thermal.txt: once a second the CPU clusters', the GPU's, the board's and the battery's temperatures, the clocks and
        /// the GPU's load, USB's and the battery's current and voltage, the CPUs online and the cooling devices' states, while
        /// the VM runs (read only)
        #[arg(long)]
        thermal_log: bool,
        /// Each touch frame goes to the VM with the time the touchscreen's driver stamped it with (displayctl touch
        /// --scan-time): iOS then gets every touch at the time the finger was there, not 0 to 4 ms later (the guest polls).
        /// touch.txt says how many frames went with it; the guest's `oneios-hid: touch latency:` line whether it took them
        #[arg(long)]
        touch_time: bool,
        /// The VM's refresh tick on the panel's own vsync (runtime/pace's pacectl asks surfaceflinger and writes its grid into
        /// the VM's frame sequence page; the kernel's tick follows it: display/vsync_lock.h). For a shared-RAM Image with the
        /// screen. Without the kernel's part nothing changes; pace.txt says what the kernel did
        #[arg(long)]
        vsync_lock: bool,
        /// With --vsync-lock: the kernel's tick this many microseconds after surfaceflinger's deadline for a frame (negative:
        /// before). While the VM runs, the number in the VM folder's vsync.phase replaces it
        #[arg(long, value_name = "US", default_value_t = 0, allow_hyphen_values = true)]
        vsync_phase_us: i32,
        /// pacectl for the phone (runtime/pace: make), pushed with --vsync-lock
        #[arg(long, value_name = "FILE", default_value = "runtime/pace/build/android/pacectl")]
        pacectl: String,
        /// perf.txt: what the host knows of a frame's road (read only): the GPU's limits and its time at each clock, every
        /// thread of crosvm (its CPUs, class, CPU time and wait for a CPU) 25 s in, each minute and at the stop,
        /// surfaceflinger's vsync model and its TimeStats of the VM's layer, and logcat's lines of crosvm's display client and
        /// drm2kgsl (dropped flips, KGSL_DIAG)
        #[arg(long)]
        perf_log: bool,
        /// crosvm's device workers (touch, disks, network, sound: every thread but the vCPUs') on the process's CPUs. With
        /// --pin this prebuilt holds each to the core of the vCPU that started its device (fixed in Droid-VM's crosvm after
        /// it); each one moved is a line of perf.txt with the CPUs it had
        #[arg(long)]
        unpin_workers: bool,
        /// With an Image that has the sound card (xnuloader-crosvm --audio): crosvm's AAudio backend in its own process under
        /// this uid (crosvm's uid=; Droid-VM's crosvm: a stream of uid 0 is silenced while One UI runs). Default: in crosvm's
        #[arg(long, value_name = "UID")]
        audio_uid: Option<u32>,
        /// With an Image that has the sound card and the microphone on (boot argument oneios-audio-mic=1): crosvm's card
        /// with `capture=true` in place of `capture=false`. Droid-VM's crosvm says "capture=(false,true) - Disable/enable audio
        /// capture. Default is false"; with `capture=false` its AAudio backend still opened the phone's input at the guest's
        /// PREPARE (take16: crosvm.log), so what the key changes there is not known: for a session whose microphone gives
        /// silence (`ONEIOS AUDIO: mic self-test ... largest sample 0`). Not run on the phone
        #[arg(long)]
        mic_capture: bool,
        /// With an Image made with xnuloader-crosvm --media: crosvm's virtio-media decoder (`--virtio-media
        /// kind=decoder,uid=UID`, the phone's MediaCodec in a helper process), so that iOS's VideoToolbox decodes H.264 and
        /// HEVC on the phone's hardware (the kernel's codec archive). Without it such an Image still gets the decoder's pool
        /// (its layout has it) and no device: iOS decodes in software, as before. Not run on the phone
        #[arg(long)]
        video_decoder: bool,
        /// The uid of crosvm's decoder helper (crosvm's uid=, which its virtio-media needs)
        #[arg(long, value_name = "UID", default_value_t = vmboot::VIDEO_DECODER_UID)]
        video_decoder_uid: u32,
        /// The Wi-Fi's bridge for the phone, for an Image made with --wifi, used in a takeover (runtime/bridges: make)
        #[arg(long, value_name = "FILE", default_value = "runtime/bridges/build/android/wifictl")]
        wifictl: String,
        /// The bridge channel's phone end, for an Image made with xnuloader-crosvm --bridge (runtime/bridges: make): the phone's
        /// battery to iOS and iOS's haptics to the phone's motor, over crosvm's second serial port (`bridgectl serve`)
        #[arg(long, value_name = "FILE", default_value = "runtime/bridges/build/android/bridgectl")]
        bridgectl: String,
        /// With such an Image: the battery alone, no haptics (`bridgectl serve --no-haptics`: iOS then has no actuator, as before)
        #[arg(long)]
        no_haptics: bool,
        /// The host renderer crosvm loads (its GPU's drm2kgsl, RUNPATH $ORIGIN): take18's build of Droid-VM/virglrenderer
        /// 2529a9f (scripts/droidvm/build-virgl.sh: 32 GPU address slices of 4-6 GiB instead of 64 of 2, the Adreno 830's
        /// 82-IB submit limit, the fault count, freed BOs unbound, KGSL timestamps, the power floors), or `prebuilt`: the
        /// package's own (virglrenderer cd71ea4 of 2026-09-01, take17's). Pushed in place of the package's file
        #[arg(long, value_name = "FILE|prebuilt", default_value = droidvm::VIRGL_TAKE18)]
        virgl: String,
        /// crosvm itself (take18b): `prebuilt`, the package's own (DroidVM-Prebuilt-Root f7c6ce8, the default), or a crosvm
        /// built from source to push in its place (Droid-VM/crosvm wip/hackintosh da647254c + samfor12 e7c1316:
        /// /mnt/giochi/Linux/oneios/take18/upstream/artifacts/crosvm). Opt-in: never run on the phone
        #[arg(long, value_name = "FILE|prebuilt", default_value = "prebuilt")]
        crosvm: String,
        /// With take18's renderer: its GPU power floors. `deadline` (take19's default): the compositor's deadline queue at
        /// the GPU's top level and the vCPUs' uclamp.min 1024 while it animates, without the process-wide rate floor
        /// (crosvm gets NCTX_PWR_CONSTRAINT=0; take18 had the GPU at 1200 MHz in 72 % of its samples, 31 % of the session
        /// from that floor alone). `on`: all three, as take18's session. `off`: none (NCTX_PWR_CONSTRAINT=0
        /// DRM2KGSL_PWR_CONSTRAINT=0 DRM2KGSL_VCPU_UCLAMP_MIN=0: the governors alone, as take17). The prebuilt renderer
        /// has none of them
        #[arg(long, value_name = "deadline|on|off", default_value = "deadline", value_parser = ["deadline", "on", "off"])]
        gpu_host_floors: String,
        /// NAME=VALUE in crosvm's environment, repeatable: the host renderer's A/B switches (NCTX_*, DRM2KGSL_*, e.g.
        /// DRM2KGSL_PRIORITIES=2, DRM2KGSL_NONCOHERENT=2; all off unless given). NAME of A-Z 0-9 _, VALUE of A-Z a-z 0-9 _ . : / -
        #[arg(long, value_name = "NAME=VALUE")]
        crosvm_env: Vec<String>,
        /// With --takeover: no zram compaction after One UI's stop (take18 compacts each zram device after the cache drop,
        /// before the pool's top-up, and says what MemAvailable did: upstream OneiOS 2894806's takeover-reclaim)
        #[arg(long)]
        no_takeover_reclaim: bool,
        /// With --takeover: vm.swappiness for the session (0-200), One UI's value written back at the end however it ends
        /// (upstream's takeover-swappiness). Default: kept
        #[arg(long, value_name = "N")]
        takeover_swappiness: Option<u32>,
        /// With --takeover: no KEYCODE_WAKEUP right before One UI's stop (take18 sends one, from YMGPwcca's bounded
        /// lock-screen wake: One UI's keyguard darkens the panel seconds after the last wake, and with the framework
        /// down nothing wakes it; the display helper then holds the panel on itself)
        #[arg(long)]
        no_takeover_wake: bool,
        /// take19, with --takeover, --touch-boost and the GPU's scanout: the panel's rate free from HZ up to its top (the S25
        /// Ultra's panel is LTPO, 1 to 120 Hz): surfaceflinger lowers it when the VM's screen is still and raises it for the
        /// layer's 120 Hz vote when frames come; it is held at the top while a touch is recent (displayctl touch's boost.state:
        /// the last touch under 1.5 s ago), since with One UI down nothing tells surfaceflinger of a touch. 0 (the default):
        /// held at the top the whole session, as take13-take18. display.txt: `the panel's rate is free from N to 120 Hz` and,
        /// at the end, how often and how long it was held
        #[arg(long, value_name = "HZ", default_value_t = 0)]
        panel_rate_min: u32,
        /// take23, with --takeover and a shared-RAM Image: the new boot order. The memory rule (--host-keep-mib left to
        /// Android beside the VM) is asked AFTER One UI's stop and the cache drop, when the pool is filled, not before the
        /// stop (where it refused an Image that fits once One UI is down). run.txt's `pick:` lines say what was found
        #[arg(long)]
        fill_after_stop: bool,
        /// take23, repeatable; implies --fill-after-stop: Images of the same build with more RAM than --image. After One
        /// UI's stop the VM runs the largest that fits (the module's table, --host-keep-mib, the pool's pages coming),
        /// --image when none of them does. Each is pushed (146 MiB a file)
        #[arg(long, value_name = "FILE")]
        image_larger: Vec<String>,
        /// take23, with --fill-after-stop: what Android keeps available beside the VM, MiB (the 1024 of the old rule; what
        /// it really needs while the VM runs is in hostmem.txt from take23 on)
        #[arg(long, value_name = "MIB", default_value_t = take23::KEEP_MIB)]
        host_keep_mib: u32,
        /// take23, with --takeover and take18's renderer: the clocks' orchestrator. The GPU's cap follows its load from 607 MHz
        /// up (the renderer's floor held it at 1200 MHz for 67 % of take22b at 16 % busy) and the vCPUs' clamp follows the
        /// guest's swaps on time (1024 down to 512). orch.txt; leave the flag out for the clocks as in take22
        #[arg(long)]
        orchestrator: bool,
        /// take23, repeatable, with --orchestrator: KEY=VALUE of gpu-floor (MHz), gpu-up, gpu-down (% busy), gpu-hold (looks),
        /// clamp-floor, clamp-step, good, bad (% of swaps on time), hot (C)
        #[arg(long, value_name = "KEY=VALUE")]
        orchestrator_set: Vec<String>,
    },
    /// One run of runtime/vmprobe's payload as a protected VM under Gunyah with AVF's crosvm: 1 vCPU, no device but
    /// the serial port; it powers off by itself, else crosvm stop after SECONDS. Refused during a session
    VmProbe {
        /// The payload (runtime/vmprobe/build.sh)
        #[arg(long, value_name = "FILE", default_value = "out/vmprobe/vmprobe.bin")]
        payload: String,
        /// The VM's RAM in MiB
        #[arg(long, value_name = "MIB", default_value_t = 128)]
        mem: u32,
        /// Seconds before crosvm stop, and again before the last resort (kill)
        #[arg(long, value_name = "SECONDS", default_value_t = 20)]
        timeout: u32,
        /// The folder for serial.txt, crosvm.log and run.txt
        #[arg(long, value_name = "DIR", default_value = "phone-vmprobe")]
        out: String,
        /// During the run only: 2 MiB folios for the VM's RAM (/sys/kernel/mm/transparent_hugepage: shmem_enabled
        /// advise, defrag madvise); the old values are written back after it
        #[arg(long)]
        thp: bool,
        /// Run Droid-VM's crosvm instead of AVF's: its package's folder on the PC (scripts/droidvm/fetch-crosvm.sh),
        /// pushed and checked first
        #[arg(long, value_name = "DIR")]
        droidvm: Option<String>,
        /// Droid-VM's shared-RAM VM (--protected-vm-pseudo-unprotected, no swiotlb); needs --droidvm and
        /// `phone hostshare load`
        #[arg(long)]
        shared_ram: bool,
        /// With --thp: shmem_enabled for the run, advise or within_size (the shared-RAM VM's window is pinned
        /// through a mapping crosvm does not madvise)
        #[arg(long, value_name = "MODE", default_value = "advise", value_parser = clap::builder::PossibleValuesParser::new(vmprobe::THP_SHMEM))]
        thp_shmem: String,
    },
    /// Only during a session: the panel's level (/sys/class/backlight/panel0-backlight/brightness)
    Backlight {
        #[command(subcommand)]
        command: BacklightCommand,
    },
    /// Stage files as the app does (scripts/phone/stage.sh): runtime DIR, runtime-asis DIR, image DIR
    Stage {
        /// runtime, runtime-asis or image
        #[arg(value_parser = ["runtime", "runtime-asis", "image"])]
        kind: String,
        /// An unpacked runtime package, or files of an image set at their paths
        dir: String,
    },
    /// The sha256 of the staged kernel.kc, its hash list, manifest.json, data.seed's line, xnuhost.conf and the runtime
    Sums,
    /// Check that the staged manifest.json lists the staged files (image.sha256), as xnuhost will; `switch` runs it first (read only)
    ImageCheck,
    /// The phone's manifest.json with the entries of DIR's files (kernel.kc, kernel.kc.hashes, ... at their paths in the
    /// set) set to theirs; every other entry must list the staged file. Printed, written to --out, or staged with --stage
    Manifest {
        /// Files of the image set at their paths (DIR's own manifest.json is ignored)
        dir: String,
        /// Write the manifest to FILE
        #[arg(long)]
        out: Option<String>,
        /// Stage DIR's files and the manifest together (phone stage image), then check the set
        #[arg(long)]
        stage: bool,
    },
    /// Copy the Data store (data.img) from the phone in chunks, each checked against the phone's sha256, zero runs
    /// as holes; a copy that stopped goes on where it stopped (OUT.part, OUT.part.progress); no session may be open
    PullData {
        out: String,
        /// vm-boot's store (/data/local/tmp/oneios-vmboot/data.img: iOS in the VM) in place of the hosted session's
        #[arg(long)]
        vm: bool,
        /// A chunk's size in MiB: one adb stream each (default 256; a copy that goes on keeps the size it began with)
        #[arg(long, value_name = "MIB")]
        chunk: Option<u64>,
        /// How many times a chunk whose stream broke or whose sum differed is copied again before the command gives up
        #[arg(long, default_value_t = 5)]
        retries: u32,
        /// Seconds before the first retry of a chunk; doubled for each further one, at most 30
        #[arg(long, value_name = "SECONDS", default_value_t = 2)]
        backoff: u64,
        /// End a stream that gave no byte for this many seconds, and try the chunk again (0: never)
        #[arg(long, value_name = "SECONDS", default_value_t = 120)]
        stall: u64,
        /// When the phone's sha256 of a chunk is asked: before the chunk is copied (a chunk of zeros is then not
        /// copied at all), or for every chunk after the last one is copied
        #[arg(long, value_enum, default_value = "chunk")]
        verify: pull::Verify,
        /// Throw away OUT.part and its progress and begin again
        #[arg(long)]
        restart: bool,
        /// Then `ioslog boots`, `ioslog checks` and `ioslog health` on the copy (the health report: ioslog-health.txt beside OUT)
        #[arg(long)]
        ioslog: bool,
    },
    /// stdin to OUT with zero runs as holes (no phone needed)
    SparseWrite {
        out: String,
        /// stdin is gzip
        #[arg(long)]
        gunzip: bool,
        /// Fail unless this many bytes came
        #[arg(long)]
        expect_size: Option<u64>,
    },
}

#[derive(clap::Subcommand)]
pub enum TouchCommand {
    /// Read the node: 1 is on, 0 is off (the IC sleeps after iOS's brightness was 0)
    Status,
    /// Write `2,0` then `2,1` (what One UI's input HAL writes when the panel comes on), then read the node back
    Wake,
}

#[derive(clap::Subcommand)]
pub enum HugepoolCommand {
    /// The module's state: refill_stat, pool_size_max, vm_owners, shmem_enabled, its kernel lines (read only)
    Status,
    /// The ABI preflight (kapi_check on the phone's BTF, on the PC), the .ko pushed and checked, a sync, then insmod with
    /// pool_want=N beside One UI (the CMA side off). Refused during a session or when it is loaded already
    Load {
        /// The module (scripts/hugepage/build.sh)
        #[arg(long, value_name = "FILE", default_value = "/mnt/tkg-build/oneios/hugepage/out/android15-6.6.ko")]
        ko: String,
        /// Its preflight, built for the PC by the same script
        #[arg(long, value_name = "FILE", default_value = "/mnt/tkg-build/oneios/hugepage/out/kapi_check")]
        kapi_check: String,
        /// The pool's want at load, in pages of 2 MiB (64 = 128 MiB)
        #[arg(long, value_name = "N", default_value_t = 64)]
        want: u32,
        /// Where the preflight's files go (btf.vmlinux, kapi_check.txt, load.txt)
        #[arg(long, value_name = "DIR", default_value = "phone-hugepool")]
        out: String,
        /// take18's alternative module instead of ours (scripts/hugepage/build.sh WORK samfor12, hugepool::KO_SAMFOR12):
        /// samfor12/gh-hugepage-reserve-temp-root-support 361d499, our pin 00c8a03 plus six commits (pool pages held with
        /// PG_reserved, cleared before every free and before a page is served; a dead VM's death grace 3 -> 13 s). The same
        /// module name, parameters and preflight. Off unless named; `unload`, then `load` without it, goes back
        #[arg(long, conflicts_with = "ko")]
        samfor12: bool,
        /// take23: the module's system_reserve_mb (insmod only): the RAM its table never counts as the pool's. Its own 6144
        /// leaves the S25 Ultra a table of about 2700 pages; 3072 one of about 4000 (8 GiB), for the Images above 5 GiB of the
        /// new boot order. It bounds what the pool may take, not what it takes: vm-boot asks --host-keep-mib at each boot
        #[arg(long, value_name = "MIB")]
        system_reserve_mb: Option<u32>,
    },
    /// One UI's framework stopped (as the switch stops it), the file cache dropped, the pool's want raised to N and its
    /// acquire pressed, One UI started again (as the return starts it): a script on the phone, detached, with a
    /// failsafe that starts One UI LIMIT s after the stop. No restart of the phone unless One UI does not come back
    Grow {
        /// The pool's new want, in pages of 2 MiB (1536 = 3 GiB)
        #[arg(long, value_name = "N")]
        want: u32,
        /// The module's acquire: 3 sweep and per-window evict, 2 sweep and system-wide reclaim
        #[arg(long, value_name = "MODE", default_value_t = 3)]
        mode: u32,
        /// Seconds One UI may be down before the failsafe starts it (90..600)
        #[arg(long, value_name = "SECONDS", default_value_t = 180)]
        limit: u32,
        /// Where the cycle's script and log go
        #[arg(long, value_name = "DIR", default_value = "phone-hugepool")]
        out: String,
    },
    /// The module's own acquire beside One UI (nothing stopped): 1 contig-any, 2 sweep and reclaim, 3 sweep and
    /// per-window evict; the want first when given; cancelled after SECONDS
    Acquire {
        /// The pool's new want, in pages of 2 MiB
        #[arg(long, value_name = "N")]
        want: Option<u32>,
        /// The acquire mode
        #[arg(long, value_name = "MODE", default_value_t = 1)]
        mode: u32,
        /// Seconds before it is cancelled
        #[arg(long, value_name = "SECONDS", default_value_t = 60)]
        seconds: u32,
    },
    /// rmmod, when no VM holds pool pages and no acquire runs
    Unload,
}

#[derive(clap::Subcommand)]
pub enum HaltpollCommand {
    /// Its parameters and kernel lines (read only)
    Status,
    /// The .ko checked against Droid-VM's prebuilt, pushed and checked again, a sync, then insmod
    Load {
        /// The module (DroidVM-Prebuilt-Root f7c6ce8)
        #[arg(long, value_name = "FILE", default_value = "/mnt/tkg-build/oneios/dl/halt-poll/gunyah-halt-poll-gki-6.6.ko")]
        ko: String,
        /// Where load.txt goes
        #[arg(long, value_name = "DIR", default_value = "phone-haltpoll")]
        out: String,
    },
    /// rmmod (refused by the kernel while a VM runs)
    Unload,
}

#[derive(clap::Subcommand)]
pub enum UdmabufCommand {
    /// Its mode, limits and counts, the built-in's limits, its kernel lines (read only)
    Status,
    /// The built-in's symbols checked in /proc/kallsyms, the .ko pushed and checked, a sync, then insmod; it says its mode
    Load {
        /// The module (scripts/hugepage/build-udmabuf.sh)
        #[arg(long, value_name = "FILE", default_value = "/mnt/tkg-build/oneios/hugepage/out/udmabuf-android15-6.6.ko")]
        ko: String,
        /// Where load.txt goes
        #[arg(long, value_name = "DIR", default_value = "phone-udmabuf")]
        out: String,
    },
    /// rmmod (refused by the kernel while a buffer of the module's lives)
    Unload,
}

#[derive(clap::Subcommand)]
pub enum HostshareCommand {
    /// Its parameters, /dev/gunyah_share and its kernel lines (read only)
    Status,
    /// The preflight (the phone's BTF on the PC with pahole and pfunct: struct gunyah_vm's offsets, the parcel structs,
    /// the six functions' prototypes; /proc/kallsyms on the phone), the .ko pushed and checked, a sync, then insmod
    /// with the offsets read. Refused during a session or when it is loaded already
    Load {
        /// The module (scripts/hugepage/build-share.sh)
        #[arg(long, value_name = "FILE", default_value = "/mnt/tkg-build/oneios/hugepage/out/gunyah_host_share-android15-6.6.ko")]
        ko: String,
        /// Where the preflight's files go (btf.vmlinux, pahole.txt, functions.txt, load.txt)
        #[arg(long, value_name = "DIR", default_value = "phone-hostshare")]
        out: String,
    },
    /// rmmod, when it holds no parcel
    Unload,
}

#[derive(clap::Subcommand)]
pub enum BacklightCommand {
    /// The level, max_brightness, and One UI's own value that the return writes back (read only)
    Status,
    /// Write LEVEL (1 to max_brightness: 12750 on the S24 Ultra); the return writes One UI's own value back
    Set { level: u32 },
}

#[derive(clap::Args)]
pub struct LogArgs {
    /// Which log: xnuhost (default), switch, return, outcome, display, watchdog, usb, or logs/xnuhost-<epoch>.log
    #[arg(long, default_value = "xnuhost")]
    file: String,
    /// The named logs with their sizes, and the earlier sessions' (the fork's `oneios_root.sh logs`)
    #[arg(long)]
    list: bool,
    /// Only the last N lines (with --follow: start N lines before the end)
    #[arg(long)]
    tail: Option<u64>,
    /// Keep reading as the log grows (tail -F), connecting again when adb drops; Ctrl-C ends it
    #[arg(long, short = 'f')]
    follow: bool,
    /// Also write every line read (not only the ones printed) to PATH
    #[arg(long)]
    save: Option<String>,
    /// --save to out/session-logs/DATE/live-HHMMSS.log
    #[arg(long)]
    save_auto: bool,
    /// Print only lines matching REGEX (repeatable; with the named filters: any of them)
    #[arg(long)]
    grep: Vec<String>,
    /// Panics, faults, crashes, the session's end
    #[arg(long)]
    panic: bool,
    /// The boot's stages (kernel, launchd, backboardd, SpringBoard, the first frame), each once, and panics and the end
    #[arg(long)]
    markers: bool,
    #[arg(long)]
    display: bool,
    #[arg(long)]
    gpu: bool,
    #[arg(long)]
    wifi: bool,
    #[arg(long)]
    hid: bool,
    #[arg(long)]
    mem: bool,
    /// Only lines from this second of the log's own clock on (its [seconds.millis] stamps)
    #[arg(long, value_name = "SECONDS")]
    since: Option<f64>,
    /// When the reading ends: `oneios-tool log timeline` of what was read, and `log stacks --summary` when a sym index exists
    #[arg(long)]
    analyze: bool,
}

fn phone_log(serial: Option<&str>, a: LogArgs) -> anyhow::Result<()> {
    let adb = Adb::connect(serial)?;
    if a.list {
        return log::list(&adb);
    }
    let named: Vec<&str> = [("panic", a.panic), ("display", a.display), ("gpu", a.gpu), ("wifi", a.wifi), ("hid", a.hid), ("mem", a.mem)].iter().filter(|(_, on)| *on).map(|(name, _)| *name).collect();
    let filter = log::Filter::new(&a.grep, &named, a.markers, a.since)?;
    let save = match (a.save, a.save_auto || a.analyze) {
        (Some(path), _) => Some(path),
        (None, true) => Some(log::auto_save_path(&cycle::repo_root().unwrap_or_else(|_| std::path::PathBuf::from(".")))),
        (None, false) => None,
    };
    log::log(&adb, &log::Options { file: a.file, tail: a.tail, follow: a.follow, save, analyze: a.analyze }, filter)
}

/// Run a parsed command; the exit status.
pub fn main(cli: Cli) -> i32 {
    let serial = cli.serial.as_deref();
    let result = match cli.command {
        Command::SparseWrite { out, gunzip, expect_size } => phone::sparse_write(&out, gunzip, expect_size),
        Command::Status { watch: None } => Adb::connect(serial).and_then(|adb| phone::status(&adb)),
        Command::Status { watch: Some(seconds) } => Adb::connect(serial).and_then(|adb| {
            log::catch_stop();
            loop {
                let (date, time) = cycle::local_date_time();
                println!("--- {date} {}:{}:{}", &time[..2], &time[2..4], &time[4..]);
                // a look that fails (adb dropping while the framework stops) is said, and the watch goes on
                if let Err(e) = phone::status(&adb) {
                    println!("status: {e:#}");
                }
                if log::sleep_or_stop(std::time::Duration::from_secs(seconds.max(1))) {
                    return Ok(());
                }
            }
        }),
        Command::Log(args) => phone_log(serial, args),
        Command::Switch { version_code } => Adb::connect(serial).and_then(|adb| phone::switch(&adb, version_code).map(|_| ())),
        Command::Return { timeout, note } => Adb::connect(serial).and_then(|adb| phone::do_return(&adb, &note, timeout)),
        Command::DisplayRestart { lib, dex, timeout, kill_after } => {
            Adb::connect(serial).and_then(|adb| session::display_restart(&adb, &session::Restart { lib, dex, timeout, kill_after }))
        }
        Command::Touch { command: TouchCommand::Status } => Adb::connect(serial).and_then(|adb| session::touch_status(&adb)),
        Command::Touch { command: TouchCommand::Wake } => Adb::connect(serial).and_then(|adb| session::touch_wake(&adb)),
        Command::Cpu { trace, out } => Adb::connect(serial).and_then(|adb| cpu::cpu(&adb, trace, out.as_deref())),
        Command::Hw { out } => Adb::connect(serial).and_then(|adb| hw::hw(&adb, &out)),
        Command::Dmesg { grep, tail, previous } => Adb::connect(serial).and_then(|adb| hw::dmesg(&adb, &grep, tail, previous)),
        Command::Hugeblocks { free } => Adb::connect(serial).and_then(|adb| vmprobe::hugeblocks(&adb, free)),
        Command::Hugepool { command } => Adb::connect(serial).and_then(|adb| match command {
            HugepoolCommand::Status => hugepool::status(&adb),
            HugepoolCommand::Load { ko, kapi_check, want, out, samfor12, system_reserve_mb } => {
                hugepool::load_with(&adb, if samfor12 { hugepool::KO_SAMFOR12 } else { &ko }, &kapi_check, want, &out, system_reserve_mb)
            }
            HugepoolCommand::Grow { want, mode, limit, out } => hugepool::grow(&adb, want, mode, limit, &out),
            HugepoolCommand::Acquire { want, mode, seconds } => hugepool::acquire(&adb, want, mode, seconds),
            HugepoolCommand::Unload => hugepool::unload(&adb),
        }),
        Command::Hostshare { command } => Adb::connect(serial).and_then(|adb| match command {
            HostshareCommand::Status => hostshare::status(&adb),
            HostshareCommand::Load { ko, out } => hostshare::load(&adb, &ko, &out),
            HostshareCommand::Unload => hostshare::unload(&adb),
        }),
        Command::DroidvmCrosvm { dir, grep, virgl, crosvm } => Adb::connect(serial).and_then(|adb| droidvm::crosvm(&adb, &dir, &grep, &virgl, &crosvm)),
        Command::Udmabuf { command } => Adb::connect(serial).and_then(|adb| match command {
            UdmabufCommand::Status => udmabuf::status(&adb),
            UdmabufCommand::Load { ko, out } => udmabuf::load(&adb, &ko, &out),
            UdmabufCommand::Unload => udmabuf::unload(&adb),
        }),
        Command::Haltpoll { command } => Adb::connect(serial).and_then(|adb| match command {
            HaltpollCommand::Status => haltpoll::status(&adb),
            HaltpollCommand::Load { ko, out } => haltpoll::load(&adb, &ko, &out),
            HaltpollCommand::Unload => haltpoll::unload(&adb),
        }),
        Command::VmBoot { image, seconds, thp, out, stop, bridge, swipe_test, pin, droidvm, gpu_floor, cpu_floor, gpu_idle_timer, touch_boost, takeover, gpu_cap_lift, keep_gpu_cap, no_max_volume, pin_first, thermal_log, touch_time, vsync_lock, vsync_phase_us, pacectl, perf_log, unpin_workers, audio_uid, mic_capture, video_decoder, video_decoder_uid, wifictl, bridgectl, no_haptics, virgl, crosvm, gpu_host_floors, crosvm_env, no_takeover_reclaim, takeover_swappiness, no_takeover_wake, panel_rate_min, fill_after_stop, image_larger, host_keep_mib, orchestrator, orchestrator_set } => Adb::connect(serial).and_then(|adb| {
            if stop { vmboot::stop(&adb) } else { vmboot::vm_boot(&adb, &image, &out, &vmboot::Options { seconds, thp, bridge, swipe_test, pin, droidvm, gpu_floor_mhz: gpu_floor, cpu_floor_percent: cpu_floor, gpu_idle_ms: gpu_idle_timer, touch_boost, takeover, gpu_cap_lift: vmboot::gpu_cap_lift(takeover, gpu_cap_lift, keep_gpu_cap), max_volume: !no_max_volume, pin_first, thermal_log, touch_time, vsync: vsync_lock.then_some(vsync_phase_us), pacectl, perf: vmperf::Perf { log: perf_log, unpin: unpin_workers }, audio_uid, mic_capture, video_decoder: video_decoder.then_some(video_decoder_uid), wifictl, bridgectl, haptics: !no_haptics, virgl, crosvm, gpu_host_floors: vmboot::Floors::parse(&gpu_host_floors)?, crosvm_env, takeover_reclaim: !no_takeover_reclaim, takeover_swappiness, takeover_wake: !no_takeover_wake, panel_rate_min, take23: take23::Opts { fill_after_stop: fill_after_stop || !image_larger.is_empty(), larger: image_larger, keep_mib: host_keep_mib, orchestrator, orch_set: orchestrator_set } }) }
        }),
        Command::VmProbe { payload, mem, timeout, out, thp, droidvm, shared_ram, thp_shmem } => Adb::connect(serial).and_then(|adb| {
            let crosvm = match droidvm.as_deref() {
                Some(dir) => {
                    adb.need_root()?;
                    Some(droidvm::push(&adb, dir)?)
                }
                None => None,
            };
            let thp_shmem = vmprobe::THP_SHMEM.into_iter().find(|m| *m == thp_shmem).unwrap_or("advise");
            vmprobe::vm_probe(&adb, &payload, &out, &vmprobe::Options { mem, timeout, thp, crosvm, shared_ram, thp_shmem })
        }),
        Command::Backlight { command: BacklightCommand::Status } => Adb::connect(serial).and_then(|adb| session::backlight_status(&adb)),
        Command::Backlight { command: BacklightCommand::Set { level } } => Adb::connect(serial).and_then(|adb| session::backlight_set(&adb, level)),
        Command::Stage { kind, dir } => Adb::connect(serial).and_then(|adb| phone::stage(&adb, &kind, &dir)),
        Command::Sums => Adb::connect(serial).and_then(|adb| phone::sums_text(&adb).map(|text| print!("{text}"))),
        Command::ImageCheck => Adb::connect(serial).and_then(|adb| {
            adb.need_root()?;
            phone::image_check(&adb).map(|n| println!("image: manifest.json and the {n} files it lists agree"))
        }),
        Command::Manifest { dir, out, stage } => Adb::connect(serial).and_then(|adb| phone::manifest_cmd(&adb, &dir, out.as_deref(), stage)),
        Command::PullData { out, vm, chunk, retries, backoff, stall, verify, restart, ioslog } => Adb::connect(serial).and_then(|adb| {
            // Ctrl-C ends the chunk that is being copied and says how to go on; the chunks done stay
            log::catch_stop();
            let image = vm.then(|| format!("{}/data.img", vmboot::DIR));
            pull::pull_data(&adb, &out, &pull::Options { chunk, retries, backoff, stall, verify, restart, ioslog, image })
        }),
    };
    match result {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("oneios-tool phone: {e:#}");
            1
        }
    }
}

#[derive(clap::Args)]
pub struct AppCli {
    /// The OneOS checkout (else ONEOS_DIR, the current folder, app/ of the OneiOS checkout, OneOS beside it)
    #[arg(long, global = true)]
    oneos: Option<String>,
    /// The phone's adb serial (or ANDROID_SERIAL)
    #[arg(long, short = 's', global = true)]
    serial: Option<String>,
    #[command(subcommand)]
    command: AppCommand,
}

#[derive(clap::Subcommand)]
pub enum AppCommand {
    /// versionName, versionCode, applicationId, the commit, and the installed app's version and certificate
    Version {
        /// Do not ask a phone
        #[arg(long)]
        no_phone: bool,
    },
    /// cd app-android && ./gradlew test assembleFullDebug; then the APK's path, size, sha256 and version
    Build {
        /// Without the unit tests
        #[arg(long)]
        no_test: bool,
    },
    /// The build's prerequisites: JDK 21, an Android SDK with platform 37, the GitHub Packages login
    Doctor,
    /// adb install -r of the built APK, of APK, or of the CI prerelease oneios-VERSION; refuses another certificate
    Install {
        /// An APK file (default: the checkout's build output)
        apk: Option<String>,
        /// The CI prerelease oneios-VERSION, downloaded with gh
        #[arg(long)]
        release: Option<String>,
        /// The GitHub repository of --release
        #[arg(long, default_value = "akshaynexus/OneOS")]
        repo: String,
        /// Where --release puts the download
        #[arg(long, default_value = "out/app")]
        dir: String,
    },
    /// The fork's oneios_switch.sh, oneios_return.sh and oneios_root.sh into the stage, with their sha256
    StageScripts,
    /// The checkout's path alone, as every app command and the cycle find it (for scripts)
    Dir,
}

/// Run a parsed `app` command; the exit status.
pub fn app_main(cli: AppCli) -> i32 {
    let (oneos, serial) = (cli.oneos.as_deref(), cli.serial.as_deref());
    let fork = || app::find_fork(oneos);
    let result = match cli.command {
        AppCommand::Version { no_phone } => fork().and_then(|f| app::version(&f, serial, no_phone)),
        AppCommand::Build { no_test } => fork().and_then(|f| app::build(&f, no_test)),
        AppCommand::Doctor => fork().and_then(|f| app::doctor(&f)),
        AppCommand::Install { apk, release, repo, dir } => {
            // the checkout is needed only for its build output
            let f = if apk.is_none() && release.is_none() { fork().map(Some) } else { Ok(None) };
            f.and_then(|f| app::install(f.as_ref(), serial, apk.as_deref(), release.as_deref(), &repo, std::path::Path::new(&dir)))
        }
        AppCommand::StageScripts => fork().and_then(|f| app::stage_scripts(&f, serial)),
        AppCommand::Dir => fork().map(|f| println!("{}", f.dir.display())),
    };
    match result {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("oneios-tool app: {e:#}");
            1
        }
    }
}

#[cfg(test)]
mod cli_tests {
    use super::*;
    use clap::Parser;

    #[derive(Parser)]
    struct T {
        #[command(subcommand)]
        c: Command,
    }

    #[test]
    fn hugepool_load_takes_the_alternative_module_by_name_only() {
        let t = T::try_parse_from(["t", "hugepool", "load", "--samfor12", "--want", "64"]).unwrap();
        assert!(matches!(t.c, Command::Hugepool { command: HugepoolCommand::Load { samfor12: true, .. } }));
        let t = T::try_parse_from(["t", "hugepool", "load"]).unwrap();
        assert!(matches!(t.c, Command::Hugepool { command: HugepoolCommand::Load { samfor12: false, .. } }));
        assert!(T::try_parse_from(["t", "hugepool", "load", "--samfor12", "--ko", "/x.ko"]).is_err());
    }

    #[test]
    fn vm_boot_takes_the_new_renderer_by_default() {
        let t = T::try_parse_from(["t", "vm-boot"]).unwrap();
        let Command::VmBoot { virgl, gpu_host_floors, crosvm_env, .. } = t.c else { panic!() };
        assert_eq!(virgl, droidvm::VIRGL_TAKE18);
        assert_eq!(gpu_host_floors, "deadline");
        assert!(crosvm_env.is_empty());
        let t = T::try_parse_from(["t", "vm-boot", "--virgl", "prebuilt", "--gpu-host-floors", "off", "--crosvm-env", "A=1", "--crosvm-env", "B=2"]).unwrap();
        let Command::VmBoot { virgl, gpu_host_floors, crosvm_env, .. } = t.c else { panic!() };
        assert_eq!((virgl.as_str(), gpu_host_floors.as_str(), crosvm_env.len()), ("prebuilt", "off", 2));
        assert!(T::try_parse_from(["t", "vm-boot", "--gpu-host-floors", "maybe"]).is_err());
    }
}
