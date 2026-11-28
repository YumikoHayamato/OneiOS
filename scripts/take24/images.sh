#!/bin/bash
# scripts/take24/images.sh: take24's crosvm Images (docs/take24.md). take23's recipe (the same kernel, loader options and boot
# arguments) with take24's addon and two more RAM sizes, 7680 and 8192 MiB. The paths are variables:
#   KC     the kernel collection (default out/kernel/native-take22/kernel.kc: take22's kernel, scripts/take22/kbuild.sh)
#   SET    the image set that trusts the addon (default out/take24/set/24A446: OUT=out/take24 scripts/take22/mk-addon.sh)
#   ADDON  the merged addon (default out/take24/ALL-metal-apps.gpuaddon)
#   PRIME  the shader priming list (default scripts/take22/primelist.min)
#   IDT    the decoded device tree of the iPhone18,2 (default out/kernel-inputs/24A446/devicetree.bin)
#   OUT    default out/take24/images
#   RAMS   the RAM sizes (default "4608 5120 5632 6144 6656 7168 7680 8192"; given, -plain is not made)
# One Image a RAM size: vm-boot fills the pool after One UI's stop and runs the largest the pool holds
# (oneios-tool phone vm-boot --image ...4608... --image-larger ... for each of the others). No 8704: docs/take24.md.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
L=$R/runtime/loader/build/host/xnuloader-crosvm
[ -x "$L" ] || make -C "$R/runtime/loader"
KC=${KC:-$R/out/kernel/native-take22/kernel.kc}
SET=${SET:-$R/out/take24/set/24A446}
ADDON=${ADDON:-$R/out/take24/ALL-metal-apps.gpuaddon}
PRIME=${PRIME:-$R/scripts/take22/primelist.min}
IDT=${IDT:-$R/out/kernel-inputs/24A446/devicetree.bin}
OUT=${OUT:-$R/out/take24/images}
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
for ram in ${RAMS:-4608 5120 5632 6144 6656 7168 7680 8192}; do
  make_img kernel-take24-ALL-$ram-v54.img $ram "" "${IDENT[@]}"
done
# the conservative one: without the two identity groups (take22's tree) and with NativeMetal 0011's archive asks left alone
[ -n "${RAMS:-}" ] || make_img kernel-take24-ALL-4608-v54-plain.img 4608 " oneios-prewarm-archive=0"
(cd "$OUT" && sha256sum kernel-take24-*.img > SHA256SUMS)
