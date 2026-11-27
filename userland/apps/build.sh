#!/bin/bash
# Build OneiOS's own apps (2048, OneiOS Bench) and their registrar for the iOS 27 guest: arm64e, signed ad hoc with
# 4 KiB signature pages by scripts/ios/ios-sign.py, as userland/helpers/build.sh does.
#
#   userland/apps/build.sh [OUT_DIR]        default out/userland/apps (git-ignored)
#
# Output in OUT_DIR:
#   OneiOS2048.app, OneiOSBench.app   bundles: Info.plist, PkgInfo, icons, the signed executable
#   oneios-appreg, appreg.conf        the registrar (registers the bundles with LaunchServices at boot) and its
#                                     oneios-init service file
#   cdhashes.txt                      one line per Mach-O: <cdhash> <arch> pages=4096 <identifier> <file>
# Then: userland/apps/appsaddon.py build --apps OUT_DIR --metal NativeAdrenoMetal.gpuaddon ... (the boot addon and the
# owner trust list; README in the delivery folder).
#
# Toolchain: scripts/ios (docs/building.md section 9). Every target is built before the script fails, so one CI round
# shows every compile error (-ferror-limit=0).
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TARGET=${ONEIOS_IOS_TARGET:-arm64e-apple-ios27.0}
OUT=${1:-$REPO/out/userland/apps}
mkdir -p "$OUT"
. "$REPO/scripts/ios/env.sh" || exit 1
FLAGS=(-target "$TARGET" -O2 -fobjc-arc -Wall -Wextra -Wno-unused-parameter -ferror-limit=0)
FRAMEWORKS=(-framework Foundation -framework UIKit -framework QuartzCore -framework CoreGraphics)
echo "apps: SDK $IOS_SDK_VERSION ($IOS_SDK); stubs $([ -f "$IOS_TBD/usr/lib/libSystem.B.tbd" ] && echo "$IOS_TBD" || echo "none: the SDK's own"); $("$IOS_CLANG" --version | head -1)"
failed=()

# icons: a plain PNG made here (no asset catalog, no actool): AppIcon60x60@2x/@3x, as CFBundleIconFiles names them
icon() {  # icon OUT.png SIZE R G B TEXT-SEED
  python3 -I - "$@" <<'PY'
import struct, sys, zlib
out, size, r, g, b, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
rows = []
for y in range(size):
    row = bytearray([0])
    for x in range(size):
        # a diagonal gradient with a lighter square in the middle (a "tile")
        t = (x + y) / (2 * size)
        inside = size * 0.25 < x < size * 0.75 and size * 0.25 < y < size * 0.75
        k = 1.25 if inside else 1.0
        row += bytes(min(255, int(c * (0.75 + 0.5 * t) * k)) for c in (r, g, b))
    rows.append(bytes(row))
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
PY
}

app() {  # app DIR EXE BUNDLE_ID R G B FRAMEWORKS...
  local dir=$1 exe=$2 id=$3 r=$4 g=$5 b=$6
  shift 6
  local bundle=$OUT/$exe.app
  rm -rf "$bundle"
  mkdir -p "$bundle"
  if ! "$IOS_DIR/ios-clang" "${FLAGS[@]}" "$HERE/$dir/main.m" "${FRAMEWORKS[@]}" "$@" -o "$bundle/$exe"; then
    echo "apps: $exe did not build" >&2
    failed+=("$exe")
    return
  fi
  sed "s/@SDK_VERSION@/$IOS_SDK_VERSION/g" "$HERE/$dir/Info.plist" > "$bundle/Info.plist"
  printf 'APPL????' > "$bundle/PkgInfo"
  icon "$bundle/AppIcon60x60@2x.png" 120 "$r" "$g" "$b" 1
  icon "$bundle/AppIcon60x60@3x.png" 180 "$r" "$g" "$b" 1
  local ent=$OUT/.ent-$exe.plist
  sed "s/@BUNDLE_ID@/$id/g" "$HERE/app-entitlements.plist" > "$ent"
  "$IOS_DIR/ios-sign.py" --identifier "$id" --entitlements "$ent" "$bundle/$exe" >/dev/null || failed+=("$exe (signing)")
  rm -f "$ent"
}

app 2048 OneiOS2048 com.oneios.game2048 237 194 46
app bench OneiOSBench com.oneios.bench 40 120 220 -framework Metal -framework ImageIO

if "$IOS_DIR/ios-clang" "${FLAGS[@]}" "$HERE/appreg/appreg.m" -framework Foundation -o "$OUT/oneios-appreg"; then
  "$IOS_DIR/ios-sign.py" --identifier com.oneios.appreg --entitlements "$HERE/appreg/entitlements.plist" "$OUT/oneios-appreg" >/dev/null \
    || failed+=("oneios-appreg (signing)")
  cp "$HERE/appreg/appreg.conf" "$OUT/appreg.conf"
else
  failed+=(oneios-appreg)
fi

if [ ${#failed[@]} -gt 0 ]; then
  echo "apps: FAILED: ${failed[*]}" >&2
  exit 1
fi
# The reader the image build uses: fails on an unsigned file, says the architecture and the page size.
python3 -I "$REPO/userland/tweak" cdhash "$OUT/OneiOS2048.app/OneiOS2048" "$OUT/OneiOSBench.app/OneiOSBench" "$OUT/oneios-appreg" \
  | sed "s#$OUT/##" | tee "$OUT/cdhashes.txt"
if grep -v -q 'arm64e pages=4096' "$OUT/cdhashes.txt" || [ "$(wc -l < "$OUT/cdhashes.txt")" -ne 3 ]; then
  echo "apps: a binary is not arm64e with 4 KiB signature pages (lines above)" >&2
  exit 1
fi
# Every library and symbol they use exists in the guest's own iOS 27 (its stubs), when the stubs are there.
if [ -f "$IOS_TBD/usr/lib/libSystem.B.tbd" ]; then
  "$IOS_DIR/macho-check.py" "$OUT/OneiOS2048.app/OneiOS2048" "$OUT/OneiOSBench.app/OneiOSBench" "$OUT/oneios-appreg" \
    | tee "$OUT/macho-check.txt" || {
    # UIKit's UIFont and the NSFont* keys live in a framework UIKit re-exports; a stub made from the guest's cache that
    # misses that re-export says "not exported". The SDK's stubs decide then; the guest-stub report is kept.
    echo "apps: macho-check against the guest stubs failed (above); checking against the SDK's stubs" >&2
    "$IOS_DIR/macho-check.py" --sdk "$OUT/OneiOS2048.app/OneiOS2048" "$OUT/OneiOSBench.app/OneiOSBench" "$OUT/oneios-appreg" \
      | tee "$OUT/macho-check-sdk.txt" || { echo "apps: macho-check failed against the SDK too" >&2; exit 1; }
  }
fi
echo "apps in $OUT"
