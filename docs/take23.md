# take23 (2026-11-28): identity a + e, the new boot order, the clocks' orchestrator

Made on the PC from take22's two sessions; nothing of it has run on the phone. The owner's copy of the Images, the
Italian README and the whole analysis: `/mnt/giochi/Linux/oneios/take23/` (README.txt, analysis/analysis-take23.txt).
The kernel is take22's, unchanged. The plug-in is `ci/metal-upgrade` 72dc37c, the apps `ci/apps` 94ab61c.

## Loader (`runtime/loader`)

- `device-perf-memory-class` is CoreUI's enumeration, not the GiB. Its table (`_gThemeMemoryPerformanceClasses`, read in
  24A446's shared cache at 0x1e1555ca0; `docs/take23/coreui-memclass.py`): 0 512MB, 1 1GB, 15 1.5GB, 2 2GB, **4 3GB,
  3 4GB**, 6 6GB, 8 8GB, 9 9GB, 12 12GB, 16 16GB. The memclass group now writes the largest class the VM's RAM holds
  (`xl_machine_memory_class_of`): 4352 to 6143 MiB give 3, 6144 to 8191 give 6. take19 wrote the whole GiB (4 = the 3 GB class).
- `--identity-omit NAMES`: properties an identity group that is on leaves out (the artwork group without
  `graphics-featureset-class,graphics-featureset-fallbacks`). `--display-gamut sRGB|P3`: the artwork group's gamut.
- `--ram-mib` up to 8192 (crosvm places its PCI window after the RAM; no Image above 4608 MiB has booted yet).

## oneios-tool (`crates/oneios-phone/src/take23.rs`)

- `phone hugepool load --system-reserve-mb N`: gh_hugepage_reserve sizes its table at insmod
  (`(RAM - min(RAM/2, system_reserve_mb)) / 2 MiB`, 6144 by default: about 2770 pages on the S25 Ultra). 3072 gives about 4000.
- `phone vm-boot --fill-after-stop --image-larger FILE... [--host-keep-mib 1024]`: the memory rule is asked on the phone
  after One UI's stop; the largest Image that fits is run (`pick:` lines in run.txt). `hostmem.txt`: Android's memory
  beside the VM every 5 s.
- `phone vm-boot --orchestrator [--orchestrator-set KEY=VALUE]`: with the renderer's `deadline` floors the GPU sat at
  1200 MHz for 67 % of take22b at 20 % busy and the vCPUs' uclamp.min was 1024 whenever the compositor animated. The
  orchestrator moves the GPU's cap (Samsung's gpu_max_clock) with its load from 607 MHz up, and the clamp (the renderer's
  `/data/local/tmp/drm2kgsl-vcpu-uclamp`) with the guest's swaps on time (`oneios-display-pace`), 1024 down to 512. `orch.txt`.
- Without these options the boot script is take22b's line for line but for the list of files it removes at its start.
- What take22b's rounds say of the menus that lag at every opening (the analysis, section 6): backboardd needs 16 ms of CPU
  a swap there against 6-7 when scrolling, with 2-3 times the unix system calls; not blobs, not the GPU, not compiles.
  The `-diag` Image (frame trace, pipeline journal, 0011's allocation counter) is the next measurement; no cure is in take23.

## Plug-in and apps (the CI branches)

- NativeMetal 0011: asks with `MTLPipelineOptionFailOnBinaryArchiveMiss` are answered from the kept states or made in
  the background (`oneios-prewarm-archive=0` opts out); `oneios-res-log=1`: what backboardd allocates, by size and time.
- ScreenRecord.inc: the copy's time split (event waits, IOSurface locks, copy) and the copies a second of a recording.
- oneios-appreg: its own bindfs union of `/private/var/oneios/appsview` over /Applications (the kernel's view of the
  staged apps is the OS volume's, taken before Data is mounted), with a check and an unmount when /Applications lists
  less than before. `ONEIOS_APPREG_VIEW=0` opts out.

## Recipes

`scripts/take23/mk-addon.sh` (trust list and merged addon), `scripts/take23/images.sh` (the ten Images: seven RAM sizes, -plain, -diag, -gpuirq),
`docs/take23/t8.py` (clocks, load and frames of a session: thermal.txt, serial.txt, perf.txt).
