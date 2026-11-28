//! The generators against the frozen output of the Python scripts they replaced (tests/kernelgen/golden), for
//! everything that needs no kernel build tree:
//!
//!   - the bindings and the header that come from a record in the repository alone (aes, gpu, the display's header),
//!     against the files of the Python build (golden/build);
//!   - every edit list on a made-up source (golden/synthetic/NAME.in), against what the Python script's own staging
//!     function wrote for it (NAME.out);
//!   - drivers/build.py's compile command for a made-up record;
//!   - build-record.py's record of a made-up build.
//!
//! No file of another project and none of Apple's is read. What needs compiled objects (the vtable checks, hid's,
//! net's and the display's bindings) is compared in a build tree: tests/kernelgen/compare-golden.sh.

use oneios_kernel::{build_oneios_aes, build_oneios_audio, build_oneios_display, build_oneios_gpu, build_oneios_hid, build_oneios_keystore,
                    build_oneios_net, build_record, drivers_build};
use std::path::{Path, PathBuf};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../..").canonicalize().expect("the repository")
}

fn golden(name: &str) -> String {
    let path = repo().join("tools/oneios-tool/tests/kernelgen/golden").join(name);
    std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn hosted(name: &str) -> PathBuf {
    repo().join("kernel/patches/oneios-hosted").join(name)
}

#[test]
fn the_aes_bindings_are_the_python_builds() {
    let (stubs, targets) = build_oneios_aes::bindings(&hosted("aes/IOAESAccelerator-24A446-v54.json")).unwrap();
    assert_eq!(stubs, golden("build/oneios-aes/IOAESAccelerator-bindings.S"));
    assert_eq!(targets, golden("build/oneios-aes/IOAESAccelerator-originals.S"));
}

#[test]
fn the_gpu_bindings_are_the_python_builds() {
    let (stubs, targets, bound) = build_oneios_gpu::bindings(&hosted("gpu/IOAcceleratorES-24A446-v54.json")).unwrap();
    assert_eq!(stubs, golden("build/oneios-gpu/adreno/accelerator/IOAcceleratorES-bindings.S"));
    assert_eq!(targets, golden("build/oneios-gpu/adreno/accelerator/IOAcceleratorES-originals.S"));
    assert_eq!(bound.len(), 3);
}

#[test]
fn the_displays_header_is_the_python_builds() {
    let abi = build_oneios_display::Abi::load(&hosted("display/legacy-display-24A446-v54.json")).unwrap();
    assert_eq!(build_oneios_display::header(&abi).unwrap(), golden("build/native-legacy-display/OriginalLegacyFramebufferABI.h"));
}

#[test]
fn the_discriminator_is_clangs() {
    // ptrauth_string_discriminator("isa"): the constant objc4 writes out (ISA_SIGNING_DISCRIMINATOR). The display's
    // header() checks every reviewed slot name of its record against the slot's diversity with the same function.
    assert_eq!(build_oneios_display::discriminator("isa"), 0x6ae1);
}

#[test]
fn the_probe_sources_are_the_python_builds() {
    assert_eq!(build_oneios_hid::PROBE, golden("build/oneios-hid/OneiOSHidLayoutCheck.cpp"));
    assert_eq!(build_oneios_net::PROBE, golden("build/oneios-net/OneiOSNetCheck.cpp"));
    assert_eq!(build_oneios_net::CRUMBS, golden("build/oneios-net/ionetworking-include/oneios-mbuf-crumbs.h"));
}

#[test]
fn the_edit_lists_write_what_the_python_functions_wrote() {
    let made_up = |name: &str| golden(&format!("synthetic/{name}.in"));
    // ONEIOS_GOLDEN_UPDATE=1 (take17): an edit list of ours changed (a new anchor is first added to synthetic/NAME.in by
    // hand): what the lists write now is recorded as NAME.out, which the comparison below then reads back. For the lists
    // that have no Python original any more to compare with; say why in the same change.
    if std::env::var_os("ONEIOS_GOLDEN_UPDATE").is_some_and(|v| v == "1") {
        let write = |name: &str, text: String| {
            let path = repo().join("tools/oneios-tool/tests/kernelgen/golden/synthetic").join(format!("{name}.out"));
            std::fs::write(&path, text).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        };
        write("audio", build_oneios_audio::staged_text(made_up("audio")).unwrap());
        write("gpu", build_oneios_gpu::staged_text(made_up("gpu")).unwrap());
        write("gpu3d", build_oneios_gpu::staged_3d_text(made_up("gpu3d")).unwrap());
        write("display", build_oneios_display::staged_text(made_up("display")).unwrap());
        write("display_scanout", build_oneios_display::staged_scanout(made_up("display_scanout")).unwrap());
    }
    let expected = |name: &str| golden(&format!("synthetic/{name}.out"));
    assert_eq!(build_oneios_aes::staged_text(made_up("aes"), "// made-up keys\n").unwrap(), expected("aes"));
    assert_eq!(build_oneios_keystore::staged_text(made_up("keystore")).unwrap(), expected("keystore"));
    assert_eq!(build_oneios_audio::staged_text(made_up("audio")).unwrap(), expected("audio"));
    assert_eq!(build_oneios_hid::staged_hid(made_up("hid")).unwrap(), expected("hid"));
    assert_eq!(build_oneios_hid::staged_transport(made_up("hid_transport"), "// made-up side keys\n", "// made-up touch time\n").unwrap(),
               expected("hid_transport"));
    assert_eq!(build_oneios_net::staged_text(made_up("net_transport")).unwrap(), expected("net_transport"));
    assert_eq!(build_oneios_gpu::staged_text(made_up("gpu")).unwrap(), expected("gpu"));
    assert_eq!(build_oneios_gpu::staged_3d_text(made_up("gpu3d")).unwrap(), expected("gpu3d"));   // no Python original
    assert!(build_oneios_gpu::staged_3d_text(made_up("gpu3d").repeat(2)).is_err());
    assert_eq!(build_oneios_display::staged_text(made_up("display")).unwrap(), expected("display"));
    assert_eq!(build_oneios_display::staged_scanout(made_up("display_scanout")).unwrap(), expected("display_scanout"));
}

#[test]
fn the_display_edits_fit_the_pinned_sources() {
    // The edit lists against the real upstream files, where external/ has them (a checkout without the references
    // skips this; the kernel build applies the same lists and stops on a missing anchor).
    let package = repo().join("external/UniXNU-droidvm/package");
    let (probe, scanout) = (package.join("LegacyDisplayProbe.cpp"), package.join("DroidVMVirtioGpuDisplay.cpp"));
    if !probe.exists() || !scanout.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the display's edits were checked on the synthetic sources only");
        return;
    }
    let staged = build_oneios_display::staged_text(std::fs::read_to_string(&probe).unwrap()).unwrap();
    assert!(staged.contains("oneios_virtio_gpu_offer_format(display,memory,stride,width,height,format);"));
    let staged = build_oneios_display::staged_scanout(std::fs::read_to_string(&scanout).unwrap()).unwrap();
    assert_eq!(staged.matches("scanoutFormat(r)").count(), 3);                  // CREATE_2D and the two SET_SCANOUT_BLOBs
    assert!(!staged.contains("r.width, r.height, 2, 0,") && staged.contains("format == 2 ? OneiOSHalf : 0};"));
}

#[test]
fn the_boot_video_edits_fit_the_pinned_sources() {
    // native, 2026-11-23: the boot video write-back in the native kernel (staged/display.rs LEGACY_BOOT_VIDEO,
    // GOP_BOOT_VIDEO): in crosvm's restricted DMA pool the host reads it write-back. The hosted kernel keeps the old.
    let display = repo().join("external/NativeMobileDisplay");
    let (legacy, gop) = (display.join("NativeLegacyDisplay.cpp"), display.join("NativeGOPPresenter.cpp"));
    if !legacy.exists() || !gop.exists() {
        eprintln!("external/NativeMobileDisplay is not here: the boot video edits were not checked");
        return;
    }
    for staged in [
        build_oneios_display::staged_legacy(std::fs::read_to_string(&legacy).unwrap()).unwrap(),
        build_oneios_display::staged_gop(std::fs::read_to_string(&gop).unwrap()).unwrap(),
    ] {
        // the native block of the boot video's mapping (NativeLegacyDisplay.cpp has another: the CPU copy's wait for the GPU)
        let block = staged
            .split("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n")
            .skip(1)
            .map(|rest| &rest[..rest.find("#endif\n").unwrap()])
            .find(|block| block.contains("kIOMapCopybackCache"))
            .unwrap();
        assert!(block.contains("#else\n") && block.contains("kIOMapInhibitCache"));
        assert_eq!(staged.matches("kIOMapCopybackCache); // OneiOS native: crosvm's pool").count(), 1);
    }
}

#[test]
fn the_transport_edits_fit_the_pinned_source() {
    // native, 2026-11-20: the virtio-gpu transport's control queue as long as the device takes (staged/gpu.rs TRANSPORT),
    // against the real DroidVMVirtioGpu.cpp where external/ has it; the kernel build applies the same list and stops on a
    // missing anchor.
    let source = repo().join("external/UniXNU-droidvm/package/DroidVMVirtioGpu.cpp");
    if !source.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the transport's edits were not checked");
        return;
    }
    let text = std::fs::read_to_string(&source).unwrap();
    let staged = build_oneios_gpu::staged_transport_text(text.clone()).unwrap();
    assert!(staged.contains("uint32_t queueSize = QueueSize, usable = Slots, largeFrom = Large;"));
    assert!(staged.contains("queueSize = size = max;") && staged.contains("wr(0x38, size);"));
    assert!(staged.contains("i < usable && s == Slots") && staged.contains("available % queueSize"));
    // every ring index and descriptor check follows the device's length
    assert!(!staged.contains("% QueueSize") && !staged.contains("head < QueueSize"));
    assert!(build_oneios_gpu::staged_transport_text(text.repeat(2)).is_err());
}

#[test]
fn the_pen_edits_fit_the_pinned_source() {
    // spen, 2026-11-15: the pen digitizer's edits (staged/hid_pen.rs) apply after the phone's (staged/hid.rs) to the real
    // NativeHidInput.cpp, where external/ has it; the kernel build applies the same lists and stops on a missing anchor.
    let source = repo().join("external/NativeHidInput/NativeHidInput.cpp");
    if !source.exists() {
        eprintln!("external/NativeHidInput is not here: the pen's edits were not checked");
        return;
    }
    let staged = build_oneios_hid::staged_hid_pen(build_oneios_hid::staged_hid(std::fs::read_to_string(&source).unwrap()).unwrap()).unwrap();
    // Report 4 in a Digitizer / Stylus application collection; the sink set and cleared only for the pen device.
    assert!(staged.contains("static const unsigned char penDescriptor[]={\n    0x05,0x0d,0x09,0x20,0xa1,0x01, 0x85,0x04,"));
    assert_eq!(staged.matches("native_pen_set_sink(").count(), 3);               // the declaration, start, stop
    assert!(staged.contains("if(penOnly)native_pen_set_sink(&NativeHidInput::penNotify,this);"));
    // backboardd's pencil digitizer: SPI, Built-In, the hover range and the maximum pressure on the device.
    assert!(staged.contains("touchOnly||penOnly?\"SPI\":\"Virtual\""));
    assert!(staged.contains("setProperty(\"MaxHoverHeight\",1ULL,32);setProperty(\"AccurateMaxDigitizerPressureValue\",500ULL,32);"));
    // The stylus event is completed by the wrapper (setFixedValue, slot 26) and is not marked as a finger.
    assert!(staged.contains("enum : uint16_t {kDivSetFixed=0x6e23};"));
    assert!(staged.contains("==kStylus) {orient(child);return false;}"));
    assert!(staged.contains("(touchEnabled || touchOnly || penOnly)){wrappedDriver=driver;edge::install(driver);}"));
    // The report: 11 bytes, bit 0 Tip, 1 Barrel, 2 In Range, 3 Eraser from the sink's flags (1 range, 2 tip, 4 barrel, 8 eraser).
    assert!(staged.contains("unsigned char report[11]={4,bits,"));
    assert!(staged.contains("((flags&2)?1:0)|((flags&4)?2:0)|((flags&1)?4:0)|((flags&8)?8:0)"));
    assert!(staged.ends_with("if(device)device->setPenOnly();return device;}\n"));
    // Applied twice, the anchors are gone: the generator stops.
    assert!(build_oneios_hid::staged_hid_pen(staged).is_err());
}

#[test]
fn the_contact_identifier_is_a_free_one_of_1_to_28_and_the_pointer_gets_29() {
    // hid, 2026-11-12 (a6346c1): backboardd drops a touch whose identifier is 30 or more (MAX_PATH_COUNT)
    let staged = golden("synthetic/hid_transport.out");
    assert!(staged.contains("uint8_t id = 29;\n"));
    assert!(staged.contains("oneios_next_id = c >= 28 ? 1 : (uint8_t)(c + 1);\n"));
    assert!(!staged.contains("% 120"));
}

#[test]
fn an_anchor_that_is_gone_stops_the_generator() {
    let error = build_oneios_keystore::staged_text("namespace {\n".to_string()).unwrap_err().to_string();
    assert_eq!(error, "build-oneios-keystore: NativeKeyStoreEvents.cpp no longer has exactly one default: break;");
    let twice = golden("synthetic/net_transport.in").repeat(2);
    assert!(build_oneios_net::staged_text(twice).unwrap_err().to_string().starts_with("build-oneios-net: DroidVMVirtioNet.c no longer has exactly one 'if ("));
}

#[test]
fn the_compile_command_is_drivers_build_pys() {
    let record = drivers_build::record(&repo().join("tools/oneios-tool/tests/kernelgen/golden/synthetic"), "command.record.json").unwrap();
    for kind in ["c", "cpp"] {
        let command = drivers_build::command(Path::new("/REPO/drivers"), &record, Path::new(&format!("/s/b.{kind}")), Path::new("/o/b.o"));
        let list = format!("[{}]\n", command.iter().map(|part| format!("\"{part}\"")).collect::<Vec<_>>().join(", "));
        assert_eq!(list, golden(&format!("synthetic/command.{kind}.json")));
    }
}

/// The made-up build of the frozen record: a checkout with one patch and one UniXNU patch, a work directory with a
/// made-up kernel, its inputs, its configuration, UniXNU's record and two archives' listings.
fn made_up_build(root: &Path) -> PathBuf {
    let write = |path: PathBuf, data: &[u8]| {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, data).unwrap();
    };
    write(root.join("kernel/patches/unixnu/0001-made-up.patch"), b"a made-up UniXNU patch\n");
    write(root.join("kernel/patches/a.patch"), b"x");
    let work = root.join("out/kernel/unixnu/oneios/ios-test/oneios_test");
    write(work.join("kernel/kernel.macho"), &build_record::fake_kernel());
    write(work.join("kernel/kernel.json"), br#"{"kernelcache_kexts_added": false, "prelink_skipped": true}"#);
    let inputs = format!(r#"{{"commit": "{}", "patches": [{{"path": "{}", "sha256": "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881"}}]}}"#,
                         "c".repeat(40), root.join("kernel/patches/a.patch").display());
    write(work.join("xnu-inputs.json"), inputs.as_bytes());
    write(work.join(".config"), b"CONFIG_XNU_ARMCPU=y\n# a comment\nCONFIG__XNU_VERSION=\"12377.121.6\"\nCONFIG_NAME=\"a b\"\nCONFIG_XNU_SOURCE_CORECRYPTO14=y\n");
    write(work.join("tree/build/oneios-board/objects.json"), br#"{"sources": [{"path": "external/x.c", "universe": "mach", "sha256": "ab"}]}"#);
    write(work.join("tree/build/oneios-drivers/objects.json"),
          br#"{"records": "r", "sources": [{"path": "drivers/a.c", "universe": "mach", "sha256": "cd"}]}"#);
    work
}

#[test]
fn the_build_record_is_build_record_pys() {
    use oneios_common::pyjson::{self, Value};
    let root = std::env::temp_dir().join(format!("oneios-kernel-golden-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).unwrap();
    let root = root.canonicalize().unwrap();
    let work = made_up_build(&root);
    let out = root.join("out/kernel");
    build_record::finish(&root, "linux", &work, "oneios_test_defconfig", &out).unwrap();
    let target = out.join("current").canonicalize().unwrap();
    // the machine's part (the compiler's and the linker's versions) and the checkouts' commits are not frozen
    let record = pyjson::load(target.join("xnu.macho.build.json")).unwrap();
    let Value::Dict(items) = record else { panic!("the record is no object") };
    let kept = Value::Dict(items.into_iter().filter(|(key, _)| key != "host" && key != "sources").collect());
    assert_eq!(pyjson::dumps(&kept, Some(2), false) + "\n", golden("synthetic/build-record.json"));
    assert_eq!(std::fs::read_to_string(target.join("series.txt")).unwrap(), golden("synthetic/build-record.series.txt"));
    assert_eq!(std::fs::read(target.join("xnu.macho")).unwrap(), build_record::fake_kernel());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_input_pci_edits_fit_the_pinned_source() {
    // native, 2026-11-23: the touch transport on crosvm's PCI with its pages in the pool (staged/hid_transport.rs
    // HID_TRANSPORT_PCI), after the hosted edits, against the real DroidVMVirtioInput.c where external/ has it.
    let source = repo().join("external/UniXNU-droidvm/package/DroidVMVirtioInput.c");
    if !source.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the input transport's PCI edits were not checked");
        return;
    }
    let text = std::fs::read_to_string(&source).unwrap();
    let hosted = build_oneios_hid::staged_transport(text.clone(), "// made-up side keys\n", "// made-up touch time\n").unwrap();
    let staged = build_oneios_hid::staged_transport_pci(hosted.clone()).unwrap();
    // the MMIO accessors renamed, the dispatchers in their place, each PCI path inside the native switch
    for name in ["mmio_read32", "mmio_read8", "mmio_write32", "mmio_write8"] {
        assert_eq!(staged.matches(&format!(" {name}(const struct droidvm_input *d, unsigned offset")).count(), 1, "{name}");
    }
    assert_eq!(staged.matches("static uint32_t input_read32(const struct droidvm_input *d, unsigned offset) {\n").count(), 1);
    let start = staged.find("static void oneios_input_pci_start(const uint64_t ranges[3]) {").unwrap();
    let call = staged.find("    oneios_input_pci_start(ranges);").unwrap();
    let started = staged.find("    droidvm_input_started = droidvm_input_count;").unwrap();
    assert!(start < call && call < started);
    for block in staged.split("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n").skip(1) {
        let inside = &block[..block.find("#endif\n").unwrap()];
        assert!(!inside.contains("#if "), "a native block holds another #if");
    }
    // outside the native blocks the hosted transport is what it was
    let mut outside = String::new();
    for (n, part) in staged.split("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n").enumerate() {
        outside += if n == 0 { part } else { &part[part.find("#endif\n").unwrap() + 7..] };
    }
    assert!(outside.contains("static uint64_t input_physical(const void *address) {\n    return kvtophys((vm_offset_t)address);\n}\n"));
    assert!(staged.contains("value |= pci_rd32(d->common + 0x04) & 2;"));                     // VIRTIO_F_ACCESS_PLATFORM
    assert!(staged.contains("pci_rd32(config) != (0x1af4u | 0x1052u << 16)"));
    // the pages are the window's before the device starts (crosvm may touch nothing else)
    let pages = staged.find("        droidvm_input_pages[d->index] = NULL;").unwrap();
    assert!(pages < staged[pages..].find("const char *why = input_start(d);").unwrap() + pages);
    assert!(build_oneios_hid::staged_transport_pci(hosted.repeat(2)).is_err());
}

#[test]
fn the_gpu_pci_edits_fit_the_pinned_source() {
    // native, 2026-11-24: the virtio-gpu transport on crosvm's PCI (staged/gpu.rs TRANSPORT_PCI), after the hosted
    // edits, against the real DroidVMVirtioGpu.cpp where external/ has it.
    let source = repo().join("external/UniXNU-droidvm/package/DroidVMVirtioGpu.cpp");
    if !source.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the GPU transport's PCI edits were not checked");
        return;
    }
    let text = std::fs::read_to_string(&source).unwrap();
    let hosted = build_oneios_gpu::staged_transport_text(text.clone()).unwrap();
    let staged = build_oneios_gpu::staged_transport_pci_text(hosted.clone()).unwrap();
    // the MMIO accessors renamed, the dispatchers in their place
    assert_eq!(staged.matches("uint32_t mmioRd(unsigned offset) {\n").count(), 1);
    assert_eq!(staged.matches("void mmioWr(unsigned offset, uint32_t v) {\n").count(), 1);
    assert!(staged.contains("uint32_t rd(unsigned offset) {\n#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n    if (pci) return pciRd(offset);\n#endif\n    return mmioRd(offset);\n}\n"));
    assert!(staged.contains("void wr(unsigned offset, uint32_t v) {\n#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n    if (pci) { pciWr(offset, v); return; }\n#endif\n    mmioWr(offset, v);\n}\n"));
    // the PCI start: the node's compatible, the function's identity, its regions; the MMIO start kept on both sides
    assert!(staged.contains("!memcmp(value, \"oneios,virtio-gpu-pci\", 22)"));
    assert!(staged.contains("pciRd32(config) != (0x1af4u | 0x1050u << 16)"));
    assert_eq!(staged.matches("registers = ml_io_map(reg[0] - ranges[0] + ranges[1], 0x1000);").count(), 2);
    // every native block is flat, and the accessors' block comes before the start that uses it
    for block in staged.split("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n").skip(1) {
        let inside = &block[..block.find("#endif\n").unwrap()];
        assert!(!inside.contains("#if "), "a native block holds another #if");
    }
    assert!(staged.find("bool pciRegions(uintptr_t config) {").unwrap() < staged.find("bool start() {").unwrap());
    // the notification goes to the selected queue's offset times the multiplier, the ISR read acknowledges
    assert!(staged.contains("pciWr16(pciNotify + uintptr_t(pciNotifyOffset[value]) * pciNotifyMultiplier, uint16_t(value));"));
    assert!(staged.contains("case 0x60: return pciRd8(pciIsr) & 3;"));
    assert!(build_oneios_gpu::staged_transport_pci_text(staged.clone()).is_err());
    assert!(build_oneios_gpu::staged_transport_pci_text(text).is_ok(), "the PCI edits do not depend on the hosted ones");
}

#[test]
fn the_native_probe_leaves_the_virtio_scanout_alone() {
    // native, 2026-11-24: the board probe does not start crosvm's scanout in the native kernel (staged/display.rs
    // NATIVE_PROBE), after DISPLAY, against the real LegacyDisplayProbe.cpp where external/ has it.
    let probe = repo().join("external/UniXNU-droidvm/package/LegacyDisplayProbe.cpp");
    if !probe.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the native probe's edit was not checked");
        return;
    }
    let staged = build_oneios_display::staged_text(std::fs::read_to_string(&probe).unwrap()).unwrap();
    let native = build_oneios_display::staged_native_probe(staged).unwrap();
    let at = native.find("void native_legacy_display_late_probe(void) {\n").unwrap();
    let body = &native[at..at + 400];
    assert!(body.contains("PE_parse_boot_argn(\"oneios-virtio-scanout\",&scanout,sizeof(scanout));if(scanout)droidvm_virtio_gpu_display_start();"));
    assert!(body.contains("#else\n    droidvm_virtio_gpu_display_start();\n#endif\n"));
    assert_eq!(native.matches("droidvm_virtio_gpu_display_start();").count(), 2);
    assert!(build_oneios_display::staged_native_probe(native.clone()).is_err());
}

#[test]
fn the_native_3d_edit_logs_refusals_in_the_pinned_source() {
    // native, 2026-11-24: the refused user calls of the 3D driver in the kernel's log (staged/gpu.rs GPU3D_NATIVE), after GPU3D.
    let source = repo().join("external/UniXNU-droidvm/package/DroidVMVirtioGpu3D.cpp");
    if !source.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the 3D driver's native edit was not checked");
        return;
    }
    let hosted = build_oneios_gpu::staged_3d_text(std::fs::read_to_string(&source).unwrap()).unwrap();
    let native = build_oneios_gpu::staged_3d_native_text(hosted).unwrap();
    let at = native.find("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n        {\n            static volatile uint32_t refusalsLogged;").unwrap();
    assert!(native[at..at + 500].contains("droidvm_virtgpu_last_refusal[0] = op;"), "the log sits where the refusal is counted");
    // the creating task's hint only outside the native kernel: crosvm's virglrenderer refuses context_init bits above the capset
    assert!(native.contains("#if !(defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE)\n        create.contextInit |= uint32_t(hint[0]) << 8 | uint32_t(hint[1]) << 16;\n#endif\n"));
    // the display's fence counters from the virtio GPU, before the function whose names they use
    let fences = native.find("extern \"C\" bool native_adreno_fences(uint32_t *submitted, uint32_t *retired) {").unwrap();
    assert!(fences < native.find("extern \"C\" bool droidvm_virtgpu_finished_by(uint64_t order, int pid) {\n").unwrap());
    assert!(native.contains("    *retired = uint32_t(oldest - 1);\n"));
    assert!(build_oneios_gpu::staged_3d_native_text(native).is_err());
}

#[test]
fn the_stage_edits_write_their_lines_on_a_made_up_source() {
    // native-perf, 2026-11-26: staged/gpu.rs TRANSPORT_STAGES and GPU3D_STAGES on sources that hold each anchor once, as the
    // other lists' made-up sources do (no file of another project is read).
    use oneios_kernel::staged::gpu::{GPU3D_STAGES, TRANSPORT_STAGES};
    let made_up = |list: &[(&str, &str)]| list.iter().map(|(anchor, _)| format!("// before\n{anchor}// after\n")).collect::<String>();
    let transport = build_oneios_gpu::staged_transport_stages_text(made_up(TRANSPORT_STAGES)).unwrap();
    for (anchor, replacement) in TRANSPORT_STAGES {
        assert_eq!(transport.matches(replacement).count(), 1, "{anchor}");
    }
    // The counter of unfenced commands goes up at the post and down as the device's answer is taken, never below 0.
    assert!(transport.contains("slot.postedAhead = unfenced; if (!slot.fenced) ++unfenced;"));
    assert!(transport.contains("if (!slot.fenced && unfenced) --unfenced;"));
    assert!(transport.contains("bool times(uint32_t ticket, uint64_t *out) {\n"));
    let three_d = build_oneios_gpu::staged_3d_stages_text(made_up(GPU3D_STAGES)).unwrap();
    for (anchor, replacement) in GPU3D_STAGES {
        assert_eq!(three_d.matches(replacement).count(), 1, "{anchor}");
    }
    // The stamps are read before poll() frees the slot, and handed over where "frames" is counted.
    let (read, polled) = (three_d.find("times(c->pending[i].ticket, stamps);").unwrap(), three_d.find("if (!poll(c->pending[i].ticket, &result))").unwrap());
    assert!(read < polled);
    assert!(three_d.contains("if (oneios_gpu_stage_fence) oneios_gpu_stage_fence(stamped, stamps, nowUsLocal());"));
    // Off by default: the stamps (times(), which takes the transport's lock) are asked for only when the kernel says
    // oneios-gpu-stages=1 (the weak oneios_gpu_stage_wanted); with no answer of it, or 0, times() is never called.
    let (wanted, asked) = (three_d.find("oneios_gpu_stage_wanted()").unwrap(), three_d.find("times(c->pending[i].ticket, stamps);").unwrap());
    assert!(wanted < asked && three_d.contains("bool oneios_gpu_stage_wanted(void) __attribute__((weak));"));
    assert!(three_d.contains("oneios_gpu_stage_fence && oneios_gpu_stage_wanted && oneios_gpu_stage_wanted() &&"));
    // An anchor twice, or gone, stops the generator.
    assert!(build_oneios_gpu::staged_transport_stages_text(made_up(TRANSPORT_STAGES).repeat(2)).is_err());
    assert_eq!(build_oneios_gpu::staged_3d_stages_text(String::new()).unwrap_err(),
               "DroidVMVirtioGpu3D.cpp no longer has exactly one volatile uint64_t droidvm_virtgpu_rs_fences[3], droidvm_virtgpu_rs_fence_hist[10];");
}

#[test]
fn the_stage_edits_fit_the_pinned_sources() {
    // The same lists at the end of each file's whole chain, against the real DroidVMVirtioGpu.cpp and DroidVMVirtioGpu3D.cpp
    // where external/ has them; the kernel build applies the same chains (staged_transport_source, staged_3d_source).
    let package = repo().join("external/UniXNU-droidvm/package");
    let (transport, three_d) = (package.join("DroidVMVirtioGpu.cpp"), package.join("DroidVMVirtioGpu3D.cpp"));
    if !transport.exists() || !three_d.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the stage edits were checked on the made-up sources only");
        return;
    }
    let staged = build_oneios_gpu::staged_transport_source(std::fs::read_to_string(&transport).unwrap()).unwrap();
    // times() is the transport's own function (after poll, before postDetached), outside the unnamed namespace.
    let (poll, times, detached) = (staged.find("\nbool poll(uint32_t ticket").unwrap(), staged.find("\nbool times(uint32_t ticket, uint64_t *out) {").unwrap(),
                                   staged.find("\nIOReturn postDetached(").unwrap());
    assert!(poll < times && times < detached);
    // One counter, raised in post() after the slot's own flag is set and lowered in reap() where the answer is stamped.
    assert_eq!(staged.matches("\nuint32_t unfenced;").count(), 1);
    let (fenced, raised, lowered, stamped) = (staged.find("slot.fenced = hdr->flags & FlagFence;").unwrap(), staged.find("if (!slot.fenced) ++unfenced;").unwrap(),
                                              staged.find("if (!slot.fenced && unfenced) --unfenced;").unwrap(), staged.find("slot.doneUs = nowUs();").unwrap());
    assert!(fenced < raised && stamped < lowered);
    assert_eq!(staged.matches("++unfenced").count() + staged.matches("--unfenced").count(), 2);
    assert!(staged.contains("    uint32_t postedAhead;   // OneiOS:"));
    // The earlier lists are still there: the queue's length from the device, the PCI function.
    assert!(staged.contains("queueSize = size = max;") && staged.contains("pciRd8(pciIsr) & 3"));
    let staged = build_oneios_gpu::staged_3d_source(std::fs::read_to_string(&three_d).unwrap()).unwrap();
    assert!(staged.contains("namespace DroidVMVirtioGpu {\nbool times(uint32_t, uint64_t *);"));
    assert_eq!(staged.matches("oneios_gpu_stage_wanted").count(), 3);   // the declaration and the two in the test
    assert!(staged.find("oneios_gpu_stage_wanted()").unwrap() < staged.find("times(c->pending[i].ticket, stamps)").unwrap());
    assert_eq!(staged.matches("oneios_gpu_stage_fence").count(), 4);   // the declaration, the two uses in the test of `stamped`, the call
    // Inside the render server's branch of reapFences, after its histogram.
    let (hist, call, drop) = (staged.find("if (us > droidvm_virtgpu_rs_fences[2]) droidvm_virtgpu_rs_fences[2] = us;").unwrap(),
                              staged.find("oneios_gpu_stage_fence(stamped, stamps, nowUsLocal());").unwrap(),
                              staged.find("c->pending[i] = c->pending[--c->pendingCount];").unwrap());
    assert!(hist < call && call < drop);
    assert!(staged.contains("IOLog(\"virtgpu: %.16s op %u refused"));   // GPU3D_NATIVE before it
}

#[test]
fn the_vsync_edit_asks_for_the_next_tick_and_keeps_the_free_running_one() {
    // native-perf, 2026-11-26: staged/display.rs LEGACY_VSYNC. On a made-up source with each anchor once, then on the real
    // NativeLegacyDisplay.cpp where external/ has it (the kernel build applies staged_legacy_source and stops on a missing anchor).
    use oneios_kernel::staged::display::LEGACY_VSYNC;
    let made_up: String = LEGACY_VSYNC.iter().map(|(anchor, _)| format!("// before\n{anchor}// after\n")).collect();
    let staged = build_oneios_display::staged_legacy_vsync(made_up.clone()).unwrap();
    for (anchor, replacement) in LEGACY_VSYNC {
        assert_eq!(staged.matches(replacement).count(), 1, "{anchor}");
    }
    assert!(build_oneios_display::staged_legacy_vsync(made_up.repeat(2)).is_err());
    let source = repo().join("external/NativeMobileDisplay/NativeLegacyDisplay.cpp");
    if !source.exists() {
        eprintln!("external/NativeMobileDisplay is not here: the vsync edit was checked on the made-up source only");
        return;
    }
    let text = std::fs::read_to_string(&source).unwrap();
    let staged = build_oneios_display::staged_legacy_source(text.clone()).unwrap();
    // The tick: the free-running next time is worked out first, exactly as the original did (last + interval, or now +
    // interval when that is past), and only then offered to the hook, which is weak.
    let tick = &staged[staged.find("    void vsyncTick() {").unwrap()..staged.find("    void startVsync() {").unwrap()];
    let at = |what: &str| tick.find(what).unwrap_or_else(|| panic!("no {what:?} in\n{tick}"));
    assert!(at("uint64_t oneiosNext=vsyncNext+vsyncInterval,oneiosInterval=vsyncInterval,oneiosStamp=now;") < at("if(oneiosLate)oneiosNext=now+vsyncInterval;"));
    assert!(at("if(oneiosLate)oneiosNext=now+vsyncInterval;") < at("if(oneios_display_vsync_next)oneiosNext=oneios_display_vsync_next(now,vsyncNext,oneiosNext,vsyncInterval,&oneiosInterval,&oneiosStamp);"));
    assert!(at("native_legacy_vsync_notify(this,oneiosStamp,mach_continuous_time()-(now-oneiosStamp),oneiosInterval);") < at("vsyncNext=oneiosNext;"));
    assert!(at("vsyncNext=oneiosNext;") < at("vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);"));
    // nothing of the old arithmetic is left beside it, and the late count is kept
    assert!(!tick.contains("vsyncNext+=vsyncInterval;") && tick.contains("if(oneiosLate)native_legacy_vsync_observed(3,vsyncHz);"));
    assert_eq!(staged.matches("__attribute__((weak));").count(), text.matches("__attribute__((weak));").count() + 1);
    // the boot video's edit before it is still there
    assert_eq!(staged.matches("kIOMapCopybackCache); // OneiOS native: crosvm's pool").count(), 1);
    assert!(build_oneios_display::staged_legacy_vsync(staged).is_err());
}

#[test]
fn the_kernel_halves_of_native_perf_are_off_unless_their_boot_argument_says_one() {
    // native-perf: the refresh tick's lock (OneiOSDisplay.cpp) and the stage lines (OneiOSAdreno.cpp, with the 3D half's call of
    // times()) default to 0. The kernel is not run here: the source is read for each default and for what the argument guards.
    let patches = repo().join("kernel/patches/oneios-hosted");
    let display = std::fs::read_to_string(patches.join("display/OneiOSDisplay.cpp")).unwrap();
    let parse = display.find("PE_parse_boot_argn(\"oneios-vsync-lock\", &on, sizeof on);").unwrap();
    assert!(display[..parse].trim_end().ends_with("uint32_t on = 0;   // off unless the image says oneios-vsync-lock=1: no mapping, no record, the tick as it was"));
    // the page is only asked for when the switch is 1, and the answer without it is the caller's own time
    assert!(display.contains("volatile uint64_t *page = vsyncLockSwitch == 1 ? oneios_display_page() : nullptr;\n    if (!page) return nextFree;"));
    assert!(!display.contains("oneios-vsync-lock=0 keeps"));
    let adreno = std::fs::read_to_string(patches.join("gpu/OneiOSAdreno.cpp")).unwrap();
    assert!(adreno.contains("uint32_t v = 0; PE_parse_boot_argn(\"oneios-gpu-stages\", &v, sizeof v); on = v ? 1 : 0;"));
    assert!(adreno.contains("extern \"C\" bool oneios_gpu_stage_wanted(void)\n{\n    return stages::enabled();"));
    // the display's own lines come before the gate, so that the stage switch does not silence them
    let (pace, gate) = (adreno.find("if (oneios_display_pace_report) oneios_display_pace_report(every);\n    if (!enabled()) return;").unwrap(), adreno.find("oneios_stage_read(&hostLeg, &h);").unwrap());
    assert!(pace < gate);
}

#[test]
fn the_audio_edits_fit_the_pinned_sources() {
    // take17 (2026-11-26): both audio lists against the real DroidVMAudio.cpp and DroidVMVirtioSnd.c where external/ has
    // them (the kernel build applies the same lists and stops on a missing anchor;
    // `oneios-tool kernel staged audio|snd` prints the two staged files, to read or to compile before a kernel build).
    let package = repo().join("external/UniXNU-droidvm/package");
    let (devices, transport) = (package.join("DroidVMAudio.cpp"), package.join("DroidVMVirtioSnd.c"));
    if !devices.exists() || !transport.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the audio edits were checked on the made-up sources only");
        return;
    }
    let audio = build_oneios_audio::staged_text(std::fs::read_to_string(&devices).unwrap()).unwrap();
    // the Mic's StartIO and StopIO are said, its engine counts its own periods, a refused rate or format is counted
    assert_eq!(audio.matches("if (device->isInput) oneiosMicIO(").count(), 2);
    assert_eq!(audio.matches("oneios_mic_periods = oneios_mic_periods + 1;").count(), 1);
    assert_eq!(audio.matches("oneiosRefused(device, selector, ").count(), 2);
    let (helper, method) = (audio.find("\nstatic void oneiosMicIO(bool start").unwrap(), audio.find("\nIOReturn DroidVMAudioUserClient::externalMethod(").unwrap());
    let counters = audio.find("volatile uint64_t oneios_mic_ios[2], oneios_mic_periods, oneios_audio_refused_formats;").unwrap();
    assert!(counters < helper && helper < method);
    // whatever touches the Mic device is counted: a client opened and closed, a selector, a mapping, a property set
    for touch in ["    if (device->isInput) oneiosMicTouch(0, 0);", "    if (device && device->isInput) oneiosMicTouch(1, 0);\n",
                  "    if (device && device->isInput) oneiosMicTouch(2, selector);\n", "    if (device->isInput) oneiosMicTouch(3, type);\n",
                  "    if (device && device->isInput) oneiosMicTouch(4, 0);\n"] {
        assert_eq!(audio.matches(touch).count(), 1, "{touch}");
    }
    assert!(audio.find("\nstatic void oneiosMicTouch(uint32_t kind, uint32_t value) {\n").unwrap() < audio.find("\nbool DroidVMAudioUserClient::start(").unwrap());
    // take18: the Mic publishes its input data sources (a 'dsrc' multi-selector), before the description that uses it,
    // and takes the HAL's SetControlValue and SetMultiControlValue for it, before the source's own cases
    let (controls, publish) = (audio.find("\nOSArray *oneiosMicControls() {\n").unwrap(), audio.find("\nvoid DroidVMAudioDevice::publish(IORegistryEntry *e) {\n").unwrap());
    assert!(controls < publish);
    assert_eq!(audio.matches("isInput ? oneiosMicControls() : oneiosControls(speakerEngine())").count(), 1);
    assert_eq!(audio.matches("        c->setObject(\"multi-selector\", kOSBooleanTrue);\n").count(), 1);
    let (two, eight, six) = (audio.find("a->scalarInput[0] == OneiosMicSourceID").unwrap(), audio.find("    case 8: {").unwrap(),
                             audio.find("    case 6: case 12:").unwrap());
    assert!(method < two && two < eight && eight < six);
    assert_eq!(audio.matches("case 8:").count(), 1);
    let snd = build_oneios_audio::staged_transport_text(std::fs::read_to_string(&transport).unwrap()).unwrap();
    // the microphone's stream is set up by its own thread, after the card is up and its lock is there
    let (lock, up, start) = (snd.find("snd_lock = lck_mtx_alloc_init(").unwrap(), snd.find("                        snd.up = true;\n").unwrap(),
                             snd.find("                        snd_mic_start();").unwrap());
    assert!(lock < up && up < start);
    // the helpers come before their users, in the file's order: the request's note, the slots, the capture, the START
    let at = |text: &str| snd.find(text).unwrap_or_else(|| panic!("the staged transport has no `{text}`"));
    assert!(at("static void snd_control_said(") < at("static bool control(uint32_t request_size, uint32_t reply_size) {\n"));
    assert!(at("static void snd_rx_post_missing(void) {") < at("void droidvm_snd_running(bool output, bool input) {\n"));
    assert!(at("static bool snd_control_late_answer(") < at("static bool control(uint32_t request_size, uint32_t reply_size) {\n"));
    assert!(at("static void snd_rx_post_missing(void) {") < at("static bool snd_stream_start(int32_t stream, bool input) {\n"));
    assert!(at("static bool snd_stream_start(int32_t stream, bool input) {\n") < at("void droidvm_snd_running(bool output, bool input) {\n"));
    // both streams start through it; the hosted kernel's START is the source's line
    assert_eq!(snd.matches(" = snd_stream_start(snd.").count(), 2);
    assert!(at("static bool set_params(") < at("static void snd_mic_setup(void *arg, wait_result_t result) {"));
    assert!(at("static void snd_mic_start(void) {") < at("\nvoid droidvm_snd_start(void) {\n"));
    // the function's two ends: the native kernel's lets the queue go, the hosted kernel's is the source's
    assert_eq!(snd.matches("    lck_mtx_lock(snd_lock);").count(), 2);     // droidvm_snd_running and the set-up thread
    assert_eq!(snd.matches("    lck_mtx_unlock(snd_lock);").count(), 2);
    assert_eq!(snd.matches("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n").count(), snd.matches("#endif\n").count() - snd.matches("#if DROIDVM_DEVICE_DYNAMIC\n").count());
    // take18: the control replies by interrupt (staged/audio.rs SND_IRQ), opt-in: the ISR mapped, the slot kept, the attach
    // before DRIVER_OK, both control waits through the nap, the hosted kernel's waits the source's IOSleep(1)
    let irq = build_oneios_audio::staged_irq_text(snd.clone()).unwrap();
    let at = |text: &str| irq.find(text).unwrap_or_else(|| panic!("the staged transport has no `{text}`"));
    assert!(irq.contains("snd_pci_device = mapped[4]; snd_pci_isr = mapped[3];\n") && irq.contains("snd_pci = true; snd_pci_slot = dev;\n"));
    assert_eq!(irq.matches("snd_control_nap();").count(), 2);
    assert!(at("static void snd_control_nap(void) {") < at("static bool snd_control_late_answer("));
    assert!(at("#define snd_control_nap() IOSleep(1)\n") < at("static bool control(uint32_t request_size, uint32_t reply_size) {\n"));
    assert!(at("    snd_irq_attach();") < at("    snd_write32(0x70, 15);         // DRIVER_OK\n"));
    assert!(at("static void snd_irq_attach(void) {") < at("    snd_irq_attach();"));
    // the queues stay NO_INTERRUPT unless the attach took: only the control queue's flag is cleared, after it
    assert_eq!(irq.matches("= 1;   // VIRTQ_AVAIL_F_NO_INTERRUPT: we poll").count(), 1);
    assert!(at("*(volatile uint16_t *)(snd_rings[SND_CONTROL] + 1024) = 0;") > at("armcpu_spi_attach(intid, false, snd_irq,"));
    assert!(build_oneios_audio::staged_irq_text(irq.clone()).is_err(), "twice is refused");
}

#[test]
fn the_room_and_swap_gap_hooks_are_weak_and_fit_the_pinned_sources() {
    // take17 (2026-11-26): staged/gpu.rs GPU3D_ROOM on a source that holds each anchor once, and with the scanout's two new
    // entries (staged/display.rs SCANOUT) against the real files where external/ has them.
    use oneios_kernel::staged::gpu::GPU3D_ROOM;
    let made_up: String = GPU3D_ROOM.iter().map(|(anchor, _)| format!("// before\n{anchor}// after\n")).collect();
    let room = build_oneios_gpu::staged_3d_room_text(made_up.clone()).unwrap();
    for (anchor, replacement) in GPU3D_ROOM {
        assert_eq!(room.matches(replacement).count(), 1, "{anchor}");
    }
    // The hook is asked for only when it is there, for the DRM capset alone, and after the device's answer was good.
    assert!(room.contains("void oneios_gpu_capset_seen(uint32_t, uint64_t, uint64_t) __attribute__((weak));\n"));
    assert!(room.contains("if (!status && in.capset_id == CapsetIdDrm && bytes >= sizeof(CapsetDrm) && oneios_gpu_capset_seen) {\n"));
    assert!(build_oneios_gpu::staged_3d_room_text(made_up.repeat(2)).is_err());
    assert_eq!(build_oneios_gpu::staged_3d_room_text(String::new()).unwrap_err(),
               "DroidVMVirtioGpu3D.cpp no longer has exactly one void oneios_gpu_hint(uint8_t *) __attribute__((weak));");
    let package = repo().join("external/UniXNU-droidvm/package");
    let (three_d, scanout) = (package.join("DroidVMVirtioGpu3D.cpp"), package.join("DroidVMVirtioGpuDisplay.cpp"));
    if !three_d.exists() || !scanout.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the room and swap-gap edits were checked on the made-up sources only");
        return;
    }
    let staged = build_oneios_gpu::staged_3d_source(std::fs::read_to_string(&three_d).unwrap()).unwrap();
    // In getCaps, under the clients' lock, after the answer is copied out; the capset's layout is the file's own.
    let (copied, seen, unlocked) = (staged.find("    if (!status) memcpy(out, answer + sizeof(Header), outSize);\n").unwrap(),
                                    staged.find("oneios_gpu_capset_seen(uint32_t(proc_selfpid()), caps.vaStart, caps.vaSize);").unwrap(),
                                    staged.find("IOReturn contextInit(Client *c").unwrap());
    assert!(copied < seen && seen < unlocked && staged.contains("    uint64_t vaStart, vaSize;\n"));
    let staged = build_oneios_display::staged_scanout(std::fs::read_to_string(&scanout).unwrap()).unwrap();
    // The gap goes out where it is measured, with the frame lock held: the hook only stores.
    let (measured, told, last) = (staged.find("const uint64_t gap = now - lastOfferUs;").unwrap(),
                                  staged.find("if (oneios_gpu_swap_gap) oneios_gpu_swap_gap(gap, now);").unwrap(), staged.find("    lastOfferUs = now;\n").unwrap());
    assert!(measured < told && told < last);
    assert_eq!(staged.matches("void oneios_gpu_swap_gap(uint64_t, uint64_t) __attribute__((weak));\n").count(), 1);
}

#[test]
fn the_codec_edits_fit_the_pinned_sources() {
    // take18b (2026-11-26): the native kernel's codec archive. Each list on a made-up source holding each anchor once, and
    // applied twice it stops (its anchor is gone); then both staged files against the real ones where external/ has them
    // (`oneios-tool kernel staged media|vthost` prints them, to read or to compile before a kernel build).
    use oneios_kernel::build_oneios_codec;
    use oneios_kernel::staged::codec_transport::MEDIA_TRANSPORT_PCI;
    let made_up: String = MEDIA_TRANSPORT_PCI.iter().map(|(anchor, _)| format!("// before\n{anchor}// after\n")).collect();
    let staged = build_oneios_codec::staged_transport(made_up.clone()).unwrap();
    for (_, replacement) in MEDIA_TRANSPORT_PCI {
        assert_eq!(staged.matches(replacement).count(), 1);
    }
    assert!(build_oneios_codec::staged_transport(made_up.repeat(2)).is_err());
    assert_eq!(build_oneios_codec::REQUIRED, ["_oneios_codec_start", "_droidvm_media_start", "_droidvm_vt_paravirt_attach",
                                              "__GLOBAL__sub_I_DroidVMVTParavirt.cpp"]);
    let package = repo().join("external/UniXNU-droidvm/package");
    let (transport, host) = (package.join("DroidVMVirtioMedia.c"), package.join("DroidVMVTHost.cpp"));
    if !transport.exists() || !host.exists() {
        eprintln!("external/UniXNU-droidvm is not here: the codec's edits were checked on the made-up sources only");
        return;
    }
    let media = build_oneios_codec::staged_transport(std::fs::read_to_string(&transport).unwrap()).unwrap();
    // the PCI accessors before their first user, the hosted kernel's as the source's (under #else: so the anchors stay)
    let at = |text: &str| media.find(text).unwrap_or_else(|| panic!("the staged transport has no `{text}`"));
    assert!(at("static uintptr_t media_pci_find(unsigned n, vm_offset_t bus, uint64_t size) {") < at("static void post(struct media_device *d"));
    assert!(at("d->registers = media_pci_find(media_count - 1, ") < at("        d->registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);\n"));
    // the accessors, the node's mapping, the late commands' state, their taking off before a command, the 12 s wait
    assert_eq!(media.matches("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n").count(), 5);
    assert!(at("static uint16_t media_late[MEDIA_MAX];") < at("static int command(struct media_device *d,"));
    assert!(at("        if (waited == media_command_wait_ms) { droidvm_media_timeouts++; media_late[index_of(d)]++; return -1; }")
            < at("        if (waited == 5000) { droidvm_media_timeouts++; return -1; }   // 5 s\n"));
    assert!(media.contains("uint64_t droidvm_media_pool_size(unsigned device) { return device < media_count ? media[device].pool_size : 0; }\n"));
    let vthost = build_oneios_codec::staged_text(std::fs::read_to_string(&host).unwrap()).unwrap();
    let at = |text: &str| vthost.find(text).unwrap_or_else(|| panic!("the staged host has no `{text}`"));
    // dcap and dcan answered; the codecs from the device's OUTPUT formats; helpers before their users
    assert!(vthost.contains("    case cc(\"dcap\"): decoderCapabilities(c, h, m, n); break;\n"));
    assert!(vthost.contains("    case cc(\"dcan\"): {\n"));
    assert!(at("uint32_t decoderCodecs()") < at("void emitFrame(") && at("int codecIndex(") < at("void copyFrame("));
    assert!(at("bool configurationAtom(") < at("int startSession(") && at("void refreshCapture(") < at("void workerMain("));
    // crosvm's decoder (CODEC_CROSVM): the LAST buffer of a mid-stream announce resumes the session, an ERROR ends it, HEVC
    // at 8 bits, the pool's room at dsta, dcap from the device's menus, a smaller CAPTURE set when the pool is short
    assert!(at("bool resumeAfterChange(Session *s) {") < at("void workerMain(") && at("bool resumeAfterChange(") > at("void refreshCapture("));
    assert!(vthost.contains("            if ((e.flags & BufFlagLast) && s->changePending) {\n                if (!resumeAfterChange(s)) continue;"));
    assert!(vthost.contains("        return chroma == 1 && lumaDepth == 8 && chromaDepth == 8;\n") && !vthost.contains("lumaDepth <= 10"));
    assert!(at("uint64_t sessionBytes(uint64_t samples) {") < at("bool decodableSize(Session *s, uint32_t pixelformat) {"));
    assert!(at("DecoderCaps decoderCaps(int index) {") < at("void decoderCapabilities(") && !vthost.contains("table[index]"));
    assert!(vthost.contains("        const DecoderCaps t = decoderCaps(index);\n"));
    assert!(vthost.contains("    if (s->deviceGone) return -12911;") && vthost.contains("if (!s || s->deviceGone || !s->outputStreaming"));
    assert!(at("                trace(\"decoder session %u: ended by the device\", s->v4l2);") > at("void workerMain("));
    assert!(vthost.contains("        const unsigned fewer[2] = {control[1] ? control[1] : 4u, 6u};\n"));
    assert!(vthost.contains("    f.planes[0].sizeimage = outputBytes(codedSamples(d.width, d.height));"));
    // the added lines keep the source's indentation (a Rust line continuation would have eaten it)
    assert!(vthost.contains("uint32_t(out.length);\n    if (input.size > out.length) oneios_decode_cut = oneios_decode_cut + 1;"));
    assert!(vthost.contains("the device ended the session\n    if (!s->outputStreaming || !param(m, n, cc(\",sbf\"), sbf)"));
    // the session's crosvm flags cleared with it (dsta and dinv reuse the instance), at teardown's end
    let teardown = at("void teardown(Session *s) {");
    let reset = at("    s->changePending = s->changeFits = s->deviceGone = false;");
    assert!(teardown < reset && reset < at("void supportedProperties(Client *c, const Header &h) {"));
    // UniXNU-droidvm 2cb737d: a Dbuf surface from another process's pool, through the process's own client
    assert!(vthost.contains("                surface = s->surfaces && !s->holder && lookupSurface(s->surfaces, surfaces[0], view);\n"));
    assert!(vthost.contains("                    surface = s->holder && lookupSurface(s->surfaces, surfaces[0], view, s->holder);\n"));
    assert_eq!(build_oneios_codec::SESSION.len(), 6);
}
