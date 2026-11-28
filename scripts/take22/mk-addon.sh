#!/bin/bash
# scripts/take22/mk-addon.sh: take22's merged boot addon (Metal plug-in + OneiOS's apps) and the image set that trusts it.
# The owner's recipe with the paths made variables (docs/TAKE22-GUIDE.md). Inputs: the two CI artifacts, decrypted.
#   M=DIR       NativeAdrenoMetal.gpuaddon and NativeAdrenoMetal.build.json (artifact metal-plugin)
#   A=DIR       OneiOS2048.app, OneiOSBench.app, oneios-appreg, appreg.conf, cdhashes.txt (artifact oneios-apps)
#   NOTE=TEXT   a note for the new trust entries (e.g. "ci/metal-upgrade 6b8d8b4")
#   SET_IN=DIR  the image set made by scripts/image/build.sh (default out/image/24A446)
#   OUT=DIR     where everything goes (default out/take22): OUT/ALL-metal-apps.gpuaddon, OUT/ALL-imageset/{owner-trust,manifest}.json,
#               OUT/set/24A446 (a shadow of SET_IN: symbolic links, plus its own owner-trust.json and manifest.json)
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
M=${M:?the folder with NativeAdrenoMetal.gpuaddon and NativeAdrenoMetal.build.json}
A=${A:?the folder with the apps (OneiOS2048.app, OneiOSBench.app, oneios-appreg, appreg.conf, cdhashes.txt)}
NOTE=${NOTE:?a note for the trust entries}
SET_IN=$(cd "${SET_IN:-$R/out/image/24A446}" && pwd)
OUT=${OUT:-$R/out/take22}
T=$OUT/tmp
ADDON_OUT=$OUT/ALL-metal-apps.gpuaddon
JSON_OUT=$OUT/ALL-imageset
[ ! -e "$OUT/set" ] || { echo "mk-addon: $OUT/set exists; move it away first" >&2; exit 1; }
mkdir -p "$T" "$JSON_OUT" "$OUT/set"
cp -rs "$SET_IN" "$OUT/set/24A446"
S=$OUT/set/24A446
for f in overlay/userland/owner-trust.json manifest.json; do [ -L "$S/$f" ] && unlink "$S/$f"; done
python3 -I "$R/scripts/take22/mk-trust.py" "$SET_IN/overlay/userland/owner-trust.json" "$SET_IN/manifest.json" \
  "$M/NativeAdrenoMetal.build.json" "$NOTE" "$T/trust-metal.json" "$T/manifest-metal.json"
cp "$T/trust-metal.json" "$S/overlay/userland/owner-trust.json"; cp "$T/manifest-metal.json" "$S/manifest.json"
python3 -I "$R/userland/apps/appsaddon.py" build --apps "$A" --metal "$M/NativeAdrenoMetal.gpuaddon" \
  --out "$ADDON_OUT" --trust "$S/overlay/userland/owner-trust.json" --trust-out "$T/owner-trust.json" \
  --manifest "$S/manifest.json" --manifest-out "$T/manifest.json" --expect "$A/cdhashes.txt"
cp "$T/owner-trust.json" "$S/overlay/userland/owner-trust.json"; cp "$T/manifest.json" "$S/manifest.json"
cp "$T/owner-trust.json" "$T/manifest.json" "$JSON_OUT/"
python3 -I "$R/userland/apps/appsaddon.py" list "$ADDON_OUT"
sha256sum "$ADDON_OUT" "$JSON_OUT/owner-trust.json" "$JSON_OUT/manifest.json"
