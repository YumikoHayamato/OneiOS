# appinst: installing a decrypted `.ipa` on the guest, no PC

Owner: userland. Code: `userland/appinst/` (the tool), `userland/appinst/host/` (the host-side reference),
services in `userland/appinst/services.d/`. This is the on-device installer for third-party apps — the
OneiOS answer to a TrollStore-class tool, shaped to this project's architecture rather than to a jailbreak of
stock iOS.

**Scope and the one hard rule.** appinst installs an **already-decrypted** `.ipa`. It never strips FairPlay or
any DRM; a Mach-O slice that is still encrypted (`LC_ENCRYPTION_INFO_64` with `cryptid=1`) is detected and the
install is refused. It is for apps the owner has the right to run: their own builds, open-source apps, and free
apps they have decrypted themselves. The capability — re-sign ad-hoc, trust the CDHash, register with
SpringBoard — is exactly what the project already does for its own Metal plugin and helpers; appinst only
generalises it to a whole app bundle.

## Why the obvious approaches don't fit

Two routes were considered and rejected after reading the kernel:

- **Patch AMFI to accept a directory of ad-hoc binaries.** AMFI is Apple's closed kext in the kernelcache. The
  project never patches its signature check — `kernel/prelink/README.md` ("AMFI and launch constraints") states
  the signature checks are unchanged and *the only way a binary runs is its CDHash in a trust cache*. Even if
  AMFI were patched, this device is **SPTM/TXM** (`#if CONFIG_SPTM` is the live path in
  `bsd/kern/kern_trustcache.c`, `osfmk/arm64/sptm/pmap/pmap.c`): the Trusted Execution Monitor is the authority,
  AMFI and XNU only *ask* it (`query_trust_cache` → `txm_query_trust_cache` via `txm_kernel_call`). XNU cannot
  override the monitor's verdict, so a path rule is both fragile and insufficient.

- **Load a trust cache at runtime.** On SPTM the only runtime loader, `load_trust_cache_with_type`, requires an
  **IMG4-signed** cache validated by TXM plus `com.apple.private.pmap.load-trust-cache` — we have no signing
  keys. The raw/legacy runtime loader is not available at all: `txm_load_legacy_trust_cache()` is
  `panic("legacy trust caches are not supported on this platform")`. So there is no no-reboot runtime path.

## Route A′: lean on the fact that the guest is a VM

The guest's trust cache is a **sidecar that xnuhost injects at boot** (`docs/booter.md`: trust caches are
"DarwinBoot's sidecars beside the kernel"). That injection is how the owner trust module — the Adreno Metal
plugin and the board tools in `owner-trust-extra/*.json` — is trusted today without Apple's keys
(`make-owner-trust-bundle.py`). appinst reuses that exact mechanism and that exact module format. The only new
idea is to feed new CDHashes into the sidecar and take a **guest-VM restart** as the commit point. A guest
restart is cheap and, per the project's rule, the owner's decision — appinst requests it and never reboots on
its own.

### Two phases

| Phase | When | What |
|---|---|---|
| `appinst install FILE.ipa` | user action | unzip; thin each Mach-O to **arm64e** (fallback arm64), never setting the platform-binary flag; **refuse** an encrypted slice; re-sign ad-hoc at 4 KiB with `ldid`, preserving the app's own entitlements on the main executable; compute CDHashes; write the **trust inbox**; place the `.app` under `/var/containers/Bundle/Application/<UUID>` and create the data container under `/var/mobile/Containers/Data/Application/<UUID>`; write a pending marker; request a restart |
| (guest-VM restart) | owner's call | xnuhost folds the trust inbox into the owner trust module it injects; TXM now trusts the new CDHashes |
| `appinst finalize` | at boot (service) | register each pending app with installd/LaunchServices now that its signature validates; clear the marker |

Re-signing before the restart is deliberate: the CDHash a trust cache stores is the hash of the final signature,
so the app must be signed first, then its hashes trusted, then — only after the restart — registered. Phase 2
exists because installd validates the signature at install time and would reject before the restart.

### Paths and formats

- Trust inbox: `/private/var/oneios/trust-inbox/<bundle-id>.json`, one per app, in the shape
  `make-owner-trust-bundle.py` already reads: `{"cdhashes":["<40 hex>", ...], "category":0}`. The CDHash is the
  SHA-256 of the binary's SHA-256 CodeDirectory, truncated to 20 bytes (the trust module's entry width), picked
  from the correct CodeDirectory when `ldid` emits more than one (`macho.c:superblob_best_codedir`).
- Owner trust module: version-2, `struct { u32 version=2; u8 uuid[16]; u32 count; }` then `count` entries of
  `{ u8 cdhash[20]; u8 hashType=2; u8 _; u8 category; u8 _; }`, hashes strictly ascending — the format in
  `make-owner-trust-bundle.py` and checked by DarwinBoot's `ValidTrustCaches`.
- Pending marker: `/private/var/oneios/appinst/pending/<bundle-id>.plist` (app path, data path, UUIDs).

## Host wiring (the one open item outside the guest)

At guest boot xnuhost must fold every `/private/var/oneios/trust-inbox/*.json` into the first module (the owner
module) of the trust-cache bundle it injects, before handing it to the guest. `host/fold-trust-inbox.py` is the
reference: it rebuilds the owner module to include the inbox hashes, leaves every other module (Apple's two
originals, the App cryptex) byte-for-byte, re-sorts the hashes, and re-runs the loader's structural check. The
phone has no Python, so the device-side fold is a small **xnuhost (Rust) change** that must emit exactly those
bytes; the Python tool is the authority on the format and also serves the build PC.

Where the trust inbox lives relative to xnuhost matters: it sits on the guest's writable Data volume, which
xnuhost backs. The restart-time fold reads it from there (or from a dedicated small shared region if that proves
simpler on the device) — this choice is the xnuhost author's, and is the main integration decision left.

## Built and signed like the helpers

`userland/appinst/build.sh` uses the `scripts/ios` toolchain and signs with `scripts/ios/ios-sign.py`
(`--entitlements userland/appinst/entitlements.plist`, identifier `com.oneios.appinst`), arm64e, 4 KiB pages,
verified by `userland/tweak cdhash`. appinst's own CDHash goes into `owner-trust.json` through `roottweak`, so
it runs like the other helpers, and its private entitlements (place app bundles, create data containers, rebuild
the LaunchServices database) are honoured because it is itself trust-cached. The on-device signer `ldid` is a
separate arm64e dependency, placed beside the helpers like `dropbear` (`userland/appinst/README.md`).

## What needs a Mac build and on-device verification

1. **Build on a Mac or the Linux cross toolchain** — iOS binaries can't be compiled on the dev box used to write
   this; the C/Obj-C is syntax-clean and the host-side fold is tested, but nothing here has been run on arm64e.
2. **Supply `ldid`** (arm64e, 4 KiB, trust-cached) at `/private/var/oneios/bin/ldid`.
3. **Wire the xnuhost fold** of the trust inbox at guest boot (reference: `host/fold-trust-inbox.py`).
4. **LaunchServices registration** (`register.m`): `registerApplicationDictionary:` in place is the primary
   path. installd's provisioning-profile requirement is the most likely thing to reject a profile-less app; on
   this guest a trust-cached binary should read as anchor-trusted and skip it, but if installd still refuses, the
   fallbacks are (a) the installd/MobileInstallation SPI path (entitlements already granted) or (b) a small
   installd relaxation in the project's existing style (as with the App cryptex and SystemOS volumes). Verify
   whether the icon appears on first `finalize` or needs a respring. Also check the data container: appinst
   pre-creates one (owned by mobile, uid 501); if LaunchServices/installd instead assigns its own container UUID
   at registration, the pre-created one is orphaned — then either pass the container path in the registration
   dictionary or let installd create it and drop the pre-creation.
5. **Entitlements of installed apps** carry over from the original signature; capabilities that need Apple
   services (push, associated domains) won't function — expected for an ad-hoc install.
