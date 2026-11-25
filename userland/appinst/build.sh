#!/bin/bash
# Build appinst, OneiOS's on-device .ipa installer, for the iOS 27 guest (arm64e) and sign it ad hoc with 4 KiB
# signature pages, like userland/helpers. See README.md.
#
#   userland/appinst/build.sh [OUT_DIR]        default out/userland/appinst (git-ignored)
#
# Output: OUT_DIR/appinst (one binary with the install/finalize/serve subcommands) and OUT_DIR/cdhashes.txt.
# Put OUT_DIR/appinst — and an arm64e `ldid` named "ldid" (see README "the signer") — into the helpers output
# directory so `roottweak apply --helpers` installs them in /private/var/oneios/bin and trusts their CDHashes.
#
# Toolchain: scripts/ios (docs/building.md section 9), same as the helpers: ios-clang, then ios-sign.py. The link
# is against the guest's iOS 27 stubs (out/ios27-tbd) when present, else the SDK's. arm64e because iOS 27 kills
# arm64 platform binaries at exec.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TARGET=${ONEIOS_IOS_TARGET:-arm64e-apple-ios27.0}
WARN=(-O2 -Wall -Wextra -Wno-unused-parameter)
OUT=${1:-$REPO/out/userland/appinst}
mkdir -p "$OUT"

. "$REPO/scripts/ios/env.sh"
cc_c()  { "$IOS_DIR/ios-clang" -target "$TARGET" -std=c11 "${WARN[@]}" "$@"; }
cc_m()  { "$IOS_DIR/ios-clang" -target "$TARGET" -fobjc-arc "${WARN[@]}" "$@"; }
echo "appinst: SDK $IOS_SDK_VERSION ($IOS_SDK); stubs $([ -f "$IOS_TBD/usr/lib/libSystem.B.tbd" ] && echo "$IOS_TBD" || echo "the SDK's own"); $("$IOS_CLANG" --version | head -1)"

OBJ="$OUT/obj"
mkdir -p "$OBJ"
cc_c -c "$HERE/macho.c"  -o "$OBJ/macho.o"
cc_c -c "$HERE/unzip.c"  -o "$OBJ/unzip.o"
cc_c -c "$HERE/vm.c"     -o "$OBJ/vm.o"
cc_m -c "$HERE/appinst.m"  -o "$OBJ/appinst.o"
cc_m -c "$HERE/register.m" -o "$OBJ/register.o"
cc_m -c "$HERE/serve.m"    -o "$OBJ/serve.o"

# CommonCrypto (CC_SHA256) and LaunchServices (via the ObjC runtime) are in libSystem / resolved at runtime;
# only Foundation and zlib need naming.
"$IOS_DIR/ios-clang" -target "$TARGET" -fobjc-arc \
  "$OBJ"/macho.o "$OBJ"/unzip.o "$OBJ"/vm.o "$OBJ"/appinst.o "$OBJ"/register.o "$OBJ"/serve.o \
  -framework Foundation -lz -o "$OUT/appinst"

"$IOS_DIR/ios-sign.py" --identifier com.oneios.appinst --entitlements "$HERE/entitlements.plist" "$OUT/appinst" >/dev/null

# Same reader the image build uses: fails on an unsigned file, and prints arch + page size.
python3 -I "$REPO/userland/tweak" cdhash "$OUT/appinst" | tee "$OUT/cdhashes.txt"
if grep -v -q 'arm64e pages=4096' "$OUT/cdhashes.txt"; then
  echo "build.sh: appinst is not arm64e with 4 KiB signature pages (line above)" >&2; exit 1
fi

# ldid is a separate dependency (the on-device signer); like dropbear for the helpers, it is not built here.
if [ ! -f "$OUT/ldid" ]; then
  cat >&2 <<EOF

appinst built. One dependency is still needed at install time: an arm64e 'ldid' at
  /private/var/oneios/bin/ldid   (the on-device ad-hoc signer appinst calls).
Build it for arm64e (e.g. the ProcursusTeam/ldid 'ldid' target with the scripts/ios toolchain, or take
Droid-VM/ownervisor's), sign it ad hoc with 4 KiB pages like these binaries, and place it in the helpers
output directory before 'roottweak apply' so its CDHash is trusted too. See README.md "the signer".
EOF
fi
echo "appinst in $OUT"
