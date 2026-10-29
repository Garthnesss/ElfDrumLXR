#include "PluginProcessor.h"
#if ! ELF_NO_EDITOR
 #include "PluginEditor.h"
#endif
#include "InitKit.h"
#include <cstring>

extern "C" {
#include "engine.h"   // lxr_morph_kit (pure kit interpolation, firmware morph semantics)
}

namespace {
constexpr uint8_t kStateVersion = 3;   // v2: + morph target name; v3: + performance faders (v1/v2 still load)
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
    params.reserve (kNumKitParams + 8);
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
    return { params.begin(), params.end() };
}

ElfDrumLXRProcessor::ElfDrumLXRProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Main (DAC1)", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Out 2 (DAC2)", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    for (int i = 0; i < kNumKitParams; ++i)
        kitParamPtrs[i] = apvts.getRawParameterValue (kitParamId (i));
    morphParamPtr = apvts.getRawParameterValue ("morph");
    for (int v = 0; v < 6; ++v)
        faderPtrs[v] = apvts.getRawParameterValue (faderParamId (v));
    masterFaderPtr = apvts.getRawParameterValue (faderParamId (6));
    lastFaders.fill (-1.0f);   // force a fader push on the first block

    std::memcpy (baseKit.data(), initkit::params, kNumKitParams);
    std::memcpy (morphTargetKit.data(), initkit::params, kNumKitParams);
    std::array<char, 8> n {};
    std::memcpy (n.data(), initkit::name, std::min (strlen (initkit::name), (size_t) 8));
    setKitNameBytes (n);
    setMorphTargetNameBytes (n);   // startup target IS the current kit — the editor shows it
}

ElfDrumLXRProcessor::~ElfDrumLXRProcessor()
{
    cancelPendingUpdate();
    delete pendingRequest.exchange (nullptr);
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

//==============================================================================
void ElfDrumLXRProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    host.prepare (sampleRate);
    setLatencySamples (host.getLatencySamples());
    scratch.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
    engineKitLoaded = false;   // (re)load the kit into the engine on the next block
}

void ElfDrumLXRProcessor::releaseResources() { host.release(); }

bool ElfDrumLXRProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.outputBuses.size() > 2)
        return false;
    if (layouts.outputBuses.size() == 2
        && layouts.outputBuses[1] != juce::AudioChannelSet::stereo()
        && ! layouts.outputBuses[1].isDisabled())
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

void ElfDrumLXRProcessor::applyKitRequest (KitRequest& req)
{
    if (req.targetOnly)
    {
        // Explicit morph target (editor's "Load target .SND"): the current kit and the
        // APVTS bridge are untouched; re-apply at the current morph amount so a target
        // swapped in mid-morph is heard immediately.
        morphTargetKit = req.params;
        setMorphTargetNameBytes (req.name);
        const float morph = morphParamPtr->load();
        std::array<uint8_t, kNumKitParams> desired;
        if (morph > 0.001f)
            lxr_morph_kit (baseKit.data(), morphTargetKit.data(),
                           (uint8_t) juce::roundToInt (morph * 255.0f), desired.data());
        else
            desired = baseKit;
        host.loadKit (desired.data());
        lastBaseKit = baseKit;
        lastSentKit = desired;
        lastMorph = morph;
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
    setKitNameBytes (req.name);
    if (req.hasMorph)
        lastMorph = -1.0f;   // force the bridge to re-evaluate with the restored morph below

    // Full load through the wrapper's single entry path, honouring the current morph amount.
    const float morph = req.hasMorph ? req.morph : morphParamPtr->load();
    std::array<uint8_t, kNumKitParams> desired;
    if (morph > 0.001f)
        lxr_morph_kit (baseKit.data(), morphTargetKit.data(),
                       (uint8_t) juce::roundToInt (morph * 255.0f), desired.data());
    else
        desired = baseKit;
    host.loadKit (desired.data());
    lastBaseKit = baseKit;
    lastSentKit = desired;
    lastMorph = morph;
    engineKitLoaded = true;

    bridgeSuspended = true;      // the async updater aligns APVTS with baseKit; don't fight it
    apvtsSyncPending = true;
    triggerAsyncUpdate();
}

void ElfDrumLXRProcessor::handleAsyncUpdate()
{
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
    std::array<uint8_t, kNumKitParams> desired;
    if (lastMorph > 0.001f)
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

    if (! engineKitLoaded)
    {
        lastBaseKit = cur;
        lastMorph = morph;
        const auto desired = desiredKit();
        host.loadKit (desired.data());
        lastSentKit = desired;
        engineKitLoaded = true;
        return;
    }

    if (cur == lastBaseKit && morph == lastMorph)
        return;   // nothing moved: leave the engine (and any parser-driven CC state) alone

    lastBaseKit = cur;
    lastMorph = morph;
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

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
        {
            const int chan = msg.getChannel() - 1, note = msg.getNoteNumber(), vel = msg.getVelocity();
            const uint64_t at = blockStart + (uint64_t) msg.getTimeStamp();
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
                        host.queueHit (v, note, vel, at);
                }
                else if (note == kGmMap[v])
                {
                    // Stock firmware notes are all 48 — an inert placeholder upstream (the
                    // parameterArray hooks for PAR_MIDI_NOTE* are commented out in the firmware),
                    // so while they are, the GM drum map selects the voice (as PHX-D01 does)
                    // and the played note's offset from the map note pitches it around 60.
                    host.queueHit (v, 60 + (note - kGmMap[v]), vel, at);
                }
            }
        }
        else if (msg.isNoteOff())
        {
            // one-shot drums: the firmware ignores note offs, and so do we
        }
        else if (! msg.isSysEx() && ! msg.isMidiClock() && ! msg.isMidiStart()
                 && ! msg.isMidiStop() && ! msg.isMidiContinue())
        {
            // Everything else (CC incl. the NRPN sequence, pitch bend, program change,
            // pressure) goes to the firmware's own parser as a raw byte stream.
            host.midiBytes (msg.getRawData(), msg.getRawDataSize());
        }
    }

    // --- Audio: 4 engine jacks -> bus 1 (jacks 1/2 = DAC1), bus 2 (jacks 3/4 = DAC2) ---
    const int numSamples = buffer.getNumSamples();
    if ((int) scratch.size() < numSamples)
        scratch.assign ((size_t) numSamples, 0.0f);   // host exceeded its declared block size
    std::array<float*, 4> chans;
    for (int c = 0; c < 4; ++c)
        chans[c] = (c < buffer.getNumChannels()) ? buffer.getWritePointer (c)
                                                 : scratch.data();
    host.render (chans.data(), numSamples);
    if (buffer.getNumChannels() > 4)
        for (int c = 4; c < buffer.getNumChannels(); ++c)
            buffer.clear (c, 0, numSamples);
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
void ElfDrumLXRProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream out (destData, true);
    out.writeByte ((char) kStateVersion);
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
}

void ElfDrumLXRProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
    const auto version = (uint8_t) in.readByte();
    if (version != 1 && version != 2 && version != (char) kStateVersion)
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
    postKitRequest (std::move (req));
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
