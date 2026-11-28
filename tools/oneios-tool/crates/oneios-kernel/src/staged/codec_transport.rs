// Written for `oneios-tool kernel gen codec` (take18b, 2026-11-26): no Python original.
/// The native kernel's virtio-media on crosvm's PCI (xnuloader-crosvm --media). Droid-VM's crosvm f7c6ce8 has no
/// `--virtio-mmio-media`: its `--virtio-media kind=decoder` card is a virtio-pci function crosvm places itself, so
/// /arm-io/virtio-media-0 ("oneios,virtio-media-pci") is all of bus 0 of its ECAM, as the sound card's and the network
/// card's nodes are, with media-host-pool (crosvm's `--pre-alloc media-host-mb` pool, the FDT node media_host@PA that
/// vm-boot checks). DroidVMVirtioMedia.c's lines that change for it, compiled only with XNU_ONEIOS_NATIVE: the MMIO
/// transport's register offsets go to the function's regions (VirtIO 1.2 4.1.4), as the sound card's do (staged/audio.rs
/// SND_TRANSPORT_PCI), each device with its own (MEDIA_MAX of them: the K-th "oneios,virtio-media-pci" node takes the bus's
/// K-th 1af4:1070 function), the card's configuration (device_caps, device_type, card[32]) at 0x100. Rings, events and the
/// command buffers stay in the transport's DMA region of the guest's RAM, which the shared-RAM VM's host reads; the queues
/// keep VIRTQ_AVAIL_F_NO_INTERRUPT and are polled as at the pin (a command 1 ms at a time, the event queue every millisecond
/// while a session streams and every 50 otherwise): the ISR is not read and MSI-X is not enabled. The hosted kernel
/// compiles the file as it is.
pub const MEDIA_TRANSPORT_PCI: &[(&str, &str)] = &[
    (r#"// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t media_read32(const struct media_device *d, unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(d->registers + offset) : "memory");
    return value;
}
static void media_write32(const struct media_device *d, unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(d->registers + offset) : "memory");
}
"#,
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/codec_transport.rs MEDIA_TRANSPORT_PCI): a device as a virtio-pci function of crosvm's ECAM host
// (/arm-io/virtio-media-N "oneios,virtio-media-pci" is bus 0, xnuloader-crosvm --media), the MMIO transport's offsets
// (VirtIO 1.2 4.2.2) on its regions (4.1.4), as the sound card's (staged/audio.rs SND_TRANSPORT_PCI); each device its own.
// Single loads and stores with a plain base register, as the MMIO accessors.
extern int printf(const char *, ...);
struct media_pci {
    bool on;
    uint8_t slot;
    uintptr_t common, notify, device;
    uint32_t multiplier;
    uint16_t notify_off[2];
};
static struct media_pci media_pci[MEDIA_MAX];
static unsigned media_pci_taken;                   // 1af4:1070 functions given to a device so far (the next node takes the next)
static uint32_t media_pci_rd32(uintptr_t p) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static uint16_t media_pci_rd16(uintptr_t p) { uint32_t v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint16_t)v; }
static uint8_t media_pci_rd8(uintptr_t p) { uint32_t v; __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint8_t)v; }
static void media_pci_wr32(uintptr_t p, uint32_t v) { __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(p) : "memory"); }
static void media_pci_wr16(uintptr_t p, uint16_t v) { __asm__ volatile("strh %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static void media_pci_wr8(uintptr_t p, uint8_t v) { __asm__ volatile("strb %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static uint32_t media_pci_read(const struct media_pci *f, unsigned offset) {
    if (offset >= 0x100) return media_pci_rd32(f->device + (offset - 0x100));   // device_caps, device_type, card[32]
    switch (offset) {
    case 0x00: return 0x74726976;                                // MagicValue, Version, DeviceID: the function is 1af4:1070
    case 0x04: return 2;
    case 0x08: return VIRTIO_ID_MEDIA;
    case 0x10: return media_pci_rd32(f->common + 0x04);          // DeviceFeatures, of the word DeviceFeaturesSel chose
    case 0x34: return media_pci_rd16(f->common + 0x18);          // QueueNumMax: the selected queue's size until it is set
    case 0x44: return media_pci_rd16(f->common + 0x1c);          // QueueReady: queue_enable
    case 0x70: return media_pci_rd8(f->common + 0x14);           // Status
    default: return 0;
    }
}
static void media_pci_write(struct media_pci *f, unsigned offset, uint32_t value) {
    switch (offset) {
    case 0x14: media_pci_wr32(f->common + 0x00, value); break;   // DeviceFeaturesSel
    case 0x20: media_pci_wr32(f->common + 0x0c, value); break;   // DriverFeatures (the driver keeps ACCESS_PLATFORM)
    case 0x24: media_pci_wr32(f->common + 0x08, value); break;   // DriverFeaturesSel
    case 0x30:                                                   // QueueSel, and that queue's notification offset
        media_pci_wr16(f->common + 0x16, (uint16_t)value);
        if (value < 2) f->notify_off[value] = media_pci_rd16(f->common + 0x1e);
        break;
    case 0x38: media_pci_wr16(f->common + 0x18, (uint16_t)value); break;
    case 0x44: media_pci_wr16(f->common + 0x1c, (uint16_t)value); break;
    case 0x50:                                                   // QueueNotify
        if (value < 2) media_pci_wr16(f->notify + (uintptr_t)f->notify_off[value] * f->multiplier, (uint16_t)value);
        break;
    case 0x70: media_pci_wr8(f->common + 0x14, (uint8_t)value); break;
    case 0x80: case 0x84: media_pci_wr32(f->common + 0x20 + (offset - 0x80), value); break;   // queue_desc
    case 0x90: case 0x94: media_pci_wr32(f->common + 0x28 + (offset - 0x90), value); break;   // queue_driver
    case 0xa0: case 0xa4: media_pci_wr32(f->common + 0x30 + (offset - 0xa0), value); break;   // queue_device
    default: break;
    }
}
// The function's regions from its vendor capabilities: common configuration (1), notifications (2) and the device's
// configuration (4), each in the BAR crosvm assigned, mapped as device memory; the ISR (3) is not read (polled). False
// when one is missing.
static bool media_pci_regions(struct media_pci *f, uintptr_t config) {
    if (!(media_pci_rd16(config + 0x06) & 0x10)) return false;                  // no capabilities list
    struct { uint8_t bar; uint32_t offset, length; bool found; } r[5];
    memset(r, 0, sizeof(r));
    uint32_t multiplier = 0;
    uint8_t at = media_pci_rd8(config + 0x34) & 0xfc;
    for (unsigned walked = 0; at >= 0x40 && at <= 0x100 - 20 && walked < 48; ++walked) {
        const uint8_t id = media_pci_rd8(config + at), next = media_pci_rd8(config + at + 1) & 0xfc, type = media_pci_rd8(config + at + 3);
        if (id == 0x09 && type >= 1 && type <= 4 && !r[type].found) {
            r[type].bar = media_pci_rd8(config + at + 4);
            r[type].offset = media_pci_rd32(config + at + 8);
            r[type].length = media_pci_rd32(config + at + 12);
            r[type].found = true;
            if (type == 2) multiplier = media_pci_rd32(config + at + 16);
        }
        at = next;
    }
    // the device's configuration: device_caps, device_type and card[32] (40 bytes)
    if (!r[1].found || !r[2].found || !r[4].found || r[1].length < 0x38 || r[2].length < 2 || r[4].length < 40) return false;
    media_pci_wr16(config + 0x04, (uint16_t)(media_pci_rd16(config + 0x04) | 6));   // memory space and bus master
    uintptr_t mapped[5] = {0};
    for (unsigned t = 1; t <= 4; ++t) {
        if (t == 3) continue;
        const unsigned bar = r[t].bar;
        if (bar > 5 || r[t].offset > UINT32_MAX - r[t].length) return false;
        const uint32_t low = media_pci_rd32(config + 0x10 + bar * 4);
        if (low & 1) return false;                                              // an I/O BAR
        uint64_t base = low & ~(uint64_t)0xf;
        if ((low & 6) == 4) base |= (uint64_t)media_pci_rd32(config + 0x10 + bar * 4 + 4) << 32;
        if (!base) return false;
        const uint64_t from = (base + r[t].offset) & ~(uint64_t)0x3fff;
        const uint64_t to = (base + r[t].offset + r[t].length + 0x3fff) & ~(uint64_t)0x3fff;
        const uintptr_t m = ml_io_map((vm_offset_t)from, (vm_size_t)(to - from));
        if (!m) return false;
        mapped[t] = m + (uintptr_t)(base + r[t].offset - from);
    }
    f->common = mapped[1]; f->notify = mapped[2]; f->device = mapped[4];
    f->multiplier = multiplier;
    return true;
}
// The bus's next 1af4:1070 function not yet taken (function 0 of each device: 32 KiB a device on crosvm's ECAM) with its
// regions mapped, for media[n]: its configuration space, or 0.
static uintptr_t media_pci_find(unsigned n, vm_offset_t bus, uint64_t size) {
    const uintptr_t space = ml_io_map(bus, (vm_size_t)size);
    if (!space) { printf("droidvm-media: the PCI bus at %#llx not mapped\n", (unsigned long long)bus); return 0; }
    unsigned seen = 0;
    for (unsigned dev = 0; dev < 32 && ((uint64_t)dev << 15) + 0x1000 <= size; ++dev) {
        const uintptr_t config = space + ((uintptr_t)dev << 15);
        if (media_pci_rd32(config) != (0x1af4u | 0x1070u << 16) || seen++ < media_pci_taken) continue;
        if (!media_pci_regions(&media_pci[n], config)) {
            printf("droidvm-media: the virtio-media function at 00:%02x.0 has not its regions\n", dev);
            return 0;
        }
        media_pci[n].on = true;
        media_pci[n].slot = (uint8_t)dev;
        ++media_pci_taken;
        printf("droidvm-media: virtio-pci media device at 00:%02x.0 (1af4:1070), notifications x%u, polled\n", dev,
               media_pci[n].multiplier);
        return config;
    }
    printf("droidvm-media: no %s1af4:1070 function on bus 0 (crosvm without --virtio-media? vm-boot --video-decoder)\n",
           media_pci_taken ? "further " : "");
    return 0;
}
static uint32_t media_read32(const struct media_device *d, unsigned offset) {
    if (media_pci[d - media].on) return media_pci_read(&media_pci[d - media], offset);
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(d->registers + offset) : "memory");
    return value;
}
static void media_write32(const struct media_device *d, unsigned offset, uint32_t value) {
    if (media_pci[d - media].on) { media_pci_write(&media_pci[d - media], offset, value); return; }
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(d->registers + offset) : "memory");
}
#else
// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t media_read32(const struct media_device *d, unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(d->registers + offset) : "memory");
    return value;
}
static void media_write32(const struct media_device *d, unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(d->registers + offset) : "memory");
}
#endif
"#),
    // A command crosvm answers after the wait (take18b). crosvm's decoder creates, configures and starts the phone's codec at
    // STREAMON(OUTPUT) and waits up to 10 s for it (android.rs CODEC_START_TIMEOUT); the pin's 5 s wait then gave up while the
    // device still had the chain, and its late completion was read by the next command as that command's own, and so on for
    // every later one (the off-by-one the audio transport had in take7). The native kernel waits 12 s, and a command that got
    // no answer in it is the device's still: its completion is taken off the used ring before the next command is posted, and
    // while it has not come no other command is sent (the chain's descriptors and buffers are the device's).
    ("static int command(struct media_device *d, uint32_t command_size, uint32_t out, uint32_t response, uint32_t in) {\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/codec_transport.rs): commands the device answered after their wait, per device, and how long one
// waits; counted for OneiOSCodec.c's transport line (late: answered after the wait and taken off before the next one).
static uint16_t media_late[MEDIA_MAX];
static const unsigned media_command_wait_ms = 12000;
volatile uint64_t oneios_media_late_taken, oneios_media_late_refused;
#endif
static int command(struct media_device *d, uint32_t command_size, uint32_t out, uint32_t response, uint32_t in) {
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    for (unsigned waited = 0; media_late[index_of(d)]; ++waited) {
        if (used(d, MEDIA_COMMANDQ)[1] != d->used[MEDIA_COMMANDQ]) {
            media_barrier();
            ++d->used[MEDIA_COMMANDQ];
            --media_late[index_of(d)];
            oneios_media_late_taken++;
            continue;
        }
        if (waited >= 2000) { oneios_media_late_refused++; return -1; }   // still the device's: nothing else can be sent
        reap_events(d);
        IOSleep(1);
    }
#endif
"#),
    ("        if (waited == 5000) { droidvm_media_timeouts++; return -1; }   // 5 s\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
        if (waited == media_command_wait_ms) { droidvm_media_timeouts++; media_late[index_of(d)]++; return -1; }   // 12 s
#else
        if (waited == 5000) { droidvm_media_timeouts++; return -1; }   // 5 s
#endif
"#),
    // The pool's size, for the decoder's sessions to fit their buffers in it (staged/codec_decode.rs CODEC_CROSVM).
    ("unsigned droidvm_media_devices(void) { return media_count; }\n",
     "unsigned droidvm_media_devices(void) { return media_count; }\n\
// OneiOS (staged/codec_transport.rs): the media_host pool of a device (0: none), which its MMAP buffers come out of.\n\
uint64_t droidvm_media_pool_size(unsigned device) { return device < media_count ? media[device].pool_size : 0; }\n"),
    ("        d->registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
        // OneiOS native: on crosvm's ECAM bus ("oneios,virtio-media-pci", xnuloader-crosvm --media) the next 1af4:1070
        // function, wherever crosvm put it; an MMIO window as before
        if (SecureDTGetProperty(node, "compatible", &value, &size) == kSuccess && size >= 24 &&
            !memcmp(value, "oneios,virtio-media-pci", 24))
            d->registers = media_pci_find(media_count - 1, (vm_offset_t)(reg[0] - ranges[0] + ranges[1]), reg[1]);
        else
#endif
        d->registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
"#),
];
