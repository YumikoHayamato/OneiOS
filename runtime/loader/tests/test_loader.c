// SPDX-License-Identifier: MIT
// Loader unit tests on generated fixtures (tests/fixture.h): no Apple files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixture.h"
#include "xnuloader/adt.h"
#include "xnuloader/boot.h"
#include "xnuloader/machine.h"
#include "xnuloader/macho.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static xl_kernel K;   // large: static

static void test_macho(void)
{
  const char *err = "";
  fx_image im = fx_kernel(FX_BASE, 0, NULL, 0, 1);
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == 0);
  CHECK(K.nseg == 2 && K.base == FX_BASE && K.end == FX_BASE + 0x5000 && K.entry == FX_BASE + 0x1000);
  CHECK(!K.fileset && K.cpusubtype == 0 && !strcmp(K.seg[0].name, "__TEXT") && K.seg[1].filesize == 0);
  uint64_t v = 0;
  CHECK(xl_macho_symbol(im.data, im.size, 0, "_fixture_symbol", &v) == 0 && v == FX_BASE + 0x1008);
  CHECK(xl_macho_symbol(im.data, im.size, 0, "_absent", &v) == -1);
  CHECK(xl_macho_offset(&K, FX_BASE + 0x1000, 16) == 0x1000);
  CHECK(xl_macho_offset(&K, FX_BASE + 0x4000, 16) == -1);          // zero-fill has no file bytes

  // arm64e with capability bits in the subtype's top byte
  fx_w32(im.data + 8, 0x80000002u);
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == 0 && K.cpusubtype == 2);
  fx_w32(im.data + 8, 0);

  // DarwinBoot's rejected variants
  struct { size_t off; int wide; uint64_t value; } bad[] = {
    { 0, 0, 0xfeedface },                         // 32-bit magic
    { 4, 0, 0x01000007 },                         // x86_64
    { 8, 0, 3 },                                  // bad subtype
    { 12, 0, 6 },                                 // MH_DYLIB
    { 20, 0, (uint64_t)im.size },                 // commands past the file
    { 32 + 72 + 24, 1, FX_BASE + 0x1000 },        // __DATA overlaps __TEXT
    { 32 + 144 + 272, 1, FX_BASE + 0x4000 },      // entry in zero-fill, not executable
    { 32 + 144 + 272, 1, FX_BASE + 0x1002 },      // unaligned entry
    { 32 + 40, 1, 0x100000 },                     // segment file offset outside the file
    { 32 + 4, 0, 71 },                            // command length not a multiple of 8
  };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    uint8_t saved[8];
    memcpy(saved, im.data + bad[i].off, 8);
    if (bad[i].wide) fx_w64(im.data + bad[i].off, bad[i].value); else fx_w32(im.data + bad[i].off, (uint32_t)bad[i].value);
    err = NULL;
    CHECK(xl_macho_parse(im.data, im.size, &K, &err) == -1 && err != NULL);
    memcpy(im.data + bad[i].off, saved, 8);
  }
  CHECK(xl_macho_parse(im.data, 16, &K, &err) == -1);
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == 0);
  free(im.data);
}

static void test_fileset(void)
{
  const char *err = "";
  fx_image im = fx_collection(FX_BASE, 2, NULL, 0);
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == 0);
  if (err) printf("  fileset: %s\n", err);
  CHECK(K.fileset && K.nseg == 4 && K.nentries == 2 && K.kernel_header == 0x4000);
  CHECK(!strcmp(K.entries[0].name, "com.apple.kernel") && !strcmp(K.entries[1].name, "com.example.kext"));
  CHECK(K.entries[1].vmaddr == FX_BASE + 0xc000 && K.entry == FX_BASE + 0x5000 && K.base == FX_BASE);
  uint64_t v = 0;
  CHECK(xl_macho_symbol(im.data, im.size, K.kernel_header, "_fixture_symbol", &v) == 0 && v == FX_BASE + 0x5008);

  uint8_t *entry0 = im.data + 32 + 4 * 72 + 288;
  uint64_t saved = 0;
  memcpy(&saved, entry0 + 8, 8);
  fx_w64(entry0 + 8, FX_BASE + 0x8000);           // member header not mapped where the entry says
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == -1);
  fx_w64(entry0 + 8, saved);
  entry0[32 + 15] = 'x';                          // no com.apple.kernel
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == -1);
  entry0[32 + 15] = 'l';
  fx_w32(im.data + 0x4000 + 24, 0);               // the kernel member lost MH_DYLIB_IN_CACHE
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == -1);
  fx_w32(im.data + 0x4000 + 24, 0x80000000u);
  fx_w64(im.data + 0x4000 + 32 + 144 + 272, FX_BASE + 0x5004);    // member entry differs
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == -1);
  fx_w64(im.data + 0x4000 + 32 + 144 + 272, FX_BASE + 0x5000);
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == 0);
  free(im.data);
}

static void test_adt(void)
{
  xl_adt_node *root = xl_adt_new("device-tree");
  xl_adt_set_u32(root, "#address-cells", 2);
  xl_adt_node *map = xl_adt_path(root, "chosen/memory-map");
  uint8_t zero[16] = { 0 };
  xl_adt_set(map, "RAMDisk", zero, 16);
  xl_adt_set_str(xl_adt_path(root, "chosen"), "osenvironment", "main");
  xl_adt_set(xl_adt_path(root, "arm-io/dev"), "dma-coherent", "", 0);
  xl_adt_set(xl_adt_path(root, "arm-io/dev"), "odd", "abcde", 5);       // padded to 8 in the flat form
  CHECK(xl_adt_set(root, "a-property-name-that-is-longer-than-31", "", 0) == -1);
  CHECK(xl_adt_find(root, "chosen/memory-map") == map && xl_adt_find(root, "chosen/none") == NULL);

  size_t size = xl_adt_flat_size(root);
  uint8_t *flat = malloc(size);
  xl_adt_flatten(root, flat);
  // the layout XNU reads: root = 2 properties ("name" first), 2 children
  uint32_t head[2];
  memcpy(head, flat, 8);
  CHECK(head[0] == 2 && head[1] == 2 && !memcmp(flat + 8, "name", 5));
  uint32_t namelen;
  memcpy(&namelen, flat + 8 + 32, 4);
  CHECK(namelen == 12 && !memcmp(flat + 8 + 36, "device-tree", 12));
  CHECK(size % 4 == 0);

  const char *err = NULL;
  xl_adt_node *back = xl_adt_parse(flat, size, &err);
  CHECK(back != NULL);
  if (back) {
    uint8_t *again = malloc(size);
    CHECK(xl_adt_flat_size(back) == size);
    xl_adt_flatten(back, again);
    CHECK(!memcmp(flat, again, size));
    const xl_adt_prop *p = xl_adt_get(xl_adt_find(back, "arm-io/dev"), "odd");
    CHECK(p && p->length == 5 && !memcmp(p->value, "abcde", 5));
    CHECK(xl_adt_remove(back, "arm-io") == 0 && xl_adt_find(back, "arm-io") == NULL && xl_adt_remove(back, "arm-io") == -1);
    CHECK(xl_adt_unset(back, "#address-cells") == 0 && !xl_adt_get(back, "#address-cells"));
    free(again);
    xl_adt_free(back);
  }
  // malformed flat trees
  CHECK(xl_adt_parse(flat, size - 4, &err) == NULL && err);
  CHECK(xl_adt_parse(flat, 4, &err) == NULL);
  uint8_t *bad = malloc(size + 8);
  memcpy(bad, flat, size);
  memset(bad + size, 0, 8);
  CHECK(xl_adt_parse(bad, size + 8, &err) == NULL);                     // trailing bytes
  memcpy(bad, flat, size);
  uint32_t huge = 0x7ffffff0;
  memcpy(bad + 8 + 32, &huge, 4);
  CHECK(xl_adt_parse(bad, size, &err) == NULL);                         // property past the tree
  memcpy(bad, flat, size);
  huge = 0xffffff;
  memcpy(bad + 4, &huge, 4);
  CHECK(xl_adt_parse(bad, size, &err) == NULL);                         // child count past the tree
  free(bad);
  free(flat);
  xl_adt_free(root);
}

static void machine_default(xl_machine *m)
{
  static const xl_device_kind kinds[] = { XL_DEV_BLOCK, XL_DEV_BLOCK, XL_DEV_DATA_STORE, XL_DEV_INPUT, XL_DEV_GPU };
  memset(m, 0, sizeof *m);
  m->cpu_cluster[2] = 'E';
  xl_machine_board(m, 4, kinds, 5);
  m->timebase_hz = 19200000;
  m->dram_base = 0x800004000ull;
  m->dram_size = 0x40000000;
  m->trust_cache = true;
  m->model = "iPhone18,2";
  m->root_volume_uuid = "0a1b2c3d-0000-4000-8000-00000000abcd";
  m->host_vector = 0x2000100000ull;
  m->user_va_limit = 0x7e00000000ull;
  m->host_page_size = 4096;
  for (int i = 0; i < 256; i++) m->random_seed[i] = (uint8_t)i;
}

static void test_machine(void)
{
  xl_machine m;
  const char *err = NULL;
  machine_default(&m);
  xl_adt_node *t = xl_machine_tree(&m, &err);
  CHECK(t != NULL);
  if (!t) { printf("  machine: %s\n", err); return; }
  // what ArmDTPlatform::configure requires
  CHECK(xl_adt_find(t, "cpus") && xl_adt_find(t, "arm-io") && xl_adt_find(t, "defaults") && xl_adt_find(t, "options"));
  const xl_adt_node *chosen = xl_adt_find(t, "chosen");
  const xl_adt_prop *p = xl_adt_get(chosen, "nvram-bank-size");
  CHECK(p && p->length == 4);
  p = xl_adt_get(chosen, "nvram-proxy-data");
  CHECK(p && p->length == 8192 && p->value[0] == 0x70 && !memcmp(p->value + 4, "nvram", 5));
  CHECK(xl_adt_get(chosen, "random-seed")->length == 256);
  CHECK(xl_adt_get(chosen, "oneios-prime-list") == NULL);   // only when the loader is given one
  p = xl_adt_get(chosen, "root-matching");
  CHECK(p && strstr((const char *)p->value, "<string>0A1B2C3D-0000-4000-8000-00000000ABCD</string>"));
  // what ArmCpu reads
  const xl_adt_node *cpus = xl_adt_find(t, "cpus");
  CHECK(cpus->nchildren == 4);
  CHECK(!strcmp((const char *)xl_adt_get(cpus->children[0], "state")->value, "running"));
  CHECK(!strcmp((const char *)xl_adt_get(cpus->children[3], "state")->value, "waiting"));
  CHECK(!strcmp((const char *)xl_adt_get(cpus->children[2], "cluster-type")->value, "E"));
  uint32_t v32;
  memcpy(&v32, xl_adt_get(cpus->children[1], "timebase-frequency")->value, 4);
  CHECK(v32 == 19200000);
  const xl_adt_node *intc = xl_adt_find(t, "arm-io/intc");
  p = xl_adt_get(intc, "reg");
  uint64_t r[4];
  CHECK(p && p->length == 32);
  memcpy(r, p->value, 32);
  CHECK(r[0] == XL_BOARD_GIC_DIST - 0x1000 && r[1] >= 0x10000 && r[2] == XL_BOARD_GIC_REDIST - 0x1000 && r[3] == 4 * 0x20000);
  CHECK(!memcmp(xl_adt_get(intc, "compatible")->value, "arm,gic-v3", 11));
  CHECK(!strcmp((const char *)xl_adt_get(xl_adt_find(t, "psci"), "method")->value, "hvc"));
  // the devices, by the names and compatibles their drivers match
  p = xl_adt_get(xl_adt_find(t, "arm-io/virtio-mmio-1"), "compatible");
  CHECK(p && p->length == 12 && !memcmp(p->value, "virtio,mmio", 12));
  CHECK(xl_adt_find(t, "arm-io/data-store") && xl_adt_find(t, "arm-io/input-0") && xl_adt_find(t, "arm-io/virtio-gpu"));
  CHECK(!xl_adt_get(xl_adt_find(t, "arm-io/virtio-gpu"), "drm-host-pool"));                 // no 3D renderer: no pool
  p = xl_adt_get(xl_adt_find(t, "arm-io/data-store"), "reg");
  memcpy(r, p->value, 16);
  CHECK(r[0] == XL_BOARD_VIRTIO + 2 * XL_BOARD_VIRTIO_SIZE - 0x1000 && r[1] == XL_BOARD_VIRTIO_SIZE);
  uint32_t intid;
  memcpy(&intid, xl_adt_get(xl_adt_find(t, "arm-io/data-store"), "droidvm,intid")->value, 4);
  CHECK(intid == XL_BOARD_VIRTIO_SPI + 2);
  memcpy(&v32, xl_adt_get(xl_adt_find(t, "bluetooth"), "transport-encoding")->value, 4);
  CHECK(v32 == 10 && !xl_adt_get(xl_adt_find(t, "bluetooth"), "transport-speed") && !xl_adt_find(t, "arm-io/cpufreq"));
  // memory-map placeholders and the host node
  const xl_adt_node *map = xl_adt_find(t, "chosen/memory-map");
  CHECK(xl_adt_get(map, "TrustCache") && !xl_adt_get(map, "RAMDisk"));
  p = xl_adt_get(xl_adt_find(t, "chosen/xnuhost"), "vector");
  uint64_t v64;
  memcpy(&v64, p->value, 8);
  CHECK(v64 == 0x2000100000ull);
  // it flattens and parses back
  size_t size = xl_adt_flat_size(t);
  uint8_t *flat = malloc(size);
  xl_adt_flatten(t, flat);
  xl_adt_node *back = xl_adt_parse(flat, size, &err);
  CHECK(back != NULL);
  xl_adt_free(back);
  free(flat);
  xl_adt_free(t);

  // identity from a donor tree: model, target-type, board first in compatible, /product filtered
  xl_adt_node *donor = xl_adt_new("device-tree");
  xl_adt_set_str(donor, "model", "Fixture1,1");
  xl_adt_set_str(donor, "target-type", "F1");
  xl_adt_set(donor, "compatible", "F1AP\0Fixture1,1\0AppleARM", sizeof "F1AP\0Fixture1,1\0AppleARM");
  xl_adt_node *product = xl_adt_add(donor, "product");
  xl_adt_set_str(product, "product-name", "Fixture phone");
  xl_adt_set_u32(product, "has-exclaves", 1);
  xl_adt_set_str(product, "graphics-featureset-fallbacks", "MTL1,2:APPLE2:MTL2,2:GLES2,0");
  xl_adt_set_u32(xl_adt_add(product, "audio"), "acoustic-id", 77);
  xl_adt_set_u32(product, "artwork-scale-factor", 3);
  xl_adt_set_u32(product, "artwork-device-subtype", 2868);
  m.identity = donor;
  // the display: the mode, the UI scale and the refresh rate (the scale in /chosen and in /product too)
  static const char prime[] = "/System/Library/a.metallib\n/Applications/B.app/default.metallib\n";
  m.prime_list = prime; m.prime_list_length = sizeof prime - 1;
  m.display_width = 1440; m.display_height = 3120; m.display_scale = 2; m.display_hz = 120;
  t = xl_machine_tree(&m, &err);
  CHECK(t != NULL);
  if (t) {
    const xl_adt_node *ch = xl_adt_find(t, "chosen"), *host = xl_adt_find(t, "chosen/xnuhost");
    uint32_t word = 0;
    p = xl_adt_get(ch, "oneios-prime-list");   // the shader priming list as given, without a NUL
    CHECK(p && p->length == sizeof prime - 1 && !memcmp(p->value, prime, sizeof prime - 1));
    p = xl_adt_get(ch, "display-scale");
    CHECK(p && p->length == 4 && (memcpy(&word, p->value, 4), word) == 2);
    p = xl_adt_get(ch, "display-rotation");
    CHECK(p && p->length == 4 && (memcpy(&word, p->value, 4), word) == 0);
    p = xl_adt_get(host, "display-width");
    CHECK(p && (memcpy(&word, p->value, 4), word) == 1440);
    p = xl_adt_get(host, "display-height");
    CHECK(p && (memcpy(&word, p->value, 4), word) == 3120);
    p = xl_adt_get(host, "display-hz");
    CHECK(p && (memcpy(&word, p->value, 4), word) == 120);
    p = xl_adt_get(host, "display-scale");
    CHECK(p && (memcpy(&word, p->value, 4), word) == 2);
    p = xl_adt_get(xl_adt_find(t, "product"), "artwork-scale-factor");
    CHECK(p && p->length == 4 && (memcpy(&word, p->value, 4), word) == 2);
    p = xl_adt_get(xl_adt_find(t, "product"), "artwork-device-subtype");
    CHECK(p && (memcpy(&word, p->value, 4), word) == 2868);                        // the identity's set of artwork stays
    CHECK(!strcmp((const char *)xl_adt_get(t, "model")->value, "Fixture1,1"));
    p = xl_adt_get(t, "compatible");
    CHECK(p && !memcmp(p->value, "F1AP\0oneios,xnuhost", sizeof "F1AP\0oneios,xnuhost"));
    const xl_adt_node *pr = xl_adt_find(t, "product");
    CHECK(!xl_adt_get(pr, "has-exclaves") && xl_adt_get(pr, "product-name"));
    CHECK(!strcmp((const char *)xl_adt_get(pr, "graphics-featureset-fallbacks")->value, "MTL1,2:GLES2,0"));
    CHECK(xl_adt_get(xl_adt_find(t, "product/audio"), "acoustic-id"));
    xl_adt_free(t);
  }
  xl_adt_remove(donor, "product");
  CHECK(xl_machine_tree(&m, &err) == NULL && err);
  xl_adt_free(donor);

  // the bridged Bluetooth controller, the cpufreq windows and the bridge channel
  machine_default(&m);
  m.bt_transport = 3; m.bt_speed = 3000000; memcpy(m.bt_mac, "\x10\x20\x30\x40\x50\x60", 6);
  m.cpufreq_base = XL_BOARD_CPUFREQ; m.cpufreq_size = 4 * 0x1000;
  m.dev[4].kind = XL_DEV_BRIDGE;
  t = xl_machine_tree(&m, &err);
  CHECK(t != NULL);
  if (t) {
    const xl_adt_node *b = xl_adt_find(t, "bluetooth");
    memcpy(&v32, xl_adt_get(b, "transport-encoding")->value, 4);
    CHECK(v32 == 3 && !memcmp(xl_adt_get(b, "local-mac-address")->value, m.bt_mac, 6) && xl_adt_get(b, "transport-speed"));
    CHECK(!memcmp(xl_adt_get(xl_adt_find(t, "arm-io/bridge"), "compatible")->value, "oneios,bridge", 14));
    memcpy(r, xl_adt_get(xl_adt_find(t, "arm-io/cpufreq"), "reg")->value, 16);
    CHECK(r[0] == XL_BOARD_CPUFREQ - 0x1000 && r[1] == 0x4000);
    uint8_t wifi[6], eth[6];
    xl_machine_mac(&m, 1, wifi); xl_machine_mac(&m, 2, eth);
    CHECK(wifi[0] == 0x02 && memcmp(wifi, eth, 6) && !memcmp(xl_adt_get(xl_adt_find(t, "chosen"), "mac-address-wifi0")->value, wifi, 6));
    xl_adt_free(t);
  }

  // refused machines
  machine_default(&m); m.ncpu = 9;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  machine_default(&m); m.timebase_hz = 0;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  machine_default(&m); m.armio_base = 0;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  machine_default(&m); m.dev[0].base = 0x50000000;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  machine_default(&m); m.dev[1].kind = XL_DEV_GPU;
  CHECK(xl_machine_tree(&m, &err) == NULL);     // two GPUs
  // the 3D renderer's pool: base and size on the GPU's node, as UniXNU-droidvm's DroidVMVirtioGpu3D.cpp reads them
  machine_default(&m); m.gpu_pool_base = 0x8ff800000ull; m.gpu_pool_size = 8 << 20;
  xl_adt_node *gt = xl_machine_tree(&m, &err);
  const xl_adt_prop *pool = gt ? xl_adt_get(xl_adt_find(gt, "arm-io/virtio-gpu"), "drm-host-pool") : NULL;
  uint64_t pv[2] = { 0, 0 };
  if (pool && pool->length == 16) memcpy(pv, pool->value, 16);
  CHECK(pv[0] == 0x8ff800000ull && pv[1] == 8 << 20);
  xl_adt_free(gt);

  // disks on PCI (xnuloader-crosvm, xnuloader-qemu --pci): a node per function, its configuration space as reg,
  // OneiOSHostedDisk's compatible, no board line
  machine_default(&m);
  m.ndev = 3;
  m.dev[0] = (xl_device){ XL_DEV_BLOCK, 0x10000 + (2 << 11), 0x100, true };
  m.dev[1] = (xl_device){ XL_DEV_BLOCK, 0x10000 + (3 << 11), 0x100, true };
  m.dev[2] = (xl_device){ XL_DEV_DATA_STORE, 0x10000 + (6 << 11), 0x100, true };
  xl_adt_node *pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *second = xl_adt_find(pt, "arm-io/virtio-pci-1"), *store = xl_adt_find(pt, "arm-io/data-store");
    CHECK(second && store && !xl_adt_find(pt, "arm-io/virtio-mmio-0"));
    p = second ? xl_adt_get(second, "compatible") : NULL;
    CHECK(p && p->length == 18 && !memcmp(p->value, "oneios,virtio-pci", 18));
    p = store ? xl_adt_get(store, "compatible") : NULL;
    CHECK(p && p->length == 18 && !memcmp(p->value, "oneios,virtio-pci", 18));
    p = second ? xl_adt_get(second, "reg") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x10000 + (3 << 11) - 0x1000 && r[1] == 0x100);
    CHECK(store && !xl_adt_get(store, "droidvm,intid") && xl_adt_get(store, "dma-coherent"));
    xl_adt_free(pt);
  }
  // an input device on PCI: the bus crosvm's virtio-input functions are found on (one node, the CAM's bus 0 as reg)
  machine_default(&m);
  m.dev[3] = (xl_device){ XL_DEV_INPUT, 0x10000, 0x10000, true };
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *bus = xl_adt_find(pt, "arm-io/input-pci");
    p = bus ? xl_adt_get(bus, "compatible") : NULL;
    CHECK(p && p->length == 24 && !memcmp(p->value, "oneios,virtio-input-pci", 24));
    p = bus ? xl_adt_get(bus, "reg") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x10000 - 0x1000 && r[1] == 0x10000);
    CHECK(bus && !xl_adt_get(bus, "droidvm,intid") && !xl_adt_find(pt, "arm-io/input-0"));
    xl_adt_free(pt);
  }
  // the GPU on PCI (xnuloader-crosvm --gpu): its function's configuration space in crosvm's ECAM as reg, the drm2kgsl
  // host pool beside it, no board line (the driver polls)
  machine_default(&m);
  m.dev[4] = (xl_device){ XL_DEV_GPU, 0x10000 + (12 << 15), 0x1000, true };
  m.gpu_pool_base = 0x15f800000ull; m.gpu_pool_size = 8 << 20;
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *gpu = xl_adt_find(pt, "arm-io/virtio-gpu");
    p = gpu ? xl_adt_get(gpu, "compatible") : NULL;
    CHECK(p && p->length == 22 && !memcmp(p->value, "oneios,virtio-gpu-pci", 22));
    p = gpu ? xl_adt_get(gpu, "reg") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x10000 + (12 << 15) - 0x1000 && r[1] == 0x1000);
    p = gpu ? xl_adt_get(gpu, "drm-host-pool") : NULL;
    uint64_t gp[2] = { 0, 0 };
    if (p && p->length == 16) memcpy(gp, p->value, 16);
    CHECK(gp[0] == 0x15f800000ull && gp[1] == 8 << 20);
    CHECK(gpu && !xl_adt_get(gpu, "droidvm,intid"));
    xl_adt_free(pt);
  }
  // with --gpu-intid: crosvm's line for the GPU's function as droidvm,intid on its node, and on no other PCI node; an INTID
  // that is not an SPI, and a line without a GPU on PCI, are refused
  machine_default(&m);
  m.dev[4] = (xl_device){ XL_DEV_GPU, 0x10000 + (12 << 15), 0x1000, true };
  m.dev[2].pci = true;
  m.gpu_intid = 36;
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    p = xl_adt_get(xl_adt_find(pt, "arm-io/virtio-gpu"), "droidvm,intid");
    uint32_t line = 0;
    if (p && p->length == 4) memcpy(&line, p->value, 4);
    CHECK(p && p->length == 4 && line == 36);
    const xl_adt_node *store = xl_adt_find(pt, "arm-io/data-store");
    CHECK(store && !xl_adt_get(store, "droidvm,intid"));
    xl_adt_free(pt);
  }
  m.gpu_intid = 31;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  m.gpu_intid = 1020;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  machine_default(&m);
  m.gpu_intid = 36;
  CHECK(xl_machine_tree(&m, &err) == NULL);    // the GPU of the default board is an MMIO window: its line is the board's
  // take18, --audio-intid N,SLOT: the sound card on PCI (bus 0) gets droidvm,intid and oneios,intid-slot; without it neither;
  // a line that is not an SPI, a slot out of 1..31, or a line without the card on PCI are refused
  machine_default(&m);
  m.dev[m.ndev++] = (xl_device){ XL_DEV_SND, 0x10000, 32 << 15, true };
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *snd = xl_adt_find(pt, "arm-io/virtio-snd");
    CHECK(snd && !xl_adt_get(snd, "droidvm,intid") && !xl_adt_get(snd, "oneios,intid-slot"));
    xl_adt_free(pt);
  }
  m.audio_intid = 44; m.audio_slot = 8;
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *snd = xl_adt_find(pt, "arm-io/virtio-snd");
    uint32_t line = 0, slot = 0;
    p = snd ? xl_adt_get(snd, "droidvm,intid") : NULL;
    if (p && p->length == 4) memcpy(&line, p->value, 4);
    CHECK(line == 44);
    p = snd ? xl_adt_get(snd, "oneios,intid-slot") : NULL;
    if (p && p->length == 4) memcpy(&slot, p->value, 4);
    CHECK(slot == 8);
    xl_adt_free(pt);
  }
  m.audio_intid = 31;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  m.audio_intid = 44; m.audio_slot = 0;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  m.audio_slot = 32;
  CHECK(xl_machine_tree(&m, &err) == NULL);
  machine_default(&m);
  m.audio_intid = 44; m.audio_slot = 8;
  CHECK(xl_machine_tree(&m, &err) == NULL);    // no sound card on PCI
  // the jetsam properties' name (xnuloader-crosvm --jetsam-product): /product jetsam-properties-product-type, with its
  // NUL, and nothing else: no target-type, and the tree without the property is the tree of a machine without the name
  machine_default(&m);
  xl_adt_node *plain = xl_machine_tree(&m, &err);
  CHECK(plain != NULL);
  if (plain) {
    CHECK(!xl_adt_get(xl_adt_find(plain, "product"), "jetsam-properties-product-type"));
    CHECK(!xl_adt_get(plain, "target-type"));
  }
  m.jetsam_product = "V54";
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt && plain) {
    xl_adt_node *pr = (xl_adt_node *)xl_adt_find(pt, "product");
    p = pr ? xl_adt_get(pr, "jetsam-properties-product-type") : NULL;
    CHECK(p && p->length == 4 && !memcmp(p->value, "V54", 4));
    CHECK(!xl_adt_get(pt, "target-type"));
    const size_t with = xl_adt_flat_size(pt), without = xl_adt_flat_size(plain);
    CHECK(with == without + 32 + 4 + 4);      // the property alone: its name, its length, 4 bytes of value
    CHECK(pr && xl_adt_unset(pr, "jetsam-properties-product-type") == 0);
    uint8_t *a = malloc(without), *b = malloc(without);
    CHECK(xl_adt_flat_size(pt) == without);
    if (a && b && xl_adt_flat_size(pt) == without) {
      xl_adt_flatten(pt, a);
      xl_adt_flatten(plain, b);
      CHECK(!memcmp(a, b, without));
    }
    free(a);
    free(b);
  }
  xl_adt_free(pt);
  xl_adt_free(plain);
  // a name that is no part of a file name is refused: empty, a dot first, a slash, a blank, 32 characters; 31 are taken
  CHECK(!xl_machine_jetsam_product_ok(NULL) && !xl_machine_jetsam_product_ok(""));
  static const char *const bad[] = { "", ".V54", "V54/..", "V 54", "V54\n", "0123456789012345678901234567890X" };
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
    m.jetsam_product = bad[i];
    err = NULL;
    CHECK(!xl_machine_jetsam_product_ok(bad[i]));
    CHECK(xl_machine_tree(&m, &err) == NULL && err && strstr(err, "jetsam"));
  }
  m.jetsam_product = "0123456789012345678901234567890";
  CHECK(strlen(m.jetsam_product) == 31 && xl_machine_jetsam_product_ok(m.jetsam_product));
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  xl_adt_free(pt);
  CHECK(xl_machine_jetsam_product_ok("V54") && xl_machine_jetsam_product_ok("J820-b_1.x"));
  // an identity tree's own name stays without the option, and the option's replaces it
  {
    xl_adt_node *donor = xl_adt_new("device-tree");
    xl_adt_set_str(donor, "model", "Fixture1,1");
    xl_adt_set_str(donor, "target-type", "F1");
    xl_adt_set(donor, "compatible", "F1AP\0Fixture1,1\0AppleARM", sizeof "F1AP\0Fixture1,1\0AppleARM");
    xl_adt_node *product = xl_adt_add(donor, "product");
    xl_adt_set_str(product, "product-name", "Fixture phone");
    xl_adt_set_str(product, "jetsam-properties-product-type", "F1b");
    machine_default(&m);
    m.identity = donor;
    pt = xl_machine_tree(&m, &err);
    CHECK(pt != NULL);
    if (pt) {
      p = xl_adt_get(xl_adt_find(pt, "product"), "jetsam-properties-product-type");
      CHECK(p && p->length == 4 && !memcmp(p->value, "F1b", 4));
      p = xl_adt_get(pt, "target-type");
      CHECK(p && p->length == 3 && !memcmp(p->value, "F1", 3));
      xl_adt_free(pt);
    }
    m.jetsam_product = "V54";
    pt = xl_machine_tree(&m, &err);
    CHECK(pt != NULL);
    if (pt) {
      p = xl_adt_get(xl_adt_find(pt, "product"), "jetsam-properties-product-type");
      CHECK(p && p->length == 4 && !memcmp(p->value, "V54", 4));
      p = xl_adt_get(pt, "target-type");
      CHECK(p && p->length == 3 && !memcmp(p->value, "F1", 3));     // the identity's own, not the option's
      xl_adt_free(pt);
    }
    xl_adt_free(donor);
  }
  // the sound card on PCI (xnuloader-crosvm --audio): all of bus 0 of crosvm's ECAM as reg, no board line (the driver polls)
  machine_default(&m);
  m.dev[4] = (xl_device){ XL_DEV_SND, 0x10000, 0x100000, true };
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *snd = xl_adt_find(pt, "arm-io/virtio-snd");
    p = snd ? xl_adt_get(snd, "compatible") : NULL;
    CHECK(p && p->length == 22 && !memcmp(p->value, "oneios,virtio-snd-pci", 22));
    p = snd ? xl_adt_get(snd, "reg") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x10000 - 0x1000 && r[1] == 0x100000);
    CHECK(snd && !xl_adt_get(snd, "droidvm,intid"));
    xl_adt_free(pt);
  }
  // the network card on PCI (xnuloader-crosvm --wifi): all of bus 0 of crosvm's ECAM as reg, no board line (the driver polls)
  machine_default(&m);
  m.dev[4] = (xl_device){ XL_DEV_NET, 0x10000, 0x100000, true };
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *net = xl_adt_find(pt, "arm-io/net");
    p = net ? xl_adt_get(net, "compatible") : NULL;
    CHECK(p && p->length == 22 && !memcmp(p->value, "oneios,virtio-net-pci", 22));
    p = net ? xl_adt_get(net, "reg") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x10000 - 0x1000 && r[1] == 0x100000);
    CHECK(net && !xl_adt_get(net, "droidvm,intid"));
    xl_adt_free(pt);
  }
  // the video decoder on PCI (xnuloader-crosvm --media): all of bus 0 of crosvm's ECAM as reg, crosvm's media_host pool as
  // media-host-pool, no board line (the driver polls)
  machine_default(&m);
  m.dev[4] = (xl_device){ XL_DEV_MEDIA, 0x10000, 0x100000, true };
  m.media_pool_base = 0x1f4000000ull;
  m.media_pool_size = 192ull << 20;
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *media = xl_adt_find(pt, "arm-io/virtio-media-0");
    p = media ? xl_adt_get(media, "compatible") : NULL;
    CHECK(p && p->length == 24 && !memcmp(p->value, "oneios,virtio-media-pci", 24));
    p = media ? xl_adt_get(media, "reg") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x10000 - 0x1000 && r[1] == 0x100000);
    p = media ? xl_adt_get(media, "media-host-pool") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x1f4000000ull && r[1] == (192ull << 20));
    CHECK(media && !xl_adt_get(media, "droidvm,intid"));
    xl_adt_free(pt);
  }
  machine_default(&m); m.dev[4] = (xl_device){ XL_DEV_BRIDGE, 0x10000 + (7 << 11), 0x100, true };   // the bridge: not on PCI
  CHECK(xl_machine_tree(&m, &err) == NULL);
  // the bridge on a serial port (xnuloader-crosvm --bridge): crosvm's second ns16550a, its reg absolute as the console's
  machine_default(&m);
  m.uart_base = 0x3f8; m.uart_size = 8; m.uart_ns16550 = true;
  m.bridge_uart_base = 0x2f8; m.bridge_uart_size = 8;
  pt = xl_machine_tree(&m, &err);
  CHECK(pt != NULL);
  if (pt) {
    const xl_adt_node *bridge = xl_adt_find(pt, "arm-io/bridge");
    p = bridge ? xl_adt_get(bridge, "compatible") : NULL;
    CHECK(p && p->length == 19 && !memcmp(p->value, "oneios,bridge-uart", 19));
    p = bridge ? xl_adt_get(bridge, "reg") : NULL;
    if (p && p->length == 16) memcpy(r, p->value, 16);
    CHECK(p && p->length == 16 && r[0] == 0x2f8 && r[1] == 8);
    CHECK(bridge && !xl_adt_get(bridge, "droidvm,intid"));
    xl_adt_free(pt);
  }
  m.bridge_uart_base = 0x3f8;                                            // the console's own port: refused
  CHECK(xl_machine_tree(&m, &err) == NULL);
  machine_default(&m);                                                   // and never beside the virtio bridge
  m.dev[4] = (xl_device){ XL_DEV_BRIDGE, 0x10000 + (7 << 11), 0x100, false };
  m.bridge_uart_base = 0x2f8; m.bridge_uart_size = 8;
  CHECK(xl_machine_tree(&m, &err) == NULL);
}

// What xnuloader-crosvm's identity options make (--identity-tree with --identity-groups, --memory-class, --soc-generation,
// --display-corner-radius): the six groups of properties (five of /product's, one of the root's), each a switch of its own and
// none on by default, the memory class of the VM's RAM, /arm-io's soc-generation, the corner radius, and xnuhost's whole merge as
// it was. fx_identity's tree has what each group takes and what none does.

// the /product properties each group takes from fx_identity's tree, by the group's bit number (the artwork group's gamut is the
// machine's own, written with the group); the root group takes none
static const char *const group_props[XL_IDENTITY_GROUPS][7] = {
  { "artwork-device-idiom", "artwork-device-subtype", "artwork-dynamic-displaymode", "graphics-featureset-class",
    "graphics-featureset-fallbacks", "artwork-display-gamut" },
  { "ui-background-quality", "ui-weather-quality" },
  { "display-workgroup-interval", "hid-workgroup-interval" },
  { "udid-version", "product-description", "public-key-accelerator" },
  { "device-perf-memory-class" },
  { NULL },
};
// what /product has with no group on, for the machine of machine_display with the jetsam name V54
static const char *const base_props[] = { "product-name", "model", "artwork-scale-factor", "jetsam-properties-product-type" };
// the phone's body, the exclaves and the rest that no group takes, as fx_identity has them
static const char *const body_names[] = {
  "island-notch-location", "supports-camera-button", "supports-burnin-mitigation", "has-virtualization", "display-corner-radius",
  "exclaves-fixture", "exclaves-enabled", "has-exclaves", "oled-display", "device-colors", "AAPL,phandle",
};
#define NBODY   (sizeof body_names / sizeof *body_names)
#define NBASE   (sizeof base_props / sizeof *base_props)

// a /product property's little-endian value if it is exactly `width` bytes, else all ones
static uint64_t product_int(const xl_adt_node *tree, const char *name, uint32_t width)
{
  const xl_adt_prop *p = xl_adt_get(xl_adt_find(tree, "product"), name);
  uint64_t v = 0;
  if (!p || p->length != width) return ~0ull;
  for (uint32_t i = width; i--; ) v = v << 8 | p->value[i];
  return v;
}

// a /product property that is exactly `text` and its NUL
static int product_str(const xl_adt_node *tree, const char *name, const char *text)
{
  const xl_adt_prop *p = xl_adt_get(xl_adt_find(tree, "product"), name);
  return p && p->length == strlen(text) + 1 && !memcmp(p->value, text, p->length);
}

// a /product property that is exactly these bytes
static int product_bytes(const xl_adt_node *tree, const char *name, const void *bytes, uint32_t length)
{
  const xl_adt_prop *p = xl_adt_get(xl_adt_find(tree, "product"), name);
  return p && p->length == length && !memcmp(p->value, bytes, length);
}

// a root property that is exactly these bytes
static int root_is(const xl_adt_node *tree, const char *name, const void *bytes, size_t length)
{
  const xl_adt_prop *p = tree ? xl_adt_get(tree, name) : NULL;
  return p && p->length == length && !memcmp(p->value, bytes, length);
}

// a /product property of the tree as the donor's own is, byte for byte
static int product_is_donors(const xl_adt_node *tree, const xl_adt_node *donor_product, const char *name)
{
  const xl_adt_prop *p = xl_adt_get(xl_adt_find(tree, "product"), name), *d = xl_adt_get(donor_product, name);
  return p && d && p->length == d->length && !memcmp(p->value, d->value, d->length);
}

static unsigned product_count(const xl_adt_node *tree) { return xl_adt_find(tree, "product")->nprops; }

// /product has exactly the `n` properties named (and the node's own name): as many, and each of them
static int product_has_only(const xl_adt_node *tree, const char *const *names, unsigned n)
{
  const xl_adt_node *pr = xl_adt_find(tree, "product");
  if (!pr || pr->nprops != n + 1 || !xl_adt_get(pr, "name")) return 0;
  for (unsigned i = 0; i < n; i++)
    if (!xl_adt_get(pr, names[i])) return 0;
  return 1;
}

// the names /product must have for a mask of groups: the base and each group's properties
static unsigned expected_props(unsigned mask, const char **out)
{
  unsigned n = 0;
  for (unsigned i = 0; i < NBASE; i++) out[n++] = base_props[i];
  for (unsigned g = 0; g < XL_IDENTITY_GROUPS; g++)
    for (unsigned i = 0; (mask >> g & 1) && group_props[g][i]; i++) out[n++] = group_props[g][i];
  return n;
}

// the machine of the tests below: the default board with a display of the take18 phone
static void machine_display(xl_machine *m)
{
  machine_default(m);
  m->display_width = 720; m->display_height = 1560; m->display_scale = 2; m->display_hz = 120;
}

// the two trees' flat forms are the same bytes once /product is taken out of both and, with `root`, the root's target-type and
// compatible too (they are freed)
static int same_but(xl_adt_node *a, xl_adt_node *b, int root)
{
  if (!a || !b) {
    xl_adt_free(a);
    xl_adt_free(b);
    return 0;
  }
  xl_adt_remove(a, "product");
  xl_adt_remove(b, "product");
  if (root) {
    xl_adt_unset(a, "target-type"); xl_adt_unset(b, "target-type");
    xl_adt_unset(a, "compatible"); xl_adt_unset(b, "compatible");
  }
  const size_t n = xl_adt_flat_size(a);
  int same = n == xl_adt_flat_size(b);
  uint8_t *x = malloc(n), *y = malloc(n);
  if (same) {
    xl_adt_flatten(a, x);
    xl_adt_flatten(b, y);
    same = !memcmp(x, y, n);
  }
  free(x);
  free(y);
  xl_adt_free(a);
  xl_adt_free(b);
  return same;
}

// the two machines' trees are the same bytes, /product and all (both are freed)
static int same_tree(const xl_machine *a, const xl_machine *b)
{
  const char *err = NULL;
  xl_adt_node *x = xl_machine_tree(a, &err), *y = xl_machine_tree(b, &err);
  int same = x && y && xl_adt_flat_size(x) == xl_adt_flat_size(y);
  if (same) {
    const size_t n = xl_adt_flat_size(x);
    uint8_t *p = malloc(n), *q = malloc(n);
    xl_adt_flatten(x, p);
    xl_adt_flatten(y, q);
    same = !memcmp(p, q, n);
    free(p);
    free(q);
  }
  xl_adt_free(x);
  xl_adt_free(y);
  return same;
}

// the class a machine with `donor` as the source of the memclass group and a VM of `ram` MiB gets, as a property of `width` bytes;
// all ones where the tree was refused or has no such property
static uint64_t class_for(xl_machine *m, const xl_adt_node *donor, uint32_t ram, uint32_t width)
{
  const char *err = NULL;
  m->identity_product = donor;
  m->identity_groups = XL_IDENTITY_MEMCLASS;
  m->vm_ram_mib = ram;
  xl_adt_node *t = xl_machine_tree(m, &err);
  const uint64_t v = t ? product_int(t, "device-perf-memory-class", width) : ~0ull;
  xl_adt_free(t);
  return v;
}

static int refused(const xl_machine *m, const char *word)
{
  const char *err = NULL;
  xl_adt_node *t = xl_machine_tree(m, &err);
  xl_adt_free(t);
  return !t && err && strstr(err, word);
}

static void test_identity_groups(void)
{
  xl_machine m;
  const char *err = NULL, *names[40];
  xl_adt_node *donor = fx_identity(), *dp = xl_adt_find(donor, "product"), *t;

  // the group of each property, the names, and the list of groups as --identity-groups reads it
  static const struct { const char *name; unsigned group; } of[] = {
    { "artwork-device-idiom", XL_IDENTITY_ARTWORK }, { "artwork-device-subtype", XL_IDENTITY_ARTWORK },
    { "artwork-dynamic-displaymode", XL_IDENTITY_ARTWORK }, { "artwork-anything-else", XL_IDENTITY_ARTWORK },
    { "graphics-featureset-class", XL_IDENTITY_ARTWORK }, { "graphics-featureset-fallbacks", XL_IDENTITY_ARTWORK },
    { "ui-background-quality", XL_IDENTITY_QUALITY }, { "ui-weather-quality", XL_IDENTITY_QUALITY },
    { "display-workgroup-interval", XL_IDENTITY_WORKGROUP }, { "hid-workgroup-interval", XL_IDENTITY_WORKGROUP },
    { "udid-version", XL_IDENTITY_IDS }, { "product-description", XL_IDENTITY_IDS }, { "public-key-accelerator", XL_IDENTITY_IDS },
    { "device-perf-memory-class", XL_IDENTITY_MEMCLASS },
    { "artwork-display-gamut", 0 }, { "artwork-scale-factor", 0 }, { "artwork", 0 }, { "product-name", 0 }, { "model", 0 },
    { "target-type", 0 }, { "compatible", 0 }, { "display-corner-radius", 0 }, { "island-notch-location", 0 }, { "name", 0 },
    { "jetsam-properties-product-type", 0 }, { "soc-generation", 0 }, { "ui-quality", 0 }, { "", 0 },
  };
  for (unsigned i = 0; i < sizeof of / sizeof *of; i++) CHECK(xl_identity_group_of(of[i].name) == of[i].group);
  static const char *const group_name[XL_IDENTITY_GROUPS] = { "artwork", "quality", "workgroup", "ids", "memclass", "root" };
  for (unsigned g = 0; g < XL_IDENTITY_GROUPS; g++) {
    const char *name = xl_identity_group_name(1u << g);
    CHECK(name && !strcmp(name, group_name[g]));
  }
  CHECK(!xl_identity_group_name(0) && !xl_identity_group_name(3) && !xl_identity_group_name(1u << XL_IDENTITY_GROUPS) &&
        !xl_identity_group_name(XL_IDENTITY_ALL));
  CHECK(XL_IDENTITY_ALL == 0x3f && XL_IDENTITY_ARTWORK == 1 && XL_IDENTITY_ROOT == 0x20);
  {
    char bad[16];
    unsigned mask = 0x80;
    static const struct { const char *list; unsigned mask; } good[] = {
      { "artwork", XL_IDENTITY_ARTWORK }, { "a", XL_IDENTITY_ARTWORK }, { "quality", XL_IDENTITY_QUALITY }, { "b", XL_IDENTITY_QUALITY },
      { "workgroup", XL_IDENTITY_WORKGROUP }, { "c", XL_IDENTITY_WORKGROUP }, { "ids", XL_IDENTITY_IDS }, { "d", XL_IDENTITY_IDS },
      { "memclass", XL_IDENTITY_MEMCLASS }, { "e", XL_IDENTITY_MEMCLASS }, { "root", XL_IDENTITY_ROOT }, { "f", XL_IDENTITY_ROOT },
      { "all", XL_IDENTITY_ALL }, { "artwork,ids", XL_IDENTITY_ARTWORK | XL_IDENTITY_IDS }, { "a,d", XL_IDENTITY_ARTWORK | XL_IDENTITY_IDS },
      { "d,artwork", XL_IDENTITY_ARTWORK | XL_IDENTITY_IDS }, { "quality,c,root", XL_IDENTITY_QUALITY | XL_IDENTITY_WORKGROUP | XL_IDENTITY_ROOT },
      { "artwork,artwork,a", XL_IDENTITY_ARTWORK }, { "all,root", XL_IDENTITY_ALL }, { "a,b,c,d,e,f", XL_IDENTITY_ALL },
      { "artwork,quality,workgroup,ids,memclass,root", XL_IDENTITY_ALL },
    };
    for (unsigned i = 0; i < sizeof good / sizeof *good; i++) {
      mask = 0x80;
      CHECK(xl_identity_groups_parse(good[i].list, &mask, bad, sizeof bad) && mask == good[i].mask);
    }
    static const struct { const char *list; const char *word; } none[] = {
      { "", "" }, { ",", "" }, { "artwork,", "" }, { ",artwork", "" }, { "artwork,,ids", "" }, { "foo", "foo" }, { "artwork,foo", "foo" },
      { "foo,artwork", "foo" }, { "g", "g" }, { "A", "A" }, { "Artwork", "Artwork" }, { "ALL", "ALL" }, { " artwork", " artwork" },
      { "artwork ", "artwork " }, { "art", "art" }, { "artworks", "artworks" }, { "none", "none" }, { "ab", "ab" }, { "a;b", "a;b" },
      { "allx", "allx" }, { "al", "al" }, { "all1", "all1" }, { "a,allroot", "allroot" }, { "roo", "roo" }, { "rootx", "rootx" },
      { "artwork,ids,0123456789abcdef0123", "0123456789abcde" },       // the word is cut to fit `bad` (16 bytes)
    };
    for (unsigned i = 0; i < sizeof none / sizeof *none; i++) {
      mask = 0x80;
      memset(bad, 'x', sizeof bad);
      CHECK(!xl_identity_groups_parse(none[i].list, &mask, bad, sizeof bad) && mask == 0x80 && !strcmp(bad, none[i].word));
    }
    CHECK(!xl_identity_groups_parse(NULL, &mask, bad, sizeof bad) && mask == 0x80 && !bad[0]);
    CHECK(xl_identity_groups_parse("ids", &mask, NULL, 0) && mask == XL_IDENTITY_IDS && !xl_identity_groups_parse("zz", &mask, NULL, 0));
  }

  // no identity field, or a source and no group: /product is what it was (its name, product-name and model; with a display the
  // scale), the root is the machine's own, nothing is written for the SoC or the panel, and the tree is the same bytes
  machine_default(&m);
  t = xl_machine_tree(&m, &err);
  CHECK(t && product_count(t) == 3 && product_str(t, "product-name", "OneiOS guest") && product_str(t, "model", "oneios,xnuhost"));
  CHECK(t && !xl_adt_get(xl_adt_find(t, "arm-io"), "soc-generation") && !xl_adt_get(xl_adt_find(t, "product"), "display-corner-radius"));
  CHECK(t && !xl_adt_get(xl_adt_find(t, "product"), "artwork-display-gamut") && !xl_adt_get(t, "target-type"));
  xl_adt_free(t);
  machine_display(&m);
  t = xl_machine_tree(&m, &err);
  CHECK(t && product_count(t) == 4 && product_int(t, "artwork-scale-factor", 4) == 2);
  xl_adt_free(t);
  {
    xl_machine with = m;
    with.identity_product = donor;                      // the tree alone copies nothing
    CHECK(same_tree(&m, &with));
    with.vm_ram_mib = 4608;                             // the RAM is read only with the memclass group
    CHECK(same_tree(&m, &with));
    with.jetsam_product = m.jetsam_product = "V54";
    CHECK(same_tree(&m, &with));
    with.display_gamut = "sRGB";                        // the gamut is written only with the artwork group
    CHECK(same_tree(&m, &with));
    with.identity_groups = 0;
    CHECK(same_tree(&m, &with));
  }

  // each group alone, then each mask of groups: /product has exactly the base and the properties of those groups, whatever else
  // the tree has; outside /product the tree is the machine's without the identity, byte for byte (the root group aside, which
  // changes the root's target-type and compatible alone)
  for (unsigned mask = 0; mask < 1u << XL_IDENTITY_GROUPS; mask++) {
    machine_display(&m);
    m.jetsam_product = "V54";
    m.identity_product = donor;
    m.identity_groups = mask;
    m.vm_ram_mib = 4608;
    xl_machine without = m;
    without.identity_product = NULL; without.identity_groups = 0; without.vm_ram_mib = 0;
    const unsigned n = expected_props(mask, names);
    t = xl_machine_tree(&m, &err);
    CHECK(t && product_has_only(t, names, n));
    CHECK(t && xl_adt_find(t, "product")->nchildren == 0);
    CHECK(t && !xl_adt_get(xl_adt_find(t, "arm-io"), "soc-generation"));
    CHECK(t && !!xl_adt_get(t, "target-type") == !!(mask & XL_IDENTITY_ROOT));
    xl_adt_free(t);
    CHECK(same_but(xl_machine_tree(&m, &err), xl_machine_tree(&without, &err), !!(mask & XL_IDENTITY_ROOT)));
  }

  // the values: the tree's own bytes but for the featureset list (APPLE2 and MTL2,2 out) and the class (the VM's, 4608 MiB: 4); the
  // gamut and the scale are the machine's (not SRGB and 3); the root group's target-type and board name
  machine_display(&m);
  m.jetsam_product = "V54";
  m.identity_product = donor;
  m.identity_groups = XL_IDENTITY_ALL;
  m.vm_ram_mib = 4608;
  t = xl_machine_tree(&m, &err);
  CHECK(t != NULL);
  if (t) {
    for (unsigned g = 0; g < XL_IDENTITY_GROUPS; g++)
      for (unsigned i = 0; group_props[g][i]; i++) {
        const char *name = group_props[g][i];
        if (strcmp(name, "graphics-featureset-fallbacks") && strcmp(name, "device-perf-memory-class") && strcmp(name, "artwork-display-gamut"))
          CHECK(product_is_donors(t, dp, name));
      }
    CHECK(product_str(t, "graphics-featureset-fallbacks", "APPLE9:APPLE3v1:APPLE1:GLES2,0"));
    CHECK(product_int(t, "device-perf-memory-class", 4) == 3);   // 4608 MiB: CoreUI's 4 GB class (take23)
    CHECK(product_str(t, "artwork-display-gamut", "P3") && product_int(t, "artwork-scale-factor", 4) == 2);
    CHECK(product_str(t, "product-name", "OneiOS guest") && product_str(t, "model", "oneios,xnuhost") &&
          product_str(t, "jetsam-properties-product-type", "V54"));
    for (unsigned i = 0; i < NBODY; i++) CHECK(!xl_adt_get(xl_adt_find(t, "product"), body_names[i]));
    CHECK(!xl_adt_find(t, "product/iopm") && !xl_adt_find(t, "product/camera") && !xl_adt_find(t, "product/audio"));
    CHECK(root_is(t, "target-type", "F1", sizeof "F1"));
    CHECK(root_is(t, "compatible", "F1AP\0oneios,xnuhost", sizeof "F1AP\0oneios,xnuhost"));
    CHECK(root_is(t, "model", "iPhone18,2", sizeof "iPhone18,2"));                   // not the tree's "Fixture1,1"
    xl_adt_free(t);
  }
  // the root group alone: only the root's target-type and compatible change (and not /product), and the root without it has neither
  machine_display(&m);
  m.identity_product = donor;
  m.identity_groups = XL_IDENTITY_ROOT;
  t = xl_machine_tree(&m, &err);
  CHECK(t && product_count(t) == 4 && product_str(t, "model", "oneios,xnuhost"));        // name, product-name, the scale, model
  CHECK(root_is(t, "target-type", "F1", sizeof "F1") && root_is(t, "compatible", "F1AP\0oneios,xnuhost", sizeof "F1AP\0oneios,xnuhost"));
  xl_adt_free(t);
  m.identity_groups = 0;
  t = xl_machine_tree(&m, &err);
  CHECK(t && !xl_adt_get(t, "target-type") && root_is(t, "compatible", "oneios,xnuhost", sizeof "oneios,xnuhost"));
  xl_adt_free(t);
  // a source with a longer compatible: its first entry alone, then the machine's own
  {
    xl_adt_node *other = fx_identity();
    xl_adt_set(other, "compatible", "J999AP\0iPhone99,9\0AppleARM", sizeof "J999AP\0iPhone99,9\0AppleARM");
    xl_adt_set_str(other, "target-type", "J999");
    m.identity_product = other;
    m.identity_groups = XL_IDENTITY_ROOT;
    t = xl_machine_tree(&m, &err);
    CHECK(root_is(t, "compatible", "J999AP\0oneios,xnuhost", sizeof "J999AP\0oneios,xnuhost") && root_is(t, "target-type", "J999", sizeof "J999"));
    xl_adt_free(t);
    // a root without what the group takes: no target-type, an empty one, no compatible, an empty one, one with no first entry
    xl_adt_unset(other, "target-type");
    CHECK(refused(&m, "root group"));
    xl_adt_set(other, "target-type", "", 0);
    CHECK(refused(&m, "root group"));
    xl_adt_set_str(other, "target-type", "J999");
    xl_adt_unset(other, "compatible");
    CHECK(refused(&m, "root group"));
    xl_adt_set(other, "compatible", "", 0);
    CHECK(refused(&m, "root group"));
    xl_adt_set(other, "compatible", "\0X", 2);
    CHECK(refused(&m, "root group"));
    // the other groups need no root of the source
    m.identity_groups = XL_IDENTITY_ALL & ~XL_IDENTITY_ROOT;
    m.vm_ram_mib = 4608;
    t = xl_machine_tree(&m, &err);
    CHECK(t && !xl_adt_get(t, "target-type"));
    xl_adt_free(t);
    xl_adt_free(other);
  }
  m.identity_product = donor;

  // the gamut is the machine's, written with the artwork group alone; a display_gamut replaces the "P3"
  machine_default(&m);
  m.identity_product = donor; m.identity_groups = XL_IDENTITY_ARTWORK; m.display_gamut = "sRGB";
  t = xl_machine_tree(&m, &err);
  CHECK(t && !xl_adt_get(xl_adt_find(t, "product"), "artwork-scale-factor") && product_str(t, "artwork-display-gamut", "sRGB"));
  xl_adt_free(t);
  m.identity_groups = XL_IDENTITY_QUALITY | XL_IDENTITY_WORKGROUP | XL_IDENTITY_IDS | XL_IDENTITY_MEMCLASS | XL_IDENTITY_ROOT;
  m.vm_ram_mib = 4608;
  t = xl_machine_tree(&m, &err);
  CHECK(t && !xl_adt_get(xl_adt_find(t, "product"), "artwork-display-gamut"));
  xl_adt_free(t);

  // a property that is a placeholder is not copied (iBoot's recipe for a value, not the value); the others of its group still are
  ((xl_adt_prop *)xl_adt_get(dp, "product-description"))->flags = 1;
  m.identity_groups = XL_IDENTITY_IDS;
  t = xl_machine_tree(&m, &err);
  CHECK(t && !xl_adt_get(xl_adt_find(t, "product"), "product-description") && xl_adt_get(xl_adt_find(t, "product"), "udid-version") &&
        xl_adt_get(xl_adt_find(t, "product"), "public-key-accelerator") && product_count(t) == 3 + 2);
  xl_adt_free(t);
  ((xl_adt_prop *)xl_adt_get(dp, "product-description"))->flags = 0;
  // a tree with none of the properties writes none (but for the gamut of the artwork group)
  {
    xl_adt_node *poor = xl_adt_new("device-tree");
    xl_adt_add(poor, "product");
    machine_default(&m);
    m.identity_product = poor;
    m.identity_groups = XL_IDENTITY_ALL & ~XL_IDENTITY_ROOT;
    m.vm_ram_mib = 4608;
    t = xl_machine_tree(&m, &err);
    CHECK(t && product_count(t) == 4 && product_str(t, "artwork-display-gamut", "P3"));    // name, product-name, model, the gamut
    xl_adt_free(t);
    m.identity_groups = XL_IDENTITY_MEMCLASS;
    t = xl_machine_tree(&m, &err);                                                      // no class in the tree and none asked for
    CHECK(t && product_count(t) == 3);
    xl_adt_free(t);
    xl_adt_free(poor);
  }

  // the memory class of the VM's RAM (take23): the largest of CoreUI's classes its RAM holds, at most Apple's own (12). The value
  // is CoreUI's, not the GiB: 4 is its 3 GB class, 3 its 4 GB one, 15 its 1.5 GB one, and there is no 5 or 7
  static const struct { uint32_t ram, expect; } scaled[] = {
    { 512, 0 }, { 1023, 0 }, { 1024, 1 }, { 1536, 15 }, { 2048, 2 }, { 3072, 4 }, { 4096, 3 }, { 4352, 3 }, { 4608, 3 }, { 5120, 3 },
    { 5632, 3 }, { 6144, 6 }, { 6656, 6 }, { 7168, 6 }, { 8192, 8 }, { 9216, 9 }, { 12288, 12 }, { 16384, 12 }, { 65536, 12 },
  };
  CHECK(xl_machine_memory_class_mib(3) == 4096 && xl_machine_memory_class_mib(4) == 3072 && xl_machine_memory_class_mib(0) == 512 &&
        xl_machine_memory_class_mib(5) == 0 && xl_machine_memory_class_mib(7) == 0 && xl_machine_memory_class_mib(15) == 1536);
  machine_default(&m);
  for (unsigned i = 0; i < sizeof scaled / sizeof *scaled; i++) CHECK(class_for(&m, donor, scaled[i].ram, 4) == scaled[i].expect);
  // where Apple's is smaller the tool's RAM can reach the upper bound (by size); one that is 0 is none
  xl_adt_set_u32(dp, "device-perf-memory-class", 6);
  CHECK(class_for(&m, donor, 8192, 4) == 6 && class_for(&m, donor, 4608, 4) == 3 && class_for(&m, donor, 512, 4) == 0);
  xl_adt_set_u32(dp, "device-perf-memory-class", 1);
  CHECK(class_for(&m, donor, 4608, 4) == 1 && class_for(&m, donor, 512, 4) == 0);
  xl_adt_set_u32(dp, "device-perf-memory-class", 0);
  CHECK(class_for(&m, donor, 4608, 4) == 3 && class_for(&m, donor, 65536, 4) == 16 && xl_machine_memory_class_max(donor) == 16);
  // written in the width Apple's property has (1, 2, 4 or 8 bytes); another width is refused
  static const uint8_t twelve[8] = { 12 };
  for (uint32_t w = 1; w <= 8; w *= 2) {
    xl_adt_set(dp, "device-perf-memory-class", twelve, w);
    CHECK(class_for(&m, donor, 4608, w) == 3 && class_for(&m, donor, 65536, w) == 12 && xl_machine_memory_class_max(donor) == 12);
  }
  for (uint32_t w = 0; w <= 8; w += w == 0 ? 3 : 4) {          // 0, 3, 7 bytes
    xl_adt_set(dp, "device-perf-memory-class", twelve, w);
    m.identity_product = donor; m.identity_groups = XL_IDENTITY_MEMCLASS; m.vm_ram_mib = 4608;
    CHECK(refused(&m, "device-perf-memory-class"));
    m.identity_groups = XL_IDENTITY_QUALITY;                   // ... and read only with the memclass group
    t = xl_machine_tree(&m, &err);
    CHECK(t != NULL);
    xl_adt_free(t);
  }
  xl_adt_set_u32(dp, "device-perf-memory-class", 12);
  // --memory-class N: N itself (1 to Apple's 12); more is refused; with no such property in the tree 1 to 16, in 4 bytes, and
  // none written without N
  m.memory_class = 6;
  CHECK(class_for(&m, donor, 4608, 4) == 6 && class_for(&m, donor, 65536, 4) == 6);
  m.memory_class = 12;
  CHECK(class_for(&m, donor, 4608, 4) == 12);
  m.memory_class = 1;
  CHECK(class_for(&m, donor, 4608, 4) == 1);
  m.memory_class = 13;
  CHECK(refused(&m, "memory class"));
  xl_adt_unset(dp, "device-perf-memory-class");
  CHECK(xl_machine_memory_class_max(donor) == 16);
  m.memory_class = 16;
  CHECK(class_for(&m, donor, 4608, 4) == 16);
  m.memory_class = 17;
  CHECK(refused(&m, "memory class"));
  m.memory_class = 0;
  CHECK(class_for(&m, donor, 4608, 4) == ~0ull);               // no property in the tree, none asked for: none written
  xl_adt_set_u32(dp, "device-perf-memory-class", 12);
  // the RAM of the VM is needed where Apple's 12 would be copied (a class of 12 is not this VM's), not where N is given
  machine_default(&m);
  m.identity_product = donor; m.identity_groups = XL_IDENTITY_MEMCLASS;
  CHECK(refused(&m, "RAM"));
  m.memory_class = 5;
  CHECK(class_for(&m, donor, 0, 4) == 5);
  // a memory class is for the memclass group only: with no group, with others, with no tree at all
  for (unsigned groups = 0; groups < 1u << XL_IDENTITY_GROUPS; groups++) {
    machine_default(&m);
    m.identity_product = donor; m.identity_groups = groups; m.vm_ram_mib = 4608; m.memory_class = 4;
    if (groups & XL_IDENTITY_MEMCLASS) {
      t = xl_machine_tree(&m, &err);
      CHECK(t && product_int(t, "device-perf-memory-class", 4) == 4);
      xl_adt_free(t);
    } else {
      CHECK(refused(&m, "memclass"));
    }
  }
  machine_default(&m);
  m.memory_class = 4;
  CHECK(refused(&m, "memclass"));
  m.identity_groups = XL_IDENTITY_MEMCLASS;                       // the group, but no tree
  CHECK(refused(&m, "identity groups need"));

  // refused: a mask with no tree, a bit that is no group, a tree without /product, a tree for both ways
  machine_default(&m);
  m.identity_groups = XL_IDENTITY_ARTWORK;
  CHECK(refused(&m, "identity groups need"));
  m.identity_groups = XL_IDENTITY_ALL;
  CHECK(refused(&m, "identity groups need"));
  m.identity = donor;                                           // the whole identity is not the source of groups
  CHECK(refused(&m, "identity groups need"));
  machine_default(&m);
  m.identity_product = donor;
  for (unsigned bit = XL_IDENTITY_GROUPS; bit < 32; bit++) {
    m.identity_groups = 1u << bit;
    CHECK(refused(&m, "does not exist"));
    m.identity_groups = XL_IDENTITY_ALL | 1u << bit;
    CHECK(refused(&m, "does not exist"));
  }
  xl_adt_node *bare = xl_adt_new("device-tree");
  m.identity_groups = 0;
  m.identity_product = bare;
  CHECK(refused(&m, "/product"));
  m.identity_groups = XL_IDENTITY_ALL;
  CHECK(refused(&m, "/product"));
  m.identity_product = donor; m.identity = donor;
  m.identity_groups = 0;
  CHECK(refused(&m, "not both"));
  m.identity_groups = XL_IDENTITY_ARTWORK;
  CHECK(refused(&m, "not both"));
  xl_adt_free(bare);

  // xnuhost's merge (only `identity`) is as it was: the body is copied, the class is Apple's 12, the exclaves keys the old list drops
  machine_display(&m);
  m.identity = donor;
  t = xl_machine_tree(&m, &err);
  CHECK(t != NULL);
  if (t) {
    const xl_adt_node *pr = xl_adt_find(t, "product");
    for (unsigned i = 0; i < 5; i++) CHECK(xl_adt_get(pr, body_names[i]));             // the notch, the camera button, ... the corner radius
    CHECK(xl_adt_get(pr, "exclaves-fixture"));
    CHECK(product_int(t, "island-notch-location", 4) == 0x90 && product_int(t, "display-corner-radius", 8) == 0x10000003eull);
    CHECK(product_int(t, "device-perf-memory-class", 4) == 12);
    CHECK(!xl_adt_get(pr, "exclaves-enabled") && !xl_adt_get(pr, "has-exclaves") && !xl_adt_get(pr, "AAPL,phandle"));
    CHECK(product_count(t) == 27);        // the tree's 29 less the 5 the old list leaves out, plus name, the gamut and model
    CHECK(product_str(t, "product-name", "Fixture phone") && product_str(t, "artwork-display-gamut", "P3") && product_int(t, "artwork-scale-factor", 4) == 2);
    CHECK(product_str(t, "graphics-featureset-fallbacks", "APPLE9:APPLE3v1:APPLE1:GLES2,0"));
    CHECK(xl_adt_get(xl_adt_find(t, "product/audio"), "acoustic-id") && xl_adt_get(xl_adt_find(t, "product/audio"), "wireless-splitter"));
    CHECK(!xl_adt_get(xl_adt_find(t, "arm-io"), "soc-generation"));
    CHECK(root_is(t, "model", "Fixture1,1", sizeof "Fixture1,1") && root_is(t, "target-type", "F1", sizeof "F1"));
    CHECK(root_is(t, "compatible", "F1AP\0oneios,xnuhost", sizeof "F1AP\0oneios,xnuhost"));
    xl_adt_free(t);
  }
  // the machine's own corner radius replaces the identity's (xl_adt_set keeps one property of the name)
  m.has_display_corner_radius = true; m.display_corner_radius = 30; m.display_corner_word2 = 1;
  t = xl_machine_tree(&m, &err);
  {
    static const uint8_t thirty[8] = { 30, 0, 0, 0, 1, 0, 0, 0 };
    CHECK(t && product_bytes(t, "display-corner-radius", thirty, 8) && product_count(t) == 27);
  }
  xl_adt_free(t);
  m.identity = NULL;

  xl_adt_free(donor);
}

static void test_identity_misc(void)
{
  xl_machine m;
  const char *err = NULL;
  xl_adt_node *t;
  xl_adt_node *donor = fx_identity();

  // /arm-io soc-generation: a string and its NUL, nothing else of the tree changes (36 + 4 bytes in the flat form), none by default
  machine_default(&m);
  xl_adt_node *without = xl_machine_tree(&m, &err);
  m.soc_generation = "H18";
  t = xl_machine_tree(&m, &err);
  CHECK(t && without);
  if (t && without) {
    const xl_adt_prop *soc = xl_adt_get(xl_adt_find(t, "arm-io"), "soc-generation");
    CHECK(soc && soc->length == 4 && !memcmp(soc->value, "H18", 4));
    CHECK(!xl_adt_get(xl_adt_find(t, "arm-io"), "compatible") && !xl_adt_get(xl_adt_find(without, "arm-io"), "soc-generation"));
    CHECK(xl_adt_flat_size(t) == xl_adt_flat_size(without) + 36 + 4);
    CHECK(xl_adt_unset(xl_adt_find(t, "arm-io"), "soc-generation") == 0);
    const size_t n = xl_adt_flat_size(t);
    uint8_t *a = malloc(n), *b = malloc(n);
    CHECK(n == xl_adt_flat_size(without));
    if (n == xl_adt_flat_size(without)) {
      xl_adt_flatten(t, a);
      xl_adt_flatten(without, b);
      CHECK(!memcmp(a, b, n));
    }
    free(a);
    free(b);
  }
  xl_adt_free(t);
  xl_adt_free(without);
  m.soc_generation = "H17G";
  t = xl_machine_tree(&m, &err);
  CHECK(t && xl_adt_get(xl_adt_find(t, "arm-io"), "soc-generation")->length == 5);
  xl_adt_free(t);
  static const char *const bad_soc[] = { "", "H 18", "H18/x", "H18\n", "0123456789012345678901234567890X" };
  for (unsigned i = 0; i < sizeof bad_soc / sizeof *bad_soc; i++) {
    m.soc_generation = bad_soc[i];
    CHECK(!xl_machine_soc_generation_ok(bad_soc[i]) && refused(&m, "SoC generation"));
  }
  CHECK(!xl_machine_soc_generation_ok(NULL) && xl_machine_soc_generation_ok("H18") && xl_machine_soc_generation_ok("0123456789012345678901234567890"));

  // /product display-corner-radius: 8 bytes, two little-endian u32 (the radius, then the second word); not written unless asked,
  // whatever the two numbers are; one property, and independent of the identity fields; nothing else changes
  static const struct { uint32_t radius, word2; uint8_t bytes[8]; } corners[] = {
    { 30, 1, { 30, 0, 0, 0, 1, 0, 0, 0 } },
    { 62, 1, { 0x3e, 0, 0, 0, 1, 0, 0, 0 } },                // Apple's iPhone18,2: u32 62, u32 1
    { 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { 0, 1, { 0, 0, 0, 0, 1, 0, 0, 0 } },
    { 200, 65535, { 200, 0, 0, 0, 0xff, 0xff, 0, 0 } },
    { 1, 0x0102, { 1, 0, 0, 0, 0x02, 0x01, 0, 0 } },        // the second word is little-endian too
    { 100, 256, { 100, 0, 0, 0, 0, 1, 0, 0 } },
  };
  for (unsigned i = 0; i < sizeof corners / sizeof *corners; i++) {
    machine_display(&m);
    xl_machine plain = m;
    m.has_display_corner_radius = true;
    m.display_corner_radius = corners[i].radius;
    m.display_corner_word2 = corners[i].word2;
    t = xl_machine_tree(&m, &err);
    CHECK(t && product_bytes(t, "display-corner-radius", corners[i].bytes, 8) && product_count(t) == 5);
    CHECK(t && product_int(t, "artwork-scale-factor", 4) == 2 && product_str(t, "model", "oneios,xnuhost"));
    xl_adt_free(t);
    xl_adt_node *p = xl_machine_tree(&plain, &err);
    t = xl_machine_tree(&m, &err);
    CHECK(p && t && xl_adt_remove(p, "product") == 0 && xl_adt_remove(t, "product") == 0 && xl_adt_flat_size(p) == xl_adt_flat_size(t));
    xl_adt_free(p);
    xl_adt_free(t);
    // with every group: the machine's own value, not Apple's (62 and 1 in the tree: 62 is the machine's where it says 62)
    m.identity_product = donor;
    m.identity_groups = XL_IDENTITY_ALL;
    m.vm_ram_mib = 4608;
    t = xl_machine_tree(&m, &err);
    CHECK(t && product_bytes(t, "display-corner-radius", corners[i].bytes, 8));
    xl_adt_free(t);
  }
  // the numbers are read only with has_display_corner_radius, and the groups never copy Apple's
  machine_display(&m);
  m.display_corner_radius = 30; m.display_corner_word2 = 1;
  t = xl_machine_tree(&m, &err);
  CHECK(t && !xl_adt_get(xl_adt_find(t, "product"), "display-corner-radius"));
  xl_adt_free(t);
  m.identity_product = donor; m.identity_groups = XL_IDENTITY_ALL; m.vm_ram_mib = 4608;
  t = xl_machine_tree(&m, &err);
  CHECK(t && !xl_adt_get(xl_adt_find(t, "product"), "display-corner-radius"));
  xl_adt_free(t);
  // out of range: refused, with the numbers it names
  machine_display(&m);
  m.has_display_corner_radius = true;
  static const struct { uint32_t radius, word2; int ok; } ranges[] = {
    { 0, 0, 1 }, { 200, 65535, 1 }, { 201, 1, 0 }, { 1, 65536, 0 }, { 0xffffffffu, 1, 0 }, { 1, 0xffffffffu, 0 }, { 256, 1, 0 }, { 30, 65535, 1 },
  };
  for (unsigned i = 0; i < sizeof ranges / sizeof *ranges; i++) {
    m.display_corner_radius = ranges[i].radius;
    m.display_corner_word2 = ranges[i].word2;
    t = xl_machine_tree(&m, &err);
    CHECK(ranges[i].ok ? t != NULL : (t == NULL && err && strstr(err, "corner radius")));
    xl_adt_free(t);
  }
  xl_adt_free(donor);
}

static void test_sidecars(void)
{
  fx_image tc = fx_trustcache(3);
  CHECK(xl_trustcaches_valid(tc.data, tc.size));
  fx_w32(tc.data + 8, 3);
  CHECK(!xl_trustcaches_valid(tc.data, tc.size));     // version
  fx_w32(tc.data + 8, 2);
  fx_w32(tc.data + 28, 0xffffffff);
  CHECK(!xl_trustcaches_valid(tc.data, tc.size));     // count
  fx_w32(tc.data + 28, 3);
  fx_w32(tc.data + 4, 0xfffffff0);
  CHECK(!xl_trustcaches_valid(tc.data, tc.size));     // offset
  fx_w32(tc.data + 4, 8);
  memset(tc.data + 32 + 24, 0x05, 20);
  CHECK(!xl_trustcaches_valid(tc.data, tc.size));     // order
  CHECK(!xl_trustcaches_valid(tc.data, 4));
  free(tc.data);

  // packed ramdisk: 16 pages logical, two extents
  size_t size = 32 + 2 * 16 + 2 * 4096;
  uint8_t *rd = calloc(1, size);
  memcpy(rd, "DBRDISK1", 8);
  fx_w32(rd + 8, 1); fx_w32(rd + 12, 4096); fx_w64(rd + 16, 16 * 4096); fx_w64(rd + 24, 2);
  fx_w64(rd + 32, 4096); fx_w64(rd + 40, 4096); fx_w64(rd + 48, 8 * 4096); fx_w64(rd + 56, 4096);
  memset(rd + 64, 0xaa, 4096);
  memset(rd + 64 + 4096, 0xbb, 4096);
  uint64_t expanded = 0;
  bool packed = false;
  CHECK(xl_ramdisk_layout(rd, size, &expanded, &packed) && packed && expanded == 16 * 4096);
  uint8_t *out = calloc(1, expanded);
  xl_ramdisk_copy(out, rd, size, packed);
  CHECK(out[0] == 0 && out[4096] == 0xaa && out[8191] == 0xaa && out[8192] == 0 && out[8 * 4096] == 0xbb);
  free(out);
  fx_w64(rd + 48, 4096);                               // overlapping extents
  CHECK(!xl_ramdisk_layout(rd, size, &expanded, &packed));
  fx_w64(rd + 48, 8 * 4096);
  CHECK(!xl_ramdisk_layout(rd, size - 1, &expanded, &packed));
  CHECK(xl_ramdisk_layout((const uint8_t *)"plain disk", 10, &expanded, &packed) && !packed && expanded == 10);
  free(rd);
}

static void test_place(void)
{
  const char *err = NULL;
  static const uint8_t code[] = { 0x1f, 0x20, 0x03, 0xd5, 0xc0, 0x03, 0x5f, 0xd6 };    // nop; ret
  fx_image im = fx_kernel(FX_BASE, 0, code, sizeof code, 0);
  CHECK(xl_macho_parse(im.data, im.size, &K, &err) == 0);
  xl_machine m;
  machine_default(&m);
  m.ramdisk = true;
  const uint64_t window = 64ull << 20;
  uint8_t *ram = calloc(1, window);
  fx_image tc = fx_trustcache(2);
  uint8_t disk[4097];
  memset(disk, 0x5a, sizeof disk);

  for (int hosted = 0; hosted < 2; hosted++) {
    for (int legacy = 0; legacy < 2; legacy++) {
      xl_adt_node *tree = xl_machine_tree(&m, &err);
      xl_boot_request r = { .image = { im.data, im.size }, .kernel = &K, .ram = ram, .window = window,
                            .phys_base = 0x800004000ull,
                            .virt_base = hosted ? 0x2000004000ull : FX_BASE,
                            .tree = tree, .command_line = "debug=0x8 -v", .legacy_boot_args = legacy,
                            .video = { 0x900000000ull, 1, 4 * 1080, 1080, 2340, 32 },
                            .ramdisk = { disk, sizeof disk }, .trustcache = { tc.data, tc.size } };
      xl_boot_layout l;
      CHECK(xl_boot_place(&r, &l, &err) == 0);
      if (err) { printf("  place: %s\n", err); xl_adt_free(tree); continue; }
      CHECK(l.slide == r.virt_base - FX_BASE && l.entry_va == r.virt_base + 0x1000 && l.entry_pa == r.phys_base + 0x1000);
      CHECK(!memcmp(ram + 0x1000, code, sizeof code));                         // __TEXT copied
      CHECK(l.args_pa == r.phys_base + 0x8000 && l.tree_pa == l.args_pa + 0x1000);    // 16 KiB after the kernel's end
      const uint8_t *args = ram + (l.args_pa - r.phys_base);
      uint64_t f[4];
      memcpy(f, args + 8, 32);
      CHECK(args[0] == 2 && args[2] == 2 && f[0] == r.virt_base && f[1] == r.phys_base && f[2] == window &&
            f[3] == r.phys_base + l.used);
      uint64_t dt, video0, actual;
      uint32_t dtlen;
      memcpy(&video0, args + 0x28, 8);
      memcpy(&dt, args + 0x60, 8);
      memcpy(&dtlen, args + 0x68, 4);
      memcpy(&actual, args + (legacy ? 728 : 0x478), 8);
      CHECK(video0 == 0x900000000ull && dt == r.virt_base + (l.tree_pa - r.phys_base) && dtlen == l.tree_size && actual == window);
      CHECK(!strcmp((const char *)args + 0x6c, "debug=0x8 -v"));
      // the tree in the window parses and its memory-map names the copies
      xl_adt_node *placed = xl_adt_parse(ram + (l.tree_pa - r.phys_base), l.tree_size, &err);
      CHECK(placed != NULL);
      if (placed) {
        uint64_t pair[2];
        memcpy(pair, xl_adt_get(xl_adt_find(placed, "chosen/memory-map"), "RAMDisk")->value, 16);
        CHECK(pair[0] == l.ramdisk_pa && pair[1] == 0x2000 && ram[pair[0] - r.phys_base + 4096] == 0x5a &&
              ram[pair[0] - r.phys_base + 4097] == 0);
        memcpy(pair, xl_adt_get(xl_adt_find(placed, "chosen/memory-map"), "TrustCache")->value, 16);
        CHECK(pair[0] == l.trustcache_pa && pair[1] == tc.size && !memcmp(ram + (pair[0] - r.phys_base), tc.data, tc.size));
        xl_adt_free(placed);
      }
      CHECK(l.used % 0x4000 == 0 && l.used <= window - 0x200000);
      xl_adt_free(tree);
    }
  }

  // refusals
  xl_adt_node *tree = xl_machine_tree(&m, &err);
  xl_boot_request r = { .image = { im.data, im.size }, .kernel = &K, .ram = ram, .window = window,
                        .phys_base = 0x800004000ull, .virt_base = FX_BASE, .tree = tree,
                        .ramdisk = { disk, sizeof disk }, .trustcache = { tc.data, tc.size } };
  xl_boot_layout l;
  xl_boot_request b = r;
  b.trustcache = (xl_blob){ 0 };
  CHECK(xl_boot_place(&b, &l, &err) == -1);          // tree names a trust cache that is not supplied
  b = r; b.virt_base = FX_BASE + 0x4000;
  CHECK(xl_boot_place(&b, &l, &err) == -1);          // slide not a multiple of 2 MiB
  b = r; b.phys_base = 0x800000000ull;
  CHECK(xl_boot_place(&b, &l, &err) == -1);          // physical base loses the 2 MiB offset
  b = r; b.window = 16ull << 20;
  CHECK(xl_boot_place(&b, &l, &err) == -1);          // window too small
  b = r; b.command_line = "caf\xc3\xa9";
  CHECK(xl_boot_place(&b, &l, &err) == -1);          // not ASCII
  char longline[1100];
  memset(longline, 'a', sizeof longline - 1);
  longline[sizeof longline - 1] = 0;
  b = r; b.command_line = longline;
  CHECK(xl_boot_place(&b, &l, &err) == -1);
  tc.data[8] = 9;
  CHECK(xl_boot_place(&r, &l, &err) == -1);          // bad trust cache
  tc.data[8] = 2;
  uint8_t stale[16] = { 1 };
  xl_adt_set(xl_adt_find(tree, "chosen/memory-map"), "RAMDisk", stale, 16);
  CHECK(xl_boot_place(&r, &l, &err) == -1);          // stale memory-map entry
  xl_adt_free(tree);
  free(tc.data);
  free(ram);
  free(im.data);
}

int main(void)
{
  test_macho();
  test_fileset();
  test_adt();
  test_machine();
  test_identity_groups();
  test_identity_misc();
  test_sidecars();
  test_place();
  printf("loader tests: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
