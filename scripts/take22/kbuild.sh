#!/bin/bash
# scripts/take22/kbuild.sh: take22's native kernel and its kernel collection, from this checkout (docs/TAKE22-GUIDE.md).
# The recipe the owner ran (from his work tree) with the paths made relative: oneios-tool first (the staged edit list of
# the kernel's drivers is compiled into it), then kernel/build-native.sh with NATIVE_OUT=take22, then kernel/prelink.
#   LIBDISPATCH_PREFIX=DIR   where libdispatch/BlocksRuntime live when not in /usr (docs/building.md section 0)
#   LOGS=DIR                 the three logs (default out/take22-logs)
# -> out/kernel/native-take22/kernel.kc
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
export NATIVE_OUT=take22 GIT_OPTIONAL_LOCKS=0
LOGS=${LOGS:-$R/out/take22-logs}
mkdir -p "$LOGS"
cd "$R/tools/oneios-tool"
cargo build --release > "$LOGS/tool-build.log" 2>&1 || { echo "kbuild: oneios-tool failed ($LOGS/tool-build.log)" >&2; exit 1; }
export ONEIOS_TOOL=$R/tools/oneios-tool/target/release/oneios-tool
cd "$R"
kernel/build-native.sh > "$LOGS/kernel-build.log" 2>&1 || { echo "kbuild: kernel failed ($LOGS/kernel-build.log)" >&2; exit 1; }
KERNEL=$R/out/kernel/current-native-take22/xnu.macho OUT=$R/out/kernel/native-take22 kernel/prelink/build.sh > "$LOGS/prelink.log" 2>&1 \
  || { echo "kbuild: prelink failed ($LOGS/prelink.log)" >&2; exit 1; }
ls -la "$R/out/kernel/native-take22/kernel.kc"
