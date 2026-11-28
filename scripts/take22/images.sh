#!/bin/bash
# scripts/take22/images.sh: take22's four crosvm Images (docs/TAKE22-GUIDE.md). The owner's recipe with the paths made
# variables; the boot arguments and loader options are take22's, byte for byte.
#   KC     the kernel collection (default out/kernel/native-take22/kernel.kc: scripts/take22/kbuild.sh)
#   SET    the image set that trusts the addon (default out/take22/set/24A446: scripts/take22/mk-addon.sh)
#   ADDON  the merged addon (default out/take22/ALL-metal-apps.gpuaddon)
#   PRIME  the shader priming list (default scripts/take22/primelist.min, the one the owner's images carry)
#   OUT    default out/take22/images
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
L=$R/runtime/loader/build/host/xnuloader-crosvm
[ -x "$L" ] || make -C "$R/runtime/loader"
KC=${KC:-$R/out/kernel/native-take22/kernel.kc}
SET=${SET:-$R/out/take22/set/24A446}
ADDON=${ADDON:-$R/out/take22/ALL-metal-apps.gpuaddon}
PRIME=${PRIME:-$R/scripts/take22/primelist.min}
OUT=${OUT:-$R/out/take22/images}
mkdir -p "$OUT"
T18ARGS="oneios-data-wait=1800 oneios-hid-buttons-builtin=1 oneios-button-haptics=1 oneios-audio-mic=1 oneios-audio-mic-selftest=3 oneios-gpu-stages=1 oneios-hid-queue-kib=256 oneios-feature-off=CoreLocation/CLAutoCohort,ReplayKit/recordingHEVC,ReplayKit/recordingHDR"
JOBS="oneios-enable-jobs=com.apple.analyticsd,com.apple.navd,com.apple.duetexpertd,com.apple.email.maild"
ALL="oneios-prewarm=1 oneios-screenrec=1 oneios-addon-services=1"
# take22: apps in the background are reaped after 30 minutes, not 4; the memory warning at 600 MiB, not 676; the two
# launchd jobs that restarted every 10 s (aonsensed, prngseedd) off (axassetsd is that argument's built-in default, kept).
# memorystatus_pressure_threshold_mb must stay under half the VM's RAM (an Image under ~1300 MiB panics at boot).
T22ARGS="memorystatus_reaper_minimum_age_apps_seconds=1800 memorystatus_pressure_threshold_mb=600 oneios-disable-jobs=com.apple.accessibility.axassetsd,com.apple.aonsensed,com.apple.prngseedd"
base=(--jetsam-product V54 --swiotlb 32 --cpus 6 --display 720x1560 --scale 2 --hz 120 --shared-ram --gpu-scanout
      --audio --wifi --bridge --prime-list "$PRIME")
make_img() { local name=$1 ram=$2; shift 2
  "$L" "$KC" "$OUT/$name" --ram-mib "$ram" --image "$SET" --gpu-addon "$ADDON" "${base[@]}" "$@" --args-add "$T18ARGS $JOBS $ALL $T22ARGS" > "$OUT/$name.log" 2>&1
  echo "$name: $(grep -c . "$OUT/$name.log") lines"; }
make_img kernel-take22-ALL-4608-v54.img 4608
make_img kernel-take22-ALL-4352-v54.img 4352
make_img kernel-take22-ALL-4608-v54-gpuirq.img 4608 --gpu-intid 36
# diagnostics, not for judging smoothness: the pipeline journal and the warm pass's counters
T22ARGS="$T22ARGS log-gpu=1 diag=1"
make_img kernel-take22-ALL-4352-v54-loggpu.img 4352
(cd "$OUT" && sha256sum kernel-take22-*.img > SHA256SUMS)
