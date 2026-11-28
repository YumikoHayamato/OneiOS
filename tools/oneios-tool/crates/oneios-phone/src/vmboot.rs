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
//! With `gpu-intid N` (xnuloader-crosvm --gpu-intid) the kernel's GPU sleeps on INTID N, so the script also wants crosvm's
//! `interrupt-map` to route the GPU's function to SPI N-32 level-high, or stops the VM (`gpu_intid_check`).
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
/// The uid crosvm's virtio-media decoder helper runs under (vm-boot --video-decoder-uid; crosvm's `uid=`): shell's.
pub const VIDEO_DECODER_UID: u32 = 2000;

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
    /// `audio-intid N SLOT` (xnuloader-crosvm --audio-intid, take18): the line the sound card's control replies come by with
    /// oneios-audio-irq=1, and the slot it was read for, checked against crosvm's interrupt-map (audio_intid_check)
    pub audio_intid: Option<(u32, u32)>,
    /// `wifi` (xnuloader-crosvm --wifi): the kernel's Wi-Fi, crosvm's virtio-net on a tap that `wifictl tap` carries to wlan0
    pub wifi: bool,
    /// `bridge` (xnuloader-crosvm --bridge): the kernel's bridge channel on crosvm's second serial port, a Unix socket whose
    /// other end is `bridgectl serve` on the phone: the phone's battery to iOS, iOS's haptics to the phone's motor
    pub bridge: bool,
    /// false (vm-boot --no-haptics): `bridgectl serve --no-haptics`, the battery alone
    pub bridge_haptics: bool,
    /// `media MIB POOL-PA` (xnuloader-crosvm --media): the kernel's video decoder, crosvm's virtio-media decoder, whose
    /// buffers are crosvm's media_host pool of MIB MiB at POOL-PA (`--pre-alloc media-host-mb=MIB`: always given, the
    /// layout needs it; the device only with vm-boot --video-decoder)
    pub media: Option<Media>,
}

/// The video decoder's pool (crosvm's media_host): its size in MiB and where the Image expects it.
#[derive(Debug, PartialEq, Clone, Copy)]
pub struct Media {
    pub mib: u64,
    pub host: u64,
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
    let (mut audio, mut wifi, mut gpu_intid, mut audio_intid) = (false, false, None, None);
    let (mut bridge, mut media) = (false, None);
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
            ["bridge"] => bridge = true,
            ["audio-intid", n, slot] => {
                let (n, slot): (u32, u32) = (n.parse().context("audio-intid N")?, slot.parse().context("audio-intid SLOT")?);
                if !(32..=1019).contains(&n) || !(1..=31).contains(&slot) {
                    bail!("audio-intid {n} {slot}: an SPI of the GIC (32 to 1019) and a PCI slot (1 to 31)");
                }
                audio_intid = Some((n, slot));
            }
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
            ["media", mib, pa] => {
                // as xnuloader-crosvm --media-mib takes it: whole 2 MiB folios, 16 to 1024 MiB
                let mib: u64 = mib.parse().context("media MIB")?;
                if !(16..=1024).contains(&mib) || mib % 2 != 0 {
                    bail!("media {mib}: the decoder's pool is 16 to 1024 MiB, even");
                }
                let host = u64::from_str_radix(pa.strip_prefix("0x").context("media POOL-PA 0x...")?, 16).context("media POOL-PA")?;
                if host % (2 << 20) != 0 {
                    bail!("media: the pool at 0x{host:x} is not where crosvm puts one (2 MiB aligned)");
                }
                media = Some(Media { mib, host });
            }
            _ => bail!("a line of the disks list that is not one of swiotlb-mib, pool, store, block, fb, input, gpu, gpu-intid, scanout, audio, wifi, media, bridge: {line:?}"),
        }
    }
    let (store, store_gib) = store.context("no store line")?;
    let d = Disks { swiotlb: swiotlb.context("no swiotlb-mib line")?, pool: pool.context("no pool line")?, store, store_gib, blocks, fb, input, gpu, scanout, gpu_intid, audio, audio_intid, wifi, bridge, bridge_haptics: true, media };
    if d.scanout.is_some() && (d.gpu.is_none() || d.fb.is_none()) {
        bail!("a scanout without the GPU or the screen: xnuloader-crosvm gives it with --gpu-scanout and --display");
    }
    if d.gpu_intid.is_some() && d.gpu.is_none() {
        bail!("a GPU interrupt without the GPU: xnuloader-crosvm gives it with --gpu-intid beside --gpu");
    }
    if d.audio_intid.is_some() && !d.audio {
        bail!("a sound card interrupt without the sound card: xnuloader-crosvm gives it with --audio-intid beside --audio");
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

/// take18: the sound card's line (`audio-intid N SLOT`, xnuloader-crosvm --audio-intid) checked as the GPU's is
/// (gpu_intid_check): crosvm's interrupt-map must route 00:SLOT.0 to SPI N-32 level-high, or the VM is stopped before the
/// kernel's sound card attaches the line (the transport attaches it only with oneios-audio-irq=1, to a card at SLOT).
pub fn audio_intid_check(slot: u32, intid: u32) -> String {
    let (child, spi) = (slot << 11, intid - 32);
    format!(
        "snd_irq() {{\n  \
         set -- $(od -An -tx1 -v $D/fdt.dtb | tr -d ' \\n' | sed 's/......../& /g' | \
         grep -o -E ' {child:08x} 00000000 00000000 0000000[1-4] [0-9a-f]{{8}} 00000000 00000000 [0-9a-f]{{8}} [0-9a-f]{{8}} [0-9a-f]{{8}}')\n  \
         if [ $# = 0 ]; then echo \"audio: crosvm's interrupt-map has no entry for 00:{slot:02x}.0: intid {intid} is not known to be the sound card's line, stopped before the kernel's card attaches it\"; return 1; fi\n  \
         if [ $# != 10 ]; then echo \"audio: crosvm's interrupt-map has more than one entry for 00:{slot:02x}.0 ($*): stopped before the kernel's card attaches intid {intid}\"; return 1; fi\n  \
         if [ $8 = 00000000 ] && [ $9 = {spi:08x} ] && [ ${{10}} = 00000004 ]; then echo \"audio: the sound card's line is intid {intid} (crosvm's SPI {spi}, 00:{slot:02x}.0): its control replies by it with oneios-audio-irq=1\"; return 0; fi\n  \
         echo \"audio: crosvm routes 00:{slot:02x}.0 to GIC type 0x$8 number 0x$9 flags 0x${{10}}, not SPI {spi} level-high as the Image's intid {intid}: stopped before the kernel's card attaches a line that is not its own\"; return 1\n\
         }}\n\
         if [ \"$pool\" != bad ] && ! snd_irq; then pool=bad; touch $D/stop.request; fi\n"
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
/// `idle_ms` (`--gpu-idle-timer`, apart from the floor: the two are tried one at a time): how long the GPU stays awake after
/// its last work while the VM runs (KGSL's idle_timer, ms; Android's is 80), put back at the end; 0 leaves it alone.
pub fn gpu_floor_script(l: &Label, floor_mhz: u32, idle_ms: u32) -> (String, String) {
    if !l.gpu || (floor_mhz == 0 && idle_ms == 0) {
        return (String::new(), String::new());
    }
    let hz = u64::from(floor_mhz) * 1_000_000;
    let idle = if idle_ms == 0 {
        String::new()
    } else {
        format!(
            "# awake after its last work for longer than 80 ms: the first frames after a pause waited for it to wake (take2, take13)\n\
             if [ -w $K/idle_timer ]; then gpu_idle=$(cat $K/idle_timer); echo {idle_ms} > $K/idle_timer && \
             echo \"gpu: it stays awake $(cat $K/idle_timer) ms after its last work while the VM runs (it was $gpu_idle ms)\"; \
             else echo \"gpu: no $K/idle_timer: its sleep is the driver's\"; fi\n"
        )
    };
    let floor = if floor_mhz == 0 {
        String::new()
    } else {
        format!(
            "if [ -w $G/min_freq ] && [ -r $G/available_frequencies ]; then\n  \
             floor=\n  \
             for f in $(cat $G/available_frequencies); do\n    \
             if [ \"$f\" -ge {hz} ] && {{ [ -z \"$floor\" ] || [ \"$f\" -lt \"$floor\" ]; }}; then floor=$f; fi\n  done\n  \
             if [ -n \"$floor\" ]; then\n    \
             gpu_min=$(cat $G/min_freq)\n    \
             echo $floor > $G/min_freq && echo \"gpu: the GPU's lowest clock is $(cat $G/min_freq) Hz while the VM runs (it was $gpu_min Hz)\"\n  \
             else echo \"gpu: no clock of {floor_mhz} MHz or more: the GPU's clocks are the governor's\"; fi\n\
             else echo \"gpu: no $G/min_freq: the GPU's clocks are the governor's\"; fi\n"
        )
    };
    (
        format!(
            "G=/sys/class/kgsl/kgsl-3d0/devfreq\nK=/sys/class/kgsl/kgsl-3d0\ngpu_min=\ngpu_idle=\n\
             gpu_restore() {{\n  \
             [ -z \"$gpu_idle\" ] || {{ echo \"$gpu_idle\" > $K/idle_timer && echo \"gpu: it sleeps $(cat $K/idle_timer) ms after its work again\"; }}\n  \
             gpu_idle=\n  [ -n \"$gpu_min\" ] || return 0\n  \
             echo \"$gpu_min\" > $G/min_freq && echo \"gpu: its lowest clock is $(cat $G/min_freq) Hz again\"\n  \
             gpu_min=\n}}\n{idle}{floor}"
        ),
        "gpu_restore\n".to_string(),
    )
}

/// The thermal log (`vm-boot --thermal-log`, thermal.txt beside the VM's other files): once a second while crosvm runs, the
/// hottest zone of each CPU cluster (cpu-0-*: cores 0-5, cpu-1-*: 6 and 7), of the GPU (gpuss-*), the board (sys-therm-0), in
/// tenths of a degree, the battery's (tenths), each cluster's clock and its top, the GPU's clock and load; then, after those
/// columns, what USB and the battery give and take (power_supply's own units; usb-online 0: nothing plugged in), the CPUs
/// online, the GPU's caps (`cap`: KGSL's max_gpuclk in Hz, `pwr`: its thermal_pwrlevel, `gmax`: Samsung's
/// /sys/kernel/gpu/gpu_max_clock in MHz, `-` when the file is not there), Qualcomm's limits-management ceiling of each
/// cluster (`lmh`: cpufreq's dcvsh_freq_limit, kHz) and the clock the hardware reports (`hw`: cpuinfo_cur_freq, kHz; take19:
/// a throttle of LMh/dcvsh shows there and in neither scaling file nor a cooling device), and the cooling devices that limit something now (their number and state; `cool -`: none does), whose types and
/// highest states are in the header. `-`: no such file. The zones and the cooling devices are found by their type once, at the
/// start (the owner allowed reading /sys/class/thermal for this, 2026-11-24): the material of a controller of our own for
/// smoothness and temperature. Read only; the added columns are read by the shell itself (no process each).
pub fn thermal_log_script(on: bool) -> String {
    if !on {
        return String::new();
    }
    concat!(
        "# the thermal log (vm-boot --thermal-log): read only, once a second while crosvm runs\n",
        "tz() { for z in /sys/class/thermal/thermal_zone*; do case \"$(cat $z/type 2>/dev/null)\" in $1) echo $z/temp ;; esac; done; }\n",
        "hot() { m=-1000000; for f in \"$@\"; do v=$(cat $f 2>/dev/null); [ -n \"$v\" ] && [ \"$v\" -gt $m ] && m=$v; done; echo $((m / 100)); }\n",
        "one() { v=; [ -r $1 ] && { read -r v < $1; } 2>/dev/null; v=${v:--}; }\n",
        "(\n",
        "  c0=$(tz 'cpu-0-*'); c1=$(tz 'cpu-1-*'); gz=$(tz 'gpuss-*'); bz=$(tz 'sys-therm-0'); Q=/sys/devices/system/cpu/cpufreq; K=/sys/class/kgsl/kgsl-3d0\n",
        "  W=/sys/class/power_supply; cds=\n",
        "  for c in /sys/class/thermal/cooling_device*; do\n",
        "    [ -r $c/cur_state ] || continue\n",
        "    one $c/type; ty=$v; one $c/max_state; cds=\"$cds $c/cur_state\"; echo \"# cool ${c##*cooling_device} $ty max $v\"\n",
        "  done\n",
        "  echo \"# time c0 c1 gpu board bat (tenths of a degree) f0/top0 f6/top6 (kHz) gpu-clock gpu-busy cap max_gpuclk (Hz) pwr thermal_pwrlevel gmax gpu_max_clock (MHz; -: no such file) usb-online usb-uA usb-uV bat-uA bat-uV cpus-online lmh dcvsh_freq_limit0 6 (kHz) hw cpuinfo_cur_freq0 6 (kHz) cool NUMBER=STATE...\"\n",
        "  while kill -0 $pid 2>/dev/null; do\n",
        "    one $W/usb/online; x=\"usb $v\"; one $W/usb/current_now; x=\"$x $v\"; one $W/usb/voltage_now; x=\"$x $v\"\n",
        "    one $W/battery/current_now; x=\"$x batt $v\"; one $W/battery/voltage_now; x=\"$x $v\"; one /sys/devices/system/cpu/online; x=\"$x online $v\"\n",
        "    one $Q/policy0/dcvsh_freq_limit; x=\"$x lmh $v\"; one $Q/policy6/dcvsh_freq_limit; x=\"$x $v\"\n",
        "    one $Q/policy0/cpuinfo_cur_freq; x=\"$x hw $v\"; one $Q/policy6/cpuinfo_cur_freq; x=\"$x $v cool\"\n",
        "    cs=; for c in $cds; do one $c; case $v in 0|-) ;; *) n=${c%/cur_state}; cs=\"$cs ${n##*cooling_device}=$v\" ;; esac; done\n",
        "    one $K/max_gpuclk; gc=$v; one $K/thermal_pwrlevel; gp=$v; one /sys/kernel/gpu/gpu_max_clock; gm=$v\n",
        "    echo \"$(date +%T) c0 $(hot $c0) c1 $(hot $c1) gpu $(hot $gz) board $(hot $bz) bat $(cat /sys/class/power_supply/battery/temp 2>/dev/null) \\\n",
        "f0 $(cat $Q/policy0/scaling_cur_freq)/$(cat $Q/policy0/scaling_max_freq) f6 $(cat $Q/policy6/scaling_cur_freq)/$(cat $Q/policy6/scaling_max_freq) \\\n",
        "g $(cat $K/gpuclk) $(cat $K/gpu_busy_percentage | tr -d ' %') cap $gc pwr $gp gmax $gm $x${cs:- -}\"\n",
        "    sleep 1\n",
        "  done\n",
        ") > $D/thermal.txt 2>&1 &\n",
        "echo \"thermal: the log is $D/thermal.txt, once a second\"\n",
    )
    .to_string()
}

/// The touch boost's clocks (`vm-boot --touch-boost`): the GPU's lowest clock and a moderate share of each CPU cluster's top.
pub const BOOST_GPU_MHZ: u32 = 607;
pub const BOOST_CPU_PERCENT: u32 = 40;

/// The boost's part before the forwarder starts (runtime/display's `displayctl touch --boost`, boost.h): `boostv`, the files
/// and the values a touch raises them to (the lowest clock of each table at or above the target), empty when there is none.
/// The forwarder reads what to put back itself, while the VM runs: a minimum read here, at the start, is often one of
/// Samsung's that is gone seconds later (take12 saved 2227200 and 2246400 kHz). boost_restore, after crosvm and in the EXIT
/// traps (it leaves the forwarder of a running crosvm alone): the forwarder ended (it puts the clocks back itself), and what a killed one left put back from its file (boost_leftover).
pub fn touch_boost_script(l: &Label) -> String {
    let hz = u64::from(BOOST_GPU_MHZ) * 1_000_000;
    let gpu = if l.gpu { format!("boost_add /sys/class/kgsl/kgsl-3d0/devfreq/min_freq /sys/class/kgsl/kgsl-3d0/devfreq/available_frequencies {hz}\n") } else { String::new() };
    format!(
        "# the touch boost (vm-boot --touch-boost): the forwarder raises these while fingers move and puts them back\n\
         boostv=\n\
         boost_add() {{\n  \
         [ -w $1 ] && [ -r $2 ] || {{ echo \"boost: no $1\"; return 0; }}\n  \
         at=; for f in $(cat $2); do if [ \"$f\" -ge $3 ] && {{ [ -z \"$at\" ] || [ \"$f\" -lt \"$at\" ]; }}; then at=$f; fi; done\n  \
         [ -n \"$at\" ] || {{ echo \"boost: $2 has no clock of $3 or more\"; return 0; }}\n  \
         boostv=\"${{boostv:+$boostv,}}$1=$at\"\n\
         }}\n\
         {gpu}\
         for p in 0 1 2 3 4 5 6 7; do\n  \
         f=/sys/devices/system/cpu/cpufreq/policy$p\n  [ -r $f/cpuinfo_max_freq ] || continue\n  \
         boost_add $f/scaling_min_freq $f/scaling_available_frequencies $(($(cat $f/cpuinfo_max_freq) * {BOOST_CPU_PERCENT} / 100))\n\
         done\n\
         echo \"boost: a touch raises ${{boostv:-nothing}}\"\n\
         boost_restore() {{\n  \
         # crosvm still there (a signal ended the script): the forwarder is its touch and puts the clocks back itself\n  \
         if [ -n \"$forward\" ] && kill -0 $forward 2>/dev/null && [ -n \"$pid\" ] && kill -0 $pid 2>/dev/null; then return 0; fi\n  \
         if [ -n \"$forward\" ]; then\n    \
         n=0; while [ $n -lt 3 ] && kill -0 $forward 2>/dev/null; do [ $n != 1 ] || kill $forward 2>/dev/null; sleep 1; n=$((n + 1)); done\n    \
         if kill -0 $forward 2>/dev/null; then kill -9 $forward 2>/dev/null; fi\n    \
         # ended: its number may be another process's from here\n    \
         forward=\n  \
         fi\n  \
         n=0; while [ $n -lt 3 ] && [ -e $D/boost.state ]; do sleep 1; n=$((n + 1)); done\n  \
         boost_leftover\n\
         }}\n"
    )
}

/// What a killed forwarder or CPU floor left raised (boost.state, boost.h: `boot ID` and `PATH VALUE` lines, there only
/// meanwhile), put back: at the end, and first thing at the next start whatever its flags and whether or not it is refused.
/// Another boot's file says nothing about this one; one with a write that failed stays for the next try.
pub const BOOST_LEFTOVER: &str = r#"boost_leftover() {
  [ -f $D/boost.state ] || return 0
  left=
  if [ "$(head -n 1 $D/boost.state)" = "boot $(cat /proc/sys/kernel/random/boot_id)" ]; then
    while read -r p v; do
      case "$v" in ''|*[!0-9]*) continue ;; esac
      case "$p" in
        *..*) ;;
        /sys/*min_freq)
          if { echo "$v" > $p; } 2>/dev/null; then echo "boost: $p was left raised: it is $(cat $p) again"
          else left=1; echo "boost: $p could not be put back at $v: $D/boost.state is kept"; fi ;;
      esac
    done < $D/boost.state
  fi
  [ -n "$left" ] || rm -f $D/boost.state
}
boost_leftover
"#;

/// The CPUs' floor for the VM's life, percent of each cluster's top clock, as the `vm-boot --cpu-floor` default.
/// **0 = off** (the governor, WALT, scales freely). Off by default alongside the GPU floor: the stutter was the panel
/// (above), not slow clocks, and pinning each cluster at 60% of its top the whole session held the cores high and warm
/// with no measured frame-time win. A session that wants to A/B a pinned floor passes `--cpu-floor 60` (about 2.25 GHz
/// perf, 2.84 GHz prime once Samsung's cap is lifted). The wake-ramp latency this once guarded against belongs to the
/// idle governor / haltpoll, not to holding every core high the whole run.
pub const CPU_FLOOR_PERCENT: u32 = 0;

/// How long the CPUs' floor only reads the lowest clocks: displayctl's BOOST_SETTLE_S, for the same reason.
pub const FLOOR_SETTLE_S: u32 = 30;

/// The CPUs' floor for the VM's life (`floor_percent` > 0): each cpufreq policy's lowest clock becomes the lowest of its
/// clocks at or above that share of its top one now (scaling_max_freq: a cap Android set stays), put back at the end.
/// Policies are looked for by name (policy0 to policy7), not by a walk of /sys. A process of its own, once crosvm runs: for
/// FLOOR_SETTLE_S it only reads, and the lowest value seen of each is what is put back (take12 saved, and wrote back,
/// minimums Samsung held around the start only). The values go to boost.state (BOOST_LEFTOVER's) before the first write; it
/// puts them back when crosvm or boot.sh is gone or cpu_restore ends it, and a killed one's are boost_leftover's. One left
/// over from before that cannot be put back now: no floor, since its clock's reading is not an original.
pub fn cpu_floor_script(floor_percent: u32) -> (String, String) {
    if floor_percent == 0 {
        return (String::new(), String::new());
    }
    let pct = floor_percent.min(100);
    let body = r#"# the CPUs' floor (vm-boot --cpu-floor): read only for the first SETTLE s, then raised until the VM ends
cpu_restore() {
  [ -n "$floorp" ] || return 0
  kill $floorp 2>/dev/null
  n=0; while [ $n -lt 5 ] && kill -0 $floorp 2>/dev/null; do sleep 1; n=$((n + 1)); done
  if kill -0 $floorp 2>/dev/null; then kill -9 $floorp 2>/dev/null; sleep 1; fi
  floorp=
  boost_leftover
}
(
  trap - EXIT; go=1; trap 'go=' HUP INT TERM
  CF=/sys/devices/system/cpu/cpufreq; n=0
  runs() { [ -n "$go" ] && kill -0 $pid 2>/dev/null && kill -0 $$ 2>/dev/null; }
  while runs && [ $n -lt SETTLE ]; do
    boost_leftover
    for q in 0 1 2 3 4 5 6 7; do
      old=; { read -r old < $CF/policy$q/scaling_min_freq; } 2>/dev/null
      case "$old" in ''|*[!0-9]*) continue ;; esac
      eval "low=\${low$q:-}"
      if [ -z "$low" ] || [ "$old" -lt "$low" ]; then eval "low$q=$old"; fi
    done
    sleep 1; n=$((n + 1))
  done
  boost_leftover
  runs || exit 0
  if [ -e $D/boost.state ]; then echo "cpu: an earlier session's clocks are not back: no floor"; exit 0; fi
  todo=
  for q in 0 1 2 3 4 5 6 7; do
    f=$CF/policy$q; eval "low=\${low$q:-}"
    [ -n "$low" ] && [ -w $f/scaling_min_freq ] && [ -r $f/scaling_available_frequencies ] || continue
    top=$(cat $f/scaling_max_freq); case "$top" in ''|*[!0-9]*) continue ;; esac
    want=$((top * PCT / 100)); floor=
    for c in $(cat $f/scaling_available_frequencies); do
      if [ "$c" -ge "$want" ] && [ "$c" -le "$top" ] && { [ -z "$floor" ] || [ "$c" -lt "$floor" ]; }; then floor=$c; fi
    done
    [ -n "$floor" ] || floor=$top
    if [ "$low" -ge "$floor" ]; then echo "cpu: policy$q's lowest clock is $low kHz already"; continue; fi
    todo="$todo $q=$low=$floor=$top"
  done
  [ -n "$todo" ] || exit 0
  { echo "boot $(cat /proc/sys/kernel/random/boot_id)"; for w in $todo; do q=${w%%=*}; w=${w#*=}; echo "$CF/policy$q/scaling_min_freq ${w%%=*}"; done; } > $D/boost.state.new &&
    mv $D/boost.state.new $D/boost.state || { echo "cpu: no $D/boost.state: no floor"; exit 0; }
  for w in $todo; do
    q=${w%%=*}; w=${w#*=}; low=${w%%=*}; w=${w#*=}; floor=${w%%=*}; f=$CF/policy$q
    echo $floor > $f/scaling_min_freq && \
    echo "cpu: policy$q's lowest clock is $(cat $f/scaling_min_freq) kHz while the VM runs (it was $low; its top is ${w#*=})"
  done
  while runs; do sleep 1; done
  # here too when boot.sh is gone: a refused write is tried again for a while, then its file is the next start's
  n=0; while [ -e $D/boost.state ] && [ $n -lt 10 ]; do [ $n = 0 ] || sleep 1; boost_leftover; n=$((n + 1)); done
) &
floorp=$!
echo "cpu: the floor comes after SETTLE s (PCT% of each cluster's top)"
"#;
    (body.replace("SETTLE", &FLOOR_SETTLE_S.to_string()).replace("PCT", &pct.to_string()), "cpu_restore\n".to_string())
}

/// How the VM's session treats the phone: the GPU's and the CPUs' floors (gpu_floor_script, cpu_floor_script) and whether One
/// UI's framework is stopped for the VM's life (takeover_script).
pub struct Tuning {
    pub gpu_floor_mhz: u32,
    pub cpu_floor_percent: u32,
    /// KGSL's idle_timer while the VM runs, ms (gpu_floor_script; 0: the driver's own)
    pub gpu_idle_ms: u32,
    /// The touch boost (touch_boost_script)
    pub touch_boost: bool,
    /// Each touch frame with its scan time (displayctl touch --scan-time, runtime/display/src/scantime.h)
    pub touch_time: bool,
    /// The panel's vsync for the VM's refresh tick, and its phase in microseconds (vmperf::vsync_start_script); None: not asked
    pub vsync: Option<i32>,
    pub takeover: bool,
    /// With the takeover: Samsung's GPU max cap lifted to the GPU's top for the session, put back at the end (takeover_script)
    pub gpu_cap_lift: bool,
    /// With the takeover: the media stream's volume to its maximum before One UI stops (media_volume_script)
    pub max_volume: bool,
    /// With --pin: vCPU i on core pin_first + i, the cores below it the host's (crosvm's device threads, surfaceflinger)
    pub pin_first: u32,
    /// The thermal log (thermal_log_script)
    pub thermal_log: bool,
    /// The performance log and the device workers' CPUs (vmperf.rs)
    pub perf: crate::vmperf::Perf,
    /// With the sound card: crosvm's AAudio backend in its own process under this uid (crosvm's uid=), or None: in crosvm's
    pub audio_uid: Option<u32>,
    /// With the sound card: crosvm's card with `capture=true` (vm-boot --mic-capture), for an Image with the microphone on
    pub mic_capture: bool,
    /// `NAME=VALUE ` words for crosvm's environment, from host_renderer_env (the host renderer's switches); empty: none
    pub crosvm_env: String,
    /// With the takeover: each zram device compacted after the stop, with a line (takeover_memory_steps; take18)
    pub takeover_reclaim: bool,
    /// With the takeover: vm.swappiness for the session, put back at the end (takeover_memory_steps); None: kept
    pub takeover_swappiness: Option<u32>,
    /// With the takeover: a KEYCODE_WAKEUP right before One UI's stop (TAKEOVER_WAKE; take18)
    pub takeover_wake: bool,
    /// With the video decoder's pool (xnuloader-crosvm --media): crosvm's virtio-media decoder, its MediaCodec helper under
    /// this uid (vm-boot --video-decoder, --video-decoder-uid), or None: the pool alone, no device (iOS decodes in software)
    pub video_decoder: Option<u32>,
    /// take19, with the takeover's screen and the touch boost: the panel's rate free from this many Hz up to its top, and held
    /// at the top while a touch is recent (OneiOSDisplay --rate-min, --rate-hold-file $D/boost.state); 0: held at the top
    pub panel_rate_min: u32,
}

impl Tuning {
    pub const DEFAULT: Tuning =
        Tuning { gpu_floor_mhz: GPU_FLOOR_MHZ, cpu_floor_percent: CPU_FLOOR_PERCENT, gpu_idle_ms: 0, touch_boost: false, touch_time: false, vsync: None, takeover: false, gpu_cap_lift: false, max_volume: true, pin_first: 0, thermal_log: false,
                 perf: crate::vmperf::Perf { log: false, unpin: false }, audio_uid: None, mic_capture: false, crosvm_env: String::new(),
                 takeover_reclaim: true, takeover_swappiness: None, takeover_wake: true, video_decoder: None, panel_rate_min: 0 };
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

/// Samsung's GPU max cap (`vm-boot --gpu-cap-lift`, in the takeover, beside the cpufreq_limit lift): One UI's SSRM writes
/// /sys/kernel/gpu/gpu_max_clock (MHz) when the phone is warm (500 in take-sessions; KGSL shows it as thermal_pwrlevel 9 and
/// max_gpuclk 500000000 Hz), and with One UI down nothing puts it back. Each node is saved, written the top of its own table
/// (gpu_freq_table in MHz, gpu_available_frequencies in Hz) only when it is below it, and written back at the end (the fork's
/// oneios_switch.sh does the same, `clocks_save`). The kernel's thermal zones and cooling devices are not written.
/// `gpu_cap_top FILE`: the highest number of a table, empty when none.
const GPU_CAP_LIFT: &str = "\
gpu_cap_top() { ct=; for cn in $(cat $1 2>/dev/null); do case $cn in ''|*[!0-9]*) continue ;; esac; [ \"$cn\" -gt \"${ct:-0}\" ] && ct=$cn; done; echo $ct; }\n\
GM=/sys/kernel/gpu/gpu_max_clock\nGK=/sys/class/kgsl/kgsl-3d0/max_gpuclk\ngm_old=\ngk_old=\n\
for g in \"$GM /sys/kernel/gpu/gpu_freq_table gm_old\" \"$GK /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies gk_old\"; do\n  \
set -- $g; [ -w $1 ] || { echo \"gpu: no $1: its cap is not Samsung's to lift\"; continue; }\n  \
old=$(cat $1); top=$(gpu_cap_top $2)\n  \
case $old in ''|*[!0-9]*) top=x ;; esac\n  \
case $top in ''|*[!0-9]*) echo \"gpu: $1 reads '$old', top '$top': left alone\"; continue ;; esac\n  \
[ \"$old\" -lt \"$top\" ] || { echo \"gpu: $1 is $old, at the top already\"; continue; }\n  \
echo $top > $1 && eval \"$3=$old\" && echo \"gpu: Samsung's cap $1 $old lifted to $(cat $1) while One UI is down (thermal_pwrlevel $(cat /sys/class/kgsl/kgsl-3d0/thermal_pwrlevel 2>/dev/null))\"\n\
done\n";
const GPU_CAP_RESTORE: &str = "\
[ -z \"$gk_old\" ] || { echo \"$gk_old\" > $GK && echo \"gpu: Samsung's cap $GK is $(cat $GK) again\"; }\n  \
[ -z \"$gm_old\" ] || { echo \"$gm_old\" > $GM && echo \"gpu: Samsung's cap $GM is $(cat $GM) again\"; }\n  \
gk_old=\n  gm_old=\n  ";

/// take19: what One UI's display manager says of the panel, read once before the takeover stops it (`dumpsys display`, read
/// only), for the guest's tunables: the rounded corners' radii and centres in the panel's pixels (xnuloader-crosvm
/// --display-corner-radius, in points: radius x 720 / native width / 2 at 720x1560@2x), the display cutout (the camera's
/// hole) and the modes with their refresh rates (a variable rate, vm-boot --panel-rate-min). One `panel:` line each in
/// run.txt; nothing when dumpsys says none.
pub const PANEL_FACTS: &str = "\
# the panel's corners, cutout and modes from One UI's display manager (read only), before it stops\n\
pd=$(timeout 10 dumpsys display 2>/dev/null </dev/null)\n\
for pat in 'RoundedCorner{[^}]*}' 'DisplayCutout{[^}]*}' 'supportedModes [^]]*]' 'appVsyncOffsetNanos [0-9]*' 'refreshRateRange[^,]*,[^,]*'; do\n  \
pv=$(echo \"$pd\" | grep -o \"$pat\" | head -4 | tr '\\n' ' ' | cut -c1-600)\n  \
[ -z \"$pv\" ] || echo \"panel: $pv\"\n\
done\n\
pd=\n";

/// The media stream's volume to its maximum (a takeover, before One UI stops: `cmd media_session` is One UI's own service):
/// stream 3 is STREAM_MUSIC. The range and the current value are read from `--get` ("volume is 7 in range [0..15]"), the maximum
/// written with `--set`, and what it reads afterwards is logged on the `audio:` line; Android's older `media volume` is the
/// second try. The value read is kept in `vold` (with the tool in `vtool`) and `volume_restore` puts it back once One UI is
/// back: it runs after `start_ui` in the script's trap, waits for the service to answer again (60 s at most: the framework
/// takes its time to come up, and the failsafe may be the one that starts it), writes the value and logs it. A volume whose
/// current value could not be read is still set to the maximum (the owner asked for it) and is not put back: the line says so.
/// Nothing is guessed: no range read, nothing written. `--no-max-volume`: none of this is in the script (volume_restore is
/// the empty function takeover_script defines).
pub const MEDIA_VOLUME: &str = "\
# the media volume to its maximum before One UI stops (cmd media_session; the old `media volume` as the second try); the value\n\
# it had is kept and written back by volume_restore after One UI is up again\n\
vmax=\nrs=\nvold=\nvtool=\nfor tool in \"cmd media_session volume\" \"media volume\"; do\n  \
r=$(timeout 10 $tool --stream 3 --get 2>&1 </dev/null | tr '\\n' ' ')\n  \
rs=\"$rs[$tool: ${r:-no output}] \"\n  \
m=$(echo \"$r\" | sed -n 's/.*\\.\\.\\([0-9][0-9]*\\)\\].*/\\1/p')\n  \
case $m in ''|*[!0-9]*) continue ;; esac\n  \
vc=$(echo \"$r\" | sed -n 's/.*volume is \\([0-9][0-9]*\\) in range.*/\\1/p')\n  \
timeout 10 $tool --stream 3 --set $m >/dev/null 2>&1 </dev/null\n  \
now=$(timeout 10 $tool --stream 3 --get 2>&1 </dev/null | tr '\\n' ' ')\n  \
echo \"audio: the media volume was: $r; set to $m with '$tool': now: $now\"; vmax=$m; vtool=$tool\n  \
case $vc in ''|*[!0-9]*) echo \"audio: its value before was not read: it will not be put back\" ;; *) vold=$vc; echo \"audio: $vold will be put back after One UI is up\" ;; esac\n  \
break\n\
done\n\
[ -n \"$vmax\" ] || echo \"audio: the media volume is as it was: no range read: $rs\"\n\
volume_restore() {\n  \
[ -n \"$vold\" ] || return 0\n  \
vw=0; while [ $vw -lt 60 ]; do\n    \
vnow=$(timeout 10 $vtool --stream 3 --get 2>&1 </dev/null | tr '\\n' ' ')\n    \
case $vnow in *\"in range\"*) break ;; esac\n    \
sleep 2; vw=$((vw + 2))\n  \
done\n  \
case $vnow in *\"in range\"*) ;; *) echo \"audio: the media volume service did not answer in $vw s: $vold is not put back\"; vold=; return 0 ;; esac\n  \
timeout 10 $vtool --stream 3 --set $vold >/dev/null 2>&1 </dev/null\n  \
vnow=$(timeout 10 $vtool --stream 3 --get 2>&1 </dev/null | tr '\\n' ' ')\n  \
echo \"audio: the media volume is put back to $vold: now: $vnow\"; vold=\n\
}\n";

/// The takeover (vm-boot --takeover), after the checks and before crosvm: One UI's framework stopped as hugepool's grow stops
/// it (ui.sh: the way back armed first, the failsafe at LIMIT and a trap that starts One UI on any exit, the framework kept
/// down, Samsung's software watchdog fed), its RAM freed, and the CPUs' caps (the profile's or the power saver's
/// scaling_max_freq; thermal limits are not these) lifted to each cluster's top clock until the end. With One UI down nothing
/// puts them back, turns the screen off for want of touches or lowers the clocks between them (full3: the profile's 2 GHz
/// cap came back a minute after it was lifted). Empty without the takeover.
/// The takeover's memory steps after One UI's stop (take18, from upstream OneiOS 2894806 "takeover-ram"), as two parts.
/// The first, right after the stop (before the pool's top-up, which it helps): `sync` and `drop_caches` 3 as before, then
/// (RECLAIM) each zram device's `compact`, with a line of MemAvailable before and after and zram's pool (mm_stat: original,
/// compressed and RAM used, MiB). The second, once caps_restore is defined (the trap and the end both run it, and it runs
/// swap_restore first): SWAPPINESS (0-200) as `vm.swappiness` for the session, One UI's value saved first.
pub fn takeover_memory_steps(reclaim: bool, swappiness: Option<u32>) -> (String, String) {
    let mut early = String::from("swap_restore() { :; }\nsync\necho 3 > /proc/sys/vm/drop_caches\n");
    if reclaim {
        early += "m0=$(sed -n 's/^MemAvailable: *\\([0-9]*\\).*/\\1/p' /proc/meminfo)\nzn=0\n\
              for z in /sys/block/zram*; do [ -w $z/compact ] || continue; echo 1 > $z/compact && zn=$((zn + 1)); done\n\
              m1=$(sed -n 's/^MemAvailable: *\\([0-9]*\\).*/\\1/p' /proc/meminfo)\n\
              zr=; for z in /sys/block/zram*; do [ -r $z/mm_stat ] || continue; set -- $(cat $z/mm_stat); zr=\"$zr ${z##*/} $(($1 >> 20))/$(($2 >> 20))/$(($3 >> 20))\"; done\n\
              echo \"takeover: caches dropped, zram compacted on $zn device(s); MemAvailable $((${m0:-0} / 1024)) -> $((${m1:-0} / 1024)) MiB; zram orig/compressed/RAM MiB:${zr:- none}\"\n";
    }
    let late = match swappiness {
        None => String::new(),
        Some(v) => format!(
            "sw_old=$(cat /proc/sys/vm/swappiness)\n\
             swap_restore() {{ [ -z \"$sw_old\" ] || {{ echo \"$sw_old\" > /proc/sys/vm/swappiness && echo \"takeover: vm.swappiness back: $(cat /proc/sys/vm/swappiness)\"; }}; sw_old=; }}\n\
             echo {v} > /proc/sys/vm/swappiness && echo \"takeover: vm.swappiness $sw_old -> $(cat /proc/sys/vm/swappiness) while One UI is down\" || sw_old=\n"
        ),
    };
    (early, late)
}

/// The panel awake as One UI stops (take18, from YMGPwcca/OneiOS 843b06e and f048161, "bounded lockscreen WAKEUP pulses"):
/// One UI's keyguard darkens the panel seconds after the last wake, and once the framework is down nothing wakes it (input
/// needs system_server). A wake right before the stop, while it is still up; the display helper then holds the panel on
/// itself (OneiOSDisplay --screen-file: POWER_MODE_NORMAL at its start). One event, not a loop: a refused one is a line.
const TAKEOVER_WAKE: &str = "input keyevent KEYCODE_WAKEUP </dev/null >/dev/null 2>&1 && echo \"takeover: KEYCODE_WAKEUP sent as One UI stops\" || \
echo \"takeover: input refused KEYCODE_WAKEUP (the panel as it was)\"\n";

/// vm-boot's Samsung GPU cap lift (take18): the default with --takeover (take17's whole session ran at a cap of 607 MHz and
/// the lock screen's frames were GPU-bound at it); --keep-gpu-cap keeps the cap as One UI left it; --gpu-cap-lift asks for
/// the lift whatever --keep-gpu-cap says, and alone (without --takeover) is refused by check_boost as before.
pub fn gpu_cap_lift(takeover: bool, lift: bool, keep: bool) -> bool {
    lift || (takeover && !keep)
}

pub fn takeover_script(takeover: bool, need_pages: u64, gpu_cap: bool) -> (String, String) {
    takeover_script_with(takeover, need_pages, gpu_cap, true, None, true)
}

pub fn takeover_script_with(takeover: bool, need_pages: u64, gpu_cap: bool, reclaim: bool, swappiness: Option<u32>, wake: bool) -> (String, String) {
    if !takeover {
        return (String::new(), String::new());
    }
    let memory = takeover_memory_steps(reclaim, swappiness);
    let stop = format!(
        "# One UI's framework stopped for the VM's life (vm-boot --takeover): ui.sh, the way back armed first\n\
         gpu_restore() {{ :; }}; cpu_restore() {{ :; }}; caps_restore() {{ :; }}; wifi_restore() {{ :; }}; volume_restore() {{ :; }}\n\
         . $D/ui.sh\n\
         rm -rf $CTL; mkdir -p $CTL\n\
         for svc in zygote_secondary perfmond media.tuner zygote_next; do getprop init.svc.$svc > $CTL/base.$svc; done\n\
         {wake}{}\
         trap 'thp_restore; gpu_restore; cpu_restore; caps_restore; wifi_restore; start_ui \"the session ended\"; volume_restore' EXIT HUP INT TERM\n\
         {memory_early}\
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
         {gpu_cap_lift}\
         CF=/sys/devices/system/cpu/cpufreq\ncaps=\n\
         caps_restore() {{\n  \
         swap_restore\n  \
         [ -z \"$sl_old\" ] || [ \"$sl_old\" = -1 ] || {{ echo \"$sl_old\" > $SL && echo \"cpu: Samsung's cpufreq_limit max is $(cat $SL) again\"; }}\n  \
         sl_old=\n  {gpu_cap_restore}[ -n \"$caps\" ] || return 0\n  \
         for pm in $caps; do echo \"${{pm#*=}}\" > $CF/policy${{pm%%=*}}/scaling_max_freq; done\n  \
         echo \"cpu: the clusters' caps are back:$caps\"\n  caps=\n}}\n\
         for p in 0 1 2 3 4 5 6 7; do\n  \
         f=$CF/policy$p\n  [ -w $f/scaling_max_freq ] && [ -r $f/cpuinfo_max_freq ] || continue\n  \
         old=$(cat $f/scaling_max_freq); top=$(cat $f/cpuinfo_max_freq)\n  [ \"$old\" = \"$top\" ] && continue\n  \
         echo $top > $f/scaling_max_freq && caps=\"$caps $p=$old\" && \
         echo \"cpu: policy$p's cap is lifted to $(cat $f/scaling_max_freq) kHz while One UI is down (it was $old)\"\n\
         done\n{memory_late}",
        crate::hugepool::ui_stop(),
        memory_early = memory.0,
        memory_late = memory.1,
        wake = if wake { TAKEOVER_WAKE } else { "" },
        gpu_cap_lift = if gpu_cap { GPU_CAP_LIFT } else { "" },
        gpu_cap_restore = if gpu_cap { GPU_CAP_RESTORE } else { "" },
    );
    (stop, "caps_restore\n".to_string())
}

#[allow(clippy::too_many_arguments)]
pub fn boot_script_with(l: &Label, seconds: u32, thp: bool, sha: &str, disks: Option<&Disks>, swipe: bool, pin: bool, t: &Tuning) -> String {
    let (gpu_floor, gpu_restore) = gpu_floor_script(l, t.gpu_floor_mhz, t.gpu_idle_ms);
    let (cpu_floor, cpu_restore) = cpu_floor_script(t.cpu_floor_percent);
    // the pool pages the VM's RAM takes, the shared-RAM VM's allowance outside it aside (check_pool)
    let need = (u64::from(l.ram).saturating_sub(SMALL_PAGES_MIB_SHARED) + 1) / 2;
    let (mut takeover_stop, caps_restore) =
        takeover_script_with(t.takeover, need, t.gpu_cap_lift, t.takeover_reclaim, t.takeover_swappiness, t.takeover_wake);
    // the touch boost: what ends the forwarder after crosvm, before the floors' and the caps' ends
    let boost = t.touch_boost && disks.is_some_and(|d| d.input);
    // at the script's end only: a signal does what it does without the boost (the VM may run on, and needs its touch)
    let (boost_trap, boost_end) = if boost { ("trap 'thp_restore; boost_restore' EXIT\n", "boost_restore\n") } else { ("", "") };
    let thp_trap = if boost { "trap thp_restore HUP INT TERM\n  trap 'thp_restore; boost_restore' EXIT" } else { "trap thp_restore EXIT HUP INT TERM" };
    if boost {
        let ends = "gpu_restore; cpu_restore; caps_restore; wifi_restore; start_ui \"the session ended\"; volume_restore'";
        takeover_stop = takeover_stop.replace(
            &format!("trap 'thp_restore; {ends} EXIT HUP INT TERM\n"),
            &format!("trap 'thp_restore; {ends} HUP INT TERM\ntrap 'thp_restore; boost_restore; {ends} EXIT\n"),
        );
    }
    if t.takeover && t.max_volume {
        // after the placeholders (volume_restore's empty one) and before ui.sh: One UI is still up while it is read and set
        let at = takeover_stop.find(". $D/ui.sh\n").expect("ui.sh is sourced");
        takeover_stop.insert_str(at, MEDIA_VOLUME);
    }
    if t.takeover {
        // take19: the panel's corners, cutout and modes as One UI's display manager states them, read before it stops
        let at = takeover_stop.find(". $D/ui.sh\n").expect("ui.sh is sourced");
        takeover_stop.insert_str(at, PANEL_FACTS);
    }
    // the performance log and the device workers' CPUs (vmperf.rs): started with the thermal log, once crosvm runs; its end is
    // waited for after everything is put back
    let thermal_log = thermal_log_script(t.thermal_log) + &crate::vmperf::start_script(&t.perf);
    let perf_end = crate::vmperf::end_script(&t.perf);
    // the panel's vsync for the refresh tick (vm-boot --vsync-lock): a shared-RAM VM with the screen's page; pacectl starts
    // once crosvm's tree was accepted and is ended after the VM, outside every restore
    let vsync = t.vsync.filter(|_| l.shared).and_then(|phase| disks.and_then(|d| d.fb.as_ref().map(|f| (d.pool - RAM_BASE + f.seq, phase))));
    let (vsync_start, vsync_end) = match vsync {
        Some((page, phase)) => (crate::vmperf::vsync_start_script(page, phase), crate::vmperf::vsync_end_script()),
        None => (String::new(), String::new()),
    };
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
    // the bridge channel (an Image of xnuloader-crosvm --bridge): bridgectl serve listens before crosvm starts, crosvm's
    // second serial port connects to its socket (given only when the socket is there: crosvm does not start without it),
    // and it ends when crosvm closes the port
    let (serve_start, serve_serial, serve_end) = match disks {
        Some(d) if d.bridge => (
            BRIDGE_START.replace("{flags}", if d.bridge_haptics { "" } else { " --no-haptics" }),
            "${bsock:+ --serial type=unix-stream,hardware=serial,num=2,path=$bsock,input-unix-stream}",
            BRIDGE_END,
        ),
        _ => (String::new(), "", ""),
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
            "  forward=\n",
            "fi\n",
            "if [ -n \"$tsock\" ]; then\n",
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
                tk = if !t.takeover {
                    String::new()
                } else if t.panel_rate_min > 0 {
                    format!(" --takeover --screen-file $D/screen.off --rate-min {} --rate-hold-file $D/boost.state", t.panel_rate_min)
                } else {
                    " --takeover --screen-file $D/screen.off".to_string()
                }
            );
        }
        // Its last word: the bridge's, or in the scanout's mode the helper's (which has no `displayctl pool:` line)
        let ends = if d.scanout.is_some() {
            "^oneios-display: crosvm's (layer removed|display service is gone)"
        } else {
            "^displayctl pool: (the VM ended|time is up|stopped|the helper left)"
        };
        // After the VM: the bridge sees crosvm gone and closes its session (the layer goes); a bridge still there after
        // 3 s is asked to end, and after 8 s ended
        bridge_end = format!("if [ -n \"$bridge\" ]; then\n  \
             t=0; while [ $t -lt 8 ] && kill -0 $bridge 2>/dev/null; do [ $t = 3 ] && kill -TERM $bridge 2>/dev/null; sleep 1; t=$((t + 1)); done\n  \
             kill -0 $bridge 2>/dev/null && kill -9 $bridge 2>/dev/null\n  wait $bridge 2>/dev/null\n  \
             last=$(grep -E \"{ends}\" $D/display.txt | tail -n 1)\n  \
             echo \"display: ${{last:-ended without a last word; display.txt ends: $(tail -n 1 $D/display.txt)}}\"\nfi\n");
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
            // the decoder's pool rides on the same --pre-alloc (one option; crosvm lays drm2kgsl_host out first, then media_host)
            blocks += &format!(
                " \\\n  --gpu backend=virglrenderer,context-types={contexts},udmabuf=true,pci-address={},pci-bar-size=0x10000000{display} \\\n  --pre-alloc drm-host-mb={}{}",
                g.pci,
                g.drm_mib,
                d.media.map(|m| format!(",media-host-mb={}", m.mib)).unwrap_or_default()
            );
            if d.scanout.is_some() {
                blocks += &format!(" \\\n  --android-display-service name={CROSVM_DISPLAY},screen=gpu-0");
            }
        }
        if d.audio {
            // the speaker: crosvm's virtio-snd card played through AAudio (the guest's 48 kHz S16 periods, iOS's own volume
            // applied in the guest), no capture. Its backend in crosvm's process unless --audio-uid: Droid-VM's crosvm says a
            // stream of uid 0 is silenced while One UI runs; a takeover stops the framework, and the upstream port's root
            // AAudio helper played in its sessions so.
            // The microphone (an Image with oneios-audio-mic=1): take16's card had `capture=false` and crosvm's AAudio
            // backend opened the phone's input all the same at the guest's PREPARE (crosvm.log: `host audio endpoint (by
            // id) opened on device 22`, 10 s after the output's), so the key does not gate the stream there; what else it
            // changes is not known (its help: "Disable/enable audio capture"). --mic-capture gives `capture=true`.
            blocks += &format!(" \\\n  --virtio-snd backend=aaudio,capture={}{}", t.mic_capture, t.audio_uid.map(|u| format!(",uid={u}")).unwrap_or_default());
        }
        if d.wifi && t.takeover {
            // the Wi-Fi: crosvm's virtio-net card on the tap wifictl made, with wlan0's address (WIFI_START); none when
            // wifictl did not take wlan0
            blocks += &format!(" \\\n  ${{wifi_mac:+--net tap-name={WIFI_TAP},mac=$wifi_mac}}");
        }
        if let Some(m) = d.media {
            // the video decoder's pool: always, since the Image's layout has it (without the GPU's --pre-alloc above, its
            // own); the device (crosvm's virtio-media decoder, MediaCodec in a helper under the uid) only with --video-decoder
            if d.gpu.is_none() {
                blocks += &format!(" \\\n  --pre-alloc media-host-mb={}", m.mib);
            }
            if let Some(uid) = t.video_decoder {
                blocks += &format!(" \\\n  --virtio-media kind=decoder,uid={uid}");
            }
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
                    &(d.gpu
                        .as_ref()
                        .map(|g| {
                            format!(
                                "  if [ \"$pool\" != bad ]; then if grep -q -F drm2kgsl_host@{x:x} $D/fdt.dtb; then echo \"gpu: drm2kgsl's host arena is at 0x{x:x}, as the Image's drm-host-pool\"; \
                                 else echo \"gpu: crosvm's tree has no drm2kgsl_host@{x:x}: stopped before the kernel's GPU starts\"; pool=bad; touch $D/stop.request; fi; fi\n",
                                x = g.drm_host
                            )
                            + &d.gpu_intid.map(|n| gpu_intid_check(&g.pci, n)).unwrap_or_default()
                        })
                        .unwrap_or_default()
                        + &d.audio_intid.map(|(n, slot)| audio_intid_check(slot, n)).unwrap_or_default()
                        + &d
                            .media
                            .map(|m| {
                                // the decoder's pool where the Image's media-host-pool says (crosvm's media_host@ node)
                                format!(
                                    "  if [ \"$pool\" != bad ]; then if grep -q -F media_host@{x:x} $D/fdt.dtb; then echo \"video: crosvm's media_host pool is at 0x{x:x}, as the Image's media-host-pool ({mib} MiB)\"; \
                                     else echo \"video: crosvm's tree has no media_host@{x:x}: stopped before the kernel's decoder starts\"; pool=bad; touch $D/stop.request; fi; fi\n",
                                    x = m.host,
                                    mib = m.mib
                                )
                            })
                            .unwrap_or_default()),
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
        // The frames with their scan time (vm-boot --touch-time): a flag of the forwarder's, before its redirections
        if t.touch_time {
            touch_find = touch_find.replace(" </dev/null >>$D/touch.txt", " --scan-time </dev/null >>$D/touch.txt");
        }
        if boost {
            touch_find = touch_find.replace("tsock=\nforward=\n", &format!("tsock=\nforward=\n{}", touch_boost_script(l)))
                .replace(" </dev/null >>$D/touch.txt", "${boostv:+ --boost $boostv --boost-state $D/boost.state} </dev/null >>$D/touch.txt");
        }
        // In a takeover the side keys go to the VM too (read, never grabbed: One UI is down) and a click of power switches the
        // panel: displayctl touch makes screen.off, the helper switches the panel by it (OneiOSDisplay --screen-file)
        if t.takeover {
            touch_find = touch_find
                .replacen("tsock=\nforward=\n", "tsock=\nforward=\nkl=; for k in $keys; do kl=\"${kl:+$kl,}/dev/input/$k\"; done\n", 1)
                .replace(" </dev/null >>$D/touch.txt 2>&1 &\n", "${kl:+ --keys $kl --screen-file $D/screen.off} </dev/null >>$D/touch.txt 2>&1 &\n");
        }
    }
    // crosvm's Android display blits each frame into the helper's Surface on the GPU with the Vulkan driver it is named (it
    // loads none by itself, and does a CPU copy per frame without one): Droid-VM's Turnip of the package (droidvm::FILES)
    let crosvm_env = if disks.is_some_and(|d| d.scanout.is_some()) {
        format!("CROSVM_DISPLAY_VULKAN_LIBRARY={}/libvulkan_freedreno.so ", crate::droidvm::DIR)
    } else {
        String::new()
    } + &t.crosvm_env;
    format!(
        r#"D={DIR}
C={crosvm}
H=$D/oneios-display
exec >>$D/run.txt 2>&1 </dev/null
cd $D || exit 1
end() {{ echo "$*" > $D/result; }}
{BOOST_LEFTOVER}got=$(sha256sum kernel.img | cut -d' ' -f1)
[ "$got" = "{sha}" ] || {{ echo "sha256 $got, not {sha}"; end "refused sha256"; exit 1; }}
[ -c /dev/gunyah ] || {{ echo "no /dev/gunyah"; end "refused no /dev/gunyah"; exit 1; }}
{share_check}
tdev=
tsock=
pid=
{checks}# the last session's, kept once more: when the PC did not see its end they are all there is of it
for f in ui.log stop.txt; do [ ! -s $f ] || mv -f $f $f.prev; done
rm -f serial.txt crosvm.log crosvm.sock fdt.dtb dmesg.txt display.txt touch.txt touch.sock stop.request screen.off wifi.txt wifi.mac thermal.txt ui.log stop.txt perf.txt pace.txt hostmem.txt orch.txt orch.stop vsync.phase boost.state.new bridge.txt bridge.sock
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
{boost_trap}if [ {thp} = 1 ]; then
  old_shmem=$(tr ' ' '\n' < $T/shmem_enabled | grep '^\[' | tr -d '\133\135')
  old_defrag=$(tr ' ' '\n' < $T/defrag | grep '^\[' | tr -d '\133\135')
  case "$old_shmem" in always|within_size|advise|never|deny|force) ;; *) echo "thp: shmem_enabled '$old_shmem'?"; end "refused thp"; exit 1 ;; esac
  case "$old_defrag" in always|defer|defer+madvise|madvise|never) ;; *) echo "thp: defrag '$old_defrag'?"; end "refused thp"; exit 1 ;; esac
  thp_done=0
  {thp_trap}
  if ! {{ echo advise > $T/shmem_enabled && echo madvise > $T/defrag; }}; then
    echo "thp: the write was refused"; thp_restore; end "refused thp"; exit 1
  fi
  echo "thp set: shmem_enabled $(cat $T/shmem_enabled) | defrag $(cat $T/defrag)"
fi
grep -E '^(MemAvailable|SwapFree):' /proc/meminfo | tr -s ' ' | sed 's/^/before /'
echo "start $(date +%s) up $(cut -d' ' -f1 /proc/uptime)"
{takeover_stop}{gpu_floor}{wifi_start}{serve_start}pool=
# crosvm logs at info: at debug its GPU renderer wrote a line per submission and fence (take8: 407 MB in 45 minutes,
# whose copy at the end broke the adb link and left the session's other files empty)
set -f
{crosvm_env}$C --log-level info run --disable-sandbox --no-balloon --no-rng --no-usb --hypervisor gunyah \
  {protection} --hugepages --mem size={mem} --cpus num-cores={cpus}{affinity} \
  --socket $D/crosvm.sock${{tsock:+ --input multi-touch[path=$tsock,width=4095,height=4095,name=oneios-touch]}} --serial type=file,hardware=serial,num=1,path=$D/serial.txt,console{serve_serial} \
  --dump-device-tree-blob $D/fdt.dtb{blocks} $D/kernel.img </dev/null >$D/crosvm.log 2>&1 &
pid=$!
set +f
{pool_check}{bridge_start}{vsync_start}{escape_start}{cpu_floor}{thermal_log}t=0
while [ $t -lt {seconds} ] && [ ! -e $D/stop.request ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
how=itself
if kill -0 $pid 2>/dev/null; then
  how=stop; $C stop $D/crosvm.sock </dev/null >$D/stop.txt 2>&1; echo "stop: crosvm stop's status $?"
  t=0; while [ $t -lt 20 ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
fi
if kill -0 $pid 2>/dev/null; then
  how=kill
  # what it waited for, for the reason of the 20 s (stop.txt): nothing here may hold the kill back
  ( for k in /proc/$pid/task/*; do echo "${{k##*/}} $(cat $k/comm 2>/dev/null): $(grep '^State:' $k/status 2>/dev/null | tr '\t' ' ') $(cat $k/wchan 2>/dev/null)"; done >>$D/stop.txt 2>&1 ) &
  sleep 1
  kill -9 $pid
fi
wait $pid; rc=$?
thp_restore
{boost_end}{wifi_end}{gpu_restore}{cpu_restore}{caps_restore}{bridge_end}{escape_end}{serve_end}{vsync_end}{perf_end}echo "end $(date +%s) up $(cut -d' ' -f1 /proc/uptime) how $how exit $rc"
dmesg 2>/dev/null | grep -i -E 'gunyah|folio|gh_|crosvm|scm|memextent' | tail -n 80 > $D/dmesg.txt
if [ "$pool" = bad ]; then end "refused pool: end how $how exit $rc"; else end "end how $how exit $rc"; fi
"#,
        thp = if thp { 1 } else { 0 }
    )
}

/// The bridge channel's start in boot.sh (an Image of xnuloader-crosvm --bridge): `bridgectl serve` listens on bridge.sock
/// before crosvm starts, which connects its second serial port to it while it makes its devices (as the touch's forwarder
/// and its socket). crosvm is given the port only when the socket is there: it does not start without it. bridgectl reads
/// the phone's battery (read only) and drives the motor's force-feedback device; never a side key's device.
pub const BRIDGE_START: &str = r#"# the bridge channel: the phone's battery to iOS, iOS's haptics to the motor (bridgectl serve, runtime/bridges)
bsock=; bridged=
if [ -x $D/bridgectl ]; then
  rm -f $D/bridge.sock
  $D/bridgectl serve $D/bridge.sock --wait 120{flags} </dev/null >>$D/bridge.txt 2>&1 &
  bridged=$!
  t=0; while [ $t -lt 50 ] && [ ! -S $D/bridge.sock ] && kill -0 $bridged 2>/dev/null; do sleep 0.1 2>/dev/null || sleep 1; t=$((t + 1)); done
  if [ -S $D/bridge.sock ]; then
    bsock=$D/bridge.sock; echo "bridge: bridgectl serve ($bridged) is the other end of crosvm's second serial port: $(grep '^serve: listening' $D/bridge.txt | tail -n 1 | sed 's/.*serial port: //'); bridge.txt"
  else
    echo "bridge: bridgectl serve did not start (bridge.txt): the VM runs without the battery and the haptics"; kill $bridged 2>/dev/null; bridged=
  fi
else
  echo "bridge: off: no $D/bridgectl"
fi
"#;

/// After the VM: bridgectl serve saw crosvm close the port and ended (its motor stopped); one still there 3 s on is ended.
pub const BRIDGE_END: &str = r#"if [ -n "$bridged" ]; then
  t=0; while [ $t -lt 3 ] && kill -0 $bridged 2>/dev/null; do sleep 1; t=$((t + 1)); done
  kill $bridged 2>/dev/null; wait $bridged 2>/dev/null
  echo "bridge: $(grep '^serve: ' $D/bridge.txt | tail -n 1)"
fi
"#;

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
    /// KGSL's idle_timer while the VM runs, ms (Tuning::gpu_idle_ms)
    pub gpu_idle_ms: u32,
    /// The touch boost (Tuning::touch_boost)
    pub touch_boost: bool,
    /// Each touch frame with its scan time (Tuning::touch_time)
    pub touch_time: bool,
    /// The panel's vsync for the VM's refresh tick and its phase (Tuning::vsync)
    pub vsync: Option<i32>,
    /// pacectl for the phone (runtime/pace: make), pushed with the vsync
    pub pacectl: String,
    /// The performance log and the device workers' CPUs (Tuning::perf)
    pub perf: crate::vmperf::Perf,
    /// One UI's framework stopped for the VM's life (takeover_script)
    pub takeover: bool,
    /// --gpu-cap-lift, with the takeover (Tuning::gpu_cap_lift)
    pub gpu_cap_lift: bool,
    /// false (--no-max-volume): the media volume is left as it is (Tuning::max_volume)
    pub max_volume: bool,
    /// With pin: the first core of the vCPUs (Tuning::pin_first)
    pub pin_first: u32,
    /// thermal.txt (Tuning::thermal_log)
    pub thermal_log: bool,
    /// The sound card's backend uid (Tuning::audio_uid)
    pub audio_uid: Option<u32>,
    /// The sound card with capture=true (Tuning::mic_capture)
    pub mic_capture: bool,
    /// crosvm's virtio-media decoder for an Image with its pool, its helper under this uid (Tuning::video_decoder)
    pub video_decoder: Option<u32>,
    /// wifictl for the phone (runtime/bridges: make), pushed for an Image with the Wi-Fi in a takeover
    pub wifictl: String,
    /// bridgectl for the phone (runtime/bridges: make), pushed for an Image with the bridge channel (xnuloader-crosvm --bridge)
    pub bridgectl: String,
    /// false (--no-haptics): the bridge channel serves the battery alone
    pub haptics: bool,
    /// The host renderer pushed with Droid-VM's package (droidvm::push_with): a libvirglrenderer.so, or `prebuilt`
    pub virgl: String,
    /// crosvm pushed with Droid-VM's package (droidvm::push_with, take18b): `prebuilt` (the package's own, the default), or a
    /// crosvm built from source (artifacts/crosvm) in its place
    pub crosvm: String,
    /// --gpu-host-floors: which of take18's renderer's power floors crosvm keeps (host_renderer_env, Floors)
    pub gpu_host_floors: Floors,
    /// NAME=VALUE for crosvm's environment (--crosvm-env), checked by host_renderer_env
    pub crosvm_env: Vec<String>,
    /// false (--no-takeover-reclaim): no zram compaction after the takeover's stop (Tuning::takeover_reclaim)
    pub takeover_reclaim: bool,
    /// --takeover-swappiness N (Tuning::takeover_swappiness)
    pub takeover_swappiness: Option<u32>,
    /// false (--no-takeover-wake): no KEYCODE_WAKEUP before the stop (Tuning::takeover_wake)
    pub takeover_wake: bool,
    /// --panel-rate-min HZ (Tuning::panel_rate_min; 0: the panel held at its top as before)
    pub panel_rate_min: u32,
    /// take23: the new boot order, hostmem.txt and the clocks' orchestrator (take23.rs)
    pub take23: crate::take23::Opts,
}

/// The power floors of take18's renderer (virglrenderer 2529a9f) that crosvm keeps (--gpu-host-floors). The renderer has
/// three: the process-wide rate floor (NCTX_PWR_CONSTRAINT: every context at the GPU's top level while all the guest's
/// processes together submit 130 or more a second, held 400 ms), the deadline queues' constraint (DRM2KGSL_PWR_CONSTRAINT:
/// backboardd's queue, made at high priority, at the top level while it submits 60 or more a second) and the vCPUs' clamp
/// that follows that one (DRM2KGSL_VCPU_UCLAMP_MIN 1024).
/// take18 (thermal.txt, 1295 samples): the GPU at 1200 MHz in 72 % of them, 407 of those under 20 % busy, i.e. held there,
/// not asked for; crosvm.log's uncapped `PWR hot`/`PWR cold` lines put the rate floor alone at 511 s of the 1638 (31 %), on
/// contexts of every app (video, Maps), not only the compositor's. The deadline constraint and the clamp print 16 lines
/// each and then nothing (the renderer caps them), so their share is not in the log.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Floors {
    /// all three (take18's session)
    On,
    /// take19's default: the compositor's deadline constraint and the vCPUs' clamp while it animates (app closes and the
    /// home screen's animations keep the GPU and the vCPUs at their top), without the process-wide rate floor
    /// (NCTX_PWR_CONSTRAINT=0): an app's own drawing (a video, Maps) has the governor's clock
    Deadline,
    /// none: the governors alone, as take17 (NCTX_PWR_CONSTRAINT=0 DRM2KGSL_PWR_CONSTRAINT=0 DRM2KGSL_VCPU_UCLAMP_MIN=0)
    Off,
}

impl Floors {
    /// --gpu-host-floors on|deadline|off
    pub fn parse(word: &str) -> Result<Floors> {
        match word {
            "on" => Ok(Floors::On),
            "deadline" => Ok(Floors::Deadline),
            "off" => Ok(Floors::Off),
            other => bail!("--gpu-host-floors {other}: on, deadline or off"),
        }
    }
}

/// crosvm's environment for the host renderer: the floors' off switches (Floors) for those not wanted, then each
/// --crosvm-env, as `NAME=VALUE ` words before crosvm's path. A name or value outside the safe set is refused (the words go
/// into the boot script unquoted).
pub fn host_renderer_env(floors: Floors, extra: &[String]) -> Result<String> {
    let mut words = Vec::new();
    match floors {
        Floors::On => {}
        Floors::Deadline => words.push("NCTX_PWR_CONSTRAINT=0".to_string()),
        Floors::Off => {
            words.extend(["NCTX_PWR_CONSTRAINT=0", "DRM2KGSL_PWR_CONSTRAINT=0", "DRM2KGSL_VCPU_UCLAMP_MIN=0"].map(String::from))
        }
    }
    for e in extra {
        let Some((name, value)) = e.split_once('=') else { bail!("--crosvm-env {e}: NAME=VALUE") };
        let name_ok = !name.is_empty() && name.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_')
            && !name.as_bytes()[0].is_ascii_digit();
        let value_ok = value.bytes().all(|b| b.is_ascii_alphanumeric() || b"_.:/-".contains(&b));
        if !name_ok || !value_ok {
            bail!("--crosvm-env {e}: NAME of A-Z 0-9 _, VALUE of A-Z a-z 0-9 _ . : / -");
        }
        if ["LD_PRELOAD", "LD_LIBRARY_PATH", "PATH", "CROSVM_DISPLAY_VULKAN_LIBRARY"].contains(&name) {
            bail!("--crosvm-env {name}: not this way (vm-boot sets what crosvm runs with)");
        }
        words.push(e.clone());
    }
    Ok(words.iter().map(|w| format!("{w} ")).collect())
}

/// The GPU's idle_timer and the touch boost: an Image that has what they are of, and one at a time with the floors, which the
/// boost would read as the clocks to put back.
fn check_boost(l: &Label, o: &Options, disks: Option<&Disks>) -> Result<()> {
    if o.gpu_idle_ms > 60000 {
        bail!("--gpu-idle-timer {}: 0 (the driver's own) to 60000 ms", o.gpu_idle_ms);
    }
    if o.gpu_idle_ms > 0 && !l.gpu {
        bail!("--gpu-idle-timer needs an Image with the GPU (xnuloader-crosvm --gpu)");
    }
    if o.gpu_cap_lift && !o.takeover {
        bail!("--gpu-cap-lift needs --takeover: with One UI running, Samsung's services write the cap back");
    }
    if o.touch_boost && !disks.is_some_and(|d| d.input) {
        bail!("--touch-boost needs an Image with the touch (xnuloader-crosvm --display)");
    }
    if o.touch_boost && (o.gpu_floor_mhz > 0 || o.cpu_floor_percent > 0) {
        bail!("--touch-boost with --gpu-floor or --cpu-floor: one or the other");
    }
    if o.panel_rate_min > 0 && (o.panel_rate_min > 119 || !o.takeover || !o.touch_boost || !disks.is_some_and(|d| d.scanout.is_some())) {
        bail!("--panel-rate-min HZ: 1 to 119, with --takeover, --touch-boost (its boost.state holds the top while a touch is recent) and an Image with the GPU's scanout");
    }
    if o.touch_boost {
        let has = fs::read(&o.bridge).with_context(|| format!("cannot read {}", o.bridge))?.windows(13).any(|w| w == b"--boost-state");
        if !has {
            bail!("{} has no touch boost (an older displayctl, which would refuse its line and leave the VM without touch): make -C runtime/display", o.bridge);
        }
    }
    if let Some(phase) = o.vsync {
        if !l.shared || !disks.is_some_and(|d| d.fb.is_some()) {
            bail!("--vsync-lock needs a shared-RAM Image with the screen (xnuloader-crosvm --shared-ram --display): the page it writes is in the VM's RAM");
        }
        if !(-100_000..=100_000).contains(&phase) {
            bail!("--vsync-phase-us {phase}: -100000 to 100000 (a tenth of a second either way)");
        }
        if !Path::new(&o.pacectl).is_file() {
            bail!("{} is not there: make -C runtime/pace (or name it with --pacectl)", o.pacectl);
        }
    }
    if o.touch_time && !disks.is_some_and(|d| d.input) {
        bail!("--touch-time needs an Image with the touch (xnuloader-crosvm --display)");
    }
    if o.touch_time {
        let has = fs::read(&o.bridge).with_context(|| format!("cannot read {}", o.bridge))?.windows(11).any(|w| w == b"--scan-time");
        if !has {
            bail!("{} has no --scan-time (an older displayctl, which would refuse its line and leave the VM without touch): make -C runtime/display", o.bridge);
        }
    }
    Ok(())
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
    if o.takeover_swappiness.is_some_and(|v| v > 200) || (o.takeover_swappiness.is_some() && !o.takeover) {
        bail!("--takeover-swappiness N: 0 to 200, with --takeover");
    }
    // the host renderer's switches, checked before anything goes to the phone
    let crosvm_env = host_renderer_env(o.gpu_host_floors, &o.crosvm_env)?;
    if !crosvm_env.is_empty() {
        println!("vm-boot: crosvm's environment for the host renderer: {}", crosvm_env.trim_end());
    }
    if l.ram > 480 && !o.thp {
        bail!("a VM of {} MiB needs --thp and gh_hugepage_reserve's pool (Gunyah's own memory ends near 0.5 GB of 4 KiB pages)", l.ram);
    }
    let list = format!("{image}.disks");
    let mut disks = match fs::read_to_string(&list) {
        Ok(text) => Some(parse_disks(&text).with_context(|| format!("{list}"))?),
        Err(_) => None,
    };
    if let Some(d) = disks.as_mut() {
        d.bridge_haptics = o.haptics;
    }
    if disks.as_ref().is_some_and(|d| d.bridge) && !Path::new(&o.bridgectl).is_file() {
        bail!("the Image has the bridge channel and {} is not there: runtime/build.sh (or make -C runtime/bridges build/android/bridgectl), or name it with --bridgectl", o.bridgectl);
    }
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
    // take23 (take23.rs): the new boot order's Images, the largest first and the main one last; the orchestrator's numbers
    let t23 = &o.take23;
    let mut cands: Vec<crate::take23::Candidate> = Vec::new();
    let mut larger_files: Vec<(String, String)> = Vec::new();
    let orch = if t23.orchestrator {
        if !o.takeover || !o.gpu_cap_lift || !l.gpu || o.gpu_host_floors == Floors::Off || o.virgl == "prebuilt" {
            bail!("--orchestrator: with --takeover (its GPU cap lifted), an Image with the GPU, take18's renderer and its floors (--gpu-host-floors deadline or on)");
        }
        Some(crate::take23::Orch::parse(&t23.orch_set)?)
    } else {
        if !t23.orch_set.is_empty() {
            bail!("--orchestrator-set needs --orchestrator");
        }
        None
    };
    if t23.fill_after_stop {
        let need_of = |ram: u32| (u64::from(ram).saturating_sub(SMALL_PAGES_MIB_SHARED) + 1) / 2;
        let arena_of = |d: Option<&Disks>| d.and_then(|d| d.gpu.as_ref()).map(|g| format!("{:x}", g.drm_host)).unwrap_or_default();
        if !o.takeover || !l.shared {
            bail!("--fill-after-stop and --image-larger: with --takeover and a shared-RAM Image");
        }
        if o.vsync.is_some() {
            bail!("--fill-after-stop with --vsync-lock: the vsync page's place follows the RAM's size; not done");
        }
        if !(512..=4096).contains(&t23.keep_mib) {
            bail!("--host-keep-mib: 512 to 4096 MiB");
        }
        if l.ram > crate::take23::MAX_RAM_MIB {
            bail!("the Image's {} MiB: {} at most", l.ram, crate::take23::MAX_RAM_MIB);
        }
        // the same build but for the RAM: FILE.disks equal but for the two addresses that follow it
        let shape = |text: &str| text.lines().filter(|l| !l.starts_with("pool ") && !l.starts_with("gpu ")).collect::<Vec<_>>().join("\n");
        let main_text = fs::read_to_string(&list).unwrap_or_default();
        for path in &t23.larger {
            let more = fs::read(path).with_context(|| format!("cannot read {path}"))?;
            let l2 = label(&more).with_context(|| format!("{path}: not an Image of xnuloader-crosvm"))?;
            if l2.ram <= l.ram || l2.ram > crate::take23::MAX_RAM_MIB || (l2.cpus, l2.swiotlb, l2.shared, l2.gpu) != (l.cpus, l.swiotlb, l.shared, l.gpu) {
                bail!("--image-larger {path}: {} MiB, {} vCPUs: it must have more RAM than --image ({} MiB), {} MiB at most, and the same vCPUs, pool and GPU",
                      l2.ram, l2.cpus, l.ram, crate::take23::MAX_RAM_MIB);
            }
            let text2 = fs::read_to_string(format!("{path}.disks")).with_context(|| format!("no {path}.disks"))?;
            let d2 = parse_disks(&text2).with_context(|| format!("{path}.disks"))?;
            let drm = d2.gpu.as_ref().map(|g| g.drm_mib << 20).unwrap_or(0);
            if shape(&text2) != shape(&main_text) || d2.pool != RAM_BASE + ((l2.ram as u64) << 20) - drm - ((l2.swiotlb as u64) << 20) {
                bail!("--image-larger {path}: its .disks is not {list} with the pool and the GPU's arena moved to its own RAM's top: not the same build");
            }
            if cands.iter().any(|c| c.ram == l2.ram) {
                bail!("--image-larger: two Images of {} MiB", l2.ram);
            }
            let file = format!("kernel-{}.img", l2.ram);
            larger_files.push((path.clone(), file.clone()));
            cands.push(crate::take23::Candidate { ram: l2.ram, need: need_of(l2.ram), arena: arena_of(Some(&d2)), file, sha: format!("{:x}", Sha256::digest(&more)) });
        }
        cands.sort_by(|a, b| b.ram.cmp(&a.ram));
        cands.push(crate::take23::Candidate { ram: l.ram, need: need_of(l.ram), arena: arena_of(disks.as_ref()), file: "kernel.img".into(), sha: sha.clone() });
    } else if !t23.larger.is_empty() {
        bail!("--image-larger needs --fill-after-stop");
    }
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
    if t23.fill_after_stop {
        // take23: the rule is the script's, after One UI's stop (take23::pick_block): asked here it refused what fits then
        println!(
            "vm-boot: --fill-after-stop: Android has {available} MiB available and the pool {pool_mib} MiB free now, beside One UI; the Images ({}) are tried after its stop, the largest first, with {} MiB left to Android (run.txt's `pick:` lines)",
            cands.iter().map(|c| c.ram.to_string()).collect::<Vec<_>>().join(", "), t23.keep_mib
        );
    } else {
    crate::vmprobe::check_mem(l.ram, available + pool_mib.min(l.ram as u64))?;
    // A takeover tops the pool up itself once One UI is down (takeover_script); beside One UI it must hold the VM already
    match check_pool(l.ram as u64, pool_mib, l.shared) {
        Err(e) if o.takeover => println!("vm-boot: {e:#}\nvm-boot: the takeover tops the pool up while One UI is down, and stops if it cannot"),
        r => r?,
    }
    }
    if l.shared {
        let dev = adb.root_ok(&format!("[ -c {} ] && echo there || echo absent\n", crate::hostshare::DEV), "gunyah_share")?;
        if !dev.contains("there") {
            bail!("a shared-RAM Image and no {} on the phone: `phone hostshare load` first", crate::hostshare::DEV);
        }
        let c = crate::droidvm::push_with(adb, &o.droidvm, crate::droidvm::Swaps::from_words(&o.virgl, &o.crosvm))?;
        println!("vm-boot: a shared-RAM Image: Droid-VM's crosvm ({c}), --protected-vm-pseudo-unprotected");
    }
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR} && rm -f {DIR}/result {DIR}/run.txt {DIR}/serial.txt\n"), "the VM's folder")?;
    println!("vm-boot: {} MiB Image, sha256 {sha}: pushing", data.len() >> 20);
    adb.push(image, &format!("{DIR}/kernel.img"))?;
    if t23.fill_after_stop {
        adb.root_ok(&format!("rm -f {DIR}/kernel-[0-9]*.img\n"), "the last session's larger Images")?;
        for (path, file) in &larger_files {
            println!("vm-boot: --image-larger {path}: pushing as {file}");
            adb.push(path, &format!("{DIR}/{file}"))?;
        }
    }
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
        if o.vsync.is_some() {
            adb.push(&o.pacectl, &format!("{DIR}/pacectl"))?;
            adb.root_ok(&format!("cd {DIR} && chown root:root pacectl && chmod 755 pacectl\n"), "pacectl's owner")?;
            println!("vm-boot: the panel's vsync goes to the VM (pacectl; the kernel's refresh tick follows it only when the Image has oneios-vsync-lock=1)");
        }
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
    if disks.as_ref().is_some_and(|d| d.bridge) {
        // the VM's own copy, root's, as displayctl and wifictl
        adb.push(&o.bridgectl, &format!("{DIR}/bridgectl"))?;
        adb.root_ok(&format!("cd {DIR} && chown root:root bridgectl && chmod 755 bridgectl\n"), "bridgectl's owner")?;
        println!(
            "vm-boot: the bridge channel: the phone's battery to iOS{} (bridgectl serve on crosvm's second serial port; bridge.txt)",
            if o.haptics { " and iOS's haptics to the phone's motor" } else { ", no haptics (--no-haptics)" }
        );
    }
    match (disks.as_ref().and_then(|d| d.media), o.video_decoder) {
        (None, Some(_)) => bail!("--video-decoder needs an Image with the decoder's pool (xnuloader-crosvm --shared-ram --media)"),
        (Some(_), _) if !l.shared => bail!("the Image has the decoder's pool but is not a shared-RAM one: xnuloader-crosvm gives --media with --shared-ram"),
        (Some(m), Some(uid)) => println!(
            "vm-boot: the video decoder: crosvm's virtio-media decoder (MediaCodec in a helper under uid {uid}), its buffers in the \
             {} MiB media_host pool at 0x{:x}: iOS's VideoToolbox decodes H.264 and HEVC on the phone's hardware",
            m.mib, m.host
        ),
        (Some(m), None) => println!(
            "vm-boot: the Image has the video decoder's pool ({} MiB at 0x{:x}), given to crosvm for its layout; no device without \
             --video-decoder: iOS decodes in software, as before",
            m.mib, m.host
        ),
        (None, None) => {}
    }
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let script = Path::new(out).join("boot.sh");
    if o.pin && o.pin_first + l.cpus > 8 {
        bail!("--pin-first {} with {} vCPUs: past the S25's 8 cores", o.pin_first, l.cpus);
    }
    if o.swipe_test && !disks.as_ref().is_some_and(|d| d.input) {
        bail!("--swipe-test needs an Image with the touch (xnuloader-crosvm --display)");
    }
    check_boost(&l, o, disks.as_ref())?;
    let mut text = boot_script_with(&l, o.seconds, o.thp, &sha, disks.as_ref(), o.swipe_test, o.pin, &Tuning { gpu_floor_mhz: o.gpu_floor_mhz, cpu_floor_percent: o.cpu_floor_percent, gpu_idle_ms: o.gpu_idle_ms, touch_boost: o.touch_boost, touch_time: o.touch_time, vsync: o.vsync, takeover: o.takeover, gpu_cap_lift: o.gpu_cap_lift, max_volume: o.max_volume, pin_first: o.pin_first, thermal_log: o.thermal_log, perf: o.perf, audio_uid: o.audio_uid, mic_capture: o.mic_capture, crosvm_env: crosvm_env.clone(),
        takeover_reclaim: o.takeover_reclaim, takeover_swappiness: o.takeover_swappiness, takeover_wake: o.takeover_wake, video_decoder: o.video_decoder,
        panel_rate_min: o.panel_rate_min });
    // take23: the choice of the Image after One UI's stop, Android's memory log and the clocks' orchestrator (take23.rs),
    // each at a line of the script that must be there
    if t23.fill_after_stop {
        let main = cands.last().context("no main Image among the candidates")?;
        if let Some(d) = &disks {
            if text.contains(&format!("{:x}", d.pool)) {
                bail!("--fill-after-stop: the boot script names the pool's address (an Image without the GPU's scanout): not done for it");
            }
        }
        text = crate::take23::apply_pick(&text, main, &crate::take23::pick_block(&cands, t23.keep_mib))?;
    }
    if t23.fill_after_stop || orch.is_some() {
        let (start, after) = match &orch {
            Some(orch) => (format!("{}{}", crate::take23::HOSTMEM, crate::take23::orch_start(orch)), "orch_end\n"),
            None => (crate::take23::HOSTMEM.to_string(), ""),
        };
        text = crate::take23::around_wait(&text, o.seconds, &start, after)?;
        if orch.is_some() {
            // an end that is not the script's own (its trap): the clamp's file must not outlive the session
            // (and the loop must not write the cap after caps_restore put Samsung's back)
            let trap = "trap 'thp_restore; gpu_restore; cpu_restore; caps_restore;";
            if text.matches(trap).count() != 1 {
                bail!("take23: the boot script has not its one takeover trap");
            }
            text = text.replace(trap, "trap '[ -z \"$orchp\" ] || kill $orchp 2>/dev/null; rm -f /data/local/tmp/drm2kgsl-vcpu-uclamp; thp_restore; gpu_restore; cpu_restore; caps_restore;");
        }
    }
    fs::write(&script, text)?;
    adb.push(script.to_str().context("path")?, &format!("{DIR}/boot.sh"))?;
    // take23: the choice of the Image runs between One UI's stop and crosvm's start; the failsafe and this tool's own wait
    // for the end are that much longer
    let pick_extra = if t23.fill_after_stop { crate::take23::PICK_SECONDS * cands.len() as u32 } else { 0 };
    if o.takeover {
        // One UI's stop and start (ui.sh, sourced by boot.sh): its failsafe gives the session 5 minutes past its seconds
        let ui = Path::new(out).join("ui.sh");
        fs::write(&ui, ui_script(o.seconds + 300 + pick_extra))?;
        adb.push(ui.to_str().context("path")?, &format!("{DIR}/ui.sh"))?;
        adb.root_ok(&format!("chown root:root {DIR}/ui.sh && chmod 755 {DIR}/ui.sh\n"), "ui.sh's owner")?;
        println!(
            "vm-boot: takeover: One UI's framework stops while the VM runs and starts again when it ends (volume down 10 s, \
             --stop, its seconds, or any end of the script); its failsafe starts it after {} s whatever happens",
            o.seconds + 300 + pick_extra
        );
    }
    let r = adb.root(&crate::adb::detached_start(&format!("{DIR}/boot.sh")))?;
    if !r.out.contains("started") {
        bail!("the VM's script did not start (status {}): {}", r.status, r.err.trim());
    }
    if t23.fill_after_stop {
        println!("vm-boot: the Image is chosen on the phone after One UI's stop: {} MiB (the largest of the list that fits); the line below names --image's", cands.iter().map(|c| c.ram.to_string()).collect::<Vec<_>>().join(" or "));
    }
    if orch.is_some() {
        println!("vm-boot: the clocks' orchestrator is on (orch.txt): the GPU's cap by its load, the vCPUs' clamp by the guest's swaps on time");
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
        if t0.elapsed() > Duration::from_secs(o.seconds as u64 + 120 + pick_extra as u64) {
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
    for name in ["serial.txt", "crosvm.log", "dmesg.txt", "run.txt", "fdt.dtb", "display.txt", "touch.txt", "thermal.txt", "wifi.txt", "bridge.txt", "ui.log", "stop.txt", "perf.txt", "pace.txt", "hostmem.txt", "orch.txt"] {
        let path = Path::new(out).join(name);
        let mut child = adb.exec_out(&format!("su -c 'cat {DIR}/{name}'"))?;
        let mut file = fs::File::create(&path)?;
        copy(child.stdout.as_mut().context("no output from adb")?, &mut file)?;
        child.wait()?;
    }
    let run = fs::read_to_string(Path::new(out).join("run.txt")).unwrap_or_default();
    for line in run.lines().filter(|l| {
        l.starts_with("disk") || l.starts_with("store:") || l.starts_with("pool:") || l.starts_with("/data:") || l.starts_with("display:")
            || l.starts_with("touch:") || l.starts_with("bridge:") || l.starts_with("gpu:") || l.starts_with("boost:") || l.starts_with("stop:") || l.starts_with("perf:") || l.starts_with("vsync:")
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

    // take23: the whole chain on the script of a takeover with the GPU's scanout (take22's Images' .disks)
    #[test]
    fn take23s_additions_find_their_lines_in_a_takeover_script() {
        let text = "swiotlb-mib 32\npool 0x19d800000\nstore /data/local/tmp/oneios-vmboot/data.img 16\nfb 0 4492800 720 1560 2880 4505600\ninput touch\n\
            gpu 00:0c.0 8 0x19f800000\nscanout 720 1560 120\naudio\nwifi\nbridge\n\
            block 10359930880 path=/data/local/tmp/oneios/image/os.img,ro=true,id=os,pci-address=00:02.0\n\
            block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false,id=data,pci-address=00:06.0\n";
        let d = parse_disks(text).unwrap();
        let l = Label { ram: 4608, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let script = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true,
            &Tuning { takeover: true, gpu_cap_lift: true, touch_boost: true, thermal_log: true, pin_first: 2, ..Tuning::DEFAULT });
        assert!(!script.contains("19d800000"), "the pool's address is in the script");
        let cand = |ram: u32, arena: &str, file: &str| crate::take23::Candidate { ram, need: (u64::from(ram) - 32 + 1) / 2, arena: arena.into(), file: file.into(), sha: "cd".repeat(32) };
        let main = cand(4608, "19f800000", "kernel.img");
        let cands = [cand(7168, "23f800000", "kernel-7168.img"), cand(6144, "1ff800000", "kernel-6144.img"), main.clone()];
        let picked = crate::take23::apply_pick(&script, &main, &crate::take23::pick_block(&cands, 1024)).unwrap();
        // the choice comes after One UI's stop and the cache drop, before the top-up, and nothing names 4608 MiB's values after it
        let at = |s: &str| picked.find(s).unwrap_or_else(|| panic!("{s}"));
        assert!(at("takeover: One UI is down while the VM runs") < at("# take23 (vm-boot --fill-after-stop"));
        assert!(at("# take23 (vm-boot --fill-after-stop") < at("need=$NEED\n") && at("need=$NEED\n") < at("topped up while One UI is down"));
        assert!(picked.contains("--mem size=$RAM ") && picked.contains(" $KIMG </dev/null >$D/crosvm.log") && picked.contains("drm2kgsl_host@$ARENA"));
        assert!(!picked.contains("--mem size=4608") && !picked.contains("need=2288") && picked.matches("19f800000").count() == 1);
        assert!(picked.contains("\n4608 2288 19f800000 kernel.img "));
        let orch = crate::take23::Orch::DEFAULT;
        let whole = crate::take23::around_wait(&picked, 3600, &format!("{}{}", crate::take23::HOSTMEM, crate::take23::orch_start(&orch)), "orch_end\n").unwrap();
        let at = |s: &str| whole.find(s).unwrap_or_else(|| panic!("{s}"));
        assert!(at("pid=$!\n") < at(") > $D/hostmem.txt 2>&1 &") && at(") > $D/hostmem.txt 2>&1 &") < at("orch_end() {"));
        assert!(at("orchp=$!\n") < at("how=itself\n") && at("wait $pid; rc=$?\norch_end\n") < at("\ncaps_restore\n"));
        assert_eq!(whole.matches("trap 'thp_restore; gpu_restore; cpu_restore; caps_restore;").count(), 1);
        // without the takeover there is no `need=` line: refused, the script not half changed
        let plain = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        assert!(crate::take23::apply_pick(&plain, &main, "X").is_err());
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
        assert!(s.contains("caps_restore; wifi_restore; start_ui \"the session ended\"; volume_restore' EXIT HUP INT TERM"));
        assert!(s.contains("wifi_restore() { :; }") && s.find("wifi_restore() { :; }").unwrap() < start);
        // without a takeover One UI keeps wlan0
        let plain = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        assert!(!plain.contains("wifictl") && !plain.contains("--net "));
        assert!(parse_disks(&format!("{SCREEN}wifi on\n")).is_err());
    }

    #[test]
    fn an_image_with_the_bridge_gets_bridgectl_serve_on_crosvms_second_serial_port() {
        let d = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x15f800000\nscanout 720 1560 120\naudio\nwifi\nbridge\n")).unwrap();
        assert!(d.bridge && d.bridge_haptics);
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, ..Tuning::DEFAULT });
        // it listens before crosvm starts, which is given the port only when the socket is there
        let start = s.find("$D/bridgectl serve $D/bridge.sock --wait 120 </dev/null >>$D/bridge.txt 2>&1 &").unwrap();
        assert!(start < s.find("$C --log-level info run").unwrap());
        assert!(s.contains("[ ! -S $D/bridge.sock ] && kill -0 $bridged"));
        assert!(s.contains(
            "--serial type=file,hardware=serial,num=1,path=$D/serial.txt,console${bsock:+ --serial type=unix-stream,hardware=serial,num=2,path=$bsock,input-unix-stream} \\\n"
        ));
        // an old socket and log do not outlive the start; the tool is ended after crosvm and its last line shown
        assert!(s.contains("boost.state.new bridge.txt bridge.sock\n"));
        let end = s.find("kill $bridged 2>/dev/null; wait $bridged 2>/dev/null").unwrap();
        assert!(end > s.find("wait $pid; rc=$?").unwrap() && end < s.find("echo \"end $(date +%s)").unwrap());
        // the battery alone
        let mut quiet = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x15f800000\nscanout 720 1560 120\nbridge\n")).unwrap();
        quiet.bridge_haptics = false;
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&quiet), false, true, &Tuning::DEFAULT);
        assert!(s.contains("$D/bridgectl serve $D/bridge.sock --wait 120 --no-haptics </dev/null"));
        // an Image without it: no tool, no second port
        let none = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&none), false, true, &Tuning::DEFAULT);
        assert!(!none.bridge && !s.contains("bridgectl") && !s.contains("num=2") && !s.contains("bsock"));
        assert!(parse_disks(&format!("{SCREEN}bridge 1\n")).is_err());
        // and it parses as POSIX sh
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, ..Tuning::DEFAULT });
        let path = std::env::temp_dir().join(format!("oneios-vmboot-bridge-{}.sh", std::process::id()));
        fs::write(&path, &s).unwrap();
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

    #[test]
    fn an_image_with_the_sound_card_gets_crosvms_aaudio_card() {
        let d = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x15f800000\nscanout 720 1560 120\naudio\n")).unwrap();
        assert!(d.audio);
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, ..Tuning::DEFAULT });
        assert!(s.contains(" \\\n  --virtio-snd backend=aaudio,capture=false \\\n  --block "), "in crosvm's process, before the disks");
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { audio_uid: Some(2000), ..Tuning::DEFAULT });
        assert!(s.contains(" \\\n  --virtio-snd backend=aaudio,capture=false,uid=2000 \\\n"));
        // --mic-capture: the card with capture=true, and nothing else of the line changes
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, mic_capture: true, ..Tuning::DEFAULT });
        assert!(s.contains(" \\\n  --virtio-snd backend=aaudio,capture=true \\\n  --block ") && !s.contains("capture=false"));
        assert!(parse_disks(&format!("{SCREEN}audio 1\n")).is_err());
    }

    #[test]
    fn an_image_with_the_decoders_pool_always_gets_it_and_the_device_only_when_asked() {
        // take18b: xnuloader-crosvm --media's line; the pool rides on the GPU's --pre-alloc (one option: crosvm's layout puts
        // drm2kgsl_host first, then media_host) and is checked in crosvm's tree; the device only with --video-decoder
        let d = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x153800000\nscanout 720 1560 120\nmedia 192 0x154000000\n")).unwrap();
        assert_eq!(d.media, Some(Media { mib: 192, host: 0x1_5400_0000 }));
        let l = Label { ram: 3584, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let off = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        assert!(off.contains(" \\\n  --pre-alloc drm-host-mb=8,media-host-mb=192 \\\n"), "one --pre-alloc for both pools");
        assert!(!off.contains("--virtio-media"), "no device without --video-decoder");
        assert!(off.contains("grep -q -F media_host@154000000 $D/fdt.dtb; then echo \"video: crosvm's media_host pool is at 0x154000000"));
        assert!(off.contains("grep -q -F drm2kgsl_host@153800000 $D/fdt.dtb"), "the GPU's arena where the Image moved it");
        let on = boot_script_with(&l, 3600, true, "ab12", Some(&d), false, true, &Tuning { video_decoder: Some(2000), ..Tuning::DEFAULT });
        assert!(on.contains(" \\\n  --virtio-media kind=decoder,uid=2000 \\\n"));
        assert_eq!(on.matches("--pre-alloc").count(), 1);
        // without the GPU the pool has a --pre-alloc of its own
        let alone = parse_disks(&format!("{SCREEN}media 64 0x15c000000\n")).unwrap();
        let s = boot_script_with(&Label { gpu: false, ..l }, 3600, true, "ab12", Some(&alone), false, true, &Tuning { video_decoder: Some(10123), ..Tuning::DEFAULT });
        assert!(s.contains(" \\\n  --pre-alloc media-host-mb=64 \\\n  --virtio-media kind=decoder,uid=10123 \\\n"));
        // an Image without the line: neither, as before
        let plain = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x15f800000\nscanout 720 1560 120\n")).unwrap();
        let s = boot_script_with(&l, 3600, true, "ab12", Some(&plain), false, true, &Tuning::DEFAULT);
        assert!(!s.contains("media-host-mb") && !s.contains("media_host@") && s.contains(" \\\n  --pre-alloc drm-host-mb=8 \\\n"));
        for bad in ["media 15 0x15c000000", "media 33 0x15c000000", "media 2048 0x15c000000", "media 64 0x15c100000", "media 64 15c000000", "media 64"] {
            assert!(parse_disks(&format!("{SCREEN}{bad}\n")).is_err(), "{bad}");
        }
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
        assert!(at("trap 'thp_restore; gpu_restore; cpu_restore; caps_restore; wifi_restore; start_ui \"the session ended\"; volume_restore' EXIT HUP INT TERM")
            > at("setprop ctl.stop zygote"));
        // Samsung's cpufreq_limit lifted with One UI down, before the caps and the floors; put back with the caps
        assert!(at("setprop ctl.stop zygote") < at("echo -1 > $SL"));
        assert!(at("echo -1 > $SL") < at("echo $top > $f/scaling_max_freq"));
        assert!(s.contains("echo \"$sl_old\" > $SL") && !s.contains("ctl.restart perf2-hal"));
        // the caps lifted, then the GPU's floor, then crosvm, then the CPUs' (which only reads at first)
        assert!(at("echo $top > $f/scaling_max_freq") < at("echo $floor > $G/min_freq"));
        // the pool topped up with One UI down when it holds less than the VM's 1776 pages, else left alone
        assert!(s.contains("need=1776\n") && at("echo 3 > /proc/sys/vm/drop_caches") < at("echo 3 > $P/acquire"));
        assert!(s.contains("{ end \"refused pool $avail of $need pages after a top-up\"; exit 1; }"));
        assert!(at("echo $floor > $G/min_freq") < at("$C --log-level info run"));
        assert!(at("$C --log-level info run") < at("echo $floor > $f/scaling_min_freq"));
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
        assert!(logged.contains(") > $D/thermal.txt 2>&1 &\n") && !s.contains(") > $D/thermal.txt"));
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

    /// Everything opted in: the boost, the GPU's idle_timer, the thermal log, on the scanout's Image.
    fn boosted(takeover: bool) -> String {
        let d = parse_disks(&format!("{SCREEN}input touch\n{GPU}scanout 720 1560 120\nwifi\n")).unwrap();
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        boot_script_with(&l, 900, true, "ab12", Some(&d), true, true, &Tuning { takeover, touch_boost: true, gpu_idle_ms: 400, thermal_log: true, ..Tuning::DEFAULT })
    }

    /// `body` run by the PC's sh in a folder of its own: its output.
    fn sh(dir: &Path, body: &str) -> String {
        let out = std::process::Command::new("sh").arg("-c").arg(format!("D={}\n{body}", dir.display())).output().unwrap();
        format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr))
    }

    fn folder(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("oneios-vmboot-{name}-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn the_touch_boost_is_opt_in_and_every_end_puts_the_clocks_back() {
        for takeover in [true, false] {
            let s = boosted(takeover);
            let at = |what: &str| s.find(what).unwrap_or_else(|| panic!("no {what:?}"));
            assert!(s.contains("${boostv:+ --boost $boostv --boost-state $D/boost.state}"));
            assert!(at("boostv=\n") < at("$D/displayctl touch --evdev"));
            assert!(s.contains("devfreq/available_frequencies 607000000\n") && s.contains("cpuinfo_max_freq) * 40 / 100"));
            // no minimum is read as the original by the script, and every trap has the boost's end
            assert!(!s.contains("$(cat $f/scaling_min_freq)") && !s.contains("gpu_min=$(cat"));
            let traps: Vec<&str> = s.lines().filter(|l| l.trim_start().starts_with("trap ") && l.contains("thp_restore")).collect();
            assert_eq!(traps.len(), if takeover { 5 } else { 3 });
            // the end's, all of them; a signal's only what it was without the boost: the VM may run on and needs its touch
            assert!(traps.iter().all(|t| t.contains("boost_restore") == t.ends_with(" EXIT") && t.contains("boost_restore") != t.ends_with(" HUP INT TERM")), "{traps:?}");
            // after crosvm: the forwarder ended before anything else's clocks come back
            assert!(at("wait $pid; rc=$?") < s.rfind("thp_restore\nboost_restore\n").unwrap());
            // the idle_timer alone: no floor
            assert!(s.contains("echo 400 > $K/idle_timer") && !s.contains("echo $floor"));
            assert_eq!(s.matches("gpu_restore\n").count(), 1);
        }
        // a floor alone: no idle_timer write; neither: nothing but a killed forwarder's leftover
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let (floor, _) = gpu_floor_script(&l, 607, 0);
        assert!(floor.contains("echo $floor > $G/min_freq") && !floor.contains("> $K/idle_timer && echo \"gpu: it stays"));
        assert_eq!(gpu_floor_script(&l, 0, 0), (String::new(), String::new()));
        let d = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        let plain = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false }, 900, true, "ab12", Some(&d));
        assert!(!plain.contains("--boost") && !plain.contains("boost_restore") && plain.contains("\nboost_leftover\n"));
        // before any check can refuse the start
        assert!(plain.find("\nboost_leftover\n").unwrap() < plain.find("sha256sum").unwrap());
    }

    #[test]
    fn a_killed_forwarders_clocks_come_back_from_its_file() {
        let dir = folder("boost");
        let clock = dir.join("sys/policy0/scaling_min_freq");
        fs::create_dir_all(clock.parent().unwrap()).unwrap();
        // the file's paths are the phone's /sys: here under the folder
        let script = BOOST_LEFTOVER.replace("/sys/*min_freq)", &format!("{}/sys/*min_freq)", dir.display()));
        let id = fs::read_to_string("/proc/sys/kernel/random/boot_id").unwrap();
        let other = dir.join("other");
        let state = |boot: &str| format!("boot {boot}\n{c} 384000\n{o} 5\n{c} x\n{d}/sys/../other_min_freq 7\n", c = clock.display(), o = other.display(), d = dir.display());
        fs::write(&clock, "1497600\n").unwrap();
        fs::write(dir.join("boost.state"), state(id.trim())).unwrap();
        let out = sh(&dir, &script);
        assert!(out.contains("it is 384000 again"), "{out}");
        assert_eq!(fs::read_to_string(&clock).unwrap().trim(), "384000");
        assert!(!other.exists() && !dir.join("other_min_freq").exists() && !dir.join("boost.state").exists());
        // a refused write (the clock is not there): the file is kept, and the next try has it
        fs::write(&clock, "1497600\n").unwrap();
        fs::write(dir.join("boost.state"), state(id.trim())).unwrap();
        fs::rename(&clock, dir.join("away")).unwrap();
        fs::create_dir(&clock).unwrap();
        assert!(sh(&dir, &script).contains("could not be put back at 384000") && dir.join("boost.state").exists());
        fs::remove_dir(&clock).unwrap();
        fs::rename(dir.join("away"), &clock).unwrap();
        sh(&dir, &script);
        assert_eq!(fs::read_to_string(&clock).unwrap().trim(), "384000");
        assert!(!dir.join("boost.state").exists());
        // another boot's: only removed
        fs::write(&clock, "1497600\n").unwrap();
        fs::write(dir.join("boost.state"), state("0")).unwrap();
        sh(&dir, &script);
        assert_eq!(fs::read_to_string(&clock).unwrap().trim(), "1497600");
        assert!(!dir.join("boost.state").exists());
        // boost_restore: a forwarder that takes SIGTERM for nothing is killed, its file used; then the script goes on
        fs::write(dir.join("boost.state"), state(id.trim())).unwrap();
        let s = boosted(false);
        let from = s.find("boost_restore() {").unwrap();
        let f = &s[from..from + s[from..].find("\n}\n").unwrap() + 3];
        let fns = script.replace("\nboost_leftover\n", "\n");
        let out = sh(&dir, &format!("{fns}pid=999999999\nsh -c 'trap : TERM; while :; do sleep 0.1; done' &\nforward=$!; was=$forward\n{f}boost_restore\nkill -0 $was 2>/dev/null || echo \"dead <$forward>\"\n"));
        // and its number forgotten: a second pass signals nobody
        assert!(out.contains("it is 384000 again") && out.contains("dead <>"), "{out}");
        // crosvm still there: the forwarder is left alone
        let out = sh(&dir, &format!("{fns}sleep 5 & pid=$!\nsleep 5 & forward=$!\n{f}boost_restore\nkill -0 $forward && echo kept\nkill $pid $forward"));
        assert!(out.contains("kept"), "{out}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_cpu_floor_puts_back_the_lowest_it_saw() {
        let script = |dir: &Path| {
            let cf = format!("{}/sys/cpufreq", dir.display());
            let left = BOOST_LEFTOVER.replace("/sys/*min_freq)", &format!("{}/sys/*min_freq)", dir.display())).replace("\nboost_leftover\n", "\n");
            let s = cpu_floor_script(60).0.replace("/sys/devices/system/cpu/cpufreq", &cf).replace(&format!("-lt {FLOOR_SETTLE_S} ]"), "-lt 3 ]");
            format!("{left}F={cf}/policy0/scaling_min_freq; G={cf}/policy6/scaling_min_freq\n{s}")
        };
        // how it ends: cpu_restore; the floor's process killed first; crosvm gone; boot.sh killed; ended while it only reads
        for (n, end) in ["cpu_restore", "kill -9 $floorp; cpu_restore", "kill $pid; wait $pid 2>/dev/null; sleep 3", "kill -9 $$", ""].iter().enumerate() {
            let dir = folder(&format!("floor{n}"));
            let min = dir.join("sys/cpufreq/policy0/scaling_min_freq");
            for (p, low) in [("policy0", "2227200"), ("policy6", "2900000")] {
                let d = dir.join("sys/cpufreq").join(p);
                fs::create_dir_all(&d).unwrap();
                fs::write(d.join("scaling_min_freq"), format!("{low}\n")).unwrap();
                fs::write(d.join("scaling_max_freq"), "3532800\n").unwrap();
                fs::write(d.join("scaling_available_frequencies"), "384000 1152000 2246400 3532800\n").unwrap();
            }
            let s = script(&dir);
            let out = if end.is_empty() {
                sh(&dir, &format!("sleep 20 & pid=$!\n{s}sleep 1\ncpu_restore\nsleep 3; cat $F; ls $D; kill $pid"))
            } else {
                // Samsung's minimum goes away while it only reads
                sh(&dir, &format!("sleep 12 </dev/null >/dev/null 2>&1 & pid=$!\n{s}sleep 1.5; cat $F; echo 384000 > $F\nsleep 4; cat $F $G; cat $D/boost.state\n{end}\n"))
            };
            if end.is_empty() {
                assert!(out.contains("\n2227200\n") && !out.contains("boost.state"), "{out}");
            } else {
                assert!(out.contains("2227200\n"), "{n}: {out}");
                assert!(out.contains("\n2246400\n2900000\nboot "), "raised, and one above the floor left alone; {n}: {out}");
                assert!(out.contains("policy0/scaling_min_freq 384000\n") && !out.contains("policy6/scaling_min_freq "), "{n}: {out}");
                let t = Instant::now();
                while dir.join("boost.state").exists() && t.elapsed() < Duration::from_secs(5) {
                    std::thread::sleep(Duration::from_millis(100));
                }
                assert_eq!(fs::read_to_string(&min).unwrap().trim(), "384000", "{n}: {out}");
                assert!(!dir.join("boost.state").exists());
            }
            fs::remove_dir_all(&dir).ok();
        }
        // an earlier session's clock that cannot be put back: no floor, and its file stays
        let dir = folder("floorleft");
        fs::create_dir_all(dir.join("sys/cpufreq/policy0")).unwrap();
        fs::create_dir_all(dir.join("sys/gone_min_freq")).unwrap();
        for (f, v) in [("min_freq", "384000"), ("max_freq", "3532800"), ("available_frequencies", "384000 2246400")] {
            fs::write(dir.join(format!("sys/cpufreq/policy0/scaling_{f}")), format!("{v}\n")).unwrap();
        }
        let id = fs::read_to_string("/proc/sys/kernel/random/boot_id").unwrap();
        fs::write(dir.join("boost.state"), format!("boot {}\n{}/sys/gone_min_freq 5\n", id.trim(), dir.display())).unwrap();
        let out = sh(&dir, &format!("sleep 20 & pid=$!\n{}sleep 5; cat $F; cpu_restore; kill $pid", script(&dir)));
        assert!(out.contains("no floor") && out.contains("\n384000\n") && dir.join("boost.state").exists(), "{out}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_last_sessions_ui_log_is_kept_once_more() {
        let dir = folder("uilog");
        let s = boosted(true);
        let line = s.lines().find(|l| l.starts_with("for f in ui.log ")).unwrap();
        assert!(s.find(line).unwrap() < s.find("rm -f serial.txt").unwrap());
        fs::write(dir.join("ui.log"), "failsafe\n").unwrap();
        for _ in 0..2 {
            // a second start without one keeps it
            sh(&dir, &format!("cd $D\n{line}\nrm -f ui.log stop.txt"));
            assert_eq!(fs::read_to_string(dir.join("ui.log.prev")).unwrap(), "failsafe\n");
        }
        assert!(!dir.join("stop.txt.prev").exists());
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_boosts_clocks_are_the_tables() {
        let dir = folder("boostv");
        fs::write(dir.join("min"), "1\n").unwrap();
        fs::write(dir.join("table"), "160000000 222000000 525000000 607000000 1200000000\n").unwrap();
        let s = boosted(false);
        let from = s.find("boostv=\n").unwrap();
        let add = &s[from..from + s[from..].find("\n}\n").unwrap() + 3];
        let out = sh(&dir, &format!("{add}boost_add $D/min $D/table 607000000\nboost_add $D/min $D/table 600000001\nboost_add $D/min $D/table 1300000000\nboost_add $D/none $D/table 1\necho $boostv"));
        let want = format!("{d}/min=607000000,{d}/min=607000000", d = dir.display());
        assert_eq!(out.lines().last(), Some(want.as_str()), "{out}");
        assert!(out.contains("no clock of 1300000000") && out.contains("boost: no "));
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_thermal_log_has_the_cooling_usb_battery_and_cpus() {
        let dir = folder("thermal");
        for (f, v) in [("sys/class/thermal/cooling_device0/type", "cpufreq-cpu0"), ("sys/class/thermal/cooling_device0/cur_state", "0"),
                       ("sys/class/thermal/cooling_device0/max_state", "20"), ("sys/class/thermal/cooling_device12/type", "gpu"),
                       ("sys/class/thermal/cooling_device12/cur_state", "3"), ("sys/class/thermal/cooling_device12/max_state", "14"),
                       ("sys/class/power_supply/usb/online", "1"), ("sys/class/power_supply/usb/current_now", "500000"),
                       ("sys/class/power_supply/battery/current_now", "-797000"), ("sys/class/power_supply/battery/voltage_now", "4100000"),
                       ("sys/devices/system/cpu/online", "0-7")] {
            let p = dir.join(f);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, format!("{v}\n")).unwrap();
        }
        let script = thermal_log_script(true).replace("/sys/", &format!("{}/sys/", dir.display()));
        for (state, want) in [("3", " cool 12=3"), ("0", " cool -")] {
            fs::write(dir.join("sys/class/thermal/cooling_device12/cur_state"), format!("{state}\n")).unwrap();
            sh(&dir, &format!("sleep 0.2 & pid=$!\n{script}wait"));
            let log = fs::read_to_string(dir.join("thermal.txt")).unwrap();
            assert!(log.contains("# cool 0 cpufreq-cpu0 max 20\n") && log.contains("# cool 12 gpu max 14\n"), "{log}");
            let line = log.lines().find(|l| l.contains(" g ")).unwrap_or_else(|| panic!("{log}"));
            assert!(line.contains(" usb 1 500000 - batt -797000 4100000 online 0-7 lmh - - hw - -") && line.ends_with(want), "{line}");
        }
        assert!(boosted(true).contains(" wifi.mac thermal.txt ui.log stop.txt "), "an older session's files are not this one's");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_thermal_log_has_the_gpus_caps() {
        let dir = folder("thermal-caps");
        for (f, v) in [("sys/class/kgsl/kgsl-3d0/max_gpuclk", "500000000"), ("sys/class/kgsl/kgsl-3d0/thermal_pwrlevel", "9"), ("sys/kernel/gpu/gpu_max_clock", "500")] {
            let p = dir.join(f);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, format!("{v}\n")).unwrap();
        }
        let script = thermal_log_script(true).replace("/sys/", &format!("{}/sys/", dir.display()));
        sh(&dir, &format!("sleep 0.2 & pid=$!\n{script}wait"));
        let log = fs::read_to_string(dir.join("thermal.txt")).unwrap();
        assert!(log.contains("gpu-busy cap max_gpuclk (Hz) pwr thermal_pwrlevel gmax gpu_max_clock (MHz"), "{log}");
        let line = log.lines().find(|l| l.contains(" g ")).unwrap_or_else(|| panic!("{log}"));
        assert!(line.contains(" cap 500000000 pwr 9 gmax 500 usb "), "{line}");
        // take19: the LMh ceiling and the hardware's own clock of each cluster, `-` where the file is not
        for (f, v) in [("sys/devices/system/cpu/cpufreq/policy0/dcvsh_freq_limit", "2745600"),
                       ("sys/devices/system/cpu/cpufreq/policy6/cpuinfo_cur_freq", "1958400")] {
            let p = dir.join(f);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, format!("{v}\n")).unwrap();
        }
        sh(&dir, &format!("sleep 0.2 & pid=$!\n{script}wait"));
        let log = fs::read_to_string(dir.join("thermal.txt")).unwrap();
        assert!(log.lines().any(|l| l.contains(" lmh 2745600 - hw - 1958400 cool ")), "{log}");
        // Samsung's gpu_max_clock is not on every phone: `-`, and the others are still read
        fs::remove_file(dir.join("sys/kernel/gpu/gpu_max_clock")).unwrap();
        sh(&dir, &format!("sleep 0.2 & pid=$!\n{script}wait"));
        let log = fs::read_to_string(dir.join("thermal.txt")).unwrap();
        assert!(log.lines().any(|l| l.contains(" cap 500000000 pwr 9 gmax - usb ")), "{log}");
        fs::remove_dir_all(&dir).ok();
    }

    /// The GPU cap lift and its restore, run by the PC's sh against a fake sysfs.
    fn cap_run(dir: &Path) -> String {
        let body = format!("{GPU_CAP_LIFT}caps_restore() {{\n  {GPU_CAP_RESTORE}:\n}}\necho ---; caps_restore\n").replace("/sys/", &format!("{}/sys/", dir.display()));
        sh(dir, &body)
    }

    fn cap_files(dir: &Path, gm: &str, gk: &str) {
        for (f, v) in [("sys/kernel/gpu/gpu_max_clock", gm), ("sys/class/kgsl/kgsl-3d0/max_gpuclk", gk), ("sys/kernel/gpu/gpu_freq_table", "1200 1000 900 500 160"),
                       ("sys/class/kgsl/kgsl-3d0/gpu_available_frequencies", "1200000000 1000000000 500000000 160000000"), ("sys/class/kgsl/kgsl-3d0/thermal_pwrlevel", "9")] {
            let p = dir.join(f);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, format!("{v}\n")).unwrap();
        }
    }

    #[test]
    fn the_gpu_cap_lift_saves_lifts_and_puts_back_samsungs_cap() {
        let dir = folder("gpucap");
        cap_files(&dir, "500", "500000000");
        let rd = |f: &str| fs::read_to_string(dir.join(f)).unwrap().trim().to_string();
        let out = cap_run(&dir);
        let (lift, back) = out.split_once("---\n").unwrap();
        assert!(lift.contains("cap ") && lift.contains("sys/kernel/gpu/gpu_max_clock 500 lifted to 1200") && lift.contains("max_gpuclk 500000000 lifted to 1200000000"), "{out}");
        assert!(back.contains("gpu_max_clock is 500 again") && back.contains("max_gpuclk is 500000000 again"), "{out}");
        // the end wrote the saved values: the files read what One UI had
        assert_eq!((rd("sys/kernel/gpu/gpu_max_clock"), rd("sys/class/kgsl/kgsl-3d0/max_gpuclk")), ("500".to_string(), "500000000".to_string()), "{out}");
        // at the top already: nothing written and nothing put back
        cap_files(&dir, "1200", "1200000000");
        let out = cap_run(&dir);
        assert!(out.contains("is 1200, at the top already") && !out.contains("lifted") && !out.contains("again"), "{out}");
        assert_eq!(rd("sys/kernel/gpu/gpu_max_clock"), "1200");
        // a node that is not there, or a table that is not numbers: logged, left alone
        fs::remove_file(dir.join("sys/kernel/gpu/gpu_max_clock")).unwrap();
        fs::write(dir.join("sys/class/kgsl/kgsl-3d0/gpu_available_frequencies"), "none\n").unwrap();
        fs::write(dir.join("sys/class/kgsl/kgsl-3d0/max_gpuclk"), "500000000\n").unwrap();
        let out = cap_run(&dir);
        assert!(out.contains("no ") && out.contains("its cap is not Samsung's to lift") && out.contains("left alone") && !out.contains("lifted"), "{out}");
        assert_eq!(rd("sys/class/kgsl/kgsl-3d0/max_gpuclk"), "500000000");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_gpu_cap_lift_is_the_takeovers_default_and_kept_out_of_other_sessions() {
        // take18: lifted with --takeover unless --keep-gpu-cap; --gpu-cap-lift alone still asks (and check_boost refuses it)
        assert!(gpu_cap_lift(true, false, false) && !gpu_cap_lift(true, false, true) && gpu_cap_lift(true, true, true));
        assert!(!gpu_cap_lift(false, false, false) && !gpu_cap_lift(false, false, true) && gpu_cap_lift(false, true, false));
        let d = parse_disks(&format!("{SCREEN}{GPU}scanout 720 1560 120\n")).unwrap();
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let script = |t: Tuning| boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &t);
        assert!(!script(Tuning { takeover: true, ..Tuning::DEFAULT }).contains("gpu_max_clock"));
        assert!(!script(Tuning { gpu_cap_lift: true, ..Tuning::DEFAULT }).contains("gpu_max_clock"), "not without the takeover");
        let s = script(Tuning { takeover: true, gpu_cap_lift: true, ..Tuning::DEFAULT });
        let at = |p: &str| s.find(p).unwrap_or_else(|| panic!("no {p}"));
        // after the cpufreq_limit lift, before the caps' function that puts it back
        assert!(at("sl_old=$(cat $SL)") < at("gm_old=") && at("gm_old=") < at("caps_restore() {\n"));
        assert!(at("caps_restore() {\n") < at("echo \"$gk_old\" > $GK") && at("echo \"$gk_old\" > $GK") < at("[ -n \"$caps\" ] || return 0"));
        let o = Options { seconds: 900, thp: true, bridge: String::new(), swipe_test: false, pin: false, droidvm: String::new(), gpu_floor_mhz: 0, cpu_floor_percent: 0, gpu_idle_ms: 0,
                          touch_boost: false, touch_time: false, vsync: None, pacectl: String::new(), perf: crate::vmperf::Perf { log: false, unpin: false },
                          takeover: false, gpu_cap_lift: true, max_volume: true, pin_first: 0, thermal_log: false, audio_uid: None, mic_capture: false, video_decoder: None, wifictl: String::new(),
                          bridgectl: String::new(), haptics: true, virgl: String::new(), crosvm: String::new(), gpu_host_floors: Floors::Deadline, crosvm_env: Vec::new(),
                          takeover_reclaim: true, takeover_swappiness: None, takeover_wake: true, panel_rate_min: 0, take23: Default::default() };
        assert!(check_boost(&l, &o, Some(&d)).unwrap_err().to_string().contains("--gpu-cap-lift needs --takeover"));
        assert!(check_boost(&l, &Options { takeover: true, ..o }, Some(&d)).is_ok());
    }

    #[test]
    fn the_media_volume_is_set_to_the_maximum_before_one_ui_stops() {
        let d = parse_disks(SCREEN).unwrap();
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: false };
        let s = boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, ..Tuning::DEFAULT });
        assert!(s.find("--stream 3 --set $m").unwrap() < s.find(". $D/ui.sh").unwrap(), "One UI is up while it is set");
        // restored after One UI is back: in the trap, after start_ui, and defined (after the empty one) before One UI stops
        assert!(s.contains("start_ui \"the session ended\"; volume_restore' EXIT HUP INT TERM"));
        let (empty, real) = (s.find("volume_restore() { :; }").unwrap(), s.find("volume_restore() {\n").unwrap());
        assert!(empty < real && real < s.find(". $D/ui.sh").unwrap());
        for off in [Tuning { takeover: true, max_volume: false, ..Tuning::DEFAULT }, Tuning::DEFAULT] {
            let s = boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &off);
            assert!(!s.contains("--stream 3") && !s.contains("vold") && !s.contains("volume_restore() {\n"));
        }
        // with the touch boost's own trap the restore is still after start_ui
        let boosted = boot_script_with(&l, 900, true, "ab12", Some(&parse_disks(&format!("{SCREEN}input touch\n")).unwrap()), false, true, &Tuning { takeover: true, touch_boost: true, ..Tuning::DEFAULT });
        assert!(boosted.contains("start_ui \"the session ended\"; volume_restore'"), "{boosted}");
        // against fake `cmd` and `media`: the range is read, the maximum written and the new value logged
        let dir = folder("volume");
        let fake = |first: &str| {
            format!(
                "timeout() {{ shift; \"$@\"; }}\nv=7\n\
                 cmd() {{ {first}; }}\n\
                 media() {{ case \"$*\" in *--set*) v=${{4:-$v}}; v=$(echo \"$*\" | sed 's/.*--set //') ;; *) echo \"volume is $v in range [0..25]\" ;; esac; }}\n"
            )
        };
        // cmd media_session works: "volume is 7 in range [0..15]", then 15
        let ok = fake("case \"$*\" in *--set*) v=$(echo \"$*\" | sed 's/.*--set //') ;; *) echo \"volume is $v in range [0..15]\" ;; esac");
        let out = sh(&dir, &format!("{ok}{MEDIA_VOLUME}"));
        assert!(out.contains("audio: the media volume was: volume is 7 in range [0..15] ; set to 15 with 'cmd media_session volume': now: volume is 15 in range [0..15]"), "{out}");
        // cmd gives nothing usable: the old `media volume` is the second try
        let second = fake("echo 'Error: unknown command'");
        let out = sh(&dir, &format!("{second}{MEDIA_VOLUME}"));
        assert!(out.contains("set to 25 with 'media volume': now: volume is 25 in range [0..25]"), "{out}");
        // put back after One UI is up: the value it had (7), with the tool that read it, and the line says what it reads then
        let out = sh(&dir, &format!("{ok}{MEDIA_VOLUME}volume_restore\n"));
        assert!(out.contains("audio: 7 will be put back after One UI is up\n") && out.ends_with("audio: the media volume is put back to 7: now: volume is 7 in range [0..15] \n"), "{out}");
        let out = sh(&dir, &format!("{second}{MEDIA_VOLUME}volume_restore\nvolume_restore\n"));
        assert!(out.contains("put back to 7: now: volume is 7 in range [0..25]") && out.matches("put back to").count() == 1, "{out}");   // once: it forgets
        // the framework is not up when it is asked, and stays down: it is waited for (60 s, the sleeps skipped here), nothing written
        let gone = format!(
            "sleep() {{ :; }}\ntimeout() {{ shift; \"$@\"; }}\nv=7\nup=1\ncmd() {{ [ $up = 1 ] || {{ echo 'Can not find service: media_session'; return 0; }}; case \"$*\" in *--set*) v=$(echo \"$*\" | sed 's/.*--set //') ;; *) echo \"volume is $v in range [0..15]\" ;; esac; }}\n{MEDIA_VOLUME}up=0\nvolume_restore\nup=1\nvolume_restore\n");
        let out = sh(&dir, &gone);
        assert!(out.contains("audio: the media volume service did not answer in 60 s: 7 is not put back\n") && !out.contains("put back to"), "{out}");
        // it answers on the fifth look (after four 2 s waits): the value is written then. The count is in a file: the reads run in
        // command substitutions, whose variables do not come back.
        let slow = "sleep() { :; }\ntimeout() { shift; \"$@\"; }\nv=7\nup=1\ncmd() { case \"$*\" in *--set*) v=$(echo \"$*\" | sed 's/.*--set //') ;; *) if [ $up = 0 ]; then n=$(cat $D/asked 2>/dev/null || echo 0); echo $((n + 1)) > $D/asked; [ $((n + 1)) -ge 5 ] || { echo 'Can not find service'; return 0; }; fi; echo \"volume is $v in range [0..15]\" ;; esac; }\n";
        let out = sh(&dir, &format!("{slow}{MEDIA_VOLUME}rm -f $D/asked\nup=0\nvolume_restore\ncat $D/asked\n"));
        assert!(out.contains("audio: the media volume is put back to 7: now: volume is 7 in range [0..15]") && out.ends_with("6\n"), "{out}");   // 4 looks that failed, the 5th that answered, and the read after the set
        // a value that cannot be read is not put back (and nothing is written by volume_restore)
        let unread = fake("case \"$*\" in *--set*) v=$(echo \"$*\" | sed 's/.*--set //') ;; *) echo \"volume is ? in range [0..15]\" ;; esac");
        let out = sh(&dir, &format!("{unread}{MEDIA_VOLUME}volume_restore\n"));
        assert!(out.contains("audio: its value before was not read: it will not be put back\n") && !out.contains("put back to"), "{out}");
        // neither: nothing is written and the line says so
        let none = "timeout() { shift; \"$@\"; }\ncmd() { echo nope; }\nmedia() { return 1; }\n";
        let out = sh(&dir, &format!("{none}{MEDIA_VOLUME}"));
        assert!(out.contains("audio: the media volume is as it was: no range read: [cmd media_session volume: nope ] [media volume: no output] ") && !out.contains("set to"), "{out}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_displays_last_word_is_never_empty() {
        let dir = folder("display");
        let part = |s: &str| {
            let from = s.find("if [ -n \"$bridge\" ]; then\n").unwrap();
            s[from..from + s[from..].find("\nfi\n").unwrap() + 4].to_string()
        };
        let run = |part: &str, text: &str| {
            fs::write(dir.join("display.txt"), text).unwrap();
            sh(&dir, &format!("bridge=999999999\n{part}"))
        };
        let scan = part(&boosted(true));
        let take13 = "oneios-display: the panel is on\noneios-display: crosvm's display service is gone: the VM ended\noneios-display: crosvm's layer removed\n";
        assert_eq!(run(&scan, take13), "display: oneios-display: crosvm's layer removed\n");
        assert!(run(&scan, "oneios-display: something\n").contains("display.txt ends: oneios-display: something"));
        let pool = part(&boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32, shared: false, gpu: false }, 900, true, "ab12", Some(&parse_disks(SCREEN).unwrap())));
        assert_eq!(run(&pool, "displayctl pool: the VM ended after 3.0 s\nx\n"), "display: displayctl pool: the VM ended after 3.0 s\n");
        fs::remove_dir_all(&dir).ok();
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
        assert!(pinned_floor.contains("want=$((top * 60 / 100)); floor=") && pinned_floor.find("$C --log-level info run").unwrap() < pinned_floor.find("echo $floor > $f/scaling_min_freq").unwrap());
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
                            boot_script(&l32, 900, true, "ab12", Some(&touch)), boot_script(&lgpu, 900, true, "ab12", Some(&irq)), boosted(true), boosted(false), ui_script(900),
                            boot_script_with(&lgpu, 900, true, "ab12", Some(&irq), false, true, &Tuning { cpu_floor_percent: 60, ..Tuning::DEFAULT })]
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

    #[test]
    fn the_performance_log_and_the_workers_cpus_are_opt_in_and_outside_every_restore() {
        let d = parse_disks(&format!("{SCREEN}input touch\n{GPU}scanout 720 1560 120\nwifi\n")).unwrap();
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let with = |perf: crate::vmperf::Perf| boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, pin_first: 2, perf, ..Tuning::DEFAULT });
        let plain = with(crate::vmperf::Perf::default());
        // without the flags: the last session's perf.txt is cleared with the other files, and that is all
        assert!(!plain.contains("perf_") && !plain.contains("perfp") && !plain.contains("taskset") && !plain.contains("CROSVM_DRM2KGSL_DIAG") && !plain.contains("--timestats"));
        assert!(plain.contains(" stop.txt perf.txt pace.txt hostmem.txt orch.txt orch.stop vsync.phase boost.state.new bridge.txt bridge.sock\n"));
        let both = with(crate::vmperf::Perf { log: true, unpin: true });
        let at = |what: &str| both.find(what).unwrap_or_else(|| panic!("no {what:?}"));
        // crosvm's environment is the same with the log: no switch of drm2kgsl's (its prebuilt reads CROSVM_DRM2KGSL_DIAG as on by default)
        assert!(!both.contains("CROSVM_DRM2KGSL_DIAG"));
        assert!(both.contains("\nCROSVM_DISPLAY_VULKAN_LIBRARY=/data/local/tmp/oneios-droidvm/libvulkan_freedreno.so $C --log-level info run "));
        // started once crosvm runs (its pid), and waited for after everything is put back, right before the last line
        assert!(at("pid=$!\nset +f\n") < at("perf_stamp() {") && at(") > $D/perf.txt 2>&1 &\nperfp=$!\n") < at("\nwhile [ $t -lt 900 ] && [ ! -e $D/stop.request ]"));
        let end = at("if [ -n \"$perfp\" ]; then\n");
        for restore in ["thp_restore\n", "wifi_restore\n", "caps_restore\n", "kill -0 $bridge 2>/dev/null && kill -9 $bridge"] {
            assert!(both.rfind(restore).unwrap() < end, "{restore}");
        }
        assert!(end < at("echo \"end $(date +%s)"));
        // no trap of the script names it: a signal's and the exit's restores are what they were
        let traps = |s: &str| s.lines().filter(|l| l.trim_start().starts_with("trap ")).map(str::to_string).collect::<Vec<_>>();
        let mut theirs = traps(&both);
        theirs.retain(|t| t != "  trap - EXIT HUP INT TERM");
        assert_eq!(theirs, traps(&plain));
        // the script is otherwise the same: the log's parts taken out again give the plain one
        let start = crate::vmperf::start_script(&crate::vmperf::Perf { log: true, unpin: true });
        assert_eq!(both.replace(&start, "").replace(&crate::vmperf::end_script(&crate::vmperf::Perf { log: true, unpin: true }), ""),
                   plain);
        // the workers alone: no switch for crosvm, no dumpsys
        let unpin = with(crate::vmperf::Perf { log: false, unpin: true });
        assert!(unpin.contains("perf_unpin; fi\n") && !unpin.contains("CROSVM_DRM2KGSL_DIAG") && !unpin.contains("perf_sf start") && !unpin.contains("perf_sf restore"));
        for (n, script) in [both, unpin].iter().enumerate() {
            let path = std::env::temp_dir().join(format!("oneios-vmboot-perf-{}-{n}.sh", std::process::id()));
            fs::write(&path, script).unwrap();
            for shell in ["bash", "sh", "dash", "mksh"] {
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

    #[test]
    fn the_scan_time_is_one_flag_of_the_forwarder_and_goes_with_every_other() {
        let d = parse_disks(&format!("{SCREEN}input touch\n{GPU}scanout 720 1560 120\nwifi\n")).unwrap();
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let line = |s: &str| s.lines().find(|l| l.contains("$D/displayctl touch --evdev")).unwrap().trim().to_string();
        let plain = boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &Tuning::DEFAULT);
        let timed = boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &Tuning { touch_time: true, ..Tuning::DEFAULT });
        assert_eq!(line(&plain), "$D/displayctl touch --evdev /dev/input/$tdev --socket $D/touch.sock </dev/null >>$D/touch.txt 2>&1 &");
        assert_eq!(line(&timed), "$D/displayctl touch --evdev /dev/input/$tdev --socket $D/touch.sock --scan-time </dev/null >>$D/touch.txt 2>&1 &");
        // nothing else of the script changes
        assert_eq!(timed.replace(" --scan-time", ""), plain);
        // with the takeover's keys, the boost and the test swipe: one line, the flag never the last word before a value is due
        let all = boot_script_with(&l, 900, true, "ab12", Some(&d), true, true, &Tuning { touch_time: true, touch_boost: true, takeover: true, ..Tuning::DEFAULT });
        assert_eq!(line(&all), "$D/displayctl touch --evdev /dev/input/$tdev --socket $D/touch.sock --swipe-at 200 --swipe-every 60 --scan-time\
${boostv:+ --boost $boostv --boost-state $D/boost.state}${kl:+ --keys $kl --screen-file $D/screen.off} </dev/null >>$D/touch.txt 2>&1 &");
        // an Image without the touch has no forwarder: the script has no such flag
        let screen = parse_disks(SCREEN).unwrap();
        assert!(!boot_script_with(&l, 900, true, "ab12", Some(&screen), false, true, &Tuning { touch_time: true, ..Tuning::DEFAULT }).contains("--scan-time"));
    }

    #[test]
    fn the_panels_vsync_is_opt_in_for_a_shared_ram_vm_and_outside_every_restore() {
        let d = parse_disks(&format!("{SCREEN}input touch\n{GPU}scanout 720 1560 120\nwifi\n")).unwrap();
        let l = Label { ram: 2560, cpus: 6, swiotlb: 32, shared: true, gpu: true };
        let with = |vsync: Option<i32>, l: &Label| boot_script_with(l, 900, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, vsync, ..Tuning::DEFAULT });
        let plain = with(None, &l);
        assert!(!plain.contains("pacectl") && !plain.contains("pacep"));
        let locked = with(Some(2500), &l);
        let at = |what: &str| locked.find(what).unwrap_or_else(|| panic!("no {what:?}"));
        // the page: the pool's place in crosvm's memfd plus the Image's SEQ (as displayctl pool's --pool-offset and --seq-offset)
        let f = d.fb.as_ref().unwrap();
        let page = d.pool - RAM_BASE + f.seq;
        assert!(locked.contains(&format!("$D/pacectl vsync --pid $pid --page-offset {page} --phase-us 2500 --phase-file $D/vsync.phase </dev/null >>$D/pace.txt 2>&1 &\n")));
        assert_eq!(page % 16384, 0);
        // after crosvm's tree was looked at and the display's helper started; ended after the VM and every restore, before the log's end
        assert!(at("display: the helper ($bridge)") < at("$D/pacectl vsync") && at("$D/pacectl vsync") < at("\nwhile [ $t -lt 900 ] && [ ! -e $D/stop.request ]"));
        let end = at("if [ -n \"$pacep\" ]; then\n");
        for restore in ["thp_restore\n", "wifi_restore\n", "caps_restore\n"] {
            assert!(locked.rfind(restore).unwrap() < end, "{restore}");
        }
        assert!(end < at("echo \"end $(date +%s)"));
        // nothing else changes, and the files of the last session go
        assert_eq!(locked.replace(&crate::vmperf::vsync_start_script(page, 2500), "").replace(&crate::vmperf::vsync_end_script(), ""), plain);
        assert!(plain.contains(" perf.txt pace.txt hostmem.txt orch.txt orch.stop vsync.phase boost.state.new bridge.txt bridge.sock\n"));
        // a protected VM's host cannot write the VM's RAM: no pacectl there, whatever was asked
        let protected = Label { shared: false, ..l };
        assert!(!with(Some(0), &protected).contains("pacectl"));
        // an Image without the screen has no page
        let bare = parse_disks(DISKS).unwrap();
        assert!(!boot_script_with(&l, 900, true, "ab12", Some(&bare), false, true, &Tuning { vsync: Some(0), ..Tuning::DEFAULT }).contains("pacectl"));
        let path = std::env::temp_dir().join(format!("oneios-vmboot-vsync-{}.sh", std::process::id()));
        fs::write(&path, &locked).unwrap();
        for shell in ["bash", "sh", "dash", "mksh"] {
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
    #[test]
    fn the_host_renderers_switches_go_before_crosvm() {
        assert_eq!(host_renderer_env(Floors::On, &[]).unwrap(), "");
        // take19's default: the process-wide rate floor off, the compositor's deadline constraint and the clamp kept
        assert_eq!(host_renderer_env(Floors::Deadline, &[]).unwrap(), "NCTX_PWR_CONSTRAINT=0 ");
        assert_eq!(host_renderer_env(Floors::Off, &[]).unwrap(), "NCTX_PWR_CONSTRAINT=0 DRM2KGSL_PWR_CONSTRAINT=0 DRM2KGSL_VCPU_UCLAMP_MIN=0 ");
        assert_eq!(host_renderer_env(Floors::On, &["DRM2KGSL_PRIORITIES=2".into(), "NCTX_PWR_HOLD_MS=200".into()]).unwrap(),
                   "DRM2KGSL_PRIORITIES=2 NCTX_PWR_HOLD_MS=200 ");
        for bad in ["X", "x=1", "A=$(id)", "A=b c", "1A=2", "=1", "LD_PRELOAD=/x.so", "A=;reboot"] {
            assert!(host_renderer_env(Floors::On, &[bad.to_string()]).is_err(), "{bad}");
        }
        assert_eq!(Floors::parse("deadline").unwrap(), Floors::Deadline);
        assert!(Floors::parse("maybe").is_err());
        let d = parse_disks(&format!("{SCREEN}gpu 00:0c.0 8 0x15f800000\nscanout 1080 2340 60\n")).unwrap();
        let l = Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true };
        let env = host_renderer_env(Floors::Off, &["DRM2KGSL_NONCOHERENT=2".into()]).unwrap();
        let s = boot_script_with(&l, 900, true, "ab12", Some(&d), false, false, &Tuning { crosvm_env: env, ..Tuning::DEFAULT });
        // after the display's Vulkan library, right before crosvm's path
        assert!(s.contains("set -f\nCROSVM_DISPLAY_VULKAN_LIBRARY=/data/local/tmp/oneios-droidvm/libvulkan_freedreno.so NCTX_PWR_CONSTRAINT=0 \
                            DRM2KGSL_PWR_CONSTRAINT=0 DRM2KGSL_VCPU_UCLAMP_MIN=0 DRM2KGSL_NONCOHERENT=2 $C --log-level info run "));
        // without switches the line is as before
        let plain = boot_script_with(&l, 900, true, "ab12", Some(&d), false, false, &Tuning::DEFAULT);
        assert!(!plain.contains("NCTX_") && !plain.contains("DRM2KGSL_"));
        assert!(plain.contains("set -f\nCROSVM_DISPLAY_VULKAN_LIBRARY=/data/local/tmp/oneios-droidvm/libvulkan_freedreno.so $C --log-level info run "));
    }

    #[test]
    fn the_takeovers_memory_steps_compact_zram_and_put_swappiness_back() {
        let (plain, late) = takeover_memory_steps(false, None);
        assert!(plain.starts_with("swap_restore() { :; }\nsync\necho 3 > /proc/sys/vm/drop_caches\n") && !plain.contains("compact") && late.is_empty());
        let (early, late) = takeover_memory_steps(true, Some(20));
        assert!(early.contains("echo 1 > $z/compact"));
        assert!(early.contains("takeover: caches dropped, zram compacted on $zn device(s)"));
        assert!(late.starts_with("sw_old=$(cat /proc/sys/vm/swappiness)\nswap_restore() {") && late.contains("echo 20 > /proc/sys/vm/swappiness"));
        // in the takeover's script: after the trap, before the pool's top-up; put back by caps_restore, which the trap and the end run
        let (stop, end) = takeover_script_with(true, 1792, false, true, Some(20), true);
        let at = |p: &str| stop.find(p).unwrap_or_else(|| panic!("no {p}"));
        assert!(at("trap 'thp_restore; gpu_restore; cpu_restore; caps_restore;") < at("swap_restore() { :; }") && at("swap_restore() { :; }") < at("echo 1 > $z/compact"));
        // the compaction before the pool's top-up; the swappiness after caps_restore (which puts it back) is defined
        assert!(at("echo 1 > $z/compact") < at("need=1792"));
        assert!(at("caps_restore() {\n  swap_restore\n") < at("echo 20 > /proc/sys/vm/swappiness"));
        assert_eq!(end, "caps_restore\n");
        // the default is take17's step plus the compaction, swappiness kept
        let (d, _) = takeover_script(true, 1792, false);
        assert!(d.contains("echo 1 > $z/compact") && !d.contains("> /proc/sys/vm/swappiness") && d.contains("swap_restore() { :; }"));
        // the script as a whole still parses for sh
        let l = Label { ram: 3584, cpus: 6, swiotlb: 32, shared: true, gpu: false };
        let s = boot_script_with(&l, 900, true, "ab12", None, false, true, &Tuning { takeover: true, takeover_swappiness: Some(20), ..Tuning::DEFAULT });
        assert!(s.contains("echo 20 > /proc/sys/vm/swappiness"));
        let dir = std::env::temp_dir().join(format!("oneios-takeover-sh-{}", std::process::id()));
        fs::write(&dir, &s).unwrap();
        let ok = std::process::Command::new("sh").arg("-n").arg(&dir).status().map(|st| st.success()).unwrap_or(true);
        let _ = fs::remove_file(&dir);
        assert!(ok, "sh -n refused the boot script");
    }

    #[test]
    fn the_takeover_wakes_the_panel_once_before_one_ui_stops() {
        let (stop, _) = takeover_script(true, 1792, false);
        let wake = stop.find("input keyevent KEYCODE_WAKEUP").expect("the wake");
        assert_eq!(stop.matches("KEYCODE_WAKEUP </dev/null").count(), 1, "one event, not a loop");
        // with One UI up: before the stop's first step, after ui.sh is sourced
        assert!(wake < stop.find("# the watchdog nodes system_server keeps open").unwrap());
        assert!(stop.find(". $D/ui.sh\n").unwrap() < wake);
        let (none, _) = takeover_script_with(true, 1792, false, true, None, false);
        assert!(!none.contains("KEYCODE_WAKEUP"));
    }

    #[test]
    fn the_panel_rate_is_free_only_when_asked_and_its_facts_are_read_before_the_stop() {
        let d = parse_disks(&format!("{SCREEN}gpu 00:0c.0 8 0x15f800000\nscanout 720 1560 120\n")).unwrap();
        let l = Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true };
        let t = Tuning { takeover: true, touch_boost: true, panel_rate_min: 24, ..Tuning::DEFAULT };
        let s = boot_script_with(&l, 900, true, "ab12", Some(&d), false, false, &t);
        assert!(s.contains("--crosvm-display oneios_disp --size 720x1560 --hz 120 --wait 120 --takeover --screen-file $D/screen.off \
                            --rate-min 24 --rate-hold-file $D/boost.state </dev/null"), "{s}");
        let held = boot_script_with(&l, 900, true, "ab12", Some(&d), false, false, &Tuning { panel_rate_min: 0, ..t });
        assert!(!held.contains("--rate-min") && held.contains("--takeover --screen-file $D/screen.off </dev/null"));
        // the panel's facts: with One UI up (before ui.sh's stop), in every takeover, never without one
        assert!(s.find("# the panel's corners").unwrap() < s.find(". $D/ui.sh\n").unwrap());
        let (stop, _) = takeover_script(true, 1792, false);
        assert!(!stop.contains("# the panel's corners"), "the stop alone carries no facts: boot_script_with adds them");
        let plain = boot_script_with(&l, 900, true, "ab12", Some(&d), false, false, &Tuning::DEFAULT);
        assert!(!plain.contains("# the panel's corners"));
        let dir = folder("panel-facts");
        let fake = "timeout() { shift; \"$@\"; }\ndumpsys() { echo 'mDisplayInfos: roundedCorners RoundedCorners{[RoundedCorner{position=TopLeft, radius=128, \
                    center=Point(128, 128)}, RoundedCorner{position=TopRight, radius=128, center=Point(1312, 128)}]} cutout DisplayCutout{insets=Rect(0, 104 - 0, 0) \
                    waterfall=Insets{left=0, top=0, right=0, bottom=0}}'; }\n";
        let out = sh(&dir, &format!("{fake}{PANEL_FACTS}"));
        assert!(out.contains("panel: RoundedCorner{position=TopLeft, radius=128, center=Point(128, 128)} RoundedCorner{position=TopRight"), "{out}");
        assert!(out.contains("panel: DisplayCutout{insets=Rect(0, 104 - 0, 0) waterfall=Insets{left=0, top=0, right=0, bottom=0}"), "{out}");
        assert!(sh(&dir, &format!("timeout() {{ shift; \"$@\"; }}\ndumpsys() {{ :; }}\n{PANEL_FACTS}")).is_empty(), "nothing when dumpsys says nothing");
        fs::remove_dir_all(&dir).ok();
        // the option's terms
        let o = Options { seconds: 900, thp: true, bridge: String::new(), swipe_test: false, pin: false, droidvm: String::new(), gpu_floor_mhz: 0,
                          cpu_floor_percent: 0, gpu_idle_ms: 0, touch_boost: false, takeover: true, gpu_cap_lift: false, max_volume: true, pin_first: 0,
                          thermal_log: false, touch_time: false, vsync: None, pacectl: String::new(), perf: crate::vmperf::Perf { log: false, unpin: false },
                          audio_uid: None, mic_capture: false, video_decoder: None, wifictl: String::new(), bridgectl: String::new(), haptics: true,
                          virgl: String::new(), crosvm: String::new(), gpu_host_floors: Floors::Deadline, crosvm_env: Vec::new(), takeover_reclaim: true,
                          takeover_swappiness: None, takeover_wake: true, panel_rate_min: 24, take23: Default::default() };
        let refused = check_boost(&l, &o, Some(&d)).unwrap_err().to_string();
        assert!(refused.contains("--panel-rate-min HZ: 1 to 119, with --takeover, --touch-boost"), "{refused}");
        assert!(check_boost(&l, &Options { panel_rate_min: 120, touch_boost: true, ..o }, Some(&d)).is_err());
    }

    #[test]
    fn the_sound_cards_line_is_checked_against_crosvms_map() {
        let d = parse_disks(&format!("{SCREEN}gpu 00:0c.0 8 0x15f800000\naudio\naudio-intid 44 8\n")).unwrap();
        assert_eq!(d.audio_intid, Some((44, 8)));
        assert!(parse_disks(&format!("{SCREEN}audio-intid 44 8\n")).is_err(), "a line without the card");
        assert!(parse_disks(&format!("{SCREEN}audio\naudio-intid 31 8\n")).is_err());
        assert!(parse_disks(&format!("{SCREEN}audio\naudio-intid 44 32\n")).is_err());
        let check = audio_intid_check(8, 44);
        assert!(check.contains(" 00004000 00000000 00000000 0000000[1-4] ") && check.contains("[ $9 = 0000000c ]"));
        let l = Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true };
        let s = boot_script(&l, 900, true, "ab12", Some(&d));
        assert!(s.contains("snd_irq() {") && s.contains("if [ \"$pool\" != bad ] && ! snd_irq; then pool=bad; touch $D/stop.request; fi"));
        assert!(!boot_script(&l, 900, true, "ab12", Some(&parse_disks(&format!("{SCREEN}gpu 00:0c.0 8 0x15f800000\naudio\n")).unwrap())).contains("snd_irq"));
        // the check finds take17's map entry for 00:08.0 (SPI 12): the shell's own parse, on the bytes crosvm wrote
        let dir = std::env::temp_dir().join(format!("oneios-snd-irq-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let map: Vec<u32> = vec![0x6000, 0, 0, 1, 1, 0, 0, 0, 4, 4, 0x4000, 0, 0, 1, 1, 0, 0, 0, 0xc, 4];
        let bytes: Vec<u8> = map.iter().flat_map(|w| w.to_be_bytes()).collect();
        fs::write(dir.join("fdt.dtb"), &bytes).unwrap();
        for (slot, intid, ok) in [(8, 44, true), (8, 45, false), (9, 44, false)] {
            let script = format!("D={}\npool=ok\n{}echo \"pool=$pool\"\n", dir.display(), audio_intid_check(slot, intid));
            let out = std::process::Command::new("sh").arg("-c").arg(&script).output().unwrap();
            let text = String::from_utf8_lossy(&out.stdout);
            assert_eq!(text.contains("pool=ok"), ok, "slot {slot} intid {intid}: {text}");
        }
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_lines_and_the_decoders_pool_are_checked_in_one_script() {
        // take18b, every check of the Image's PCI layout at once: the GPU's line (gpu-intid), the sound card's (audio-intid)
        // and the video decoder's pool (media) share one expression of the boot script
        let d = parse_disks(&format!("{SCREEN}input touch\ngpu 00:0c.0 8 0x153800000\nscanout 720 1560 120\ngpu-intid 36\naudio\naudio-intid 44 8\nmedia 192 0x154000000\n")).unwrap();
        assert_eq!((d.gpu_intid, d.audio_intid, d.media.as_ref().map(|m| (m.mib, m.host))), (Some(36), Some((44, 8)), Some((192, 0x154000000))));
        let l = Label { ram: 2880, cpus: 1, swiotlb: 32, shared: true, gpu: true };
        let s = boot_script_with(&l, 900, true, "ab12", Some(&d), false, true, &Tuning { takeover: true, video_decoder: Some(2000), ..Tuning::DEFAULT });
        let (gpu, snd, pool) = (s.find("gpu_irq() {"), s.find("snd_irq() {"), s.find("media_host@154000000"));
        assert!(gpu.is_some() && snd.is_some() && pool.is_some(), "{s}");
        assert!(gpu < snd && snd < pool, "the checks in order: the GPU's line, the sound card's, the decoder's pool");
        assert!(s.contains(" \\\n  --virtio-media kind=decoder,uid=2000 \\\n"));
        // and the whole script parses as POSIX sh
        let path = std::env::temp_dir().join(format!("oneios-vmboot-all-lines-{}.sh", std::process::id()));
        fs::write(&path, &s).unwrap();
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
