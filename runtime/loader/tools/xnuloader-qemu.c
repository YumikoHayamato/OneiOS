// SPDX-License-Identifier: MIT
// xnuloader-qemu: a kernel placed for qemu-system-aarch64's virt board the way a firmware loader (DarwinBoot) places it
// on a phone: EL1, MMU and caches off, at the physical address of its entry, x0 = the physical address of boot_args.
// The bench for the native kernel (kernel/build-native.sh; the plain one of kernel/build-linux.sh too) before any phone
// (docs/native.md).
//   xnuloader-qemu KERNEL OUTDIR [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--qemu-cpu M]
//                  [--image DIR --data FILE] [--jetsam-product NAME]
// Writes into OUTDIR:
//   window.bin  the kernel's window from its physical base: its segments, the device tree, boot_args and the trust
//               caches (xl_boot_place, the code xnuhost uses, with virt_base = the kernel's own base as a firmware gives it)
//   stub.bin    2 MiB into RAM: interrupts masked, x0 = boot_args, a branch to the entry
//   board.adt   the device tree the kernel was given (xnuloader-inspect adt FILE prints it)
//   qemu.args   the machine, the two loader devices and the disks, one argument per line (scripts/qemu/boot-vm.sh)
// and prints the layout. The board: virt with GICv3 (distributor 0x08000000, redistributors 0x080a0000, 0x20000 a
// CPU), the PL011 at 0x09000000, PSCI by HVC (the machine has no EL2 or EL3), Cortex-A710 CPUs (--qemu-cpu: why).
// --image DIR: the image set (docs/image-format.md), read as xnuhost reads it (runtime/xnuhost/src/imageset.c: the
// manifest, each file's SHA-256, the hash lists, the trust caches). Its disks go on virt's virtio-mmio transports (modern
// virtio, transport K at 0x0a000000 + 0x200 * K): os, systemos and app read-only, the Data seed read-only, and the Data
// store FILE (--data, a raw file the kernel formats on its first start) writable, each node with the identity
// xnuhost gives it. A read-only image's node also carries its hash list ("oneios,hash-list"): the native kernel's
// OneiOSHostedDisk checks every chunk it reads against it, as xnuhost does for a hosted kernel. /chosen root-matching
// names the OS volume; the trust caches go where the hosted kernel's do.
// --pci: the disks as virtio-blk PCI functions instead (QEMU's virtio-blk-pci on virt's PCIe host, 00:02.0 to 00:06.0),
// the bench of the crosvm VM's disks (xnuloader-crosvm, docs/native.md "A VM under Gunyah"): each node is the function's
// configuration space in virt's ECAM, with a window for the BAR the kernel places (virt has no firmware to assign it)
// and a DMA window, as crosvm's restricted DMA pool gives one: everything the device touches lies there, after the
// window and the boot video, outside the RAM the kernel manages.
// --jetsam-product NAME (default none): /product jetsam-properties-product-type, as xnuloader-crosvm gives it (its header
// comment: the name of the jetsam properties launchd reads, V54 for the iPhone18,2 set), so that the list can be tried
// on this bench before a phone.
// --media (take18b): the video decoder's node as xnuloader-crosvm --media gives it, /arm-io/virtio-media-0
// ("oneios,virtio-media-pci") = bus 0 of virt's ECAM, and the boot arguments oneios-codec=1 droidvm-vt-paravirt=1. QEMU has
// no virtio-media device: the bench boots the kernel's codec archive with its PCI scan and no decoder (serial.log:
// `droidvm-media: no 1af4:1070 function on bus 0`, `ONEIOS CODEC: 1 virtio-media device(s), 0 up; ... not published`).
// No media-host-pool (nothing maps a buffer without a device).
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

#define RAM_BASE      0x40000000ull     // virt's DRAM
#define STUB_PA       (RAM_BASE + 0x200000) // past the FDT QEMU writes at the start of RAM (1 MiB) for a bare image
#define KERNEL_ALIGN  (32ull << 20)     // the kernel's VA and PA agree modulo 32 MiB: 16K's L2 blocks, and 4K's 2 MiB ones
#define VIRTIO_BASE   0x0a000000ull     // virt's 32 virtio-mmio transports; QEMU's bus virtio-mmio-bus.K is transport K
#define VIRTIO_SIZE   0x200ull
#define VIRTIO_INTID  48u               // transport K's line: SPI 16 + K (the driver polls; the node says it all the same)
#define PCI_ECAM      0x3f000000ull     // virt's PCIe ECAM with highmem=off: bus << 20 | device << 15 | function << 12
#define PCI_FIRST_DEV 2u                // disk K is 00:(2 + K).0, as xnuloader-crosvm pins them in crosvm
#define PCI_BARS      0x10000000ull     // virt's 32-bit PCI memory window: 1 MiB of it for each disk's BAR
#define PCI_BAR_SLOT  0x100000ull
#define DMA_SLICE     0x110000ull       // a disk's DMA window: its queue page, then 1 MiB of transfer buffer at 64 KiB
#define DMA_POOL      (8ull << 20)      // the pool of all of them, as crosvm's --swiotlb 8

// The machine's own arguments from xnuhost's default (runtime/xnuhost/src/config.c), less the hosted kernel's devices.
static const char default_args[] =
  "debug=0x14e serial=3 -v kt_fixed_zones=120 -no_slto_panic ml-timeout-lock-panic-scale=40 tlto_us=0 "
  "sheng-smp=1 armcpu-self-ipi=1 armcpu-spi-ipi=1 use_contiguous_hint=0 interrupt-masked-debug-mode=0 "
  "sched_preemption_disable_debug_mode=0 wdt=-1";

static uint8_t *slurp(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  uint8_t *data = malloc(n > 0 ? (size_t)n : 1);
  if (n < 0 || !data || fread(data, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s: read failed\n", path); exit(1); }
  fclose(f);
  *size = (size_t)n;
  return data;
}

static void spit(const char *dir, const char *name, const void *data, size_t size)
{
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(data, 1, size, f) != size || fclose(f)) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); exit(1); }
}

static void usage(void)
{
  fprintf(stderr, "usage: xnuloader-qemu KERNEL OUTDIR [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--qemu-cpu M] [--image DIR --data FILE [--pci]] [--display WxH [--scale 2|3] [--hz N]] [--input tablet,keyboard] [--audio] [--media] [--jetsam-product NAME]\n");
  exit(2);
}

int main(int argc, char **argv)
{
  if (argc < 3) usage();
  const char *kernel_path = argv[1], *out = argv[2], *args = default_args, *image_dir = NULL, *data_path = NULL;
  const char *jetsam_product = NULL;
  unsigned cpus = 1, ram_mib = 1024;
  // The display (--display WxH): QEMU's virtio-gpu-device on the transport after the disks, the boot video right after the
  // kernel's window, the mode and scale in the tree as xnuhost gives them (docs/booter.md 2.8). 0: no display.
  unsigned display_w = 0, display_h = 0, display_scale = 2, display_hz = 60;
  // Input (--input tablet,keyboard): QEMU's virtio-tablet-device (an absolute pointer: Droid-VM's virtio-input transport
  // takes it as one finger while its button is down, NativeHidInput's touch screen) and virtio-keyboard-device, on the
  // transports after the GPU, as /arm-io/input-N ("droidvm,virtio-input"). The pointer is the VNC client's mouse.
  bool input_tablet = false, input_keyboard = false;
  // The sound card (--audio; take17): QEMU's virtio-sound-device (one output and one input stream, audiodev none: what is
  // played is dropped, what is recorded is silence at the stream's rate) on the transport after the input devices, as
  // /arm-io/virtio-snd ("droidvm,virtio-snd"): the bench of the kernel's virtio-snd transport and its microphone
  // (DroidVMVirtioSnd.c as staged; boot arguments oneios-audio-mic=1 oneios-audio-mic-selftest=N, OneiOSAudio.c).
  bool audio = false;
  // The CPU: a Cortex-A710 (Armv9.0, PAuth, BTI, 64-byte DC ZVA, no FEAT_LVA), the kind of core a phone has, not QEMU's
  // max: max has FEAT_LVA, and the DEVELOPMENT kernel asserts that no CPU has it (vm_sanitize.c:643, launchd's first
  // jobs, 2026-11-20). No SVE (Apple's cores have none; neither has the S25's Oryon). Its counter at 1 GHz, as the tree's
  // timebase says (--timebase and cntfrq= go together).
  const char *qemu_cpu = "cortex-a710,pauth-impdef=on,sve=off,cntfrq=1000000000";
  uint32_t timebase = 1000000000;
  bool pci = false, media = false;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--pci")) { pci = true; continue; }
    if (!strcmp(argv[i], "--audio")) { audio = true; continue; }
    if (!strcmp(argv[i], "--media")) { media = true; continue; }
    if (i + 1 >= argc) usage();
    if (!strcmp(argv[i], "--cpus")) cpus = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--qemu-cpu")) qemu_cpu = argv[++i];
    else if (!strcmp(argv[i], "--ram-mib")) ram_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--timebase")) timebase = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--args")) args = argv[++i];
    else if (!strcmp(argv[i], "--args-add")) {
      // more arguments after the defaults (or after --args, whichever came first)
      static char joined[4096];
      if (snprintf(joined, sizeof joined, "%s %s", args, argv[++i]) >= (int)sizeof joined) { fprintf(stderr, "xnuloader-qemu: boot arguments too long\n"); return 2; }
      args = joined;
    }
    else if (!strcmp(argv[i], "--image")) image_dir = argv[++i];
    else if (!strcmp(argv[i], "--data")) data_path = argv[++i];
    else if (!strcmp(argv[i], "--display")) {
      if (sscanf(argv[++i], "%ux%u", &display_w, &display_h) != 2) usage();
    }
    else if (!strcmp(argv[i], "--scale")) display_scale = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--hz")) display_hz = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--jetsam-product")) jetsam_product = argv[++i];
    else if (!strcmp(argv[i], "--input")) {
      const char *list = argv[++i];
      input_tablet = strstr(list, "tablet") != NULL;
      input_keyboard = strstr(list, "keyboard") != NULL;
      if (!input_tablet && !input_keyboard) usage();
    }
    else usage();
  }
  if (display_w && (display_w < 320 || display_h < 320 || display_w > 4096 || display_h > 4096 || (display_scale != 2 && display_scale != 3) ||
                    display_hz < 24 || display_hz > 120)) {
    fprintf(stderr, "xnuloader-qemu: --display 320x320 to 4096x4096, --scale 2 or 3, --hz 24 to 120\n");
    return 2;
  }
  if (cpus < 1 || cpus > XL_MAX_CPUS || ram_mib < 256 || ram_mib > 3072) { fprintf(stderr, "xnuloader-qemu: 1-%d CPUs, 256-3072 MiB\n", XL_MAX_CPUS); return 2; }
  if (!image_dir != !data_path) { fprintf(stderr, "xnuloader-qemu: --image and --data go together\n"); return 2; }
  if (jetsam_product && !xl_machine_jetsam_product_ok(jetsam_product)) {
    fprintf(stderr, "xnuloader-qemu: --jetsam-product: 1 to 31 of A-Z a-z 0-9 . _ -, not starting with a dot (iPhone18,2's list is V54)\n");
    return 2;
  }
  if (pci && !image_dir) { fprintf(stderr, "xnuloader-qemu: --pci is for the image set's disks (--image)\n"); return 2; }
  static xh_imageset set;
  static image_disk disks[IMAGE_DISKS_MAX];
  static char seed_hex[65];
  unsigned ndisk = image_dir ? image_disks(image_dir, data_path, true, &set, disks, seed_hex) : 0;
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

  uint64_t window = (uint64_t)ram_mib << 20;
  uint64_t phys = RAM_BASE + KERNEL_ALIGN + (k.base & (KERNEL_ALIGN - 1));
  xl_machine m;
  memset(&m, 0, sizeof m);
  m.ncpu = cpus;
  for (unsigned i = 0; i < cpus; i++) { m.cpu_reg[i] = i; m.cpu_cluster[i] = 'P'; }   // virt: Aff0 = the CPU's index
  m.timebase_hz = timebase;
  m.dram_base = phys;
  m.dram_size = window;
  m.armio_base = XL_BOARD_ARMIO_BASE;
  m.armio_size = XL_BOARD_ARMIO_SIZE;
  m.gic_dist = 0x08000000ull;
  m.gic_dist_size = 0x10000ull;
  m.gic_redist = 0x080a0000ull;
  m.gic_redist_size = 0x20000ull * cpus;
  m.gic_redist_stride = 0x20000ull;
  m.uart_base = 0x09000000ull;
  m.uart_size = 0x1000ull;
  m.psci_method = "hvc";
  m.model = "iPhone18,2";
  m.jetsam_product = jetsam_product;
  m.have_chip_id = true;
  m.chip_id = 0x8150;
  m.unique_chip_id = 0x0ce10511e5000001ull;     // a bench's own: not a phone's
  FILE *r = fopen("/dev/urandom", "rb");
  if (!r || fread(m.random_seed, 1, sizeof m.random_seed, r) != sizeof m.random_seed) { fprintf(stderr, "/dev/urandom: read failed\n"); return 1; }
  fclose(r);
  // The disks: transport K for disk K, read-only ones as virtio,mmio nodes (virtio-mmio-0..), the store as data-store;
  // with --pci PCI function 00:(2 + K).0 for disk K (virtio-pci-0.., data-store), no virtio-mmio transport.
  for (unsigned i = 0; i < ndisk; i++)
    m.dev[m.ndev++] = pci ? (xl_device){ disks[i].writable ? XL_DEV_DATA_STORE : XL_DEV_BLOCK, PCI_ECAM + ((uint64_t)(PCI_FIRST_DEV + i) << 15), 0x1000, true }
                          : (xl_device){ disks[i].writable ? XL_DEV_DATA_STORE : XL_DEV_BLOCK, VIRTIO_BASE + i * VIRTIO_SIZE, VIRTIO_SIZE, false };
  const unsigned disk_transports = pci ? 0 : ndisk;
  // The display: /arm-io/virtio-gpu on the next transport (Droid-VM's transport maps it and takes "droidvm,intid"), and
  // the boot video, which the display driver presents until the compositor's first frame, after the window (as xnuhost
  // puts it after the window and its pools: not RAM the kernel manages).
  const unsigned gpu_transport = disk_transports;
  const uint64_t fb_pa = phys + window, fb_bytes = display_w ? ((uint64_t)display_w * display_h * 4 + 0x1fffff) & ~0x1fffffull : 0;
  // --pci: the DMA pool after the boot video (a slice per disk), as crosvm's restricted DMA pool sits after a VM's RAM.
  const uint64_t pool_pa = (fb_pa + fb_bytes + 0xffff) & ~0xffffull, pool_bytes = pci ? DMA_POOL : 0;
  if (pci && ndisk * DMA_SLICE > DMA_POOL) { fprintf(stderr, "xnuloader-qemu: %u disks do not fit the DMA pool\n", ndisk); return 2; }
  if (display_w) {
    m.dev[m.ndev++] = (xl_device){ XL_DEV_GPU, VIRTIO_BASE + gpu_transport * VIRTIO_SIZE, VIRTIO_SIZE, false };
    m.display_width = display_w;
    m.display_height = display_h;
    m.display_scale = display_scale;
    m.display_hz = display_hz;
    // What xnuhost passes beside the tree's display-scale: the backend's unit scale and its vsync tick (display/README.md).
    static char with_display[4096];
    size_t at = (size_t)snprintf(with_display, sizeof with_display, "%s", args);
    if (at < sizeof with_display && !strstr(args, "offline-legacy-unit-scale="))
      at += (size_t)snprintf(with_display + at, sizeof with_display - at, " offline-legacy-unit-scale=%u", display_scale);
    if (at < sizeof with_display && !strstr(args, "sheng-vsync="))
      at += (size_t)snprintf(with_display + at, sizeof with_display - at, " sheng-vsync=%u", display_hz);
    if (at >= sizeof with_display) { fprintf(stderr, "xnuloader-qemu: boot arguments too long\n"); return 2; }
    args = with_display;
  }
  // --media: the codec archive starts and would publish VideoToolbox's service with a decoder up (none here)
  if (media) {
    static char with_media[4096];
    size_t at = (size_t)snprintf(with_media, sizeof with_media, "%s", args);
    if (at < sizeof with_media && !strstr(args, "oneios-codec="))
      at += (size_t)snprintf(with_media + at, sizeof with_media - at, " oneios-codec=1");
    if (at < sizeof with_media && !strstr(args, "droidvm-vt-paravirt="))
      at += (size_t)snprintf(with_media + at, sizeof with_media - at, " droidvm-vt-paravirt=1");
    if (at >= sizeof with_media) { fprintf(stderr, "xnuloader-qemu: boot arguments too long\n"); return 2; }
    args = with_media;
    m.dev[m.ndev++] = (xl_device){ XL_DEV_MEDIA, PCI_ECAM, 0x100000, true };
  }
  // The input devices on the transports after the GPU's (or the disks', without a display): input-0 the tablet, then the
  // keyboard, in that order (xl_machine_tree numbers the input nodes as it meets them).
  unsigned next_transport = disk_transports + (display_w ? 1 : 0), tablet_transport = 0, keyboard_transport = 0, ninput = 0;
  unsigned input_transport[2];
  if (input_tablet) {
    tablet_transport = next_transport++;
    input_transport[ninput++] = tablet_transport;
    m.dev[m.ndev++] = (xl_device){ XL_DEV_INPUT, VIRTIO_BASE + tablet_transport * VIRTIO_SIZE, VIRTIO_SIZE, false };
  }
  if (input_keyboard) {
    keyboard_transport = next_transport++;
    input_transport[ninput++] = keyboard_transport;
    m.dev[m.ndev++] = (xl_device){ XL_DEV_INPUT, VIRTIO_BASE + keyboard_transport * VIRTIO_SIZE, VIRTIO_SIZE, false };
  }
  unsigned snd_transport = 0;
  if (audio) {
    snd_transport = next_transport++;
    m.dev[m.ndev++] = (xl_device){ XL_DEV_SND, VIRTIO_BASE + snd_transport * VIRTIO_SIZE, VIRTIO_SIZE, false };
  }
  const xh_imageset_file *os = image_dir ? xh_imageset_role(&set, "os") : NULL;
  if (os && *os->volume_uuid) m.root_volume_uuid = os->volume_uuid;
  m.trust_cache = trust.data != NULL;
  xl_adt_node *tree = xl_machine_tree(&m, &err);
  if (!tree) { fprintf(stderr, "board: %s\n", err); return 1; }
  for (unsigned i = 0, blocks = 0; i < ndisk; i++) {
    char name[40];
    if (disks[i].writable) snprintf(name, sizeof name, "arm-io/data-store");
    else snprintf(name, sizeof name, pci ? "arm-io/virtio-pci-%u" : "arm-io/virtio-mmio-%u", blocks++);
    xl_adt_node *node = xl_adt_find(tree, name);
    if (!node) { fprintf(stderr, "board: no /%s\n", name); return 1; }
    xh_imageset_describe(node, &disks[i].identity);
    if (disks[i].list && xl_adt_set(node, "oneios,hash-list", disks[i].list, disks[i].list_size)) {
      fprintf(stderr, "board: /%s: no room for its hash list\n", name);
      return 1;
    }
    if (pci) {
      const uint64_t dma[2] = { pool_pa + i * DMA_SLICE, DMA_SLICE }, bar[2] = { PCI_BARS + i * PCI_BAR_SLOT, PCI_BAR_SLOT };
      if (xl_adt_set(node, "oneios,dma-window", dma, sizeof dma) || xl_adt_set(node, "oneios,pci-bar-window", bar, sizeof bar)) {
        fprintf(stderr, "board: /%s: no room for its windows\n", name);
        return 1;
      }
    } else {
      xl_adt_set_u32(node, "droidvm,intid", VIRTIO_INTID + i);
    }
  }
  if (display_w) {
    xl_adt_node *gpu = xl_adt_find(tree, "arm-io/virtio-gpu");
    if (!gpu) { fprintf(stderr, "board: no /arm-io/virtio-gpu\n"); return 1; }
    xl_adt_set_u32(gpu, "droidvm,intid", VIRTIO_INTID + gpu_transport);
  }
  for (unsigned i = 0; i < ninput; i++) {
    char name[40];
    snprintf(name, sizeof name, "arm-io/input-%u", i);
    xl_adt_node *node = xl_adt_find(tree, name);
    if (!node) { fprintf(stderr, "board: no /%s\n", name); return 1; }
    xl_adt_set_u32(node, "droidvm,intid", VIRTIO_INTID + input_transport[i]);
  }
  // The console: XNU's PL011 driver (the board's oneios-board-pl011-console.patch) takes the node that /defaults
  // serial-device names by its AAPL,phandle (pexpert/arm/pe_serial.c get_serial_device_phandle).
  xl_adt_node *uart = xl_adt_find(tree, "arm-io/uart0"), *defaults = xl_adt_find(tree, "defaults");
  if (!uart || !defaults || xl_adt_set_u32(uart, "AAPL,phandle", 0x10001) || xl_adt_set_u32(defaults, "serial-device", 0x10001)) {
    fprintf(stderr, "board: no /arm-io/uart0 or /defaults for the console\n");
    return 1;
  }

  uint8_t *ram = calloc(1, window);
  if (!ram) { fprintf(stderr, "xnuloader-qemu: %u MiB: out of memory\n", ram_mib); return 1; }
  xl_boot_request req = { .image = { image, size }, .kernel = &k, .ram = ram, .window = window, .phys_base = phys,
                          .virt_base = k.base, .tree = tree, .command_line = args, .trustcache = trust,
                          .video = { display_w ? fb_pa : 0, display_w ? 1 : 0, (uint64_t)display_w * 4, display_w, display_h, display_w ? 32 : 0 } };
  xl_boot_layout lay;
  if (xl_boot_place(&req, &lay, &err)) { fprintf(stderr, "place: %s\n", err); return 1; }

  // The flat tree as the kernel got it, for xnuloader-inspect.
  uint64_t tree_off = lay.tree_pa - phys;
  spit(out, "board.adt", ram + tree_off, lay.tree_size);
  uint64_t used = (lay.used + 0x3fff) & ~0x3fffull;
  spit(out, "window.bin", ram, used);

  // msr daifset, #0xf; ldr x0, [pc, #12]; ldr x1, [pc, #16]; br x1; then boot_args' and the entry's physical addresses
  // (8-byte aligned: with the MMU off every load is to Device memory, which takes no unaligned access).
  uint32_t stub[8] = { 0xd5034fdfu, 0x58000060u, 0x58000081u, 0xd61f0020u };
  memcpy(&stub[4], &lay.args_pa, 8);
  memcpy(&stub[6], &lay.entry_pa, 8);
  spit(out, "stub.bin", stub, sizeof stub);

  uint64_t ram_end = pci ? pool_pa + pool_bytes : phys + window + fb_bytes;
  uint64_t qemu_mib = ((ram_end - RAM_BASE) + (256ull << 20) - 1) >> 20 & ~255ull;
  if (RAM_BASE + (qemu_mib << 20) > 0x100000000ull) { fprintf(stderr, "xnuloader-qemu: the window and the boot video end above 4 GiB (highmem=off)\n"); return 2; }
  char line[16384];
  int n = snprintf(line, sizeof line,
                   "-M\nvirt,gic-version=3,virtualization=off,secure=off,highmem=off\n-cpu\n%s\n-smp\n%u\n-m\n%lluM\n"
                   "-device\nloader,file=%s/window.bin,addr=0x%llx,force-raw=on\n-device\nloader,file=%s/stub.bin,addr=0x%llx,cpu-num=0,force-raw=on\n",
                   qemu_cpu, cpus, (unsigned long long)qemu_mib, out, (unsigned long long)phys, out, (unsigned long long)STUB_PA);
  // The disks: modern virtio (OneiOSHostedDisk takes version 2 only), disk K on transport K. A path with a comma would
  // need QEMU's doubling: refused instead.
  if ((ndisk || display_w || ninput || audio) && n >= 0 && (size_t)n < sizeof line) n += snprintf(line + n, sizeof line - n, "-global\nvirtio-mmio.force-legacy=false\n");
  // The GPU: 2D only (no virgl), its first mode the panel's; QEMU's display backend is boot-vm.sh's (VNC or none).
  if (display_w && n >= 0 && (size_t)n < sizeof line)
    n += snprintf(line + n, sizeof line - n, "-device\nvirtio-gpu-device,bus=virtio-mmio-bus.%u,xres=%u,yres=%u,max_outputs=1\n",
                  gpu_transport, display_w, display_h);
  if (input_tablet && n >= 0 && (size_t)n < sizeof line)
    n += snprintf(line + n, sizeof line - n, "-device\nvirtio-tablet-device,bus=virtio-mmio-bus.%u\n", tablet_transport);
  if (input_keyboard && n >= 0 && (size_t)n < sizeof line)
    n += snprintf(line + n, sizeof line - n, "-device\nvirtio-keyboard-device,bus=virtio-mmio-bus.%u\n", keyboard_transport);
  if (audio && n >= 0 && (size_t)n < sizeof line)
    n += snprintf(line + n, sizeof line - n, "-audiodev\nnone,id=snd0\n-device\nvirtio-sound-device,bus=virtio-mmio-bus.%u,audiodev=snd0\n", snd_transport);
  for (unsigned i = 0; i < ndisk && n >= 0 && (size_t)n < sizeof line; i++) {
    if (strchr(disks[i].path, ',') || strchr(disks[i].path, '\n')) { fprintf(stderr, "%s: a disk path with a comma\n", disks[i].path); return 1; }
    n += snprintf(line + n, sizeof line - n, "-drive\nif=none,id=disk%u,format=raw,file=%s,readonly=%s\n",
                  i, disks[i].path, disks[i].writable ? "off" : "on");
    // --pci: modern only (device 0x1042, its regions in BAR 4), no MSI-X (the driver polls), at 00:(2 + K).0
    if (pci && n >= 0 && (size_t)n < sizeof line)
      n += snprintf(line + n, sizeof line - n, "-device\nvirtio-blk-pci,drive=disk%u,bus=pcie.0,addr=0x%x,disable-legacy=on,disable-modern=off,vectors=0\n",
                    i, PCI_FIRST_DEV + i);
    else if (n >= 0 && (size_t)n < sizeof line)
      n += snprintf(line + n, sizeof line - n, "-device\nvirtio-blk-device,drive=disk%u,bus=virtio-mmio-bus.%u\n", i, i);
  }
  if (n < 0 || (size_t)n >= sizeof line) { fprintf(stderr, "xnuloader-qemu: OUTDIR or the disks' paths too long\n"); return 1; }
  spit(out, "qemu.args", line, (size_t)n);

  printf("xnuloader-qemu: %s %s, base 0x%llx, entry 0x%llx\n", k.fileset ? "MH_FILESET" : "MH_EXECUTE", k.cpusubtype == 2 ? "arm64e" : "arm64",
         (unsigned long long)k.base, (unsigned long long)k.entry);
  printf("  window at PA 0x%llx (%u MiB), %llu MiB used; entry PA 0x%llx; boot_args PA 0x%llx; device tree PA 0x%llx (%llu bytes)\n",
         (unsigned long long)phys, ram_mib, (unsigned long long)(lay.used >> 20), (unsigned long long)lay.entry_pa,
         (unsigned long long)lay.args_pa, (unsigned long long)lay.tree_pa, (unsigned long long)lay.tree_size);
  printf("  stub at PA 0x%llx; QEMU RAM %llu MiB; %u CPU%s (%s), timebase %u Hz; boot args: %s\n", (unsigned long long)STUB_PA,
         (unsigned long long)qemu_mib, cpus, cpus == 1 ? "" : "s", qemu_cpu, timebase, args);
  if (jetsam_product)
    printf("  jetsam properties: /product jetsam-properties-product-type \"%s\": launchd reads /System/Library/LaunchDaemons/com.apple.jetsamproperties.%s.plist"
           " (the OS image must have it)\n", jetsam_product, jetsam_product);
  if (trust.data) printf("  trust caches: %u modules, %zu bytes; root: the OS volume %s\n", (unsigned)trust.data[0], trust.size, m.root_volume_uuid);
  for (unsigned i = 0; i < ndisk; i++) {
    if (pci)
      printf("  disk %u (PCI 00:%02x.0, config 0x%llx, BAR window 0x%llx, DMA window 0x%llx+0x%llx): %s, %s, %s%s\n", i, PCI_FIRST_DEV + i,
             (unsigned long long)(PCI_ECAM + ((uint64_t)(PCI_FIRST_DEV + i) << 15)), (unsigned long long)(PCI_BARS + i * PCI_BAR_SLOT),
             (unsigned long long)(pool_pa + i * DMA_SLICE), (unsigned long long)DMA_SLICE, disks[i].identity.role, disks[i].path,
             disks[i].writable ? "writable" : "read-only", disks[i].list ? ", hash list in its node" : "");
    else
      printf("  disk %u (transport 0x%llx): %s, %s, %s%s\n", i, (unsigned long long)(VIRTIO_BASE + i * VIRTIO_SIZE), disks[i].identity.role,
             disks[i].path, disks[i].writable ? "writable" : "read-only", disks[i].list ? ", hash list in its node" : "");
  }
  if (display_w)
    printf("  display: %ux%u @%ux, %u Hz, virtio-gpu on transport 0x%llx, boot video at PA 0x%llx (%llu MiB)\n", display_w, display_h,
           display_scale, display_hz, (unsigned long long)(VIRTIO_BASE + gpu_transport * VIRTIO_SIZE), (unsigned long long)fb_pa,
           (unsigned long long)(fb_bytes >> 20));
  if (input_tablet) printf("  input: tablet (one finger while the button is down) on transport 0x%llx\n", (unsigned long long)(VIRTIO_BASE + tablet_transport * VIRTIO_SIZE));
  if (input_keyboard) printf("  input: keyboard on transport 0x%llx\n", (unsigned long long)(VIRTIO_BASE + keyboard_transport * VIRTIO_SIZE));
  if (media) printf("  video decoder: /arm-io/virtio-media-0 on bus 0 of the ECAM at 0x%llx (QEMU has no virtio-media device: the kernel finds none)\n",
                    (unsigned long long)PCI_ECAM);
  return 0;
}
