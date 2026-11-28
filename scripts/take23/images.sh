#!/bin/bash
# scripts/take23/images.sh: take23's ten crosvm Images (docs/take23.md). The owner's recipe with the paths made variables;
# the boot arguments and loader options are take23's, byte for byte.
#   KC     the kernel collection (default out/kernel/native-take22/kernel.kc: take23 runs take22's kernel, scripts/take22/kbuild.sh)
#   SET    the image set that trusts the addon (default out/take23/set/24A446: OUT=out/take23 scripts/take22/mk-addon.sh
#          with the plug-in of ci/metal-upgrade 72dc37c and the apps of ci/apps 94ab61c)
#   ADDON  the merged addon (default out/take23/ALL-metal-apps.gpuaddon)
#   PRIME  the shader priming list (default scripts/take22/primelist.min)
#   IDT    the decoded device tree of the iPhone18,2 (default out/kernel-inputs/24A446/devicetree.bin): the identity groups' source
#   OUT    default out/take23/images
# What differs from take22's: the identity groups a (artwork: idiom phone, subtype 2868, dynamic-displaymode 0, gamut sRGB;
# without graphics-featureset-class and -fallbacks) and e (device-perf-memory-class = CoreUI's class of the Image's own RAM);
# no memorystatus_pressure_threshold_mb and memorystatus_reaper_minimum_age_apps_seconds; oneios-prewarm-max=2048; one
# Image a RAM size for the new boot order (oneios-tool phone vm-boot --fill-after-stop --image-larger ...).
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
L=$R/runtime/loader/build/host/xnuloader-crosvm
[ -x "$L" ] || make -C "$R/runtime/loader"
KC=${KC:-$R/out/kernel/native-take22/kernel.kc}
SET=${SET:-$R/out/take23/set/24A446}
ADDON=${ADDON:-$R/out/take23/ALL-metal-apps.gpuaddon}
PRIME=${PRIME:-$R/scripts/take22/primelist.min}
IDT=${IDT:-$R/out/kernel-inputs/24A446/devicetree.bin}
OUT=${OUT:-$R/out/take23/images}
mkdir -p "$OUT"
T18ARGS="oneios-data-wait=1800 oneios-hid-buttons-builtin=1 oneios-button-haptics=1 oneios-audio-mic=1 oneios-audio-mic-selftest=3 oneios-gpu-stages=1 oneios-hid-queue-kib=256 oneios-feature-off=CoreLocation/CLAutoCohort,ReplayKit/recordingHEVC,ReplayKit/recordingHDR"
JOBS="oneios-enable-jobs=com.apple.analyticsd,com.apple.navd,com.apple.duetexpertd,com.apple.email.maild"
# oneios-prewarm-max=2048 (1024 by default): the kept states' limit, raised for the descriptors NativeMetal 0011 adds from
# Core Animation's archive asks (take22b had about 600 states kept before them; how many the asks add is not known yet)
ALL="oneios-prewarm=1 oneios-prewarm-max=2048 oneios-screenrec=1 oneios-addon-services=1"
# take22's three arguments less the two memorystatus ones
T23ARGS="oneios-disable-jobs=com.apple.accessibility.axassetsd,com.apple.aonsensed,com.apple.prngseedd"
base=(--jetsam-product V54 --swiotlb 32 --cpus 6 --display 720x1560 --scale 2 --hz 120 --shared-ram --gpu-scanout
      --audio --wifi --bridge --prime-list "$PRIME")
IDENT=(--identity-tree "$IDT" --identity-groups artwork,memclass --identity-omit graphics-featureset-class,graphics-featureset-fallbacks --display-gamut sRGB)
make_img() { local name=$1 ram=$2 args=$3; shift 3
  "$L" "$KC" "$OUT/$name" --ram-mib "$ram" --image "$SET" --gpu-addon "$ADDON" "${base[@]}" "$@" --args-add "$T18ARGS $JOBS $ALL $T23ARGS$args" > "$OUT/$name.log" 2>&1
  echo "$name: $(grep -c . "$OUT/$name.log") lines; boot args $(grep -a -m1 "boot args" "$OUT/$name.log" | sed "s/^.*boot args: *//" | wc -c) bytes"; }
# the main Images: one a RAM size (the new boot order picks the largest that fits; 4608 and 4352 also boot the old way)
for ram in 4352 4608 5120 5632 6144 6656 7168; do
  make_img kernel-take23-ALL-$ram-v54.img $ram "" "${IDENT[@]}"
done
# the conservative one: no identity group (its tree is take22's) and NativeMetal 0011's archive asks left alone
make_img kernel-take23-ALL-4608-v54-plain.img 4608 " oneios-prewarm-archive=0"
# a diagnosis of the menus' opening: the frontend's journal and frame trace (log-gpu=1 diag=1), 0011's allocation counter
make_img kernel-take23-ALL-4608-v54-diag.img 4608 " log-gpu=1 diag=1 oneios-res-log=1" "${IDENT[@]}"
# A/B for the heat: the GPU's interrupt instead of the polling (take22's -gpuirq, never run)
make_img kernel-take23-ALL-4608-v54-gpuirq.img 4608 "" "${IDENT[@]}" --gpu-intid 36
(cd "$OUT" && sha256sum kernel-take23-*.img > SHA256SUMS)
