// SPDX-License-Identifier: MIT
// xnuloader-crosvm: the native kernel (kernel/build-native.sh) as the payload of a VM of Android's own crosvm under the
// phone's Gunyah (docs/native.md "A VM under Gunyah"). crosvm loads an arm64 Linux Image at the start of the VM's RAM
// (0x80000000, `oneios-tool phone vm-probe` saw it there) and enters it at EL1 with the MMU and caches off, x0 = its own
// FDT. This Image is the RAM as the kernel wants it: the Linux header in its first 64 bytes; at 0x40 xnuloader-qemu's
// stub (interrupts masked, x0 = boot_args, a branch to the entry); the kernel's window (xl_boot_place: its segments, the
// device tree, boot_args) at its physical base, 32 MiB aligned with the kernel's own base as on the bench.
//   xnuloader-crosvm KERNEL OUT [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--swiotlb MIB]
//                    [--image DIR [--phone-image DIR] [--phone-data FILE]] [--shared-ram [--gpu | --gpu-scanout [--gpu-intid N]] [--audio [--audio-intid N,SLOT]] [--wifi]
//                    [--media [--media-mib N]]] [--bridge]
//                    [--jetsam-product NAME] [--identity-tree FILE [--identity-groups LIST [--memory-class N]]] [--soc-generation NAME|none]
//                    [--display-corner-radius PT[,WORD2]]
// --ram-mib is the VM's (crosvm --mem): the window ends below crosvm's FDT and its restricted DMA pool (--swiotlb, 8 MiB
// by default, 32 with --display, up to 1024; the RAM's last MIB). Writes OUT (the Image), OUT.adt (the device tree the kernel is given:
// xnuloader-inspect adt FILE) and, with --image, OUT.disks; prints the layout.
// The machine is crosvm's, as its FDT for a VM described it (vm-probe run 10, `fdt.dtb`): "linux,dummy-virt", RAM at
// 0x80000000, a GICv3 distributor at 0x3fff0000 and the redistributors right below it (0x20000 a vCPU), the arch timer
// (19.2 MHz on the S25: CNTFRQ_EL0 in the VM), PSCI by HVC, four ns16550a at 0x3f8, 0x2f8, 0x3e8 and 0x2e8. The console
// is the first, as the native kernel's ns16550 driver takes it (kernel patch 0077: the node's reg absolute, below the
// arm-io window, which may not start at 0 and so begins at 0x1000 as Droid-VM's make-droidvm-adt.py has it).
// --image DIR: the image set's disks (tools/image_disks.c, as xnuloader-qemu --pci): crosvm's virtio-blk PCI functions
// 00:02.0 to 00:06.0 (os, systemos, app, the Data seed, the Data store), which `phone vm-boot` pins with pci-address=;
// each node is the function's configuration space in crosvm's CAM (0x10000 + device << 11), with the image's hash list
// and a DMA window. A protected VM's host reads and writes only crosvm's restricted DMA pool (the RAM's last --swiotlb
// MiB: crosvm's get_swiotlb_addr, and the tree of every VM the S25 made so far): each disk gets 1 MiB + 64 KiB of it (its
// queue page and its transfer buffer), and the kernel's disk driver gives the device no other address. The files are the
// phone's (--phone-image DIR, default /data/local/tmp/oneios/image, the staged set) and the Data store a file of its own
// (--phone-data FILE, default /data/local/tmp/oneios-vmboot/data.img: not the hosted session's); DIR on this machine is
// the same set, read for the identities, the hash lists and the trust caches. OUT.disks lists them for vm-boot, one line
// each: "swiotlb-mib N", "pool 0xPA" (where crosvm's tree must put it), "store FILE GIB", and "block BYTES VALUE" per
// disk (BYTES the size the phone's file must have, "-" for the store; VALUE crosvm's --block value).
// --display WxH [--scale N] [--hz N] (with --image): the boot video, B,G,R,X at W * 4 bytes a row, at the START of the
// pool, the disks' windows after it (the pool then 32 MiB unless --swiotlb says more): the guest's display driver
// copies every frame into it (it has no virtio scanout here) and maps it write-back (staged/display.rs LEGACY_BOOT_VIDEO),
// and the phone's bridge (runtime/display displayctl pool) reads it out of the pool and hands it to oneios-display.
// OUT.disks then has "fb OFFSET BYTES WIDTH HEIGHT STRIDE" (OFFSET in the pool). /chosen display-scale and the boot
// arguments as xnuloader-qemu gives them (offline-legacy-unit-scale, sheng-vsync), and oneios-hid-queue-kib=256.
// The calendar: /chosen "oneios,rtc" is crosvm's PL030 (0x2000, absolute), which kernel patch 0078 reads for the time.
// The touch, with --display: the input bus, node /arm-io/input-pci (bus 0 of crosvm's CAM: the kernel finds crosvm's
// virtio-input functions there, wherever crosvm put them), and a 64 KiB window of the pool after the disks' for their
// queues and events (three 4 KiB pages a device); OUT.disks then has "input touch": vm-boot hands crosvm the phone's
// touchscreen (--evdev).
// --shared-ram: for Droid-VM's crosvm's pseudo-unprotected VM (--protected-vm-pseudo-unprotected, gunyah_host_share;
// docs/native.md "The shared-RAM VM") instead of AVF's protected one. Its first 6 MiB are its own: a lent region with
// its boot shim and its device tree (4 MiB), then the page the host and the shim talk through (2 MiB); the guest's RAM
// is the window after them, which the host shares with the VM after it started and the shim accepts. crosvm puts the
// payload at the window's start (0x80600000, vm-probe's run shared1 saw it there), so the Image begins there; the
// kernel's own window stays where it was (32 MiB in, which is above the shim's region). There is no restricted DMA
// pool: the host reads all of the VM's RAM, and the region where the pool would be is plain RAM that keeps the devices'
// windows as before (--swiotlb sets its size; crosvm is not given it). The label says shared-ram=1, for vm-boot.
// Droid-VM's crosvm has an ECAM host where AVF's has a CAM one (its tree: pci-host-ecam-generic, 1 MiB at 0x10000:
// bus 0, 32 KiB a device, 4 KiB a function; vm-boot shared1, whose disks the kernel looked for 2 KiB apart and found
// ffff:ffff). Each disk's node is then its function's place in the ECAM (the disk driver reads the function there,
// wherever it is), and /arm-io/input-pci is device 1's first 2 KiB, where that crosvm puts the multi-touch function
// (shared1: its interrupt map has the disks at 2 to 6 and the input at 1): the kernel's scan, 2 KiB a step over the
// node, then finds it as function 0 of its one step, with no change to the kernel.
// --gpu (with --shared-ram): the GPU, crosvm's virtio-gpu with Droid-VM's virglrenderer and drm2kgsl (`--gpu
// backend=virglrenderer,context-types=drm,udmabuf=true`, pinned at 00:0c.0) for the kernel's virtio-gpu client and
// Metal's NativeAdreno (docs/native.md "The GPU in the shared-RAM VM"). /arm-io/virtio-gpu is the function's place in the
// ECAM ("oneios,virtio-gpu-pci": the transport of staged/gpu.rs TRANSPORT_PCI) with drm-host-pool, drm2kgsl's host arena:
// `--pre-alloc drm-host-mb=8` takes the RAM's last 8 MiB for it (that crosvm puts its pools after the block that --mem
// less them makes: its tree's drm2kgsl_host node, which vm-boot checks), so the devices' region and the kernel's window end
// below it. The label says gpu=1 and OUT.disks has "gpu PCI-ADDRESS DRM-HOST-MIB DRM-HOST-PA".
// --gpu-addon FILE: a Metal plug-in for this boot (userland/metal/build.sh addon: NativeAdrenoMetal.gpuaddon, NGPUADD2),
// placed after the trust caches with /chosen/memory-map GpuAddon as xnuhost does with kernel.kc.gpuaddon; the kernel
// writes it to /private/var/ngpu/addons and mounts that over /System/Library/Extensions instead of the Data seed's
// folder (hosted 0031, "oneios-gpu-addon: boot addon: N files"). Without it the seed's plug-in is used, as before.
// Its CDHashes must be in the image set's overlay/userland/owner-trust.json (the trust caches of --image).
// --prime-list FILE: the Metal plug-in's shader priming list (every .metallib of the OS image, one absolute path a line:
// oneios-tool image apfs-find), carried in /chosen/oneios-prime-list; the kernel writes it beside the plug-in at boot
// (NativeAdrenoMetal.primelist, patch 0080) and the render server compiles those libraries ahead when the desktop is quiet.
// --gpu-scanout (with --gpu and --display): the screen through the virtio GPU's scanout instead of the boot video: the kernel
// starts its scanout client (oneios-virtio-scanout=1, staged/display.rs NATIVE_PROBE), crosvm shows the scanout in an
// Android Surface it is given (its --android-display-service, the display helper's --crosvm-display), with no copy on
// either CPU. OUT.disks then has "scanout WIDTH HEIGHT HZ" for crosvm's display.
// --gpu-intid N (with --gpu or --gpu-scanout, and --image; default none): the GPU's interrupt, INTID N, as /arm-io/virtio-gpu's droidvm,intid:
// the transport (DroidVMVirtioGpu.cpp, staged) then spins 300 us on a wait and sleeps on that level line after it (20 ms guard,
// the PCI ISR's read acknowledges it), where without it the waits poll the used ring with 100 us sleeps. crosvm's PCI host gives
// each function its own level-high line (its tree's interrupt-map: 00:0c.0, the GPU, to SPI 4 = INTID 36; the disks 5-9);
// the kernel cannot read that map, so N is given here and vm-boot checks it against crosvm's tree before the GPU starts
// (a wrong line could be another device's level interrupt), with --image only. The label is unchanged; OUT.disks then has
// "gpu-intid N".
// --audio (with --shared-ram): the speaker, crosvm's virtio-snd card (vm-boot: `--virtio-snd backend=aaudio`, AAudio on the
// phone) for the kernel's audio archive (DroidVMVirtioSnd.c with staged/audio.rs SND_TRANSPORT_PCI under DroidVMAudio.cpp's
// Speaker and Mic). crosvm places the card itself (its option has no pci-address), so /arm-io/virtio-snd
// ("oneios,virtio-snd-pci") is all of bus 0 of the ECAM and the kernel takes the first 1af4:1059 function there.
// OUT.disks then has "audio".
// --audio-intid N,SLOT (with --audio and --image; take18, default none): the sound card's interrupt, INTID N, read from crosvm's
// interrupt-map for the card's slot SLOT (crosvm places the card: take17's tree had 00:08.0 at SPI 12, INTID 44), as
// /arm-io/virtio-snd's droidvm,intid and oneios,intid-slot. The transport uses it only with the boot argument
// oneios-audio-irq=1 and only for a card it finds at SLOT (staged/audio.rs SND_IRQ: its control replies then wake it instead
// of its 1 ms polls); vm-boot checks crosvm's map for SLOT before the kernel's sound card starts. OUT.disks then has
// "audio-intid N SLOT".
// --wifi (with --shared-ram): the Wi-Fi, en0 on the phone's radio: crosvm's virtio-net card (vm-boot: `--net tap-name=...`
// with wlan0's address) for the kernel's network archive (OneiOSWifi.cpp over DroidVMVirtioNet.c with staged/net_transport.rs
// NET_TRANSPORT_PCI); `wifictl tap` on the phone carries its frames and the station's messages between crosvm's tap and
// wlan0 (runtime/bridges/wifi, as xnuhost's bridge does). /arm-io/net ("oneios,virtio-net-pci") is all of bus 0 of the
// ECAM, as the sound card's node. OUT.disks then has "wifi".
// --media [--media-mib N] (with --shared-ram): the hardware video decoder, crosvm's virtio-media decoder (vm-boot
// --video-decoder: `--virtio-media kind=decoder`, the phone's MediaCodec in a helper under an app's uid) for the kernel's
// codec archive (DroidVMVirtioMedia.c with staged/codec_transport.rs MEDIA_TRANSPORT_PCI under DroidVMVTHost.cpp and
// DroidVMVTParavirt.cpp: iOS's paravirtualized VideoToolbox decoder). crosvm places the function itself, so
// /arm-io/virtio-media-0 ("oneios,virtio-media-pci") is all of bus 0 of the ECAM and the kernel takes the first 1af4:1070
// function there. The device's buffers live in crosvm's media_host pool (`--pre-alloc media-host-mb=N`, N = --media-mib,
// default 192, even; a Gunyah VM's virtio-media refuses to start without it): crosvm puts its pools above the block that
// --mem less them makes, in its order (drm2kgsl_host first, then media_host), so with --gpu the GPU's arena moves N MiB
// down and the pool is the RAM's last N MiB; /arm-io/virtio-media-0 media-host-pool names it. The boot arguments get
// oneios-codec=1 (the archive starts) and droidvm-vt-paravirt=1 (the service is published when the device is up).
// OUT.disks then has "media N POOL-PA": vm-boot always gives crosvm the pool (the layout needs it) and the device only
// with --video-decoder; without the device the kernel finds no function and iOS keeps its software decoders.
// --bridge: the bridge channel (battery, haptics: drivers/bridge, drivers/battery, drivers/haptics) on crosvm's second
// ns16550a (0x2f8), since this VM has no "oneios,bridge" virtio device: /arm-io/bridge is "oneios,bridge-uart" with the
// port's absolute address, and the kernel's bridge driver carries its messages there as frames
// (drivers/bridge/oneios_bridge_stream.h), polled as the console is. vm-boot gives crosvm the port as a Unix socket
// (`--serial type=unix-stream,hardware=serial,num=2,input-unix-stream`) whose other end is `bridgectl serve` on the
// phone (runtime/bridges): the phone's battery to iOS, iOS's haptics to the phone's motor. OUT.disks then has "bridge".
// --jetsam-product NAME (default none): /product jetsam-properties-product-type, the name of iOS's jetsam properties. The
// kernel reads the property at boot and answers hw.jetsam_properties_product_type with it (xnu-12377's
// sysctl_load_jetsam_properties_product_type, read in the native kernel's code; without it the root's target-type, which this
// machine has not unless --identity-groups root gives it: its tree has model and no identity); launchd reads /System/Library/LaunchDaemons/com.apple.jetsamproperties.NAME.plist with that
// name, the list of each daemon's jetsam band and memory limits. Without the name launchd has no list and every daemon
// keeps its default band: take15's kernel printed `oneios-band: backboardd[54] 180 -> 30 ... by xpcproxy[54]` where
// iPhone18,2's list (V54) says 170, and take13 and take14 lost backboardd to jetsam (vm-pageshortage) four times. NAME
// is V54 for the iPhone18,2 set (userland/config/device-iphone18,2.toml target_type; `oneios-tool image apfs-ls os.img
// /System/Library/LaunchDaemons` shows com.apple.jetsamproperties.V54.plist). Nothing else in the tree changes (no
// target-type: hw.targettype stays empty), the label and OUT.disks are as without it, and the layout names it.
// The list is more than bands: its memory limits, RecoverOnCrash (on a sealed root launchd restarts the guest after
// three crashes in 60 s of backboardd, SpringBoard, runningboardd or configd) and DisableAllowed = false (jobs the
// launch policy disables run all the same) come with it, as for the hosted kernel, whose tree has the target-type
// (docs/native.md "What take15's memory lines say").
// --identity-tree FILE (default none; 2026-11-27): the decoded device tree of an Apple device, the source of --identity-groups (the
// IPSW's DeviceTree payload as `xnuloader-inspect adt FILE` reads it: 24A446's iPhone18,2, V54, is
// out/kernel-inputs/24A446/devicetree.bin, from scripts/image/kernel-inputs.sh). On its own it copies NOTHING, and the layout says
// so. The file is read and checked before anything is written: it must parse as a device tree and have a /product, else status 2
// and no Image (a missing file: status 1, as for the other files).
// --identity-groups LIST (needs --identity-tree; default none): which properties of that tree the machine's own tree takes. Each
// group is a switch of its own and off unless named. LIST is comma-separated group names, their letters a to f, or all (a later
// --identity-groups replaces an earlier one; an unknown word, an empty one, a capital or a blank: status 2). The tree's bytes are
// copied but for the two said below; a property that is a placeholder (iBoot's recipe for a value, "syscfg/DClr") is left out and no
// child node (iopm, camera, audio, ...) is ever copied. The groups:
//   a artwork    /product: every artwork-* but artwork-display-gamut and artwork-scale-factor, graphics-featureset-class and
//                graphics-featureset-fallbacks (less APPLE2 and MTL2,2, as xnuhost's merge has it); and artwork-display-gamut, which is
//                the machine's "P3" and not the tree's, written only with this group (artwork-scale-factor is --scale's with
//                --display, group or not)
//   b quality    /product: ui-background-quality, ui-weather-quality
//   c workgroup  /product: display-workgroup-interval, hid-workgroup-interval
//   d ids        /product: udid-version, product-description, public-key-accelerator
//   e memclass   /product: device-perf-memory-class. The tree has 0xc (4 bytes) for the iPhone18,2, read as the device's RAM in GiB
//                (not verified), and the VM has less: the class written is the VM's own, --ram-mib / 1024 whole, at least 2 and at
//                most the tree's (4352 and 4608 MiB give 4), in the width the tree's property has. --memory-class N (valid only with
//                this group) is N instead: 1 to the tree's own (to 16 where the tree has none, and then written in 4 bytes); where
//                the tree has no such property and no N is given, nothing is written
//   f root       not /product: the root's target-type, from the tree's root ("V54"), and its compatible: the tree's FIRST entry (the
//                board name, "V54AP") and then "oneios,xnuhost" (without the group: no target-type, "oneios,xnuhost" alone). The
//                root's model stays "iPhone18,2". The tree needs a target-type and a compatible, else status 2. xnuhost's merge
//                (xl_machine.identity) takes the root the same way; its effect in this VM is not verified
// /product keeps its product-name "OneiOS guest", model and jetsam name. Never copied, with any group: island-notch-location,
// supports-camera-button, supports-burnin-mitigation, has-virtualization, display-corner-radius (see below), the exclaves keys, and
// the rest of the tree's /product. What iOS does with any of these properties is not verified.
// --soc-generation NAME|none (default none): /arm-io soc-generation, a string as Apple's tree has it ("H18" for the iPhone18,2), 40
// bytes of the tree. Only this property of the SoC: arm-io's compatible stays as it was (not "arm-io,t8150": Apple's kexts would
// match it). NAME is 1 to 31 of A-Z a-z 0-9 . _ - ; its effect is not verified.
// --display-corner-radius PT[,WORD2] (default none; independent of --identity-tree): /product display-corner-radius, 8 bytes, two
// little-endian u32: PT (0 to 200, in points) and then WORD2 (0 to 65535, default 1). Apple's iPhone18,2 tree has u32 62 and u32 1;
// what the second word means is not verified. Whole decimal numbers only (status 2 otherwise).
// The layout prints a line for each group (on or off, and the property names written), one for --soc-generation and one for
// --display-corner-radius, each read back from the tree the Image carries.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image_disks.h"
#include "imageset.h"
#include "xnuloader/adt.h"
#include "xnuloader/boot.h"
#include "xnuloader/machine.h"
#include "xnuloader/macho.h"

#define RAM_BASE      0x80000000ull     // crosvm's DRAM on aarch64
#define SHIM_REGION   (6ull << 20)      // --shared-ram: the shim's lent region (4 MiB) and the handoff page (2 MiB)
#define KERNEL_ALIGN  (32ull << 20)     // the kernel's VA and PA agree modulo 32 MiB (xnuloader-qemu)
#define FDT_RESERVE   (8ull << 20)      // below the pool: crosvm's FDT (2 MiB, 2 MiB aligned, right below it) and room
#define ARMIO_BASE    0x1000ull         // crosvm's MMIO (RTC 0x2000, PCI 0x10000, GIC) up to 1 GiB; not 0
#define ARMIO_SIZE    (0x40000000ull - ARMIO_BASE)
#define GICD          0x3fff0000ull
#define GICD_SIZE     0x10000ull
#define GICR_STRIDE   0x20000ull        // GICv3 without VLPI: RD_base and SGI_base
#define UART          0x3f8ull          // crosvm's first ns16550a (serial num=1)
#define UART_SIZE     0x8ull
#define BRIDGE_UART   0x2f8ull          // crosvm's second ns16550a (serial num=2): --bridge
#define PCI_CAM       0x10000ull        // crosvm's CAM (pci-host-cam-generic): bus << 16 | device << 11 | function << 8
#define PCI_FIRST_DEV 2u                // disk K is 00:(2 + K).0 (pinned; crosvm's pvpanic device takes a free one)
#define DMA_SLICE     0x110000ull       // a disk's DMA window: its queue page, then 1 MiB of transfer buffer at 64 KiB
#define INPUT_WINDOW  0x10000ull        // the input devices' window: three 4 KiB pages a device
#define PCI_BUS0      0x10000ull        // bus 0 of the CAM: 32 devices, 0x800 apart
#define ECAM_DEV_SHIFT 15               // --shared-ram: Droid-VM's crosvm's ECAM, a device every 32 KiB (bus 0 only)
#define CAM_DEV_SHIFT  11               // AVF's crosvm's CAM, a device every 2 KiB
#define ECAM_INPUT_DEV 1u               // --shared-ram: the multi-touch function's device on that ECAM (shared1)
#define INPUT_STEP    0x800ull          // what the kernel's input scan steps by (staged/hid_transport.rs)
#define GPU_PCI_DEV   12u               // --gpu: the GPU's device on crosvm's bus 0 (vm-boot pins it: pci-address=00:0c.0)
#define DRM_HOST_MIB  8ull              // --gpu: drm2kgsl's host arena (crosvm --pre-alloc drm-host-mb), the RAM's last MiB
#define MEDIA_HOST_MIB 192u             // --media: the video decoder's buffers (crosvm --pre-alloc media-host-mb), after it
#define ECAM_BUS0     (32ull << ECAM_DEV_SHIFT)   // --audio: all of bus 0 of that ECAM (1 MiB): the kernel finds the card there
#define RTC           0x2000ull         // crosvm's PL030 (rtc@2000 in its tree): RTCDR is the host's seconds since 1970
#define RTC_SIZE      0x1000ull

// xnuloader-qemu's arguments: xnuhost's default (runtime/xnuhost/src/config.c), less the hosted kernel's devices.
static const char default_args[] =
  "debug=0x14e serial=3 -v kt_fixed_zones=120 -no_slto_panic ml-timeout-lock-panic-scale=40 tlto_us=0 "
  "sheng-smp=1 armcpu-self-ipi=1 armcpu-spi-ipi=1 use_contiguous_hint=0 interrupt-masked-debug-mode=0 "
  "sched_preemption_disable_debug_mode=0 wdt=-1";

static uint8_t *slurp(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s: read failed\n", path); exit(1); }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

static void usage(void)
{
  fprintf(stderr, "usage: xnuloader-crosvm KERNEL OUT [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--swiotlb MIB]\n"
                  "                        [--image DIR [--phone-image DIR] [--phone-data FILE] [--display WxH [--scale 2|3] [--hz N]]] [--shared-ram [--gpu | --gpu-scanout [--gpu-intid N]] [--audio [--audio-intid N,SLOT]] [--wifi] [--media [--media-mib N]]] [--bridge]\n"
                  "                        [--jetsam-product NAME] [--prime-list FILE] [--gpu-addon FILE]\n"
                  "                        [--identity-tree FILE [--identity-groups LIST [--memory-class N]]] [--soc-generation NAME|none]\n"
                  "                        [--display-corner-radius PT[,WORD2]]\n"
                  "  LIST: artwork, quality, workgroup, ids, memclass, root (or a to f), comma-separated, or all\n");
  exit(2);
}

static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// A path crosvm's --block takes as it is: no comma (its key separator), no quote or blank for the phone's shell.
static int plain_path(const char *path)
{
  if (!*path || *path != '/') return 0;
  for (const char *c = path; *c; c++)
    if (*c == ',' || *c == '\'' || *c == '"' || *c == ' ' || *c == '\n' || *c == '\\' || *c == '$' || *c == '`') return 0;
  return 1;
}

// The first `n` characters of `text` as a whole decimal number of 1 to 5 digits and nothing else (strtoul takes a sign, a blank and
// a prefix too), at most `max`: false for anything else, so that 0 stays a number
static bool parse_decimal(const char *text, size_t n, unsigned max, unsigned *value)
{
  if (n < 1 || n > 5 || strspn(text, "0123456789") < n) return false;
  unsigned v = 0;
  for (size_t i = 0; i < n; i++) v = v * 10 + (unsigned)(text[i] - '0');
  if (v > max) return false;
  *value = v;
  return true;
}

// The names of the /product properties of the tree the Image carries that group `group` took, as a list for the layout (in `names`,
// the first ones that fit): how many there are. The group of a name is the library's (xl_identity_group_of), so what is named here
// is what xl_machine_tree copied.
static unsigned group_written(const xl_adt_node *tree, unsigned group, char *names, size_t size)
{
  const xl_adt_node *product = xl_adt_find(tree, "product");
  unsigned count = 0;
  size_t used = 0;
  bool more = false;
  names[0] = 0;
  for (unsigned i = 0; product && i < product->nprops; i++) {
    const xl_adt_prop *p = &product->props[i];
    if (xl_identity_group_of(p->name) != group) continue;
    count++;
    const size_t n = strlen(p->name);
    if (used + n + 6 >= size) { more = true; continue; }
    used += (size_t)snprintf(names + used, size - used, "%s%s", used ? " " : "", p->name);
  }
  if (more) snprintf(names + used, size - used, " ...");
  return count;
}

// A /product integer of the tree the Image carries, or 0 and `*found` false
static uint64_t product_integer(const xl_adt_node *tree, const char *name, bool *found)
{
  const xl_adt_prop *p = xl_adt_get(xl_adt_find(tree, "product"), name);
  uint64_t v = 0;
  *found = p && p->length >= 1 && p->length <= 8;
  for (uint32_t i = *found ? p->length : 0; i--; ) v = v << 8 | p->value[i];
  return v;
}

int main(int argc, char **argv)
{
  if (argc < 3) usage();
  const char *kernel_path = argv[1], *out = argv[2], *args = default_args, *image_dir = NULL;
  const char *phone_image = "/data/local/tmp/oneios/image", *phone_data = "/data/local/tmp/oneios-vmboot/data.img";
  unsigned cpus = 1, ram_mib = 2048, swiotlb_mib = 0;
  unsigned gpu_intid = 0, audio_intid = 0, audio_slot = 0;
  const char *jetsam_product = NULL;
  const char *identity_path = NULL, *identity_list = NULL, *soc_generation = NULL, *corner_text = NULL;
  unsigned identity_groups = 0, memory_class = 0, corner_radius = 0, corner_word2 = 1;
  bool memory_class_given = false;
  unsigned display_w = 0, display_h = 0, display_scale = 3, display_hz = 60;
  uint32_t timebase = 19200000;         // the S25's (vm-probe: CNTFRQ_EL0 in the VM)
  bool shared_ram = false, gpu = false, gpu_scanout = false, audio = false, wifi = false, bridge = false, media = false;
  unsigned media_mib = MEDIA_HOST_MIB;
  const char *prime_list_path = NULL, *gpu_addon_path = NULL;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--shared-ram")) { shared_ram = true; continue; }
    if (!strcmp(argv[i], "--gpu")) { gpu = true; continue; }
    if (!strcmp(argv[i], "--gpu-scanout")) { gpu = gpu_scanout = true; continue; }
    if (!strcmp(argv[i], "--audio")) { audio = true; continue; }
    if (!strcmp(argv[i], "--wifi")) { wifi = true; continue; }
    if (!strcmp(argv[i], "--media")) { media = true; continue; }
    if (!strcmp(argv[i], "--bridge")) { bridge = true; continue; }
    if (i + 1 >= argc) usage();
    if (!strcmp(argv[i], "--cpus")) cpus = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--ram-mib")) ram_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--timebase")) timebase = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--args")) args = argv[++i];
    else if (!strcmp(argv[i], "--args-add")) {
      static char joined[4096];
      if (snprintf(joined, sizeof joined, "%s %s", args, argv[++i]) >= (int)sizeof joined) { fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n"); return 2; }
      args = joined;
    }
    else if (!strcmp(argv[i], "--swiotlb")) swiotlb_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--image")) image_dir = argv[++i];
    else if (!strcmp(argv[i], "--phone-image")) phone_image = argv[++i];
    else if (!strcmp(argv[i], "--phone-data")) phone_data = argv[++i];
    else if (!strcmp(argv[i], "--display")) { if (sscanf(argv[++i], "%ux%u", &display_w, &display_h) != 2) usage(); }
    else if (!strcmp(argv[i], "--scale")) display_scale = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--hz")) display_hz = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--prime-list")) prime_list_path = argv[++i];
    else if (!strcmp(argv[i], "--gpu-addon")) gpu_addon_path = argv[++i];
    else if (!strcmp(argv[i], "--gpu-intid")) gpu_intid = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--audio-intid")) {
      char *end = NULL;
      audio_intid = (unsigned)strtoul(argv[++i], &end, 0);
      if (!end || *end != ',') { fprintf(stderr, "xnuloader-crosvm: --audio-intid N,SLOT (e.g. 44,8)\n"); return 2; }
      audio_slot = (unsigned)strtoul(end + 1, &end, 0);
      if (*end) { fprintf(stderr, "xnuloader-crosvm: --audio-intid N,SLOT (e.g. 44,8)\n"); return 2; }
    }
    else if (!strcmp(argv[i], "--jetsam-product")) jetsam_product = argv[++i];
    else if (!strcmp(argv[i], "--identity-tree")) identity_path = argv[++i];
    else if (!strcmp(argv[i], "--identity-groups")) identity_list = argv[++i];
    else if (!strcmp(argv[i], "--memory-class")) {
      const char *text = argv[++i];
      memory_class_given = true;
      if (!parse_decimal(text, strlen(text), 0xffff, &memory_class)) memory_class = 0;     // 0 is no class: refused below with the range
    }
    else if (!strcmp(argv[i], "--soc-generation")) { soc_generation = strcmp(argv[i + 1], "none") ? argv[i + 1] : NULL; i++; }
    else if (!strcmp(argv[i], "--display-corner-radius")) corner_text = argv[++i];
    else if (!strcmp(argv[i], "--media-mib")) media_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else usage();
  }
  if (cpus < 1 || cpus > XL_MAX_CPUS || ram_mib < 512 || ram_mib > 6144) { fprintf(stderr, "xnuloader-crosvm: 1-%d CPUs, 512-6144 MiB\n", XL_MAX_CPUS); return 2; }
  // the GPU's buffers are the guest's own pages, which only a shared-RAM VM's host reaches
  if (gpu && !shared_ram) { fprintf(stderr, "xnuloader-crosvm: --gpu needs --shared-ram\n"); return 2; }
  // the sound card's queues and periods are the guest's own pages too
  if (audio && !shared_ram) { fprintf(stderr, "xnuloader-crosvm: --audio needs --shared-ram\n"); return 2; }
  if (audio_intid && (!audio || !image_dir || audio_intid < 32 || audio_intid > 1019 || audio_slot < 1 || audio_slot > 31)) {
    fprintf(stderr, "xnuloader-crosvm: --audio-intid needs --audio and --image, an SPI (32 to 1019) and a slot (1 to 31)\n"); return 2;
  }
  if (wifi && !shared_ram) { fprintf(stderr, "xnuloader-crosvm: --wifi needs --shared-ram\n"); return 2; }
  // the decoder's rings and commands are the guest's own pages, its buffers crosvm's pool: 16 to 1024 MiB, whole 2 MiB folios
  if (media && !shared_ram) { fprintf(stderr, "xnuloader-crosvm: --media needs --shared-ram\n"); return 2; }
  if (media && (media_mib < 16 || media_mib > 1024 || (media_mib & 1))) { fprintf(stderr, "xnuloader-crosvm: --media-mib 16 to 1024, even\n"); return 2; }
  if (gpu_scanout && !display_w) { fprintf(stderr, "xnuloader-crosvm: --gpu-scanout needs --display\n"); return 2; }
  // an SPI of the GIC, for the GPU only (the other PCI functions poll), with --image: OUT.disks carries it to vm-boot's check
  if (gpu_intid && (!gpu || !image_dir || gpu_intid < 32 || gpu_intid > 1019)) { fprintf(stderr, "xnuloader-crosvm: --gpu-intid needs --gpu or --gpu-scanout and --image, an SPI: 32 to 1019\n"); return 2; }
  // a part of the file name launchd reads (com.apple.jetsamproperties.NAME.plist), as the kernel's sysctl gives it
  if (jetsam_product && !xl_machine_jetsam_product_ok(jetsam_product)) {
    fprintf(stderr, "xnuloader-crosvm: --jetsam-product: 1 to 31 of A-Z a-z 0-9 . _ -, not starting with a dot (iPhone18,2's list is V54)\n");
    return 2;
  }
  // the identity options: the groups say which properties of the tree are copied, and with none nothing is
  if (identity_list) {
    char bad[48];
    if (!xl_identity_groups_parse(identity_list, &identity_groups, bad, sizeof bad)) {
      fprintf(stderr, "xnuloader-crosvm: --identity-groups: unknown group \"%s\" (artwork, quality, workgroup, ids, memclass, root, or a to f, or all; comma-separated)\n", bad);
      return 2;
    }
    if (!identity_path) { fprintf(stderr, "xnuloader-crosvm: --identity-groups needs --identity-tree\n"); return 2; }
  }
  if (memory_class_given && !(identity_groups & XL_IDENTITY_MEMCLASS)) {
    fprintf(stderr, "xnuloader-crosvm: --memory-class needs --identity-tree and --identity-groups memclass (or all)\n");
    return 2;
  }
  if (soc_generation && !xl_machine_soc_generation_ok(soc_generation)) {
    fprintf(stderr, "xnuloader-crosvm: --soc-generation: 1 to 31 of A-Z a-z 0-9 . _ -, or none (default none)\n");
    return 2;
  }
  // PT[,WORD2]: whole decimal numbers, PT 0 to 200 points and WORD2 0 to 65535 (default 1, as Apple's iPhone18,2 tree has it)
  if (corner_text) {
    const char *comma = strchr(corner_text, ',');
    if (!parse_decimal(corner_text, comma ? (size_t)(comma - corner_text) : strlen(corner_text), XL_CORNER_RADIUS_MAX, &corner_radius) ||
        (comma && !parse_decimal(comma + 1, strlen(comma + 1), XL_CORNER_WORD2_MAX, &corner_word2))) {
      fprintf(stderr, "xnuloader-crosvm: --display-corner-radius PT[,WORD2]: whole decimal numbers, PT 0 to %u points, WORD2 0 to %u (default 1)\n",
              XL_CORNER_RADIUS_MAX, XL_CORNER_WORD2_MAX);
      return 2;
    }
  }
  // the tree is read and checked here, before anything is written: it must be a device tree with a /product (and, for the root
  // group, a target-type and a compatible), and --memory-class a class its device-perf-memory-class allows
  xl_adt_node *identity = NULL;
  if (identity_path) {
    size_t n = 0;
    uint8_t *data = slurp(identity_path, &n);
    const char *why = n > (16u << 20) ? "more than 16 MiB" : NULL;
    identity = why ? NULL : xl_adt_parse(data, n, &why);
    if (!identity) { fprintf(stderr, "xnuloader-crosvm: --identity-tree %s: not a device tree: %s\n", identity_path, why); return 2; }
    if (!xl_adt_child(identity, "product")) { fprintf(stderr, "xnuloader-crosvm: --identity-tree %s: no /product\n", identity_path); return 2; }
    const xl_adt_prop *target = xl_adt_get(identity, "target-type"), *board = xl_adt_get(identity, "compatible");
    if ((identity_groups & XL_IDENTITY_ROOT) && (!target || !target->length || !board || !board->length || !board->value[0])) {
      fprintf(stderr, "xnuloader-crosvm: --identity-groups root: %s has no target-type or compatible\n", identity_path);
      return 2;
    }
    const xl_adt_prop *apple = xl_adt_get(xl_adt_child(identity, "product"), "device-perf-memory-class");
    if ((identity_groups & XL_IDENTITY_MEMCLASS) && apple && apple->length != 1 && apple->length != 2 && apple->length != 4 && apple->length != 8) {
      fprintf(stderr, "xnuloader-crosvm: --identity-groups memclass: %s's device-perf-memory-class is not an integer of 1, 2, 4 or 8 bytes\n", identity_path);
      return 2;
    }
    const unsigned most = xl_machine_memory_class_max(identity);
    if (memory_class_given && (memory_class < 1 || memory_class > most)) {
      fprintf(stderr, "xnuloader-crosvm: --memory-class: 1 to %u (the tree's own device-perf-memory-class; 16 where it has none)\n", most);
      return 2;
    }
  }
  if (display_w && (!image_dir || display_w < 320 || display_h < 320 || display_w > 4096 || display_h > 4096 ||
                    (display_scale != 2 && display_scale != 3) || display_hz < 24 || display_hz > 120)) {
    fprintf(stderr, "xnuloader-crosvm: --display 320x320 to 4096x4096 with --image, --scale 2 or 3, --hz 24 to 120\n");
    return 2;
  }
  // The boot video's slot at the pool's start (2 MiB aligned), the disks' windows after it.
  // and the frame sequence's page right after it (16 KiB, the guest's page: OneiOSDisplay.cpp oneios_display_frame_done)
  const uint64_t fb_stride = (uint64_t)display_w * 4, fb_bytes = fb_stride * display_h, seq_off = (fb_bytes + 0x3fff) & ~0x3fffull;
  const uint64_t fb_slot = display_w ? (seq_off + 0x4000 + 0x1fffff) & ~0x1fffffull : 0;
  if (!swiotlb_mib) swiotlb_mib = display_w ? 32 : 8;
  // up to 1 GiB: the GPU's buffers would lie there (docs/native.md, the GPU: Android's GPU writes into the pool)
  if (swiotlb_mib < 2 || swiotlb_mib > 1024 || (swiotlb_mib & 1)) { fprintf(stderr, "xnuloader-crosvm: --swiotlb 2 to 1024 MiB, even\n"); return 2; }
  static char with_display[4096];
  if (display_w) {
    // what xnuhost passes beside the tree's display-scale: the backend's unit scale and its vsync tick (display/README.md)
    size_t at = (size_t)snprintf(with_display, sizeof with_display, "%s", args);
    if (at < sizeof with_display && !strstr(args, "offline-legacy-unit-scale="))
      at += (size_t)snprintf(with_display + at, sizeof with_display - at, " offline-legacy-unit-scale=%u", display_scale);
    if (at < sizeof with_display && !strstr(args, "sheng-vsync="))
      at += (size_t)snprintf(with_display + at, sizeof with_display - at, " sheng-vsync=%u", display_hz);
    // backboardd's HID event queue, 256 KiB in place of 16: a full one drops events (hid/README.md, "backboardd's event queue")
    if (at < sizeof with_display && !strstr(args, "oneios-hid-queue-kib="))
      at += (size_t)snprintf(with_display + at, sizeof with_display - at, " oneios-hid-queue-kib=256");
    if (at >= sizeof with_display) { fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n"); return 2; }
    args = with_display;
  }
  if (image_dir && (!plain_path(phone_image) || !plain_path(phone_data))) {
    fprintf(stderr, "xnuloader-crosvm: --phone-image and --phone-data: absolute paths without commas, quotes or blanks\n");
    return 2;
  }

  // The image set's disks: this machine's copy for what the nodes say, the phone's files for crosvm.
  static xh_imageset set;
  static image_disk disks[IMAGE_DISKS_MAX];
  static char seed_hex[65];
  unsigned ndisk = image_dir ? image_disks(image_dir, phone_data, false, &set, disks, seed_hex) : 0;
  xl_blob trust = { 0 };
  if (image_dir) {
    const char *error = NULL;
    trust = xh_imageset_trustcaches(&set, &error);
    if (!trust.data) { fprintf(stderr, "%s: trust caches: %s\n", image_dir, error); return 1; }
  }

  const char *err = NULL;
  size_t size;
  uint8_t *image = slurp(kernel_path, &size);
  static xl_kernel k;
  if (xl_macho_parse(image, size, &k, &err)) { fprintf(stderr, "%s: %s\n", kernel_path, err); return 1; }

  const uint64_t phys = RAM_BASE + KERNEL_ALIGN + (k.base & (KERNEL_ALIGN - 1));
  const uint64_t ram_end = RAM_BASE + ((uint64_t)ram_mib << 20);
  // --gpu: drm2kgsl's host arena at the RAM's top, where that crosvm puts its pools; everything else below it. --media:
  // the decoder's pool after it (crosvm's pool_specs order: drm2kgsl_host, then media_host, each 2 MiB aligned, above the
  // block that --mem less both makes), so the arena moves down by the pool
  const uint64_t media_bytes = media ? (uint64_t)media_mib << 20 : 0, drm_bytes = gpu ? DRM_HOST_MIB << 20 : 0;
  const uint64_t block_end = ram_end - drm_bytes - media_bytes;
  const uint64_t drm_host = gpu ? block_end : 0, media_host = media ? block_end + drm_bytes : 0;
  // crosvm's restricted DMA pool: the RAM's last MIB (aarch64/src/lib.rs get_swiotlb_addr; Gunyah wants it static).
  const uint64_t pool = block_end - ((uint64_t)swiotlb_mib << 20);
  const uint64_t top_reserve = (ram_end - block_end) + ((uint64_t)swiotlb_mib << 20) + FDT_RESERVE;
  if (phys + (256ull << 20) + top_reserve > ram_end) { fprintf(stderr, "xnuloader-crosvm: %u MiB leave the kernel less than 256 MiB\n", ram_mib); return 2; }
  // the input window after the disks', 16 KiB aligned (the guest's page)
  const uint64_t input_off = display_w ? (fb_slot + ndisk * DMA_SLICE + 0x3fff) & ~0x3fffull : 0;
  const uint64_t pool_need = display_w ? input_off + INPUT_WINDOW : fb_slot + ndisk * DMA_SLICE;
  if (pool_need > ((uint64_t)swiotlb_mib << 20)) {
    fprintf(stderr, "xnuloader-crosvm: the boot video, %u disks and the input need %llu KiB of the pool\n", ndisk, (unsigned long long)(pool_need >> 10));
    return 2;
  }
  const uint64_t fb_pa = pool, disks_pa = pool + fb_slot, input_pa = pool + input_off;
  if (display_w) {
    // the page the guest counts its frames in (a new argument: the user's --args cannot have named the pool)
    size_t at = strlen(with_display);
    if (snprintf(with_display + at, sizeof with_display - at, " oneios-frame-seq=0x%llx", (unsigned long long)(fb_pa + seq_off)) >= (int)(sizeof with_display - at)) {
      fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n");
      return 2;
    }
  }
  // --gpu: what xnuhost passes for its GPU (runtime/xnuhost/src/config.c): Turnip asks the kernel whether the virtio GPU is the
  // board's 3D path (DroidVMVirtioGpu3D.cpp answers DROIDVM_VIRTGPU_PARAM_DEFAULT_3D with vgpu-3d; without it Turnip took the
  // native Adreno connection, open type 0x4147, which answered kIOReturnBusy, and every Metal device failed: vm-boot gpu2), and the
  // accelerator of the original Metal discovery (droidvm-gpu-metal)
  static char with_gpu[4096];
  if (gpu) {
    size_t at = (size_t)snprintf(with_gpu, sizeof with_gpu, "%s", args);
    if (at < sizeof with_gpu && !strstr(args, "vgpu-3d="))
      at += (size_t)snprintf(with_gpu + at, sizeof with_gpu - at, " vgpu-3d=1");
    if (at < sizeof with_gpu && !strstr(args, "droidvm-gpu-metal="))
      at += (size_t)snprintf(with_gpu + at, sizeof with_gpu - at, " droidvm-gpu-metal=1");
    if (gpu_scanout && at < sizeof with_gpu && !strstr(args, "oneios-virtio-scanout="))
      at += (size_t)snprintf(with_gpu + at, sizeof with_gpu - at, " oneios-virtio-scanout=1");
    if (at >= sizeof with_gpu) { fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n"); return 2; }
    args = with_gpu;
  }
  // --media: the codec archive starts (the native kernel's default is off) and publishes VideoToolbox's paravirtualization
  // service once the decoder is up (kernel/patches/oneios-hosted/codec/OneiOSCodec.c)
  static char with_media[4096];
  if (media) {
    size_t at = (size_t)snprintf(with_media, sizeof with_media, "%s", args);
    if (at < sizeof with_media && !strstr(args, "oneios-codec="))
      at += (size_t)snprintf(with_media + at, sizeof with_media - at, " oneios-codec=1");
    if (at < sizeof with_media && !strstr(args, "droidvm-vt-paravirt="))
      at += (size_t)snprintf(with_media + at, sizeof with_media - at, " droidvm-vt-paravirt=1");
    if (at >= sizeof with_media) { fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n"); return 2; }
    args = with_media;
  }
  const uint64_t window = (ram_end - top_reserve - phys) & ~0x1fffffull;

  xl_machine m;
  memset(&m, 0, sizeof m);
  m.ncpu = cpus;
  for (unsigned i = 0; i < cpus; i++) { m.cpu_reg[i] = i; m.cpu_cluster[i] = 'P'; }   // crosvm: Aff0 = the vCPU's index
  m.timebase_hz = timebase;
  m.dram_base = phys;
  m.dram_size = window;
  m.armio_base = ARMIO_BASE;
  m.armio_size = ARMIO_SIZE;
  m.gic_dist = GICD;
  m.gic_dist_size = GICD_SIZE;
  m.gic_redist = GICD - GICR_STRIDE * cpus;
  m.gic_redist_size = GICR_STRIDE * cpus;
  m.gic_redist_stride = GICR_STRIDE;
  m.uart_base = UART;
  m.uart_size = UART_SIZE;
  m.uart_ns16550 = true;
  if (bridge) {
    m.bridge_uart_base = BRIDGE_UART;
    m.bridge_uart_size = UART_SIZE;
    printf("xnuloader-crosvm: the bridge: /arm-io/bridge, crosvm's second serial port at 0x%llx (vm-boot: --serial num=2 on a socket, bridgectl serve)\n",
           (unsigned long long)BRIDGE_UART);
  }
  m.psci_method = "hvc";
  m.model = "iPhone18,2";
  m.jetsam_product = jetsam_product;
  // the identity: the groups' source (with no group it copies nothing), which groups, the VM's RAM for the memory class; the SoC's
  // generation and the panel's corner radius, which are the machine's own
  m.identity_product = identity;
  m.identity_groups = identity_groups;
  m.vm_ram_mib = ram_mib;
  m.memory_class = memory_class;
  m.soc_generation = soc_generation;
  if (corner_text) {
    m.has_display_corner_radius = true;
    m.display_corner_radius = corner_radius;
    m.display_corner_word2 = corner_word2;
  }
  m.have_chip_id = true;
  m.chip_id = 0x8150;
  m.unique_chip_id = 0x0ce10511e5000002ull;     // a VM's own: not a phone's, not the bench's
  if (display_w) {
    m.display_width = display_w;
    m.display_height = display_h;
    m.display_scale = display_scale;
    m.display_hz = display_hz;
  }
  FILE *r = fopen("/dev/urandom", "rb");
  if (!r || fread(m.random_seed, 1, sizeof m.random_seed, r) != sizeof m.random_seed) { fprintf(stderr, "/dev/urandom: read failed\n"); return 1; }
  fclose(r);
  // The disks: PCI function 00:(2 + K).0 for disk K, its configuration space the node's reg.
  for (unsigned i = 0; i < ndisk; i++)
    m.dev[m.ndev++] = (xl_device){ disks[i].writable ? XL_DEV_DATA_STORE : XL_DEV_BLOCK,
                                   PCI_CAM + ((uint64_t)(PCI_FIRST_DEV + i) << (shared_ram ? ECAM_DEV_SHIFT : CAM_DEV_SHIFT)), 0x100, true };
  if (prime_list_path) {
    size_t n = 0;
    uint8_t *text = slurp(prime_list_path, &n);
    unsigned lines = 0;
    for (size_t k = 0; k < n; k++) {
      if (!text[k] || (text[k] < 0x20 && text[k] != '\n')) { fprintf(stderr, "xnuloader-crosvm: --prime-list: not a list of paths\n"); return 2; }
      lines += text[k] == '\n';
    }
    if (!n || n > (256u << 10) || text[0] != '/' || text[n - 1] != '\n') {
      fprintf(stderr, "xnuloader-crosvm: --prime-list: 1 byte to 256 KiB of absolute paths, a line each\n");
      return 2;
    }
    m.prime_list = (const char *)text;
    m.prime_list_length = (uint32_t)n;
    printf("xnuloader-crosvm: the shader priming list, %u libraries, in /chosen/oneios-prime-list\n", lines);
  }
  if (gpu) {
    m.dev[m.ndev++] = (xl_device){ XL_DEV_GPU, PCI_CAM + ((uint64_t)GPU_PCI_DEV << ECAM_DEV_SHIFT), 0x1000, true };
    m.gpu_pool_base = drm_host;
    m.gpu_pool_size = DRM_HOST_MIB << 20;
    m.gpu_intid = gpu_intid;
  }
  // the input bus: all of bus 0 on AVF's CAM; on Droid-VM's ECAM the input's device only, one step of the kernel's scan
  if (display_w)
    m.dev[m.ndev++] = shared_ram ? (xl_device){ XL_DEV_INPUT, PCI_CAM + ((uint64_t)ECAM_INPUT_DEV << ECAM_DEV_SHIFT), INPUT_STEP, true }
                                 : (xl_device){ XL_DEV_INPUT, PCI_CAM, PCI_BUS0, true };
  // the sound card: the kernel finds it on bus 0, wherever crosvm put it
  if (audio) {
    m.dev[m.ndev++] = (xl_device){ XL_DEV_SND, PCI_CAM, ECAM_BUS0, true };
    m.audio_intid = audio_intid;
    m.audio_slot = audio_slot;
    printf("xnuloader-crosvm: the sound card: /arm-io/virtio-snd, bus 0 of crosvm's ECAM (vm-boot: --virtio-snd backend=aaudio)\n");
  }
  // the network card: the kernel finds it on bus 0 as well
  if (wifi) {
    m.dev[m.ndev++] = (xl_device){ XL_DEV_NET, PCI_CAM, ECAM_BUS0, true };
    printf("xnuloader-crosvm: the Wi-Fi: /arm-io/net, bus 0 of crosvm's ECAM (vm-boot: --net tap-name=, wifictl tap to wlan0)\n");
  }
  // the video decoder: the kernel finds crosvm's virtio-media function on bus 0 too; its buffers are crosvm's media_host pool
  if (media) {
    m.dev[m.ndev++] = (xl_device){ XL_DEV_MEDIA, PCI_CAM, ECAM_BUS0, true };
    m.media_pool_base = media_host;
    m.media_pool_size = media_bytes;
    printf("xnuloader-crosvm: the video decoder: /arm-io/virtio-media-0, bus 0 of crosvm's ECAM, media-host-pool %u MiB at 0x%llx"
           " (vm-boot --video-decoder: --virtio-media kind=decoder, --pre-alloc media-host-mb=%u)\n", media_mib,
           (unsigned long long)media_host, media_mib);
  }
  const xh_imageset_file *os = image_dir ? xh_imageset_role(&set, "os") : NULL;
  if (os && *os->volume_uuid) m.root_volume_uuid = os->volume_uuid;
  m.trust_cache = trust.data != NULL;
  xl_blob addon = { 0 };
  if (gpu_addon_path) {
    size_t n = 0;
    uint8_t *data = slurp(gpu_addon_path, &n);
    uint64_t declared = 0;
    if (data && n >= 24) memcpy(&declared, data + 16, 8);
    if (!data || n < 24 || n > (64u << 20) || memcmp(data, "NGPUADD2", 8) || declared != n) {
      fprintf(stderr, "xnuloader-crosvm: --gpu-addon: %s is not a GPU addon (NGPUADD2, its own size, at most 64 MiB)\n", gpu_addon_path);
      return 2;
    }
    addon.data = data;
    addon.size = n;
    m.gpu_addon = true;
    printf("xnuloader-crosvm: the GPU addon %s, %zu bytes, in /chosen/memory-map GpuAddon (the kernel mounts it over the seed's plug-in)\n",
           gpu_addon_path, n);
  }
  xl_adt_node *tree = xl_machine_tree(&m, &err);
  if (!tree) { fprintf(stderr, "board: %s\n", err); return 1; }
  for (unsigned i = 0, blocks = 0; i < ndisk; i++) {
    char name[40];
    if (disks[i].writable) snprintf(name, sizeof name, "arm-io/data-store");
    else snprintf(name, sizeof name, "arm-io/virtio-pci-%u", blocks++);
    xl_adt_node *node = xl_adt_find(tree, name);
    if (!node) { fprintf(stderr, "board: no /%s\n", name); return 1; }
    xh_imageset_describe(node, &disks[i].identity);
    const uint64_t dma[2] = { disks_pa + i * DMA_SLICE, DMA_SLICE };
    if ((disks[i].list && xl_adt_set(node, "oneios,hash-list", disks[i].list, disks[i].list_size)) ||
        xl_adt_set(node, "oneios,dma-window", dma, sizeof dma)) {
      fprintf(stderr, "board: /%s: no room for its hash list or DMA window\n", name);
      return 1;
    }
  }
  if (display_w) {
    xl_adt_node *bus = xl_adt_find(tree, "arm-io/input-pci");
    const uint64_t window[2] = { input_pa, INPUT_WINDOW };
    if (!bus || xl_adt_set(bus, "oneios,dma-window", window, sizeof window)) { fprintf(stderr, "board: no /arm-io/input-pci for the touch\n"); return 1; }
  }
  // The calendar: crosvm's PL030, absolute (below the arm-io window), for kernel patch 0078's getGMTTimeOfDay.
  {
    xl_adt_node *chosen = xl_adt_find(tree, "chosen");
    const uint64_t rtc[2] = { RTC, RTC_SIZE };
    if (!chosen || xl_adt_set(chosen, "oneios,rtc", rtc, sizeof rtc)) { fprintf(stderr, "board: no /chosen for the clock\n"); return 1; }
  }
  // The console: the node /defaults serial-device names by its AAPL,phandle (pe_serial.c get_serial_device_phandle).
  xl_adt_node *uart = xl_adt_find(tree, "arm-io/uart0"), *defaults = xl_adt_find(tree, "defaults");
  if (!uart || !defaults || xl_adt_set_u32(uart, "AAPL,phandle", 0x10001) || xl_adt_set_u32(defaults, "serial-device", 0x10001)) {
    fprintf(stderr, "board: no /arm-io/uart0 or /defaults for the console\n");
    return 1;
  }

  uint8_t *ram = calloc(1, window);
  if (!ram) { fprintf(stderr, "xnuloader-crosvm: out of memory\n"); return 1; }
  xl_boot_request req = { .image = { image, size }, .kernel = &k, .ram = ram, .window = window, .phys_base = phys,
                          .virt_base = k.base, .tree = tree, .command_line = args, .trustcache = trust, .gpu_addon = addon,
                          .video = { display_w ? fb_pa : 0, display_w ? 1 : 0, fb_stride, display_w, display_h, display_w ? 32 : 0 } };
  xl_boot_layout lay;
  if (xl_boot_place(&req, &lay, &err)) { fprintf(stderr, "place: %s\n", err); return 1; }
  const uint64_t used = (lay.used + 0x3fff) & ~0x3fffull;

  // The Image: [the header and the stub][zeros][the window at phys - the Image's base]
  const uint64_t image_base = shared_ram ? RAM_BASE + SHIM_REGION : RAM_BASE;
  const uint64_t at = phys - image_base, total = at + used;
  uint8_t head[0x60];
  memset(head, 0, sizeof head);
  put32(head + 0x00, 0x14000010u);              // code0: b 0x40
  put64(head + 0x08, 0);                        // text_offset
  put64(head + 0x10, total);                    // image_size
  put64(head + 0x18, 0x2);                      // flags: little-endian, 4 KiB pages, 2 MiB-aligned base at DRAM's start
  memcpy(head + 0x38, "ARM\x64", 4);            // magic
  // 0x40: msr daifset, #0xf; ldr x0, [pc, #12]; ldr x1, [pc, #16]; br x1; then boot_args' and the entry's physical
  // addresses at 0x50 and 0x58, 8-byte aligned (MMU off: every load is to Device memory, which takes no unaligned one)
  put32(head + 0x40, 0xd5034fdfu);
  put32(head + 0x44, 0x58000060u);
  put32(head + 0x48, 0x58000081u);
  put32(head + 0x4c, 0xd61f0020u);
  put64(head + 0x50, lay.args_pa);
  put64(head + 0x58, lay.entry_pa);

  // At 0x100 a label of the VM this Image is made for, which `oneios-tool phone vm-boot` reads and starts crosvm with:
  // the window was cut for that much RAM, that many vCPUs and that pool
  char label[128];
  int label_len = snprintf(label, sizeof label, "oneios-crosvm ram-mib=%u cpus=%u swiotlb-mib=%u%s%s", ram_mib, cpus, swiotlb_mib,
                           shared_ram ? " shared-ram=1" : "", gpu ? " gpu=1" : "");
  if (label_len < 0 || (size_t)label_len >= sizeof label) return 1;

  FILE *f = fopen(out, "wb");
  if (!f) { fprintf(stderr, "%s: %s\n", out, strerror(errno)); return 1; }
  static uint8_t zeros[1 << 16];
  if (fwrite(head, 1, sizeof head, f) != sizeof head) goto write_failed;
  if (fwrite(zeros, 1, 0x100 - sizeof head, f) != 0x100 - sizeof head || fwrite(label, 1, (size_t)label_len + 1, f) != (size_t)label_len + 1)
    goto write_failed;
  for (uint64_t left = at - 0x100 - (uint64_t)label_len - 1; left; ) {
    size_t n = left > sizeof zeros ? sizeof zeros : (size_t)left;
    if (fwrite(zeros, 1, n, f) != n) goto write_failed;
    left -= n;
  }
  if (fwrite(ram, 1, used, f) != used || fclose(f)) goto write_failed;
  {
    char adt[4096];
    snprintf(adt, sizeof adt, "%s.adt", out);
    FILE *a = fopen(adt, "wb");
    if (!a || fwrite(ram + (lay.tree_pa - phys), 1, lay.tree_size, a) != lay.tree_size || fclose(a)) { fprintf(stderr, "%s: write failed\n", adt); return 1; }
  }
  if (ndisk) {
    // OUT.disks for vm-boot: the pool crosvm's tree must name, the store to make, and crosvm's --block values.
    char list[4096];
    snprintf(list, sizeof list, "%s.disks", out);
    FILE *d = fopen(list, "w");
    if (!d) { fprintf(stderr, "%s: %s\n", list, strerror(errno)); return 1; }
    fprintf(d, "swiotlb-mib %u\npool 0x%llx\nstore %s %u\n", swiotlb_mib, (unsigned long long)pool, phone_data, set.store_gib ? set.store_gib : 16);
    if (display_w)
      fprintf(d, "fb %llu %llu %u %u %llu %llu\n", (unsigned long long)(fb_pa - pool), (unsigned long long)fb_bytes, display_w, display_h,
              (unsigned long long)fb_stride, (unsigned long long)(fb_pa - pool + seq_off));
    if (display_w) fprintf(d, "input touch\n");
    if (gpu) fprintf(d, "gpu 00:%02x.0 %llu 0x%llx\n", GPU_PCI_DEV, (unsigned long long)DRM_HOST_MIB, (unsigned long long)drm_host);
    if (gpu_scanout) fprintf(d, "scanout %u %u %u\n", display_w, display_h, display_hz);
    if (gpu_intid) fprintf(d, "gpu-intid %u\n", gpu_intid);
    if (audio) fprintf(d, "audio\n");
    if (audio_intid) fprintf(d, "audio-intid %u %u\n", audio_intid, audio_slot);
    if (wifi) fprintf(d, "wifi\n");
    if (media) fprintf(d, "media %u 0x%llx\n", media_mib, (unsigned long long)media_host);
    if (bridge) fprintf(d, "bridge\n");
    for (unsigned i = 0; i < ndisk; i++) {
      char file[1200];
      if (disks[i].writable) snprintf(file, sizeof file, "%s", phone_data);
      else snprintf(file, sizeof file, "%s/%s", phone_image, disks[i].rel);
      if (!plain_path(file)) { fprintf(stderr, "%s: a path crosvm or the phone's shell would split\n", file); return 1; }
      if (disks[i].writable) fprintf(d, "block - path=%s,ro=false,id=%s,pci-address=00:%02x.0\n", file, disks[i].identity.role, PCI_FIRST_DEV + i);
      else fprintf(d, "block %llu path=%s,ro=true,id=%s,pci-address=00:%02x.0\n", (unsigned long long)disks[i].size, file,
                   disks[i].identity.role, PCI_FIRST_DEV + i);
    }
    if (fclose(d)) { fprintf(stderr, "%s: write failed\n", list); return 1; }
  }

  printf("xnuloader-crosvm: %s %s, base 0x%llx, entry 0x%llx\n", k.fileset ? "MH_FILESET" : "MH_EXECUTE", k.cpusubtype == 2 ? "arm64e" : "arm64",
         (unsigned long long)k.base, (unsigned long long)k.entry);
  printf("  Image %s: %llu MiB, loaded at PA 0x%llx; stub at 0x%llx: boot_args PA 0x%llx, entry PA 0x%llx\n", out,
         (unsigned long long)(total >> 20), (unsigned long long)image_base, (unsigned long long)(image_base + 0x40),
         (unsigned long long)lay.args_pa, (unsigned long long)lay.entry_pa);
  printf("  window at PA 0x%llx (%llu MiB of the VM's %u), %llu MiB used; device tree PA 0x%llx (%llu bytes)\n",
         (unsigned long long)phys, (unsigned long long)(window >> 20), ram_mib, (unsigned long long)(lay.used >> 20),
         (unsigned long long)lay.tree_pa, (unsigned long long)lay.tree_size);
  printf("  %u vCPU%s, timebase %u Hz, GICD 0x%llx, GICR 0x%llx, console ns16550a at 0x%llx; boot args: %s\n", cpus,
         cpus == 1 ? "" : "s", timebase, (unsigned long long)GICD, (unsigned long long)m.gic_redist, (unsigned long long)UART, args);
  if (shared_ram)
    printf("  shared RAM (Droid-VM's crosvm, --protected-vm-pseudo-unprotected): the devices' region, %u MiB of plain RAM, at PA 0x%llx;"
           " PCI on its ECAM (the disks at 0x%llx + device << 15)\n", swiotlb_mib, (unsigned long long)pool, (unsigned long long)PCI_CAM);
  else
    printf("  restricted DMA pool (crosvm --swiotlb %u) at PA 0x%llx\n", swiotlb_mib, (unsigned long long)pool);
  if (gpu)
    printf("  GPU: virtio-gpu at 00:%02x.0 (crosvm --gpu virglrenderer, drm2kgsl), drm2kgsl's host arena %llu MiB at PA 0x%llx\n",
           GPU_PCI_DEV, (unsigned long long)DRM_HOST_MIB, (unsigned long long)drm_host);
  if (gpu_intid)
    printf("  GPU interrupt: INTID %u (crosvm's SPI %u) as /arm-io/virtio-gpu droidvm,intid: its waits sleep on it (vm-boot checks crosvm's interrupt-map)\n",
           gpu_intid, gpu_intid - 32);
  if (audio_intid)
    printf("  sound card interrupt: INTID %u (crosvm's SPI %u) for the card at 00:%02x.0, as /arm-io/virtio-snd droidvm,intid: its control replies by it with"
           " oneios-audio-irq=1 (vm-boot checks crosvm's interrupt-map)\n", audio_intid, audio_intid - 32, audio_slot);
  if (media)
    printf("  video decoder: crosvm's media_host pool %u MiB at PA 0x%llx (the RAM's last %u MiB%s); boot args oneios-codec=1 droidvm-vt-paravirt=1\n",
           media_mib, (unsigned long long)media_host, media_mib, gpu ? ", the GPU's arena right below it" : "");
  if (jetsam_product)
    printf("  jetsam properties: /product jetsam-properties-product-type \"%s\": launchd reads /System/Library/LaunchDaemons/com.apple.jetsamproperties.%s.plist"
           " (the OS image must have it)\n", jetsam_product, jetsam_product);
  else
    printf("  jetsam properties: none named (no --jetsam-product): launchd finds no list for this machine, every daemon keeps its default band\n");
  // what the identity options wrote, read back from the tree the Image carries (which group took which /product property is the
  // library's: xl_identity_group_of)
  {
    const xl_adt_node *product = xl_adt_find(tree, "product");
    if (!identity)
      printf("  identity tree: none (no --identity-tree): no group can be on\n");
    else if (!identity_groups)
      printf("  identity tree: %s, read; no --identity-groups: nothing is copied from it\n", identity_path);
    else
      printf("  identity tree: %s, groups %s (--identity-groups)\n", identity_path, identity_list);
    for (unsigned i = 0; i < XL_IDENTITY_GROUPS; i++) {
      const unsigned group = 1u << i;
      printf("  identity group %s (%c): ", xl_identity_group_name(group), 'a' + (int)i);
      if (!(identity_groups & group)) { printf("off\n"); continue; }
      if (group == XL_IDENTITY_ROOT) {
        const xl_adt_prop *model = xl_adt_get(tree, "model"), *target = xl_adt_get(tree, "target-type"), *board = xl_adt_get(tree, "compatible");
        const char *first = board ? (const char *)board->value : "";
        printf("on: root target-type \"%s\", compatible \"%s\" then \"%s\" (the tree's first entry, its board name, and the machine's own), model \"%s\" as without the group\n",
               target ? (const char *)target->value : "", first, board ? first + strlen(first) + 1 : "", model ? (const char *)model->value : "");
      } else if (group == XL_IDENTITY_MEMCLASS) {
        const xl_adt_prop *wrote = xl_adt_get(product, "device-perf-memory-class");
        bool found;
        const uint64_t klass = product_integer(tree, "device-perf-memory-class", &found);
        if (!found)
          printf("on: nothing written (the tree has no device-perf-memory-class and there is no --memory-class)\n");
        else if (memory_class_given)
          printf("on: device-perf-memory-class %llu in %u bytes (--memory-class, 1 to %u)\n", (unsigned long long)klass, wrote->length,
                 xl_machine_memory_class_max(identity));
        else
          printf("on: device-perf-memory-class %llu in %u bytes (the VM's %u MiB: its whole GiB, from 2 to %u)\n", (unsigned long long)klass,
                 wrote->length, ram_mib, xl_machine_memory_class_max(identity));
      } else {
        char names[1024];
        const unsigned count = group_written(tree, group, names, sizeof names);
        const xl_adt_prop *gamut = group == XL_IDENTITY_ARTWORK ? xl_adt_get(product, "artwork-display-gamut") : NULL;
        if (count) printf("on: %u from the tree (%s)", count, names);
        else printf("on: nothing from the tree (it has none of them)");
        if (gamut) printf(", and artwork-display-gamut \"%s\" (the machine's)", (const char *)gamut->value);
        printf("\n");
      }
    }
    const xl_adt_prop *soc = xl_adt_get(xl_adt_find(tree, "arm-io"), "soc-generation");
    if (soc)
      printf("  soc-generation: /arm-io soc-generation \"%s\" (--soc-generation)\n", (const char *)soc->value);
    else
      printf("  soc-generation: none written (no --soc-generation)\n");
    bool corner;
    const uint64_t words = product_integer(tree, "display-corner-radius", &corner);
    if (corner)
      printf("  display corner radius: /product display-corner-radius u32 %u, u32 %u (--display-corner-radius PT[,WORD2]; Apple's iPhone18,2 has 62 and 1; the second word's meaning is not verified)\n",
             (unsigned)(words & 0xffffffffu), (unsigned)(words >> 32));
    else
      printf("  display corner radius: none written (no --display-corner-radius)\n");
  }
  if (display_w)
    printf("  display: %ux%u @%ux, %u Hz; the boot video at PA 0x%llx (the pool's start, %llu bytes, %llu a row); the disks' windows from 0x%llx\n",
           display_w, display_h, display_scale, display_hz, (unsigned long long)fb_pa, (unsigned long long)fb_bytes,
           (unsigned long long)fb_stride, (unsigned long long)disks_pa);
  if (display_w)
    printf("  touch: crosvm's virtio-input functions on PCI bus 0 (the kernel finds them), their pages in the pool at 0x%llx (%llu KiB)\n",
           (unsigned long long)input_pa, (unsigned long long)(INPUT_WINDOW >> 10));
  if (trust.data) printf("  trust caches: %u modules, %zu bytes; root: the OS volume %s\n", (unsigned)trust.data[0], trust.size, m.root_volume_uuid);
  for (unsigned i = 0; i < ndisk; i++)
    printf("  disk %u (PCI 00:%02x.0, config 0x%llx, DMA window 0x%llx+0x%llx): %s, %s%s%s, %s%s\n", i, PCI_FIRST_DEV + i,
           (unsigned long long)(PCI_CAM + ((uint64_t)(PCI_FIRST_DEV + i) << 11)), (unsigned long long)(disks_pa + i * DMA_SLICE),
           (unsigned long long)DMA_SLICE, disks[i].identity.role, disks[i].writable ? phone_data : phone_image,
           disks[i].writable ? "" : "/", disks[i].writable ? "" : disks[i].rel, disks[i].writable ? "writable" : "read-only",
           disks[i].list ? ", hash list in its node" : "");
  return 0;
write_failed:
  fprintf(stderr, "%s: write failed\n", out);
  return 1;
}
