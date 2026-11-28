// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// OneiOSHostedDisk: every disk xnuhost serves to the hosted kernel (runtime/xnuhost/src/virtio_blk.c: virtio-blk
// on a virtio-mmio window), as one IOBlockStorageDevice class. The read-only images of the set (os.img,
// systemos.img, app.img), the Data seed (a tar on a raw disk) and the writable Data store.
//
// After UniXNU's NativeVirtioBlock (https://github.com/UniXNU/VirtioMmioBlock 4e1013e, BSD-2-Clause-Patent):
// the register sequence and the polled split queue are its own. What differs:
//   - The registers are reached through xnuhost's mmio_read/mmio_write host calls (oneios_hosted_mmio_*,
//     osfmk/arm64/hosted/hosted_blk.c), not through a mapping: a mapped device page of a hosted kernel is a
//     fault per access. xnuhost serves a queue inside the notify call, so a request is done when it returns.
//   - Writes and flush, for a disk the device does not offer as read-only.
//   - Verified root: xnuhost checks every chunk of a read-only image against the image set's hash lists before
//     the guest sees it (runtime/xnuhost/src/imageset.c) and says so in the disk's device tree node
//     ("oneios,verified" = the SHA-256 of the hash list). That property is this disk's policy: no signed block
//     hashes are checked a second time here. xnuhost holds the kernel's memory, so it is the root of trust.
//   - Which disk is which comes from the node too: "oneios,role" (os, systemos, app, data, seed) and
//     "oneios,volume-uuid" (the APFS volume to mount from an image).
//
// The native kernel (XNU_ONEIOS_NATIVE, kernel/build-native.sh, docs/native.md: XNU alone at EL1, first on QEMU's
// virt board) has the same class and the same nodes, with two differences:
//   - The registers are a device mapping of the window (mapDeviceMemoryWithIndex), and the device answers when it
//     answers: the queue is polled, as before.
//   - Nobody checks the reads before the kernel sees them, so this driver does what xnuhost does: the loader puts the
//     image's chunk hash list (oneios-hash-list-1, the file the manifest lists beside the image) in the node as
//     "oneios,hash-list", and every chunk is read whole and checked against its SHA-256 the first time it is read; a
//     chunk that differs is a read error and stays unchecked. The disk is a verified one ("oneios,verified") only when
//     that list's SHA-256 is the one the node names. The loader is the root of trust here: it writes the kernel's
//     memory, as xnuhost does for a hosted kernel.
// A native disk can also be a virtio-blk PCI function (compatible "oneios,virtio-pci": a VM of Android's crosvm under the
// phone's Gunyah, docs/native.md "A VM under Gunyah"; QEMU's virtio-blk-pci on the bench). Its node's reg is the function's
// configuration space; the virtio capabilities there name the regions of its BAR (VirtIO 1.2 section 4.1). Its BAR is
// the VMM's (crosvm assigns them, linux,pci-probe-only), else placed in the node's "oneios,pci-bar-window" (QEMU without
// a firmware). In a protected VM the host reads and writes no guest memory but a pool it shares (crosvm's restricted DMA
// pool), and a device access anywhere else faults in the host: everything the device touches (the queue, the request
// header, the status byte, the transfer buffer) lies in the node's "oneios,dma-window", the loader's piece of that pool,
// mapped cacheable as the host maps it, and the driver refuses any address outside it. The queue is polled as above.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/crypto/sha2.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <sys/types.h>
#include <sys/param.h>

extern "C" {
#if XNU_HOSTED
uint32_t oneios_hosted_mmio_read32(uint64_t physical);
void oneios_hosted_mmio_write32(uint64_t physical, uint32_t value);
#else
// The native kernel's chunk checks, for a debugger and the boot log: chunks checked, chunks that differed.
volatile uint64_t oneios_native_chunks_checked = 0;
volatile uint32_t oneios_native_chunk_failures = 0;
// A PCI disk's DMA window: a cacheable mapping of memory outside the kernel's window (osfmk/arm64/machine_routines.c).
vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
#endif

int native_verified_block_root_authorized(dev_t device);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);
int oneios_hosted_volume_uuid(const char *role, char uuid[37]);
int oneios_hosted_disk_device(const char *role, dev_t *device, char *name, size_t size, uint64_t *bytes, char sha256[65]);
int oneios_hosted_data_volume(dev_t *device, char *path, size_t size);
int oneios_hosted_disk_zero(const char *role, uint64_t bytes);

volatile uint32_t oneios_hosted_disks = 0;
volatile uint32_t oneios_hosted_disk_failures = 0;
volatile uint64_t oneios_hosted_disk_requests = 0;
void oneios_disk_report(uint32_t every);
}

// VirtIO 1.2 MMIO, modern split queue. All numbers here are protocol offsets.
namespace {
constexpr unsigned queueSize = 8;
constexpr unsigned transferSize = 1U << 20;
constexpr unsigned maxDisks = 16;
enum : uint32_t { opRead = 0, opWrite = 1, opFlush = 4 };
enum : uint32_t { featureReadOnly = 1U << 5, featureFlush = 1U << 9 };
struct Descriptor { uint64_t address; uint32_t length; uint16_t flags; uint16_t next; };
struct Request { uint32_t operation; uint32_t reserved; uint64_t sector; };
struct Used { uint32_t id; uint32_t length; };
static_assert(sizeof(Descriptor) == 16 && sizeof(Request) == 16 && sizeof(Used) == 8, "VirtIO wire layout");
inline void dmaBarrier() { __asm__ volatile("dmb osh" ::: "memory"); }
const char apfsVolumeContent[] = "41504653-0000-11AA-AA11-00306543ECAC";

#if !XNU_HOSTED
// virtio-pci (VirtIO 1.2 section 4.1). The registers are reached with one load or store of one register and an
// immediate-free address: under a hypervisor a device access is emulated from its syndrome, which a load or store with
// writeback or of a pair does not give (Gunyah would end the VM), so the compiler is not left to pick the instruction.
inline uint8_t rd8(volatile const uint8_t *p) { uint32_t v; __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return static_cast<uint8_t>(v); }
inline uint16_t rd16(volatile const uint8_t *p) { uint32_t v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return static_cast<uint16_t>(v); }
inline uint32_t rd32(volatile const uint8_t *p) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
inline void wr8(volatile uint8_t *p, uint8_t v) { __asm__ volatile("strb %w0, [%1]" :: "r"(static_cast<uint32_t>(v)), "r"(p) : "memory"); }
inline void wr16(volatile uint8_t *p, uint16_t v) { __asm__ volatile("strh %w0, [%1]" :: "r"(static_cast<uint32_t>(v)), "r"(p) : "memory"); }
inline void wr32(volatile uint8_t *p, uint32_t v) { __asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(p) : "memory"); }
// The configuration space (its first 256 bytes, as crosvm's CAM has them).
enum : unsigned { pciVendor = 0x00, pciDevice = 0x02, pciCommand = 0x04, pciStatus = 0x06, pciBar0 = 0x10, pciCapabilities = 0x34,
                  pciConfigBytes = 0x100 };
enum : uint16_t { pciVendorVirtio = 0x1af4, pciDeviceVirtioBlock = 0x1042,   // modern virtio-blk: 0x1040 + device 2
                  pciCommandMemory = 1U << 1, pciCommandBusMaster = 1U << 2, pciStatusCapabilities = 1U << 4 };
// The vendor capabilities that name the regions: common configuration, notification, ISR, device configuration.
enum : uint8_t { capVendor = 0x09, capCommon = 1, capNotify = 2, capISR = 3, capDevice = 4 };
// virtio_pci_common_cfg
enum : unsigned { ccDeviceFeatureSelect = 0x00, ccDeviceFeature = 0x04, ccDriverFeatureSelect = 0x08, ccDriverFeature = 0x0c,
                  ccDeviceStatus = 0x14, ccConfigGeneration = 0x15, ccQueueSelect = 0x16, ccQueueSize = 0x18,
                  ccQueueMsixVector = 0x1a, ccQueueEnable = 0x1c, ccQueueNotifyOff = 0x1e, ccQueueDesc = 0x20,
                  ccQueueDriver = 0x28, ccQueueDevice = 0x30, ccBytes = 0x38 };
// The high feature word: VIRTIO_F_VERSION_1 (32) and VIRTIO_F_ACCESS_PLATFORM (33: the device's DMA is the platform's,
// which crosvm offers a protected VM; here its addresses are the pool's physical ones).
enum : uint32_t { featureVersion1 = 1U << 0, featureAccessPlatform = 1U << 1 };
// The DMA window: the queue page at its start, the transfer buffer 64 KiB in.
constexpr uint64_t dmaTransferOffset = 0x10000;
// The kernel's normal memory attributes (VM_WIMG_COPYBACK, osfmk/arm/memory_types.h): write-back, as the host maps the pool.
constexpr unsigned wimgCopyback = 0x2;
#endif
}

class OneiOSHostedDisk : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(OneiOSHostedDisk)
  IOBufferMemoryDescriptor *queueMemory = nullptr;
  IOBufferMemoryDescriptor *transferMemory = nullptr;
  IOLock *gate = nullptr;
  uint64_t registers = 0;            // the window's physical address
  uint64_t queuePhysical = 0;
  uint64_t transferPhysical = 0;
  uint64_t capacity = 0;             // sectors
  uint16_t availableIndex = 0;
  uint16_t usedIndex = 0;
  bool online = false;
  bool writable = false;
  bool flushes = false;
  bool verified = false;
  uint8_t verifiedDigest[32] = {};
  char role[16] = {};
  char volumeUUID[37] = {};
  char seedSHA256[65] = {};
  uint64_t contentBytes = 0;
  // For oneios_disk_report, under the gate (ticks): requests (timed out ones too, and their number) and their bytes, the wait
  // for the device and its longest, the callers' wait for the gate and its longest, the chunks checked and the time hashing them. `told`: what was printed.
  struct Work { uint64_t requests, timeouts, bytes, wait, waitMax, gate, gateMax, chunks, hash; } work = {}, told = {};
  void lockGate();
  void count(uint64_t asked);
#if XNU_HOSTED
  uint32_t read32(unsigned offset) const { return oneios_hosted_mmio_read32(registers + offset); }
  void write32(unsigned offset, uint32_t value) { oneios_hosted_mmio_write32(registers + offset, value); }
#else
  IOMemoryMap *registerMap = nullptr;
  volatile uint32_t *mapped = nullptr;
  uint32_t read32(unsigned offset) const { return mapped[offset / 4]; }
  void write32(unsigned offset, uint32_t value) { mapped[offset / 4] = value; }
  // The chunk checks: the node's hash list (oneios-hash-list-1: a 32-byte header, then a SHA-256 per chunk) and one bit
  // per chunk, set once the chunk was read whole and matched. Under the gate, as every transfer.
  OSData *hashList = nullptr;
  uint8_t *checkedChunks = nullptr;
  uint32_t chunkBytes = 0;
  uint64_t chunkCount = 0;
  uint64_t imageBytes = 0;
  bool loadHashList(IOService *provider);
  IOReturn readChecked(IOMemoryDescriptor *buffer, uint64_t into, uint64_t at, uint32_t *bytes);
  // A virtio-pci disk: the config space (registerMap, the node's reg), its BAR's regions, the DMA window.
  bool pci = false;
  volatile uint8_t *configSpace = nullptr;
  IOMemoryMap *barMap = nullptr;
  volatile uint8_t *common = nullptr, *notifyBase = nullptr, *notifyAt = nullptr, *isr = nullptr, *deviceConfig = nullptr;
  uint32_t notifyLength = 0, notifyMultiplier = 0;
  vm_offset_t dmaVirtual = 0;
  uint64_t dmaPhysical = 0, dmaBytes = 0;
  bool pciStart(IOService *provider);
  bool configurePci();
  bool dmaInside(uint64_t address, uint64_t length) const {
    return address >= dmaPhysical && length <= dmaBytes && address - dmaPhysical <= dmaBytes - length;
  }
#endif
  // The queue page and the transfer buffer: the two buffers' bytes, or a PCI disk's DMA window.
  uint8_t *queueBytes = nullptr;
  uint8_t *transferBytes = nullptr;
  void writeAddress(unsigned offset, uint64_t value) {
    write32(offset, static_cast<uint32_t>(value));
    write32(offset + 4, static_cast<uint32_t>(value >> 32));
  }
  bool reset();
  bool configureQueue();
  void identity(IOService *provider);
  IOReturn transfer(uint32_t operation, uint64_t sector, uint32_t bytes);
public:
  static OneiOSHostedDisk *disks[maxDisks];
  static OneiOSHostedDisk *withRole(const char *wanted);
  static OneiOSHostedDisk *ofMedia(IOService *media, unsigned *depth = nullptr);
  bool isVerified() const { return online && verified && !writable; }
  const uint8_t *digest() const { return verifiedDigest; }
  const char *diskRole() const { return role; }
  const char *diskVolumeUUID() const { return volumeUUID; }
  const char *diskSeedSHA256() const { return seedSHA256; }
  uint64_t diskBytes() const { return contentBytes ? contentBytes : capacity * 512; }
  IOReturn zero(uint64_t bytes);
  void report(uint32_t every);

  IOService *probe(IOService *, SInt32 *) override;
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("OneiOS"); }
  char *getProductString() override { return const_cast<char *>("xnuhost disk"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override { return role; }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = capacity - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = !writable; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override;
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { *value = writable && flushes; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors(OneiOSHostedDisk, IOBlockStorageDevice)
OneiOSHostedDisk *OneiOSHostedDisk::disks[maxDisks];

static bool compatibleWith(IOService *provider, const char *name)
{
  OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
  size_t length = strlen(name) + 1;
  return compatible && compatible->getLength() == length && !memcmp(compatible->getBytesNoCopy(), name, length);
}

IOService *OneiOSHostedDisk::probe(IOService *provider, SInt32 *score)
{
  // The read-only disks and the Data store, as runtime/loader/src/machine.c names them.
  if (!compatibleWith(provider, "virtio,mmio") && !compatibleWith(provider, "droidvm,data-store")
#if !XNU_HOSTED
      && !compatibleWith(provider, "oneios,virtio-pci")
#endif
      ) return nullptr;
  return IOBlockStorageDevice::probe(provider, score);
}

bool OneiOSHostedDisk::reset()
{
  if (!registers) return true;
  online = false;
#if !XNU_HOSTED
  if (pci) {
    if (!common) return true;
    // Writing 0 resets the device; it is done when the status reads 0 again.
    wr8(common + ccDeviceStatus, 0);
    for (unsigned wait = 0; wait < 10000 && rd8(common + ccDeviceStatus); ++wait) IODelay(10);
    return rd8(common + ccDeviceStatus) == 0;
  }
#endif
  write32(0x70, 0);
  return read32(0x70) == 0;
}

bool OneiOSHostedDisk::configureQueue()
{
  if (!reset()) return false;
  write32(0x70, 1);
  write32(0x70, 3);
  write32(0x14, 0);
  uint32_t lowFeatures = read32(0x10);
  write32(0x14, 1);
  uint32_t highFeatures = read32(0x10);
  if (!(highFeatures & 1)) return false;                    // VIRTIO_F_VERSION_1
  writable = !(lowFeatures & featureReadOnly);
  flushes = (lowFeatures & featureFlush) != 0;
  write32(0x24, 0);
  write32(0x20, lowFeatures & (featureReadOnly | featureFlush));
  write32(0x24, 1);
  write32(0x20, 1);
  write32(0x70, 11);
  if (!(read32(0x70) & 8)) return false;
  write32(0x30, 0);
  if (read32(0x44) || read32(0x34) < queueSize) return false;
  queueMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), 4096, 4096);
  transferMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), transferSize, 4096);
  if (!queueMemory || !transferMemory) return false;
  IOByteCount queueLength = 0, transferLength = 0;
  queuePhysical = queueMemory->getPhysicalSegment(0, &queueLength);
  transferPhysical = transferMemory->getPhysicalSegment(0, &transferLength);
  if (!queuePhysical || !transferPhysical || queueLength < 4096 || transferLength < transferSize) return false;
  queueBytes = static_cast<uint8_t *>(queueMemory->getBytesNoCopy());
  transferBytes = static_cast<uint8_t *>(transferMemory->getBytesNoCopy());
  bzero(queueBytes, 4096);
  // No used-buffer interrupts: the host serves the queue inside the notify call.
  *reinterpret_cast<volatile uint16_t *>(queueBytes + 128) = 1;
  availableIndex = usedIndex = 0;
  dmaBarrier();
  write32(0x38, queueSize);
  writeAddress(0x80, queuePhysical);
  writeAddress(0x90, queuePhysical + 128);
  writeAddress(0xa0, queuePhysical + 256);
  write32(0x44, 1);
  bool stable = false;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    uint32_t generation = read32(0xfc);
    uint64_t low = read32(0x100), high = read32(0x104);
    if (generation == read32(0xfc)) {
      capacity = low | (high << 32);
      stable = true;
      break;
    }
  }
  if (!stable || !capacity || capacity > UINT64_MAX / 512) return false;
  write32(0x70, 15);
  online = (read32(0x70) & 15) == 15;
  return online;
}

#if !XNU_HOSTED
// A BAR's address, 0 when the VMM left it unassigned; *wide for a 64-bit one (it takes the next BAR too).
static uint64_t barAddress(volatile uint8_t *config, unsigned index, bool *wide)
{
  uint32_t low = rd32(config + pciBar0 + index * 4);
  *wide = (low & 0x7) == 0x4;
  if (low & 1) return 0;                           // an I/O BAR: not one of virtio's regions
  uint64_t address = low & ~UINT64_C(0xf);
  if (*wide) address |= static_cast<uint64_t>(rd32(config + pciBar0 + index * 4 + 4)) << 32;
  return address;
}

// The function's capabilities, its BAR (placed from the node's window when the VMM did not assign it), the regions
// mapped, the DMA window mapped cacheable: everything configurePci needs. Called with nothing of the device started.
bool OneiOSHostedDisk::pciStart(IOService *provider)
{
  volatile uint8_t *config = configSpace;
  uint16_t vendor = rd16(config + pciVendor), device = rd16(config + pciDevice);
  if (vendor != pciVendorVirtio || device != pciDeviceVirtioBlock) {
    printf("OneiOSHostedDisk: %s: PCI %04x:%04x is not a modern virtio-blk function\n", provider->getName(), vendor, device);
    return false;
  }
  if (!(rd16(config + pciStatus) & pciStatusCapabilities)) return false;
  struct Region { uint8_t bar; uint32_t offset, length; bool found; } regions[capDevice + 1] = {};
  uint32_t multiplier = 0;
  uint8_t at = rd8(config + pciCapabilities) & 0xfc;
  for (unsigned walked = 0; at >= 0x40 && at <= pciConfigBytes - 20 && walked < 48; ++walked) {
    uint8_t id = rd8(config + at), next = rd8(config + at + 1) & 0xfc;
    uint8_t type = rd8(config + at + 3);
    if (id == capVendor && type >= capCommon && type <= capDevice && !regions[type].found) {
      regions[type] = { rd8(config + at + 4), rd32(config + at + 8), rd32(config + at + 12), true };
      if (type == capNotify) multiplier = rd32(config + at + 16);
    }
    at = next;
  }
  // The four regions in one memory BAR (crosvm's BAR 0, QEMU's BAR 4), each large enough for what is read there.
  const unsigned bar = regions[capCommon].bar;
  uint64_t span = 0;
  for (unsigned type = capCommon; type <= capDevice; ++type) {
    const Region &r = regions[type];
    if (!r.found || r.bar != bar || bar > 5 || r.offset > UINT32_MAX - r.length) {
      printf("OneiOSHostedDisk: %s: virtio capability %u missing or not in BAR %u\n", provider->getName(), type, bar);
      return false;
    }
    if (r.offset + static_cast<uint64_t>(r.length) > span) span = r.offset + static_cast<uint64_t>(r.length);
  }
  if (regions[capCommon].length < ccBytes || regions[capISR].length < 1 || regions[capDevice].length < 8 ||
      regions[capNotify].length < 2 || span > (16U << 20)) return false;
  span = (span + 0xfff) & ~UINT64_C(0xfff);
  bool wide = false;
  uint64_t base = barAddress(config, bar, &wide);
  if (!base) {
    // Not assigned (QEMU's virt has no firmware to do it): sized with all ones, placed at the window's start.
    OSData *window = OSDynamicCast(OSData, provider->getProperty("oneios,pci-bar-window"));
    uint64_t place[2] = {};
    if (!window || window->getLength() != sizeof(place)) {
      printf("OneiOSHostedDisk: %s: BAR %u not assigned and no oneios,pci-bar-window\n", provider->getName(), bar);
      return false;
    }
    memcpy(place, window->getBytesNoCopy(), sizeof(place));
    wr16(config + pciCommand, static_cast<uint16_t>(rd16(config + pciCommand) & ~(pciCommandMemory | pciCommandBusMaster)));
    volatile uint8_t *low = config + pciBar0 + bar * 4;
    uint32_t flags = rd32(low) & 0xf;
    wr32(low, 0xffffffff);
    uint64_t mask = rd32(low) & ~UINT64_C(0xf);
    if (wide) {
      wr32(low + 4, 0xffffffff);
      mask |= static_cast<uint64_t>(rd32(low + 4)) << 32;
    } else {
      mask |= UINT64_C(0xffffffff00000000);
    }
    uint64_t size = ~mask + 1;
    if (!mask || (size & (size - 1)) || size < span || (place[0] & (size - 1)) || size > place[1] || (!wide && place[0] + size > (UINT64_C(1) << 32))) {
      printf("OneiOSHostedDisk: %s: BAR %u of 0x%llx bytes does not fit its window 0x%llx+0x%llx\n", provider->getName(), bar, size, place[0], place[1]);
      return false;
    }
    wr32(low, static_cast<uint32_t>(place[0]) | flags);
    if (wide) wr32(low + 4, static_cast<uint32_t>(place[0] >> 32));
    base = barAddress(config, bar, &wide);
    if (base != place[0]) return false;
  }
  wr16(config + pciCommand, static_cast<uint16_t>(rd16(config + pciCommand) | pciCommandMemory | pciCommandBusMaster));
  IODeviceMemory *barMemory = IODeviceMemory::withRange(base, span);
  barMap = barMemory ? barMemory->map() : nullptr;
  OSSafeReleaseNULL(barMemory);
  if (!barMap) return false;
  auto *mappedBar = reinterpret_cast<volatile uint8_t *>(barMap->getVirtualAddress());
  common = mappedBar + regions[capCommon].offset;
  isr = mappedBar + regions[capISR].offset;
  deviceConfig = mappedBar + regions[capDevice].offset;
  notifyBase = mappedBar + regions[capNotify].offset;   // queue 0's notify address: configurePci adds its offset
  notifyLength = regions[capNotify].length;
  notifyMultiplier = multiplier;
  // The DMA window: the queue page, then the transfer buffer.
  OSData *dma = OSDynamicCast(OSData, provider->getProperty("oneios,dma-window"));
  uint64_t window[2] = {};
  if (!dma || dma->getLength() != sizeof(window)) {
    printf("OneiOSHostedDisk: %s: no oneios,dma-window: the device may touch no memory, not started\n", provider->getName());
    return false;
  }
  memcpy(window, dma->getBytesNoCopy(), sizeof(window));
  if (!window[0] || (window[0] & 0xfff) || (window[1] & 0xfff) || window[1] < dmaTransferOffset + transferSize ||
      window[0] > UINT64_MAX - window[1]) return false;
  dmaVirtual = ml_io_map_unmappable(static_cast<vm_offset_t>(window[0]), static_cast<vm_size_t>(window[1]), wimgCopyback);
  if (!dmaVirtual) return false;
  dmaPhysical = window[0];
  dmaBytes = window[1];
  queueBytes = reinterpret_cast<uint8_t *>(dmaVirtual);
  queuePhysical = dmaPhysical;
  transferBytes = queueBytes + dmaTransferOffset;
  transferPhysical = dmaPhysical + dmaTransferOffset;
  if (!dmaInside(queuePhysical, 4096) || !dmaInside(transferPhysical, transferSize)) return false;
  printf("OneiOSHostedDisk: %s: PCI %04x:%04x, BAR %u at 0x%llx (notify x%u), DMA window 0x%llx+0x%llx\n", provider->getName(),
         vendor, device, bar, base, multiplier, dmaPhysical, dmaBytes);
  return true;
}

// The virtio-pci form of configureQueue: one queue of queueSize in the DMA window, polled, no MSI-X vector.
bool OneiOSHostedDisk::configurePci()
{
  if (!reset()) return false;
  wr8(common + ccDeviceStatus, 1);
  wr8(common + ccDeviceStatus, 3);
  wr32(common + ccDeviceFeatureSelect, 0);
  uint32_t lowFeatures = rd32(common + ccDeviceFeature);
  wr32(common + ccDeviceFeatureSelect, 1);
  uint32_t highFeatures = rd32(common + ccDeviceFeature);
  if (!(highFeatures & featureVersion1)) return false;
  writable = !(lowFeatures & featureReadOnly);
  flushes = (lowFeatures & featureFlush) != 0;
  wr32(common + ccDriverFeatureSelect, 0);
  wr32(common + ccDriverFeature, lowFeatures & (featureReadOnly | featureFlush));
  wr32(common + ccDriverFeatureSelect, 1);
  wr32(common + ccDriverFeature, highFeatures & (featureVersion1 | featureAccessPlatform));
  wr8(common + ccDeviceStatus, 11);
  if (!(rd8(common + ccDeviceStatus) & 8)) return false;
  wr16(common + ccQueueSelect, 0);
  if (rd16(common + ccQueueSize) < queueSize || rd16(common + ccQueueEnable)) return false;
  uint64_t notifyOffset = static_cast<uint64_t>(rd16(common + ccQueueNotifyOff)) * notifyMultiplier;
  if (notifyOffset + 2 > notifyLength) return false;
  notifyAt = notifyBase + notifyOffset;
  bzero(queueBytes, 4096);
  *reinterpret_cast<volatile uint16_t *>(queueBytes + 128) = 1;   // no used-buffer interrupts: the queue is polled
  availableIndex = usedIndex = 0;
  dmaBarrier();
  wr16(common + ccQueueSize, queueSize);
  wr16(common + ccQueueMsixVector, 0xffff);                       // VIRTIO_MSI_NO_VECTOR
  const uint64_t rings[3] = { queuePhysical, queuePhysical + 128, queuePhysical + 256 };
  for (unsigned i = 0; i < 3; ++i) {
    wr32(common + ccQueueDesc + i * 8, static_cast<uint32_t>(rings[i]));
    wr32(common + ccQueueDesc + i * 8 + 4, static_cast<uint32_t>(rings[i] >> 32));
  }
  wr16(common + ccQueueEnable, 1);
  bool stable = false;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    uint8_t generation = rd8(common + ccConfigGeneration);
    uint64_t low = rd32(deviceConfig), high = rd32(deviceConfig + 4);
    if (generation == rd8(common + ccConfigGeneration)) {
      capacity = low | (high << 32);
      stable = true;
      break;
    }
  }
  if (!stable || !capacity || capacity > UINT64_MAX / 512) return false;
  wr8(common + ccDeviceStatus, 15);
  uint8_t status = rd8(common + ccDeviceStatus);
  online = (status & 15) == 15 && !(status & 0x40);                // not DEVICE_NEEDS_RESET
  return online;
}
#endif

// The gate, with the time a caller waited for it counted.
void OneiOSHostedDisk::lockGate()
{
  const uint64_t start = mach_absolute_time();
  IOLockLock(gate);
  const uint64_t waited = mach_absolute_time() - start;
  work.gate += waited;
  if (waited > work.gateMax) work.gateMax = waited;
}

// One line a disk that worked since the last one; called every 10 s by the GPU archive's report thread. The wait is the
// device's part of a request; "gate" is all its callers' waits for other requests together.
void OneiOSHostedDisk::report(uint32_t every)
{
  if (!gate) return;
  IOLockLock(gate);
  const Work now = work, was = told;
  told = work;
  work.waitMax = work.gateMax = 0;
  IOLockUnlock(gate);
  if (now.requests == was.requests) return;
  const auto us = [](uint64_t ticks) { uint64_t ns = 0; absolutetime_to_nanoseconds(ticks, &ns); return ns / 1000; };
  IOLog("oneios-disk: %u s: %s +%llu requests (%llu timed out) %llu KiB, device %llu ms (max %llu us), gate %llu ms (max %llu us), chunks checked +%llu (%llu ms)\n",
        every, role[0] ? role : "?", now.requests - was.requests, now.timeouts - was.timeouts, (now.bytes - was.bytes) >> 10, us(now.wait - was.wait) / 1000, us(now.waitMax),
        us(now.gate - was.gate) / 1000, us(now.gateMax), now.chunks - was.chunks, us(now.hash - was.hash) / 1000);
}

extern "C" void oneios_disk_report(uint32_t every)
{
  for (OneiOSHostedDisk *disk : OneiOSHostedDisk::disks) if (disk) disk->report(every);
}

// A request's end, answered or timed out: its wait for the device.
void OneiOSHostedDisk::count(uint64_t asked)
{
  const uint64_t waited = mach_absolute_time() - asked;
  work.requests++;
  work.wait += waited;
  if (waited > work.waitMax) work.waitMax = waited;
}

// One request: header, the transfer buffer (not for a flush), status. Called with the gate held.
IOReturn OneiOSHostedDisk::transfer(uint32_t operation, uint64_t sector, uint32_t bytes)
{
  if (!online) return kIOReturnNotReady;
  if (operation != opFlush && (!bytes || bytes > transferSize || (bytes & 511) || sector >= capacity ||
      bytes / 512 > capacity - sector)) return kIOReturnBadArgument;
#if !XNU_HOSTED
  // A PCI disk's device touches nothing outside the DMA window (a protected VM's host faults on any other address).
  if (pci && (!dmaInside(queuePhysical, 4096) || !dmaInside(transferPhysical, transferSize))) return kIOReturnInternalError;
#endif
  auto *queue = queueBytes;
  auto *descriptors = reinterpret_cast<Descriptor *>(queue);
  auto *request = reinterpret_cast<Request *>(queue + 512);
  volatile uint8_t *status = queue + 528;
  *request = {operation, 0, operation == opFlush ? 0 : sector};
  *status = 0xff;
  if (operation == opFlush) {
    descriptors[0] = {queuePhysical + 512, sizeof(Request), 1, 1};
    descriptors[1] = {queuePhysical + 528, 1, 2, 0};
  } else {
    descriptors[0] = {queuePhysical + 512, sizeof(Request), 1, 1};
    descriptors[1] = {transferPhysical, bytes, static_cast<uint16_t>(operation == opRead ? 3 : 1), 2};
    descriptors[2] = {queuePhysical + 528, 1, 2, 0};
  }
  auto *available = reinterpret_cast<volatile uint16_t *>(queue + 128);
  available[2 + availableIndex % queueSize] = 0;
  dmaBarrier();
  ++availableIndex;
  available[1] = availableIndex;
  dmaBarrier();
  const uint64_t asked = mach_absolute_time();
#if !XNU_HOSTED
  if (pci) wr16(notifyAt, 0);                // queue 0
  else
#endif
  write32(0x50, 0);
  auto *used = reinterpret_cast<volatile uint16_t *>(queue + 256);
  uint64_t deadline;
  clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
  while (used[1] == usedIndex) {             // a device model that answers later than its notify
    if (mach_absolute_time() >= deadline) {
      count(asked);
      work.timeouts++;
      reset();
      __atomic_fetch_add(&oneios_hosted_disk_failures, 1, __ATOMIC_RELAXED);
      return kIOReturnTimeout;
    }
    dmaBarrier();
    IODelay(10);
  }
  count(asked);
  work.bytes += bytes;
  dmaBarrier();
  auto *element = reinterpret_cast<volatile Used *>(queue + 260) + usedIndex % queueSize;
  bool valid = used[1] == static_cast<uint16_t>(usedIndex + 1) && element->id == 0 && *status == 0 &&
               (operation != opRead || element->length >= bytes + 1);
  ++usedIndex;
#if !XNU_HOSTED
  if (pci) {
    (void)rd8(isr);                          // reading the ISR status clears it (and a level INTx it raised)
  } else
#endif
  {
    uint32_t interrupts = read32(0x60) & 3;
    if (interrupts) write32(0x64, interrupts);
  }
  if (!valid) {
    // A read the host refuses is a chunk that failed its hash: the disk stays up, the read fails.
    __atomic_fetch_add(&oneios_hosted_disk_failures, 1, __ATOMIC_RELAXED);
    return kIOReturnIOError;
  }
  __atomic_fetch_add(&oneios_hosted_disk_requests, 1, __ATOMIC_RELAXED);
  return kIOReturnSuccess;
}

static void stringProperty(IOService *provider, const char *key, char *out, size_t size)
{
  out[0] = 0;
  OSData *data = OSDynamicCast(OSData, provider->getProperty(key));
  if (!data || !data->getLength()) return;
  size_t length = data->getLength() < size - 1 ? data->getLength() : size - 1;
  memcpy(out, data->getBytesNoCopy(), length);
  out[length] = 0;
}

void OneiOSHostedDisk::identity(IOService *provider)
{
  stringProperty(provider, "oneios,role", role, sizeof(role));
  stringProperty(provider, "oneios,volume-uuid", volumeUUID, sizeof(volumeUUID));
  stringProperty(provider, "oneios,sha256", seedSHA256, sizeof(seedSHA256));
  for (char *c = volumeUUID; *c; ++c) if (*c >= 'a' && *c <= 'f') *c = static_cast<char>(*c - 32);   // as IOMedia's UUID
  OSData *bytes = OSDynamicCast(OSData, provider->getProperty("oneios,bytes"));
  if (bytes && bytes->getLength() == 8) memcpy(&contentBytes, bytes->getBytesNoCopy(), 8);
  if (contentBytes > capacity * 512) contentBytes = 0;
  OSData *policy = OSDynamicCast(OSData, provider->getProperty("oneios,verified"));
  // Only a disk the device itself refuses to write can be a verified one.
  verified = policy && policy->getLength() == sizeof(verifiedDigest) && !writable;
#if !XNU_HOSTED
  // Native: verified only when this driver checks the reads, against the list the node names by its SHA-256.
  if (verified) {
    uint8_t listDigest[SHA256_DIGEST_LENGTH];
    SHA256_CTX context;
    SHA256_Init(&context);
    if (hashList) SHA256_Update(&context, hashList->getBytesNoCopy(), hashList->getLength());
    SHA256_Final(listDigest, &context);
    verified = hashList && !memcmp(listDigest, policy->getBytesNoCopy(), sizeof(listDigest));
  }
#endif
  if (verified) memcpy(verifiedDigest, policy->getBytesNoCopy(), sizeof(verifiedDigest));
  if (role[0]) setProperty("oneios,role", role);
  setProperty("oneios,verified", verified);
}

#if !XNU_HOSTED
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | static_cast<uint64_t>(le32(p + 4)) << 32; }

// The node's "oneios,hash-list", as xnuhost's xh_verify_open reads the file: false for a list that is there and does
// not describe this disk (the disk is then not started: its reads could not be checked). No list: nothing to check.
bool OneiOSHostedDisk::loadHashList(IOService *provider)
{
  OSData *list = OSDynamicCast(OSData, provider->getProperty("oneios,hash-list"));
  if (!list) return true;
  const auto *p = static_cast<const uint8_t *>(list->getBytesNoCopy());
  uint64_t size = list->getLength();
  if (writable || size < 32 || memcmp(p, "OIHL1\0\0\0", 8) || le32(p + 12) != 1) return false;
  uint32_t chunk = le32(p + 8);
  uint64_t bytes = le64(p + 16), count = le64(p + 24);
  // A chunk is read whole into the transfer buffer, and a piece of a request never crosses one: chunks of 4 KiB to
  // the buffer's 1 MiB that divide it (the image sets' are 1 MiB).
  if (chunk < 4096 || chunk > transferSize || (transferSize % chunk) || !bytes || bytes > capacity * 512 ||
      capacity * 512 - bytes >= 512 || count != (bytes + chunk - 1) / chunk || size != 32 + count * 32) return false;
  checkedChunks = static_cast<uint8_t *>(IOMallocZeroData(static_cast<vm_size_t>((count + 7) / 8)));
  if (!checkedChunks) return false;
  list->retain();
  hashList = list;
  chunkBytes = chunk;
  chunkCount = count;
  imageBytes = bytes;
  return true;
}

// One piece of a read of a checked disk, at byte `at` of the image into `buffer` at `into`: *bytes is cut at the end of
// the piece's chunk. A chunk read for the first time is read whole and compared with the list; a chunk that matched
// before is read as asked (the device is the loader's and nothing else writes it, as xnuhost trusts its files). A chunk
// that differs is a read error and stays unchecked: the next read checks it again.
IOReturn OneiOSHostedDisk::readChecked(IOMemoryDescriptor *buffer, uint64_t into, uint64_t at, uint32_t *bytes)
{
  uint64_t chunk = at / chunkBytes, start = chunk * chunkBytes;
  if (chunk >= chunkCount) return kIOReturnIOError;           // past what the list describes
  if (*bytes > start + chunkBytes - at) *bytes = static_cast<uint32_t>(start + chunkBytes - at);
  auto *data = transferBytes;
  IOReturn result;
  if (checkedChunks[chunk / 8] & (1U << (chunk % 8))) {
    if ((result = transfer(opRead, at / 512, *bytes))) return result;
    return buffer->writeBytes(into, data, *bytes) == *bytes ? kIOReturnSuccess : kIOReturnIOError;
  }
  uint64_t want = imageBytes - start < chunkBytes ? imageBytes - start : chunkBytes;
  uint32_t length = static_cast<uint32_t>((want + 511) & ~UINT64_C(511));
  if ((result = transfer(opRead, start / 512, length))) return result;
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256_CTX context;
  const uint64_t hashing = mach_absolute_time();
  SHA256_Init(&context);
  SHA256_Update(&context, data, static_cast<size_t>(want));
  SHA256_Final(digest, &context);
  work.hash += mach_absolute_time() - hashing;
  work.chunks++;
  if (memcmp(digest, static_cast<const uint8_t *>(hashList->getBytesNoCopy()) + 32 + chunk * 32, sizeof(digest))) {
    uint32_t failures = __atomic_add_fetch(&oneios_native_chunk_failures, 1, __ATOMIC_RELAXED);
    if (failures <= 8) printf("OneiOSHostedDisk: %s: chunk %llu differs from its hash list: a read error\n", role, chunk);
    return kIOReturnIOError;
  }
  // The bytes after the image in its last sector are not in the list: zeros, and that chunk is checked at every read.
  bzero(data + want, length - want);
  if (want == chunkBytes) checkedChunks[chunk / 8] |= static_cast<uint8_t>(1U << (chunk % 8));
  __atomic_fetch_add(&oneios_native_chunks_checked, 1, __ATOMIC_RELAXED);
  uint64_t inner = at - start;
  if (inner + *bytes > length) return kIOReturnIOError;
  return buffer->writeBytes(into, data + inner, *bytes) == *bytes ? kIOReturnSuccess : kIOReturnIOError;
}
#endif

bool OneiOSHostedDisk::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider)) return false;
  IODeviceMemory *window = provider->getDeviceMemoryWithIndex(0);
#if !XNU_HOSTED
  pci = compatibleWith(provider, "oneios,virtio-pci");
  if (!window || window->getLength() < (pci ? pciConfigBytes : 0x108)) return false;
#else
  if (!window || window->getLength() < 0x108) return false;
#endif
  registers = window->getPhysicalAddress();
#if !XNU_HOSTED
  // Native: the window mapped as device memory (uncached, the default for a device's memory).
  registerMap = provider->mapDeviceMemoryWithIndex(0);
  mapped = registerMap ? reinterpret_cast<volatile uint32_t *>(registerMap->getVirtualAddress()) : nullptr;
  if (!mapped) { registers = 0; return false; }
  if (pci) {
    // A virtio-pci function: the window is its configuration space.
    configSpace = reinterpret_cast<volatile uint8_t *>(registerMap->getVirtualAddress());
    gate = IOLockAlloc();
    if (!gate || !pciStart(provider) || !configurePci()) {
      printf("OneiOSHostedDisk: %s: the virtio-pci disk did not start\n", provider->getName());
      reset();
      return false;
    }
  } else
#endif
  {
    if (read32(0x00) != 0x74726976 || read32(0x04) != 2 || read32(0x08) != 2) { registers = 0; return false; }
    gate = IOLockAlloc();
    if (!gate || !configureQueue()) return false;
  }
#if !XNU_HOSTED
  if (!loadHashList(provider)) {
    printf("OneiOSHostedDisk: %s: its hash list does not describe it: not started\n", provider->getName());
    reset();
    return false;
  }
#endif
  identity(provider);
  IOLockLock(gate);
  IOReturn first = transfer(opRead, 0, 512);
  IOLockUnlock(gate);
  if (first) {
    printf("OneiOSHostedDisk: %s (%s): the first sector cannot be read (%x)\n", provider->getName(), role, first);
    return false;
  }
  unsigned slot = __atomic_fetch_add(&oneios_hosted_disks, 1, __ATOMIC_RELAXED);
  if (slot < maxDisks) disks[slot] = this;
#if XNU_HOSTED
  printf("OneiOSHostedDisk: %s: %llu MiB, role %s%s%s\n", provider->getName(), capacity >> 11, role[0] ? role : "none",
         writable ? ", writable" : ", read-only", verified ? ", verified by xnuhost" : "");
#else
  printf("OneiOSHostedDisk: %s: %llu MiB, role %s%s%s\n", provider->getName(), capacity >> 11, role[0] ? role : "none",
         writable ? ", writable" : ", read-only",
         verified ? ", verified: each chunk checked against its hash list when first read" :
         hashList ? ", its chunks checked (the node names no list digest: not a verified disk)" : "");
#endif
  registerService();
  return true;
}

IOReturn OneiOSHostedDisk::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                            IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= capacity ||
      count > capacity - block || count > UINT64_MAX / 512 || buffer->getLength() < count * 512) return kIOReturnBadArgument;
  const bool reading = buffer->getDirection() == kIODirectionIn;
  if (!reading && buffer->getDirection() != kIODirectionOut) return kIOReturnBadArgument;
  if (!reading && !writable) return kIOReturnNotWritable;
  uint64_t actual = 0;
  IOReturn result = kIOReturnNotReady;
  lockGate();
  while (online && actual < count * 512) {
    uint32_t bytes = static_cast<uint32_t>(count * 512 - actual > transferSize ? transferSize : count * 512 - actual);
#if !XNU_HOSTED
    if (reading && hashList) {
      // a checked disk (native): piece by piece, never across a chunk
      if ((result = readChecked(buffer, actual, block * 512 + actual, &bytes))) break;
      actual += bytes;
      continue;
    }
#endif
    if (!reading && buffer->readBytes(actual, transferBytes, bytes) != bytes) {
      result = kIOReturnIOError;
      break;
    }
    result = transfer(reading ? opRead : opWrite, block + actual / 512, bytes);
    if (result) break;
    if (reading && buffer->writeBytes(actual, transferBytes, bytes) != bytes) {
      result = kIOReturnIOError;
      break;
    }
    actual += bytes;
  }
  IOLockUnlock(gate);
  completion->action(completion->target, completion->parameter, result, actual);
  return kIOReturnSuccess;
}

IOReturn OneiOSHostedDisk::doSynchronizeCache()
{
  if (!online) return kIOReturnNotReady;
  if (!writable || !flushes) return kIOReturnSuccess;
  lockGate();
  IOReturn result = transfer(opFlush, 0, 0);
  IOLockUnlock(gate);
  return result;
}

// Zeros at the start of a writable disk, below the storage stack: a Data store that is started over loses
// its container (its superblock and the first checkpoints) and is a blank disk at the next start.
IOReturn OneiOSHostedDisk::zero(uint64_t bytes)
{
  if (!writable) return kIOReturnNotWritable;
  if (bytes > capacity * 512) bytes = capacity * 512;
  bytes &= ~UINT64_C(511);
  IOReturn result = kIOReturnSuccess;
  lockGate();
  bzero(transferBytes, transferSize);
  for (uint64_t at = 0; at < bytes && !result;) {
    uint32_t part = static_cast<uint32_t>(bytes - at > transferSize ? transferSize : bytes - at);
    result = transfer(opWrite, at / 512, part);
    at += part;
  }
  if (!result && flushes) result = transfer(opFlush, 0, 0);
  IOLockUnlock(gate);
  return result;
}

void OneiOSHostedDisk::stop(IOService *provider)
{
  reset();
  IOBlockStorageDevice::stop(provider);
}

void OneiOSHostedDisk::free()
{
  reset();
  for (unsigned i = 0; i < maxDisks; ++i) if (disks[i] == this) disks[i] = nullptr;
  if (queueMemory) { queueMemory->release(); queueMemory = nullptr; }
  if (transferMemory) { transferMemory->release(); transferMemory = nullptr; }
  if (gate) { IOLockFree(gate); gate = nullptr; }
#if !XNU_HOSTED
  if (checkedChunks) { IOFreeData(checkedChunks, static_cast<vm_size_t>((chunkCount + 7) / 8)); checkedChunks = nullptr; }
  if (hashList) { hashList->release(); hashList = nullptr; }
  if (dmaVirtual) { ml_io_unmap(dmaVirtual, static_cast<vm_size_t>(dmaBytes)); dmaVirtual = 0; queueBytes = transferBytes = nullptr; }
  if (barMap) { barMap->release(); barMap = nullptr; common = notifyBase = notifyAt = isr = deviceConfig = nullptr; }
  if (registerMap) { registerMap->release(); registerMap = nullptr; mapped = nullptr; configSpace = nullptr; }
#endif
  IOBlockStorageDevice::free();
}

OneiOSHostedDisk *OneiOSHostedDisk::withRole(const char *wanted)
{
  if (!wanted) return nullptr;
  for (unsigned i = 0; i < maxDisks; ++i) if (disks[i] && !strcmp(disks[i]->role, wanted)) return disks[i];
  return nullptr;
}

// The disk a media object sits on: its container, its partition scheme and the block driver are between them.
OneiOSHostedDisk *OneiOSHostedDisk::ofMedia(IOService *media, unsigned *depth)
{
  unsigned level = 0;
  for (IOService *provider = media; provider && level < 64; provider = provider->getProvider(), ++level) {
    if (OneiOSHostedDisk *disk = OSDynamicCast(OneiOSHostedDisk, provider)) {
      if (depth) *depth = level;
      return disk;
    }
  }
  return nullptr;
}

static OSIterator *allMedia()
{
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  if (!matching) return nullptr;
  OSIterator *iterator = IOService::getMatchingServices(matching);
  matching->release();
  return iterator;
}

static bool mediaDevice(IOService *media, dev_t *device, char *name, size_t size)
{
  OSString *bsd = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
  OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
  OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
  if (!bsd || !mjr || !mnr || !bsd->getLength() || bsd->getLength() >= size ||
      mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) return false;
  *device = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
  strlcpy(name, bsd->getCStringNoCopy(), size);
  return true;
}

// The one media object with this device number, and its disk when xnuhost verifies it.
static OneiOSHostedDisk *verifiedDiskOf(dev_t device, const char *expectedUUID)
{
  OSIterator *iterator = allMedia();
  if (!iterator) return nullptr;
  OneiOSHostedDisk *found = nullptr;
  unsigned matches = 0;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    dev_t candidate = NODEV;
    char name[32];
    if (!media || !mediaDevice(media, &candidate, name, sizeof(name)) || candidate != device) continue;
    if (++matches != 1) { found = nullptr; break; }
    OSString *uuid = OSDynamicCast(OSString, media->getProperty("UUID"));
    if (expectedUUID && (!uuid || strcmp(uuid->getCStringNoCopy(), expectedUUID))) continue;
    OneiOSHostedDisk *disk = OneiOSHostedDisk::ofMedia(media);
    if (disk && disk->isVerified()) found = disk;
  }
  iterator->release();
  return matches == 1 ? found : nullptr;
}

// UniXNU's owner-root questions (bsd_init.c, kern_exec.c), answered from xnuhost's policy.
extern "C" int native_verified_block_root_authorized(dev_t device)
{
  return verifiedDiskOf(device, nullptr) != nullptr;
}

// The identity of a verified volume for the boot manifest: SHA-384 of the hash list's SHA-256 and the volume UUID.
extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
  if (!digest) return 0;
  bzero(digest, SHA384_DIGEST_LENGTH);
  if (!uuid || strlen(uuid) != 36) return 0;
  OneiOSHostedDisk *disk = verifiedDiskOf(device, uuid);
  if (!disk) return 0;
  SHA384_CTX context;
  SHA384_Init(&context);
  SHA384_Update(&context, disk->digest(), 32);
  SHA384_Update(&context, uuid, 36);
  SHA384_Final(digest, &context);
  return 1;
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
  if (!uuid || !device || !path || size < 16 || strlen(uuid) != 36) return 0;
  *device = NODEV;
  path[0] = 0;
  OSIterator *iterator = allMedia();
  if (!iterator) return 0;
  unsigned matches = 0;
  dev_t candidate = NODEV;
  char name[32] = {};
  bool authenticated = false;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    OSString *identity = media ? OSDynamicCast(OSString, media->getProperty("UUID")) : nullptr;
    if (!identity || strcmp(identity->getCStringNoCopy(), uuid)) continue;
    if (++matches != 1) { authenticated = false; break; }
    OneiOSHostedDisk *disk = OneiOSHostedDisk::ofMedia(media);
    authenticated = disk && disk->isVerified() && mediaDevice(media, &candidate, name, sizeof(name));
  }
  iterator->release();
  if (!authenticated || matches != 1) return 0;
  *device = candidate;
  snprintf(path, size, "/dev/%s", name);
  return 1;
}

// The APFS volume to mount from the disk of a role ("os", "systemos", "app"), as the image set's manifest names it.
extern "C" int oneios_hosted_volume_uuid(const char *role, char uuid[37])
{
  OneiOSHostedDisk *disk = OneiOSHostedDisk::withRole(role);
  if (!disk || strlen(disk->diskVolumeUUID()) != 36) return 0;
  strlcpy(uuid, disk->diskVolumeUUID(), 37);
  return 1;
}

// The whole disk of a role: its BSD name ("disk4"), the bytes of what it carries and, for the seed, the SHA-256 the
// manifest lists for it.
extern "C" int oneios_hosted_disk_device(const char *role, dev_t *device, char *name, size_t size, uint64_t *bytes, char sha256[65])
{
  OneiOSHostedDisk *disk = OneiOSHostedDisk::withRole(role);
  if (!disk || !device || !name) return 0;
  OSIterator *iterator = allMedia();
  if (!iterator) return 0;
  int found = 0;
  unsigned best = ~0U;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    unsigned depth = 0;
    dev_t candidate = NODEV;
    char candidateName[32];
    // The media nearest the disk is the whole disk; a container's synthesized disk is further away.
    if (!media || OneiOSHostedDisk::ofMedia(media, &depth) != disk || depth >= best ||
        !mediaDevice(media, &candidate, candidateName, sizeof(candidateName))) continue;
    best = depth;
    *device = candidate;
    strlcpy(name, candidateName, size);
    found = 1;
  }
  iterator->release();
  if (found && bytes) *bytes = disk->diskBytes();
  if (found && sha256) strlcpy(sha256, disk->diskSeedSHA256(), 65);
  return found;
}

// The Data volume: the APFS volume on the disk with the role "data" (the first, if a container has several).
// Not by UUID: the volume is made by the guest's own newfs_apfs on the first start.
extern "C" int oneios_hosted_data_volume(dev_t *device, char *path, size_t size)
{
  OneiOSHostedDisk *disk = OneiOSHostedDisk::withRole("data");
  if (!disk || !device || !path || size < 16) return 0;
  OSIterator *iterator = allMedia();
  if (!iterator) return 0;
  int found = 0;
  char best[32] = {};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    OSString *content = media ? OSDynamicCast(OSString, media->getProperty("Content")) : nullptr;
    dev_t candidate = NODEV;
    char name[32];
    if (!content || strcmp(content->getCStringNoCopy(), apfsVolumeContent) || OneiOSHostedDisk::ofMedia(media) != disk ||
        !mediaDevice(media, &candidate, name, sizeof(name))) continue;
    if (found && (strlen(name) > strlen(best) || (strlen(name) == strlen(best) && strcmp(name, best) >= 0))) continue;
    strlcpy(best, name, sizeof(best));
    *device = candidate;
    found = 1;
  }
  iterator->release();
  if (found) snprintf(path, size, "/dev/%s", best);
  return found;
}

extern "C" int oneios_hosted_disk_zero(const char *role, uint64_t bytes)
{
  OneiOSHostedDisk *disk = OneiOSHostedDisk::withRole(role);
  return disk && disk->zero(bytes) == kIOReturnSuccess;
}
