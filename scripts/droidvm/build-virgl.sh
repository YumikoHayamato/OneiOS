#!/bin/sh
# scripts/droidvm/build-virgl.sh: libvirglrenderer.so for the phone (Android arm64, NDK r28c, API 35) from Droid-VM/virglrenderer,
# the way their AOSP build (Soong, Android.bp "libvirglrenderer") makes it: the same source list, the same
# prebuilt-intermediates/config.h, libepoxy linked in statically, liblog from the system. Two differences:
#   - libcutils is not linked: its one use (property_get, src/mesa/util/os_misc.c) is served by a 10-line shim over
#     bionic's __system_property_get (virgl/cutils_shim.c), which is what libcutils' own property_get does, and the two
#     AOSP headers the source includes are reduced to what it uses (virgl/compat: log/log.h, cutils/properties.h);
#   - libc++ is not linked (virglrenderer is C; the AOSP binary's NEEDED libc++.so imports nothing).
# Usage: build-virgl.sh WORK [COMMIT [OUT]]
#   WORK    a scratch folder (tmpfs is fine, ~1 GB): sources, libepoxy and libdrm's headers are fetched into it
#   COMMIT  a commit of https://github.com/Droid-VM/virglrenderer (default 2529a9f, wip/hackintosh of 2026-11-13: take18's)
#   OUT     where the .so goes (default /mnt/tkg-build/oneios/dl/droidvm-prebuilt/virgl-COMMIT7, vm-boot --virgl's default
#           for 2529a9f: tools/oneios-tool/crates/oneios-phone/src/droidvm.rs VIRGL_TAKE18)
# Its ABI: virglrenderer.h is the same as cd71ea4's (the prebuilt's), and the .so exports every virgl_renderer_* function
# the prebuilt crosvm (f7c6ce8) imports, Droid-VM's own virgl_renderer_resource_export_display_blob and
# virgl_renderer_resource_set_guest_blob_fd among them (checked at the end).
# Needs: git, meson, ninja, python3 (libepoxy's generator), the NDK at $NDK
#        (default /mnt/giochi/Linux/oneios/tools/Android/Sdk/ndk/28.2.13676358).
set -eu
WORK=${1:?usage: build-virgl.sh WORK [COMMIT [OUT]]}
COMMIT=${2:-2529a9f77a1a3f9cd41d50b9174d2e41ea1b75dd}
OUT=${3:-/mnt/tkg-build/oneios/dl/droidvm-prebuilt/virgl-$(printf %.7s "$COMMIT")}
HERE=$(cd "$(dirname "$0")" && pwd)/virgl
PREBUILT_CROSVM=${PREBUILT_CROSVM:-/mnt/tkg-build/oneios/dl/droidvm-prebuilt/f7c6ce8/crosvm}
NDK=${NDK:-/mnt/giochi/Linux/oneios/tools/Android/Sdk/ndk/28.2.13676358}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=${API:-35}
CC="$TC/bin/aarch64-linux-android$API-clang"
SRCREPO=${VIRGL_REPO:-https://github.com/Droid-VM/virglrenderer.git}
EPOXY_TAG=1.5.10
DRM_TAG=libdrm-2.4.124
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
mkdir -p "$WORK" "$OUT"
cd "$WORK"
# meson's cross file for libepoxy (NDK clang, API level, static and position independent)
cat > android-aarch64.ini <<EOF2
[binaries]
c = '$TC/bin/aarch64-linux-android$API-clang'
cpp = '$TC/bin/aarch64-linux-android$API-clang++'
ar = '$TC/bin/llvm-ar'
strip = '$TC/bin/llvm-strip'
pkg-config = 'false'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
[built-in options]
c_args = ['-fPIC', '-O2']
default_library = 'static'
EOF2

# 1. the source at COMMIT
if [ ! -d src/.git ]; then
  rm -rf src && git init -q src && git -C src remote add origin "$SRCREPO"
fi
git -C src fetch -q --depth 1 origin "$COMMIT" 2>/dev/null || git -C src fetch -q origin
git -C src -c advice.detachedHead=false checkout -q --detach "$COMMIT"
git -C src clean -q -fdx
git -C src checkout -q -- .
echo "virgl: source $(git -C src rev-parse HEAD)"
# take22: VIRGL_PATCHES=DIR applies DIR/*.patch (name order) on the checkout: OneiOS's own changes to the host renderer
# (scripts/droidvm/virgl-patches). Without it the source is built as fetched (take18's 2529a9f build, bit for bit).
PATCHED=
if [ -n "${VIRGL_PATCHES:-}" ]; then
  for p in "$VIRGL_PATCHES"/*.patch; do
    [ -f "$p" ] || { echo "virgl: no patch in $VIRGL_PATCHES" >&2; exit 1; }
    git -C src apply --whitespace=nowarn "$p" || { echo "virgl: $p does not apply" >&2; exit 1; }
    echo "virgl: applied $(basename "$p")"; PATCHED="$PATCHED $(basename "$p")"
  done
fi

# 2. libepoxy, static, EGL only (what AOSP's static libepoxy is for Android)
if [ ! -f sysroot/lib/libepoxy.a ]; then
  [ -d epoxy-src ] || git clone -q --depth 1 --branch "$EPOXY_TAG" https://github.com/anholt/libepoxy.git epoxy-src
  rm -rf build-epoxy
  meson setup build-epoxy epoxy-src --cross-file "$WORK/android-aarch64.ini" --prefix "$WORK/sysroot" \
    -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false -Dbuildtype=release -Db_ndebug=true >epoxy-setup.log
  ninja -C build-epoxy install >epoxy-build.log
fi
# 3. libdrm's headers (Android.bp: header_libs libdrm_headers; no libdrm function is called on this build: the
#    drm2kgsl backend is a character device, so drm_renderer.c's drmOpenWithType path folds away; -z defs proves it)
[ -d libdrm-src ] || git clone -q --depth 1 --branch "$DRM_TAG" https://gitlab.freedesktop.org/mesa/drm.git libdrm-src

# 4. compile Android.bp's list
S=$WORK/src
awk '/^cc_library \{/{f=1} f&&/name: "libvirglrenderer"/{g=1} g&&/srcs: \[/{s=1;next} s&&/\],/{exit} s{gsub(/[",]/,""); gsub(/^ +/,""); if($0!="")print}' \
  "$S/Android.bp" > srcs.txt
[ "$(wc -l < srcs.txt)" -gt 50 ] || { echo "virgl: Android.bp's source list not found" >&2; exit 1; }
INC="-I$S/prebuilt-intermediates -I$S/prebuilt-intermediates/venus -I$S/src -I$S/src/drm -I$S/src/drm/drm-uapi \
 -I$S/src/gallium/auxiliary -I$S/src/gallium/auxiliary/util -I$S/src/gallium/include -I$S/src/mesa -I$S/src/mesa/compat \
 -I$S/src/mesa/pipe -I$S/src/mesa/util -I$S/src/venus -I$HERE/compat -I$WORK/sysroot/include -I$WORK/libdrm-src -I$WORK/libdrm-src/include/drm"
# Soong's defaults for a device C library (what the AOSP binary shows: fortify, stack protector, BIND_NOW, relro)
CFLAGS="-std=gnu17 -O2 -g0 -fPIC -ffunction-sections -fdata-sections -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
 -DHAVE_CONFIG_H -include $S/prebuilt-intermediates/config.h -D__USE_GNU -DHAVE_MEMFD_CREATE=1 -DANDROID -DNDEBUG \
 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers -Wno-sign-compare \
 -Wno-unused-but-set-variable -Wno-gnu-variable-sized-type-not-at-end -Wno-pointer-arith"
rm -rf obj && mkdir -p obj
cp "$HERE/cutils_shim.c" obj/cutils_shim.c
n=0
while read -r f; do
  n=$((n + 1))
  o=obj/$(echo "$f" | tr '/' '_').o
  echo "$CC $CFLAGS $INC -c $S/$f -o $o"
done < srcs.txt > compile.lst
echo "$CC -std=gnu17 -O2 -fPIC -c obj/cutils_shim.c -o obj/cutils_shim.o" >> compile.lst
# parallel compile, one log per run
: > compile.log
xargs -P "$JOBS" -I{} sh -c '{} >>compile.log 2>&1 || { echo "FAILED: {}" >>compile.log; exit 255; }' < compile.lst || {
  grep -B5 -A20 'error:' compile.log | head -120; exit 1; }
echo "virgl: compiled $n files ($(grep -c 'warning:' compile.log) warnings)"

# 5. link
"$CC" -shared -o "$OUT/libvirglrenderer.so" obj/*.o "$WORK/sysroot/lib/libepoxy.a" \
  -Wl,-soname,libvirglrenderer.so -Wl,-z,defs -Wl,-z,now -Wl,-z,relro -Wl,--gc-sections -Wl,--build-id=sha1 \
  -Wl,-z,max-page-size=16384 -llog -lm -ldl
"$TC/bin/llvm-strip" --strip-unneeded "$OUT/libvirglrenderer.so"
echo "virgl: $OUT/libvirglrenderer.so $(wc -c < "$OUT/libvirglrenderer.so") bytes, sha256 $(sha256sum "$OUT/libvirglrenderer.so" | cut -d' ' -f1)"
# 6. what crosvm needs of it: every virgl_* function the prebuilt crosvm imports (when that crosvm is on this PC)
if [ -f "$PREBUILT_CROSVM" ]; then
  "$TC/bin/llvm-readelf" --dyn-syms -W "$PREBUILT_CROSVM" | awk '$7=="UND"{print $8}' | sed 's/@.*//' | grep '^virgl' | sort -u > needs.txt
  "$TC/bin/llvm-readelf" --dyn-syms -W "$OUT/libvirglrenderer.so" | awk '$7!="UND"{print $8}' | sed 's/@.*//' | sort -u > has.txt
  missing=$(comm -23 needs.txt has.txt)
  [ -z "$missing" ] || { echo "virgl: crosvm imports what this build lacks: $missing" >&2; exit 1; }
  echo "virgl: exports all $(wc -l < needs.txt) virgl functions $PREBUILT_CROSVM imports"
fi
"$TC/bin/llvm-readelf" -d "$OUT/libvirglrenderer.so" | grep -q 'NEEDED.*libcutils' && { echo "virgl: links libcutils" >&2; exit 1; }
echo "virgl: source $(git -C src rev-parse HEAD), libepoxy $EPOXY_TAG (static), libdrm headers $DRM_TAG, NDK $(basename "$NDK") API $API${PATCHED:+; patches:$PATCHED}" > "$OUT/libvirglrenderer.so.build.txt"
