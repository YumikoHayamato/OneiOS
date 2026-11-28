#!/bin/bash
# take23 (take22's mk-addon.sh, its two folders changed): the image set's trust list and the merged addon, from the two CI artifacts (decrypted into $T/ci-metal and
# $T/ci-apps: README "COM'E' FATTA"). The set is take21's shadow set (links into out-image/24A446) with two new files.
set -euo pipefail
D=/mnt/giochi/Linux/oneios/take23
T=/tmp/claude-1000/t23
M=${M:?the folder with NativeAdrenoMetal.gpuaddon and NativeAdrenoMetal.build.json}
A=${A:?the folder with the apps (OneiOS2048.app, OneiOSBench.app, oneios-appreg, appreg.conf, cdhashes.txt)}
NOTE=${NOTE:?a note for the trust entries}
ADDON_OUT=${ADDON_OUT:-$D/ALL-metal-apps.gpuaddon}      # the merged addon
SETDIR=${SETDIR:-$T/set}                                 # the shadow image set (SETDIR/24A446 goes to xnuloader --image)
JSON_OUT=${JSON_OUT:-$D/ALL-imageset}                    # where the set's two own files are kept
rm -rf "$T/set.new" && mkdir -p "$T/set.new" && cp -a /tmp/claude-1000/t21/set/24A446 "$T/set.new/24A446"
S=$T/set.new/24A446
python3 -I "$D/mk-trust.py" /mnt/giochi/Linux/oneios/take21/ALL-imageset/owner-trust.json /mnt/giochi/Linux/oneios/take21/ALL-imageset/manifest.json \
  "$M/NativeAdrenoMetal.build.json" "$NOTE" "$T/trust-metal.json" "$T/manifest-metal.json"
cp "$T/trust-metal.json" "$S/overlay/userland/owner-trust.json"; cp "$T/manifest-metal.json" "$S/manifest.json"
python3 -I /dev/shm/apps-wt/userland/apps/appsaddon.py build --apps "$A" --metal "$M/NativeAdrenoMetal.gpuaddon" \
  --out "$ADDON_OUT" --trust "$S/overlay/userland/owner-trust.json" --trust-out "$T/owner-trust.json" \
  --manifest "$S/manifest.json" --manifest-out "$T/manifest.json" --expect "$A/cdhashes.txt"
cp "$T/owner-trust.json" "$S/overlay/userland/owner-trust.json"; cp "$T/manifest.json" "$S/manifest.json"
mkdir -p "$JSON_OUT" && cp "$T/owner-trust.json" "$T/manifest.json" "$JSON_OUT/"
[ ! -e "$SETDIR" ] || mv "$SETDIR" "$SETDIR.old.$$"
mv "$T/set.new" "$SETDIR"
python3 -I /dev/shm/apps-wt/userland/apps/appsaddon.py list "$ADDON_OUT"
sha256sum "$ADDON_OUT" "$JSON_OUT/owner-trust.json" "$JSON_OUT/manifest.json"
