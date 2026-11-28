# usage: fill-drive.sh MEMAVAIL_MIB POOL_AVAIL POOL_WANT PMAX|none KEEP "GAINS" [census]
# take24's fill and choice (oneios-tool, crates/oneios-phone/src/take23.rs PICK) under the PC's sh against fake files.
# GAINS: the pool's pages after each press of the module's acquire, in order (the last one repeats), bounded by the want.
SRC=${SRC:-$(cd "$(dirname "$0")/../../.." && pwd)}
F=$PWD/fake; rm -rf $F; mkdir -p $F/P $F/d $F/proc/sys/vm $F/slab/a $F/slab/b; P=$F/P; D=$F/d
printf 'MemTotal: 11379708 kB\nMemFree: %s kB\nMemAvailable:   %s kB\nCmaFree: 600000 kB\nSlab: 500000 kB\nSUnreclaim: 400000 kB\nPageTables: 90000 kB\nKernelStack: 60000 kB\nVmallocUsed: 400000 kB\nUnevictable: 100000 kB\nShmem: 20000 kB\nKgslShmemUsage: 90000 kB\n' $(($1 * 1024 - 90000)) $(($1 * 1024)) > $F/proc/meminfo
printf 'Page block order: 9\nNode    0, zone   Normal, type    Unmovable    100     90     80     70     60     50     40     30     20      1      0 \nNode    0, zone   Normal, type      Movable   9000   8000   7000   6000   5000   4000   3000   2000   1000      5      2 \n\nNumber of blocks type     Unmovable      Movable  Reclaimable   HighAtomic          CMA      Isolate \nNode 0, zone   Normal         1581         3722          121            0          312            0 \n' > $F/proc/pagetypeinfo
: > $F/proc/sys/vm/drop_caches; : > $F/proc/sys/vm/compact_memory; : > $F/slab/a/shrink; : > $F/slab/b/shrink
printf 'state=idle\npool_avail=%s\npool_total=%s\npool_want=%s\nacquire_active=0\nacquire_stop_reason=idle\n' $2 $2 $3 > $P/refill_stat
[ "$4" = none ] || echo $4 > $P/pool_size_max; echo $3 > $P/pool_want; : > $P/acquire; echo "o9_seen=1 del_hit=2" > $P/reclaim_debug
for r in 8192 7168 6144 4608; do echo img$r > $D/k$r; done
[ -z "$7" ] || { printf '#!/bin/sh\necho "pagecensus: 5000 windows of 2 MiB"\n' > $D/pagecensus; chmod 755 $D/pagecensus; }
now() { date +%s; }; stat_line() { tr '\n' ' ' < $P/refill_stat; }; mem_line() { grep -E '^(MemFree|MemAvailable):' $F/proc/meminfo | tr '\n' ' '; }; end() { echo "END: $1"; }
( n=1; while :; do m=$(cat $P/acquire 2>/dev/null); if [ "$m" = 3 ] || [ "$m" = 2 ]; then : > $P/acquire
  g=$(echo "$6" | cut -d' ' -f$n); [ -n "$g" ] || g=$(echo "$6" | awk '{print $NF}'); n=$((n + 1))
  w=$(cat $P/pool_want); a=$(grep pool_avail= $P/refill_stat | cut -d= -f2); [ $g -le $w ] || g=$w; [ $g -gt $a ] || g=$a
  r="scanned all present memory"; [ $g -lt $w ] || r="reached target"
  ma=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' $F/proc/meminfo); sed -i "s/^MemAvailable:.*/MemAvailable:   $((ma - (g - a) * 2048)) kB/" $F/proc/meminfo
  printf 'state=idle\npool_avail=%s\npool_total=%s\npool_want=%s\nacquire_active=0\nacquire_stop_reason=%s\n' $g $g $w "$r" > $P/refill_stat; fi; sleep 0.2; done ) & mod=$!
sha() { sha256sum $D/$1 | cut -d' ' -f1; }
L="8192 4080 27f800000 k8192 $(sha k8192)
7168 3568 23f800000 k7168 $(sha k7168)
6144 3056 1ff800000 k6144 $(sha k6144)
4608 2288 19f800000 k4608 $(sha k4608)
"
python3 - "$SRC" "$L" "$5" <<'P' > fill-run.sh
import sys
s=open(sys.argv[1]+'/tools/oneios-tool/crates/oneios-phone/src/take23.rs').read()
a=s.index('const PICK: &str = r##"')+len('const PICK: &str = r##"'); b=s.index('"##;',a)
s=s[a:b].replace('__LIST__',sys.argv[2]).replace('__KEEP__',sys.argv[3]).replace('__OVER__','600').replace('__MAXNEED__',sys.argv[2].split()[1])
print(s.replace('/proc/','$F/proc/').replace('/sys/kernel/slab/','$F/slab/'))
P
( . ./fill-run.sh; echo "RESULT RAM=$RAM NEED=$NEED ARENA=$ARENA KIMG=$KIMG want=$(cat $P/pool_want) slab=$(cat $F/slab/a/shrink) compact=$(cat $F/proc/sys/vm/compact_memory)" )
kill $mod 2>/dev/null
