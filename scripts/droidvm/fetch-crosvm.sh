#!/bin/sh
# scripts/droidvm/fetch-crosvm.sh [DIR [LINE]]: Droid-VM's own build of crosvm for Android (BSD-3-Clause, with Droid-VM's
# GPL-2.0-or-later additions; source https://github.com/Droid-VM/crosvm, wip/hackintosh) and the three libraries of
# its package that the phone does not have, from their published prebuilts (https://github.com/Droid-VM/
# DroidVM-Prebuilt-Root, manual-build/arm64-v8a), at a pinned commit, each file checked against its SHA-256.
# This crosvm has the pseudo-unprotected VM (--protected-vm-pseudo-unprotected): a protected VM whose RAM is SHAREd
# with it after start, so Android can still read it (docs/native.md "The shared-RAM VM"); it needs
# gunyah_host_share (scripts/hugepage/build-share.sh). crosvm finds the libraries next to itself (RUNPATH $ORIGIN);
# the rest it links (libbase, libcutils, libminijail, libcap, ...) are the phone's own, in /system/lib64.
# Built by Droid-VM, not here: building it needs their AOSP tree (crosvm-minimal-manifest, tens of GB). The owner
# chose the prebuilt (2026-11-23). Nothing of it goes into this repository.
#
# LINE: droidvm (the default: their main line, 82c59c2 of 2026-09-12) or hackintosh (their macOS branch, f7c6ce8 of
# 2026-11-11: the crosvm of their XNU guests, Droid-VM/crosvm wip/hackintosh 0f2db62ae, with "the virtio-gpu display
# import"; its three libraries are the main line's, byte for byte). The main line's crosvm refuses the kernel's scanout
# resources (context-less guest blobs) with EINVAL: it hands virglrenderer no pages for a blob that comes with its
# udmabuf, which virglrenderer requires (docs/native.md "The GPU in the shared-RAM VM").
#
# libvirglrenderer.so of both lines is virglrenderer cd71ea4 (2026-09-01). take18 builds a newer one from source,
# scripts/droidvm/build-virgl.sh (2529a9f), which vm-boot pushes in its place (`vm-boot --virgl`, `--virgl prebuilt` for this one).
#
# libvulkan_freedreno.so is their Turnip (Mesa, MIT; the same file in both lines): the Vulkan driver crosvm's Android
# display blits the VM's frames with when it is named (CROSVM_DISPLAY_VULKAN_LIBRARY, set by vm-boot with the scanout).
#
# Output: DIR/82c59c2/ or DIR/f7c6ce8/ {crosvm,libvirglrenderer.so,libgfxstream_backend.so,libvncserver.so,
#         libvulkan_freedreno.so}
set -eu
DIR=${1:-/mnt/tkg-build/oneios/dl/droidvm-prebuilt}
case ${2:-droidvm} in
  droidvm) COMMIT=82c59c2834cb7e2f0284b03849fc921057804b75 CROSVM=cd3693ba876720f7ab7c08e5d6358101336f3bbcdb3d7d4f324ac29274bf6a7b ;;
  hackintosh) COMMIT=f7c6ce856d5df95bc9cc04599b390ad2435bdc5a CROSVM=7994700ebe50b88d81ff86c8f1a1765912081ba162d976276139b6da2ae57bbd ;;
  *) echo "usage: fetch-crosvm.sh [DIR [droidvm|hackintosh]]" >&2; exit 2 ;;
esac
BASE=https://raw.githubusercontent.com/Droid-VM/DroidVM-Prebuilt-Root/$COMMIT/manual-build/arm64-v8a/usr
OUT=$DIR/$(printf %.7s "$COMMIT")
mkdir -p "$OUT"
# FILE PATH-UNDER-usr SHA-256
while read -r f at sum; do
  [ -n "$f" ] || continue
  if [ -f "$OUT/$f" ] && [ "$(sha256sum < "$OUT/$f" | cut -d' ' -f1)" = "$sum" ]; then
    continue
  fi
  echo "fetch-crosvm: $f"
  curl -fsSL --retry 3 -A git-client -o "$OUT/$f.part" "$BASE/$at"
  got=$(sha256sum < "$OUT/$f.part" | cut -d' ' -f1)
  [ "$got" = "$sum" ] || { echo "fetch-crosvm: $f: sha256 $got, not $sum" >&2; rm -f "$OUT/$f.part"; exit 1; }
  mv "$OUT/$f.part" "$OUT/$f"
done <<EOF
crosvm bin/crosvm $CROSVM
libvirglrenderer.so lib/libvirglrenderer.so 40b5edaa7c722b801b2b08d2f2d64fd55e56bebf62baec2dbb17e662a49f633d
libgfxstream_backend.so lib/libgfxstream_backend.so ed5395e443fb2b3f71f647e80e6f434e93b342b463ce841c9319903affedbf52
libvncserver.so lib/libvncserver.so eaf7d95c7f4fe0ca8a935192442f332e9313ba51563c5990a063759348eeeb00
libvulkan_freedreno.so lib/libvulkan_freedreno.so 829655e69d4620c2caaf737ccc3ea08afe1cc3d450b42460ac4a2a3ab7bd9abf
EOF
echo "fetch-crosvm: $OUT"
ls -l "$OUT"
