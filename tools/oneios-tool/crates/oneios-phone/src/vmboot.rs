//! `oneios-tool phone vm-boot --image FILE [--seconds N] [--thp] [--out DIR]` and `phone vm-boot --stop`: the native
//! kernel as runtime/loader's xnuloader-crosvm made it (an arm64 Linux Image) booted in a protected VM of AVF's crosvm
//! under the phone's Gunyah, its console (the ns16550a at 0x3f8, kernel patch 0077) followed live (docs/native.md "A VM
//! under Gunyah"). The VM's RAM, vCPUs and restricted DMA pool come from the Image's own label (`oneios-crosvm ram-mib=N
//! cpus=N swiotlb-mib=N` at 0x100; an older label without swiotlb-mib means 8): the window was cut for them. Refused
//! during a session and while a VM of ours is up; the RAM checked as vm-probe checks it (MemAvailable plus
//! gh_hugepage_reserve's pool, 1 GiB left to Android). A VM above ~0.5 GB needs the pool (`phone hugepool`) and `--thp`
//! (vm-probe's: shmem_enabled advise and defrag madvise for the run, written back).
//!
//! The image set's disks: FILE.disks beside the Image (xnuloader-crosvm --image) names them, crosvm's `--block` values
//! with the PCI address each is pinned to. Before crosvm starts, the script checks each read-only file's size on the
//! phone against the manifest's, makes the VM's own Data store (sparse, at the manifest's size) when it is missing, and
//! wants 4 GiB free on /data. A protected VM's host can touch only crosvm's restricted DMA pool, where the kernel's disk
//! driver keeps every buffer: the moment crosvm has written its tree (`fdt.dtb`, before the kernel runs more than a few
//! instructions; its disks start seconds later), the script checks that the pool is where the Image's windows are
//! (`restricted_dma_reserved@ADDR`) and stops the VM if not.
//!
//! The run: the Image pushed to /data/local/tmp/oneios-vmboot/kernel.img, its SHA-256 checked on the phone, then a
//! script started there detached runs crosvm (vm-probe's flags, N vCPUs, serial to `serial.txt`), stops it through its
//! control socket after SECONDS or at `stop.request` (`vm-boot --stop`), kills it only when that does not end it, and
//! writes `result`. The tool prints `serial.txt` as it grows, then keeps `serial.txt`, `crosvm.log`, `dmesg.txt`,
//! `run.txt` and `fdt.dtb` in DIR.
//!
//! The screen: an Image made with xnuloader-crosvm --display has the boot video at the start of the pool and the
//! guest's frame count after it (FILE.disks: `fb OFFSET BYTES WIDTH HEIGHT STRIDE SEQ`, offsets in the pool). The tool
//! then pushes runtime/display's displayctl (`--bridge`) beside the Image, and the script starts it the moment the pool
//! check passed: `displayctl pool` maps only that part of crosvm's pool and serves it to the staged display helper
//! (runtime/bin/oneios-display), a layer over One UI that goes away when the VM ends. Touches still go to One UI under
//! it. Its log is `display.txt`.
//!
//! The touch: such an Image also has the input bus (`input touch` in FILE.disks). The script finds the phone's touchscreen
//! by its name in /proc/bus/input/devices (`sec_touchscreen`, with INPUT_PROP_DIRECT; refused should it be a side key's
//! device) and gives it to the VM through runtime/display's `displayctl touch` (pushed as the bridge is): it listens on
//! `touch.sock`, and when crosvm connects there (`--input multi-touch[path=...]`) it takes the touchscreen for the VM's
//! life (One UI under the layer gets no touches) and passes its frames on. crosvm's own `--evdev` cannot take it: it wants
//! the device's serial name, which sec_touchscreen has none of (boot 4's first start). The kernel reads crosvm's device as
//! a virtio-input PCI function, its queues in the pool. The side keys stay with Android (a takeover reads their presses, without
//! taking them, and passes them on to the VM: power, volume up and down); volume down held 10 s stops the
//! VM (getevent reads the key and does not take it): the way back without the PC. Their logs are `touch.txt` and run.txt.
//!
//! The shared-RAM VM: an Image of xnuloader-crosvm --shared-ram (label `shared-ram=1`) runs under Droid-VM's own crosvm
//! (`--droidvm`, pushed and checked as `phone droidvm-crosvm` does) as its pseudo-unprotected VM
//! (`--protected-vm-pseudo-unprotected`, no swiotlb): its RAM is shared with it after start through gunyah_host_share
//! (`phone hostshare load` first; the script checks /dev/gunyah_share) and its shim accepts it, so the host reads all
//! of it (docs/native.md "The shared-RAM VM"). The pool check is then the tree's `shim_handoff@` node (crosvm made the
//! shared-RAM VM) instead of the restricted DMA pool, and the bridge maps the boot video out of crosvm's mapping of the
//! guest's RAM (`displayctl pool --vm shared`). The window comes from gh_hugepage_reserve's 2 MiB pages as before: only
//! they stay where they are when gunyah_host_share pins them (vm-probe shared2: 1 GiB of other pages moved, 126651 runs,
//! and the resource manager refused to map it).
//!
//! The GPU: a shared-RAM Image of xnuloader-crosvm --gpu (label `gpu=1`, FILE.disks `gpu PCI-ADDRESS DRM-HOST-MIB
//! DRM-HOST-PA`) gets crosvm's virtio-gpu with Droid-VM's virglrenderer and drm2kgsl (`--gpu backend=virglrenderer,
//! context-types=drm,udmabuf=true,pci-address=...,pci-bar-size=0x10000000`: no host-visible window is needed, a guest
//! blob is the guest's own pages, and the 8 GiB default does not fit that crosvm's 64-bit window) and drm2kgsl's host arena
//! (`--pre-alloc drm-host-mb=N`); the tree's `drm2kgsl_host@PA` node is checked beside the shim's.
//!
//! The screen through the GPU's scanout (xnuloader-crosvm --gpu-scanout, FILE.disks `scanout WIDTH HEIGHT HZ`): crosvm's
//! virtio-gpu gets that display (`displays=[[mode=windowed[W,H],refresh-rate=HZ]]`) and its Android display backend
//! (`--android-display-service name=oneios_disp,screen=gpu-0`, which registers that service), and the display helper
//! starts in its crosvm mode (`oneios-display --crosvm-display oneios_disp`): its layer's Surface goes to crosvm, which
//! blits each frame the guest flushes into it on the GPU. No bridge, no copy on either CPU.
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{copy, Write};
use std::path::Path;
use std::time::{Duration, Instant};

pub const DIR: &str = "/data/local/tmp/oneios-vmboot";
/// The service name crosvm registers its Android display under (--android-display-service), and the helper looks up.
pub const CROSVM_DISPLAY: &str = "oneios_disp";
/// crosvm's DRAM on aarch64 (xnuloader-crosvm RAM_BASE).
const RAM_BASE: u64 = 0x8000_0000;
/// What /data must have free before a VM with a Data store starts: its first start writes the seed's ~2 GB into it.
const DATA_FREE_KIB: u64 = 4 << 20;

/// The label xnuloader-crosvm writes at 0x100.
#[derive(Debug, PartialEq)]
pub struct Label {
    pub ram: u32,
    pub cpus: u32,
    pub swiotlb: u32,
    /// xnuloader-crosvm --shared-ram: Droid-VM's pseudo-unprotected VM
    pub shared: bool,
    /// xnuloader-crosvm --gpu: the VM's virtio-gpu, and drm2kgsl's host arena at the RAM's top
    pub gpu: bool,
}

pub fn label(image: &[u8]) -> Option<Label> {
    if image.len() < 0x200 || &image[0x38..0x3c] != b"ARM\x64" {
        return None;
    }
    let end = image[0x100..0x200].iter().position(|b| *b == 0)?;
    let text = std::str::from_utf8(&image[0x100..0x100 + end]).ok()?;
    let rest = text.strip_prefix("oneios-crosvm ")?;
    let (mut ram, mut cpus, mut swiotlb, mut shared, mut gpu) = (None, None, Some(8), false, false);
    for word in rest.split_whitespace() {
        if let Some(v) = word.strip_prefix("ram-mib=") {
            ram = v.parse().ok();
        } else if let Some(v) = word.strip_prefix("cpus=") {
            cpus = v.parse().ok();
        } else if let Some(v) = word.strip_prefix("swiotlb-mib=") {
            swiotlb = v.parse().ok();
        } else if word == "shared-ram=1" {
            shared = true;
        } else if word == "gpu=1" {
            gpu = true;
        }
    }
    Some(Label { ram: ram?, cpus: cpus?, swiotlb: swiotlb?, shared, gpu })
}

/// FILE.disks: the pool crosvm's tree must name, the Data store and crosvm's `--block` values.
#[derive(Debug, PartialEq)]
pub struct Disks {
    pub swiotlb: u32,
    pub pool: u64,
    pub store: String,
    pub store_gib: u32,
    /// The size the phone's file must have (None: the writable store) and crosvm's --block value.
    pub blocks: Vec<(Option<u64>, String)>,
    /// The boot video and the frame count in the pool (xnuloader-crosvm --display).
    pub fb: Option<Fb>,
    /// The kernel has the input bus (`input touch`): the phone's touchscreen goes to crosvm.
    pub input: bool,
    /// `gpu PCI-ADDRESS DRM-HOST-MIB DRM-HOST-PA` (xnuloader-crosvm --gpu)
    pub gpu: Option<Gpu>,
    /// `scanout WIDTH HEIGHT HZ` (xnuloader-crosvm --gpu-scanout): the screen through crosvm's own display
    pub scanout: Option<(u32, u32, u32)>,
    /// `gpu-intid N` (xnuloader-crosvm --gpu-intid): the GIC line the kernel's GPU sleeps on, checked against crosvm's
    /// interrupt-map (gpu_intid_check)
    pub gpu_intid: Option<u32>,
    /// `audio` (xnuloader-crosvm --audio): the kernel's sound card, crosvm's virtio-snd with its AAudio backend (the speaker)
    pub audio: bool,
    /// `wifi` (xnuloader-crosvm --wifi): the kernel's Wi-Fi, crosvm's virtio-net on a tap that `wifictl tap` carries to wlan0
    pub wifi: bool,
}

/// The GPU's PCI address (crosvm's pci-address=), drm2kgsl's host arena in MiB and where the Image expects it.
#[derive(Debug, PartialEq)]
pub struct Gpu {
    pub pci: String,
    pub drm_mib: u64,
    pub drm_host: u64,
}

/// `fb OFFSET BYTES WIDTH HEIGHT STRIDE SEQ`: offsets in the pool, the frame first, the count's page after it.
#[derive(Debug, PartialEq)]
pub struct Fb {
    pub offset: u64,
    pub bytes: u64,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub seq: u64,
}

/// A value the phone's shell and crosvm's key=value parser take as it is: no blank, quote or shell character.
fn plain(text: &str) -> bool {
    !text.is_empty() && text.chars().all(|c| c.is_ascii_alphanumeric() || "/._-=:,".contains(c))
}

pub fn parse_disks(text: &str) -> Result<Disks> {
    let (mut swiotlb, mut pool, mut store, mut fb, mut input, mut gpu, mut scanout) = (None, None, None, None, false, None, None);
    let (mut audio, mut wifi, mut gpu_intid) = (false, false, None);
    let mut blocks = Vec::new();
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let words: Vec<&str> = line.split_whitespace().collect();
        match words.as_slice() {
            ["swiotlb-mib", v] => swiotlb = Some(v.parse::<u32>().context("swiotlb-mib")?),
            ["pool", v] => pool = Some(u64::from_str_radix(v.strip_prefix("0x").context("pool 0x...")?, 16).context("pool")?),
            ["store", path, gib] => store = Some((path.to_string(), gib.parse::<u32>().context("store GIB")?)),
            ["block", size, value] => {
                let size = if *size == "-" { None } else { Some(size.parse::<u64>().context("block BYTES")?) };
                blocks.push((size, value.to_string()));
            }
            ["fb", offset, bytes, width, height, stride, seq] => {
                fb = Some(Fb {
                    offset: offset.parse().context("fb OFFSET")?,
                    bytes: bytes.parse().context("fb BYTES")?,
                    width: width.parse().context("fb WIDTH")?,
                    height: height.parse().context("fb HEIGHT")?,
                    stride: stride.parse().context("fb STRIDE")?,
                    seq: seq.parse().context("fb SEQ")?,
                })
            }
            ["input", "touch"] => input = true,
            ["audio"] => audio = true,
            ["wifi"] => wifi = true,
            ["gpu-intid", n] => {
                let n: u32 = n.parse().context("gpu-intid N")?;
                if !(32..=1019).contains(&n) {
                    bail!("gpu-intid {n}: an SPI of the GIC, 32 to 1019");
                }
                gpu_intid = Some(n);
            }
            ["gpu", pci, mib, pa] => {
                let ok = pci.len() == 7 && pci.as_bytes()[2] == b':' && pci.as_bytes()[5] == b'.' && plain(pci);
                if !ok {
                    bail!("gpu: {pci:?} is not a PCI address of the form 00:0c.0");
                }
                gpu = Some(Gpu {
                    pci: pci.to_string(),
                    drm_mib: mib.parse().context("gpu DRM-HOST-MIB")?,
                    drm_host: u64::from_str_radix(pa.strip_prefix("0x").context("gpu DRM-HOST-PA 0x...")?, 16).context("gpu DRM-HOST-PA")?,
                })
            }
            ["scanout", w, h, hz] => {
                let (w, h, hz): (u32, u32, u32) = (w.parse().context("scanout WIDTH")?, h.parse().context("scanout HEIGHT")?, hz.parse().context("scanout HZ")?);
                if !(320..=4096).contains(&w) || !(320..=4096).contains(&h) || !(24..=120).contains(&hz) {
                    bail!("scanout {w}x{h} at {hz} Hz: 320 to 4096 a side, 24 to 120 Hz");
                }
                scanout = Some((w, h, hz));
            }
            _ => bail!("a line of the disks list that is not one of swiotlb-mib, pool, store, block, fb, input, gpu, gpu-intid, scanout, audio, wifi: {line:?}"),
        }
    }
    let (store, store_gib) = store.context("no store line")?;
    let d = Disks { swiotlb: swiotlb.context("no swiotlb-mib line")?, pool: pool.context("no pool line")?, store, store_gib, blocks, fb, input, gpu, scanout, gpu_intid, audio, wifi };
    if d.scanout.is_some() && (d.gpu.is_none() || d.fb.is_none()) {
        bail!("a scanout without the GPU or the screen: xnuloader-crosvm gives it with --gpu-scanout and --display");
    }
    if d.gpu_intid.is_some() && d.gpu.is_none() {
        bail!("a GPU interrupt without the GPU: xnuloader-crosvm gives it with --gpu-intid beside --gpu");
    }
    if d.input && d.fb.is_none() {
        bail!("the input bus without a screen: xnuloader-crosvm gives both with --display");
    }
    if let Some(f) = &d.fb {
        // as displayctl pool takes them: the frame page aligned at the pool's start or after, the count after it
        let pool_bytes = (d.swiotlb as u64) << 20;
        let ok = (16..=16384).contains(&f.width) && (16..=16384).contains(&f.height) && f.stride as u64 >= f.width as u64 * 4
            && f.bytes == f.stride as u64 * f.height as u64 && f.offset % 16384 == 0 && f.seq % 16384 == 0
            && f.seq >= f.offset + f.bytes && f.seq + 16384 <= pool_bytes;
        if !ok {
            bail!("the fb line {f:?} does not lie in the pool of {} MiB as the frame and then its count", d.swiotlb);
        }
    }
    if d.blocks.is_empty() || d.blocks.len() > 5 || !(1..=256).contains(&d.store_gib) {
        bail!("1 to 5 disks and a store of 1 to 256 GiB");
    }
    if !d.store.starts_with("/data/") || !plain(&d.store) || d.store.contains(',') {
        bail!("the store {:?} is not a plain path under /data", d.store);
    }
    let mut writable = 0;
    for (size, value) in &d.blocks {
        let path = value.strip_prefix("path=").and_then(|v| v.split(',').next()).unwrap_or("");
        if !plain(value) || !path.starts_with("/data/") || !value.contains(",pci-address=00:") {
            bail!("the --block value {value:?} is not a plain path under /data with a pinned PCI address");
        }
        match size {
            None if path == d.store && value.contains(",ro=false") => writable += 1,
            Some(_) if value.contains(",ro=true") => {}
            _ => bail!("{value:?}: the store is the one writable disk, every other disk read-only with its size"),
        }
    }
    if writable != 1 {
        bail!("the disks list names the store {:?} {writable} times", d.store);
    }
    Ok(d)
}

/// The run on the phone (detached; its own log is `run.txt`).
/// The GPU's lowest clock while an Image with the GPU runs, MHz, as the `vm-boot --gpu-floor` default. **0 = off** (the
/// governor, msm-adreno-tz, scales freely). Off by default since the 120 Hz stutter was traced to the panel held at
/// 60 Hz (`native-frame-latency`), not the GPU clock: the frame-time histogram was the same at a pinned 607 MHz as at
/// the idle 160-222 MHz, because the GPU is only ~1-30% busy and frames are bound by the submit->fence round trip, not
/// the clock. Pinning 607 MHz the whole session only burned power. A session that wants to A/B a pinned floor passes
/// `--gpu-floor 607` (607 MHz is one of the S25 GPU's 15 clocks, 160 to 1200 MHz).
pub const GPU_FLOOR_MHZ: u32 = 0;

/// The check of the GPU's interrupt (`gpu-intid N`, xnuloader-crosvm --gpu-intid), run once crosvm's tree is whole: its
/// PCI host's `interrupt-map` must route the GPU's function (`pci`, crosvm's pci-address=; the map's child address is
/// the device's bits alone, `interrupt-map-mask` 0xf800) to SPI N-32, level-high, in one entry. An entry is 10 cells:
/// the child's address (3) and pin, the GIC's phandle (any), its address (2, zero), the SPI's type (0), number and flags
/// (4, level-high); the tree is read as hex words with the phone's toybox (`od`, `tr`, `sed`, `grep -o -E`). Not so, or
/// no entry: the line is not the GPU's (it could be another device's level interrupt, which the GPU's handler would
/// never acknowledge), so the VM is stopped before the kernel's GPU sleeps on it. The function is the script's
/// `gpu_irq`, which says what crosvm gave on `gpu:` lines and fails on a mismatch.
pub fn gpu_intid_check(pci: &str, intid: u32) -> String {
    let dev = u32::from_str_radix(pci.get(3..5).unwrap_or("0"), 16).unwrap_or(0);
    let (child, spi) = (dev << 11, intid - 32);
    format!(
        "gpu_irq() {{\n  \
         set -- $(od -An -tx1 -v $D/fdt.dtb | tr -d ' \\n' | sed 's/......../& /g' | \
         grep -o -E ' {child:08x} 00000000 00000000 0000000[1-4] [0-9a-f]{{8}} 00000000 00000000 [0-9a-f]{{8}} [0-9a-f]{{8}} [0-9a-f]{{8}}')\n  \
         if [ $# = 0 ]; then echo \"gpu: crosvm's interrupt-map has no entry for {pci}: intid {intid} is not known to be the GPU's line, stopped before the kernel's GPU sleeps on it\"; return 1; fi\n  \
         if [ $# != 10 ]; then echo \"gpu: crosvm's interrupt-map has more than one entry for {pci} ($*): stopped before the kernel's GPU sleeps on intid {intid}\"; return 1; fi\n  \
         if [ $8 = 00000000 ] && [ $9 = {spi:08x} ] && [ ${{10}} = 00000004 ]; then echo \"gpu: the GPU's interrupt is intid {intid} (crosvm's SPI {spi}): its waits sleep on it\"; return 0; fi\n  \
         echo \"gpu: crosvm routes {pci} to GIC type 0x$8 number 0x$9 flags 0x${{10}}, not SPI {spi} level-high (type 0, number 0x{spi:x}, flags 4) as the Image's intid {intid}: stopped before the kernel's GPU sleeps on a line that is not its own\"; return 1\n\
         }}\n\
         if [ \"$pool\" != bad ] && ! gpu_irq; then pool=bad; touch $D/stop.request; fi\n"
    )
}

pub fn boot_script(l: &Label, seconds: u32, thp: bool, sha: &str, disks: Option<&Disks>) -> String {
    boot_script_with(l, seconds, thp, sha, disks, false, false, &Tuning::DEFAULT)
}

/// The same, with `swipe`: the touch forwarder's test swipes (`--swipe-test`: 200 s after crosvm connected, then every
/// 60 s), a test of the VM's touch without a finger.
/// `pin` (`--pin`): vCPU N on the phone's core N (crosvm `--cpu-affinity`), each where the scheduler would otherwise
/// move it about: on the S25 cores 0 to 5 run at up to 3.53 GHz and 6 and 7 at 4.47 GHz.
/// The GPU's floor for the VM's life (an Image with the GPU and `floor_mhz` > 0): devfreq's lowest clock becomes the
/// lowest of the GPU's clocks at or above `floor_mhz`, and is put back at the end; the governor still raises the clock as
/// it likes. KGSL's min_pwrlevel is not it: on the S25 the level written there was back at 14 (160 MHz) seconds later,
/// while devfreq's min_freq stays (full2). Without the GPU, or at 0: nothing.
/// How long the GPU stays awake after its last work while the VM runs (KGSL's idle_timer, ms; Android's is 80).
pub const GPU_AWAKE_MS: u32 = 30000;

pub fn gpu_floor_script(l: &Label, floor_mhz: u32) -> (String, String) {
    if !l.gpu || floor_mhz == 0 {
        return (String::new(), String::new());
    }
    let hz = u64::from(floor_mhz) * 1_000_000;
    (
        format!(
            "G=/sys/class/kgsl/kgsl-3d0/devfreq\nK=/sys/class/kgsl/kgsl-3d0\ngpu_min=\ngpu_idle=\n\
             gpu_restore() {{\n  \
             [ -z \"$gpu_idle\" ] || {{ echo \"$gpu_idle\" > $K/idle_timer && echo \"gpu: it sleeps $(cat $K/idle_timer) ms after its work again\"; }}\n  \
             gpu_idle=\n  [ -n \"$gpu_min\" ] || return 0\n  \
             echo \"$gpu_min\" > $G/min_freq && echo \"gpu: its lowest clock is $(cat $G/min_freq) Hz again\"\n  \
             gpu_min=\n}}\n\
             # awake 30 s after its last work, not 80 ms: the first frames after a pause waited for it to wake (take2)\n\
             if [ -w $K/idle_timer ]; then gpu_idle=$(cat $K/idle_timer); echo {GPU_AWAKE_MS} > $K/idle_timer && \
             echo \"gpu: it stays awake $(cat $K/idle_timer) ms after its last work while the VM runs (it was $gpu_idle ms)\"; fi\n\
             if [ -w $G/min_freq ] && [ -r $G/available_frequencies ]; then\n  \
             floor=\n  \
             for f in $(cat $G/available_frequencies); do\n    \
             if [ \"$f\" -ge {hz} ] && {{ [ -z \"$floor\" ] || [ \"$f\" -lt \"$floor\" ]; }}; then floor=$f; fi\n  done\n  \
             if [ -n \"$floor\" ]; then\n    \
             gpu_min=$(cat $G/min_freq)\n    \
             echo $floor > $G/min_freq && echo \"gpu: the GPU's lowest clock is $(cat $G/min_freq) Hz while the VM runs (it was $gpu_min Hz)\"\n  \
             else echo \"gpu: no clock of {floor_mhz} MHz or more: the GPU's clocks are the governor's\"; fi\n\
             else echo \"gpu: no $G/min_freq: the GPU's clocks are the governor's\"; fi\n"
        ),
        "gpu_restore\n".to_string(),
    )
}

/// The thermal log (`vm-boot --thermal-log`, thermal.txt beside the VM's other files): once a second while crosvm runs, the
/// hottest zone of each CPU cluster (cpu-0-*: cores 0-5, cpu-1-*: 6 and 7), of the GPU (gpuss-*), the board (sys-therm-0), in
/// tenths of a degree, the battery's (tenths), each cluster's clock and its top, the GPU's clock and load. The zones are found
/// by their type once, at the start (the owner allowed reading /sys/class/thermal for this, 2026-11-24): the material of a
/// controller of our own for smoothness and temperature. Read only.
pub fn thermal_log_script(on: bool) -> String {
    if !on {
        return String::new();
    }
    concat!(
        "# the thermal log (vm-boot --thermal-log): read only, once a second while crosvm runs\n",
        "tz() { for z in /sys/class/thermal/thermal_zone*; do case \"$(cat $z/type 2>/dev/null)\" in $1) echo $z/temp ;; esac; done; }\n",
        "hot() { m=-1000000; for f in \"$@\"; do v=$(cat $f 2>/dev/null); [ -n \"$v\" ] && [ \"$v\" -gt $m ] && m=$v; done; echo $((m / 100)); }\n",
        "(\n",
        "  c0=$(tz 'cpu-0-*'); c1=$(tz 'cpu-1-*'); gz=$(tz 'gpuss-*'); bz=$(tz 'sys-therm-0'); Q=/sys/devices/system/cpu/cpufreq; K=/sys/class/kgsl/kgsl-3d0\n",
        "  echo \"# time c0 c1 gpu board bat (tenths of a degree) f0/top0 f6/top6 (kHz) gpu-clock gpu-busy\"\n",
        "  while kill -0 $pid 2>/dev/null; do\n",
        "    echo \"$(date +%T) c0 $(hot $c0) c1 $(hot $c1) gpu $(hot $gz) board $(hot $bz) bat $(cat /sys/class/power_supply/battery/temp 2>/dev/null) \\\n",
        "f0 $(cat $Q/policy0/scaling_cur_freq)/$(cat $Q/policy0/scaling_max_freq) f6 $(cat $Q/policy6/scaling_cur_freq)/$(cat $Q/policy6/scaling_max_freq) \\\n",
        "g $(cat $K/gpuclk) $(cat $K/gpu_busy_percentage | tr -d ' %')\"\n",
        "    sleep 1\n",
        "  done\n",
        ") > $D/thermal.txt 2>&1 &\n",
        "echo \"thermal: the log is $D/thermal.txt, once a second\"\n",
    )
    .to_string()
}

/// The CPUs' floor for the VM's life, percent of each cluster's top clock, as the `vm-boot --cpu-floor` default.
/// **0 = off** (the governor, WALT, scales freely). Off by default alongside the GPU floor: the stutter was the panel
/// (above), not slow clocks, and pinning each cluster at 60% of its top the whole session held the cores high and warm
/// with no measured frame-time win. A session that wants to A/B a pinned floor passes `--cpu-floor 60` (about 2.25 GHz
/// perf, 2.84 GHz prime once Samsung's cap is lifted). The wake-ramp latency this once guarded against belongs to the
/// idle governor / haltpoll, not to holding every core high the whole run.
pub const CPU_FLOOR_PERCENT: u32 = 0;

/// The CPUs' floor for the VM's life (`floor_percent` > 0): each cpufreq policy's lowest clock becomes the lowest of its
/// clocks at or above that share of its top one now (scaling_max_freq: a cap Android set stays), put back at the end.
/// Policies are looked for by name (policy0 to policy7), not by a walk of /sys.
pub fn cpu_floor_script(floor_percent: u32) -> (String, String) {
    if floor_percent == 0 {
        return (String::new(), String::new());
    }
    let pct = floor_percent.min(100);
    (
        format!(
            "CF=/sys/devices/system/cpu/cpufreq\ncpu_mins=\n\
             cpu_restore() {{\n  [ -n \"$cpu_mins\" ] || return 0\n  \
             for pm in $cpu_mins; do echo \"${{pm#*=}}\" > $CF/policy${{pm%%=*}}/scaling_min_freq; done\n  \
             echo \"cpu: the clusters' lowest clocks are back:$cpu_mins\"\n  cpu_mins=\n}}\n\
             for p in 0 1 2 3 4 5 6 7; do\n  \
             f=$CF/policy$p\n  [ -w $f/scaling_min_freq ] && [ -r $f/scaling_available_frequencies ] || continue\n  \
             old=$(cat $f/scaling_min_freq); top=$(cat $f/scaling_max_freq); want=$((top * {pct} / 100)); floor=\n  \
             for c in $(cat $f/scaling_available_frequencies); do\n    \
             if [ \"$c\" -ge \"$want\" ] && [ \"$c\" -le \"$top\" ] && {{ [ -z \"$floor\" ] || [ \"$c\" -lt \"$floor\" ]; }}; then floor=$c; fi\n  done\n  \
             [ -n \"$floor\" ] || floor=$top\n  \
             echo $floor > $f/scaling_min_freq && cpu_mins=\"$cpu_mins $p=$old\" && \
             echo \"cpu: policy$p's lowest clock is $(cat $f/scaling_min_freq) kHz while the VM runs (it was $old; its top is $top)\"\n\
             done\n"
        ),
        "cpu_restore\n".to_string(),
    )
}

/// How the VM's session treats the phone: the GPU's and the CPUs' floors (gpu_floor_script, cpu_floor_script) and whether One
/// UI's framework is stopped for the VM's life (takeover_script).
pub struct Tuning {
    pub gpu_floor_mhz: u32,
    pub cpu_floor_percent: u32,
    pub takeover: bool,
    /// With --pin: vCPU i on core pin_first + i, the cores below it the host's (crosvm's device threads, surfaceflinger)
    pub pin_first: u32,
    /// The thermal log (thermal_log_script)
    pub thermal_log: bool,
    /// With the sound card: crosvm's AAudio backend in its own process under this uid (crosvm's uid=), or None: in crosvm's
    pub audio_uid: Option<u32>,
}

impl Tuning {
    pub const DEFAULT: Tuning =
        Tuning { gpu_floor_mhz: GPU_FLOOR_MHZ, cpu_floor_percent: CPU_FLOOR_PERCENT, takeover: false, pin_first: 0, thermal_log: false,
                 audio_uid: None };
}

/// ui.sh for a takeover: hugepool's cycle helpers (hugepool::ui_functions: the stop loop, the watchdog feeder, the failsafe,
/// start_ui and its last resort), with this folder's names. boot.sh sources it for start_ui and runs it again with a word for
/// its helpers. Their outcome goes to ui-ctl/result: $D/result is the VM's. LIMIT: seconds One UI may be down before the
/// failsafe starts it whatever the session does.
pub fn ui_script(limit: u32) -> String {
    format!(
        "#!/system/bin/sh\n# vm-boot --takeover: One UI's stop and start, the helpers of hugepool's cycle (crates/oneios-phone/src/hugepool.rs)\n\
         D={DIR}\nSELF=$D/ui.sh\nCTL=$D/ui-ctl\nP=/sys/module/gh_hugepage_reserve/parameters\nLIMIT={limit}\n{}",
        crate::hugepool::ui_functions()
            .replace("$D/result", "$CTL/result")
            .replace("$D/cycle.log", "$D/ui.log")
            .replace("oneios-hugepool-failsafe", "oneios-vmboot-failsafe")
    )
}

/// The takeover (vm-boot --takeover), after the checks and before crosvm: One UI's framework stopped as hugepool's grow stops
/// it (ui.sh: the way back armed first, the failsafe at LIMIT and a trap that starts One UI on any exit, the framework kept
/// down, Samsung's software watchdog fed), its RAM freed, and the CPUs' caps (the profile's or the power saver's
/// scaling_max_freq; thermal limits are not these) lifted to each cluster's top clock until the end. With One UI down nothing
/// puts them back, turns the screen off for want of touches or lowers the clocks between them (full3: the profile's 2 GHz
/// cap came back a minute after it was lifted). Empty without the takeover.
pub fn takeover_script(takeover: bool, need_pages: u64) -> (String, String) {
    if !takeover {
        return (String::new(), String::new());
    }
    let stop = format!(
        "# One UI's framework stopped for the VM's life (vm-boot --takeover): ui.sh, the way back armed first\n\
         gpu_restore() {{ :; }}; cpu_restore() {{ :; }}; caps_restore() {{ :; }}; wifi_restore() {{ :; }}\n\
         . $D/ui.sh\n\
         rm -rf $CTL; mkdir -p $CTL\n\
         for svc in zygote_secondary perfmond media.tuner zygote_next; do getprop init.svc.$svc > $CTL/base.$svc; done\n\
         {}\
         trap 'thp_restore; gpu_restore; cpu_restore; caps_restore; wifi_restore; start_ui \"the session ended\"' EXIT HUP INT TERM\n\
         sync\necho 3 > /proc/sys/vm/drop_caches\n\
         echo \"takeover: One UI is down while the VM runs; memory $(mem_line)\"\n\
         need={need_pages}\n\
         avail=$(grep '^pool_avail=' $P/refill_stat | cut -d= -f2)\n\
         if [ \"${{avail:-0}}\" -lt $need ]; then\n  \
         echo \"pool: $avail pages free, $need wanted: topped up while One UI is down (the module's acquire, as hugepool grow)\"\n  \
         want=$(grep '^pool_want=' $P/refill_stat | cut -d= -f2); [ \"${{want:-0}}\" -ge $need ] || echo $need > $P/pool_want\n  \
         echo 3 > $P/acquire; sleep 3\n  \
         t=0; while [ $t -lt 90 ] && ! grep -q '^acquire_active=0$' $P/refill_stat; do sleep 1; t=$((t + 1)); done\n  \
         [ $t -lt 90 ] || {{ echo 0 > $P/acquire; sleep 2; }}\n  \
         avail=$(grep '^pool_avail=' $P/refill_stat | cut -d= -f2)\n  \
         echo \"pool: $avail pages free after the top-up: $(stat_line)\"\n  \
         [ \"${{avail:-0}}\" -ge $need ] || {{ end \"refused pool $avail of $need pages after a top-up\"; exit 1; }}\n\
         fi\n\
         # Samsung's own limit (its cpufreq_limit driver, /sys/devices/system/cpu/cpufreq_limit): One UI's services ask it for a\n\
         # virtual top clock that its table turns into each cluster's cap (1958400: 1.96 GHz prime, 2.0 GHz perf, take3), and it\n\
         # stays when they are gone; -1 is none. Thermal cooling and Qualcomm's own limits (dcvsh) are not this and stay.\n\
         SL=/sys/devices/system/cpu/cpufreq_limit/cpufreq_max_limit\nsl_old=\n\
         if [ -w $SL ]; then\n  \
         sl_old=$(cat $SL)\n  \
         [ \"$sl_old\" = -1 ] || {{ echo -1 > $SL && echo \"cpu: Samsung's cpufreq_limit max $sl_old lifted (now $(cat $SL)) while One UI is down\"; }}\n\
         fi\n\
         CF=/sys/devices/system/cpu/cpufreq\ncaps=\n\
         caps_restore() {{\n  \
         [ -z \"$sl_old\" ] || [ \"$sl_old\" = -1 ] || {{ echo \"$sl_old\" > $SL && echo \"cpu: Samsung's cpufreq_limit max is $(cat $SL) again\"; }}\n  \
         sl_old=\n  [ -n \"$caps\" ] || return 0\n  \
         for pm in $caps; do echo \"${{pm#*=}}\" > $CF/policy${{pm%%=*}}/scaling_max_freq; done\n  \
         echo \"cpu: the clusters' caps are back:$caps\"\n  caps=\n}}\n\
         for p in 0 1 2 3 4 5 6 7; do\n  \
         f=$CF/policy$p\n  [ -w $f/scaling_max_freq ] && [ -r $f/cpuinfo_max_freq ] || continue\n  \
         old=$(cat $f/scaling_max_freq); top=$(cat $f/cpuinfo_max_freq)\n  [ \"$old\" = \"$top\" ] && continue\n  \
         echo $top > $f/scaling_max_freq && caps=\"$caps $p=$old\" && \
         echo \"cpu: policy$p's cap is lifted to $(cat $f/scaling_max_freq) kHz while One UI is down (it was $old)\"\n\
         done\n",
        crate::hugepool::ui_stop()
    );
    (stop, "caps_restore\n".to_string())
}

#[allow(clippy::too_many_arguments)]
pub fn boot_script_with(l: &Label, seconds: u32, thp: bool, sha: &str, disks: Option<&Disks>, swipe: bool, pin: bool, t: &Tuning) -> String {
    let (gpu_floor, gpu_restore) = gpu_floor_script(l, t.gpu_floor_mhz);
    let (cpu_floor, cpu_restore) = cpu_floor_script(t.cpu_floor_percent);
    // the pool pages the VM's RAM takes, the shared-RAM VM's allowance outside it aside (check_pool)
    let need = (u64::from(l.ram).saturating_sub(SMALL_PAGES_MIB_SHARED) + 1) / 2;
    let (takeover_stop, caps_restore) = takeover_script(t.takeover, need);
    let thermal_log = thermal_log_script(t.thermal_log);
    let (mem, cpus, swiotlb) = (l.ram, l.cpus, l.swiotlb);
    // Droid-VM's crosvm and its shared-RAM VM, or AVF's and its protected one with the restricted DMA pool
    let (crosvm, protection) = if l.shared {
        (format!("{}/crosvm", crate::droidvm::DIR), "--protected-vm-pseudo-unprotected".to_string())
    } else {
        ("/apex/com.android.virt/bin/crosvm".to_string(), format!("--protected-vm-without-firmware --swiotlb {swiotlb}"))
    };
    let share_check = if l.shared {
        "[ -c /dev/gunyah_share ] || { echo \"no /dev/gunyah_share: oneios-tool phone hostshare load\"; end \"refused no gunyah_share\"; exit 1; }\n\
         [ -x $C ] || { echo \"no $C: --droidvm\"; end \"refused no crosvm\"; exit 1; }\n"
    } else {
        ""
    };
    let affinity = if pin {
        format!(" --cpu-affinity {}", (0..cpus).map(|i| format!("{i}={}", i + t.pin_first)).collect::<Vec<_>>().join(":"))
    } else {
        String::new()
    };
    let mut checks = String::new();
    let mut blocks = String::new();
    let mut pool_check = String::new();
    let (mut bridge_start, mut bridge_end) = (String::new(), String::new());
    // the Wi-Fi in a takeover (One UI keeps wlan0 otherwise): wifictl tap before crosvm, given back after it
    let (wifi_start, wifi_end) = if t.takeover && disks.is_some_and(|d| d.wifi) {
        (WIFI_START.replace("{tap}", WIFI_TAP), "wifi_restore\n".to_string())
    } else {
        (String::new(), String::new())
    };
    let (mut touch_find, mut escape_start, mut escape_end) = (String::new(), String::new(), String::new());
    if disks.is_some_and(|d| d.input) {
        // The touchscreen by its name (and INPUT_PROP_DIRECT), never a side key's device (rule: the way back to One UI)
        touch_find = concat!(
            "dev_of() {\n",
            "  awk -v want=\"N: Name=\\\"$1\\\"\" 'BEGIN { RS = \"\" } index($0, want \"\\n\") && (p == \"\" || index($0, \"B: PROP=\" p \"\\n\")) {\n",
            "    if (match($0, /Handlers=[^\\n]*/)) { h = substr($0, RSTART, RLENGTH); if (match(h, /event[0-9]+/)) print substr(h, RSTART, RLENGTH) } }' p=\"$2\" /proc/bus/input/devices\n",
            "}\n",
            "tdev=$(dev_of sec_touchscreen 2)\n",
            "keys=\" $(dev_of gpio-keys | tr '\\n' ' ')$(dev_of pmic_pwrkey | tr '\\n' ' ')$(dev_of pmic_resin | tr '\\n' ' ')\"\n",
            "case \"$tdev\" in event[0-9]|event[0-9][0-9]) ;; *) tdev= ;; esac\n",
            "case \"$keys\" in *\" $tdev \"*) [ -n \"$tdev\" ] && echo \"touch: $tdev is a side key's device: not given to the VM\"; tdev= ;; esac\n",
            "if [ -n \"$tdev\" ] && [ -c /dev/input/$tdev ]; then\n",
            "  echo \"touch: /dev/input/$tdev (sec_touchscreen) goes to the VM while it runs\"\n",
            "else\n",
            "  tdev=; echo \"touch: no sec_touchscreen with INPUT_PROP_DIRECT: the VM runs without touch\"\n",
            "fi\n",
            // The forwarder listens before crosvm starts: crosvm connects to its socket while it makes its devices
            "tsock=\nforward=\n",
            "if [ -n \"$tdev\" ] && [ -x $D/displayctl ]; then\n",
            "  rm -f $D/touch.sock\n",
            "  $D/displayctl touch --evdev /dev/input/$tdev --socket $D/touch.sock </dev/null >>$D/touch.txt 2>&1 &\n",
            "  forward=$!\n",
            "  t=0; while [ $t -lt 50 ] && [ ! -S $D/touch.sock ] && kill -0 $forward 2>/dev/null; do sleep 0.1 2>/dev/null || sleep 1; t=$((t + 1)); done\n",
            "  if [ -S $D/touch.sock ]; then\n",
            "    tsock=$D/touch.sock; echo \"touch: the forwarder ($forward) takes it when crosvm connects to $tsock: touch.txt\"\n",
            "  else\n",
            "    echo \"touch: the forwarder did not start (touch.txt): the VM runs without touch\"; kill $forward 2>/dev/null; forward=\n",
            "  fi\n",
            "fi\n"
        )
        .to_string();
        // Volume down held 10 s: stop.request (getevent reads pmic_resin's events and takes nothing; in a takeover the VM gets
        // the key too, as its volume down, so a press for the volume must not stop it: 3 s did, 10 s is far past any of those)
        escape_start = concat!(
            "escape=\n",
            "vd=$(dev_of pmic_resin)\n",
            "if [ -n \"$tsock\" ] && [ -n \"$vd\" ] && [ -c /dev/input/$vd ]; then\n",
            "  ( getevent -l /dev/input/$vd 2>/dev/null | while read -r a b c e; do\n",
            "      case \"$a $b $c $e\" in\n",
            "        *KEY_VOLUMEDOWN*DOWN*) down=$(cut -d. -f1 /proc/uptime) ;;\n",
            "        *KEY_VOLUMEDOWN*UP*) now=$(cut -d. -f1 /proc/uptime)\n",
            "          if [ -n \"$down\" ] && [ $((now - down)) -ge 10 ]; then echo \"touch: volume down held $((now - down)) s: the VM stops\"; touch $D/stop.request; fi\n",
            "          down= ;;\n",
            "      esac\n",
            "    done ) &\n",
            "  escape=$!\n",
            "  echo \"touch: volume down held 10 s stops the VM (the way back without the PC)\"\n",
            "fi\n"
        )
        .to_string();
        // After the VM: the forwarder sees crosvm gone and gives the touchscreen back; one still there 3 s on is ended
        escape_end = concat!(
            "if [ -n \"$escape\" ]; then kill $escape 2>/dev/null; pkill -f \"getevent -l /dev/input/$vd\" 2>/dev/null; wait $escape 2>/dev/null; fi\n",
            "if [ -n \"$forward\" ]; then\n",
            "  t=0; while [ $t -lt 3 ] && kill -0 $forward 2>/dev/null; do sleep 1; t=$((t + 1)); done\n",
            "  kill $forward 2>/dev/null; wait $forward 2>/dev/null\n",
            "  echo \"touch: $(grep '^displayctl touch:' $D/touch.txt | tail -n 1)\"\n",
            "fi\n"
        )
        .to_string();
    }
    if let Some((d, f)) = disks.and_then(|d| d.fb.as_ref().map(|f| (d, f))) {
        // The screen, once crosvm's tree shows the pool where the Image put it: the bridge maps only the frame and its
        // count of the pool (displayctl pool), the helper is the staged runtime's
        bridge_start = format!(
            "bridge=\nif [ \"$pool\" != bad ] && kill -0 $pid 2>/dev/null; then\n  \
             if [ -x $D/displayctl ] && [ -x $H ]; then\n    \
             TMPDIR=$D $D/displayctl pool --pid $pid --pool-offset {po} --pool-bytes {pb} --fb-offset {fo} --size {w}x{h} --stride {s} \
             --seq-offset {sq}{vm} --helper $H </dev/null >>$D/display.txt 2>&1 &\n    \
             bridge=$!\n    echo \"display: the bridge ($bridge) shows the VM's screen on a layer over One UI: display.txt\"\n  \
             else\n    echo \"display: no $D/displayctl or no $H: the VM runs without its screen\"\n  fi\nfi\n",
            vm = if l.shared { " --vm shared" } else { "" },
            po = d.pool - RAM_BASE,
            pb = (d.swiotlb as u64) << 20,
            fo = f.offset,
            w = f.width,
            h = f.height,
            s = f.stride,
            sq = f.seq
        );
        // The screen through crosvm's own display instead (--gpu-scanout): the helper hands its layer's Surface to the
        // service crosvm registers, and crosvm draws the VM's frames there; it ends when crosvm's service goes
        if let Some((w, h, hz)) = d.scanout {
            bridge_start = format!(
                "bridge=\nif [ \"$pool\" != bad ] && kill -0 $pid 2>/dev/null; then\n  \
                 if [ -x $H ]; then\n    \
                 $H --crosvm-display {CROSVM_DISPLAY} --size {w}x{h} --hz {hz} --wait 120{tk} </dev/null >>$D/display.txt 2>&1 &\n    \
                 bridge=$!\n    echo \"display: the helper ($bridge) gives crosvm a layer over One UI for the VM's screen: display.txt\"\n  \
                 else\n    echo \"display: no $H: the VM runs without its screen\"\n  fi\nfi\n",
                tk = if t.takeover { " --takeover --screen-file $D/screen.off" } else { "" }
            );
        }
        // After the VM: the bridge sees crosvm gone and closes its session (the layer goes); a bridge still there after
        // 3 s is asked to end, and after 8 s ended
        bridge_end = "if [ -n \"$bridge\" ]; then\n  \
             t=0; while [ $t -lt 8 ] && kill -0 $bridge 2>/dev/null; do [ $t = 3 ] && kill -TERM $bridge 2>/dev/null; sleep 1; t=$((t + 1)); done\n  \
             kill -0 $bridge 2>/dev/null && kill -9 $bridge 2>/dev/null\n  wait $bridge 2>/dev/null\n  \
             echo \"display: $(grep -E '^displayctl pool: (the VM ended|time is up|stopped|the helper left)' $D/display.txt | tail -n 1)\"\nfi\n"
            .to_string();
    }
    if let Some(d) = disks {
        if let Some(g) = &d.gpu {
            // the GPU: Droid-VM's virglrenderer with drm2kgsl in crosvm's process, guest blobs over the guest's pages
            // (udmabuf); a small host-visible window, which nothing here uses; drm2kgsl's host arena
            // With the scanout the virgl renderer runs too (virgl2): the kernel's scanout resources are context-less blobs of
            // guest memory, which crosvm's virglrenderer refuses (EINVAL, "ComponentError(-22)") unless that renderer is up --
            // Droid-VM's deploy/SETUP.md says the same of its drm2kgsl route ("virgl2 必須在", else a black screen).
            let (contexts, display) = match d.scanout {
                Some((w, h, hz)) => ("virgl2:drm,egl=true,gles=true", format!(",displays=[[mode=windowed[{w},{h}],refresh-rate={hz}]]")),
                None => ("drm", String::new()),
            };
            blocks += &format!(
                " \\\n  --gpu backend=virglrenderer,context-types={contexts},udmabuf=true,pci-address={},pci-bar-size=0x10000000{display} \\\n  --pre-alloc drm-host-mb={}",
                g.pci, g.drm_mib
            );
            if d.scanout.is_some() {
                blocks += &format!(" \\\n  --android-display-service name={CROSVM_DISPLAY},screen=gpu-0");
            }
        }
        if d.audio {
            // the speaker: crosvm's virtio-snd card played through AAudio (the guest's 48 kHz S16 periods, iOS's own volume
            // applied in the guest), no capture. Its backend in crosvm's process unless --audio-uid: Droid-VM's crosvm says a
            // stream of uid 0 is silenced while One UI runs; a takeover stops the framework, and the upstream port's root
            // AAudio helper played in its sessions so
            blocks += &format!(" \\\n  --virtio-snd backend=aaudio,capture=false{}", t.audio_uid.map(|u| format!(",uid={u}")).unwrap_or_default());
        }
        if d.wifi && t.takeover {
            // the Wi-Fi: crosvm's virtio-net card on the tap wifictl made, with wlan0's address (WIFI_START); none when
            // wifictl did not take wlan0
            blocks += &format!(" \\\n  ${{wifi_mac:+--net tap-name={WIFI_TAP},mac=$wifi_mac}}");
        }
        for (size, value) in &d.blocks {
            blocks += &format!(" \\\n  --block {value}");
            let path = value.strip_prefix("path=").and_then(|v| v.split(',').next()).unwrap_or("");
            if let Some(size) = size {
                checks += &format!(
                    "s=$(stat -c %s {path} 2>/dev/null); [ \"$s\" = {size} ] || {{ echo \"disk {path}: '$s' bytes, not {size}\"; end \"refused disk\"; exit 1; }}\n"
                );
            }
        }
        let store = &d.store;
        let parent = store.rsplit_once('/').map(|(p, _)| p).unwrap_or("/data");
        checks += &format!(
            "if [ ! -f {store} ]; then\n  mkdir -p {parent} && truncate -s {gib}G {store} || {{ echo \"store: cannot make {store}\"; end \"refused store\"; exit 1; }}\n  \
             echo \"store: a new {store} ({gib} GiB, sparse): the kernel formats it and unpacks the seed\"\nfi\n\
             avail=$(df -k /data | awk 'NR==2 {{print $4}}')\n\
             [ \"${{avail:-0}}\" -ge {DATA_FREE_KIB} ] || {{ echo \"/data: '$avail' KiB free, {DATA_FREE_KIB} needed\"; end \"refused space\"; exit 1; }}\n\
             echo \"disks: {n}, the store {store} ($(du -k {store} | cut -f1) KiB used); /data $avail KiB free\"\n",
            gib = d.store_gib,
            n = d.blocks.len()
        );
        // The pool's node, as crosvm names it (aarch64/src/fdt.rs: the address in lowercase hex, no 0x); a shared-RAM
        // VM has none, and its tree's shim_handoff node says crosvm made that kind
        // (read once the file is whole: its size is the header's totalsize; a check of a file crosvm was still writing found no
        // shim_handoff node and stopped a good 4 GiB VM, full1)
        pool_check = if l.shared {
            "fdt_whole() { set -- $(od -An -tu1 -j4 -N4 $D/fdt.dtb 2>/dev/null); [ $# = 4 ] && [ $(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 )) = \"$(wc -c < $D/fdt.dtb | tr -d ' ')\" ]; }\nt=0\nwhile [ $t -lt 100 ] && ! fdt_whole && kill -0 $pid 2>/dev/null; do sleep 0.1 2>/dev/null || sleep 1; t=$((t + 1)); done\n\
             sleep 0.1 2>/dev/null\n\
             if kill -0 $pid 2>/dev/null; then\n  \
             if grep -q -F shim_handoff@ $D/fdt.dtb; then echo \"pool: crosvm's shared-RAM VM (its tree has the shim's handoff page): the disks' windows are plain RAM\"; \
             else echo \"pool: crosvm's tree has no shim_handoff node: not the shared-RAM VM, stopped before the kernel's disks start\"; pool=bad; touch $D/stop.request; fi\n\
             {drm}fi\n"
                .replace(
                    "{drm}",
                    &d.gpu
                        .as_ref()
                        .map(|g| {
                            format!(
                                "  if [ \"$pool\" != bad ]; then if grep -q -F drm2kgsl_host@{x:x} $D/fdt.dtb; then echo \"gpu: drm2kgsl's host arena is at 0x{x:x}, as the Image's drm-host-pool\"; \
                                 else echo \"gpu: crosvm's tree has no drm2kgsl_host@{x:x}: stopped before the kernel's GPU starts\"; pool=bad; touch $D/stop.request; fi; fi\n",
                                x = g.drm_host
                            )
                            + &d.gpu_intid.map(|n| gpu_intid_check(&g.pci, n)).unwrap_or_default()
                        })
                        .unwrap_or_default(),
                )
        } else {
            format!(
            "fdt_whole() {{ set -- $(od -An -tu1 -j4 -N4 $D/fdt.dtb 2>/dev/null); [ $# = 4 ] && [ $(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 )) = \"$(wc -c < $D/fdt.dtb | tr -d ' ')\" ]; }}\nt=0\nwhile [ $t -lt 100 ] && ! fdt_whole && kill -0 $pid 2>/dev/null; do sleep 0.1 2>/dev/null || sleep 1; t=$((t + 1)); done\n\
             sleep 0.1 2>/dev/null\n\
             if kill -0 $pid 2>/dev/null; then\n  \
             if grep -q -F restricted_dma_reserved@{pool:x} $D/fdt.dtb; then echo \"pool: crosvm's restricted DMA pool is at 0x{pool:x}, as the disks' windows\"; \
             else echo \"pool: crosvm's tree has no restricted_dma_reserved@{pool:x}: the VM is stopped before the kernel's disks start\"; pool=bad; touch $D/stop.request; fi\n\
             fi\n",
                pool = d.pool
            )
        };
        if swipe {
            touch_find = touch_find.replace("--socket $D/touch.sock </dev/null", "--socket $D/touch.sock --swipe-at 200 --swipe-every 60 </dev/null");
        }
        // In a takeover the side keys go to the VM too (read, never grabbed: One UI is down) and a click of power switches the
        // panel: displayctl touch makes screen.off, the helper switches the panel by it (OneiOSDisplay --screen-file)
        if t.takeover {
            touch_find = touch_find
                .replace("tsock=\nforward=\n", "tsock=\nforward=\nkl=; for k in $keys; do kl=\"${kl:+$kl,}/dev/input/$k\"; done\n")
                .replace(" </dev/null >>$D/touch.txt 2>&1 &\n", "${kl:+ --keys $kl --screen-file $D/screen.off} </dev/null >>$D/touch.txt 2>&1 &\n");
        }
    }
    // crosvm's Android display blits each frame into the helper's Surface on the GPU with the Vulkan driver it is named (it
    // loads none by itself, and does a CPU copy per frame without one): Droid-VM's Turnip of the package (droidvm::FILES)
    let crosvm_env = if disks.is_some_and(|d| d.scanout.is_some()) {
        format!("CROSVM_DISPLAY_VULKAN_LIBRARY={}/libvulkan_freedreno.so ", crate::droidvm::DIR)
    } else {
        String::new()
    };
    format!(
        r#"D={DIR}
C={crosvm}
H=$D/oneios-display
exec >>$D/run.txt 2>&1 </dev/null
cd $D || exit 1
end() {{ echo "$*" > $D/result; }}
got=$(sha256sum kernel.img | cut -d' ' -f1)
[ "$got" = "{sha}" ] || {{ echo "sha256 $got, not {sha}"; end "refused sha256"; exit 1; }}
[ -c /dev/gunyah ] || {{ echo "no /dev/gunyah"; end "refused no /dev/gunyah"; exit 1; }}
{share_check}
tdev=
tsock=
{checks}rm -f serial.txt crosvm.log crosvm.sock fdt.dtb dmesg.txt display.txt touch.txt touch.sock stop.request screen.off wifi.txt wifi.mac
{touch_find}
ulimit -l unlimited 2>/dev/null || echo "ulimit -l unlimited: refused"
# crosvm holds a file for every GPU buffer of the guest: at 32768 (the shell's) it ran out after 11 minutes of iOS (full2:
# 43727 buffers made, "Too many open files" on 580 of them and 243 GPU submissions, then the guest's kernel panicked)
ulimit -n 1048576 2>/dev/null || ulimit -n $(ulimit -Hn) 2>/dev/null; echo "files: crosvm may hold $(ulimit -n) open files"
T=/sys/kernel/mm/transparent_hugepage
thp_done=1
thp_restore() {{
  [ "$thp_done" = 0 ] || return 0
  thp_done=1
  echo "$old_shmem" > $T/shmem_enabled
  echo "$old_defrag" > $T/defrag
  echo "thp restored: shmem_enabled $(cat $T/shmem_enabled) | defrag $(cat $T/defrag)"
}}
if [ {thp} = 1 ]; then
  old_shmem=$(tr ' ' '\n' < $T/shmem_enabled | grep '^\[' | tr -d '\133\135')
  old_defrag=$(tr ' ' '\n' < $T/defrag | grep '^\[' | tr -d '\133\135')
  case "$old_shmem" in always|within_size|advise|never|deny|force) ;; *) echo "thp: shmem_enabled '$old_shmem'?"; end "refused thp"; exit 1 ;; esac
  case "$old_defrag" in always|defer|defer+madvise|madvise|never) ;; *) echo "thp: defrag '$old_defrag'?"; end "refused thp"; exit 1 ;; esac
  thp_done=0
  trap thp_restore EXIT HUP INT TERM
  if ! {{ echo advise > $T/shmem_enabled && echo madvise > $T/defrag; }}; then
    echo "thp: the write was refused"; thp_restore; end "refused thp"; exit 1
  fi
  echo "thp set: shmem_enabled $(cat $T/shmem_enabled) | defrag $(cat $T/defrag)"
fi
grep -E '^(MemAvailable|SwapFree):' /proc/meminfo | tr -s ' ' | sed 's/^/before /'
echo "start $(date +%s) up $(cut -d' ' -f1 /proc/uptime)"
{takeover_stop}{gpu_floor}{cpu_floor}{wifi_start}pool=
# crosvm logs at info: at debug its GPU renderer wrote a line per submission and fence (take8: 407 MB in 45 minutes,
# whose copy at the end broke the adb link and left the session's other files empty)
set -f
{crosvm_env}$C --log-level info run --disable-sandbox --no-balloon --no-rng --no-usb --hypervisor gunyah \
  {protection} --hugepages --mem size={mem} --cpus num-cores={cpus}{affinity} \
  --socket $D/crosvm.sock${{tsock:+ --input multi-touch[path=$tsock,width=4095,height=4095,name=oneios-touch]}} --serial type=file,hardware=serial,num=1,path=$D/serial.txt,console \
  --dump-device-tree-blob $D/fdt.dtb{blocks} $D/kernel.img </dev/null >$D/crosvm.log 2>&1 &
pid=$!
set +f
{pool_check}{bridge_start}{escape_start}{thermal_log}t=0
while [ $t -lt {seconds} ] && [ ! -e $D/stop.request ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
how=itself
if kill -0 $pid 2>/dev/null; then
  how=stop; $C stop $D/crosvm.sock </dev/null >/dev/null 2>&1
  t=0; while [ $t -lt 20 ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
fi
if kill -0 $pid 2>/dev/null; then how=kill; kill -9 $pid; fi
wait $pid; rc=$?
thp_restore
{wifi_end}{gpu_restore}{cpu_restore}{caps_restore}{bridge_end}{escape_end}echo "end $(date +%s) up $(cut -d' ' -f1 /proc/uptime) how $how exit $rc"
dmesg 2>/dev/null | grep -i -E 'gunyah|folio|gh_|crosvm|scm|memextent' | tail -n 80 > $D/dmesg.txt
if [ "$pool" = bad ]; then end "refused pool: end how $how exit $rc"; else end "end how $how exit $rc"; fi
"#,
        thp = if thp { 1 } else { 0 }
    )
}

/// The tap wifictl makes for crosvm's network card.
pub const WIFI_TAP: &str = "oneios0";

/// The Wi-Fi's start in boot.sh (an Image of xnuloader-crosvm --wifi, in a takeover): `wifictl tap` takes wlan0 as
/// xnuhost's bridge does (its state in wifi.state), makes the persistent tap {tap} and writes wlan0's address to
/// wifi.mac, which crosvm's card gets (`--net tap-name={tap},mac=...`); it carries the frames and the station's messages
/// until crosvm lets go of the tap, then leaves the network, gives wlan0 back and removes the tap. wifi_restore ends it
/// (after crosvm, and in the takeover's trap before One UI starts); one that does not end in 15 s is killed and its
/// wifi.state given back by `wifictl restore`.
pub const WIFI_START: &str = r#"# the Wi-Fi: wlan0 is iOS's en0 for the session (wifictl tap, runtime/bridges/wifi)
wifi=; wifi_mac=
wifi_restore() {
  [ -n "$wifi" ] || return 0
  kill -TERM $wifi 2>/dev/null
  n=0; while [ $n -lt 15 ] && kill -0 $wifi 2>/dev/null; do sleep 1; n=$((n + 1)); done
  if kill -0 $wifi 2>/dev/null; then kill -9 $wifi 2>/dev/null; $D/wifictl restore $D/wifi.state >>$D/wifi.txt 2>&1; fi
  echo "wifi: $(grep -E '^tap: [0-9]+ frames' $D/wifi.txt | tail -n 1)"
  wifi=
}
if [ -x $D/wifictl ]; then
  $D/wifictl tap {tap} $D </dev/null >>$D/wifi.txt 2>&1 &
  wifi=$!
  n=0; while [ ! -s $D/wifi.mac ] && [ $n -lt 150 ] && kill -0 $wifi 2>/dev/null; do sleep 0.1; n=$((n + 1)); done
  wifi_mac=$(cat $D/wifi.mac 2>/dev/null)
  case "$wifi_mac" in
    ??:??:??:??:??:??) echo "wifi: wlan0 ($wifi_mac) is iOS's en0 for the session: wifictl tap, crosvm's card on the tap {tap}" ;;
    *) echo "wifi: off: wifictl did not take wlan0: $(tail -n 3 $D/wifi.txt | tr '\n' ' ')"; wifi_restore; wifi_mac= ;;
  esac
else
  echo "wifi: off: no $D/wifictl"
fi
"#;

fn gen_path(dir: &Path, name: &str) -> String {
    dir.join(name).to_string_lossy().into_owned()
}

/// What of a VM's RAM may be outside gh_hugepage_reserve's pool, in 4 KiB pages: Gunyah's own heap maps no more than
/// about 470 MiB of those for all VMs (vm-probe runs 3 to 8). A VM whose RAM it cannot map ends at the first fault there:
/// boot 4b, the pool at 198 pages after boot 4, "gunyah_vm_mgr: Failed to map guest address ...: 10" and crosvm's vCPU
/// "unknown gh exit reason: 3", 48 s into the run.
pub const SMALL_PAGES_MIB: u64 = 384;
/// The shared-RAM VM's: the window is pinned for good when it is shared, and the kernel moves every page of it outside
/// the pool that lies in a CMA block into 4 KiB pages first (vm-probe shared1: 4 MiB of such pages made ~1000 runs;
/// shared2: 960 MiB made 126651, which the resource manager refused to map)
pub const SMALL_PAGES_MIB_SHARED: u64 = 32;

/// A VM above 480 MiB with no more than SMALL_PAGES_MIB (SMALL_PAGES_MIB_SHARED for the shared-RAM VM) of its RAM
/// outside the pool's free pages (`pool_mib`).
pub fn check_pool(ram_mib: u64, pool_mib: u64, shared: bool) -> Result<()> {
    let small = if shared { SMALL_PAGES_MIB_SHARED } else { SMALL_PAGES_MIB };
    if ram_mib > 480 && ram_mib.saturating_sub(pool_mib) > small {
        bail!(
            "gh_hugepage_reserve's pool has {pool_mib} MiB free, and a{} VM of {ram_mib} MiB may take no more than {small} MiB \
             outside it ({}): refill the pool first (`phone hugepool acquire`, beside One UI, or `phone hugepool grow`)",
            if shared { " shared-RAM" } else { "" },
            if shared { "the rest is moved at its pin and scattered" } else { "4 KiB pages, Gunyah's heap" }
        );
    }
    Ok(())
}

pub struct Options {
    pub seconds: u32,
    pub thp: bool,
    /// displayctl for the phone, pushed when the Image has a screen
    pub bridge: String,
    /// The touch forwarder's test swipes (boot_script_with)
    pub swipe_test: bool,
    /// vCPU N on core N (boot_script_with)
    pub pin: bool,
    /// Droid-VM's crosvm package on the PC (scripts/droidvm/fetch-crosvm.sh), pushed for a shared-RAM Image
    pub droidvm: String,
    /// The GPU's lowest clock while an Image with the GPU runs, MHz (gpu_floor_script; 0: the governor's alone)
    pub gpu_floor_mhz: u32,
    /// The CPUs' lowest clocks while the VM runs, percent of each cluster's top (cpu_floor_script; 0: the governor's alone)
    pub cpu_floor_percent: u32,
    /// One UI's framework stopped for the VM's life (takeover_script)
    pub takeover: bool,
    /// With pin: the first core of the vCPUs (Tuning::pin_first)
    pub pin_first: u32,
    /// thermal.txt (Tuning::thermal_log)
    pub thermal_log: bool,
    /// The sound card's backend uid (Tuning::audio_uid)
    pub audio_uid: Option<u32>,
    /// wifictl for the phone (runtime/bridges: make), pushed for an Image with the Wi-Fi in a takeover
    pub wifictl: String,
}

/// `phone vm-boot --stop`: ask the running VM's script to stop it now (crosvm stop through its socket).
pub fn stop(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    adb.root_ok(&format!("[ -d {DIR} ] && touch {DIR}/stop.request; echo asked\n"), "the stop")?;
    println!("vm-boot: asked the VM's script to stop it (it uses crosvm stop)");
    Ok(())
}

/// `phone vm-boot`.
pub fn vm_boot(adb: &Adb, image: &str, out: &str, o: &Options) -> Result<()> {
    let data = fs::read(image).with_context(|| format!("cannot read {image} (runtime/loader's xnuloader-crosvm makes it)"))?;
    let l = label(&data).context("no `oneios-crosvm ram-mib=N cpus=N` label at 0x100: not an Image of xnuloader-crosvm")?;
    if !(5..=3600).contains(&o.seconds) || !(1..=8).contains(&l.cpus) || !(2..=1024).contains(&l.swiotlb) {
        bail!("--seconds 5..3600; the Image's vCPUs 1..8, its pool 2..1024 MiB");
    }
    if l.ram > 480 && !o.thp {
        bail!("a VM of {} MiB needs --thp and gh_hugepage_reserve's pool (Gunyah's own memory ends near 0.5 GB of 4 KiB pages)", l.ram);
    }
    let list = format!("{image}.disks");
    let disks = match fs::read_to_string(&list) {
        Ok(text) => Some(parse_disks(&text).with_context(|| format!("{list}"))?),
        Err(_) => None,
    };
    if let Some(d) = &disks {
        // --gpu: drm2kgsl's host arena at the RAM's top, the pool below it
        let drm = d.gpu.as_ref().map(|g| g.drm_mib << 20).unwrap_or(0);
        let pool = RAM_BASE + ((l.ram as u64) << 20) - drm - ((l.swiotlb as u64) << 20);
        if d.gpu.is_some() != l.gpu || (l.gpu && !l.shared) {
            bail!("{list} and the Image's label disagree about the GPU (or a GPU without the shared RAM): make both again");
        }
        if d.swiotlb != l.swiotlb || d.pool != pool {
            bail!("{list} names a pool of {} MiB at 0x{:x}, the Image's label {} MiB at 0x{pool:x}: make both again", d.swiotlb, d.pool, l.swiotlb);
        }
    }
    let sha = format!("{:x}", Sha256::digest(&data));
    adb.need_root()?;
    adb.refuse_in_session()?;
    if disks.as_ref().is_some_and(|d| d.fb.is_some()) && !Path::new(&o.bridge).is_file() {
        bail!("the Image has a screen and {} is not there: make -C runtime/display (or name it with --bridge)", o.bridge);
    }
    let up = adb.root_ok(
        "ps -A -o PID=,ARGS= | grep -F -e 'oneios-vm' -e 'displayctl pool' | grep -F -e crosvm -e displayctl | grep -v grep || true\n",
        "the VM check",
    )?;
    if !up.trim().is_empty() {
        bail!("a VM of ours is still up on the phone: {}", up.trim());
    }
    let meminfo = adb.root_ok(
        "grep '^MemAvailable:' /proc/meminfo; grep '^pool_avail=' /sys/module/gh_hugepage_reserve/parameters/refill_stat 2>/dev/null\n",
        "the memory check",
    )?;
    let available = crate::vmprobe::mem_available_mib(&meminfo).with_context(|| format!("no MemAvailable in \"{}\"", meminfo.trim()))?;
    let pool_mib = crate::hugepool::stat(&meminfo, "pool_avail").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0) * 2;
    crate::vmprobe::check_mem(l.ram, available + pool_mib.min(l.ram as u64))?;
    // A takeover tops the pool up itself once One UI is down (takeover_script); beside One UI it must hold the VM already
    match check_pool(l.ram as u64, pool_mib, l.shared) {
        Err(e) if o.takeover => println!("vm-boot: {e:#}\nvm-boot: the takeover tops the pool up while One UI is down, and stops if it cannot"),
        r => r?,
    }
    if l.shared {
        let dev = adb.root_ok(&format!("[ -c {} ] && echo there || echo absent\n", crate::hostshare::DEV), "gunyah_share")?;
        if !dev.contains("there") {
            bail!("a shared-RAM Image and no {} on the phone: `phone hostshare load` first", crate::hostshare::DEV);
        }
        let c = crate::droidvm::push(adb, &o.droidvm)?;
        println!("vm-boot: a shared-RAM Image: Droid-VM's crosvm ({c}), --protected-vm-pseudo-unprotected");
    }
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR} && rm -f {DIR}/result {DIR}/run.txt {DIR}/serial.txt\n"), "the VM's folder")?;
    println!("vm-boot: {} MiB Image, sha256 {sha}: pushing", data.len() >> 20);
    adb.push(image, &format!("{DIR}/kernel.img"))?;
    if let Some(f) = disks.as_ref().and_then(|d| d.fb.as_ref()) {
        // The bridge and the display helper the VM runs, as built beside each other (runtime/display: make): the VM's
        // own copy, root's, so that neither depends on the staged runtime's version nor can be changed by the shell
        // user while root runs them.
        let built = Path::new(&o.bridge).parent().unwrap_or(Path::new("."));
        for (file, to) in [(o.bridge.clone(), "displayctl"), (gen_path(built, "oneios-display"), "oneios-display"),
                           (gen_path(built, "liboneios-display.so"), "liboneios-display.so"), (gen_path(built, "oneios-display.dex"), "oneios-display.dex")] {
            if !Path::new(&file).is_file() {
                bail!("{file} is not there: make -C runtime/display");
            }
            adb.push(&file, &format!("{DIR}/{to}"))?;
        }
        adb.root_ok(
            &format!(
                "cd {DIR} && chown root:root displayctl oneios-display liboneios-display.so oneios-display.dex && \
                 chmod 755 displayctl oneios-display && chmod 644 liboneios-display.so oneios-display.dex\n"
            ),
            "the bridge's and the helper's owner",
        )?;
        if disks.as_ref().is_some_and(|d| d.wifi) {
            if o.takeover {
                if !Path::new(&o.wifictl).is_file() {
                    bail!("{} is not there: runtime/build.sh (or make -C runtime/bridges build/android/wifictl), or name it with --wifictl", o.wifictl);
                }
                adb.push(&o.wifictl, &format!("{DIR}/wifictl"))?;
                adb.root_ok(&format!("cd {DIR} && chown root:root wifictl && chmod 755 wifictl\n"), "wifictl's owner")?;
                println!("vm-boot: the Wi-Fi: wlan0 is iOS's en0 for the session (wifictl tap; the phone's Wi-Fi comes back with One UI)");
            } else {
                println!("vm-boot: the Image has the Wi-Fi, but without --takeover One UI keeps wlan0: the VM runs without it");
            }
        }
        if disks.as_ref().is_some_and(|d| d.input) {
            println!(
                "vm-boot: the screen ({}x{}) on a layer over One UI, the touchscreen to the VM while it runs; volume down held 10 s stops it; in a takeover the side keys go to iOS too (power: the panel and the lock)",
                f.width, f.height
            );
        } else {
            println!("vm-boot: the screen ({}x{}) on a layer over One UI while the VM runs (touches still go to One UI under it)", f.width, f.height);
        }
    }
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let script = Path::new(out).join("boot.sh");
    if o.pin && o.pin_first + l.cpus > 8 {
        bail!("--pin-first {} with {} vCPUs: past the S25's 8 cores", o.pin_first, l.cpus);
    }
    if o.swipe_test && !disks.as_ref().is_some_and(|d| d.input) {
        bail!("--swipe-test needs an Image with the touch (xnuloader-crosvm --display)");
    }
    fs::write(&script, boot_script_with(&l, o.seconds, o.thp, &sha, disks.as_ref(), o.swipe_test, o.pin, &Tuning { gpu_floor_mhz: o.gpu_floor_mhz, cpu_floor_percent: o.cpu_floor_percent, takeover: o.takeover, pin_first: o.pin_first, thermal_log: o.thermal_log, audio_uid: o.audio_uid }))?;
    adb.push(script.to_str().context("path")?, &format!("{DIR}/boot.sh"))?;
    if o.takeover {
        // One UI's stop and start (ui.sh, sourced by boot.sh): its failsafe gives the session 5 minutes past its seconds
        let ui = Path::new(out).join("ui.sh");
        fs::write(&ui, ui_script(o.seconds + 300))?;
        adb.push(ui.to_str().context("path")?, &format!("{DIR}/ui.sh"))?;
        adb.root_ok(&format!("chown root:root {DIR}/ui.sh && chmod 755 {DIR}/ui.sh\n"), "ui.sh's owner")?;
        println!(
            "vm-boot: takeover: One UI's framework stops while the VM runs and starts again when it ends (volume down 10 s, \
             --stop, its seconds, or any end of the script); its failsafe starts it after {} s whatever happens",
            o.seconds + 300
        );
    }
    let r = adb.root(&crate::adb::detached_start(&format!("{DIR}/boot.sh")))?;
    if !r.out.contains("started") {
        bail!("the VM's script did not start (status {}): {}", r.status, r.err.trim());
    }
    match &disks {
        Some(d) => println!(
            "vm-boot: crosvm runs it: {} MiB, {} vCPU(s), {} disk(s) (the store {}), {} s at most (`phone vm-boot --stop` ends it sooner); its console:",
            l.ram, l.cpus, d.blocks.len(), d.store, o.seconds
        ),
        None => println!(
            "vm-boot: crosvm runs it: {} MiB, {} vCPU(s), no disk, {} s at most (`phone vm-boot --stop` ends it sooner); its console:",
            l.ram, l.cpus, o.seconds
        ),
    }
    let t0 = Instant::now();
    let mut shown = 0u64;
    let result = loop {
        std::thread::sleep(Duration::from_secs(2));
        if t0.elapsed() > Duration::from_secs(o.seconds as u64 + 120) {
            bail!("no end after {} s: read {DIR}/run.txt", t0.elapsed().as_secs());
        }
        let r = match adb.root(&format!(
            "tail -c +{} {DIR}/serial.txt 2>/dev/null | head -c 262144; echo; echo '=== oneios result'; cat {DIR}/result 2>/dev/null; true\n",
            shown + 1
        )) {
            Ok(r) if r.ok() => r,
            _ => continue,
        };
        let (new, res) = r.out.rsplit_once("\n=== oneios result\n").unwrap_or((r.out.as_str(), ""));
        // the trailing newline the script adds after `head`
        let new = new.strip_suffix('\n').unwrap_or(new);
        if !new.is_empty() {
            print!("{new}");
            std::io::stdout().flush().ok();
            shown += new.len() as u64;
        }
        if !res.trim().is_empty() {
            break res.trim().to_string();
        }
    };
    println!();
    for name in ["serial.txt", "crosvm.log", "dmesg.txt", "run.txt", "fdt.dtb", "display.txt", "touch.txt", "thermal.txt", "wifi.txt"] {
        let path = Path::new(out).join(name);
        let mut child = adb.exec_out(&format!("su -c 'cat {DIR}/{name}'"))?;
        let mut file = fs::File::create(&path)?;
        copy(child.stdout.as_mut().context("no output from adb")?, &mut file)?;
        child.wait()?;
    }
    let run = fs::read_to_string(Path::new(out).join("run.txt")).unwrap_or_default();
    for line in run.lines().filter(|l| {
        l.starts_with("disk") || l.starts_with("store:") || l.starts_with("pool:") || l.starts_with("/data:") || l.starts_with("display:")
            || l.starts_with("touch:") || l.starts_with("gpu:")
    }) {
        println!("vm-boot: {line}");
    }
    let log = fs::read_to_string(Path::new(out).join("crosvm.log")).unwrap_or_default();
    let lines: Vec<&str> = log.lines().collect();
    println!("--- crosvm.log, last 15 of {} lines", lines.len());
    for l in &lines[lines.len().saturating_sub(15)..] {
        println!("{l}");
    }
    println!("vm-boot: {result} (files in {out})");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const DISKS: &str = "swiotlb-mib 8\npool 0x13f800000\nstore /data/local/tmp/oneios-vmboot/data.img 16\n\
        block 10359930880 path=/data/local/tmp/oneios/image/os.img,ro=true,id=os,pci-address=00:02.0\n\
        block 1267978240 path=/data/local/tmp/oneios/image/data.seed,ro=true,id=seed,pci-address=00:05.0\n\
        block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false,id=data,pci-address=00:06.0\n";

    fn image(text: &[u8]) -> Vec<u8> {
        let mut image = vec![0u8; 0x400];
        image[0x38..0x3c].copy_from_slice(b"ARM\x64");
        image[0x100..0x100 + text.len()].copy_from_slice(text);
        image
    }

    #[test]
    fn the_pool_must_hold_what_gunyah_cannot_map_in_small_pages() {
        assert!(check_pool(2880, 2688, false).is_ok());      // boot 3: 192 MiB in 4 KiB pages
        assert!(check_pool(2400, 2406, false).is_ok());      // boot 4
        assert!(check_pool(2400, 396, false).is_err());      // boot 4b's first try: the VM ended at a fault 48 s in
        assert!(check_pool(2400, 2016, false).is_ok() && check_pool(2400, 2014, false).is_err());
        assert!(check_pool(256, 0, false).is_ok());          // a small VM needs no pool
        // the shared-RAM VM: 32 MiB outside the pool at most (shared2)
        assert!(check_pool(2048, 2016, true).is_ok() && check_pool(2048, 2014, true).is_err());
        assert!(check_pool(2048, 1664, false).is_ok() && check_pool(2048, 1664, true).is_err());
    }

    #[test]
    fn the_label_gives_the_vm() {
        assert_eq!(label(&image(b"oneios-crosvm ram-mib=2048 cpus=1")), Some(Label { ram: 2048, cpus: 1, swiotlb: 8, shared: false, gpu: false }));
        assert_eq!(label(&image(b"oneios-crosvm ram-mib=3072 cpus=2 swiotlb-mib=16")), Some(Label { ram: 3072, cpus: 2, swiotlb: 16, shared: false, gpu: false }));
        let mut bad = image(b"oneios-crosvm ram-mib=2048 cpus=1");
        bad[0x100] = b'x';
        assert_eq!(label(&bad), None);
        assert_eq!(label(&bad[..0x80]), None);
    }

    #[test]
    fn the_disks_list_is_checked() {
        let d = parse_disks(DISKS).unwrap();
        assert_eq!((d.swiotlb, d.pool, d.store_gib, d.blocks.len()), (8, 0x13f800000, 16, 3));
        assert_eq!(d.blocks[0].0, Some(10359930880));
        assert_eq!(d.blocks[2].0, None);
        // a shell character, a store that is not the writable disk, a disk outside /data, two stores, no PCI address
        assert!(parse_disks(&DISKS.replace("os.img", "os.img;reboot")).is_err());
        assert!(parse_disks(&DISKS.replace("block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false", "block - path=/data/other.img,ro=false")).is_err());
        assert!(parse_disks(&DISKS.replace("path=/data/local/tmp/oneios/image/os.img", "path=/sdcard/os.img")).is_err());
        assert!(parse_disks(&DISKS.replace("block 1267978240 path=/data/local/tmp/oneios/image/data.seed,ro=true", "block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false")).is_err());
        assert!(parse_disks(&DISKS.replace(",pci-address=00:02.0", "")).is_err());
        assert!(parse_disks(&DISKS.replace("10359930880 path=/data/local/tmp/oneios/image/os.img,ro=true", "10359930880 path=/data/local/tmp/oneios/image/os.img,ro=false")).is_err());
        assert!(parse_disks("swiotlb-mib 8\npool 0x13f800000\n").is_err());
    }

    #[test]
    fn the_boot_stops_before_it_kills_and_writes_back_thp() {
        let l = Label { ram: 2048, cpus: 1, swiotlb: 8, shared: false, gpu: false };
        let s = boot_script(&l, 120, true, "ab12", None);
        assert!(s.find("$C stop $D/crosvm.sock").unwrap() < s.find("kill -9 $pid").unwrap());
        assert!(s.contains("--swiotlb 8 --hugepages --mem size=2048 --cpus num-cores=1"));
        assert!(s.contains("$D/fdt.dtb $D/kernel.img </dev/null"));
        assert!(s.contains("[ ! -e $D/stop.request ]"));
        let wait = s.find("wait $pid; rc=$?").unwrap();
        assert!(s[wait..].starts_with("wait $pid; rc=$?\nthp_restore\n"));
        assert!(s.contains("if [ 1 = 1 ]; then"));
        assert!(boot_script(&l, 30, false, "x", None).contains("if [ 0 = 1 ]; then"));
        assert!(!s.contains("/sys/power") && !s.contains("--block") && !s.contains("restricted_dma_reserved"));
    }

    #[test]
    fn the_disks_are_checked_before_crosvm_and_the_pool_right_after() {
        let d = parse_disks(DISKS).unwrap();
        let l = Label { ram: 3072, cpus: 1, swiotlb: 8, shared: false, gpu: false };
        let s = boot_script(&l, 600, true, "ab12", Some(&d));
        let crosvm = s.find("$C --log-level info run").unwrap();
        // the sizes, the store and the free space before crosvm; the store made only when missing
        let size = s.find("s=$(stat -c %s /data/local/tmp/oneios/image/os.img 2>/dev/null); [ \"$s\" = 10359930880 ]").unwrap();
        let store = s.find("if [ ! -f /data/local/tmp/oneios-vmboot/data.img ]; then").unwrap();
        let space = s.find("[ \"${avail:-0}\" -ge 4194304 ]").unwrap();
        assert!(size < crosvm && store < crosvm && space < crosvm);
        assert!(s.contains("truncate -s 16G /data/local/tmp/oneios-vmboot/data.img"));
        assert!(!s.contains("stat -c %s /data/local/tmp/oneios-vmboot/data.img"));
        // every disk on crosvm's line, pinned
        assert_eq!(s.matches("  --block path=").count(), 3);
        assert!(s.contains("--block path=/data/local/tmp/oneios-vmboot/data.img,ro=false,id=data,pci-address=00:06.0 $D/kernel.img"));
        // the pool: right after crosvm starts, before the main wait; a wrong one stops the VM
        let pool = s.find("grep -q -F restricted_dma_reserved@13f800000 $D/fdt.dtb").unwrap();
        let wait = s.find("while [ $t -lt 600 ]").unwrap();
        assert!(crosvm < pool && pool < wait);
        assert!(s.contains("pool=bad; touch $D/stop.request"));
        assert!(s.contains("end \"refused pool: end how $how exit $rc\""));
    }

    /// xnuloader-crosvm --display's list (2880 MiB, a pool of 32 MiB: the frame at its start, the count at 17973248).
    const SCREEN: &str = "swiotlb-mib 32\npool 0x132000000\nstore /data/local/tmp/oneios-vmboot/data.img 16\n\
        fb 0 17971200 1440 3120 5760 17973248\n\
        block 10359930880 path=/data/local/tmp/oneios/image/os.img,ro=true,id=os,pci-address=00:02.0\n\
        block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false,id=data,pci-address=00:06.0\n";

    #[test]
    fn the_screen_is_in_the_pool() {
        let d = parse_disks(SCREEN).unwrap();
        assert_eq!(d.fb, Some(Fb { offset: 0, bytes: 17971200, width: 1440, height: 3120, stride: 5760, seq: 17973248 }));
        assert_eq!(parse_disks(DISKS).unwrap().fb, None);
        // the count inside the frame, past the pool's end, a frame that is not stride x height, an odd offset
        assert!(parse_disks(&SCREEN.replace(" 17973248\n", " 16384\n")).is_err());
        assert!(parse_disks(&SCREEN.replace(" 17973248\n", " 33554432\n")).is_err());
        assert!(parse_disks(&SCREEN.replace("fb 0 17971200", "fb 0 17971204")).is_err());
        assert!(parse_disks(&SCREEN.replace("fb 0 17971200", "fb 4096 17971200")).is_err());
        assert!(parse_disks(&SCREEN.replace("fb 0 17971200 1440 3120 5760 17973248", "fb 0 17971200 1440 3120")).is_err());
    }

    #[test]
    fn a_scanout_image_shows_its_screen_through_crosvms_own_display() {
        let d = parse_disks(&format!("{SCREEN}gpu 00:0c.0 8 0x15f800000\nscanout 1080 2340 60\n")).unwrap();
        assert_eq!(d.scanout, Some((1080, 2340, 60)));
        let s = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true }, 900, true, "ab12", Some(&d));
        assert!(s.contains("--gpu backend=virglrenderer,context-types=virgl2:drm,egl=true,gles=true,udmabuf=true,pci-address=00:0c.0,"));
        assert!(s.contains("pci-bar-size=0x10000000,displays=[[mode=windowed[1080,2340],refresh-rate=60]] \\\n  --pre-alloc drm-host-mb=8"));
        assert!(s.contains(" \\\n  --android-display-service name=oneios_disp,screen=gpu-0"));
        assert!(s.contains("$H --crosvm-display oneios_disp --size 1080x2340 --hz 60 --wait 120 </dev/null >>$D/display.txt 2>&1 &"));
        assert!(!s.contains("$D/displayctl pool"), "no bridge: crosvm draws the screen");
        // the GPU's floor is off by default now: the governor scales (power); DEFAULT does not touch kgsl
        assert!(!s.contains("kgsl-3d0"), "default --gpu-floor 0: the governor's alone");
        // opted in with --gpu-floor 607: set while the VM runs, put back after crosvm ends and before the bridge's end
        let gf = boot_script_with(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true }, 900, true, "ab12", Some(&d), false, false, &Tuning { gpu_floor_mhz: 607, ..Tuning::DEFAULT });
        assert!(gf.contains("if [ \"$f\" -ge 607000000 ] && { [ -z \"$floor\" ] || [ \"$f\" -lt \"$floor\" ]; }; then floor=$f; fi"));
        let (set, back) = (gf.find("echo $floor > $G/min_freq").unwrap(), gf.find("thp_restore\ngpu_restore\n").unwrap());
        assert!(set < gf.find("$C --log-level info run").unwrap());
        assert!(gf.find("wait $pid").unwrap() < back);
        assert!(!boot_script_with(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true }, 900, true, "ab12", Some(&d), false, false, &Tuning { gpu_floor_mhz: 0, ..Tuning::DEFAULT })
            .contains("kgsl-3d0"), "--gpu-floor 0: the governor's alone");
        // the open files crosvm may hold: a file per GPU buffer
        assert!(s.find("ulimit -n 1048576").unwrap() < s.find("$C --log-level info run").unwrap());
        // its frames blitted on the GPU by the package's Turnip
        assert!(s.contains("set -f\nCROSVM_DISPLAY_VULKAN_LIBRARY=/data/local/tmp/oneios-droidvm/libvulkan_freedreno.so $C --log-level info run "));
        // the helper starts after the shim's and the arena's checks, and goes with the VM as the bridge did
        assert!(s.find("drm2kgsl_host@").unwrap() < s.find("--crosvm-display").unwrap());
        assert!(s.find("--crosvm-display").unwrap() < s.find("kill -TERM $bridge").unwrap());
        // a scanout needs the GPU and the screen
        assert!(parse_disks("swiotlb-mib 32\npool 0x132000000\nstore /x 16\nscanout 1080 2340 60\n").is_err());
        // no sound card without its line
        assert!(!s.contains("--virtio-snd"));
    }

    #[test]
    fn an_image_with_the_wifi_gets_wifictls_tap_in_a_takeover_only() {
        let d = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x15f800000\nscanout 720 1560 120\naudio\nwifi\n")).unwrap();
        assert!(d.wifi && d.audio);
        let l = Label { ram: 3584, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, ..Tuning::DEFAULT });
        let start = s.find("$D/wifictl tap oneios0 $D </dev/null >>$D/wifi.txt 2>&1 &").unwrap();
        // after One UI is down, before crosvm, whose card gets wlan0's address on the tap
        assert!(s.find("takeover: One UI is down").unwrap() < start && start < s.find("$C --log-level info run").unwrap());
        assert!(s.contains(" \\\n  ${wifi_mac:+--net tap-name=oneios0,mac=$wifi_mac} \\\n  --block "));
        // given back after crosvm, and by the trap before One UI starts
        assert!(s.find("wait $pid; rc=$?\nthp_restore\nwifi_restore\n").unwrap() > start);
        assert!(s.contains("caps_restore; wifi_restore; start_ui \"the session ended\"' EXIT HUP INT TERM"));
        assert!(s.contains("wifi_restore() { :; }") && s.find("wifi_restore() { :; }").unwrap() < start);
        // without a takeover One UI keeps wlan0
        let plain = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        assert!(!plain.contains("wifictl") && !plain.contains("--net "));
        assert!(parse_disks(&format!("{SCREEN}wifi on\n")).is_err());
    }

    #[test]
    fn an_image_with_the_sound_card_gets_crosvms_aaudio_card() {
        let d = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x15f800000\nscanout 720 1560 120\naudio\n")).unwrap();
        assert!(d.audio);
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, ..Tuning::DEFAULT });
        assert!(s.contains(" \\\n  --virtio-snd backend=aaudio,capture=false \\\n  --block "), "in crosvm's process, before the disks");
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { audio_uid: Some(2000), ..Tuning::DEFAULT });
        assert!(s.contains(" \\\n  --virtio-snd backend=aaudio,capture=false,uid=2000 \\\n"));
        assert!(parse_disks(&format!("{SCREEN}audio 1\n")).is_err());
    }

    #[test]
    fn a_takeover_stops_one_ui_after_the_checks_and_starts_it_on_every_end() {
        let d = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x15f800000\nscanout 1080 2340 120\n")).unwrap();
        let l = Label { ram: 3584, cpus: 8, swiotlb: 32, shared: true, gpu: true };
        // floors opted in here so the ordering below is exercised; off by default (asserted at the end)
        let t = Tuning { takeover: true, gpu_floor_mhz: 607, cpu_floor_percent: 60, ..Tuning::DEFAULT };
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &t);
        let at = |what: &str| s.find(what).unwrap_or_else(|| panic!("no {what:?}"));
        // the way back armed (ui.sh's failsafe and the trap) before the stop, the stop after every check and the THP's trap
        assert!(at("[ -c /dev/gunyah_share ] ||") < at(". $D/ui.sh"));
        assert!(at("trap thp_restore EXIT") < at(". $D/ui.sh"));
        assert!(at(". $D/ui.sh") < at("bg /system/bin/sh $SELF --failsafe"));
        assert!(at("bg /system/bin/sh $SELF --failsafe") < at("setprop ctl.stop zygote"));
        assert!(at("trap 'thp_restore; gpu_restore; cpu_restore; caps_restore; wifi_restore; start_ui \"the session ended\"' EXIT HUP INT TERM")
            > at("setprop ctl.stop zygote"));
        // Samsung's cpufreq_limit lifted with One UI down, before the caps and the floors; put back with the caps
        assert!(at("setprop ctl.stop zygote") < at("echo -1 > $SL"));
        assert!(at("echo -1 > $SL") < at("echo $top > $f/scaling_max_freq"));
        assert!(s.contains("echo \"$sl_old\" > $SL") && !s.contains("ctl.restart perf2-hal"));
        // the caps lifted, then the floors over them, then crosvm
        assert!(at("echo $top > $f/scaling_max_freq") < at("echo $floor > $f/scaling_min_freq"));
        // the pool topped up with One UI down when it holds less than the VM's 1776 pages, else left alone
        assert!(s.contains("need=1776\n") && at("echo 3 > /proc/sys/vm/drop_caches") < at("echo 3 > $P/acquire"));
        assert!(s.contains("{ end \"refused pool $avail of $need pages after a top-up\"; exit 1; }"));
        assert!(at("echo $floor > $f/scaling_min_freq") < at("$C --log-level info run"));
        // the helper places its layer on the panel itself, and the caps come back after crosvm
        assert!(s.contains("--crosvm-display oneios_disp --size 1080x2340 --hz 120 --wait 120 --takeover --screen-file $D/screen.off </dev/null"));
        // the side keys to the VM, read and never taken, and the panel switched by a click of power
        assert!(s.contains("kl=; for k in $keys; do kl=\"${kl:+$kl,}/dev/input/$k\"; done\n"));
        assert!(s.contains("--socket $D/touch.sock${kl:+ --keys $kl --screen-file $D/screen.off} </dev/null >>$D/touch.txt 2>&1 &\n"));
        assert!(at("wait $pid") < at("thp_restore\ngpu_restore\ncpu_restore\ncaps_restore\n"));
        // the same takeover with the default tuning: floors off, the governor scales (the power default)
        let d_off = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, ..Tuning::DEFAULT });
        assert!(!d_off.contains("scaling_min_freq") && !d_off.contains("kgsl-3d0/devfreq"), "floors off by default even in a takeover");
        // the thermal log once crosvm runs, beside its other files
        let logged = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { thermal_log: true, ..Tuning::DEFAULT });
        assert!(logged.find("$C --log-level info run").unwrap() < logged.find("# the thermal log").unwrap());
        assert!(logged.contains(") > $D/thermal.txt 2>&1 &\n") && !s.contains("thermal.txt"));
        // without --takeover nothing of it
        let plain = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        assert!(!plain.contains("ui.sh") && !plain.contains("ctl.stop") && !plain.contains("--takeover"));
        // ui.sh: hugepool's helpers with this folder's names, its results its own
        let u = ui_script(3900);
        if let Ok(dir) = std::env::var("ONEIOS_DUMP_DIR") {
            std::fs::write(format!("{dir}/takeover-boot.sh"), &s).unwrap();
            std::fs::write(format!("{dir}/takeover-ui.sh"), &u).unwrap();
        }
        assert!(u.contains("D=/data/local/tmp/oneios-vmboot\nSELF=$D/ui.sh\nCTL=$D/ui-ctl\n") && u.contains("LIMIT=3900\n"));
        assert!(u.contains("start_ui() {") && u.contains("--failsafe)") && !u.contains("$D/result") && u.contains("oneios-vmboot-failsafe"));
    }

    #[test]
    fn the_clock_floors_are_off_by_default_and_opt_in_with_the_flags() {
        // The power default (2026-11-25): no floor, the governor scales. The stutter the floors chased was the panel
        // held at 60 Hz (native-frame-latency), not the clocks, so pinning the GPU at 607 MHz and the cores at 60% of
        // top the whole session only burned power. The flags still pin a floor for an A/B.
        assert_eq!((GPU_FLOOR_MHZ, CPU_FLOOR_PERCENT), (0, 0), "the shipped defaults are off");
        let d = parse_disks(&format!("{SCREEN}gpu 00:0c.0 8 0x15f800000\nscanout 1080 2340 120\n")).unwrap();
        let l = Label { ram: 2880, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let def = boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        assert!(!def.contains("kgsl-3d0/devfreq") && !def.contains("scaling_min_freq"), "default: the governor's clocks alone");
        let floored = boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &Tuning { gpu_floor_mhz: 607, cpu_floor_percent: 60, ..Tuning::DEFAULT });
        assert!(floored.contains("echo $floor > $G/min_freq") && floored.contains("echo $floor > $f/scaling_min_freq"), "opted in: both floors set");
    }

    #[test]
    fn a_gpu_image_gets_crosvms_virtio_gpu_and_drm2kgsls_arena() {
        assert_eq!(label(&image(b"oneios-crosvm ram-mib=3584 cpus=8 swiotlb-mib=32 shared-ram=1 gpu=1")).map(|l| l.gpu), Some(true));
        let d = parse_disks(&format!("{SCREEN}gpu 00:0c.0 8 0x15f800000\n")).unwrap();
        assert_eq!(d.gpu, Some(Gpu { pci: "00:0c.0".into(), drm_mib: 8, drm_host: 0x15f800000 }));
        let s = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true }, 900, true, "ab12", Some(&d));
        assert!(s.contains(" \\\n  --gpu backend=virglrenderer,context-types=drm,udmabuf=true,pci-address=00:0c.0,pci-bar-size=0x10000000 \\\n  --pre-alloc drm-host-mb=8"));
        assert!(s.contains("grep -q -F drm2kgsl_host@15f800000 $D/fdt.dtb"));
        // the arena's check after the shim's, both before the bridge
        assert!(s.find("shim_handoff@").unwrap() < s.find("drm2kgsl_host@").unwrap());
        assert!(s.find("drm2kgsl_host@").unwrap() < s.find("TMPDIR=$D $D/displayctl pool").unwrap());
        assert!(parse_disks(&format!("{SCREEN}gpu 0c.0 8 0x15f800000\n")).is_err());
        // without the line there is no GPU
        let plain = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: false }, 900, true, "ab12", Some(&parse_disks(SCREEN).unwrap()));
        assert!(!plain.contains("--gpu ") && !plain.contains("drm2kgsl_host"));
    }

    const GPU: &str = "gpu 00:0c.0 8 0x15f800000\n";

    /// The GPU interrupt's check of a boot script run by the PC's `sh` on a tree of `words` (big-endian, as an FDT's
    /// cells): what it said, `$pool` after it, and whether it asked for the VM's stop.
    fn gpu_irq_run(script: &str, words: &[u32]) -> (String, String, bool) {
        let from = script.find("gpu_irq() {\n").unwrap();
        let end = "! gpu_irq; then pool=bad; touch $D/stop.request; fi\n";
        let to = from + script[from..].find(end).unwrap() + end.len();
        let dir = std::env::temp_dir().join(format!("oneios-vmboot-gpuirq-{}-{}", std::process::id(), words.iter().map(|w| *w as u64).sum::<u64>()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("fdt.dtb"), words.iter().flat_map(|w| w.to_be_bytes()).collect::<Vec<u8>>()).unwrap();
        let body = format!("D={}\npool=\n{}echo \"pool=$pool\"\n", dir.display(), &script[from..to]);
        let out = std::process::Command::new("sh").arg("-c").arg(&body).output().unwrap();
        let stop = dir.join("stop.request").exists();
        fs::remove_dir_all(&dir).ok();
        let text = String::from_utf8_lossy(&out.stdout).to_string();
        let pool = text.lines().last().unwrap_or("").trim_start_matches("pool=").to_string();
        (text, pool, stop)
    }

    /// An interrupt-map entry as crosvm writes it: device DEV's INTA to the GIC (phandle 1) as SPI `spi` with `flags`.
    fn map_entry(dev: u32, spi: u32, flags: u32) -> [u32; 10] {
        [dev << 11, 0, 0, 1, 1, 0, 0, 0, spi, flags]
    }

    #[test]
    fn the_gpu_interrupt_is_given_only_when_crosvms_map_routes_the_gpu_to_it() {
        // the line: an SPI, only beside the GPU; none, no check
        let d = parse_disks(&format!("{SCREEN}{GPU}gpu-intid 36\n")).unwrap();
        assert_eq!(d.gpu_intid, Some(36));
        assert_eq!(parse_disks(&format!("{SCREEN}{GPU}")).unwrap().gpu_intid, None);
        assert!(parse_disks(&format!("{SCREEN}gpu-intid 36\n")).is_err());
        assert!(parse_disks(&format!("{SCREEN}{GPU}gpu-intid 31\n")).is_err() && parse_disks(&format!("{SCREEN}{GPU}gpu-intid 1020\n")).is_err());
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let s = boot_script(&l, 900, true, "ab12", Some(&d));
        let without = boot_script(&l, 900, true, "ab12", Some(&parse_disks(&format!("{SCREEN}{GPU}")).unwrap()));
        assert!(!without.contains("gpu_irq") && !without.contains("interrupt-map"));
        // the check: after the arena's, before the bridge, on 00:0c.0's child address and SPI 4
        assert!(s.contains("grep -o -E ' 00006000 00000000 00000000 0000000[1-4] [0-9a-f]{8} 00000000 00000000 [0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8}'"));
        assert!(s.contains("[ $9 = 00000004 ] && [ ${10} = 00000004 ]"));
        assert!(s.find("drm2kgsl_host@").unwrap() < s.find("gpu_irq() {").unwrap());
        assert!(s.find("! gpu_irq; then pool=bad; touch $D/stop.request").unwrap() < s.find("TMPDIR=$D $D/displayctl pool").unwrap());
        // run by the PC's sh on trees as crosvm writes them (take12's: the disks at 2 to 6 on SPI 5 to 9, the GPU at 12 on 4)
        let header = [0xd00d_feedu32, 0x1de2, 0x38, 0x1c00, 3, 400, 0x1a5];
        let tree = |gpu: Option<[u32; 10]>| -> Vec<u32> {
            let mut w = header.to_vec();
            w.extend(map_entry(2, 5, 4));
            w.extend(map_entry(3, 6, 4));
            if let Some(e) = gpu {
                w.extend(e);
            }
            w.extend(map_entry(1, 10, 4));
            w.extend([0xf800, 0, 0, 7, 9]);
            w
        };
        let (said, pool, stop) = gpu_irq_run(&s, &tree(Some(map_entry(12, 4, 4))));
        assert!(said.contains("gpu: the GPU's interrupt is intid 36 (crosvm's SPI 4): its waits sleep on it"), "{said}");
        assert!(pool.is_empty() && !stop);
        // another line, an edge line, no entry, two entries: stopped, and the run says what crosvm gave
        let (said, pool, stop) = gpu_irq_run(&s, &tree(Some(map_entry(12, 6, 4))));
        assert!(said.contains("crosvm routes 00:0c.0 to GIC type 0x00000000 number 0x00000006 flags 0x00000004, not SPI 4") && pool == "bad" && stop, "{said}");
        let (said, pool, stop) = gpu_irq_run(&s, &tree(Some(map_entry(12, 4, 1))));
        assert!(said.contains("flags 0x00000001, not SPI 4 level-high") && pool == "bad" && stop, "{said}");
        let (said, pool, stop) = gpu_irq_run(&s, &tree(None));
        assert!(said.contains("has no entry for 00:0c.0") && pool == "bad" && stop, "{said}");
        let mut two = tree(Some(map_entry(12, 4, 4)));
        two.extend(map_entry(12, 4, 4));
        let (said, pool, stop) = gpu_irq_run(&s, &two);
        assert!(said.contains("more than one entry for 00:0c.0") && pool == "bad" && stop, "{said}");
        // a pool already refused is not checked again
        let refused = s.replace("if [ \"$pool\" != bad ] && ! gpu_irq", "pool=bad\nif [ \"$pool\" != bad ] && ! gpu_irq");
        let (said, _, stop) = gpu_irq_run(&refused, &tree(None));
        assert!(!said.contains("gpu:") && !stop, "{said}");
    }

    #[test]
    fn a_shared_ram_image_runs_under_droid_vms_crosvm() {
        assert_eq!(
            label(&image(b"oneios-crosvm ram-mib=2048 cpus=8 swiotlb-mib=32 shared-ram=1")),
            Some(Label { ram: 2048, cpus: 8, swiotlb: 32, shared: true, gpu: false })
        );
        let d = parse_disks(SCREEN).unwrap();
        let l = Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: false };
        let s = boot_script(&l, 900, true, "ab12", Some(&d));
        assert!(s.contains("C=/data/local/tmp/oneios-droidvm/crosvm\n"));
        assert!(s.contains("[ -c /dev/gunyah_share ] ||") && s.contains("[ -x $C ] ||"));
        assert!(s.contains("--hypervisor gunyah \\\n  --protected-vm-pseudo-unprotected --hugepages --mem size=2880 "));
        assert!(!s.contains("--swiotlb") && !s.contains("restricted_dma_reserved"));
        assert!(s.contains("grep -q -F shim_handoff@ $D/fdt.dtb"));
        assert!(s.contains(" --vm shared --helper $H"));
        // the shim's check comes before the bridge, as the pool's does
        assert!(s.find("shim_handoff@").unwrap() < s.find("TMPDIR=$D $D/displayctl pool").unwrap());
        // AVF's protected VM stays as it was
        let p = boot_script(&Label { shared: false, ..l }, 900, true, "ab12", Some(&d));
        assert!(p.contains("C=/apex/com.android.virt/bin/crosvm\n") && p.contains("--swiotlb 32 --hugepages"));
        assert!(!p.contains("--vm shared") && !p.contains("gunyah_share") && !p.contains("shim_handoff"));
    }

    #[test]
    fn the_bridge_starts_after_the_pool_check_and_ends_after_the_vm() {
        let d = parse_disks(SCREEN).unwrap();
        let l = Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false };
        let s = boot_script(&l, 900, true, "ab12", Some(&d));
        let pool = s.find("grep -q -F restricted_dma_reserved@132000000 $D/fdt.dtb").unwrap();
        let bridge = s.find("TMPDIR=$D $D/displayctl pool --pid $pid").unwrap();
        let wait = s.find("while [ $t -lt 900 ]").unwrap();
        assert!(pool < bridge && bridge < wait);
        // the pool's offset in the memfd and its size, the frame and the count; the staged helper
        assert!(s.contains("--pool-offset 2986344448 --pool-bytes 33554432 --fb-offset 0 --size 1440x3120 --stride 5760 --seq-offset 17973248 --helper $H"));
        assert!(s.contains("H=$D/oneios-display\n"));
        assert!(s.contains("if [ \"$pool\" != bad ] && kill -0 $pid 2>/dev/null; then"));
        // after the VM, the bridge is waited for, asked to end, then ended
        let end = s.find("if [ -n \"$bridge\" ]; then").unwrap();
        assert!(s.find("wait $pid; rc=$?\nthp_restore\n").unwrap() < end);
        assert!(s[end..].find("kill -TERM $bridge").unwrap() < s[end..].find("kill -9 $bridge").unwrap());
        assert!(s.contains("--swiotlb 32 --hugepages --mem size=2880"));
        assert!(!boot_script(&l, 60, true, "x", Some(&parse_disks(DISKS).unwrap())).contains("displayctl"));
    }

    /// The S25's input devices that matter (2026-11-23: `cat /proc/bus/input/devices`), in their order.
    const DEVICES: &str = "I: Bus=0019 Vendor=0001 Product=0001 Version=0100\nN: Name=\"gpio-keys\"\nP: Phys=gpio-keys/input0\n\
        S: Sysfs=/devices/platform/soc/soc:gpio_keys/input/input0\nU: Uniq=\nH: Handlers=event0 sec_key_notifier cpufreq sec_tsp_dumpkey \n\
        B: PROP=0\nB: EV=3\nB: KEY=8000000000000 0\n\n\
        I: Bus=0000 Vendor=0000 Product=0000 Version=0000\nN: Name=\"pmic_pwrkey\"\nP: Phys=pmic_pwrkey/input0\n\
        S: Sysfs=/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-00/pwrkey/input/input3\nU: Uniq=\n\
        H: Handlers=event3 sec_key_notifier cpufreq sec_tsp_dumpkey \nB: PROP=0\nB: EV=3\nB: KEY=10000000000000 0\n\n\
        I: Bus=0000 Vendor=0000 Product=0000 Version=0000\nN: Name=\"pmic_resin\"\nP: Phys=pmic_resin/input0\n\
        S: Sysfs=/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-00/resin/input/input4\nU: Uniq=\n\
        H: Handlers=event4 sec_key_notifier cpufreq sec_tsp_dumpkey \nB: PROP=0\nB: EV=3\nB: KEY=4000000000000 0\n\n\
        I: Bus=0018 Vendor=0000 Product=0000 Version=0000\nN: Name=\"sec_touchscreen\"\nP: Phys=sec_touchscreen\n\
        S: Sysfs=/devices/platform/soc/ac0000.qcom,qupv3_1_geni_se/a90000.spi/spi_master/spi1/spi1.0/input/input8\nU: Uniq=\n\
        H: Handlers=event8 sec_key_notifier cpufreq sec_tsp_dumpkey kgsl sde \nB: PROP=2\nB: EV=2b\n\
        B: KEY=4000000000000000 0 4000000000 0 0 420 3000000 0 8000 0 0\nB: ABS=263800000000003\nB: SW=0\n\n\
        I: Bus=0018 Vendor=0000 Product=0000 Version=0000\nN: Name=\"sec_touchpad\"\nP: Phys=sec_touchpad\n\
        S: Sysfs=/devices/platform/soc/ac0000.qcom,qupv3_1_geni_se/a90000.spi/spi_master/spi1/spi1.0/input/input9\nU: Uniq=\n\
        H: Handlers=event9 sec_key_notifier cpufreq sec_tsp_dumpkey kgsl sde \nB: PROP=1\nB: EV=2b\nB: SW=0\n\n";

    /// The script's touchscreen lookup run by the PC's sh and awk on DEVICES (or a variant): what it leaves in tdev.
    fn touch_lookup(devices: &str) -> String {
        let d = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        let s = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false }, 900, true, "ab12", Some(&d));
        let (from, to) = (s.find("dev_of() {\n").unwrap(), s.find("if [ -n \"$tdev\" ] && [ -c /dev/input/$tdev ]; then").unwrap());
        let dir = std::env::temp_dir().join(format!("oneios-vmboot-touch-{}-{}", std::process::id(), devices.len()));
        fs::create_dir_all(&dir).unwrap();
        let file = dir.join("devices");
        fs::write(&file, devices).unwrap();
        let script = s[from..to].replace("/proc/bus/input/devices", file.to_str().unwrap()) + "echo \"tdev=$tdev\"\n";
        let out = std::process::Command::new("sh").arg("-c").arg(&script).output().unwrap();
        fs::remove_dir_all(&dir).ok();
        String::from_utf8_lossy(&out.stdout).trim().to_string()
    }

    #[test]
    fn the_touchscreen_is_found_by_name_and_never_a_side_key() {
        let d = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        assert!(d.input && !parse_disks(SCREEN).unwrap().input);
        assert!(parse_disks(&format!("{SCREEN}input mouse\n")).is_err());
        assert!(parse_disks(&format!("{DISKS}input touch\n")).is_err());           // the input bus comes with the screen
        assert_eq!(touch_lookup(DEVICES), "tdev=event8");
        // a touchscreen that is not direct, none at all, one on a side key's device: no touch
        assert_eq!(touch_lookup(&DEVICES.replace("B: PROP=2\n", "B: PROP=1\n")), "tdev=");
        assert_eq!(touch_lookup(&DEVICES.replace("sec_touchscreen", "other_ts")), "tdev=");
        let shared = DEVICES.replace("Handlers=event8 sec_key", "Handlers=event4 sec_key");
        assert_eq!(touch_lookup(&shared), "touch: event4 is a side key's device: not given to the VM\ntdev=");
        // the script: the lookup, then the forwarder listening, before crosvm; crosvm's multi-touch device on its socket
        // (no glob expansion of the brackets); the way back after the bridge; both ended after the VM
        let s = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false }, 900, true, "ab12", Some(&d));
        let lookup = s.find("tdev=$(dev_of sec_touchscreen 2)").unwrap();
        let forward = s.find("$D/displayctl touch --evdev /dev/input/$tdev --socket $D/touch.sock </dev/null >>$D/touch.txt 2>&1 &").unwrap();
        let crosvm = s.find("$C --log-level info run").unwrap();
        assert!(lookup < forward && forward < crosvm);
        assert!(s.contains("--socket $D/crosvm.sock${tsock:+ --input multi-touch[path=$tsock,width=4095,height=4095,name=oneios-touch]} --serial"));
        assert!(s[..crosvm].ends_with("set -f\n") && s[crosvm..].contains("pid=$!\nset +f\n"));
        assert_eq!(s.matches("--evdev").count(), 1);                                // the forwarder's, not crosvm's
        assert!(s.contains("if [ -S $D/touch.sock ]; then\n    tsock=$D/touch.sock;"));
        let ended = s.find("if [ -n \"$forward\" ]; then\n").unwrap();
        assert!(s.find("wait $pid; rc=$?").unwrap() < ended && s[ended..].contains("kill $forward 2>/dev/null; wait $forward"));
        let escape = s.find("( getevent -l /dev/input/$vd 2>/dev/null").unwrap();
        assert!(s.find("TMPDIR=$D $D/displayctl pool").unwrap() < escape && escape < s.find("while [ $t -lt 900 ]").unwrap());
        assert!(s.contains("[ $((now - down)) -ge 10 ]; then echo \"touch: volume down held $((now - down)) s: the VM stops\"; touch $D/stop.request"));
        assert!(s.find("thp_restore\n").unwrap() < s.find("if [ -n \"$escape\" ]; then kill $escape").unwrap());
        assert!(!s.contains("EVIOCGRAB") && !s.contains("--evdev /dev/input/event"));
        // without the input bus: no lookup, and the empty tdev puts nothing on crosvm's line
        let plain = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false }, 900, true, "ab12", Some(&parse_disks(SCREEN).unwrap()));
        assert!(!plain.contains("dev_of") && !plain.contains("getevent") && plain.contains("tdev=\ntsock=\n"));
        assert!(!plain.contains("displayctl touch"));
        // the test swipes only when asked, on the forwarder's line
        let swiped = boot_script_with(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false }, 900, true, "ab12", Some(&d), true, false, &Tuning::DEFAULT);
        // the vCPUs on their cores only when asked
        let pinned = boot_script_with(&Label { ram: 2048, cpus: 8, swiotlb: 512, shared: false, gpu: false }, 900, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        assert!(!pinned.contains("kgsl"), "no GPU: its clocks are left alone");
        // the CPUs' floor is off by default: the governor scales (power)
        assert!(!pinned.contains("scaling_min_freq"), "default --cpu-floor 0: the governor's alone");
        // opted in with --cpu-floor 60: the floor is set and put back after crosvm ends
        let pinned_floor = boot_script_with(&Label { ram: 2048, cpus: 8, swiotlb: 512, shared: false, gpu: false }, 900, true, "ab12", Some(&d), false, true, &Tuning { cpu_floor_percent: 60, ..Tuning::DEFAULT });
        assert!(pinned_floor.contains("want=$((top * 60 / 100)); floor=") && pinned_floor.find("echo $floor > $f/scaling_min_freq").unwrap() < pinned_floor.find("$C --log-level info run").unwrap());
        assert!(pinned_floor.find("wait $pid").unwrap() < pinned_floor.find("thp_restore\ncpu_restore\n").unwrap());
        assert!(pinned.contains("--cpus num-cores=8 --cpu-affinity 0=0:1=1:2=2:3=3:4=4:5=5:6=6:7=7 \\\n"));
        // six from core 2: both 4.47 GHz cores the VM's, cores 0 and 1 the host's
        let six = boot_script_with(&Label { ram: 2048, cpus: 6, swiotlb: 512, shared: false, gpu: false }, 900, true, "ab12", Some(&d), false, true,
            &Tuning { pin_first: 2, ..Tuning::DEFAULT });
        assert!(six.contains("--cpus num-cores=6 --cpu-affinity 0=2:1=3:2=4:3=5:4=6:5=7 \\\n"));
        assert!(!s.contains("--cpu-affinity"));
        assert!(swiped.contains("--socket $D/touch.sock --swipe-at 200 --swipe-every 60 </dev/null") && !s.contains("--swipe-at"));
    }

    #[test]
    fn the_boot_parses_as_posix_sh() {
        let d = parse_disks(DISKS).unwrap();
        let screen = parse_disks(SCREEN).unwrap();
        let l = Label { ram: 3072, cpus: 1, swiotlb: 8, shared: false, gpu: false };
        let l32 = Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false };
        let touch = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        let irq = parse_disks(&format!("{SCREEN}{GPU}gpu-intid 36\n")).unwrap();
        let lgpu = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        for (n, script) in [boot_script(&l, 600, true, "ab12", Some(&d)), boot_script(&l, 60, false, "ab12", None), boot_script(&l32, 900, true, "ab12", Some(&screen)),
                            boot_script(&l32, 900, true, "ab12", Some(&touch)), boot_script(&lgpu, 900, true, "ab12", Some(&irq))]
            .iter()
            .enumerate()
        {
            let path = std::env::temp_dir().join(format!("oneios-vmboot-{}-{n}.sh", std::process::id()));
            fs::write(&path, script).unwrap();
            for shell in ["bash", "sh", "dash"] {
                let mut c = std::process::Command::new(shell);
                if shell == "bash" {
                    c.arg("--posix");
                }
                if let Ok(o) = c.arg("-n").arg(&path).output() {
                    assert!(o.status.success(), "{shell} -n: {}", String::from_utf8_lossy(&o.stderr));
                }
            }
            fs::remove_file(&path).ok();
        }
    }
}
