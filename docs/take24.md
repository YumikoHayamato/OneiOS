# take24 (2026-11-28): the pool's fill in steps toward 8 GiB, and what take23's first session showed

Made on the PC from take23's first session on the phone (4608 MiB, the new boot order); nothing of take24 has run on the
phone. The Images, the Italian README and the analysis with the session's log lines are the owner's own copy, not in
this repository. The kernel is take22's and the loader take23's, both unchanged.

## What take23's session said about the pool

With One UI stopped Android had 9.4 GiB available (9.3 free). The module's acquire (mode 3) took 2437 pages of 2 MiB in
2 s and ended with `scanned all present memory`; two more presses added 6 pages. 4.4 GiB stayed available to Android and
the 4608 MiB Image ran. gh_hugepage_reserve takes a 2 MiB window only when every page in it is free, on the LRU or
movable (`gh_a_candidate_range`): eviction and migration act on the windows that pass, so with the memory free already
the fill stopped on windows that hold a page nothing moves (slab, page tables, kernel stacks, drivers' pages), not on
a lack of free memory. While the VM ran Android's MemAvailable went from 4402 MiB (after the fill) to 3816 at the lowest:
crosvm, the renderer and the GPU's memory take about 590 MiB beyond the pool.

## oneios-tool

- `phone vm-boot --fill-after-stop`: **one fill, in steps, before the choice** (`crates/oneios-phone/src/take23.rs`, PICK).
  The fill goes to the largest candidate's pages, bounded by the module's table (`pool_size_max`) and by what leaves
  Android `--host-keep-mib` beside `--host-overhead-mib` of MemAvailable. Steps, each run only while the pool is short and
  each logged (`fill: step NAME: pool A -> B pages (+N) in T s; the module's stop reason: ...`):
  1. `sweep`: acquire 3 (the module drops the slab caches itself, then sweeps every window);
  2. `after-slab-shrink`: `echo 1` to every `/sys/kernel/slab/*/shrink` (the empty and half-empty slabs SLUB keeps for
     each CPU and cache), `sync`, `drop_caches` 3 again, acquire 3;
  3. `after-compaction`: `/proc/sys/vm/compact_memory`, acquire 3;
  4. `with-reclaim`: acquire 2 (the sweep with the module's system-wide reclaim).
  Before and after: `fill: before|after:` lines with meminfo's kernel memory (Slab, SUnreclaim, PageTables, KernelStack,
  VmallocUsed, KgslShmemUsage, CmaFree), the free blocks by kind and order and the 2 MiB blocks by kind
  (`/proc/pagetypeinfo`), and the module's `reclaim_debug`. `fill: done:` says why it stopped (the target reached and
  which bound set it, or the module's reason and the pages short). Then the choice: the largest Image whose pages the
  pool holds; it presses no acquire of its own. Pages the pool holds beyond the Image's are logged (`stay unused`).
- `--host-keep-mib` is 600 by default (1024 in take23) and now means what Android has **while the VM runs**:
  `--host-overhead-mib` (600: the 586 measured above) is counted beside it. Under about 512 MiB of MemAvailable the phone
  stalls and restarts by itself (the module's authors measured it; their sweep brakes there).
- `--pagecensus FILE` (`runtime/pagecensus`, `make -C runtime/pagecensus`): pushed and run once after the fill, read
  only. From `/proc/kpageflags` over `/proc/iomem`'s System RAM: the 2 MiB windows all free, with movable pages only,
  held whole, and those refused, by the kinds of page that refuse them (slab, page tables, other kernel pages, reserved),
  with the free memory inside and how many pages refuse (1-4, 5-64, more). `fill: census:` lines. `make test` runs its
  classifier on a made-up flags file.
- `phone hugepool load --want N --leave-mib M`: the earlier lever. The module's insmod takes the want from the 2 MiB
  blocks free at that moment (its cheap and full stages), and at 40 s of uptime (take23's load) far more blocks are whole
  than after One UI has started. `--leave-mib` lowers the want on the phone to `(MemAvailable - M) / 2` pages so that
  Android can go on starting. The load prints the uptime, MemFree, MemAvailable, CmaFree and `/proc/buddyinfo` before and
  after the insmod. Not tried on the phone: a want Android cannot spare stalls it (a forced restart, nothing is kept).
- `hostmem.txt`: crosvm's resident memory is summed over every crosvm process (take23's column read 0), and CmaFree and
  KgslShmemUsage are added.
- The takeover's failsafe gets `FILL_SECONDS` (480) plus 10 s a candidate instead of 200 s a candidate.

## Images

`scripts/take24/images.sh`: 4608, 5120, 5632, 6144, 6656, 7168, 7680, 8192 MiB (CoreUI's memory class 3, 3, 3, 6, 6,
6, 6, 8) and `-plain` (4608, no identity groups). 8704 MiB is not made: with One UI stopped about 9400 MiB are available,
624 of them CMA that only movable pages may use, and an iOS of 8704 would leave Android about 100 MiB outside it.
By the same numbers 8192 leaves Android about 670 MiB while the VM runs, most of it CMA: the owner's choice, at the edge.

## Checks on the PC

`docs/take24/sim/fill-drive.sh MEMAVAIL_MIB POOL_AVAIL POOL_WANT PMAX KEEP "GAINS"`: the fill and the choice under the
PC's shell against fake `/proc` and module files (a pool that stops short and gains at each step, one that fills at once,
an early pool bounded by the keep, a full pool with Android short). `cargo test -p oneios-phone`.
