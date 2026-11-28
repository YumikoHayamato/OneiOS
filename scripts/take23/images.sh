#!/bin/bash
# take23 images: take22's recipe (kernel of take22, unchanged: kernel-native-take22.kc) with
#   - the addon of take23 (Metal plug-in of ci/metal-upgrade 72dc37c: NativeMetal 0011, ScreenRecord's time split; the apps
#     of ci/apps 94ab61c: oneios-appreg's own view over /Applications): mk-addon.sh
#   - take23's loader (branch take23: CoreUI's memory classes, --identity-omit, --display-gamut, RAM up to 8192 MiB)
#   - the identity groups a (artwork: idiom phone, subtype 2868, dynamic-displaymode 0, gamut sRGB; WITHOUT
#     graphics-featureset-class and -fallbacks) and e (device-perf-memory-class = CoreUI's class of the Image's own RAM)
#   - take22's boot arguments WITHOUT memorystatus_pressure_threshold_mb=600 and
#     memorystatus_reaper_minimum_age_apps_seconds=1800 (the owner's decision; take22: the Safari page went grey all the same)
#   - one Image a RAM size, for the new boot order (oneios-tool phone vm-boot --fill-after-stop --image-larger ...)
set -euo pipefail
W=/dev/shm/t19/work
L=${L:-/tmp/claude-1000/t23/src/runtime/loader/build/host/xnuloader-crosvm}
KC=${KC:-$W/out/kernel/native-take22/kernel.kc}
T=/tmp/claude-1000/t23
SET=${SET:-$T/set/24A446}
ADDON=${ADDON:-/mnt/giochi/Linux/oneios/take23/ALL-metal-apps.gpuaddon}
PRIME=/mnt/tkg-build/oneios/OneiOS/out/userland/prime/NativeAdrenoMetal.primelist.min
IDT=${IDT:-$W/out/kernel-inputs/24A446/devicetree.bin}
OUT=${OUT:-/mnt/giochi/Linux/oneios/take23/images}
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
make() { local name=$1 ram=$2 args=$3; shift 3
  "$L" "$KC" "$OUT/$name" --ram-mib "$ram" --image "$SET" --gpu-addon "$ADDON" "${base[@]}" "$@" --args-add "$T18ARGS $JOBS $ALL $T23ARGS$args" > "$OUT/$name.log" 2>&1
  echo "$name: $(grep -c . "$OUT/$name.log") lines; boot args $(grep -a -m1 "boot args" "$OUT/$name.log" | sed "s/^.*boot args: *//" | wc -c) bytes"; }
# the main Images: one a RAM size (the new boot order picks the largest that fits; 4608 and 4352 also boot the old way)
for ram in 4352 4608 5120 5632 6144 6656 7168; do
  make kernel-take23-ALL-$ram-v54.img $ram "" "${IDENT[@]}"
done
# the conservative one: take23 without the two identity groups (its tree is take22's) and with NativeMetal 0011's archive
# asks left alone (oneios-prewarm-archive=0): the fixes only (appreg, the two arguments gone)
make kernel-take23-ALL-4608-v54-plain.img 4608 " oneios-prewarm-archive=0"
# DIAGNOSIS for the menus' opening (item 6): the pipeline journal (log-gpu=1 diag=1) and the allocation counter (oneios-res-log=1)
make kernel-take23-ALL-4608-v54-diag.img 4608 " log-gpu=1 diag=1 oneios-res-log=1" "${IDENT[@]}"
# A/B for the heat (item 8), after the main one: the GPU's interrupt instead of the polling (take22's -gpuirq, never run)
make kernel-take23-ALL-4608-v54-gpuirq.img 4608 "" "${IDENT[@]}" --gpu-intid 36
(cd "$OUT" && sha256sum kernel-take23-*.img > SHA256SUMS)
