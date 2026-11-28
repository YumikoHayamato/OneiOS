#!/bin/bash
# scripts/hugepage/build.sh [WORK]: Droid-VM's gh_hugepage_reserve (GPL-2.0, https://github.com/Droid-VM/gh-hugepage-reserve)
# for the phone's KMI, android15-6.6, built as its authors build it (their build.sh and docker_exec.sh) but without
# Docker: their DDK image (ghcr.io/ylarod/ddk-min:android15-6.6, the GKI tree and Google's clang, pinned by digest) is
# fetched from the registry, every layer checked against its digest and unpacked under WORK, and the build runs in it
# under bubblewrap (no network, the source read-only, nothing written outside WORK). Also kapi_check, the module's
# ABI preflight, built for the PC: it reads the phone's BTF (`oneios-tool phone hw` copies /sys/kernel/btf/vmlinux)
# and names the kernel functions whose signatures drifted, which the module then leaves alone instead of
# kCFI-panicking on them. The module's source stays outside this repository (GPL: docs/references.md).
#
# Output: WORK/out/android15-6.6.ko, WORK/out/kapi_check, WORK/out/build.txt (the pins and the .ko's modinfo).
# docs/native.md "A VM under Gunyah" ("Stopping One UI for the module").
#
# scripts/hugepage/build.sh WORK samfor12: the alternative module instead (take18, off unless loaded by name:
# `phone hugepool load --ko WORK/out/android15-6.6-samfor12.ko`): samfor12/gh-hugepage-reserve-temp-root-support 361d499,
# our pin 00c8a03 plus six commits of 2026-11-18 (pool pages held with PG_reserved while the module keeps them and
# cleared before every free and before a page is served; the death grace 3 -> 13 s so the release worker can take
# a dead VM's served pages back). The same module name, parameters and sysfs files as ours.
# Output: WORK/out/android15-6.6-samfor12.ko, WORK/out/build-samfor12.txt.
set -euo pipefail
WORK=${1:-/mnt/tkg-build/oneios/hugepage}
VARIANT=${2:-}
IMAGE=ylarod/ddk-min
TAG=android15-6.6
DIGEST=sha256:51159bc935c75a282fc65bc87e02f16bd098d1cdf55dc558c4fdd0fcf7a132e2
REPO=https://github.com/Droid-VM/gh-hugepage-reserve.git
COMMIT=00c8a0322aa6fb25593e00352af2764be5317d7e
DDK=$WORK/ddk-$TAG
ROOTFS=$DDK/rootfs
SRC=$WORK/gh-hugepage-reserve
OUT=$WORK/out
KO=$TAG.ko
RECORD=build.txt
case "$VARIANT" in
  '') ;;
  samfor12)
    REPO=https://github.com/samfor12/gh-hugepage-reserve-temp-root-support.git
    COMMIT=361d4999bd6b7e2a3e2c83dcac17a0313e4b6b1d
    SRC=$WORK/gh-hugepage-reserve-samfor12
    KO=$TAG-samfor12.ko
    RECORD=build-samfor12.txt ;;
  *) echo "usage: build.sh [WORK [samfor12]]" >&2; exit 2 ;;
esac
mkdir -p "$DDK/blobs" "$OUT"
sha() { sha256sum < "$1" | cut -d' ' -f1; }

# 1. The DDK image, by its manifest's digest; its layers by theirs.
if [ ! -e "$ROOTFS/.oneios-done" ]; then
  tok=$(curl -fsS "https://ghcr.io/token?scope=repository:$IMAGE:pull" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
  [ -n "$tok" ] || { echo "build: no registry token" >&2; exit 1; }
  curl -fsS -H "Authorization: Bearer $tok" -H "Accept: application/vnd.docker.distribution.manifest.v2+json" \
    "https://ghcr.io/v2/$IMAGE/manifests/$DIGEST" -o "$DDK/manifest.json"
  [ "sha256:$(sha "$DDK/manifest.json")" = "$DIGEST" ] || { echo "build: the manifest is not $DIGEST" >&2; exit 1; }
  # the first digest is the image's config; the layers follow in order (the empty layer twice: fetched once)
  layers=$(grep -o '"digest": *"sha256:[0-9a-f]*"' "$DDK/manifest.json" | sed 's/.*sha256://; s/"//' | tail -n +2)
  for d in $(printf '%s\n' $layers | sort -u); do
    f=$DDK/blobs/$d
    [ -e "$f" ] && [ "$(sha "$f")" = "$d" ] && continue
    echo "build: layer $d"
    curl -fsSL --retry 3 -H "Authorization: Bearer $tok" "https://ghcr.io/v2/$IMAGE/blobs/sha256:$d" -o "$f.part"
    [ "$(sha "$f.part")" = "$d" ] || { echo "build: layer $d: the digest differs" >&2; exit 1; }
    mv "$f.part" "$f"
  done
  rm -rf "$ROOTFS"
  mkdir -p "$ROOTFS"
  for d in $layers; do
    f=$DDK/blobs/$d
    # whiteouts first: .wh.NAME removes NAME of the layers below, .wh..wh..opq the folder's whole content
    tar -tzf "$f" | { grep -E '(^|/)\.wh\.' || true; } | while read -r w; do
      dir=$(dirname "$w"); base=$(basename "$w")
      case "$base" in
        .wh..wh..opq) [ -d "$ROOTFS/$dir" ] && find "$ROOTFS/$dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} + ;;
        .wh.*) rm -rf "${ROOTFS:?}/$dir/${base#.wh.}" ;;
      esac
    done
    tar -xzf "$f" -C "$ROOTFS" --no-same-owner --exclude='dev/*' --exclude='*/.wh.*' --exclude='.wh.*'
    chmod -R u+w "$ROOTFS"
  done
  mkdir -p "$ROOTFS/src" "$ROOTFS/out_gh_hugepage_reserve" "$ROOTFS/proc" "$ROOTFS/dev" "$ROOTFS/tmp"
  touch "$ROOTFS/.oneios-done"
fi
ls -d "$ROOTFS"/opt/ddk/clang/clang-* "$ROOTFS"/opt/ddk/kdir/* >/dev/null || { echo "build: no /opt/ddk in the image" >&2; exit 1; }

# 2. The module's source at the pinned commit.
if [ ! -d "$SRC/.git" ]; then
  git clone -q "$REPO" "$SRC"
fi
if [ "$(git -C "$SRC" rev-parse HEAD)" != "$COMMIT" ]; then
  git -C "$SRC" fetch -q --depth 1 origin "$COMMIT" 2>/dev/null || git -C "$SRC" fetch -q origin
  git -C "$SRC" checkout -q "$COMMIT"
fi
[ -z "$(git -C "$SRC" status --porcelain)" ] || { echo "build: $SRC has local changes" >&2; exit 1; }

# 3. The .ko, in the image, as docker_exec.sh builds it (into a folder of its own: it names the file $TAG.ko).
BOUT=$OUT/.build${VARIANT:+-$VARIANT}
rm -rf "$BOUT" "${OUT:?}/$KO"
mkdir -p "$BOUT"
bwrap --unshare-all --die-with-parent --clearenv \
  --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin --setenv HOME /tmp \
  --ro-bind "$ROOTFS" / --proc /proc --dev /dev --tmpfs /tmp \
  --ro-bind "$SRC" /src --bind "$BOUT" /out_gh_hugepage_reserve \
  sh /src/docker_exec.sh gh_hugepage_reserve.c:gh_hugepage_reserve
[ -f "$BOUT/$TAG.ko" ] || { echo "build: no $TAG.ko" >&2; exit 1; }
mv "$BOUT/$TAG.ko" "$OUT/$KO"
rmdir "$BOUT" 2>/dev/null || true
if [ -n "$VARIANT" ]; then
  { echo "image $IMAGE:$TAG $DIGEST"; echo "source $REPO $COMMIT"; echo "ko sha256 $(sha "$OUT/$KO")"; modinfo "$OUT/$KO"; } > "$OUT/$RECORD"
  cat "$OUT/$RECORD"
  exit 0
fi

# 4. kapi_check for the PC (libbpf), from the same commit's abi/.
awk -f "$SRC/abi/gen_kapi.awk" "$SRC/abi/kapi_abi.tsv" > "$OUT/kapi_abi.gen.h"
cc -O2 -Wall -I"$OUT" -I"$SRC/abi" "$SRC/abi/kapi_check.c" -lbpf -lelf -lz -o "$OUT/kapi_check"

{
  echo "image $IMAGE:$TAG $DIGEST"
  echo "source $REPO $COMMIT"
  echo "ko sha256 $(sha "$OUT/$TAG.ko")"
  modinfo "$OUT/$TAG.ko"
} > "$OUT/build.txt"
cat "$OUT/build.txt"
