# codec/: the video decoder of the native kernel (take18b, 2026-11-26; first written for the hosted encoder, 2026-11-15)

The whole path and its route: `docs/hardware.md`, section 7g. The hosted encoder's host side (xnuhost's virtio-media
device and the MediaCodec helper) is `runtime/bridges/encoder`; the hosted kernel of this tree does not build this archive.

**State (2026-11-26, take18b):** the archive is **built into the native kernel** (`oneios-tool kernel gen codec`,
`kernel/build-hosted.sh` with `ONEIOS_MACHINE=native`, `oneios_native_defconfig`), prelinked (audit 0 failures) and booted
on the QEMU bench with the decoder's node and **no decoder behind it** (QEMU has none): the transport's PCI scan finds no
1af4:1070 function, nothing is published, and the boot goes on as before (the kernel alone; and with the image set on PCI
through the Data store's first start, `seed verified`, launchd and its boot tasks). **Not run on the phone**: nothing of
the decode path (crosvm's device, MediaCodec, a picture in an IOSurface) has run anywhere yet.

It is **off by default**: the native kernel starts it only with the boot argument `oneios-codec=1`, which only
`xnuloader-crosvm --media` gives (with `droidvm-vt-paravirt=1`); an Image without `--media` runs nothing of it.

## Why this route

iOS's VideoToolbox in 24A446 carries a whole **paravirtualized** decoder and encoder (`ParavirtualizedVideoDecoder_*`,
`ParavirtualizedVideoEncoder_*`, read in the 24A446 shared cache) and uses them for every eligible codec once
`VTParavirtualizationIsRunningInGuest`, i.e. once the service `AppleVideoToolboxParavirtualizationDriver` is published.
UniXNU-droidvm's three files are that service and its host half, written for crosvm's virtio-media devices (MediaCodec
behind them). YouTube in Safari decoded on the guest's CPU (take15-17: mediaplaybackd 70-130 %, heat, 1080p pictures
that went black): with the service published and crosvm's decoder behind it, H.264 and HEVC (VP9 and AV1 when the phone
has hardware decoders for them) decode on the phone's MediaCodec and the pictures are copied into the IOSurfaces
VideoToolbox gives.

## The sources

| File | What |
|---|---|
| `DroidVMVirtioMedia.c` (UniXNU-droidvm f9121cd, staged) | the virtio-media transport. Staged with `staged/codec_transport.rs` MEDIA_TRANSPORT_PCI, compiled only in the native kernel: crosvm's card is a virtio-pci function of its ECAM bus (1af4:1070; crosvm f7c6ce8 has no `--virtio-mmio-media`), so `/arm-io/virtio-media-0` "oneios,virtio-media-pci" is all of bus 0 and the K-th such node takes the bus's K-th 1af4:1070 function; the MMIO register offsets go to the function's regions, as the sound card's do. Polled (no interrupt, no MSI-X). A command waits 12 s (crosvm starts the phone's codec at STREAMON and waits up to 10 s for it; the pin's 5 s left a late completion that every later command read as its own), and a command answered after its wait is taken off the used ring before the next one. `droidvm_media_pool_size` for the decoder's sums. `-DDROIDVM_DEVICE_DYNAMIC=1`, Mach universe |
| `DroidVMVTHost.cpp` (f9121cd, staged) | the session logic. Staged with `staged/codec_decode.rs`: CODEC_DECODER (upstream OneiOS 79c4921's edit 6, as it is: VP9 and AV1, `dcap`, `dcan`, the surfaces' kernel mappings kept for the session, SOURCE_CHANGE while streaming, the counters) and then CODEC_CROSVM (what crosvm's decoder needs, below) |
| `DroidVMVTParavirt.cpp` (f9121cd) | Apple's `AppleVideoToolboxParavirtualizationDriver` and its user client, unchanged; its static initializer is taken out of `__mod_init_func` and run by `droidvm_vt_paravirt_attach` in its own module load |
| `OneiOSCodec.c` (this folder) | `oneios_codec_start` (the weak hook of `drivers/bridge/OneiOSBridge.c`): off unless `oneios-codec=1` in the native kernel, then the transport, the classes and the service, and a report thread (below) |

Upstream's encoder edits 1 to 5 (`staged/codec.rs` there: hvcC levels, iOS 27's tile session, 10-bit tiles) are not in
this tree: the native VM has crosvm's decoder only (`--virtio-media kind=decoder`). Upstream's edit 7 (the event queue on
xnuhost's interrupt) is not either: crosvm's PCI function would need MSI-X set up before DRIVER_OK, which this transport
does not do (below, "What is left").

## What crosvm's decoder does, and what changed for it (CODEC_CROSVM)

Read in Droid-VM crosvm 0f2db62 (the source of the prebuilt f7c6ce8; `devices/src/virtio/media/android_codec_backend/
android.rs`, `pool.rs`, `media.rs`) and in the binary's strings; the full notes are the take18 folder's
`patches/codec/crosvm-virtio-media-analysis.txt`. The commands, events and V4L2 structures are those f9121cd writes; card
"droidvm decoder" (the host finds it by "decoder"); OUTPUT formats H264, HEVC, VP80, VP90, AV01, each only when the
phone's codec store has a hardware decoder for it (`allow_sw=false`); CAPTURE NV12, one plane, tightly packed at the coded
size (chroma at bytesperline x height: what `copyFrame` reads); MMAP buffers in the VM's one media_host pool, MMAP
answering the pool offset; the codec created at STREAMON(OUTPUT); timestamps carried (the frame ID in tv_sec).

- **A mid-stream announce parks the session.** After a SOURCE_CHANGE while CAPTURE streams, crosvm hands one CAPTURE
  buffer back with V4L2_BUF_FLAG_LAST and writes no picture until the CAPTURE queue restarts or V4L2_DEC_CMD_START. The
  pin ignored the event and edit 6 only read the new geometry: the session stalled for good. That LAST buffer now resumes
  it: DEC_CMD_START when the new pictures fit the buffers, else STREAMOFF, REQBUFS 0 and a new CAPTURE set
  (`resumeAfterChange`).
- **An ERROR event ends the device's session.** What was given comes back dropped at once (not at dfin's 2 s deadline),
  the session's queued frames are freed, and the session refuses further frames (`dfrm` -12911) and formats (`dcan` 0),
  so VideoToolbox makes a new session.
- **8-bit only.** crosvm copies a picture into CAPTURE as 8-bit NV12 and refuses a 16-bit one (android_codec `image.rs`
  `check_yuv420`): an HEVC Main 10 stream gave no picture. HEVC is now taken at 8 bits only (VP9 profile 0 and AV1 8-bit
  were edit 6's already); a 10-bit stream is refused at `dsta` and VideoToolbox decodes it in software.
- **The pool.** Every MMAP buffer of every media device comes out of one pool (`--pre-alloc media-host-mb`), and REQBUFS
  gets ENOMEM when it is short. A session plans for 23 CAPTURE pictures (c2.qti.avc.decoder announces 21 output slots;
  the guest asks the minimum + 2) and 4 OUTPUT buffers: about 80 MiB at 1080p, about 300 MiB at 4K. A session whose
  buffers the pool cannot hold is refused at `dsta` (software decode); a CAPTURE set the pool cannot give whole is asked
  again with the codec's minimum, then 6.
- **dcap** answers what this path decodes: the profiles of the device's menus (VIDIOC_QUERYCTRL/QUERYMENU, which crosvm
  fills from the phone's codec store) that a session takes (H.264 Baseline, Main, High; HEVC Main; VP9 profile 0; AV1
  Main), at the device's highest level (H.264 and HEVC have level menus), at most the highest level whose largest picture
  the pool holds with a whole session. With the default 192 MiB pool that is H.264 4.2, HEVC 4.1, VP9 4.1, AV1 4.1 (the
  1080p class); `--media-mib 352` reaches 5.1 for all four (4K; HEVC's 5.x needs 322 MiB by these sums, H.264's 5.1 343).
  Upstream's fixed list (and the pool's level) where the device has no menu.
  Every level `dcap` answers is one `dsta` takes.
- **dcan** (edit 6): yes for the same codec and size in a format the device decodes (its new parameter sets go before
  the next frame); no for a session the device ended.
- **OUTPUT buffers** are as large as the codec's input buffer (crosvm's KEY_MAX_INPUT_SIZE, w x h x 3 / 4 + 128, at least
  2 MiB); a larger access unit is still cut, now counted.
- **Safari's pixel buffers** (not crosvm's; UniXNU-droidvm 2cb737d from its head): a `Dbuf` answered with a surface of a
  pool another process made (WebKit's) cannot be looked up by ID (kIOReturnNotPermitted, `stage 41 failed: -536870174`,
  every frame dropped); it is looked up through the process's own IOSurface client, as an encoder's frame is, and every
  later surface of that session goes the same way (one stage 41 line a session).
- The new flags of a session (the pending announce, the device's end) are cleared by `teardown`, since `dsta` and `dinv`
  reuse the instance.

## Switches (all off by default)

| Where | Switch | What |
|---|---|---|
| boot argument | `oneios-codec=1` | the archive starts (native default 0; `xnuloader-crosvm --media` adds it) |
| boot argument | `droidvm-vt-paravirt=1` | publish the service once a decoder is up (`--media` adds it; without it root can publish later: `sysctl kern.droidvm_vt_paravirt=1`) |
| boot argument | `droidvm-media=0` | no transport |
| boot argument | `oneios-codec-codecs=MASK` | list only these of the device's codecs: avc1 1, hvc1 2, vp09 4, av01 8 (default 0xf) |
| boot argument | `oneios-codec-report=0` | no report lines |
| loader | `xnuloader-crosvm ... --shared-ram --media [--media-mib N]` | the node, the pool (192 MiB, even, 16 to 1024; the RAM's last N MiB, the GPU's arena right below it), the two boot arguments, `media N POOL-PA` in OUT.disks |
| loader (bench) | `xnuloader-qemu --media` (`MEDIA=1 scripts/qemu/boot-vm.sh`) | the node on virt's ECAM bus 0, no pool, no device |
| vm-boot | `--video-decoder [--video-decoder-uid UID]` | crosvm `--virtio-media kind=decoder,uid=UID` (default 2000). Without it a `--media` Image still gets the pool (its layout needs it) and no device: iOS decodes in software |

## The lines a boot gives

- `droidvm-media: virtio-pci media device at 00:NN.0 (1af4:1070), notifications x4, polled` (or `no 1af4:1070 function on
  bus 0 (crosvm without --virtio-media? vm-boot --video-decoder)`).
- `ONEIOS CODEC: 1 virtio-media device(s), 1 up; VideoToolbox paravirtualization published (stage 2)`; without a device
  `0 up; ... not published (stage 1)`; without `oneios-codec=1` `ONEIOS CODEC: off (...)`.
- Every 30 s while something moves: `ONEIOS CODEC: device 0: droidvm decoder: up, caps 0x4004000 type 0, pool ...` (once),
  `ONEIOS CODEC: decoded: avc1 N, hvc1 N, vp09 N, av01 N; dropped ...; dfrm to Demt avg N us, max N us`, the counters line,
  `ONEIOS CODEC: decoder sessions: ended by the device N, resumed after a new format N (set up again N), CAPTURE sets cut
  short N, access units cut N, refused for the pool N; commands answered late N, not sent N`, the messages
  (`message N: NAME[PID] 'dcap'`), and the session lines (`session: decoder codecs 0x...`, `session: decoder avc1 caps:
  profile items 0x... level items 0x...; 3 profiles at level 42 (pool 192 MiB)`, `session: decoder session N WxH avc1`).
- vm-boot's run.txt: `video: crosvm's media_host pool is at 0x..., as the Image's media-host-pool (192 MiB)`.

## What a phone session must show (YouTube in Safari, an Image with `--media`, vm-boot `--video-decoder`)

The first session with `--args-add oneios-codec-codecs=3` in the Image (H.264 and HEVC only: the path Droid-VM ran on
crosvm), with One UI running; then without the mask (VP9 and AV1 too, which YouTube prefers once they are listed, and
whose sample path upstream never booted), then in a takeover.

- crosvm.log: the decoder helper started under the uid, its store walk (`decoder: ... hardware ...` per format), a
  session's codec created at STREAMON (`c2.qti.avc.decoder` or `.vp9`/`.av1`).
- The guest: the lines above, `decoded:` growing, `dropped` near 0, `ended by the device` 0, `dfrm to Demt` in the tens of
  milliseconds at most; iOS's log: no `ParavirtualizedVideoDecoder ... failed`.
- The owner: video without the black pictures, and mediaplaybackd / the WebContent process well below their 70-130 %.

## Tested

- `cargo test -p oneios-kernel --test golden`: every edit list on the pinned sources (`the_codec_edits_fit_the_pinned_
  sources`), the made-up source checks; `make -C runtime/loader test`; `cargo test -p oneios-phone vmboot`.
- Kernel build (`NATIVE_OUT=codec kernel/build-native.sh`), no warnings in the archive's objects; prelink audit 0 failures.
- QEMU bench: `KERNEL=.../kernel.kc MEDIA=1 scripts/qemu/boot-vm.sh 100` (the kernel alone: the two lines above, the same
  rounds as take17's bench-short) and `... IMAGE=... PCI=1 MEDIA=1 CPUS=4 RAM_MIB=2816 ARGS_ADD=oneios-data-wait=1800`
  (the image set on virtio-blk-pci: the scan passes the disks' functions, then the first start, launchd and its boot tasks).
- **Not tested:** anything with a virtio-media device (QEMU has none; crosvm runs only on the phone): the commands and
  events against crosvm, the pool's offsets, dcap's menus, a decoded picture, MediaCodec under the helper's uid with One UI
  stopped, the resume after a mid-stream announce, the ERROR path.

## What is left

- The phone session above. If crosvm's helper cannot reach the codec services with the framework stopped (a takeover), a
  session with One UI running is the first test.
- Upstream edit 7 for PCI: MSI-X (or crosvm's INTx line, as `--gpu-intid`) with the vectors set before DRIVER_OK, so the
  event queue and the commands sleep on an interrupt instead of the 1 ms poll while a session streams.
- USERPTR buffers in the guest's RAM instead of the pool (UniXNU-droidvm 6cb29bb, a229647): no pool limit, 4K and several
  sessions at once. crosvm 0f2db62 takes USERPTR for the decoder (guest_buf.rs) but this path has not been read against it.
- The 10-bit path (UniXNU-droidvm a5e9d5c: x420 surfaces) once a 10-bit stream is seen to come out of crosvm.
