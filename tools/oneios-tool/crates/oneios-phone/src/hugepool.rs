//! `oneios-tool phone hugepool status | load | grow | unload`: Droid-VM's gh_hugepage_reserve (GPL-2.0, built by
//! scripts/hugepage/build.sh) on the phone, so that a VM's RAM comes in 2 MiB pages (docs/native.md "A VM under
//! Gunyah": in 4 KiB pages Gunyah's own 18 MiB stops a VM near 0.5 GB, and after some uptime almost every 2 MiB
//! window of Android's RAM holds a page nothing can move). The module keeps a pool of 2 MiB pages and hands them to
//! a VM's huge page allocations (crosvm `--hugepages`, `shmem_enabled` advise: `vm-probe --thp`).
//! - `status`: the module's state, read only (`/sys/module/gh_hugepage_reserve/parameters`, named files).
//! - `load --want N`: the module's own ABI preflight on the PC (its `kapi_check` on the phone's BTF, for the phone's
//!   kernel version), the .ko pushed and checked, a `sync`, then `insmod ... pool_want=N disable_kapi=...` beside
//!   One UI: the pool fills from what is free now. Its CMA side stays off.
//! - `grow --want N`: One UI's framework stopped as oneios_switch.sh stops it (zygote held stopped, Samsung's software
//!   watchdog fed), the file cache dropped, the pool's want raised and its acquire pressed while the RAM One UI held
//!   is free, then One UI started again as oneios_return.sh starts it (zygote's own restart list by hand). A script
//!   does it on the phone, detached: adb may go while system_server is gone. A failsafe starts One UI LIMIT seconds
//!   after the stop whatever the cycle is doing; a One UI that does not come back makes the phone restart, the
//!   return's own last resort. The tool follows the script's log over adb, reconnecting.
//! - `unload`: rmmod, when no VM holds pool pages and no acquire runs.
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::copy;
use std::path::Path;
use std::process::Command;
use std::time::{Duration, Instant};

/// The module's folder on the phone, its parameters, and its name.
pub const DIR: &str = "/data/local/tmp/oneios-hugepool";
/// take23: the least `hugepool load --system-reserve-mb` takes. Not what Android needs (vm-boot's --host-keep-mib asks that
/// at each boot, after One UI's stop): only the bound of the module's table.
pub const RESERVE_MIN_MB: u32 = 2048;
/// take23: the most pool pages a want may ask for (4096 = 8 GiB; 3072 before).
pub const WANT_MAX: u32 = 4096;
pub const PARAMS: &str = "/sys/module/gh_hugepage_reserve/parameters";
pub const KO: &str = "/data/local/tmp/oneios-hugepool/gh_hugepage_reserve.ko";
/// take18's alternative module on the PC (`hugepool load --samfor12`; scripts/hugepage/build.sh WORK samfor12): samfor12's
/// temp-root fork 361d499 of our pin 00c8a03. It goes to the phone as KO, as ours does.
pub const KO_SAMFOR12: &str = "/mnt/tkg-build/oneios/hugepage/out/android15-6.6-samfor12.ko";

/// `status`'s read: each named file after a `=== NAME` line.
pub fn status_script() -> String {
    let mut s = format!("if [ ! -d {PARAMS} ]; then echo '=== loaded'; echo no; exit 0; fi\necho '=== loaded'; echo yes\n");
    for f in ["refill_stat", "pool_size_max", "system_reserve_mb_default", "vm_owners"] {
        s += &format!("echo '=== {f}'; cat {PARAMS}/{f} 2>&1\n");
    }
    s += "echo '=== shmem_enabled'; cat /sys/kernel/mm/transparent_hugepage/shmem_enabled\n";
    s += "echo '=== dmesg'; dmesg 2>/dev/null | grep 'gh_hugepage_reserve:' | tail -n 30\n";
    s
}

/// A value of `refill_stat` (`key=value` lines).
pub fn stat<'a>(refill_stat: &'a str, key: &str) -> Option<&'a str> {
    refill_stat.lines().find_map(|l| l.strip_prefix(key).and_then(|r| r.strip_prefix('=')))
}

/// What `kapi_check` says to pass: (disable_kapi, migrate_cma).
pub fn kapi_args(out: &str) -> (String, Option<String>) {
    let disable = out.lines().find_map(|l| l.strip_prefix("disable=")).unwrap_or("").trim().to_string();
    let cma = out.lines().find_map(|l| l.strip_prefix("migrate_cma=")).map(|v| v.trim().to_string());
    (disable, cma)
}

/// The kernel's version as kapi_check takes it ("6.6.127") from `uname -r` ("6.6.127-android15-8-...").
pub fn kernel_version(uname_r: &str) -> Option<String> {
    let v: String = uname_r.trim().chars().take_while(|c| c.is_ascii_digit() || *c == '.').collect();
    (v.split('.').filter(|p| !p.is_empty()).count() == 3).then_some(v)
}

/// The page block order from `/proc/pagetypeinfo` ("Page block order: 9").
pub fn pageblock_order(pagetypeinfo: &str) -> Option<u32> {
    pagetypeinfo.lines().find_map(|l| l.strip_prefix("Page block order:")).and_then(|v| v.trim().parse().ok())
}

/// The insmod line: the pool's want, the page block order, the ABI guard; the CMA reservoir off
/// (`migrate_cma_val=-1`, `pool_want_with_cma=0`). The page block order is not optional: without it the module takes
/// -1 for its block order and computes its block size S as `1u << (-1 - 9)` (crash 1, 2026-11-23: a NULL slot in
/// `gh_try_complete_block`, 4 Mi siblings scanned into a hole GB). Its own load.sh always passes it.
pub fn insmod_line(want: u32, pb_order: u32, disable_kapi: &str) -> String {
    insmod_line_with(want, pb_order, disable_kapi, None)
}

/// take23: the most pool pages the module's table can hold is set at insmod and never after (its system_reserve_mb, 0400):
/// (RAM - min(RAM / 2, system_reserve_mb)) / 2 MiB, and the module's own 6144 MiB leave the S25 Ultra's 11 GiB a table of
/// about 2500 pages (5 GiB): the "2560 pages" no session got past. `reserve_mb` (hugepool load --system-reserve-mb) is that
/// parameter; None: the module's own. It bounds what the pool MAY take, not what it takes (pool_want and the acquire do).
pub fn insmod_line_with(want: u32, pb_order: u32, disable_kapi: &str, reserve_mb: Option<u32>) -> String {
    let mut s = format!("insmod {KO} pool_want={want} pageblock_order_val={pb_order} migrate_cma_val=-1 pool_want_with_cma=0");
    if let Some(mb) = reserve_mb {
        s += &format!(" system_reserve_mb={mb}");
    }
    if !disable_kapi.is_empty() {
        s += &format!(" disable_kapi={disable_kapi}");
    }
    s
}

fn print_parts(out: &str) {
    for (name, text) in crate::hw::split(out) {
        println!("--- {name}");
        print!("{text}");
    }
}

/// `phone hugepool status`.
pub fn status(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    print_parts(&adb.root_ok(&status_script(), "the module's status")?);
    Ok(())
}

/// `phone hugepool load`.
pub fn load(adb: &Adb, ko: &str, kapi_check: &str, want: u32, out: &str) -> Result<()> {
    load_with(adb, ko, kapi_check, want, out, None)
}

/// `phone hugepool load --system-reserve-mb N` (insmod_line_with).
pub fn load_with(adb: &Adb, ko: &str, kapi_check: &str, want: u32, out: &str, reserve_mb: Option<u32>) -> Result<()> {
    load_full(adb, ko, kapi_check, want, out, reserve_mb, None)
}

/// take24: the lines around the insmod. `leave_mib` (hugepool load --leave-mib): the want is lowered on the phone to
/// (MemAvailable - leave) / 2 pages, 64 at least. Always: Android's memory and its free blocks by order before and after,
/// with the uptime (the pool's pages come from blocks of 2 MiB that are free at that moment: how many there are is the
/// whole question).
pub fn load_lines(line: &str, want: u32, leave_mib: Option<u32>) -> String {
    let mem = "mem() { echo \"load: $1: uptime $(cut -d' ' -f1 /proc/uptime) s; $(grep -E '^(MemFree|MemAvailable|CmaFree):' /proc/meminfo | tr -s ' ' | tr '\\n' ' '); free blocks by order: $(tr -s ' ' < /proc/buddyinfo | tr '\\n' ';')\"; }\n";
    let mut s = String::from(mem);
    s += "mem before\n";
    let line = match leave_mib {
        Some(leave) => {
            s += &format!(
                "ma=$(sed -n 's/^MemAvailable: *\\([0-9]*\\).*/\\1/p' /proc/meminfo); ma=$((${{ma:-0}} / 1024))\n\
                 W={want}; r=$(( (ma - {leave}) / 2 )); [ $r -ge 64 ] || r=64; [ $W -le $r ] || W=$r\n\
                 echo \"load: Android has $ma MiB available: the want is $W pages of the {want} asked ({leave} MiB left to Android while it starts)\"\n"
            );
            line.replace(&format!("pool_want={want} "), "pool_want=$W ")
        }
        None => line.to_string(),
    };
    s += &format!("sync\nt0=$(cut -d' ' -f1 /proc/uptime)\n{line}; rc=$?\nt1=$(cut -d' ' -f1 /proc/uptime)\necho \"insmod exit $rc ($t0 to $t1)\"\nmem after\n");
    s
}

/// `phone hugepool load` with every option (load_with, and take24's --leave-mib: load_lines).
pub fn load_full(adb: &Adb, ko: &str, kapi_check: &str, want: u32, out: &str, reserve_mb: Option<u32>, leave_mib: Option<u32>) -> Result<()> {
    if leave_mib.is_some_and(|mb| !(1024..=8192).contains(&mb)) {
        bail!("--leave-mib: 1024 to 8192 MiB");
    }
    if reserve_mb.is_some_and(|mb| !(RESERVE_MIN_MB..=6144).contains(&mb)) {
        bail!("--system-reserve-mb: {RESERVE_MIN_MB} to 6144 MiB (the module's own is 6144; what Android keeps is asked again at each vm-boot, --host-keep-mib)");
    }
    let data = fs::read(ko).with_context(|| format!("cannot read {ko} (scripts/hugepage/build.sh makes it)"))?;
    let sha = format!("{:x}", Sha256::digest(&data));
    if want == 0 || want > WANT_MAX {
        bail!("--want {want}: 1 to {WANT_MAX} pages of 2 MiB");
    }
    adb.need_root()?;
    adb.refuse_in_session()?;
    let r = adb.root_ok(
        &format!("[ -d {PARAMS} ] && echo loaded || echo absent; uname -r; grep '^Page block order:' /proc/pagetypeinfo\n"),
        "the module check",
    )?;
    if r.contains("loaded") {
        bail!("gh_hugepage_reserve is loaded already: `phone hugepool status`, or `grow`");
    }
    let version = kernel_version(r.lines().nth(1).unwrap_or("")).with_context(|| format!("no kernel version in \"{}\"", r.trim()))?;
    // the module's ABI preflight, on the PC, on the phone's own BTF
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let btf = Path::new(out).join("btf.vmlinux");
    let mut child = adb.exec_out("su -c 'cat /sys/kernel/btf/vmlinux'")?;
    let mut file = fs::File::create(&btf)?;
    copy(child.stdout.as_mut().context("no output from adb")?, &mut file)?;
    child.wait()?;
    let check = Command::new(kapi_check).arg(&btf).arg(&version).output().with_context(|| format!("cannot run {kapi_check}"))?;
    let said = format!("{}{}", String::from_utf8_lossy(&check.stdout), String::from_utf8_lossy(&check.stderr));
    fs::write(Path::new(out).join("kapi_check.txt"), &said)?;
    if !check.status.success() || !said.contains("disable=") {
        bail!("kapi_check failed ({}): {}", check.status, said.trim());
    }
    let (disable, cma) = kapi_args(&said);
    println!("kapi_check (kernel {version}): disable_kapi '{disable}', MIGRATE_CMA {} (the CMA side stays off)", cma.unwrap_or("?".into()));
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the module's folder")?;
    adb.push(ko, KO)?;
    // the module's block order: 9 (2 MiB) is the only one this tool knows the module to handle
    let pb = pageblock_order(&r).with_context(|| format!("no page block order in /proc/pagetypeinfo (\"{}\")", r.trim()))?;
    if pb != 9 {
        bail!("the phone's page block order is {pb}, not 9: not loading the module");
    }
    let line = insmod_line_with(want, pb, &disable, reserve_mb);
    println!("load: {} bytes, sha256 {sha}; {line}", data.len());
    let script = format!(
        "got=$(sha256sum {KO} | cut -d' ' -f1); [ \"$got\" = \"{sha}\" ] || {{ echo \"sha256 $got, not {sha}\"; exit 1; }}\n{}",
        load_lines(&line, want, leave_mib)
    );
    let r = adb.root(&script)?;
    print!("{}{}", r.out, r.err);
    fs::write(Path::new(out).join("load.txt"), format!("{line}\n{}{}", r.out, r.err))?;
    if !r.out.contains("insmod exit 0") {
        bail!("the module did not load");
    }
    status(adb)
}

/// The cycle's helpers, for another script that stops and starts One UI (vm-boot --takeover): `now` .. `start_ui` and the
/// word modes (`--stop-loop`, `--feed`, `--failsafe`), as CYCLE has them. They use $D, $SELF, $CTL, $P and $LIMIT.
pub fn ui_functions() -> &'static str {
    let begin = CYCLE.find("now() {").expect("the cycle's helpers");
    let end = CYCLE.find("\nesac\n").expect("the cycle's word modes") + "\nesac\n".len();
    &CYCLE[begin..end]
}

/// The cycle's stop of One UI (oneios_switch.sh [3/5]): the watchdog nodes system_server holds, the way back armed (the
/// failsafe and a trap), the framework stopped and kept so, the watchdogs closed and fed. Expects $CTL made and the
/// services' base states saved.
pub fn ui_stop() -> &'static str {
    let begin = CYCLE.find("# the watchdog nodes system_server keeps open").expect("the cycle's stop");
    let line = CYCLE.find("say \"stopped: zygote").expect("the cycle's stopped line");
    let end = line + CYCLE[line..].find('\n').expect("a line") + 1;
    &CYCLE[begin..end]
}

/// `grow`'s script on the phone. WANT pages of 2 MiB, acquire MODE (2 sweep + reclaim, 3 sweep + per-window evict),
/// LIMIT seconds of One UI down before the failsafe starts it.
pub fn cycle_script(want: u32, mode: u32, limit: u32) -> String {
    CYCLE.replace("__WANT__", &want.to_string()).replace("__MODE__", &mode.to_string()).replace("__LIMIT__", &limit.to_string())
}

/// The cycle (POSIX sh for Android's mksh). Its helpers are this file run again with a word, as oneios_switch.sh's.
pub const CYCLE: &str = r#"#!/system/bin/sh
# oneios-tool phone hugepool grow, on the phone, detached. See crates/oneios-phone/src/hugepool.rs.
D=/data/local/tmp/oneios-hugepool
SELF=$D/cycle.sh
CTL=$D/ctl
P=/sys/module/gh_hugepage_reserve/parameters
WANT=__WANT__
MODE=__MODE__
LIMIT=__LIMIT__
exec >>$D/cycle.log 2>&1 </dev/null
now() { cut -d. -f1 /proc/uptime; }
say() { echo "$(date +%T) $*"; }
stat_line() { grep -E '^(state|pool_avail|pool_total|pool_want|acquire_active|acquire_stop_reason|active_vms)=' $P/refill_stat 2>/dev/null | tr '\n' ' '; }
mem_line() { grep -E '^(MemFree|MemAvailable):' /proc/meminfo | tr -s ' ' | tr '\n' ' '; }
bg() { if command -v setsid >/dev/null 2>&1; then setsid "$@" </dev/null >>$D/cycle.log 2>&1 & else "$@" </dev/null >>$D/cycle.log 2>&1 & fi; }
# Is this pid still our helper (a pid file outlives its process; ids are reused)? cat, then tr: oneios_return.sh's
# is_ours (toybox's tr reads again for ever on a failed read).
is_ours() { case "$(cat /proc/$1/cmdline 2>/dev/null | tr '\0' ' ')" in *"$2"*) return 0 ;; esac; return 1; }

# The last resort, as oneios_return.sh's: a phone without One UI restarts into it.
last_resort() {
    say "LAST RESORT: $* - restarting the phone into One UI"
    echo "failsafe $*" > $D/result
    timeout 10 sync
    setprop sys.powerctl reboot,oneios-hugepool-failsafe
    sleep 20
    /system/bin/reboot oneios-hugepool-failsafe </dev/null >/dev/null 2>&1 &
}

# One UI started again, as oneios_return.sh starts it after a stop: once (a lock), from the main flow, its trap or the
# failsafe, whichever comes first.
start_ui() {
    mkdir $CTL/start.lock 2>/dev/null || return 0
    if [ ! -e $CTL/stopped ]; then
        say "start: One UI was not stopped ($1)"
        true > $CTL/feeder.off; true > $CTL/stop-loop.off
        [ -e $D/result ] || echo "refused $1" > $D/result
        return 0
    fi
    say "start: One UI ($1)"
    true > $CTL/stop-loop.off
    # the stop loop gone before anything starts (its last ctl.stop could stop the new zygote)
    sl=$(cat $CTL/stop-loop.pid 2>/dev/null)
    n=0
    while [ -n "$sl" ] && [ $n -lt 10 ] && is_ours "$sl" "cycle.sh --stop-loop"; do sleep 0.2; n=$((n + 1)); done
    [ -n "$sl" ] && is_ours "$sl" "cycle.sh --stop-loop" && kill -9 "$sl" 2>/dev/null
    # zygote's onrestart list by hand: a start after a stop skips it
    timeout 10 vdc volume abort_fuse >/dev/null 2>&1
    setprop service.bootanim.exit 0
    setprop ctl.start bootanim
    setprop ctl.stop audioserver
    setprop ctl.start audioserver
    setprop ctl.restart cameraserver
    for svc in media mediaextractor mediametrics; do setprop ctl.start $svc; done
    setprop ctl.restart wificond
    for svc in perfmond media.tuner; do [ "$(cat $CTL/base.$svc 2>/dev/null)" = running ] && setprop ctl.restart $svc; done
    [ "$(cat $CTL/base.zygote_next 2>/dev/null)" = running ] && setprop ctl.restart zygote_next
    setprop ctl.restart netd
    n=0
    while [ $n -lt 20 ] && [ "$(getprop init.svc.zygote)" != running ]; do sleep 0.5; n=$((n + 1)); done
    [ "$(getprop init.svc.zygote)" = running ] || setprop ctl.start zygote
    if [ "$(cat $CTL/base.zygote_secondary 2>/dev/null)" = running ] && [ "$(getprop init.svc.zygote_secondary)" != running ]; then
        setprop ctl.start zygote_secondary
    fi
    t0=$(now); ss=""
    while [ $(($(now) - t0)) -lt 90 ]; do
        ss=$(pidof system_server); case "$ss" in *" "*) ss="" ;; esac
        [ -n "$ss" ] && break
        [ $(($(now) - t0)) -ge 45 ] && [ "$(getprop init.svc.zygote)" != running ] && setprop ctl.start zygote
        sleep 1
    done
    [ -n "$ss" ] || { last_resort "One UI's system_server did not start in 90 s"; return 1; }
    # system_server feeds Samsung's software watchdog again from here
    true > $CTL/feeder.off
    say "start: system_server $ss"
    t0=$(now); up=0
    while :; do
        sleep 2; t=$(($(now) - t0))
        [ "$(cat /proc/$ss/comm 2>/dev/null)" = system_server ] || { last_resort "system_server $ss died after ${t}s"; return 1; }
        if [ $up = 0 ]; then
            case "$(timeout 5 am get-started-user-state 0 2>&1)" in
                *RUNNING_UNLOCKED*) up=$t; say "start: user 0 unlocked after ${t}s" ;;
            esac
        fi
        [ $up -gt 0 ] && [ $t -ge $((up + 20)) ] && break
        [ $up = 0 ] && [ $t -ge 180 ] && { say "start: user 0 not unlocked within 180 s (system_server lives)"; break; }
    done
    say "start: One UI up; memory $(mem_line)"
    echo "done $1: $(stat_line)" > $D/result
}

case "${1:-}" in
--stop-loop)
    # One UI's zygote must stay stopped: other services restart it (onrestart). oneios_switch.sh's loop.
    while [ ! -e $CTL/stop-loop.off ]; do
        [ "$(getprop init.svc.zygote)" = running ] && setprop ctl.stop zygote
        [ "$(getprop init.svc.zygote_secondary)" = running ] && setprop ctl.stop zygote_secondary
        [ "$(getprop init.svc.bootanim)" = running ] && setprop ctl.stop bootanim
        sleep 0.2
    done
    exit 0 ;;
--feed)
    # Samsung's software watchdog (100 s), which system_server kicked: its magic close, or a kick. oneios_switch.sh's.
    while [ ! -e $CTL/feeder.off ]; do
        for wd in /dev/watchdog /dev/softdog $(cat $CTL/softdog_nodes 2>/dev/null); do
            [ -c "$wd" ] && printf V > "$wd" 2>/dev/null
        done
        sleep 2
    done
    exit 0 ;;
--failsafe)
    # One UI started LIMIT s after the stop, whatever the main flow does (an acquire that does not end, a stuck write)
    t0=$(now)
    while [ ! -e $CTL/start.lock ]; do
        if [ -e $CTL/stopped ] && [ $(($(now) - $(cat $CTL/stopped))) -ge $LIMIT ]; then
            say "failsafe: One UI down for $LIMIT s"
            start_ui "the failsafe"
            break
        fi
        [ $(($(now) - t0)) -ge $((LIMIT + 120)) ] && break
        sleep 1
    done
    exit 0 ;;
esac

# The cycle. Preflight: nothing is changed before these hold.
rm -rf $CTL; mkdir -p $CTL
say "hugepool grow: want $WANT pages of 2 MiB, acquire mode $MODE, One UI down for $LIMIT s at most"
refuse() { say "REFUSED: $*"; echo "refused $*" > $D/result; exit 1; }
[ -d $P ] || refuse "gh_hugepage_reserve is not loaded"
[ "$(getprop init.svc.zygote)" = running ] && pidof system_server >/dev/null 2>&1 || refuse "One UI's framework is not running"
grep -q '^active_vms=0$' $P/refill_stat || refuse "a VM holds pool pages"
grep -q '^acquire_active=0$' $P/refill_stat || refuse "an acquire run is in flight"
x=$(cat /data/local/tmp/oneios/session/xnuhost.pid 2>/dev/null)
[ -n "$x" ] && cat /proc/$x/cmdline 2>/dev/null | tr '\0' ' ' | grep -q xnuhost && refuse "an iOS session is open"
for svc in zygote_secondary perfmond media.tuner zygote_next; do getprop init.svc.$svc > $CTL/base.$svc; done
say "before: $(stat_line); memory $(mem_line)"
say "before: $(cat /proc/buddyinfo)"
# the watchdog nodes system_server keeps open (oneios_switch.sh [3/5])
for p in $(pidof system_server); do
    for fd in /proc/$p/fd/*; do
        t=$(readlink "$fd" 2>/dev/null)
        case "$t" in /dev/watchdog*|/dev/softdog*) echo "$t" ;; esac
    done
done 2>/dev/null | sort -u > $CTL/softdog_nodes
say "softdog nodes: $(cat $CTL/softdog_nodes | tr '\n' ' ')"

# The way back, armed before the stop.
bg /system/bin/sh $SELF --failsafe
trap 'start_ui "the cycle ended"' EXIT HUP INT TERM

# One UI's framework stopped (oneios_switch.sh [3/5]).
now > $CTL/stopped
setprop ctl.stop zygote
[ -n "$(getprop init.svc.zygote_secondary)" ] && setprop ctl.stop zygote_secondary
n=0
while [ $n -lt 50 ] && pidof system_server >/dev/null 2>&1; do sleep 0.2; n=$((n + 1)); done
kill -9 $(pidof system_server) 2>/dev/null
kill -9 $(pidof zygote64) $(pidof zygote) 2>/dev/null
bg /system/bin/sh $SELF --stop-loop
echo $! > $CTL/stop-loop.pid
for wd in /dev/watchdog /dev/softdog $(cat $CTL/softdog_nodes); do
    [ -c "$wd" ] || continue
    printf V > "$wd" 2>/dev/null && say "softdog: $wd closed with V"
done
bg /system/bin/sh $SELF --feed
say "stopped: zygote $(getprop init.svc.zygote), system_server $(pidof system_server 2>/dev/null || echo gone); memory $(mem_line)"

# The RAM One UI held, free; then the pool grown by the module's own acquire.
sync
echo 3 > /proc/sys/vm/drop_caches
say "freed: memory $(mem_line)"
say "freed: $(cat /proc/buddyinfo)"
echo $WANT > $P/pool_want || say "pool_want $WANT refused"
echo $MODE > $P/acquire || say "acquire $MODE refused"
# the run starts in the module's worker: its acquire_active may still read 0 for a moment
sleep 3
t0=$(now); last=0
while :; do
    s=$(stat_line)
    case "$s" in *acquire_active=0*) break ;; esac
    t=$(($(now) - t0))
    if [ $t -ge $((LIMIT - 40)) ]; then say "acquire: still running after ${t}s: cancelled"; echo 0 > $P/acquire; sleep 2; break; fi
    [ $t -ge $((last + 5)) ] && { last=$t; say "acquire ${t}s: $s"; }
    sleep 1
done
say "pool: $(stat_line)"
start_ui "the pool is grown"
"#;

/// `phone hugepool grow`.
pub fn grow(adb: &Adb, want: u32, mode: u32, limit: u32, out: &str) -> Result<()> {
    if want == 0 || want > WANT_MAX || !(2..=3).contains(&mode) || !(90..=600).contains(&limit) {
        bail!("--want 1..{WANT_MAX}, --mode 2 or 3, --limit 90..600");
    }
    adb.need_root()?;
    adb.refuse_in_session()?;
    let st = adb.root_ok(&format!("cat {PARAMS}/refill_stat 2>/dev/null || echo absent\n"), "the module's state")?;
    if st.contains("absent") {
        bail!("gh_hugepage_reserve is not loaded: `phone hugepool load` first");
    }
    if stat(&st, "active_vms") != Some("0") || stat(&st, "acquire_active") != Some("0") {
        bail!("the module is busy (active_vms {:?}, acquire_active {:?})", stat(&st, "active_vms"), stat(&st, "acquire_active"));
    }
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let local = Path::new(out).join("cycle.sh");
    fs::write(&local, cycle_script(want, mode, limit))?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR} && rm -f {DIR}/cycle.log {DIR}/result\n"), "the cycle's folder")?;
    adb.push(local.to_str().context("path")?, &format!("{DIR}/cycle.sh"))?;
    println!("grow: One UI's framework stops for at most {limit} s while the pool grows to {want} pages of 2 MiB");
    // started is started: the adb shell that starts it may end with 255 when One UI's framework goes (grow 1,
    // 2026-11-23: the cycle ran to its end while the tool had given up on that status)
    let r = adb.root(&crate::adb::detached_start(&format!("{DIR}/cycle.sh")))?;
    if !r.out.contains("started") {
        bail!("the cycle did not start (status {}): {}", r.status, r.err.trim());
    }
    if !r.ok() {
        println!("(the start's adb shell ended with status {}: adb went with One UI's framework; following the log)", r.status);
    }
    // its log, over adb, which may go and come back while system_server is gone
    let t0 = Instant::now();
    let mut printed = 0usize;
    let mut gone = false;
    let result = loop {
        std::thread::sleep(Duration::from_secs(3));
        if t0.elapsed() > Duration::from_secs(limit as u64 + 600) {
            bail!("no end of the cycle after {} s: read {DIR}/cycle.log", t0.elapsed().as_secs());
        }
        let r = match adb.root(&format!("cat {DIR}/cycle.log 2>/dev/null; echo '=== result'; cat {DIR}/result 2>/dev/null\n")) {
            Ok(r) if r.ok() => r,
            _ => {
                if !gone {
                    println!("(adb: the phone is not reachable now; waiting)");
                    gone = true;
                }
                continue;
            }
        };
        gone = false;
        let (log, res) = r.out.split_once("=== result\n").unwrap_or((r.out.as_str(), ""));
        let lines: Vec<&str> = log.lines().collect();
        for l in &lines[printed.min(lines.len())..] {
            println!("{l}");
        }
        printed = lines.len();
        if !res.trim().is_empty() {
            fs::write(Path::new(out).join("cycle.log"), log)?;
            break res.trim().to_string();
        }
    };
    println!("grow: {result}");
    if !result.starts_with("done") {
        bail!("the cycle did not end well: {result}");
    }
    status(adb)
}

/// `acquire`'s script: the want (when given), the module's acquire MODE, then refill_stat every second while
/// acquire_active=1, cancelled (acquire 0) after SECONDS.
pub fn acquire_script(want: Option<u32>, mode: u32, seconds: u32) -> String {
    let mut s = String::new();
    if let Some(w) = want {
        s += &format!("echo {w} > {PARAMS}/pool_want || echo 'pool_want {w} refused'\n");
    }
    s += &format!(
        "echo {mode} > {PARAMS}/acquire || echo 'acquire {mode} refused'\nsleep 3\nt=0\n\
         while grep -q '^acquire_active=1$' {PARAMS}/refill_stat && [ $t -lt {seconds} ]; do sleep 1; t=$((t + 1)); done\n\
         if grep -q '^acquire_active=1$' {PARAMS}/refill_stat; then echo 0 > {PARAMS}/acquire; echo \"cancelled after {seconds} s\"; sleep 2; fi\n\
         echo \"after $((t + 3)) s\"\n"
    );
    s
}

/// `phone hugepool acquire`: the module's own acquire beside One UI (nothing stopped).
pub fn acquire(adb: &Adb, want: Option<u32>, mode: u32, seconds: u32) -> Result<()> {
    if !(1..=3).contains(&mode) || want.is_some_and(|w| w == 0 || w > WANT_MAX) || !(5..=600).contains(&seconds) {
        bail!("--mode 1..3, --want 1..{WANT_MAX}, --seconds 5..600");
    }
    adb.need_root()?;
    adb.refuse_in_session()?;
    let st = adb.root_ok(&format!("cat {PARAMS}/refill_stat 2>/dev/null || echo absent\n"), "the module's state")?;
    if st.contains("absent") {
        bail!("gh_hugepage_reserve is not loaded: `phone hugepool load` first");
    }
    if stat(&st, "acquire_active") != Some("0") {
        bail!("an acquire runs already");
    }
    let r = adb.root(&acquire_script(want, mode, seconds))?;
    print!("{}{}", r.out, r.err);
    status(adb)
}

/// `phone hugepool unload`.
pub fn unload(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    let st = adb.root_ok(&format!("cat {PARAMS}/refill_stat 2>/dev/null || echo absent\n"), "the module's state")?;
    if st.contains("absent") {
        println!("unload: gh_hugepage_reserve is not loaded");
        return Ok(());
    }
    if stat(&st, "active_vms") != Some("0") || stat(&st, "acquire_active") != Some("0") {
        bail!("the module is busy (active_vms {:?}, acquire_active {:?})", stat(&st, "active_vms"), stat(&st, "acquire_active"));
    }
    let r = adb.root("rmmod gh_hugepage_reserve; echo \"rmmod exit $?\"\n")?;
    print!("{}{}", r.out, r.err);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_ui_helpers_are_the_cycles_own() {
        let f = ui_functions();
        assert!(f.starts_with("now() {") && f.ends_with("\nesac\n"));
        assert!(f.contains("start_ui() {") && f.contains("--stop-loop)") && f.contains("--feed)") && f.contains("--failsafe)"));
        let s = ui_stop();
        assert!(s.starts_with("# the watchdog nodes system_server keeps open"));
        assert!(s.contains("bg /system/bin/sh $SELF --failsafe") && s.contains("setprop ctl.stop zygote") && s.ends_with("echo gone); memory $(mem_line)\"\n"));
        // the cycle itself is unchanged by them
        assert!(cycle_script(64, 3, 180).contains(f) && cycle_script(64, 3, 180).contains(s));
    }

    #[test]
    fn kapi_args_and_the_kernel_version() {
        let out = "kapi_check: 1 symbol(s) flagged\nmigrate_cma=3\ndisable=kallsyms_lookup_name\n";
        assert_eq!(kapi_args(out), ("kallsyms_lookup_name".to_string(), Some("3".to_string())));
        assert_eq!(kapi_args("disable=\n").0, "");
        assert_eq!(kernel_version("6.6.127-android15-8-p87b1ce1-abogkiS938BXXSDDZJ1-4k\n").as_deref(), Some("6.6.127"));
        assert_eq!(kernel_version("garbage"), None);
        assert_eq!(
            insmod_line(64, 9, ""),
            format!("insmod {KO} pool_want=64 pageblock_order_val=9 migrate_cma_val=-1 pool_want_with_cma=0")
        );
        assert!(insmod_line(64, 9, "kallsyms_lookup_name").ends_with(" disable_kapi=kallsyms_lookup_name"));
        // take24: --leave-mib lowers the want on the phone; without it the insmod line is the one given
        let line = insmod_line_with(4080, 9, "", Some(3072));
        let plain = load_lines(&line, 4080, None);
        assert!(plain.contains(&format!("\n{line}; rc=$?\n")) && plain.starts_with("mem() {") && plain.ends_with("mem after\n"));
        let left = load_lines(&line, 4080, Some(2560));
        assert!(left.contains("W=4080; r=$(( (ma - 2560) / 2 )); [ $r -ge 64 ] || r=64; [ $W -le $r ] || W=$r\n"));
        assert!(left.contains(" pool_want=$W pageblock_order_val=9 ") && !left.contains("pool_want=4080"));
        assert_eq!(pageblock_order("absent\n6.6.127-x\nPage block order: 9\n"), Some(9));
        assert_eq!(pageblock_order("absent\n6.6.127-x\n"), None);
        assert_eq!(stat("state=idle\npool_avail=12\nactive_vms=0\n", "pool_avail"), Some("12"));
        assert_eq!(stat("pool_avail_cma_able=3\n", "pool_avail"), None, "a key, not a prefix of one");
    }

    #[test]
    fn the_cycle_checks_first_arms_the_way_back_and_starts_one_ui_once() {
        let s = cycle_script(1536, 3, 150);
        assert!(s.contains("WANT=1536") && s.contains("MODE=3") && s.contains("LIMIT=150") && !s.contains("__"));
        let main = s.find("# The cycle. Preflight").unwrap();
        let body = &s[main..];
        // the preflight refuses before anything is changed
        let first_change = body.find("now > $CTL/stopped").unwrap();
        for check in ["[ -d $P ] || refuse", "One UI's framework is not running", "a VM holds pool pages", "an iOS session is open"] {
            assert!(body.find(check).unwrap() < first_change, "{check}");
        }
        // the failsafe and the trap before the stop; the feeder right after it
        assert!(body.find("--failsafe").unwrap() < first_change);
        assert!(body.find("trap 'start_ui").unwrap() < first_change);
        let stop = body.find("setprop ctl.stop zygote").unwrap();
        assert!(first_change < stop && stop < body.find("bg /system/bin/sh $SELF --feed").unwrap());
        // One UI starts once: a lock
        assert!(s.contains("mkdir $CTL/start.lock 2>/dev/null || return 0"));
        // the feeder ends only once system_server is back
        let f = s.find("start_ui() {").unwrap();
        let start = &s[f..];
        assert!(start.find("ss=$(pidof system_server)").unwrap() < start.find("true > $CTL/feeder.off\n    say \"start: system_server").unwrap());
        // the writes: the pool's two parameters, the cache drop and the module's cancel; nothing under /sys/power
        assert!(!s.contains("/sys/power"));
        let writes: Vec<&str> = s.lines().map(str::trim).filter(|l| l.contains(" > /") || l.contains(" > $P")).collect();
        for w in &writes {
            assert!(
                w.contains("> $P/pool_want") || w.contains("> $P/acquire") || w.contains("> /proc/sys/vm/drop_caches")
                    || w.contains("> \"$wd\"") || w.contains("> $CTL/") || w.contains("> $D/result"),
                "{w}"
            );
        }
        // the phone restarts only when One UI does not come back
        assert_eq!(s.matches("last_resort \"").count(), 2);
        assert!(s.contains("[ -n \"$ss\" ] || { last_resort"));
        // the stop loop's pid is kept, and checked before it is signalled
        assert!(s.contains("echo $! > $CTL/stop-loop.pid") && s.contains("is_ours \"$sl\" \"cycle.sh --stop-loop\" && kill -9"));
    }

    #[test]
    fn the_cycle_parses_as_posix_sh() {
        let path = std::env::temp_dir().join(format!("oneios-hugepool-cycle-{}.sh", std::process::id()));
        fs::write(&path, cycle_script(64, 3, 180)).unwrap();
        for sh in [["bash", "--posix"], ["sh", "-n"]] {
            let mut c = Command::new(sh[0]);
            if sh[0] == "bash" {
                c.args(["--posix", "-n"]);
            } else {
                c.arg("-n");
            }
            if let Ok(o) = c.arg(&path).output() {
                assert!(o.status.success(), "{} -n: {}", sh[0], String::from_utf8_lossy(&o.stderr));
            }
        }
        fs::remove_file(&path).ok();
    }
}
