# YumikoHayamato's obsessed build

*The Galaxy S25 Ultra build of **OneiOS** — iOS 27 (Darwin 26) running on the phone's own silicon, from a live Android session, with no bootloader unlock and no flashing.*

OneiOS runs a source-built Apple kernel (**XNU 12377**, from Droid-VM's UniXNU fork) and Apple's unmodified
iPhone18,2 iOS 27.0.1 (`24A446`) user space on a rooted Samsung Galaxy, next to One UI, using
[OneOS](https://github.com/akshaynexus/OneOS)'s session model: One UI's Linux kernel keeps running, the image lives
in a folder, and the switch and the way back stay OneOS-shaped. Apple's binaries come from the owner's own IPSW
download and are never redistributed here.

This branch (`s25-ultra-sm8750`) is the **Galaxy S25 Ultra (SM-S938B, SM8750 "sun", Adreno 830)** build. It descends
from the S24 Ultra (SM8650) guest-boot baseline and the S26 Ultra (SM8850) branch, but every result below was
measured on the owner's S25 Ultra (the `pa3q` sessions in [docs/SM8750_PORT.md](docs/SM8750_PORT.md)).

> **Honesty up front.** Root on this phone is a **temporary KernelSU LKM loaded through an exploit on a *locked*
> bootloader**; it is gone after any reboot. iOS runs **entirely from RAM** and writes nothing to UFS. There is no
> Secure Enclave, no passcode, no Face ID — the key store is software and is **not a security boundary**. Sessions
> are owner-driven, reversible tests, not a daily driver you install. Nothing here is flashed.

---

## Two ways it runs

OneiOS drives iOS on the S25 Ultra through **two independent paths**. Both run the same Apple user space; they differ
in how XNU gets onto the CPU.

### 1. Hosted path — XNU as a Linux process (`runtime/xnuhost`)

iOS cannot run on Linux's system calls, so OneiOS runs **XNU as an ordinary Linux process**. Every iOS process is a
Linux process whose system calls and faults are trapped (seccomp) and handed to that XNU in another thread. No
hypervisor, no emulation — iOS code runs **natively** on the ARM64 cores, PAC and all.

```
 One UI's Linux kernel (unchanged, KernelSU root)            Samsung vendor half keeps running
 ├─ surfaceflinger + composer HAL ◄── oneios-display (a full-screen layer with iOS's frames)
 ├─ /dev/kgsl-3d0, /dev/udmabuf   ◄── xnuhost's virtio-gpu 3D (drm2kgsl → KGSL → Adreno 830)
 ├─ audioserver, vibrator evdev, backlight sysfs, ALS, MediaCodec ◄── bridges in xnuhost
 ├─ wlan0 + the vendor's wpa_supplicant (AIDL) ◄── the Wi-Fi bridge: iOS's en0, no NAT
 └─ xnuhost  (one Linux process; its own mount namespace and cgroup)
     ├─ one host thread per vCPU; GICv3, timer, PSCI and EL1 system registers emulated
     ├─ virtio devices: block (the image set), input (evdev), GPU, sound, net, camera, bridge channel
     ├─ XNU 12377 + Apple's iPhone18,2 kexts, prelinked into one kernel.kc
     └─ iOS tasks = Linux processes from one factory (one PAC key set); a stub traps svc and faults
         └─ launchd → daemons → backboardd → SpringBoard  (iOS 27.0.1, Apple's binaries unchanged)
```

### 2. Native path — XNU alone in a Gunyah VM (`docs/native.md`)

The S25's own hypervisor (**Qualcomm Gunyah**) runs protected VMs for Android (AVF/crosvm). The native path puts
**XNU alone at EL1 in such a VM**, on the bare cores, with its own page tables, while One UI stays up. Droid-VM's
`gh_hugepage_reserve` grows a shared-RAM pool (with One UI's framework briefly stopped, ~37 s, no phone reboot) so
the VM gets gigabytes of real memory; the VM's RAM is shared with Android, so GPU work goes to Android's KGSL on
iOS's own buffers. This is the owner's answer to the hosted path's trap overhead and heat.

---

## What runs today on the Galaxy S25 Ultra

**Hosted path** (sessions 1–7, 2026-11-19, `logs/pa3q-s*`):

- **Full boot to SpringBoard** with the owner's finger: Home Screen, apps, Spotlight, Control Center. launchd →
  60 boot tasks → backboardd → SpringBoard, first content frame ~24 s in.
- **Real Metal on the Adreno 830**: NativeAdrenoMetal → Turnip/freedreno (Vulkan) → drm2kgsl, on iOS's own buffers.
  **A locked 120 fps at 1080×2340** (up to 1202 scanout flushes in 10 s), GPU 0.7–5.4 ms/frame.
  - The **one missing piece** is a frequency orchestrator: there is **no iOS-tuned CPU/GPU governor yet**, so it
    rides Android's governor, which isn't calibrated for iOS. That's the source of the light stutter — not a low
    frame rate. Under sustained load the SoC also hits the thermal trap (session 7: case 46 °C → iOS's "too hot"
    state → ~10 fps), which is exactly why the owner opened the native path.
- Wi-Fi, haptics, thermal-aware clocking, multi-touch and the three side keys, **6.4 GiB locked** for the guest
  (iOS's RAM is never swapped).

**Native path** (Gunyah shared-RAM VM via crosvm, through 2026-11-24):

- **8 vCPUs, 3.5 GiB**, full boot to SpringBoard (steps 1–9 in `docs/native.md`: disks over virtio-PCI, the boot
  video, the lock screen on the panel, touch, the owner's finger, the real clock, the shared GPU pool).
- **GPU scanout** shown in the Android display through a Turnip blit; the guest draws for **120 Hz** (about 100
  frames a second in a swipe). The panel itself followed One UI's last policy (60 Hz with power saving, take12);
  from 2026-11-25 the display helper holds it at 120 Hz in a takeover (not yet run on the phone).
- **Audio**: virtio-snd over PCI → AAudio, speaker with the volume keys.
- **Wi-Fi for real**: iOS joins a WPA2 network through the phone's own radio and gets a DHCP lease.
- **Input**: multi-touch, the on-screen keyboard (the dictation loop that pinned a core is fixed), and the hardware
  side keys — power switches the panel, volume goes to iOS, a long press exits.
- **Takeover mode**: One UI's framework is paused while iOS runs, and restored on exit.
- **In progress:** the GPU *device* inside the VM (drm2kgsl + virtio-gpu over crosvm's PCI) so the guest renders in
  the VM instead of on the CPU; today the shared pool and scanout are proven, the in-VM renderer is next.

Per-session logs and raw numbers: [docs/SM8750_PORT.md](docs/SM8750_PORT.md) (hosted) and
[docs/native.md](docs/native.md) (the Gunyah VM).

---

## The phone (measured on the owner's S25 Ultra, read-only)

- **SM-S938B** (`pa3qxeea`), SM8750 board `sun`, One UI 9.0 on Android 17, kernel `6.6.127-android15-8`, **4 KiB
  pages** (no 16 KiB granule on Oryon — iOS's 16 K code signatures are re-derived once per first boot).
- **CPU:** Qualcomm Oryon, 6 E-cores 0.38–3.53 GHz + 2 P-cores 1.02–4.47 GHz; PAC + BTI. Mapped to 6 E + 2 P vCPUs.
- **GPU:** Adreno 830 (`Adreno830v2`), KGSL, GMEM 12 MiB, 160–832 MHz; the guest's Turnip knows the a8xx gen1 chip.
- **RAM:** 11.4 GB (largest block 9 GiB in one piece); **display** `nt37801` AMOLED, **command-mode** 1440×3120 at
  120/80/60/48/30/24/10 Hz.
- **Hypervisor:** Gunyah with AVF (`/dev/gunyah`, protected VMs only), plus Samsung's `CONFIG_UH`/`RKP`/`KDP`
  protections and a locked bootloader (no kexec, no `fastboot boot`).

Full inventory — CPU, GPU, input, thermal, battery, the live FDT, the partitions to back up before any native
test — is in [docs/SM8750_PORT.md](docs/SM8750_PORT.md) §0b.

---

## On-device `.ipa` installer (`appinst`, in review)

[PR #1](https://github.com/YumikoHayamato/OneiOS/pull/1) adds a TrollStore-style **on-device installer**: download an
already-decrypted `.ipa` in Safari, open a small local page, tap Install, and after a guest-VM restart the icon
appears on SpringBoard — no PC, no 7-day certificate.

Because the S25 runs in **SPTM/TXM** mode (the Trusted Execution Monitor is the signing authority; a binary runs only
if its CDHash is in a trust cache, and AMFI is never patched), the installer leans on the fact that **the owner trust
cache is a sidecar xnuhost injects at guest boot**: it re-signs the app ad-hoc at 4 KiB pages, adds its CDHashes to
the owner module, and a guest-VM restart makes TXM trust it. It **only accepts an already-decrypted `.ipa`** — there
is no FairPlay decryption or DRM-stripping anywhere in it; an encrypted slice is detected and refused. Design:
[userland/appinst/README.md](userland/appinst/README.md) and `docs/appinst.md`.

---

## Build and run

- How the parts fit together: [docs/architecture.md](docs/architecture.md)
- How to build and test it end to end: [docs/building.md](docs/building.md)
- Each part in depth: [docs/booter.md](docs/booter.md) (xnuhost + loader),
  [kernel/prelink/README.md](kernel/prelink/README.md) (`kernel.kc`), [docs/gpu.md](docs/gpu.md),
  [docs/userland.md](docs/userland.md), [docs/hardware.md](docs/hardware.md), [docs/native.md](docs/native.md).

```
docs/            architecture, building, and one file per part
profiles/        the pinned IPSW profile (iPhone18,2 iOS 27.0.1 24A446)
scripts/fetch/   fetch only the needed IPSW members from Apple's CDN (never from here)
scripts/image/   build the image set (SHA-256, .tar.zst)
userland/        iOS 27 root and Data tweaks, guest helpers, appinst, the camera/Metal plug-ins
kernel/          XNU 12377 (UniXNU / Droid-VM fork) on Linux; oneios-hosted/ patches; prelink/ (kernel.kc)
runtime/         xnuhost + its loader, oneios-display, the hardware bridges, the runtime package
drivers/         iOS-side drivers for the bridges (CPU, network, Bluetooth, haptics, audio, brightness, camera)
oneos/           the contract with the OneOS fork (akshaynexus/OneOS, branch oneios)
tools/           oneios-tool (Rust): image, prelink, phone, app, cycle, log, sym, …
```

## A session, step by step

1. **Stage** (One UI running): the runtime package and the image set are verified against their manifests and staged
   as root under `/data/local/tmp/oneios/`.
2. **Arm the way back:** a watcher, the Volume Up + Volume Down readers and a wakelock start *before* anything stops.
3. **Stop One UI's framework** (zygote, system_server). The Linux kernel, vendor HALs, surfaceflinger and the
   composer keep running.
4. **Start xnuhost** (or the Gunyah VM) in its own mount namespace and cgroup, then `oneios-display`.
5. **Boot:** XNU mounts the verified OS volume, formats the Data volume and unpacks the seed on first start, mounts
   the cryptexes, starts launchd; backboardd draws on the framebuffer; at its first non-black frame xnuhost writes
   `session/ready`.
6. **Return:** Volume Up + Volume Down for 3 s, a request from inside iOS, xnuhost exiting, or no `ready` in time —
   any of them ends the session, restarts the composer and zygote, and restores One UI **in place**. Power + Volume
   Down (Samsung's forced restart) is the backstop below all software.

## What it is not

- **Not an emulator:** no instruction translation (that's the vphone app's QEMU path).
- **Not a secure store:** no Secure Enclave, no passcode, no lock. The key store is software with derivable keys
  (`kernel/patches/oneios-hosted/keystore/README.md`); the Data store is only as private as One UI keeps the file.
- **Not Apple's kernel binary:** XNU is built from Apple's open source; only Apple's kexts and user space are
  Apple's, taken from the owner's own IPSW and never redistributed.
- **Never:** passcode, Face ID, Wallet, calls or SMS.

## Lineage and history

The Galaxy S24 Ultra (SM8650) is the original guest-boot baseline: on it the hosted path first reached the lock
screen and, after unlock, an empty Home Screen composited on the GPU at 40–60 fps (2026-11-11/12). The S26 Ultra
(SM8850) branch carried that forward; the `s25-ultra-sm8750` branch starts from it because the S25 is closer to the
S26 than to the S24. The earlier S24/S26 records and the full on-PC (QEMU) boot results live in
[docs/hardware.md](docs/hardware.md) and the per-device port notes.

## Rules (for contributors)

- **No Apple files in git or in releases.** IPSWs, cryptexes, kernelcaches, kexts, `kernel.kc` and every image built
  from them stay in `out/`/`inputs/` (git-ignored); releases carry only this project's own code. See
  [CLAUDE.md](CLAUDE.md) for the full working agreement (fix don't only probe; iOS RAM never swapped; the way back
  must always work; docs updated in the same commit).
- Long jobs run in tmux via `scripts/bg.sh NAME COMMAND…`.

## Credits

[OneOS](https://github.com/akshaynexus/OneOS) / [OneLineage](https://github.com/WeWaffles/OneLineage): the Android
container hot-switch. [MysticGSI](https://github.com/MysticGSI/mysticgsi): the image-build model.
[UniXNU](https://github.com/UniXNU/UniXNU) and [Droid-VM](https://github.com/Droid-VM): XNU on non-Apple ARM64,
drm2kgsl, the guest drivers, `gh_hugepage_reserve`. [iOSPlus10](https://github.com/akshaynexus/iOSPlus10): XNU and
iOS user space on Qualcomm phones, haptics and brightness. [ios-dodge](https://github.com/palazik): the S25's live
hardware map. [Darling](https://github.com/darlinghq/darling): Mach and XNU on Linux.
