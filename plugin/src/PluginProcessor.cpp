#include "PluginProcessor.h"
#if ! ELF_NO_EDITOR
 #include "PluginEditor.h"
#endif
#include "InitKit.h"
#include "VoiceSlice.h"
#include <cstring>

extern "C" {
#include "engine.h"   // lxr_morph_kit (pure kit interpolation, firmware morph semantics)
}

namespace {
constexpr uint8_t kStateVersion = 5;   // v2: + morph target name; v3: + performance faders;
                                       // v4: + pattern set, song data, pattern/song names, seq settings
                                       // v5: + Phase 5 block (morph target B + XY, macros, perf modes)
                                       // (v1-v4 still load; their pattern set stays at firmware init)
// GM drum map, used ONLY while the firmware's per-voice note parameters are all at their
// stock placeholder value (48) — see processBlock. Same map PHX-D01 ships (host/midi.c).
constexpr int kGmMap[7] = { 36, 45, 50, 38, 39, 42, 46 };
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout ElfDrumLXRProcessor::createParameterLayout()
{
    // Display names and ranges come from the generated firmware metadata
    // (ParamMetadata.generated.h); the IDs are unchanged from Phase 1
    // (kit000..kit246), so existing sessions/automation still map. Phase 1
    // clamped every parameter to 0..127 — the metadata ranges fix the
    // mod-target params, whose values run to 228.
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.reserve (kNumKitParams + 16);
    for (int i = 0; i < kNumKitParams; ++i)
    {
        const auto& meta = elfmeta::kKitParams[i];
        const int def = juce::jlimit ((int) meta.minValue, (int) meta.maxValue,
                                      (int) initkit::params[i]);
        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { kitParamId (i), 1 },
            meta.displayName, meta.minValue, meta.maxValue, def));
    }
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "morph", 1 }, "Morph",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    // Performance faders: NOT kit parameters. They drive the engine wrapper's
    // fader path (the hardware's performance-page faders) plus a host-side
    // master trim, and persist in the plugin state (v3), never in .SND files.
    static const char* faderNames[6] = { "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "HiHat" };
    for (int v = 0; v < 6; ++v)
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { faderParamId (v), 1 },
            juce::String ("Fader ") + faderNames[v],
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { faderParamId (6), 1 }, "Fader Master",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    // Phase 3 sequencer parameters (new IDs; no collision with kitNNN/morph/fader*).
    // In a DAW the play state and tempo follow the host transport; seqPlay/seqTempo
    // drive the internal clock when no playhead position is available (Standalone).
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "seqPlay", 1 }, "Seq Play", false));
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "seqPattern", 1 }, "Seq Pattern", 0, 63, 0));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "seqSongMode", 1 }, "Seq Song Mode", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "seqTempo", 1 }, "Seq Tempo",
        juce::NormalisableRange<float> (30.0f, 300.0f, 0.1f), 120.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "seqExtSync", 1 }, "Seq Ext Sync", false));
    // Phase 5 (item 4): Morph XY. When morphXY is on, the single `morph`
    // amount is bypassed and the desired kit is the documented two-stage
    // composition toward target A (the classic morph target) by morphX, then
    // toward target B by morphY (kitops::morphXY). New IDs; nothing frozen
    // changes.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "morphX", 1 }, "Morph X (to A)",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "morphY", 1 }, "Morph Y (to B)",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "morphXY", 1 }, "Morph XY On", false));
    // Phase 5 (item 8): infinity mode + 16 Pitches tonal mode.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "seqInfinity", 1 }, "Infinity (evolving sequencer)", false));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "pitch16", 1 }, "16 Pitches On", false));
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "pitch16Voice", 1 }, "16 Pitches Voice", 0, 3, 0));
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "pitch16Scale", 1 }, "16 Pitches Scale", 0, 4, 1));
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "pitch16Root", 1 }, "16 Pitches Root", 0, 11, 0));
    return { params.begin(), params.end() };
}

ElfDrumLXRProcessor::ElfDrumLXRProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Main (DAC1)", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Out 2 (DAC2)", juce::AudioChannelSet::stereo(), true)
                          // Phase 4 stems (patch 04): per-voice breakouts, disabled by
                          // default — a host enables the ones it wants; the default
                          // layout stays exactly the two hardware stereo pairs.
                          .withOutput ("Stem Drum 1", juce::AudioChannelSet::stereo(), false)
                          .withOutput ("Stem Drum 2", juce::AudioChannelSet::stereo(), false)
                          .withOutput ("Stem Drum 3", juce::AudioChannelSet::stereo(), false)
                          .withOutput ("Stem Snare", juce::AudioChannelSet::stereo(), false)
                          .withOutput ("Stem Cymbal", juce::AudioChannelSet::stereo(), false)
                          .withOutput ("Stem HiHat", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    for (int i = 0; i < kNumKitParams; ++i)
        kitParamPtrs[i] = apvts.getRawParameterValue (kitParamId (i));
    morphParamPtr = apvts.getRawParameterValue ("morph");
    for (int v = 0; v < 6; ++v)
        faderPtrs[v] = apvts.getRawParameterValue (faderParamId (v));
    masterFaderPtr = apvts.getRawParameterValue (faderParamId (6));
    lastFaders.fill (-1.0f);   // force a fader push on the first block

    morphXPtr = apvts.getRawParameterValue ("morphX");
    morphYPtr = apvts.getRawParameterValue ("morphY");
    morphXYPtr = apvts.getRawParameterValue ("morphXY");
    seqInfinityPtr = apvts.getRawParameterValue ("seqInfinity");
    pitch16Ptr = apvts.getRawParameterValue ("pitch16");
    pitch16VoicePtr = apvts.getRawParameterValue ("pitch16Voice");
    pitch16ScalePtr = apvts.getRawParameterValue ("pitch16Scale");
    pitch16RootPtr = apvts.getRawParameterValue ("pitch16Root");

    seqPlayPtr = apvts.getRawParameterValue ("seqPlay");
    seqPatternPtr = apvts.getRawParameterValue ("seqPattern");
    seqSongModePtr = apvts.getRawParameterValue ("seqSongMode");
    seqTempoPtr = apvts.getRawParameterValue ("seqTempo");
    seqExtSyncPtr = apvts.getRawParameterValue ("seqExtSync");
    for (auto& n : patternNames) n.fill (' ');
    for (auto& n : songNames) n.fill (' ');

    std::memcpy (baseKit.data(), initkit::params, kNumKitParams);
    std::memcpy (morphTargetKit.data(), initkit::params, kNumKitParams);
    std::memcpy (morphTarget2Kit.data(), initkit::params, kNumKitParams);
    std::array<char, 8> n {};
    std::memcpy (n.data(), initkit::name, std::min (strlen (initkit::name), (size_t) 8));
    setKitNameBytes (n);
    setMorphTargetNameBytes (n);   // startup target IS the current kit — the editor shows it
    setMorphTarget2NameBytes (n);
}

ElfDrumLXRProcessor::~ElfDrumLXRProcessor()
{
    cancelPendingUpdate();
    delete pendingRequest.exchange (nullptr);
    delete pendingSeqRequest.exchange (nullptr);
}

void ElfDrumLXRProcessor::setKitNameBytes (const std::array<char, 8>& n)
{
    const juce::ScopedLock sl (nameLock);
    kitNameBytes = n;
}

void ElfDrumLXRProcessor::setMorphTargetNameBytes (const std::array<char, 8>& n)
{
    const juce::ScopedLock sl (nameLock);
    morphTargetNameBytes = n;
}

juce::String ElfDrumLXRProcessor::getKitName()
{
    const juce::ScopedLock sl (nameLock);
    return juce::String (kitNameBytes.data(), 8).trimEnd();
}

juce::String ElfDrumLXRProcessor::getMorphTargetName()
{
    const juce::ScopedLock sl (nameLock);
    return juce::String (morphTargetNameBytes.data(), 8).trimEnd();
}

void ElfDrumLXRProcessor::setMorphTarget2NameBytes (const std::array<char, 8>& n)
{
    const juce::ScopedLock sl (nameLock);
    morphTarget2NameBytes = n;
}

juce::String ElfDrumLXRProcessor::getMorphTarget2Name()
{
    const juce::ScopedLock sl (nameLock);
    return juce::String (morphTarget2NameBytes.data(), 8).trimEnd();
}

bool ElfDrumLXRProcessor::loadMorphTarget2File (const juce::File& file)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() < 255)
        return false;
    auto req = std::make_unique<KitRequest>();
    req->target2Only = true;
    std::memcpy (req->name.data(), data.getData(), 8);
    std::memcpy (req->params.data(), (const uint8_t*) data.getData() + 8, kNumKitParams);
    postKitRequest (std::move (req));
    return true;
}

//==============================================================================
void ElfDrumLXRProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Stems latch inside EngineHost::prepare (stream state resets there):
    // enabled iff the host enabled at least one stem bus in this layout.
    bool anyStem = false;
    for (int b = 2; b < getBusCount (false); ++b)
        if (auto* bus = getBus (false, b))
            anyStem = anyStem || bus->isEnabled();
    host.setStemsEnabled (anyStem);
    host.prepare (sampleRate);
    setLatencySamples (host.getLatencySamples());
    scratch.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
    engineKitLoaded = false;   // (re)load the kit into the engine on the next block
    seqWasPlaying = false;     // sequencer transport re-syncs on the next block
    seqHaveAnchor = false;
    requestSeqSnapshot();      // so a state save after playback starts has a pattern set
}

void ElfDrumLXRProcessor::releaseResources() { host.release(); }

bool ElfDrumLXRProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.outputBuses.size() > 8)
        return false;
    // Bus 2 (DAC2) and the six Phase 4 stem buses: stereo when enabled,
    // or disabled. Nothing else.
    for (int b = 1; b < layouts.outputBuses.size(); ++b)
        if (layouts.outputBuses[b] != juce::AudioChannelSet::stereo()
            && ! layouts.outputBuses[b].isDisabled())
            return false;
    return layouts.getMainInputChannelSet().isDisabled();
}

//==============================================================================
// Kit requests: created on the message thread, consumed (and deleted) on the audio thread.
void ElfDrumLXRProcessor::postKitRequest (std::unique_ptr<KitRequest> req)
{
    if (auto* old = pendingRequest.exchange (req.release()))
        delete old;
}

std::array<uint8_t, ElfDrumLXRProcessor::kNumKitParams>
ElfDrumLXRProcessor::computeDesired (const std::array<uint8_t, kNumKitParams>& base,
                                     float legacyMorph) const
{
    std::array<uint8_t, kNumKitParams> desired;
    if (morphXYPtr->load() > 0.5f)
    {
        // Phase 5 Morph XY: two-stage firmware-morph composition (KitOps.h).
        kitops::morphXY (base, morphTargetKit, morphTarget2Kit,
                         morphXPtr->load(), morphYPtr->load(), desired);
    }
    else if (legacyMorph > 0.001f)
    {
        lxr_morph_kit (base.data(), morphTargetKit.data(),
                       (uint8_t) juce::roundToInt (legacyMorph * 255.0f), desired.data());
    }
    else
    {
        desired = base;
    }
    return desired;
}

void ElfDrumLXRProcessor::syncMorphLasts (float legacyMorph)
{
    lastMorph = legacyMorph;
    lastMorphX = morphXPtr->load();
    lastMorphY = morphYPtr->load();
    lastMorphXY = morphXYPtr->load() > 0.5f;
}

void ElfDrumLXRProcessor::applyKitRequest (KitRequest& req)
{
    if (req.targetOnly || req.target2Only)
    {
        // Explicit morph target (editor's "Load target .SND" / target B): the
        // current kit and the APVTS bridge are untouched; re-apply at the
        // current morph amounts so a target swapped in mid-morph is heard
        // immediately.
        if (req.targetOnly)
        {
            morphTargetKit = req.params;
            setMorphTargetNameBytes (req.name);
        }
        else
        {
            morphTarget2Kit = req.params;
            setMorphTarget2NameBytes (req.name);
        }
        const float morph = morphParamPtr->load();
        const auto desired = computeDesired (baseKit, morph);
        host.loadKit (desired.data());
        lastBaseKit = baseKit;
        lastSentKit = desired;
        syncMorphLasts (morph);
        engineKitLoaded = true;
        return;
    }

    const auto oldBase = baseKit;
    baseKit = req.params;
    if (req.hasTarget)
    {
        morphTargetKit = req.target;                          // state restore
        setMorphTargetNameBytes (req.targetName);
    }
    else
    {
        morphTargetKit = oldBase;                             // file loads: morph back to the previous kit
        const juce::ScopedLock sl (nameLock);
        morphTargetNameBytes = kitNameBytes;                  // ...whose name is still the old one here
    }
    if (req.hasTarget2)
    {
        morphTarget2Kit = req.target2;                        // state restore (v5)
        setMorphTarget2NameBytes (req.target2Name);
    }
    setKitNameBytes (req.name);
    if (req.hasMorph)
        lastMorph = -1.0f;   // force the bridge to re-evaluate with the restored morph below

    // Full load through the wrapper's single entry path, honouring the current morph amount.
    const float morph = req.hasMorph ? req.morph : morphParamPtr->load();
    const auto desired = computeDesired (baseKit, morph);
    host.loadKit (desired.data());
    lastBaseKit = baseKit;
    lastSentKit = desired;
    syncMorphLasts (morph);
    engineKitLoaded = true;

    bridgeSuspended = true;      // the async updater aligns APVTS with baseKit; don't fight it
    apvtsSyncPending = true;
    triggerAsyncUpdate();
}

int ElfDrumLXRProcessor::retroCommit (int pattern)
{
    if (pattern < 0 || pattern >= 64)
        return 0;
    seqops::PatternData p;
    if (! readPatternData (pattern, p))
        p = seqops::clearedPattern();
    const uint32_t total = retroWrite.load (std::memory_order_acquire);
    const uint32_t count = juce::jmin (total, (uint32_t) retroRing.size());
    int written = 0;
    for (uint32_t i = total - count; i < total; ++i)
    {
        const auto& e = retroRing[i % retroRing.size()];
        if (e.pattern != pattern)
            continue;
        auto& st = p.steps[e.track][e.step];
        st.note = e.note;
        st.volume = e.vel;
        st.prob = 100;
        p.main[e.track][e.step] = true;
        ++written;
    }
    if (written > 0)
        writePatternData (pattern, p);
    return written;
}

void ElfDrumLXRProcessor::handleAsyncUpdate()
{
    // Phase 5 infinity: one guarded mutation per counted 4-bar cycle. Runs
    // on the message thread (mutateKit is an APVTS-path operation); pendings
    // counted while the mode is off are discarded, not banked.
    const int pending = infinityPending.exchange (0);
    if (pending > 0 && seqInfinityPtr->load() > 0.5f)
        for (int i = 0; i < pending; ++i)
            mutateKit (0.12f, 0x1A2B3C00u + (uint32_t) infinityMutations++);

    if (! apvtsSyncPending.exchange (false))
        return;
    // message thread: write the loaded kit into APVTS so host automation shows the truth.
    std::array<uint8_t, kNumKitParams> snapshot;
    {
        // baseKit is audio-thread-owned; this exchange is serialised by bridgeSuspended
        // (the audio thread won't touch baseKit from APVTS while the bridge is suspended,
        // and kit loads are the only other writer — they re-post a fresh async update).
        snapshot = baseKit;
    }
    for (int i = 0; i < kNumKitParams; ++i)
        if (auto* p = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (kitParamId (i))))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) snapshot[i]));
    bridgeSuspended = false;
}

std::array<uint8_t, ElfDrumLXRProcessor::kNumKitParams> ElfDrumLXRProcessor::desiredKit()
{
    // Bridge-path desired kit: base + last* morph state (kept in sync with the
    // APVTS atomics by bridgeParameters / applyKitRequest). Uses the same
    // composition as computeDesired, but with the tracked amounts so a host's
    // per-block parameter reads stay consistent within the block.
    std::array<uint8_t, kNumKitParams> desired;
    if (lastMorphXY)
        kitops::morphXY (lastBaseKit, morphTargetKit, morphTarget2Kit,
                         lastMorphX, lastMorphY, desired);
    else if (lastMorph > 0.001f)
        lxr_morph_kit (lastBaseKit.data(), morphTargetKit.data(),
                       (uint8_t) juce::roundToInt (lastMorph * 255.0f), desired.data());
    else
        desired = lastBaseKit;
    return desired;
}

void ElfDrumLXRProcessor::bridgeParameters()
{
    if (bridgeSuspended)
        return;

    std::array<uint8_t, kNumKitParams> cur;
    for (int i = 0; i < kNumKitParams; ++i)
        cur[i] = (uint8_t) juce::roundToInt (kitParamPtrs[i]->load());
    const float morph = morphParamPtr->load();
    const float morphX = morphXPtr->load(), morphY = morphYPtr->load();
    const bool morphXY = morphXYPtr->load() > 0.5f;

    if (! engineKitLoaded)
    {
        lastBaseKit = cur;
        syncMorphLasts (morph);
        const auto desired = desiredKit();
        host.loadKit (desired.data());
        lastSentKit = desired;
        engineKitLoaded = true;
        return;
    }

    if (cur == lastBaseKit && morph == lastMorph && morphX == lastMorphX
        && morphY == lastMorphY && morphXY == lastMorphXY)
        return;   // nothing moved: leave the engine (and any parser-driven CC state) alone

    lastBaseKit = cur;
    syncMorphLasts (morph);
    const auto desired = desiredKit();
    if (desired != lastSentKit)
    {
        host.editKit (desired.data());   // wrapper diffs against the engine's active kit
        lastSentKit = desired;
    }
}

//==============================================================================
void ElfDrumLXRProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    // Phase 4 (5): this instance's engine state stays live for the whole
    // block — kit application, the seq command drain, the transport drive
    // and the render below all call the engine directly or via the host.
    auto engineActivation = host.activation();

    if (auto* req = pendingRequest.exchange (nullptr))
    {
        applyKitRequest (*req);
        delete req;
    }
    bridgeParameters();

    // --- performance faders (not kit params): push changes into the engine ---
    for (int v = 0; v < 6; ++v)
    {
        const float f = faderPtrs[v]->load();
        if (f != lastFaders[v]) { host.setFader (v, f); lastFaders[v] = f; }
    }
    {
        const float f = masterFaderPtr->load();
        if (f != lastFaders[6]) { host.setMasterGain (f); lastFaders[6] = f; }
    }

    // --- Phase 3 sequencer: requests, editor commands, transport, snapshot ---
    if (auto* sreq = pendingSeqRequest.exchange (nullptr))
    {
        applySeqRequest (*sreq);
        delete sreq;
    }
    drainSeqCommands();
    bridgeSequencer (buffer.getNumSamples());
    fulfilSeqSnapshot();

    // --- queued editor triggers fire at this block's start ---
    const uint64_t blockStartPos = host.getHostPosition();
    {
        int start1, size1, start2, size2;
        const int ready = triggerFifo.getNumReady();
        triggerFifo.prepareToRead (ready, start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i)
        {
            const auto& t = triggerRing[(size_t) (start1 + i)];
            host.queueHit (t.voice, t.note, t.velocity, blockStartPos);
        }
        for (int i = 0; i < size2; ++i)
        {
            const auto& t = triggerRing[(size_t) (start2 + i)];
            host.queueHit (t.voice, t.note, t.velocity, blockStartPos);
        }
        triggerFifo.finishedRead (size1 + size2);
    }

    // --- MIDI ---
    const uint64_t blockStart = host.getHostPosition();
    const int gchanStored = host.globalChannel();          // 0 = "all"
    bool stockNotes = true;
    for (int v = 1; v < 7; ++v)
        if (host.midiNote (v) != host.midiNote (0)) { stockNotes = false; break; }

    // Phase 5 retro-capture: remember hits (with their sequencer position)
    // while the sequencer runs; retroCommit() turns them into steps later.
    auto recordRetro = [this] (int voice, int hitNote, int vel)
    {
        if (! lxr_seq_is_running())
            return;
        const int track = juce::jmin (voice, 5);   // open hat records onto the hihat track
        const int step = lxr_seq_step_index (track);
        if (step < 0 || step > 63)
            return;
        RetroEvent e;
        e.pattern = (uint8_t) lxr_seq_active_pattern();
        e.track = (uint8_t) track;
        e.step = (uint8_t) step;
        e.note = (uint8_t) juce::jlimit (0, 127, hitNote);
        e.vel = (uint8_t) juce::jlimit (1, 127, vel);
        const uint32_t w = retroWrite.load (std::memory_order_relaxed);
        retroRing[w % retroRing.size()] = e;
        retroWrite.store (w + 1, std::memory_order_release);
    };

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
        {
            const int chan = msg.getChannel() - 1, note = msg.getNoteNumber(), vel = msg.getVelocity();
            const uint64_t at = blockStart + (uint64_t) msg.getTimeStamp();
            if (pitch16Ptr->load() > 0.5f)
            {
                // Phase 5, 16 Pitches: every note-on plays the selected tonal
                // voice (drums 1-3 + snare) at the scale-quantised pitch.
                const int voice = juce::roundToInt (pitch16VoicePtr->load());
                const int q = kitops::quantizeToScale (note,
                                                       juce::roundToInt (pitch16RootPtr->load()),
                                                       juce::roundToInt (pitch16ScalePtr->load()));
                host.queueHit (voice, q, vel, at);
                recordRetro (voice, q, vel);
                continue;
            }
            for (int v = 0; v < 7; ++v)
            {
                const bool chanOk = (gchanStored == 0)
                    || chan == host.midiChannel (v)
                    || chan == gchanStored - 1;
                if (! chanOk) continue;
                if (! stockNotes)
                {
                    // The firmware's own configuration: per-voice note + channel globals.
                    if (note == host.midiNote (v))
                    {
                        host.queueHit (v, note, vel, at);
                        recordRetro (v, note, vel);
                    }
                }
                else if (note == kGmMap[v])
                {
                    // Stock firmware notes are all 48 — an inert placeholder upstream (the
                    // parameterArray hooks for PAR_MIDI_NOTE* are commented out in the firmware),
                    // so while they are, the GM drum map selects the voice (as PHX-D01 does)
                    // and the played note's offset from the map note pitches it around 60.
                    host.queueHit (v, 60 + (note - kGmMap[v]), vel, at);
                    recordRetro (v, 60 + (note - kGmMap[v]), vel);
                }
            }
        }
        else if (msg.isNoteOff())
        {
            // one-shot drums: the firmware ignores note offs, and so do we
        }
        else if (! msg.isSysEx())
        {
            // Everything else (CC incl. the NRPN sequence, pitch bend, program change,
            // pressure — and, since Phase 3, the realtime transport bytes) goes to the
            // firmware's own parser as a raw byte stream. The parser routes MIDI
            // clock/start/continue/stop into the sequencer's sync path only when
            // external sync is armed (seqExtSync), exactly as on hardware; before
            // Phase 3 those bytes were dropped here and the sync path was unreachable.
            host.midiBytes (msg.getRawData(), msg.getRawDataSize());
        }
    }

    // --- Audio: 4 engine jacks -> bus 0 (jacks 1/2 = DAC1), bus 1 (jacks 3/4 = DAC2);
    //     patch-04 stems -> buses 2..7 when the host enabled them ---
    const int numSamples = buffer.getNumSamples();
    if ((int) scratch.size() < numSamples)
        scratch.assign ((size_t) numSamples, 0.0f);   // host exceeded its declared block size
    auto busChannel = [&buffer, this] (int bus, int chan) -> float*
    {
        auto bb = getBusBuffer (buffer, false, bus);
        return chan < bb.getNumChannels() ? bb.getWritePointer (chan) : scratch.data();
    };
    std::array<float*, 4> chans { busChannel (0, 0), busChannel (0, 1),
                                  busChannel (1, 0), busChannel (1, 1) };
    host.render (chans.data(), numSamples);
    if (host.getStemsEnabled())
    {
        std::array<float*, EngineHost::kStemChannels> stemChans;
        for (int v = 0; v < EngineHost::kStems; ++v)
            for (int c = 0; c < 2; ++c)
                stemChans[2 * v + c] = busChannel (2 + v, c);
        host.renderStems (stemChans.data(), numSamples);
    }
}

//==============================================================================
bool ElfDrumLXRProcessor::loadKitFile (const juce::File& file)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() < 255)
        return false;
    auto req = std::make_unique<KitRequest>();
    std::memcpy (req->name.data(), data.getData(), 8);
    std::memcpy (req->params.data(), (const uint8_t*) data.getData() + 8, kNumKitParams);
    postKitRequest (std::move (req));
    return true;
}

bool ElfDrumLXRProcessor::loadMorphTargetFile (const juce::File& file)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() < 255)
        return false;
    auto req = std::make_unique<KitRequest>();
    req->targetOnly = true;
    std::memcpy (req->name.data(), data.getData(), 8);
    std::memcpy (req->params.data(), (const uint8_t*) data.getData() + 8, kNumKitParams);
    postKitRequest (std::move (req));
    return true;
}

bool ElfDrumLXRProcessor::saveKitFile (const juce::File& file)
{
    // Write the current kit exactly as the hardware does: 8 name bytes + 247 parameter bytes.
    std::array<uint8_t, kNumKitParams> snapshot;
    std::array<char, 8> name;
    {
        // Read the bridge's view: APVTS is the user-visible truth.
        for (int i = 0; i < kNumKitParams; ++i)
            snapshot[i] = (uint8_t) juce::roundToInt (kitParamPtrs[i]->load());
        const juce::ScopedLock sl (nameLock);
        name = kitNameBytes;
    }
    juce::MemoryBlock data (255);
    std::memcpy (data.getData(), name.data(), 8);
    std::memcpy ((uint8_t*) data.getData() + 8, snapshot.data(), kNumKitParams);
    return file.replaceWithData (data.getData(), data.getSize());
}

void ElfDrumLXRProcessor::loadFactoryInit()
{
    auto req = std::make_unique<KitRequest>();
    std::memcpy (req->params.data(), initkit::params, kNumKitParams);
    std::array<char, 8> n {};
    std::memcpy (n.data(), initkit::name, std::min (strlen (initkit::name), (size_t) 8));
    req->name = n;
    postKitRequest (std::move (req));
}

//==============================================================================
// Phase 2 byte-level access (message thread; APVTS is the truth).
std::array<uint8_t, ElfDrumLXRProcessor::kNumKitParams> ElfDrumLXRProcessor::currentKitBytes() const
{
    std::array<uint8_t, kNumKitParams> out;
    for (int i = 0; i < kNumKitParams; ++i)
        out[i] = (uint8_t) juce::roundToInt (kitParamPtrs[i]->load());
    return out;
}

void ElfDrumLXRProcessor::writeParamValues (const int* indices, const uint8_t* values, int count)
{
    for (int n = 0; n < count; ++n)
    {
        const int i = indices[n];
        if (i < 0 || i >= kNumKitParams)
            continue;
        if (auto* p = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (kitParamId (i))))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) values[n]));
    }
}

bool ElfDrumLXRProcessor::applyVoiceSlice (int voice, const std::vector<uint8_t>& slice)
{
    const auto& idxs = voiceslice::indicesForVoice (voice);
    if (slice.size() != idxs.size())
        return false;
    std::vector<int> indices (idxs.begin(), idxs.end());
    writeParamValues (indices.data(), slice.data(), (int) slice.size());
    return true;
}

std::array<char, 8> ElfDrumLXRProcessor::currentKitNameBytes() const
{
    const juce::ScopedLock sl (nameLock);
    return kitNameBytes;
}

void ElfDrumLXRProcessor::randomiseParams (int voice, uint32_t seed)
{
    // Guarded randomise. The guards (mirroring the morph keep-set's spirit and
    // the LXR02XXL community randomiser's level floor):
    //   - routing params are never touched (metadata kFlagRouting: the six
    //     per-voice AUDIO_OUT params + PAR_FX_ROUTING),
    //   - mod-target selectors are never touched (kFlagModTarget: the twelve
    //     DTYPE_TARGET_SELECTION_* params + PAR_MORPH_KIT_DEST, the morph
    //     destination preset selector),
    //   - level params (kFlagLevel: PAR_VOL1..6, PAR_TRANS*_VOL) are floored
    //     at 30% of their range so a voice can't be silenced by chance,
    //   - PAR_NONE (the dummy slot) and the Global/System params are out of
    //     scope: per-voice randomise covers exactly that voice's slice;
    //     whole-kit covers the six voice slices + the FX page +
    //     PAR_VOICE_DECIMATION_ALL (the one performance-global in the kit).
    // Everything goes through writeParamValues, i.e. the normal APVTS path,
    // so the panel reflects the result and the bridge replays it to the engine.
    juce::Random rng ((juce::int64) seed);
    std::vector<int> indices;
    std::vector<uint8_t> values;
    for (const auto& meta : elfmeta::kKitParams)
    {
        const int i = (int) meta.index;
        if (voice >= 0)
        {
            if (meta.voice != (uint8_t) voice)
                continue;
        }
        else
        {
            const bool inScope = meta.voice < 6 || meta.page == elfmeta::PageFX
                                 || i == 114 /* PAR_VOICE_DECIMATION_ALL */;
            if (! inScope)
                continue;
        }
        if (meta.flags & (elfmeta::kFlagRouting | elfmeta::kFlagModTarget))
            continue;
        const int lo = meta.minValue, hi = meta.maxValue;
        int v = lo + rng.nextInt (hi - lo + 1);
        if (meta.flags & elfmeta::kFlagLevel)
            v = juce::jmax (v, lo + (int) (0.30f * (float) (hi - lo)));
        indices.push_back (i);
        values.push_back ((uint8_t) v);
    }
    writeParamValues (indices.data(), values.data(), (int) indices.size());
}

namespace {
// Write only the bytes that differ (the panel-edit path the bridge diffs into
// the engine; used by the Phase 5 kit transforms below).
void writeKitDiff (ElfDrumLXRProcessor& proc, const kitops::Kit& from, const kitops::Kit& to)
{
    std::vector<int> idx;
    std::vector<uint8_t> val;
    for (int i = 0; i < kitops::kNumParams; ++i)
        if (from[i] != to[i])
        {
            idx.push_back (i);
            val.push_back (to[i]);
        }
    if (! idx.empty())
        proc.writeParamValues (idx.data(), val.data(), (int) idx.size());
}

void pushHistory (std::vector<kitops::Kit>& history, const kitops::Kit& kit)
{
    history.push_back (kit);
    if (history.size() > 32)
        history.erase (history.begin());
}
} // namespace

void ElfDrumLXRProcessor::mutateKit (float amount, uint32_t seed)
{
    const auto cur = currentKitBytes();
    pushHistory (kitHistory, cur);
    writeKitDiff (*this, cur, kitops::mutate (cur, amount, seed));
}

void ElfDrumLXRProcessor::breedWithKit (const kitops::Kit& other, bool perVoice, uint32_t seed)
{
    const auto cur = currentKitBytes();
    pushHistory (kitHistory, cur);
    writeKitDiff (*this, cur, kitops::breed (cur, other,
                                             perVoice ? kitops::BreedMode::perVoice
                                                      : kitops::BreedMode::perParameter,
                                             seed));
}

void ElfDrumLXRProcessor::undoKitChange()
{
    if (kitHistory.empty())
        return;
    const auto target = kitHistory.back();
    kitHistory.pop_back();
    writeKitDiff (*this, currentKitBytes(), target);
}

//==============================================================================
// Phase 5 (item 6): macros / CTRL-ALL / TEMP. All message-thread, all through
// the APVTS write path (writeKitDiff), so the bridge replays the results into
// the engine like any panel edit.
void ElfDrumLXRProcessor::setMacroAssignment (int macro, int slot, int paramIdx, int depth)
{
    if (macro < 0 || macro >= kNumMacros || slot < 0 || slot >= kMacroSlots)
        return;
    auto& ms = macroState[(size_t) macro];
    ms.slots[slot].param = (paramIdx >= 0 && paramIdx < kNumKitParams) ? (uint8_t) paramIdx : (uint8_t) 0xFF;
    ms.slots[slot].depth = (int16_t) juce::jlimit (-247, 247, depth);
    ms.anchors[slot] = paramIdx >= 0 && paramIdx < kNumKitParams ? currentKitBytes()[(size_t) paramIdx] : 0;
    ms.anchorPos = ms.position;
}

ElfDrumLXRProcessor::MacroAssign ElfDrumLXRProcessor::getMacroAssignment (int macro, int slot) const
{
    if (macro < 0 || macro >= kNumMacros || slot < 0 || slot >= kMacroSlots)
        return {};
    return macroState[(size_t) macro].slots[slot];
}

void ElfDrumLXRProcessor::setMacroValue (int macro, float pos)
{
    if (macro < 0 || macro >= kNumMacros)
        return;
    auto& ms = macroState[(size_t) macro];
    ms.position = juce::jlimit (0.0f, 1.0f, pos);
    const auto cur = currentKitBytes();
    auto desired = cur;
    for (int s = 0; s < kMacroSlots; ++s)
    {
        const auto& a = ms.slots[s];
        if (a.param == 0xFF)
            continue;
        const auto& meta = elfmeta::kKitParams[a.param];
        const int v = ms.anchors[s] + juce::roundToInt ((float) a.depth * (ms.position - ms.anchorPos));
        desired[a.param] = (uint8_t) juce::jlimit ((int) meta.minValue, (int) meta.maxValue, v);
    }
    writeKitDiff (*this, cur, desired);
}

std::vector<int> ElfDrumLXRProcessor::ctrlAllParams (int paramIdx)
{
    std::vector<int> out;
    if (paramIdx < 0 || paramIdx >= kNumKitParams)
        return out;
    const auto& m = elfmeta::kKitParams[paramIdx];
    if (m.voice >= 6)
    {
        out.push_back (paramIdx);   // non-voice param: CTRL-ALL is just itself
        return out;
    }
    for (const auto& p : elfmeta::kKitParams)
        if (p.voice < 6 && juce::String (p.name) == juce::String (m.name)
            && juce::String (p.section) == juce::String (m.section))
            out.push_back ((int) p.index);
    return out;
}

void ElfDrumLXRProcessor::beginCtrlAll (int paramIdx)
{
    ctrlAllTargets = ctrlAllParams (paramIdx);
    if (ctrlAllTargets.empty())
        return;
    ctrlAllSnapshot = currentKitBytes();
    ctrlAllOn = true;
}

void ElfDrumLXRProcessor::setCtrlAllOffset (int offset)
{
    if (! ctrlAllOn)
        return;
    const auto cur = currentKitBytes();
    auto desired = cur;
    for (int t : ctrlAllTargets)
    {
        const auto& meta = elfmeta::kKitParams[t];
        desired[(size_t) t] = (uint8_t) juce::jlimit ((int) meta.minValue, (int) meta.maxValue,
                                                      (int) ctrlAllSnapshot[(size_t) t] + offset);
    }
    writeKitDiff (*this, cur, desired);
}

void ElfDrumLXRProcessor::endCtrlAll()
{
    if (! ctrlAllOn)
        return;
    ctrlAllOn = false;
    writeKitDiff (*this, currentKitBytes(), ctrlAllSnapshot);
}

void ElfDrumLXRProcessor::tempStore()
{
    tempKit = currentKitBytes();
    hasTempKit = true;
}

void ElfDrumLXRProcessor::tempRecall()
{
    if (! hasTempKit)
        return;
    writeKitDiff (*this, currentKitBytes(), tempKit);
}

void ElfDrumLXRProcessor::triggerVoice (int voice)
{
    if (voice < 0 || voice > 6) return;
    int start1, size1, start2, size2;
    triggerFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
    {
        triggerRing[(size_t) start1] = { voice, 60, 127 };
        triggerFifo.finishedWrite (1);
    }
}

//==============================================================================
// Phase 3 sequencer bridge.
//
// Threading: the editor (message thread) sends absolute-value commands through
// seqFifo and heap requests through pendingSeqRequest; the audio thread applies
// them inside processBlock, drives transport from the host playhead (or the
// internal-clock APVTS parameters in Standalone), and publishes a live-state
// struct + on-demand pattern-set snapshots back. Pattern data itself never
// leaves the firmware's seq_patternSet — the snapshot is a byte copy of it.
void ElfDrumLXRProcessor::sendSeqCmd (const SeqCmd& cmd)
{
    int start1, size1, start2, size2;
    seqFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
    {
        seqRing[(size_t) start1] = cmd;
        seqFifo.finishedWrite (1);
    }
}

void ElfDrumLXRProcessor::drainSeqCommands()
{
    int start1, size1, start2, size2;
    const int ready = seqFifo.getNumReady();
    if (ready <= 0)
        return;
    seqFifo.prepareToRead (ready, start1, size1, start2, size2);
    auto apply = [this] (const SeqCmd& c)
    {
        switch (c.type)
        {
            case SeqCmd::setStep:        lxr_seq_set_step (c.pattern, c.track, c.step, &c.stepData); break;
            case SeqCmd::setMainStep:    lxr_seq_set_main_step (c.pattern, c.track, c.step, c.a); break;
            case SeqCmd::setLength:      lxr_seq_set_track_length (c.pattern, c.track, c.value); break;
            case SeqCmd::setMute:        lxr_seq_set_mute (c.track, c.a); break;
            case SeqCmd::setShuffle:     lxr_seq_set_shuffle (c.pattern, c.value); break;
            case SeqCmd::setKitNr:       lxr_seq_set_kit_nr (c.pattern, c.value); break;
            case SeqCmd::clearPattern:   lxr_seq_clear_pattern (c.pattern); break;
            case SeqCmd::clearTrack:     lxr_seq_clear_track (c.pattern, c.track); break;
            case SeqCmd::copyPattern:    lxr_seq_copy_pattern (c.pattern, (uint8_t) c.value); break;
            case SeqCmd::euklid:         lxr_seq_euklid (c.track, c.pattern, c.value, c.a, c.b); break;
            case SeqCmd::setRoll:        lxr_seq_set_roll (c.track, c.a); break;
            case SeqCmd::setRollRate:    lxr_seq_set_roll_rate (c.value); break;
            case SeqCmd::setSongEntry:   lxr_seq_set_song_entry (c.track, c.a, c.b); break;   // track=slot, a=pos, b=value
            case SeqCmd::setSongSlot:    lxr_seq_set_song_slot (c.value); break;
            case SeqCmd::requestPattern:
                if (c.pattern != lxr_seq_active_pattern() && c.pattern != lxr_seq_pending_pattern())
                    lxr_seq_request_pattern (c.pattern);
                break;
            case SeqCmd::commitPattern:  lxr_seq_commit_pattern (c.pattern); break;
        }
    };
    for (int i = 0; i < size1; ++i) apply (seqRing[(size_t) (start1 + i)]);
    for (int i = 0; i < size2; ++i) apply (seqRing[(size_t) (start2 + i)]);
    seqFifo.finishedRead (size1 + size2);
    seqSnapRequested = true;   // the editor wants to see the result
}

bool ElfDrumLXRProcessor::readPatternData (int pattern, seqops::PatternData& out)
{
    if (pattern < 0 || pattern >= 64)
        return false;
    std::vector<uint8_t> blob;
    uint32_t version = 0;
    if (! copySeqSnapshot (blob, version))
        return false;
    if (blob.size() < lxr_seq_patternset_size())
        return false;
    seqops::decode (blob.data(), pattern, out);
    return true;
}

void ElfDrumLXRProcessor::writePatternData (int pattern, const seqops::PatternData& data)
{
    if (pattern < 0 || pattern >= 64)
        return;
    SeqCmd cmd;
    cmd.pattern = (uint8_t) pattern;
    cmd.type = SeqCmd::setShuffle;
    cmd.value = data.shuffle;
    sendSeqCmd (cmd);
    for (int t = 0; t < seqops::kTracks; ++t)
    {
        cmd.type = SeqCmd::setLength;
        cmd.track = (uint8_t) t;
        cmd.value = data.length[t];
        sendSeqCmd (cmd);
        for (int s = 0; s < seqops::kSteps; ++s)
        {
            cmd.type = SeqCmd::setStep;
            cmd.step = (uint8_t) s;
            cmd.stepData = data.steps[t][s];
            sendSeqCmd (cmd);
            cmd.type = SeqCmd::setMainStep;
            cmd.a = data.main[t][s] ? 1 : 0;
            sendSeqCmd (cmd);
        }
    }
    seqSnapRequested = true;
}

void ElfDrumLXRProcessor::postSeqRequest (std::unique_ptr<SeqRequest> req)
{
    if (auto* old = pendingSeqRequest.exchange (req.release()))
        delete old;
}

void ElfDrumLXRProcessor::applySeqRequest (SeqRequest& req)
{
    using Kind = SeqRequest::Kind;
    auto storePatName = [this] (int p, const char* nm)
    {
        const juce::ScopedLock sl (patNameLock);
        std::memcpy (patternNames[(size_t) p].data(), nm, 8);
    };
    auto storeSngName = [this] (int s, const char* nm)
    {
        const juce::ScopedLock sl (patNameLock);
        std::memcpy (songNames[(size_t) s].data(), nm, 8);
    };
    switch (req.kind)
    {
        case Kind::restoreSet:
            if (req.patternSet.size() == lxr_seq_patternset_size())
                lxr_seq_patternset_write (req.patternSet.data());
            if (req.songData.size() == lxr_seq_songdata_size())
                lxr_seq_songdata_write (req.songData.data());
            lxr_seq_set_song_slot (req.songSlot);
            {
                const juce::ScopedLock sl (patNameLock);
                patternNames = req.patNames;
                songNames = req.sngNames;
            }
            if (req.activePattern != lxr_seq_active_pattern()
                && req.activePattern != lxr_seq_pending_pattern())
                lxr_seq_request_pattern (req.activePattern);
            break;
        case Kind::loadPattern:
        {
            char nm[8];
            if (lxr_pat_load_pattern (req.path.toRawUTF8(), req.pattern, nm) == 0)
                storePatName (req.pattern, nm);
            break;
        }
        case Kind::savePattern:
        {
            std::array<char, 8> nm;
            { const juce::ScopedLock sl (patNameLock); nm = patternNames[(size_t) req.pattern]; }
            lxr_pat_save_pattern (req.path.toRawUTF8(), req.pattern, nm.data());
            break;
        }
        case Kind::loadSong:
        {
            char nm[8];
            if (lxr_pat_load_song (req.path.toRawUTF8(), req.slot, nm) == 0)
                storeSngName (req.slot, nm);
            break;
        }
        case Kind::saveSong:
        {
            std::array<char, 8> nm;
            { const juce::ScopedLock sl (patNameLock); nm = songNames[(size_t) req.slot]; }
            lxr_pat_save_song (req.path.toRawUTF8(), req.slot, nm.data());
            break;
        }
        case Kind::exportSet:
        {
            const juce::File dir (req.path);
            dir.createDirectory();
            std::array<std::array<char, 8>, 64> pn, sn;
            { const juce::ScopedLock sl (patNameLock); pn = patternNames; sn = songNames; }
            for (int p = 0; p < 64; ++p)
                lxr_pat_save_pattern (dir.getChildFile ("pattern-" + juce::String (p).paddedLeft ('0', 2) + ".pat")
                                          .getFullPathName().toRawUTF8(), p, pn[(size_t) p].data());
            for (int s = 0; s < 64; ++s)
                lxr_pat_save_song (dir.getChildFile ("song-" + juce::String (s).paddedLeft ('0', 2) + ".sng")
                                       .getFullPathName().toRawUTF8(), s, sn[(size_t) s].data());
            break;
        }
        case Kind::importSet:
        {
            const juce::File dir (req.path);
            for (int p = 0; p < 64; ++p)
            {
                const auto f = dir.getChildFile ("pattern-" + juce::String (p).paddedLeft ('0', 2) + ".pat");
                if (! f.existsAsFile()) continue;
                char nm[8];
                if (lxr_pat_load_pattern (f.getFullPathName().toRawUTF8(), p, nm) == 0)
                    storePatName (p, nm);
            }
            for (int s = 0; s < 64; ++s)
            {
                const auto f = dir.getChildFile ("song-" + juce::String (s).paddedLeft ('0', 2) + ".sng");
                if (! f.existsAsFile()) continue;
                char nm[8];
                if (lxr_pat_load_song (f.getFullPathName().toRawUTF8(), s, nm) == 0)
                    storeSngName (s, nm);
            }
            break;
        }
    }
    seqSnapRequested = true;
}

void ElfDrumLXRProcessor::seqSeekToPpq (double ppq)
{
    // Position the running sequencer at a host ppq: the step within the active
    // pattern's span (its longest track) plus the prescale phase. A ppq at or
    // before 0 is the pattern start, which seq_setRunning has already reset to.
    const double stepFloat = ppq * 4.0;
    if (stepFloat <= 0.0)
        return;
    const int active = lxr_seq_active_pattern();
    int span = 1;
    for (int t = 0; t < 7; ++t)
        span = std::max (span, lxr_seq_track_length (active, t));
    const long long g = (long long) std::floor (stepFloat);
    const int stepInSpan = (int) (g % span);
    const int prescale = (int) ((stepFloat - (double) g) * 24.0);
    if (stepInSpan != 0 || prescale != 0)
        lxr_seq_seek (stepInSpan, prescale);
}

void ElfDrumLXRProcessor::bridgeSequencer (int /*numSamples*/)
{
    // Mode toggles first: the transport logic below uses the fresh ext-sync state.
    const bool songMode = seqSongModePtr->load() > 0.5f;
    if (songMode != lastSeqSongParam) { lastSeqSongParam = songMode; lxr_seq_set_song_mode (songMode); }
    const bool extSync = seqExtSyncPtr->load() > 0.5f;
    if (extSync != lastSeqExtParam) { lastSeqExtParam = extSync; lxr_seq_set_ext_sync (extSync); }

    // Pattern requests (automatable parameter): routed through the firmware's
    // pending mechanism. The guard matters — a redundant request for the playing
    // pattern would restart it at the wrap and could clobber a song advance.
    const int patParam = juce::roundToInt (seqPatternPtr->load());
    if (patParam != lastSeqPatternParam)
    {
        lastSeqPatternParam = patParam;
        if (patParam != lxr_seq_active_pattern() && patParam != lxr_seq_pending_pattern())
            lxr_seq_request_pattern (patParam);
    }

    bool dawValid = false, dawPlaying = false;
    double dawPpq = 0.0, dawBpm = 0.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (pos->getPpqPosition().hasValue() && pos->getBpm().hasValue())
            {
                dawValid = true;
                dawPpq = *pos->getPpqPosition();
                dawBpm = *pos->getBpm();
                dawPlaying = pos->getIsPlaying();
            }

    if (extSync)
    {
        // Slaved to incoming MIDI clock via the firmware's own sync path (the
        // bytes are forwarded in the MIDI section of processBlock). Host
        // transport is ignored, as it is on the hardware when slaved.
        seqWasPlaying = false;
        seqHaveAnchor = false;
    }
    else if (dawValid)
    {
        const int bpmI = juce::jlimit (30, 300, juce::roundToInt (dawBpm));
        if (dawPlaying)
        {
            if (! seqWasPlaying)
            {
                if (bpmI != lxr_seq_get_bpm()) lxr_seq_set_bpm (bpmI);
                lxr_seq_set_running (1);
                seqSeekToPpq (dawPpq);
                seqAnchorPpq = dawPpq;
                seqAnchorSystick = host.seqSystick();
                seqHaveAnchor = true;
            }
            else
            {
                if (bpmI != lxr_seq_get_bpm()) lxr_seq_set_bpm (bpmI);
                if (seqHaveAnchor)
                {
                    // Host position vs engine musical position (systick runs at
                    // 4000 units/s; one quarter = 240000/BPM units). A loop or
                    // jump diverges instantly; a fractional host tempo against
                    // the firmware's integer BPM diverges slowly. Resync past
                    // one step (0.25 quarters) — the correction itself is the
                    // firmware-granular seek, so it never exceeds a step.
                    const double engineQ = (double) (host.seqSystick() - seqAnchorSystick)
                                           * (double) lxr_seq_get_bpm() / 240000.0;
                    const double hostQ = dawPpq - seqAnchorPpq;
                    if (std::abs (hostQ - engineQ) > 0.25)
                    {
                        seqSeekToPpq (dawPpq);
                        seqAnchorPpq = dawPpq;
                        seqAnchorSystick = host.seqSystick();
                    }
                }
            }
        }
        else
        {
            if (seqWasPlaying) lxr_seq_set_running (0);
            seqHaveAnchor = false;
        }
        seqWasPlaying = dawPlaying;
    }
    else
    {
        // Internal clock: Standalone, or a host that reports no position.
        const bool play = seqPlayPtr->load() > 0.5f;
        if (play != lastSeqPlayParam) { lastSeqPlayParam = play; lxr_seq_set_running (play ? 1 : 0); }
        const float tempo = seqTempoPtr->load();
        if (tempo != lastSeqTempoParam)
        {
            lastSeqTempoParam = tempo;
            lxr_seq_set_bpm (juce::jlimit (30, 300, juce::roundToInt (tempo)));
        }
        seqWasPlaying = false;
        seqHaveAnchor = false;
    }

    // Live state for the editor (read on the message thread under the same lock).
    {
        const juce::ScopedLock sl (seqLiveLock);
        auto& L = seqLiveState;
        L.activePattern = lxr_seq_active_pattern();
        L.pendingPattern = lxr_seq_pending_pattern();
        L.running = lxr_seq_is_running();
        L.songMode = lxr_seq_song_mode();
        L.songPos = lxr_seq_song_position();
        L.songSlot = lxr_seq_song_slot();
        L.bpm = lxr_seq_get_bpm();
        L.extSync = lxr_seq_get_ext_sync();
        L.dawActive = dawValid ? 1 : 0;
        for (int t = 0; t < 7; ++t) L.stepIndex[t] = lxr_seq_step_index (t);
        L.systick = host.seqSystick();

        // Phase 5 infinity: count track-1 loop wraps (one wrap = one bar of
        // the pattern's polymeter anchor); every 4th bar asks the message
        // thread for one guarded mutation (handleAsyncUpdate applies it).
        if (L.running && prevStep0 >= 8 && L.stepIndex[0] == 0)
        {
            if (++infinityBars % 4 == 0)
            {
                infinityPending.fetch_add (1);
                triggerAsyncUpdate();
            }
        }
        prevStep0 = L.stepIndex[0];
    }
}

void ElfDrumLXRProcessor::fulfilSeqSnapshot()
{
    if (! seqSnapRequested.exchange (false))
        return;
    const juce::ScopedLock sl (seqSnapLock);
    const size_t ps = lxr_seq_patternset_size(), sg = lxr_seq_songdata_size();
    seqSnapBlob.resize (ps + sg);
    lxr_seq_patternset_read (seqSnapBlob.data());
    lxr_seq_songdata_read (seqSnapBlob.data() + ps);
    ++seqSnapVersionCounter;
}

void ElfDrumLXRProcessor::requestSeqSnapshot() { seqSnapRequested = true; }

bool ElfDrumLXRProcessor::copySeqSnapshot (std::vector<uint8_t>& blobOut, uint32_t& versionOut)
{
    const juce::ScopedLock sl (seqSnapLock);
    if (seqSnapBlob.empty())
        return false;
    blobOut = seqSnapBlob;
    versionOut = seqSnapVersionCounter;
    return true;
}

ElfDrumLXRProcessor::SeqLive ElfDrumLXRProcessor::getSeqLive()
{
    const juce::ScopedLock sl (seqLiveLock);
    return seqLiveState;
}

void ElfDrumLXRProcessor::seqLoadPatternFile (int pattern, const juce::File& file)
{
    auto req = std::make_unique<SeqRequest>();
    req->kind = SeqRequest::Kind::loadPattern;
    req->pattern = pattern;
    req->path = file.getFullPathName();
    postSeqRequest (std::move (req));
}

void ElfDrumLXRProcessor::seqSavePatternFile (int pattern, const juce::File& file)
{
    auto req = std::make_unique<SeqRequest>();
    req->kind = SeqRequest::Kind::savePattern;
    req->pattern = pattern;
    req->path = file.getFullPathName();
    postSeqRequest (std::move (req));
}

void ElfDrumLXRProcessor::seqLoadSongFile (int slot, const juce::File& file)
{
    auto req = std::make_unique<SeqRequest>();
    req->kind = SeqRequest::Kind::loadSong;
    req->slot = slot;
    req->path = file.getFullPathName();
    postSeqRequest (std::move (req));
}

void ElfDrumLXRProcessor::seqSaveSongFile (int slot, const juce::File& file)
{
    auto req = std::make_unique<SeqRequest>();
    req->kind = SeqRequest::Kind::saveSong;
    req->slot = slot;
    req->path = file.getFullPathName();
    postSeqRequest (std::move (req));
}

void ElfDrumLXRProcessor::seqExportSet (const juce::File& folder)
{
    auto req = std::make_unique<SeqRequest>();
    req->kind = SeqRequest::Kind::exportSet;
    req->path = folder.getFullPathName();
    postSeqRequest (std::move (req));
}

void ElfDrumLXRProcessor::seqImportSet (const juce::File& folder)
{
    auto req = std::make_unique<SeqRequest>();
    req->kind = SeqRequest::Kind::importSet;
    req->path = folder.getFullPathName();
    postSeqRequest (std::move (req));
}

juce::String ElfDrumLXRProcessor::getPatternName (int pattern)
{
    if (pattern < 0 || pattern >= 64) return {};
    const juce::ScopedLock sl (patNameLock);
    return juce::String (patternNames[(size_t) pattern].data(), 8).trimEnd();
}

void ElfDrumLXRProcessor::setPatternName (int pattern, const juce::String& name)
{
    if (pattern < 0 || pattern >= 64) return;
    std::array<char, 8> n;
    n.fill (' ');
    const auto utf8 = name.toRawUTF8();
    std::memcpy (n.data(), utf8, std::min ((size_t) 8, std::strlen (utf8)));
    const juce::ScopedLock sl (patNameLock);
    patternNames[(size_t) pattern] = n;
}

juce::String ElfDrumLXRProcessor::getSongName (int slot)
{
    if (slot < 0 || slot >= 64) return {};
    const juce::ScopedLock sl (patNameLock);
    return juce::String (songNames[(size_t) slot].data(), 8).trimEnd();
}

void ElfDrumLXRProcessor::setSongName (int slot, const juce::String& name)
{
    if (slot < 0 || slot >= 64) return;
    std::array<char, 8> n;
    n.fill (' ');
    const auto utf8 = name.toRawUTF8();
    std::memcpy (n.data(), utf8, std::min ((size_t) 8, std::strlen (utf8)));
    const juce::ScopedLock sl (patNameLock);
    songNames[(size_t) slot] = n;
}

//==============================================================================
void ElfDrumLXRProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // v4 needs the pattern set, which lives on the audio thread: request a fresh
    // snapshot and wait briefly for the audio thread to fulfil it (state saves
    // happen on the message thread while audio runs). prepareToPlay already
    // requests one, so a blob normally exists; if none can be had, degrade to a
    // v3 blob (kit only) rather than write a corrupt v4.
    std::vector<uint8_t> blob;
    uint32_t version = 0;
    if (! copySeqSnapshot (blob, version))
    {
        requestSeqSnapshot();
        for (int i = 0; i < 100 && ! copySeqSnapshot (blob, version); ++i)
            juce::Thread::sleep (1);
    }
    const size_t psSize = lxr_seq_patternset_size(), sgSize = lxr_seq_songdata_size();
    const bool haveSeq = blob.size() == psSize + sgSize;

    juce::MemoryOutputStream out (destData, true);
    out.writeByte ((char) (haveSeq ? kStateVersion : 3));
    std::array<uint8_t, kNumKitParams> base, target;
    for (int i = 0; i < kNumKitParams; ++i)
        base[i] = (uint8_t) juce::roundToInt (kitParamPtrs[i]->load());
    target = morphTargetKit;   // written by the audio thread; state save is a rare, benign race
    out.write (base.data(), kNumKitParams);
    out.write (target.data(), kNumKitParams);
    out.writeFloat (morphParamPtr->load());
    {
        const juce::ScopedLock sl (nameLock);
        out.write (kitNameBytes.data(), 8);
        out.write (morphTargetNameBytes.data(), 8);   // v2 addition
    }
    for (int v = 0; v < 6; ++v)                        // v3 addition: performance faders
        out.writeFloat (faderPtrs[v]->load());
    out.writeFloat (masterFaderPtr->load());
    if (! haveSeq)
        return;

    // v4 addition: pattern/song names, the full pattern set (233,536 B), the song
    // data (4,096 B) and the sequencer settings tail. Total v4 blob ≈ 239 KB.
    {
        const juce::ScopedLock sl (patNameLock);
        for (const auto& n : patternNames) out.write (n.data(), 8);
        for (const auto& n : songNames) out.write (n.data(), 8);
    }
    out.write (blob.data(), (int) psSize);
    out.write (blob.data() + psSize, (int) sgSize);
    const auto live = getSeqLive();
    out.writeByte ((char) live.activePattern);
    out.writeByte ((char) live.songSlot);
    out.writeFloat (seqTempoPtr->load());
    out.writeByte ((char) (seqSongModePtr->load() > 0.5f ? 1 : 0));
    out.writeByte ((char) (seqExtSyncPtr->load() > 0.5f ? 1 : 0));
    out.writeByte ((char) juce::roundToInt (seqPatternPtr->load()));

    // v5 addition: the Phase 5 block — uint32 size, then a self-describing
    // block (later Phase 5 slices append inside it; readers parse what they
    // know). Block v1: version byte + morph target B kit (247 B) + its name
    // (8 B) + morphX/morphY floats + morphXY flag. Block v2 appends the
    // performance macros: 4 position floats + 16 x (param u8, depth i16).
    // Block v3 appends 5 bytes: seqInfinity, pitch16 on/voice/scale/root.
    {
        juce::MemoryOutputStream block;
        block.writeByte (3);
        std::array<uint8_t, kNumKitParams> target2 = morphTarget2Kit;   // benign race, as with target
        block.write (target2.data(), kNumKitParams);
        {
            const juce::ScopedLock sl (nameLock);
            block.write (morphTarget2NameBytes.data(), 8);
        }
        block.writeFloat (morphXPtr->load());
        block.writeFloat (morphYPtr->load());
        block.writeByte ((char) (morphXYPtr->load() > 0.5f ? 1 : 0));
        for (int m = 0; m < kNumMacros; ++m)
            block.writeFloat (macroState[(size_t) m].position);
        for (int m = 0; m < kNumMacros; ++m)
            for (int s = 0; s < kMacroSlots; ++s)
            {
                block.writeByte ((char) macroState[(size_t) m].slots[s].param);
                block.writeShort ((short) macroState[(size_t) m].slots[s].depth);
            }
        block.writeByte ((char) (seqInfinityPtr->load() > 0.5f ? 1 : 0));
        block.writeByte ((char) (pitch16Ptr->load() > 0.5f ? 1 : 0));
        block.writeByte ((char) juce::roundToInt (pitch16VoicePtr->load()));
        block.writeByte ((char) juce::roundToInt (pitch16ScalePtr->load()));
        block.writeByte ((char) juce::roundToInt (pitch16RootPtr->load()));
        out.writeInt ((int) block.getDataSize());
        out.write (block.getData(), (int) block.getDataSize());
    }
}

void ElfDrumLXRProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
    const auto version = (uint8_t) in.readByte();
    if (version < 1 || version > (char) kStateVersion)
        return;
    auto req = std::make_unique<KitRequest>();
    if (in.read (req->params.data(), kNumKitParams) != kNumKitParams) return;
    if (in.read (req->target.data(), kNumKitParams) != kNumKitParams) return;
    req->morph = in.readFloat();
    if (in.read (req->name.data(), 8) != 8) return;
    if (version >= 2)
    {
        if (in.read (req->targetName.data(), 8) != 8) return;
    }
    else
    {
        req->targetName = req->name;   // v1 saved no target name; best available label
    }
    req->hasTarget = true;
    req->hasMorph = true;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("morph")))
        p->setValueNotifyingHost (req->morph);
    // The kit request is posted at the END of this function (not here, as
    // before Phase 5): the v5 block parsed below folds morph target B into
    // the same request, and pendingRequest is a single slot — posting twice
    // could clobber the kit load.
    if (version >= 3)
    {
        // v3 addition: performance faders (plain APVTS values, not kit data).
        for (int v = 0; v < 7; ++v)
        {
            const float f = in.readFloat();
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (faderParamId (v))))
                p->setValueNotifyingHost (f);
        }
    }
    if (version >= 4)
    {
        // v4 addition: pattern set + song data + names + sequencer settings.
        auto sreq = std::make_unique<SeqRequest>();
        sreq->kind = SeqRequest::Kind::restoreSet;
        for (auto& n : sreq->patNames) if (in.read (n.data(), 8) != 8) return;
        for (auto& n : sreq->sngNames) if (in.read (n.data(), 8) != 8) return;
        const size_t psSize = lxr_seq_patternset_size(), sgSize = lxr_seq_songdata_size();
        sreq->patternSet.resize (psSize);
        sreq->songData.resize (sgSize);
        if (in.read (sreq->patternSet.data(), (int) psSize) != (int) psSize) return;
        if (in.read (sreq->songData.data(), (int) sgSize) != (int) sgSize) return;
        sreq->activePattern = (uint8_t) in.readByte();
        sreq->songSlot = (uint8_t) in.readByte();
        const float tempo = in.readFloat();
        const bool songMode = in.readByte() != 0;
        const bool extSync = in.readByte() != 0;
        const int patParam = (uint8_t) in.readByte();
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("seqTempo")))
            p->setValueNotifyingHost (p->convertTo0to1 (tempo));
        if (auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("seqSongMode")))
            p->setValueNotifyingHost (songMode ? 1.0f : 0.0f);
        if (auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("seqExtSync")))
            p->setValueNotifyingHost (extSync ? 1.0f : 0.0f);
        if (auto* p = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter ("seqPattern")))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) patParam));
        lastSeqPatternParam = patParam;   // the restore request applies it; don't re-request
        postSeqRequest (std::move (sreq));
    }
    if (version >= 5)
    {
        // v5 addition: the Phase 5 block. Parsed defensively — a malformed
        // block never costs the kit/pattern restore above; it just leaves
        // target B and the XY state at their current values.
        const int blockSize = in.readInt();
        const int remaining = (int) in.getNumBytesRemaining();
        if (blockSize > 0 && blockSize <= remaining)
        {
            std::vector<uint8_t> block ((size_t) blockSize);
            if (in.read (block.data(), blockSize) == blockSize && blockSize >= 1 + kNumKitParams + 8 + 4 + 4 + 1
                && (block[0] == 1 || block[0] == 2 || block[0] == 3))
            {
                const uint8_t* bp = block.data() + 1;
                std::memcpy (req->target2.data(), bp, kNumKitParams); bp += kNumKitParams;
                std::memcpy (req->target2Name.data(), bp, 8); bp += 8;
                req->hasTarget2 = true;
                // writeFloat == writeInt of the bit pattern (little-endian).
                auto readF = [] (const uint8_t* p)
                {
                    const uint32_t u = juce::ByteOrder::littleEndianInt (p);
                    float f;
                    std::memcpy (&f, &u, 4);
                    return f;
                };
                const float mx = readF (bp); bp += 4;
                const float my = readF (bp); bp += 4;
                const bool mxy = *bp != 0; ++bp;
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("morphX")))
                    p->setValueNotifyingHost (mx);
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("morphY")))
                    p->setValueNotifyingHost (my);
                if (auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("morphXY")))
                    p->setValueNotifyingHost (mxy ? 1.0f : 0.0f);
                if (block[0] >= 2 && blockSize - (int) (bp - block.data()) >= kNumMacros * 4 + kNumMacros * kMacroSlots * 3)
                {
                    // Macros: anchors re-capture from the restored base kit
                    // (req->params) at the restored position.
                    for (int m = 0; m < kNumMacros; ++m)
                    {
                        macroState[(size_t) m].position = readF (bp);
                        bp += 4;
                    }
                    for (int m = 0; m < kNumMacros; ++m)
                    {
                        auto& ms = macroState[(size_t) m];
                        ms.anchorPos = ms.position;
                        for (int s = 0; s < kMacroSlots; ++s)
                        {
                            ms.slots[s].param = *bp++;
                            const uint16_t du = (uint16_t) bp[0] | ((uint16_t) bp[1] << 8);
                            bp += 2;
                            int16_t dv;
                            std::memcpy (&dv, &du, 2);
                            ms.slots[s].depth = dv;
                            ms.anchors[s] = ms.slots[s].param < kNumKitParams
                                                ? req->params[ms.slots[s].param] : 0;
                        }
                    }
                }
                if (block[0] >= 3 && blockSize - (int) (bp - block.data()) >= 5)
                {
                    auto setBool = [this] (const char* id, bool on)
                    {
                        if (auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (id)))
                            p->setValueNotifyingHost (on ? 1.0f : 0.0f);
                    };
                    auto setInt = [this] (const char* id, int v)
                    {
                        if (auto* p = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (id)))
                            p->setValueNotifyingHost (p->convertTo0to1 ((float) v));
                    };
                    setBool ("seqInfinity", *bp++ != 0);
                    setBool ("pitch16", *bp++ != 0);
                    setInt ("pitch16Voice", *bp++);
                    setInt ("pitch16Scale", *bp++);
                    setInt ("pitch16Root", *bp++);
                }
            }
        }
    }
    postKitRequest (std::move (req));
}

//==============================================================================
juce::AudioProcessorEditor* ElfDrumLXRProcessor::createEditor()
{
#if ELF_NO_EDITOR
    return nullptr;   // headless builds (plugin-render) have no editor
#else
    return new ElfDrumLXRProcessorEditor (*this);
#endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ElfDrumLXRProcessor();
}
