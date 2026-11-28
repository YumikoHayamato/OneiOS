# The display helper

Puts iOS's frames on the phone's panel during a OneiOS session. xnuhost keeps the guest's frame in a memfd and
serves it on `session/display.sock` (`docs/booter.md` 2.8); One UI's surfaceflinger and composer keep running and
hold the panel. `oneios-display` runs on One UI's side, as root, outside xnuhost's mount namespace: it takes the
frame from the socket and shows it on a surfaceflinger layer above everything else.

**On the phone since the sessions of 2026-11-11** (backboardd's frames on the panel, `README.md` "Status"; the 2026-11-12 23:45 boot logged `1440x3120 buffer, stride 1472, format 5`). The colour handling of 2026-11-13 (dataspaces, half floats) has not run there; its first test is `displayctl test --format rgba16f` below.

What the guest shows comes from its framebuffer driver (`kernel/patches/oneios-hosted/display/`): the kernel
console until `backboardd`'s first frame, then iOS. On the PC, `display-capture.py` in that folder is a client of
`display.sock` in this helper's place: it says when the picture changed and writes it as PNGs.

```sh
make               # build/android: oneios-display, liboneios-display.so, oneios-display.dex, displayctl
make test          # host tests: the socket protocol, the fit, the damage rectangles, the copy, displayctl,
                   # and the choice of the panel's native mode (tests/TestModes.java, on the host's JVM)
```

Needs NDK 28.2, a JDK (`javac`) and `d8` from the Android SDK's build-tools (`ANDROID_SDK_ROOT`, `ANDROID_HOME`,
or the SDK the NDK is in). No `android.jar`: the Java source reaches the framework by reflection only.
`liboneios-display.so` links `-landroid -lnativewindow -lm` (the dataspace calls are libnativewindow's) with `../host.mk`'s `ANDROID_SO_LDFLAGS` (`-Wl,-z,defs`: a symbol that none of them has fails the link), and `runtime/build.sh` checks its NEEDED libraries and undefined symbols against the NDK's API 30 stubs (`runtime/package/elf-check.sh`; `docs/building.md` section 4).

| File | What it is |
|---|---|
| `src/xhd.c`, `xhd.h` | the portable core: `display.sock` client (and the serving side for the tests), hello checks, damage rectangles, letterbox fit, row copy |
| `src/jni.c` | `liboneios-display.so`: the socket, the mapped frame, the copy into the layer's buffers, the stop signals |
| `java/OneiOSDisplay.java` | `oneios-display.dex`: the layers (`SurfaceControl`, `BLASTBufferQueue`), the panel's mode, the placement |
| `src/launcher.c` | `oneios-display`: finds the dex and the library next to itself and execs `app_process` (same pid) |
| `src/displayctl.c` | `displayctl`: the phone test (`test`: stands in for xnuhost with a moving pattern), and the screen and the touch of a VM of crosvm (`pool`, `touch`) |
| `tests/test_display.c` | the host tests |

## How the layer is made, and why

By a dex that `app_process` runs, on the framework's hidden `SurfaceControl` classes: a `SurfaceControl.Builder`
BLAST layer with no parent, a `BLASTBufferQueue` on it, and the queue's `Surface`. This is what the OneOS fork's
`OneOSBootStatus` does for its entry and return screens (`switcher/patches/OneOSBootStatus.java`), which draw
while the framework is down, so it is the one way already shown to work with surfaceflinger alone on these
phones. `app_process` starts its own runtime; no zygote and no system_server are needed, only surfaceflinger and
servicemanager.

Not native libgui through `dlopen`: `SurfaceComposerClient` and `Surface` are C++ with mangled, unversioned
symbols and class layouts that change between Android releases (this phone is on Android 17), and there is no NDK
call that makes a parentless layer (`ASurfaceControl_create` needs a parent). The Java classes are the same
library underneath with a surface that changes far less; where a method was renamed, the dex falls back.

The frame's path is native all the same. The dex only makes and places the layers; the copy goes through the
NDK's `ANativeWindow` on that `Surface`, from the mapped memfd straight into the layer's gralloc buffer.

## What it does

- **Protocol.** Connects to `display.sock` (waiting for it to appear, `--wait`), works out the panel (below), sends
  `panel {"XHDP", width, height, hz, 0}`, the mode the layers will be placed in, from which xnuhost sizes the guest's
  frame in a session (`docs/booter.md` 2.8), then takes the hello and the memfd and
  maps the frame read-only. The session folder is writable by the guest, so nothing is trusted: the path must be
  a socket (not a link), the sizes are bounded, the stride must cover a row, the frame must fit in the memfd,
  flush rectangles are clipped, unknown messages are ignored.
- **Layers.** The frame's layer has buffers of the guest's size. The guest renders the panel's native mode
  (1440x3120), so in a session the layer covers the panel pixel for pixel: no matrix, no bars, no background
  layer (the log says `the frame is the panel's size: shown 1:1, not scaled`). A frame of another size (a
  mismatch: an override on xnuhost's side, or a panel that stayed in a lower mode) is scaled by the compositor
  to the largest rectangle of its aspect, centred, over a black layer that fills the bars (1320x2868 on
  1440x3120: 1436x3120 at x = 2). Both layers are at the top of the built-in display's stack.
- **The panel.** The modes come from surfaceflinger (`DisplayControl`, the way scrcpy reads them): the active
  one, and the native one, which is the largest the panel has at its highest refresh rate. With `--takeover` (a
  session: system_server is gone) the panel is put in the native mode: the display's desired mode specs are read,
  their default mode replaced and their refresh ranges (primary and app request, physical and render) opened to
  that mode's rate, and written back (`SurfaceControl.setDesiredDisplayModeSpecs`); the helper waits up to a second
  for the mode to become active. It does so whenever the panel is in another size (FHD+ 1080x2340 is One UI's
  default resolution setting) or surfaceflinger's policy (One UI's last word before its framework stopped) has
  another default mode or caps the rate below the native mode's (`OneiOSDisplay.wantsSpecs`, host-tested). In the
  crosvm mode (`--crosvm-display`) the rate is also **held**: the ranges are that rate alone (120 to 120), set as
  well when the panel runs below it or the policy lets it fall below it. Until 2026-11-25 only another size did: with power saving on (or the
  standard motion setting) One UI's policy is 10 to 60 Hz, the layer's vote for 120 Hz counts for nothing against
  it, and the panel showed at most 60 of the 100 frames a second the guest drew for a 120 Hz tick (docs/native.md
  "The frame rate in the shared-RAM VM"; inferred from surfaceflinger's policy after the session). Held, not only
  allowed, where crosvm presents: with the framework gone nothing tells surfaceflinger of a touch, and its idle
  timer (250 ms here) would lower the rate between gestures. The session's helper (xnuhost's frames) keeps ranges
  from 0, as before. The specs as
  they were are kept and given back when the helper leaves (its shutdown hook in the crosvm mode, the end of the
  run otherwise: `surfaceflinger's policy is One UI's again: ...`); a helper killed with SIGKILL leaves the 120 Hz
  specs until One UI's framework sets its own at its start (not tested).
  Then the display's projection is set to the active mode, unrotated, so the layer space is the panel's own
  pixels whatever rotation One UI left. If the mode is refused the panel stays as it was and the frame is
  scaled (`the panel shows N frames a second, not the M the guest is told` when the rate was not taken). Without
  `--takeover` nothing of the display is changed and the logical size comes from the display manager. The layer
  votes for the mode's highest refresh rate (`setFrameRate`, 120 Hz on this panel), which is the rate the guest's
  vsync tick runs at.
  The lines: `panel modes: active 1440x3120 at 60 Hz (mode 2; up to 120 Hz at that size), native 1440x3120 at 120
  Hz; surfaceflinger's policy: default mode 2, the panel at 10 to 60 Hz` (the active mode's own rate; the old line
  gave only the size's highest), `asked surfaceflinger for the native mode (id 0) held at 120 Hz` (`..., up to 120 Hz` in the session's helper), `the panel is in
  its native mode 1440x3120 at 120 Hz (mode 0); surfaceflinger's policy: default mode 0, the panel at 120 to 120 Hz`.
  In the crosvm mode (`--crosvm-display`) the helper also looks at the panel's rate every 5 s (one call to
  surfaceflinger): `the panel runs at 120 Hz (mode 0, 1440x3120); the guest draws for 120 Hz` when it changes
  (`: at most 60 of its frames a second are shown` when it is below `--hz`), and at the end `the panel's rate over
  the session: 120 Hz 100 % (N looks, 5 s apart)`.
  Whether One UI's resolution setting is a panel mode or only a logical size on this phone is not known; the
  helper's `panel modes: active ..., native ...` line of the first run says.
- **Frames.** One copy per frame, of the damaged rectangle only: the window copies back what the last buffer had
  and hands out the rectangle to draw. The layer's buffers are in the guest frame's own layout, so the copy is a
  `memcpy`: an 8-bit frame (hello format 1) goes into `BGRA_8888` buffers (format 5; the layer is marked opaque), a
  half-float frame (format 2, xnuhost's `display-format=rgba16f`) into `RGBA_FP16` buffers (format 22). If the phone
  gives no `BGRA_8888` buffer the helper falls back to `R,G,B,X` and swaps while it copies; with no `RGBA_FP16` one
  it falls back to `BGRA_8888` and encodes the half floats to sRGB, clamped (what is outside sRGB is lost). Pacing is
  the buffer queue's: the next buffer is handed out when surfaceflinger has taken the last, so the loop runs at the
  panel's vsync and every flush that came meanwhile is one post. **The copy loop's thread runs at uclamp.min 1024**
  (2026-11-19, `jni.c` `copy_loop_clamp`: sched_setattr on itself before the loop, KEEP_POLICY and KEEP_PARAMS, so
  SCHED_OTHER and its nice stay; no realtime thread): the phone's CPU floors are the governor's in a session (the
  fork's switch writes `cpuinfo_min_freq`), and the copy (about 1 ms for 1080x2340 at the top clock) must not wait for
  its ramp; the clamp counts only while the thread runs, not while it polls. Line: `oneios-display: the copy loop at
  uclamp.min 1024 while it runs` (or `... keeps the governor's clock: uclamp.min not set (ERROR)`). Host tests: 133
  and 10 checks, 0 failed (the clamp is set or refused on the host; nothing checks the clock). Not yet run on the phone.
- **The first post waits for `session/ready` (since 2026-11-15).** With `--session DIR` the helper posts nothing
  until `DIR/ready` exists (`xhd_hold_first`): xnuhost makes it at the guest's first frame of its own that is not
  all black, or `ready-content-wait` seconds (120) after its first black one (`docs/booter.md` 2.8, "the ready
  rule"). Until then the layer is up but has no buffer, so the OneOS boot screen under it stays on the panel. The
  flushes that come meanwhile add up to the damage (the first post is the whole frame anyway); the helper looks for
  the file at every flush and every 250 ms between them, and only until its first post. Before this the helper
  posted the empty memfd (black) at once, a second after `layer up`, then the console's and backboardd's black
  frames: its opaque layer covered the boot screen and the fork ended the boot screen at that `first frame posted`,
  and the panel was black until iOS's Apple logo (phone, main 3233fed, 2026-11-15). Lines: `the first frame waits
  for DIR/ready (xnuhost's first frame of the guest's own with content); the boot screen stays`, then `first frame
  posted (...; held T s for session/ready, N wakes for flushes not shown)`. A helper started again in a session that is ready
  (`oneios-tool phone display-restart`) posts at once. `--socket PATH` (and `displayctl` after the SM8850 diagnostic fix) never waits.
  The production session's first-frame hold is covered by `test_hold_first`, while the standalone test's
  socket selection is checked by `test_displayctl`. The bars' black background layer (only for a frame that is not
  the panel's size) is still drawn before the hold: in a session the frame is the panel's size, so there is none.
- **Colour.** The buffers are tagged with a dataspace (`ANativeWindow_setBuffersDataSpace`, libnativewindow's: the
  library links `-lnativewindow` and `-Wl,-z,defs`; without the first, `1f7dd1f`'s library could not be loaded on the
  phone, 2026-11-14: `cannot locate symbol "ANativeWindow_getBuffersDataSpace"`, and the panel stayed on the OneOS
  splash while iOS drew), which says what their
  numbers mean, so that surfaceflinger turns them into the panel's colours: `SRGB` (`0x8810000`) for an 8-bit frame
  (iOS composites this display in sRGB: QuartzCore's `IOMFBDisplay::compositing_color_space` is `kCGColorSpaceSRGB`,
  `kernel/patches/oneios-hosted/display/README.md` "What iOS renders into"), `SCRGB_LINEAR` (`0x18410000`) for half
  floats (linear light, sRGB primaries, extended range: Display P3's colours are components below 0 and above 1). Before
  2026-11-13 the buffers had no dataspace (`UNKNOWN`: surfaceflinger's legacy handling). `--dataspace` overrides it for
  a test (`auto`, `unknown`, `srgb`, `p3`, `scrgb`, `scrgb-linear`). The helper sets no colour mode, no brightness
  and no HDR ratio of its own; a wide dataspace on screen may make surfaceflinger pick another colour mode of the
  panel while the layer is shown (its own choice, undone when the layer goes; the return restarts surfaceflinger).
  It logs what it found and what it set: `panel colour mode N (of ...); HDR types [...], luminance max ... nits, max
  average ..., min ...` (read only), `buffers tagged srgb (dataspace 0x8810000)`, and in `first frame posted` the
  buffer's format and dataspace and the guest frame's pixels.
- **Leaving.** On SIGTERM, SIGINT or SIGHUP, and when the socket closes (xnuhost ended, or a newer helper
  connected), it removes its layers and exits 0. Killed with SIGKILL, surfaceflinger drops a dead client's layers.

```
oneios-display (--session DIR | --socket PATH) [--takeover] [--wait SECONDS] [--logical WxH]
               [--hz N] [--format bgra|rgbx] [--dataspace auto|unknown|srgb|p3|scrgb|scrgb-linear] [--no-projection]
```

`--format` chooses among the 8-bit layouts only; a half-float frame always gets `RGBA_FP16` buffers first.

Exit codes: 0 the session ended or it was asked to stop, 2 usage or a missing file, 3 no socket or no usable
hello, 4 the layer's buffers failed, 5 an exception (the framework's classes changed; the trace is on stderr).

It grabs no input device, uses no realtime thread, writes nothing under `/sys/power`, and changes nothing that
outlives surfaceflinger.

## In a session

The OneOS fork (`oneios` branch) starts it from `oneios_switch.sh` once xnuhost runs, from the runtime the
preflight hashed: `runtime/bin/oneios-display --session <stage>/session --takeover --wait 60`, in a session of
its own, in the cgroup `oneios_session`, pid in `control/display.pid`, output in `control/display.log` (the
guest cannot write `control/`). It is not needed for the session to run: with a runtime that has no helper, or
a helper that fails, the session goes on with a dark screen and the way back works as before.
`oneios_return.sh` ends it after xnuhost (SIGTERM, one second, SIGKILL) before it restarts the composer and
surfaceflinger; the watcher kills it before it starts the return; the cgroup kill gets it in any case.

In the runtime package: `bin/oneios-display`, `bin/liboneios-display.so`, `lib/oneios-display.dex`.

## The phone test: `displayctl`

With One UI running, as root. `displayctl` makes a session folder of its own under `/data/local/tmp`, serves a
1440x3120 frame the way xnuhost does, starts the helper on it and draws for 5 s: eight colour bars, darker at the
top, inside a white frame two pixels wide, with a white square that bounces. Then it closes the socket; the
helper removes its layer and One UI is back. Nothing of the display's state is changed (no `--takeover`). The `displayctl test` command **explicitly rejects** forwarding `--takeover` to the helper, even after `--`; regression tests enforce this, so a diagnostic cannot accidentally select the session-only mode. It still displays a transient top layer: close any sensitive app content before testing and be prepared to restart the display helper if it misbehaves.

```sh
make -C runtime/display
adb shell 'mkdir -p /data/local/tmp/oneios-display-test'
adb push runtime/display/build/android/oneios-display runtime/display/build/android/liboneios-display.so \
         runtime/display/build/android/oneios-display.dex runtime/display/build/android/displayctl \
         /data/local/tmp/oneios-display-test/
adb shell su -c 'chmod 0755 /data/local/tmp/oneios-display-test/oneios-display /data/local/tmp/oneios-display-test/displayctl'
adb shell su -c '/data/local/tmp/oneios-display-test/displayctl test'
```

What to see: the pattern over everything (status bar and navigation bar included), the white frame whole on all
four sides of the picture, at the very edges of the screen when One UI's resolution setting is WQHD+ (with FHD+
or HD+ the test, which changes nothing of the display, shows the frame scaled to that size), the square moving smoothly, colours in this
order from the left: white, yellow, cyan, green, magenta, red, blue, dark grey. Red and blue swapped means the
buffer format is not what the copy assumed.

**SM8850 first physical diagnostic and fix (2026-11-16):** The initial `displayctl test --seconds 5` connected and detected the S26 Ultra panel (1440x3120/120 Hz) and SurfaceControl layer, but the helper logged `the first frame waits for .../displayctl.*/ready` and `the socket closed after 0 frames`, despite the test sending 482 flushes. Root cause: `displayctl` launched the helper with `--session DIR`, so the helper awaited the **real XNU boot's** `session/ready` marker, which a standalone display test never creates. The test now launches with `--socket DIR/display.sock`: the helper's existing diagnostic mode has **no ready hold**. This does not change how an actual XNU session uses `--session`, and does not allow `--takeover`. The CLI now allowlists forwarded diagnostic-only options, preventing `--session` and `--socket` from overriding the safe endpoint.

What to send back: the command's whole output **and** a visual confirmation that the bars/square actually appeared. `displayctl: ok` only means the command connected and the helper exited cleanly: **it is not alone proof that any frame reached the display**. The acceptance log must include `oneios-display: first frame posted` and a subsequent `oneios-display: the socket closed after N frames` with `N > 0`. The helper's lines also report the panel resolution, buffer format and actual layer geometry.

Variants:

```sh
# longer, or another frame size (a mismatch: scaled, with bars)
adb shell su -c '/data/local/tmp/oneios-display-test/displayctl test --seconds 20 --size 1320x2868'
# options after -- go to the helper: the swapping copy, a fixed refresh rate, another dataspace
adb shell su -c '/data/local/tmp/oneios-display-test/displayctl test -- --format rgbx'
adb shell su -c '/data/local/tmp/oneios-display-test/displayctl test -- --hz 60'
adb shell su -c '/data/local/tmp/oneios-display-test/displayctl test -- --dataspace unknown'
# wide colour: a half-float frame (what xnuhost's display-format=rgba16f serves), see below
adb shell su -c '/data/local/tmp/oneios-display-test/displayctl test --seconds 20 --format rgba16f'
# what surfaceflinger has while it runs (second shell)
adb shell su -c 'dumpsys SurfaceFlinger --list' | grep OneiOS
# clean up
adb shell su -c 'rm -rf /data/local/tmp/oneios-display-test'
```

If the helper hangs, `displayctl` ends it 3 s after the test; if `displayctl` itself is killed,
`adb shell su -c 'pkill -f OneiOSDisplay'` removes the layer.

**SM8850 physical result, 2026-11-16:** The owner confirmed the BGRX moving-square flicker was eliminated by one union damage flush per animation step in the new `displayctl`. A second on-phone test with `displayctl test --seconds 5 --format rgba16f` verified native Android `format 22 RGBA_FP16` buffer posting (actual stride 1472 pixels), `scrgb-linear` dataspace `0x18410000`, **459 posted buffers** and **496 diagnostic animation steps in 5.00s**, with clean layer removal and `displayctl: ok`. No fallback was logged. The result proves the **format and SurfaceFlinger presentation path**, not observable P3 gamut differences, HDR headroom, a stable 120 application FPS, or absence of FP16 flicker: the user has not yet described those visual aspects.

**Wide colour (`--format rgba16f`).** The same bars in half floats, and in the lowest quarter of the screen eight
patches from the left: sRGB's red, Display P3's red, sRGB's green, P3's green, sRGB's blue, P3's blue (P3's as linear
extended sRGB: components below 0 and above 1), white at 1.0, white at 2.0. What to look for:
- the helper's lines `buffers tagged scrgb-linear (dataspace 0x18410000)` and `first frame posted (... format 22
  RGBA_FP16, dataspace 0x18410000 scrgb-linear ...)`. `no buffer in format 22 RGBA_FP16 (...), using BGRA_8888` means
  this phone's gralloc or surfaceflinger refused half floats: then the band shows each pair the same.
- each P3 patch visibly more saturated than the sRGB patch on its left (red and green most; blue barely). The same
  colour in a pair means the layer was clipped to sRGB. Compare in One UI's Settings > Display > Screen mode both
  "Natural" (colour-managed) and "Vivid" (Samsung stretches every layer to the panel's gamut; say which mode the
  phone was in). A photo of the band and the command's output are what to send back.
- the 2.0 white the same as the 1.0 white is expected: the helper asks for no HDR headroom (no
  `setExtendedRangeBrightness`), so surfaceflinger shows extended range at SDR white.
- the line `panel colour mode ...; HDR types ..., luminance max ...`: the panel's own numbers (what the guest's EDR
  headroom and nits should be set from, `drivers/brightness`).
- after it, One UI's colours, brightness and resolution as before (nothing of the display was changed).

## The screen of a VM of crosvm: `displayctl pool`

The native path (`docs/native.md`, the display) boots the native kernel in a protected VM of Android's own crosvm. Such
a VM's memory is lent to the guest, and the host faults on an access to it, all but crosvm's restricted DMA pool:
`xnuloader-crosvm --display WxH` puts the boot video at the pool's start (B,G,R,X, `W * 4` bytes a row) and a page for
the guest's frame count after it, and the kernel's display driver copies every frame into the boot video (mapped
write-back in the native kernel) and then counts it (`OneiOSDisplay.cpp` `oneios_display_frame_done`). `oneios-tool
phone vm-boot` starts the bridge beside crosvm:

```sh
displayctl pool --pid CROSVM --pool-offset N --pool-bytes N --size 1440x3120 --stride 5760 --seq-offset N [--fb-offset 0]
                [--vm protected|shared] [--seconds N] [--helper PATH] [-- helper options]
```

It finds crosvm's own mapping of the pool in `/proc/PID/maps` (a shared mapping of `/memfd:crosvm_guest` at the
pool's offset, exactly the pool's size: anything else is refused; with `--vm shared`, for Droid-VM's shared-RAM VM
of `xnuloader-crosvm --shared-ram` whose whole RAM the host may read, any shared mapping of it that holds the devices'
region, which is then part of crosvm's mapping of the guest's RAM), opens the memfd through `/proc/PID/map_files`
(checked: the same inode, long enough), and maps **only** the frame and the count's page, read-only. It serves a frame
of its own (a memfd) to the helper as `test` does (the same session folder, the same refusal of `--takeover` and the
other disruptive options). While the guest has not counted a frame yet (the kernel's console during boot) it compares
the frame four times a second, its cache lines cleaned and invalidated first (the console writes uncached); once the
count is there, it copies when the count moves, and every 2 s as well. Only rows whose pages are resident are read
(`mincore`: no read makes the host allocate the VM's memory) and only rows that changed are written and flushed (one
band), each after the helper's ack of the last. It polls the count every 2 ms while frames come and every 16 ms when the
screen is still. It ends when crosvm's process is gone (or a zombie), at `--seconds`, on SIGTERM or SIGINT; the layer
goes with the socket. `--test-guest FILE` is for the host tests only: a plain file stands for the memfd (never one
under `/proc` or `/dev`). The host tests (`make test`) check the maps lookup on crosvm's lines from the S25, the
row copy with a page that is not resident, the count, a whole run against the stand-in helper with a moving count,
and the refusals.

## Write-only buffers: `ONEIOS_DISPLAY_PRESENT=ahb` (2026-11-23)

The window path's buffers are the CPU's cached ones (`ANativeWindow_lock` is Surface::lock, which asks for
SW_READ_OFTEN and SW_WRITE_OFTEN): on the S25 each lock and each post cleaned and invalidated the CPU caches over the
whole buffer, whatever the damage (simpleperf at 1080x2340, 60 frames a second: `dcache_inval_poc` 36%,
`dcache_clean_poc` 31%, `arch_sync_dma_for_cpu` 9%, `arch_sync_dma_for_device` 9%, the copy 10%; the lock 13.7 ms a
frame with its wait). With `ONEIOS_DISPLAY_PRESENT=ahb` in its environment the helper posts its frames otherwise:
OneiOSDisplay.java makes a child of the layer ("OneiOS frame", `setParent`), the native side takes it
(`ASurfaceControl_fromJava`, looked up: Android 14's, the library is built for API 30), and three AHardwareBuffers of
the frame's size that the CPU only writes (`CPU_WRITE_RARELY | CPU_READ_NEVER | GPU_SAMPLED_IMAGE | COMPOSER_OVERLAY`,
B,G,R,A, else R,G,B,X) go to it with ASurfaceTransaction (`setBuffer`, `setDamageRegion`; the first one shown, opaque,
with the frame's dataspace). Each buffer gets the union of the damage of the frames since it was last filled; at most
two transactions are in flight, and a buffer is filled again after the release fence its replacement's completion gave.
The layer's matrix and position (set by the Java side) place the child. When anything of it is missing the helper says
so and keeps the window path. Measured (`displayctl test`, the same frame): 114 frames a second at 17.1% of a core
against 60 at 49.5% on the window path. `displayctl pool` starts its helper with it, and with
`ONEIOS_DISPLAY_UCLAMP_MIN=0` (the copy loop's floor: 1024 by default, 0 none); the hosted session keeps the window
path and its floor.

## The touch of a VM of crosvm: `displayctl touch`

```sh
displayctl touch --evdev /dev/input/eventN --socket PATH [--wait S] [--swipe-at S [--swipe-every S]]
                 [--boost PATH=VALUE,... --boost-state FILE] [--scan-time]
```

crosvm's multi-touch device (`--input multi-touch[path=PATH,width=W,height=H,name=N]`) connects to PATH and reads
`virtio_input_event`'s from it (type, code, value: 8 bytes, little-endian). This listens on PATH, checks the event device
(a direct multi-touch screen: INPUT_PROP_DIRECT, the MT slot and positions; none of the side keys: KEY_POWER,
KEY_VOLUMEUP, KEY_VOLUMEDOWN refuse it, those stay Android's), and once crosvm has connected (within `--wait`, 60 s) takes
the device (EVIOCGRAB: One UI gets no touches) and passes each frame on: SYN_REPORT, BTN_TOUCH, ABS_X/Y, the MT slot,
tracking id, positions and contact size, as they are (`xhd_touch_event`); a frame the kernel's buffer dropped
(SYN_DROPPED) is skipped up to its SYN_REPORT. It ends when crosvm's socket closes, on SIGTERM or SIGINT, and the device
is Android's again (closing the device lets it go even when the forwarder is killed). AVF's crosvm cannot take the S25's
touchscreen itself (`--evdev` asks for the serial name, EVIOCGUNIQ, which `sec_touchscreen` has none of). `oneios-tool
phone vm-boot` starts it before crosvm with the touchscreen it found by name; the host tests check the event filter and
the refusals of a path that is not an event device's. `--swipe-at S` (`vm-boot --swipe-test`): S seconds after crosvm
connected, and every `--swipe-every` seconds after, the forwarder sends a swipe of its own, from the middle of the bottom
edge up to the middle of the panel in 20 steps of 16 ms, then the lift: a test without a finger (boot 4b, 2026-11-23:
the lock screen followed it up, and the next one opened the Home screen).

`--boost PATH=VALUE,...` with `--boost-state FILE` (`vm-boot --touch-boost`, `src/boost.h`): a touch frame raises the lowest
clocks PATH (devfreq's `min_freq`, cpufreq's `scaling_min_freq`) to VALUE; they are put back 1.5 s after the last frame (a
finger held still long enough to send none lets go too). A process of its own writes them, told by a pipe, so a touch never
waits for a clock; the pipe's end, whatever ended the forwarder, puts them back. FILE (the originals) is there only while
something is raised: the forwarder uses it as soon as it sees that process ended (it looks at every turn of its loop, at
most 500 ms apart), boot.sh when both were killed. A refused put-back (a cluster's cores offline) is tried again every
second. A FILE of this boot that is there at the start is taken over: its values are the originals. The original is the lowest
value seen while nothing was raised, and the first 30 s only read: Samsung holds minimums of its own around the start. A
clock already at VALUE or above is left alone. Host tests: raise, hold, release, the pipe's end, both kills, the refusals, the stuck clocks.

`--scan-time` (`vm-boot --touch-time`, `src/scantime.h`): each frame goes with the time the touchscreen's driver stamped
it with. The guest polls its virtio-input every 4 ms and the S25's touchscreen sends no `MSC_TIMESTAMP`, so iOS got
every touch stamped 0 to 4 ms and the path's jitter late, each frame differently; a finger's speed times that error is
how far content that follows it was off. The forwarder asks the device for CLOCK_MONOTONIC event times
(`EVIOCSCLOCKID`: without it, said once, the frames go as before) and puts `EV_MSC / MSC_TIMESTAMP` before each frame's
`SYN_REPORT`: the low 32 bits of the frame's time in ticks of the counter the guest's clock reads (xnuhost's
`scan_ticks`; `CNTVCT_EL0` and `CNTFRQ_EL0` read here). A time that cannot be the scan's (over 1 s old, or ahead of now)
goes without one. The guest's kernel has taken such a time since it has had the touch (`kernel/patches/oneios-hosted/
hid/README.md`, "Latency"). At its end the forwarder says how many frames went with it and how long after its scan it
had each (least, mean, most). `--test-evdev` (the host tests): `--evdev` names a FIFO of the tests' own in the
touchscreen's place; nothing is asked of it, taken or set. Host tests: the arithmetic and the frame, then the forwarder
with a FIFO for the touchscreen and the test program for crosvm, with and without the flag (the events as crosvm gets
them, two frames of one read each with its own time, a time too old and one from another clock passed on bare).
`make test-arm64` runs the same tests on the phone's own static binary under `qemu-aarch64` (the counter is the
emulator's there: the events and their spacing are checked).

## Not done

- The dataspaces, `RGBA_FP16` buffers and the half-float path (2026-11-13) are host-tested only (`make test`: 110 +
  10 checks): whether this phone's surfaceflinger takes an `RGBA_FP16` BLAST layer, and how One UI's screen modes
  show `SRGB` and `SCRGB_LINEAR` layers, is the wide-colour test above. No HDR headroom is asked for.
- Not every framework call has been seen working on Android 17 (the native-mode request and the projection, `--takeover`, have no log of a run here): the hidden methods used are all
  in `java/OneiOSDisplay.java`, and each has a fallback or a logged failure.
- The copy is not zero-copy. For that xnuhost would have to keep the guest's frame in a gralloc buffer
  (`AHardwareBuffer`) instead of a memfd and pass that; `virtio_gpu.c` and the socket's hello would change.
- The native-mode request (`setDesiredDisplayModeSpecs` by reflection) at another size is unproven like the rest; a
  refusal is logged and the frame is scaled. The request at the native size (the rate alone, 2026-11-25) is
  host-tested only (`make test`: the choice, 21 checks; the calls to surfaceflinger -- reading the policy, giving the
  specs back, the look at the rate every 5 s -- have run nowhere yet); whether Samsung's surfaceflinger takes a 120 Hz policy
  from the helper while power saving is on is the next phone session's `the panel is in its native mode ... at 120
  Hz` and `the panel's rate over the session` lines. At 1440x3120 a full-frame copy is 18 MB, 2.2 GB/s at 120 Hz when every frame
  is all damage.
- A guest that truncates the memfd kills the helper (SIGBUS) and the screen goes dark; the session and the way
  back are not affected.
- The guest can write the frame while a row is copied: tearing is possible for a sender that does not use the
  frame handshake. The handshake (2026-11-19, `xhd.h`): a sender sends `XHDW` after the hello; the helper then
  answers each post with `XHDA` (posts, copied rectangle), and the sender writes the frame again only after the ack.
  `displayctl test` uses it (`N acked by the helper (each step drawn only after the helper copied the last)`; with an
  older helper it falls back to pacing by time after 1 s and says `0 acked ... (no frame handshake ...)`). On the
  S25 Ultra (panel at about 60 Hz) the square flickered without it in both HD+ and WQHD+, and was steady with it
  (483 steps in 8 s, all acked; seen by the owner). **xnuhost does not send `XHDW` yet**: a session is unchanged, and
  iOS's frames can still tear while the helper copies (next: retire the guest's swap only after the ack).
- Rotation: the picture is the guest's portrait frame; the panel is not rotated.

## When the panel goes dark while iOS draws (2026-11-12)

In the 15:35 session the helper printed `the socket closed after 7 frames` and `layer removed` about a minute into
the boot, while xnuhost went on flushing frames: the panel stayed dark until the helper was started again by hand
(the switch script's own command). Neither side said why. Now:
- xnuhost closes the helper's socket only when the helper is gone (`EPIPE`, `ECONNRESET`, `ENOTCONN`) and logs
  `display: the helper's socket is gone (...): closed`. Any other send error (a full queue, `ENOBUFS`, `ENOMEM`)
  drops that one flush and keeps the socket: `display: a flush to the helper failed (...): dropped, the socket
  kept (N so far)`, once per 1000.
- The helper logs why its read ended: `oneios-display: xnuhost closed the socket` or `oneios-display: reading
  xnuhost's socket failed: <error>`.

**To get the picture back during a session: `oneios-tool phone display-restart`** (`tools/oneios-tool/README.md`,
"The session's own commands"; refused without a session). It ends the helper that `control/display.pid` names
(only a pid whose `/proc/PID/cmdline` is a display helper's; SIGTERM, and SIGKILL after 3 s), starts a new one with
the switch script's own command (the same file and arguments, stdin, `control/display.log`, `control/display.pid`,
OOM score 0, the cgroup `oneios_session`, init's mount namespace), and waits for the new helper's `layer up` and
`first frame posted`, which it prints. `--lib PATH` and `--dex PATH` first put a new `liboneios-display.so` or
`oneios-display.dex` into the staged runtime (`runtime/bin/`, `runtime/lib/`; the SHA-256 printed for the local
file and the phone's, refused when they differ; `runtime.sha256` and `runtime.ids` follow, so the next switch's
preflight takes the new files): the way to try a rebuilt helper without ending the session, as after the library
fault of 2026-11-14 (above, "Colour"). xnuhost and iOS are not touched, and the return ends the new helper by the
same pid file. Tested against the stand-in phone only (`tools/oneios-tool/tests/phone/session-cmds.sh`); not run on
the phone yet.

To get the picture back by hand during a session (as root on the phone, the switch script's own command):
`busybox setsid /data/local/tmp/oneios/runtime/bin/oneios-display --session /data/local/tmp/oneios/session --takeover --wait 60 >> /data/local/tmp/oneios/control/display.log 2>&1 & echo $! >> /data/local/tmp/oneios/control/display.pid`
(the pid appended to `control/display.pid`: the fork's boot-screen hand-off and the return find the helper by it; without it the
OneOS boot screen stays up under the helper's layer, as on 2026-11-13, and shows when the helper ends.)

**The OneOS boot screen and this layer (2026-11-14, read in the fork 3876dd5) [F, I].** The fork's renderer
(`OneOSBootStatus`, `control/bootui/renderer.log`) puts its layer at z 2,000,000,000; this helper's is at
2,147,483,646 and opaque, so it is always above. The helper does not re-create or re-place its layer on a display
off or on, a power-key press or a refresh-rate change: nothing in it listens for them, and iOS's display off is a
black (blank) frame on the same layer. So once the helper has posted a frame, the boot screen can only show through
when this layer is gone, i.e. the helper ended or was killed [I: the 2026-11-13 helper had been started late, by
hand, after the libnativewindow fault, and was not in `control/display.pid`, so the hand-off never ended the
renderer]. The fork's feeder ends the renderer once a helper in `control/display.pid` logs `first frame posted`, or
`session/ready` exists and the helper logged `layer up` (OneOS `switcher/assets/oneios_switch.sh` `display_up`); since
OneOS `fix-bootui-handoff` the switch starts each session with an empty `display.log`, so an earlier session's line
cannot end it early.
**The black screen between the boot screen and the Apple logo (phone, main 3233fed, 2026-11-15), fixed here and in
xnuhost [F for the code, not run on the phone].** Before the fix both of the fork's markers came with black
frames: `first frame posted` a second after `layer up` (the helper posted the empty memfd at once, and the frame
stayed black or the console's), and `session/ready` at backboardd's first frame, which is black. Whichever ended the
renderer, this opaque layer had covered it with black, and it stayed black until iOS drew its logo. Now
`session/ready` comes at the guest's first frame with content (`docs/booter.md` 2.8, "the ready rule") and this
helper posts nothing before it (above, "The first post waits for `session/ready`"), so both markers come with the
logo: the boot screen shows until the logo replaces it. The fork needs no change: its markers, its 30 s rule for a
renderer that never drew, and the dead-man timer (600 s without `ready`) are as they were. If iOS only ever draws
black, `ready` comes `ready-content-wait` (120) seconds after its first frame: then the helper posts that black
frame, the boot screen ends, and the panel is black as before. On the phone, read: in `control/display.log` the
`first frame posted (...; held T s for session/ready ...)` line; in `session/xnuhost.log` `display: the guest's first
frame of its own: all black ...` (where the old rule was ready) and `guest ready: first frame with content, T s and N
frames after the guest's first (black) frame`; in `control/bootui.log` the feeder's `the boot screen ends` line at
the same time, and on the glass the boot screen until the Apple logo.

## take15: the power key and the panel

In a takeover `displayctl touch --keys` passes power, volume up and volume down to the VM and nothing else: a short click of
power no longer blanks the panel (take14: it went black by itself; iOS owns the screen). `--power-blanks` (with
`--screen-file`) restores the old behavior (a click toggles the panel through the screen file and holds the touches back
while it is off); `xhd_power_blanks_panel` is the decision and has a unit test. The screen file still works when something
asks for it; nothing in iOS asks for it yet.

## take18: the panel on at a takeover's start

From YMGPwcca/OneiOS 843b06e and f048161 (bounded lock-screen WAKEUP pulses): One UI's keyguard darkens the panel a few
seconds after its last wake, and once One UI's framework is stopped nothing wakes it again (`input keyevent` needs
system_server). Two parts: `vm-boot --takeover` sends one `KEYCODE_WAKEUP` right before the stop while One UI is still up
(`takeover: KEYCODE_WAKEUP sent as One UI stops` in run.txt; `--no-takeover-wake` leaves it out), and the helper's
`--crosvm-display ... --screen-file` (a takeover's) puts the panel on once at its start with POWER_MODE_NORMAL unless the
screen file is there already (`the panel is on (the takeover's start)` in display.txt). Upstream's four pulses 2 s apart
are not copied: here the helper holds the panel itself, so one event is enough. Built (the dex: javac -Werror, d8;
`TestModes` 21 checks); not run on the phone. `vmboot::tests::the_takeover_wakes_the_panel_once_before_one_ui_stops`.

take19, a variable rate (`vm-boot --panel-rate-min HZ`): `--crosvm-display ... --takeover --rate-min HZ --rate-hold-file
FILE` sets the native mode's ranges to [HZ, top] after the layer is up (surfaceflinger then follows the layer's frames and
its 120 Hz vote, and its idle timer lowers the panel on a still screen), and to [top, top] while FILE exists (displayctl
touch's boost.state: with One UI's framework down no touch reaches surfaceflinger), looked at every 10 ms. Lines: `the
panel's rate is free from HZ to TOP Hz, held at TOP while FILE is there`, `touch: the panel held at TOP Hz (hold N)` (the
first three) and at the end `the panel's rate was held at TOP Hz K times, P % of the session`. Without both options, or
without --takeover, the rate is held as before. Built (javac -Xlint:all -Werror, d8; TestModes 21 checks); not run on the
phone.
