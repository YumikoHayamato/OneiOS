// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ported from UniXNU-droidvm ipados27/scripts/make-droidvm-adt.py (commit f9121cd). See
// include/xnuloader/machine.h for the list of changes. The comments on single properties are the
// reasons that script records for them.
#include "xnuloader/machine.h"
#include <string.h>

#define FAIL(text) do { *error = (text); xl_adt_free(root); return NULL; } while (0)

static uint32_t adler32(const uint8_t *p, size_t n)
{
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
  return b << 16 | a;
}

static void chrp_header(uint8_t *at, const char *name, unsigned size)
{
  memset(at, 0, 16);
  at[0] = 0x70;
  at[2] = (uint8_t)(size / 16);
  at[3] = (uint8_t)(size / 16 >> 8);
  memcpy(at + 4, name, strlen(name));
  unsigned sum = 0;
  for (int i = 0; i < 16; i++) sum += at[i];
  while (sum > 255) sum = (sum & 255) + (sum >> 8);
  at[1] = (uint8_t)sum;
}

void xl_nvram_proxy(uint8_t image[8192])
{
  memset(image, 0, 8192);
  chrp_header(image, "nvram", 32);
  chrp_header(image + 32, "common", 16 + 2048);
  unsigned free_at = 32 + 16 + 2048;
  chrp_header(image + free_at, "free", 8192 - free_at);
  uint32_t sum = adler32(image + 20, 8192 - 20);
  memcpy(image + 16, &sum, 4);
}

void xl_machine_mac(const xl_machine *m, unsigned which, uint8_t mac[6])
{
  static const uint8_t salt[3] = { 0, 0x5a, 0x69 };
  uint64_t seed = m->have_chip_id && m->unique_chip_id ? m->unique_chip_id : 0x4f6e6569;
  mac[0] = 0x02; mac[1] = 0xd7;                    // locally administered
  mac[2] = (uint8_t)(seed >> 24) ^ salt[which % 3];
  mac[3] = (uint8_t)(seed >> 16); mac[4] = (uint8_t)(seed >> 8); mac[5] = (uint8_t)seed;
}

void xl_machine_board(xl_machine *m, unsigned ncpu, const xl_device_kind *kinds, unsigned ndev)
{
  m->ncpu = ncpu;
  for (unsigned i = 0; i < ncpu && i < XL_MAX_CPUS; i++) {
    m->cpu_reg[i] = i;                    // Affinity0, as crosvm numbers its vCPUs
    if (!m->cpu_cluster[i]) m->cpu_cluster[i] = 'P';
  }
  m->armio_base = XL_BOARD_ARMIO_BASE;
  m->armio_size = XL_BOARD_ARMIO_SIZE;
  m->gic_dist = XL_BOARD_GIC_DIST;
  m->gic_dist_size = XL_BOARD_GIC_DIST_SIZE;
  m->gic_redist = XL_BOARD_GIC_REDIST;
  m->gic_redist_stride = XL_BOARD_GIC_STRIDE;
  m->gic_redist_size = XL_BOARD_GIC_STRIDE * (ncpu ? ncpu : 1);
  m->uart_base = XL_BOARD_UART;
  m->uart_size = XL_BOARD_UART_SIZE;
  m->psci_method = "hvc";
  m->ndev = ndev < XL_MAX_DEVICES ? ndev : XL_MAX_DEVICES;
  for (unsigned i = 0; i < m->ndev; i++) {
    m->dev[i].kind = kinds[i];
    m->dev[i].base = XL_BOARD_VIRTIO + i * XL_BOARD_VIRTIO_SIZE;
    m->dev[i].size = XL_BOARD_VIRTIO_SIZE;
  }
  m->host_io_base = XL_BOARD_IO_BASE;
  m->host_io_size = XL_BOARD_IO_SIZE;
}

// The /product properties of the identity tree a hosted machine does not have: the exclaves of
// Apple's SoCs, and the panel's gamut, which display_gamut sets.
static bool product_dropped(const char *name)
{
  static const char *const dropped[] = { "name", "AAPL,phandle", "exclaves-enabled", "has-exclaves", "artwork-display-gamut" };
  for (unsigned i = 0; i < sizeof dropped / sizeof *dropped; i++)
    if (!strcmp(name, dropped[i])) return true;
  return false;
}

// The identity groups (machine.h identity_groups): their names, by the bit's number, and below the /product properties each takes
// (the artwork group's are every artwork-* but the two the machine sets itself, display_gamut's and display_scale's).
static const char *const group_names[XL_IDENTITY_GROUPS] = { "artwork", "quality", "workgroup", "ids", "memclass", "root" };

unsigned xl_identity_group_of(const char *name)
{
  static const struct { const char *name; unsigned group; } named[] = {
    { "graphics-featureset-class", XL_IDENTITY_ARTWORK }, { "graphics-featureset-fallbacks", XL_IDENTITY_ARTWORK },
    { "ui-background-quality", XL_IDENTITY_QUALITY }, { "ui-weather-quality", XL_IDENTITY_QUALITY },
    { "display-workgroup-interval", XL_IDENTITY_WORKGROUP }, { "hid-workgroup-interval", XL_IDENTITY_WORKGROUP },
    { "udid-version", XL_IDENTITY_IDS }, { "product-description", XL_IDENTITY_IDS }, { "public-key-accelerator", XL_IDENTITY_IDS },
    { "device-perf-memory-class", XL_IDENTITY_MEMCLASS },
  };
  if (!strncmp(name, "artwork-", 8)) return strcmp(name, "artwork-display-gamut") && strcmp(name, "artwork-scale-factor") ? XL_IDENTITY_ARTWORK : 0;
  for (unsigned i = 0; i < sizeof named / sizeof *named; i++)
    if (!strcmp(name, named[i].name)) return named[i].group;
  return 0;
}

const char *xl_identity_group_name(unsigned group)
{
  for (unsigned i = 0; i < XL_IDENTITY_GROUPS; i++)
    if (group == 1u << i) return group_names[i];
  return NULL;
}

bool xl_identity_groups_parse(const char *list, unsigned *groups, char *bad, size_t size)
{
  unsigned mask = 0;
  if (bad && size) bad[0] = 0;
  if (!list) return false;
  for (const char *word = list; ; ) {
    const char *comma = strchr(word, ',');
    const size_t n = comma ? (size_t)(comma - word) : strlen(word);
    unsigned bit = n == 3 && !memcmp(word, "all", 3) ? XL_IDENTITY_ALL : 0;
    for (unsigned i = 0; !bit && i < XL_IDENTITY_GROUPS; i++)
      if ((n == strlen(group_names[i]) && !memcmp(word, group_names[i], n)) || (n == 1 && word[0] == (char)('a' + i))) bit = 1u << i;
    if (!bit) {
      if (bad && size) {
        const size_t k = n < size - 1 ? n : size - 1;
        memcpy(bad, word, k);
        bad[k] = 0;
      }
      return false;
    }
    mask |= bit;
    if (!comma) break;
    word = comma + 1;
  }
  *groups = mask;
  return true;
}

// device-perf-memory-class is an integer of 1, 2, 4 or 8 bytes (little-endian: Apple's tree has 4: 0xc).
static bool integer_width(uint32_t length) { return length == 1 || length == 2 || length == 4 || length == 8; }

static uint64_t integer_value(const xl_adt_prop *p)
{
  uint64_t v = 0;
  for (uint32_t i = p->length; i--; ) v = v << 8 | p->value[i];
  return v;
}

// The most a VM's class can be: Apple's own where it is a usable one (1 to 65535), else 16.
static uint32_t class_max(const xl_adt_prop *apple)
{
  const uint64_t v = apple && integer_width(apple->length) ? integer_value(apple) : 0;
  return v >= 1 && v <= 0xffff ? (uint32_t)v : 16;
}

uint32_t xl_machine_memory_class_max(const xl_adt_node *identity)
{
  const xl_adt_node *product = identity ? xl_adt_child(identity, "product") : NULL;
  return class_max(product ? xl_adt_get(product, "device-perf-memory-class") : NULL);
}

// CoreUI's memory classes (machine.h xl_machine_memory_class_of), the largest first: the class's size in MiB and its value.
static const struct { uint32_t mib, value; } memory_classes[] = {
  { 16384, 16 }, { 12288, 12 }, { 9216, 9 }, { 8192, 8 }, { 6144, 6 }, { 4096, 3 }, { 3072, 4 }, { 2048, 2 }, { 1536, 15 }, { 1024, 1 }, { 0, 0 },
};

uint32_t xl_machine_memory_class_mib(uint32_t value)
{
  for (unsigned i = 0; i < sizeof memory_classes / sizeof memory_classes[0]; i++)
    if (memory_classes[i].value == value) return memory_classes[i].mib ? memory_classes[i].mib : 512;
  return 0;
}

uint32_t xl_machine_memory_class_of(uint32_t ram_mib, uint32_t most)
{
  // `most` (Apple's own class) bounds the size, not the number: a class the table does not hold bounds nothing
  const uint32_t most_mib = xl_machine_memory_class_mib(most);
  for (unsigned i = 0; i < sizeof memory_classes / sizeof memory_classes[0]; i++)
    if (memory_classes[i].mib <= ram_mib && (!most_mib || memory_classes[i].mib <= most_mib)) return memory_classes[i].value;
  return 0;
}

// Is `name` in the comma-separated list `list` (xl_machine.identity_omit)?
static bool omitted(const char *list, const char *name)
{
  const size_t n = strlen(name);
  for (const char *p = list; p && *p; ) {
    const char *end = strchr(p, ',');
    const size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && !memcmp(p, name, n)) return true;
    p = end ? end + 1 : p + len;
  }
  return false;
}

static void set_integer(xl_adt_node *node, const char *name, uint32_t width, uint64_t value)
{
  uint8_t bytes[8];
  for (uint32_t i = 0; i < width; i++) bytes[i] = (uint8_t)(value >> (8 * i));
  xl_adt_set(node, name, bytes, width);
}

// graphics-featureset-fallbacks without APPLE2 and MTL2,2 (a NUL-terminated, colon-separated list):
// with them MobileGestalt answers DeviceSupportsASTC and SplashBoard writes app snapshots in the AGX
// GPU's own ASTC layout, which no other GPU decodes.
static void set_featuresets(xl_adt_node *product, const xl_adt_prop *p)
{
  char out[512];
  size_t used = 0, at = 0, length = p->length;
  while (length && p->value[length - 1] == 0) length--;
  while (at <= length && length) {
    size_t end = at;
    while (end < length && p->value[end] != ':') end++;
    size_t n = end - at;
    bool drop = (n == 6 && !memcmp(p->value + at, "APPLE2", 6)) || (n == 6 && !memcmp(p->value + at, "MTL2,2", 6));
    if (!drop && used + n + 2 < sizeof out) {
      if (used) out[used++] = ':';
      memcpy(out + used, p->value + at, n);
      used += n;
    }
    at = end + 1;
  }
  out[used++] = 0;
  xl_adt_set(product, p->name, out, used);
}

// One of the source's /product properties into `product` as it is (Apple's bytes), but for two: the featureset fallbacks are
// filtered (above) and device-perf-memory-class is the VM's (machine.h vm_ram_mib), of Apple's width. NULL, or why it cannot be.
static const char *copy_product(xl_adt_node *product, const xl_adt_prop *p, const xl_machine *m)
{
  if (!strcmp(p->name, "graphics-featureset-fallbacks")) set_featuresets(product, p);
  else if (strcmp(p->name, "device-perf-memory-class")) xl_adt_set(product, p->name, p->value, p->length);
  else if (!integer_width(p->length)) return "the identity tree's device-perf-memory-class is not an integer of 1, 2, 4 or 8 bytes";
  else if (!m->memory_class && !m->vm_ram_mib) return "the identity tree's device-perf-memory-class needs the VM's RAM size or a memory class";
  else {
    uint64_t value = m->memory_class;
    // take23: the largest of CoreUI's classes that the VM's RAM holds, at most Apple's (take19 wrote the whole GiB: 4 for
    // 4608 MiB, which CoreUI's table reads as its 3 GB class, and 5 or 7, which it does not hold)
    if (!value) value = xl_machine_memory_class_of(m->vm_ram_mib, class_max(p));
    set_integer(product, p->name, p->length, value);
  }
  return NULL;
}

// The board name at the start of a compatible property (its first entry, "V54AP"), with its NUL, in out[size]: its length with
// the NUL, or 0 where there is no entry or it does not fit.
static size_t first_compatible(const xl_adt_prop *p, char *out, size_t size)
{
  if (!p || !p->length) return 0;
  const size_t n = strnlen((const char *)p->value, p->length);
  if (!n || n + 1 >= size) return 0;
  memcpy(out, p->value, n);
  out[n] = 0;
  return n + 1;
}

static void reg(xl_adt_node *n, uint64_t armio_base, uint64_t base, uint64_t size)
{
  uint64_t v[2] = { base - armio_base, size };
  xl_adt_set(n, "reg", v, sizeof v);
}

// A name launchd can put in a file name and the kernel's sysctl in its answer.
bool xl_machine_jetsam_product_ok(const char *name)
{
  size_t n = name ? strlen(name) : 0;
  if (n < 1 || n > 31 || name[0] == '.') return false;
  for (size_t i = 0; i < n; i++) {
    const char c = name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

// A SoC generation as Apple's trees give it ("H18"): the characters of the jetsam name, a dot allowed first.
bool xl_machine_soc_generation_ok(const char *name)
{
  size_t n = name ? strlen(name) : 0;
  if (n < 1 || n > 31) return false;
  for (size_t i = 0; i < n; i++) {
    const char c = name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

xl_adt_node *xl_machine_tree(const xl_machine *m, const char **error)
{
  const char *ignored;
  xl_adt_node *root = NULL;
  if (!error) error = &ignored;
  *error = NULL;
  if (m->ncpu == 0 || m->ncpu > XL_MAX_CPUS) FAIL("1 to 8 CPUs");
  if (m->timebase_hz == 0) FAIL("the timebase frequency is missing");
  if ((m->dram_base | m->dram_size) & 0x3fff) FAIL("the RAM base and size must be 16 KiB aligned");
  if (m->armio_base == 0) FAIL("the arm-io base must not be 0");
  if (m->gpu_intid && (m->gpu_intid < 32 || m->gpu_intid > 1019)) FAIL("the GPU's interrupt must be an SPI: INTID 32 to 1019");
  if (m->audio_intid && (m->audio_intid < 32 || m->audio_intid > 1019 || m->audio_slot == 0 || m->audio_slot > 31))
    FAIL("the sound card's interrupt must be an SPI (INTID 32 to 1019) read for a slot 1 to 31");
  if (m->ncpu > 1 && (!m->psci_method || (strcmp(m->psci_method, "hvc") && strcmp(m->psci_method, "smc"))))
    FAIL("more than one CPU needs a PSCI conduit");
  if (m->gic_redist_size < m->gic_redist_stride * m->ncpu) FAIL("the redistributor window is smaller than one frame per CPU");
  const uint64_t windows[][2] = { { m->gic_dist, m->gic_dist_size }, { m->gic_redist, m->gic_redist_size } };
  for (unsigned i = 0; i < 2; i++)
    if (windows[i][0] < m->armio_base || windows[i][0] + windows[i][1] > m->armio_base + m->armio_size)
      FAIL("a GIC window lies outside the arm-io window");
  for (unsigned i = 0; i < m->ndev; i++)
    if (m->dev[i].base < m->armio_base || m->dev[i].base + m->dev[i].size > m->armio_base + m->armio_size)
      FAIL("a device window lies outside the arm-io window");
  if (m->panic_size && ((m->panic_base | m->panic_size) & 0x3fff)) FAIL("the panic region must be 16 KiB aligned");

  if (m->jetsam_product && !xl_machine_jetsam_product_ok(m->jetsam_product))
    FAIL("the jetsam properties' name: 1 to 31 of A-Z a-z 0-9 . _ -, not starting with a dot");

  const xl_adt_node *id = m->identity, *id_product = id ? xl_adt_child(id, "product") : NULL;
  if (id && (!id_product || !xl_adt_get(id, "model") || !xl_adt_get(id, "target-type")))
    { *error = "the identity tree has no model, target-type or /product"; return NULL; }
  // The identity groups (machine.h identity_groups): their source, and the root and the memory class they ask for
  const xl_adt_node *source = m->identity_product, *from = source ? xl_adt_child(source, "product") : NULL;
  const unsigned groups = m->identity_groups;
  if (id && source) FAIL("an identity tree is the whole identity or the groups' source, not both");
  if (groups & ~XL_IDENTITY_ALL) FAIL("an identity group that does not exist");
  if (groups && !source) FAIL("the identity groups need an identity tree");
  if (source && !from) FAIL("the identity tree has no /product");
  const xl_adt_prop *source_target = source ? xl_adt_get(source, "target-type") : NULL;
  char board[160];
  const size_t board_size = source ? first_compatible(xl_adt_get(source, "compatible"), board, sizeof board - 32) : 0;
  if ((groups & XL_IDENTITY_ROOT) && (!source_target || !source_target->length || !board_size))
    FAIL("the identity tree has no target-type or compatible for the root group");
  if (m->memory_class && !(groups & XL_IDENTITY_MEMCLASS)) FAIL("a memory class needs the identity group memclass");
  for (const char *o = m->identity_omit; o && *o; ) {
    char name[64];
    const char *end = strchr(o, ',');
    const size_t len = end ? (size_t)(end - o) : strlen(o);
    if (!len || len >= sizeof name) FAIL("an --identity-omit name that is empty or too long");
    memcpy(name, o, len); name[len] = 0;
    if (!(groups & xl_identity_group_of(name))) FAIL("an --identity-omit name that no identity group that is on takes");
    o = end ? end + 1 : o + len;
  }
  if (m->memory_class && m->memory_class > xl_machine_memory_class_max(source))
    FAIL("the memory class: 1 to the identity's own (16 where it has none)");
  if (m->soc_generation && !xl_machine_soc_generation_ok(m->soc_generation))
    FAIL("the SoC generation: 1 to 31 of A-Z a-z 0-9 . _ -");
  if (m->has_display_corner_radius && (m->display_corner_radius > XL_CORNER_RADIUS_MAX || m->display_corner_word2 > XL_CORNER_WORD2_MAX))
    FAIL("the display corner radius: 0 to 200 points and a second word of 0 to 65535");

  root = xl_adt_new("device-tree");
  char compatible[160];
  size_t used = 0;
  if (id) {
    const xl_adt_prop *p = xl_adt_get(id, "model");
    xl_adt_set(root, "model", p->value, p->length);
    p = xl_adt_get(id, "target-type");
    xl_adt_set(root, "target-type", p->value, p->length);
    // The device's board name first: libMobileGestalt reads the first entry as the hardware model,
    // and CoreMotion, the audio server and MediaExperience key their per-board tables on it.
    p = xl_adt_get(id, "compatible");
    if (p && p->length) {
      size_t n = strnlen((const char *)p->value, p->length);
      if (n && n + 1 < sizeof compatible - 32) { memcpy(compatible, p->value, n); compatible[n] = 0; used = n + 1; }
    }
  } else {
    xl_adt_set_str(root, "model", m->model ? m->model : "oneios-xnuhost");
    // the root group: the identity's target-type, and its board name first in compatible, as with `identity` (the model stays)
    if (groups & XL_IDENTITY_ROOT) {
      xl_adt_set(root, "target-type", source_target->value, source_target->length);
      memcpy(compatible, board, board_size);
      used = board_size;
    }
  }
  memcpy(compatible + used, "oneios,xnuhost", sizeof "oneios,xnuhost");
  used += sizeof "oneios,xnuhost";
  xl_adt_set(root, "compatible", compatible, used);
  xl_adt_set_u32(root, "#address-cells", 2);
  xl_adt_set_u32(root, "#size-cells", 2);

  xl_adt_node *chosen = xl_adt_add(root, "chosen");
  xl_adt_set(chosen, "random-seed", m->random_seed, sizeof m->random_seed);
  if (m->have_chip_id) {
    xl_adt_set_u64(chosen, "unique-chip-id", m->unique_chip_id);
    xl_adt_set_u32(chosen, "chip-id", m->chip_id);
  }
  xl_adt_set_str(chosen, "osenvironment", "main");
  xl_adt_set_u32(chosen, "certificate-production-status", 0);
  xl_adt_set_u32(chosen, "effective-production-status-ap", 0);
  xl_adt_set_u32(chosen, "certificate-security-mode", 1);
  xl_adt_set_u32(chosen, "effective-security-mode-sep", 1);
  if (m->ramdisk || m->packed_ramdisk) xl_adt_set_u32(chosen, "ephemeral-storage", 1);
  xl_adt_set_u64(chosen, "dram-base", m->dram_base);
  xl_adt_set_u64(chosen, "dram-size", m->dram_size);
  xl_adt_set_str(chosen, "crypto-hash-method", "sha2-384");
  if (m->root_volume_uuid) {
    char match[256], uuid[40];
    size_t u = 0;
    for (; m->root_volume_uuid[u] && u < sizeof uuid - 1; u++) {
      char c = m->root_volume_uuid[u];
      uuid[u] = c >= 'a' && c <= 'f' ? (char)(c - 32) : c;
    }
    uuid[u] = 0;
    if (m->root_volume_uuid[u]) FAIL("root volume UUID too long");
    int n = snprintf(match, sizeof match,
                     "<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key>"
                     "<dict><key>UUID</key><string>%s</string></dict></dict>", uuid);
    xl_adt_set(chosen, "root-matching", match, (size_t)n + 1);
  }
  xl_adt_set_u32(chosen, "amfi-only-platform-code", 1);
  // What iBoot writes: 0 = nothing locks the kernel's text (no CTRR under a host or a hypervisor). xnu-12377 asserts
  // that it was given before DTrace's SDT probes ask ml_unsafe_kernel_text (bsd_init -> sdt_init).
  xl_adt_set_u32(chosen, "kernel-ctrr-to-be-enabled", 0);
  if (m->panic_size) xl_adt_set_u32(chosen, "embedded-panic-log-size", (uint32_t)m->panic_size);
  if (m->text_fixups && m->text_fixups_length) xl_adt_set(chosen, "text-fixups", m->text_fixups, m->text_fixups_length);
  if (m->prime_list && m->prime_list_length) xl_adt_set(chosen, "oneios-prime-list", m->prime_list, m->prime_list_length);
  {
    uint8_t image[8192];
    xl_nvram_proxy(image);
    xl_adt_set_u32(chosen, "nvram-bank-size", sizeof image);
    xl_adt_set_u32(chosen, "nvram-bank-count", 1);
    xl_adt_set_u32(chosen, "nvram-current-bank", 0);
    xl_adt_set(chosen, "nvram-proxy-data", image, sizeof image);
  }
  static const uint8_t empty[16];
  xl_adt_node *map = xl_adt_add(chosen, "memory-map");
  if (m->ramdisk || m->packed_ramdisk) xl_adt_set(map, m->packed_ramdisk ? "PackedRAMDisk" : "RAMDisk", empty, 16);
  if (m->trust_cache) xl_adt_set(map, "TrustCache", empty, 16);
  if (m->gpu_addon) xl_adt_set(map, "GpuAddon", empty, 16);
  if (m->root_policies) xl_adt_set(map, "RootPolicies", empty, 16);
  xl_adt_add(chosen, "manifest-properties");
  if (m->display_scale) {
    // The UI scale: MobileGestalt's main-screen-scale. The display driver publishes the same two words and
    // refuses to start its scale policy on a value that differs (LegacyDisplayProbe.cpp publishOrMatchWord).
    xl_adt_set_u32(chosen, "display-scale", m->display_scale);
    xl_adt_set_u32(chosen, "display-rotation", 0);
  }
  if (m->host_vector) {
    xl_adt_node *host = xl_adt_add(chosen, "xnuhost");
    xl_adt_set_u32(host, "abi", 1);
    xl_adt_set_u64(host, "vector", m->host_vector);
    uint64_t io[2] = { m->host_io_base, m->host_io_size };
    xl_adt_set(host, "io-aperture", io, sizeof io);
    xl_adt_set_u64(host, "user-va-limit", m->user_va_limit);
    xl_adt_set_u32(host, "host-page-size", m->host_page_size);
    if (m->display_width && m->display_height) {
      xl_adt_set_u32(host, "display-width", m->display_width);
      xl_adt_set_u32(host, "display-height", m->display_height);
      xl_adt_set_u32(host, "display-scale", m->display_scale);
      xl_adt_set_u32(host, "display-hz", m->display_hz);
    }
  }

  xl_adt_node *cpus = xl_adt_add(root, "cpus");
  for (unsigned i = 0; i < m->ncpu; i++) {
    char name[16];
    snprintf(name, sizeof name, "cpu%u", i);
    xl_adt_node *cpu = xl_adt_add(cpus, name);
    xl_adt_set_str(cpu, "state", i == 0 ? "running" : "waiting");
    xl_adt_set_u32(cpu, "reg", m->cpu_reg[i]);
    xl_adt_set_str(cpu, "device_type", "cpu");
    xl_adt_set_str(cpu, "compatible", "arm,armv8");
    xl_adt_set_u32(cpu, "timebase-frequency", m->timebase_hz);
    // An AMP kernel panics on a CPU of no cluster type, so every cpu node carries one.
    char cluster[2] = { m->cpu_cluster[i] == 'E' ? 'E' : 'P', 0 };
    xl_adt_set_str(cpu, "cluster-type", cluster);
  }
  if (m->ncpu > 1) {
    xl_adt_node *psci = xl_adt_add(root, "psci");
    xl_adt_set_str(psci, "compatible", "arm,psci-1.0");
    xl_adt_set_str(psci, "method", m->psci_method);
  }
  if (m->panic_size) {
    uint64_t v[2] = { m->panic_base, m->panic_size };
    xl_adt_set(xl_adt_add(root, "pram"), "reg", v, sizeof v);
  }
  xl_adt_node *defaults = xl_adt_add(root, "defaults");
  xl_adt_set_u32(defaults, "sleep-disabled", 1);
  // XNU publishes this as kern.hv_vmm_present; 24A446's bluetoothd then skips chip initialisation, and its power init
  // succeeds only with transport 10, its own simulated controller (docs/hardware.md 4, "The virtual transport").
  xl_adt_set_u32(defaults, "vmm-present", 1);
  // The vnode limit XNU sizes from memory, (bytes / 64 KiB) + 1024: without it the BCM2837
  // configuration clips it to 1024 and the vnode jetsam kills every idle process.
  xl_adt_set_u32(defaults, "kern.maxvnodes", (uint32_t)(m->dram_size / 0x10000 + 1024));
  xl_adt_add(root, "options");

  // bluetoothd finds its controller by name; transport-encoding 10 is the virtual transport.
  xl_adt_node *bt = xl_adt_add(root, "bluetooth");
  xl_adt_set_str(bt, "device_type", "bluetooth");
  xl_adt_set_u32(bt, "transport-encoding", 10);
  uint8_t mac[6];
  if (m->bt_transport) {
    xl_adt_set_u32(bt, "transport-encoding", m->bt_transport);
    if (m->bt_speed) xl_adt_set_u32(bt, "transport-speed", m->bt_speed);
    memcpy(mac, m->bt_mac, 6);
  } else xl_machine_mac(m, 0, mac);
  xl_adt_set(bt, "local-mac-address", mac, 6);
  // The three addresses the userland's boot contract requires, each stable for one unique chip ID.
  xl_adt_set(chosen, "mac-address-bluetooth0", mac, 6);
  xl_machine_mac(m, 1, mac);
  xl_adt_set(chosen, "mac-address-wifi0", mac, 6);
  xl_machine_mac(m, 2, mac);
  xl_adt_set(chosen, "mac-address-ethernet0", mac, 6);

  xl_adt_node *buttons = xl_adt_add(root, "buttons");
  xl_adt_set_str(buttons, "compatible", "buttons");
  xl_adt_set_str(buttons, "device_type", "buttons");
  xl_adt_set(buttons, "button-names", "hold\0voldown\0volup", sizeof "hold\0voldown\0volup");
  xl_adt_set_u32(buttons, "home-button-type", 2);

  xl_adt_node *product = xl_adt_add(root, "product");
  if (id_product) {
    for (unsigned i = 0; i < id_product->nprops; i++) {
      const xl_adt_prop *p = &id_product->props[i];
      if (product_dropped(p->name)) continue;
      if (!strcmp(p->name, "graphics-featureset-fallbacks")) set_featuresets(product, p);
      else xl_adt_set(product, p->name, p->value, p->length);
    }
    xl_adt_set_str(product, "artwork-display-gamut", m->display_gamut ? m->display_gamut : "P3");
    xl_adt_node *audio = xl_adt_add(product, "audio");
    xl_adt_set_u32(audio, "wireless-splitter", 1);
    const xl_adt_node *id_audio = xl_adt_child(id_product, "audio");
    const xl_adt_prop *acoustic = id_audio ? xl_adt_get(id_audio, "acoustic-id") : NULL;
    if (acoustic) xl_adt_set(audio, "acoustic-id", acoustic->value, acoustic->length);
  } else {
    xl_adt_set_str(product, "product-name", "OneiOS guest");
    // the groups that are /product's: the source's own values of some properties, and no child node. A placeholder is iBoot's
    // recipe for a value ("syscfg/DClr"), not the value: it is left out.
    for (unsigned i = 0; from && i < from->nprops; i++) {
      const xl_adt_prop *p = &from->props[i];
      if (!(groups & xl_identity_group_of(p->name)) || (p->flags & 1)) continue;
      if (omitted(m->identity_omit, p->name)) continue;   // take23: --identity-omit
      const char *why = copy_product(product, p, m);
      if (why) FAIL(why);
    }
    // the artwork group's gamut is the machine's (the source's is not copied), written with the group and not without it
    if (groups & XL_IDENTITY_ARTWORK) xl_adt_set_str(product, "artwork-display-gamut", m->display_gamut ? m->display_gamut : "P3");
    // a memory class given for a source that has no such property
    if (m->memory_class && !xl_adt_get(product, "device-perf-memory-class")) set_integer(product, "device-perf-memory-class", 4, m->memory_class);
  }
  // The artwork scale follows the UI scale (the identity tree's is the iPhone panel's 3). The artwork subtype
  // stays the identity's (2868 for iPhone18,2): it names a set of assets, and no set exists for another height.
  if (m->display_scale) xl_adt_set_u32(product, "artwork-scale-factor", m->display_scale);
  // The corner radius (machine.h display_corner_radius): two little-endian words, whatever the identity fields say
  if (m->has_display_corner_radius) {
    uint8_t radius[8];
    for (unsigned i = 0; i < 4; i++) {
      radius[i] = (uint8_t)(m->display_corner_radius >> (8 * i));
      radius[4 + i] = (uint8_t)(m->display_corner_word2 >> (8 * i));
    }
    xl_adt_set(product, "display-corner-radius", radius, sizeof radius);
  }
  xl_adt_set_str(product, "model", "oneios,xnuhost");
  // The name of the jetsam properties (machine.h): the kernel reads this property of /product at boot (xnu-12377's
  // sysctl_load_jetsam_properties_product_type, read in the native kernel's code) and prefers it to the target-type.
  if (m->jetsam_product) xl_adt_set_str(product, "jetsam-properties-product-type", m->jetsam_product);

  xl_adt_node *io = xl_adt_add(root, "arm-io");
  xl_adt_set_str(io, "device_type", "arm-io");
  xl_adt_set_u32(io, "#address-cells", 2);
  xl_adt_set_u32(io, "#size-cells", 2);
  uint64_t ranges[3] = { 0, m->armio_base, m->armio_size };
  xl_adt_set(io, "ranges", ranges, sizeof ranges);
  if (m->soc_generation) xl_adt_set_str(io, "soc-generation", m->soc_generation);
  xl_adt_node *intc = xl_adt_add(io, "intc");
  xl_adt_set_str(intc, "compatible", "arm,gic-v3");
  uint64_t gic[4] = { m->gic_dist - m->armio_base, m->gic_dist_size, m->gic_redist - m->armio_base, m->gic_redist_size };
  xl_adt_set(intc, "reg", gic, sizeof gic);
  xl_adt_set_u64(intc, "redistributor-stride", m->gic_redist_stride);
  // launchd loads wifid only where the tree has a node named wlan; no driver matches it.
  xl_adt_set_str(xl_adt_add(io, "wlan"), "device_type", "wlan");

  unsigned blocks = 0, inputs = 0;
  bool gpu_line = false; bool snd_line = false;
  for (unsigned i = 0; i < m->ndev; i++) {
    const xl_device *d = &m->dev[i];
    char name[32];
    const char *compat;
    if (d->pci && d->kind != XL_DEV_BLOCK && d->kind != XL_DEV_DATA_STORE && d->kind != XL_DEV_INPUT && d->kind != XL_DEV_GPU &&
        d->kind != XL_DEV_SND && d->kind != XL_DEV_NET && d->kind != XL_DEV_MEDIA)
      FAIL("only a disk, the input bus, the GPU, the sound card, the network card or the video decoder can be on PCI");
    switch (d->kind) {
    case XL_DEV_BLOCK:
      snprintf(name, sizeof name, d->pci ? "virtio-pci-%u" : "virtio-mmio-%u", blocks++);
      compat = d->pci ? "oneios,virtio-pci" : "virtio,mmio";
      break;
    case XL_DEV_DATA_STORE: snprintf(name, sizeof name, "data-store"); compat = d->pci ? "oneios,virtio-pci" : "droidvm,data-store"; break;
    case XL_DEV_INPUT:
      // on PCI: the bus the kernel looks for virtio-input functions on (crosvm places them; DroidVMVirtioInput.c, staged)
      if (d->pci) { snprintf(name, sizeof name, "input-pci"); compat = "oneios,virtio-input-pci"; }
      else { snprintf(name, sizeof name, "input-%u", inputs++); compat = "droidvm,virtio-input"; }
      break;
    case XL_DEV_NET:
      // on PCI: bus 0 of crosvm's ECAM, where the kernel takes the first virtio-pci network card (DroidVMVirtioNet.c, staged)
      snprintf(name, sizeof name, "net");
      compat = d->pci ? "oneios,virtio-net-pci" : "droidvm,net";
      break;
    case XL_DEV_GPU:
      // on PCI: a virtio-pci function, its configuration space as reg (DroidVMVirtioGpu.cpp, staged/gpu.rs TRANSPORT_PCI)
      snprintf(name, sizeof name, "virtio-gpu");
      compat = d->pci ? "oneios,virtio-gpu-pci" : "droidvm,virtio-gpu";
      break;
    case XL_DEV_SND:
      // on PCI: bus 0 of crosvm's ECAM, where the kernel takes the first virtio-pci sound card (DroidVMVirtioSnd.c, staged)
      snprintf(name, sizeof name, "virtio-snd");
      compat = d->pci ? "oneios,virtio-snd-pci" : "droidvm,virtio-snd";
      break;
    case XL_DEV_BRIDGE:     snprintf(name, sizeof name, "bridge"); compat = "oneios,bridge"; break;
    case XL_DEV_CAMERA:     snprintf(name, sizeof name, "camera"); compat = "oneios,camera"; break;
    case XL_DEV_MEDIA:
      // on PCI: bus 0 of crosvm's ECAM, where the kernel takes the first virtio-pci media function (DroidVMVirtioMedia.c,
      // staged/codec_transport.rs MEDIA_TRANSPORT_PCI): crosvm's --virtio-media decoder, which crosvm places itself
      snprintf(name, sizeof name, "virtio-media-0");
      compat = d->pci ? "oneios,virtio-media-pci" : "droidvm,virtio-media";
      break;
    default: FAIL("unknown device kind");
    }
    if (xl_adt_child(io, name)) FAIL("two devices of a kind that has one node");
    xl_adt_node *n = xl_adt_add(io, name);
    xl_adt_set_str(n, "compatible", compat);
    reg(n, m->armio_base, d->base, d->size);
    xl_adt_set(n, "dma-coherent", "", 0);
    // The line of the window on the board's GIC: SPI INTID 32 + its index, under the name UniXNU-droidvm's
    // transports read. Not "interrupts": the tree has no interrupt-controller node (ArmCpu owns the GIC), and
    // IOKit's IODTMapOneInterrupt walks the parents of a node with "interrupts" forever looking for one.
    // On PCI the line is the PCI host's (crosvm's interrupt-map), not the board's: only the GPU is given its own
    // (gpu_intid, which vm-boot checks against crosvm's tree); the other functions poll.
    if (!d->pci) xl_adt_set_u32(n, "droidvm,intid", XL_BOARD_VIRTIO_SPI + i);
    else if (d->kind == XL_DEV_GPU && m->gpu_intid) { xl_adt_set_u32(n, "droidvm,intid", m->gpu_intid); gpu_line = true; }
    else if (d->kind == XL_DEV_SND && m->audio_intid) {   // take18: the sound card's line and the slot it is for
      xl_adt_set_u32(n, "droidvm,intid", m->audio_intid);
      xl_adt_set_u32(n, "oneios,intid-slot", m->audio_slot);
      snd_line = true;
    }
    if (d->kind == XL_DEV_GPU && m->gpu_pool_size) {
      uint64_t pool[2] = { m->gpu_pool_base, m->gpu_pool_size };
      xl_adt_set(n, "drm-host-pool", pool, sizeof pool);
    }
    if (d->kind == XL_DEV_MEDIA && m->media_pool_size) {
      uint64_t pool[2] = { m->media_pool_base, m->media_pool_size };
      xl_adt_set(n, "media-host-pool", pool, sizeof pool);
    }
  }
  if (m->gpu_intid && !gpu_line) FAIL("an interrupt for the GPU without the GPU on PCI");
  if (m->audio_intid && !snd_line) FAIL("an interrupt for the sound card without the sound card on PCI");
  if (m->cpufreq_size) {
    xl_adt_node *freq = xl_adt_add(io, "cpufreq");
    xl_adt_set_str(freq, "compatible", "oneios,cpufreq");
    reg(freq, m->armio_base, m->cpufreq_base, m->cpufreq_size);
  }
  // XNU's PL011 console finds the node named "uart0" at soc_base + reg; the native kernel's ns16550 driver (patch
  // 0077) takes its reg as the physical address itself.
  if (m->uart_size && m->uart_ns16550) {
    xl_adt_node *uart = xl_adt_add(io, "uart0");
    xl_adt_set_str(uart, "compatible", "ns16550a");
    const uint64_t absolute[2] = { m->uart_base, m->uart_size };
    xl_adt_set(uart, "reg", absolute, sizeof absolute);
  } else if (m->uart_size) {
    xl_adt_node *uart = xl_adt_add(io, "uart0");
    xl_adt_set_str(uart, "compatible", "arm,pl011");
    reg(uart, m->armio_base, m->uart_base, m->uart_size);
  }
  // The bridge channel on a serial port (drivers/bridge/OneiOSBridge.c, its second transport): absolute, as the console's.
  if (m->bridge_uart_size) {
    if (xl_adt_child(io, "bridge")) FAIL("the bridge is a virtio device or a serial port, not both");
    if (m->uart_size && m->bridge_uart_base < m->uart_base + m->uart_size && m->uart_base < m->bridge_uart_base + m->bridge_uart_size)
      FAIL("the bridge's serial port is the console's");
    xl_adt_node *bridge = xl_adt_add(io, "bridge");
    xl_adt_set_str(bridge, "compatible", "oneios,bridge-uart");
    const uint64_t absolute[2] = { m->bridge_uart_base, m->bridge_uart_size };
    xl_adt_set(bridge, "reg", absolute, sizeof absolute);
  }
  return root;
}
