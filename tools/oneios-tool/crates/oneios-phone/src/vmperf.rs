//! `oneios-tool phone vm-boot --perf-log` and `--unpin-workers`: what the host knows of a frame's road that the guest
//! cannot see, into perf.txt beside the session's other files (docs/native.md "Frame pacing"), and the device workers'
//! CPUs. Both are parts of boot.sh, run in a process of their own once crosvm is up; nothing here is in a restore trap,
//! everything is best effort (a file or a tool that is not there is skipped). What is changed: the CPUs of crosvm's own
//! threads (they end with crosvm), and surfaceflinger's TimeStats with the log (enabled at the start, disabled at the end).
//!
//! **The log** (read only):
//! - `kgsl start|end NAME VALUE`: the GPU's limits as KGSL and Samsung's driver give them (`max_gpuclk`,
//!   `thermal_pwrlevel`, `max_pwrlevel`, `devfreq/max_freq`, `/sys/kernel/gpu/gpu_max_clock` ...): take12 to take14 never
//!   passed 832 MHz and nothing logged why. And devfreq's `trans_stat`, the time at each clock to the millisecond.
//! - `thread TID cpus LIST policy P rtprio R nice N last CPU ticks T sched RUN WAIT SLICES comm NAME`, every thread of
//!   crosvm 25 s after its start, then once a minute and when the stop is asked: where each may run (the device
//!   workers' one core), its class, its CPU time (clock ticks) and the scheduler's own account (ns on a CPU, ns runnable
//!   and waiting for one, time slices). `WAIT / SLICES` of `v_gpu` and the `kgsl-sync` threads is what a fence's return
//!   waits for a CPU: the one host stage of a frame nothing else times.
//! - `surfaceflinger:` its vsync model (`VSyncPredictor`: the panel's measured period, against the guest's 120.00075
//!   Hz) and the layer's lines 25 s in and at the end; its TimeStats (enabled and cleared at the start, dumped every
//!   5 minutes and at the end): `present2present` of the layer "OneiOS display (crosvm)" is what the panel showed.
//!   Three things here touch surfaceflinger: its TimeStats are enabled and cleared at the start (Android keeps them for its
//!   own statistics, which lose what they had gathered), they are disabled again after the last dump (`perf_sf restore`:
//!   whether they were on before is not asked, and a phone that had them on is left with them off until its next boot),
//!   and the whole dump is asked for twice (25 s in and after the VM), each a moment of its main thread.
//! - `logcat:` the lines of crosvm's display client and of drm2kgsl, which do not go to crosvm.log: `dropped N flip(s)`
//!   (a frame for which surfaceflinger had no free buffer), `KGSL_DIAG` (drm2kgsl's own counts: `sync-mode`,
//!   `fence_immediate`, `fence_queued`; the prebuilt libvirglrenderer already has them on: the log sets nothing for crosvm).
//!
//! **The workers' CPUs** (`--unpin-workers`). crosvm starts a virtio device's worker when the guest writes DRIVER_OK,
//! on the vCPU thread that handled the write, and a new thread keeps its creator's CPUs: with `--pin` every worker but
//! the GPU's (started before the vCPUs) is held to one vCPU's core, where it waits for that vCPU to leave the guest.
//! Droid-VM fixed it in their crosvm on 2026-11-01 (`base: a worker thread takes the process's CPUs`, 47cd593: "every
//! block, input, network, serial ... worker was pinned to vCPU 0's core (cpu 2 on the phone)"); the phone's prebuilt
//! is of 2026-10-14 (0f2db62). Here the same from outside: every thread of crosvm that is not a vCPU's and whose CPUs
//! are not the main thread's is given the main thread's (`taskset -p`), looked for every 5 s in the first minute (the
//! guest starts its devices as it boots) and every 30 s after. Each change is one line, with the CPUs it had: `sched:
//! v_input (22950) was on cpus 2: it has the process's (ff) now`. Without the flag the thread lines show the one core.

/// What of this module a session uses.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Perf {
    /// perf.txt (`--perf-log`)
    pub log: bool,
    /// The device workers on the process's CPUs (`--unpin-workers`)
    pub unpin: bool,
}

/// The functions: each one works on `$PR/$pid` (crosvm) and writes lines to its standard output.
pub const FUNCTIONS: &str = r#"# the performance log (vm-boot --perf-log, perf.txt) and the device workers' CPUs (--unpin-workers): best effort, read only
# but for taskset on crosvm's own threads and surfaceflinger's TimeStats (enabled at the log's start, disabled at its end)
PR=/proc
KG=/sys/class/kgsl/kgsl-3d0
SG=/sys/kernel/gpu
perf_stamp() { echo "$(date +%T) $*"; }
perf_thread() {
  pt=$1; pc=; { read -r pc < $pt/comm; } 2>/dev/null; [ -n "$pc" ] || return 0
  pl=-; { while read -r pk pv; do case $pk in Cpus_allowed_list:) pl=$pv; break ;; esac; done < $pt/status; } 2>/dev/null
  ps=; { read -r ps < $pt/stat; } 2>/dev/null; set -- ${ps##*) }
  pss=; { read -r pss < $pt/schedstat; } 2>/dev/null
  echo "thread ${pt##*/} cpus $pl policy ${39:--} rtprio ${38:--} nice ${17:--} last ${37:--} ticks $(( ${12:-0} + ${13:-0} )) sched ${pss:-- - -} comm $pc"
}
perf_threads() { perf_stamp "threads $1"; for pd in $PR/$pid/task/*; do [ -d "$pd" ] && perf_thread $pd; done; }
perf_refused=
perf_unpin() {
  um=; { while read -r pk pv; do case $pk in Cpus_allowed:) um=${pv##*,}; break ;; esac; done < $PR/$pid/status; } 2>/dev/null
  [ -n "$um" ] || return 0
  for pd in $PR/$pid/task/*; do
    pc=; { read -r pc < $pd/comm; } 2>/dev/null
    case "$pc" in ''|crosvm_vcpu*) continue ;; esac
    tm=; tl=; { while read -r pk pv; do case $pk in Cpus_allowed:) tm=${pv##*,} ;; Cpus_allowed_list:) tl=$pv; break ;; esac; done < $pd/status; } 2>/dev/null
    [ -n "$tm" ] && [ "$tm" != "$um" ] || continue
    case " $perf_refused " in *" ${pd##*/} "*) continue ;; esac
    if taskset -p $um ${pd##*/} >/dev/null 2>&1; then perf_stamp "sched: $pc (${pd##*/}) was on cpus $tl: it has the process's ($um) now"
    else perf_refused="$perf_refused ${pd##*/}"; perf_stamp "sched: $pc (${pd##*/}) is on cpus $tl and taskset refused: left there"; fi
  done
}
perf_kgsl() {
  for pf in gpu_model max_gpuclk gpuclk max_clock_mhz min_clock_mhz thermal_pwrlevel max_pwrlevel min_pwrlevel default_pwrlevel num_pwrlevels \
      idle_timer wake_timeout force_no_nap bus_split popp gpu_available_frequencies devfreq/governor devfreq/min_freq devfreq/max_freq \
      devfreq/polling_interval; do
    pv=; { read -r pv < $KG/$pf; } 2>/dev/null; [ -z "$pv" ] || echo "kgsl $1 $pf $pv"
  done
  for pf in gpu_max_clock gpu_min_clock gpu_governor gpu_tmu; do
    pv=; { read -r pv < $SG/$pf; } 2>/dev/null; [ -z "$pv" ] || echo "kgsl $1 samsung/$pf $pv"
  done
  if [ -r $KG/devfreq/trans_stat ]; then echo "kgsl $1 trans_stat:"; head -n 24 $KG/devfreq/trans_stat 2>/dev/null; fi
}
perf_sf() {
  command -v dumpsys >/dev/null 2>&1 || { [ "$1" != start ] || echo "surfaceflinger: no dumpsys here"; return 0; }
  case "$1" in
    start) perf_stamp "surfaceflinger: timestats $(dumpsys SurfaceFlinger --timestats -clear -enable 2>&1 | head -n 1)" ;;
    restore) dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1; perf_stamp "surfaceflinger: timestats disabled again" ;;
    look) perf_stamp "surfaceflinger: its vsync and the layer"
      dumpsys SurfaceFlinger 2>/dev/null | grep -E 'VSyncPredictor|VSyncReactor|mIdealPeriod|Idealized Period|slope|[Rr]efresh.[Rr]ate|OneiOS display' | head -n 60 ;;
    *) perf_stamp "surfaceflinger: timestats $1"; dumpsys SurfaceFlinger --timestats -dump -maxlayers 6 2>/dev/null | head -n 400 ;;
  esac
}
perf_logcat() {
  command -v logcat >/dev/null 2>&1 || return 0
  perf_stamp "logcat: crosvm's display client and drm2kgsl"
  # each kind by itself: many lines of one must not push the few of another out
  logcat -d 2>/dev/null | grep -E 'flip\(s\)|CROSVM_DISPLAY_|display surface ' | tail -n 40
  logcat -d 2>/dev/null | grep -F 'KGSL_DIAG' | tail -n 80
}
"#;

/// The part of boot.sh after crosvm started (`$pid`, `$D`): the functions, and the process that uses them until crosvm
/// is gone. Empty when neither is asked for.
pub fn start_script(p: &Perf) -> String {
    if !p.log && !p.unpin {
        return String::new();
    }
    let what = match (p.log, p.unpin) {
        (true, true) => "the log, and the device workers take the process's CPUs",
        (true, false) => "the log",
        _ => "the device workers take the process's CPUs",
    };
    let mut s = String::from(FUNCTIONS);
    s.push_str("perfp=\n(\n  trap - EXIT HUP INT TERM\n");
    s.push_str(&format!("  perf_stamp \"perf: crosvm $pid: {what}\"\n"));
    if p.log {
        s.push_str("  perf_kgsl start\n  perf_sf start\n");
    }
    s.push_str("  pn=0; pstop=\n  while kill -0 $pid 2>/dev/null; do\n    sleep 1; pn=$((pn + 1))\n");
    if p.unpin {
        // the guest starts its devices as it boots: often at first, then now and then (a device that is reset starts its worker again)
        s.push_str("    if { [ $pn -le 60 ] && [ $((pn % 5)) = 0 ]; } || [ $((pn % 30)) = 0 ]; then perf_unpin; fi\n");
    }
    if p.log {
        s.push_str(concat!(
            "    if [ $pn = 25 ]; then perf_threads \"25 s after the start\"; perf_sf look; fi\n",
            "    if [ $pn -gt 25 ] && [ $((pn % 60)) = 0 ]; then perf_threads \"at $pn s\"; fi\n",
            "    if [ $pn -gt 25 ] && [ $((pn % 300)) = 0 ]; then perf_sf \"at $pn s\"; fi\n",
            // the last look at the threads while they are still there: as the stop is asked
            "    if [ -z \"$pstop\" ] && [ -e $D/stop.request ]; then pstop=1; perf_threads \"as the stop was asked, at $pn s\"; fi\n",
        ));
    }
    s.push_str("  done\n");
    if p.log {
        s.push_str("  perf_kgsl end\n  perf_sf look\n  perf_sf end\n  perf_sf restore\n  perf_logcat\n");
    }
    s.push_str("  perf_stamp \"perf: the end\"\n) > $D/perf.txt 2>&1 &\nperfp=$!\n");
    s.push_str(&format!("echo \"perf: {what}: perf.txt\"\n"));
    s
}

/// The part of boot.sh before its last line, after everything is put back: the log's process writes its last part once
/// crosvm is gone, and is waited for (20 s at most) so that the PC reads a whole file.
pub fn end_script(p: &Perf) -> String {
    if !p.log && !p.unpin {
        return String::new();
    }
    concat!(
        "if [ -n \"$perfp\" ]; then\n",
        "  t=0; while [ $t -lt 20 ] && kill -0 $perfp 2>/dev/null; do sleep 1; t=$((t + 1)); done\n",
        "  kill $perfp 2>/dev/null\n",
        "  echo \"perf: $(grep -c '' $D/perf.txt 2>/dev/null) lines; device workers given the process's CPUs: $(grep -c \"it has the process's\" $D/perf.txt 2>/dev/null)\"\n",
        "fi\n",
    )
    .to_string()
}

/// The panel's vsync for the VM's refresh tick (`vm-boot --vsync-lock`, runtime/pace's pacectl; display/vsync_lock.h in the
/// kernel): the part of boot.sh once crosvm runs and its tree was accepted (`$pid`, `$pool`). `page_offset` is the frame
/// sequence's page in crosvm's guest memfd (the pool's place less the RAM's base, plus the Image's `fb ... SEQ`).
/// pacectl ends by itself when crosvm does; pacectl writes nothing until the guest has put its magic in the page (it waits as long as crosvm lives); a kernel without the code, or an Image without `oneios-vsync-lock=1` (default 0), ignores what it writes, and without pacectl (or
/// when it cannot map the page or reach surfaceflinger: pace.txt says so) the tick is the free-running one. The phase
/// can be changed while the VM runs: the number in vsync.phase (microseconds) is read at every turn.
pub fn vsync_start_script(page_offset: u64, phase_us: i32) -> String {
    format!(
        "pacep=\nif [ \"$pool\" != bad ] && kill -0 $pid 2>/dev/null; then\n  \
         if [ -x $D/pacectl ]; then\n    \
         $D/pacectl vsync --pid $pid --page-offset {page_offset} --phase-us {phase_us} --phase-file $D/vsync.phase </dev/null >>$D/pace.txt 2>&1 &\n    \
         pacep=$!\n    echo \"vsync: pacectl ($pacep) gives the VM the panel's vsync (phase {phase_us} us; $D/vsync.phase changes it): pace.txt\"\n  \
         else\n    echo \"vsync: no $D/pacectl: the VM's refresh tick runs free\"\n  fi\nfi\n"
    )
}

/// After the VM: pacectl has seen crosvm go; one still there is ended. Its last lines go to run.txt.
pub fn vsync_end_script() -> String {
    concat!(
        "if [ -n \"$pacep\" ]; then\n",
        "  t=0; while [ $t -lt 3 ] && kill -0 $pacep 2>/dev/null; do sleep 1; t=$((t + 1)); done\n",
        "  kill $pacep 2>/dev/null\n",
        "  echo \"vsync: $(grep -c 'on the panel' $D/pace.txt 2>/dev/null) reports; the last: $(grep '^pacectl: panel ' $D/pace.txt 2>/dev/null | tail -n 1)\"\n",
        "  echo \"vsync: $(tail -n 1 $D/pace.txt 2>/dev/null)\"\n",
        "fi\n",
    )
    .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::{Path, PathBuf};

    fn folder(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("oneios-vmperf-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// A thread of the stand-in crosvm (pid 4242): its files as the kernel writes them.
    fn thread(root: &Path, tid: u32, comm: &str, mask: &str, list: &str, policy: u32, utime: u32, stime: u32, sched: &str) {
        let dir = root.join(format!("proc/4242/task/{tid}"));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("comm"), format!("{comm}\n")).unwrap();
        fs::write(dir.join("status"), format!("Name:\t{comm}\nUmask:\t0077\nState:\tS (sleeping)\nCpus_allowed:\t{mask}\nCpus_allowed_list:\t{list}\nMems_allowed:\t1\n")).unwrap();
        // pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime priority nice
        // threads itreal start vsize rss rsslim 11 more, then exit_signal processor rt_priority policy
        fs::write(dir.join("stat"),
                  format!("{tid} ({comm}) S 1 4242 4242 0 -1 4194368 10 0 0 0 {utime} {stime} 0 0 20 0 45 0 100 1000 200 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 3 0 {policy} 0 0 0\n"))
            .unwrap();
        if !sched.is_empty() {
            fs::write(dir.join("schedstat"), format!("{sched}\n")).unwrap();
        }
    }

    /// The phone's picture with --pin and the prebuilt of 2026-10-14: the vCPUs on their cores, the device workers on vCPU 0's.
    fn phone(root: &Path) {
        fs::create_dir_all(root.join("proc/4242")).unwrap();
        fs::write(root.join("proc/4242/status"), "Name:\tcrosvm\nCpus_allowed:\tff\nCpus_allowed_list:\t0-7\n").unwrap();
        thread(root, 4242, "crosvm", "ff", "0-7", 0, 120, 30, "1500000000 20000000 900");
        thread(root, 4250, "crosvm_vcpu0", "04", "2", 0, 9000, 400, "94000000000 300000000 51000");
        thread(root, 4251, "crosvm_vcpu1", "08", "3", 0, 8000, 300, "83000000000 200000000 47000");
        thread(root, 4260, "v_gpu", "ff", "0-7", 0, 700, 500, "12000000000 900000000 240000");
        thread(root, 4261, "kgsl-sync", "ff", "0-7", 0, 90, 60, "1500000000 450000000 110000");
        thread(root, 4270, "v_input", "04", "2", 0, 3, 2, "50000000 80000000 9000");
        thread(root, 4271, "virtio_blk", "04", "2", 0, 300, 200, "5000000000 700000000 30000");
        thread(root, 4272, "crosvm resample", "04", "2", 0, 0, 1, "");
    }

    /// The functions with the phone's places under `root`, the stand-in tools first on PATH, then `body`.
    fn run(root: &Path, body: &str) -> String {
        let bin = root.join("bin");
        fs::create_dir_all(&bin).unwrap();
        let functions = FUNCTIONS
            .replace("PR=/proc\n", &format!("PR={}/proc\n", root.display()))
            .replace("KG=/sys/class/kgsl/kgsl-3d0\n", &format!("KG={}/kgsl\n", root.display()))
            .replace("SG=/sys/kernel/gpu\n", &format!("SG={}/samsung\n", root.display()));
        assert!(!functions.contains("=/proc\n") && !functions.contains("=/sys/"));
        let script = format!("PATH={}:$PATH\nD={}\npid=4242\n{functions}{body}", bin.display(), root.display());
        let mut said = String::new();
        for shell in ["sh", "dash", "bash"] {
            let Ok(out) = std::process::Command::new(shell).arg("-c").arg(&script).output() else { continue };
            let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
            // every shell says the same but for the clock
            let plain: String = text.lines().map(|l| if l.len() > 9 && l.as_bytes()[2] == b':' && l.as_bytes()[5] == b':' { &l[9..] } else { l }).collect::<Vec<_>>().join("\n");
            if said.is_empty() {
                said = plain;
            } else {
                assert_eq!(said, plain, "{shell}");
            }
        }
        said
    }

    fn tool(root: &Path, name: &str, body: &str) {
        use std::os::unix::fs::PermissionsExt;
        let path = root.join("bin").join(name);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
    }

    #[test]
    fn a_threads_line_has_its_cpus_class_time_and_wait() {
        let root = folder("threads");
        phone(&root);
        let said = run(&root, "perf_threads now\n");
        let lines: Vec<&str> = said.lines().collect();
        assert_eq!(lines[0], "threads now");
        assert_eq!(lines.len(), 9);
        // the main thread, a vCPU on its core, the GPU's worker and its fence thread anywhere, the device workers on vCPU 0's core
        assert!(lines.contains(&"thread 4242 cpus 0-7 policy 0 rtprio 0 nice 0 last 3 ticks 150 sched 1500000000 20000000 900 comm crosvm"));
        assert!(lines.contains(&"thread 4250 cpus 2 policy 0 rtprio 0 nice 0 last 3 ticks 9400 sched 94000000000 300000000 51000 comm crosvm_vcpu0"));
        assert!(lines.contains(&"thread 4261 cpus 0-7 policy 0 rtprio 0 nice 0 last 3 ticks 150 sched 1500000000 450000000 110000 comm kgsl-sync"));
        assert!(lines.contains(&"thread 4270 cpus 2 policy 0 rtprio 0 nice 0 last 3 ticks 5 sched 50000000 80000000 9000 comm v_input"));
        // a name with a space stays whole (it is last), a kernel without schedstat gives dashes
        assert!(lines.contains(&"thread 4272 cpus 2 policy 0 rtprio 0 nice 0 last 3 ticks 1 sched - - - comm crosvm resample"));
        // a thread that ended between the listing and the read: no line, no error
        fs::create_dir_all(root.join("proc/4242/task/4299")).unwrap();
        assert_eq!(run(&root, "perf_threads again\n").lines().count(), 9);
        // a class of its own shows: SCHED_FIFO 97, as CROSVM_GPU_RT_PRIO would make v_gpu
        let dir = root.join("proc/4242/task/4260/stat");
        fs::write(&dir, fs::read_to_string(&dir).unwrap().replace(" 17 3 0 0 0 0 0\n", " 17 1 97 1 0 0 0\n")).unwrap();
        assert!(run(&root, "perf_threads rt\n").contains("thread 4260 cpus 0-7 policy 1 rtprio 97 nice 0 last 1 ticks 1200"));
        fs::remove_dir_all(&root).ok();
    }

    #[test]
    fn the_device_workers_get_the_process_cpus_and_the_vcpus_keep_theirs() {
        let root = folder("unpin");
        phone(&root);
        // taskset as the phone's: `taskset -p MASK TID` sets the thread's CPUs (here: its status file) and says so
        tool(&root, "taskset", &format!(
            "[ \"$1\" = -p ] && [ $# = 3 ] || exit 2\necho \"$2 $3\" >> {r}/taskset.calls\nf={r}/proc/4242/task/$3/status\n\
             [ \"$3\" = 4271 ] && [ -e {r}/refuse ] && exit 1\n\
             sed -e \"s/^Cpus_allowed:.*/Cpus_allowed:\\t$2/\" -e 's/^Cpus_allowed_list:.*/Cpus_allowed_list:\\t0-7/' $f > $f.new && mv $f.new $f\n", r = root.display()));
        // one shell (the picture changes as it runs); twice, to see that a moved thread is left alone and a refusal is not tried again
        fs::write(root.join("refuse"), "").unwrap();
        let functions = FUNCTIONS.replace("PR=/proc\n", &format!("PR={}/proc\n", root.display()));
        let script = format!("PATH={}/bin:$PATH\npid=4242\n{functions}perf_unpin\nperf_unpin\nperf_threads after\n", root.display());
        let out = std::process::Command::new("sh").arg("-c").arg(&script).output().unwrap();
        let text = String::from_utf8_lossy(&out.stdout).to_string();
        let lines: Vec<&str> = text.lines().map(|l| &l[9..]).collect();
        assert_eq!(lines[0], "sched: v_input (4270) was on cpus 2: it has the process's (ff) now");
        // one that taskset refuses is said once and not tried again
        assert_eq!(lines[1], "sched: virtio_blk (4271) is on cpus 2 and taskset refused: left there");
        assert_eq!(lines[2], "sched: crosvm resample (4272) was on cpus 2: it has the process's (ff) now");
        assert_eq!(lines[3], "threads after");
        assert_eq!(fs::read_to_string(root.join("taskset.calls")).unwrap(), "ff 4270\nff 4271\nff 4272\n");
        // the vCPUs keep their cores, the GPU's threads were never held
        assert!(text.contains("thread 4250 cpus 2 ") && text.contains("thread 4251 cpus 3 ") && text.contains("thread 4270 cpus 0-7 ") && text.contains("thread 4271 cpus 2 "));
        // masks with a comma (a kernel built for more than 32 CPUs writes every one so): the last word is compared and given
        // to taskset, so the one worker still on its core is the one moved
        fs::write(root.join("proc/4242/status"), "Cpus_allowed:\t00000000,000000ff\n").unwrap();
        for tid in [4242, 4250, 4251, 4260, 4261, 4270, 4271, 4272] {
            let status = root.join(format!("proc/4242/task/{tid}/status"));
            fs::write(&status, fs::read_to_string(&status).unwrap().replace("Cpus_allowed:\t", "Cpus_allowed:\t00000000,000000")).unwrap();
        }
        fs::remove_file(root.join("taskset.calls")).unwrap();
        fs::remove_file(root.join("refuse")).unwrap();
        std::process::Command::new("sh").arg("-c").arg(&script).output().unwrap();
        assert_eq!(fs::read_to_string(root.join("taskset.calls")).unwrap(), "000000ff 4271\n");
        fs::remove_file(root.join("proc/4242/status")).unwrap();
        fs::remove_file(root.join("taskset.calls")).unwrap();
        let out = std::process::Command::new("sh").arg("-c").arg(script.replace("perf_threads after\n", "")).output().unwrap();
        assert!(out.stdout.is_empty() && out.stderr.is_empty() && !root.join("taskset.calls").exists());
        fs::remove_dir_all(&root).ok();
    }

    #[test]
    fn the_gpu_limits_surfaceflinger_and_logcat_are_read_when_they_are_there() {
        let root = folder("facts");
        phone(&root);
        // nothing there: no line but the one that says dumpsys is missing, and no error
        tool(&root, "date", "echo 00:00:00");
        let path_only = |body: &str| {
            let functions = FUNCTIONS.replace("KG=/sys/class/kgsl/kgsl-3d0\n", &format!("KG={}/kgsl\n", root.display())).replace("SG=/sys/kernel/gpu\n", &format!("SG={}/samsung\n", root.display()));
            let out = std::process::Command::new("sh").arg("-c").arg(format!("PATH={}/bin:/usr/bin:/bin\npid=4242\n{functions}{body}", root.display())).output().unwrap();
            format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr))
        };
        if std::process::Command::new("sh").arg("-c").arg("command -v dumpsys || command -v logcat").output().unwrap().stdout.is_empty() {
            assert_eq!(path_only("perf_kgsl start\nperf_sf start\nperf_sf look\nperf_sf end\nperf_logcat\n"), "surfaceflinger: no dumpsys here\n");
        }
        // the phone's files: the cap that take12 to take14 ran under, Samsung's own, and the time at each clock
        fs::create_dir_all(root.join("kgsl/devfreq")).unwrap();
        fs::create_dir_all(root.join("samsung")).unwrap();
        fs::write(root.join("kgsl/max_gpuclk"), "832000000\n").unwrap();
        fs::write(root.join("kgsl/thermal_pwrlevel"), "5\n").unwrap();
        fs::write(root.join("kgsl/idle_timer"), "80\n").unwrap();
        fs::write(root.join("kgsl/devfreq/max_freq"), "832000000\n").unwrap();
        fs::write(root.join("kgsl/devfreq/trans_stat"), "     From  :   To\n  160000000:         0        12     10500\n  222000000:        11         0     90200\n").unwrap();
        fs::write(root.join("samsung/gpu_max_clock"), "832\n").unwrap();
        assert_eq!(path_only("perf_kgsl start\n"),
                   "kgsl start max_gpuclk 832000000\nkgsl start thermal_pwrlevel 5\nkgsl start idle_timer 80\nkgsl start devfreq/max_freq 832000000\n\
                    kgsl start samsung/gpu_max_clock 832\nkgsl start trans_stat:\n     From  :   To\n  160000000:         0        12     10500\n  222000000:        11         0     90200\n");
        // surfaceflinger's words: the model's period among everything else, and the timestats as they come
        tool(&root, "dumpsys", concat!(
            "case \"$*\" in\n",
            "  'SurfaceFlinger --timestats -clear -enable') echo 'TimeStats enabled' ;;\n",
            "  'SurfaceFlinger --timestats -disable') echo \"$*\" >> ${0%/*}/disabled.log; echo 'TimeStats disabled' ;;\n",
            "  'SurfaceFlinger --timestats -dump -maxlayers 6') printf 'SurfaceFlinger TimeStats:\\ntotalFrames = 1000\\nmissedFrames = 7\\nlayerName = OneiOS display (crosvm)#88\\npresent2present histogram is as below:\\n8ms=900 16ms=60 25ms=4\\n' ;;\n",
            "  SurfaceFlinger) printf 'Build configuration: x\\nVSyncPredictor:\\n\\tmIdealPeriod=8333333.00\\n\\t\\tIdealized Period=8333333.00, slope=8358021.00, intercept=1234.00\\n* Layer 0x7 (OneiOS display (crosvm)#88)\\nunrelated line\\n' ;;\n",
            "  *) echo \"dumpsys $*?\" ;;\n",
            "esac"));
        assert_eq!(path_only("perf_sf start\nperf_sf look\nperf_sf \"at 300 s\"\n"),
                   "00:00:00 surfaceflinger: timestats TimeStats enabled\n00:00:00 surfaceflinger: its vsync and the layer\nVSyncPredictor:\n\tmIdealPeriod=8333333.00\n\
                    \t\tIdealized Period=8333333.00, slope=8358021.00, intercept=1234.00\n* Layer 0x7 (OneiOS display (crosvm)#88)\n\
                    00:00:00 surfaceflinger: timestats at 300 s\nSurfaceFlinger TimeStats:\ntotalFrames = 1000\nmissedFrames = 7\nlayerName = OneiOS display (crosvm)#88\n\
                    present2present histogram is as below:\n8ms=900 16ms=60 25ms=4\n");
        // what the log enabled it ends by disabling: one call, with nothing else asked of surfaceflinger
        assert_eq!(path_only("perf_sf restore\n"), "00:00:00 surfaceflinger: timestats disabled again\n");
        assert_eq!(fs::read_to_string(root.join("bin/disabled.log")).unwrap(), "SurfaceFlinger --timestats -disable\n");
        // logcat: the display client's dropped flips and drm2kgsl's counts, nothing else of the buffer
        tool(&root, "logcat", concat!(
            "[ \"$*\" = -d ] || exit 2\n",
            "printf '%s\\n' '11-26 00:45:01.000  100  100 I crosvm  : display surface scanout dropped 1 flip(s) so far: no free display buffer (panel-paced)' \\\n",
            "  '11-26 00:45:02.000  100  100 I other   : something else' \\\n",
            "  '11-26 00:45:03.000  100  100 E crosvm  : KGSL_DIAG sync-mode gpu_id=6850 chip_id=0x44050000 mode=legacy-sync-file' \\\n",
            "  '11-26 00:45:04.000  100  100 I crosvm  : CROSVM_DISPLAY_IMPORT fd=9 offset=0x0 stride=0xb40'"));
        let said = path_only("perf_logcat\n");
        assert_eq!(said.lines().count(), 4);
        assert!(said.starts_with("00:00:00 logcat: crosvm's display client and drm2kgsl\n") && said.contains("dropped 1 flip(s)") && said.contains("mode=legacy-sync-file")
                && said.contains("CROSVM_DISPLAY_IMPORT") && !said.contains("something else"));
        // the display client's lines first, drm2kgsl's after them: a hundred of the second kind leave the first whole
        assert!(said.find("dropped 1 flip(s)").unwrap() < said.find("CROSVM_DISPLAY_IMPORT").unwrap() && said.find("CROSVM_DISPLAY_IMPORT").unwrap() < said.find("KGSL_DIAG").unwrap());
        tool(&root, "logcat", "echo 'I crosvm  : display surface scanout dropped 1 flip(s) so far: no free display buffer (panel-paced)'\ni=0; while [ $i -lt 300 ]; do echo \"E crosvm  : KGSL_DIAG where=context-destroy attach=$i\"; i=$((i + 1)); done");
        let said = path_only("perf_logcat\n");
        assert_eq!(said.lines().count(), 82);
        assert!(said.contains("dropped 1 flip(s)") && said.contains("attach=299") && !said.contains("attach=219\n") && said.contains("attach=220\n"));
        fs::remove_dir_all(&root).ok();
    }

    #[test]
    fn the_parts_of_boot_sh_are_there_only_when_asked_and_every_shell_reads_them() {
        let none = Perf::default();
        assert!(start_script(&none).is_empty() && end_script(&none).is_empty());
        let both = Perf { log: true, unpin: true };
        let (log, unpin, all) = (start_script(&Perf { log: true, unpin: false }), start_script(&Perf { log: false, unpin: true }), start_script(&both));
        // the log alone changes nothing: no taskset is ever run; the workers alone read no dumpsys, no logcat
        assert!(!log.contains("perf_unpin;") && log.contains("perf_sf start") && log.contains("perf_logcat\n") && log.contains("perf_threads \"25 s after the start\""));
        assert!(unpin.contains("perf_unpin;") && !unpin.contains("perf_sf start") && !unpin.contains("perf_logcat\n") && !unpin.contains("perf_kgsl start"));
        assert!(all.contains("perf_unpin;") && all.contains("perf_sf end"));
        // TimeStats are enabled at the start, and disabled again once crosvm is gone (before logcat, after the last dump)
        assert!(log.contains("perf_sf start") && log.contains("perf_sf end\n  perf_sf restore\n  perf_logcat\n") && !unpin.contains("perf_sf restore"));
        // its own process, which does not run the script's traps, writes one file and is waited for at the end, 20 s at most
        for s in [&log, &unpin, &all] {
            assert!(s.contains("(\n  trap - EXIT HUP INT TERM\n") && s.contains(") > $D/perf.txt 2>&1 &\nperfp=$!\n"));
            assert!(s.contains("while kill -0 $pid 2>/dev/null; do\n    sleep 1;"));
        }
        let end = end_script(&both);
        assert!(end.contains("while [ $t -lt 20 ] && kill -0 $perfp 2>/dev/null") && end.contains("kill $perfp 2>/dev/null"));
        for (n, script) in [log, unpin, all, end].iter().enumerate() {
            let path = std::env::temp_dir().join(format!("oneios-vmperf-{}-{n}.sh", std::process::id()));
            fs::write(&path, script).unwrap();
            for shell in ["bash", "sh", "dash", "mksh"] {
                let mut c = std::process::Command::new(shell);
                if shell == "bash" {
                    c.arg("--posix");
                }
                if let Ok(o) = c.arg("-n").arg(&path).output() {
                    assert!(o.status.success(), "{shell} -n: {}", String::from_utf8_lossy(&o.stderr));
                }
            }
            fs::remove_file(&path).ok();
        }
    }

    #[test]
    fn the_vsync_part_starts_pacectl_on_the_page_and_ends_it() {
        // take14's Image: the pool at 0x15d800000 (0xdd800000 into the memfd), the frame sequence 4505600 bytes into it
        let start = vsync_start_script(0xdd80_0000 + 4_505_600, -1500);
        assert!(start.contains("$D/pacectl vsync --pid $pid --page-offset 3720658944 --phase-us -1500 --phase-file $D/vsync.phase </dev/null >>$D/pace.txt 2>&1 &\n"));
        assert_eq!(0x8000_0000u64 + 3720658944, 0x15dc4c000);   // the address the kernel's boot argument oneios-frame-seq had
        // only for a VM whose tree was accepted, and never a reason to stop: without the tool the tick runs free
        assert!(start.starts_with("pacep=\nif [ \"$pool\" != bad ] && kill -0 $pid 2>/dev/null; then\n") && start.contains("the VM's refresh tick runs free"));
        let end = vsync_end_script();
        assert!(end.contains("kill $pacep 2>/dev/null") && end.contains("grep '^pacectl: panel ' $D/pace.txt"));
        for (n, script) in [start, end].iter().enumerate() {
            let path = std::env::temp_dir().join(format!("oneios-vmperf-vsync-{}-{n}.sh", std::process::id()));
            fs::write(&path, script).unwrap();
            for shell in ["bash", "sh", "dash", "mksh"] {
                let mut c = std::process::Command::new(shell);
                if shell == "bash" {
                    c.arg("--posix");
                }
                if let Ok(o) = c.arg("-n").arg(&path).output() {
                    assert!(o.status.success(), "{shell} -n: {}", String::from_utf8_lossy(&o.stderr));
                }
            }
            fs::remove_file(&path).ok();
        }
    }

    #[test]
    fn a_whole_run_unpins_logs_and_ends_with_crosvm() {
        // The process of boot.sh from its start to crosvm's end, with a stand-in crosvm that lives 3 s: seconds are the
        // script's own, so the 5 s unpin and the 25 s report are made to come sooner (the same lines, smaller numbers).
        let root = folder("run");
        phone(&root);
        tool(&root, "taskset", &format!(
            "f={r}/proc/4242/task/$3/status\nsed -e \"s/^Cpus_allowed:.*/Cpus_allowed:\\t$2/\" -e 's/^Cpus_allowed_list:.*/Cpus_allowed_list:\\t0-7/' $f > $f.new && mv $f.new $f\n", r = root.display()));
        tool(&root, "dumpsys", &format!("echo \"dumpsys $*\"\necho \"$*\" >> {}/dumpsys.calls", root.display()));
        tool(&root, "logcat", "echo 'I crosvm: display surface scanout dropped 1025 flip(s) so far: no free display buffer (panel-paced)'");
        let start = start_script(&Perf { log: true, unpin: true })
            .replace("PR=/proc\n", &format!("PR={}/proc\n", root.display()))
            .replace("KG=/sys/class/kgsl/kgsl-3d0\n", &format!("KG={}/kgsl\n", root.display()))
            .replace("SG=/sys/kernel/gpu\n", &format!("SG={}/samsung\n", root.display()))
            .replace("[ $((pn % 5)) = 0 ]", "[ $((pn % 1)) = 0 ]")
            .replace("[ $pn = 25 ]", "[ $pn = 2 ]")
            .replace("while kill -0 $pid 2>/dev/null; do\n    sleep 1;", "while kill -0 $real 2>/dev/null; do\n    sleep 1;");
        assert!(start.contains("[ $pn = 2 ]") && start.contains("kill -0 $real"));
        // $pid names the stand-in's files; $real is a process that ends by itself
        let script = format!("PATH={b}/bin:$PATH\nD={d}\npid=4242\nsleep 3 &\nreal=$!\n{start}{end}echo \"left: $(kill -0 $perfp 2>/dev/null && echo running || echo gone)\"\n",
                             b = root.display(), d = root.display(), end = end_script(&Perf { log: true, unpin: true }));
        let out = std::process::Command::new("sh").arg("-c").arg(&script).output().unwrap();
        let said = String::from_utf8_lossy(&out.stdout).to_string();
        assert!(out.stderr.is_empty(), "{}", String::from_utf8_lossy(&out.stderr));
        let perf = fs::read_to_string(root.join("perf.txt")).unwrap();
        let at = |what: &str| perf.find(what).unwrap_or_else(|| panic!("no {what:?} in\n{perf}"));
        // the order of a session: the start, the workers moved, the threads' report (after the move: their old CPUs are in the
        // sched lines), surfaceflinger, then crosvm's end and the last part
        assert!(at("perf: crosvm 4242: the log, and the device workers take the process's CPUs") < at("surfaceflinger: timestats dumpsys SurfaceFlinger --timestats -clear -enable"));
        assert!(at("sched: v_input (4270) was on cpus 2: it has the process's (ff) now") < at("threads 25 s after the start"));
        assert!(at("threads 25 s after the start") < at("surfaceflinger: its vsync and the layer"));
        assert!(perf.contains("thread 4270 cpus 0-7 ") && perf.contains("thread 4250 cpus 2 "));
        assert!(at("surfaceflinger: timestats end") < at("surfaceflinger: timestats disabled again") && at("surfaceflinger: timestats disabled again") < at("logcat: crosvm's display client and drm2kgsl"));
        // enabled once at the start, disabled once at the end: the last call to surfaceflinger's TimeStats is the disable
        let calls = fs::read_to_string(root.join("dumpsys.calls")).unwrap();
        let stats: Vec<&str> = calls.lines().filter(|c| c.contains("--timestats") && !c.contains("-dump")).collect();
        assert_eq!(stats, ["SurfaceFlinger --timestats -clear -enable", "SurfaceFlinger --timestats -disable"]);
        assert!(perf.contains("dropped 1025 flip(s)") && perf.trim_end().ends_with("perf: the end"));
        // boot.sh's own words, and its wait for the file to be whole
        assert!(said.contains("perf: the log, and the device workers take the process's CPUs: perf.txt\n"));
        assert!(said.contains(" lines; device workers given the process's CPUs: 3\n") && said.ends_with("left: gone\n"), "{said}");
        fs::remove_dir_all(&root).ok();
    }
}
