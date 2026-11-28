// From build-oneios-display.py (STAGED), written out by a helper of the port and kept by hand since.
/// The probe's lines that change. Each anchor must be there exactly once.
pub const DISPLAY: &[(&str, &str)] = &[
    ("#include \"DroidVMVirtioGpu.h\"\n",
     concat!("#include \"DroidVMVirtioGpu.h\"\n",
             "extern \"C\" bool oneios_display_parent_bound(void);\n",
             "extern \"C\" void oneios_display_parent_slide(void);\n",
             "extern \"C\" void oneios_display_client_seen(IOReturn,int,uint32_t,IOUserClient *);\n",
             "extern \"C\" void oneios_display_frame_seen(unsigned,uint64_t,uint64_t);\n",
             "extern \"C\" void oneios_display_swap_seen(bool,unsigned,const void *);\n",
             "extern \"C\" bool oneios_display_wide(void);\n",
             "extern \"C\" bool oneios_virtio_gpu_offer_format(void *,IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t,uint32_t);\n",
             "extern \"C\" void oneios_display_frame_done(void);\n")),
    // the bindings are for one kernel collection: nothing is registered under another class
    ("    native_legacy_stage=1;void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);\n",
     concat!("    native_legacy_stage=1;if(!oneios_display_parent_bound()){native_legacy_display_checkpoint();return;}\n",
             "    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);\n")),
    // the superclass link is the parent's link address until it is slid (OneiOSDisplay.cpp)
    ("native_legacy_display_static_init();native_gop_presenter_static_init();native_legacy_meta_status=OSMetaClass::postModLoad(handle);",
     "native_legacy_display_static_init();native_gop_presenter_static_init();oneios_display_parent_slide();native_legacy_meta_status=OSMetaClass::postModLoad(handle);"),
    // the log: who opened the framebuffer, the first swaps (and each surface format the compositor swaps), the first frames
    ("    native_legacy_client_status=status;native_legacy_client_object=(uintptr_t)client;\n",
     concat!("    native_legacy_client_status=status;native_legacy_client_object=(uintptr_t)client;\n",
             "    oneios_display_client_seen(status,native_legacy_client_pid,native_legacy_client_type,client);\n")),
    ("    if(kind<2)droidvm_virtio_gpu_boot_dirty();\n",
     concat!("    if(kind<2)droidvm_virtio_gpu_boot_dirty();\n",
             "    oneios_display_frame_seen(kind,maxx,maxy);\n")),
    ("    if(accepted)native_legacy_swap_stage(0);\n",
     concat!("    if(accepted)native_legacy_swap_stage(0);\n",
             "    oneios_display_swap_seen(accepted,reason,surface);\n")),
    // a half-float frame (NGP_RGBA16F_LINEAR, iOS's 'RGhA') goes to the virtio scanout as it is when xnuhost keeps half
    // floats (oneios-display-wide=1, display-format=rgba16f), instead of the CPU's conversion to 8 bits
    ("    offeredToVirtioDisplay=format==1 && droidvm_virtio_gpu_offer(display,memory,stride,width,height);\n",
     concat!("    offeredToVirtioDisplay=(format==1 || (format==2 && oneios_display_wide())) &&\n",
             "        oneios_virtio_gpu_offer_format(display,memory,stride,width,height,format);\n")),
    // every frame written into the boot video (a copy, a blank) counts in the page a VM's host polls (OneiOSDisplay.cpp
    // oneios_display_frame_done: only with boot argument oneios-frame-seq, which xnuloader-crosvm --display gives)
    ("    native_legacy_transfer_status=status;native_legacy_transfer_request=(uintptr_t)request;native_legacy_transfer_bytes=bytes;\n",
     concat!("    native_legacy_transfer_status=status;native_legacy_transfer_request=(uintptr_t)request;native_legacy_transfer_bytes=bytes;\n",
             "    if(status==kIOReturnSuccess)oneios_display_frame_done();\n")),
];

/// The virtio scanout client's lines that change (DroidVMVirtioGpuDisplay.cpp): a resource and a frame carry their
/// scanout format, so that a half-float surface is created and scanned out as xnuhost's 'RGhA'
/// (runtime/xnuhost/include/xnuhost/pixels.h, XH_VIRTIO_GPU_FORMAT_RGBA16F) with 8 bytes a pixel, and every other one
/// as before (B8G8R8X8, format 2). Each anchor must be there exactly once.
pub const SCANOUT: &[(&str, &str)] = &[
    ("    uint32_t width, height, id;\n    bool prepared, usable;\n};\n",
     concat!("    uint32_t width, height, id;\n    bool prepared, usable;\n",
             "    uint32_t format;   // OneiOS: 0 B,G,R,X; OneiOSHalf: half floats (oneios-display-wide=1)\n",
             "};\n")),
    ("struct Frame { IOService *display; IOMemoryDescriptor *memory; uint64_t stride; uint32_t width, height; uint64_t work, holds; int pid; };\n",
     concat!("struct Frame { IOService *display; IOMemoryDescriptor *memory; uint64_t stride; uint32_t width, height; uint64_t work, holds; int pid;\n",
             "               uint32_t format; };\n",
             "// OneiOS: xnuhost's scanout format for half floats, IOSurface's 'RGhA' (virtio-gpu has none of its own).\n",
             "constexpr uint32_t OneiOSHalf = 0x52476841;\n",
             "uint32_t scanoutFormat(const Resource &r) { return r.format == OneiOSHalf ? OneiOSHalf : 2; }\n",
             "uint64_t scanoutBpp(const Resource &r) { return r.format == OneiOSHalf ? 8 : 4; }\n")),
    ("r.id, 2, uint32_t(stride / 4), height};\n",
     "r.id, scanoutFormat(r), uint32_t(stride / scanoutBpp(r)), height};\n"),
    (concat!("r.width, r.height, 2, 0, {uint32_t(r.stride)}, {}};\n",
             "            status = DroidVMVirtioGpu::post(&scanout,"),
     concat!("r.width, r.height, scanoutFormat(r), 0, {uint32_t(r.stride)}, {}};\n",
             "            status = DroidVMVirtioGpu::post(&scanout,")),
    (concat!("r.width, r.height, 2, 0, {uint32_t(r.stride)}, {}};\n",
             "            status = send(scanout);\n"),
     concat!("r.width, r.height, scanoutFormat(r), 0, {uint32_t(r.stride)}, {}};\n",
             "            status = send(scanout);\n")),
    ("        r.width == frame.width && r.height == frame.height) { r.used = ++useClock; return &r; }\n",
     "        r.width == frame.width && r.height == frame.height && r.format == frame.format) { r.used = ++useClock; return &r; }\n"),
    ("    IOReturn status = create(*slot, frame.memory, frame.stride, frame.width, frame.height, true);\n",
     concat!("    slot->format = frame.format;\n",
             "    IOReturn status = create(*slot, frame.memory, frame.stride, frame.width, frame.height, true);\n")),
    ("extern \"C\" bool droidvm_virtio_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height) {\n",
     concat!("static bool oneiosOffer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height, uint32_t format);\n",
             "extern \"C\" bool droidvm_virtio_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height) {\n",
             "    return oneiosOffer(display, memory, stride, width, height, 1);\n",
             "}\n",
             "// OneiOS: the offer with the frame's NGP format (1 B,G,R,X; 2 half floats of linear light, LegacyDisplayProbe.cpp).\n",
             "extern \"C\" bool oneios_virtio_gpu_offer_format(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t, uint32_t);\n",
             "extern \"C\" bool oneios_virtio_gpu_offer_format(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width,\n",
             "                                                uint32_t height, uint32_t format) {\n",
             "    return (format == 1 || format == 2) && oneiosOffer(display, memory, stride, width, height, format);\n",
             "}\n",
             "static bool oneiosOffer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height, uint32_t format) {\n")),
    ("        pending = {service, memory, stride, width, height, droidvm_virtgpu_submitted(), droidvm_virtgpu_holds(), pid};\n",
     concat!("        pending = {service, memory, stride, width, height, droidvm_virtgpu_submitted(), droidvm_virtgpu_holds(), pid,\n",
             "                   format == 2 ? OneiOSHalf : 0};\n")),
    // take17 (2026-11-26): every gap between two swaps goes to OneiOSAdreno.cpp as the later swap is offered (hitch::): a
    // freeze of a game is a gap of hundreds of ms in the middle of frames, which the 10 s histogram only counts ("pauses").
    // Weak: without OneiOSAdreno.cpp nothing changes.
    ("volatile uint64_t droidvm_virtio_offer_gap_hist[41], droidvm_virtio_offer_gaps[2], droidvm_virtio_offer_refused;\n",
     concat!("volatile uint64_t droidvm_virtio_offer_gap_hist[41], droidvm_virtio_offer_gaps[2], droidvm_virtio_offer_refused;\n",
             "// OneiOS (take17): each gap between two swaps (us) and the time the later one was offered (us), for OneiOSAdreno.cpp's\n",
             "// oneios-hitch lines. Weak: without it nothing changes.\n",
             "void oneios_gpu_swap_gap(uint64_t, uint64_t) __attribute__((weak));\n")),
    ("        if (gap < 50000) { droidvm_virtio_offer_gaps[0] = droidvm_virtio_offer_gaps[0] + 1; droidvm_virtio_offer_gaps[1] = droidvm_virtio_offer_gaps[1] + gap; }\n",
     concat!("        if (gap < 50000) { droidvm_virtio_offer_gaps[0] = droidvm_virtio_offer_gaps[0] + 1; droidvm_virtio_offer_gaps[1] = droidvm_virtio_offer_gaps[1] + gap; }\n",
             "        if (oneios_gpu_swap_gap) oneios_gpu_swap_gap(gap, now);   // OneiOS (take17): two words stored, nothing printed here\n")),
];

/// UniXNU's NativeLegacyDisplay.cpp: its GOP path maps the boot video write-back in the native kernel. In a VM of
/// Android's crosvm the boot video lies in the restricted DMA pool, which the host reads through a write-back mapping of
/// crosvm's memfd: a non-cacheable one in the guest would leave the host reading stale lines (docs/native.md, "A VM under
/// Gunyah", the display). The hosted kernel and the bench keep kIOMapInhibitCache.
pub const LEGACY_BOOT_VIDEO: &[(&str, &str)] = &[(
    "        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);\n",
    concat!(
        "#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
        "        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapCopybackCache); // OneiOS native: crosvm's pool\n",
        "#else\n",
        "        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);\n",
        "#endif\n"
    ),
)];

/// UniXNU's NativeGOPPresenter.cpp: the same for the presenter's own mapping of the boot video.
pub const GOP_BOOT_VIDEO: &[(&str, &str)] = &[(
    "    mapping=(memorySource?memorySource:provider)->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);\n",
    concat!(
        "#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
        "    mapping=(memorySource?memorySource:provider)->mapDeviceMemoryWithIndex(0,kIOMapCopybackCache); // OneiOS native: crosvm's pool\n",
        "#else\n",
        "    mapping=(memorySource?memorySource:provider)->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);\n",
        "#endif\n"
    ),
)];

/// The board probe's lines that change in the native kernel (LegacyDisplayProbe.cpp, after DISPLAY): crosvm's virtio-gpu is
/// the 3D device only there, and its scanout has no display on the phone, so the probe does not start the scanout client;
/// every frame stays in the boot video, which the phone's bridge shows (displayctl pool). `oneios-virtio-scanout=1` starts it
/// anyway. The hosted kernel starts it as before.
pub const NATIVE_PROBE: &[(&str, &str)] = &[(
    "void native_legacy_display_late_probe(void) {\n    droidvm_virtio_gpu_display_start();\n",
    concat!(
        "void native_legacy_display_late_probe(void) {\n",
        "#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
        "    {unsigned scanout=0;PE_parse_boot_argn(\"oneios-virtio-scanout\",&scanout,sizeof(scanout));if(scanout)droidvm_virtio_gpu_display_start();}\n",
        "#else\n",
        "    droidvm_virtio_gpu_display_start();\n",
        "#endif\n"
    ),
)];

/// UniXNU's NativeLegacyDisplay.cpp, after LEGACY_BOOT_VIDEO (native-perf, 2026-11-26; both kernels): the refresh tick asks
/// OneiOSDisplay.cpp where the next one goes. The timer was last + 1/120 s of the guest's counter whatever the panel did;
/// with the phone publishing the panel's vsync (`vm-boot --vsync-lock`, runtime/pace) the next tick is put on the panel's
/// grid, the compositor is told the panel's period, and a tick that fired within a quarter period of its place carries the
/// place's time, as a vblank's timestamp does. `oneios_display_vsync_next` is weak and answers the free-running time when
/// nothing is published (display/vsync_lock.h): without it, or without a publisher, the tick is what it was. Each anchor
/// must be there exactly once.
pub const LEGACY_VSYNC: &[(&str, &str)] = &[
    ("extern \"C\" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);\n",
     concat!("extern \"C\" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);\n",
             "// OneiOS: the tick after this one (OneiOSDisplay.cpp, vsync_lock.h): its time, the period the compositor is told and the\n",
             "// time this tick is given. Weak: without it the tick is the free-running one (staged/display.rs LEGACY_VSYNC).\n",
             "extern \"C\" uint64_t oneios_display_vsync_next(uint64_t now,uint64_t scheduled,uint64_t nextFree,uint64_t nominal,uint64_t *interval,\n",
             "    uint64_t *stamp) __attribute__((weak));\n")),
    ("        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);\n",
     concat!("        uint64_t oneiosNext=vsyncNext+vsyncInterval,oneiosInterval=vsyncInterval,oneiosStamp=now;   // OneiOS: LEGACY_VSYNC\n",
             "        const bool oneiosLate=oneiosNext<=now;\n",
             "        if(oneiosLate)oneiosNext=now+vsyncInterval;\n",
             "        if(oneios_display_vsync_next)oneiosNext=oneios_display_vsync_next(now,vsyncNext,oneiosNext,vsyncInterval,&oneiosInterval,&oneiosStamp);\n",
             "        native_legacy_vsync_notify(this,oneiosStamp,mach_continuous_time()-(now-oneiosStamp),oneiosInterval);\n")),
    (concat!("        vsyncNext+=vsyncInterval;\n",
             "        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}\n"),
     concat!("        vsyncNext=oneiosNext;\n",
             "        if(oneiosLate)native_legacy_vsync_observed(3,vsyncHz);\n")),
];
