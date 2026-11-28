//! Droid-VM's own crosvm on the phone (scripts/droidvm/fetch-crosvm.sh: their prebuilt, BSD-3-Clause with Droid-VM's
//! GPL-2.0-or-later additions; the owner chose it over a build from source, 2026-11-23). Its package, crosvm and the
//! three libraries of it the phone has not, goes to `/data/local/tmp/oneios-droidvm`, each file's SHA-256 checked
//! there; crosvm finds those libraries next to itself (RUNPATH `$ORIGIN`) and the rest (libbase, libcutils,
//! libminijail, libcap, libbinder_ndk, ...) in /system/lib64. It runs the pseudo-unprotected VM
//! (`--protected-vm-pseudo-unprotected`: `vm-probe --shared-ram`), which needs gunyah_host_share (`phone hostshare`).
//! `oneios-tool phone droidvm-crosvm [--dir DIR] [--grep REGEX] [--virgl FILE|prebuilt] [--crosvm FILE|prebuilt]`: the
//! push, then the lines of `crosvm run --help` that match REGEX. `--virgl` (take18) and `--crosvm` (take18b) put another
//! build of the renderer or of crosvm itself in the package's file (`Swaps`); vm-boot takes the same two.
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;

/// The package's folder on the phone, and its files.
pub const DIR: &str = "/data/local/tmp/oneios-droidvm";
/// libvulkan_freedreno.so is Droid-VM's Turnip (Mesa, MIT): the Vulkan driver crosvm's Android display blits the VM's frames
/// with (`CROSVM_DISPLAY_VULKAN_LIBRARY`, vm-boot with the GPU's scanout); crosvm does not link it.
pub const FILES: [&str; 5] = ["crosvm", "libvirglrenderer.so", "libgfxstream_backend.so", "libvncserver.so", "libvulkan_freedreno.so"];

/// The package's files in DIR: (name, SHA-256, bytes), in FILES' order.
pub fn package(dir: &str) -> Result<Vec<(String, String, usize)>> {
    package_with(dir, Swaps::default())
}

/// The host renderer crosvm loads (RUNPATH `$ORIGIN`): the package's own, or another build of it.
pub const VIRGL: &str = "libvirglrenderer.so";
/// take18's build of the host renderer (scripts/droidvm/build-virgl.sh: Droid-VM/virglrenderer 2529a9f, wip/hackintosh, with
/// the NDK), the default of `vm-boot --virgl`. The package's own (DroidVM-Prebuilt-Root f7c6ce8: virglrenderer cd71ea4 of
/// 2026-09-01) is `--virgl prebuilt`.
pub const VIRGL_TAKE18: &str = "/mnt/tkg-build/oneios/dl/droidvm-prebuilt/virgl-2529a9f/libvirglrenderer.so";
/// crosvm itself: the package's own (the default of `--crosvm`), or another build of it, e.g. take18b's from source
/// (Droid-VM/crosvm wip/hackintosh da647254c + samfor12 e7c1316, built with cargo and the NDK outside AOSP:
/// /mnt/giochi/Linux/oneios/take18/upstream/artifacts/crosvm, its build-crosvm.sh and REPORT.txt). It still loads the
/// package's libraries beside it (RUNPATH `$ORIGIN`), so only this one file changes.
pub const CROSVM: &str = "crosvm";

/// `--virgl`'s and `--crosvm`'s word for the package's own file: `prebuilt` (or `package`, or nothing); anything else
/// names the file to push in its place.
pub fn virgl_choice(virgl: &str) -> Option<&str> {
    match virgl {
        "" | "prebuilt" | "package" => None,
        file => Some(file),
    }
}

/// The package's files that come from elsewhere when named (take18): the host renderer (`--virgl FILE`) and crosvm itself
/// (`--crosvm FILE`). Each goes to the phone under the package's own name; a later push of the other puts the package's
/// back (its SHA-256 differs).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Swaps<'a> {
    pub virgl: Option<&'a str>,
    pub crosvm: Option<&'a str>,
}

impl<'a> Swaps<'a> {
    /// From the two switches' words (virgl_choice: `prebuilt` is the package's own).
    pub fn from_words(virgl: &'a str, crosvm: &'a str) -> Self {
        Swaps { virgl: virgl_choice(virgl), crosvm: virgl_choice(crosvm) }
    }
}

/// Where a file of the package comes from on the PC: DIR, or the named file for the renderer or crosvm.
pub fn source(dir: &str, swaps: Swaps, name: &str) -> String {
    match name {
        VIRGL if swaps.virgl.is_some() => swaps.virgl.unwrap_or_default().to_string(),
        CROSVM if swaps.crosvm.is_some() => swaps.crosvm.unwrap_or_default().to_string(),
        _ => format!("{dir}/{name}"),
    }
}

/// A file named in a package's place must be a 64-bit little-endian ELF for arm64 (EM_AARCH64, 183): the phone's.
fn arm64_elf(data: &[u8]) -> bool {
    data.len() > 20 && data.starts_with(b"\x7fELF") && data[4] == 2 && data[5] == 1 && data[18] == 183 && data[19] == 0
}

/// As `package`, with the renderer and crosvm from the named files when named (each goes to the phone under the
/// package's name).
pub fn package_with(dir: &str, swaps: Swaps) -> Result<Vec<(String, String, usize)>> {
    FILES
        .iter()
        .map(|name| {
            let path = source(dir, swaps, name);
            // named by the switch, wherever the file is (a named file may sit in DIR too)
            let swapped = (*name == VIRGL && swaps.virgl.is_some()) || (*name == CROSVM && swaps.crosvm.is_some());
            let hint = match *name {
                _ if !swapped => "scripts/droidvm/fetch-crosvm.sh",
                CROSVM => "take18b's build-crosvm.sh builds it; --crosvm prebuilt for the package's own",
                _ => "scripts/droidvm/build-virgl.sh builds it; --virgl prebuilt for the package's own",
            };
            let data = fs::read(&path).with_context(|| format!("cannot read {path} ({hint})"))?;
            if *name == VIRGL && !data.starts_with(b"\x7fELF") {
                bail!("{path}: not an ELF file");
            }
            if swapped && !arm64_elf(&data) {
                bail!("{path}: not an arm64 ELF file (the phone's {name})");
            }
            Ok((name.to_string(), format!("{:x}", Sha256::digest(&data)), data.len()))
        })
        .collect()
}

/// The names whose SHA-256 on the phone (`sha256sum` lines) is not the package's: what to push.
pub fn to_push<'a>(files: &'a [(String, String, usize)], sums: &str) -> Vec<&'a (String, String, usize)> {
    files
        .iter()
        .filter(|(name, sha, _)| {
            !sums.lines().any(|l| {
                let mut w = l.split_whitespace();
                w.next() == Some(sha.as_str()) && w.next() == Some(name.as_str())
            })
        })
        .collect()
}

/// The phone's check of the package after the push: every file's SHA-256, crosvm executable.
pub fn check_script(files: &[(String, String, usize)]) -> String {
    let mut s = format!("cd {DIR} || exit 1\n");
    for (name, sha, _) in files {
        s += &format!("got=$(sha256sum {name} | cut -d' ' -f1); [ \"$got\" = \"{sha}\" ] || {{ echo \"{name}: sha256 $got, not {sha}\"; exit 1; }}\n");
    }
    // and the folder root's too: root's crosvm loads its libraries and the display's driver from it, which no other user may
    // swap (adb's push wants it open: `push` opens it first)
    s += "chown root:root crosvm *.so && chmod 755 crosvm && chmod 644 *.so && chown root:root . && chmod 755 . && echo package ok\n";
    s
}

/// The package on the phone, pushing only what differs; crosvm's path there.
pub fn push(adb: &Adb, dir: &str) -> Result<String> {
    push_with(adb, dir, Swaps::default())
}

/// As `push`, with the renderer and crosvm from the named files when named (`Swaps`): the phone's folder then holds those
/// under the package's names, and a later push of the package's own puts them back (the SHA-256 differs).
pub fn push_with(adb: &Adb, dir: &str, swaps: Swaps) -> Result<String> {
    let files = package_with(dir, swaps)?;
    if let Some(v) = swaps.virgl {
        println!("droidvm: the host renderer {VIRGL} from {v}, not the package's");
    }
    if let Some(c) = swaps.crosvm {
        let sha = files.iter().find(|f| f.0 == CROSVM).map(|f| f.1.as_str()).unwrap_or("?");
        println!("droidvm: crosvm from {c} (sha256 {sha}), not the package's");
    }
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the package's folder")?;
    let sums = adb.root_ok(&format!("cd {DIR} && sha256sum {} 2>/dev/null; true\n", FILES.join(" ")), "the package's sums")?;
    for (name, _, len) in to_push(&files, &sums) {
        println!("droidvm: {name} ({len} bytes) to {DIR}");
        // a root-owned file of an earlier push: adb's shell cannot overwrite it
        adb.root_ok(&format!("rm -f {DIR}/{name}\n"), "the old file")?;
        adb.push(&source(dir, swaps, name), &format!("{DIR}/{name}"))?;
    }
    let r = adb.root(&check_script(&files))?;
    if !r.out.contains("package ok") {
        bail!("Droid-VM's package on the phone: {}{}", r.out.trim(), r.err.trim());
    }
    Ok(format!("{DIR}/crosvm"))
}

/// `phone droidvm-crosvm`: with `--virgl FILE`, crosvm starts with that renderer (crosvm links it: a renderer the phone
/// cannot load is a CANNOT LINK here, before any VM); with `--crosvm FILE`, that crosvm (the same check for its own links,
/// and its `run --help` is the one shown).
pub fn crosvm(adb: &Adb, dir: &str, grep: &str, virgl: &str, crosvm: &str) -> Result<()> {
    if grep.contains('\'') {
        bail!("--grep: no single quotes");
    }
    adb.need_root()?;
    let c = push_with(adb, dir, Swaps::from_words(virgl, crosvm))?;
    let r = adb.root(&format!("{c} run --help > {DIR}/help.txt 2>&1; echo \"crosvm exit $?\"; grep -i -E -A3 '{grep}' {DIR}/help.txt\n"))?;
    print!("{}{}", r.out, r.err);
    if r.out.contains("CANNOT LINK") || r.out.contains("not found") {
        bail!("crosvm does not start on the phone");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_what_differs_is_pushed() {
        let files = vec![("crosvm".to_string(), "aa".to_string(), 3), ("libvncserver.so".to_string(), "bb".to_string(), 2)];
        assert_eq!(to_push(&files, "").len(), 2);
        assert_eq!(to_push(&files, "aa  crosvm\ncc  libvncserver.so\n").iter().map(|f| f.0.as_str()).collect::<Vec<_>>(), ["libvncserver.so"]);
        assert!(to_push(&files, "aa  crosvm\nbb  libvncserver.so\n").is_empty());
        let s = check_script(&files);
        assert!(s.contains("got=$(sha256sum crosvm | cut -d' ' -f1); [ \"$got\" = \"aa\" ]"));
        assert!(s.ends_with("echo package ok\n"));
    }

    #[test]
    fn the_renderer_comes_from_virgl_when_named() {
        assert_eq!(virgl_choice("prebuilt"), None);
        assert_eq!(virgl_choice(""), None);
        assert_eq!(virgl_choice("/x/libvirglrenderer.so"), Some("/x/libvirglrenderer.so"));
        let r = |v| Swaps { virgl: Some(v), crosvm: None };
        assert_eq!(source("/p", Swaps::default(), VIRGL), "/p/libvirglrenderer.so");
        assert_eq!(source("/p", r("/b/libvirglrenderer.so"), VIRGL), "/b/libvirglrenderer.so");
        assert_eq!(source("/p", r("/b/libvirglrenderer.so"), "crosvm"), "/p/crosvm");
        // a package folder with stand-ins: the named renderer's sum replaces the package's, the others stay
        let dir = std::env::temp_dir().join(format!("oneios-droidvm-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        for name in FILES {
            fs::write(dir.join(name), format!("\x7fELF {name}")).unwrap();
        }
        let other = dir.join("other.so");
        fs::write(&other, arm64_stand_in("other renderer")).unwrap();
        let d = dir.to_str().unwrap();
        let own = package_with(d, Swaps::default()).unwrap();
        let named = package_with(d, r(other.to_str().unwrap())).unwrap();
        assert_eq!(own.len(), FILES.len());
        for (a, b) in own.iter().zip(&named) {
            assert_eq!(a.0, b.0);
            assert_eq!(a.1 == b.1, a.0 != VIRGL, "{}", a.0);
        }
        fs::write(&other, "not an ELF").unwrap();
        assert!(package_with(d, r(other.to_str().unwrap())).is_err());
        assert!(package_with(d, r("/nonexistent/libvirglrenderer.so")).is_err());
        fs::remove_dir_all(&dir).unwrap();
    }

    /// An ELF header for arm64 (64-bit, little-endian, EM_AARCH64) and a tag, as a stand-in for a named file.
    fn arm64_stand_in(tag: &str) -> Vec<u8> {
        let mut h = vec![0u8; 64];
        h[..4].copy_from_slice(b"\x7fELF");
        h[4] = 2;
        h[5] = 1;
        h[18] = 183;
        h.extend_from_slice(tag.as_bytes());
        h
    }

    #[test]
    fn crosvm_comes_from_the_named_build_only_when_named() {
        // take18b: `--crosvm FILE|prebuilt`, default the package's own; the renderer's switch is independent of it
        assert_eq!(Swaps::from_words("prebuilt", "prebuilt"), Swaps::default());
        assert_eq!(Swaps::from_words("", "/c/crosvm"), Swaps { virgl: None, crosvm: Some("/c/crosvm") });
        let both = Swaps::from_words("/b/libvirglrenderer.so", "/c/crosvm");
        assert_eq!(source("/p", both, CROSVM), "/c/crosvm");
        assert_eq!(source("/p", both, VIRGL), "/b/libvirglrenderer.so");
        assert_eq!(source("/p", both, "libvncserver.so"), "/p/libvncserver.so");
        let dir = std::env::temp_dir().join(format!("oneios-droidvm-crosvm-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        for name in FILES {
            fs::write(dir.join(name), format!("\x7fELF {name}")).unwrap();
        }
        let built = dir.join("built-crosvm");
        fs::write(&built, arm64_stand_in("crosvm from source")).unwrap();
        let d = dir.to_str().unwrap();
        let c = Swaps { virgl: None, crosvm: Some(built.to_str().unwrap()) };
        let own = package_with(d, Swaps::default()).unwrap();
        let named = package_with(d, c).unwrap();
        for (a, b) in own.iter().zip(&named) {
            assert_eq!(a.1 == b.1, a.0 != CROSVM, "{}", a.0);
        }
        // a crosvm for another machine (x86-64's EM_X86_64, 62) or not an ELF is refused before any push
        let mut x86 = arm64_stand_in("x86");
        x86[18] = 62;
        fs::write(&built, x86).unwrap();
        assert!(package_with(d, c).unwrap_err().to_string().contains("not an arm64 ELF"));
        fs::write(&built, "#!/bin/sh\n").unwrap();
        assert!(package_with(d, c).is_err());
        assert!(package_with(d, Swaps { virgl: None, crosvm: Some("/nonexistent/crosvm") }).is_err());
        fs::remove_dir_all(&dir).unwrap();
    }
}
