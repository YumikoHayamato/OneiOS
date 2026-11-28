// From build-oneios-audio.py (stage_audio), written out by a helper of the port and kept by hand since.
/// DroidVMAudio.cpp's lines that change.
pub const AUDIO: &[(&str, &str)] = &[
    ("extern kmod_info_t g_kernel_kmod_info;\n",
     concat!("extern kmod_info_t g_kernel_kmod_info;\n",
             "// OneiOS (build-oneios-audio.py): the UIDs and the speaker's output latency, from boot arguments at attach.\n",
             "#include <pexpert/pexpert.h>\n",
             "static uint32_t oneios_output_latency = ", output_latency!(), ";\n",
             "static char oneios_out_uid[32] = \"Speaker\", oneios_in_uid[32] = \"Mic\";\n",
             "// OneiOS: the \"Actuator\" device (haptics): drivers/haptics/oneios_haptic.c takes its periods.\n",
             "extern \"C\" bool oneios_haptic_actuator_ready(void);\n",
             "extern \"C\" void oneios_haptic_actuator_published(bool yes);\n",
             "extern \"C\" void oneios_haptic_actuator_running(bool on);\n",
             "extern \"C\" uint32_t oneios_haptic_actuator_play(const int16_t *frames, uint32_t count, uint32_t channels);\n",
             "// OneiOS: what the Actuator's user clients do (the kinds are drivers/bridge/oneios_bridge.h's OH_EV_*).\n",
             "extern \"C\" void oneios_haptic_actuator_event(uint32_t kind, uint32_t a, uint32_t b);\n",
             "static uint32_t oneios_actuator_on = 1, oneios_actuator_channels = 1;\n",
             "static bool oneios_publishing_actuator;\n",
             "extern \"C\" { volatile uint64_t oneios_actuator_frames; }\n")),
    ("    bool isInput;\n",
     concat!("    bool isInput;\n",
             "    bool isActuator = false;   // OneiOS: the \"Actuator\" output: its periods go to drivers/haptics\n")),
    ("    uint32_t channels() const { return isInput ? InputChannels : OutputChannels; }\n",
     "    uint32_t channels() const { return isInput ? InputChannels : isActuator ? oneios_actuator_channels : OutputChannels; }\n"),
    ("        transportSet(!isInput, true);\n",
     "        if (isActuator) oneios_haptic_actuator_running(true); else transportSet(!isInput, true);\n"),
    ("        transportSet(!isInput, false);\n",
     "        if (isActuator) oneios_haptic_actuator_running(false); else transportSet(!isInput, false);\n"),
    ("            const uint32_t played = droidvm_snd_play(at, PeriodFrames);\n",
     "            const uint32_t played = isActuator ? oneios_haptic_actuator_play(at, PeriodFrames, ch) : droidvm_snd_play(at, PeriodFrames);\n"),
    ("            droidvm_audio_played = droidvm_audio_played + played;\n",
     concat!("            if (isActuator) oneios_actuator_frames = oneios_actuator_frames + played;\n",
             "            else droidvm_audio_played = droidvm_audio_played + played;\n")),
    ("        droidvm_audio_periods = droidvm_audio_periods + 1;\n",
     concat!("        if (!isActuator) droidvm_audio_periods = droidvm_audio_periods + 1;\n",
             "        if (isInput) oneios_mic_periods = oneios_mic_periods + 1;   // OneiOS (take17): the Mic's own\n")),
    ("    DroidVMAudioDevice *device = OSTypeAlloc(DroidVMAudioDevice);\n",
     concat!("    DroidVMAudioDevice *device = OSTypeAlloc(DroidVMAudioDevice);\n",
             "    if (device) device->isActuator = oneios_publishing_actuator;   // OneiOS: before initRole sizes the ring\n")),
    ("    setNumber(e, \"output latency\", Latency);\n",
     "    setNumber(e, \"output latency\", isActuator ? Latency : oneios_output_latency);   // OneiOS: the host path's latency\n"),
    (concat!("    bool speaker = publishDevice(resources, false, \"Speaker\", \"droidvm-audio-out\");\n",
             "    bool mic = publishDevice(resources, true, \"Mic\", \"droidvm-audio-in\");\n"),
     concat!("    // OneiOS: oneios-audio-out-uid, oneios-audio-in-uid (none: no microphone), oneios-audio-latency.\n",
             "    PE_parse_boot_arg_str(\"oneios-audio-out-uid\", oneios_out_uid, sizeof(oneios_out_uid));\n",
             "    PE_parse_boot_arg_str(\"oneios-audio-in-uid\", oneios_in_uid, sizeof(oneios_in_uid));\n",
             "    PE_parse_boot_argn(\"oneios-audio-latency\", &oneios_output_latency, sizeof(oneios_output_latency));\n",
             "    bool speaker = publishDevice(resources, false, oneios_out_uid, \"droidvm-audio-out\");\n",
             "    bool mic = strcmp(oneios_in_uid, \"none\") != 0 && publishDevice(resources, true, oneios_in_uid, \"droidvm-audio-in\");\n",
             "    // OneiOS: the Actuator (haptics), when drivers/haptics has the host's actuator; oneios-actuator=0 leaves it out.\n",
             "    PE_parse_boot_argn(\"oneios-actuator\", &oneios_actuator_on, sizeof(oneios_actuator_on));\n",
             "    PE_parse_boot_argn(\"oneios-actuator-channels\", &oneios_actuator_channels, sizeof(oneios_actuator_channels));\n",
             "    if (oneios_actuator_channels != 2) oneios_actuator_channels = 1;\n",
             "    oneios_publishing_actuator = true;\n",
             "    bool actuator = oneios_actuator_on && oneios_haptic_actuator_ready() && publishDevice(resources, false, \"Actuator\", \"oneios-actuator\");\n",
             "    oneios_publishing_actuator = false;\n",
             "    oneios_haptic_actuator_published(actuator);\n")),
    ("    IOLog(\"DroidVMAudio: published Speaker=%d Mic=%d as IOAudio2Devices\\n\", speaker, mic);\n",
     concat!("    IOLog(\"DroidVMAudio: published Speaker=%d Mic=%d Actuator=%d as IOAudio2Devices (UIDs %s, %s, Actuator x%u; output latency %u frames)\\n\", speaker, mic,\n",
             "          actuator, oneios_out_uid, oneios_in_uid, oneios_actuator_channels, oneios_output_latency);\n")),
    // fix-haptics (2026-11-13): the Actuator's user clients, for drivers/haptics' log lines and counter line: who opens
    // it, which selectors it gets and refuses, which format the HAL asks for, which memory it maps (OH_EV_* 0..5).
    ("    droidvm_audio_clients = droidvm_audio_clients + 1;\n",
     concat!("    droidvm_audio_clients = droidvm_audio_clients + 1;\n",
             "    if (device->isActuator) oneios_haptic_actuator_event(0, 0, 0);   // OneiOS: opened\n")),
    ("    droidvm_audio_clients = droidvm_audio_clients - 1;\n",
     concat!("    droidvm_audio_clients = droidvm_audio_clients - 1;\n",
             "    if (device && device->isActuator) oneios_haptic_actuator_event(1, 0, 0);   // OneiOS: closed\n")),
    ("    droidvm_audio_selectors = droidvm_audio_selectors + 1;\n",
     concat!("    droidvm_audio_selectors = droidvm_audio_selectors + 1;\n",
             "    if (device && device->isActuator) oneios_haptic_actuator_event(2, selector, 0);   // OneiOS: a selector\n")),
    ("        droidvm_audio_bad_selectors = droidvm_audio_bad_selectors + 1;\n",
     concat!("        droidvm_audio_bad_selectors = droidvm_audio_bad_selectors + 1;\n",
             "        if (device && device->isActuator) oneios_haptic_actuator_event(3, selector, 0);   // OneiOS: refused\n")),
    ("        if (a->scalarInputCount < 1 || a->structureInputSize != sizeof f) return kIOReturnBadArgument;\n",
     concat!("        if (a->scalarInputCount < 1 || a->structureInputSize != sizeof f) {\n",
             "            if (device->isActuator) oneios_haptic_actuator_event(3, selector, 0);   // OneiOS: refused (its size)\n",
             "            return kIOReturnBadArgument;\n",
             "        }\n")),
    ("        memcpy(&f, a->structureInput, sizeof f);\n",
     concat!("        memcpy(&f, a->structureInput, sizeof f);\n",
             "        if (device->isActuator)   // OneiOS: the format the HAL asks of the Actuator\n",
             "            oneios_haptic_actuator_event(4, f.channels | device->channels() << 16, uint32_t(f.rate >> 32));\n")),
    ("    droidvm_audio_maps = droidvm_audio_maps + 1;\n",
     concat!("    droidvm_audio_maps = droidvm_audio_maps + 1;\n",
             "    if (device->isActuator) oneios_haptic_actuator_event(5, type, 0);   // OneiOS: a mapping\n")),
    // fix-audio-latency (2026-11-15): the speaker's latency inside the guest, and the ring's wrap (every device).
    //  - iOS's I/O buffer: the HAL caps it at 0.375 of the device's "io buffer frame size" (CoreAudio's
    //    HALS_IOContextDescription::CalculateMaximumIOBufferFrameSize: the smallest ring of the context x 0.375, at
    //    most 4096), and system sounds take the cap: 3072 frames (64 ms) of the 8192-frame ring. The speaker's ring is
    //    oneios-audio-ring=FRAMES (default 1920, 8 periods: at most 720 frames, 15 ms).
    //  - The speaker hands a period on as it starts (the HAL has written it by then: its safety offset is that
    //    period, 240 frames, not 480), so the device clock is the handoff and the output latency is the host's alone.
    //  - A period that runs over the ring's end goes through a bounce buffer in two parts. 8192 is not a whole
    //    number of 240-frame periods: the period at 8160 read and zeroed (the microphone's wrote) 208 frames past the
    //    ring's buffer, and the ring's first 208 frames were never played. The zero time stamp moves at each wrap
    //    (the multiple of the ring the last period crossed), not only when a period ends on one (every 245760 frames).
    //  - The speaker engine's timing for OneiOSAudio.c's report: how late it took each period, how far ahead the HAL
    //    has written (EngineStatus +0x20), how often that came within a period of the ring's end.
    ("uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count);\n",
     concat!("uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count);\n",
             "// OneiOS (fix-audio-latency): the speaker's ring (oneios-audio-ring) and its engine's timing (OneiOSAudio.c reports it).\n",
             "uint32_t oneios_speaker_ring = 1920;\n",
             "volatile uint64_t oneios_speaker_periods, oneios_speaker_late_sum, oneios_speaker_late_max, oneios_speaker_leads,\n",
             "    oneios_speaker_lead_sum, oneios_speaker_lead_min = ~0ULL, oneios_speaker_lead_max, oneios_speaker_lead_last, oneios_speaker_tight;\n")),
    ("    uint32_t seed;\n",
     concat!("    uint32_t seed;\n",
             "    uint32_t ringFrames = RingFrames;   // OneiOS: the speaker's is oneios_speaker_ring\n",
             "    bool speakerEngine() const { return !isInput && !isActuator; }   // OneiOS: hands a period on as it starts\n",
             "    void oneiosSpeakerPeriod(uint64_t deadline, uint64_t done);\n")),
    ("    ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, RingFrames * 2 * channels(), page_size);\n",
     concat!("    ringFrames = speakerEngine() ? oneios_speaker_ring : RingFrames;   // OneiOS\n",
             "    ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, ringFrames * 2 * channels(), page_size);\n")),
    ("    setNumber(e, \"output safety offset\", SafetyOffset);\n",
     "    setNumber(e, \"output safety offset\", speakerEngine() ? PeriodFrames : SafetyOffset);   // OneiOS: written by the period's start\n"),
    ("    setNumber(e, \"io buffer frame size\", RingFrames);\n",
     "    setNumber(e, \"io buffer frame size\", ringFrames);   // OneiOS: iOS's I/O buffer is at most 0.375 of it\n"),
    ("void DroidVMAudioDevice::engineMain(void *self, wait_result_t) { static_cast<DroidVMAudioDevice *>(self)->run(); }\n",
     concat!("void DroidVMAudioDevice::engineMain(void *self, wait_result_t) { static_cast<DroidVMAudioDevice *>(self)->run(); }\n",
             "\n",
             "// OneiOS (fix-audio-latency): the speaker at the start of its period `done` (the deadline has passed): the zero time\n",
             "// stamp of a wrap the last period crossed (in the past now, as a DMA wrap's), and the timing counters: how late the\n",
             "// period was taken (us), how far ahead of it the HAL has written (its write end: iOS's I/O buffer and safety offset as\n",
             "// used), and whether that came within a period of overwriting what is not yet taken (the ring too small for them).\n",
             "void DroidVMAudioDevice::oneiosSpeakerPeriod(uint64_t deadline, uint64_t done) {\n",
             "    EngineStatus *s = status();\n",
             "    const uint64_t wrap = done / ringFrames * ringFrames;\n",
             "    if (done && wrap + PeriodFrames > done) {\n",
             "        s->hostTime = startTime + absolute(wrap);\n",
             "        __asm__ volatile(\"dmb ish\" ::: \"memory\");\n",
             "        s->sampleTime = wrap;\n",
             "        __asm__ volatile(\"dmb ish\" ::: \"memory\");\n",
             "    }\n",
             "    const uint64_t now = mach_absolute_time();\n",
             "    uint64_t late = 0;\n",
             "    if (now > deadline) absolutetime_to_nanoseconds(now - deadline, &late);\n",
             "    late /= 1000;\n",
             "    oneios_speaker_periods = oneios_speaker_periods + 1;\n",
             "    oneios_speaker_late_sum = oneios_speaker_late_sum + late;\n",
             "    if (late > oneios_speaker_late_max) oneios_speaker_late_max = late;\n",
             "    const uint64_t written = s->writeEnd;\n",
             "    oneios_speaker_lead_last = written;\n",
             "    if (written <= done) return;\n",
             "    const uint64_t lead = written - done;\n",
             "    oneios_speaker_leads = oneios_speaker_leads + 1;\n",
             "    oneios_speaker_lead_sum = oneios_speaker_lead_sum + lead;\n",
             "    if (lead < oneios_speaker_lead_min) oneios_speaker_lead_min = lead;\n",
             "    if (lead > oneios_speaker_lead_max) oneios_speaker_lead_max = lead;\n",
             "    if (lead + PeriodFrames > ringFrames) oneios_speaker_tight = oneios_speaker_tight + 1;\n",
             "}\n")),
    ("        const uint64_t deadline = startTime + absolute(done + PeriodFrames);\n",
     concat!("        // OneiOS: the speaker takes a period as it starts (the HAL has written it a safety offset of one period\n",
             "        // ahead), the others once it has passed.\n",
             "        const bool early = speakerEngine();\n",
             "        const uint64_t deadline = startTime + absolute(done + (early ? 0 : PeriodFrames));\n")),
    ("        int16_t *at = buffer + uint32_t(done % RingFrames) * ch;\n",
     concat!("        if (early) oneiosSpeakerPeriod(deadline, done);\n",
             "        // OneiOS: a period over the ring's end goes through `wrapped`, in two parts.\n",
             "        int16_t wrapped[PeriodFrames * 2];\n",
             "        const uint32_t first = uint32_t(done % ringFrames), part = ringFrames - first < PeriodFrames ? ringFrames - first : PeriodFrames;\n",
             "        int16_t *at = part == PeriodFrames ? buffer + first * ch : wrapped;\n",
             "        if (at == wrapped && !isInput) {\n",
             "            memcpy(wrapped, buffer + first * ch, part * 2 * ch);\n",
             "            memcpy(wrapped + part * ch, buffer, (PeriodFrames - part) * 2 * ch);\n",
             "        }\n")),
    ("        done += PeriodFrames;\n",
     concat!("        if (at == wrapped) {   // OneiOS: back into the ring (the microphone's frames; the speaker's silence)\n",
             "            memcpy(buffer + first * ch, wrapped, part * 2 * ch);\n",
             "            memcpy(buffer, wrapped + part * ch, (PeriodFrames - part) * 2 * ch);\n",
             "        }\n",
             "        done += PeriodFrames;\n")),
    ("        if (done % RingFrames == 0) {\n",
     concat!("        // OneiOS: at the wrap the period crossed (the speaker's is set as its next period starts).\n",
             "        const uint64_t wrap = done / ringFrames * ringFrames;\n",
             "        if (!early && wrap + PeriodFrames > done) {\n")),
    ("            s->hostTime = startTime + absolute(done);\n",
     "            s->hostTime = startTime + absolute(wrap);\n"),
    ("            s->sampleTime = done;\n",
     "            s->sampleTime = wrap;\n"),
    ("    droidvm_audio_stage = 3;\n",
     concat!("    // OneiOS: oneios-audio-ring=FRAMES, the speaker's ring (960..8192; best a whole number of 240-frame periods).\n",
             "    PE_parse_boot_argn(\"oneios-audio-ring\", &oneios_speaker_ring, sizeof(oneios_speaker_ring));\n",
             "    if (oneios_speaker_ring < 4 * PeriodFrames) oneios_speaker_ring = 4 * PeriodFrames;\n",
             "    if (oneios_speaker_ring > RingFrames) oneios_speaker_ring = RingFrames;\n",
             "    droidvm_audio_stage = 3;\n")),
    ("    droidvm_audio_stage = 4;\n",
     concat!("    droidvm_audio_stage = 4;\n",
             "    IOLog(\"DroidVMAudio: speaker: ring %u frames (iOS's I/O buffer at most %u), safety offset %u, each period handed on as it \"\n",
             "          \"starts; output latency %u frames\\n\", oneios_speaker_ring, oneios_speaker_ring * 3 / 8, (unsigned)PeriodFrames, oneios_output_latency);\n")),
    // fix-audio-volume (2026-11-22): the speaker's hardware volume. VirtualAudio's speaker route has the software volume
    // mode HardwareOnly, and a physical device without a volume control made audiomxd throw at every volume set
    // ("Volume Mode is HardwareOnly but physical device does not support HW volume", OutputVolumeControl_HAL_Common.cpp
    // :1423, 10-17 times a boot): no volume, no HUD, the keys and the slider changed nothing. The Speaker now publishes
    // a volume control in its "controls" (the HAL's IOAudio2 description, read in 24A446's CoreAudio:
    // HALS_IOA2Control::SynchronizeWithDictionary, HALS_IOA2UCDevice::SetControlValue), takes the HAL's SetControlValue
    // (selector 2: control ID and value in, the value taken out) and scales its periods by it before the transport,
    // so iOS's volume is the one that counts (the host's helper plays at full scale, runtime/bridges/audio).
    ("uint32_t oneios_speaker_ring = 1920;\n",
     concat!("uint32_t oneios_speaker_ring = 1920;\n",
             "// OneiOS (fix-audio-volume): the speaker's volume step (0..128, VirtualAudio's), its gain (Q16) and how often it changed.\n",
             "volatile uint32_t oneios_speaker_volume = 128, oneios_speaker_gain = 65536, oneios_speaker_volume_sets;\n",
             "// OneiOS (take22): the step at or under which the speaker is silent. VirtualAudio's lowest volume for this route is\n",
             "// -58 dB (step 12: \"Set decibel volume value of -58\" at the slider's zero, take21's iOS log), not step 0, and its\n",
             "// own mute (a software volume command to a DSP) has nothing to act on here, so volume 0 was still heard: the host\n",
             "// plays at full scale. oneios-audio-zero-step=N moves the step (0: only step 0 is silent, as before take22).\n",
             "uint32_t oneios_speaker_zero_step = 12;\n")),
    ("uint64_t absolute(uint64_t frames) { uint64_t t; nanoseconds_to_absolutetime(frames * 1000000000ULL / Rate, &t); return t; }\n",
     concat!("uint64_t absolute(uint64_t frames) { uint64_t t; nanoseconds_to_absolutetime(frames * 1000000000ULL / Rate, &t); return t; }\n",
             "// OneiOS (fix-audio-volume): the speaker's volume control as the HAL reads it (24A446 CoreAudio's HALS_IOA2Control):\n",
             "// class 'vlme', base class 'levl', output scope, main element; integers 0..128 over one range from -64 dB in 0.5 dB\n",
             "// steps (dB values Fixed64, as the sample rates). Step 0 is silence, 128 is 0 dB (the stream as iOS made it).\n",
             "enum : uint32_t { OneiosVolumeID = 1, OneiosVolumeSteps = 128 };\n",
             "const uint32_t oneios_volume_gain[OneiosVolumeSteps + 1] = {   // Q16: 65536 x 10^((step / 2 - 64) / 20), 0 at step 0\n",
             "    0, 44, 46, 49, 52, 55, 58, 62, 66, 69, 74, 78, 83, 87, 93, 98,\n",
             "    104, 110, 117, 123, 131, 139, 147, 155, 165, 174, 185, 196, 207, 220, 233, 246,\n",
             "    261, 276, 293, 310, 328, 348, 369, 390, 414, 438, 464, 491, 521, 551, 584, 619,\n",
             "    655, 694, 735, 779, 825, 874, 926, 981, 1039, 1100, 1165, 1234, 1308, 1385, 1467, 1554,\n",
             "    1646, 1744, 1847, 1957, 2072, 2195, 2325, 2463, 2609, 2764, 2927, 3101, 3285, 3479, 3685, 3904,\n",
             "    4135, 4380, 4640, 4915, 5206, 5514, 5841, 6187, 6554, 6942, 7353, 7789, 8250, 8739, 9257, 9806,\n",
             "    10387, 11002, 11654, 12345, 13076, 13851, 14672, 15541, 16462, 17437, 18471, 19565, 20724, 21952, 23253, 24631,\n",
             "    26090, 27636, 29274, 31008, 32846, 34792, 36854, 39037, 41350, 43801, 46396, 49145, 52057, 55142, 58409, 61870,\n",
             "    65536,\n",
             "};\n",
             "OSArray *oneiosControls(bool volume) {\n",
             "    OSArray *list = OSArray::withCapacity(1);\n",
             "    if (!list || !volume) return list;\n",
             "    OSDictionary *c = OSDictionary::withCapacity(12), *range = OSDictionary::withCapacity(4);\n",
             "    OSArray *map = OSArray::withCapacity(1);\n",
             "    if (c && range && map) {\n",
             "        setNumber(c, \"control ID\", OneiosVolumeID);\n",
             "        setNumber(c, \"base class\", 0x6c65766c);   // 'levl'\n",
             "        setNumber(c, \"class\", 0x766c6d65);        // 'vlme'\n",
             "        setNumber(c, \"scope\", 0x6f757470);        // 'outp'\n",
             "        setNumber(c, \"element\", 0);\n",
             "        c->setObject(\"read only\", kOSBooleanFalse);\n",
             "        setNumber(c, \"value\", oneios_speaker_volume);\n",
             "        setNumber(c, \"minimum value\", 0);\n",
             "        setNumber(c, \"maximum value\", OneiosVolumeSteps);\n",
             "        setNumber(range, \"start int value\", 0);\n",
             "        setNumber(range, \"start db value\", uint64_t(int64_t(-64) * (int64_t(1) << 32)), 64);\n",
             "        setNumber(range, \"integer steps\", OneiosVolumeSteps);\n",
             "        setNumber(range, \"db per step\", uint64_t(1) << 31, 64);   // 0.5 dB\n",
             "        map->setObject(range);\n",
             "        c->setObject(\"range map\", map);\n",
             "        list->setObject(c);\n",
             "    }\n",
             "    OSSafeReleaseNULL(c); OSSafeReleaseNULL(range); OSSafeReleaseNULL(map);\n",
             "    return list;\n",
             "}\n",
             "// One period of the speaker at the volume's gain, in place (480 multiplies a 5 ms period; nothing at 0 dB).\n",
             "void oneiosVolume(int16_t *s, uint32_t n) {\n",
             "    const int32_t g = int32_t(oneios_speaker_gain);\n",
             "    if (g >= 65536) return;\n",
             "    for (uint32_t i = 0; i < n; i++) s[i] = int16_t((int32_t(s[i]) * g) >> 16);\n",
             "}\n",
             "// OneiOS (take18): the Mic's input data sources, as an iPhone's built-in microphone has them. VirtualAudio's route\n",
             "// for recording (RoutingHandler_PlaybackAndRecord_GenericConfig1) sets the built-in mic's sub-ports through the\n",
             "// device's input data sources (SubPortPolicy: AudioObjectGetPropertyDataSize of ['ssrc', 'inpt', 0]) and gives the\n",
             "// whole route up when the device has none (take17: \"Error getting data size of property [ssrc/inpt/0] on device 37\",\n",
             "// then \"Routing is not supported\", 30 times): Voice Memos' engine saw an input of 0 Hz (AURemoteIO -10851) and\n",
             "// Safari's capture did not start (-66637), and the HAL never asked the Mic for StartIO. 24A446's HAL serves 'ssrc'\n",
             "// from the scope's data source control ('dsrc', a selector: HALB_Info::TranslateDeviceAddressToControlAddress), whose\n",
             "// items are its \"selectors\" ({\"value\": ID, \"name\": text}) and, a multi-selector, its \"value\" an array of the\n",
             "// selected IDs (HALS_IOA2UCDevice::CopySelectorControlInfo_SelectorMap/_MultiSelectorValue). VirtualAudio reads and\n",
             "// sets a list (the size over 4). The IDs are an iPhone's microphones as VirtualAudio's built-in mic tables name them\n",
             "// ('mc01' bottom, 'mc02' front, 'mc03' back, 'mc04' a second bottom one; 1835216945.. as AVAudioSession shows them):\n",
             "// every one of them is the one mono stream here. The HAL changes the selection by SetControlValue (selector 2) or\n",
             "// SetMultiControlValue (selector 8: the control ID and the IDs in, the IDs taken out), and every ID asked is taken.\n",
             "enum : uint32_t { OneiosMicSourceID = 2, OneiosMicSources = 4 };\n",
             "const uint32_t oneios_mic_source_ids[OneiosMicSources] = { 0x6d633031, 0x6d633032, 0x6d633033, 0x6d633034 };   // 'mc01'..'mc04'\n",
             "const char *const oneios_mic_source_names[OneiosMicSources] = { \"Bottom\", \"Front\", \"Back\", \"Bottom 2\" };\n",
             "volatile uint32_t oneios_mic_selected[OneiosMicSources] = { 0x6d633031 }, oneios_mic_selected_count = 1, oneios_mic_source_sets;\n",
             "OSArray *oneiosMicControls() {\n",
             "    OSArray *list = OSArray::withCapacity(1), *items = OSArray::withCapacity(OneiosMicSources), *value = OSArray::withCapacity(OneiosMicSources);\n",
             "    OSDictionary *c = OSDictionary::withCapacity(12);\n",
             "    if (list && items && value && c) {\n",
             "        setNumber(c, \"control ID\", OneiosMicSourceID);\n",
             "        setNumber(c, \"base class\", 0x736c6374);   // 'slct'\n",
             "        setNumber(c, \"class\", 0x64737263);        // 'dsrc'\n",
             "        setNumber(c, \"scope\", 0x696e7074);        // 'inpt'\n",
             "        setNumber(c, \"element\", 0);\n",
             "        c->setObject(\"read only\", kOSBooleanFalse);\n",
             "        c->setObject(\"multi-selector\", kOSBooleanTrue);\n",
             "        for (uint32_t i = 0; i < OneiosMicSources; i++) {\n",
             "            OSDictionary *item = OSDictionary::withCapacity(2);\n",
             "            OSString *name = OSString::withCString(oneios_mic_source_names[i]);\n",
             "            if (item && name) { setNumber(item, \"value\", oneios_mic_source_ids[i]); item->setObject(\"name\", name); items->setObject(item); }\n",
             "            OSSafeReleaseNULL(item); OSSafeReleaseNULL(name);\n",
             "        }\n",
             "        const uint32_t n = oneios_mic_selected_count < OneiosMicSources ? oneios_mic_selected_count : OneiosMicSources;\n",
             "        for (uint32_t i = 0; i < n; i++)\n",
             "            if (OSNumber *id = OSNumber::withNumber(uint64_t(oneios_mic_selected[i]), 32)) { value->setObject(id); id->release(); }\n",
             "        c->setObject(\"selectors\", items);\n",
             "        c->setObject(\"value\", value);\n",
             "        list->setObject(c);\n",
             "    }\n",
             "    OSSafeReleaseNULL(items); OSSafeReleaseNULL(value); OSSafeReleaseNULL(c);\n",
             "    return list;\n",
             "}\n")),
    ("    if (OSArray *a = OSArray::withCapacity(1)) { e->setProperty(\"controls\", a); a->release(); }\n",
     concat!("    // OneiOS: the speaker's volume; the Mic's input data sources (take18)\n",
             "    if (OSArray *a = isInput ? oneiosMicControls() : oneiosControls(speakerEngine())) { e->setProperty(\"controls\", a); a->release(); }\n")),
    ("    void oneiosSpeakerPeriod(uint64_t deadline, uint64_t done);\n",
     concat!("    void oneiosSpeakerPeriod(uint64_t deadline, uint64_t done);\n",
             "    uint32_t oneiosSetVolume(uint64_t value, IORegistryEntry *client);   // OneiOS: the speaker's volume control\n",
             "    uint32_t oneiosSetMicSources(const uint32_t *ids, uint32_t count, IORegistryEntry *client);   // OneiOS (take18)\n")),
    ("    if (lead + PeriodFrames > ringFrames) oneios_speaker_tight = oneios_speaker_tight + 1;\n}\n",
     concat!("    if (lead + PeriodFrames > ringFrames) oneios_speaker_tight = oneios_speaker_tight + 1;\n}\n",
             "\n",
             "// OneiOS (fix-audio-volume): VirtualAudio's volume through the HAL's SetControlValue: the step (clamped to 0..128), its\n",
             "// gain for the engine, and the description again (on the device and the client: the HAL reads it through both). The\n",
             "// first 16 changes are logged, then every 64th.\n",
             "uint32_t DroidVMAudioDevice::oneiosSetVolume(uint64_t value, IORegistryEntry *client) {\n",
             "    const int32_t asked = int32_t(uint32_t(value));\n",
             "    const uint32_t v = asked < 0 ? 0 : asked > int32_t(OneiosVolumeSteps) ? OneiosVolumeSteps : uint32_t(asked);\n",
             "    const bool changed = v != oneios_speaker_volume;\n",
             "    static bool zeroRead;\n",
             "    if (!zeroRead) {\n",
             "        zeroRead = true;\n",
             "        PE_parse_boot_argn(\"oneios-audio-zero-step\", &oneios_speaker_zero_step, sizeof(oneios_speaker_zero_step));\n",
             "        if (oneios_speaker_zero_step >= OneiosVolumeSteps) oneios_speaker_zero_step = OneiosVolumeSteps - 1;\n",
             "    }\n",
             "    oneios_speaker_gain = v <= oneios_speaker_zero_step ? 0 : oneios_volume_gain[v];\n",
             "    oneios_speaker_volume = v;\n",
             "    if (OSArray *a = oneiosControls(true)) {\n",
             "        setProperty(\"controls\", a);\n",
             "        if (client) client->setProperty(\"controls\", a);\n",
             "        a->release();\n",
             "    }\n",
             "    if (changed) {\n",
             "        const uint32_t n = oneios_speaker_volume_sets + 1;   // no read of a volatile assignment (C++20)\n",
             "        oneios_speaker_volume_sets = n;\n",
             "        const uint32_t down = OneiosVolumeSteps - v;   // half dB below 0 dB\n",
             "        if (n <= 16 || n % 64 == 0)\n",
             "            IOLog(\"DroidVMAudio: speaker volume %u of %u (%s%u.%u dB, gain %u of 65536), change %u: iOS's volume, applied here \"\n",
             "                  \"(the host plays at full scale)\\n\", v, (unsigned)OneiosVolumeSteps, oneios_speaker_gain == 0 ? \"silent, -\" : down ? \"-\" : \"\",\n",
             "                  down / 2, down % 2 * 5, oneios_speaker_gain, n);\n",
             "    }\n",
             "    return v;\n",
             "}\n",
             "\n",
             "// OneiOS (take18): the Mic's selected input data sources, as the HAL sets them (SetControlValue: one; SetMultiControlValue:\n",
             "// a list, at most 4 kept). Every ID is taken, also one the control's items do not name: they are all the same mono\n",
             "// stream here, and a refusal would end VirtualAudio's route as the missing property did. The description again on the\n",
             "// device and the client (the HAL reads it through both). The first 16 changes are said with the caller, then every 64th.\n",
             "uint32_t DroidVMAudioDevice::oneiosSetMicSources(const uint32_t *ids, uint32_t count, IORegistryEntry *client) {\n",
             "    const uint32_t n = count < OneiosMicSources ? count : OneiosMicSources;\n",
             "    bool changed = n != oneios_mic_selected_count;\n",
             "    for (uint32_t i = 0; i < n; i++) {\n",
             "        if (oneios_mic_selected[i] != ids[i]) changed = true;\n",
             "        oneios_mic_selected[i] = ids[i];\n",
             "    }\n",
             "    oneios_mic_selected_count = n;\n",
             "    if (OSArray *a = oneiosMicControls()) {\n",
             "        setProperty(\"controls\", a);\n",
             "        if (client) client->setProperty(\"controls\", a);\n",
             "        a->release();\n",
             "    }\n",
             "    const uint32_t sets = oneios_mic_source_sets + 1;   // no read of a volatile assignment (C++20)\n",
             "    oneios_mic_source_sets = sets;\n",
             "    if (changed && (sets <= 16 || sets % 64 == 0)) {\n",
             "        char list[OneiosMicSources * 5 + 1], who[24] = \"?\";   // 'mc01 mc02 ...'\n",
             "        uint32_t at = 0;\n",
             "        for (uint32_t i = 0; i < n; i++) {\n",
             "            if (i) list[at++] = ' ';\n",
             "            for (int shift = 24; shift >= 0; shift -= 8) {\n",
             "                const char c = char(ids[i] >> shift);\n",
             "                list[at++] = c >= ' ' && c <= '~' ? c : '?';\n",
             "            }\n",
             "        }\n",
             "        list[at] = 0;\n",
             "        proc_selfname(who, sizeof who);\n",
             "        IOLog(\"DroidVMAudio: mic: data sources %s (%u) set by %s[%d], set %u: all of them the one mono stream\\n\",\n",
             "              n ? list : \"none\", n, who, proc_selfpid(), sets);\n",
             "    }\n",
             "    return n;\n",
             "}\n")),
    (concat!("    case 2:                                                // SetControlValue: no controls are published\n",
             "        return kIOReturnBadArgument;\n"),
     concat!("    case 2: {                                              // SetControlValue: the speaker's volume (OneiOS)\n",
             "        if (device->isInput && a->scalarInputCount >= 2 && a->scalarInput[0] == OneiosMicSourceID) {   // the Mic's data source (take18)\n",
             "            const uint32_t id = uint32_t(a->scalarInput[1]);\n",
             "            device->oneiosSetMicSources(&id, 1, this);\n",
             "            if (a->scalarOutputCount >= 1) { a->scalarOutput[0] = id; a->scalarOutputCount = 1; }\n",
             "            return kIOReturnSuccess;\n",
             "        }\n",
             "        if (!device->speakerEngine() || a->scalarInputCount < 2 || a->scalarInput[0] != OneiosVolumeID) return kIOReturnBadArgument;\n",
             "        const uint32_t v = device->oneiosSetVolume(a->scalarInput[1], this);\n",
             "        if (a->scalarOutputCount >= 1) { a->scalarOutput[0] = v; a->scalarOutputCount = 1; }\n",
             "        return kIOReturnSuccess;\n",
             "    }\n")),
    // take18 (2026-11-26): SetMultiControlValue (selector 8; HALS_IOA2UCDevice::SetMultiControlValue: no scalars, the control
    // ID and the values as the input structure, the values taken as the output one), for the Mic's multi-selector.
    ("    case 6: case 12:                                       // SetStreamActive, SetClientDescription\n",
     concat!("    case 8: {                                              // SetMultiControlValue: the Mic's data sources (OneiOS take18)\n",
             "        const uint32_t *in = static_cast<const uint32_t *>(a->structureInput);\n",
             "        if (!device->isInput || !in || a->structureInputSize < 4 || a->structureInputSize % 4 || in[0] != OneiosMicSourceID)\n",
             "            return kIOReturnBadArgument;\n",
             "        const uint32_t n = device->oneiosSetMicSources(in + 1, a->structureInputSize / 4 - 1, this);\n",
             "        if (uint32_t *out = static_cast<uint32_t *>(a->structureOutput)) {\n",
             "            const uint32_t room = a->structureOutputSize / 4, k = n < room ? n : room;\n",
             "            for (uint32_t i = 0; i < k; i++) out[i] = in[1 + i];\n",
             "            a->structureOutputSize = k * 4;\n",
             "        }\n",
             "        return kIOReturnSuccess;\n",
             "    }\n",
             "    case 6: case 12:                                       // SetStreamActive, SetClientDescription\n")),
    ("            const uint32_t played = isActuator ? oneios_haptic_actuator_play(at, PeriodFrames, ch) : droidvm_snd_play(at, PeriodFrames);\n",
     concat!("            if (!isActuator) oneiosVolume(at, PeriodFrames * ch);   // OneiOS: iOS's volume (the speaker's control)\n",
             "            const uint32_t played = isActuator ? oneios_haptic_actuator_play(at, PeriodFrames, ch) : droidvm_snd_play(at, PeriodFrames);\n")),
    // take13 (2026-11-25): the user clients' other calls, counted for OneiOSAudio.c's 10 s line (kernel patch 0084's report).
    ("volatile int droidvm_audio_family_status = -1, droidvm_audio_hog_owner = -1;\n",
     concat!("volatile int droidvm_audio_family_status = -1, droidvm_audio_hog_owner = -1;\n",
             "volatile uint64_t oneios_audio_opens, oneios_audio_properties, oneios_audio_ports, oneios_audio_bad_maps;   // OneiOS\n",
             "// OneiOS (take17): the Mic's StartIOs and StopIOs, its engine's own periods, and the formats and rates a device refused.\n",
             "volatile uint64_t oneios_mic_ios[2], oneios_mic_periods, oneios_audio_refused_formats;\n",
             "// OneiOS (take17): what touches the Mic device at all: user clients opened [0] and closed [1], selectors [2] (a bit for\n",
             "// each selector seen in [5]), mappings [3] and property sets [4].\n",
             "volatile uint64_t oneios_mic_touch[6];\n")),
    ("    droidvm_audio_clients = droidvm_audio_clients + 1;\n",
     concat!("    droidvm_audio_clients = droidvm_audio_clients + 1;\n",
             "    oneios_audio_opens = oneios_audio_opens + 1;   // OneiOS\n")),
    ("    if (!m) return kIOReturnBadArgument;\n",
     "    if (!m) { oneios_audio_bad_maps = oneios_audio_bad_maps + 1; return kIOReturnBadArgument; }   // OneiOS: counted\n"),
    ("IOReturn DroidVMAudioUserClient::setProperties(OSObject *properties) {\n",
     concat!("IOReturn DroidVMAudioUserClient::setProperties(OSObject *properties) {\n",
             "    oneios_audio_properties = oneios_audio_properties + 1;   // OneiOS\n")),
    ("IOReturn DroidVMAudioUserClient::registerNotificationPort(mach_port_t, UInt32, io_user_reference_t) {\n",
     concat!("IOReturn DroidVMAudioUserClient::registerNotificationPort(mach_port_t, UInt32, io_user_reference_t) {\n",
             "    oneios_audio_ports = oneios_audio_ports + 1;   // OneiOS\n")),
    // take17 (2026-11-26): what the HAL asks of the microphone. In take16 Voice Memos recorded nothing and the report could not
    // say whether iOS had started the Mic device at all (the engines' periods were one count; they were all the speaker's).
    // Each StartIO and StopIO of the Mic is a line with its caller and its answer (the first 24, then every 32nd), the Mic's
    // engine has its own period count (the entry of droidvm_audio_periods above), and a sample rate or a format a device
    // refuses is counted with the refused selectors and said (the first 16): the HAL gives a route up without a word here.
    ("OSDefineMetaClassAndStructors(DroidVMAudioUserClient, IOUserClient)\n",
     concat!("OSDefineMetaClassAndStructors(DroidVMAudioUserClient, IOUserClient)\n",
             "\n",
             "// OneiOS (take17): the Mic's StartIO and StopIO, each with its caller and answer (the first 24, then every 32nd), and a\n",
             "// sample rate or format a device refuses (counted with the refused selectors; the first 16 are said).\n",
             "extern \"C\" { void proc_selfname(char *, int); int proc_selfpid(void); }\n",
             "static void oneiosMicIO(bool start, uint32_t selector, IOReturn result) {\n",
             "    const uint64_t n = oneios_mic_ios[start ? 0 : 1] + 1;\n",
             "    oneios_mic_ios[start ? 0 : 1] = n;\n",
             "    if (n > 24 && n % 32) return;\n",
             "    char who[24] = \"?\";\n",
             "    proc_selfname(who, sizeof who);\n",
             "    IOLog(\"DroidVMAudio: mic: %s %llu (selector %u) by %s[%d]: %#x\\n\", start ? \"StartIO\" : \"StopIO\", n, selector, who,\n",
             "          proc_selfpid(), result);\n",
             "}\n",
             "static void oneiosRefused(DroidVMAudioDevice *device, uint32_t selector, uint32_t hz, uint32_t channels, uint32_t bits) {\n",
             "    droidvm_audio_bad_selectors = droidvm_audio_bad_selectors + 1;\n",
             "    const uint64_t n = oneios_audio_refused_formats + 1;\n",
             "    oneios_audio_refused_formats = n;\n",
             "    if (n > 16) return;\n",
             "    if (selector == 4)\n",
             "        IOLog(\"DroidVMAudio: %s: a sample rate of %u Hz asked: refused (the device has %u Hz alone)\\n\", device->uid, hz, unsigned(Rate));\n",
             "    else\n",
             "        IOLog(\"DroidVMAudio: %s: a format of %u channel(s) of %u bits at %u Hz asked: refused (the device has %u of 16 bits at %u Hz)\\n\",\n",
             "              device->uid, channels, bits, hz, device->channels(), unsigned(Rate));\n",
             "}\n",
             "// What touches the Mic device: counted (oneios_mic_touch), its opens and closes said with their caller (the first 12).\n",
             "static void oneiosMicTouch(uint32_t kind, uint32_t value) {\n",
             "    const uint64_t n = oneios_mic_touch[kind] + 1;\n",
             "    oneios_mic_touch[kind] = n;\n",
             "    if (kind == 2) oneios_mic_touch[5] = oneios_mic_touch[5] | 1ull << (value < 63 ? value : 63);\n",
             "    if (kind > 1 || n > 12) return;\n",
             "    char who[24] = \"?\";\n",
             "    proc_selfname(who, sizeof who);\n",
             "    IOLog(\"DroidVMAudio: mic: %s %llu by %s[%d]\\n\", kind ? \"closed\" : \"opened\", n, who, proc_selfpid());\n",
             "}\n")),
    // The Mic device's user clients, selectors, mappings and property sets (take17). take16's lines had "3 clients (+3 opened)"
    // and "+7 selectors, +11 maps" for the three devices together: whether the HAL had opened the Mic and what it asked of it
    // before giving it up could not be read. The anchors are the Actuator's own event lines (above).
    ("    if (device->isActuator) oneios_haptic_actuator_event(0, 0, 0);   // OneiOS: opened\n",
     concat!("    if (device->isActuator) oneios_haptic_actuator_event(0, 0, 0);   // OneiOS: opened\n",
             "    if (device->isInput) oneiosMicTouch(0, 0);   // OneiOS (take17): the Mic's\n")),
    ("    if (device && device->isActuator) oneios_haptic_actuator_event(1, 0, 0);   // OneiOS: closed\n",
     concat!("    if (device && device->isActuator) oneios_haptic_actuator_event(1, 0, 0);   // OneiOS: closed\n",
             "    if (device && device->isInput) oneiosMicTouch(1, 0);\n")),
    ("    if (device && device->isActuator) oneios_haptic_actuator_event(2, selector, 0);   // OneiOS: a selector\n",
     concat!("    if (device && device->isActuator) oneios_haptic_actuator_event(2, selector, 0);   // OneiOS: a selector\n",
             "    if (device && device->isInput) oneiosMicTouch(2, selector);\n")),
    ("    if (device->isActuator) oneios_haptic_actuator_event(5, type, 0);   // OneiOS: a mapping\n",
     concat!("    if (device->isActuator) oneios_haptic_actuator_event(5, type, 0);   // OneiOS: a mapping\n",
             "    if (device->isInput) oneiosMicTouch(3, type);\n")),
    ("    oneios_audio_properties = oneios_audio_properties + 1;   // OneiOS\n",
     concat!("    oneios_audio_properties = oneios_audio_properties + 1;   // OneiOS\n",
             "    if (device && device->isInput) oneiosMicTouch(4, 0);\n")),
    ("        IOReturn r = device->startIO();\n",
     concat!("        IOReturn r = device->startIO();\n",
             "        if (device->isInput) oneiosMicIO(true, selector, r);   // OneiOS (take17)\n")),
    ("        startedIO = false;\n        return device->stopIO();\n",
     concat!("        startedIO = false;\n",
             "        {   // OneiOS (take17): the Mic's stops are said\n",
             "            const IOReturn r = device->stopIO();\n",
             "            if (device->isInput) oneiosMicIO(false, selector, r);\n",
             "            return r;\n",
             "        }\n")),
    ("        return rate == fixed64(Rate) ? kIOReturnSuccess : kIOReturnUnsupported;\n",
     concat!("        if (rate != fixed64(Rate)) oneiosRefused(device, selector, uint32_t(uint64_t(rate) >> 32), 0, 0);   // OneiOS (take17)\n",
             "        return rate == fixed64(Rate) ? kIOReturnSuccess : kIOReturnUnsupported;\n")),
    (concat!("        return f.rate == fixed64(Rate) && f.id == FormatLinearPCM && f.channels == device->channels() && f.bits == 16\n",
             "            ? kIOReturnSuccess : kIOReturnUnsupported;\n"),
     concat!("        if (!(f.rate == fixed64(Rate) && f.id == FormatLinearPCM && f.channels == device->channels() && f.bits == 16))   // OneiOS (take17)\n",
             "            oneiosRefused(device, selector, uint32_t(uint64_t(f.rate) >> 32), f.channels, f.bits);\n",
             "        return f.rate == fixed64(Rate) && f.id == FormatLinearPCM && f.channels == device->channels() && f.bits == 16\n",
             "            ? kIOReturnSuccess : kIOReturnUnsupported;\n")),
];

/// The native kernel's virtio-snd on crosvm's PCI (xnuloader-crosvm --audio): Droid-VM's crosvm has no virtio-mmio sound
/// card, so the card is a virtio-pci function of its ECAM host, and crosvm picks its device (`--virtio-snd` has no
/// pci-address). /arm-io/virtio-snd ("oneios,virtio-snd-pci") is all of bus 0, and the transport takes the first
/// 1af4:1059 function there. DroidVMVirtioSnd.c's lines that change for it, compiled only with XNU_ONEIOS_NATIVE: the
/// MMIO transport's register offsets go to the function's regions (VirtIO 1.2 4.1.4), as the GPU's do (gpu.rs
/// TRANSPORT_PCI), with the device configuration (the stream count at 0x104) and the four queues' notifications. The
/// queues and periods stay in the transport's DMA region of the guest's RAM, which the shared-RAM VM's host reads; polled,
/// as before. The hosted kernel compiles the file as before.
pub const SND_TRANSPORT_PCI: &[(&str, &str)] = &[
    (r#"// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t snd_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(snd.registers + offset) : "memory");
    return value;
}
static void snd_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(snd.registers + offset) : "memory");
}
"#,
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/audio.rs SND_TRANSPORT_PCI): the card as a virtio-pci function of crosvm's ECAM host
// (/arm-io/virtio-snd "oneios,virtio-snd-pci" is bus 0, xnuloader-crosvm --audio), the MMIO transport's offsets
// (VirtIO 1.2 4.2.2) on its regions (4.1.4), as the GPU's (staged/gpu.rs TRANSPORT_PCI). Single loads and stores with a
// plain base register, as the MMIO accessors.
extern int printf(const char *, ...);
static bool snd_pci;
static uintptr_t snd_pci_common, snd_pci_notify, snd_pci_device;
static uint32_t snd_pci_multiplier;
static uint16_t snd_pci_notify_off[4];
static uint32_t snd_pci_rd32(uintptr_t p) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static uint16_t snd_pci_rd16(uintptr_t p) { uint32_t v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint16_t)v; }
static uint8_t snd_pci_rd8(uintptr_t p) { uint32_t v; __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint8_t)v; }
static void snd_pci_wr32(uintptr_t p, uint32_t v) { __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(p) : "memory"); }
static void snd_pci_wr16(uintptr_t p, uint16_t v) { __asm__ volatile("strh %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static void snd_pci_wr8(uintptr_t p, uint8_t v) { __asm__ volatile("strb %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static uint32_t snd_pci_read(unsigned offset) {
    if (offset >= 0x100) return snd_pci_rd32(snd_pci_device + (offset - 0x100));   // the card's configuration
    switch (offset) {
    case 0x00: return 0x74726976;                                // MagicValue, Version, DeviceID: the function is 1af4:1059
    case 0x04: return 2;
    case 0x08: return 25;
    case 0x10: return snd_pci_rd32(snd_pci_common + 0x04);       // DeviceFeatures, of the word DeviceFeaturesSel chose
    case 0x34: return snd_pci_rd16(snd_pci_common + 0x18);       // QueueNumMax: the selected queue's size until it is set
    case 0x44: return snd_pci_rd16(snd_pci_common + 0x1c);       // QueueReady: queue_enable
    case 0x70: return snd_pci_rd8(snd_pci_common + 0x14);        // Status
    default: return 0;
    }
}
static void snd_pci_write(unsigned offset, uint32_t value) {
    switch (offset) {
    case 0x14: snd_pci_wr32(snd_pci_common + 0x00, value); break;   // DeviceFeaturesSel
    case 0x20: snd_pci_wr32(snd_pci_common + 0x0c, value); break;   // DriverFeatures (the driver keeps ACCESS_PLATFORM)
    case 0x24: snd_pci_wr32(snd_pci_common + 0x08, value); break;   // DriverFeaturesSel
    case 0x30:                                                      // QueueSel, and that queue's notification offset
        snd_pci_wr16(snd_pci_common + 0x16, (uint16_t)value);
        if (value < 4) snd_pci_notify_off[value] = snd_pci_rd16(snd_pci_common + 0x1e);
        break;
    case 0x38: snd_pci_wr16(snd_pci_common + 0x18, (uint16_t)value); break;
    case 0x44: snd_pci_wr16(snd_pci_common + 0x1c, (uint16_t)value); break;
    case 0x50:                                                      // QueueNotify
        if (value < 4) snd_pci_wr16(snd_pci_notify + (uintptr_t)snd_pci_notify_off[value] * snd_pci_multiplier, (uint16_t)value);
        break;
    case 0x70: snd_pci_wr8(snd_pci_common + 0x14, (uint8_t)value); break;
    case 0x80: case 0x84: snd_pci_wr32(snd_pci_common + 0x20 + (offset - 0x80), value); break;   // queue_desc
    case 0x90: case 0x94: snd_pci_wr32(snd_pci_common + 0x28 + (offset - 0x90), value); break;   // queue_driver
    case 0xa0: case 0xa4: snd_pci_wr32(snd_pci_common + 0x30 + (offset - 0xa0), value); break;   // queue_device
    default: break;
    }
}
// The function's regions from its vendor capabilities: common configuration (1), notifications (2) and the card's
// configuration (4), each in the BAR crosvm assigned, mapped as device memory; the ISR (3) is not read (polled). False
// when one is missing.
static bool snd_pci_regions(uintptr_t config) {
    if (!(snd_pci_rd16(config + 0x06) & 0x10)) return false;                    // no capabilities list
    struct { uint8_t bar; uint32_t offset, length; bool found; } r[5];
    memset(r, 0, sizeof(r));
    uint32_t multiplier = 0;
    uint8_t at = snd_pci_rd8(config + 0x34) & 0xfc;
    for (unsigned walked = 0; at >= 0x40 && at <= 0x100 - 20 && walked < 48; ++walked) {
        const uint8_t id = snd_pci_rd8(config + at), next = snd_pci_rd8(config + at + 1) & 0xfc, type = snd_pci_rd8(config + at + 3);
        if (id == 0x09 && type >= 1 && type <= 4 && !r[type].found) {
            r[type].bar = snd_pci_rd8(config + at + 4);
            r[type].offset = snd_pci_rd32(config + at + 8);
            r[type].length = snd_pci_rd32(config + at + 12);
            r[type].found = true;
            if (type == 2) multiplier = snd_pci_rd32(config + at + 16);
        }
        at = next;
    }
    if (!r[1].found || !r[2].found || !r[4].found || r[1].length < 0x38 || r[2].length < 2 || r[4].length < 12) return false;
    snd_pci_wr16(config + 0x04, (uint16_t)(snd_pci_rd16(config + 0x04) | 6));   // memory space and bus master
    uintptr_t mapped[5] = {0};
    for (unsigned t = 1; t <= 4; ++t) {
        if (t == 3) continue;
        const unsigned bar = r[t].bar;
        if (bar > 5 || r[t].offset > UINT32_MAX - r[t].length) return false;
        const uint32_t low = snd_pci_rd32(config + 0x10 + bar * 4);
        if (low & 1) return false;                                              // an I/O BAR
        uint64_t base = low & ~(uint64_t)0xf;
        if ((low & 6) == 4) base |= (uint64_t)snd_pci_rd32(config + 0x10 + bar * 4 + 4) << 32;
        if (!base) return false;
        const uint64_t from = (base + r[t].offset) & ~(uint64_t)0x3fff;
        const uint64_t to = (base + r[t].offset + r[t].length + 0x3fff) & ~(uint64_t)0x3fff;
        const uintptr_t m = ml_io_map((vm_offset_t)from, (vm_size_t)(to - from));
        if (!m) return false;
        mapped[t] = m + (uintptr_t)(base + r[t].offset - from);
    }
    snd_pci_common = mapped[1]; snd_pci_notify = mapped[2]; snd_pci_device = mapped[4];
    snd_pci_multiplier = multiplier;
    return true;
}
// The bus's first 1af4:1059 function (function 0 of each device: 32 KiB a device on crosvm's ECAM) with its regions
// mapped: its configuration space, or 0.
static uintptr_t snd_pci_find(vm_offset_t bus, uint64_t size) {
    const uintptr_t space = ml_io_map(bus, (vm_size_t)size);
    if (!space) { printf("droidvm-snd: the PCI bus at %#llx not mapped\n", (unsigned long long)bus); return 0; }
    for (unsigned dev = 0; dev < 32 && ((uint64_t)dev << 15) + 0x1000 <= size; ++dev) {
        const uintptr_t config = space + ((uintptr_t)dev << 15);
        if (snd_pci_rd32(config) != (0x1af4u | 0x1059u << 16)) continue;
        if (!snd_pci_regions(config)) { printf("droidvm-snd: the sound card at 00:%02x.0 has not its regions\n", dev); return 0; }
        snd_pci = true;
        printf("droidvm-snd: virtio-pci sound card at 00:%02x.0 (1af4:1059), notifications x%u, polled\n", dev, snd_pci_multiplier);
        return config;
    }
    printf("droidvm-snd: no 1af4:1059 function on bus 0 (crosvm without --virtio-snd?)\n");
    return 0;
}
static uint32_t snd_read32(unsigned offset) {
    if (snd_pci) return snd_pci_read(offset);
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(snd.registers + offset) : "memory");
    return value;
}
static void snd_write32(unsigned offset, uint32_t value) {
    if (snd_pci) { snd_pci_write(offset, value); return; }
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(snd.registers + offset) : "memory");
}
#else
// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t snd_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(snd.registers + offset) : "memory");
    return value;
}
static void snd_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(snd.registers + offset) : "memory");
}
#endif
"#),
    ("                snd.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
                // OneiOS native: on crosvm's ECAM bus ("oneios,virtio-snd-pci", xnuloader-crosvm --audio) its 1af4:1059
                // function, wherever crosvm put it; an MMIO window as before
                if (SecureDTGetProperty(node, "compatible", &value, &size) == kSuccess && size >= 22 &&
                    !memcmp(value, "oneios,virtio-snd-pci", 22))
                    snd.registers = snd_pci_find((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), reg[1]);
                else
#endif
                snd.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
"#),
    // The microphone's counters, for OneiOSAudio.c's report in both kernels (the hosted kernel's stay 0: its transport is the
    // source as it is but for these two lines).
    ("char droidvm_snd_status[64];\n",
     r#"char droidvm_snd_status[64];
// OneiOS (staged/audio.rs SND_TRANSPORT_PCI; OneiOSAudio.c reports them): the microphone's stream on the device (0 none, or
// oneios-audio-mic=0; 1 being set up; 2 ready; 3 refused or not answered) and how long its SET_PARAMS and PREPARE took; its
// STARTs, those refused, those asked while it was not ready; what was captured: the largest sample since the report cleared
// it, periods with a sample over 32 (not silence), periods handed back unheard (the engine was more than four behind);
// control requests that took 100 ms or more, failed or were not answered.
volatile uint32_t oneios_snd_mic_state, oneios_snd_rx_peak;
volatile uint64_t oneios_snd_mic_ms, oneios_snd_mic_starts, oneios_snd_mic_refused, oneios_snd_mic_unready, oneios_snd_rx_loud,
    oneios_snd_rx_skipped, oneios_snd_slow_controls;
"#),
    // A request the device answered after its wait: its reply is taken off the used ring before the next request, which
    // otherwise read that late reply as its own (take7: Android's audio server, One UI stopped, took 10 s to open the
    // microphone at its PREPARE; the speaker's START then looked refused, the stream stayed started on the device and every
    // later START was "START to START", NOT_SUPP, so no period reached the phone). take17: the wait is the request's own
    // (2 s; 60 s for the microphone's set-up), one request at a time, and each slow, refused or unanswered one is a line.
    ("static bool control(uint32_t request_size, uint32_t reply_size) {\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/audio.rs SND_TRANSPORT_PCI): requests the device answered after their wait; their replies are taken
// off the used ring before the next request, so that each request reads its own reply.
static uint16_t snd_control_late;
// One request at a time: the engines' threads (droidvm_snd_running) and the microphone's own (snd_mic_setup). A request's wait
// in ms: 2 s, and 60 s for the microphone's SET_PARAMS and PREPARE (crosvm opens the phone's input at PREPARE, which took
// 10 s with One UI stopped in take7 and take16; the 2 s wait then gave the stream up). What a request that took 100 ms or
// more, was refused or got no answer was: the first 24 are printed, all are counted (oneios_snd_slow_controls).
#include <kern/locks.h>
static lck_mtx_t *snd_lock;
static uint32_t snd_control_wait_ms = 2000;
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
static void snd_control_said(uint64_t began, uint32_t status, bool unanswered) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    if (ns < 100000000ULL && status == SND_S_OK && !unanswered) return;
    uint32_t request[2];
    memcpy(request, snd_control_request, sizeof(request));
    if (oneios_snd_slow_controls++ < 24)
        printf("droidvm-snd: request %#x for stream %u: %s after %llu ms (status %#x)\n", request[0], request[1],
               unanswered ? "no answer" : status == SND_S_OK ? "answered" : "refused", (unsigned long long)(ns / 1000000), status);
}
// A request that got no answer in its wait is the device's still, and no other can be sent: its answer, waited for for
// `ms` more. True when the queue is free again, with that answer's status (the reply buffer is the request's own until
// the next one is sent).
static bool snd_control_late_answer(uint32_t ms, uint32_t *status) {
    for (uint32_t waited = 0; snd_control_late; ++waited) {
        if (used(SND_CONTROL)[1] != snd.used[SND_CONTROL]) {
            snd_barrier(); ++snd.used[SND_CONTROL]; --snd_control_late;
            memcpy(status, snd_control_reply, 4);
            printf("droidvm-snd: a late answer taken off the control queue after %u ms more (status %#x)\n", waited, *status);
            continue;
        }
        if (waited >= ms) return false;
        IOSleep(1);
    }
    return true;
}
#endif
static bool control(uint32_t request_size, uint32_t reply_size) {
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    {
        uint32_t late = 0;
        snd_control_late_answer(0, &late);   // what was answered since: off the used ring, so that this request reads its own
    }
    if (snd_control_late) { droidvm_snd_errors++; return false; }   // still unanswered: the queue's one chain is the device's
    const uint64_t snd_control_began = mach_absolute_time();
#endif
"#),
    ("        if (waited == 2000) { droidvm_snd_errors++; return false; }   // 2 s\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
        if (waited == snd_control_wait_ms) {   // 2 s (60 s for the microphone's set-up); its reply is taken later
            droidvm_snd_errors++; snd_control_late++; snd_control_said(snd_control_began, 0, true); return false;
        }
#else
        if (waited == 2000) { droidvm_snd_errors++; return false; }   // 2 s
#endif
"#),
    ("    memcpy(&status, snd_control_reply, 4);\n    if (status != SND_S_OK) droidvm_snd_errors++;\n",
     r#"    memcpy(&status, snd_control_reply, 4);
    if (status != SND_S_OK) droidvm_snd_errors++;
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    snd_control_said(snd_control_began, status, false);
#endif
"#),
    // The microphone's receive chains by slot (take17). The source posts all sixteen at every START and takes the used ring as
    // empty after a STOP ("the device returns what it held"); crosvm keeps a stopped stream's chains until RELEASE, so a second
    // START put every slot's chain on the queue twice. The native kernel posts only the slots the device gave back, hands a
    // chain without a whole period back unheard, keeps the engine at most four periods behind, and keeps the largest sample
    // for the report (silence, or sound).
    ("static void rx_post_all(void) {\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
__attribute__((unused))   // OneiOS native: snd_rx_post_missing posts (below)
#endif
static void rx_post_all(void) {
"#),
    ("uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count) {\n    if (!snd.in_running || count != SND_PERIOD_FRAMES) return 0;\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/audio.rs SND_TRANSPORT_PCI): the receive chains by slot, a bit for each one the device has. A STOP
// may give the chains back or keep them (crosvm keeps them until RELEASE): a START posts only the slots that came back.
static uint32_t snd_rx_out;
static void snd_rx_take_back(void) {
    while (used(SND_RX)[1] != snd.used[SND_RX]) {
        snd_barrier();
        const uint32_t *ring = (const uint32_t *)(used(SND_RX) + 2);
        const uint32_t id = ring[(snd.used[SND_RX] % SND_QUEUE) * 2];
        ++snd.used[SND_RX];
        snd_rx_out &= ~(1u << (id / 3 % SND_SLOTS));
    }
}
static void snd_rx_post(uint16_t s) {
    struct snd_desc *d = descriptors(SND_RX);
    const uint32_t period = SND_PERIOD_FRAMES * 2 * snd.in_channels;
    snd_rx[s].stream_id = (uint32_t)snd.in_stream;
    snd_rx[s].status[0] = 0;
    d[3 * s] = (struct snd_desc){pa(&snd_rx[s].stream_id), 4, VIRTQ_DESC_F_NEXT, (uint16_t)(3 * s + 1)};
    d[3 * s + 1] = (struct snd_desc){pa(snd_rx[s].data), period, VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE, (uint16_t)(3 * s + 2)};
    d[3 * s + 2] = (struct snd_desc){pa(snd_rx[s].status), 8, VIRTQ_DESC_F_WRITE, 0};
    post(SND_RX, (uint16_t)(3 * s));
    snd_rx_out |= 1u << s;
}
static void snd_rx_post_missing(void) {
    for (uint16_t s = 0; s < SND_SLOTS; ++s) if (!(snd_rx_out & (1u << s))) snd_rx_post(s);
}
// One period for the engine: the oldest the device gave back, unless more than four wait (the engine is 20 ms and more
// behind the microphone), whose oldest are handed back unheard. A chain that came back with an error status, or with no
// status and less than a period written (a stream being stopped), is handed back too. Mono: the first channel of a wider
// stream. The period's largest sample is kept for the report.
uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count) {
    if (!snd.in_running || count != SND_PERIOD_FRAMES) return 0;
    const uint32_t period = SND_PERIOD_FRAMES * 2 * snd.in_channels;
    uint16_t waiting = (uint16_t)(used(SND_RX)[1] - snd.used[SND_RX]);
    bool got = false;
    while (waiting && !got) {
        snd_barrier();
        const uint32_t *ring = (const uint32_t *)(used(SND_RX) + 2);
        const uint32_t id = ring[(snd.used[SND_RX] % SND_QUEUE) * 2], written = ring[(snd.used[SND_RX] % SND_QUEUE) * 2 + 1];
        ++snd.used[SND_RX];
        --waiting;
        const uint16_t s = (uint16_t)(id / 3 % SND_SLOTS);
        const uint32_t status = snd_rx[s].status[0];
        if (status != SND_S_OK && (status || written < period)) droidvm_snd_errors++;
        else if (waiting > 4) oneios_snd_rx_skipped++;
        else {
            const int16_t *data = (const int16_t *)snd_rx[s].data;
            uint32_t peak = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const int16_t v = data[i * snd.in_channels];
                const uint32_t size = v < 0 ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
                frames[i] = v;
                if (size > peak) peak = size;
            }
            if (peak > oneios_snd_rx_peak) oneios_snd_rx_peak = peak;
            if (peak > 32) oneios_snd_rx_loud++;
            got = true;
        }
        snd_rx_post(s);
    }
    if (!got) { droidvm_snd_rx_empty++; return 0; }
    droidvm_snd_rx_periods = droidvm_snd_rx_periods + 1;
    return count;
}
#else
uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count) {
    if (!snd.in_running || count != SND_PERIOD_FRAMES) return 0;
"#),
    ("    droidvm_snd_rx_periods++;\n    return count;\n}\n",
     r#"    droidvm_snd_rx_periods++;
    return count;
}
#endif
"#),
    // A stream's START (take17: one function for both streams). A START the device refuses may be one it had taken late
    // before ("START to START"): STOP and START once more (take7). A START that gets no answer in its 2 s could not be
    // retried at all: the queue's one chain is the device's until it answers, so the STOP and the second START failed at
    // once and the stream, started late on the device, was not running for the guest. Its answer is now waited for for
    // 13 s more (the phone's audio server took 10 s to open the input in take7 and take16) and is the START's.
    ("void droidvm_snd_running(bool output, bool input) {\n    if (!snd.up) return;\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/audio.rs SND_TRANSPORT_PCI): a stream's START. Not answered in its 2 s: the answer is waited for
// for 13 s more and is the START's (no other request can be sent before it). Refused: the device may have taken an earlier
// START late, so the stream is stopped and started again, once.
static bool snd_stream_start(int32_t stream, bool input) {
    const char *what = input ? "microphone" : "speaker";
    if (pcm_command(SND_R_PCM_START, stream)) return true;
    if (snd_control_late) {
        uint32_t status = ~0u;
        const bool answered = snd_control_late_answer(13000, &status);
        printf("droidvm-snd: the %s's START was not answered in its wait; then: %s\n", what,
               !answered ? "no answer in 13 s more" : status == SND_S_OK ? "running" : "refused");
        if (!answered) return false;
        if (status == SND_S_OK) return true;
    }
    pcm_command(SND_R_PCM_STOP, stream);
    if (input) {
        IOSleep(20);
        snd_rx_take_back();
        snd_rx_post_missing();
    }
    const bool running = pcm_command(SND_R_PCM_START, stream);
    printf("droidvm-snd: the %s's START failed once; again: %s\n", what, running ? "running" : "refused");
    return running;
}
#endif
void droidvm_snd_running(bool output, bool input) {
    if (!snd.up) return;
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    lck_mtx_lock(snd_lock);   // OneiOS native: one request at a time (the microphone's set-up has the queue for 10 s)
#endif
"#),
    ("            snd.out_running = pcm_command(SND_R_PCM_START, snd.out_stream);\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
            snd.out_running = snd_stream_start(snd.out_stream, false);   // OneiOS native: a late or refused START (above)
#else
            snd.out_running = pcm_command(SND_R_PCM_START, snd.out_stream);
#endif
"#),
    // The microphone's START and STOP (take17): only once its stream is ready, a failed START stopped and tried once more as
    // the speaker's, and only the slots the device gave back posted (snd_rx_out above). The function's end with it: the
    // native kernel's lets the queue go.
    (r#"    if (snd.in_stream >= 0 && input != snd.in_running) {
        if (input) {
            snd.used[SND_RX] = used(SND_RX)[1];
            rx_post_all();
            snd.in_running = pcm_command(SND_R_PCM_START, snd.in_stream);
        } else {
            pcm_command(SND_R_PCM_STOP, snd.in_stream);
            snd.in_running = false;
            IOSleep(20);
            snd.used[SND_RX] = used(SND_RX)[1];
        }
    }
}
"#,
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    // OneiOS native: the microphone, once its stream is ready (snd_mic_setup). An engine that asks before that records silence,
    // and is started by the next call (the function is asked again whenever either device starts or stops).
    if (snd.in_stream >= 0 && input != snd.in_running) {
        if (input && oneios_snd_mic_state != 2) {
            if (oneios_snd_mic_unready++ < 8)
                printf("droidvm-snd: the microphone is asked for and its stream is %s: silence\n",
                       oneios_snd_mic_state == 1 ? "still being set up" : "not there");
        } else if (input) {
            snd_rx_take_back();
            snd_rx_post_missing();
            snd.in_running = snd_stream_start(snd.in_stream, true);
            if (!snd.in_running) oneios_snd_mic_refused++;
            if (oneios_snd_mic_starts++ < 16)
                printf("droidvm-snd: the microphone's START: %s\n", snd.in_running ? "running" : "refused or not answered");
        } else {
            pcm_command(SND_R_PCM_STOP, snd.in_stream);
            snd.in_running = false;
            IOSleep(20);
            snd_rx_take_back();
        }
    }
    lck_mtx_unlock(snd_lock);
}
#else
    if (snd.in_stream >= 0 && input != snd.in_running) {
        if (input) {
            snd.used[SND_RX] = used(SND_RX)[1];
            rx_post_all();
            snd.in_running = pcm_command(SND_R_PCM_START, snd.in_stream);
        } else {
            pcm_command(SND_R_PCM_STOP, snd.in_stream);
            snd.in_running = false;
            IOSleep(20);
            snd.used[SND_RX] = used(SND_RX)[1];
        }
    }
}
#endif
"#),
    // The microphone only when asked (oneios-audio-mic=1), and set up by a thread of its own (take17). crosvm's AAudio backend
    // opens the phone's input at the stream's PREPARE, which with One UI stopped takes 10 s (take7; take16: crosvm.log has the
    // output's endpoint at 12:25:02.2 and the input's at 12:25:12.2). Until take16 the PREPARE was the boot thread's, with the
    // 2 s wait: it gave the stream up ("input stream -1", 1 error) and the Mic device recorded nothing for the session.
    ("    if (snd.in_stream >= 0 && !set_params(snd.in_stream, snd.in_channels)) snd.in_stream = -1;\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    {   // OneiOS native: the microphone only with oneios-audio-mic=1; its SET_PARAMS and PREPARE are snd_mic_setup's (crosvm
        // opens the phone's input at PREPARE: 10 s with One UI stopped, and this is the boot's thread)
        uint32_t mic = 0;
        PE_parse_boot_argn("oneios-audio-mic", &mic, sizeof(mic));
        if (!mic) snd.in_stream = -1;
        oneios_snd_mic_state = snd.in_stream >= 0 ? 1 : 0;
    }
#else
    if (snd.in_stream >= 0 && !set_params(snd.in_stream, snd.in_channels)) snd.in_stream = -1;
#endif
"#),
    ("void droidvm_snd_start(void) {\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/audio.rs SND_TRANSPORT_PCI): the microphone's stream, set up off the boot's thread: SET_PARAMS and
// PREPARE with a wait of 60 s each. The engines' requests wait for it (snd_lock): the first speaker START of a boot comes
// tens of seconds later.
static void snd_mic_setup(void *arg, wait_result_t result) {
    (void)arg; (void)result;
    lck_mtx_lock(snd_lock);
    const uint64_t began = mach_absolute_time();
    snd_control_wait_ms = 60000;
    const bool ready = set_params(snd.in_stream, snd.in_channels);
    snd_control_wait_ms = 2000;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    oneios_snd_mic_ms = ns / 1000000;
    oneios_snd_mic_state = ready ? 2 : 3;
    lck_mtx_unlock(snd_lock);
    printf("droidvm-snd: the microphone's stream %d (x%u): %s after %llu ms\n", snd.in_stream, snd.in_channels,
           ready ? "ready" : "refused or not answered, no microphone", (unsigned long long)(ns / 1000000));
}
static void snd_mic_start(void) {
    thread_t thread;
    if (oneios_snd_mic_state != 1) return;
    if (kernel_thread_start(snd_mic_setup, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else oneios_snd_mic_state = 3;
}
#endif
void droidvm_snd_start(void) {
"#),
    ("                        snd.up = true;\n                        droidvm_snd_stage = 3;\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
                        snd_lock = lck_mtx_alloc_init(lck_grp_alloc_init("oneios-snd", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
#endif
                        snd.up = true;
                        droidvm_snd_stage = 3;
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
                        snd_mic_start();   // OneiOS native: the microphone's stream, by a thread of its own
#endif
"#),
];

/// take18: the control queue's completions by interrupt, opt-in, on the native kernel's PCI transport (after
/// SND_TRANSPORT_PCI; from UniXNU-droidvm 793bb45 "virtio-snd: completion interrupts instead of polling" and 2cd4c8e, "polling
/// where there is none", reduced to what this transport polls for: a control request's reply, 1 ms sleeps for up to 2 s, 60 s
/// for the microphone's set-up). With the boot argument oneios-audio-irq=1 and /arm-io/virtio-snd's droidvm,intid (the line
/// crosvm's PCI host routes the card's function to; xnuloader-crosvm --audio-intid N,SLOT, which also writes
/// oneios,intid-slot, the function the line was read for: vm-boot checks crosvm's interrupt-map for it, and the transport
/// attaches the line only to a card found at that slot) the control queue asks for interrupts, the line (level-high,
/// ArmCpu's armcpu_spi_attach) acknowledges at the function's ISR (VirtIO 1.2 4.1.4.5: the read clears it and lowers the
/// line) and wakes the control waiter, which sleeps up to 1 ms a turn as before, so its timeouts are what they were. The PCM
/// queues stay NO_INTERRUPT (their periods are taken at the engine's own 5 ms pace). Without the argument, the slot or the
/// line: polled, as take17 (`droidvm-snd: control replies polled` and why). Upstream's worker thread and its new
/// PREPARE/RELEASE order are not taken: those paths are the microphone's, which another change owns.
pub const SND_IRQ: &[(&str, &str)] = &[
    ("static uint32_t snd_pci_multiplier;\n",
     concat!("static uint32_t snd_pci_multiplier;\n",
             "// OneiOS (take18, oneios-audio-irq): the function's ISR (mapped when it has one), the slot it was found at, and\n",
             "// whether the control queue's replies come by interrupt.\n",
             "static uintptr_t snd_pci_isr;\n",
             "static unsigned snd_pci_slot;\n",
             "static bool snd_irq_on;\n",
             "volatile uint64_t oneios_snd_interrupts;\n")),
    ("        if (t == 3) continue;\n",
     "        if (t == 3 && !r[3].found) continue;   // OneiOS (take18): the ISR too, read by the interrupt (oneios-audio-irq)\n"),
    ("    snd_pci_common = mapped[1]; snd_pci_notify = mapped[2]; snd_pci_device = mapped[4];\n",
     "    snd_pci_common = mapped[1]; snd_pci_notify = mapped[2]; snd_pci_device = mapped[4]; snd_pci_isr = mapped[3];\n"),
    ("        snd_pci = true;\n",
     "        snd_pci = true; snd_pci_slot = dev;\n"),
    ("// One control request and its reply, synchronously (the control queue has a single chain in flight). The request and reply\n",
     concat!("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
             "// OneiOS (take18, oneios-audio-irq): a control waiter's turn: 1 ms, or less when the card's line says the device\n",
             "// answered (snd_irq). Only the control queue interrupts, so a turn ends early only for its own reply.\n",
             "#include <kern/sched_prim.h>\n",
             "extern int armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);\n",
             "static void snd_control_nap(void) {\n",
             "    if (!snd_irq_on) { IOSleep(1); return; }\n",
             "    assert_wait_timeout((event_t)&snd.used[SND_CONTROL], THREAD_UNINT, 1000, NSEC_PER_USEC);\n",
             "    if (used(SND_CONTROL)[1] != snd.used[SND_CONTROL]) { clear_wait(current_thread(), THREAD_AWAKENED); return; }\n",
             "    thread_block(THREAD_CONTINUE_NULL);\n",
             "}\n",
             "// The card's level line: the ISR's read acknowledges it (and lowers the line); the control waiter is woken.\n",
             "static void snd_irq(void *context) {\n",
             "    (void)context;\n",
             "    (void)snd_pci_rd8(snd_pci_isr);\n",
             "    oneios_snd_interrupts = oneios_snd_interrupts + 1;\n",
             "    thread_wakeup((event_t)&snd.used[SND_CONTROL]);\n",
             "}\n",
             "// At bring-up, before DRIVER_OK: the line attached when it is asked for and known to be this card's.\n",
             "static void snd_irq_attach(void) {\n",
             "    uint32_t want = 0, intid = 0, slot = 0;\n",
             "    PE_parse_boot_argn(\"oneios-audio-irq\", &want, sizeof(want));\n",
             "    DTEntry node; void const *value; unsigned int size;\n",
             "    if (SecureDTLookupEntry(NULL, \"/arm-io/virtio-snd\", &node) == kSuccess) {\n",
             "        if (SecureDTGetProperty(node, \"droidvm,intid\", &value, &size) == kSuccess && size == sizeof(uint32_t)) memcpy(&intid, value, 4);\n",
             "        if (SecureDTGetProperty(node, \"oneios,intid-slot\", &value, &size) == kSuccess && size == sizeof(uint32_t)) memcpy(&slot, value, 4);\n",
             "    }\n",
             "    const char *why = !want ? \"oneios-audio-irq=1 not given\" : !snd_pci ? \"not on PCI\" : !snd_pci_isr ? \"the function has no ISR\"\n",
             "        : intid < 32 || intid > 1019 ? \"no droidvm,intid (xnuloader-crosvm --audio-intid)\"\n",
             "        : slot != snd_pci_slot ? \"the line was read for another slot\" : NULL;\n",
             "    if (!why && armcpu_spi_attach(intid, false, snd_irq, NULL, NULL, NULL) != 0) why = \"the line could not be attached\";\n",
             "    if (why) { printf(\"droidvm-snd: control replies polled (%s)\\n\", why); return; }\n",
             "    *(volatile uint16_t *)(snd_rings[SND_CONTROL] + 1024) = 0;   // the control queue's completions interrupt\n",
             "    snd_irq_on = true;\n",
             "    printf(\"droidvm-snd: control replies by interrupt: INTID %u (crosvm's SPI %u), the card at 00:%02x.0\\n\", intid, intid - 32, snd_pci_slot);\n",
             "}\n",
             "#else\n",
             "#define snd_control_nap() IOSleep(1)\n",
             "#endif\n",
             "// One control request and its reply, synchronously (the control queue has a single chain in flight). The request and reply\n")),
    ("        if (waited >= ms) return false;\n        IOSleep(1);\n",
     "        if (waited >= ms) return false;\n        snd_control_nap();   // OneiOS (take18): an interrupt ends the turn sooner\n"),
    ("#endif\n        IOSleep(1);\n    }\n    snd_barrier();\n    ++snd.used[SND_CONTROL];\n",
     "#endif\n        snd_control_nap();   // OneiOS (take18): IOSleep(1), or less with oneios-audio-irq\n    }\n    snd_barrier();\n    ++snd.used[SND_CONTROL];\n"),
    ("    snd_barrier();\n    snd_write32(0x70, 15);         // DRIVER_OK\n",
     concat!("#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE\n",
             "    snd_irq_attach();   // OneiOS (take18): the control queue's replies by interrupt, when asked (oneios-audio-irq=1)\n",
             "#endif\n",
             "    snd_barrier();\n    snd_write32(0x70, 15);         // DRIVER_OK\n")),
];
