/* plugin-phase5-test: headless gate harness for Phase 5 (the bonus programme).
   Each mode exits non-zero on failure and prints its numbers either way.
   Modes are added per Phase 5 item; the full list lives in STATUS.md.
     blend <flt-bp.SND> <INIT.SND> <tmpDir>
        Item 1 (Kit Blender): the voice-bank layer scans/saves .lxv presets,
        a slice applied through the processor's applyVoiceSlice path (the
        Blender page's own path) byte-matches a manual splice, extraction
        from a .SND matches voiceslice::extract, and a render spot-check
        matches the Phase 2 slice gate's numbers. */
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include "../src/PluginProcessor.h"
#include "../src/Phase5.h"
#include "../src/SeqOps.h"
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
    renderBlocks (proc, 40);
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

bool readKitParams (const juce::File& f, kitops::Kit& params, std::array<char, 8>& name)
{
    juce::MemoryBlock data;
    if (! f.loadFileAsData (data) || data.getSize() < 255)
        return false;
    std::memcpy (name.data(), data.getData(), 8);
    std::memcpy (params.data(), (const uint8_t*) data.getData() + 8, 247);
    return true;
}

void prepare (ElfDrumLXRProcessor& proc)
{
    proc.setPlayConfigDetails (0, 4, kRate, kBlock);
    proc.prepareToPlay (kRate, kBlock);
}

// Defined with the librarian section below.
bool waitSnapshot (ElfDrumLXRProcessor& proc, std::vector<uint8_t>& blob);
bool filesIdentical (const juce::File& a, const juce::File& b);
void setParam (ElfDrumLXRProcessor& proc, const char* id, float plainValue);

//==============================================================================
int modeBlend (const juce::File& fltBpFile, const juce::File& initFile, const juce::File& tmpDir)
{
    std::printf ("== blend: Kit Blender / voice library ==\n");
    kitops::Kit fltBp, init;
    std::array<char, 8> fltName {}, initName {};
    check (readKitParams (fltBpFile, fltBp, fltName), "read flt-bp kit");
    check (readKitParams (initFile, init, initName), "read INIT kit");

    const auto bankDir = tmpDir.getChildFile ("voices");
    bankDir.createDirectory();

    // Save the snare of flt-bp and the drum 1 of INIT into the bank, through
    // the same voicebank::save the Blender page's "Save current voice" uses.
    juce::File savedSnare, savedDrum;
    check (voicebank::save (bankDir, "BpSnare", 3, fltBp, fltName, savedSnare), "save snare .lxv");
    check (voicebank::save (bankDir, "InitKick", 0, init, initName, savedDrum), "save drum1 .lxv");

    // Scan: the bank layer finds both, tagged with the right voice + source.
    const auto all = voicebank::scan (bankDir);
    check (all.size() == 2, "scan finds 2 presets (got " + juce::String (all.size()) + ")");
    int snareSeen = 0, drumSeen = 0;
    for (const auto& p : all)
    {
        if (p.voice == 3 && p.sourceKitName == juce::String (fltName.data(), 8).trimEnd()) ++snareSeen;
        if (p.voice == 0) ++drumSeen;
    }
    check (snareSeen == 1 && drumSeen == 1, "scan voices + source kit names correct");

    // Extraction from a .SND matches the Phase 2 voiceslice groundwork.
    std::vector<uint8_t> slice;
    std::array<char, 8> nm {};
    check (voicebank::sliceFromKitFile (fltBpFile, 3, slice, nm), "extract snare from flt-bp.SND");
    check (slice == voiceslice::extract (fltBp, 3), "extracted slice == voiceslice::extract");

    // Apply through the processor path the Blender page uses; the result must
    // byte-match a manual splice of the same slice into INIT.
    ElfDrumLXRProcessor proc;
    prepare (proc);
    proc.loadKitFile (initFile);
    settle (proc);
    voiceslice::LxvData d;
    check (voicebank::loadFile (savedSnare, d) && d.voice == 3, "load saved snare .lxv");
    check (proc.applyVoiceSlice (d.voice, d.slice), "applyVoiceSlice accepts the slice");
    pump();
    kitops::Kit expected = init;
    voiceslice::splice (expected, 3, d.slice.data());
    const auto got = proc.currentKitBytes();
    check (got == expected, "applied kit == manual byte splice");

    // Render spot-check, same shape as the Phase 2 slice gate: the blended
    // kit's snare matches flt-bp's statistically (noise voice), drum 1 is
    // INIT's exactly (deterministic pitched path).
    settle (proc);
    const auto blended = renderHits (proc);
    ElfDrumLXRProcessor ref;
    prepare (ref);
    ref.loadKitFile (fltBpFile);
    settle (ref);
    const auto fltRender = renderHits (ref);
    const auto sBlend = segmentStats (blended, 3), sRef = segmentStats (fltRender, 3);
    const auto dBlend = segmentStats (blended, 0);
    std::printf ("  snare centroid blended %.1f Hz vs flt-bp %.1f Hz; drum1 peak %d\n",
                 sBlend.centroidHz, sRef.centroidHz, dBlend.peak);
    check (std::abs (sBlend.centroidHz - sRef.centroidHz) < 0.05 * sRef.centroidHz,
           "blended snare centroid within 5% of flt-bp");
    check (dBlend.peak == 14631, "blended drum1 peak == INIT's 14631 exactly");
    return failures;
}

//==============================================================================
int modeMutate (const juce::File& fltBpFile, const juce::File& fxDistFile, const juce::File& initFile)
{
    std::printf ("== mutate/breed: guarded kit transforms ==\n");
    kitops::Kit A, B, init;
    std::array<char, 8> nm {};
    check (readKitParams (fltBpFile, A, nm), "read kit A (flt-bp)");
    check (readKitParams (fxDistFile, B, nm), "read kit B (fx-dist)");
    check (readKitParams (initFile, init, nm), "read INIT");

    auto keepsetUntouched = [] (const kitops::Kit& from, const kitops::Kit& to)
    {
        for (int i = 0; i < kitops::kNumParams; ++i)
            if (kitops::isKeepSet (i) && from[i] != to[i])
                return false;
        return true;
    };
    auto scopeUntouched = [] (const kitops::Kit& from, const kitops::Kit& to)
    {
        for (int i = 0; i < kitops::kNumParams; ++i)
            if (! kitops::inScope (i) && from[i] != to[i])
                return false;
        return true;
    };
    // The Phase 2 floor semantics: level values an op WRITES are floored at
    // 30% of range; values it leaves alone keep the kit's own value (INIT's
    // transient volumes are legitimately 0).
    auto writesFloored = [] (const kitops::Kit& from, const kitops::Kit& to)
    {
        for (int i = 0; i < kitops::kNumParams; ++i)
            if (kitops::inScope (i) && kitops::isLevel (i) && to[i] != from[i])
            {
                const auto& m = elfmeta::kKitParams[i];
                if (to[i] < m.minValue + (int) (0.30f * (float) (m.maxValue - m.minValue)))
                    return false;
            }
        return true;
    };

    // --- mutate, pure ---
    check (kitops::mutate (init, 0.0f, 42) == init, "mutate amount 0 is identity");
    bool pureOk = true, detOk = true;
    int changedAtFull = 0;
    for (uint32_t seed = 1; seed <= 50; ++seed)
    {
        for (float amt : { 0.1f, 0.5f, 1.0f })
        {
            const auto m = kitops::mutate (init, amt, seed);
            pureOk = pureOk && keepsetUntouched (init, m) && scopeUntouched (init, m) && writesFloored (init, m);
            detOk = detOk && (m == kitops::mutate (init, amt, seed));
            if (amt == 1.0f && seed == 1)
                for (int i = 0; i < kitops::kNumParams; ++i)
                    if (m[i] != init[i]) ++changedAtFull;
        }
    }
    check (pureOk, "mutate: keep-set + scope + level floor hold for 50 seeds x 3 amounts");
    check (detOk, "mutate: seed-reproducible");
    std::printf ("  mutate amount 1.0 seed 1 changed %d params\n", changedAtFull);
    check (changedAtFull > 100, "mutate at full amount actually moves the kit");

    // --- breed, pure ---
    const auto childPP = kitops::breed (A, B, kitops::BreedMode::perParameter, 7);
    check (keepsetUntouched (A, childPP), "breed per-param: keep-set from parent A");
    check (scopeUntouched (A, childPP), "breed per-param: out-of-scope from parent A");
    check (childPP != A && childPP != B, "breed per-param: child differs from both parents");
    check (childPP == kitops::breed (A, B, kitops::BreedMode::perParameter, 7),
           "breed per-param: seed-reproducible");
    bool ppOk = true;
    for (int i = 0; i < kitops::kNumParams; ++i)
        if (kitops::inScope (i) && ! kitops::isKeepSet (i) && childPP[i] != A[i] && childPP[i] != B[i])
            ppOk = false;
    check (ppOk, "breed per-param: every mutable byte is one parent's byte");

    const auto childPV = kitops::breed (A, B, kitops::BreedMode::perVoice, 7);
    check (keepsetUntouched (A, childPV) && childPV != A, "breed per-voice: keep-set from A, child differs");
    bool pvOk = true;
    for (int v = 0; v < 6; ++v)
    {
        bool allA = true, allB = true;
        for (int idx : voiceslice::indicesForVoice (v))
        {
            if (kitops::isKeepSet (idx))
                continue;
            if (childPV[idx] != A[idx]) allA = false;
            if (childPV[idx] != B[idx]) allB = false;
        }
        pvOk = pvOk && (allA || allB);
    }
    check (pvOk, "breed per-voice: each voice inherited whole from one parent");

    // --- through the processor (the Create page's own calls) ---
    ElfDrumLXRProcessor proc;
    prepare (proc);
    proc.loadKitFile (fltBpFile);
    settle (proc);
    check (proc.currentKitBytes() == A, "processor holds kit A");
    proc.mutateKit (0.5f, 42);
    pump();
    check (proc.currentKitBytes() == kitops::mutate (A, 0.5f, 42),
           "processor mutateKit == pure kitops::mutate");
    check (proc.kitHistorySize() == 1, "lineage holds the pre-mutate kit");
    proc.breedWithKit (B, false, 7);
    pump();
    const auto afterBreed = kitops::breed (kitops::mutate (A, 0.5f, 42), B,
                                           kitops::BreedMode::perParameter, 7);
    check (proc.currentKitBytes() == afterBreed, "processor breedWithKit == pure kitops::breed");
    proc.undoKitChange();
    pump();
    check (proc.currentKitBytes() == kitops::mutate (A, 0.5f, 42), "undo returns to the mutated kit");
    proc.undoKitChange();
    pump();
    check (proc.currentKitBytes() == A && proc.kitHistorySize() == 0, "second undo returns to kit A");
    return failures;
}

//==============================================================================
// Peak frequency of one voice segment (FFT argmax from the onset), the
// measurement the Phase 1 morph proof used.
double segmentPeakFreq (const juce::AudioBuffer<float>& stereo, int voice)
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
    juce::dsp::FFT fft (12);
    std::vector<float> data (8192, 0.0f);
    for (int i = 0; i < 4096 && onset + i < stereo.getNumSamples(); ++i)
        data[(size_t) i] = 0.5f * (stereo.getSample (0, onset + i) + stereo.getSample (1, onset + i));
    fft.performFrequencyOnlyForwardTransform (data.data());
    int best = 1;
    for (int bin = 2; bin < 2048; ++bin)
        if (data[(size_t) bin] > data[(size_t) best]) best = bin;
    return best * kRate / 4096.0;
}

void setParam (ElfDrumLXRProcessor& proc, const char* id, float plainValue)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
}

int modeMorphXY (const juce::File& targetAFile, const juce::File& targetBFile, const juce::File& initFile)
{
    std::printf ("== morphxy: two-target morph composition ==\n");
    kitops::Kit A, B, base;
    std::array<char, 8> nm {}, bName {};
    check (readKitParams (targetAFile, A, nm), "read target A (morph-target)");
    check (readKitParams (targetBFile, B, bName), "read target B (flt-bp)");
    check (readKitParams (initFile, base, nm), "read base (INIT)");

    // Pure composition corners, byte-exact against single-target morphs.
    kitops::Kit out, ref;
    kitops::morphXY (base, A, B, 1.0f, 0.0f, out);
    lxr_morph_kit (base.data(), A.data(), 255, ref.data());
    check (out == ref, "pure corner (1,0) == single-target morph to A");
    kitops::morphXY (base, A, B, 0.0f, 1.0f, out);
    lxr_morph_kit (base.data(), B.data(), 255, ref.data());
    check (out == ref, "pure corner (0,1) == single-target morph to B");
    kitops::morphXY (base, A, B, 1.0f, 1.0f, out);
    check (out == ref, "pure corner (1,1) == (0,1) by the documented composition");
    kitops::morphXY (base, A, B, 0.0f, 0.0f, out);
    check (out == base, "pure corner (0,0) == base");

    // Through the processor: XY (1,0) must render like the Phase 1 morph
    // proof — target A is tests/kits/morph-target.SND, whose full-morph drum
    // peak frequencies are 1975/3772/2625 Hz.
    auto renderXY = [&] (float x, float y)
    {
        ElfDrumLXRProcessor proc;
        prepare (proc);
        // Kit requests share one pending slot: settle between posts so each
        // is consumed before the next replaces it.
        proc.loadKitFile (initFile);
        settle (proc);
        proc.loadMorphTargetFile (targetAFile);
        settle (proc);
        proc.loadMorphTarget2File (targetBFile);
        settle (proc);
        setParam (proc, "morphXY", 1.0f);
        setParam (proc, "morphX", x);
        setParam (proc, "morphY", y);
        renderBlocks (proc, 4);
        pump();
        settle (proc);
        return renderHits (proc);
    };
    const auto r10 = renderXY (1.0f, 0.0f);
    // Reference: the legacy single-target path to A, measured with the same
    // probe (FFT peak bins are probe-dependent; the corner must match the
    // legacy corner under identical measurement, and sit far from INIT's
    // ~54/108/226 Hz).
    juce::AudioBuffer<float> rLegacyA;
    {
        ElfDrumLXRProcessor proc;
        prepare (proc);
        proc.loadKitFile (initFile);
        settle (proc);
        proc.loadMorphTargetFile (targetAFile);
        settle (proc);
        setParam (proc, "morph", 1.0f);
        renderBlocks (proc, 4);
        pump();
        settle (proc);
        rLegacyA = renderHits (proc);
    }
    const double f1 = segmentPeakFreq (r10, 0), f2 = segmentPeakFreq (r10, 1), f3 = segmentPeakFreq (r10, 2);
    const double g1 = segmentPeakFreq (rLegacyA, 0), g2 = segmentPeakFreq (rLegacyA, 1), g3 = segmentPeakFreq (rLegacyA, 2);
    std::printf ("  XY(1,0) drum peak freqs: %.0f / %.0f / %.0f Hz; legacy-to-A: %.0f / %.0f / %.0f Hz\n",
                 f1, f2, f3, g1, g2, g3);
    check (std::abs (f1 - g1) < 25.0 && std::abs (f2 - g2) < 25.0 && std::abs (f3 - g3) < 25.0
           && f1 > 1500.0 && f2 > 3000.0 && f3 > 2000.0,
           "XY(1,0) renders target A at full morph (matches legacy corner, far from INIT)");
    {
        bool segSame = true;
        const int seg = (int) (0.6 * kRate);
        for (int i = 0; i < seg; ++i)
            if (r10.getSample (0, i) != rLegacyA.getSample (0, i) || r10.getSample (1, i) != rLegacyA.getSample (1, i))
                segSame = false;
        check (segSame, "XY(1,0) drum 1 segment is sample-identical to legacy morph 1.0 to A");
    }

    // XY (0,1) must render identically to the legacy single-target path at
    // morph 1.0 toward B (two fresh processors, deterministic engine RNG:
    // drum 1's segment must match sample-for-sample).
    const auto r01 = renderXY (0.0f, 1.0f);
    juce::AudioBuffer<float> rLegacy;
    {
        ElfDrumLXRProcessor proc;
        prepare (proc);
        proc.loadKitFile (initFile);
        proc.loadMorphTargetFile (targetBFile);
        settle (proc);
        setParam (proc, "morph", 1.0f);
        renderBlocks (proc, 4);
        pump();
        settle (proc);
        rLegacy = renderHits (proc);
    }
    bool segSame = true;
    const int seg = (int) (0.6 * kRate);
    for (int i = 0; i < seg; ++i)
        if (r01.getSample (0, i) != rLegacy.getSample (0, i) || r01.getSample (1, i) != rLegacy.getSample (1, i))
            segSame = false;
    check (segSame, "XY(0,1) drum 1 segment is sample-identical to legacy morph 1.0 to B");
    const auto r11 = renderXY (1.0f, 1.0f);
    bool seg11 = true;
    for (int i = 0; i < seg; ++i)
        if (r11.getSample (0, i) != r01.getSample (0, i) || r11.getSample (1, i) != r01.getSample (1, i))
            seg11 = false;
    check (seg11, "XY(1,1) drum 1 segment is sample-identical to XY(0,1)");

    // State v5: target B + XY survive a save/restore; the re-saved blob is
    // byte-identical; a v4-truncated blob still loads (kit yes, XY defaults).
    juce::MemoryBlock blobA;
    {
        ElfDrumLXRProcessor a;
        prepare (a);
        a.loadKitFile (initFile);
        settle (a);
        a.loadMorphTargetFile (targetAFile);
        settle (a);
        a.loadMorphTarget2File (targetBFile);
        settle (a);
        setParam (a, "morphXY", 1.0f);
        setParam (a, "morphX", 0.25f);
        setParam (a, "morphY", 0.75f);
        renderBlocks (a, 4);
        pump();
        a.getStateInformation (blobA);
        check (((const uint8_t*) blobA.getData())[0] == 5, "state is version 5");
    }
    {
        ElfDrumLXRProcessor b;
        prepare (b);
        b.setStateInformation (blobA.getData(), (int) blobA.getSize());
        settle (b);
        check (b.getMorphTarget2Name() == juce::String (bName.data(), 8).trimEnd(),
               "target B name restored: " + b.getMorphTarget2Name());
        std::printf ("  restored morphX %.4f morphY %.4f morphXY %.2f\n",
                     b.getApvts().getRawParameterValue ("morphX")->load(),
                     b.getApvts().getRawParameterValue ("morphY")->load(),
                     b.getApvts().getRawParameterValue ("morphXY")->load());
        check (std::abs (b.getApvts().getRawParameterValue ("morphX")->load() - 0.25f) < 0.001f
               && std::abs (b.getApvts().getRawParameterValue ("morphY")->load() - 0.75f) < 0.001f
               && b.getApvts().getRawParameterValue ("morphXY")->load() > 0.5f,
               "XY amounts + switch restored");
        juce::MemoryBlock blobB;
        b.getStateInformation (blobB);
        check (blobB == blobA, "re-saved v5 blob byte-identical");
    }
    {
        // v4 compat: strip the v5 block (last 4 + 334 bytes), flip version.
        juce::MemoryBlock blob4;
        blob4.append (blobA.getData(), blobA.getSize() - 4 - 334);
        ((uint8_t*) blob4.getData())[0] = 4;
        ElfDrumLXRProcessor c;
        prepare (c);
        c.setStateInformation (blob4.getData(), (int) blob4.getSize());
        settle (c);
        check (c.currentKitBytes() == base, "v4 blob still loads (kit restored)");
    }
    return failures;
}

//==============================================================================
int modePattern()
{
    std::printf ("== pattern: generator + remix over the firmware model ==\n");
    const float density[7] = { 0.5f, 0.25f, 0.25f, 0.5f, 0.15f, 0.35f, 0.0f };

    // Pure transforms.
    const auto g1 = seqops::generate (42, density, true);
    check (g1 == seqops::generate (42, density, true), "generate is seed-deterministic");
    check (g1 != seqops::generate (43, density, true), "generate varies with the seed");
    int hits0 = 0;
    for (int s = 0; s < 64; ++s) if (g1.main[0][s]) ++hits0;
    check (hits0 == 32, "euclidean density 0.5 -> 8 hits/bar x 4 bars (got " + juce::String (hits0) + ")");
    bool rangesOk = true;
    for (int t = 0; t < 7; ++t)
        for (int s = 0; s < 64; ++s)
        {
            const auto& st = g1.steps[t][s];
            rangesOk = rangesOk && st.prob <= 100 && st.volume <= 127 && st.note <= 127
                       && st.flam <= 5 && st.shift >= -7 && st.shift <= 7;
        }
    check (rangesOk, "generated steps are inside firmware ranges");

    auto rot = g1;
    seqops::remixRotate (rot, -1, 64);
    check (rot == g1, "rotate by 64 is identity");
    seqops::remixRotate (rot, -1, 1);
    check (rot != g1 && rot.main[0][1] == g1.main[0][0], "rotate by 1 shifts steps later");
    auto thin = g1;
    seqops::remixThin (thin, 9, 1.0f);
    check (thin == g1, "thin with keep 1.0 is identity");
    seqops::remixThin (thin, 9, 0.0f);
    bool anyMain = false;
    for (int t = 0; t < 7; ++t) for (int s = 0; s < 64; ++s) anyMain = anyMain || thin.main[t][s];
    check (! anyMain, "thin with keep 0.0 silences every step");
    auto jit = g1;
    seqops::remixJitterLocks (jit, 9, 0);
    check (jit == g1, "jitter amount 0 is identity");

    // Through the processor: writePatternData lands on the firmware model,
    // other patterns are untouched, and the generated pattern plays.
    ElfDrumLXRProcessor proc;
    prepare (proc);
    settle (proc);
    std::vector<uint8_t> blob;
    check (waitSnapshot (proc, blob), "initial snapshot");
    seqops::PatternData before6;
    seqops::decode (blob.data(), 6, before6);

    proc.writePatternData (5, g1);
    renderBlocks (proc, 6);
    pump();
    check (waitSnapshot (proc, blob), "snapshot after write");
    seqops::PatternData after5, after6;
    seqops::decode (blob.data(), 5, after5);
    seqops::decode (blob.data(), 6, after6);
    check (after5 == g1, "pattern 5 in the engine == the generated PatternData");
    check (after6 == before6, "pattern 6 untouched by the write to pattern 5");

    // Remix through the same path, seed-reproducible end to end.
    auto remixed = g1;
    seqops::remixRotate (remixed, -1, 3);
    seqops::remixThin (remixed, 5, 0.7f);
    proc.writePatternData (5, remixed);
    renderBlocks (proc, 6);
    pump();
    check (waitSnapshot (proc, blob), "snapshot after remix write");
    seqops::decode (blob.data(), 5, after5);
    check (after5 == remixed, "remixed pattern in the engine == pure transform result");

    // Play it: internal clock (no playhead), pattern 5 requested, density on
    // track 0 is high — two seconds of blocks must produce real audio.
    proc.writePatternData (0, g1);
    renderBlocks (proc, 6);
    setParam (proc, "seqTempo", 120.0f);
    setParam (proc, "seqPlay", 1.0f);
    juce::AudioBuffer<float> block (4, kBlock);
    juce::MidiBuffer midi;
    float peak = 0.0f;
    for (int i = 0; i < 400; ++i)
    {
        block.clear();
        proc.processBlock (block, midi);
        for (int c = 0; c < 4; ++c)
            for (int s = 0; s < kBlock; ++s)
                peak = std::max (peak, std::abs (block.getSample (c, s)));
    }
    std::printf ("  generated-pattern playback peak: %.3f\n", peak);
    check (peak > 0.05f, "generated pattern plays through the sequencer (non-silent)");
    return failures;
}

//==============================================================================
int modePerform (const juce::File& initFile)
{
    std::printf ("== perform: macros / CTRL-ALL / TEMP ==\n");
    ElfDrumLXRProcessor proc;
    prepare (proc);
    proc.loadKitFile (initFile);
    settle (proc);
    const auto base = proc.currentKitBytes();

    // Macros: slot 0 -> param 8 (Drum 1 Coarse), depth +100; slot 1 ->
    // param 9 (Drum 1 Fine), depth -40. Anchors capture at assign.
    proc.setMacroAssignment (0, 0, 8, 100);
    proc.setMacroAssignment (0, 1, 9, -40);
    const int anchor8 = base[8], anchor9 = base[9];
    proc.setMacroValue (0, 1.0f);
    pump();
    auto cur = proc.currentKitBytes();
    auto clampP = [] (int idx, int v)
    {
        const auto& m = elfmeta::kKitParams[idx];
        return juce::jlimit ((int) m.minValue, (int) m.maxValue, v);
    };
    check (cur[8] == clampP (8, anchor8 + 50), "macro at 1.0: param 8 = anchor + depth*(1.0-0.5)");
    check (cur[9] == clampP (9, anchor9 - 20), "macro at 1.0: param 9 = anchor - 20");
    proc.setMacroValue (0, 0.0f);
    pump();
    cur = proc.currentKitBytes();
    check (cur[8] == clampP (8, anchor8 - 50) && cur[9] == clampP (9, anchor9 + 20),
           "macro at 0.0: deltas invert around the anchor position");
    int otherDiffs = 0;
    for (int i = 0; i < 247; ++i)
        if (i != 8 && i != 9 && cur[i] != base[i]) ++otherDiffs;
    check (otherDiffs == 0, "macro moved only its assigned params");

    // CTRL-ALL over Coarse (a per-voice param): all six voices move together.
    const auto targets = ElfDrumLXRProcessor::ctrlAllParams (8);
    check (targets.size() == 6, "CTRL-ALL finds 6 per-voice Coarse params (got "
           + juce::String (targets.size()) + ")");
    const auto preCtrl = proc.currentKitBytes();
    proc.beginCtrlAll (8);
    proc.setCtrlAllOffset (10);
    pump();
    cur = proc.currentKitBytes();
    bool ctrlOk = true;
    for (int t : targets)
        ctrlOk = ctrlOk && cur[t] == clampP (t, preCtrl[t] + 10);
    check (ctrlOk, "CTRL-ALL +10 lands on all six voices");
    proc.setCtrlAllOffset (0);
    pump();
    check (proc.currentKitBytes() == preCtrl, "CTRL-ALL offset 0 returns to the snapshot values");
    proc.setCtrlAllOffset (-25);
    proc.endCtrlAll();
    pump();
    check (proc.currentKitBytes() == preCtrl, "CTRL-ALL end restores the kit byte-for-byte");

    // TEMP: store, mangle, recall.
    proc.tempStore();
    const auto stored = proc.currentKitBytes();
    proc.mutateKit (0.6f, 3);
    pump();
    check (proc.currentKitBytes() != stored, "kit mangled after TEMP store");
    proc.tempRecall();
    pump();
    check (proc.currentKitBytes() == stored, "TEMP recall restores the stored kit exactly");

    // Macro persistence through state v5.
    proc.setMacroAssignment (2, 3, 100, -77);
    proc.setMacroValue (2, 0.8f);
    juce::MemoryBlock blobA;
    proc.getStateInformation (blobA);
    ElfDrumLXRProcessor b;
    prepare (b);
    b.setStateInformation (blobA.getData(), (int) blobA.getSize());
    settle (b);
    const auto a2 = b.getMacroAssignment (2, 3);
    check (a2.param == 100 && a2.depth == -77, "macro assignment survives state round-trip");
    check (std::abs (b.getMacroPosition (2) - 0.8f) < 0.0001f, "macro position survives state round-trip");
    return failures;
}

//==============================================================================
float wavPeak (const juce::File& f, int channel, double fromSec, double toSec)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr || channel >= (int) reader->numChannels)
        return -1.0f;
    const int from = (int) (fromSec * reader->sampleRate);
    const int to = std::min ((int) (toSec * reader->sampleRate), (int) reader->lengthInSamples);
    if (to <= from)
        return 0.0f;
    juce::AudioBuffer<float> buf (1, to - from);
    if (! reader->read (&buf, 0, to - from, from, channel == 0, channel == 1))
        return -1.0f;
    float peak = 0.0f;
    for (int i = 0; i < buf.getNumSamples(); ++i)
        peak = std::max (peak, std::abs (buf.getSample (0, i)));
    return peak;
}

int modeExport (const juce::File& initFile, const juce::File& tmpDir)
{
    std::printf ("== export: pattern MIDI + one-shots + stems bundle ==\n");
    tmpDir.createDirectory();
    // --- pattern -> MIDI -> parse-back ---
    auto p = seqops::clearedPattern();
    auto setStep = [&p] (int t, int s, int note, int vel, int prob, int p1nr, int p1val, int p2nr, int p2val)
    {
        p.main[t][s] = true;
        auto& st = p.steps[t][s];
        st.note = (uint8_t) note; st.volume = (uint8_t) vel; st.prob = (uint8_t) prob;
        st.p1nr = (uint8_t) p1nr; st.p1val = (uint8_t) p1val;
        st.p2nr = (uint8_t) p2nr; st.p2val = (uint8_t) p2val;
    };
    setStep (0, 0, 60, 127, 100, 20, 99, LXR_SEQ_NO_AUTO, 0);          // one plain-CC lock
    setStep (0, 4, 62, 100, 100, 200, 77, 30, 55);                     // NRPN lock + CC lock
    setStep (3, 8, 38, 90, 100, LXR_SEQ_NO_AUTO, 0, LXR_SEQ_NO_AUTO, 0);
    setStep (3, 12, 38, 90, 0, LXR_SEQ_NO_AUTO, 0, LXR_SEQ_NO_AUTO, 0);   // prob 0: never exports
    setStep (5, 2, 42, 80, 50, LXR_SEQ_NO_AUTO, 0, LXR_SEQ_NO_AUTO, 0);   // prob 50: policy decides
    const auto expected = exportops::exportedSteps (p, 3);
    check (expected.size() >= 3 && expected.size() <= 4,
           "exported step count sane (got " + juce::String (expected.size()) + ")");
    bool prob0Excluded = true;
    for (const auto& e : expected)
        if (e.track == 3 && e.step == 12) prob0Excluded = false;
    check (prob0Excluded, "probability-0 step is excluded by the policy");
    const auto midFile = tmpDir.getChildFile ("pattern3.mid");
    check (exportops::writePatternMidi (p, 3, midFile), "write pattern .mid");
    std::vector<exportops::ExportedStep> parsed;
    check (exportops::parseExportedMidi (midFile, parsed), "parse the .mid back");
    check (parsed == expected, "parsed step set == exported step set (notes, velocities, locks)");
    check (exportops::writePatternMidi (p, 3, tmpDir.getChildFile ("pattern3b.mid"))
           && filesIdentical (midFile, tmpDir.getChildFile ("pattern3b.mid")),
           "export is deterministic (identical bytes on re-export)");

    // --- one-shots ---
    ElfDrumLXRProcessor proc;
    prepare (proc);
    proc.loadKitFile (initFile);
    settle (proc);
    const auto shotDir = tmpDir.getChildFile ("oneshots");
    juce::String err;
    check (exportops::renderOneShots (proc, shotDir, err), "render one-shots: " + err);
    const float d1 = wavPeak (shotDir.getChildFile ("Drum1.wav"), 0, 0.0, 1.2);
    std::printf ("  Drum1.wav peak %.0f (expect 14631)\n", d1 * 32767.0f);
    check ((int) (d1 * 32767.0f) == 14631, "one-shot Drum1 peak == harness render peak");
    check (wavPeak (shotDir.getChildFile ("HiHat.wav"), 0, 0.0, 1.2) > 0.05f, "one-shot HiHat non-silent");
    check (wavPeak (shotDir.getChildFile ("OpenHat.wav"), 0, 0.0, 1.2) > 0.05f, "one-shot OpenHat non-silent");

    // --- stems bundle (16-channel layout, as a multi-out host arranges) ---
    ElfDrumLXRProcessor sproc;
    sproc.setPlayConfigDetails (0, 16, kRate, kBlock);
    {
        juce::AudioProcessor::BusesLayout layout;
        for (int b = 0; b < 8; ++b) layout.outputBuses.add (juce::AudioChannelSet::stereo());
        check (sproc.setBusesLayout (layout), "16-channel layout accepted");
    }
    sproc.prepareToPlay (kRate, kBlock);
    sproc.loadKitFile (initFile);
    {   // settle with 16-channel blocks (the 4-channel settle would be a
        // buffer-size violation on a stems-configured processor)
        juce::AudioBuffer<float> block (16, kBlock);
        juce::MidiBuffer midi;
        for (int i = 0; i < 48; ++i) { block.clear(); sproc.processBlock (block, midi); }
        pump();
    }
    const auto stemDir = tmpDir.getChildFile ("stems");
    check (exportops::renderStemsBundle (sproc, stemDir, err), "render stems bundle: " + err);
    // Segment windows stop at 0.59 s: the engine schedules hits at 64-frame
    // sub-block granularity, so the next voice can sound up to ~1.5 ms early.
    const float mainSeg0 = wavPeak (stemDir.getChildFile ("Main.wav"), 0, 0.0, 0.59);
    const float stemSeg0 = wavPeak (stemDir.getChildFile ("Stem-Drum1.wav"), 0, 0.0, 0.59);
    const float stem2Seg0 = wavPeak (stemDir.getChildFile ("Stem-Drum2.wav"), 0, 0.0, 0.59);
    std::printf ("  bundle seg0: Main %.0f, Stem-Drum1 %.0f, Stem-Drum2 %.0f\n",
                 mainSeg0 * 32767.0f, stemSeg0 * 32767.0f, stem2Seg0 * 32767.0f);
    check (mainSeg0 > 0.1f && std::abs (mainSeg0 - stemSeg0) < 0.001f,
           "bundle: segment 0 Main == Stem-Drum1 (only voice sounding)");
    check (stem2Seg0 == 0.0f, "bundle: Stem-Drum2 silent in segment 0 (isolation)");
    check (wavPeak (stemDir.getChildFile ("Stem-Snare.wav"), 0, 1.8, 2.4) > 0.05f,
           "bundle: Stem-Snare sounds in its segment");
    return failures;
}

//==============================================================================
int modeGroove (const juce::File& tmpDir)
{
    std::printf ("== groove: extraction from MIDI + humanize ==\n");
    tmpDir.createDirectory();
    // Craft a MIDI file: two bars, one note per 16th; velocity pattern
    // 100/110/120/130 repeating; step 4 pushed +6 ticks (late).
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (96);
    juce::MidiMessageSequence seq;
    for (int bar = 0; bar < 2; ++bar)
        for (int s = 0; s < 16; ++s)
        {
            const int tick = bar * 16 * 24 + s * 24 + (s == 4 ? 6 : 0);
            seq.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) (90 + (s % 4) * 10)), tick);
            seq.addEvent (juce::MidiMessage::noteOff (1, 60), tick + 12);
        }
    seq.updateMatchedPairs();
    mf.addTrack (seq);
    const auto file = tmpDir.getChildFile ("groove.mid");
    {
        juce::FileOutputStream out (file);
        out.setPosition (0);
        out.truncate();
        mf.writeTo (out);
        out.flush();
    }
    seqops::GrooveTemplate g;
    juce::String err;
    check (groove::extractFromMidi (file, g, err), "extract groove: " + err);
    check (g.valid, "template valid");
    std::printf ("  velScales: %.2f %.2f %.2f %.2f | shifts: %d %d %d %d\n",
                 g.velScale[0], g.velScale[1], g.velScale[2], g.velScale[3],
                 g.shift[0], g.shift[1], g.shift[2], g.shift[3]);
    check (std::abs (g.velScale[0] - 0.9f) < 0.01f && std::abs (g.velScale[1] - 1.0f) < 0.01f
           && std::abs (g.velScale[3] - 1.2f) < 0.01f, "velocity scales match the crafted pattern");
    check (g.shift[4] == 2 && g.shift[0] == 0 && g.shift[7] == 0,
           "step 4 shift == +2 units, others 0 (got " + juce::String (g.shift[4]) + ")");

    // Apply at full strength to a flat pattern.
    auto p = seqops::clearedPattern();
    for (int s = 0; s < 16; ++s)
    {
        p.main[0][s] = true;
        p.steps[0][s].volume = 100;
    }
    auto applied = p;
    seqops::applyGroove (applied, g, 1.0f);
    check (applied.steps[0][1].volume == 100 && applied.steps[0][3].volume == 120,
           "humanize scales velocities by the template");
    check (applied.steps[0][4].shift == 2, "humanize moves step 4 by the template shift");
    auto identity = p;
    seqops::applyGroove (identity, g, 0.0f);
    check (identity == p, "strength 0 is identity");
    return failures;
}

int modeInfinity (const juce::File& initFile)
{
    std::printf ("== infinity: evolving kit mutation ==\n");
    ElfDrumLXRProcessor proc;
    prepare (proc);
    proc.loadKitFile (initFile);
    settle (proc);
    const auto kit0 = proc.currentKitBytes();
    setParam (proc, "seqTempo", 240.0f);
    setParam (proc, "seqInfinity", 1.0f);
    setParam (proc, "seqPlay", 1.0f);
    // At 240 BPM one bar (16 steps) is ~86 blocks; 4 bars trigger a mutation.
    for (int i = 0; i < 700; ++i)
    {
        renderBlocks (proc, 10);
        pump (5);
        if (proc.currentKitBytes() != kit0)
            break;
    }
    const auto kit1 = proc.currentKitBytes();
    check (kit1 != kit0, "kit evolved after 4 bars of infinity playback");
    bool keepOk = true;
    for (int i = 0; i < 247; ++i)
        if (kitops::isKeepSet (i) && kit1[i] != kit0[i]) keepOk = false;
    check (keepOk, "infinity mutation respects the keep-set");
    check (proc.kitHistorySize() >= 1, "infinity mutation is on the lineage (undoable)");
    std::printf ("  bars counted toward mutation; history depth %d\n", proc.kitHistorySize());
    return failures;
}

juce::AudioBuffer<float> renderOneHit (ElfDrumLXRProcessor& proc, int note)
{
    const int frames = (int) (0.6 * kRate) + kBlock;
    juce::AudioBuffer<float> rendered (4, frames);
    rendered.clear();
    int written = 0;
    bool sent = false;
    while (written < frames)
    {
        const int n = std::min (kBlock, frames - written);
        juce::MidiBuffer midi;
        if (! sent)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 127), 0);
            sent = true;
        }
        juce::AudioBuffer<float> block (rendered.getArrayOfWritePointers(), 4, written, n);
        proc.processBlock (block, midi);
        written += n;
    }
    juce::AudioBuffer<float> stereo (2, (int) (0.6 * kRate));
    for (int i = 0; i < stereo.getNumSamples(); ++i)
    {
        stereo.setSample (0, i, juce::jlimit (-1.0f, 1.0f, rendered.getSample (0, i) + rendered.getSample (2, i)));
        stereo.setSample (1, i, juce::jlimit (-1.0f, 1.0f, rendered.getSample (1, i) + rendered.getSample (3, i)));
    }
    return stereo;
}

int modePitch16 (const juce::File& initFile)
{
    std::printf ("== pitch16: scale-locked tonal playing ==\n");
    auto freqFor = [&] (int note)
    {
        ElfDrumLXRProcessor proc;
        prepare (proc);
        proc.loadKitFile (initFile);
        settle (proc);
        setParam (proc, "pitch16", 1.0f);
        setParam (proc, "pitch16Voice", 0.0f);
        setParam (proc, "pitch16Scale", 1.0f);   // major
        setParam (proc, "pitch16Root", 0.0f);    // C
        renderBlocks (proc, 2);
        return segmentPeakFreq (renderOneHit (proc, note), 0);
    };
    // Played an octave up (72/75/76): at the kit's raw ~54 Hz the 4096-bin
    // FFT (10.8 Hz bins) cannot resolve a whole tone; at ~108 Hz it can.
    const double f60 = freqFor (72), f63 = freqFor (75), f64 = freqFor (76);
    std::printf ("  peak freqs: C %.0f Hz, Eb->D %.0f Hz, E %.0f Hz\n", f60, f63, f64);
    check (f63 < f64, "Eb quantises down to D (below E)");
    check (f64 / f60 > 1.15 && f64 / f60 < 1.40, "E/C ratio ~ 2^(4/12) = 1.26");
    check (f63 / f60 > 1.03 && f63 / f60 < 1.22, "D/C ratio ~ 2^(2/12) = 1.12");
    return failures;
}

int modeRetro (const juce::File& initFile)
{
    std::printf ("== retro: capture live playing into a pattern ==\n");
    ElfDrumLXRProcessor proc;
    prepare (proc);
    proc.loadKitFile (initFile);
    settle (proc);
    setParam (proc, "seqTempo", 120.0f);
    setParam (proc, "seqPlay", 1.0f);
    juce::AudioBuffer<float> block (4, kBlock);
    bool sent = false;
    for (int i = 0; i < 2000 && ! sent; ++i)
    {
        juce::MidiBuffer midi;
        if (proc.getSeqLive().stepIndex[0] == 2)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
            sent = true;
        }
        block.clear();
        proc.processBlock (block, midi);
    }
    check (sent, "played a note while step 2 was live");
    for (int i = 0; i < 30; ++i) { block.clear(); juce::MidiBuffer m; proc.processBlock (block, m); }
    setParam (proc, "seqPlay", 0.0f);
    for (int i = 0; i < 10; ++i) { block.clear(); juce::MidiBuffer m; proc.processBlock (block, m); }
    pump();
    check (proc.retroEventCount() >= 1, "retro ring captured the hit");
    proc.requestSeqSnapshot();
    std::vector<uint8_t> blob;
    check (waitSnapshot (proc, blob), "snapshot before commit");
    const int written = proc.retroCommit (0);
    check (written >= 1, "retroCommit wrote " + juce::String (written) + " step(s)");
    renderBlocks (proc, 6);
    pump();
    check (waitSnapshot (proc, blob), "snapshot after commit");
    seqops::PatternData p;
    seqops::decode (blob.data(), 0, p);
    const bool at2 = p.main[0][2] && p.steps[0][2].note == 60 && p.steps[0][2].volume == 127;
    const bool at3 = p.main[0][3] && p.steps[0][3].note == 60;
    check (at2 || at3, "captured step landed at step 2 (or 3, block boundary)");
    return failures;
}

//==============================================================================
// Librarian helpers: script a pattern + song into a processor through its own
// SeqCmd path, then save them through its own file requests.
void scriptPattern (ElfDrumLXRProcessor& proc, int pattern)
{
    for (int s = 0; s < 16; s += 4)
    {
        ElfDrumLXRProcessor::SeqCmd cmd;
        cmd.type = ElfDrumLXRProcessor::SeqCmd::setStep;
        cmd.pattern = (uint8_t) pattern; cmd.track = 0; cmd.step = (uint8_t) s;
        cmd.stepData.volume = 120; cmd.stepData.prob = 100; cmd.stepData.note = 60;
        cmd.stepData.p1nr = 20; cmd.stepData.p1val = 99;
        cmd.stepData.p2nr = LXR_SEQ_NO_AUTO; cmd.stepData.flam = 0; cmd.stepData.shift = 0;
        proc.sendSeqCmd (cmd);
        cmd.type = ElfDrumLXRProcessor::SeqCmd::setMainStep; cmd.a = 1;
        proc.sendSeqCmd (cmd);
    }
    ElfDrumLXRProcessor::SeqCmd len;
    len.type = ElfDrumLXRProcessor::SeqCmd::setLength;
    len.pattern = (uint8_t) pattern; len.track = 1; len.value = 12;
    proc.sendSeqCmd (len);
}

bool waitFile (ElfDrumLXRProcessor& proc, const juce::File& f, juce::int64 size)
{
    for (int i = 0; i < 200; ++i)
    {
        renderBlocks (proc, 2);
        pump (10);
        if (f.existsAsFile() && f.getSize() == size)
            return true;
    }
    return false;
}

bool waitSnapshot (ElfDrumLXRProcessor& proc, std::vector<uint8_t>& blob)
{
    uint32_t version = 0;
    proc.requestSeqSnapshot();
    for (int i = 0; i < 200; ++i)
    {
        renderBlocks (proc, 1);
        if (proc.copySeqSnapshot (blob, version))
            return true;
    }
    return false;
}

bool filesIdentical (const juce::File& a, const juce::File& b)
{
    juce::MemoryBlock x, y;
    return a.loadFileAsData (x) && b.loadFileAsData (y) && x == y;
}

int modeLibrarian (const juce::File& fltBpFile, const juce::File& initFile,
                   const juce::File& fxDistFile, const juce::File& tmpDir)
{
    std::printf ("== librarian: SD-card scan / import / packs ==\n");
    const auto card = tmpDir.getChildFile ("card");
    const auto proj1 = card.getChildFile ("PROJ01");
    const auto proj2 = card.getChildFile ("PROJ02");
    proj1.createDirectory();
    proj2.createDirectory();

    // Author a real .PAT + .SNG through the processor's own paths.
    ElfDrumLXRProcessor author;
    prepare (author);
    settle (author);
    scriptPattern (author, 3);
    ElfDrumLXRProcessor::SeqCmd song;
    song.type = ElfDrumLXRProcessor::SeqCmd::setSongEntry;
    song.track = 2;                       // song slot 2
    song.a = 0; song.b = 3;               // pos 0 -> pattern 3
    author.sendSeqCmd (song);
    song.a = 1; song.b = LXR_SONG_END;
    author.sendSeqCmd (song);
    renderBlocks (author, 10);
    pump();
    const auto patFile = proj1.getChildFile ("PAT1.PAT");
    author.seqSavePatternFile (3, patFile);
    check (waitFile (author, patFile, 3663), ".PAT saved (3663 bytes)");
    const auto sngFile = proj1.getChildFile ("SONG1.SNG");
    author.seqSaveSongFile (2, sngFile);
    check (waitFile (author, sngFile, 72), ".SNG saved (72 bytes)");

    // The pattern as authored, decoded from the author's own snapshot —
    // the reference the import must reproduce.
    std::vector<uint8_t> blob;
    check (waitSnapshot (author, blob), "author snapshot");
    seqops::PatternData authored;
    seqops::decode (blob.data(), 3, authored);

    fltBpFile.copyFileTo (proj1.getChildFile ("KITA.SND"));
    initFile.copyFileTo (proj1.getChildFile ("KITB.SND"));
    fxDistFile.copyFileTo (proj2.getChildFile ("KITD.SND"));
    initFile.copyFileTo (card.getChildFile ("LOOSE.SND"));

    // Scan.
    const auto projects = cardlib::scanCard (card);
    check (projects.size() == 3, "scan finds 3 projects (got " + juce::String (projects.size()) + ")");
    if (projects.size() == 3)
    {
        check (projects[0].name == "PROJ01" && projects[0].kits.size() == 2
               && projects[0].patterns.size() == 1 && projects[0].songs.size() == 1,
               "PROJ01 contents: 2 kits, 1 pattern, 1 song");
        check (projects[1].name == "PROJ02" && projects[1].kits.size() == 1, "PROJ02 contents: 1 kit");
        check (projects[2].name == "(loose files)" && projects[2].kits.size() == 1, "loose files project");
        check (cardlib::readKitName (projects[0].kits[0]) == "flt-bp",
               "kit header name readable: " + cardlib::readKitName (projects[0].kits[0]));
    }

    // Import through the plugin's own paths into a fresh processor.
    ElfDrumLXRProcessor proc;
    prepare (proc);
    settle (proc);
    check (proc.loadKitFile (proj1.getChildFile ("KITA.SND")), "import kit loads");
    settle (proc);
    kitops::Kit fltBp;
    std::array<char, 8> nm {};
    check (readKitParams (fltBpFile, fltBp, nm), "read reference kit");
    check (proc.currentKitBytes() == fltBp, "imported kit bytes identical to the card file's");

    // NB: SeqRequests occupy a single pending slot — each must be consumed
    // by rendered blocks before the next is posted, or it is replaced.
    proc.seqLoadPatternFile (7, patFile);
    renderBlocks (proc, 10);
    pump();
    proc.seqLoadSongFile (9, sngFile);
    renderBlocks (proc, 10);
    pump();
    std::vector<uint8_t> blob2;
    check (waitSnapshot (proc, blob2), "importer snapshot");
    seqops::PatternData imported;
    seqops::decode (blob2.data(), 7, imported);
    if (imported != authored)
    {
        for (int s = 0; s < 64; ++s)
        {
            seqops::PatternData cand;
            seqops::decode (blob2.data(), s, cand);
            if (cand == authored)
                std::printf ("  (diagnostic: authored pattern found in importer slot %d)\n", s);
        }
        std::printf ("  pattern diff: shuffle %d/%d kitNr %d/%d\n", imported.shuffle, authored.shuffle,
                     imported.kitNr, authored.kitNr);
        for (int t = 0; t < 7; ++t)
        {
            if (imported.length[t] != authored.length[t])
                std::printf ("  length track %d: %d vs %d\n", t, imported.length[t], authored.length[t]);
            for (int s = 0; s < 64; ++s)
            {
                const auto& a = imported.steps[t][s];
                const auto& b = authored.steps[t][s];
                if (imported.main[t][s] != authored.main[t][s] || a.volume != b.volume || a.prob != b.prob
                    || a.note != b.note || a.p1nr != b.p1nr || a.p1val != b.p1val || a.p2nr != b.p2nr
                    || a.p2val != b.p2val || a.flam != b.flam || a.shift != b.shift)
                    std::printf ("  step t%d s%d: main %d/%d vol %d/%d prob %d/%d note %d/%d "
                                 "p1 %d:%d/%d:%d p2 %d:%d/%d:%d flam %d/%d shift %d/%d\n",
                                 t, s, (int) imported.main[t][s], (int) authored.main[t][s],
                                 a.volume, b.volume, a.prob, b.prob, a.note, b.note,
                                 a.p1nr, a.p1val, b.p1nr, b.p1val, a.p2nr, a.p2val, b.p2nr, b.p2val,
                                 a.flam, b.flam, a.shift, b.shift);
            }
        }
    }
    check (imported == authored, "imported pattern == authored pattern (all 7 tracks x 64 steps)");
    const size_t psSize = lxr_seq_patternset_size();
    const uint8_t* songData = blob2.data() + psSize;
    check (songData[9 * 64 + 0] == 3 && songData[9 * 64 + 1] == LXR_SONG_END,
           "imported song slot 9 holds the authored entries");

    // Pack export/import round-trip.
    const auto packFile = tmpDir.getChildFile ("pack.zip");
    juce::String err;
    check (cardlib::exportPack (proj1, packFile, "Gate Tester", err), "export pack: " + err);
    {
        juce::ZipFile zip (packFile);
        check (zip.getNumEntries() == 5, "pack holds 4 files + manifest (got "
               + juce::String (zip.getNumEntries()) + ")");
        const int mi = zip.getIndexOfFileName ("manifest.json");
        check (mi >= 0, "manifest.json present");
        if (mi >= 0)
        {
            std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (mi));
            const auto manifest = juce::JSON::parse (in->readEntireStreamAsString());
            check (manifest.getProperty ("format", "").toString() == "ElfDrumLXR pack v1"
                   && manifest.getProperty ("author", "").toString() == "Gate Tester"
                   && manifest.getProperty ("files", juce::var()).size() == 4,
                   "manifest: format, author, 4 file entries");
        }
    }
    const auto destRoot = tmpDir.getChildFile ("imported");
    juce::File importedDir;
    check (cardlib::importPack (packFile, destRoot, importedDir, err), "import pack: " + err);
    check (importedDir.getFileName() == "PROJ01", "imported project folder is PROJ01");
    bool allSame = true;
    for (auto& f : proj1.findChildFiles (juce::File::findFiles, false))
        allSame = allSame && filesIdentical (f, importedDir.getChildFile (f.getFileName()));
    check (allSame, "every packed file round-trips byte-identical");
    return failures;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode (argc > 1 ? argv[1] : "");
    if (mode == "blend" && argc >= 5)
        return modeBlend (juce::File (argv[2]), juce::File (argv[3]), juce::File (argv[4])) == 0 ? 0 : 1;
    if (mode == "librarian" && argc >= 6)
        return modeLibrarian (juce::File (argv[2]), juce::File (argv[3]), juce::File (argv[4]),
                              juce::File (argv[5])) == 0 ? 0 : 1;
    if (mode == "mutate" && argc >= 5)
        return modeMutate (juce::File (argv[2]), juce::File (argv[3]), juce::File (argv[4])) == 0 ? 0 : 1;
    if (mode == "morphxy" && argc >= 5)
        return modeMorphXY (juce::File (argv[2]), juce::File (argv[3]), juce::File (argv[4])) == 0 ? 0 : 1;
    if (mode == "pattern")
        return modePattern() == 0 ? 0 : 1;
    if (mode == "perform" && argc >= 3)
        return modePerform (juce::File (argv[2])) == 0 ? 0 : 1;
    if (mode == "export" && argc >= 4)
        return modeExport (juce::File (argv[2]), juce::File (argv[3])) == 0 ? 0 : 1;
    if (mode == "groove" && argc >= 3)
        return modeGroove (juce::File (argv[2])) == 0 ? 0 : 1;
    if (mode == "infinity" && argc >= 3)
        return modeInfinity (juce::File (argv[2])) == 0 ? 0 : 1;
    if (mode == "pitch16" && argc >= 3)
        return modePitch16 (juce::File (argv[2])) == 0 ? 0 : 1;
    if (mode == "retro" && argc >= 3)
        return modeRetro (juce::File (argv[2])) == 0 ? 0 : 1;
    std::printf ("usage: plugin_phase5_test blend <flt-bp.SND> <INIT.SND> <tmpDir>\n"
                 "       plugin_phase5_test librarian <flt-bp.SND> <INIT.SND> <fx-dist.SND> <tmpDir>\n"
                 "       plugin_phase5_test mutate <flt-bp.SND> <fx-dist.SND> <INIT.SND>\n"
                 "       plugin_phase5_test morphxy <morph-target.SND> <flt-bp.SND> <INIT.SND>\n"
                 "       plugin_phase5_test pattern\n"
                 "       plugin_phase5_test perform <INIT.SND>\n"
                 "       plugin_phase5_test export <INIT.SND> <tmpDir>\n"
                 "       plugin_phase5_test groove <tmpDir> | infinity <INIT.SND> | pitch16 <INIT.SND> | retro <INIT.SND>\n");
    return 2;
}
