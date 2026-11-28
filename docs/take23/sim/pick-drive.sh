# usage: pick-drive.sh MEMAVAIL_KB POOL_AVAIL PMAX GROW_TO
F=$PWD/fakep; rm -rf $F; mkdir -p $F/P $F/d; P=$F/P; D=$F/d
printf 'MemTotal: 11350016 kB\nMemAvailable:   %s kB\n' $1 > $F/meminfo
printf 'state=idle\npool_avail=%s\npool_total=%s\npool_want=%s\nacquire_active=0\n' $2 $2 $2 > $P/refill_stat
[ "$3" = none ] || echo $3 > $P/pool_size_max; : > $P/pool_want; : > $P/acquire
for r in 7168 6144 4608; do echo img$r > $D/k$r; done
stat_line() { tr '\n' ' ' < $P/refill_stat; }; mem_line() { cat $F/meminfo | tr '\n' ' '; }; end() { echo "END: $1"; }
# the module: when acquire is pressed, the pool reaches GROW_TO (if the want is that high), after 1 s
( while :; do if [ "$(cat $P/acquire 2>/dev/null)" = 3 ]; then : > $P/acquire; w=$(cat $P/pool_want); g=$4; [ "${w:-0}" -lt $g ] && g=${w:-0}; a=$(grep pool_avail= $P/refill_stat | cut -d= -f2); [ $g -gt $a ] || g=$a
  ma=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' $F/meminfo); printf 'MemAvailable:   %s kB\n' $((ma - (g - a) * 2048)) > $F/meminfo
  printf 'state=idle\npool_avail=%s\npool_total=%s\npool_want=%s\nacquire_active=0\n' $g $g $w > $P/refill_stat; fi; sleep 0.3; done ) & mod=$!
L="7168 3568 23f800000 k7168 $(sha256sum $D/k7168 | cut -d' ' -f1)
6144 3056 1ff800000 k6144 $(sha256sum $D/k6144 | cut -d' ' -f1)
4608 2288 19f800000 k4608 $(sha256sum $D/k4608 | cut -d' ' -f1)
"
python3 - "$L" <<'P' > pick-run.sh
import sys
s=open('pick.tpl').read().replace('__LIST__',sys.argv[1]).replace('/proc/meminfo','$F/meminfo')
print(s)
P
( . ./pick-run.sh; echo "RESULT RAM=$RAM NEED=$NEED ARENA=$ARENA KIMG=$KIMG" )
kill $mod 2>/dev/null
