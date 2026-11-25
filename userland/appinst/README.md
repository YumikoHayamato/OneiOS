# appinst — on-device .ipa installer for the OneiOS guest

Install a **decrypted** `.ipa` straight on the phone — no PC, no 7-day certificate — and get an icon on
SpringBoard's Home Screen. It is the OneiOS equivalent of a TrollStore-class installer, but built for this
project's architecture: the guest is a VM whose trust cache xnuhost injects at boot, so there is no AMFI or TXM
surgery and no Apple signing keys involved. The design note is [`docs/appinst.md`](../../docs/appinst.md).

> **Decrypted input only.** appinst never strips FairPlay/DRM. A Mach-O slice that is still encrypted
> (`cryptid=1`) is detected and the install is refused with a clear message. Use it for apps you have the right
> to run: your own builds, open-source apps, free apps you have decrypted yourself.

## Why two phases (and a restart)

This device is SPTM/TXM: the Trusted Execution Monitor, not AMFI, decides a signature is valid, and it only
trusts a CDHash that is in a trust cache. The guest's trust cache is a sidecar that **xnuhost injects at boot**
(`docs/booter.md`); runtime trust-cache loading is unavailable here (`txm_load_legacy_trust_cache` panics, and
the IMG4 path needs Apple's keys). So a freshly re-signed app cannot be trusted until the next guest-VM restart.
installd validates the signature at install time, so we split the work:

1. **`appinst install FILE.ipa`** (phase 1, before the restart): unzip → thin to arm64e → refuse an encrypted
   slice → re-sign ad-hoc at 4 KiB with `ldid` → compute CDHashes → drop them in the **trust inbox** → place the
   `.app` under `/var/containers/Bundle/Application/<UUID>` and create its data container → write a pending
   marker → request a guest-VM restart (the owner's call; appinst never reboots by itself).
2. **restart**: xnuhost folds the trust inbox into the owner trust module it injects; TXM now trusts the hashes.
3. **`appinst finalize`** (phase 2, at boot): register each pending app with installd/LaunchServices now that its
   signature validates, then clear the marker. Run automatically by the `appinst-finalize` service.

## Build

Source-only, built with the `scripts/ios` toolchain (`docs/building.md` section 9) and signed ad-hoc at 4 KiB
like `userland/helpers`:

```sh
userland/appinst/build.sh [OUT_DIR]        # default out/userland/appinst
```

Output: `OUT_DIR/appinst` (one binary: `install`, `finalize`, `serve` subcommands) and `OUT_DIR/cdhashes.txt`.
Built on the Mac (Xcode) or on Linux with the cross toolchain; it links `-framework Foundation -lz` (CommonCrypto
and LaunchServices resolve at runtime). It is **arm64e** because iOS 27 kills arm64 platform binaries at exec.

### The signer (`ldid`) — a dependency, not built here

appinst re-signs on the device, where there is no Python or cctools, so it calls **`ldid`** at
`/private/var/oneios/bin/ldid` (like the helpers' debug shell uses a prebuilt `dropbear`). Build an **arm64e**
`ldid` (ProcursusTeam/ldid, or take Droid-VM/ownervisor's), sign it ad-hoc at 4 KiB like these binaries, and put
it in the helpers output directory before `roottweak apply` so its own CDHash is trusted. `ldid`'s default code
signature page size is 4 KiB, which matches the kernel.

## Install it into an image

Put `OUT_DIR/appinst`, the two files in `services.d/`, and the `ldid` binary into the helpers output directory,
then `roottweak apply --helpers DIR` (it installs every signed Mach-O into `/private/var/oneios/bin` and lists
its CDHash in `owner-trust.json`). Enable the services with `enabled/appinst-finalize` and
`enabled/appinst-serve` in the Data seed. See `userland/helpers/README.md`.

## Use

**From the phone (no PC):** download a decrypted `.ipa` in Safari (it lands in Files), open
`http://127.0.0.1:8088/` in Safari, pick the file, tap **Install**. Then restart the guest when the owner is
ready; the app appears after the restart.

**From a shell:** `appinst install /path/to/app.ipa`, restart, done (the `appinst-finalize` service registers it).

## Host wiring (the one piece that lives outside this directory)

At guest boot xnuhost must fold the trust inbox (`/private/var/oneios/trust-inbox/*.json`, each
`{"cdhashes":[...],"category":0}`) into the owner trust module it injects. `host/fold-trust-inbox.py` is the
reference implementation and build-PC tool — it rebuilds the bundle's owner module to include the new hashes,
leaving Apple's modules byte-for-byte, with the same structural checks the boot loader applies. The on-device
restart-time fold is a small xnuhost (Rust) change that must produce exactly those bytes; see `docs/appinst.md`.

## What still needs a Mac build and on-device checks

See the bottom of `docs/appinst.md`. In short: build on a Mac/Linux toolchain (can't compile iOS here); supply
`ldid`; wire the xnuhost fold; and verify the LaunchServices registration path (installd's provisioning-profile
check is the most likely thing to need the project's usual relaxation — the fallbacks are noted in `register.m`).
