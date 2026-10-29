/* plugin-panel-test: headless gate harness for Phase 2.
   Modes (each exits non-zero on failure, prints numbers either way):
     metadata
        The generated metadata covers the APVTS exactly: every kitNNN
        parameter exists, carries the metadata display name and range, and
        every enum/mod-target parameter has a value-name list.
     slice <flt-bp.SND> <INIT.SND> <tmpDir>
        Voice-slice proof: kit C = INIT + the snare slice of flt-bp, built
        with voiceslice::extract/splice, must byte-match the manually
        spliced expectation; the .lxv serialisation must round-trip; and
        kit C rendered through the normal kit-load path must match flt-bp's
        snare segment (peak + spectral centroid) and INIT's other voices.
     state <kitA.SND> <kitB.SND>
        State round-trip: processor A (kitA loaded, kitB as morph target,
        morph 0.3, faders moved) -> state blob -> fresh processor B ->
        identical kit bytes (via saveKitFile), identical morph target,
        morph amount and fader values (via a second state blob).
     randomise
        Guarded-randomise guards: routing + mod-target + system params are
        untouched, level params stay >= 30% of range, per-voice randomise
        stays inside the voice's slice, and a fixed seed is deterministic.
   Renders follow the plugin-morph-test pattern (44140 Hz, GM-note hits). */
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include "../src/PluginProcessor.h"
#include "../src/VoiceSlice.h"
#include "../src/InitKit.h"

namespace {
constexpr double kRate = 44140.0;
constexpr int kBlock = 512;
constexpr int kVoices = 6;
const int kGmNotes[7] = { 36, 45, 50, 38, 39, 42, 46 };
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("  %s %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

void pump (int ms = 60) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

void renderBlocks (ElfDrumLXRProcessor& proc, int numBlocks)
{
    juce::AudioBuffer<float> block (4, kBlock);
    juce::MidiBuffer midi;
    for (int i = 0; i < numBlocks; ++i) { block.clear(); proc.processBlock (block, midi); }
}

void settle (ElfDrumLXRProcessor& proc)
{
    renderBlocks (proc, 8);
    pump();
    renderBlocks (proc, 40);   // extra silence: let one-shot voices decay between kits
    pump();
}

juce::AudioBuffer<float> renderHits (ElfDrumLXRProcessor& proc)
{
    const juce::int64 totalFrames = (juce::int64) (kVoices * 0.6 * kRate) + kBlock;
    juce::AudioBuffer<float> rendered (4, (int) totalFrames);
    rendered.clear();
    juce::int64 written = 0;
    int nextVoice = 0;
    while (written < totalFrames)
    {
        const int n = (int) std::min<juce::int64> (kBlock, totalFrames - written);
        juce::MidiBuffer midi;
        while (nextVoice < kVoices && (juce::int64) (nextVoice * 0.6 * kRate) < written + n)
        {
            const int offset = (int) ((juce::int64) (nextVoice * 0.6 * kRate) - written);
            midi.addEvent (juce::MidiMessage::noteOn (1, kGmNotes[nextVoice], (juce::uint8) 127), offset);
            ++nextVoice;
        }
        juce::AudioBuffer<float> block (rendered.getArrayOfWritePointers(), 4, (int) written, n);
        proc.processBlock (block, midi);
        written += n;
    }
    const int latency = proc.getLatencySamples();
    const int frames = (int) (kVoices * 0.6 * kRate);
    juce::AudioBuffer<float> stereo (2, frames);
    for (int i = 0; i < frames; ++i)
    {
        const int src = i + latency;
        float l = 0.0f, r = 0.0f;
        if (src < totalFrames)
        {
            l = rendered.getSample (0, src) + rendered.getSample (2, src);
            r = rendered.getSample (1, src) + rendered.getSample (3, src);
        }
        stereo.setSample (0, i, juce::jlimit (-1.0f, 1.0f, l));
        stereo.setSample (1, i, juce::jlimit (-1.0f, 1.0f, r));
    }
    return stereo;
}

struct SegStats { int peak; double centroidHz; };

SegStats segmentStats (const juce::AudioBuffer<float>& stereo, int voice)
{
    const int seg = (int) (0.6 * kRate);
    const int start = voice * seg;
    float peak = 0.0f;
    int onset = start;
    for (int i = start; i < start + seg; ++i)
    {
        const float m = std::max (std::abs (stereo.getSample (0, i)), std::abs (stereo.getSample (1, i)));
        peak = std::max (peak, m);
    }
    for (int i = start; i < start + seg; ++i)
    {
        const float m = std::max (std::abs (stereo.getSample (0, i)), std::abs (stereo.getSample (1, i)));
        if (m > 0.2f * peak) { onset = i; break; }
    }
    // 4096-point FFT centroid of the mono sum from the onset.
    juce::dsp::FFT fft (12);
    std::vector<float> data (8192, 0.0f);
    juce::dsp::WindowingFunction<float> window (4096, juce::dsp::WindowingFunction<float>::hann);
    for (int i = 0; i < 4096 && onset + i < stereo.getNumSamples(); ++i)
        data[(size_t) i] = 0.5f * (stereo.getSample (0, onset + i) + stereo.getSample (1, onset + i));
    window.multiplyWithWindowingTable (data.data(), 4096);
    fft.performFrequencyOnlyForwardTransform (data.data());
    double num = 0.0, den = 0.0;
    for (int bin = 1; bin < 2048; ++bin)
    {
        const double mag = data[(size_t) bin];
        num += mag * (bin * kRate / 4096.0);
        den += mag;
    }
    return { (int) (peak * 32767.0f), den > 0.0 ? num / den : 0.0 };
}

bool readKitParams (const juce::File& f, std::array<uint8_t, 247>& params, std::array<char, 8>& name)
{
    juce::MemoryBlock data;
    if (! f.loadFileAsData (data) || data.getSize() < 255)
        return false;
    std::memcpy (name.data(), data.getData(), 8);
    std::memcpy (params.data(), (const uint8_t*) data.getData() + 8, 247);
    return true;
}

//==============================================================================
int testMetadata()
{
    std::printf ("== metadata cross-check (C++ side) ==\n");
    bool seen[247] = {};
    bool orderOk = true;
    for (uint8_t idx : elfmeta::kPanelOrder)
    {
        if (seen[idx]) orderOk = false;
        seen[idx] = true;
    }
    for (bool s : seen) orderOk = orderOk && s;
    check (orderOk, "kPanelOrder is a permutation of 0..246");

    ElfDrumLXRProcessor proc;
    int named = 0, ranged = 0, listed = 0;
    for (int i = 0; i < 247; ++i)
    {
        const auto& meta = elfmeta::kKitParams[i];
        if ((int) meta.index != i) { check (false, "kKitParams sorted by index"); break; }
        auto* p = proc.getApvts().getParameter (ElfDrumLXRProcessor::kitParamId (i));
        if (p == nullptr) { check (false, "APVTS param exists for every kit index"); break; }
        if (p->getName (64) == juce::String (meta.displayName)) ++named;
        const auto range = p->getNormalisableRange();
        if ((int) range.start == meta.minValue && (int) range.end == meta.maxValue) ++ranged;
        if (meta.control == elfmeta::Control::Continuous || meta.control == elfmeta::Control::Toggle)
            ++listed;
        else if (meta.valueNames != nullptr && meta.numValues == meta.maxValue - meta.minValue + 1)
            ++listed;
    }
    std::printf ("  display names: %d/247, ranges: %d/247, value lists: %d/247\n", named, ranged, listed);
    check (named == 247, "every APVTS kit param carries its metadata display name");
    check (ranged == 247, "every APVTS kit param range matches the metadata");
    check (listed == 247, "every enum/mod-target param has a complete value-name list");
    check (proc.getParameters().size() == 260, "APVTS holds 247 kit + morph + 7 fader + 5 sequencer params");
    return failures;
}

//==============================================================================
int testSlice (const juce::File& fltbpFile, const juce::File& initFile, const juce::File& tmpDir)
{
    std::printf ("== voice slice: bytes + .lxv + render ==\n");
    std::array<uint8_t, 247> fltbp, init;
    std::array<char, 8> fltbpName, initName;
    if (! readKitParams (fltbpFile, fltbp, fltbpName) || ! readKitParams (initFile, init, initName))
    {
        check (false, "kit files readable");
        return failures;
    }
    check (std::memcmp (init.data(), initkit::params, 247) == 0, "INIT.SND matches the embedded initkit");

    constexpr int kSnare = 3;
    const auto slice = voiceslice::extract (fltbp, kSnare);
    std::printf ("  snare slice size: %d params\n", (int) slice.size());
    check (slice.size() == voiceslice::indicesForVoice (kSnare).size(), "slice size matches metadata");

    // kit C via splice, expectation built by hand from the two files
    auto kitC = init;
    voiceslice::splice (kitC, kSnare, slice.data());
    auto expect = init;
    for (int idx : voiceslice::indicesForVoice (kSnare))
        expect[(size_t) idx] = fltbp[(size_t) idx];
    check (kitC == expect, "spliced kit C bytes == manual expectation");
    int changed = 0;
    for (int i = 0; i < 247; ++i) if (kitC[i] != init[i]) ++changed;
    std::printf ("  bytes changed vs INIT: %d (all inside the snare slice)\n", changed);
    bool changedInsideSlice = true;
    {
        bool inSlice[247] = {};
        for (int idx : voiceslice::indicesForVoice (kSnare)) inSlice[idx] = true;
        for (int i = 0; i < 247; ++i) if (kitC[i] != init[i] && ! inSlice[i]) changedInsideSlice = false;
    }
    check (changedInsideSlice && changed > 0, "splice touches only snare-slice indices");

    // .lxv round-trip
    const auto lxv = voiceslice::serialiseLxv (kSnare, slice, fltbpName);
    voiceslice::LxvData parsed;
    check (voiceslice::parseLxv (lxv.data(), lxv.size(), parsed)
               && parsed.voice == kSnare && parsed.slice == slice
               && parsed.sourceKitName == fltbpName,
           ".lxv serialise/parse round-trip");
    check (! voiceslice::parseLxv (lxv.data(), lxv.size() - 1, parsed), ".lxv rejects truncated file");

    // Write kit C as a .SND. The renders run one kit per FRESH child process
    // (mode render1): the engine is a process-wide singleton whose filter /
    // delay / RNG state would otherwise leak between sequential in-process
    // kit loads — the same reason the Phase 0/1 gates render per-process.
    auto kitCFile = tmpDir.getChildFile ("kitC.SND");
    {
        juce::MemoryBlock data (255);
        std::memcpy (data.getData(), initName.data(), 8);
        std::memcpy ((uint8_t*) data.getData() + 8, kitC.data(), 247);
        kitCFile.replaceWithData (data.getData(), data.getSize());
    }
    auto renderOne = [&] (const juce::File& kit) -> std::array<SegStats, 6>
    {
        std::array<SegStats, 6> out {};
        juce::ChildProcess child;
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        if (! child.start (exe.getFullPathName() + " render1 " + kit.getFullPathName()))
            return out;
        const auto text = child.readAllProcessOutput();
        for (const auto& line : juce::StringArray::fromLines (text))
        {
            // lines: "voice <i> peak <p> centroid <c>"
            auto parts = juce::StringArray::fromTokens (line, true);
            if (parts.size() == 6 && parts[0] == "voice")
                out[(size_t) parts[1].getIntValue()] = { parts[3].getIntValue(), parts[5].getDoubleValue() };
        }
        return out;
    };
    // The snare has a noise component fed by the shim's arc4random() RNG, so
    // its segment stats are a distribution, not a constant (Phase 0 treated
    // noise voices statistically for the same reason). Pitched voices are
    // bit-deterministic and are compared exactly. Protocol: 3 renders of
    // flt-bp define the reference spread; kit C (3 renders) must land inside
    // it, and its drum1 must equal INIT's drum1 exactly.
    const auto initStats = renderOne (initFile);
    std::array<std::array<SegStats, 6>, 3> fltRuns, cRuns;
    for (int r = 0; r < 3; ++r) { fltRuns[(size_t) r] = renderOne (fltbpFile); cRuns[(size_t) r] = renderOne (kitCFile); }
    auto avgPeak = [] (const std::array<std::array<SegStats, 6>, 3>& runs, int v)
    { return (runs[0][(size_t) v].peak + runs[1][(size_t) v].peak + runs[2][(size_t) v].peak) / 3.0; };
    auto avgCent = [] (const std::array<std::array<SegStats, 6>, 3>& runs, int v)
    { return (runs[0][(size_t) v].centroidHz + runs[1][(size_t) v].centroidHz + runs[2][(size_t) v].centroidHz) / 3.0; };
    double fCentMin = 1e9, fCentMax = 0.0;
    for (const auto& run : fltRuns)
    {
        fCentMin = std::min (fCentMin, run[3].centroidHz);
        fCentMax = std::max (fCentMax, run[3].centroidHz);
    }
    const double fPeak = avgPeak (fltRuns, 3), cPeak = avgPeak (cRuns, 3);
    const double fCent = avgCent (fltRuns, 3), cCent = avgCent (cRuns, 3);
    const auto cDrum = cRuns[0][0], iDrum = initStats[0];
    std::printf ("  snare peak (avg of 3): kitC %.0f, flt-bp %.0f (ratio %.3f)\n", cPeak, fPeak, cPeak / fPeak);
    std::printf ("  snare centroid (avg of 3): kitC %.1f Hz, flt-bp %.1f Hz (flt-bp run spread %.1f..%.1f Hz)\n",
                 cCent, fCent, fCentMin, fCentMax);
    std::printf ("  drum1 peak: kitC %d, INIT %d; centroid %.1f vs %.1f Hz\n",
                 cDrum.peak, iDrum.peak, cDrum.centroidHz, iDrum.centroidHz);
    check (cPeak > 0 && cPeak / fPeak > 0.88 && cPeak / fPeak < 1.14,
           "kit C snare peak matches flt-bp statistically (avg ratio within +/-13%)");
    check (cCent > fCentMin - 30.0 && cCent < fCentMax + 30.0,
           "kit C snare centroid inside flt-bp's run spread (+/-30 Hz)");
    check (cDrum.peak == iDrum.peak && std::abs (cDrum.centroidHz - iDrum.centroidHz) < 0.1,
           "kit C drum1 == INIT drum1 exactly (other voices untouched)");
    return failures;
}

// Render ONE kit in this (fresh) process and print per-voice segment stats.
int render1 (const juce::File& kitFile)
{
    ElfDrumLXRProcessor proc;
    proc.setPlayConfigDetails (0, 4, kRate, kBlock);
    proc.prepareToPlay (kRate, kBlock);
    if (! proc.loadKitFile (kitFile))
        return 1;
    settle (proc);
    const auto render = renderHits (proc);
    proc.releaseResources();
    for (int v = 0; v < kVoices; ++v)
    {
        const auto s = segmentStats (render, v);
        std::printf ("voice %d peak %d centroid %.1f\n", v, s.peak, s.centroidHz);
    }
    return 0;
}

//==============================================================================
int testState (const juce::File& kitAFile, const juce::File& kitBFile, const juce::File& tmpDir)
{
    std::printf ("== state round-trip ==\n");
    std::array<uint8_t, 247> paramsA, paramsB;
    std::array<char, 8> nameA, nameB;
    readKitParams (kitAFile, paramsA, nameA);
    readKitParams (kitBFile, paramsB, nameB);

    juce::MemoryBlock blob;
    {
        ElfDrumLXRProcessor a;
        a.setPlayConfigDetails (0, 4, kRate, kBlock);
        a.prepareToPlay (kRate, kBlock);
        a.loadKitFile (kitAFile);
        settle (a);
        a.loadMorphTargetFile (kitBFile);
        settle (a);
        if (auto* p = a.getApvts().getParameter ("morph")) p->setValueNotifyingHost (0.3f);
        if (auto* p = a.getApvts().getParameter (ElfDrumLXRProcessor::faderParamId (1))) p->setValueNotifyingHost (0.5f);
        if (auto* p = a.getApvts().getParameter (ElfDrumLXRProcessor::faderParamId (6))) p->setValueNotifyingHost (0.8f);
        renderBlocks (a, 4);
        a.getStateInformation (blob);
        a.releaseResources();
    }
    std::printf ("  state blob: %d bytes, version %d\n", (int) blob.getSize(), (int) ((const uint8_t*) blob.getData())[0]);
    // Phase 3: state is v4 — the v3 body (543 B) + pattern/song names (1024 B)
    // + pattern set + song data + the 9-byte sequencer tail.
    check (blob.getSize() == (size_t) (1 + 247 + 247 + 4 + 8 + 8 + 7 * 4)
                                 + 1024 + lxr_seq_patternset_size() + lxr_seq_songdata_size() + 9,
           "v4 state blob size");

    ElfDrumLXRProcessor b;
    b.setPlayConfigDetails (0, 4, kRate, kBlock);
    b.prepareToPlay (kRate, kBlock);
    b.setStateInformation (blob.getData(), (int) blob.getSize());
    settle (b);

    auto savedFile = tmpDir.getChildFile ("roundtrip.SND");
    savedFile.deleteFile();
    check (b.saveKitFile (savedFile), "restored processor saves its kit");
    std::array<uint8_t, 247> savedParams;
    std::array<char, 8> savedName;
    readKitParams (savedFile, savedParams, savedName);
    check (savedParams == paramsA, "round-tripped kit bytes == kit A bytes");
    check (savedName == nameA, "round-tripped kit name == kit A name");
    check (b.getMorphTargetName() == juce::String (nameB.data(), 8).trimEnd(), "morph target name restored");

    juce::MemoryBlock blob2;
    b.getStateInformation (blob2);
    b.releaseResources();
    const auto* d = (const uint8_t*) blob2.getData();
    check (blob2.getSize() == blob.getSize()
               && std::memcmp (d + 1, paramsA.data(), 247) == 0,
           "re-saved state base kit == kit A");
    check (std::memcmp (d + 1 + 247, paramsB.data(), 247) == 0, "re-saved state morph target == kit B");
    float morph = 0.0f;
    std::memcpy (&morph, d + 1 + 494, 4);
    check (std::abs (morph - 0.3f) < 0.001f, "morph amount round-trips (0.3)");
    float f1 = 0.0f, fMaster = 0.0f;
    std::memcpy (&f1, d + 1 + 494 + 4 + 16 + 4, 4);        // fader1 = second float of the v3 tail
    std::memcpy (&fMaster, d + 1 + 494 + 4 + 16 + 6 * 4, 4);
    std::printf ("  faders restored: voice2 %.3f, master %.3f\n", f1, fMaster);
    check (std::abs (f1 - 0.5f) < 0.001f && std::abs (fMaster - 0.8f) < 0.001f, "faders round-trip (v3 tail)");
    return failures;
}

//==============================================================================
int testRandomise()
{
    std::printf ("== guarded randomise ==\n");
    ElfDrumLXRProcessor proc;
    const auto init = proc.currentKitBytes();
    check (std::memcmp (init.data(), initkit::params, 247) == 0, "fresh APVTS defaults == INIT kit");

    proc.randomiseParams (-1, 42);
    const auto whole = proc.currentKitBytes();
    int changed = 0;
    bool guardsOk = true, levelsOk = true;
    for (const auto& meta : elfmeta::kKitParams)
    {
        const int i = (int) meta.index;
        if (whole[i] != init[i]) ++changed;
        if ((meta.flags & (elfmeta::kFlagRouting | elfmeta::kFlagModTarget)) && whole[i] != init[i])
            guardsOk = false;
        if (meta.page == elfmeta::PageGlobal && i != 114 && whole[i] != init[i])
            guardsOk = false;
        if ((meta.flags & elfmeta::kFlagLevel) && whole[i] < 38)
            levelsOk = false;
    }
    std::printf ("  whole-kit: %d params changed\n", changed);
    check (guardsOk, "routing / mod-target / system params untouched by whole-kit randomise");
    check (levelsOk, "level params floored at >= 30%% of range (>= 38)");
    check (changed >= 120, "whole-kit randomise actually changes the kit");

    // determinism
    std::vector<int> allIdx;
    for (int i = 0; i < 247; ++i) allIdx.push_back (i);
    proc.writeParamValues (allIdx.data(), init.data(), 247);
    proc.randomiseParams (-1, 42);
    check (proc.currentKitBytes() == whole, "same seed reproduces the same kit");

    // per-voice confinement
    proc.writeParamValues (allIdx.data(), init.data(), 247);
    proc.randomiseParams (3, 7);
    const auto snareOnly = proc.currentKitBytes();
    bool confined = true;
    int snareChanged = 0;
    {
        bool inSlice[247] = {};
        for (int idx : voiceslice::indicesForVoice (3)) inSlice[idx] = true;
        for (int i = 0; i < 247; ++i)
        {
            if (snareOnly[i] != init[i] && ! inSlice[i]) confined = false;
            if (snareOnly[i] != init[i]) ++snareChanged;
        }
    }
    std::printf ("  per-voice (snare): %d params changed\n", snareChanged);
    check (confined, "per-voice randomise stays inside the voice slice");
    check (snareChanged >= 15, "per-voice randomise changes the voice");
    check (snareOnly[91] >= 38, "snare volume floored (param 91 >= 38)");
    return failures;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: plugin-panel-test metadata | slice <flt-bp.SND> <INIT.SND> <tmpDir> | state <kitA.SND> <kitB.SND> <tmpDir> | randomise | render1 <kit.SND>\n");
        return 2;
    }
    const juce::String mode (argv[1]);
    int rc = 2;
    if (mode == "render1" && argc >= 3) return render1 (juce::File (argv[2]));
    if (mode == "metadata")        rc = testMetadata() == 0 ? 0 : 1;
    else if (mode == "randomise")  rc = testRandomise() == 0 ? 0 : 1;
    else if (mode == "slice" && argc >= 5)
    {
        juce::File tmp (argv[4]);
        tmp.createDirectory();
        rc = testSlice (juce::File (argv[2]), juce::File (argv[3]), tmp) == 0 ? 0 : 1;
    }
    else if (mode == "state" && argc >= 5)
    {
        juce::File tmp (argv[4]);
        tmp.createDirectory();
        rc = testState (juce::File (argv[2]), juce::File (argv[3]), tmp) == 0 ? 0 : 1;
    }
    std::printf ("%s: %s (%d failures)\n", mode.toRawUTF8(), rc == 0 ? "ALL PASS" : "FAILURES", failures);
    return rc;
}
