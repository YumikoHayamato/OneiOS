#!/bin/bash
# Build the Metal plug-in, NativeAdrenoMetal.bundle, for iOS 27 arm64e on macOS (Xcode) or Linux, for this guest's shared cache.
#
#   scripts/bg.sh metal userland/metal/build.sh            everything (about 25 minutes the first time)
#   userland/metal/build.sh stage|turnip|plugin|finish|addon   one step
#
# The stack (docs/gpu.md section 7.2): ONEIOS_METAL_STACK names the pins the three parts are built from and the folder of
# OneiOS's patches that goes with them.
#   555b13d (the default)  the GPU stack Droid-VM booted iPadOS 27 on (Droid-VM/UniXNU 421e982, 2026-11-16):
#                          external/Droid-VM-UniXNU-8789b8d (its scripts, its board patches of metal2vulkan carried to
#                          555b13d, its Mesa list), external/NativeMetal-935c468, external/metal2vulkan-555b13d;
#                          OneiOS's patches-555b13d/ and Switches-555b13d.inc; staged in out/metal-555b13d
#   624bab3                take18's stack, the fallback: external/Droid-VM-UniXNU 62d0b11 (the kernel's base),
#                          external/NativeMetal b0d6a07, external/metal2vulkan 624bab3; patches/ and Switches.inc; staged
#                          in out/metal. ONEIOS_METAL_STACK=624bab3 builds what ci/metal-plugin 68f7072 built.
# Both use external/mesa-droidvm cecc0e9, NativeAdreno, NativeMobileDisplay and AdrenoPM4 (the base pins them the same).
#
# What is built (docs/gpu.md section 7, docs/userland.md 7.1):
#   libvulkan_freedreno.dylib   Mesa's Turnip (external/mesa-droidvm cecc0e9 + the base's patches/mesa, the list of
#                               scripts/data/adreno-compiler-inputs.json, + the stack's patches*/mesa) with the darwin
#                               virtio-gpu backend, a Mach-O dylib
#   libadreno_metal_bridge.a    metal2vulkan (the stack's pin + NativeMetal bridge/patches + the base's
#                               patches/metal2vulkan + the stack's patches*/metal2vulkan) behind NativeMetal's C ABI; Rust,
#                               arm64e-apple-ios, std from source
#   NativeAdrenoMetal           the stack's NativeMetal + the stack's patches*/NativeMetal: the plug-in and the Metal
#                               frontend, linked with the bridge (GpuScanoutPresenter.h includes the wire header of
#                               external/NativeMobileDisplay)
#   -> out/userland/NativeAdrenoMetal.bundle (Info.plist, NativeAdrenoMetal, libvulkan_freedreno.dylib, oneios-target)
#      out/userland/NativeAdrenoMetal.gpuaddon: the bundle as the kernel.kc.gpuaddon sidecar (build.sh addon makes it alone)
#      out/userland/NativeAdrenoMetal.build.json: the SDK and stubs used, the pins, sizes and CDHashes
#
# How: the kernel base's own build scripts (Droid-VM-UniXNU scripts/build-turnip-darwin.py, build-adreno-metal-bridge.py,
# build-adreno-metal-plugin.py, --target ios27) run from a staged copy of its tree (out/metal/root), with this folder's
# shim/ in front of PATH: xcrun's clang is scripts/ios/ios-clang (so every link is against the guest's stubs), codesign
# is scripts/ios/ios-sign.py (byte for byte Apple's codesign for a thin file; no bundle seal), strip and nm the linker's
# siblings. On macOS ios-clang is Xcode's clang and ld, and xcrun's other answers are Xcode's; on Linux the cross toolchain.
# The staged copies differ from upstream in the few lines `stage` prints. Before the compile, the plug-in's shared
# cache addresses are replaced with this guest's (hooks/<product>-<build>.json) and three OneiOS files are appended for
# backboardd (drivers/brightness/BacklightMode.inc; DisplayWide.inc: half floats with oneios-display-wide=1; ScreenRecord.inc:
# a CPU scaler for screen recording with oneios-screenrec=1, left out by the ONEIOS_METAL_SKIP word `screenrec`), and for every
# process the stack's Switches file (the boot arguments of the patches below); `finish` then marks the bundle with
# `oneios-target`, which is what lets userland/roottweak place it.
#
# OneiOS's patches (docs/gpu.md 7.1, 7.2): NativeMetal, metal2vulkan and mesa under the stack's folder (patches/ for
# 624bab3, patches-555b13d/ for 555b13d), NNNN-<name>.patch, each file's header gives its upstream commit, what, why and
# its switch. `stage` applies the NativeMetal ones to the
# staged drivers/AdrenoMetal, lists the metal2vulkan ones after the base's in the staged bridge script and the mesa ones
# after the recorded ones in the staged adreno-compiler-inputs.json (so the base's scripts apply and check them as their
# own), then checks, before any compile, that the base's scripts find what they edit and that the three lists apply.
# ONEIOS_METAL_SKIP="WORD ..." leaves patches out; a word is a patch's name with or without its number and .patch, alone
# or after its folder (idle-trim-timer-lives, 0002-idle-trim-timer-lives, NativeMetal/0002-idle-trim-timer-lives.patch);
# a word that names no patch stops the build. Each patch applies with or without any other, except one whose header says
# "Applies only after: FILE ..." (patches of its folder): it is left out with them, and said so. The build record
# (NativeAdrenoMetal.build.json "oneios_patches") lists those taken (and "ScreenRecord.inc" when it is appended). A changed list puts the staged Mesa and metal2vulkan
# trees back at their pins first (build/mesa-patches.stamp, build/m2v-patches.stamp).
#
# Needs: scripts/setup/ios-toolchain.sh says "ok"; scripts/setup/get-references.sh (NativeAdreno, NativeMobileDisplay,
# AdrenoPM4, mesa-droidvm and, for 555b13d, NativeMetal-935c468, metal2vulkan-555b13d, Droid-VM-UniXNU-8789b8d; for
# 624bab3, NativeMetal, metal2vulkan and kernel/fetch.sh Droid-VM-UniXNU); meson, ninja, glslang, glslc, spirv-val, bison, flex, llvm-as,
# llvm-objcopy, llvm-ar, python3 with mako and yaml; rustup with 1.98.0 and its rust-src
# (rustup toolchain install 1.98.0 && rustup +1.98.0 component add rust-src).
# macOS: brew install ninja glslang shaderc spirv-tools bison llvm (LLVM's tools are found in `brew --prefix llvm`/bin,
# Homebrew's bison is put before /usr/bin's 2.3); uv pip install --python "$(command -v python3)" meson mako pyyaml packaging.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE=$ROOT/userland/metal
. "$ROOT/scripts/ios/env.sh"
TARGET_ID=${ONEIOS_METAL_TARGET:-iPhone18,2 24A446}
HOOKS=$HERE/hooks/${TARGET_ID/ /-}.json
EXT=${ONEIOS_METAL_EXT:-$ROOT/external}
STACK=${ONEIOS_METAL_STACK:-555b13d}
case "$STACK" in
  # BASE_NAME: the Droid-VM-UniXNU checkout whose scripts and patches build the stack; LAST_BOARD: the last name of its
  # bridge script's metal2vulkan list, after which OneiOS's are listed
  555b13d) BASE_NAME=Droid-VM-UniXNU-8789b8d NM_NAME=NativeMetal-935c468 M2V_NAME=metal2vulkan-555b13d
           PATCH_DIR=patches-555b13d SWITCHES=Switches-555b13d.inc WORK_NAME=metal-555b13d
           LAST_BOARD=0031-backing-cover-switch-and-pool-report.patch ;;
  624bab3) BASE_NAME=Droid-VM-UniXNU NM_NAME=NativeMetal M2V_NAME=metal2vulkan
           PATCH_DIR=patches SWITCHES=Switches.inc WORK_NAME=metal
           LAST_BOARD=0038-settled-gpu-writes-read-at-commit.patch ;;
  *) echo "metal: ONEIOS_METAL_STACK=$STACK names no stack (555b13d, 624bab3)" >&2; exit 2 ;;
esac
W=${METAL_WORK:-$ROOT/out/$WORK_NAME}
R=$W/root
BASE=$EXT/$BASE_NAME
OUT=${METAL_OUT:-$ROOT/out/userland}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
export PATH="$HERE/shim:$PATH"
AR=llvm-ar
if [ "$IOS_HOST" = Darwin ]; then
  # Homebrew's LLVM is keg-only: its llvm-ar here, its llvm-as and llvm-objcopy found by the base's scripts themselves
  LLVM_BIN=${LLVM_BIN:-$(brew --prefix llvm 2>/dev/null || echo /opt/homebrew/opt/llvm)/bin}
  if [ -x "$LLVM_BIN/llvm-ar" ]; then AR=$LLVM_BIN/llvm-ar; else AR=$(/usr/bin/xcrun --sdk iphoneos --find ar); fi
fi
# cc-rs (the Rust crates' C and assembly) and rustc's own probes: the same compiler and SDK. On macOS SDKROOT stays
# unset: the crates' build scripts are linked for the Mac with its SDK, and rustc asks xcrun (the shim) for iOS's.
[ "$IOS_HOST" = Darwin ] || export SDKROOT=$IOS_SDK
export CC_arm64e_apple_ios=$IOS_DIR/ios-clang CXX_arm64e_apple_ios=$IOS_DIR/ios-clang++
export AR_arm64e_apple_ios=$AR CARGO_TARGET_ARM64E_APPLE_IOS_LINKER=$IOS_DIR/ios-clang

say() { echo "metal: $*"; }

# OneiOS's patches of a tree (NativeMetal, metal2vulkan, mesa), in order, one path a line, without those ONEIOS_METAL_SKIP
# names and those whose header's "Applies only after:" names one left out (said on stderr); check_skip stops the build at a
# word that names no patch.
PATCHES=$HERE/$PATCH_DIR
patch_words() { # FILE TREE: the words that name it
  local name
  name=$(basename "$1" .patch)
  echo "$name $name.patch ${name#[0-9][0-9][0-9][0-9]-} $2/$name $2/$name.patch $2/${name#[0-9][0-9][0-9][0-9]-}"
}
skipped() { # FILE TREE: whether ONEIOS_METAL_SKIP names it
  local w n
  for w in ${ONEIOS_METAL_SKIP:-}; do
    for n in $(patch_words "$1" "$2"); do [ "$w" = "$n" ] && return 0; done
  done
  return 1
}
patches_of() { # TREE
  local p need left=" "
  for p in "$PATCHES/$1"/[0-9][0-9][0-9][0-9]-*.patch; do
    [ -f "$p" ] || continue
    if skipped "$p" "$1"; then
      echo "metal: left out (ONEIOS_METAL_SKIP): $1/$(basename "$p")" >&2
      left="$left$(basename "$p") "
      continue
    fi
    for need in $(sed -n '/^diff /q; s/^Applies only after: //p' "$p"); do
      case "$left" in
        *" $need "*)
          echo "metal: left out (it applies only after $1/$need, left out): $1/$(basename "$p")" >&2
          left="$left$(basename "$p") "
          continue 2;;
      esac
    done
    echo "$p"
  done
}
check_skip() {
  local w t p n found need
  for t in NativeMetal metal2vulkan mesa; do
    for p in "$PATCHES/$t"/[0-9][0-9][0-9][0-9]-*.patch; do
      [ -f "$p" ] || continue
      for need in $(sed -n '/^diff /q; s/^Applies only after: //p' "$p"); do
        [ -f "$PATCHES/$t/$need" ] || { echo "metal: $t/$(basename "$p") applies only after $t/$need, which is not there" >&2; exit 1; }
      done
    done
  done
  for w in ${ONEIOS_METAL_SKIP:-}; do
    found=0
    [ "$w" = screenrec ] && found=1   # ScreenRecord.inc, appended in stage
    for t in NativeMetal metal2vulkan mesa; do
      for p in "$PATCHES/$t"/[0-9][0-9][0-9][0-9]-*.patch; do
        [ -f "$p" ] || continue
        for n in $(patch_words "$p" "$t"); do [ "$w" = "$n" ] && found=1; done
      done
    done
    [ $found = 1 ] || { echo "metal: ONEIOS_METAL_SKIP names no patch of userland/metal/$PATCH_DIR: $w" >&2; exit 1; }
  done
}

stage() {
  [ -f "$HOOKS" ] || { echo "metal: no $HOOKS: the plug-in's shared cache addresses are not known for $TARGET_ID" >&2; exit 1; }
  for d in "$BASE_NAME" "$NM_NAME" NativeAdreno NativeMobileDisplay AdrenoPM4 "$M2V_NAME" mesa-droidvm; do
    [ -d "$EXT/$d" ] || { echo "metal: no external/$d for the stack $STACK (scripts/setup/get-references.sh $d; kernel/fetch.sh for Droid-VM-UniXNU)" >&2; exit 1; }
  done
  say "stack $STACK: $BASE_NAME $(git -C "$BASE" rev-parse --short HEAD), $NM_NAME $(git -C "$EXT/$NM_NAME" rev-parse --short HEAD), $M2V_NAME $(git -C "$EXT/$M2V_NAME" rev-parse --short HEAD); OneiOS's patches userland/metal/$PATCH_DIR, $SWITCHES"
  check_skip
  mkdir -p "$R/drivers" "$R/kits" "$R/.cache/gpu-reference" "$R/.cache/mesa-venv/bin" "$R/.cache/mesa-tools/bin" "$R/build"
  rsync -a --delete "$BASE/scripts/" "$R/scripts/"
  rsync -a --delete "$BASE/patches/" "$R/patches/"
  rsync -a --delete --exclude .git "$EXT/$NM_NAME/" "$R/drivers/AdrenoMetal/"
  rsync -a --delete --exclude .git "$EXT/NativeAdreno/" "$R/drivers/Adreno740/"
  rsync -a --delete --exclude .git "$EXT/NativeMobileDisplay/" "$R/drivers/GopFramebuffer/"
  rsync -a --delete --exclude .git "$EXT/AdrenoPM4/" "$R/kits/AdrenoPM4/"
  # OneiOS's patches. The staged drivers/AdrenoMetal is no git checkout: GIT_CEILING_DIRECTORIES keeps git apply from
  # taking an enclosing repository's (this checkout's, out/ being in it), inside which it would skip every file silently.
  local p taken=$R/build/oneios-patches.txt m2v_names="" mesa_names=""
  : > "$taken"
  while read -r p; do
    GIT_CEILING_DIRECTORIES="$R/drivers" git -C "$R/drivers/AdrenoMetal" apply "$p" ||
      { echo "metal: $PATCH_DIR/NativeMetal/$(basename "$p") does not apply to external/$NM_NAME" >&2; exit 1; }
    echo "NativeMetal/$(basename "$p")" >> "$taken"
    say "staged drivers/AdrenoMetal: $PATCH_DIR/NativeMetal/$(basename "$p")"
  done < <(patches_of NativeMetal)
  mkdir -p "$R/patches/oneios/metal2vulkan" "$R/patches/oneios/mesa"
  while read -r p; do
    cp "$p" "$R/patches/oneios/metal2vulkan/"
    m2v_names="$m2v_names $(basename "$p")"
    echo "metal2vulkan/$(basename "$p")" >> "$taken"
  done < <(patches_of metal2vulkan)
  while read -r p; do
    cp "$p" "$R/patches/oneios/mesa/"
    mesa_names="$mesa_names $(basename "$p")"
    echo "mesa/$(basename "$p")" >> "$taken"
  done < <(patches_of mesa)
  # The Mesa list: the recorded patches, then ours (build-turnip-darwin.py checks the source against the whole list)
  # shellcheck disable=SC2086
  python3 - "$R" $mesa_names <<'EOF'
import json, sys
root, names = sys.argv[1], sys.argv[2:]
path = root + '/scripts/data/adreno-compiler-inputs.json'
data = json.load(open(path))
data['patches'] += ['patches/oneios/mesa/' + name for name in names]
open(path, 'w').write(json.dumps(data, indent=2) + '\n')
for name in names:
    print(f'metal: staged scripts/data/adreno-compiler-inputs.json: patches/oneios/mesa/{name} after the recorded patches')
EOF
  # The staged Mesa is the pin with the list applied; a list or a patch that changed since (another ONEIOS_METAL_SKIP,
  # an edited patch) puts it back at the pin first.
  local mesa_stamp=$R/build/mesa-patches.stamp mesa_want apply_mesa=0
  mesa_want=$(python3 - "$R" <<'EOF'
import hashlib, json, sys
root = sys.argv[1]
digest = hashlib.sha256()
for patch in json.load(open(root + '/scripts/data/adreno-compiler-inputs.json'))['patches']:
    digest.update(patch.encode() + b'\0' + open(root + '/' + patch, 'rb').read())
print(digest.hexdigest())
EOF
)
  if [ -d "$R/.cache/mesa-work/.git" ] && [ "$(cat "$mesa_stamp" 2>/dev/null)" != "$mesa_want" ]; then
    say "the Mesa patch list changed since the last stage: $R/.cache/mesa-work back at the pin, the list applied again"
    git -C "$R/.cache/mesa-work" reset -q --hard
    git -C "$R/.cache/mesa-work" clean -fdq
    apply_mesa=1
  fi
  ln -sf "$(command -v meson)" "$R/.cache/mesa-venv/bin/meson"
  # macOS's own bison is 2.3: Homebrew's goes first (the base's scripts put .cache/mesa-tools/bin first in PATH)
  if [ "$IOS_HOST" = Darwin ] && [ -x "$(brew --prefix bison 2>/dev/null)/bin/bison" ]; then
    ln -sf "$(brew --prefix bison)/bin/bison" "$R/.cache/mesa-tools/bin/bison"
  fi
  if [ ! -d "$R/.cache/mesa-work/.git" ]; then
    git clone -q "$EXT/mesa-droidvm" "$R/.cache/mesa-work"
    apply_mesa=1
  fi
  if [ $apply_mesa = 1 ]; then
    python3 - "$R" <<'EOF'
import json, subprocess, sys
root = sys.argv[1]
for patch in json.load(open(root + '/scripts/data/adreno-compiler-inputs.json'))['patches']:
    subprocess.run(['git', '-C', root + '/.cache/mesa-work', 'apply', root + '/' + patch], check=True)
EOF
    echo "$mesa_want" > "$mesa_stamp"
    say "Mesa $(git -C "$R/.cache/mesa-work" rev-parse --short HEAD) with the recorded patches and OneiOS's in $R/.cache/mesa-work"
  fi
  [ -d "$R/.cache/gpu-reference/metal2vulkan/.git" ] || git clone -q "$EXT/$M2V_NAME" "$R/.cache/gpu-reference/metal2vulkan"
  # The staged scripts and sources: what differs from upstream, each said.
  # clang 23's -Wunused-but-set-global: Xcode 27.2's clang 21 does not know it (and -Werror makes an unknown
  # warning option an error), so the plug-in script is told about it only when the compiler knows it
  local knows=0
  echo 'int x;' | "$IOS_DIR/ios-clang" -Werror -Wno-error=unused-but-set-global -x c -c - -o /dev/null 2>/dev/null && knows=1
  # shellcheck disable=SC2086
  python3 - "$R" "$HOOKS" "$JOBS" "$knows" "$STACK" "$LAST_BOARD" $m2v_names <<'EOF'
import json, re, sys
from pathlib import Path
root, hooks, jobs, knows = Path(sys.argv[1]), json.load(open(sys.argv[2])), sys.argv[3], sys.argv[4] == '1'
stack, last_board, m2v = sys.argv[5], sys.argv[6], sys.argv[7:]

def edit(path, pairs):
    text = (root / path).read_text()
    for old, new, why in pairs:
        if text.count(old) != 1:
            sys.exit(f'metal: {path} no longer has exactly one {old!r}')
        text = text.replace(old, new)
        print(f'metal: staged {path}: {why}')
    (root / path).write_text(text)

edit('scripts/build-turnip-darwin.py', [
    ("env.update(CC=find('clang'), CXX=find('clang++'), CFLAGS='', CXXFLAGS='', CPPFLAGS='', LDFLAGS='')",
     "env.update(CC='clang', CXX='clang++', CFLAGS='', CXXFLAGS='', CPPFLAGS='', LDFLAGS='')",
     "meson's build-machine compiler is the host's clang (on a Mac xcrun's clang is both)"),
    ("'-j6'", "'-j%s'" % jobs, 'ninja jobs'),
])
edit('scripts/build-adreno-metal-bridge.py', [
    ('timeout=1800)', 'timeout=10800)', 'a longer limit for the fat-LTO Rust build'),
] + ([
    # OneiOS's metal2vulkan patches after the board's: the script applies and checks them as its own (pin + a prefix)
    ("    '%s']]\n" % last_board,
     ("    '%s']] + [ROOT / 'patches/oneios/metal2vulkan' / name for name in [\n" % last_board) +
     "    " + ', '.join(repr(name) for name in m2v) + "]]\n",
     "OneiOS's metal2vulkan patches after the board's: " + ', '.join(m2v)),
] if m2v else []))
# TurnipReplay.h is recorded on a Mac's host simulation, and NativeAdrenoUserTest.h includes it only when it exists: 62d0b11's
# script loses the line that records it; 8789b8d's has ADRENO_METAL_REPLAY=0 for that (build.sh's plugin step sets it).
edit('scripts/build-adreno-metal-plugin.py', ([
    ("subprocess.run(['python3', ROOT / 'scripts/gen-turnip-replay.py'], check=True)\n", '',
     "no TurnipReplay.h: it is recorded on a Mac's host simulation, and NativeAdrenoUserTest.h includes it only when it exists"),
] if stack == '624bab3' else []) + ([
    ("'-Wall', '-Wextra', '-Werror',", "'-Wall', '-Wextra', '-Werror', '-Wno-error=unused-but-set-global',",
     "clang 23's -Wunused-but-set-global (gZeroRest), which Xcode's clang does not have, is not an error"),
] if knows else []))
# The frontend at b0d6a07 has the caller-by-framework answer of supportsFamily: for RenderBox and WebGPU; the base's
# plug-in script inserts its own, newer one (ANGLE too) before installAnswers and the two collide. The frontend's
# goes, the script's is inserted.
# (NativeMetal 935c468 and Droid-VM-UniXNU 8789b8d: the frontend's own answer is the only one, nothing to remove.)
if stack == '624bab3':
    frontend = root / 'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm'
    text = frontend.read_text()
    begin, end = 'static BOOL (*gFamilyAnswer)(id,SEL,NSInteger);\n', 'static void installAnswers(Class cls) {\n'
    if text.count(begin) != 1 or text.count(end) != 1 or text.index(begin) > text.index(end):
        sys.exit('metal: NativeBridgeMetalFrontend.mm: the supportsFamily: caller block is not where it was')
    frontend.write_text(text[:text.index(begin)] + text[text.index(end):])
    print("metal: staged NativeBridgeMetalFrontend.mm: its supportsFamily: caller block removed; build-adreno-metal-plugin.py inserts the base's (RenderBox, WebGPU, ANGLE)")
# The plug-in's shared cache addresses: upstream's are the iPad17,3 24A446 cache's.
plugin = root / 'drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm'
text = plugin.read_text()
anchor, branch = hooks['anchor'], hooks['branch']
for item in [anchor] + hooks['replace']:
    if not re.search(re.escape(item['upstream']) + r'(?![0-9a-fA-F])', text):
        sys.exit(f'metal: NativeAdrenoMetalPlugin.mm has no {item["upstream"]}: the hooks file is for another revision')
    text, n = re.subn(re.escape(item['upstream']) + r'(?![0-9a-fA-F])', item['value'], text)
    if item['upstream'] != item['value']:
        print(f'metal: staged NativeAdrenoMetalPlugin.mm: {item["upstream"]} -> {item["value"]} ({n} places): {item.get("what", item.get("symbol"))}')
for fixed in (branch['address'], f'word != {branch["word"]}'):
    if fixed not in text:
        sys.exit(f'metal: NativeAdrenoMetalPlugin.mm has no {fixed!r}')
# Every address the arm64e code uses must now be one the hooks file names. b0d6a07's plug-in also holds addresses of the iOS 17
# cache outside its arm64e code, so only that code is looked at; 935c468's holds none but the arm64e code's (the whole file is
# looked at: a new address anywhere stops the stage until the hooks file names it).
known = {i['value'] for i in hooks['replace']} | {i['value'] for i in hooks['same']} | {anchor['value']}
if stack == '624bab3':
    arm64e_only = text[text.index('setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION"'):]
    arm64e_only = arm64e_only[:arm64e_only.index('#endif')] + text[text.index('static unsigned patchFramebufferBranch'):text.index('static unsigned gReports')]
else:
    if '#if defined(__arm64e__)' not in text or 'static unsigned patchFramebufferBranch' not in text:
        sys.exit('metal: NativeAdrenoMetalPlugin.mm: the arm64e code that uses the shared cache addresses is not where it was')
    arm64e_only = text
unknown = sorted(set(re.findall(r'0x1[0-9a-f]{8}\b', arm64e_only)) - known)
if unknown:
    sys.exit('metal: shared cache addresses in the arm64e code that the hooks file does not name: ' + ', '.join(unknown))
plugin.write_text(text)
EOF
  # drivers/brightness/BacklightMode.inc (owner: brightness): backboardd's CoreBrightness gets BLControl, not the
  # VMBLControl that vmm-present selects, so the slider reaches the backlight service. The plug-in is the OneiOS code
  # that runs in backboardd; the file is appended whole, so its constructor is in the plug-in's image.
  cat "$ROOT/drivers/brightness/BacklightMode.inc" >> "$R/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm"
  say "staged NativeAdrenoMetalPlugin.mm: drivers/brightness/BacklightMode.inc appended (backboardd: initBLControl: mode 3 -> 1)"
  # userland/metal/DisplayWide.inc (owner: display): with the kernel's oneios-display-wide=1 (xnuhost's
  # display-format=rgba16f) backboardd's framebuffer is half floats ('RGhA'), which the display path then keeps whole.
  cat "$HERE/DisplayWide.inc" >> "$R/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm"
  say "staged NativeAdrenoMetalPlugin.mm: userland/metal/DisplayWide.inc appended (backboardd: CA_FORCE_FP16_FRAMEBUFFER with oneios-display-wide=1)"
  # userland/metal/ScreenRecord.inc (owner: display; /mnt/giochi/Linux/oneios/screenrec README): screen recording. backboardd's
  # clone of the screen into replayd's buffers fails on the GPU way; with no scaler service there is no other. A CPU
  # IOSurfaceAccelerator in backboardd and the scaler way opened while a display is a clone. Opt-in experiment:
  # oneios-screenrec=1 (nothing happens without it). The ONEIOS_METAL_SKIP word `screenrec` leaves the file out.
  case " ${ONEIOS_METAL_SKIP:-} " in
    *" screenrec "*) say "left out (ONEIOS_METAL_SKIP): userland/metal/ScreenRecord.inc" ;;
    *) cat "$HERE/ScreenRecord.inc" >> "$R/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm"
       echo "ScreenRecord.inc" >> "$taken"
       say "staged NativeAdrenoMetalPlugin.mm: userland/metal/ScreenRecord.inc appended (backboardd: CPU scaler for screen recording with oneios-screenrec=1)" ;;
  esac
  # userland/metal/Switches.inc: the boot arguments of OneiOS's patches (oneios-surface-use=, oneios-idle-trim=0, ...),
  # as environment variables before the process opens its device; the idle tick of Turnip's BO cache trim.
  cat "$HERE/$SWITCHES" >> "$R/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm"
  say "staged NativeAdrenoMetalPlugin.mm: userland/metal/$SWITCHES appended (the boot arguments of userland/metal/$PATCH_DIR)"
  # The staged metal2vulkan is the pin with a prefix of the bridge script's list applied (the script checks it); a list or
  # a patch that changed since puts it back at the pin.
  local m2v=$R/.cache/gpu-reference/metal2vulkan m2v_stamp=$R/build/m2v-patches.stamp m2v_want
  m2v_want=$(python3 - "$R" <<'EOF'
import hashlib, sys
from pathlib import Path
root = Path(sys.argv[1])
source = (root / 'scripts/build-adreno-metal-bridge.py').read_text()
listed = source[source.index('PATCHES = ['):source.index('\ndef git_tree')]
names = {'ROOT': root}
exec(listed, names)
digest = hashlib.sha256(listed.encode())
for patch in names['PATCHES']:
    digest.update(str(patch.relative_to(root)).encode() + b'\0' + patch.read_bytes())
print(digest.hexdigest())
EOF
)
  if [ "$(cat "$m2v_stamp" 2>/dev/null)" != "$m2v_want" ]; then
    if [ -n "$(git -C "$m2v" status --porcelain)" ]; then
      say "the metal2vulkan patch list changed since the last stage: $m2v back at the pin (the bridge script applies the list)"
      git -C "$m2v" reset -q --hard
      git -C "$m2v" clean -fdq
    fi
    echo "$m2v_want" > "$m2v_stamp"
  fi
  # Before any compile: the base's plug-in script finds every line it edits in the staged sources (the slice of it that
  # only reads them), the staged Mesa is the pin with its whole list (build-turnip-darwin.py's own check), and the bridge
  # script applies its list to the pin (its --manifest-only stops before cargo).
  python3 - "$R" <<'EOF'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
source = (root / 'scripts/build-adreno-metal-plugin.py').read_text()
exec(source[source.index('ADDED_METHODS = ['):source.index("staged_plugin = OUT / 'NativeAdrenoMetalPlugin.mm'")], {'ROOT': root})
print("metal: checked: build-adreno-metal-plugin.py finds every line it edits in the staged frontend and plug-in")
sys.path.insert(0, str(root / 'scripts'))
from mesa_patch_identity import validate_recorded_patches
pins = json.loads((root / 'scripts/data/adreno-compiler-inputs.json').read_text())
validate_recorded_patches(root, root / '.cache/mesa-work', pins)
print(f"metal: checked: the staged Mesa is {pins['mesa']['commit'][:7]} with its {len(pins['patches'])} listed patches")
EOF
  (cd "$R" && python3 scripts/build-adreno-metal-bridge.py --target ios27 --manifest-only >/dev/null) ||
    { echo "metal: the bridge script refused the staged metal2vulkan or its patch list" >&2; exit 1; }
  say "checked: the bridge script applied its $(python3 - "$R" <<'EOF'
import sys
from pathlib import Path
root = Path(sys.argv[1])
source = (root / 'scripts/build-adreno-metal-bridge.py').read_text()
names = {'ROOT': root}
exec(source[source.index('PATCHES = ['):source.index('\ndef git_tree')], names)
print(len(names['PATCHES']))
EOF
) metal2vulkan patches to the pin; OneiOS's patches taken: $(wc -l < "$taken")"
}

turnip() {
  (cd "$R" && python3 scripts/build-turnip-darwin.py --target ios27)
}

plugin() {
  # ADRENO_METAL_REPLAY=0: no TurnipReplay.h (8789b8d's script; 62d0b11's staged copy has no such line at all)
  (cd "$R" && ADRENO_METAL_REPLAY=0 python3 scripts/build-adreno-metal-plugin.py --target ios27)
}

finish() {
  local built=$R/build/adreno-metal-plugin27/NativeAdrenoMetal.bundle bundle=$OUT/NativeAdrenoMetal.bundle
  [ -f "$built/NativeAdrenoMetal" ] && [ -f "$built/libvulkan_freedreno.dylib" ] || { echo "metal: no built bundle in $built" >&2; exit 1; }
  mkdir -p "$OUT"
  rm -rf "$bundle"
  cp -r "$built" "$bundle"
  # The file userland/roottweak looks for: this bundle's addresses are this guest's.
  echo "$TARGET_ID" > "$bundle/oneios-target"
  "$IOS_DIR/macho-check.py" --local "$bundle/libvulkan_freedreno.dylib" "$bundle/NativeAdrenoMetal" "$bundle/libvulkan_freedreno.dylib" | tee "$OUT/NativeAdrenoMetal.check.txt"
  python3 - "$ROOT" "$R" "$bundle" "$OUT/NativeAdrenoMetal.build.json" "$HOOKS" "$TARGET_ID" "$EXT" "$STACK" \
    "$NM_NAME" "$M2V_NAME" mesa-droidvm "$BASE_NAME" NativeAdreno AdrenoPM4 NativeMobileDisplay <<'EOF'
import json, os, subprocess, sys
root, staged, bundle, record, hooks, target, ext, stack = sys.argv[1:9]
source_names = sys.argv[9:]
sys.path.insert(0, os.path.join(root, 'userland'))
from roottweak import macho
def head(path):
    return subprocess.run(['git', '-C', path, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
files = {}
for name in ('NativeAdrenoMetal', 'libvulkan_freedreno.dylib'):
    data = open(os.path.join(bundle, name), 'rb').read()
    info = macho.describe(data)[0]
    files[name] = {'bytes': len(data), 'cdhash': info['cdhash'], 'arch': info['arch'], 'page_size': info['page_size'],
                   'identifier': info['identifier'], 'minos': info['minos']}
env = os.environ
tbd = os.path.join(env['IOS_TBD'], 'tbd.json')
json.dump({
    'bundle': 'NativeAdrenoMetal.bundle', 'target': target, 'files': files,
    'cdhashes': sorted(f['cdhash'] for f in files.values()),
    'sdk': {'path': env['IOS_SDK'], 'version': env['IOS_SDK_VERSION']},
    'stubs': json.load(open(tbd)) if os.path.exists(tbd) else None,
    'deployment_target': env['IOS_MIN'], 'clang': subprocess.run([env['IOS_CLANG'], '--version'], capture_output=True, text=True).stdout.splitlines()[0],
    'ld64': env['IOS_LD_VERSION'] or None,
    'linker': (subprocess.run([env['IOS_LD'], '-v'], capture_output=True, text=True).stderr.splitlines() or [''])[0].strip(),
    'rustc': subprocess.run(['rustc', '+1.98.0', '--version'], capture_output=True, text=True).stdout.strip(),
    'stack': stack,
    'sources': {name: head(os.path.join(ext, name)) for name in source_names},
    'hooks': os.path.relpath(hooks, root), 'check': 'macho-check.py passed against the stubs above',
    'oneios_patches': open(os.path.join(staged, 'build/oneios-patches.txt')).read().split()
                      if os.path.exists(os.path.join(staged, 'build/oneios-patches.txt')) else None,
    'oneios_metal_skip': env.get('ONEIOS_METAL_SKIP', '').split(),
}, open(record, 'w'), indent=2)
print('metal: built for', target, 'from the stack', stack)
for name, f in files.items():
    print(f'metal:   {name}: {f["bytes"]} bytes, CDHash {f["cdhash"]} ({f["arch"]}, {f["page_size"]}-byte pages)')
print('metal:   SDK', env['IOS_SDK_VERSION'], env['IOS_SDK'])
EOF
  say "bundle $bundle; record $OUT/NativeAdrenoMetal.build.json"
}

# The bundle as a GPU addon: the sidecar kernel.kc.gpuaddon (format NGPUADD2, the format of Droid-VM-UniXNU's
# make-gpu-addon.py). xnuhost hands it to the kernel, which writes it into /private/var/ngpu before its addon mount
# (0031-gpu-addon-mount.patch), so the Data store survives a rebuilt plug-in. The bundle's CDHashes must be in the
# image's overlay/userland/owner-trust.json all the same; this is for changing the plug-in without a new Data seed.
# ONEIOS_ADDON_DEBUG="NAME NAME=TEXT ..." (docs/gpu.md 7.2) adds debug/NAME to the addon (empty, or holding TEXT and a new line):
# the files under /private/var/ngpu/debug that the base's plug-in and runtime read as each process opens its device, their A/B
# switches (no-scheduled-pacing, keep-room-mib=16, dont-care-mask=5, full-barriers ...). None without it. Not tried on a phone.
addon() {
  local bundle=$OUT/NativeAdrenoMetal.bundle
  [ -f "$bundle/NativeAdrenoMetal" ] || { echo "metal: no bundle in $OUT: run the build first" >&2; exit 1; }
  # shellcheck disable=SC2086
  python3 -I - "$bundle" "$OUT/NativeAdrenoMetal.gpuaddon" ${ONEIOS_ADDON_DEBUG:-} <<'EOF2'
import os, re, struct, sys, zlib
bundle, output = sys.argv[1:3]
files = []
for current, dirs, names in os.walk(bundle):
    dirs.sort()
    for name in sorted(names):
        path = os.path.join(current, name)
        rel = os.path.relpath(path, bundle).replace(os.sep, '/')
        files.append(('addons/NativeAdrenoMetal.bundle/' + rel, 0o755 if os.access(path, os.X_OK) else 0o644, open(path, 'rb').read()))
files.append(('addons/NativeAdrenoMetal.publish', 0o644, b'1\n'))
debug = {}
for word in sys.argv[3:]:
    name, _, text = word.partition('=')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', name):
        sys.exit(f'metal: ONEIOS_ADDON_DEBUG: {name!r} is no file name')
    debug.setdefault(name, []).append(text)   # a name given again adds a line (env=A=1 env=B=2: two lines of debug/env)
for name, lines in debug.items():
    files.append(('debug/' + name, 0o644, ''.join(line + '\n' for line in lines if line).encode()))
    print(f'metal: addon: debug/{name}' + (f' holding {[line for line in lines if line]}' if any(lines) else ' (empty)'))
def pad(data):
    return data + bytes(-len(data) % 8)
body = b''
for name, mode, data in files:
    encoded = name.encode() + b'\0'
    assert len(encoded) <= 160
    packed = zlib.compress(data, 9)
    body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
assert len(blob) <= 64 << 20, 'over xnuhost\'s 64 MiB limit for the sidecar'
open(output, 'wb').write(blob)
print(f'metal: {output}: {len(files)} files, {len(blob)} bytes')
EOF2
}

say "SDK $IOS_SDK_VERSION ($IOS_SDK); stubs $([ -f "$IOS_TBD/usr/lib/libSystem.B.tbd" ] && echo "$IOS_TBD" || echo "none: the SDK's own"); target $IOS_TARGET"
case "${1:-all}" in
  stage) stage ;;
  turnip) turnip ;;
  plugin) plugin ;;
  finish) finish; addon ;;
  addon) addon ;;
  all) stage; turnip; plugin; finish; addon ;;
  *) sed -n '2,6p' "$0"; exit 2 ;;
esac
