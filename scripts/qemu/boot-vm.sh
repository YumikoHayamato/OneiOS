#!/bin/bash
# Boot a native kernel on qemu-system-aarch64's virt board, placed as a firmware loader places it on a phone
# (runtime/loader/tools/xnuloader-qemu.c): the bench of the native path (docs/native.md) before anything runs on the
# phone. Always in tmux, under a timeout:
#   scripts/bg.sh qemu-vm scripts/qemu/boot-vm.sh [SECONDS]                 (default 30)
#   KERNEL=FILE CPUS=N RAM_MIB=N BOOT_ARGS="..." ARGS_ADD="..." OUT=DIR ... another kernel, CPUs, the kernel's window,
#                                                                            all the boot arguments, a few more of them
#   IMAGE=DIR [DATA=FILE] ...   with an image set's disks (os, systemos, app, the Data seed, the Data store): the native
#                               kernel of kernel/build-native.sh, as a collection (kernel/prelink/build.sh). DATA is the
#                               writable store, kept between runs (first start: formatted and filled from the seed by
#                               the kernel); made sparse at the manifest's size when missing. Default
#                               ${TMPDIR:-/tmp}/oneios-qemu-data.img, in RAM on this PC: the image set itself stays on
#                               its disk, read-only, and every chunk the kernel reads is checked against its hash list.
#   QEMU_DEBUG=LIST ...         QEMU's -d log (default int,guest_errors,unimp: every exception, for a kernel that stops
#                               early; with IMAGE guest_errors,unimp, because a long boot's interrupts would fill the
#                               log's 64 MB and stop QEMU)
#   SCREEN=WxH [SCALE=2|3] [HZ=N] ...  a display: QEMU's virtio-gpu-device (2D) and the boot video, the kernel's display
#                               driver on it (kernel/patches/oneios-hosted/display). Without a 3D GPU iOS draws with the CPU.
#   VNC=N ...                   QEMU's VNC server on 127.0.0.1:590N (this PC only), to watch it: krdc vnc://127.0.0.1:590N
#   INPUT=tablet,keyboard ...   QEMU's virtio-tablet-device (the VNC client's mouse: one finger while the button is down)
#                               and virtio-keyboard-device, for the kernel's touch screen and keys (the hid archive)
#   PCI=1 IMAGE=DIR ...         the disks as virtio-blk-pci functions (00:02.0 to 00:06.0) with DMA windows, as in a VM of
#                               the phone's crosvm (xnuloader-qemu --pci; docs/native.md "A VM under Gunyah")
#   AUDIO=1 ...                 QEMU's virtio-sound-device with `-audiodev none` (xnuloader-qemu --audio): the kernel's
#                               virtio-snd transport and its microphone on the bench. With ARGS_ADD="oneios-audio-mic=1
#                               oneios-audio-mic-selftest=2" serial.log has `droidvm-snd: the microphone's stream 1 (x1): ready
#                               after N ms` and two `ONEIOS AUDIO: mic self-test` lines (silence: `largest sample 0`)
#   JETSAM_PRODUCT=V54 ...      the name of iOS's jetsam properties (xnuloader-qemu --jetsam-product, as xnuloader-crosvm's:
#                               launchd then reads com.apple.jetsamproperties.V54.plist; serial.log: `oneios-band:
#                               backboardd[N] 180 -> 170` with it, `-> 30` without)
#   MEDIA=1 ...                 the video decoder's node on bus 0 of virt's ECAM and oneios-codec=1 droidvm-vt-paravirt=1
#                               (xnuloader-qemu --media): the kernel's codec archive with no decoder behind it (QEMU has no
#                               virtio-media device); serial.log: `droidvm-media: no 1af4:1070 function on bus 0` and
#                               `ONEIOS CODEC: 1 virtio-media device(s), 0 up; VideoToolbox paravirtualization not published`
# Out: OUT (out/qemu/vm) serial.log (the kernel's console on the PL011), qemu.log (QEMU's -d log, the first 64 MB), and
# xnuloader-qemu's layout files; monitor.sock, QEMU's monitor while it runs (scripts/qemu/shot.sh takes a picture of the
# display), and qmp.sock, its QMP socket (scripts/qemu/touch.sh taps and swipes on the tablet). A guest restart ends the run (-no-reboot). QEMU comes from the project's sysroot
# (/mnt/tkg-build/oneios/sysroot, QEMU_SYSROOT=; Arch's qemu-system-aarch64 extracted there, never installed to /), and
# its modules from there too (QEMU_MODULE_DIR: virtio-gpu-device is qemu-hw-display-virtio-gpu's module).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SECS=${1:-30}
SYSROOT=${QEMU_SYSROOT:-/mnt/tkg-build/oneios/sysroot}
QEMU=${QEMU:-$SYSROOT/usr/bin/qemu-system-aarch64}
KERNEL=${KERNEL:-$ROOT/out/kernel/current/xnu.macho}
OUT=${OUT:-$ROOT/out/qemu/vm}
IMAGE=${IMAGE:-}
[ -x "$QEMU" ] || { echo "boot-vm: no $QEMU (docs/native.md: QEMU in the sysroot)" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "boot-vm: no $KERNEL (scripts/bg.sh kernel-vm kernel/build-linux.sh)" >&2; exit 1; }
make -s -C "$ROOT/runtime/loader" build/host/xnuloader-qemu
rm -rf "$OUT"
mkdir -p "$OUT"
opts=()
[ -z "${CPUS:-}" ] || opts+=(--cpus "$CPUS")
[ -z "${RAM_MIB:-}" ] || opts+=(--ram-mib "$RAM_MIB")
[ -z "${BOOT_ARGS:-}" ] || opts+=(--args "$BOOT_ARGS")
[ -z "${ARGS_ADD:-}" ] || opts+=(--args-add "$ARGS_ADD")
[ -z "${SCREEN:-}" ] || opts+=(--display "$SCREEN" --scale "${SCALE:-2}" --hz "${HZ:-60}")
[ "${MEDIA:-0}" = 0 ] || opts+=(--media)
[ -z "${INPUT:-}" ] || opts+=(--input "$INPUT")
[ "${AUDIO:-0}" = 0 ] || opts+=(--audio)
[ -z "${JETSAM_PRODUCT:-}" ] || opts+=(--jetsam-product "$JETSAM_PRODUCT")
display=(-display none)
[ -z "${VNC:-}" ] || display+=(-vnc "127.0.0.1:$VNC")
debug=${QEMU_DEBUG:-int,guest_errors,unimp}
if [ -n "$IMAGE" ]; then
  [ -f "$IMAGE/manifest.json" ] || { echo "boot-vm: $IMAGE has no manifest.json" >&2; exit 1; }
  DATA=${DATA:-${TMPDIR:-/tmp}/oneios-qemu-data.img}
  if [ ! -f "$DATA" ]; then
    gib=$(grep -o '"store_gib": *[0-9]*' "$IMAGE/manifest.json" | grep -o '[0-9]*$' || true)
    truncate -s "${gib:-16}G" "$DATA"
    echo "boot-vm: a new Data store $DATA (${gib:-16} GiB, sparse): the kernel formats it and unpacks the seed"
  fi
  opts+=(--image "$IMAGE" --data "$DATA")
  [ "${PCI:-0}" = 0 ] || opts+=(--pci)
  debug=${QEMU_DEBUG:-guest_errors,unimp}
fi
"$ROOT/runtime/loader/build/host/xnuloader-qemu" "$KERNEL" "$OUT" "${opts[@]}" | tee "$OUT/layout.txt"
mapfile -t args < "$OUT/qemu.args"
echo "boot-vm: $SECS s of QEMU $(LD_LIBRARY_PATH=$SYSROOT/usr/lib "$QEMU" --version | head -1)"
set +e
# QEMU's own log (-d) goes to stderr, here into the pipe; when head has its 64 MB QEMU gets SIGPIPE and stops.
QEMU_MODULE_DIR=$SYSROOT/usr/lib/qemu LD_LIBRARY_PATH=$SYSROOT/usr/lib timeout "$SECS" "$QEMU" "${args[@]}" "${display[@]}" -no-reboot \
  -monitor "unix:$OUT/monitor.sock,server=on,wait=off" -qmp "unix:$OUT/qmp.sock,server=on,wait=off" \
  -serial "file:$OUT/serial.log" -d "$debug" 2>&1 >/dev/null | head -c 64000000 > "$OUT/qemu.log"
code=${PIPESTATUS[0]}
set -e
echo "boot-vm: QEMU exit $code (124 is the timeout); console $(wc -c < "$OUT/serial.log") bytes, QEMU log $(wc -l < "$OUT/qemu.log") lines"
echo "--- the console's last lines ---"
tail -n 25 "$OUT/serial.log" || true
echo "--- the first exceptions ---"
grep -m 12 -E 'Taking exception|ESR|FAR|ELR' "$OUT/qemu.log" || true
