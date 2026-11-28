#!/bin/sh
# pagecensus on a made-up /proc/kpageflags: 6 windows from pfn 0x200:
#   0 all free; 1 free + LRU; 2 one slab page among free ones; 3 a whole 2 MiB allocation (head + 511 tails);
#   4 page tables and a flagless kernel page among LRU pages; 5 reserved
set -eu
bin=$1
t=$(mktemp -d)
python3 - "$t/flags" <<'PY'
import struct, sys
BUDDY, LRU, SLAB, HEAD, TAIL, PGT, RES = 1 << 10, 1 << 5, 1 << 7, 1 << 15, 1 << 16, 1 << 26, 1 << 32
w = [[BUDDY] * 512, [BUDDY] * 256 + [LRU] * 256, [BUDDY] * 511 + [SLAB], [HEAD] + [TAIL] * 511,
     [LRU] * 500 + [PGT] * 11 + [0], [RES] * 512]
with open(sys.argv[1], "wb") as f:
    f.write(b"\0" * (0x200 * 8))
    for win in w:
        f.write(struct.pack("<512Q", *win))
PY
out=$("$bin" --kpageflags "$t/flags" --ranges 200-e00)
echo "$out"
want() { echo "$out" | grep -qF -- "$1" || { echo "FAIL: no \"$1\""; exit 1; }; }
want "6 windows of 2 MiB in 1 RAM ranges (0 not read)"
want "whole 2 MiB allocations (the pool's pages and the like): 1 windows"
want "takeable: 1 windows all free, 1 with only free and movable pages (1 MiB to move)"
want "refused: 3 windows hold a page nothing moves, with 1 MiB free inside them"
want "by slab: 1 windows, 1 MiB free inside, 4 KiB of such pages"
want "by pagetable other-kernel: 1 windows, 0 MiB free inside, 48 KiB of such pages"
want "by reserved: 1 windows"
want "1-4 pages 1 windows (1 MiB free inside), 5-64 1 (0 MiB), more 1 (0 MiB)"
rm -rf "$t"
echo "pagecensus: test passed"
