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
  3.5 GiB, GPU scanout, audio, real Wi-Fi, touch + side keys + keyboard, takeover. Our ~200 commits; **not in
  upstream.** This is our unique value.

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

### P0 — prove the two big levers, fix the visible bugs
- 📱 **take13: floors-off + 120 Hz panel, one session.** Rebuild `oneios-tool` from `native-energy`, run with
  `--bridge` from the `native-frame-latency` worktree. Proof: `run.txt` no longer prints `the GPU's lowest clock
  is 607000000 Hz`; `thermal.txt` `f6`/`g` fall back when the screen is still; the panel line reads 120 Hz.
  → **ready; owner presses go.**
- 🔄 **Native-VM bridge bugs** (branch `native-vm-bridges`, agent running): battery stuck at 0 %, haptics fire
  once then die, Wi-Fi forgets the network, power-button exit "click" delayed.

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
- ⏳ **RAM 6–7.5 GB:** owner's idea. Partly done already (takeover tops up the pool while One UI is down,
  ~5.48 GB free then). Reserve the full amount up front = a hugepool-sizing change; verify the way-back still fits.

### P3 — big / later
- ⏳ **Safari RAM:** 99 processes each privatize the *same* 32 MiB shared-cache range at launch (a per-process
  RAM tax; likely the 4K-vs-16K shared-region nesting). Fix is deep XNU pmap/shared-region work — dedicated pass.
- ⏳📱 **Mobile data:** no public code fits; treat as uplink-only first (native VM: does iOS reach the internet
  over the phone's data link with Wi-Fi off?). Real SIM/calls = separate large project.
- ⏳ **Screen recording:** nobody has it; needs the HEVC/H.264 encoder bridge first, then read `replayd`.
- ⏳ **Camera:** near-term = a virtio camera bridge (raw frames from Android's camera HAL into AVFoundation, aim
  4K, no Deep Fusion). Reverse-engineering the iPhone ANE/ISP "dump" for computational photography = someday R&D,
  explicitly not now.

## 4. Decisions made (so they stop being re-litigated)

- Next session = **take13 combined** (floors-off + 120 Hz). Owner just runs it.
- **No wholesale upstream merge** — cherry-pick BT AIDL + sensor bridge (§P1).
- Clock floors **off by default** (shipped, `native-energy`).
- **Camera:** virtio bridge near-term; ANE reverse-engineering deferred.
- The hackios "leaderboard" is **self-reported** and doesn't track our native path — not a work driver; we're
  ahead of our own listed row.
- `appinst` IPA installer: **parked** (PR #1, unbuilt) until the performance work lands.

## 5. Branches in flight

| Branch | What | State |
|---|---|---|
| `native-frame-latency` | panel forced to 120 Hz in takeover | PC-tested, needs take13 |
| `native-energy` | clock floors off by default; GPU-poll plan in docs | PC-tested, needs take13 |
| `native-vm-bridges` | battery / haptics / Wi-Fi / power-key | in progress |
| `appinst-ipa-installer` | on-device IPA installer | parked (PR #1) |

## 6. The only two things that need the owner

1. **Run take13** (physical device, you watch it): the combined floors-off + 120 Hz session.
2. **Green-light the upstream plan** (§P1: cherry-pick, not merge) — or override it.

Everything else is mine to drive.
