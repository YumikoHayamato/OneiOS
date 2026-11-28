# runtime/bridges: the host side of OneiOS's hardware

Owner: the bridges agent. What each device is, on the phone and for iOS: `docs/hardware.md`. The kernel side:
`drivers/`.

```sh
make test          # host unit tests (ASan, UBSan); no phone, no Apple files
make               # build/android/libonebridges.a, oneios-audio, bridgectl, wifictl   (NDK r28c)
tests/host.sh      # both; CI runs it (Linux only: on a Mac, runtime/host-tests.sh runs it in Apple's container)
```

| Folder | What |
|---|---|
| `include/bridges/bridges.h` | every bridge's C API, with no dependency on xnuhost |
| `include/bridges/devices.h` | the virtio devices on xnuhost's transport (`runtime/xnuhost/src/xnuhost.h`) |
| `include/bridges/hw_backend.h`, `hw/` | the backend layer: `hw-backend=samsung\|treble`, `hw-backend-<bridge>`, `hw-backend-fallback` (`hw_backend.c`, pure: the settings, the choice; `hw_log.c`: the `hw: <bridge>: backend ...` and `... treble: missing: ...` lines); `test_hw_backend.c` (in `make test`). `docs/hardware.md` 7e: the audit table |
| `hal/` | the treble backend's HAL client (`hal.c`: a HAL by name from servicemanager on `/dev/binder`, its descriptor checked, the calls; the Wi-Fi bridge's `aidl_binder.c` underneath) and the HALs' parcels both ways (`HealthInfo`, `Temperature[]`); `tests/fake_hal.c` (`build/host/fake-hal DIR`: a fake servicemanager with fake health, thermal and vibrator HALs whose values are files of DIR) and `tests/test_hal_binder.c` (`make test-binder`: the treble bridges against it on a real binder driver) |
| `cpu/` | host topology from sysfs, the vCPU plan (E first, then P), thread placement, the cpufreq register window |
| `haptics/` | evdev force feedback (`sec_vibrator_inputff`) or, with `hw-backend-haptics=treble`, the vibrator HAL (`br_haptic_open_as`), a worker thread, the iOS-to-effect mapping, the counters (`haptics: effect N played`, `haptics: counters:`) and the stand-in actuator of xnuhost's `haptics=sim` (a boot with no vibrator) |
| `net/` | the old route, only with xnuhost's `wifi=tap`: tap back end, the built-in DHCP answer, `oneios-net.sh` (routing and NAT, root) |
| `wifi/` | the phone's Wi-Fi for iOS (`docs/hardware.md` section 3): wlan0 is the guest's en0. `frames.c` (which frames cross, coalesced frames cut), `station.c` (the guest driver's station messages, `drivers/net/oneios_wifi_proto.h`), `sim.c` (a made-up radio, xnuhost's `wifi=sim`), `wlan.c` (AF_PACKET, nl80211 scans and signal, wlan0's settings kept and given back; three threads), `supplicant.c` (joins through the vendor's wpa_supplicant), `aidl_parcel.c` + `aidl_binder.c` (stable AIDL over `/dev/binder` without Android libraries) |
| `bt/` | the Bluetooth HAL (HIDL 1.0/1.1) over `/dev/hwbinder` without Android libraries (two threads: one answers the HAL's callbacks, one makes the calls from a queue, so a vCPU never waits on the HAL); packet checks; `br_hci_hidl_probe` for xnuhost's start; `hci_sim.c`: the stand-in controller of `bluetooth=sim` (a boot with no phone): Command Complete with a 5.4 controller's return parameters for every command (Command Status for the ones answered that way), Unknown HCI Command for vendor commands, one made-up device ("OneiOS sim") heard by an LE scan, data dropped; `test_bridges.c` checks its answers |
| `audio/` | the back end xnuhost links (`backend.c`) and the AAudio helper process (`aaudio_helper.c` -> `oneios-audio`) |
| `brightness/` | the panel's backlight (sysfs, the curve, fades, restore), the light sensor `als_rear`, `brightctl` (the phone test); its own `README.md` and `Makefile` |
| `battery/` | the phone's battery for iOS: Android's `power_supply` nodes read every 3 s, normalized, changes sent to the guest; `batteryctl` (the phone test); its own `README.md` and `Makefile` |
| `camera/` | the camera (fable-camera): the helper process `oneios-camera`, the frame ring, xnuhost's end (`libonecamera.a`) and `camctl`; its own `Makefile` and `README.md` |
| `devices/` | `bridge_dev.c` (`oneios,bridge`), `bridge_stream.c` (the same channel on a byte stream, `include/bridges/stream.h`: control, the battery and haptics for a guest that has only a serial port to the phone, which is the native path's VM of crosvm; no xnuhost in it), `virtio_net.c` (`droidvm,net`; a frame of the Wi-Fi station's own type waits up to 50 ms for a guest buffer, any other is dropped when there is none), `virtio_snd.c` (`droidvm,virtio-snd`) |
| `tools/bridgectl.c` | tries each bridge on the phone without xnuhost (root); `bridgectl serve /SOCKET [--wait S] [--no-haptics] [--no-battery]` is the bridge channel's phone end for a VM of crosvm (`oneios-tool phone vm-boot` starts it before crosvm, whose second serial port connects to the socket; `docs/native.md`) |
| `tools/wifictl.c` | the Wi-Fi bridge on the phone without xnuhost (root): `status`, `scan`, `join`, `restore` |
| `tests/` | `test_bridges.c`, `host.sh`; `test_stream.c` (part of `make test`: the frames of `drivers/bridge/oneios_bridge_stream.h`, `bridge_stream.c` against a guest on a socket pair with a counted motor and a made-up battery, and `bridgectl serve` reached over its socket as crosvm reaches it: 65 checks); `test_hidl_binder.c` (`make test-binder`, part of `make test`): the HAL client against a fake hwservicemanager and IBluetoothHci on a real binder driver, a binderfs mounted in a user namespace (the PC kernel's `CONFIG_ANDROID_BINDERFS`; no root; skipped without it). A kernel whose `CONFIG_ANDROID_BINDER_DEVICES` is empty (Ubuntu's; the Mac's container kernel) mounts a binderfs with only `binder-control`: the test asks it for `hwbinder` (`BINDER_CTL_ADD`). The fake HAL retries `BINDER_SET_CONTEXT_MGR` while the last scenario's killed HAL still holds it (`EBUSY`, up to 2 s). On a Mac: `runtime/host-tests.sh`, in Apple's `container` with a kernel built with binderfs ([building.md](../../docs/building.md) section 4); `test_wifi.c` (in `make test`): the Wi-Fi bridge's frames, the nl80211 channel list reader, the DHCP reader (the REQUEST forms of RFC 2131 4.3.2, iOS's and another station's, and their log lines), station, parcels, join plan and callbacks; `test_aidl_binder.c` (`make test-binder`): the supplicant client against a fake service manager and supplicant on the same binderfs |

## Into xnuhost (bridges -> booter)

Link `build/android/libonebridges.a` (static, no library beyond libc) and add `-I runtime/bridges/include
-I drivers/bridge`. Ship `build/android/oneios-audio` as `bin/oneios-audio` (0755), `net/oneios-net.sh` as
`bin/oneios-net.sh` and `build/android/wifictl` as `bin/wifictl` in the runtime package.

```c
#include "bridges/devices.h"

br_log_sink = xnuhost_log;                              // optional: (int level, const char *line)

// CPU, before the device tree is made and the vCPUs start
br_cpu_topology host; br_cpu_plan plan;
br_cpu_read("/sys", &host);
br_cpu_plan_make(&host, config.ncpu, false, &plan);     // machine.cpu_cluster[i] = plan.v[i].type
br_cpufreq *cpufreq = br_cpufreq_create(&plan, tid_of_vcpu);
xh_mmio_add(&(xh_mmio_region){ .base = CPUFREQ_BASE, .size = plan.nvcpu * BR_CPUFREQ_WINDOW, .name = "cpufreq",
                               .opaque = cpufreq, .read = cpufreq_read, .write = cpufreq_write });   // thin wrappers
// in each vCPU thread:            br_cpu_place(0, &plan, index);
// when a vCPU runs an iOS thread: br_cpu_place(tid, &plan, index); br_cpu_request(tid, &plan, index, br_cpufreq_requested(cpufreq, index));

// One window and SPI each, as for xh_virtio_blk_create
br_bridge_config bridge = { .haptic = br_haptic_open(NULL), .hci_factory = br_hci_open_hidl };
br_bridge_create(base0, spi0, &bridge);                 // device tree: /arm-io/bridge, "oneios,bridge"

br_net_config net; br_net_config_defaults(&net);        // set net.guest_mac to en0's address
br_virtio_net_create(base1, spi1, &net, br_net_open_tap);   // /arm-io/net; then run bin/oneios-net.sh up

void *audio_ctx;
const br_audio_backend *audio = br_audio_helper("/data/runtime/bin/oneios-audio", &audio_ctx);
if (!audio) audio = br_audio_null(&audio_ctx);
br_virtio_snd_create(base2, spi2, audio, audio_ctx);    // /arm-io/virtio-snd
```

- Every create returns NULL (with a log line saying why) when its Linux side is missing; a missing device only
  turns that feature off. `br_haptic_open` and `br_hci_open_hidl` returning NULL are fine to pass on.
- Threads: each bridge has its own (haptic worker, tap receiver, binder looper, sound capture). They take the
  device's lock with `xh_virtio_lock`, as `input.c` does. None of them uses signals.
- The network: `br_wlan_prepare("wlan0", session_dir, net.guest_mac)` then `br_virtio_net_create_backend(base, spi,
  &net, the returned open, br_wlan_send)`; `br_wlan_close()` at the stop (the network left, the supplicant as it
  was found, wlan0's two settings back from `session_dir/wifi.state`).
- Stop: on `SIGTERM`, `br_haptic_close()` (stops the motor), `br_wlan_close()` (and `oneios-net.sh down` with
  `wifi=tap`); closing the descriptors does the rest (the kernel erases a closed file's force-feedback effects; the audio helper exits when its socket closes).
- Everything is under `/dev`, `/sys`, `/proc/sys/net` and the session's own files: nothing of One UI's `/data` is
  touched. System state changed: wlan0's GRO and `disable_ipv6` and its IPv4 addresses, and the supplicant's
  networks (`docs/hardware.md` section 3, "Return": what is left after a kill, and why it does not break One UI's
  Wi-Fi). With `wifi=tap` instead: the tap, the policy rules and the two iptables chains, which `down` removes.

## On the phone

```sh
adb push build/android/bridgectl build/android/oneios-audio /data/local/tmp/
adb shell su -c '/data/local/tmp/bridgectl cpu'
adb shell su -c '/data/local/tmp/bridgectl haptic-inspect'      # O_RDONLY: input capabilities
adb shell su -c '/data/local/tmp/bridgectl haptic-hal-inspect'  # AIDL getCapabilities only; no on/off/amplitude
adb shell su -c '/data/local/tmp/bridgectl haptic 200 1000' # ACTIVE: only after device-specific validation
adb shell su -c '/data/local/tmp/bridgectl haptic-pattern'
adb shell su -c '/data/local/tmp/bridgectl bt'            # with Bluetooth off in One UI's settings (see below)
adb shell su -c '/data/local/tmp/bridgectl audio /data/local/tmp/oneios-audio'          # 2 s, then 0.5 s
adb shell su -c '/data/local/tmp/bridgectl audio /data/local/tmp/oneios-audio 12 record' # 12 s, then the microphone
adb shell su -c '/data/local/tmp/bridgectl audio-loop /data/local/tmp/oneios-audio 10'   # 10 clicks: the round trip
```

`bt` takes the controller from One UI's Bluetooth app if that app has it open: turn Bluetooth off first. It prints
`HAL 1.1 registered`, `bluetooth: HAL 1.1 found, initialising ...`, `bluetooth: HAL initialisation complete: the
controller is on (status 0)`, `state 1`, two events `<- type 4, 6 bytes: 0e 04 01 03 0c 00` (Reset) and `<- type 4,
12 bytes: 0e 0a 01 09 10 00 <address, low byte first>` (Read BD_ADDR), `bt: ok: 2 events ...`, then `ok`. The
bridge's own lines (`error: bluetooth: ...`) say which step failed: not registered / find denied, `initialize`
refused, or no `initializationComplete` in 15 s (then look for `avc: denied` lines with `hal_bluetooth` in dmesg). `haptic`
and `audio` are harmless beside One UI.

**SM8850 haptic warning (2026-11-16).** The S26 Ultra reports `sec_vibrator_inputff` at `/dev/input/event7` with `FF_PERIODIC`, `FF_SINE`, `FF_CUSTOM`, `FF_GAIN`. The first actual `bridgectl haptic 80 150` trial selected the correct device, but the worker reported **`EINVAL`**, **0 effects played** and **1 failed**. The old `bridgectl` incorrectly returned `ok` despite failure. **Do not retry stronger vibrations or pattern mode.** A new `haptic-inspect` subcommand opens the input devices `O_RDONLY`, queries the name, supported waveform flags and maximum effect slots, and exits without issuing `FF_GAIN`, `EVIOCSFF` or play events. Worker error logging now distinguishes upload and playback, includes parameters (80 ms/150 maps to period 6 ms, level 72); the CLI waits for the worker and returns failure on any rejected effect. This is **diagnostic hardening, not a hardware haptics fix**. Active S26 haptic waveform support remains blocked pending verified driver behavior. In contrast, the S24 `haptic` commands above are historical references for a previously tested device.

**SM8850 haptics root-cause stage and HAL diagnostic (2026-11-16).** A second physical trial with the instrumented `bridgectl` **definitively** returned `haptics: effect upload failed: Invalid argument (80 ms, intensity 150, period 6 ms, level 72, effect id -1)`, `0 effects played, 1 failed`, and shell exit code **1**. The failing operation is **`EVIOCSFF` for `FF_PERIODIC/FF_SINE`**, before any play event was submitted. Samsung's Android path in a nearby kernel `dmesg` capture uploaded `FF_CUSTOM` with `custom_len=2` and played effect ID 0 successfully; this is evidence of a working vendor haptic path, **not** a validated `FF_CUSTOM` payload for OneiOS. Do not replay the logged internal firmware trigger indices or make an automatic waveform fallback. The S26 vendor's SINE contract remains unverified.

The new **`haptic-hal-inspect`** checks the alternative stock **`android.hardware.vibrator.IVibrator/default`** AIDL path using `br_hal_open()`, interface version and `getCapabilities()` only, then closes the binder handle **without `off()`**. It issues no vibration, gain, stop, or evdev calls, and returns failure if the service is absent or inaccessible. It does **not** establish that the HAL can safely play guest vibrations while One UI still owns it. Stand-in binder tests verify no `on/off/setAmplitude` calls. Avoid running the active `treble` backend until separately validated.

**S26 physical Vibrator HAL playback PASS (2026-11-16), intensity calibration pending.** After the read-only AIDL v3 `getCapabilities()` probe passed with mask `0x3c`, the owner explicitly ran `ONEIOS_HW_BACKEND_HAPTICS=treble ONEIOS_HW_FALLBACK=0 bridgectl haptic 80 150` while One UI was active. HAL `on(80)` and `setAmplitude` succeeded; the CLI reported `1 effects played, 0 failed`, `ok` and exit 0. The owner **felt a nearly imperceptible vibration**. This verifies **basic active HAL playback**, not a useful iOS haptic strength/shape. The S24-derived intensity map converts 150/1000 to **72/255 ≈ 28% requested amplitude**, and 80 ms is a very brief pulse. A carefully observed next candidate is **200 ms, intensity 400** (126/255 ≈ 49% requested amplitude), only once on an idle phone, with the same explicitly selected `treble` and `ONEIOS_HW_FALLBACK=0`. No auto fallback to Samsung evdev, `FF_CUSTOM` guessing, `haptic-pattern`, or production default change is justified yet. The HAL `CAP_COMPOSE_EFFECTS` flag also suggests a future way to improve crispness, but supported primitives and guest experience remain untested.

**S26 Samsung settings and native-primitive inventory (2026-11-16).** `dumpsys vibrator` is **not** the framework service name on this Android 17 device; use `adb shell dumpsys vibrator_manager` instead. Read-only Samsung settings showed `haptic_feedback_enabled=1`, `keyboard_vibration_enabled=1`, `SEM_VIBRATION_FORCE_TOUCH_INTENSITY=4`, `SEM_VIBRATION_NOTIFICATION_INTENSITY=5`, `media_vibration_intensity=5`. These values are *not* physical vibration amplitude percentages and do not explain the two weak HAL `on(ms)` pulses by themselves.

The new **`bridgectl haptic-hal-primitives`** queries `getCapabilities`, composition size/delay maxima, `getSupportedPrimitives`, `getPrimitiveDuration` and `getSupportedEffects` over AIDL, without sending **any actuation or motor-setting calls**. Supported ID names come from AOSP `CompositePrimitive`; actual Samsung support must be read from the device. When `CAP_COMPOSE_EFFECTS` is absent it reports no composition support and exits. Fake HAL tests exercise a short list of primitive IDs, durations and preset effects and verify that no `vibrator.log` entries were produced. This command is not a `compose()` test and does not enable external control. Avoid active effects until the physical inventory and framework state are reviewed.

**Optional SM8850 native impact bridge (guest protocol, experimental; disabled by default).** The real S26 physically accepted one `perform(HEAVY_CLICK, STRONG)` with a clear ~110 ms tick, while the old duration-based HAL route was too weak. The host-side `haptic.c` worker now also supports an **explicit** `OB_HAPTIC_IMPACT` event with a native preset backend; the existing `OB_HAPTIC_VIB` and `OB_HAPTIC_STOP` retain duration/frequency semantics. Enable only if using the explicitly selected `hw-backend-haptics=treble`, host environment `ONEIOS_HAPTIC_NATIVE_IMPACTS=1` and guest boot argument `oneios_native_impacts=1`. The host will advertise `OB_FEAT_HAPTIC_IMPACT` in an extended v1 HELLO **only after** querying the native HAL and confirming TICK, CLICK and HEAVY_CLICK are supported. Guest identifies a single short isolated PCM burst only after 10 ms silence; 30ms+ sustained vibrations retain VIB/STOP, with an experimental initial 30 ms classification delay. Mapping: isolated low peak TICK LIGHT, mid peak CLICK MEDIUM, high peak HEAVY_CLICK STRONG. Only HEAVY has a physically confirmed strong tick. No blind `FF_SINE` fallback or `FF_CUSTOM` uploads. After a preset naturally finishes, reset and close do not call a stale `off()`; an immediate STOP during an active preset still cancels it. Legacy defaults unchanged; no iOS guest boot validation yet.

**Production AIDL native-impact preflight (strictly passive).** The SM8850 CI host lacks binderfs, so fake Binder tests compile but skip on GitHub's runner. The on-phone `bridgectl haptic-native-inspect` is an explicit read-only verification of the *production* `br_haptic_open_as(..., treble, ...)` path, not the standalone HAL getter helper. Run with `ONEIOS_HAPTIC_NATIVE_IMPACTS=1 ONEIOS_HW_FALLBACK=0`. It reports whether the live AIDL HAL supports TICK/CLICK/HEAVY_CLICK, starts no effects, never uses evdev and closes without `off()` when native opt-in was requested and no continuous effect played. It refuses fallback=1. The default non-native close still stops legacy continuous vibration. This validates native feature negotiation before any separately authorized guest integration trial.

**Native impact telemetry correction (2026-11-16).** The host's `haptics: counters`
now counts native impacts in total guest requests (`vib + stop + impacts`),
while still reporting discrete impacts separately. Guest-side counters include
impacts sent and the first 24 successfully sent impact kinds are logged.
This is an observability fix and test hardening, **not** guest E2E validation.
Physical guest playback still requires a separately authorized phone session.
**PCM-to-VirtIO host regression (2026-11-16).** The bridge tests now feed a
stand-in guest's Actuator PCM periods through the *production* `oa_period`
classifier, send the resulting impact or duration events over the real
`xh_virtio` guest/host queues, and verify the fake HAL's callbacks.
Cases cover isolated heavy/light/medium bursts, legacy operation without
guest opt-in, sustained VIB/STOP, malformed impact frames, and active-impact
cancellation on guest reset. The guest's `oneios_haptic_impact()` also
requires `oh_native_impacts`, so the host's advertised feature alone
cannot bypass the guest boot opt-in. These are deterministic host tests;
they neither access a phone nor prove live iOS/Android actuation.
**Wi-Fi** (`build/android/wifictl`, `bin/wifictl` in the runtime package):

```sh
adb shell su -c '/data/local/tmp/oneios/runtime/bin/wifictl status'    # read only
adb shell su -c '/data/local/tmp/oneios/runtime/bin/wifictl scan'      # read only (one scan, 6 s)
adb shell su -c '/data/local/tmp/oneios/runtime/bin/wifictl join NAME PASSPHRASE'   # takes the Wi-Fi from One UI while it runs
adb shell su -c '/data/local/tmp/oneios/runtime/bin/wifictl restore /data/local/tmp/oneios/session/wifi.state'
```

`status` and `scan` change nothing and are safe beside One UI and beside a session. A pass, as on the phone on
2026-11-12 beside One UI: `wlan0: 8e:f6:ae:01:7e:c9`, `supplicant: service
android.hardware.wifi.supplicant.ISupplicant/default, station interface wlan0, address 8e:f6:ae:01:7e:c9, 0
network(s):`, `signal poll: none (not joined)`, `ok`; and for `scan` one line a network (access point, frequency,
signal, security, name) and `scan: 53 networks`, `ok`. An `error: wifi: supplicant: no ... from the service manager`
line is a lookup refused or a service not registered; `no station interface wlan0` is One UI's Wi-Fi off.
`join` is the whole host path with the tool as the guest: it takes wlan0 as a session does (the supplicant's
networks removed, wlan0's IPv4 addresses removed, GRO and IPv6 off), scans, joins, sends a DHCP discover and prints
the offer (`DHCP: 192.168.x.y offered by ...`), stays 5 s, leaves and gives wlan0 back. Not during a session
(xnuhost's bridge has the supplicant). The passphrase is on the command line: use it on your own phone only.
`restore` puts wlan0's GRO and IPv6 settings back from a killed session's `wifi.state`.

`audio` plays a 440 Hz tone at -30 dBFS (quiet at any media volume) for S seconds, then 0.5 s later a 660 Hz one
for 0.5 s (the helper's stream lingers: no new open), and with `record` takes 2 s of the microphone. A pass, as on
the phone on 2026-11-12 beside One UI (the helper of then): `oneios-audio: speaker open: 48000 Hz, 2 channel(s), none,
sharing shared, mmap 0, burst 1600, buffer 3200 of 4800 frames`, the `speaker stopped by the guest` lines with `0
underruns` and `latency about 130 ms`, `audio: microphone: 92400 frames in 2 s (96000 expected), peak ... RMS -52.0
dBFS` (room noise), `ok`. The helper of 2026-11-13 (not run on the phone yet) says `none asked, none given`,
`buffer 2800 of 4800`, then `the server pulls N frames every N ms`, and its reports a `margin` near 20 ms and, [I]
expected, `latency about 95-105 ms`; `ONEIOS_AUDIO_PERF=low bridgectl audio ...` runs it with performance mode
LOW_LATENCY for a comparison (`docs/hardware.md` section 6, "Latency per stage"). `0 frames` or `all zero` is a silenced microphone; an `open speaker: ...` line is AAudio's refusal
(then `dmesg | grep avc`). The other tools have not been run yet.

Since 2026-11-15 (fix-audio-latency; not run on the phone yet) `audio` first prints the helper's probe, `audio:
probe: speaker latency N frames (N ms): oneios-audio-latency=N in a session`, and the helper opens the speaker
LOW_LATENCY with MMAP exclusive first: its open line says what the phone's HAL gives (`low latency given, sharing
exclusive, mmap 1, burst B, ...; MMAP exclusive: the DSP reads the buffer`, or the reason it fell back: `MMAP not
given`, `MMAP shared ...`). Beside One UI system_server runs, so an MMAP shared answer is kept there (in a session
it is not): `ONEIOS_AUDIO_SYSTEM_SERVER=0 bridgectl audio ...` behaves as a session does. Its reports end
`latency about N ms; end-to-end N ms (...)`; on MMAP exclusive about 10-20 ms is expected [I], was 128-137.
`audio-loop` plays N clicks (20 ms of 1 kHz at -30 dBFS, 0.5 s apart) while the microphone records, and prints
when each was heard after it was handed to the helper (`audio-loop: click K: heard N ms after ...`), the noise and
the threshold (`noise -52 dBFS`: 18 dB above it, at least -56 dBFS), and `audio-loop: round trip (the speaker + the
air + the microphone) median N ms, MIN..MAX, K of N clicks`. The round trip includes the microphone's own path, so it
is an upper bound of the speaker's; the helper's `end-to-end` (stderr) is the speaker's part from AAudio's
timestamps, and the difference is the microphone's and the air's (about 0.5 ms for the phone's speaker to its
microphone). `not heard` for every click: the media volume at 0, or the microphone silenced.
