// Written with `oneios-tool kernel gen codec` (2026-11-24): no Python original. Upstream OneiOS 79c4921's edit 6, taken as it
// is (take18b, 2026-11-26), and this tree's CODEC_CROSVM after it (the end of this file).
/// DroidVMVTHost.cpp's decoder lines that change (edit 6 of upstream's codec archive; its edits 1 to 5, the encoder's,
/// are not in this tree: the native VM has crosvm's decoder only, so this list is applied to the pinned source directly).
///
/// 6. The hardware decoder (2026-11-24). YouTube in Safari decoded on the CPU (videocodecd and WebKit's processes,
///    `out/session-logs/2026-11-24-mac-gpumem`): xnuhost's device was an encoder only, so `decs` listed nothing. Since
///    then the device is an encoder and a decoder (`oneios-encoder+decoder`, xnuhost's `decoder=auto`; the decoder is
///    the phone's MediaCodec). What upstream's decoder half lacks, read in 24A446's VideoToolbox
///    (`out/ios27-cache`, `ipsw dyld extract`, 2026-11-24):
///    - VP9 and AV1. The guest's paravirtualized decoder takes any codec type the host lists (`vtLoadParavirtualizedVideo
///      Decoders` registers each `decs` entry; `VTIsHardwareDecodeSupported` creates an instance with a hardware decoder
///      required, VP9 behind `VTIsVP9DecodeEnabledForCurrentProcess`). `decs` lists 'vp09' and 'av01' too when the
///      device's OUTPUT queue offers V4L2_PIX_FMT_VP9 ('VP90') and 'AV01' (VIDIOC_ENUM_FMT, asked once). A VP9 or AV1
///      sample is the codec's own frame (no length prefixes): copied as it is; AV1's configOBUs (the av1C's bytes after
///      its first four: the sequence header) go before the first frame, as an avcC's parameter sets do. Only 8-bit
///      4:2:0 is taken (VP9 profile 0 by its vpcC, AV1 Main 8-bit by its av1C): the CAPTURE queue is NV12 and the guest
///      is asked for 420v; a 10-bit stream is refused at dsta and VideoToolbox decodes it in software, as before.
///    - `dcap` (`vtCreateDecoderCapabilitiesDictionary` in a guest: `VTCopyAV1DecoderCapabilitiesDictionary`, `VTCopyHEVC
///      DecoderCapabilitiesDictionary`, which WebCore asks before it offers a codec): `,ver` and `,cod`, the answer
///      `,cda`, the dictionary 24A446's own host makes: VTSupportedProfiles (an array of profile numbers),
///      VTPerProfileSupport (by the profile's number as a string: VTIsHardwareAccelerated, VTMaxDecodeLevel,
///      VTMaxPlaybackLevel). Upstream answered the unknown type's -12900, so the guest had no capabilities.
///    - `dcan` (`ParavirtualizedVideoDecoder_CanAcceptFormatDescription`, `,fds`, the answer `,can`): yes for the same
///      codec and size in a format the device decodes, its new parameter sets before the next frame; else no, and
///      VideoToolbox makes a new session. Upstream's -12900 made every format change a new session.
///    - The surface's kernel mapping is kept for the session (at most 16 surfaces): upstream mapped every surface into
///      the kernel and unmapped it again for every frame (`copyFrame`), a mapping of about 3 MB per 1080p frame. The
///      pages are wired only while the mapping is made (`complete()` at once), so a process that frees its surface never
///      waits for the session.
///    - A SOURCE_CHANGE while CAPTURE streams takes the new geometry (G_FMT) when it fits the buffers.
///    - Counters for OneiOSCodec.c's `decoded:` line: frames handed over decoded and dropped, by codec (avc1, hvc1,
///      vp09, av01), and the time from a frame's dfrm to its Demt (sum, count, max; the codec's reordering is in it).
pub const CODEC_DECODER: &[(&str, &str)] = &[
    // The counters.
    ("volatile uint32_t droidvm_vthost_trace_count;\n}\n",
     r#"volatile uint32_t droidvm_vthost_trace_count;
// OneiOS (kernel gen codec, 2026-11-24): the decoder's frames by codec (avc1, hvc1, vp09, av01), handed over decoded and
// dropped; the time from a frame's dfrm to its Demt, in microseconds (sum, count, the largest since OneiOSCodec.c's last
// line, which sets it back to 0).
volatile uint32_t oneios_decode_frames[4], oneios_decode_dropped[4];
volatile uint64_t oneios_decode_us_sum;
volatile uint32_t oneios_decode_us_count, oneios_decode_us_max;
}
"#),
    // The decoder session's state.
    ("    bool replyHasSurface = false;\n    SurfaceView replySurface;\n};\n",
     r#"    bool replyHasSurface = false;
    SurfaceView replySurface;
    // OneiOS (kernel gen codec, 2026-11-24): a decoder session's configuration record (the avcC, hvcC, vpcC or av1C body
    // dsta took, for dcan), when each frame was given (dfrm, for the time to its Demt), and the surfaces mapped into the
    // kernel for it (kept until the session ends).
    uint8_t record[512] = {};
    uint32_t recordSize = 0;
    struct { int64_t frame; uint64_t at; } given[64] = {};
    unsigned givenNext = 0;
    struct { uint32_t id; uint64_t base; IOMemoryDescriptor *memory; IOMemoryMap *map; uint8_t *bytes; } mappedSurfaces[16] = {};
    unsigned mappedCount = 0;
};
"#),
    // The codecs, before the first use (emitFrame's counters).
    ("int ioctl(Session *s, uint32_t code, void *payload, uint32_t out, uint32_t in) {\n",
     r#"// OneiOS (kernel gen codec, 2026-11-24): the decoder's codecs, in the order of the counters and of the device's bits:
// avc1, hvc1 (hev1 too), vp09, av01; -1 for another type.
int codecIndex(uint32_t codec) {
    if (codec == cc("avc1")) return 0;
    if (codec == cc("hvc1") || codec == cc("hev1")) return 1;
    if (codec == cc("vp09")) return 2;
    if (codec == cc("av01")) return 3;
    return -1;
}

// The V4L2 format of each (little endian): V4L2_PIX_FMT_H264, _HEVC, _VP9 ('VP90'), and 'AV01' (xnuhost's device).
uint32_t decoderFourcc(uint32_t codec) {
    static const uint32_t fourcc[4] = {cc("462H"), cc("CVEH"), cc("09PV"), cc("10VA")};
    const int index = codecIndex(codec);
    return index < 0 ? 0 : fourcc[index];
}

// A VP9 or AV1 sample is the codec's frame as it is, with no NAL length prefixes.
bool rawStream(uint32_t codec) { return codecIndex(codec) >= 2; }

// The codecs the decoder device offers (bit i: codecIndex i): its OUTPUT queue's formats (VIDIOC_ENUM_FMT, asked once in a
// session of its own); H.264 and HEVC, upstream's two, when it cannot be asked.
uint32_t decoderCodecs() {
    static uint32_t codecs;
    static bool asked;
    const unsigned device = decoderDevice();
    if (device == ~0u) return 0;
    if (asked) return codecs;
    uint32_t found = 0, session = 0;
    if (droidvm_media_open(device, &session) == 0) {
        for (uint32_t index = 0; index < 16; ++index) {
            uint32_t desc[16] = {index, 10 /* TypeOutputMplane */};   // struct v4l2_fmtdesc; pixelformat at 44
            if (droidvm_media_ioctl(device, session, 2 /* VIDIOC_ENUM_FMT */, desc, sizeof desc, sizeof desc)) break;
            for (int i = 0; i < 4; ++i) {
                static const uint32_t fourcc[4] = {cc("462H"), cc("CVEH"), cc("09PV"), cc("10VA")};
                if (desc[11] == fourcc[i]) found |= 1u << i;
            }
        }
        droidvm_media_close(device, session);
    }
    codecs = found ? found : 3;
    asked = true;
    trace("decoder codecs %#x (avc1 hvc1 vp09 av01)", codecs);
    return codecs;
}

int ioctl(Session *s, uint32_t code, void *payload, uint32_t out, uint32_t in) {
"#),
    // The surface's kernel mapping, kept.
    (r#"void copyFrame(Session *s, const Mapped &from, const SurfaceView &to) {
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(to.base, to.size, kIODirectionInOut, s->client->task);
    if (!memory || memory->prepare() != kIOReturnSuccess) { OSSafeReleaseNULL(memory); failed(60, 0); return; }
    IOMemoryMap *map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
    uint8_t *surface = map ? reinterpret_cast<uint8_t *>(map->getVirtualAddress()) : nullptr;
    if (surface) {
"#,
     r#"// OneiOS (kernel gen codec, 2026-11-24): a decoded picture into its surface through the surface's kernel mapping, made at
// the surface's first picture and kept for the session (the guest's pool hands the same few surfaces out again and again;
// upstream mapped and unmapped the whole surface for every frame). The pages are wired only while the mapping is made: a
// process that frees the surface must not wait for this session. A 17th surface is mapped for its picture alone.
void copyFrame(Session *s, const Mapped &from, const SurfaceView &to) {
    IOMemoryDescriptor *memory = nullptr;
    IOMemoryMap *map = nullptr;
    uint8_t *surface = nullptr;
    for (unsigned i = 0; i < s->mappedCount && !surface; ++i)
        if (s->mappedSurfaces[i].id == to.id && s->mappedSurfaces[i].base == to.base) surface = s->mappedSurfaces[i].bytes;
    if (!surface) {
        memory = IOMemoryDescriptor::withAddressRange(to.base, to.size, kIODirectionInOut, s->client->task);
        if (!memory || memory->prepare() != kIOReturnSuccess) { OSSafeReleaseNULL(memory); failed(60, 0); return; }
        map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
        memory->complete();
        surface = map ? reinterpret_cast<uint8_t *>(map->getVirtualAddress()) : nullptr;
        if (surface && s->mappedCount < 16) {
            s->mappedSurfaces[s->mappedCount++] = {to.id, to.base, memory, map, surface};
            memory = nullptr;
            map = nullptr;
        }
    }
    if (surface) {
"#),
    (r#"    } else {
        failed(61, 0);
    }
    OSSafeReleaseNULL(map);
    memory->complete();
    memory->release();
}
"#,
     r#"    } else {
        failed(61, 0);
    }
    OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(memory);
}

// OneiOS (kernel gen codec, 2026-11-24): the time from the frame's dfrm (the lock held).
uint32_t givenMicroseconds(Session *s, int64_t frame) {
    for (auto &g : s->given) {
        if (g.at && g.frame == frame) {
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(mach_absolute_time() - g.at, &ns);
            g.at = 0;
            return uint32_t(ns / 1000 > 0xffffffffull ? 0xffffffffull : ns / 1000);
        }
    }
    return 0;
}

void countDecoded(Session *s, int64_t frame, bool decoded) {
    const int index = codecIndex(s->codec);
    if (index < 0) return;
    IOLockLock(s->lock);
    const uint32_t us = givenMicroseconds(s, frame);
    IOLockUnlock(s->lock);
    if (!decoded) { oneios_decode_dropped[index] = oneios_decode_dropped[index] + 1; return; }
    oneios_decode_frames[index] = oneios_decode_frames[index] + 1;
    if (!us) return;
    __atomic_fetch_add(&oneios_decode_us_sum, uint64_t(us), __ATOMIC_RELAXED);
    __atomic_fetch_add(&oneios_decode_us_count, 1u, __ATOMIC_RELAXED);
    if (us > oneios_decode_us_max) oneios_decode_us_max = us;
}
"#),
    // A Dbuf the guest did not answer: the frame is dropped.
    ("    if (!hostRequest(s, w, message, request) || s->replyError || !s->replyHasSurface) {\n        failed(31, s->replyError);\n        return;\n    }\n",
     "    if (!hostRequest(s, w, message, request) || s->replyError || !s->replyHasSurface) {\n        failed(31, s->replyError);\n        countDecoded(s, frame, false);   // OneiOS: counted\n        return;\n    }\n"),
    ("    if (hostRequest(s, e, message, emit)) droidvm_vthost_frames_out = droidvm_vthost_frames_out + 1;\n}\n",
     "    if (hostRequest(s, e, message, emit)) droidvm_vthost_frames_out = droidvm_vthost_frames_out + 1;\n    countDecoded(s, frame, bytesused != 0);   // OneiOS (2026-11-24): the decoder's counters\n}\n"),
    ("        e.u32(cc(\",inf\"), 2);   // kVTDecodeInfo_FrameDropped\n",
     "        e.u32(cc(\",inf\"), 2);   // kVTDecodeInfo_FrameDropped\n        countDecoded(s, frame, false);   // OneiOS: counted\n"),
    // A new geometry while CAPTURE streams.
    ("void workerMain(void *argument, wait_result_t) {\n",
     r#"// OneiOS (kernel gen codec, 2026-11-24): a SOURCE_CHANGE while CAPTURE streams: the device's new geometry, when its
// pictures fit the buffers already mapped (upstream kept the first one; a picture larger than the buffers is not taken).
void refreshCapture(Session *s) {
    V4l2Format f = {};
    f.type = TypeCaptureMplane;
    const int r = ioctl(s, IocGFmt, &f, sizeof f, sizeof f);
    if (r) { failed(57, r); return; }
    const uint32_t stride = f.planes[0].bytesperline ? f.planes[0].bytesperline : f.width;
    uint64_t smallest = ~0ull;
    for (unsigned i = 0; i < s->captureCount; ++i) if (s->capture[i].length < smallest) smallest = s->capture[i].length;
    if (uint64_t(stride) * f.height * 3 / 2 > smallest) { failed(58, int32_t(f.width)); return; }
    s->captureWidth = f.width;
    s->captureHeight = f.height;
    s->captureStride = stride;
    trace("decoder capture now %ux%u stride %u", f.width, f.height, stride);
}

void workerMain(void *argument, wait_result_t) {
"#),
    ("            if (!s->captureStreaming) setUpCapture(s);\n",
     "            if (!s->captureStreaming) setUpCapture(s);\n            else refreshCapture(s);   // OneiOS (2026-11-24)\n"),
    // The decoder list: the codecs the device offers.
    (r#"        // H.264 and HEVC: what the virtio-media decoder takes (ENUM_FMT) and VideoToolbox can describe for it.
        static const char *const codecs[2][3] = {
            {"avc1", "com.droidvm.videodecoder.virtio-media.avc", "DroidVM Video Decoder - AVC (virtio-media)"},
            {"hvc1", "com.droidvm.videodecoder.virtio-media.hevc", "DroidVM Video Decoder - HEVC (virtio-media)"},
        };
        int entries[2];
        for (int i = 0; i < 2; ++i) {
"#,
     r#"        // OneiOS (kernel gen codec, 2026-11-24): H.264, HEVC, VP9 and AV1, those the virtio-media decoder takes (its
        // OUTPUT formats, decoderCodecs).
        static const char *const codecs[4][3] = {
            {"avc1", "com.droidvm.videodecoder.virtio-media.avc", "DroidVM Video Decoder - AVC (virtio-media)"},
            {"hvc1", "com.droidvm.videodecoder.virtio-media.hevc", "DroidVM Video Decoder - HEVC (virtio-media)"},
            {"vp09", "com.oneios.videodecoder.virtio-media.vp9", "OneiOS Video Decoder - VP9 (virtio-media)"},
            {"av01", "com.oneios.videodecoder.virtio-media.av1", "OneiOS Video Decoder - AV1 (virtio-media)"},
        };
        const uint32_t offered = decoderCodecs();
        int entries[4];
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i) {
            if (!(offered & (1u << i))) continue;
"#),
    ("            entries[i] = p->dict(keys, values, 5);\n        }\n        const int list = p->array(entries, decoderDevice() == ~0u ? 0 : 2);\n",
     "            entries[count++] = p->dict(keys, values, 5);\n        }\n        const int list = p->array(entries, count);\n"),
    // VP9 and AV1: 8-bit 4:2:0 only.
    ("    return false;\n}\n\nbool decodableSize(Session *s, uint32_t pixelformat) {\n",
     r#"    // OneiOS (kernel gen codec, 2026-11-24): VP9 profile 0 (8-bit 4:2:0: the vpcC, a full box, its profile, level, then
    // bit depth (4 bits), chroma subsampling (3) and full range (1)) and AV1 Main 8-bit 4:2:0 (the av1C: marker and
    // version, seq_profile (3 bits) and seq_level_idx_0 (5), then tier, high_bitdepth, twelve_bit, monochrome,
    // chroma_subsampling_x, _y, chroma_sample_position (2)). A description without its record is not taken.
    if (find(d.extensions, d.extensionsSize, cc("vpcC"), record)) {
        if (record.size < 12) return false;
        const uint8_t profile = record.body[4], depth = record.body[6] >> 4, chroma = (record.body[6] >> 1) & 7;
        return profile == 0 && depth == 8 && chroma <= 1;   // 4:2:0 vertical or colocated
    }
    if (find(d.extensions, d.extensionsSize, cc("av1C"), record)) {
        if (record.size < 4 || record.body[0] != 0x81) return false;
        const uint8_t profile = record.body[1] >> 5, flags = record.body[2];
        const bool high = flags & 0x40, mono = flags & 0x10, x = flags & 0x08, y = flags & 0x04;
        return profile == 0 && !high && !mono && x && y;
    }
    return false;
}

// OneiOS (kernel gen codec, 2026-11-24): the configuration record of a description (the avcC, hvcC, vpcC or av1C).
bool configurationAtom(const ImageDescription &d, Atom &record) {
    const uint8_t *e = d.extensions;
    const uint32_t n = d.extensionsSize;
    return find(e, n, cc("avcC"), record) || find(e, n, cc("hvcC"), record) || find(e, n, cc("vpcC"), record) || find(e, n, cc("av1C"), record);
}

// A VP9 or AV1 stream's headers: none for VP9 (every key frame carries its own); for AV1 the av1C's configOBUs (its sequence
// header), which go before the first frame. No length prefixes (nalLength 0).
bool streamHeaders(Session *s, const ImageDescription &d) {
    Atom record;
    s->nalLength = 0;
    s->parameterSetsSize = 0;
    if (d.codec != cc("av01")) return true;
    if (!find(d.extensions, d.extensionsSize, cc("av1C"), record) || record.size < 4) return false;
    const uint32_t n = record.size - 4;
    if (n > sizeof s->parameterSets) return false;
    memcpy(s->parameterSets, record.body + 4, n);
    s->parameterSetsSize = n;
    return true;
}

bool decodableSize(Session *s, uint32_t pixelformat) {
"#),
    // The session's start for any of the four.
    ("    if (d.codec != cc(\"avc1\") && d.codec != cc(\"hvc1\") && d.codec != cc(\"hev1\")) return -12906;\n",
     "    // OneiOS (kernel gen codec, 2026-11-24): any codec the device offers (H.264, HEVC, VP9, AV1)\n    if (codecIndex(d.codec) < 0 || !(decoderCodecs() & (1u << codecIndex(d.codec)))) return -12906;\n"),
    ("    if (!parameterSets(s, d)) return -12909;\n",
     r#"    if (!(rawStream(d.codec) ? streamHeaders(s, d) : parameterSets(s, d))) return -12909;   // OneiOS: VP9 and AV1 have no NAL units
    {   // OneiOS: the record, for dcan
        Atom record;
        s->recordSize = configurationAtom(d, record) && record.size <= sizeof s->record ? record.size : 0;
        if (s->recordSize) memcpy(s->record, record.body, s->recordSize);
    }
"#),
    ("    if (!decodableSize(s, d.codec == cc(\"avc1\") ? cc(\"462H\") : cc(\"CVEH\"))) { teardown(s); return -12910; }\n",
     "    if (!decodableSize(s, decoderFourcc(d.codec))) { teardown(s); return -12910; }   // OneiOS: the codec's V4L2 format\n"),
    ("    f.pixelformat = d.codec == cc(\"avc1\") ? cc(\"462H\") : cc(\"CVEH\");   // V4L2_PIX_FMT_H264 'H264', _HEVC 'HEVC' (little endian)\n",
     "    f.pixelformat = decoderFourcc(d.codec);   // OneiOS: V4L2_PIX_FMT_H264 'H264', _HEVC 'HEVC', _VP9 'VP90', 'AV01' (little endian)\n"),
    ("    IOLog(\"DroidVMVTHost: session %u %ux%u %c%c%c%c\\n\", s->v4l2, s->width, s->height, d.codec >> 24, d.codec >> 16, d.codec >> 8, d.codec);\n",
     "    IOLog(\"DroidVMVTHost: session %u %ux%u %c%c%c%c\\n\", s->v4l2, s->width, s->height, d.codec >> 24, d.codec >> 16, d.codec >> 8, d.codec);\n    trace(\"decoder session %u %ux%u %c%c%c%c\", s->v4l2, s->width, s->height, d.codec >> 24, d.codec >> 16, d.codec >> 8, d.codec);   // OneiOS\n"),
    // A VP9 or AV1 frame as it is.
    ("    const uint8_t *p = sdat.body, *end = sdat.body + sdat.size;\n    while (uint32_t(end - p) > s->nalLength) {\n",
     r#"    const uint8_t *p = sdat.body, *end = sdat.body + sdat.size;
    if (!s->nalLength && at + sdat.size <= capacity) {   // OneiOS (2026-11-24): a VP9 or AV1 frame, as it is
        memcpy(bytes + at, sdat.body, sdat.size);
        at += sdat.size;
    }
    while (s->nalLength && uint32_t(end - p) > s->nalLength) {
"#),
    ("    if (s->outstandingCount < 64) s->outstanding[s->outstandingCount++] = frame;\n",
     "    if (s->outstandingCount < 64) s->outstanding[s->outstandingCount++] = frame;\n    s->given[s->givenNext++ % 64] = {frame, mach_absolute_time()};   // OneiOS: for the time to its Demt\n"),
    // dcap and dcan, after queueFrame (which they follow in the file).
    ("}  // namespace\n\nextern \"C\" {\n\n// Whether crosvm gave the guest",
     r#"// OneiOS (kernel gen codec, 2026-11-24): dcap, a codec's capabilities as 24A446's own host answers them (,cda): the profiles
// the device decodes, each hardware accelerated, with the highest level it plays (the media pool's size bounds it: 4K).
// H.264 Baseline, Main, High at level 5.1 (51); HEVC Main and Main 10 at 5.1 (general_level_idc 153); VP9 profile 0 at 5.1
// (51, VideoToolbox's own figure); AV1 Main at 5.1 (seq_level_idx 13).
void decoderCapabilities(Client *c, const Header &h, const uint8_t *m, uint32_t n) {
    int32_t codec = 0;
    paramS32(m, n, cc(",cod"), codec);
    const int index = codecIndex(uint32_t(codec));
    if (index < 0 || !(decoderCodecs() & (1u << index))) { replyStatus(c, h, -12906); return; }   // kVTCouldNotFindVideoDecoderErr
    static const struct { uint8_t count; uint8_t profiles[3]; const char *names[3]; int level; } table[4] = {
        {3, {66, 77, 100}, {"66", "77", "100"}, 51},
        {2, {1, 2}, {"1", "2"}, 153},
        {1, {0}, {"0"}, 51},
        {1, {0}, {"0"}, 13},
    };
    Plist *p = new Plist();
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocZero(2048));
    if (p && buffer) {
        const auto &t = table[index];
        int profiles[3], names[3], entries[3];
        for (unsigned i = 0; i < t.count; ++i) {
            profiles[i] = p->integer(t.profiles[i]);
            names[i] = p->string(t.names[i]);
            const int keys[3] = {p->string("VTIsHardwareAccelerated"), p->string("VTMaxDecodeLevel"), p->string("VTMaxPlaybackLevel")};
            const int values[3] = {p->boolean(true), p->integer(t.level), p->integer(t.level)};
            entries[i] = p->dict(keys, values, 3);
        }
        const int keys[2] = {p->string("VTSupportedProfiles"), p->string("VTPerProfileSupport")};
        const int values[2] = {p->array(profiles, t.count), p->dict(names, entries, t.count)};
        const int capabilities = p->dict(keys, values, 2);
        Writer w(buffer, 2048);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), 0);
        w.plist(cc(",cda"), *p, capabilities);
        sendToGuest(c, w, buffer);
    } else {
        replyStatus(c, h, -12904);
    }
    delete p;
    if (buffer) IOFree(buffer, 2048);
}

// dcan: whether the session takes the new description (,fds) as it is: the same codec and size, in a format the device
// decodes. Its parameter sets (an AV1 sequence header) go before the next frame. ,can 0: VideoToolbox makes a new session.
int32_t acceptFormat(Session *s, const uint8_t *m, uint32_t n) {
    Param fds;
    ImageDescription d;
    if (!s || !s->outputStreaming || !param(m, n, cc(",fds"), fds) || !imageDescription(fds.value, fds.size, d)) return 0;
    if (codecIndex(d.codec) != codecIndex(s->codec) || d.width != s->width || d.height != s->height || !decodableFormat(d)) return 0;
    Atom record;
    if (!configurationAtom(d, record) || record.size > sizeof s->record) return 0;
    if (record.size == s->recordSize && !memcmp(record.body, s->record, record.size)) return 1;
    if (!(rawStream(d.codec) ? streamHeaders(s, d) : parameterSets(s, d))) return 0;
    memcpy(s->record, record.body, record.size);
    s->recordSize = record.size;
    s->parametersSent = false;
    trace("decoder session %u takes a new description", s->v4l2);
    return 1;
}

}  // namespace

extern "C" {

// Whether crosvm gave the guest"#),
    ("    case cc(\"+dec\"): createDecoder(c, h, m, n); break;\n",
     r#"    case cc("+dec"): createDecoder(c, h, m, n); break;
    // OneiOS (kernel gen codec, 2026-11-24): the decoder's capabilities (WebCore asks them) and a new description.
    case cc("dcap"): decoderCapabilities(c, h, m, n); break;
    case cc("dcan"): {
        uint8_t buffer[128];
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), 0);
        w.s32(cc(",can"), acceptFormat(s, m, n));
        sendToGuest(c, w, buffer);
        break;
    }
"#),
    // The kept mappings go with the session.
    ("    OSSafeReleaseNULL(s->holder);\n}\n",
     r#"    OSSafeReleaseNULL(s->holder);
    // OneiOS (kernel gen codec, 2026-11-24): the surfaces' kernel mappings (the worker has stopped).
    for (unsigned i = 0; i < s->mappedCount; ++i) {
        OSSafeReleaseNULL(s->mappedSurfaces[i].map);
        OSSafeReleaseNULL(s->mappedSurfaces[i].memory);
    }
    s->mappedCount = 0;
    memset(s->given, 0, sizeof s->given);   // a new session's frame IDs start again
}
"#),
];

/// DroidVMVTHost.cpp's lines that change for crosvm's decoder (take18b, 2026-11-26), applied after CODEC_DECODER: the native
/// VM's device is Droid-VM crosvm 0f2db62's `--virtio-media kind=decoder` (the prebuilt f7c6ce8; its MediaCodec backend is
/// devices/src/virtio/media/android_codec_backend/android.rs), where upstream's was xnuhost's own. What that device does
/// differently, read in its source (`patches/codec/crosvm-virtio-media-analysis.txt` of the take18 folder):
///  - A mid-stream announce (a SOURCE_CHANGE while CAPTURE streams: a new coded size or crop) is followed by one CAPTURE
///    buffer with V4L2_BUF_FLAG_LAST, and the session is "parked": no picture is written until the CAPTURE queue restarts or
///    V4L2_DEC_CMD_START. The pin (and edit 6's refreshCapture) sent neither, so the session stalled for good. That LAST
///    buffer now resumes it: DEC_CMD_START when the new pictures fit the buffers, else a new CAPTURE set at the new size.
///  - An ERROR event ends the device's session (its codec failed); the pin only counted it and the frames came back at dfin's
///    deadline. Now what was given comes back dropped at once and the session refuses further frames (dfrm -12911, dcan 0),
///    so VideoToolbox makes a new session.
///  - CAPTURE is 8-bit NV12 only and a 16-bit (10-bit) picture is refused by the copy (android_codec image.rs
///    `check_yuv420`): an HEVC Main 10 stream gave no picture. HEVC is taken at 8 bits only (VP9 and AV1 were already).
///  - Every MMAP buffer comes out of one media_host pool for the whole VM; REQBUFS gets ENOMEM when it is short, and a 1080p
///    session takes about 80 MiB (23 CAPTURE pictures: c2.qti.avc.decoder announces 21 output slots, + 2; 4 OUTPUT buffers).
///    A session whose buffers the pool cannot hold is refused at dsta (VideoToolbox decodes it in software); a CAPTURE set the
///    pool cannot give whole is asked again smaller (the codec's minimum, then 6).
///  - dcap answers what this path decodes: the profiles of the device's menus (VIDIOC_QUERYCTRL/QUERYMENU; crosvm fills them
///    from the phone's codec store) that a session takes, at the device's highest level, at most the highest whose largest
///    picture the pool holds (so every level dcap answers is one dsta takes). Upstream's fixed list where there is no menu.
///  - OUTPUT buffers are as large as the codec's input buffer (crosvm's KEY_MAX_INPUT_SIZE, w * h * 3 / 4 + 128, at least
///    2 MiB), and an access unit still larger is counted (it was cut silently).
///  - `oneios-codec-codecs=MASK` (bits: avc1 1, hvc1 2, vp09 4, av01 8): only those of the device's codecs are listed; none
///    without a media_host pool of at least 16 MiB.
///  - Not crosvm's but Safari's (UniXNU-droidvm 2cb737d, from its head): a Dbuf's pixel buffer from a pool another process
///    made (WebKit's) cannot be looked up by ID (kIOReturnNotPermitted); it is looked up through the process's own IOSurface
///    client, as an encoder's frame is.
///  - The flags above are the session's: teardown clears them (dsta and dinv reuse the instance).
/// Counters for OneiOSCodec.c's `decoder sessions:` line (oneios_decode_lost, _resumed, _resized, _short, _cut, _unfit).
pub const CODEC_CROSVM: &[(&str, &str)] = &[
    ("bool droidvm_media_is_up(unsigned device);\n",
     "bool droidvm_media_is_up(unsigned device);\nuint64_t droidvm_media_pool_size(unsigned device);   // OneiOS (staged/codec_transport.rs)\n"),
    ("#include \"DroidVMVTWire.h\"\n",
     "#include \"DroidVMVTWire.h\"\n#include <pexpert/pexpert.h>   // OneiOS (CODEC_CROSVM): oneios-codec-codecs\n"),
    ("volatile uint32_t oneios_decode_us_count, oneios_decode_us_max;\n",
     r#"volatile uint32_t oneios_decode_us_count, oneios_decode_us_max;
// OneiOS (CODEC_CROSVM): sessions crosvm's device ended (an ERROR event); sessions resumed after a mid-stream announce, and of
// them those set up again at a new size; CAPTURE sets smaller than asked (the pool was short); access units larger than their
// OUTPUT buffer; sessions refused at dsta because their buffers would not fit the pool.
volatile uint32_t oneios_decode_lost, oneios_decode_resumed, oneios_decode_resized, oneios_decode_short, oneios_decode_cut,
    oneios_decode_unfit;
"#),
    ("    unsigned mappedCount = 0;\n};\n",
     r#"    unsigned mappedCount = 0;
    // OneiOS (CODEC_CROSVM): a SOURCE_CHANGE while CAPTURE streamed, waiting for its LAST buffer, and whether the new pictures
    // fit the buffers; the device ended the session (an ERROR event).
    bool changePending = false, changeFits = false, deviceGone = false;
};
"#),
    ("    codecs = found ? found : 3;\n",
     r#"    codecs = found ? found : 3;
    // OneiOS (CODEC_CROSVM): oneios-codec-codecs=MASK lists only those of them (bits as codecIndex); none without a media_host
    // pool to put their buffers in (the node's media-host-pool, xnuloader-crosvm --media)
    {
        uint32_t mask = 0xf;
        PE_parse_boot_argn("oneios-codec-codecs", &mask, sizeof mask);
        codecs &= mask;
        if (droidvm_media_pool_size(device) < (16ull << 20)) codecs = 0;
    }
"#),
    ("        return chroma == 1 && lumaDepth <= 10 && chromaDepth <= 10;\n",
     r#"        // OneiOS (CODEC_CROSVM): 8-bit only. crosvm's decoder copies a picture into CAPTURE as 8-bit NV12 and refuses a 16-bit
        // one (android_codec image.rs check_yuv420), so a Main 10 stream gives no picture; VideoToolbox decodes it in software.
        return chroma == 1 && lumaDepth == 8 && chromaDepth == 8;
"#),
    ("bool decodableSize(Session *s, uint32_t pixelformat) {\n",
     r#"// OneiOS (CODEC_CROSVM): what a session takes of crosvm's media_host pool, the one pool of every media device of the VM: its
// CAPTURE set (the codec's minimum + 2; c2.qti.avc.decoder announces 21 output slots, so 23 are planned for), each a tightly
// packed NV12 picture of the coded size, and its OUTPUT buffers, each the larger of 2 MiB and the codec's input buffer
// (crosvm's KEY_MAX_INPUT_SIZE, w * h * 3 / 4 + 128), in whole MiB.
constexpr unsigned PlannedCaptureBuffers = 23;
uint32_t outputBytes(uint64_t samples) {
    const uint64_t input = (samples * 3 / 4 + 128 + 0xfffff) & ~0xfffffull;
    return uint32_t(input > OutputBytes ? input : OutputBytes);
}
uint64_t sessionBytes(uint64_t samples) {
    return samples * 3 / 2 * PlannedCaptureBuffers + uint64_t(OutputBuffers) * outputBytes(samples);
}
uint64_t codedSamples(uint32_t width, uint32_t height) { return uint64_t((width + 15) & ~15u) * ((height + 15) & ~15u); }

bool decodableSize(Session *s, uint32_t pixelformat) {
    // OneiOS (CODEC_CROSVM): a session whose buffers the pool cannot hold is refused here, and decoded in software, rather than
    // taken and left without pictures when REQBUFS(CAPTURE) gets ENOMEM
    if (sessionBytes(codedSamples(s->width, s->height)) > droidvm_media_pool_size(s->device)) {
        oneios_decode_unfit = oneios_decode_unfit + 1;
        trace("%ux%u: its buffers need %llu MiB, the pool has %llu", s->width, s->height,
              (unsigned long long)(sessionBytes(codedSamples(s->width, s->height)) >> 20),
              (unsigned long long)(droidvm_media_pool_size(s->device) >> 20));
        return false;
    }
"#),
    ("    f.planes[0].sizeimage = OutputBytes;\n",
     "    f.planes[0].sizeimage = outputBytes(codedSamples(d.width, d.height));   // OneiOS (CODEC_CROSVM): the codec's input buffer\n"),
    ("    const uint32_t size = input.size < out.length ? input.size : uint32_t(out.length);\n",
     concat!("    const uint32_t size = input.size < out.length ? input.size : uint32_t(out.length);\n",
             "    if (input.size > out.length) oneios_decode_cut = oneios_decode_cut + 1;   // OneiOS (CODEC_CROSVM): counted, no longer silent\n")),
    ("    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);\n    if (r || !request[0]) { failed(51, r); return false; }\n",
     r#"    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
    // OneiOS (CODEC_CROSVM): the pool every session shares may be short (crosvm answers ENOMEM): the codec's minimum, then 6
    // (crosvm copies each picture out and gives the codec its buffer back, so fewer of them slow a session, not stop it)
    {
        const unsigned fewer[2] = {control[1] ? control[1] : 4u, 6u};
        for (unsigned i = 0; i < 2 && (r || !request[0]); ++i) {
            if (fewer[i] >= count) continue;
            uint32_t none[5] = {0, TypeCaptureMplane, MemoryMmap, 0, 0};
            ioctl(s, IocReqbufs, none, sizeof none, sizeof none);   // what a refused one may have left
            const uint32_t again[5] = {fewer[i], TypeCaptureMplane, MemoryMmap, 0, 0};
            memcpy(request, again, sizeof request);
            r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
            if (!r && request[0]) {
                oneios_decode_short = oneios_decode_short + 1;
                trace("decoder capture: %u buffers, not %u (the pool)", request[0], count);
            }
        }
    }
    if (r || !request[0]) { failed(51, r); return false; }
"#),
    ("    if (r) { failed(57, r); return; }\n",
     "    if (r) { failed(57, r); s->changePending = true; s->changeFits = false; return; }   // OneiOS (CODEC_CROSVM): set up again\n"),
    (r#"    if (uint64_t(stride) * f.height * 3 / 2 > smallest) { failed(58, int32_t(f.width)); return; }
    s->captureWidth = f.width;
    s->captureHeight = f.height;
    s->captureStride = stride;
    trace("decoder capture now %ux%u stride %u", f.width, f.height, stride);
}
"#,
     r#"    // OneiOS (CODEC_CROSVM): crosvm's decoder sends one CAPTURE buffer with V4L2_BUF_FLAG_LAST right behind this event and
    // writes no picture until the CAPTURE queue restarts or V4L2_DEC_CMD_START (android.rs: the session is parked): that LAST
    // buffer resumes it (resumeAfterChange), with the buffers it has when the new pictures fit them, else with a new set.
    s->changePending = true;
    s->changeFits = uint64_t(stride) * f.height * 3 / 2 <= smallest;
    if (!s->changeFits) { trace("decoder capture now %ux%u: larger than its buffers", f.width, f.height); return; }
    s->captureWidth = f.width;
    s->captureHeight = f.height;
    s->captureStride = stride;
    trace("decoder capture now %ux%u stride %u", f.width, f.height, stride);
}

// OneiOS (CODEC_CROSVM): the LAST buffer after a mid-stream SOURCE_CHANGE (a drain going on ends with it too). True when the
// buffer goes back to the queue (the session goes on with the buffers it has: V4L2_DEC_CMD_START), false when the CAPTURE
// queue was set up again at the new size (its buffers, this one among them, are gone).
bool resumeAfterChange(Session *s) {
    s->changePending = false;
    IOLockLock(s->lock);
    const bool draining = s->draining;
    s->draining = false;
    if (draining && s->pendingFrames) s->pendingFrames = 0;
    IOLockUnlock(s->lock);
    oneios_decode_resumed = oneios_decode_resumed + 1;
    if (s->changeFits) {
        const int r = decoderCommand(s, DecCmdStart);
        if (r) failed(59, r);
        return true;
    }
    oneios_decode_resized = oneios_decode_resized + 1;
    uint32_t type = TypeCaptureMplane;
    ioctl(s, IocStreamoff, &type, 4, 0);
    s->captureStreaming = false;
    for (unsigned i = 0; i < s->captureCount; ++i) unmapBuffer(s, s->capture[i]);
    uint32_t none[5] = {0, TypeCaptureMplane, MemoryMmap, 0, 0};
    ioctl(s, IocReqbufs, none, sizeof none, sizeof none);
    s->captureCount = 0;
    if (!setUpCapture(s)) failed(62, 0);
    return false;
}
"#),
    ("            if (e.flags & BufFlagLast) endDrain(s);\n            requeueCapture(s, e.index);\n",
     r#"            // OneiOS (CODEC_CROSVM): the LAST buffer of a mid-stream SOURCE_CHANGE resumes the session (resumeAfterChange)
            if ((e.flags & BufFlagLast) && s->changePending) {
                if (!resumeAfterChange(s)) continue;   // the CAPTURE queue was set up again: this buffer is gone
            } else if (e.flags & BufFlagLast) {
                endDrain(s);
            }
            requeueCapture(s, e.index);
"#),
    ("        } else if (e.kind == MediaEvtError) {\n            failed(70, 0);\n        }\n",
     r#"        } else if (e.kind == MediaEvtError) {
            failed(70, 0);
            // OneiOS (CODEC_CROSVM): crosvm's device ended the session (its codec failed): what was given comes back dropped now,
            // not at dfin's deadline, and the session takes nothing more (dfrm and dcan refuse), so VideoToolbox makes a new one.
            if (!s->deviceGone) {
                s->deviceGone = true;
                oneios_decode_lost = oneios_decode_lost + 1;
                trace("decoder session %u: ended by the device", s->v4l2);
            }
            IOLockLock(s->lock);
            while (s->inputTail != s->inputHead) {
                const Session::Input &input = s->inputs[s->inputTail++ % 128];
                IOFree(input.bytes, input.capacity);
            }
            s->pendingFrames = 0;
            IOLockUnlock(s->lock);
            dropOutstanding(s);
        }
"#),
    ("    if (!s->outputStreaming || !param(m, n, cc(\",sbf\"), sbf) || !find(sbf.value, sbf.size, cc(\"sbuf\"), sbuf) ||\n",
     concat!("    if (s->deviceGone) return -12911;   // OneiOS (CODEC_CROSVM): kVTVideoDecoderMalfunctionErr: the device ended the session\n",
             "    if (!s->outputStreaming || !param(m, n, cc(\",sbf\"), sbf) || !find(sbf.value, sbf.size, cc(\"sbuf\"), sbuf) ||\n")),
    ("    if (!s || !s->outputStreaming || !param(m, n, cc(\",fds\"), fds) || !imageDescription(fds.value, fds.size, d)) return 0;\n",
     "    if (!s || s->deviceGone || !s->outputStreaming || !param(m, n, cc(\",fds\"), fds) || !imageDescription(fds.value, fds.size, d)) return 0;   // OneiOS (CODEC_CROSVM): not a session the device ended\n"),
    // dcap from the device's menus, bounded by the pool (decoderCaps, before decoderCapabilities, which takes its answer).
    ("void decoderCapabilities(Client *c, const Header &h, const uint8_t *m, uint32_t n) {\n",
     r#"// OneiOS (CODEC_CROSVM, take18b): dcap's answer for a codec as this path decodes it on crosvm's decoder (it replaces the fixed
// list above). The profiles: those the device's profile menu lists (VIDIOC_QUERYCTRL and VIDIOC_QUERYMENU: crosvm fills the
// menus from the phone's codec store) that a session takes (8-bit 4:2:0: H.264 Baseline, Main, High; HEVC Main; VP9 profile 0;
// AV1 Main), upstream's list where the device has no menu. The level: the highest of the device's level menu (crosvm has one
// for H.264 and HEVC), at most the highest whose largest picture the media_host pool holds with a whole session
// (sessionBytes), so every level answered here is one dsta takes. Asked once per codec, in a session of its own (crosvm makes
// no codec before STREAMON).
struct DecoderCaps { uint8_t count; uint8_t profiles[3]; const char *names[3]; int level; };
struct V4l2QueryCtrl { uint32_t id, type; char name[32]; int32_t minimum, maximum, step, defaultValue; uint32_t flags, reserved[2]; };
static_assert(sizeof(V4l2QueryCtrl) == 68, "v4l2_queryctrl");
struct __attribute__((packed)) V4l2QueryMenu { uint32_t id, index; uint8_t name[32]; uint32_t reserved; };
static_assert(sizeof(V4l2QueryMenu) == 44, "v4l2_querymenu");
enum : uint32_t { IocQueryctrl = 36, IocQuerymenu = 37 };

// The items of menu control `id` the device offers (bit i: item i), 0 when it has no such menu.
uint32_t menuItems(unsigned device, uint32_t session, uint32_t id) {
    V4l2QueryCtrl q = {};
    q.id = id;
    if (droidvm_media_ioctl(device, session, IocQueryctrl, &q, sizeof q, sizeof q) || q.minimum < 0 || q.maximum > 31 ||
        q.minimum > q.maximum)
        return 0;
    uint32_t items = 0;
    for (int32_t i = q.minimum; i <= q.maximum; ++i) {
        V4l2QueryMenu menu = {};
        menu.id = id;
        menu.index = uint32_t(i);
        if (!droidvm_media_ioctl(device, session, IocQuerymenu, &menu, sizeof menu, sizeof menu)) items |= 1u << i;
    }
    return items;
}

DecoderCaps decoderCaps(int index) {
    static DecoderCaps known[4];
    static bool asked[4];
    if (__atomic_load_n(&asked[index], __ATOMIC_ACQUIRE)) return known[index];
    // V4L2's profile and level controls of each codec (v4l2-controls.h: H.264 363 and 359, HEVC 615 and 616, VP9 512 and 515,
    // AV1 655 and 656 after V4L2_CID_CODEC_BASE)
    static const uint32_t profileId[4] = {0x00990a6b, 0x00990b67, 0x00990b00, 0x00990b8f};
    static const uint32_t levelId[4] = {0x00990a67, 0x00990b68, 0x00990b03, 0x00990b90};
    static const uint32_t codec[4] = {cc("avc1"), cc("hvc1"), cc("vp09"), cc("av01")};
    // each level menu item as VideoToolbox numbers it (level_idc; HEVC's general_level_idc; VP9's level x 10; AV1's
    // seq_level_idx, the item itself) and its largest picture in luma samples (H.264's MaxFS x 256, HEVC's and VP9's
    // MaxLumaPs, AV1's MaxPicSize; 0: a level AV1 leaves undefined)
    static const uint8_t vt[3][20] = {{10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62},
                                      {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186},
                                      {10, 11, 20, 21, 30, 31, 40, 41, 50, 51, 52, 60, 61, 62}};
    static const uint8_t levelCount[4] = {20, 13, 14, 20};
    static const uint32_t samples[4][20] = {
        {99 * 256, 99 * 256, 396 * 256, 396 * 256, 396 * 256, 396 * 256, 792 * 256, 1620 * 256, 1620 * 256, 3600 * 256, 5120 * 256,
         8192 * 256, 8192 * 256, 8704 * 256, 22080 * 256, 36864 * 256, 36864 * 256, 139264 * 256, 139264 * 256, 139264 * 256},
        {36864, 122880, 245760, 552960, 983040, 2228224, 2228224, 8912896, 8912896, 8912896, 35651584, 35651584, 35651584},
        {36864, 73728, 122880, 245760, 552960, 983040, 2228224, 2228224, 8912896, 8912896, 8912896, 35651584, 35651584, 35651584},
        {147456, 278784, 0, 0, 665856, 1065024, 0, 0, 2359296, 2359296, 0, 0, 8912896, 8912896, 8912896, 8912896, 35651584,
         35651584, 35651584, 35651584}};
    const unsigned device = decoderDevice();
    uint32_t profiles = 0, levels = 0, session = 0;
    if (device != ~0u && droidvm_media_open(device, &session) == 0) {
        V4l2Format f = {};
        f.type = TypeOutputMplane;
        f.width = 1280;
        f.height = 720;
        f.pixelformat = decoderFourcc(codec[index]);
        f.num_planes = 1;
        f.planes[0].sizeimage = OutputBytes;
        droidvm_media_ioctl(device, session, IocSFmt, &f, sizeof f, sizeof f);   // the codec's own controls, should they follow it
        profiles = menuItems(device, session, profileId[index]);
        levels = menuItems(device, session, levelId[index]);
        droidvm_media_close(device, session);
    }
    DecoderCaps c = {};
    auto add = [&](uint8_t profile, const char *name) { c.profiles[c.count] = profile; c.names[c.count++] = name; };
    if (index == 0) {   // H.264: Baseline or Constrained Baseline, Main, High or Constrained High (V4L2 items 0 1, 2, 4 17)
        if (!profiles || (profiles & 3)) add(66, "66");
        if (!profiles || (profiles & 4)) add(77, "77");
        if (!profiles || (profiles & (1u << 4 | 1u << 17))) add(100, "100");
    } else if (index == 1) {   // HEVC Main (item 0); Main 10 comes out of crosvm's decoder as no picture
        if (!profiles || (profiles & 1)) add(1, "1");
    } else if (!profiles || (profiles & 1)) {   // VP9 profile 0, AV1 Main (item 0)
        add(0, "0");
    }
    // the level: the device's highest, at most the pool's (the highest whose largest picture a session's buffers fit)
    const uint64_t pool = droidvm_media_pool_size(device);
    int top = -1, room = 0;
    for (int i = 31; i >= 0 && top < 0; --i) if (levels & (1u << i)) top = i;
    for (int i = 0; i < levelCount[index]; ++i) if (samples[index][i] && sessionBytes(samples[index][i]) <= pool) room = i;
    const int item = top >= 0 && top < room ? top : room;
    c.level = index == 3 ? item : vt[index][item];
    trace("decoder %c%c%c%c caps: profile items %#x level items %#x; %u profiles at level %d (pool %llu MiB)",
          char(codec[index] >> 24), char(codec[index] >> 16), char(codec[index] >> 8), char(codec[index]), profiles, levels,
          c.count, c.level, (unsigned long long)(pool >> 20));
    known[index] = c;
    __atomic_store_n(&asked[index], true, __ATOMIC_RELEASE);
    return c;
}

void decoderCapabilities(Client *c, const Header &h, const uint8_t *m, uint32_t n) {
"#),
    (r#"    static const struct { uint8_t count; uint8_t profiles[3]; const char *names[3]; int level; } table[4] = {
        {3, {66, 77, 100}, {"66", "77", "100"}, 51},
        {2, {1, 2}, {"1", "2"}, 153},
        {1, {0}, {"0"}, 51},
        {1, {0}, {"0"}, 13},
    };
"#,
     "    // OneiOS (CODEC_CROSVM): the profiles and level of decoderCaps, in place of upstream's fixed list\n"),
    ("        const auto &t = table[index];\n",
     "        const DecoderCaps t = decoderCaps(index);\n"),
    // A new session on the same instance (dsta tears the last one down and opens again; dinv tears down and keeps it): the
    // crosvm flags are the last session's, cleared with it, or one ERROR would refuse every later frame of the instance.
    ("    memset(s->given, 0, sizeof s->given);   // a new session's frame IDs start again\n}\n",
     concat!("    memset(s->given, 0, sizeof s->given);   // a new session's frame IDs start again\n",
             "    s->changePending = s->changeFits = s->deviceGone = false;   // OneiOS (CODEC_CROSVM): the last session's\n}\n")),
    // UniXNU-droidvm 2cb737d (BSD-2-Clause-Patent, lateautumn233; from macOS-DroidVM 58359aa): a Dbuf's surface from a pool
    // another process made. Safari's videos (WebKit's pools) came back with every lookup of the answered surface refused
    // (kIOReturnNotPermitted, "stage 41 failed: -536870174"): a task may look up by ID only what it made or what is global.
    // It is looked up as an encoder's frame is, through a send right the process's own IOSurface client makes; once a session
    // needed that, every later surface of it goes that way (one stage 41 line a session).
    ("                surface = s->surfaces && lookupSurface(s->surfaces, surfaces[0], view);\n",
     r#"                surface = s->surfaces && !s->holder && lookupSurface(s->surfaces, surfaces[0], view);
                // OneiOS (UniXNU-droidvm 2cb737d): a pool another process made (WebKit's: the task may not look it up by
                // ID, kIOReturnNotPermitted): through the process's own client, as an efrm's frame.
                if (!surface && s->surfaces) {
                    if (!s->holder) s->holder = processSurfaceClient(c->pid);
                    surface = s->holder && lookupSurface(s->surfaces, surfaces[0], view, s->holder);
                }
"#),
];
