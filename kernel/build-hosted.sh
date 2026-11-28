#!/bin/bash
# Build the hosted OneiOS XNU: the kernel of kernel/build-linux.sh plus the hosted machine layer
# (kernel/patches/oneios-hosted), which runs as a Linux process under xnuhost instead of at EL1.
#   scripts/bg.sh kernel-hosted env HOSTED_OUT=<name> kernel/build-hosted.sh
#   XNU_FILESET_REFERENCE=1 ...  also the RELEASE reference of the prelink -> out/kernel/current-hosted*/reference.macho
# Linux (x86_64 or arm64): the host's clang and xnu-linux-toolchain (kernel/linux/), as kernel/build-linux.sh.
# macOS: Xcode's clang and iphoneos SDK, as kernel/build-macos.sh (Xcode of pins.sh; ALLOW_XCODE=1 accepts another,
# XCODE_APP=... another place). Same series, archives and output on both.
# Both need Rust (cargo) for tools/oneios-tool, which builds the board's archives (`oneios-tool kernel gen NAME`); it is
# built when missing or stale (ONEIOS_TOOL=PATH: that binary instead).
# Output: out/kernel-hosted/unixnu/oneios/<os profile>/oneios_hosted/kernel/kernel.macho, linked as
# out/kernel/current-hosted/xnu.macho. The VM kernel's build (build-linux.sh, out/kernel/current) is not touched:
# this one has its own output root and its own copy of the device repository (out/hosted-device: kernel/device as committed, with
# the hosted series added to the board's patch series), so the two never share a work tree.
#
# ONEIOS_MACHINE=native (what kernel/build-native.sh sets): the native kernel instead, XNU alone at EL1 (docs/native.md).
# The same series with CONFIG_XNU_ONEIOS_NATIVE=y and no hosted machine layer (configs/oneios_native_defconfig: 0076
# opens the OS layer to it), the archives a machine without a host takes (storage, board, aes, keystore, hid, audio, net, codec,
# gpu, display), the sealed
# kernel without the hosted fixups. Its own trees: out/kernel-native, out/native-device, out/kernel/current-native
# (NATIVE_OUT=NAME: each with -NAME). HOSTED_OUT is not asked for.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
HOST=$(uname -s)
case "$HOST" in Linux|Darwin) ;; *) echo "build-hosted: Linux or macOS only" >&2; exit 1 ;; esac
MACHINE=${ONEIOS_MACHINE:-hosted}
case "$MACHINE" in hosted|native) ;; *) echo "build-hosted: ONEIOS_MACHINE is hosted or native, not $MACHINE" >&2; exit 2 ;; esac
if [ "$MACHINE" = native ]; then
  SUFFIX=${NATIVE_OUT:+-$NATIVE_OUT}
else
# HOSTED_OUT=NAME: a second, independent work tree and output (out/kernel-hosted-NAME, out/hosted-device-NAME,
# out/kernel/current-hosted-NAME), so that several people can build and edit at once. The default tree is the lead's.
# It must be named: HOSTED_OUT=lead is the default tree (out/kernel-hosted, out/kernel/current-hosted), which is the
# lead's; a build with no name once rebuilt that tree under its owner.
[ -n "${HOSTED_OUT:-}" ] || { echo "build-hosted: set HOSTED_OUT=<your name> (your own work tree; 'lead' is the lead's)" >&2; exit 2; }
[ "$HOSTED_OUT" != lead ] || HOSTED_OUT=
SUFFIX=${HOSTED_OUT:+-$HOSTED_OUT}
fi
export KERNEL_OUT=$ROOT/out/kernel-$MACHINE$SUFFIX KERNEL_DEFCONFIG=oneios_${MACHINE}_defconfig
. "$ROOT/kernel/lib/stage.sh"
. "$ROOT/kernel/lib/host.sh"
JOBS=${JOBS:-$(host_jobs)}
T0=$(date +%s)
if [ "$HOST" = Linux ]; then
  TOOLCHAIN=${XNU_LINUX_PREFIX:-$ROOT/out/toolchain}
  "$ROOT/kernel/linux/setup-toolchain.sh"
  [ -x "$TOOLCHAIN/unixnu/CommandLineTools/usr/bin/xnu-host-cc" ] && [ "$TOOLCHAIN/unixnu" -nt "$ROOT/kernel/linux/clang-wrapper" ] || "$ROOT/kernel/linux/unixnu-layout.sh"
else
  xcode_setup build-hosted
fi
stage_sources

# Build archives in this hosted profile consume pinned reference repositories
# that are not UniXNU submodules. Fetch them before invoking the archive
# generators so a fresh checkout does not fail part-way through (notably
# NativeMetal/OriginalAcceleratorABI.h in the GPU archive).
"$ROOT/scripts/setup/get-references.sh" \
  NativeHidInput NativeMobileDisplay NativeAdreno NativeMetal AdrenoPM4 ArmSmmu4K
[ -f "$ROOT/external/NativeMetal/OriginalAcceleratorABI.h" ] || {
  echo "build-hosted: NativeMetal/OriginalAcceleratorABI.h missing after pinned reference fetch" >&2
  exit 1
}

# The device repository of this build: the committed one, the hosted defconfig, and the hosted series last.
# (two levels below the repository, as kernel/device is: the board's archive script finds drivers/ from there)
DEVICE=$ROOT/out/$MACHINE-device$SUFFIX
# The archive scripts take their sources (drivers/, kernel/patches/oneios-hosted/*) from this checkout, named here:
# found from DEVICE's resolved path they came from another checkout whenever out/ is a link into it (a worktree
# whose out/ points at the shared checkout built that checkout's older drivers/ into its kernel: no touch).
export ONEIOS_REPO=$ROOT
rm -rf "$DEVICE"; mkdir -p "$DEVICE"
git -C "$ROOT" archive HEAD kernel/device | tar -x -C "$DEVICE" --strip-components=2
cp "$ROOT/kernel/device/configs/oneios_${MACHINE}_defconfig" "$DEVICE/configs/"
sed_i "s#^\.\./\.\./\.\./\.\./external/#$ROOT/external/#" "$DEVICE/patches/xnu-12377/series"
python3 -I - "$DEVICE/board.toml" "${HOSTED_SERIES:-$ROOT/kernel/patches/oneios-hosted/series}" <<'PY'
import re, sys
path, series = sys.argv[1:]
text = open(path).read()
new, n = re.subn(r'(patch_series\s*=\s*\[[^\]]*?)\]', lambda m: m.group(1).rstrip().rstrip(',') + ', "' + series + '"]', text, count=1)
if n != 1:
    raise SystemExit('build-hosted: board.toml has no [kernel] patch_series')
open(path, 'w').write(new)
PY
# The board archives of this build only, each one more entry of board.toml's [kernel] archives, built between the two
# kernel passes by `oneios-tool kernel gen NAME` (tools/oneios-tool/crates/oneios-kernel; the sources stay in
# kernel/patches/oneios-hosted/NAME). In the order they are built:
#   storage   The disks (0004-root-mount-launchd.patch): OneiOSHostedDisk and the source IOBlockStorageDevice,
#             force-loaded by the hosted defconfig. Before the board's own entry, which is the last of the committed list.
#   board     The committed entry: drivers/ and Droid-VM's cpufreq.
#   aes       Software AES (0008-software-aes.patch): UniXNU's NativeAES under the collection's IOAESAccelerator.
#             AppleImage4's start waits for it.
#   keystore  The key store (0022-keystore.patch): UniXNU's AppleKeyStoreState with OneiOS's class keys and software
#             key wrap. launchd and MobileKeyBag look for the AppleKeyStore service; the collection's AppleSEPKeyStore
#             is left out (no Secure Enclave).
#   net       The Wi-Fi, en0 (no numbered patch: oneios_drivers_start calls oneios_net_start): the Wi-Fi controller and
#             interface under the collection's original IOEthernetController and IOEthernetInterface
#             (IONetworking-24A446-v54.json is their layout; the collection keeps Apple's IONetworkingFamily), their
#             station xnuhost's Wi-Fi bridge, and UniXNU-droidvm's virtio-net transport from xnuhost's /arm-io/net
#             (README.md in the patch folder).
#   hid       Touch, buttons and the light sensor (no numbered patch: oneios_drivers_start calls
#             oneios_hid_input_start): UniXNU's NativeHidInput and OneiOSALSDevice under the collection's original
#             IOHIDDevice (kernel/patches/oneios-hosted/hid/OriginalHidDeviceABI.h is its layout), fed by
#             UniXNU-droidvm's virtio-input transport from xnuhost's input device. backboardd takes touches from its
#             Digitizer service.
#   audio     The speaker and the microphone (no numbered patch: oneios_drivers_start calls oneios_audio_start):
#             UniXNU-droidvm's virtio-snd transport and its two IOAudio2Devices from xnuhost's /arm-io/virtio-snd.
#   gpu       The GPU (0005-gpu-3d.patch): Droid-VM's virtio-gpu 3D driver, force-loaded like the board's others.
#   display   The display (0020-display.patch): UniXNU's NativeMobileDisplay under the collection's
#             IOMobileFramebufferLegacy, with Droid-VM's probe and virtio-gpu scanout client.
#             CONFIG_XNU_NATIVE_LEGACY_DISPLAY makes UniXNU's build link
#             build/native-legacy-display/libNativeLegacyDisplay.late.a and compile bsd_init's two calls into it.
#             After the GPU's archive (the last): the scanout client uses that archive's transport.
#
# UniXNU's driver runs an archive entry that is not a script as a command, in the build tree: stage_tool
# (kernel/lib/stage.sh) builds or finds oneios-tool and puts it on PATH under that name. The generators were Python
# scripts until 2026-11-13 (build-oneios-NAME.py, copied into the device repository); their output is frozen in
# tools/oneios-tool/tests/kernelgen/golden.
stage_tool
if [ "$MACHINE" = native ]; then
  # The native kernel's archives, in the hosted order: the disks before the board's own entry, then the software AES,
  # the key store, touch and keys, the GPU's transport and the display (oneios_native_defconfig force-loads all but the
  # display, which UniXNU's build links with CONFIG_XNU_NATIVE_LEGACY_DISPLAY), and the speaker and microphone (the audio
  # archive: crosvm's virtio-snd card on its ECAM bus, xnuloader-crosvm --audio), and the Wi-Fi (the network archive: crosvm's
  # virtio-net card on that bus, xnuloader-crosvm --wifi, bridged to the phone's wlan0 by wifictl tap), and the video decoder
  # (the codec archive: crosvm's virtio-media decoder on that bus, xnuloader-crosvm --media; off without oneios-codec=1).
  awk '/^[[:space:]]*"oneios-tool kernel gen board",?[[:space:]]*$/ {
         match($0, /^[[:space:]]*/); pad = substr($0, 1, RLENGTH); n++
         printf "%s\"oneios-tool kernel gen storage\",\n%s\"oneios-tool kernel gen board\",\n", pad, pad
         printf "%s\"oneios-tool kernel gen aes\",\n%s\"oneios-tool kernel gen keystore\",\n", pad, pad
         printf "%s\"oneios-tool kernel gen hid\",\n%s\"oneios-tool kernel gen audio\",\n%s\"oneios-tool kernel gen net\",\n", pad, pad, pad
         printf "%s\"oneios-tool kernel gen codec\",\n", pad
         printf "%s\"oneios-tool kernel gen gpu\",\n%s\"oneios-tool kernel gen display\",\n", pad, pad
         next }
       { print }
       END { exit n == 1 ? 0 : 1 }' "$DEVICE/board.toml" > "$DEVICE/board.toml.native" \
    || { echo "build-hosted: board.toml archives have no single entry of the board's own" >&2; exit 1; }
  mv "$DEVICE/board.toml.native" "$DEVICE/board.toml"
else
ENTRIES=()
for name in storage board aes keystore net hid audio gpu display; do
  ENTRIES+=("oneios-tool kernel gen $name")
done
python3 -I - "$DEVICE/board.toml" "$DEVICE/configs/oneios_hosted_defconfig" "${ENTRIES[@]}" <<'PY'
import re, sys
board, config, *entries = sys.argv[1:]
# The archives: the committed list ends with the board's own entry; the hosted list takes its place.
text = open(board).read()
new, n = re.subn(r'^(\s*)"oneios-tool kernel gen board",?\s*\n(\s*\])',
                 lambda m: ''.join(f'{m.group(1)}"{entry}",\n' for entry in entries) + m.group(2), text, count=1, flags=re.M)
if n != 1:
    raise SystemExit('build-hosted: board.toml archives do not end with the board\'s own entry')
open(board, 'w').write(new)
# The archives the kernel force-loads beside the committed ones, in this order (the link order).
text = open(config).read()
for name in ('gpu', 'aes', 'keystore', 'net', 'hid', 'audio'):
    text, n = re.subn(r'^(CONFIG_XNU_EXTRA_LD_ARCHIVES="[^"\n]*)"',
                      lambda m: m.group(1) + f' -Wl,-force_load,{{tree}}/build/oneios-{name}/liboneios{name}.a"', text, count=1, flags=re.M)
    if n != 1:
        raise SystemExit('build-hosted: the hosted defconfig has no CONFIG_XNU_EXTRA_LD_ARCHIVES')
if 'CONFIG_XNU_NATIVE_LEGACY_DISPLAY' in text:
    raise SystemExit('build-hosted: the hosted defconfig already sets CONFIG_XNU_NATIVE_LEGACY_DISPLAY')
open(config, 'w').write(text.rstrip('\n') + '\nCONFIG_XNU_NATIVE_LEGACY_DISPLAY=y\n')
PY
fi
mkdir -p "$UNIXNU_OUT_DIR"
[ -e "$UNIXNU_OUT_DIR/feeds" ] || [ ! -d "$ROOT/out/kernel/unixnu/feeds" ] || ln -s "$ROOT/out/kernel/unixnu/feeds" "$UNIXNU_OUT_DIR/feeds"

run_build() {
  # macOS has xcrun, plutil, codesign and csh in place (Xcode); Linux gets UniXNU's shims there in a namespace.
  if [ "$HOST" = Darwin ]; then "$@"; return; fi
  # plutil is always UniXNU's: a Linux /usr/bin/plutil is GNUstep's (Arch: gnustep-base), which cannot convert xnu's
  # Kernel.framework Info.plist ("non-NSData data argument passed to method") and stops installhdrs.
  local binds=(--ro-bind "$TOOLCHAIN/unixnu/usrbin/plutil" /usr/bin/plutil) shim
  for shim in xcrun codesign csh; do
    [ -e "/usr/bin/$shim" ] || binds+=(--ro-bind "$TOOLCHAIN/unixnu/usrbin/$shim" "/usr/bin/$shim")
  done
  bwrap --dev-bind / / --overlay-src /usr/bin --tmp-overlay /usr/bin "${binds[@]}" -- "$@"
}
export JOBS
if [ "$HOST" = Linux ]; then
  export UNIXNU_TOOLCHAIN=$TOOLCHAIN/unixnu ONEIOS_IOS_VERSION=27.0
  export PATH=$TOOLCHAIN/unixnu/hostbin:$PATH
fi
export UNIXNU_OUT=$UNIXNU_OUT_DIR
export -f unixnu
export UNIXNU DEVICE UNIXNU_OUT_DIR
unixnu defconfig "$CONFIG_NAME"
WORK=$UNIXNU_OUT_DIR/oneios/$OS_PROFILE/${CONFIG_NAME%_defconfig}
if ! run_build bash -c "unixnu build kernel --skip-prelink -j $JOBS"; then
  echo "build-hosted: FAILED. Logs: $WORK/logs" >&2
  grep -n -m 40 -E "error:|Undefined symbols|symbol\(s\) not found|No rule to make" "$WORK/logs/kernel.log" >&2 || true
  exit 1
fi
# The kernel to boot is the sealed one: the build fills corecrypto's POST HMAC field (and writes the static_if test
# sites in their boot state) in mach.development.bcm2837.unstripped only. UniXNU's driver stages the .noctf file,
# whose HMAC field is empty: corecrypto's integrity self-test fails on it (-2074).
SEALED=$WORK/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped
[ -f "$SEALED.crypto-hmac.json" ] || { echo "build-hosted: $SEALED was not sealed" >&2; exit 1; }
CURRENT=$ROOT/out/kernel/current-$MACHINE$SUFFIX
mkdir -p "$CURRENT"
cp "$SEALED" "$CURRENT/xnu.macho"
SEALED=$CURRENT/xnu.macho
if [ "$MACHINE" = hosted ]; then
# Post-link text fixes of the hosted layer (`oneios-tool kernel hosted-fixup`, kernel/hosted-fixup.py until 2026-11-13: a
# vCPU's identity comes from xnuhost, not
# MPIDR_EL1), on the copy; then the seal again, over the changed text (the build's own seal script, which takes
# an already sealed kernel). The native kernel reads MPIDR_EL1 itself: it is booted as sealed.
"$ONEIOS_TOOL" kernel hosted-fixup "$SEALED"
( cd "$WORK/tree" && UNIXNU_XNU_ARCH=arm64e python3 scripts/seal-source-crypto-kernel.py "$SEALED" )
fi
# XNU_FILESET_REFERENCE=1: the RELEASE reference of the prelink (kernel/prelink/build.sh REFERENCE=), next to the kernel.
REF=$WORK/tree/build/xnu/fileset-reference/mach.release.bcm2837.unstripped.noctf
if [ "${XNU_FILESET_REFERENCE:-0}" = 1 ]; then
  [ -f "$REF" ] || { echo "build-hosted: XNU_FILESET_REFERENCE=1 left no $REF" >&2; exit 1; }
  cp "$REF" "$CURRENT/reference.macho"
  echo "build-hosted: reference $CURRENT/reference.macho ($(file_size "$REF") bytes)"
fi
echo "build-hosted: $CURRENT/xnu.macho ($MACHINE, $(file_size "$SEALED") bytes, sha256 $(sha256 "$SEALED" | cut -c1-16)...) in $(( $(date +%s) - T0 )) s on $HOST"
