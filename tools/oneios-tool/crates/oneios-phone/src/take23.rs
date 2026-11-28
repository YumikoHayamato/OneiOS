//! take23, `phone vm-boot`'s three additions. Each is text put into the script boot_script_with made, at a line this file
//! finds in it (and refuses the boot when it does not find it): without their options the script is the one it was.
//!
//! 1. The new boot order (`--fill-after-stop`, `--image-larger FILE`...): One UI's framework stops first, the pool is filled
//!    then, and the VM gets the largest Image that fits. take22 and before: the pool was filled before the session (`hugepool
//!    grow`, beside what Android kept), and vm-boot refused an Image when Android's MemAvailable plus the pool was not the VM
//!    plus 1024 MiB BEFORE the stop. What the sessions say (run.txt, take19 to take22b): after the stop Android has 3970 to
//!    4767 MiB available beside a pool that already holds the VM. So the rule (`--host-keep-mib`, 1024 MiB left to Android
//!    beside the VM) is asked AFTER the stop and the cache drop, per candidate, the largest first; a candidate whose pool pages
//!    do not come (fragmented memory) gives way to the next. What Android itself needs while the VM runs was never measured
//!    (no session logged it): hostmem.txt (below) does, from now on.
//! 2. hostmem.txt: every 5 s MemAvailable, MemFree, the pool's free and whole pages, crosvm's resident memory.
//! 3. The clocks' orchestrator (`--orchestrator`): see ORCH.
use anyhow::{bail, Result};

/// One Image the VM may run: its RAM, the pool pages it takes, drm2kgsl's host arena (hex digits, as crosvm's tree names it;
/// empty without the GPU), its file's name in the VM's folder and its SHA-256.
#[derive(Clone, Debug, PartialEq)]
pub struct Candidate {
    pub ram: u32,
    pub need: u64,
    pub arena: String,
    pub file: String,
    pub sha: String,
}

/// vm-boot's take23 options (vmboot::Options::take23).
#[derive(Clone, Debug, Default)]
pub struct Opts {
    /// --image-larger FILE, repeatable: Images of the same build with more RAM
    pub larger: Vec<String>,
    /// --fill-after-stop: the memory rule asked after One UI's stop (implied by --image-larger)
    pub fill_after_stop: bool,
    /// --host-keep-mib N: what Android keeps available beside the VM
    pub keep_mib: u32,
    /// --orchestrator
    pub orchestrator: bool,
    /// --orchestrator-set KEY=VALUE, repeatable
    pub orch_set: Vec<String>,
}

pub const KEEP_MIB: u32 = 1024;
/// The most seconds the choice may spend on one candidate (two settles of 30 s, the acquire's 3 + 120 + 2, the hash): the
/// takeover's failsafe is given this much more for each (vm_boot).
pub const PICK_SECONDS: u32 = 200;
/// The most RAM of an Image in the new order (the loader's own limit).
pub const MAX_RAM_MIB: u32 = 8192;

/// The choice, in the takeover, right before the pool's top-up (takeover_script's `need=N` line): `cands` the largest first,
/// the main Image last. It leaves RAM, NEED, ARENA and KIMG for the lines apply_pick rewrote. A candidate is taken when
/// (a) the module's table holds its pages (pool_size_max: `hugepool load --system-reserve-mb`), (b) Android keeps KEEP MiB
/// available once the pool has them (MemAvailable now, less 2 MiB a page the pool still lacks, less the 32 MiB of the VM
/// outside the pool), (c) its file is the one that was pushed, and (d) the pages come
/// (the module's acquire, mode 3, 120 s at most). Pages the pool has beyond the chosen Image's stay in it (the module's
/// shrink has never run in a session of ours: not asked for). None taken: the session ends before crosvm starts, One UI
/// comes back (the trap).
pub fn pick_block(cands: &[Candidate], keep_mib: u32) -> String {
    let mut list = String::new();
    for c in cands {
        list += &format!("{} {} {} {} {}\n", c.ram, c.need, if c.arena.is_empty() { "-" } else { &c.arena }, c.file, c.sha);
    }
    PICK.replace("__KEEP__", &keep_mib.to_string()).replace("__LIST__", &list)
}

const PICK: &str = r##"# take23 (vm-boot --fill-after-stop / --image-larger): the largest Image that fits, now that One UI is down
KEEP=__KEEP__
PMAX=$(cat $P/pool_size_max 2>/dev/null); case $PMAX in ''|*[!0-9]*|0) PMAX=; echo "pick: the module's pool_size_max is not readable: its table is not asked (the pool's pages coming is the test)" ;; esac
RAM=; NEED=; ARENA=; KIMG=
pool_n() { v=$(grep "^$1=" $P/refill_stat 2>/dev/null | cut -d= -f2); case $v in ''|*[!0-9]*) v=0 ;; esac; echo $v; }
pool_settle() { ps=0; while [ $ps -lt $1 ] && ! grep -q '^acquire_active=0$' $P/refill_stat; do sleep 1; ps=$((ps + 1)); done; }
while read cram cneed carena cfile csha; do
  [ -n "$csha" ] || continue
  ma=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' /proc/meminfo); ma=$((${ma:-0} / 1024))
  pa=$(pool_n pool_avail)
  more=$((cneed - pa)); [ $more -gt 0 ] || more=0
  left=$((ma - 2 * more - 32))
  if [ -n "$PMAX" ] && [ $cneed -gt $PMAX ]; then
    echo "pick: $cram MiB: no: it takes $cneed pool pages and the module's table holds $PMAX (hugepool load --system-reserve-mb)"; continue
  fi
  if [ $left -lt $KEEP ]; then
    echo "pick: $cram MiB: no: Android has $ma MiB available and the pool $pa free pages; beside this VM $left MiB would be left, under $KEEP"; continue
  fi
  if [ "$(sha256sum $D/$cfile 2>/dev/null | cut -d' ' -f1)" != "$csha" ]; then
    echo "pick: $cram MiB: no: $D/$cfile is not the file that was pushed"; continue
  fi
  if [ $pa -lt $cneed ]; then
    pool_settle 30
    [ "$(pool_n pool_want)" -ge $cneed ] || echo $cneed > $P/pool_want
    echo 3 > $P/acquire; sleep 3 </dev/null
    pool_settle 120
    [ $ps -lt 120 ] || { echo 0 > $P/acquire; sleep 2 </dev/null; }
    pa=$(pool_n pool_avail)
    if [ $pa -lt $cneed ]; then
      echo "pick: $cram MiB: no: the pool reached $pa of $cneed pages in $ps s ($(stat_line))"; continue
    fi
  fi
  RAM=$cram; NEED=$cneed; ARENA=$carena; KIMG=$D/$cfile
  echo "pick: $cram MiB: yes: Android had $ma MiB available, the pool has $pa free pages of the $cneed it takes; about $left MiB stay to Android"
  break
done <<'ONEIOS_PICK'
__LIST__ONEIOS_PICK
if [ -z "$RAM" ]; then
  echo "pick: no Image fits: memory $(mem_line); $(stat_line)"
  end "refused memory: no Image of the list fits with $KEEP MiB left to Android"; exit 1
fi
# Pages the pool has beyond this VM's stay in it: giving them back would be the module's shrink (a lower pool_want), which no
# session of ours has run; they are not counted as Android's above either.
spare=$(( $(pool_n pool_avail) - NEED )); [ $spare -le 0 ] || echo "pick: $spare pages (2 MiB each) stay unused in the pool"
echo "pick: the VM runs the Image of $RAM MiB ($KIMG)"
"##;

/// The script with the choice in it: `block` before the takeover's `need=N` line, and the main Image's four own values
/// (the pool pages, crosvm's --mem, the arena's address, the kernel's file) read from what the choice left.
pub fn apply_pick(script: &str, main: &Candidate, block: &str) -> Result<String> {
    let one = |s: &str, from: &str, to: &str, what: &str| -> Result<String> {
        if s.matches(from).count() != 1 {
            bail!("take23: the boot script has not exactly one \"{from}\" ({what}): {} found", s.matches(from).count());
        }
        Ok(s.replace(from, to))
    };
    // the script's own values first, the block (whose list names every candidate's values, the main Image's too) last
    let s = one(script, &format!("--mem size={} ", main.ram), "--mem size=$RAM ", "crosvm's --mem")?;
    let mut s = one(&s, " $D/kernel.img </dev/null >$D/crosvm.log", " $KIMG </dev/null >$D/crosvm.log", "crosvm's kernel")?;
    if !main.arena.is_empty() {
        let n = s.matches(main.arena.as_str()).count();
        if n < 2 {
            bail!("take23: the boot script names the GPU's arena {} {n} times, not 2 or more", main.arena);
        }
        s = s.replace(main.arena.as_str(), "$ARENA");
    }
    one(&s, &format!("need={}\n", main.need), &format!("{block}need=$NEED\n"), "the takeover's pool pages")
}

/// hostmem.txt, started with the wait for the VM's end: Android's memory beside the VM, every 5 s.
pub const HOSTMEM: &str = r##"# take23: Android's memory while the VM runs (no session before take23 logged it)
(
  echo "# time MemAvailable MemFree (MiB) pool_avail pool_total (pages of 2 MiB) crosvm VmRSS (MiB)"
  while kill -0 $pid 2>/dev/null; do
    ma=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' /proc/meminfo); mf=$(sed -n 's/^MemFree: *\([0-9]*\).*/\1/p' /proc/meminfo)
    pa=$(grep '^pool_avail=' $P/refill_stat 2>/dev/null | cut -d= -f2); pt=$(grep '^pool_total=' $P/refill_stat 2>/dev/null | cut -d= -f2)
    rs=$(sed -n 's/^VmRSS: *\([0-9]*\).*/\1/p' /proc/$pid/status 2>/dev/null)
    echo "$(date +%T) $((${ma:-0} / 1024)) $((${mf:-0} / 1024)) ${pa:--} ${pt:--} $((${rs:-0} / 1024))"
    sleep 5
  done
) > $D/hostmem.txt 2>&1 &
echo "hostmem: Android's memory beside the VM every 5 s: hostmem.txt"
"##;

/// The orchestrator's numbers (--orchestrator-set KEY=VALUE).
#[derive(Clone, Debug, PartialEq)]
pub struct Orch {
    /// The GPU cap's lowest step, MHz
    pub gpu_floor: u32,
    /// GPU busy % at or above which the cap goes one step up (twice that step at `gpu_up + 15`)
    pub gpu_up: u32,
    /// GPU busy % at or under which, for `gpu_hold` looks, the cap goes one step down
    pub gpu_down: u32,
    pub gpu_hold: u32,
    /// The vCPUs' clamp (the renderer's uclamp.min while the compositor animates): its lowest and its step
    pub clamp_floor: u32,
    pub clamp_step: u32,
    /// The guest's swaps on time (percent of those while moving): at or above `good` the clamp goes a step down, under `bad`
    /// two steps up and the GPU's cap to its top
    pub good: u32,
    pub bad: u32,
    /// Degrees C of the hotter CPU sensor at which the clamp and the cap are stepped down whatever the frames say
    pub hot: u32,
}

impl Orch {
    pub const DEFAULT: Orch = Orch { gpu_floor: 607, gpu_up: 55, gpu_down: 30, gpu_hold: 8, clamp_floor: 512, clamp_step: 128, good: 92, bad: 85, hot: 98 };

    pub fn parse(words: &[String]) -> Result<Orch> {
        let mut o = Orch::DEFAULT;
        for w in words {
            let (k, v) = w.split_once('=').ok_or_else(|| anyhow::anyhow!("--orchestrator-set {w}: KEY=VALUE"))?;
            let v: u32 = v.parse().map_err(|_| anyhow::anyhow!("--orchestrator-set {w}: a whole number"))?;
            match k {
                "gpu-floor" => o.gpu_floor = v,
                "gpu-up" => o.gpu_up = v,
                "gpu-down" => o.gpu_down = v,
                "gpu-hold" => o.gpu_hold = v,
                "clamp-floor" => o.clamp_floor = v,
                "clamp-step" => o.clamp_step = v,
                "good" => o.good = v,
                "bad" => o.bad = v,
                "hot" => o.hot = v,
                _ => bail!("--orchestrator-set {k}: gpu-floor, gpu-up, gpu-down, gpu-hold, clamp-floor, clamp-step, good, bad or hot"),
            }
        }
        if !(607..=1200).contains(&o.gpu_floor) || !(20..=95).contains(&o.gpu_up) || o.gpu_down >= o.gpu_up || !(1..=120).contains(&o.gpu_hold)
            || o.clamp_floor > 1024 || !(16..=512).contains(&o.clamp_step) || !(50..=100).contains(&o.good) || o.bad >= o.good || !(70..=110).contains(&o.hot)
        {
            bail!("--orchestrator-set: gpu-floor 607..1200 MHz (the touch boost's lowest clock is 607: a cap under it would fight it), gpu-up 20..95 %, gpu-down under gpu-up, gpu-hold 1..120 looks, clamp-floor 0..1024, clamp-step 16..512, good 50..100 %, bad under good, hot 70..110 C");
        }
        Ok(o)
    }
}

/// The clocks' orchestrator (`vm-boot --orchestrator`; with --takeover, the GPU's scanout and take18's renderer with its
/// `deadline` floors). What it replaces: with those floors the renderer holds the GPU at its TOP level and the vCPU threads'
/// uclamp.min at 1024 whenever the compositor submits 60 times a second or more. take22b (thermal.txt, perf.txt; take23's
/// analysis): the GPU sat at 1200 MHz for 67 % of the session at 16 % busy; a frame's GPU time was under 4 ms for 86 % of
/// the frames; the busiest vCPU thread of a minute needed 1.5 GHz of the 2.6 the small cluster ran at.
/// What it does, while crosvm runs:
/// - the GPU, 4 looks a second: the cap (Samsung's gpu_max_clock, the file the takeover lifts; KGSL's constraint "the top
///   level" is then that cap) starts at `gpu_floor` and goes up a step when the GPU is `gpu_up` % busy or more at it (two
///   steps at 15 % more), down a step after `gpu_hold` looks at `gpu_down` % or less. The clocks under the cap stay the
///   governor's and the touch boost's.
/// - the vCPUs, once a round of the guest (10 s): the kernel's `oneios-display-pace` line in serial.txt says how many
///   swaps came on time while the screen moved. At `good` % or more the clamp the renderer gives the vCPU threads at the
///   next animation (its file, /data/local/tmp/drm2kgsl-vcpu-uclamp) goes a step down, to `clamp_floor` at the least; under
///   `bad` % it goes two steps up and the GPU's cap to the top until the next round.
/// - heat: at `hot` degrees on a CPU sensor the clamp and the cap go a step down every 2 s whatever the frames say (at
///   100-105 C the SoC throttles by itself, harder).
/// orch.txt: a line at each change and one every 5 s (the cap, the clamp, GPU busy mean and most, the clocks, the sensors,
/// the battery's current). At the end the cap is the lifted one again and the clamp's file is gone (the renderer's 1024);
/// caps_restore then puts Samsung's cap back as before.
pub fn orch_start(o: &Orch) -> String {
    ORCH.replace("__GF__", &o.gpu_floor.to_string())
        .replace("__GU__", &o.gpu_up.to_string())
        .replace("__GD__", &o.gpu_down.to_string())
        .replace("__GH__", &o.gpu_hold.to_string())
        .replace("__CF__", &o.clamp_floor.to_string())
        .replace("__CS__", &o.clamp_step.to_string())
        .replace("__GOOD__", &o.good.to_string())
        .replace("__BAD__", &o.bad.to_string())
        .replace("__HOT__", &o.hot.to_string())
}

const ORCH: &str = r##"# take23: the clocks' orchestrator (vm-boot --orchestrator; take23.rs ORCH)
orchp=
orch_end() {
  [ -n "$orchp" ] || return 0
  touch $D/orch.stop
  on=0; while [ $on -lt 30 ] && kill -0 $orchp 2>/dev/null; do sleep 0.1 2>/dev/null || sleep 1; on=$((on + 1)); done
  kill $orchp 2>/dev/null
  rm -f /data/local/tmp/drm2kgsl-vcpu-uclamp $D/orch.stop
  if [ -n "$OGM" ] && [ -n "$OTOP" ]; then echo $OTOP > $OGM 2>/dev/null; fi
  echo "orch: ended: the GPU's cap is $(cat $OGM 2>/dev/null) again, the vCPUs' clamp is the renderer's own; $(tail -n 1 $D/orch.txt 2>/dev/null)"
  orchp=
}
OGM=/sys/kernel/gpu/gpu_max_clock; OK=/sys/class/kgsl/kgsl-3d0
OTOP=$(cat $OGM 2>/dev/null); case $OTOP in ''|*[!0-9]*) OTOP= ;; esac
if [ ! -w $OGM ] || [ -z "$OTOP" ] || [ ! -r $OK/gpu_busy_percentage ]; then
  echo "orch: off: no writable $OGM or no $OK/gpu_busy_percentage on this phone"
else
  rm -f $D/orch.stop
  (
    oz() { for z in /sys/class/thermal/thermal_zone*; do case "$(cat $z/type 2>/dev/null)" in $1) echo $z/temp ;; esac; done; }
    ohot() { m=-1000000; for f in "$@"; do v=; { read -r v < $f; } 2>/dev/null; [ -n "$v" ] && [ "$v" -gt $m ] && m=$v; done; echo $((m / 1000)); }
    one() { v=; [ -r $1 ] && { read -r v < $1; } 2>/dev/null; v=${v:--}; }
    zs="$(oz 'cpu-0-*') $(oz 'cpu-1-*')"
    # the cap's steps: the GPU's own clocks (MHz) from the floor to the cap the takeover lifted
    steps=; for f in $(cat $OK/gpu_available_frequencies 2>/dev/null | tr ' ' '\n' | sort -n); do
      m=$((f / 1000000)); [ $m -ge __GF__ ] && [ $m -le $OTOP ] && steps="$steps $m"
    done
    [ -n "$steps" ] || steps=" $OTOP"
    nsteps=0; for m in $steps; do nsteps=$((nsteps + 1)); eval "os$nsteps=$m"; done; si=1
    step_mhz() { eval "echo \$os$1"; }
    cap=$(step_mhz $si); echo $cap > $OGM
    clamp=1024; echo $clamp > /data/local/tmp/drm2kgsl-vcpu-uclamp
    echo "# orch: GPU cap steps (MHz):$steps; up at __GU__ % busy, down after __GH__ looks at __GD__ % or less; clamp 1024 down to __CF__ by __CS__, on the guest's swaps on time (good __GOOD__ %, bad __BAD__ %); hot __HOT__ C"
    echo "# time cap(MHz) clamp busy-mean busy-max gpuclk(MHz) f0 f6 (kHz) hot(C) batt(uA) why"
    n=0; low=0; bsum=0; bmax=0; bn=0; seen=0; hold=0; why=start
    say() { one $OK/gpuclk; g=$v; one /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq; a=$v; one /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq; b=$v
      one /sys/class/power_supply/battery/current_now; i=$v
      echo "$(date +%T) $cap $clamp $((bn ? bsum / bn : 0)) $bmax $((${g:-0} / 1000000)) $a $b $t $i $1"; bsum=0; bmax=0; bn=0; }
    setcap() { [ $1 -lt 1 ] && set -- 1; [ $1 -gt $nsteps ] && set -- $nsteps; [ $1 = $si ] && return 1; si=$1; cap=$(step_mhz $si); echo $cap > $OGM; low=0; return 0; }
    setclamp() { c=$1; [ $c -lt __CF__ ] && c=__CF__; [ $c -gt 1024 ] && c=1024; [ $c = $clamp ] && return 1; clamp=$c; echo $clamp > /data/local/tmp/drm2kgsl-vcpu-uclamp; return 0; }
    t=$(ohot $zs); say start
    while kill -0 $pid 2>/dev/null && [ ! -e $D/orch.stop ]; do
      one $OK/gpu_busy_percentage; b=${v%% *}; b=${b%\%}; case $b in ''|*[!0-9]*) b=0 ;; esac
      bsum=$((bsum + b)); bn=$((bn + 1)); [ $b -gt $bmax ] && bmax=$b
      if [ $hold -gt 0 ]; then hold=$((hold - 1))
      elif [ $b -ge $((__GU__ + 15)) ]; then setcap $((si + 2)) && say "gpu $b% busy: two steps up"
      elif [ $b -ge __GU__ ]; then setcap $((si + 1)) && say "gpu $b% busy: a step up"
      elif [ $b -le __GD__ ]; then low=$((low + 1)); [ $low -ge __GH__ ] && { low=0; setcap $((si - 1)) && say "gpu at __GD__% or less for __GH__ looks: a step down"; }
      else low=0; fi
      n=$((n + 1))
      if [ $((n % 8)) = 0 ]; then
        t=$(ohot $zs)
        if [ $t -ge __HOT__ ]; then
          hc=; setcap $((si - 1)) && hc=1; setclamp $((clamp - __CS__)) && hc=1; [ -n "$hc" ] && say "hot: $t C: a step down"; hold=8
        fi
      fi
      if [ $((n % 20)) = 0 ]; then
        say tick
        # the guest's last round: swaps on time while the screen moved (serial.txt's newest oneios-display-pace line)
        l=$(tail -n 400 $D/serial.txt 2>/dev/null | grep -a 'oneios-display-pace: 10 s: swaps' | tail -n 1)
        k=$(echo "$l" | cksum); k=${k%% *}
        if [ -n "$l" ] && [ "$k" != "$seen" ]; then
          seen=$k
          set -- $(echo "$l" | sed -n 's/.*swaps +\([0-9]*\),.*<1 refresh +\([0-9]*\), 1 +\([0-9]*\), 2 (one skipped) +\([0-9]*\), more +\([0-9]*\),.*/\1 \2 \3 \4 \5/p')
          if [ $# = 5 ] && [ $(( $2 + $3 + $4 + $5 )) -ge 100 ]; then
            ok=$(( ($2 + $3) * 100 / ($2 + $3 + $4 + $5) ))
            if [ $ok -lt __BAD__ ]; then setclamp $((clamp + 2 * __CS__)); setcap $nsteps; hold=40; say "guest: $ok% of $(( $2 + $3 + $4 + $5 )) swaps on time: clamp two steps up, the cap at the top for 10 s"
            elif [ $ok -ge __GOOD__ ] && [ $t -lt __HOT__ ]; then setclamp $((clamp - __CS__)) && say "guest: $ok% of $(( $2 + $3 + $4 + $5 )) swaps on time: clamp a step down"
            else say "guest: $ok% of $(( $2 + $3 + $4 + $5 )) swaps on time: kept"; fi
          fi
        fi
      fi
      sleep 0.25 2>/dev/null || sleep 1
    done
    say end
  ) > $D/orch.txt 2>&1 &
  orchp=$!
  echo "orch: the clocks' orchestrator ($orchp): the GPU's cap from __GF__ MHz by its load, the vCPUs' clamp by the guest's swaps on time: orch.txt"
fi
"##;

/// `text` before the script's wait for the VM's end (the line that waits `seconds` for crosvm), and `after` right after
/// `wait $pid; rc=$?` (before the clocks and the caps are put back).
pub fn around_wait(script: &str, seconds: u32, text: &str, after: &str) -> Result<String> {
    let wait = format!("t=0\nwhile [ $t -lt {seconds} ] && [ ! -e $D/stop.request ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done\nhow=itself\n");
    let end = "wait $pid; rc=$?\n";
    if script.matches(wait.as_str()).count() != 1 || script.matches(end).count() != 1 {
        bail!("take23: the boot script has not its one wait for the VM's end and its one `wait $pid`");
    }
    Ok(script.replace(wait.as_str(), &format!("{text}{wait}")).replace(end, &format!("{end}{after}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cand(ram: u32, arena: &str) -> Candidate {
        Candidate { ram, need: (u64::from(ram) - 32 + 1) / 2, arena: arena.into(), file: format!("kernel-{ram}.img"), sha: format!("{ram:064x}") }
    }

    #[test]
    fn the_choice_lists_the_largest_first_and_ends_the_session_when_none_fits() {
        let b = pick_block(&[cand(7168, "23f800000"), cand(6144, "1ff800000"), cand(4608, "19f800000")], 1024);
        assert!(b.contains("KEEP=1024\n"));
        assert!(b.contains("done <<'ONEIOS_PICK'\n7168 3568 23f800000 kernel-7168.img "));
        let at = |s: &str| b.find(s).unwrap();
        assert!(at("\n7168 3568 ") < at("\n6144 3056 ") && at("\n6144 3056 ") < at("\n4608 2288 19f800000 kernel-4608.img "));
        assert!(b.contains("\nONEIOS_PICK\nif [ -z \"$RAM\" ]; then\n"));
        assert!(b.contains("end \"refused memory: no Image of the list fits with $KEEP MiB left to Android\"; exit 1"));
        // nothing of the choice writes anywhere but the pool's two files
        for l in b.lines() {
            if let Some((_, to)) = l.split_once(" > ") {
                assert!(to.starts_with("$P/pool_want") || to.starts_with("$P/acquire"), "{l}");
            }
        }
        assert!(!b.contains("__"));
        // the pool is never asked to shrink: its want is only ever raised to a candidate's pages
        assert_eq!(b.matches("> $P/pool_want").count(), 1);
        assert!(b.contains("[ \"$(pool_n pool_want)\" -ge $cneed ] || echo $cneed > $P/pool_want"));
    }

    #[test]
    fn the_choice_goes_into_the_script_at_its_four_places() {
        let script = "a\nneed=2288\navail=x\n$C run --hugepages --mem size=4608 --cpus num-cores=6 \\\n  --block x $D/kernel.img </dev/null >$D/crosvm.log 2>&1 &\n\
                      if grep -q -F drm2kgsl_host@19f800000 $D/fdt.dtb; then echo \"at 0x19f800000\"; else echo \"no drm2kgsl_host@19f800000\"; fi\n";
        let main = cand(4608, "19f800000");
        let s = apply_pick(script, &main, "PICK\n").unwrap();
        assert!(s.contains("a\nPICK\nneed=$NEED\navail=x\n"));
        assert!(s.contains("--mem size=$RAM --cpus") && s.contains("--block x $KIMG </dev/null >$D/crosvm.log"));
        assert!(s.contains("drm2kgsl_host@$ARENA $D/fdt.dtb") && s.contains("at 0x$ARENA") && !s.contains("19f800000"));
        // a script without one of the places is refused, not half changed
        assert!(apply_pick(&script.replace("need=2288\n", ""), &main, "PICK\n").is_err());
        assert!(apply_pick(&script.replace("--mem size=4608 ", "--mem size=4609 "), &main, "PICK\n").is_err());
        assert!(apply_pick(&script.replace("$D/kernel.img </dev/null", "$D/k.img </dev/null"), &main, "PICK\n").is_err());
        assert!(apply_pick(&script.replace("19f800000", "19f000000"), &main, "PICK\n").is_err());
        // no GPU: three places
        let plain = cand(4608, "");
        assert!(apply_pick(script, &plain, "PICK\n").unwrap().contains("drm2kgsl_host@19f800000"));
    }

    #[test]
    fn the_orchestrator_takes_its_numbers_and_refuses_wrong_ones() {
        assert_eq!(Orch::parse(&[]).unwrap(), Orch::DEFAULT);
        let o = Orch::parse(&["gpu-floor=734".into(), "good=95".into(), "clamp-floor=640".into()]).unwrap();
        assert_eq!((o.gpu_floor, o.good, o.clamp_floor), (734, 95, 640));
        for bad in ["gpu-floor=100", "gpu-floor=525", "gpu-up=10", "gpu-down=60", "good=40", "bad=95", "hot=200", "what=1", "gpu-floor", "gpu-floor=x", "clamp-step=0"] {
            assert!(Orch::parse(&[bad.to_string()]).is_err(), "{bad}");
        }
        let s = orch_start(&Orch::DEFAULT);
        assert!(!s.contains("__") && s.contains("up at 55 % busy") && s.contains("clamp 1024 down to 512 by 128"));
        assert!(s.contains("orch_end() {") && s.contains(") > $D/orch.txt 2>&1 &\n  orchp=$!\n"));
        // it writes two files only: Samsung's cap and the renderer's clamp file
        for l in s.lines() {
            for part in l.split(';') {
                if let Some((_, to)) = part.split_once(" > ") {
                    let to = to.trim();
                    assert!(to.starts_with("$OGM") || to.starts_with("/data/local/tmp/drm2kgsl-vcpu-uclamp") || to.starts_with("$D/orch.txt"), "{l}");
                }
            }
        }
    }

    #[test]
    fn the_additions_go_around_the_wait_for_the_end() {
        let script = "x\nt=0\nwhile [ $t -lt 3600 ] && [ ! -e $D/stop.request ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done\nhow=itself\ny\nwait $pid; rc=$?\nthp_restore\n";
        let s = around_wait(script, 3600, "START\n", "orch_end\n").unwrap();
        assert!(s.contains("x\nSTART\nt=0\nwhile [ $t -lt 3600 ]") && s.contains("wait $pid; rc=$?\norch_end\nthp_restore\n"));
        assert!(around_wait(script, 900, "START\n", "orch_end\n").is_err());
    }
}
