// From build-oneios-gpu.py (STAGED), written out by a helper of the port and kept by hand since.
/// DroidVMGpu.cpp's lines that change. Each anchor must be there exactly once.
pub const GPU: &[(&str, &str)] = &[
    ("#include \"NativeAdrenoMemory.h\"\n",
     concat!("#include \"NativeAdrenoMemory.h\"\n",
             "extern \"C\" bool oneios_adreno_wanted(void);\n",
             "extern \"C\" bool oneios_adreno_parent_bound(void);\n",
             "extern \"C\" void oneios_adreno_parent_slide(void);\n",
             "extern \"C\" void oneios_gpu_client_list(char *, size_t);\n")),
    // the start is OneiOSAdreno.cpp's decision (the host offers 3D, or oneios-gpu-metal), not two boot arguments
    ("    if (!bootArg(\"vgpu-3d\", 0) || bootArg(\"droidvm-gpu-metal\", 0) != 1) return;\n",
     "    if (!oneios_adreno_wanted()) return;\n"),
    // the bindings are for one kernel collection: nothing is registered under another class
    ("    if (!parent || parent->getClassSize() != 136) {droidvm_metal_publish[2] = uint32_t(kIOReturnNotFound); return;}\n",
     concat!("    if (!parent || parent->getClassSize() != 136) {droidvm_metal_publish[2] = uint32_t(kIOReturnNotFound); return;}\n",
             "    if (!oneios_adreno_parent_bound()) {droidvm_metal_publish[2] = uint32_t(kIOReturnNotPermitted); return;}\n")),
    // On the caller's thread (bsd_init's, in the board's late start), not on one of its own: registering a class holds
    // OSMetaClass's lock from preModLoad to postModLoad, and postModLoad takes the kext lock. A kext load on another
    // thread holds the kext lock and waits for the class lock: with the display's archive loading its graphics
    // family right after this start, the two threads stopped each other (seen: the boot hung at accelerator stage 2).
    (concat!("    thread_t thread = nullptr;\n",
             "    if (kernel_thread_start(publishThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);\n"),
     "    publishThread(nullptr, THREAD_AWAKENED);\n"),
    // the superclass link is the parent's link address until it is slid (OneiOSAdreno.cpp)
    (concat!("    native_adreno_accelerator_static_init();\n",
             "    status = OSMetaClass::postModLoad(handle);\n"),
     concat!("    native_adreno_accelerator_static_init();\n",
             "    oneios_adreno_parent_slide();\n",
             "    status = OSMetaClass::postModLoad(handle);\n")),
    // The grant's list is OneiOSAdreno.cpp's oneios_gpu_client_list: droidvm-gpu-clients, or without it the default there
    // ("*", every process, since 2026-11-12). The same list is
    // written for the plug-in as NativeAdrenoMetal.clients at every boot (0039), so the two gates never differ.
    // droidvm-gpu-clients= (empty) is backboardd alone; a list replaces the default; "*" is every process.
    ("    if (PE_parse_boot_argn(\"droidvm-gpu-clients\", clients, sizeof clients - 1)) {\n",
     concat!("    oneios_gpu_client_list(clients, sizeof clients);\n",
             "    {\n")),
];

/// DroidVMVirtioGpu3D.cpp's lines that change (hang, 2026-11-13): each blob's life goes to OneiOSAdreno.cpp, which
/// prints by client what kind the live blobs are, how old, and which closed ones the GPU's work in flight still holds
/// (oneios-gpu-kinds:, oneios-gpu-ages:; docs/gpu.md 5.1). The hooks are weak: without OneiOSAdreno.cpp nothing changes.
/// Each anchor must be there exactly once.
pub const GPU3D: &[(&str, &str)] = &[
    ("DroidVMClientMemory droidvm_virtgpu_client_memory[64];\n",
     concat!("DroidVMClientMemory droidvm_virtgpu_client_memory[64];\n",
             "// OneiOS: a blob made (its resource id, client slot, bytes, kind bits: 1 host-visible, 2 the caller's pages wired,\n",
             "// 4 this driver's pages in the caller's task, the blob flags << 4), closed while the GPU may still use it, and gone.\n",
             "void oneios_gpu_blob_made(uint32_t, uint32_t, uint64_t, uint32_t, task_t, uint64_t) __attribute__((weak));\n",
             "void oneios_gpu_blob_closed(uint32_t) __attribute__((weak));\n",
             "void oneios_gpu_blob_gone(uint32_t) __attribute__((weak));\n",
             "// OneiOS: who submits, for the host's GPU policy (role, QoS, deadline: OneiOSAdreno.cpp, docs/gpu.md 2.2b).\n",
             "void oneios_gpu_hint(uint8_t *) __attribute__((weak));\n")),
    // The hint at a context's creation (CTX_CREATE's context_init bits 8-23, beside the capset) and at each fenced
    // submission (the SUBMIT_3D header's three padding bytes), on the client's own thread (2026-11-15, fix-gpu-perf).
    ("    CtxCreate create = {{CmdCtxCreate, 0, 0, context, 0, {}}, 0, in.capset_id, {'x', 'n', 'u', ':'}};\n",
     concat!("    CtxCreate create = {{CmdCtxCreate, 0, 0, context, 0, {}}, 0, in.capset_id, {'x', 'n', 'u', ':'}};\n",
             "    if (oneios_gpu_hint) {\n",
             "        uint8_t hint[3] = {};\n",
             "        oneios_gpu_hint(hint);\n",
             "        create.contextInit |= uint32_t(hint[0]) << 8 | uint32_t(hint[1]) << 16;\n",
             "    }\n")),
    ("    Submit submit = {{CmdSubmit3D, uint32_t(FlagFence) | uint32_t(FlagRingIndex), fence, c->context, uint8_t(in.ring_idx), {}}, in.size, 0};\n",
     concat!("    Submit submit = {{CmdSubmit3D, uint32_t(FlagFence) | uint32_t(FlagRingIndex), fence, c->context, uint8_t(in.ring_idx), {}}, in.size, 0};\n",
             "    if (oneios_gpu_hint) oneios_gpu_hint(submit.h.padding);\n")),
    ("    c->resources[c->resourceCount++] = r;\n",
     concat!("    c->resources[c->resourceCount++] = r;\n",
             "    if (oneios_gpu_blob_made)\n",
             "        oneios_gpu_blob_made(r.id, c->slot, r.size, (r.hostVisible ? 1u : 0u) | (r.wired ? 2u : 0u) | (r.prepared ? 4u : 0u) |\n",
             "                             ((in.blob_flags & 0xfu) << 4), task, user);\n")),
    ("        c->closed[c->closedCount++] = {*r, c->sequence};\n",
     concat!("        c->closed[c->closedCount++] = {*r, c->sequence};\n",
             "        if (oneios_gpu_blob_closed) oneios_gpu_blob_closed(r->id);\n")),
    ("    countPages(c, r, -1);\n",
     concat!("    countPages(c, r, -1);\n",
             "    if (oneios_gpu_blob_gone) oneios_gpu_blob_gone(r.id);\n")),
    // The surface trail (widgets, 2026-11-13; docs/gpu.md 5.1 "Where a surface's pixels go"): each IOSurface use a command
    // buffer names as it commits (surface ID, written or read, the client's slot, its ticket), each use ended (the
    // command buffer completed, or ticket 0: the connection closed), each CPU lock of a surface (IOSurfaceRootUserClient's
    // lock trap, in the locking process), each submission by client, and the guest physical segments of each blob of the
    // caller's own pages (CREATE_BLOB_USER), so OneiOSAdreno.cpp can tell which process's imports share pages and read
    // what the pages hold. Weak, as above.
    ("volatile int droidvm_virtgpu_compositor_pid;\n",
     concat!("volatile int droidvm_virtgpu_compositor_pid;\n",
             "// OneiOS: the surface trail (OneiOSAdreno.cpp trail::).\n",
             "void oneios_gpu_surface_use(uint32_t, uint32_t, uint32_t, uint64_t) __attribute__((weak));\n",
             "void oneios_gpu_surface_done(uint32_t, uint32_t, uint32_t, uint64_t) __attribute__((weak));\n",
             "void oneios_gpu_surface_lock(uint32_t, uint32_t) __attribute__((weak));\n",
             "void oneios_gpu_submit(uint32_t) __attribute__((weak));\n",
             "void oneios_gpu_blob_segments(uint32_t, uint32_t, uint64_t, const void *, uint32_t) __attribute__((weak));\n",
             "bool oneios_gpu_reader_waits(int) __attribute__((weak));\n")),
    // A test switch (oneios-gpu-read-wait=1, off by default): the render server's command buffer waits, as it names the
    // surfaces it samples, for the other processes' GPU work begun before it that writes them (the kernel's own
    // awaitWriters, which this plug-in pin never asks for), at most droidvm_surface_read_wait_ms (100).
    ("    IOLockUnlock(lock);\n    return status;\n}\n}\n}\n// For the display: the holds taken so far",
     concat!("    IOLockUnlock(lock);\n",
             "    if (!status && head.count && oneios_gpu_reader_waits && oneios_gpu_reader_waits(c->pid)) awaitWriters(c, answer);\n",
             "    return status;\n}\n}\n}\n// For the display: the holds taken so far")),
    ("            gSurfaceUses[gSurfaceUseCount++] = {record.surface, record.written != 0, c, answer};\n",
     concat!("            gSurfaceUses[gSurfaceUseCount++] = {record.surface, record.written != 0, c, answer};\n",
             "            if (oneios_gpu_surface_use) oneios_gpu_surface_use(record.surface, record.written != 0, c->slot, answer);\n")),
    ("        if (gSurfaceUses[i].client == c && (!ticket || gSurfaceUses[i].ticket == ticket)) {\n",
     concat!("        if (gSurfaceUses[i].client == c && (!ticket || gSurfaceUses[i].ticket == ticket)) {\n",
             "            if (oneios_gpu_surface_done) oneios_gpu_surface_done(gSurfaceUses[i].surface, gSurfaceUses[i].write, c->slot, ticket);\n")),
    ("    droidvm_surface_lock_options[options & 3] = droidvm_surface_lock_options[options & 3] + 1;\n",
     concat!("    droidvm_surface_lock_options[options & 3] = droidvm_surface_lock_options[options & 3] + 1;\n",
             "    if (oneios_gpu_surface_lock) oneios_gpu_surface_lock(uint32_t(surface), uint32_t(options));\n")),
    ("    count(4);\n",
     concat!("    count(4);\n",
             "    if (oneios_gpu_submit) oneios_gpu_submit(c->slot);\n")),
    // Only the caller's own pages (r.wired), only once the host took the blob (status 0 here is the CreateBlob's answer).
    ("    if (entries) IODeleteData(entries, Entry, entryCapacity);\n",
     concat!("    if (!status && entries && r.wired && oneios_gpu_blob_segments) oneios_gpu_blob_segments(r.id, c->slot, r.size, entries, entryCount);\n",
             "    if (entries) IODeleteData(entries, Entry, entryCapacity);\n")),
];

/// DroidVMVirtioGpu.cpp's lines that change (native, 2026-11-20): the control queue is as long as the device takes, not
/// always 512 descriptors. QEMU's 2D virtio-gpu-device offers 64 (crosvm and xnuhost 512 or more, and there nothing
/// changes): the transport stopped at "queue unavailable or too small" and the display had no scanout. With a shorter
/// queue half of it are command slots (two descriptors each) and the last LargeSlots of those the large ones (half, with
/// fewer than 2 * LargeSlots). Each anchor must be there exactly once.
pub const TRANSPORT: &[(&str, &str)] = &[
    ("uint32_t hugeOwner = Slots;   // the slot the buffer is lent to; Slots when free\n",
     concat!("uint32_t hugeOwner = Slots;   // the slot the buffer is lent to; Slots when free\n",
             "// OneiOS: the control queue's length, the slots in use and the first large one, from the device (start()).\n",
             "uint32_t queueSize = QueueSize, usable = Slots, largeFrom = Large;\n")),
    ("    return head < QueueSize && !(head & 1) && (slots[head / 2].state == Posted || slots[head / 2].state == Abandoned);\n",
     "    return head < queueSize && !(head & 1) && (slots[head / 2].state == Posted || slots[head / 2].state == Abandoned);\n"),
    ("UsedOffset + 4) + consumed % QueueSize;\n", "UsedOffset + 4) + consumed % queueSize;\n"),
    ("                droidvm_virtio_gpu_stray[2] = head < QueueSize ? slots[head / 2].state : 0xff;\n",
     "                droidvm_virtio_gpu_stray[2] = head < queueSize ? slots[head / 2].state : 0xff;\n"),
    ("uint16_t(consumed - 1), head, unsigned(head < QueueSize ? slots[head / 2].state : 0xff));\n",
     "uint16_t(consumed - 1), head, unsigned(head < queueSize ? slots[head / 2].state : 0xff));\n"),
    ("        if (rd(0x44) || rd(0x34) < sizes[q]) return fail(\"queue unavailable or too small\");\n",
     concat!("        if (rd(0x44)) return fail(\"queue unavailable\");\n",
             "        uint32_t size = sizes[q];\n",
             "        if (q == 0 && rd(0x34) < size) {\n",
             "            const uint32_t max = rd(0x34);\n",
             "            if (max < 8 || (max & (max - 1))) return fail(\"control queue too small\");\n",
             "            queueSize = size = max;\n",
             "            usable = max / 2;\n",
             "            largeFrom = usable >= 2 * LargeSlots ? usable - LargeSlots : usable / 2;\n",
             "            for (uint32_t s = largeFrom; s < usable; ++s) {\n",
             "                Slot &slot = slots[s];\n",
             "                slot.request = largeRequest[s - largeFrom];\n",
             "                slot.response = largeResponse[s - largeFrom];\n",
             "                slot.requestCapacity = slot.responseCapacity = uint32_t(MaxCommand);\n",
             "                if (!physical(slot.request, slot.requestCapacity, slot.requestPA) ||\n",
             "                    !physical(slot.response, slot.responseCapacity, slot.responsePA)) return fail(\"noncontiguous command buffers\");\n",
             "            }\n",
             "            IOLog(\"virtio-gpu: the device's control queue is %u descriptors: %u command slots, %u of them large\\n\",\n",
             "                  max, usable, usable - largeFrom);\n",
             "        }\n",
             "        if (rd(0x34) < size) return fail(\"queue unavailable or too small\");\n")),
    ("        wr(0x38, sizes[q]); wr64(0x80, pa);", "        wr(0x38, size); wr64(0x80, pa);"),
    ("            for (uint32_t i = large ? Large : 0; i < Slots && s == Slots; ++i) if (slots[i].state == Free) s = i;\n",
     "            for (uint32_t i = large ? largeFrom : 0; i < usable && s == Slots; ++i) if (slots[i].state == Free) s = i;\n"),
    ("    avail[2 + available % QueueSize] = uint16_t(2 * s);", "    avail[2 + available % queueSize] = uint16_t(2 * s);"),
];

/// The native kernel's virtio-gpu on crosvm's PCI (docs/native.md "The GPU in the shared-RAM VM"): Droid-VM's crosvm has
/// no virtio-mmio device in its build for the phone, so the GPU is a virtio-pci function of its ECAM host, which
/// /arm-io/virtio-gpu names (compatible "oneios,virtio-gpu-pci", reg its configuration space; xnuloader-crosvm --gpu).
/// DroidVMVirtioGpu.cpp's lines that change for it, applied after TRANSPORT and compiled only with XNU_ONEIOS_NATIVE: the
/// MMIO transport's register offsets go to the function's regions (VirtIO 1.2 4.1.4), as the input's do
/// (hid_transport.rs HID_TRANSPORT_PCI). Its queues and buffers stay where the driver puts them, in the guest's RAM, which
/// the shared-RAM VM's host reads. No interrupt: without droidvm,intid the driver's watch polls the used ring. The hosted
/// kernel compiles the file as before.
pub const TRANSPORT_PCI: &[(&str, &str)] = &[
    (concat!("uint32_t rd(unsigned offset) {\n",
             "    uint32_t v; __asm__ volatile(\"ldr %w0, [%1]\" : \"=r\"(v) : \"r\"(registers + offset) : \"memory\"); return v;\n",
             "}\n",
             "void wr(unsigned offset, uint32_t v) {\n",
             "    __asm__ volatile(\"str %w0, [%1]\\n\\tdsb oshst\" :: \"r\"(v), \"r\"(registers + offset) : \"memory\");\n",
             "}\n"),
     concat!("uint32_t mmioRd(unsigned offset) {\n",
             "    uint32_t v; __asm__ volatile(\"ldr %w0, [%1]\" : \"=r\"(v) : \"r\"(registers + offset) : \"memory\"); return v;\n",
             "}\n",
             "void mmioWr(unsigned offset, uint32_t v) {\n",
             "    __asm__ volatile(\"str %w0, [%1]\\n\\tdsb oshst\" :: \"r\"(v), \"r\"(registers + offset) : \"memory\");\n",
             "}\n",
             "#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
             "// OneiOS native: the device as a virtio-pci function (/arm-io/virtio-gpu \"oneios,virtio-gpu-pci\"), the MMIO transport's\n",
             "// offsets (VirtIO 1.2 4.2.2) on its regions. Single loads and stores with a plain base register, as the MMIO accessors.\n",
             "bool pci;\n",
             "uintptr_t pciCommon, pciNotify, pciIsr;\n",
             "uint32_t pciNotifyMultiplier;\n",
             "uint16_t pciNotifyOffset[2];\n",
             "uint32_t pciRd32(uintptr_t p) { uint32_t v; __asm__ volatile(\"ldr %w0, [%1]\" : \"=r\"(v) : \"r\"(p) : \"memory\"); return v; }\n",
             "uint16_t pciRd16(uintptr_t p) { uint32_t v; __asm__ volatile(\"ldrh %w0, [%1]\" : \"=r\"(v) : \"r\"(p) : \"memory\"); return uint16_t(v); }\n",
             "uint8_t pciRd8(uintptr_t p) { uint32_t v; __asm__ volatile(\"ldrb %w0, [%1]\" : \"=r\"(v) : \"r\"(p) : \"memory\"); return uint8_t(v); }\n",
             "void pciWr32(uintptr_t p, uint32_t v) { __asm__ volatile(\"str %w0, [%1]\\n\\tdsb oshst\" :: \"r\"(v), \"r\"(p) : \"memory\"); }\n",
             "void pciWr16(uintptr_t p, uint16_t v) { __asm__ volatile(\"strh %w0, [%1]\\n\\tdsb oshst\" :: \"r\"(uint32_t(v)), \"r\"(p) : \"memory\"); }\n",
             "void pciWr8(uintptr_t p, uint8_t v) { __asm__ volatile(\"strb %w0, [%1]\\n\\tdsb oshst\" :: \"r\"(uint32_t(v)), \"r\"(p) : \"memory\"); }\n",
             "uint32_t pciRd(unsigned offset) {\n",
             "    switch (offset) {\n",
             "    case 0x10: return pciRd32(pciCommon + 0x04);              // DeviceFeatures, of the word DeviceFeaturesSel chose\n",
             "    case 0x34: return pciRd16(pciCommon + 0x18);              // QueueNumMax: the selected queue's size until it is set\n",
             "    case 0x44: return pciRd16(pciCommon + 0x1c);              // QueueReady: queue_enable\n",
             "    case 0x60: return pciRd8(pciIsr) & 3;                     // InterruptStatus: reading the ISR acknowledges it\n",
             "    case 0x70: return pciRd8(pciCommon + 0x14);               // Status\n",
             "    default: return 0;\n",
             "    }\n",
             "}\n",
             "void pciWr(unsigned offset, uint32_t value) {\n",
             "    switch (offset) {\n",
             "    case 0x14: pciWr32(pciCommon + 0x00, value); break;       // DeviceFeaturesSel\n",
             "    case 0x20: pciWr32(pciCommon + 0x0c, value); break;       // DriverFeatures (the driver keeps ACCESS_PLATFORM)\n",
             "    case 0x24: pciWr32(pciCommon + 0x08, value); break;       // DriverFeaturesSel\n",
             "    case 0x30:\n",
             "        pciWr16(pciCommon + 0x16, uint16_t(value));\n",
             "        if (value < 2) pciNotifyOffset[value] = pciRd16(pciCommon + 0x1e);\n",
             "        break;\n",
             "    case 0x38: pciWr16(pciCommon + 0x18, uint16_t(value)); break;\n",
             "    case 0x44: pciWr16(pciCommon + 0x1c, uint16_t(value)); break;\n",
             "    case 0x50:\n",
             "        if (value < 2) pciWr16(pciNotify + uintptr_t(pciNotifyOffset[value]) * pciNotifyMultiplier, uint16_t(value));\n",
             "        break;\n",
             "    case 0x70: pciWr8(pciCommon + 0x14, uint8_t(value)); break;\n",
             "    case 0x80: case 0x84: pciWr32(pciCommon + 0x20 + (offset - 0x80), value); break;   // queue_desc\n",
             "    case 0x90: case 0x94: pciWr32(pciCommon + 0x28 + (offset - 0x90), value); break;   // queue_driver\n",
             "    case 0xa0: case 0xa4: pciWr32(pciCommon + 0x30 + (offset - 0xa0), value); break;   // queue_device\n",
             "    default: break;                                            // InterruptACK (0x64): the ISR's read did it\n",
             "    }\n",
             "}\n",
             "// The function's regions from its vendor capabilities (VirtIO 1.2 4.1.4): common configuration, notifications, ISR, each\n",
             "// in the BAR crosvm assigned (its tree says linux,pci-probe-only), mapped as device memory. False when one is missing.\n",
             "bool pciRegions(uintptr_t config) {\n",
             "    if (!(pciRd16(config + 0x06) & 0x10)) return false;                    // no capabilities list\n",
             "    struct { uint8_t bar; uint32_t offset, length; bool found; } r[4] = {};\n",
             "    uint32_t multiplier = 0;\n",
             "    uint8_t at = pciRd8(config + 0x34) & 0xfc;\n",
             "    for (unsigned walked = 0; at >= 0x40 && at <= 0x100 - 20 && walked < 48; ++walked) {\n",
             "        const uint8_t id = pciRd8(config + at), next = pciRd8(config + at + 1) & 0xfc, type = pciRd8(config + at + 3);\n",
             "        if (id == 0x09 && type >= 1 && type <= 3 && !r[type].found) {\n",
             "            r[type].bar = pciRd8(config + at + 4);\n",
             "            r[type].offset = pciRd32(config + at + 8);\n",
             "            r[type].length = pciRd32(config + at + 12);\n",
             "            r[type].found = true;\n",
             "            if (type == 2) multiplier = pciRd32(config + at + 16);\n",
             "        }\n",
             "        at = next;\n",
             "    }\n",
             "    if (!r[1].found || !r[2].found || !r[3].found || r[1].length < 0x38 || r[2].length < 2 || r[3].length < 1) return false;\n",
             "    pciWr16(config + 0x04, uint16_t(pciRd16(config + 0x04) | 6));            // memory space and bus master\n",
             "    uintptr_t mapped[3];\n",
             "    for (unsigned t = 1; t <= 3; ++t) {\n",
             "        const unsigned bar = r[t].bar;\n",
             "        if (bar > 5 || r[t].offset > UINT32_MAX - r[t].length) return false;\n",
             "        const uint32_t low = pciRd32(config + 0x10 + bar * 4);\n",
             "        if (low & 1) return false;                                          // an I/O BAR\n",
             "        uint64_t base = low & ~uint64_t(0xf);\n",
             "        if ((low & 6) == 4) base |= uint64_t(pciRd32(config + 0x10 + bar * 4 + 4)) << 32;\n",
             "        if (!base) return false;\n",
             "        const uint64_t from = (base + r[t].offset) & ~uint64_t(0x3fff), to = (base + r[t].offset + r[t].length + 0x3fff) & ~uint64_t(0x3fff);\n",
             "        const uintptr_t m = ml_io_map(vm_offset_t(from), vm_size_t(to - from));\n",
             "        if (!m) return false;\n",
             "        mapped[t - 1] = m + uintptr_t(base + r[t].offset - from);\n",
             "    }\n",
             "    pciCommon = mapped[0]; pciNotify = mapped[1]; pciIsr = mapped[2];\n",
             "    pciNotifyMultiplier = multiplier;\n",
             "    return true;\n",
             "}\n",
             "#endif\n",
             "uint32_t rd(unsigned offset) {\n",
             "#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
             "    if (pci) return pciRd(offset);\n",
             "#endif\n",
             "    return mmioRd(offset);\n",
             "}\n",
             "void wr(unsigned offset, uint32_t v) {\n",
             "#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
             "    if (pci) { pciWr(offset, v); return; }\n",
             "#endif\n",
             "    mmioWr(offset, v);\n",
             "}\n")),
    (concat!("    registers = ml_io_map(reg[0] - ranges[0] + ranges[1], 0x1000);\n",
             "    if (!registers || rd(0) != 0x74726976 || rd(4) != 2 || rd(8) != 16) return fail(\"not a modern virtio GPU\");\n"),
     concat!("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
             "    // OneiOS native: a virtio-pci function of crosvm's ECAM host (xnuloader-crosvm --gpu), or the MMIO device as before\n",
             "    if (SecureDTGetProperty(node, \"compatible\", &value, &size) == kSuccess && size >= 22 &&\n",
             "        !memcmp(value, \"oneios,virtio-gpu-pci\", 22)) {\n",
             "        const uintptr_t config = ml_io_map(reg[0] - ranges[0] + ranges[1], 0x1000);\n",
             "        if (!config || pciRd32(config) != (0x1af4u | 0x1050u << 16)) return fail(\"not a modern virtio-pci GPU (1af4:1050)\");\n",
             "        if (!pciRegions(config)) return fail(\"the virtio-pci GPU's regions are not there\");\n",
             "        pci = true;\n",
             "        IOLog(\"virtio-gpu: PCI function at %#llx, notifications x%u, polled (no interrupt)\\n\", reg[0], pciNotifyMultiplier);\n",
             "    } else {\n",
             "        registers = ml_io_map(reg[0] - ranges[0] + ranges[1], 0x1000);\n",
             "        if (!registers || rd(0) != 0x74726976 || rd(4) != 2 || rd(8) != 16) return fail(\"not a modern virtio GPU\");\n",
             "    }\n",
             "#else\n",
             "    registers = ml_io_map(reg[0] - ranges[0] + ranges[1], 0x1000);\n",
             "    if (!registers || rd(0) != 0x74726976 || rd(4) != 2 || rd(8) != 16) return fail(\"not a modern virtio GPU\");\n",
             "#endif\n")),
];

/// DroidVMVirtioGpu3D.cpp's lines that change in the native kernel (after GPU3D): the first 64 refused user calls go to the
/// kernel's log by client, operation and status. A Metal device that fails (Turnip's `virtio-gpu DRM context failed`, the
/// plug-in's "no device: Vulkan(VkResultCode(-3))") leaves no other trace there: the counts of refusals by operation were
/// for xnuhost's report, which a native kernel has not.
/// And the creating task's hint stays out of CTX_CREATE's context_init there: crosvm passes the field to virglrenderer, which
/// refuses any bit above the capset (`ctx_flags & ~VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK`: EINVAL, crosvm's "CtxCreate:
/// ErrRutabaga(ComponentError(22))"), so every Metal process's context failed (vm-boot gpu5; the self-test's own context,
/// made without the hint, worked). xnuhost's device read the capset from bits 0-7 and the hint from 8-23 (fix-gpu-perf).
/// And the fence counters the display's CPU copy waits on (NativeLegacyDisplay.cpp gpuWrote, `native_adreno_fences`, which
/// OneiOSDisplay.cpp defines weak as "none": a swap was then copied at once, the compositor's GPU work still running, and on
/// the S25 every icon, widget and app the GPU drew flickered as the home screen moved, vm-boot gpu7): submitted, the latest
/// fenced submission's order (droidvm_virtgpu_submitted); retired, the order below the oldest unfinished one of every client
/// (the hosted display's rule: icons and widgets are other processes' GPU work, and with the compositor's alone the home
/// screen still flickered while it moved, gpu8), so that a swap is copied once all GPU work up to it has finished. Every
/// 5 s while swaps wait, one line: `oneios-display-wait:` the display's waits (NativeLegacyDisplay.cpp native_legacy_direct).
pub const GPU3D_NATIVE: &[(&str, &str)] = &[(
    concat!("        count(7);\n",
            "        droidvm_virtgpu_last_refusal[0] = op;"),
    concat!("        count(7);\n",
            "#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
            "        {\n",
            "            static volatile uint32_t refusalsLogged;\n",
            "            if (__atomic_fetch_add(&refusalsLogged, 1, __ATOMIC_RELAXED) < 64)\n",
            "                IOLog(\"virtgpu: %.16s op %u refused %#x (in %lu bytes, out %lu)\\n\", c->name, op, uint32_t(status),\n",
            "                      (unsigned long)inSize, (unsigned long)outSize);\n",
            "        }\n",
            "#endif\n",
            "        droidvm_virtgpu_last_refusal[0] = op;"),
), (
    "extern \"C\" bool droidvm_virtgpu_finished_by(uint64_t order, int pid) {\n",
    concat!("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
            "extern \"C\" volatile uint32_t native_legacy_direct[14] __attribute__((weak));\n",
            "extern \"C\" bool native_adreno_fences(uint32_t *submitted, uint32_t *retired) {\n",
            "    using namespace DroidVMVirtioGpu;\n",
            "    if (!submitted || !retired || !gClientsLock) return false;\n",
            "    {\n",
            "        static uint64_t reportedAt; static uint32_t waitsBefore;\n",
            "        const uint64_t now = mach_absolute_time();\n",
            "        uint64_t ns = 0; absolutetime_to_nanoseconds(now - reportedAt, &ns);\n",
            "        if (&native_legacy_direct && ns >= 5000000000ull && native_legacy_direct[9] != waitsBefore) {\n",
            "            reportedAt = now; waitsBefore = native_legacy_direct[9];\n",
            "            IOLog(\"oneios-display-wait: %u swaps waited for the GPU (%u of 8 ms or more, %u gave up at 250 ms), %u us in all, longest %u us; %u copied\\n\",\n",
            "                  native_legacy_direct[9], native_legacy_direct[13], native_legacy_direct[12], native_legacy_direct[10], native_legacy_direct[11],\n",
            "                  native_legacy_direct[7]);\n",
            "        }\n",
            "    }\n",
            "    const uint64_t latest = __atomic_load_n(&gSubmittedOrder, __ATOMIC_ACQUIRE);\n",
            "    const int pid = 0;   // every client's\n",
            "    uint64_t oldest = latest + 1;\n",
            "    IOLockLock(gClientsLock);\n",
            "    for (uint32_t i = 0; i < MaxClients; ++i) {\n",
            "        Client *c = gClients[i];\n",
            "        if (!c || (pid && c->pid != pid)) continue;\n",
            "        IOLockLock(c->fenceLock);\n",
            "        reapFences(c);\n",
            "        for (uint32_t j = 0; j < c->pendingCount; ++j) if (c->pending[j].order < oldest) oldest = c->pending[j].order;\n",
            "        IOLockUnlock(c->fenceLock);\n",
            "    }\n",
            "    IOLockUnlock(gClientsLock);\n",
            "    *submitted = uint32_t(latest);\n",
            "    *retired = uint32_t(oldest - 1);\n",
            "    return true;\n",
            "}\n",
            "#endif\n",
            "extern \"C\" bool droidvm_virtgpu_finished_by(uint64_t order, int pid) {\n"),
), (
    "        create.contextInit |= uint32_t(hint[0]) << 8 | uint32_t(hint[1]) << 16;\n",
    concat!("#if !(defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE)\n",
            "        create.contextInit |= uint32_t(hint[0]) << 8 | uint32_t(hint[1]) << 16;\n",
            "#endif\n"),
)];

/// DroidVMVirtioGpu.cpp's lines that change for the frame-stage lines (native-perf, 2026-11-26; after TRANSPORT and
/// TRANSPORT_PCI, both kernels): the transport already stamps every command when it is taken off the used ring
/// (`slot.doneUs`) and keeps the device's leg of the unfenced ones (`droidvm_virtio_gpu_legs`); a fenced one (a 3D
/// submission) left no trace of it, and `oneios-gpu-report`'s "frames" could not tell the host's time from the time until
/// the client looked. So: each slot keeps how many unfenced commands the device had still to answer as it was posted (a
/// blob made or released, a mapping: crosvm's one worker takes them first), `unfenced` counts those, and `times()` gives a
/// returned command's stamps before `poll()` frees its slot. `times()` takes the transport's lock, as `post()` and `poll()` do, so
/// the 3D half calls it only with oneios-gpu-stages=1. Nothing is printed here: OneiOSAdreno.cpp's
/// `stages::` does that (gpu/frame_stages.h). Each anchor must be there exactly once.
pub const TRANSPORT_STAGES: &[(&str, &str)] = &[
    ("    uint32_t postedInFlight;\n",
     concat!("    uint32_t postedInFlight;\n",
             "    uint32_t postedAhead;   // OneiOS: the unfenced commands the device had not answered as this one was posted\n")),
    ("uint16_t available, consumed;\n",
     concat!("uint16_t available, consumed;\n",
             "uint32_t unfenced;   // OneiOS: unfenced commands posted and not yet taken off the used ring (with the lock held)\n")),
    ("    slot.postedUs = nowUs(); slot.postedInFlight = inFlight; slot.detached = false;\n",
     concat!("    slot.postedUs = nowUs(); slot.postedInFlight = inFlight; slot.detached = false;\n",
             "    slot.postedAhead = unfenced; if (!slot.fenced) ++unfenced;   // OneiOS: staged/gpu.rs TRANSPORT_STAGES\n")),
    ("            slot.doneUs = nowUs();\n",
     concat!("            slot.doneUs = nowUs();\n",
             "            if (!slot.fenced && unfenced) --unfenced;   // OneiOS: the device has answered it\n")),
    ("// A command nobody waits for: posted as any, its slot goes back to the queue when the device returns it (reap()), the answer\n",
     concat!("// OneiOS: a returned command's stamps, before poll() frees its slot: [0] posted (before the doorbell), [1] taken off the\n",
             "// used ring (both us, nowUs()), [2] the commands in flight as it was posted, itself among them, [3] of those the unfenced\n",
             "// ones, which the device had still to do first. False while the device has it, or for another command's ticket.\n",
             "bool times(uint32_t ticket, uint64_t *out) {\n",
             "    const uint32_t s = ticket & 0xff;\n",
             "    if (s >= Slots || !lock || !out) return false;\n",
             "    IOLockLock(lock);\n",
             "    reap();\n",
             "    const Slot &slot = slots[s];\n",
             "    const bool done = slot.generation == ticket >> 8 && slot.state == Done;\n",
             "    if (done) { out[0] = slot.postedUs; out[1] = slot.doneUs; out[2] = slot.postedInFlight; out[3] = slot.postedAhead; }\n",
             "    IOLockUnlock(lock);\n",
             "    return done;\n",
             "}\n",
             "// A command nobody waits for: posted as any, its slot goes back to the queue when the device returns it (reap()), the answer\n")),
];

/// DroidVMVirtioGpu3D.cpp's lines that change for the frame-stage lines (after GPU3D and GPU3D_NATIVE, both kernels): as
/// the render server's waiter takes a fenced submission's answer (reapFences, where "frames" is counted), the command's
/// own stamps go to OneiOSAdreno.cpp (`oneios_gpu_stage_fence`, weak: without it nothing changes and `times()` is never
/// called): whether they were had (a command returned between the two looks has none), the four of `times()`, and now.
/// Only for the process "frames" is about (backboardd), and only with boot argument oneios-gpu-stages=1 (`oneios_gpu_stage_wanted`,
/// weak: without it, or with the argument 0 (the default), `times()` is not called and its lock is not taken). Each anchor must be there exactly once.
pub const GPU3D_STAGES: &[(&str, &str)] = &[
    ("volatile uint64_t droidvm_virtgpu_rs_fences[3], droidvm_virtgpu_rs_fence_hist[10];\n",
     concat!("volatile uint64_t droidvm_virtgpu_rs_fences[3], droidvm_virtgpu_rs_fence_hist[10];\n",
             "// OneiOS: the stages of each of those (OneiOSAdreno.cpp stages::, gpu/frame_stages.h; staged/gpu.rs GPU3D_STAGES).\n",
             "void oneios_gpu_stage_fence(uint32_t, const uint64_t *, uint64_t) __attribute__((weak));\n",
             "bool oneios_gpu_stage_wanted(void) __attribute__((weak));   // boot argument oneios-gpu-stages=1; default off\n")),
    ("namespace DroidVMVirtioGpu {\nnamespace {\nenum : uint32_t { MaxClients = 64,",
     concat!("namespace DroidVMVirtioGpu {\n",
             "bool times(uint32_t, uint64_t *);   // OneiOS: the staged DroidVMVirtioGpu.cpp's (TRANSPORT_STAGES)\n",
             "namespace {\nenum : uint32_t { MaxClients = 64,")),
    ("        if (!poll(c->pending[i].ticket, &result)) { ++i; continue; }\n",
     concat!("        uint64_t stamps[4] = {};   // OneiOS: the command's own, before poll() frees its slot\n",
             "        const bool stamped = oneios_gpu_stage_fence && oneios_gpu_stage_wanted && oneios_gpu_stage_wanted() &&\n",
             "                             !strncmp(c->name, \"backboardd\", sizeof c->name) &&\n",
             "                             times(c->pending[i].ticket, stamps);\n",
             "        if (!poll(c->pending[i].ticket, &result)) { ++i; continue; }\n")),
    ("            if (us > droidvm_virtgpu_rs_fences[2]) droidvm_virtgpu_rs_fences[2] = us;\n",
     concat!("            if (us > droidvm_virtgpu_rs_fences[2]) droidvm_virtgpu_rs_fences[2] = us;\n",
             "            if (oneios_gpu_stage_fence) oneios_gpu_stage_fence(stamped, stamps, nowUsLocal());\n")),
];

/// DroidVMVirtioGpu3D.cpp's lines that change for the GPU address room (take17, 2026-11-26; after GPU3D_STAGES, both
/// kernels). The phone's crosvm gives every context 2 GiB of GPU addresses (its drm2kgsl: crosvm.log's `VA slices: 64 x
/// 2GB`), which the context's Turnip is told by GET_PARAM in its own ring and takes as its whole budget; the render
/// server maps every client's surfaces beside its own blocks, and with 1080p video it held 1.9 to 2.0 GiB of blobs in
/// take14, take15 and take16 alike. The kernel does not read that ring: what it sees is the DRM capset, which names
/// the device's whole range (`va_start`, `va_size`: 128 GiB there). That range goes to OneiOSAdreno.cpp as a process
/// asks for the capset (`oneios_gpu_capset_seen`, weak: without it nothing changes), which says it once and prints
/// each client's blobs against a context's part, a figure of its own (`oneios-gpu-room:`, `oneios-gpu-room-mib`).
/// Each anchor must be there exactly once.
pub const GPU3D_ROOM: &[(&str, &str)] = &[
    ("void oneios_gpu_hint(uint8_t *) __attribute__((weak));\n",
     concat!("void oneios_gpu_hint(uint8_t *) __attribute__((weak));\n",
             "// OneiOS (take17): the GPU address range the DRM capset names (the device's whole range), as a process asks\n",
             "// for it: its pid, the range's start and size (OneiOSAdreno.cpp watch::; staged/gpu.rs GPU3D_ROOM).\n",
             "void oneios_gpu_capset_seen(uint32_t, uint64_t, uint64_t) __attribute__((weak));\n")),
    ("    if (!status) memcpy(out, answer + sizeof(Header), outSize);\n",
     concat!("    if (!status) memcpy(out, answer + sizeof(Header), outSize);\n",
             "    if (!status && in.capset_id == CapsetIdDrm && bytes >= sizeof(CapsetDrm) && oneios_gpu_capset_seen) {\n",
             "        CapsetDrm caps;   // OneiOS (take17): two words stored under the clients' lock, nothing printed here\n",
             "        memcpy(&caps, answer + sizeof(Header), sizeof caps);\n",
             "        oneios_gpu_capset_seen(uint32_t(proc_selfpid()), caps.vaStart, caps.vaSize);\n",
             "    }\n")),
];

/// DroidVMVirtioGpu3D.cpp's lines that change for the render server's read wait (take20, 2026-11-27; after GPU3D_ROOM, both
/// kernels). NativeMetal 0004 (take18) has backboardd ask, before each submission, that the kernel wait (awaitWriters, at
/// most droidvm_surface_read_wait_ms, 100) for other processes' GPU work begun before it that writes a surface it reads; that
/// work's ticket ends when its command buffer completes in the writing process. take19's second session (-loggpu,
/// 11:51:55-11:54:26 UTC, the owner's "continuous lag on the Home screen after closing the notification panel, until an app
/// opens; pulling the panel down again fixes it"): oneios-gpu-sync `reads that waited for writers +97, gave up +97, max ever
/// 101174 us` and the same in ten rounds: every frame of the render server waited the whole 100 ms for a writer whose ticket
/// never ended (`holds 94458 us a swap`), so the Home screen drew 10 frames a second while it moved and nothing when the
/// finger lifted. A writer that made a read give up is no longer waited for by any read of the render server (its use is
/// marked stale: `oneios_gpu_read_wait_stale`); its ticket still ends as before when its command buffer completes, and CPU
/// locks still wait for it. So such a writer costs
/// one frame 100 ms, not every frame until it runs again. OneiOSAdreno.cpp says each one (the first 64 of a boot: process,
/// surface, ticket) and counts them; its `oneios_gpu_read_wait_stale_on` (boot argument oneios-read-wait-stale=0) turns the
/// change off; both weak: without OneiOSAdreno.cpp nothing changes. Each anchor must be there exactly once.
pub const GPU3D_READ_WAIT: &[(&str, &str)] = &[
    ("bool oneios_gpu_reader_waits(int) __attribute__((weak));\n",
     concat!("bool oneios_gpu_reader_waits(int) __attribute__((weak));\n",
             "// OneiOS (take20): whether a writer that made the render server's read wait give up is no longer waited for, and each\n",
             "// such writer (surface, pid, ticket) as it is marked (OneiOSAdreno.cpp; staged/gpu.rs GPU3D_READ_WAIT).\n",
             "bool oneios_gpu_read_wait_stale_on(void) __attribute__((weak));\n",
             "void oneios_gpu_read_wait_stale(uint32_t, int, uint64_t) __attribute__((weak));\n")),
    // the use's own mark (aggregate initializations leave it false); `write` stays as it was, for the CPU locks and the trail
    ("struct SurfaceUse { uint32_t surface; bool write; Client *client; uint64_t ticket; };\n",
     concat!("struct SurfaceUse { uint32_t surface; bool write; Client *client; uint64_t ticket; bool stale; };   // OneiOS take20: stale\n")),
    ("            if (other.surface == mine.surface && other.write && other.ticket < ticket && other.client->pid != c->pid) return true;\n",
     concat!("            if (other.surface == mine.surface && other.write && !other.stale && other.ticket < ticket && other.client->pid != c->pid) return true;\n")),
    ("void awaitWriters(Client *c, uint64_t ticket) {\n",
     concat!("// OneiOS (take20, staged/gpu.rs GPU3D_READ_WAIT): gSurfaceLock held, after `ticket`'s read wait gave up: the other\n",
             "// processes' writes begun before it of the surfaces it reads are marked stale, so no read of the render server waits for\n",
             "// them again; their tickets end as before, and CPU locks still wait for them (waitSurface does not look at the mark).\n",
             "void staleWriters(Client *c, uint64_t ticket) {\n",
             "    for (uint32_t i = 0; i < gSurfaceUseCount; ++i) {\n",
             "        const SurfaceUse &mine = gSurfaceUses[i];\n",
             "        if (mine.client != c || mine.ticket != ticket || mine.write) continue;\n",
             "        for (uint32_t j = 0; j < gSurfaceUseCount; ++j) {\n",
             "            SurfaceUse &other = gSurfaceUses[j];\n",
             "            if (other.surface != mine.surface || !other.write || other.stale || other.ticket >= ticket || other.client->pid == c->pid) continue;\n",
             "            other.stale = true;\n",
             "            oneios_gpu_read_wait_stale(other.surface, other.client->pid, other.ticket);\n",
             "        }\n",
             "    }\n",
             "}\n",
             "void awaitWriters(Client *c, uint64_t ticket) {\n")),
    ("        if (woke == THREAD_TIMED_OUT) { droidvm_surface_read_waits[1] = droidvm_surface_read_waits[1] + 1; break; }\n",
     concat!("        if (woke == THREAD_TIMED_OUT) {\n",
             "            droidvm_surface_read_waits[1] = droidvm_surface_read_waits[1] + 1;\n",
             "            if (oneios_gpu_read_wait_stale_on && oneios_gpu_read_wait_stale && oneios_gpu_read_wait_stale_on()) staleWriters(c, ticket);\n",
             "            break;\n",
             "        }\n")),
];
