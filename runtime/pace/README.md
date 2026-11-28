# pacectl: the panel's vsync for the VM

`oneios-tool phone vm-boot --vsync-lock` (docs/native.md, "Frame pacing"). The native kernel's refresh tick was a timer
of its own: last + 1/120 s of the guest's counter. The S25's panel refreshes by its own oscillator and surfaceflinger
latches on that, so the guest's frames slipped past the latch at the difference of the two rates. `pacectl` gives the
kernel the panel's grid; the kernel's half is `kernel/patches/oneios-hosted/display/vsync_lock.h` (the record's layout
and the arithmetic, shared with this tool and its tests), `OneiOSDisplay.cpp` and the staged `NativeLegacyDisplay.cpp`
(`staged/display.rs` `LEGACY_VSYNC`).

**State, plainly.** Built for the phone and for the build host; the host tests pass. It has never run on the phone:
whether a root process outside the framework gets `AChoreographer`'s callbacks during a takeover, and whether crosvm's
memfd opens to write through `/proc/PID/map_files`, are the phone's to say (pace.txt says which step failed; the VM's
tick then runs free, as without the tool).

```sh
make -C runtime/pace          # build/android/pacectl (dynamic, API 33: libandroid's AChoreographer and ALooper)
make -C runtime/pace test     # the build host: build/host/pacectl against a stand-in page and clock (26 checks)
```

## On the phone

`vm-boot --vsync-lock [--vsync-phase-us N]` pushes `pacectl` and boot.sh starts it once crosvm's tree was accepted:

```sh
pacectl vsync --pid PID --page-offset N [--phase-us N] [--phase-file FILE] [--every-ms N] [--say-s N] [--seconds N] [--wait-s N]
```

- `PID` is crosvm's, `N` the frame sequence's page in its guest memfd (the pool's place less the RAM's base plus the
  Image's `fb ... SEQ`; a shared-RAM VM only: a protected VM's RAM is not the host's to write). The page is found in
  crosvm's own mapping (`/proc/PID/maps`, a shared `/memfd:crosvm_guest` that holds it), opened through
  `/proc/PID/map_files` and checked to be that mapping's file, as `displayctl pool` finds the frame.
- **Nothing is written until the guest has claimed the page.** The offset is a computed guess (the formula has only run read
  only, for protected VMs); the kernel writes `OFSQ` (version 1) into the page's first word once it has mapped it
  (`oneios-frame-seq`, `OneiOSDisplay.cpp`). `pacectl` waits for that word as long as crosvm lives (gone: exit 1; `--wait-s N`
  gives up after N s, exit 1, no write). The word is checked again before every record
  (a restarted guest zeroes the page), a record is skipped while it is not there. The kernel reads the page only with its
  boot argument `oneios-vsync-lock=1` (default 0), so without it the records are written and ignored.
- Every 250 ms (`--every-ms`) one `AChoreographer` vsync callback: the preferred frame timeline's deadline (the time by
  which surfaceflinger wants a frame for that refresh; the frame time where there is none) and the period (two
  timelines' presentation times apart; two callbacks apart where there is one timeline). Both go into the page as
  ticks of the counter (`CNTVCT_EL0`, which Android and the VM read alike: scantime.h), with the counter now: +0x100
  "OVSY", a sequence that is odd while the fields are written (a seqlock, fenced for arm64: `vsync_lock.h`), the grid point, the period in 1/256 tick, the time of
  writing, the phase.
- The phase: `--phase-us`, or the number in `--phase-file` (vm-boot: the VM folder's `vsync.phase`) read at every turn,
  so a session moves the kernel's tick against surfaceflinger's deadline without a restart.
- Every 10 s (`--say-s`) a line with the panel's period and the kernel's own record of the same page (+0x180: its last
  tick, ticks sent, ticks placed on the grid, the grid taken up and given up): `pacectl: panel 8355012 ns (119.688 Hz),
  phase 0 us; 480 records (0 turns without); the kernel's ticks +1197, on the panel's grid +1197 (taken up 1, given up
  0)`. A kernel whose last tick is not near this counter's now is said once: its counter is another, and it refuses
  the records by itself.
- It ends with crosvm, at `--seconds` or at a signal. Its last record is stale 2 s later and the kernel's tick runs
  free again; nothing is to be put back.

## Tests (the build host)

`tests/test_pace.c` runs the host's `pacectl` with `--test-guest FILE --test-period-ns N` (a plain file for crosvm's
memfd, never a device or `/proc`; a clock of `pacectl`'s own for surfaceflinger's) and plays the kernel with the
kernel's header: the record is whole and a display's, its grid point is on the stand-in panel's grid, 1.2 s of ticks
placed by it stay on that grid between half and one and a half periods apart, a phase written to the file reaches the
record and moves them, the frame count beside the record is untouched, a page without `OFSQ` (or with another word)
is waited for and then left untouched to the byte, one claimed late gets its first record after the claim, and 2 s after `pacectl` ended the tick is the
free-running one. The arithmetic alone (30 s of a slower panel, late ticks, stale, torn and absurd records) is in
runtime/xnuhost's host tests (`test_vsync_lock`), with the kernel's other shared headers; `make check-order` compiles the header with the NDK for arm64 and
`tests/check_order.sh` finds the `dmb ish` between the odd sequence and the fields, the `stlr` of the even one, and the
`ldar` and `dmb ishld` of the reader in the assembly.
