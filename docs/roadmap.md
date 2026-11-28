# OneiOS roadmap — native path (Galaxy S25 Ultra)

Owner goal (2026-11-25): make the native Gunyah VM feel like a real daily driver — **flatten the
iOS↔Android power gap and kill the stutter** (Safari must be fluid), then fill the missing hardware.
This file is the single to-do list; work against it, update status in the same commit as the change.

Legend: ✅ shipped to a branch · 🔄 in progress · ⏳ todo · 📱 needs an owner-run phone session to confirm.

---

## 1. Where we actually are (log-grounded)

- **Hosted path** (XNU as a Linux process): full boot, Metal on Adreno 830, up to 120 fps, but the
  syscall-trap overhead and the thermal trap (case 46 °C → iOS "too hot" → ~10 fps, session 7) pushed the
  owner to the native path.
- **Native path** (XNU alone in a Gunyah shared-RAM VM via crosvm): full boot to SpringBoard, up to 8 vCPU,
  4.3-4.6 GiB, GPU scanout, audio, real Wi-Fi (remembered), touch + side keys + keyboard, takeover. Our ~200
  commits; **not in upstream.** This is our unique value.
- **Status 2026-11-27:** take13-take15 are done. take16 (4608 MiB + `--jetsam-product V54`) fixed the backboardd
  jetsam kills: more fluid, Wi-Fi remembered. take17 ran at 4352 MiB (the pool reached only 2223 pages) and
  showed the remaining bugs (§P0). take18 combined image: built, **pending a phone run** (§P0).

## 2. The two findings that explain "it feels like 50 fps and runs hot"

1. **The stutter was the panel, not the GPU.** During take12 the panel was capped at 60 Hz (power saving),
   while iOS drew ~100 frames/s — half were dropped, unevenly. → branch `native-frame-latency` (Fable):
   the display helper now forces 120 Hz in a takeover. PC-tested only.
2. **Most of the power gap is waste, not the VM.** Evidence, take12 (my clock floors on) vs take10 (governor):
   perf cores idle **0 % vs 84 %**, GPU clock 609 vs 282 MHz, big-core zone **63.7 vs 55.7 °C** — for the *same*
   GPU work. The floors I added were a stutter cerotto that only burned power. → branch `native-energy` (Fable):
   floors **off by default**, opt-in flags kept. Plus the busy-polled virtio-gpu (100 µs spins, a vCPU never
   deep-idles) is the next structural win — see P1.

## 3. Priorities

### P0 — take18 (the next session) and what take17 showed
- ✅ take13-take15: floors-off + 120 Hz panel, native-VM bridge bugs (battery, haptics, Wi-Fi memory, power-key),
  mic kernel path. Done.
- ✅ take16: 4608 MiB + `--jetsam-product V54` stops the backboardd jetsam kills; Wi-Fi remembers the network.
- ✅ take17 findings (4352 MiB; log-grounded), each fixed in take18:
  - **Mic:** kernel path works (self-test hears sound). iOS gave up on the missing `ssrc` (input data source)
    property of the Mic device. take18 publishes a `dsrc` control (mc01..mc04).
  - **Lock screen / shade:** GPU sat at Samsung's 607 MHz cap the whole session (91/165 rounds over 32 ms). take18
    lifts the cap by default in takeover (`--keep-gpu-cap` = old way; more heat on heavy screens).
  - **YouTube:** the Wi-Fi bridge dropped 15 % of frames (aggregated QUIC UDP) — take18 cuts them back into
    datagrams. Black picture = backboardd hitting the 2 GiB GPU VA slice (9,694 `OutOfMemory` surface refusals) —
    fixed by virglrenderer 2529a9f (6 GiB slices) plus the Metal surface-use fix (NativeMetal 0003).
  - **Apple ID:** akd `-45017`, `com.apple.adid` is in the disabled group. Test with `oneios-enable-jobs`.
- 🔄📱 **take18 combined image: pending.** Carries take18's four fixes plus everything in
  `take18/upstream/README.txt` (items 1-7, each with its switch): virglrenderer 2529a9f (default), crosvm from
  source (off), guest Metal patches (25), syscall-mig 254/319 cut, video decoder (off), lock-screen wake, takeover
  zram compaction, virtio-snd IRQ (off), samfor12 pool module (off). Images 4608 (pool >= 2300 pages) or 4352.
  The Metal stack is pushed to `ci/metal-plugin` **68f7072** and building on CI. Session order: Voice Memos,
  lock screen (+ Reduce Transparency A/B), YouTube, GIF page.
- ⏳ **Left as written plans:** NativeMetal `86fd95c` (discard idle Volatile resources, ~1650 lines) and `8341127`
  (iOS shader cache; ties to the P2 shader item).

### P1 — the real performance + strategy work
- ⏳📱 **Kill the virtio-gpu busy-poll.** Give the GPU PCI function a real interrupt (or an adaptive spin→WFI
  backoff) so idle cores sleep. Biggest win for both the 6.6 ms frame round-trip and power. Can't be settled on
  the PC (vendored transport + needs a Gunyah interrupt A/B); plan written in `native.md`.
- ⏳ **Upstream strategy — decided: cherry-pick, do NOT merge.** We are 200 ahead / **535 behind** / *diverged*,
  and upstream rewrote the runtime in **Rust**. A wholesale merge means re-doing our entire C native path against
  Rust — not worth it. Instead: port the isolated wins and mine upstream's `ports-2026-11-25-hw.md`:
  - **Bluetooth (AIDL):** the S25 HAL is AIDL-only; our bridge is HIDL. Port `Musicalista/OneiOS-A56@26a2276`
    (AIDL `IBluetoothHci` client in C; `git apply --check` passes on our tree). Fix the off-by-one interface
    version upstream found.
  - **Auto-brightness:** port upstream's sensor-hub bridge (front ALS over QRTR) + the guest ALS events; our
    `als_rear` evdev reads 0 lux. First check: can a root process on the S25 reach sensor service 400 over QRTR?

### P2 — polish + cheap features (mostly reuse)
- ⏳ **Display fit:** keyboard globe glyph cut off + keyboard too high = iOS safe-area insets for the iPhone
  17 Pro Max (rounded corners / Dynamic Island / home indicator) vs our 720×1560@2x panel. Tune the identity /
  insets.
- ⏳ **Shader/pipeline cache persistence:** Settings reloads its logos every boot. The Metal pipeline cache isn't
  in the persisted Data volume, and the prime-list is 1 library (was 309). Persist the cache + restore the full
  prime list.
- ⏳📱 **S Pen → Apple Pencil:** our `pen.c` is already the best implementation; never run on the phone. Just a
  hosted session with `pen=auto`.
- ⏳📱 **GPS:** adapt `coreycusick/ios-guacamole@a3c8ef9` `sweet-gnssfeed.m` (feeds fixes into locationd, iOS 27.2 /
  XNU 12377, confirmed); build for 24A446, inject one fixed coordinate, see if Maps takes it.
- ⏳ **16K logical pages over 4K hardware PTEs in the XNU pmap** to stop the 32 MiB per-process shared-cache
  privatization (Safari RAM tax): weeks, for Fable. Reason: the 16 KB granule is **not available** at stage 1 or
  stage 2 (`oneios-granule`), so a real 16 KB XNU is out.
- ⏳📱 **RAM experiment after take15 (owner, 2026-11-26):** One UI down → push Android userspace to swap (RAM Plus) +
  compact → lower gh_hugepage_reserve's `system_reserve_mb` (6144 now) in steps 5.5/6/6.5 GB → fill the pool; measure
  pool size and that One UI comes back. Also: smaller metal2vulkan keep-room (block-mib 16 hung upstream: avoid), prune
  useless iOS daemons by RAM. Later/large: guest free-page reporting or balloon driver for XNU (check crosvm support).
- ⏳ **RAM 6–7.5 GB:** owner's idea. Growable/preload pools do **not** exceed physical RAM; the realistic ceiling is
  about **6-6.5 GB** via the RAM experiment above (upstream's own commit finds ~6.4 GiB). Reserve the full amount
  up front = a hugepool-sizing change; verify the way-back still fits.
- ⏳ **Own benchmark apps** (2048 + a Metal cube): CI build, a `roottweak --apps` step, LaunchServices registration;
  ~1.5 days. Gives repeatable perf numbers without App Store.
- ⏳ **SM8750 bare-metal repos** to read for camera / GPS drivers.

### P3 — big / later
- ⏳ **Safari RAM:** 99 processes each privatize the *same* 32 MiB shared-cache range at launch (a per-process
  RAM tax; likely the 4K-vs-16K shared-region nesting). Fix is deep XNU pmap/shared-region work — dedicated pass.
- ⏳📱 **Mobile data:** no public code fits; treat as uplink-only first (native VM: does iOS reach the internet
  over the phone's data link with Wi-Fi off?). Real SIM/calls = separate large project.
- ⏳📱 **Screen recording from the stock Control Center button — stays NEXT STAGE after take18 is confirmed on the
  phone (mic, lock screen, YouTube).** take14 (iOS log via `ioslog`): the countdown
  runs, then `RPRecordingErrorDomain -5833`. The first failing stage is capture: backboardd plugs the virtual display
  and clones Display 1, but `get_wireless_surface_options` answers `0xE00002D5` (kIOReturnBusy) and replayd gets
  0 frames out of 903 writeback requests ("start timeout"). Audio tap worked; writer, encoder and Photos never reached.
  The HEVC/HDR flags (`oneios-feature-off=...ReplayKit/recordingHEVC,ReplayKit/recordingHDR`) were written and are
  not the blocker. Order: (1) find who answers Busy on the writeback-surface path (backboardd's FigVirtualFramebuffer
  server vs replayd's pool IOSurfaces; ~1-2 days RE of QuartzCore/MediaToolbox); (2) try iOS's own software H.264
  (H264SW registers `avc1`); (3) only if too slow/hot, the hardware encoder: Droid-VM's crosvm f7c6ce8 already has
  `--virtio-media kind=encoder` (AMediaCodec; needs `uid=` and `--pre-alloc media-host-mb=`), so only the guest side
  is missing. `kind=decoder` could also move YouTube decode off the CPU; `kind=camera` is a camera bridge. Mic
  recording also needs a capture device (`-11814`).
- ⏳ **Growable RAM pool (guest driver):** large later item; needs a guest-side balloon/free-page driver.
- ⏳ **NFC tag read:** someday idea. Apple Pay is impossible (no Secure Element).
- ⏳ **Camera:** near-term = a virtio camera bridge (raw frames from Android's camera HAL into AVFoundation, aim
  4K, no Deep Fusion). Reverse-engineering the iPhone ANE/ISP "dump" for computational photography = someday R&D,
  explicitly not now.

## 4. Decisions made (so they stop being re-litigated)

- Next session = **take18 combined** (4608 or 4352 MiB by pool size). Owner just runs it.
- **No wholesale upstream merge** — cherry-pick BT AIDL + sensor bridge (§P1; from akshaynexus/OneiOS).
- **Droid-VM is the only usable crosvm fork.** Check pin ages per component: our libvirglrenderer was from
  2026-09-01 (cd71ea4) and missed months of fixes.
- Clock floors **off by default** (shipped, `native-energy`).
- **Camera:** virtio bridge near-term; ANE reverse-engineering deferred.
- The hackios "leaderboard" is **self-reported** and doesn't track our native path — not a work driver; we're
  ahead of our own listed row.
- `appinst` IPA installer: **parked** (PR #1, unbuilt) until the performance work lands.

## 5. Branches / work in flight

| Branch | What | State |
|---|---|---|
| `native-frame-latency`, `native-energy`, `native-vm-bridges` | 120 Hz panel, floors off, bridge bugs | ✅ done (take13-take15) |
| `take18` (wt-take18) | mic dsrc, Wi-Fi UDP cut, GPU cap lift | built, nothing committed, needs phone |
| `take18b` (wt-take18b) | upstream ports p01-p08 | built on the PC, nothing committed |
| `ci/metal-plugin` | Metal stack, 68f7072 | pushed, building on CI |
| `appinst-ipa-installer` | on-device IPA installer | parked (PR #1) |

## 6. The only things that need the owner

1. **Run take18** (physical device, you watch it), after the CI Metal build is green.
2. **Cleanup, later:** `/mnt/tkg-build` is full (~278 MB free): delete old take13-15 images; take18/take18b
   commits are still pending.

Everything else is mine to drive.
