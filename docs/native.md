# The native path: XNU alone on the phone

The owner's decision of 2026-11-19, after session 7 (`docs/SM8750_PORT.md`): hosted iOS is stable, but it runs at 55-98
fps while cool and at 10 fps when iOS's thermal state is "too hot" (case 46 C), and the host's Linux kernel takes more
than half of the CPU it uses. Goal: XNU alone on the Galaxy S25 Ultra, started from a running Android with Linux then
shut down (no bootloader unlock). Months of work; the GPU (Adreno 830, no XNU driver anywhere) is the wall.

## Facts that shape it
- The SM8750's Oryon cores have no 16 KiB translation granule (ID_AA64MMFR0_EL1 TGran16 = 0, measured on a OnePlus 13
  by palazik/ios-dodge, `external/ios-dodge/NOTES.md`): 4 KiB pages. The standalone kernel carries UniXNU's native-4k
  patches (`out/kernel/current/series.txt`).
- From ios-dodge, **verified on the S25's live FDT on 2026-11-20** (`oneios-tool phone hw`; the numbers are in
  `docs/SM8750_PORT.md` section 0b): GICv3 distributor 0x16000000, redistributors 0x16080000 with a stride of
  0x40000, the timer at 19.2 MHz (virtual timer INTID 27), PSCI by SMC, no usable UART. The S25 adds: 9 GiB of RAM in
  one piece at 0x8_c000_0000, a **command-mode** panel (the bootloader's splash at 0xfc800000 is not scanned out by
  itself), Samsung's hypervisor-backed protections in its kernel (`CONFIG_UH`, `RKP`, `KDP`) and no kexec.
- Starting XNU alone needs Linux to hand the CPUs over (the bootloader is locked: no `fastboot boot`, which is how
  ios-dodge, ios-sweet and ios-lemonadep start theirs). One UI runs as a VM of Qualcomm's Gunyah at EL2
  (`gh_virt_wdt` is its watchdog), with Samsung's protections beside it: whether they let a kernel module load, turn
  off Linux's MMU and set XNU's own TTBR1/TCR/SCTLR is unknown [U], and the first try that fails resets the phone.
- **The same Gunyah runs protected VMs for Android** (AVF): `ro.boot.hypervisor.protected_vm.supported=true`,
  `/dev/gunyah`, AVF's crosvm in the apex. XNU as such a guest runs at EL1 on the real cores with its own page tables,
  and One UI stays up under it (see "A VM under Gunyah" below).
- Safety (ios-dodge `RECOVERY.md`: ios-topaz lost its GPT to an iOS UFS driver that wrote LUN0): before any test on
  the phone, back up LUN0's GPT and the handset's unique partitions (Samsung's EFS and the modem's); iOS runs from
  RAM; XNU writes nothing to UFS until its driver is trusted.

## The bench (PC, QEMU)
- QEMU: Arch's `qemu-system-aarch64` 11.1.1 and its 13 dependencies, extracted into `/mnt/tkg-build/oneios/sysroot`
  (each package's SHA-256 checked against pacman's sync database; the files from the Arch archive), run with
  `LD_LIBRARY_PATH=$SYSROOT/usr/lib`. Nothing installed to `/`.
- Kernel: `scripts/bg.sh kernel-vm env LIBDISPATCH_PREFIX=/mnt/tkg-build/oneios/sysroot/usr kernel/build-linux.sh`
  (`out/kernel/current/xnu.macho`, 140 s; `build-linux.sh` now binds UniXNU's `plutil` as `build-hosted.sh` does:
  GNUstep's `/usr/bin/plutil` stopped installhdrs).
- Loader: `runtime/loader/tools/xnuloader-qemu.c` places a kernel as a firmware loader does (`xl_boot_place` with
  virt_base = the kernel's base) on virt (GICv3 0x08000000/0x080a0000, PL011 0x09000000, PSCI by HVC), with a stub 2 MiB
  into RAM that sets x0 and branches; `scripts/qemu/boot-vm.sh` runs it under a timeout in tmux and keeps the console
  and every exception (`out/qemu/vm/`).
- First run (2026-11-19, the plain `xnu.macho`): the MMU comes on and the kernel runs at its virtual addresses, then
  stops in `static_if_init` (FAR 0x2000000019e230, a pointer with PAC's error bits). An MH_EXECUTE kernel with
  `LC_DYLD_CHAINED_FIXUPS` is neither rebased nor signed by `arm_slide_rebase_and_sign_image` (only `__thread_starts`
  are), so it boots as a kernel collection, as on an iPhone.
- Its collection: `scripts/bg.sh prelink-vm env KERNEL=$PWD/out/kernel/current/xnu.macho OUT=$PWD/out/kernel/vm-24A446
  kernel/prelink/build.sh` (never over the phone's `out/kernel/24A446`): 93 MB with Apple's iPhone18,2 kexts. The emit
  step binds a leaf pair whose native name the kernel lacks to that kernel's own function with the same instructions
  (`kernel/prelink/README.md`).
- The whole loop in one job (about 8 minutes on this PC, most of it the kernel build):
  `scripts/bg.sh native-bench bash -c 'LIBDISPATCH_PREFIX=/mnt/tkg-build/oneios/sysroot/usr kernel/build-linux.sh &&
  KERNEL=$PWD/out/kernel/current/xnu.macho OUT=$PWD/out/kernel/vm-24A446 kernel/prelink/build.sh &&
  KERNEL=$PWD/out/kernel/vm-24A446/kernel.kc scripts/qemu/boot-vm.sh 60'`.
- The console: the board's `oneios-board-pl011-console.patch` compiles XNU's PL011 driver outside the hosted build
  (`PL011_UART`; the BCM2837 config had no serial driver at all), and `xnuloader-qemu` gives `/arm-io/uart0` an
  `AAPL,phandle` that `/defaults serial-device` names (`pe_serial.c` `get_serial_device_phandle`).
- Second blocker, fixed: a data abort in `_Block_copy` from `IOService::setNotification` (a kext's
  `addMatchingNotification` with a block). Apple's clang signs block descriptor pointers and stock clang does not; the
  hosted kernel stripped the signature in hosted 0007, the standalone kernel did not. The hunk is now the board's
  `oneios-kext-block-descriptors.patch` (both kernels; the hosted source is the same as before).
- 2026-11-19, QEMU, 60 s: Darwin 25.5.0 with Apple's kexts runs through BSD and IOKit start: AMFI, Sandbox, the MAC
  framework, CoreAnalyticsHub, our board drivers (off: no bridge on virt), the network stack, APFS
  (`com.apple.filesystems.apfs` 3288.2.1), and stops at `Waiting on <dict ...IOMedia ... Apple_HFS>`: the root device.

## The native kernel and the image set's disks (2026-11-20)
The plain kernel of `build-linux.sh` has no disks and none of what the hosted kernel does between the root mount and
launchd. The **native kernel** has both, from the hosted kernel's own code:
- `kernel/build-native.sh` (= `build-hosted.sh` with `ONEIOS_MACHINE=native`): the board's series and the whole hosted
  series, with `CONFIG_XNU_ONEIOS_NATIVE=y` and without `CONFIG_XNU_HOSTED` (`kernel/device/configs/oneios_native_defconfig`),
  so the hosted machine layer stays out; hosted `0076-native-os-layer.patch` opens to it what is about iOS on the image
  set and not about the host (the root, Data, seed and launch policy of 0004 and the later blocks of `kern_exec.c`, the
  boot manifest's `/chosen`, the parallel code hash, iOS 27's shared region, the AES and key store starts, one kind of
  CPU, the debugger's acknowledge timeout; `kernel/patches/oneios-hosted/README.md` "0076"). The archives: `storage`,
  `board`, `aes`, `keystore`. `out/kernel/current-native/xnu.macho`.
- The disks: `OneiOSHostedDisk` (the hosted class, the same nodes) maps its virtio-mmio window, and since no host checks
  the reads, checks every chunk of os, systemos and app against the image set's hash list when it is first read
  ("oneios,hash-list" in the node; verified only when the list's SHA-256 is the node's "oneios,verified").
- The collection: `kernel/prelink/build.sh` sees a native kernel (no `hosted_halt`) and leaves Apple's kexts as they are
  (`out/kernel/native-24A446/kernel.kc`). Its step 3, the hosted rewrites, broke a native kernel twice: smr_hash's `orr`
  turned into an `and` made APFS read `0xff00fff007ddd988` (`__smrh_empty` with bits 48..55 cleared) at the Data volume's
  first mount, and `mrs MPIDR_EL1` became `ldr [x18]`.
- The loader: `xnuloader-qemu ... --image DIR --data FILE` reads the image set as xnuhost does (its `imageset.c`: manifest,
  hash lists, trust caches): the five disks on transports 0-4 (os, systemos, app, the seed read-only; the Data store
  writable), each node with xnuhost's identity, `/chosen` root-matching on the OS volume, the four trust cache modules.
  Modern virtio (`-global virtio-mmio.force-legacy=false`), and a Cortex-A710 (`pauth-impdef=on,sve=off,cntfrq=1 GHz`):
  QEMU's `max` has FEAT_LVA, which the DEVELOPMENT kernel asserts no CPU has (`vm_sanitize.c:643`, at launchd's first
  jobs).
- The loop: `kernel/build-native.sh` (about 25 min from an empty tree, 2 min after a one-file change), then
  `KERNEL=$PWD/out/kernel/current-native/xnu.macho kernel/prelink/build.sh` (1 min), then
  `scripts/bg.sh qemu-native env KERNEL=$PWD/out/kernel/native-24A446/kernel.kc IMAGE=<image set> DATA=<store>
  CPUS=4 RAM_MIB=2816 ARGS_ADD=oneios-data-wait=1800 OUT=$PWD/out/qemu/native scripts/qemu/boot-vm.sh 3600`. The
  store (16 GiB, sparse, made by `boot-vm.sh`) is kept in RAM (`/tmp`); the image set stays where it is, read-only.

Runs of 2026-11-20 (consoles in `logs/native-qemu/`, local):
1. The disks start, all three images verified; the root (`Rave24A446.V54OS`) mounts and is accepted as a verified one;
   the SystemCryptex over `/`; AES and Image4; `newfs_apfs` for the Data store, then APFS's data abort on the cleared
   pointer above. Fixed in the prelink.
2. cpu 1 stops at its start (`cpu_common.c:957`, `cpu_signal` 0x8 instead of `SIGPdisabled`): sheng's IPI recovery
   cleared the flag from another vCPU, as it did under xnuhost (hosted 0006 turned it off; 0076 turns it off here too).
   The others went on at 0.2 MB/s of disk.
3. No stop until launchd: `newfs_apfs` (the first user process; its dyld hashed the shared cache, 5.8 GiB, at about
   12 MB/s on 4 vCPUs) formatted the store, `seed verified: 6039 files (1252972721 bytes), 14818 folders`, the launch
   policy ran (cryptex views, GPU addon, staged apps, jobs, features), **`load_init_program: /sbin/launchd`**, launchd
   started `oneios-init` and `oneios-bootstat` (`ONEIOS-GUEST stage=bootstat-up`), then `sched_clutch.c:6589` (one
   kind of CPU; hosted 0016, opened by 0076).
4. The store's second start (mounted as it is, no seed): launchd in about a minute; `hello, launchd`, `Darwin
   Bootstrapper Version 7.0.0`, and its boot tasks run (exclaves-boot, fsck, mount-phase-1, data-protection,
   detect-installed-roots, boot mode `main`, mount-phase-2, init-with-data-volume, MSUEarlyBootTask, MobileAssetEarlyBootTask,
   fips, ...), then `vm_sanitize.c:643` (the CPU model, above).
5. **The Cortex-A710 (about 15 minutes, stopped by hand): no panic.** launchd's 60 boot tasks (keybag, usermanagerd,
   dirs_cleaner, ...), then its daemons: 51 programs in all, among them runningboardd, configd, logd, amfid, keybagd,
   powerd, thermalmonitord, locationd, CommCenter, bluetoothd, sharingd, containermanager, symptomsd, driverkitd and
   **backboardd** (`ONEIOS-GUEST stage=backboardd-up t=176`: 176 s after `oneios-bootstat`); no crash on the console.
   Not seen: SpringBoard (backboardd has no display here). The clock is at 1970 (no RTC on the machine tree).
   logd_helper used 88 % of a vCPU for 100 s (`caught burning CPU`).

## The display on the bench (2026-11-20)
The hosted kernel's display, as it is: UniXNU's NativeMobileDisplay under the collection's `IOMobileFramebufferLegacy`
and Droid-VM's virtio-gpu scanout (`kernel/patches/oneios-hosted/display/README.md`), with Droid-VM's virtio-gpu transport
(the `gpu` archive). Without a 3D device the GPU archive says "display only" and publishes no Metal accelerator, so iOS
draws with the CPU (as on the PC under xnuhost, `docs/gpu.md`).
- The native kernel links `gpu` and `display` (`CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y`); 0076 turns the display on as the
  hosted kernel has it (`bsd_init.c`'s two switches) and gives the two hosted hooks the GPU archive declares weak a
  native default.
- `xnuloader-qemu --display 828x1792 --scale 2` (boot-vm.sh `SCREEN=828x1792 SCALE=2`): QEMU's `virtio-gpu-device` on
  transport 5 (2D; `qemu-hw-display-virtio-gpu`'s module, from the sysroot with `QEMU_MODULE_DIR`), the boot video right
  after the kernel's window (6 MiB at 0xf3004000), the tree's mode and scale, `offline-legacy-unit-scale=2 sheng-vsync=60`
  in the boot arguments. 828x1792 at @2x is an iPhone 11's 414x896 points: a third of the phone panel's pixels for the
  CPU to draw.
- The transport wanted a control queue of 512 descriptors; QEMU's 2D device has 64. Now it takes what the device offers
  (`staged/gpu.rs` `TRANSPORT`, 32 slots of which 12 large on QEMU; 256 as before with xnuhost and crosvm):
  `virtio-gpu: the device's control queue is 64 descriptors: 32 command slots, 12 of them large`, then
  `oneios-display: panel 828x1792 at 0xf3004000 ...; virtio scanout state 2 (2 = presenting)`.
- Watching: `VNC=1` serves the display on 127.0.0.1:5901 (`krdc vnc://127.0.0.1:5901`, this PC only);
  `scripts/qemu/shot.sh out/qemu/native` writes a PNG of it through QEMU's monitor.
- Run 6 (4 vCPUs, the store's third start): thermalmonitord, then backboardd (`ONEIOS-GUEST stage=backboardd-up
  t=208`) open the framebuffer; 15 minutes after launchd, `oneios-display: swap 1 accepted`, `the compositor swaps
  format 'BGRA' ... 828x1792 ... taken`, `frame 1 scanned out through virtio-gpu`: **the Apple logo on the screen**, drawn
  by backboardd with the CPU (`logs/native-qemu/2026-11-20-run6-apple-logo.png`). SpringBoard runs (pid 34) and had
  not drawn after 12 more minutes; chronod aborted once (EXC_CRASH). Ended at 40 minutes by a mistake of the bench's
  (killing the `timeout` that holds QEMU kills QEMU too: give a long timeout from the start).

- Run 7 (6 vCPUs, a 3 h limit, stopped by hand after 1 h 50 min): launchd 13 minutes after the start, backboardd at
  t=271 s, the logo 18 minutes after launchd, and nothing else for an hour while all six vCPUs were 65-80 % busy. iOS's
  own log, read from a copy of the store (`oneios-tool ioslog health` / `show --proc`, no mount): no hang, no watchdog,
  no jetsam, one crash (chronod); SpringBoard took the display (`FBSDisplayConfiguration ... Main; mode: "414x896@2x
  60Hz sRGB SDR"`) and bootstrapped PosterBoard, then went quiet; and **rapportd logged 1.54 million lines in 14 minutes**
  (`DiscoveryCache: After pruning 0 records, 0 records remain cached`, between Bonjour browses that mDNSResponder never
  answers: `kDNSServiceErr_Timeout`). Without a network interface rapportd spins and logd_helper with it. The bench now
  turns off rapportd and analyticsd with the phone's four (`oneios-disable-jobs=`, hosted 0053); the lasting answer is
  a network on the bench.
- Touch and keys: `xnuloader-qemu --input tablet,keyboard` (boot-vm.sh `INPUT=tablet,keyboard`) puts QEMU's
  virtio-tablet-device and virtio-keyboard-device on the transports after the GPU as `/arm-io/input-N`; Droid-VM's
  virtio-input transport takes an absolute pointer as one finger while its button is down (the VNC client's mouse), and
  the native kernel links the hosted `hid` archive. Run 8: `droidvm input /arm-io/input-0: QEMU Virtio Tablet, pointer`,
  `input-1: QEMU Virtio Keyboard, keyboard`, `touch screen: Digitizer/Touch Screen, Built-In, DisplayIntegrated published
  on the event service`, both event drivers attached.
- The CPU report runs without a GPU now: `oneios-cpu: 10 s: kernel_task[0] 298%, launchd[1] 6%, ...` (the kernel's
  threads hashing the shared cache for launchd's dyld).
- **Run 8 (rapportd and analyticsd off, touch and keys): the lock screen.** launchd 11 minutes after the start, the
  logo, and 18 minutes after launchd **iOS 27's lock screen** (the date and clock, the battery, the home indicator; the
  date is 31 December 1969 in Pacific time: no RTC), drawn by SpringBoard with the CPU
  (`logs/native-qemu/2026-11-20-run8-lock-screen.png`). `oneios-cpu` then: iconservicesagent, chronod, backboardd and
  SpringBoard. A swipe sent as VNC pointer events reached backboardd (the touch screen's event service has one user
  client, backboardd) and the lock screen answered with **"Swipe up to open"**; the unlock itself did not happen within
  the run: the clock went away and the screen stayed on the date, with backboardd and BackgroundShortcutRunner taking
  most of the six vCPUs and spindump running (an hour under QEMU is a few minutes of a phone). The run was stopped at
  2 h 25 min; its unified log was not on the store yet (logd had not written it out).
- Touching from a script: boot-vm.sh gives QEMU a QMP socket too (`qmp.sock`), and `scripts/qemu/touch.sh OUT tap X Y`
  / `swipe X1 Y1 X2 Y2 [MS]` sends the tablet's absolute events through it (QEMU's monitor has only relative ones).

## A VM under Gunyah (2026-11-20)
What the S25 says (read only, `docs/SM8750_PORT.md` section 0b): protected VMs only, `/dev/gunyah`, AVF's crosvm 0.1.0
with `--hypervisor`, `--protected-vm-without-firmware` and `--swiotlb`, its virtio devices on PCI. What others ran on
Gunyah [F, their reports]:
- OnePlus 13 (8 Elite, kernel 6.6.126, unlocked): Debian and Windows guests under Droid-VM's crosvm build,
  `--hypervisor gunyah --protected-vm-without-firmware --disable-sandbox`, virtio over PCI, a guest kernel with
  `CONFIG_DMA_RESTRICTED_POOL` (the host reads no guest RAM outside the swiotlb pool), 2 MiB huge pages reserved by a
  module against Android's fragmentation. AVF's own crosvm with a Debian kernel put that phone into Qualcomm's crash
  dump mode (a forced restart). https://blog.lyc8503.net/en/post/android-virtualization-on-snapdragon/
- Lenovo Legion Y700 gen 4 (SM8750, Android 15, unlocked): Debian under crosvm with `--protected-vm-without-firmware
  --swiotlb 64` after `ulimit -l unlimited`. https://github.com/polygraphene/gunyah-on-sd-guide
- Droid-VM's UniXNU guest (`external/UniXNU-droidvm/README.md`, "State"): XNU to the lock and Home screen under crosvm
  on Gunyah on SM8650 and SM8850, 4 KiB pages, devices on virtio-mmio (Droid-VM's crosvm branch `wip/virtio-mmio`),
  `--protected-vm-pseudo-unprotected`, which needs Droid-VM's host modules (`docs/plan.md` section 8). Phone resets
  seen there: a vCPU stuck in the hypervisor with a device the guest did not know; a guest stuck in WFE killed from
  the host (`tools/rig/pacprobe/README.md`).

None of them is a Samsung: whether Samsung's Gunyah takes a VM made from root's shell is unknown [U]; the bootloader
unlock of those phones is not what Gunyah asks for (root and `/dev/gunyah` are).

What it would change: the bench's native kernel is already a guest of that shape (GICv3, PSCI, Droid-VM's virtio-mmio
layout). Under crosvm it needs PSCI by HVC, the 19.2 MHz timer, every virtio buffer in the swiotlb pool (a protected
VM's host sees nothing else), and either a virtio-pci transport (AVF's crosvm) or Droid-VM's crosvm (BSD-3, built for
Android on the PC). The disk is the image set's files on /data served as virtio-blk, as on the bench: no UFS driver,
no RAM disk, nothing written to UFS by XNU. One UI stays up under the VM, and the way back is `crosvm stop`, no
reboot. The risk is the others': a guest that hangs in the hypervisor can reset the phone (One UI comes back, without
the temporary root).

The first test: a payload of a few instructions (Droid-VM's pacprobe is the model) under AVF's crosvm, 1 vCPU, 128
MiB, no device but the serial port: it prints a line on the 8250 at 0x3f8, reads the ID registers (TGran16, VARange,
PAC), never waits in WFE and powers off by PSCI by itself; a run still up after 20 s is stopped through
`crosvm stop`. It answers whether Samsung's Gunyah runs our VM at all.

**Run 1, 2026-11-20: it does.** `oneios-tool phone vm-probe` (`runtime/vmprobe/probe.S`, AVF's crosvm with
`--hypervisor gunyah --protected-vm-without-firmware`, 1 vCPU, 128 MiB; `logs/pa3q-vmprobe/run1/`): crosvm made the
VM ("creating hypervisor: Gunyah"), the payload ran and printed on the 8250; the 2^28-turn loop with the MMU off did
not end in 20 s, `crosvm stop` ended the VM (crosvm exit 0) and One UI ran on. What the vCPU says:
- EL1 (`CurrentEL` 4); `MIDR_EL1` `0x000f0480` (Gunyah's, not Oryon's), `MPIDR_EL1` `0x80000000`; PSCI 1.1 over HVC;
  `CNTFRQ_EL0` 19.2 MHz; the device tree at `0x87600000` (RAM from `0x80000000`), its magic right.
- `ID_AA64MMFR0_EL1` `0x100010000f001021`: 36-bit IPA, 16-bit ASIDs, **4 KiB granule only** (no 16 KiB, no 64 KiB).
  `ID_AA64MMFR2_EL1` `0x1220001112101011`: **VARange 0, no FEAT_LVA** (the bench's `vm_sanitize.c:643` assertion came
  from QEMU's `max` CPU, not from this core). `ID_AA64MMFR1_EL1` `0x1000111110312122`: PAN 3, VHE, HAFDBS 2.
- `ID_AA64ISAR1_EL1` `0x0011111101211052`: **pointer authentication, APA 5 (QARMA5 with FPACCOMBINE) and GPA 1**, LRCPC
  2, BF16, I8MM; `ID_AA64PFR1_EL1` `0x21`: BTI, SSBS 2; `ID_AA64PFR0_EL1` `0x1101000001111111`: FP16, GIC system
  registers, no SVE. `CTR_EL0` `0xb444c004`: 64-byte lines, IDC and DIC (no cache maintenance between data and code).
- `SCTLR_EL1` at entry `0x30d00980` (MMU, caches off).
The probe since run 1 times 2^20 turns with the MMU off, then turns the MMU on (an identity map, write-back RAM) and
times 2^28 turns, before it powers off: the vCPU's own speed, and whether PSCI's SYSTEM_OFF ends a VM on Gunyah.

**Run 2, 2026-11-23: the whole probe, and the VM ends by itself** (`logs/pa3q-vmprobe/run2/`): crosvm's run took about
a second, PSCI SYSTEM_OFF ended it (crosvm exit 0, no stop needed), One UI ran on. `SCTLR_EL1.I` is set when the
probe writes it (`0x30d01980`), yet with the MMU off a turn of the loop costs 105 ns (2^20 turns in 2,110,484 ticks of
19.2 MHz): every fetch goes to memory, which is why run 1's 2^28 turns could not end in 20 s. With the MMU on
(`0x30d01985`: M, C, I) 2^28 turns take 1,800,312 ticks, 93.8 ms: 0.35 ns a turn, 2.86 billion turns a second, the core's
own speed. Code that stays in the VM loses nothing to Gunyah.

Since run 2 the probe also reads `/memory` from the device tree, maps all of it, writes every 4 KiB page above itself
(but the tree's blocks) with its own address and reads it back (`MEM_PAGES`, `MEM_BAD_PAGES`, the ticks of each pass,
`MEM_AT` every 256 MiB): how much RAM a protected VM can have while One UI runs, and what lending it costs. `vm-probe`
keeps crosvm's tree (`--dump-device-tree-blob`, `fdt.dtb` in DIR: the machine an XNU guest gets) and refuses a VM
that would leave Android less than 1 GiB of its MemAvailable (the VM's RAM is pinned).

**Runs 3 and 4, 2026-11-23: 128 MiB yes, 1 GiB no** (`logs/pa3q-vmprobe/run3-128m/`, `run4-1g/`). Run 3: `/memory`
`0x80000000` + 128 MiB, the image at `0x80000000`; 31,744 pages written and read back, none bad, the whole run about
2 s; crosvm's tree kept (`fdt.dtb`, 4,251 bytes). The first write of a page costs 16.7 us and a read after it 9 ns:
Gunyah's demand paging gives the guest each page when it first touches it (`drivers/virt/gunyah/vm_mgr_mem.c`
`gunyah_gup_demand_page`: the page pinned, Qualcomm's platform hook assigns it by an SCM call, then donated and
mapped, one folio at a time). Run 4: the guest wrote past `0x90000000` (256 MiB), then crosvm logged "unknown gh exit
reason: 3" and "vcpu crashed": 3 is `GUNYAH_VCPU_EXIT_PAGE_FAULT`, the kernel could not give a page and hands the fault
to the VMM (`page_fault.attempt` is its error), which AVF's crosvm does not handle. crosvm ended by itself, One UI ran
on. The kernel's own line ("Failed to provide folio for guest addr: ...") had left the phone's log, which keeps
minutes (Wi-Fi's lines): `vm-probe` now keeps the kernel's lines of each run (`dmesg.txt`), and `phone dmesg` reads it.

Why 256 MiB [I]: that is 65,536 pages of 4 KiB, and they are 4 KiB because of the phone's transparent huge pages
(`phone hw` `thp.txt`): `enabled` always, `defrag` never, **`shmem_enabled` never**. crosvm's guest RAM is a memfd
(shmem), so `--hugepages` (MADV_HUGEPAGE) gets no 2 MiB folio; with them the same RAM takes 512 times fewer demand
paging calls (and SCM calls). buddyinfo had 11 free blocks of 2 MiB and 76 of 4 MiB: about 320 MiB without
compaction. `vm-probe --thp` sets `shmem_enabled` advise and `defrag` madvise for the run only and writes the old
values back after it (read first and checked against the words the kernel offers, a trap for an early end; a
restart of the phone also brings the defaults back).

**Runs 5 to 7, 2026-11-23: the limit is Gunyah's own memory** (`logs/pa3q-vmprobe/run5-1g-confirm/`, `run6-1g-thp/`,
`run7-1g-thp-compact/`). Run 5 (1 GiB, as run 4) kept the kernel's lines: "gunyah_vm_mgr: Failed to map guest address
0x9d557000: 10" and "Failed to provide folio ...: -12". 10 is GUNYAH_ERROR_NOMEM from the `addrspace_map` hypercall:
the hypervisor has no memory left for the VM's stage 2 and its records, after about 470 MiB of 4 KiB pages. Gunyah's
whole region is 18 MiB (`gunyah_hyp_region@80000000`, `0x1200000`) for all its VMs (One UI's, the trusted UI's,
Samsung's uH, ours). Run 6 (`--thp`, the settings set and written back as they should): to `0xa1110000`; buddyinfo
had no free 2 MiB block before it, so the folios were 4 KiB again. Run 7 (`--thp` and one `compact_memory` first):
127 free 2 MiB blocks before the VM, to `0xa64a8000` (610 MiB). Each of runs 4 to 7 filled the hypervisor's memory
for a moment (crosvm ends at once and gives it back); One UI ran on each time, but it is not a thing to do often.

Why so few 2 MiB blocks (`phone hw`, `meminfo.txt`, `thp.txt` with pagetypeinfo): MemFree 0.99 GB of 11.4 GB; the file
cache holds 3.6 GB (2.6 GB of it inactive), apps 2.5 GB, the kernel's slabs, stacks, page tables and vmalloc 1.7 GB.
Of the 5,736 pageblocks of 2 MiB, 3,722 are Movable (7.3 GB), 1,581 Unmovable (3.1 GB), 312 CMA, 121 Reclaimable.
Compaction moves pages but frees none: a VM of iOS's size (3 to 4 GiB) in 2 MiB folios needs that much memory freed
first (the file cache dropped, then compacted) and taken at once (a guest that touches all its RAM at boot), at every
start, since a temporary root cannot reserve huge pages at boot (Droid-VM's `gh_hugepage_reserve` does, from a
module loaded at boot; lyc8503 hit the same wall at 1 GB without it).

**The file cache dropped, 2026-11-23: no** (`oneios-tool phone hugeblocks --free`: a sync, `drop_caches` 3, one
`compact_memory`; no VM). First try: MemFree 505 MiB to 2,902 MiB in 2.4 s, Cached 3,872 to 1,603 MiB, free 2 MiB
blocks 0 to 124 (248 MiB). Second try: MemFree 1,461 to 2,851 MiB, 2 MiB blocks 0 to 26 (52 MiB); by order (4 KiB <<
order) `64331 53559 29200 13048 7123 2764 948 289 93 22 2`: the freed memory is in pieces of 4 KiB to 1 MiB, and
compaction does not join them (most Movable pageblocks hold pages it cannot move). So at run time this phone cannot
give a VM gigabytes of 2 MiB blocks, and in 4 KiB pages Gunyah's 18 MiB stops it near 500 MiB.

**Run 8, 2026-11-23: a bigger VM does not get further** (`logs/pa3q-vmprobe/run8-2g/`, 2 GiB, as run 5): "Failed to
map guest address 0x9632b000: 10", at about 350 MiB, before run 5's 470 MiB of a 1 GiB VM. The limit is the
hypervisor's, not the VM's.

**Verdict for this phone: a VM under Gunyah is not a way to run iOS** [F]. Its vCPUs run at the core's own speed, but
its RAM ends near 0.5 GB: Gunyah lends it in 4 KiB pages from a hypervisor heap of 18 MiB for all its VMs, the 2 MiB
blocks that would take 512 times less of it are not there at run time (52 to 248 MiB of them, even with the file
cache dropped and the memory compacted), and with a temporary root nothing can reserve them at boot (Droid-VM's way:
an unlocked bootloader or a lasting root). iOS needs 3 to 4 GB. The probe, `vm-probe`, `hugeblocks` and `dmesg` stay
for another phone or another firmware.

### gh_hugepage_reserve with One UI stopped (2026-11-23)
The owner's idea: the module of Droid-VM made for this wall (https://github.com/Droid-VM/gh-hugepage-reserve,
GPL-2.0), and One UI's framework stopped and started again around its acquire, as the iOS switch and return do, so
that the RAM apps and system_server held is free without a restart of the phone (and of its temporary root). Its
README measured the same wall ("84% of all 2MB windows hold an unmovable hard straggler" after uptime); its tests
ran in QEMU, the on-device ones are its open work. Built with `scripts/hugepage/build.sh` (the authors' DDK image
for android15-6.6 by digest, under bubblewrap); `oneios-tool phone hugepool` loads it and runs the cycle.
- Its ABI preflight on the phone's BTF (with the phone's kernel version: run with the PC's 7.0.10 it picks the
  wrong rows) passes every function but `kallsyms_lookup_name`, a false alarm: its argument is plain `char`
  (unsigned since Linux 6.2's `-funsigned-char`, encoding 0 in BTF, the same as `strlen`'s), which the preflight
  prints as `uchar`. It stays in `disable_kapi` anyway (it only turns off a CMA lever this use leaves off).
- Load 1 (One UI up, `pool_want=64`): insmod in 0.11 s, every hook attached, "RAM: 22 ranges, 11450 MB
  present", `pool_size_max` 2678; the pool stayed at 0 (no free 2 MiB block beside One UI). One UI ran on.
- **Crash 1**: `acquire 1` beside One UI panicked the kernel in the module's worker ("Unable to handle kernel
  NULL pointer dereference", `gh_pool_ext_to_avail+0x7c4`, `gh_adjust_fn`; `/proc/last_kmsg`, kept in
  `logs/pa3q-hugepool/crash1-pstore.txt`); Samsung's upload path restarted the phone in about 20 s, One UI came back,
  nothing was lost. The cause is ours: our insmod line had no `pageblock_order_val` (the module's own load.sh always
  passes the one of `/proc/pagetypeinfo`), so the module took -1 as its block order and its block size S as
  `1u << (-1 - 9)` (4 Mi slots on arm64): `gh_try_complete_block` walked siblings from slot 0 into a GB with no RAM
  (slot 1024, 4 GiB) and read a NULL slot. `hugepool load` now passes `pageblock_order_val` (refusing a phone
  whose order is not 9), `migrate_cma_val=-1` and `pool_want_with_cma=0` (the CMA side off). The module's own
  fragility (no check of that value, no NULL check there) is the authors' to hear about.
- Load 2 (`pageblock_order_val=9`, `pb_order=9` in its line), then `acquire 1` beside One UI: 11 pages of 2 MiB,
  "migration exhausted", no crash.
- **Grow 1, the owner's cycle** (`hugepool grow --want 1536 --mode 3 --limit 180`; `logs/pa3q-hugepool/grow1/`,
  the phone's `cycle.log`): One UI's framework stopped at 02:16:41 (MemFree 0.66 GB to 2.15 GB), the file cache
  dropped (MemFree 9.14 GB), the module's acquire took 680, 1,086, 1,401 and then **1,536 pages of 2 MiB (3 GiB),
  "reached target", in 20 s**; One UI started again at 02:17:03, system_server at 02:17:06, user 0 unlocked
  after 13 s: One UI was down about 37 s, and **the phone did not restart** (the temporary root stays). Samsung's
  `/dev/watchdog` refused the feeder's first writes ("Device or resource busy") while system_server went; One UI
  was back long before its 100 s. The tool gave up on the start's adb status 255 (adb goes with the framework)
  while the cycle ran to its end on the phone: the start now counts as started when the phone said so.
- **Run 9, 1 GiB VM** (`vm-probe --mem 1024 --thp`, `logs/pa3q-vmprobe/run9-1g-pool/`): the whole probe, **261,120
  pages written and read back, none bad**, in 0.70 s of first touches (2.7 us a page on average against 16.7 us in
  4 KiB pages), ended by itself; `total_served` 512 (the VM's RAM came from the pool) and the pool full again after.
- **Run 10, 3 GiB VM** (`--mem 3072 --thp`, `run10-3g-pool/`): **785,408 pages (3,068 MiB) written and read back,
  none bad**, 0.89 s of first touches, ended by itself; Android's MemAvailable the same before and after (2.84 and
  2.88 GB). `vm-probe` counts the pool beside MemAvailable for its 1 GiB floor.

**Verdict, revised: a VM of iOS's size fits on this phone** [F], with gh_hugepage_reserve loaded after the
temporary root and its pool grown once per boot with One UI's framework stopped for under a minute. Its vCPU runs at
the core's speed, PSCI ends it, 3 GiB of RAM are there. crosvm's machine for it (`fdt.dtb` of run 10): "linux,dummy-
virt", RAM at `0x80000000`, GICv3 (GICD `0x3fff0000`, GICR `0x3ffd0000`), the arch timer, four ns16550a (`0x3f8`,
`0x2f8`, `0x3e8`, `0x2e8`), PSCI by HVC, a PL030 RTC at `0x2000`, a vCPU stall detector at `0x3000`
("qemu,vcpu-stall-detector": a guest must pet it), virtio over PCI (`pci-host-cam-generic`, CAM `0x10000`, BARs
assigned, `linux,pci-probe-only`) whose DMA goes through an 8 MiB `restricted-dma-pool` at `0x1_3f800000` (the host
reads no other guest RAM: protected VM).

### The native kernel in crosvm (2026-11-23)
Step 1 of the road ("Next" 1): the bench's native kernel as the payload of AVF's crosvm under the S25's Gunyah, with
its console and nothing else.
- Kernel patch 0077 (native only): crosvm's serial ports are ns16550a (8250, byte-spaced registers) at `0x3f8`,
  `0x2f8`, `0x3e8` and `0x2e8`, and AVF's crosvm has no PL011: `pe_serial.c` gets a polled driver for
  `"ns16550a"`, whose node's `reg` is an absolute physical address (the ports sit below the arm-io window, which may
  not start at 0).
- The loader: `runtime/loader/tools/xnuloader-crosvm.c` (`xnuloader-crosvm KERNEL OUT [--cpus N] [--ram-mib N]
  [--timebase HZ] [--args TEXT] [--args-add TEXT]`) writes one arm64 Linux Image, which crosvm loads at the start of
  the VM's RAM (`0x80000000`) and enters at EL1 with the MMU off: the Linux header; at `0x40` xnuloader-qemu's stub
  (interrupts masked, x0 = `boot_args`, a branch to the entry); at `0x100` a label `oneios-crosvm ram-mib=N cpus=N`
  (the VM the window was cut for, which `vm-boot` starts crosvm with); the kernel's window (`xl_boot_place`: the
  segments, the device tree, `boot_args`) at RAM + 32 MiB + the kernel's base modulo 32 MiB, ending 16 MiB before the
  RAM's end (crosvm's FDT and its 8 MiB restricted DMA pool sit there). The device tree is crosvm's machine as run
  10's `fdt.dtb` describes it: GICD `0x3fff0000` and the redistributors right below (`0x20000` a vCPU), the 19.2 MHz
  timer, PSCI by HVC, the console on the first ns16550a; arm-io from `0x1000` to 1 GiB. It also writes `OUT.adt`
  (that tree, for `xnuloader-inspect adt`). Boot 1 had no device; the disks came with step 2 (below), and with them
  the label's `swiotlb-mib=N` and the window's end at the pool's MiB + 8 MiB before the RAM's end.
- The run: `oneios-tool phone vm-boot --image FILE [--seconds N] [--thp] [--out DIR]` (`tools/oneios-tool/README.md`):
  vm-probe's crosvm line with the label's RAM and vCPUs, the console followed live, `crosvm stop` after N seconds or
  at `vm-boot --stop`.

**Boot 1, 2026-11-23: the kernel runs in the VM** (`logs/pa3q-vmboot/boot1/`, local). `xnuloader-crosvm
out/kernel/native-24A446/kernel.kc out/crosvm/kernel.img --ram-mib 2048 --cpus 1`: the native collection (MH_FILESET
arm64e, base `0xfffffff007004000`), a 137 MiB Image, the window at PA `0x83004000` (1,982 MiB of the 2,048, 89 MiB
used), `boot_args` at `0x88950000`, the tree at `0x88951000` (11,624 bytes). `phone vm-boot --seconds 90 --thp` with
gh_hugepage_reserve's pool loaded: crosvm made the VM ("creating hypervisor: Gunyah"), and the console showed the
native kernel's whole start (`Darwin Kernel Version 25.5.0 ... DEVELOPMENT_ARM64_BCM2837`): `vm_page_bootstrap:
476905 free pages, 30487 wired pages` (the 2 GiB), the zones, AppleImage4, AMFI ("Booted in a VM"), Sandbox, the
CHRP NVRAM, APFS, the hosted storage class, the software key store, the display's framebuffer backend, the HID
devices (buttons, touch screen, light sensor, each under IOHIDResource with its event driver) and `oneios-cpu`'s
report every 10 s (the timer's interrupts arrive through the GIC). It stopped where it had to: `Waiting on ...
IOMedia ... Apple_HFS`, the root device's wait, since this VM has no disk. At 90 s `crosvm stop` ended it (crosvm
exit 0); One UI ran on. crosvm's vCPU stall detector did not end the VM, which never armed it (its driver is opt-in
in the guest [I]). Pointer authentication works in the VM too (vmprobe run 11, `logs/pa3q-vmprobe/run11-pac/`:
the keys written, PACIA and AUTIA round trip, PACGA, and the wrong modifier traps with FPAC, ESR `0x72000000`).
What it does not show yet: the disks, more than one vCPU, the display and input.

### The image set's disks in the crosvm VM (step 2, 2026-11-23)
AVF's crosvm has its virtio devices on PCI only (`pci-host-cam-generic`, CAM at `0x10000`, BARs assigned by crosvm,
`linux,pci-probe-only`), and in a protected VM its host reads and writes **only the restricted DMA pool**: crosvm
lends the VM's RAM to it and shares the pool (`hypervisor/src/gunyah/mod.rs`: `GUNYAH_MEM_FORCE_LEND` for the guest's
memory, `GUNYAH_MEM_FORCE_SHARE` for `StaticSwiotlbRegion`), so a device given any other guest address makes crosvm
touch memory its host no longer has (lyc8503's Debian kernel without a restricted DMA pool put a phone into
Qualcomm's crash dump mode that way). The pool is the RAM's last `--swiotlb` MiB (`aarch64/src/lib.rs`
`get_swiotlb_addr`), as every tree the S25 made shows: `restricted_dma_reserved@87800000` for 128 MiB, `bf800000` for
1 GiB, `ff800000` for 2 GiB, `13f800000` for 3 GiB. (crosvm's source read: AOSP `platform/external/crosvm`.)
- The kernel's disk driver (`storage/OneiOSHostedDisk.cpp`, native only; `kernel/patches/oneios-hosted/README.md`
  "0076"): a node with compatible `oneios,virtio-pci` is a modern virtio-blk PCI function (`1af4:1042`), its `reg` the
  function's configuration space. The driver walks its capabilities (common configuration, notification, ISR, device
  configuration: VirtIO 1.2 section 4.1), maps their BAR (as the VMM assigned it, else placed in the node's
  `oneios,pci-bar-window`, which only QEMU needs), turns on memory decoding and bus mastering, and drives the same one
  polled queue of 8 as on virtio-mmio, accepting `VIRTIO_F_ACCESS_PLATFORM` (crosvm offers it to a protected VM).
  Everything the device touches (the queue page, the request header, the status byte and the 1 MiB transfer buffer)
  lies in the node's `oneios,dma-window`, mapped write-back (`VM_WIMG_COPYBACK`, as the host maps the pool; a device
  mapping would not be coherent with it), and the driver refuses any address outside it. The registers are reached
  with single loads and stores of one register (inline assembly): Gunyah emulates an access from its syndrome, which
  a load or store pair or one with writeback does not give.
- The loader: `xnuloader-crosvm ... --image DIR [--phone-image DIR] [--phone-data FILE] [--swiotlb MIB]`
  (`docs/booter.md`): the image set's five disks (`tools/image_disks.c`, shared with xnuloader-qemu) as PCI functions
  00:02.0 to 00:06.0 (os, systemos, app, the Data seed, the Data store), each with its identity, its hash list and a
  1 MiB + 64 KiB slice of the pool as its DMA window; the trust caches and root matching as on the bench; the label
  `oneios-crosvm ram-mib=N cpus=N swiotlb-mib=N`; and `OUT.disks` for vm-boot: the pool's address, the store, and
  crosvm's `--block` values (`path=...,ro=true,id=os,pci-address=00:02.0`, ...). The files are the staged set on the
  phone (`/data/local/tmp/oneios/image/`, read-only); the Data store is the VM's own
  (`/data/local/tmp/oneios-vmboot/data.img`, sparse at the manifest's 16 GiB), never the hosted session's.
- The run: `phone vm-boot` reads `FILE.disks`, refuses a list whose pool is not the label's, checks on the phone
  that each read-only file has the manifest's size, makes the store when it is missing and wants 4 GiB free on
  /data; crosvm gets `--swiotlb` from the label and the `--block` values. The moment crosvm has written its tree,
  the script checks `restricted_dma_reserved@ADDR` in it and stops the VM if the pool is elsewhere: crosvm writes the
  tree before the vCPU starts, and the kernel's disks start seconds later.
- The bench: `xnuloader-qemu --pci` (`scripts/qemu/boot-vm.sh` `PCI=1`) gives QEMU's virtio-blk-pci the same nodes
  (virt's ECAM at `0x3f000000`, a BAR window each, the pool after the boot video).

**Bench, 2026-11-23 (`PCI=1`, the native collection of this kernel, `out/qemu/pci`):** the five functions found
(`PCI 1af4:1042, BAR 4 at 0x10000000 ...`, each DMA window in the pool after the boot video), the three images
verified chunk by chunk, the Data store writable; "Got boot device = .../virtio-pci-0@3F00F000/OneiOSHostedDisk/...
Rave24A446.V54OS@1", BSD root, launchd. Stopped there: it had shown the path.

**Boot 2 on the S25, 2026-11-23: iOS's launchd and its boot in the VM** (`phone vm-boot --image
out/crosvm/kernel-disks.img --seconds 900 --thp`, `logs/pa3q-vmboot/boot2/`): `xnuloader-crosvm ... --ram-mib 2880
--cpus 1 --args-add oneios-data-wait=1800 --image <the set>` (window 2814 MiB, the pool at `0x133800000`), the RAM from
gh_hugepage_reserve's pool (1,405 pages of 2 MiB). The script found crosvm's tree with `restricted_dma_reserved@133800000`
as the disks' windows. The five functions at crosvm's BARs (`BAR 0 at 0x2000000` ... `0x2020000`), os, systemos and app
verified, the seed, the new store (16 GiB, sparse, the VM's own); "Got boot device = .../virtio-pci-0@10000/
OneiOSHostedDisk/... Rave24A446.V54OS@1", BSD root `disk7s1`, launchd ("hello, launchd"), the seed unpacked into
/private/var, **all 60 boot tasks**, the daemons (wifid, securityd, cfprefsd, backboardd, dasd, logd and the rest),
and **SpringBoard about 30 s into the CPU reports**: on one vCPU, against about 75 s on the phone's hosted path and
hours on QEMU. SpringBoard then ends with status 0x5 and starts again (9 times in 900 s, "Too many corpses"): the VM
has no display ("oneios-display: panel 0x0 at 0 (0 bytes); virtio scanout state 0"), as the bench had none before its
virtio-gpu. AccessibilityUIServer turns at 99% of the vCPU meanwhile. `crosvm stop` ended the VM at 900 s; One UI ran
on all along. The tool waited at the VM's start for the whole run (root's `sh -s` waited for its `( ... ) &` child):
`detached_start` (adb.rs) now starts the VM's and the pool's scripts with a double fork.

### The screen in the crosvm VM (step 3, 2026-11-23)
A protected VM's host can read nothing of it but the restricted DMA pool, so the screen lies there, and the host
reads it out of crosvm's own mapping of the pool. crosvm maps the pool apart from the RAM (a short VM before boot 3,
`logs/pa3q-vmboot/maps1`: `6e50600000-6e50e00000 rw-s 7f800000 ... /memfd:crosvm_guest (deleted)`, the memfd's last 8
MiB, beside `6e50e00000-6ed0600000 rw-s 00000000`, its 2040 MiB of lent RAM).
- The loader: `xnuloader-crosvm --display 1440x3120` (with `--image`) puts the boot video (B,G,R,X, 5760 bytes a row,
  17,971,200 bytes) at the pool's start, a 16 KiB page for the frame count right after it (`+0x1124000`), the disks'
  windows from the next 2 MiB (`+0x1200000`); the pool is then 32 MiB unless `--swiotlb` says more. It adds
  `offline-legacy-unit-scale=3 sheng-vsync=60 oneios-frame-seq=PA` to the boot arguments and `fb OFFSET BYTES WIDTH
  HEIGHT STRIDE SEQ` to `OUT.disks`.
- The kernel, native only: NativeLegacyDisplay and NativeGOPPresenter map the boot video write-back
  (`staged/display.rs` LEGACY_BOOT_VIDEO, GOP_BOOT_VIDEO; the recorded IOKit compile line the display archive is built
  with carries `-DXNU_ONEIOS_NATIVE=1`), as the disk driver maps its windows (`VM_WIMG_COPYBACK`): the host reads the
  pool through a write-back mapping of the memfd, and with the guest's uncached mapping it would read stale lines (and
  the guest's copy into Device memory costs 100+ ms a frame). After every frame written into the boot video (the
  probe's transfer hook, `oneios_display_frame_done` in `OneiOSDisplay.cpp`) the count in that page goes up: "OFSQ"
  and version 1, then a 64-bit count stored with release order. Without `oneios-frame-seq` nothing is mapped.
- The bridge on the phone, `displayctl pool` (`runtime/display/README.md`, "The screen of a VM of crosvm"): finds that
  mapping in `/proc/PID/maps` (exactly the pool's offset and size), opens its memfd through `/proc/PID/map_files`, maps
  only the frame and the count's page, read-only, reads only resident pages (`mincore`), copies the rows that changed
  when the count moves (four times a second before there is a count: the kernel's console) and serves them to the
  staged display helper, a layer over One UI.
- `phone vm-boot`: for an Image with an `fb` line it pushes the bridge, the script starts it right after the pool
  check and ends it after the VM.

**Boot 3 on the S25, 2026-11-23: the lock screen on the phone's panel** (`phone vm-boot --image
out/crosvm/kernel-display.img --seconds 900 --thp`, `logs/pa3q-vmboot/boot3/`): `xnuloader-crosvm ... --ram-mib 2880
--cpus 1 --args-add oneios-data-wait=1800 --image <the set> --display 1440x3120`, the pool (32 MiB) at `0x132000000`,
boot 2's store (the seed unpacked already). The bridge found crosvm's mapping of the pool (`0x7b57a00000-0x7b59a00000`,
offset `0xb2000000` of the 2880 MiB memfd) and the helper's layer was up seconds after crosvm started; then
backboardd's first swap ('BGRA', 1440x3120, stride 5888), "frame 1 copied to the boot video by the CPU", "the frame
sequence at 0x133124000", and on the phone "the guest counts its frames". **iOS's lock screen on the panel**
(`screen1.png`, `screen2.png`: "Thu Jan 1", the clock, "Swipe up to open"), its clock moving, and **SpringBoard up
for the whole run** (no "Too many corpses"; boot 2 without a screen: 9 starts in 900 s). The shimmer of "Swipe up to
open" keeps the guest at about 45 frames a second, about 69 changed rows each (280 s: 7,218 frames, 7,209 flushes,
473,868 rows): crosvm at 100% of its vCPU, the bridge at 14% of a core, the helper at 50% (its copy loop runs at
uclamp.min 1024, the hosted path's choice); the phone from 44 to 51 °C (thermal_zone0). The date is 1970: the VM's
clock is the phone's counter since it booted (10 h 57 min, shown as 2:57 in the guest's time zone, UTC-8); crosvm's
RTC is not in the VM's tree yet. `crosvm stop` at 900 s; the bridge: "the VM ended after 915.4 s: 33293 frames from
the guest, 431 scans, 33283 flushes (33283 acked), 2157250 rows copied" (36 frames a second over the run, 65 rows a
flush), the helper "layer removed" and exited 0, One UI's launcher in front again, 45 °C a minute later. One UI ran
on under the layer all along.

### Touch in the crosvm VM (step 4, 2026-11-23)
Boot 3's kernel had the touch screen's HID device and its event driver up ("oneios-hid: touch screen: event driver
IOHIDEventDriver attached") and its virtio-input transport found nothing to read ("virtio-input: 0 device(s) in the
device tree"): the transport (`DroidVMVirtioInput.c`, UniXNU-droidvm) knows virtio-mmio windows only, and keeps its
queues in the kernel's memory, which a protected VM's host cannot reach.
- crosvm (AVF's, `crosvm run --help`) has `--evdev PATH` ("The device will be grabbed (unusable from the host) and made
  available to the guest with the same configuration it shows on the host"), but cannot take the S25's touchscreen: it
  asks the device for its serial name (EVIOCGUNIQ) and `sec_touchscreen` has none (`U: Uniq=`): "failed to set up input
  device ... failed to get serial name of event device: No such file or directory" (boot 4's first start; crosvm ended at
  once, the phone was not touched). Its multi-touch device reads frames from a socket it connects to instead (`--input
  multi-touch[path=SOCKET,width=W,height=H,name=N]`, Droid-VM's app feeds it that way): runtime/display's `displayctl
  touch` listens there, takes the touchscreen (EVIOCGRAB) when crosvm connects, refuses a device that is not a direct
  multi-touch screen or has a side key, and passes each frame on as it is (width and height 4095: the touchscreen's own
  ranges). The S25's touchscreen is `sec_touchscreen` (`/proc/bus/input/devices`: `event8` this boot; 0..4095 on both
  axes, 10 slots, INPUT_PROP_DIRECT, no `MSC_TIMESTAMP`); the side keys are `gpio-keys` (`event0`), `pmic_pwrkey`
  (`event3`) and `pmic_resin` (`event4`), never taken.
- The kernel (native only, `staged/hid_transport.rs` HID_TRANSPORT_PCI; `kernel/patches/oneios-hosted/hid/README.md`):
  the transport finds crosvm's virtio-input functions on bus 0 of crosvm's configuration space (`/arm-io/input-pci`),
  maps their regions from the virtio capabilities, sends its MMIO register offsets there, keeps each device's three
  pages in a 64 KiB window of the pool (`oneios,dma-window`; crosvm is given no other address) and polls every 4 ms.
- The loader: `xnuloader-crosvm --display` adds the bus and its window (after the disks' windows) and `input touch` to
  `OUT.disks`.
- `phone vm-boot`: the script looks the touchscreen up by name, refuses it should it be a side key's device, starts the
  forwarder before crosvm and gives crosvm its socket; One UI under the layer gets no touches until crosvm ends (the
  forwarder lets the touchscreen go when crosvm's socket closes, or when it is ended). The way back without the PC:
  volume down held 3 s stops the VM (`getevent` reads the key; Android still gets it).

**Boot 4 on the S25, 2026-11-23** (`logs/pa3q-vmboot/boot4/`; the Image `--ram-mib 2400`: the pool had 1,203 free pages
of the 1,440 a 2880 MiB VM takes): the first start ended at crosvm's `--evdev` (above), the next ran 914 s. The forwarder
found `event8` (0..4095), crosvm connected and the touchscreen was the VM's; the kernel: "droidvm input PCI 00:01.0:
oneios-touch, touch, its queues in the pool at 0x115750000", "oneios-hid: virtio-input: 1 device(s) in the device tree,
1 started", backboardd the touch service's client. The lock screen came up, SpringBoard stayed up (35,720 frames), and
the forwarder gave the touchscreen back at the end ("the touchscreen is Android's again"); no finger touched the screen
in the run (0 events after the grab), so the forwarder got a test: `--swipe-test`, a swipe up from the bottom edge's
middle to the panel's middle 200 s after crosvm connected and every 60 s after.

**Boot 4b, 2026-11-23: iOS's Home screen** (`phone vm-boot --image out/crosvm/kernel-touch.img --seconds 600 --thp
--swipe-test`, `logs/pa3q-vmboot/boot4b/`). Its first start ended 48 s in: the pool had **198** free pages after boot 4
(and 492 "served" with no VM up; 1,203 free before it), Gunyah's heap ran out of 4 KiB mappings ("gunyah_vm_mgr: Failed
to map guest address 0x00000000f0580000: 10", "Failed to provide folio ...: -12") and crosvm's vCPU stopped at the fault
("unknown gh exit reason: 3"). `phone hugepool acquire` (beside One UI, nothing stopped) brought the pool back to 1,160;
`phone vm-boot` now refuses a VM of which more than 384 MiB would lie outside the pool's free pages (`check_pool`). The
next start: at the first test swipe the lock screen followed the finger up (`swipe1.png`), and at the second **the
Home screen** (`after4.png`: the Weather and Calendar widgets, FaceTime ... Settings, the dock). The kernel's log: "touch
screen: 142 virtio-input events, 44 frames, 88 finger sets, 86 HID reports (last result 0x0), 2 contacts begun (2 at
an edge); the last began at x 49.9% y 100.0%, the last lift was at x 49.9% y 50.0%", "86 HID events queued for the
service's client(s), 0 dropped". Every piece of the touch between crosvm and SpringBoard works; the touchscreen's own
frames through the forwarder are the one piece a finger still has to show.

**Boot 5 on the S25, 2026-11-23: the owner's finger** (`phone vm-boot --image out/crosvm/kernel-touch.img --seconds 900
--thp`, `logs/pa3q-vmboot/boot5/`; the Image `--ram-mib 2048`: `hugepool acquire` brought the pool from 680 free pages,
479 "served" with no VM up, to 992 and no further: "acquire_stop_reason=migration"). The forwarder read the
touchscreen from the first seconds ("touch screen: 808 virtio-input events, 238 frames, 221 HID reports ... 14 contacts
begun", while the kernel was still starting); then the lock screen, the unlock, **Settings opened by a tap**
(`finger3.png`, "Preferences[246]" in the guest's CPU report) and **Control Center pulled down over it**
(`finger4.png`): 6,088 events, 1,761 HID reports, 45 contacts in the first three minutes. The touch is done. The owner
ended the run at 485 s with volume down held 5 s ("touch: volume down held 5 s: the VM stops"): 11,996 touch frames
passed on, 207 contacts, 10,007 HID reports, SpringBoard up throughout, the touchscreen Android's again.

### Four vCPUs (step 5, 2026-11-23)
Nothing new was needed: `xnuloader-crosvm --cpus 4` gives `/cpus/cpu0..3` (reg = Aff0, as crosvm numbers its vCPUs),
four GICv3 redistributors right below the distributor (`0x3ff70000`, 0x20000 each, where crosvm puts them for four
vCPUs) and `/psci` with method `hvc`; the kernel's ArmCpu (`external/Droid-VM-UniXNU/drivers/ArmCpu`) reads that method
and starts each core with PSCI CPU_ON (0xc4000003) at the kernel's reset vector page, which Gunyah serves for crosvm's
protected VM.

**Boot 6 on the S25, 2026-11-23** (`--ram-mib 2048 --cpus 4`, `phone vm-boot --image out/crosvm/kernel-4cpu.img
--seconds 900 --thp`, `logs/pa3q-vmboot/boot6/`; the pool at 974 pages after `hugepool acquire`): the guest's CPU report
during the boot "(all 264%)" (several processes at once: more than one core); backboardd's first frame 96 s after the
start (100 to 110 s on one vCPU: the boot's early part is mostly one thread and the disks), and the owner **already in
the Home screen at 122 s**, the App Library at 136 s, the Phone app and its menu at 149 and 162 s (`strip.png`; on one
vCPU the lock screen itself came at about 170 s). crosvm at 144% of a host core while the owner used the Phone app,
the guest's report at 15 to 21% (of 400), 41.8 °C (thermal_zone0), SpringBoard without a restart.

### Less work for the screen, and the clock (step 6, 2026-11-23)
Where the screen's time went, measured on the S25 with `displayctl test` (the bouncing square, 60 frames a second at
1080x2340) and simpleperf on the helper: **85% of it in the CPU caches' maintenance** (`dcache_inval_poc` 36.0%,
`dcache_clean_poc` 31.5%, `arch_sync_dma_for_cpu` 9.0%, `arch_sync_dma_for_device` 8.7%), the copy itself 9.8%:
the window's buffers are the CPU's cached ones (Surface::lock asks for SW_READ_OFTEN and SW_WRITE_OFTEN) and each lock
and post maintains the whole buffer, whatever the damage (the lock 13.7 ms a frame with its wait, the copy 0.07 ms).
The helper's uclamp.min of 1024 only moved that onto the prime cores (52% of a core at 2 to 2.8 GHz, 68% at 1.8 to
2.2 GHz without it).
- The helper (runtime/display `jni.c`): with `ONEIOS_DISPLAY_PRESENT=ahb` the frames go into three AHardwareBuffers of
  its own that the CPU only writes (`CPU_WRITE_RARELY`: nothing to maintain), on a child of the layer (made by
  OneiOSDisplay.java, taken with `ASurfaceControl_fromJava`: `ASurfaceControl_createFromWindow` gives none for the BLAST
  window), posted with ASurfaceTransaction, each buffer given the damage of every frame since it was last filled, two
  transactions in flight at most, a buffer filled again after the compositor's release fence. Measured: 114 frames a
  second at 17.1% of a core against 60 at 49.5% (**0.15% a frame a second against 0.83%**), the picture right
  (`logs/pa3q-vmboot/ahb-test.png`). `ONEIOS_DISPLAY_UCLAMP_MIN` sets the copy loop's floor (0: none).
- `displayctl pool` starts its helper with both (`ONEIOS_DISPLAY_PRESENT=ahb`, `ONEIOS_DISPLAY_UCLAMP_MIN=0`), and
  `phone vm-boot` pushes the VM's own helper (`oneios-display`, its library and dex, root's) beside the bridge: the
  hosted session keeps its staged helper and the window path.
- A smaller frame: `xnuloader-crosvm --display 1080x2340 --scale 3` (the panel's aspect, 44% fewer pixels for the
  guest to draw on its CPU and for every copy; the compositor scales it to the panel).
- Boot 7 (1080x2340, 4 vCPUs, the helper without its floor, the window path; the pool grown first, One UI down 57 s):
  over 60 s the helper at 10.7% and the bridge at 1.3% of a core against boot 3's 50% and 14% (11 frames a second there
  against 45). Boot 8 (the same, write-only buffers, 1792 MiB: the pool had 784 pages after `acquire`): in use, the
  helper at **3.3%** and the bridge at **1.9%** of a core at about 10 frames a second; the guest itself at 266% (it
  draws with its CPU), the phone from 52.3 to 54.6 °C in 30 s while the owner typed. What is left of the heat is the
  guest's own drawing: the GPU's.
- **The clock** (kernel patch 0078, `kernel/patches/oneios-hosted/README.md`): the native kernel's getGMTTimeOfDay
  answered 0, and the calendar ran from 1970 plus the phone's counter since it booted. crosvm has a PL030 at `0x2000`
  (`rtc@2000`), whose first register reads the host's time in seconds; `xnuloader-crosvm` names it in `/chosen`
  (`oneios,rtc`) and the kernel reads it. Boot 8: "oneios-native: the real-time clock at 0x2000 (crosvm's PL030)", the
  lock screen's "Mon Nov 23". The time zone is iOS's own (Settings, General, Date & Time).

### The GPU's first question: can Android's GPU work in the VM's pool? (step 7, 2026-11-23)
What is left of the heat is the guest drawing with its CPU. The Adreno stays Android's (One UI needs it; its power,
clocks and GMU firmware are kgsl's), so the VM's GPU work would go to Android's kgsl, and its buffers must lie in
memory both sides keep: the SMMU's stage 2 keeps every device of the host's off the memory crosvm lends the VM, and
crosvm shares only its restricted DMA pool. `runtime/gpuprobe` (`make -C runtime/gpuprobe`; kgsl's UAPI header from
external/virglrenderer, Droid-VM's drm2kgsl) asks it: it maps a part of crosvm's own mapping of the pool the VM does not
use (as `displayctl pool` finds the pool), gives it to kgsl, has the GPU write a word there (CP_MEM_WRITE in an indirect
buffer of its own) and reads the word back.
- kgsl takes such memory by its user address only with a length: IOCTL_KGSL_GPUOBJ_IMPORT with
  KGSL_USER_MEM_TYPE_ADDR answered EINVAL, the legacy IOCTL_KGSL_MAP_USER_MEM (I/O-coherent, write-back, as drm2kgsl
  imports a guest's memory) gave a GPU address.
- Boot 8 (a pool of 32 MiB): its last 2 MiB mapped for the GPU, **the GPU's word in the VM's pool** ("the word at
  +0x1e00100 is 0xc0ffee42"), no SMMU or kgsl fault in the kernel's log.
- A larger pool: `xnuloader-crosvm --swiotlb` up to 1024 MiB now (`phone vm-boot` takes the label's). Boot 9 (1792 MiB,
  a pool of **128 MiB** at `0xe8000000`, 4 vCPUs): crosvm's tree put the pool where the Image wanted it, crosvm maps it
  for huge pages (`VmFlags ... hg`, `ShmemPmdMapped` growing as the VM uses it), iOS ran as before (Settings, dark mode;
  no restart, no jetsam), and 64 MiB of the pool mapped for the GPU took its word too.
So a GPU for the VM can work in shared memory: the guest's GPU buffers in a large pool, Android's kgsl given them by
their address in a host process, the GPU Android's throughout. The road (the hosted path has each piece): iOS's
Metal plug-in and the kernel's virtio-gpu client (DroidVMVirtioGpu.cpp, the gpu3d patches) speak virtio-gpu with
drm2kgsl's native context; the VM needs that device on crosvm's PCI (crosvm's own `--gpu backend=virglrenderer` with a
render server of Droid-VM's virglrenderer and drm2kgsl, or crosvm's `--vhost-user type=gpu` with xnuhost's virtio-gpu
device model in a process of its own), and the kernel's GPU buffers placed in the pool.

### The shared-RAM VM (step 8, 2026-11-23)
The GPU's buffers in the pool alone would not do: every IOSurface iOS draws into (Apple's IOSurface.kext allocates
them) and every one of Turnip's buffers would have to come out of it. **Droid-VM runs XNU with its GPU otherwise**
(external/UniXNU-droidvm, `tools/rig/unixnu-vm.template.json`: `"protected_vm": "pseudo_unprotected"`): its own crosvm
(https://github.com/Droid-VM/crosvm, BSD-3-Clause with Droid-VM's GPL-2.0-or-later additions) makes a protected VM
with a small lent region only (its boot shim and the device tree), shares the rest of the VM's RAM with it after it
started, as a Gunyah memparcel, through `/dev/gunyah_share`, and the shim (`droidvm-shim`) accepts it through the
resource manager's message queue before the payload runs. Android then reads and writes all of the VM's RAM, as
xnuhost does in the hosted path, so the GPU can work on iOS's own buffers wherever they are. `/dev/gunyah_share` is
Droid-VM's **gunyah_host_share** module (https://github.com/Droid-VM/gunyah_host_mod, GPL-2.0-or-later): the owner's
question of the morning (a module that shares memory with the VM) had an answer after all; this file said none was
published, which was wrong.
- **The module, for this phone.** `scripts/hugepage/build-share.sh` builds it from its source (gunyah_host_mod
  44e8ae5, `gunyah_host_share/GKI6.6`) in the DDK image scripts/hugepage/build.sh fetched, under bubblewrap.
  `oneios-tool phone hostshare load` checks it against the phone's kernel first and loads nothing on a difference:
  the BTF read on the PC (pahole, pfunct) for the three fields of the kernel's private `struct gunyah_vm` it reads
  (on the S25's 6.6.127-android15-8: `vmid` 0, `rm` 504, `vm_status` 536, the module's own offsets), the resource
  manager's parcel structs it builds (48, 4 and 16 bytes, every member where the module has it) and the six
  functions it calls through pointers (a different type there is a kCFI panic; all six alike), and `/proc/kallsyms`
  for the functions. Loaded beside One UI: `gunyah_share: loaded v6/dmabuf+gup`, `/dev/gunyah_share`.
- **Their crosvm.** Building it needs their AOSP tree (crosvm-minimal-manifest, tens of GB: no room on this PC's
  disks); the owner chose their prebuilt (DroidVM-Prebuilt-Root 82c59c2, 2026-09-12): `scripts/droidvm/fetch-crosvm.sh`
  fetches crosvm and the three libraries of its package the phone has not, each checked by SHA-256, and
  `oneios-tool phone droidvm-crosvm` pushes them (crosvm finds them beside itself, the rest in /system/lib64). It runs
  on the S25 (`crosvm run --help`: `--protected-vm-pseudo-unprotected`, `--gpu`, `--pre-alloc`; no `--virtio-mmio-*`
  in this build, so the devices stay on PCI).
- **Run shared1** (`vm-probe --droidvm DIR --shared-ram --thp --mem 128`, logs/pa3q-vmprobe/shared1): **Samsung's
  Gunyah takes it.** crosvm shared the window (122 MiB: 31232 pages in 1080 runs, handle 0x20); the shim found the
  resource manager's queues in the tree, accepted the parcel, pointed `/memory` at `0x80600000+0x7a00000` and jumped
  to the probe, which ran from the shared RAM (its 120 MiB memory test: 0 bad pages) and powered off by itself.
  gunyah_host_share took the parcel back when crosvm had gone (`reaper reclaimed parcel`). One UI ran on.
- **Run shared2** (1 GiB): crosvm prepares the window in large pages itself (its `mthp` pass: 58 of gh_hugepage_reserve's
  2 MiB pages, which was all the pool had, then 902 runs of 1 MiB), but at the share the window came in **126651 runs**
  for 260608 pages, and the resource manager refused to map it (`SHIM DIED: the resource manager refused MEM_ACCEPT
  err=0x9`, MAP_FAILED); crosvm was stopped at 30 s, One UI ran on. gunyah_host_share pins with FOLL_LONGTERM, and the
  kernel moves a page out of a CMA block before such a pin (the S25 has 624 MiB of CMA), into whatever 4 KiB pages are
  free. The 6.6 kernel has no shmem folios between 4 KiB and 2 MiB (no `hugepages-*kB/shmem_enabled`), so those 1 MiB
  were runs of small pages, not folios. Only the pool's pages stay where they are. **So the shared-RAM VM's size is
  the pool's, as the protected VM's was**: about 1000 pages for 2 GiB, 1800 for 3.5 GiB. After shared2 the pool had 2
  pages free and 56 `served` with no VM up (the losses of item 2 under Next).
- **For XNU:** `xnuloader-crosvm --shared-ram` makes the Image start at the window (`0x80600000`: the shim's 4 MiB and
  the handoff's 2 MiB before it); the kernel's own window stays 32 MiB in, the devices' windows stay where the pool
  was (now plain RAM; crosvm gets no `--swiotlb`), and the label says `shared-ram=1`. `phone vm-boot` then runs Droid-VM's
  crosvm (`--droidvm DIR`, pushed and checked), checks `/dev/gunyah_share`, checks the tree's `shim_handoff@` node
  where it checked the pool's, and starts the bridge with `displayctl pool --vm shared` (it takes the devices' region
  inside crosvm's mapping of the guest's RAM, which only a shared-RAM VM may do: a protected VM's other mappings are
  lent memory). Not booted yet: it waits for a pool of about 1000 pages.

### The bridge, the remembered Wi-Fi network and the power key in the crosvm VM (2026-11-25; branch native-vm-bridges)
Four faults the owner reported after take9 to take12, each traced on the PC from the sessions' files
(`logs/pa3q-vmboot/`), from iOS's own log in a copy of the VM's Data store (`oneios-tool phone pull-data --vm`, then
`oneios-tool ioslog`), and from 24A446's binaries. **Built and host-tested on the PC; none of it has run on the phone
yet** (the next session is the owner's). The lines that prove each on the phone are at the end.

**1. The battery showed 0 % (and 2, no haptics): the VM had no bridge channel.** The battery, haptics, thermal and
brightness drivers of `drivers/` all talk to the phone over the bridge channel, which is xnuhost's virtio-mmio device
`/arm-io/bridge` ("oneios,bridge"). crosvm's VM has no xnuhost and no such device, so every one of them said so at
boot (take9, 10 and 12, `serial.txt`): `ONEIOS BRIDGE: off: no /arm-io/bridge in the device tree`, `ONEIOS HAPTIC:
off: no bridge device`, `ONEIOS BATTERY: off: the host serves no battery channel`, `DroidVMAudio: published Speaker=1
Mic=1 Actuator=0`. iOS's powerd in the same session: `No Battery to poll.`, `Invalid/no power source information
received`. The fix is a second transport for the same messages:
- The kernel (`drivers/bridge/OneiOSBridge.c`): when `/arm-io/bridge` is compatible `"oneios,bridge-uart"`, its `reg`
  is the absolute address of an ns16550 (crosvm's second serial port, `0x2f8`; the console is the first, patch 0077),
  and each message goes there as a frame (`drivers/bridge/oneios_bridge_stream.h`: `0x4f 0x42`, the 8-byte header,
  the payload, a CRC-16). Polled as the console is, never by interrupt: every 10 ms while messages come (and for 2 s
  after one), every 250 ms otherwise. The hello is sent again every 0.5 s until it is answered; after 20 without an
  answer the transport is given up (`ONEIOS BRIDGE: off: no answer to 20 hellos on the serial port`) and costs nothing.
  The drivers above it are unchanged.
- The loader: `xnuloader-crosvm --bridge` adds that node (and `bridge` to `OUT.disks`).
- The phone's end: `bridgectl serve /SOCKET` (`runtime/bridges/devices/bridge_stream.c`) listens on a Unix socket and
  serves control, the battery (`runtime/bridges/battery`: read only, `/sys/class/power_supply`) and haptics
  (`runtime/bridges/haptics`: the force-feedback device, `sec_vibrator_inputff`). Thermal, brightness and Bluetooth
  are not offered in its hello, so those drivers stay off as before.
- `vm-boot`: for an Image with the `bridge` line it pushes `bridgectl` (`--bridgectl FILE`), starts `bridgectl serve
  $D/bridge.sock` before crosvm and gives crosvm `--serial type=unix-stream,hardware=serial,num=2,path=$D/bridge.sock,
  input-unix-stream` (only when the socket is there: crosvm does not start without it). `--no-haptics` serves the
  battery alone. `bridge.txt` is its log; `run.txt` gets `bridge:` lines.
With the haptic channel served, `drivers/haptics` publishes the Actuator audio device and `/product/haptics` as it does
under xnuhost (`docs/hardware.md` 5): iOS's haptics then go to the motor instead of "audio only"
(`setClientPlayerBehavior: Muting haptics ... AudioOnly` in take12's log). [U] That path has never run on the S25
(`docs/SM8750_PORT.md` 2: the S26's motor refused the sine upload); `bridge.txt` says what the motor did
(`haptics: effect N played on ...` or `failed on ...`).
That changes iOS's whole haptic route, not only the motor: under xnuhost the same node once stopped the power key from
locking until the stand-in button haptics service and the built-in buttons were on (`docs/hardware.md` 5, "The power
key"), so it relies on `oneios-button-haptics=1 oneios-hid-buttons-builtin=1` staying in the Image's boot arguments
(they are in take9 to take12's). If anything regresses, `vm-boot --no-haptics` keeps the battery and leaves iOS without
an actuator as before (`oneios-actuator=0` in the boot arguments does the same in the kernel). The motor's own part can
be tried before a session, without iOS: `bridgectl haptic-pattern` on the phone (it vibrates: the owner's to run) sends
several effects in a row through this same backend, which is the "once, then never" question for the S25's motor.

**2. "It vibrated once at the first ringtone preview, then never."** Nothing of iOS can have reached the motor in
those sessions: the drivers were off (above), `ONEIOS HAPTIC: counters: ... 0 requests to the motor | vibrator service
0 requests`, and iOS's own haptic server played sound only (`start: client ID ... audio: 1 haptic: 0`, `Muting haptics
on client ... AudioOnly` in take12's log). [U] what the owner felt once (the speaker at full scale, or an effect of
Android's own). The phone's kernel log names every effect of the motor's driver (`cs40l26_upload_effect`,
`cs40l26_playback_effect`), but it holds only a few minutes and its clock stops while the phone sleeps (after take12
it read 48524 s at an uptime of 69031 s), so it no longer had the sessions when it was read: nothing is known from it.
(A first reading of it, "no effect in take12's 18 minutes", was wrong for that reason; this branch's third commit
message still says it.) The fix is 1.

**3. Wi-Fi forgot the network: iOS's join never ended, so iOS never saved it.** iOS's log of take12: wifid started with
`Fetched 0 known networks from the plist` and `WiFiSecurityCopyPassword ... Error result -25300` (no such keychain
item), and the join the owner made in Settings ended, although the phone had joined and the link was up, with
`{ASSOC-} Failed to join(-3905 ...)` and `END REQ [ASSOC] took 30.138668229s (... proc=Preferences ...) err=-3905`
(a timeout); later `WiFiManagerFlushKnownNetworksCache: Successfully flushed 0 known networks to the plist`. The
kernel's side of the same join (`serial.txt`, take10 and take12): `ONEIOS WIFI: event 9 for en0, 540 bytes -> 40`:
the association event (APPLE80211_M_ASSOC_DONE, which wifid waits for) was refused with EMSGSIZE. Patch 0049 gave
that event room (an mbuf cluster) for the hosted kernel under `#if XNU_HOSTED`; the native kernel is built without
`XNU_HOSTED`, so its limit was still one mbuf (288 bytes: `mov w10, #0x120` in the built `kern_event.o`). **Patch
0083** makes that `#if` take `XNU_ONEIOS_NATIVE` too. The link's own events (2, 3, 4: 32 bytes at most) always went
through, which is why the network worked while iOS thought the join had failed. The Data store was not the cause: it
is kept, and its checkpoints ran to the session's last seconds.

**4. The power key's click came late: iOS held the key for 150 ms.** In iOS's log of take9 (12 presses of power),
backboardd logs on every key-down `[com.apple.iohid:connection] ... Event latency:151586510 type:3
sender:0x1000001F9` (151 to 153 ms; 173 the first; none on a volume key), and its own `finished: firstDown:0.00992s
ago` for presses that lasted 0.16 s: the down event arrived just before the up. The hold is iOS's keyboard service
filter (`IOHIDKeyboardFilter::processLockKeyDelay`, read in 24A446's shared cache): a Consumer/Power key-down from a
service that conforms to a keyboard (usage page 1, usage 6) is kept for `LockKeyDelay` ms, 150 unless the service's
`HIDEventServiceProperties` names another, 0 for none. The buttons' HID device is such a keyboard; an iPhone's
buttons are not. `OneiOSHidInput.cpp` now publishes `LockKeyDelay` = 0 in the buttons' event service's
`HIDEventServiceProperties` when its event driver attaches (`oneios-hid-lock-key-delay=N` for another value). The
rest of the path was measured in the same log and is short: from SpringBoard's `Lock button down` to
`_playLockSound` to the Speaker's `StartIO` succeeded is about 13 ms, and `displayctl touch` passes each key on as
it comes (its panel switch is on the release). [U] the phone's own part after that (crosvm's AAudio stream waking an
output that Android put in standby): not visible from the PC.

Also seen, not changed: the times in iOS's log of a native session are squeezed about 52 times within a stretch (the
log's reader takes the VM's 19.2 MHz counter for nanoseconds); the mach times inside the messages are right.

Tested on the PC: `make -C runtime/bridges test` (the new `test_stream`: 65 checks: the frames, the phone's end against
a guest on a socket pair with a counted motor and a made-up battery, and `bridgectl serve` reached as crosvm reaches
it), `make -C runtime/loader test` (192 checks; the node), `cargo test -p oneios-phone` (the script: the tool before
crosvm, the port only with the socket, `--no-haptics`), `oneios-tool kernel drivers --check` against the native and
the hosted kernel's compile records (17 sources each), `OneiOSHidInput.cpp` and the patched `kern_event.c` compiled
with the native kernel's own recorded commands (with 0083 the object picks 2048 for an Apple 802.11 event:
`cmp w8, #0x6; mov w8, #0x800; csel`), `xnuloader-crosvm ... --bridge` on the native collection (the node in
`OUT.adt`). Not done: a native kernel build and a boot (the kernel's own tree is the lead's; the PC bench has no
ns16550).

To use it: build the native kernel and its collection again (0083, the HID archive, `drivers/`), `make -C
runtime/bridges build/android/bridgectl`, the Image with `xnuloader-crosvm ... --bridge`, then `vm-boot` as before.
What the next session shows:

| Fault | Read (no write) | Says it is fixed |
|---|---|---|
| battery | `serial.txt`; `bridge.txt` | `ONEIOS BRIDGE: the serial transport: an ns16550 at 0x2f8`, `ONEIOS BRIDGE: on: host protocol 1, channels 0x23`, `ONEIOS BATTERY: on: IOPMPowerSource published at N%`; `bridge: battery: iOS asks: N%, on power` |
| haptics | `serial.txt`; `bridge.txt`; during the session, `oneios-tool phone dmesg --grep 'cs40l26_playback_effect'` (the kernel log keeps only minutes) | `Actuator=1`, `ONEIOS HAPTIC: play N: ...`; `haptics: effect N played on /dev/input/event6`; the motor driver's lines right after a tap |
| Wi-Fi | `serial.txt`; the second session's `wifi.txt` | `ONEIOS WIFI: event 9 for en0, 540 bytes -> 0`; in the next session a `join:` without a touch of Settings (iOS's log: `Fetched 1 known networks from the plist`) |
| power key | `serial.txt`; iOS's log (`oneios-tool ioslog show data.img --boot last --proc backboardd --grep 'Event latency.*type:3'`) | `oneios-hid: buttons: LockKeyDelay 0 ms published in the event service's HIDEventServiceProperties`; no `Event latency` line of about 150000000 on a power key-down |

### iOS in the shared-RAM VM (step 9, 2026-11-23)
- **The pool, minutes after a restart.** At 80 min of uptime neither `acquire` beside One UI nor `grow` with it stopped
  found a new 2 MiB page: with One UI stopped 8.6 GB were free and only 5 free 2 MiB blocks lay outside the CMA areas
  (`/proc/pagetypeinfo`: every free block of order 9 or 10 was CMA; Movable free pages no larger than 256 KiB), so
  nearly every block held a page nothing moves. The owner restarted the phone and took the temporary root, and 2.5 min
  after the start `hugepool load --want 2304` and `grow` took **1961 pages (3.8 GiB) in 31 s**, One UI down about
  45 s. At 8 min of uptime the same grow had found 59 (18:34). So the pool is filled within minutes of a restart and
  kept for the session (no unload between VMs).
- **Boot shared1** (`xnuloader-crosvm --shared-ram --ram-mib 3584 --cpus 8`, `vm-boot --pin`): crosvm put the whole
  window, 3578 MiB, in the pool's pages (its mthp pass: 1789 of 1789 in 2 MiB, 100 %), shared it as one parcel in 34 ms,
  the shim accepted it, and the kernel started with 857,685 free pages (3.3 GiB; 1.6 to 1.8 before). But no disk:
  Droid-VM's crosvm has an **ECAM** PCI host (`pci-host-ecam-generic`, 1 MiB at 0x10000: a device every 32 KiB) where
  AVF's has a CAM one (2 KiB), and the kernel looked for the disks 2 KiB apart (`PCI ffff:ffff is not a modern
  virtio-blk function`); the touch was found by chance (ECAM device 1 is the CAM's step 16). crosvm's interrupt map
  had the disks at 2 to 6, where vm-boot pins them, and the input at 1. Stopped; the pool had 1930 free after it.
- **The fix, in the loader only:** in shared mode each disk's node is its function's place in the ECAM (`0x10000 +
  device << 15`) and `/arm-io/input-pci` is device 1's first 2 KiB, one step of the kernel's 2 KiB scan; nothing in
  the kernel changed.
- **Boot shared2: iOS with 3.5 GiB and 8 vCPUs.** The five disks (os, systemos, app, seed, the Data store), the
  root, the GPU add-on mounted, launchd and its boot tasks, backboardd with the touch and the buttons, SpringBoard;
  the owner's finger (Spotlight). The bridge took the boot video out of crosvm's mapping of the guest's RAM
  (`displayctl pool --vm shared`: the memfd's offset is the address less 0x80000000). The CPU still draws every
  frame: about 13 frames a second, backboardd at 111 % of a core. Next: the GPU.

### The frame rate in the shared-RAM VM (2026-11-25)
The owner's report of take12 (`logs/pa3q-vmboot/take12`: 2560 MiB, 6 vCPUs, 720x1560 at 2x, the GPU's scanout, the
GPU held at 607 MHz and the big cores at 2.84 GHz or more): it looks like 50 frames a second on a 120 Hz panel.
What the logs and the code say, and what was changed.

- **The guest hands crosvm 70 to 100 frames a second in a swipe.** `oneios-gpu-report`'s "frames" are the render
  server's fenced submissions; against the display's own `frame N scanned out` lines they were one scanout each in
  take10 (16,581 frames for the 16,384 scanouts from 16,385 to 32,769) and more than one in take12 (11,192 for the
  8,192 from 8,193 to 16,385, 4,888 for the 4,096 before: 73 to 84 scanouts per 100 frames; two of its 10 s had 1,552
  and 2,244 frames, more than 120 a second, so some submissions are not frames shown). Why take12 has more
  submissions per scanout than take10 is not known. A swipe's 10 s: `frames +1011 mean 6655 us`, 741 under 8 ms,
  252 at 8 to 16, 18 longer. So crosvm gets well over 60 frames a second; half the rate the owner expects is lost
  after it.
- **The cause, inferred (not read during the session): the panel ran at 60 Hz or less.** `oneios-display --takeover` set surfaceflinger's mode specs only when the
  panel was not in its native size; the S25 at WQHD+ already is, so the specs stayed as One UI left them when its
  framework was stopped. After the return power saving was on (`settings get global low_power` 1, read at 16:51; whether Samsung's
  cpufreq_limit of 1689600 in take12's run.txt is the power saver's is a guess) and the policy was `displayManagerPolicy={defaultModeId=2,
  ... primaryRanges={physical=[10.00 Hz, 60.00 Hz] ...} appRequestRanges={physical=[10.00 Hz, 60.00 Hz] ...}}`
  (`dumpsys SurfaceFlinger` after the return; the 34 entries of its mode specs history, 16:06:59 to 16:25:14, are all
  10-60 or 60-60 with mode 2, and surfaceflinger is not restarted by vm-boot's takeover or return: one process, up
  19 h. The history has no entry after 16:25:14, not even for the return at 16:50, so it is not complete, and the
  6 minutes before the takeover at 16:31:57 are not covered). The
  layer's `setFrameRate(120)` cannot leave the app request range, so surfaceflinger latched at most 60 of the
  guest's 100 frames, unevenly (the guest's tick is its own 120 Hz timer). The helper's line said `active
  1440x3120 (up to 120 Hz)` and `120 Hz`: the highest rate among the modes of that size and the vote, not the rate
  the panel was in. **Not read during the session**: the active mode while the VM ran (the session had ended); the
  policy is from the dump after it. take10 (the day before, cpufreq_limit 1958400: another profile) has no such dump.
- **The fix** (runtime/display, `OneiOSDisplay.java`): in a takeover the helper sets the specs whenever the panel's
  size or surfaceflinger's policy is below the native mode's; in the crosvm mode it also holds the rate (range 120
  to 120: no framework tells surfaceflinger of a touch, and its 250 ms idle timer would lower the rate between
  gestures; the hosted session's helper keeps ranges from 0),
  gives One UI's specs back when it leaves, logs the active mode's own rate and the policy, and in the crosvm mode
  looks at the panel's rate every 5 s (runtime/display/README.md "The panel"). Tested on the PC: `make -C
  runtime/display test` (173 + 21 checks: the choice with the S25's mode list and take12's policy), the dex built.
  **Not run on the phone**, and the calls to surfaceflinger (the policy read, the specs given back, the look every
  5 s) have run nowhere: whether Samsung's surfaceflinger takes a 120 Hz policy from the helper is the test. The lines that prove it there, in `display.txt`: `panel modes: active 1440x3120 at 60 Hz
  (mode 2; ...); surfaceflinger's policy: default mode 2, the panel at 10 to 60 Hz`, then `asked surfaceflinger for
  the native mode (id 0) held at 120 Hz`, `the panel is in its native mode 1440x3120 at 120 Hz (mode 0)`, `the panel
  runs at 120 Hz ...`, at the end `the panel's rate over the session: 120 Hz 100 %` and `surfaceflinger's policy is
  One UI's again: default mode 2, the panel at 10 to 60 Hz`; and during the session `dumpsys SurfaceFlinger | grep -E
  "activeMode|renderRate|displayManagerPolicy"` (read only). A test with no new code: power saving off and motion
  smoothness adaptive before the session, which should leave a policy that reaches 120 Hz.
- **Second, not changed: the guest's virtio-gpu has no interrupt on crosvm's PCI.** `virtio-gpu: PCI function at
  0x6f000, notifications x4, polled (no interrupt)` and `interrupt 0 (0)`: the loader gives a PCI function no
  `droidvm,intid` (runtime/loader `machine.c`), so every wait for a command or a fence (the render server's, and the
  display's wait for a frame's GPU work before its flush) is the transport's watch looking at the used ring with
  sleeps of 100 us (`DroidVMVirtioGpu.cpp` `pause()`), each a timer wake-up of an idle vCPU through Gunyah. The
  hosted path sleeps on the device's interrupt after 300 us. Its share of the 6.6 ms mean frame is not measured (the
  native kernel prints none of the transport's counters: `droidvm_virtio_gpu_waits`, `_legs`); the GPU's own work is
  about 2.5 to 3 ms of it (kgsl's busy 22 to 30 % at about 100 frames a second and 607 MHz; it was 1 % only on a
  still screen). The road: the function's INTx line from crosvm's interrupt map in the loader, `droidvm,intid` on
  the node, the transport as it is (it acknowledges by reading the ISR); a line that never fires costs every wait
  its 20 ms guard, and neither QEMU's bench (MMIO transports) nor this PC can show Gunyah delivering it, so it is a
  phone test of its own, with the transport's counters printed.
- **Looked at and set aside.** The GPU's clock (607 MHz against the day before's 160 to 222 MHz: the same frame
  times). crosvm's side of a fence: no timer, its worker waits on the fences' descriptors (crosvm
  `devices/src/virtio/gpu/mod.rs`); its Android display blits asynchronously by default
  (`CROSVM_ANDROID_DISPLAY_ASYNC_BLIT`, read from the binary: default on) and answers the guest's fenced flush when
  the blit has read the frame. Its threads: `v_gpu` waited 4.44 s for a core over 239,424 turns (19 us each) and
  runs on any core (affinity 0-7; `--pin` moves only the vCPUs), so no fight for cores 0 and 1 (`/proc` of crosvm,
  read as take12 was ending). `CROSVM_GPU_RT_PRIO` (a realtime `v_gpu`) is not used: no realtime threads. The vCPUs'
  wake-up: gunyah_halt_poll is loaded (`phone haltpoll status`: halt_poll_ns 200000). The shader priming list: one
  library in take10 and take12 alike (`oneios-gpu-prime: ... 1 libraries`), so not a difference between the two
  days; a shader compiled at first use runs before its submission, so it is a gap between frames, not one of the
  report's frames over 32 ms (222 in take12, 1006 in take10; 81 of take12's 222 fall in a 10 s with a GPU open or a new context).

## Next
1. The Gunyah VM fits iOS with gh_hugepage_reserve ("Verdict, revised"). Its road to an iOS guest, each step a
   test on the phone: the native kernel booted by crosvm with its console on the 8250 at `0x3f8` (done: "The native
   kernel in crosvm", boot 1); virtio over PCI with every buffer in the restricted DMA pool and the image set's disks
   as virtio-blk from /data (done: step 2, boot 2: launchd, the 60 boot tasks, the daemons, SpringBoard); the screen
   (done: step 3, boot 3: the boot video in the pool, the lock screen on the panel, SpringBoard stays up); touch
   (done: step 4, boot 4b: crosvm's multi-touch device fed by `displayctl touch`, the kernel's virtio-input on PCI, the
   Home screen from the test swipe, boot 5: the owner's finger, Settings and Control Center); more vCPUs (done: step 5,
   boot 6: four, ArmCpu's PSCI CPU_ON by HVC); the screen's work and the clock (done: step 6, boots 7 and 8: write-only
   buffers in the helper, 1080x2340, crosvm's PL030 read by patch 0078); the GPU's first question (done: step 7, boots 8
   and 9: Android's GPU writes into the VM's pool, a pool of 128 MiB works); next the GPU's device on crosvm's PCI and
   its buffers in the pool (drm2kgsl, as the hosted path's: what is left of the heat is the guest's drawing on its
   CPU). Still to try for the guest's copy: the differential
   copy (`sheng-fb-diff=1`) and the changed rows told to the bridge. The GPU, the
   owner's question (2026-11-23: a host module that shares memory with the VM, "gunyah_host_share"): it is Droid-VM's
   (step 8), and with their crosvm the VM's whole RAM is shared with Android (shared1 ran on the S25). The Adreno stays
   Android's (One UI needs it, and its power, clocks and GMU firmware are kgsl's), so the VM's GPU work goes to
   Android's kgsl, on iOS's own buffers. Next: XNU in the shared-RAM VM (a pool of about 1000 pages first), then
   virtio-gpu on crosvm's PCI (`--gpu` with Droid-VM's virglrenderer and drm2kgsl, which their package carries) and
   the kernel's virtio-gpu client over PCI, as the input's. The pool is
   grown once per boot after the temporary root (`hugepool load`, `hugepool grow`); `hugepool unload` gives the 3 GiB
   back.
   The other paths stay: the hosted one that runs today, and XNU alone (the hand-off under Samsung's protections,
   the command-mode panel, UFS behind file-based encryption).
2. **Later, the owner asked (2026-11-23): tell gh_hugepage_reserve's authors about its fragility** (an issue on
   https://github.com/Droid-VM/gh-hugepage-reserve, with the owner's go before anything is posted): an insmod line
   without `pageblock_order_val` leaves `gh_cma_order` at -1, so `GH_S` is `1u << (-1 - 9)` (undefined; 4 Mi on
   arm64) and the first acquire panics the kernel in `gh_try_complete_block`, which reads `gh_slot_at()` without a
   NULL check (crash 1 above: the panic text, the registers, the S25's 22 RAM ranges). Suggest: refuse or default
   the order at insmod, and the NULL checks in the sibling loops (`gh_try_complete_block`, `gh_classify`,
   `gh_rehome`). And the pool's pages after a VM: free pages 1,405 after boot 2, 1,344 after boot 3, 1,203 after
   another VM, **198** after boot 4 (2400 MiB), with `served` 141 and then 492 while no VM was up (`active_vms=0`) and
   the rest back in the kernel's free memory; the next VM's RAM then fell to 4 KiB pages and Gunyah's heap (boot 4b's
   first start). `acquire` refills it beside One UI.
3. On the bench: the unlock and the Home screen (more time, or less work: the display's size, the jobs that a bench has
   no use for), a network (QEMU's virtio-net: mDNSResponder, rapportd and the others have nothing to talk to), the RTC
   (virt's PL031) for the clock, and the CPU report's idle time (`hosted_processors_idle_mach` from XNU's own processor
   idle counters, so that `all` is bounded by the CPUs). The bench's speed is QEMU's (TCG on x86): it proves the paths,
   not the frame rate.
4. The phone's ID registers: read by vmprobe run 1 (no FEAT_LVA, 4 KiB only, PAC APA 5).
5. The Adreno 830's kernel side for XNU with Turnip (references: UniXNU's Adreno740, UniXNU-houji's A750 port).

## Flattening the power gap (2026-11-25)

The native VM must cost more than bare Android, but much of today's gap is waste we can cut.

- **The clock floors are off by default now** (`vm-boot --gpu-floor 0 --cpu-floor 0`; `tools/oneios-tool`). They pinned
  the GPU at 607 MHz and each CPU cluster at 60% of its top for the whole session to hide a swipe's first-frame stutter,
  but that stutter was the panel held at 60 Hz (`native-frame-latency`), not the clocks. The residency proves the cost:
  in take12 (floors on) the perf cluster held ~3.0 GHz and **never** dropped to idle and the GPU sat at 607 MHz while
  only ~12% busy; in take10 (governor, no floors) the perf cluster averaged 1.37 GHz and was idle (≤1.2 GHz) **84%** of
  the time and the GPU averaged 282 MHz for the same work — and take12 ran ~8 °C hotter on the big-core zone (63.7 vs
  55.7 °C mean). So the default is now the governor's own scaling; `--gpu-floor 607` / `--cpu-floor 60` stay as opt-in
  for an A/B. A real battery-drain number (fuel-gauge `current_now`, or `dumpsys batterystats`) still needs the phone;
  the residency and temperature are a proxy, not watts.
- **The virtio-gpu transport's waits sleep on the GPU's interrupt (implemented 2026-11-25, branch `native-gpu-irq`;
  needs the phone A/B).** Before, the guest's virtio-gpu on crosvm's PCI had no interrupt: the loader gave PCI
  functions no `droidvm,intid`, so the transport (`DroidVMVirtioGpu.cpp`, vendored under `kits/droidvm`, staged by
  `kernel/build-native.sh`, not a tracked file) waited on each fence by polling the used ring with 100 µs sleeps (a VM
  exit each), and a vCPU never deep-idled while anything animated (serial: `virtio-gpu: ... interrupt 0 (0)`). The
  transport could already sleep on a line: given the node's `droidvm,intid` it attaches that SPI (level), spins 300 µs
  on a wait, then turns the queue's interrupt on and sleeps on it with a 20 ms guard; in PCI mode its read of the ISR
  acknowledges the line. crosvm's PCI host gives every function its own level-high line (take12's `fdt.dtb`,
  `interrupt-map`: 00:0c.0, the GPU, to SPI 4 = INTID 36; the disks 00:02.0-00:06.0 to SPI 5-9, 00:01.0 to 10,
  00:07.0-00:0a.0 to 11-14: none shared). So:
  - `xnuloader-crosvm --gpu-intid N` (with `--gpu`/`--gpu-scanout` and `--image`; off by default, existing Images are
    unchanged) writes `droidvm,intid = N` on `/arm-io/virtio-gpu` (`xl_machine.gpu_intid`; the other PCI functions
    still get none and poll), prints `GPU interrupt: INTID N (crosvm's SPI N-32)` and adds `gpu-intid N` to
    `OUT.disks`. No kernel rebuild: the transport reads the property at boot.
  - `phone vm-boot` checks it before the kernel's GPU starts: once crosvm's tree is whole, beside the
    `drm2kgsl_host@` check, the script reads the tree as hex words (`od -An -tx1 -v`, `tr`, `sed`, `grep -o -E`) and
    wants exactly one `interrupt-map` entry for the GPU's device (child address `device << 11`, any pin, any GIC
    phandle) naming SPI N-32, level-high. Then run.txt says `gpu: the GPU's interrupt is intid 36 (crosvm's SPI 4):
    its waits sleep on it`; otherwise it says what crosvm gave (`gpu: crosvm routes 00:0c.0 to GIC type ... number
    ... flags ...`, or no entry, or more than one) and stops the VM (`stop.request`, result `refused pool`): a wrong
    line could be another device's level interrupt, which the GPU's handler would never acknowledge (a storm).
  - Tested on the PC: the loader's tests (the property on the PCI GPU node only when set, = N; none on the PCI
    store; an INTID outside 32-1019 and a line without a PCI GPU refused), vm-boot's tests (the parse and its
    refusals; the check run by the PC's `sh` on trees as crosvm writes them: the match passes, another SPI, an edge
    line, no entry and two entries stop the VM), and the check by hand on take12's real `fdt.dtb` (36 passes, 37
    stops). **Not tested: Gunyah's delivery of crosvm's INTx line to the guest** (the first SPI this kernel attaches
    in a crosvm VM; QEMU cannot prove it) and toybox's `od`/`sed`/`grep -o -E` on the phone.
  - What proves it on the phone (take14, `out/crosvm/kernel-hd-wifi-2560-irq.img`): run.txt's `gpu: the GPU's
    interrupt is intid 36 ...` line; serial.txt's `virtio-gpu: control queue of 256 commands and cursor queue ready,
    features ..., interrupt 36 (0)` (`0` is attach's kIOReturnSuccess; the line before it still says `polled (no
    interrupt)`: that text is the vendored transport's, printed before it reads the property); then frames as
    smooth as take12's or better with less CPU (thermal.txt's perf-cluster clock and idle, the board's temperature).
    The failure sign: frames far worse than take12's (stalls of ~20 ms on every wait that outlives the 300 µs spin)
    = the waits run to their 20 ms guard, i.e. Gunyah does not deliver the line; the A/B is then the same image
    without `--gpu-intid` (`kernel-hd-wifi-2560.img`). The transport's counters (`droidvm_virtio_gpu_waits[6]`
    interrupts, `[7]` sleeps that ran to their guard) are not printed by anything yet. Should the line not come, the
    other road stays an adaptive backoff in the vendored transport (see `native-frame-latency` for the frame-rate half).

## What take15's memory lines say, and the jetsam properties' name (2026-11-26)

take15 (`logs/pa3q-vmboot/take15`, 09:15 to 09:35: `kernel-take15-nomic.img`, 3584 MiB, 6 vCPUs, `--touch-boost`, the
panel at 120 Hz) is the first session with 0084's lines (`oneios-memory`, `oneios-jetsam`, `oneios-band`,
`oneios-cpu-why`, `oneios-disk`). The times below are the report rounds' (the first round at 09:16:51, one every 10 s);
which screen a round belongs to is read from its busiest processes and from the touches, not told by the owner.

- **The lag of YouTube at 1080p was memory, not decoding.** While the video played (09:18:52 to 09:20:32, again
  09:21:32 to 09:22:02 and at 09:24:02) backboardd's own GPU memory went from 734 MiB to 1594 MiB in one 10 s (its
  blobs of 64 MiB or more from 5 to 20, 373,702 page faults: `serial.txt:1889`, `:1899`, `:2297`, `:2309`, `:2317`),
  with 206 imported surfaces (265 MiB; 152 blobs of 1 to 4 MiB where there were 4). The VM's wired memory was 2816
  MiB of 3584 (2851 at its highest) while the GPU's blobs were 2307 MiB (own 1969, imported surfaces 338:
  `serial.txt:2305`), and `available` fell to 199 MiB, under the idle threshold of 313 and near the critical 139
  (`serial.txt:2298`). From there the file cache thrashed: the OS's system disk served 106,931 to 151,090 requests of
  about 4.7 KiB each every 10 s (0.5 to 0.7 GiB read again and again), its callers waited up to 5.3 s for the disk's
  gate (`serial.txt:2302`, `:2379`), and jetsam killed 544 processes in the session: 428 idle exits, 23 for
  vm-pageshortage (each with `available` under the critical 35,696 pages), 15 for fc-thrashing, 11 for
  vm-compressor-thrashing, 67 long-idle exits (the causes by the order of the kernel's own table in
  `xnu-take15.macho`). The compositor's frames from 09:19:12 to 09:20:22: mean 13.6 to 22.8 ms, up to 23 % over
  32 ms. The decoders took little: videocodecd 11 to 16 % of a core, WebKit's GPU process 11 to 23 %. At the low
  resolution (09:20:52 to 09:21:32) the surfaces are small (up to 781 of them in 123 MiB), backboardd stays at 434 to
  746 MiB, nothing thrashes and it draws about 1,000 frames per 10 s at 4.1 to 5.5 ms. take14 has the same jump (19
  to 21 blobs of 64 MiB or more, 1499 to 1677 MiB), the first time in the 30 s before its backboardd was killed. **Inferred, not proven:** the 1.0 GiB are the runtime's copies of the video
  frames (metal2vulkan keeps a shadow image of a two-plane surface, `bridge/patches/0011`; NativeMetal's held imports
  have "no count or age limit", `NativeBridgeMetalFrontend.mm:1117`); the plug-in's own pool report would say.
- **The stutter with many GIFs was the CPU and the heat, not memory.** 09:25:22 to 09:25:42: mediaplaybackd 110-119 % of
  a core with 600,000 unix system calls and 305,000 context switches per 10 s, WebKit 86-98 %, backboardd 81-94 %,
  WebKit's GPU process 75-77 %, videocodecd 37-52 % (it locked surfaces for the CPU 4,345 times in 10 s: several
  videos decoding at once, inferred), all six vCPUs at 507-553 % (`serial.txt:5101`-`5130`, `:5159`-`:5163`);
  `thermal.txt` has both CPU zones at 102 to 105 °C and the prime cluster down from 4.47 to 3.5-3.8 GHz under an
  unchanged limit. `available` was 487 to 619 MiB, no process was killed between 09:24:22 and 09:28:32, the system
  disk read 56 to 125 MiB per 10 s. take14 has the same storm at 00:49:02 (mediaplaybackd 104-128 %, 103-105 °C).
- **The five freezes of the web game are not in the 10 s lines.** 09:30:52 to 09:33:12 (four-way swipes at the screen's
  middle): no compositor frame over 16 ms in the last run (09:31:52 on), no fence timeout, no kill but long-idle exits,
  `available` 544 to 635 MiB, the disks quiet. What `thermal.txt` shows: the touch boost ends 1.5 s after the last touch and the
  game goes on, so about ten times the GPU fell to 160 MHz while 17 to 52 % busy and the prime cluster (vCPUs 4 and
  5, `boot.sh:263`) to 1.02 GHz; once (09:31:41) the GPU did nothing for two samples. Whether those are the freezes is not known: a zero-code test is the game under `--gpu-floor 607
  --cpu-floor 40` in place of `--touch-boost`.
- **The touch boost: faster frames beside it, not proven to be its doing.** The compositor's submissions, all rounds
  of 150 frames or more: take13 mean 7.5 ms, 24.8 % over 8 ms, 7.7 % over 16 ms; take14 7.0 ms, 24.9 %, 5.8 %; take15
  5.6 ms, 16.3 %, 4.0 % (4.8 ms, 11.7 %, 2.0 % without its 32 thrashing rounds, which take13 and take14 cannot be
  split by: they have no memory lines). Other screens, so not like for like; and take15's own split does not confirm
  it: the 47 rounds with the GPU at 607 MHz in 70 % or more of their samples have 4.6 ms and 9.3 % over 8 ms, the 5
  rounds under 30 % have 4.2 ms and 9.8 %. Its cost shows only in the clocks: the GPU at 607 MHz in 59 % of take15's
  samples (1 % in take14), the prime cluster at its 1.96 GHz floor in 43 %; with no touch both are back at their
  lowest (the idle minute at the end: 222 MHz, 1.02 GHz). Both sessions ran on the charger, so the battery's current
  says nothing, and take15's heat is its thrashing and its storm (96 and 103-105 °C at the CPUs then, 57-67 °C in the
  game and in Settings).
- **Why jetsam took backboardd in take13 and take14: it runs in band 30.** take15's only two band lines:
  `SpringBoard[32] 180 -> 160 ... by launchd[1]` and `backboardd[54] 180 -> 30 (requested 30, assertion 0) by
  xpcproxy[54]` (`serial.txt:655`-`656`), and every daemon killed has `requested 30`. The OS image's own list,
  `/System/Library/LaunchDaemons/com.apple.jetsamproperties.V54.plist`, gives backboardd 170, accountsd and
  mobileactivationd 180, locationd and healthd 140. launchd finds that file by the sysctl
  `hw.jetsam_properties_product_type` (its strings; "sysctl hw.jetsam_properties_product_type was not set"), which the
  kernel answers from `/product` `jetsam-properties-product-type`, else from the root's `target-type` (read in
  `xnu-take15.macho`: `sysctl_load_jetsam_properties_product_type` looks up "/product" and that property). This VM's
  tree has neither (`xnuloader-crosvm` sets `model` only; `kernel-take15-nomic.img.adt`), so launchd has no list. In
  take13 (three times) and take14 (once) backboardd's exit reason was jetsam's code 4, vm-pageshortage
  (`sheng-exit: backboardd[54] status 0x9 reason ns 1 code 0x4`: take13 `serial.txt:2443`, `:2728`, `:3337`, take14
  `:2898`). **Inferred:** it was taken because band 30 is the lowest band that is not idle and backboardd, which never
  leaves it, is its oldest member; in take15 a process of band 0 or 10 was there at each of the 23 kills.
- **The fix, behind an option: `xnuloader-crosvm --jetsam-product V54`** (`xl_machine.jetsam_product`): the tree's
  `/product` gets `jetsam-properties-product-type = "V54"` and nothing else changes (no `target-type`, so
  `hw.targettype` stays empty; the label and `OUT.disks` are the same). Without the option the tree is as before.
  Tested on the PC: `make -C runtime/loader test` (the property alone and with its NUL, the tree otherwise byte for
  byte the same, an identity tree's own name kept or replaced, names that are no part of a file name refused; and the
  tool itself on the generated kernel, `tests/test_crosvm_tool.sh`). **Not run on the phone.** What proves it there:
  `oneios-band: backboardd[N] 180 -> 170 ... by xpcproxy[N]` and kills with `requested 140` or `180` instead of 30.
  What to watch, since the list is more than bands (the hosted kernel's tree carries `target-type` V54, the boot
  contract's `adt.root`, so the list is what the hosted path is meant to run with; whether its sessions loaded it
  was not read, and the native VM never has): (1) its memory limits are made for a phone with 12 GB
  whose GPU memory is not the process's (`iconservicesagent` 15 MiB when inactive: here it had 66 MiB of GPU blobs
  and a footprint of 39 MiB when it was idle-killed): `oneios-jetsam: kill ... cause 7` (per-process-limit) or `2`
  (highwater) would be new; (2) `RecoverOnCrash` for backboardd, SpringBoard, runningboardd and configd: the image
  set's root is sealed, so the key is as Apple set it, and launchd restarts the guest after three crashes of one of
  them in 60 s (`boot-contract.json` notes; `userland/roottweak/tweaks.py` `launchd_crash_policy`), where today a
  killed backboardd only comes back; (3) `DisableAllowed = false`: four jobs the launch policy disables run all the
  same (`com.apple.OTATaskingAgent`, `com.apple.diagnosticextensionsd`, `com.apple.mobile.softwareupdated`,
  `com.apple.symptomsd-diag`: the contract's `force_enabled`); (4) its `System` keys `KernelHighWaterMark` 78 and
  `UserReclaimableLimit` 62, whose meaning was not read: take15's wired memory was 79 % of the RAM in the 1080p
  rounds. The Image without the option is the way back.
- **A smaller list for this VM: `--jetsam-product V54O` (2026-11-27; the script and its tests ran on the PC, nothing ran on
  the phone, and no os.img has the file yet).** V54's memory limits are a 12 GB phone's (24A446: cameracaptured 4310 MB soft,
  videocodecd 3060, mediaplaybackd 2300, WebContent 2048 hard, Games 8192) and the VM has 4352 to 4608 MiB.
  `userland/jetsam_derive.py V54.plist --out-dir DIR` (Python 3.11; `python3 -I userland/tests/test_jetsam_derive.py`) writes
  `com.apple.jetsamproperties.V54O.plist` from the OS image's own V54 file (`oneios-tool image apfs-copy`; the image is only
  read), the same bytes every time. Memory limits (keys ending in `HardMemoryLimit` or `SoftMemoryLimit`) of 200 MB or less
  stay; above that they are 0.4 times, not under 200 MB (2048 becomes 819, 4310 becomes 1724, 8192 becomes 3277); -1 stays;
  backboardd, SpringBoard, their renderers (SwitchBoard, SBRendererService, PosterBoard, the widget renderers) and every
  `RecoverOnCrash` entry stay whole; bands and all other keys are untouched; and the `InactiveHardMemoryLimit` of
  iconservicesagent (15) and MapKit's snapshot service (14), which the last session's per-process-limit kills hit with
  footprints of 15-46 and 16-20 MiB, is raised to 64. On 24A446's list that is 285 limits in 171 entries (`--threshold 64
  --floor 64`: 601). **It needs a staged os.img that has the file**: launchd reads
  `/System/Library/LaunchDaemons/com.apple.jetsamproperties.NAME.plist` of the OS volume (first the same name in
  `/AppleInternal/Library/LaunchDaemons`; read in its disassembly), the image set never changes that volume and 24A446's
  os.img has V54 only, so an Image made with `--jetsam-product V54O` today gets no list, as without the option. The
  binary list is written by Python's plistlib, which launchd has not read yet (`--format xml` is the fallback).
  - **Putting the file into os.img (nothing of this is built or tested).** `oneios-tool image` only reads APFS, so a writer
    comes first: an in-place add in `oneios-tool image`, to be checked with `apfs-ls`, `apfs-copy` and apfs-fuse, or a Mac's
    `hdiutil` (an iOS system volume may not attach read-write there: not tried). The file goes in as 0:0, 0644, like V54's.
    The volume has no seal flag (`apfs-info`: `sealed: false`; the two cryptex volumes have it), so no root hash changes,
    and the static trust cache needs nothing: a list is no code. Then `oneios-tool image hashes` of a copy of os.img (the
    set's is a hard link of `inputs/`); the manifest's two entries, `os.img` and `os.img.hashes` (`phone manifest DIR --out
    FILE` edits the phone's, or by hand as take18 did for owner-trust.json; `image manifest` redoes the whole set and
    refuses an os.img that is not the fetch's); every Image again with `xnuloader-crosvm --image SET`, on a set that has
    the three new files (the kernel checks each 1 MiB chunk of os.img against the hash list in the Image's node; the size,
    so every `.disks`, stays; `image verify` refuses links, so it wants real files); and one `phone stage image DIR` with
    `os.img`, `os.img.hashes` and `manifest.json` (not also `phone manifest --stage`, which pushes the same files again):
    10,359,930,880 bytes over adb, and /data needs about 21 GB free beside the staged file while it runs (the pushed copy in
    `/data/local/tmp/oneios-stage-tmp` and its `.part`). The Data store and `data.seed` do not depend on os.img.
  - **Without a new os.img (read in the code, not run).** (1) The GPU addon has a second mount: `native_gpu_addon_prepare`
    (hosted 0031, the native kernel's 0076) shows `/private/var/ngpu/daemons` over `/System/Library/LaunchDaemons`, before
    launchd, as it shows `/private/var/ngpu/addons` over `/System/Library/Extensions` (a read-only bindfs union:
    [gpu.md](gpu.md) "How iOS finds the plug-in"), and the blob that `xnuloader-crosvm --gpu-addon` puts in the Image
    (NGPUADD2) is a list of relative names that the kernel writes under `/private/var/ngpu` (`userland/metal/build.sh addon`
    packs `addons/...`). A blob with `daemons/com.apple.jetsamproperties.V54O.plist` besides the plug-in's files would put
    the file there: no os.img, no seed, no kernel change, if UniXNU's installer (`native_gpu_addon_install`, not in this
    tree) takes a `daemons/` name. Not read: that; whether the union over LaunchDaemons works on the native kernel (the
    addon path never ran there, the bench stops before launchd, and a failed mount could hide every job plist); the blob
    must keep the plug-in's files, since any blob makes the kernel mount `/private/var/ngpu/addons` in place of the seed's
    folder, so the oldmetal Image cannot have it. (2) launchd's own overlay, `Bootstrap.SupplementalJetsamProperties`
    (`/System/Cryptexes/Supplemental/System/Library/JetsamProperties/com.apple.jetsamproperties.cryptex.plist`; the link
    points into `/private/preboot`, which the kernel takes from the Data volume). From the disassembly: it is read after
    the base list, each label in it replaces that label's whole entry, `Global` is skipped, and only when one byte of
    the comm page (`0xFFFFFC342`) is not 0. Whether our kernel sets it, and how the file would get into the Data store,
    was not read.
- **What would help the 1080p lag, in this order** (the option above is not one of them: it is for the kills of
  backboardd): a VM with more RAM (an Image with `--ram-mib 4608` needs 2288 pool pages, `vmboot.rs` `(ram - 32 + 1) /
  2`, and take8's pool reached 2304; with One UI down the phone had 4.7 GB free beside the pool, `run.txt`); a bound
  on the frames the plug-in keeps (its build is the macOS runner's). The hardware decoder would only take the
  decoders' 11 to 23 % of a core, and its buffers come out of the guest's RAM: crosvm f7c6ce8 has `--virtio-media
  kind=decoder,uid=N` (a helper under an app's uid over MediaCodec; on Gunyah it refuses to start without
  `--pre-alloc media-host-mb=N`) and no `--virtio-mmio-media`. The guest's side exists upstream for the MMIO
  transport (UniXNU-droidvm `DroidVMVirtioMedia.c`, `DroidVMVTParavirt.cpp`, `DroidVMVTHost.cpp`: VideoToolbox's
  paravirtualized decoder and encoder) and here as the hosted kernel's codec archive
  (`kernel/patches/oneios-hosted/codec`). Since take18b (2026-11-26) the native kernel has it, on crosvm's PCI and
  with this tree's edits for crosvm's decoder (`docs/hardware.md` 7g, `codec/README.md`), behind `xnuloader-crosvm
  --media` (the node, crosvm's media_host pool, `oneios-codec=1`) and `phone vm-boot --video-decoder` (crosvm's
  `--virtio-media kind=decoder,uid=N`; the pool is checked in crosvm's tree), all off by default. Still missing: the
  proof on the phone, and first that the codec services answer crosvm's helper while One UI is stopped.

## What take16's lines say, and the lines take17 adds (2026-11-26)

take16 (`logs/pa3q-vmboot/take16`, 13:24 to 13:36: `kernel-take16-4608-v54.img`, 4608 MiB, 6 vCPUs, the jetsam
properties V54, the panel at 120 Hz for the whole session, `display.txt`) is the first session at 4608 MiB and with
the V54 list. The owner: better; the Wi-Fi network was remembered; Voice Memos does not record; the web game lags
more at its start, then is steady, and freezes when a double tap starts the skateboard and when the player runs into
a train; YouTube at 1080p holds better, but the picture goes black for a couple of seconds with the sound going on,
comes back and goes black again; the GIFs lag only much deeper into the page. The report rounds are 63, the first
at 13:25:17 and one every 10 s (local time; `crosvm.log` is in UTC, an hour behind). Which screen a round belongs to
is read from its busiest processes.

- **The checks.** No crash exit (`sheng-exit`: none). jetsam: 169 kills, all in band 0: 128 long-idle exits (cause
  17), 34 idle exits (9), 4 for vm-compressor-thrashing (11: batterytrapd twice, keybagd, callhistoryd) and the three
  the V54 list's limits made, as the section above foresaw: high-water (2) for MapKit's SnapshotService (19.9 MiB of
  14) and iconservicesagent (36.6 MiB of 15), per-process-limit (7) for iconservicesagent (18.1 MiB of 15)
  (`serial.txt:2333`, `:2335`, `:4916`). The bands are the list's: `backboardd[55] 180 -> 170`, `SpringBoard[32] 180
  -> 160` (`:662`-`663`), and backboardd was never killed. `available` 473 MiB at its lowest (critical 180, idle
  405, soft 676), wired 3081 MiB at its highest. The pool: `large-page coverage: 4594 / 4594 MB (100.0%)`
  (`crosvm.log:20`); what was left of it after the run is in no log. `perf.txt` and `pace.txt` hold "No such
  file": the per-stage counters never ran (`oneios-gpu-stages=1` was not in the Image).
- **The microphone: its stream was given up at boot, and iOS never started the device.** crosvm opened the phone's
  output at 12:25:02.21 and its input at 12:25:12.25 UTC (`crosvm.log:58`, `:61`): the input's PREPARE took 10 s, as
  in take7, the guest's wait for a control request was 2 s, so the boot line is `virtio-snd up: output stream 0,
  input stream -1 x1` (`serial.txt:159`) with `+1 errors` in the first audio line (`:388`), and no period was taken
  from the device in the session. That alone makes a recording silent. Apart from it, the Mic device's engine did
  not run for a recording: the two engines took 68,470 periods in the session and the speaker's own count is 68,459,
  so the Mic's had 11 periods (55 ms) at the very most (the two counters are not taken at the same point of a
  start), and each of the 19 half-minute lines has `mic idle`. In the rounds with Voice Memos in
  front (13:33:27 to 13:33:57, `:4336`, `:4399`, `:4470`) the selectors are the speaker's starts and stops (`last
  1`, `0`, `2`) with `3 clients (+0 opened)`: the three clients are the ones the audio server opened at boot
  (`:984`, `+7 selectors, +11 maps`, one count for the three devices). Why iOS did not start the Mic is **not found**:
  it is in iOS's own log, in the VM's data image, which could not be read (the phone was not on adb when this was
  written). The host's side: crosvm warned of an empty read four times in the session (`AAudio stream read data
  not enough. frames read: 0`, `crosvm.log:62`-`65`), so, **inferred**, the phone's input gave frames after its
  first seconds, with `capture=false` on crosvm's line and One UI stopped; whether they were sound no line says.
- **The black picture of YouTube at 1080p: the compositor's GPU memory fills the whole GPU address range of its
  context, is let go at once, and fills again.** crosvm's renderer gives every context 2 GiB of GPU addresses (`VA
  slices: 64 x 2GB`, `crosvm.log:47`). When the video started backboardd's blobs went from 513+85 MiB to 1730+298
  MiB in one round (13:28:57, `serial.txt:2296`, `:2356`: 2028 MiB of the 2048; take14's highest was 1906, take15's
  1987), and then they fell and rose five times in three and a half minutes: 1207 and 841 MiB (13:29:07 and :17),
  1682 (13:29:27 to :57), 373 (13:30:07), 1281, 1395, 1683, 1688+320 (13:30:17 to :47), 376 (13:30:57), 1648
  (13:31:37), 1301 (13:31:47), 1585 to 1615 (13:31:57 to 13:32:27), 387 (13:32:37). While they were full the VM's
  wired memory was 2869 to 3081 MiB and `available` 473 to 648 MiB, under the soft threshold of 676: the memory
  pressure level is then `warning` (this kernel: under the soft threshold, and `normal` again above 115 % of it, 777
  MiB), and only the releases took `available` back over that (1027 to 2105 MiB after them). In the rounds of a
  fill the decoder did little, videocodecd 7 to 19 % of a core (13:28:57 to 13:29:17, 13:30:17, 13:31:37 to :57)
  against 60 to 80 % in the six rounds it played through, with 343 and 350 swaps and 76 and 99 pauses in the first
  two (`:2392`, `:2449`), while the speaker's engine took its 2000 periods a round in most of them (`:2399`,
  `:2460`, `:2593` to `:2895`, `:3707` to `:3884`). **Inferred, not proven:** the picture is
  black while the plug-in's copies of the video's frames (the section above) are made again after a release, and a
  release is either (a) the plug-in's answer to the memory warning, which a process is told again every 25 s or so
  while the level stays (`WARNING_NOTIFICATION_RESTING_PERIOD`), or (b) its answer to an allocation that fails at
  the range's end. No line says which: no refusal, no fence timeout, and neither the level's changes nor the
  moments of a release were written. Not memory in the sense of take15: nothing thrashed and nothing was killed
  for a shortage. **Not fixed in take17:** a bound on the frames kept is the plug-in's (its build is the macOS
  runner's), a larger slice the host renderer's. The renderer on the phone is Droid-VM's prebuilt
  `libvirglrenderer.so` (the same file in both prebuilt sets, `dl/droidvm-prebuilt`), which cuts 64 slices of 2 GiB
  from the 128 GiB range; the copy of Droid-VM's renderer source on the PC (`dl/virglrenderer-src`, fetched
  2026-11-24, its commit not recorded) cuts 32, "not the bitmask's 64: ... An iPadOS render server at 2048x2732 held
  1.8 of a 2GB slice at an idle home screen, and the next BO its turnip made found no range: the device was lost",
  which on this phone would be 4 GiB a context. So a renderer built from that source, or Droid-VM's next prebuilt, is
  the fix for (b); neither was built or tried here. `NCTX_RELAX_SLICE` in the renderer's environment only widens the
  host's check of a guest address from the context's slice to the whole range (`kgsl_relax_slice_checks`); the
  guest's Turnip is still told its slice, so it is not a way to more room. What a session can do without code: the
  VM's screen is 720 pixels wide, so 720p is all it shows, and at that size the copies fit (take15: 434 to 746 MiB).
- **The web game's freezes are single long gaps, which the 10 s lines only count.** 13:26:07 to 13:28:37: 417 to 689
  swaps a round after the first two, 1 to 25 pauses a round, WebKit's web process 25 to 72 % of a core and its GPU
  process 34 to 77 %, `available` 975 to 1404 MiB (no pressure), the CPU zones at 67 °C on average, no fence
  timeout. The display's own line has one flush of 739 ms (the round to 13:26:47, `serial.txt:1599`) and one of
  115 ms (`:1902`): freezes, and nothing says which process had the CPUs in them or whether the GPU's clients
  submitted. The rounds before the game (to 13:25:47 and :57: SpringBoard, the icon service, Safari's launch) have
  flushes of 384 and 256 ms (`:1179`, `:1313`), and the game's third round, to 13:26:27, has 14 pauses and 71 swaps
  more than two refreshes apart (`:1479`); which of these the owner's "more lag at the start" is, they do not say.
- **The GIFs: the CPU and the heat again, later because of the RAM.** The rounds to 13:32:47 through 13:33:17 and to
  13:34:17 through 13:34:37: mediaplaybackd 83 to 122 % of a core, WebKit's web process 76 to 100 %, backboardd 44
  to 96 %, WebKit's GPU process 46 to 80 %, videocodecd 18 to 40 %, the six vCPUs at 366 to 500 % of 600
  (`serial.txt:4034`, `:4100`, `:4159`, `:4219`, `:4595`, `:4645`, `:4713`); `thermal.txt` has a CPU zone at 100 °C
  or more in 21 of 32 and 19 of 24 samples of those windows (103.9 and 105.8 °C at the highest; the zones' mean was
  67 °C in the game and 75 °C in the 1080p rounds). `available` 673 to 1168 MiB: at 3584 MiB take15 was at 487 to
  619 there. No fix in take17 (the directions are the section above's: the hardware decoder, fewer wake-ups).
- **Also in the log.** aonsensed is started about every 10 s (some 50 times) and seputil 26 times (71 and 75 in
  take14 and take15): candidates for the disabled jobs, their labels not checked. The haptics' counter line was cut
  at the console's 255 bytes (fixed below).

### take17: the microphone's stream, and lines written as things happen

Built on the PC and booted on the bench; **not run on the phone**. The patch and the Images are in
`/mnt/giochi/Linux/oneios/take17/` (README.txt there has the recipe and the hashes).

- **The microphone's stream (fixed).** The native kernel's transport sets the input up in a thread of its own with a
  wait of 60 s, one control request at a time; the microphone starts only when its stream is ready; receive chains
  are posted by slot; a START without an answer in 2 s has its answer waited for (13 s more), a refused one is
  stopped and started once more. The whole of it, the lines and how to read a recording that fails:
  `docs/hardware.md` section 6, "The native VM's microphone". `oneios-audio-mic-selftest=3` in the Image records
  three seconds and then one straight off the transport at boot: it says whether the phone gives sound before
  iOS's audio stack is asked anything. `vm-boot --mic-capture` passes `capture=true` to crosvm, for a self-test
  that says digital silence.
- **Why iOS does not start the Mic: lines, since the cause is not in the logs.** `DroidVMAudio: mic: opened N by
  NAME[PID]`, `StartIO`, `StopIO`, a refused rate or format, and every 10 s in which the device was touched two
  `ONEIOS AUDIO: mic: 10 s:` lines (the clients, the selectors seen since boot, the engine's periods; what the
  device gave and whether it was sound). No `StartIO` line while Voice Memos records is iOS's own decision: then
  the unified log of that boot is the place (`oneios-tool phone pull-data --vm` before the next boot, `oneios-tool
  ioslog show DATA.img --boot last`).
- **`oneios-hitch`** (the web game's freezes): a gap of 150 ms to 5 s between two swaps that came right after
  frames is two lines, written within 200 ms of its end, with the time of day: how long, whether one process, the
  CPUs or nothing was busy in it, what the GPU's clients submitted, and the busiest processes with their
  milliseconds (a sampler thread notes every process's CPU time each 100 ms while swaps come). A freeze with
  WebKit's web process busy for its whole length is the page's script; with its GPU process busy and no
  submission, a shader or pipeline being made; with nothing busy, a wait. The names are cut at 16 characters, so
  both of WebKit's read `com.apple.WebKit[PID]`: the round's `oneios-cpu:` line has the PID with its longer name.
- **`oneios-gpu-room`, `oneios-pressure`, `oneios-video`** (the black picture): a client's blobs against a context's
  part of the GPU addresses (2048 MiB, the slice the phone's crosvm cuts; `oneios-gpu-room-mib=N` for another: the
  kernel sees only the DRM capset, which names the device's whole 128 GiB, said once), a line when a client passes
  90 % of it and when it lets go of an eighth of it or more, each with the
  second; the memory pressure level as it changes (kernel patch 0087, which also ends `oneios-memory` with the
  level); and for each second a video decoder locks surfaces, and five more: the decoders' locks, the swaps,
  backboardd's blobs against its range, `available` and the level. With the usual Image these give each black
  picture its second and what came before it (a second of black is `decoder locks +0` or few), but they cannot tell
  (a) from (b): at 4608 MiB the level is `warning` for the whole time the blobs are full (take16: 473 to 648 MiB
  available, never back over 777), so a release at 97 % or more with the level `warning` is what both give. The
  Image with `memorystatus_pressure_threshold_mb=430` tells them apart (the soft threshold just over the idle one,
  so that those 473 to 648 MiB are no warning; a stock boot argument): the level stays `normal`, and then a picture
  that stays is (a), the warning's release (the fix: that threshold, or a bound in the plug-in), and `let go` near
  100 % with the level `normal` and the picture black again is (b), the slice (the fix: a renderer with larger
  slices).
- **`oneios-clock`**: the first line of each 10 s report, the time of day in UTC and the seconds since boot, with
  the sampler's own cost (`the sampler: W walks, C us of CPU, M ms from start to end`). A walk reads each task's own
  time sums (`recount_task_times`: no lock, no walk of its threads; the first version asked TASK_POWER_INFO, which
  takes the task's lock and each thread's, ten times a second). `oneios-watch=0` leaves the sampler out.
- **The haptics' counter line** is two lines now: as one it was over the console's 255 bytes, so take16's log has it
  without its bridge part and with the next line begun in it (`drivers/haptics/oneios_haptic.c`).
- **The bench has a sound card**: `xnuloader-qemu --audio` (`AUDIO=1 scripts/qemu/boot-vm.sh`) puts QEMU's
  `virtio-sound-device` on the next virtio-mmio transport with `-audiodev none`, which plays into nothing and
  records silence at once.

## take18: the host renderer from source (Droid-VM/virglrenderer 2529a9f, 2026-11-26)

The renderer in crosvm's process (`libvirglrenderer.so`, drm2kgsl in it) was the prebuilt of both Droid-VM package
lines, virglrenderer cd71ea4 of 2026-09-01. take18 builds Droid-VM's newer source with the NDK,
`scripts/droidvm/build-virgl.sh` (Android.bp's source list and `prebuilt-intermediates/config.h`, as their AOSP build;
libepoxy 1.5.10 linked in; no libcutils: its one function, `property_get`, is a 10-line shim over bionic's), and
`vm-boot` pushes it in the package's place (`--virgl`, `--virgl prebuilt` for the old one: tools/oneios-tool/README.md).
Built: `/mnt/tkg-build/oneios/dl/droidvm-prebuilt/virgl-2529a9f/libvirglrenderer.so`, 4181576 bytes, sha256
`3c1b261f09e315af8e4539988f4b3a1abe0c2fa26436bf657fb82c16312fc818` (the same from two clean runs).

Which line: wip/hackintosh 2529a9f (2026-11-13, the line of their XNU guests) = their main line droidvm 899a4e1 plus
the macOS branch. Over cd71ea4 it has, by commit:
- 6536a4a: 32 GPU address slices instead of 64 (take17's crosvm.log: `VA slices: 64 x 2GB over VBO 0x2000000000`): a
  context gets 4 GiB of this phone's 128 GiB range (6 GiB where KGSL grants the 192 GiB rung). This is the fix (b) of
  "What take16's lines say": backboardd filled its 2048 MiB during 1080p video. take17 never used more than 15 slices
  at once (slice 14 its highest), so 32 leaves room.
- 6e0d606 (the Adreno 830's submit IB descriptors kept inline, 82 at most a submission), c64c98a (no chunk count
  underflow for an empty IB list), 439a492/7e17c8e/0d93dfa (a guest that asks gets its context's GPU faults and resets
  counted: our guest's Turnip asks, so after a GPU fault it now loses its device instead of drawing nothing;
  take17's logs have no GPU fault), 735e693/b743904 (a freed guest-allocated BO unbound in every context), 7e84c44
  (KGSL timestamps per queue with split submissions), d31f45b + the merge (the rate-window power floor), e68e95d and
  0682b30 (a deadline queue's context at the GPU's top level, and the vCPU threads' uclamp.min at 1024, while it
  animates at 60 submissions a second or more: our guest's Turnip asks for a deadline queue for a queue of high or
  realtime priority, `turnip-darwin-deadline-queue.patch` of the kernel base's Mesa list), 5faa56f (the ring of each
  priority), 899a4e1 (a new thread takes the process's CPUs), 5469ef0 (the chip id as KGSL gives it: 0x44050001 on
  this phone instead of 0x44050000; the guest's Mesa, mesa-droidvm cecc0e9, has both: `0xffff44050000` and
  `0x44050001` in one entry, so the device is the same).
- Off unless asked (unchanged): DRM2KGSL_PRIORITIES, DRM2KGSL_NONCOHERENT, the stream check and the retire delay
  (their files under /data/local/tmp), CROSVM_DRM2KGSL_DIAG (`1` only).
- The ABI: `virglrenderer.h`, `virgl_hw.h`, `drm_hw.h`, `msm_proto.h` and `msm_drm.h` are cd71ea4's; only
  `virgl_protocol.h` has two new opcodes (Windows guests). The build checks that the .so exports all 33 `virgl_*`
  functions the prebuilt crosvm imports. NEEDED: liblog, libm, libdl, libc (the prebuilt also had libcutils and
  libc++).
- Switches: `vm-boot --virgl prebuilt` (all of it back), `--gpu-host-floors off` (the floors and the clamp off:
  `NCTX_PWR_CONSTRAINT=0 DRM2KGSL_PWR_CONSTRAINT=0 DRM2KGSL_VCPU_UCLAMP_MIN=0`), `--crosvm-env NAME=VALUE`. take19:
  the default is `--gpu-host-floors deadline` (`NCTX_PWR_CONSTRAINT=0` only: the compositor's deadline constraint and
  the clamp stay); `--gpu-host-floors on` is take18's session.
- Not tested on the phone (not touched in this work). On the PC: the build, the link with `-z defs` against API 35's
  libraries, the export check, and oneios-phone's tests. The first check on the phone is
  `oneios-tool phone droidvm-crosvm --dir /mnt/tkg-build/oneios/dl/droidvm-prebuilt/f7c6ce8 --virgl
  /mnt/tkg-build/oneios/dl/droidvm-prebuilt/virgl-2529a9f/libvirglrenderer.so` (no CANNOT LINK), then a session;
  crosvm.log then says `VA slices: 32 x 4GB` (or 6GB) and `DRM2KGSL_PWR_CONSTRAINT: queues that ask`.

## take18b: crosvm from source (opt-in, Droid-VM/crosvm wip/hackintosh da647254c + samfor12 e7c1316, 2026-11-27)

The VMM stays the package's prebuilt (DroidVM-Prebuilt-Root f7c6ce8, built by Droid-VM from crosvm 0f2db62ae + wip/vpu)
unless `vm-boot --crosvm FILE` (or `phone droidvm-crosvm --crosvm FILE`) names another; `--crosvm prebuilt` is the
default. take18b's build (`/mnt/giochi/Linux/oneios/take18/upstream/artifacts/crosvm/crosvm`, sha256 `2009d576...9520`,
17641416 bytes, stripped; `build-crosvm.sh` there rebuilds it from pinned commits with cargo and the NDK r28c outside
AOSP; its REPORT.txt is the whole account) is Droid-VM/crosvm wip/hackintosh da647254c (2026-11-18: the 57 commits after
0f2db62ae, among them da64725's PCI BAR moves, a569df4's simplefb without the cache walk and the virtio-snd/AAudio fixes
a88e7c1, 13abbc1, 746434e, 1ee4dc8, d7fdd30, 14084603f) with samfor12/crosvm e7c1316 ("rutabaga: reset each resource
through the component it belongs to") picked on top, the same per-crate features as Droid-VM's
`crosvm_device_only/Android.bp`. It replaces the package's `crosvm` alone: the package's libvirglrenderer.so (or take18's
2529a9f, `--virgl`), libgfxstream_backend.so and libvncserver.so stay beside it (RUNPATH `$ORIGIN:$ORIGIN/../../lib64`
as f7c6ce8's), and libvulkan_freedreno.so too (loaded by `CROSVM_DISPLAY_VULKAN_LIBRARY`, not linked). libc++ and libbase
are linked in.
- ctx-0 blobs (the kernel's scanout resources): accepted, as f7c6ce8 does and the main line 82c59c2 does not.
  `devices/src/virtio/gpu/virtio_gpu.rs` `resource_create_blob`: a blob with a guest handle gets its udmabuf and, for any
  blob_mem but HOST3D, its iovecs too, so a GUEST blob hands virglrenderer its pages (82c59c2 does it for HOST3D_GUEST
  only, and virglrenderer then gives EINVAL). e7c1316 touches only `Rutabaga::reset()`.
- Checked on the PC: every one of its 573 imports resolves against Android 17's own libraries (the arm64 emulator image)
  and the package's three, at the right symbol versions (5 optional weak hooks of BoringSSL and zstd absent, as allowed);
  its calls into libvirglrenderer (33), libgfxstream_backend (29), libminijail (33), libcap (5) and libvncserver (25) are
  f7c6ce8's; under qemu-aarch64 with Android 17's loader `--help`, `version` and `run --help` run (162 `run` options,
  f7c6ce8's 155 all there); vm-boot's whole command line (GPU with udmabuf, the display service, AAudio, touch, serials,
  disks, `--virtio-media`) parses and stops where f7c6ce8 stops on a PC, at "failed to open Gunyah device".
- What changes against f7c6ce8, to watch in its first session: AAudio's output opens in POWER_SAVING unless an endpoint
  names `performance=` (13abbc1: f7c6ce8 left the platform default), so expect bigger bursts and more latency (the knob is
  an endpoint table, `host_device=...#performance=low_latency` with `device_table=FILE`, not tried); output through a data
  callback over a ring, and a START that waits for playback; crosvm raises RLIMIT_NOFILE itself; device workers take the
  process's CPU set (47cd593ee); BAR moves that overlap are held, not dropped (da647254c). crosvm's PCI placements are its
  own, so the Image's `--gpu-intid` and `--audio-intid` lines are per crosvm build: vm-boot checks both against the
  interrupt-map this crosvm writes and stops the VM on a mismatch.
- Not run on the phone. The first check there is `oneios-tool phone droidvm-crosvm --dir
  /mnt/tkg-build/oneios/dl/droidvm-prebuilt/f7c6ce8 --crosvm /mnt/giochi/Linux/oneios/take18/upstream/artifacts/crosvm/crosvm`
  (no CANNOT LINK, its `run --help`), then a session with `vm-boot --crosvm` the same file; `--crosvm prebuilt` (or no
  switch) is the way back, one push of the package's crosvm.
## What take17's lines say, and what take18 changes (2026-11-26)

take17 (`logs/pa3q-vmboot/take17`, 19:53 to 20:09 local, 18:53 to 19:09 UTC in the lines: `kernel-take17-4352-v54.img`,
4352 MiB because the pool reached 2223 pages, 6 vCPUs, the panel at 120 Hz for the whole session) with iOS's own log of
the boot (`data-take17.img`, boot 9938EF18, `oneios-tool ioslog show ... --boot last`; its userland times are squeezed
into seconds, so its lines are placed by their order and their processes, not their time). The owner: the lock screen
and the notification shade drop frames, Settings is smooth; the microphone records nothing; the GIF page was slightly
better but Safari decoded the images again on the way back up; Apple ID sign-in gives "unknown error"; YouTube at 1080p
never plays (its ad does), at 720p plays a few seconds and the picture goes black with the sound going on; the web game
froze a little at its start only. Proven below means a line says it; inferred means the lines fit it and none says it.

- **The microphone: iOS's audio router gave every recording route up, because the Mic device had no input data
  source.** The transport was right this time: `the microphone's stream 1 (x1): ready after 10048 ms`, the self-test's
  `599 of 600 periods ... 580 with sound ... largest sample 541` and `199 of 200 ... 495` (`serial.txt:406`-`410`).
  audiomxd opened the Mic (`DroidVMAudio: mic: opened 1 by audiomxd[100]`, `:767`), set its stream active (selector 6,
  `seen since the boot: 6`, `:1106`) and never asked StartIO. In iOS's log each recording is the same 30 times (Voice
  Memos three times, Safari's microphone test the rest): VirtualAudio's PlayAndRecord route (`Category: 'cpar'`, the
  speaker and `source: 'pmbi'; iPhone Microphone; Built-In Microphone`) fails in
  `SubPortPolicy.cpp:134 ... "Error getting data size of property [ssrc/inpt/0] on device 37."`, then
  `RoutingManager.cpp:3665 Routing is not supported: attempt to activate the routes failed` and `Route change failed:
  nort`; Voice Memos' engine then sees an input of no rate (`AURemoteIO.cpp:1142 failed: -10851 (enable 1, outf< 2 ch,
  0 Hz ...> inf< 2 ch, 0 Hz ...>)`, `VMAudioServiceErrorDomain Code=7`, "Prepare engine failed"), and Safari's capture
  does not start (`CoreAudioCaptureUnit::start ... AudioOutputUnitStart failed with error -66637`). Device 37 is the Mic
  and `ssrc` is kAudioDevicePropertyDataSource, which the Mic device did not have. Read in 24A446 on the PC (CoreAudio in
  the dyld cache, VirtualAudio.plugin from os.img, with `out/bin/ipsw` and llvm-objdump): VirtualAudio's
  "Setting sub-ports active on device" reads the input data sources as a list (the size over 4) and throws when the
  property is missing; the HAL serves `ssrc` from the scope's data source control (class `dsrc`, a selector:
  HALB_Info::TranslateDeviceAddressToControlAddress), whose items are its `selectors` (`value`, `name`) and, for a
  multi-selector, whose `value` is an array (HALS_IOA2UCDevice::CopySelectorControlInfo_SelectorMap and
  _MultiSelectorValue); the HAL sets it by SetControlValue (user client selector 2) or SetMultiControlValue (selector 8,
  the control ID and the values in, the values taken out); VirtualAudio's built-in mic tables pair `mc01` with the
  bottom mic, `mc02` front, `mc03` back and `mc04` a second bottom one (the IDs 1835216945.. an iPhone's AVAudioSession
  shows). **Fixed in take18** (`staged/audio.rs`): the Mic publishes that control (input scope, a multi-selector of
  `mc01` Bottom, `mc02` Front, `mc03` Back, `mc04` Bottom 2, `mc01` selected) and takes selectors 2 and 8 for it,
  every ID asked (all of them are the one mono stream), with a line for the first 16 changes:
  `DroidVMAudio: mic: data sources mc01 (1) set by audiomxd[100], set 1: all of them the one mono stream`. **Not run on
  the phone.** What may come next, if the route still fails: a second missing property, which iOS's log names in the
  same way (`Error getting ... property [XXXX/inpt/0] on device N`).
- **The lock screen and the notification shade: the GPU, at a cap of 607 MHz all session.** `thermal.txt` has
  `cap 607000000 pwr 8 gmax 607` in all 734 samples (take16: `832000000 pwr 5 gmax 832` in all 546): Samsung's
  `gpu_max_clock`, which One UI's SSRM writes when the phone is warm and which nothing moves while One UI is down (the
  cooling devices `kgsl` and `gpu` were at 0: `cool -`). In the rounds of the lock screen and the cover sheet the GPU
  was at its cap and 89 to 99 % busy (19:55:01 to :08 and 19:59:01 to :09 local) and backboardd's submissions took the
  host over 32 ms 91 and 165 times a round (`oneios-gpu-stages` at 18:55:03 and 18:59:06: `>32 ms +91 mean 13863 us`,
  `+165 mean 28809 us`, the swaps 67.9 and 68.5 a second), with SpringBoard, PosterBoard's MercuryPosterExtension and
  the widgets busy; in Settings' rounds the GPU was 0 to 59 % busy and the same counter 0 to 11. So the cover sheet's
  poster and materials (blurs) are GPU work that does not fit 8.3 ms at 607 MHz (inferred: which pass costs most is not
  measured). Not chronod: its crash (`sheng-exit: chronod[99] status 0x6`, `serial.txt:811`; the report
  `chronod-2026-11-26-195446.ips`: `abort()` in `+[CADisplay displays]`, "Unable to query displays from server
  (-81182718)") was at boot, before backboardd's display server answered, and chronod[168] ran from then on.
  **take18:** `vm-boot --takeover` lifts that cap by default (the take15 `--gpu-cap-lift`, never run: the nodes are saved,
  written the top of their tables, written back at every end; `--keep-gpu-cap` keeps the old way); run.txt then has
  `gpu: Samsung's cap ... lifted to ...`. The next session's A/B for the materials: Settings, Accessibility, Display &
  Text Size, Reduce Transparency, on and off on the same screens.
- **YouTube: the network, then the memory and the GPU.** (1) **The Wi-Fi bridge dropped 15 % of the frames for iOS**:
  `wifi: closed (48714 frames to the guest, ... 0 cut from coalesced frames, 8599 dropped ...)` (`wifi.txt`; take16
  5197 of 234227, 2.2 %; take15 7410 of 71414; take14 3541 of 71834). The bridge drops a frame for the guest that is
  over 1514 bytes and not TCP (`br_wifi_segment`); GRO is off on wlan0 and no TCP frame was cut, so these were,
  inferred, UDP datagrams that the phone's Qualcomm Wi-Fi datapath aggregates itself (its flow aggregation, gso type
  UDP_L4, whatever GRO is): QUIC, which YouTube, Google and Cloudflare use. A QUIC download that loses 15 % of its
  packets slows down to a crawl, which fits "1080p loads forever" and the video's source buffer fed in appends of 1176
  to 9408 bytes (`SourceBufferPrivateAVFObjC::appendInternal ... data length = 3528`, over 2400 of them) while its
  audio kept playing. **Fixed in take18** (`runtime/bridges/wifi`): such a frame is cut back into its datagrams
  (`br_wifi_segment_udp`: each `gso_size` bytes, the UDP length and checksum and the IP length made again; host-tested),
  and every frame that is still dropped is counted by kind, the first eight with what the receive path gave:
  `wifi: a frame for iOS not handed on (N of the first 8): ... gso type T size S ...` and, at the end, `wifi: closed: N
  UDP frames cut into datagrams; dropped: ...`. (2) The memory: at 4352 MiB the warning level began under 638 MiB
  (`soft 638`) and lasted from 19:03:49 to 19:05:07 with `available` 395 to 560 MiB; 86 of the session's 240 jetsam
  kills (idle exits) fell in the YouTube rounds. (3) The GPU's room, as take16: backboardd passed 90 % of its 2048 MiB
  six times (most 1910 MiB, 19:05:14) and let go of 1331, 1224 and 856 MiB (19:03:31, 19:05:07, 19:05:25), with no
  decoder lock in the seconds after each (`oneios-video` 19:03:31-32, 19:05:08, 19:05:25-29): the black picture.
  (4) The GPU cap above. Not the sampler: `oneios-clock` puts it at 95 to 23857 us of CPU per 10 s (at most 0.24 % of
  one core). Not the decoder failing: videocodecd took 6 to 37 surfaces a second with no decoder error in iOS's log
  (the `VTTileCompressionSession -12782` lines are an encoder's). The `oneios-video` lines said `backboardd GPU 0+0 of 0
  MiB` in all 219: backboardd has a context per connection and the last one, empty, was taken (fixed: its largest).
- **The GIFs decoded again on the way back up: Safari's own decoded-image budget and the memory warning.** WebKit's
  web process (pid 683) used 77 to 100 % of a core and mediaplaybackd 96 to 130 % on the GIF page (19:07:40 to
  19:08:40), and the level went to warning and back four times there (19:07:32, :38, 19:08:17, :47), each answered in
  the web process (`Received memory pressure event`, `Memory pressure relief` twice). Inferred: WebKit keeps the
  decoded frames of images near the view only, within a budget that grows with the device's RAM (4.25 GiB here against
  an iPhone 17 Pro Max's 12), and drops more on a warning; scrolled back, they are decoded again on the CPU. More RAM,
  fewer warnings and a hardware decoder (the virtio-media bridge) are the ways out; nothing changed for it in take18.
- **Apple ID: AuthKit's anisette has no daemon.** akd[142]: `ADIGetIDMSRouting failed! Error: -45017` 208 times, with
  `failed lookup: name = com.apple.adid ... No such process`: `com.apple.adid` is in the base group of disabled jobs
  (`userland/config/daemons.toml`). An Image with `oneios-enable-jobs=com.apple.adid` would show the next refusal, which
  is expected in FairPlay or the attestation (featureaccessd already says `AuthKit BAA signer failed: No attestation
  headers were added by AuthKit`).
- **Also in the log.** `oneios-granule`: the 16 KB granule is not offered at stage 1 or stage 2 (noted, not pursued).
  The web game: one freeze at its start, none at the double taps and the crashes (the owner no longer uses it).
