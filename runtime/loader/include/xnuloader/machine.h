// SPDX-License-Identifier: BSD-2-Clause-Patent
// The device tree of the machine xnuhost presents to XNU.
// Ported from UniXNU-droidvm (https://github.com/Droid-VM/UniXNU-droidvm, commit f9121cd,
// ipados27/scripts/make-droidvm-adt.py), BSD-2-Clause-Patent: the same nodes ArmDTPlatform and ArmCpu
// consume (/chosen with a CHRP NVRAM proxy, /cpus, /psci, /defaults, /options, /product, /arm-io with
// the GICv3 and the virtio-mmio windows). Changes: C instead of Python; the hardware values come from
// xnuhost's own board instead of a crosvm FDT; the identity is an iPhone's, not an iPad's; a
// /chosen/xnuhost node tells a hosted kernel where the host-call vector is.
#ifndef XNULOADER_MACHINE_H
#define XNULOADER_MACHINE_H

#include <stdbool.h>
#include <stdint.h>
#include "xnuloader/adt.h"

#define XL_MAX_CPUS    8        // ArmCpu starts at most eight cores
#define XL_MAX_DEVICES 16

// The groups of an identity tree's properties that xl_machine.identity_groups switches on (xnuloader-crosvm --identity-groups). A
// group's letter is 'a' plus its bit's number. What iOS does with the properties of a group is not verified.
#define XL_IDENTITY_GROUPS     6
#define XL_IDENTITY_ARTWORK    (1u << 0)    // a: /product artwork-*, graphics-featureset-*, and the gamut (display_gamut's)
#define XL_IDENTITY_QUALITY    (1u << 1)    // b: /product ui-background-quality, ui-weather-quality
#define XL_IDENTITY_WORKGROUP  (1u << 2)    // c: /product display-workgroup-interval, hid-workgroup-interval
#define XL_IDENTITY_IDS        (1u << 3)    // d: /product udid-version, product-description, public-key-accelerator
#define XL_IDENTITY_MEMCLASS   (1u << 4)    // e: /product device-perf-memory-class (the VM's: vm_ram_mib, memory_class)
#define XL_IDENTITY_ROOT       (1u << 5)    // f: not /product: the root's target-type and the board name first in its compatible
#define XL_IDENTITY_ALL        ((1u << XL_IDENTITY_GROUPS) - 1)

// xl_machine.display_corner_radius and display_corner_word2: at most 200 points, a word of 16 bits
#define XL_CORNER_RADIUS_MAX   200u
#define XL_CORNER_WORD2_MAX    65535u

typedef enum {
  XL_DEV_BLOCK,       // "virtio,mmio": read-only image, NativeVirtioBlock
  XL_DEV_DATA_STORE,  // "droidvm,data-store": the writable Data store disk
  XL_DEV_INPUT,       // "droidvm,virtio-input"
  XL_DEV_NET,         // "droidvm,net"
  XL_DEV_GPU,         // "droidvm,virtio-gpu"
  XL_DEV_SND,         // "droidvm,virtio-snd"
  XL_DEV_BRIDGE,      // "oneios,bridge": the message channel of runtime/bridges (haptics, Bluetooth HCI)
  XL_DEV_CAMERA,      // "oneios,camera": camera frames from runtime/bridges/camera (drivers/camera)
  XL_DEV_MEDIA,       // "droidvm,virtio-media": the video encoder (runtime/bridges/encoder), with a media-host-pool
} xl_device_kind;

// pci (a block or the Data store only): a virtio-blk PCI function instead of a virtio-mmio window, base and size its
// configuration space (crosvm's CAM: 0x100; QEMU's ECAM: 0x1000); node "virtio-pci-N" or "data-store", compatible
// "oneios,virtio-pci", no "droidvm,intid" (OneiOSHostedDisk polls; the line is the PCI host's, not the board's: only the GPU
// takes one, xl_machine.gpu_intid). An input
// device on PCI is the bus instead (xnuloader-crosvm --display): node "input-pci", compatible "oneios,virtio-input-pci",
// base and size bus 0 of the CAM; the kernel finds crosvm's virtio-input functions on it and polls them. The sound card on
// PCI is a bus too (xnuloader-crosvm --audio): node "virtio-snd", compatible "oneios,virtio-snd-pci", base and size bus 0
// of Droid-VM's ECAM; the kernel takes the first 1af4:1059 function there and polls it. The network card likewise
// (xnuloader-crosvm --wifi): node "net", compatible "oneios,virtio-net-pci", its first 1af4:1041 function. And the video
// decoder (xnuloader-crosvm --media): node "virtio-media-0", compatible "oneios,virtio-media-pci", its first 1af4:1070
// function (crosvm's --virtio-media kind=decoder), with media-host-pool (media_pool_base/size).
typedef struct { xl_device_kind kind; uint64_t base, size; bool pci; } xl_device;

typedef struct {
  unsigned    ncpu;
  uint32_t    cpu_reg[XL_MAX_CPUS];       // MPIDR affinity of each CPU
  char        cpu_cluster[XL_MAX_CPUS];   // 'P' or 'E'
  uint32_t    timebase_hz;                // CNTFRQ_EL0 of the host
  uint64_t    dram_base, dram_size;       // the kernel's memory window (physical)
  uint64_t    armio_base, armio_size;     // must not start at 0 (pexpert takes it as the SoC base)
  uint64_t    gic_dist, gic_dist_size, gic_redist, gic_redist_size, gic_redist_stride;
  uint64_t    uart_base, uart_size;       // PL011, node "uart0"; size 0 for none
  // The uart0 node an ns16550a (8250, byte-spaced) instead, its "reg" the absolute physical address: crosvm's serial
  // ports sit at 0x3f8 and the like, below the arm-io window (kernel patch 0077, docs/native.md "A VM under Gunyah")
  bool        uart_ns16550;
  // A second ns16550a that carries the bridge channel's messages as frames (drivers/bridge/oneios_bridge_stream.h; crosvm's
  // second port in the VM of the native path, which has no "oneios,bridge" virtio device): node "bridge", compatible
  // "oneios,bridge-uart", its "reg" the absolute physical address as the console's. Size 0 for none.
  uint64_t    bridge_uart_base, bridge_uart_size;
  const char *psci_method;                // "hvc" or "smc"
  xl_device   dev[XL_MAX_DEVICES];
  unsigned    ndev;
  uint64_t    cpufreq_base, cpufreq_size; // /arm-io/cpufreq: one 4 KiB window per CPU; size 0 for none
  // /arm-io/virtio-gpu drm-host-pool: physical base and size of the memory, outside the kernel's window, in which
  // the host's 3D renderer keeps what the guest maps (UniXNU-droidvm DroidVMVirtioGpu3D.cpp); size 0 for no 3D.
  uint64_t    gpu_pool_base, gpu_pool_size;
  // /arm-io/virtio-gpu droidvm,intid on PCI (xnuloader-crosvm --gpu-intid): the GIC line crosvm's PCI host routes the GPU's
  // function to (its interrupt-map: 00:0c.0 to SPI 4, INTID 36, level-high, no other device on it); the transport then sleeps on
  // it instead of polling the used ring (DroidVMVirtioGpu.cpp, staged). 0: none, the transport polls.
  uint32_t    gpu_intid;
  // /arm-io/virtio-snd droidvm,intid and oneios,intid-slot on PCI (xnuloader-crosvm --audio-intid N,SLOT, take18): the GIC
  // line crosvm's PCI host routes the sound card's function to and the slot it was read for (crosvm places the card itself;
  // take17's tree: 00:08.0 to SPI 12, INTID 44); with oneios-audio-irq=1 the transport's control replies come by it
  // (staged/audio.rs SND_IRQ). 0: none, polled.
  uint32_t    audio_intid, audio_slot;
  // /arm-io/virtio-media media-host-pool: physical base and size of the memory, outside the kernel's window, in
  // which the video encoder's MMAP'd V4L2 buffers live (runtime/bridges/encoder; in a VM of Droid-VM's crosvm its
  // `--pre-alloc media-host-mb` pool, the FDT's media_host@PA: xnuloader-crosvm --media); the guest maps it as the codec's
  // OUTPUT/CAPTURE buffers (UniXNU-droidvm DroidVMVirtioMedia.c). Size 0 for no encoder or decoder.
  uint64_t    media_pool_base, media_pool_size;
  // /bluetooth: transport-encoding 10 (the virtual transport, no controller) when bt_transport is 0;
  // else that encoding (xnuhost: 1, H4, the phone's controller behind the bridge) with bt_speed and bt_mac.
  uint32_t    bt_transport, bt_speed;
  uint8_t     bt_mac[6];
  uint64_t    panic_base, panic_size;     // /pram; size 0 for none
  uint8_t     random_seed[256];           // xnu-12377 takes 4 x 64 seed bytes
  bool        have_chip_id;
  uint64_t    unique_chip_id;
  uint32_t    chip_id;
  const char *root_volume_uuid;           // /chosen root-matching by APFS volume UUID, or NULL
  // memory-map placeholders the loader fills (xl_boot_place)
  bool        ramdisk, packed_ramdisk, trust_cache, gpu_addon, root_policies;
  // Identity. `identity` is the device's own decoded device tree (the IPSW's DeviceTree payload), from
  // which model, target-type, the board name and /product are taken; NULL gives a generic machine
  // with model/product names from the strings below (which may be NULL).
  const xl_adt_node *identity;
  const char *model;                      // "iPhone18,2"
  const char *display_gamut;              // /product artwork-display-gamut, default "P3"
  // /product jetsam-properties-product-type (xnuloader-crosvm --jetsam-product): the name the kernel answers
  // hw.jetsam_properties_product_type with, which launchd puts in the file it reads the jetsam properties from
  // (/System/Library/LaunchDaemons/com.apple.jetsamproperties.NAME.plist: each daemon's band and memory limits).
  // Without it the kernel answers the root's target-type, which only an identity tree gives: a machine with
  // neither has no jetsam properties, and launchd leaves every daemon at its default band (take15: backboardd at
  // 30 where iPhone18,2's V54 list says 170). 1 to 31 of A-Z a-z 0-9 . _ -, not starting with a dot (a file name's
  // part); NULL: no property (an identity tree's own, if it has one, stays).
  const char *jetsam_product;
  // Identity by groups (xnuloader-crosvm --identity-tree FILE --identity-groups LIST): `identity_product` is the decoded device
  // tree of an Apple device, as `identity` is, but only a source: it copies nothing by itself. `identity_groups` is a mask of
  // XL_IDENTITY_*, each bit a switch of its own, none on by default, and the machine's own tree takes these from the source:
  //   ARTWORK   /product: every artwork-* but artwork-display-gamut and artwork-scale-factor, graphics-featureset-class and
  //             graphics-featureset-fallbacks (less APPLE2 and MTL2,2, as with `identity`: with them MobileGestalt answers
  //             DeviceSupportsASTC and SplashBoard writes snapshots in the AGX GPU's own ASTC layout), and artwork-display-gamut,
  //             which is display_gamut's ("P3"), not the source's (artwork-scale-factor is display_scale's, group or not)
  //   QUALITY   /product: ui-background-quality, ui-weather-quality
  //   WORKGROUP /product: display-workgroup-interval, hid-workgroup-interval
  //   IDS       /product: udid-version, product-description, public-key-accelerator
  //   MEMCLASS  /product: device-perf-memory-class, the VM's (vm_ram_mib, memory_class)
  //   ROOT      not /product: the root's target-type from the source's root, and its compatible: the source's FIRST entry (the board
  //             name, "V54AP") and then "oneios,xnuhost"; the root's model stays `model`
  // Properties are the source's bytes but for the featureset list and the class; one that is a placeholder is left out; no child
  // node (iopm, camera, audio) is ever copied; /product keeps its product-name, model and jetsam name. What iOS does with any of
  // these properties is not verified. Refused: a mask without a source, a bit that is no group, a source without /product (or,
  // with ROOT, without a target-type or a compatible), and a source set beside `identity`. NULL, 0: nothing is copied.
  const xl_adt_node *identity_product;
  unsigned    identity_groups;
  // The MEMCLASS group's device-perf-memory-class: Apple's value is 12 for an iPhone18,2 (read as the device's RAM in GiB, which
  // is not verified) and a VM has less. vm_ram_mib is the VM's RAM (xnuloader-crosvm --ram-mib; not dram_size, the kernel's
  // window, which is the RAM less the boot region and the pool): the class is its whole GiB, at least 2 and at most Apple's own,
  // written as an integer of the width Apple's property has (take23: xl_machine_memory_class_of, CoreUI's classes). memory_class (xnuloader-crosvm --memory-class) is the class itself,
  // 1 to Apple's own (xl_machine_memory_class_max), written even where the source has no such property; refused without the
  // MEMCLASS group. 0: not set. The group with neither set and a source that has the property is refused (Apple's 12 is not the
  // VM's); without the group vm_ram_mib is not read.
  uint32_t    vm_ram_mib, memory_class;
  // take23: names of /product properties that an identity group that is on does NOT take (xnuloader-crosvm --identity-omit):
  // a comma-separated list, NULL for none. A name that no group that is on would take is refused (a typing mistake is not silent).
  const char *identity_omit;
  // /arm-io soc-generation, a string with its NUL: the SoC's generation as Apple's trees give it ("H18" for the iPhone18,2).
  // Nothing else of the SoC's identity is written (not arm-io's compatible, "arm-io,t8150", which Apple's kexts match).
  // 1 to 31 of A-Z a-z 0-9 . _ - ; NULL: no property (the tools write one only when told: xnuloader-crosvm --soc-generation).
  const char *soc_generation;
  // /product display-corner-radius (xnuloader-crosvm --display-corner-radius PT[,WORD2]): 8 bytes, two little-endian u32:
  // display_corner_radius (PT, in points: 0 to XL_CORNER_RADIUS_MAX), then display_corner_word2 (0 to XL_CORNER_WORD2_MAX).
  // Apple's iPhone18,2 tree has u32 62 and u32 1; what the second word means is not verified. Independent of the identity
  // fields; not written unless has_display_corner_radius.
  uint32_t    display_corner_radius, display_corner_word2;
  bool        has_display_corner_radius;
  // The guest's display (0 = say nothing): the mode in pixels, the UI scale (pixels per point, 2 or 3: /chosen
  // display-scale, which MobileGestalt answers main-screen-scale from, and /product artwork-scale-factor) and the
  // panel's refresh rate. /chosen/xnuhost carries all four for the hosted kernel's display driver.
  uint32_t    display_width, display_height, display_scale, display_hz;
  const char *text_fixups;                // NUL-separated list for /chosen text-fixups, with its length
  uint32_t    text_fixups_length;
  // the Metal plug-in's shader priming list (every .metallib of the OS image, a path a line) for /chosen
  // oneios-prime-list, which the kernel writes beside the plug-in at boot (patch 0080); NULL: no property
  const char *prime_list;
  uint32_t    prime_list_length;
  // xnuhost: where a hosted kernel finds the host (0 = not hosted, node omitted)
  uint64_t    host_vector;                // virtual address of struct xh_vector
  uint64_t    host_io_base, host_io_size; // physical range of the trapped MMIO aperture
  uint64_t    user_va_limit;              // first address iOS tasks may not use
  uint32_t    host_page_size;
} xl_machine;

// The board xnuhost emulates: where its windows are (physical addresses inside /arm-io).
#define XL_BOARD_ARMIO_BASE     0x1000ull
#define XL_BOARD_ARMIO_SIZE     (0x40000000ull - XL_BOARD_ARMIO_BASE)
#define XL_BOARD_UART           0x00006000ull
#define XL_BOARD_UART_SIZE      0x1000ull
#define XL_BOARD_GIC_DIST       0x08000000ull
#define XL_BOARD_GIC_DIST_SIZE  0x10000ull
#define XL_BOARD_GIC_REDIST     0x080a0000ull
#define XL_BOARD_GIC_STRIDE     0x20000ull
#define XL_BOARD_CPUFREQ        0x09000000ull   // + cpu * 0x1000
#define XL_BOARD_VIRTIO         0x0a000000ull
#define XL_BOARD_VIRTIO_SIZE    0x1000ull   // one page per device, so each is its own trapped page
#define XL_BOARD_IO_BASE        XL_BOARD_GIC_DIST
#define XL_BOARD_IO_SIZE        0x03000000ull
#define XL_BOARD_VIRTIO_SPI     32u         // first SPI INTID of the virtio windows, one each
#define XL_BOARD_UART_SPI       31u         // not wired by default

// Fills the windows of *m for `ncpu` CPUs and the given devices from the XL_BOARD_* layout.
void xl_machine_board(xl_machine *m, unsigned ncpu, const xl_device_kind *kinds, unsigned ndev);

// The guest's stable addresses, from its unique chip ID: which = 0 Bluetooth, 1 Wi-Fi (en0), 2 Ethernet.
void xl_machine_mac(const xl_machine *m, unsigned which, uint8_t mac[6]);

// Builds the tree. NULL with *error set for a machine that cannot be described.
xl_adt_node *xl_machine_tree(const xl_machine *m, const char **error);

// Whether `name` can be xl_machine.jetsam_product (xl_machine_tree refuses one that cannot).
bool xl_machine_jetsam_product_ok(const char *name);

// Whether `name` can be xl_machine.soc_generation (xl_machine_tree refuses one that cannot).
bool xl_machine_soc_generation_ok(const char *name);

// The group (one XL_IDENTITY_* bit) whose copy takes the /product property `name` of an identity tree; 0 for the properties no
// group takes (artwork-display-gamut and artwork-scale-factor among them: the machine's own) and for the root's, which are not
// /product's. The tool's summary reads the tree it built with this, so that what it names is what was written.
unsigned xl_identity_group_of(const char *name);

// The name of a group ("artwork", "quality", "workgroup", "ids", "memclass", "root"); `group` is one XL_IDENTITY_* bit, else NULL.
const char *xl_identity_group_name(unsigned group);

// Reads a list of groups as --identity-groups takes it: words separated by commas, each a group's name, its letter ("a" to "f")
// or "all", nothing else (no blank, no capital, no empty word). true with *groups the mask; false with the first word that is
// none in `bad` (cut to size - 1 characters; empty for an empty word) and *groups as it was.
bool xl_identity_groups_parse(const char *list, unsigned *groups, char *bad, size_t size);

// The most xl_machine.memory_class can be for the identity tree `identity`: its own /product device-perf-memory-class, 16 where
// it has none (or none that is an integer of 1 to 65535).
uint32_t xl_machine_memory_class_max(const xl_adt_node *identity);
// take23: the memory class of a VM with `ram_mib` of RAM, at most `most`'s size: the largest of CoreUI's classes that is not more
// than the RAM. The classes are CoreUI's own table (_gThemeMemoryPerformanceClasses, read in 24A446's shared cache at
// 0x1e1555ca0: value, constant, name): 0 512MB, 1 1GB, 15 1.5GB, 2 2GB, 4 3GB, 3 4GB, 6 6GB, 8 8GB, 9 9GB, 12 12GB, 16 16GB.
// The value is NOT the GiB: 3 is the 4 GB class and 4 the 3 GB one, and there is no 5, 7, 10 or 11.
uint32_t xl_machine_memory_class_of(uint32_t ram_mib, uint32_t most);
// The MiB of a class of that table; 0 for a value that is not in it.
uint32_t xl_machine_memory_class_mib(uint32_t value);

// A fresh volatile CHRP NVRAM bank for IODTNVRAM (8192 bytes written to `image`).
void xl_nvram_proxy(uint8_t image[8192]);

#endif
