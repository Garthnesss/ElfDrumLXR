/* plugin-seq-test: Phase 3 gate harnesses for the plugin sequencer.
   Modes:
     log       Gate (b): drive the processor with a fake AudioPlayHead (120 BPM,
               playing from ppq 0) over the shared script (tools/seq-script.h);
               pattern 1 is requested via the APVTS parameter when ppq crosses
               1 bar. The trigger log (tools/seq-log.h; the binary links the
               engine with --wrap on the firmware's trigger entry points) goes
               to stdout — the gate is a byte diff against tools/seq-ref.
     audio     Gate (e): drum1 steps 0/4/8/12 at 120 BPM; rendered onsets must
               land within ±1 engine block (64 samples) of the expected grid.
     seqstate  State v5 round-trip: script in processor A -> state blob ->
               engine corrupted -> fresh processor B restores -> snapshot and
               re-saved blob identical to A's; plus a hand-truncated v3 blob
               still loads (kit yes, patterns stay at firmware init). */
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "../src/PluginProcessor.h"
#include "seq-log.h"
#include "seq-script.h"

namespace {
constexpr double kRate = 44140.0;
constexpr int kBlock = 512;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::fprintf (stderr, "  %s %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

class FakePlayHead : public juce::AudioPlayHead
{
public:
    double ppq = 0.0, bpm = 120.0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setPpqPosition (ppq);
        info.setBpm (bpm);
        info.setIsPlaying (playing);
        info.setTimeInSamples ((juce::int64) (ppq * 60.0 / bpm * kRate));
        return info;
    }
};

void runBlocks (ElfDrumLXRProcessor& proc, FakePlayHead& ph, int numBlocks, int& blockCounter)
{
    juce::AudioBuffer<float> block (4, kBlock);
    juce::MidiBuffer midi;
    for (int i = 0; i < numBlocks; ++i)
    {
        ph.ppq = blockCounter * (double) kBlock * (ph.bpm / 60.0) / kRate;
        block.clear();
        proc.processBlock (block, midi);
        ++blockCounter;
    }
}

bool waitSnapshot (ElfDrumLXRProcessor& proc, FakePlayHead& ph, int& bc,
                   std::vector<uint8_t>& blob, uint32_t& version)
{
    proc.requestSeqSnapshot();
    for (int i = 0; i < 200; ++i)
    {
        runBlocks (proc, ph, 1, bc);
        if (proc.copySeqSnapshot (blob, version))
            return true;
    }
    return false;
}

//==============================================================================
int modeLog()
{
    ElfDrumLXRProcessor proc;
    FakePlayHead ph;
    proc.setPlayHead (&ph);
    proc.setPlayConfigDetails (0, 4, kRate, kBlock);
    proc.prepareToPlay (kRate, kBlock);
    seq_script_build();          // engine initialised by prepare; single-threaded here
    seqlog_reset();

    juce::AudioBuffer<float> block (4, kBlock);
    juce::MidiBuffer midi;
    int bc = 0;
    bool switched = false;
    while (bc < 4000)
    {
        ph.ppq = bc * (double) kBlock * (ph.bpm / 60.0) / kRate;
        if (! switched && ph.ppq >= 4.0)   // 1 bar in: request pattern 1 (APVTS, as a host would)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterInt*> (proc.getApvts().getParameter ("seqPattern")))
                p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
            switched = true;
        }
        block.clear();
        proc.processBlock (block, midi);
        ++bc;
        if (proc.getSeqLive().systick >= SEQ_SCRIPT_TOTAL_UNITS)
            break;
    }
    proc.releaseResources();
    std::fprintf (stderr, "plugin log run: %d triggers after %d blocks, active pattern %d\n",
                  seqlog_count(), bc, proc.getSeqLive().activePattern);
    check (switched, "pattern request was issued");
    check (seqlog_count() > 40, "plugin run produced triggers");
    return failures ? 1 : 0;
}

//==============================================================================
int modeAudio()
{
    seqlog_set_out (std::fopen ("/dev/null", "w"));
    ElfDrumLXRProcessor proc;
    FakePlayHead ph;
    proc.setPlayHead (&ph);
    proc.setPlayConfigDetails (0, 4, kRate, kBlock);
    proc.prepareToPlay (kRate, kBlock);

    lxr_seq_clear_pattern (0);
    for (int st : { 0, 4, 8, 12 })
        lxr_seq_set_main_step (0, 0, st, 1);   // cleared defaults: vol 127, note 60, prob 100

    const int total = 2 * (int) kRate + 4096;  // 1 bar at 120 BPM = 2 s, plus tail room
    juce::AudioBuffer<float> rendered (4, total);
    rendered.clear();
    int bc = 0, written = 0;
    juce::MidiBuffer midi;
    while (written < total)
    {
        const int n = std::min (kBlock, total - written);
        ph.ppq = bc * (double) kBlock * (ph.bpm / 60.0) / kRate;
        juce::AudioBuffer<float> block (rendered.getArrayOfWritePointers(), 4, written, n);
        proc.processBlock (block, midi);
        written += n;
        ++bc;
    }
    const int latency = proc.getLatencySamples();
    proc.releaseResources();

    // Onsets on the summed main bus, latency-trimmed like plugin-render.
    std::vector<float> mono ((size_t) total);
    for (int i = 0; i < total; ++i)
        mono[(size_t) i] = rendered.getSample (0, i) + rendered.getSample (2, i);
    std::vector<int> onsets;
    bool hot = false;
    int quietRun = 0;
    for (int i = latency; i < total; ++i)
    {
        const float a = std::abs (mono[(size_t) i]);
        if (a < 0.02f) { if (++quietRun > 2000) hot = false; }
        else quietRun = 0;
        if (! hot && a > 0.08f) { onsets.push_back (i - latency); hot = true; }
    }
    std::fprintf (stderr, "audio gate: latency %d, %d onsets:", latency, (int) onsets.size());
    for (int o : onsets) std::fprintf (stderr, " %d", o);
    std::fprintf (stderr, "\n");
    // The render spans the loop point: the 4 pattern hits (steps 0/4/8/12)
    // plus the wrapped bar-2 step 0 (step 16) are all legitimate onsets.
    check (onsets.size() == 4 || onsets.size() == 5,
           "expected 4 onsets (+ loop wrap), got " + juce::String ((int) onsets.size()));
    // Step k fires at systick 1 + 500k (see seq-ref log); engine frame = systick * 44140/4000.
    const int steps[5] = { 0, 4, 8, 12, 16 };
    for (int k = 0; k < 5 && k < (int) onsets.size(); ++k)
    {
        const double expected = (1.0 + 500.0 * steps[k]) * kRate / 4000.0;
        const int delta = onsets[(size_t) k] - (int) std::llround (expected);
        std::fprintf (stderr, "  step %d: onset %d, expected %.0f, delta %+d samples\n",
                      steps[k], onsets[(size_t) k], expected, delta);
        check (std::abs (delta) <= 64, "step " + juce::String (steps[k]) + " onset within 1 engine block");
    }
    return failures ? 1 : 0;
}

//==============================================================================
int modeSeqState (const juce::File& kitFile)
{
    seqlog_set_out (std::fopen ("/dev/null", "w"));

    std::array<uint8_t, 247> kitParams {};
    {
        juce::MemoryBlock data;
        check (kitFile.loadFileAsData (data) && data.getSize() >= 255, "kit file readable");
        std::memcpy (kitParams.data(), (const uint8_t*) data.getData() + 8, 247);
    }
    auto settle = [] (ElfDrumLXRProcessor& p, FakePlayHead& ph, int& bc)
    {
        runBlocks (p, ph, 8, bc);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
        runBlocks (p, ph, 8, bc);
    };

    // A: load a distinctive kit, build the script, save state.
    juce::MemoryBlock blobA;
    std::vector<uint8_t> snapA;
    {
        ElfDrumLXRProcessor a;
        FakePlayHead ph; ph.playing = false;
        a.setPlayHead (&ph);
        a.setPlayConfigDetails (0, 4, kRate, kBlock);
        a.prepareToPlay (kRate, kBlock);
        check (a.loadKitFile (kitFile), "A loads kit");
        int bc = 0;
        settle (a, ph, bc);
        seq_script_build();
        a.setPatternName (3, "KICKA");
        a.setPatternName (63, "LAST");
        uint32_t v = 0;
        check (waitSnapshot (a, ph, bc, snapA, v), "snapshot A");
        a.getStateInformation (blobA);
        a.releaseResources();
    }
    check (((const uint8_t*) blobA.getData())[0] == 5, "state A is version 5");
    // v5 size = v4 size + uint32 block size + the 334-byte Phase 5 block v3
    // (v1's 265: version + target B kit 247 + name 8 + morphX/Y floats +
    // morphXY flag; v2 adds 64 of macros; v3 adds 5 of infinity/pitch16).
    const size_t expectSize = 543 + 1024 + lxr_seq_patternset_size() + lxr_seq_songdata_size() + 9
                              + 4 + 334;
    check (blobA.getSize() == expectSize, "state A size " + juce::String ((int) blobA.getSize())
                                          + " == " + juce::String ((int) expectSize));

    // Corrupt the (currently unowned) engine globals, then B restores from
    // the blob. Pre-Phase-4 this dirtied B's starting point directly; with
    // per-instance state B starts pristine either way, and the restore
    // assertions below verify the blob lands completely.
    lxr_seq_clear_pattern (0);
    {
        LxrStep junk {}; junk.volume = 3; junk.prob = 3; junk.note = 3;
        junk.p1nr = junk.p2nr = 0xff;
        lxr_seq_set_step (1, 0, 0, &junk);
    }
    juce::MemoryBlock blobB;
    std::vector<uint8_t> snapB;
    {
        ElfDrumLXRProcessor b;
        FakePlayHead ph; ph.playing = false;
        b.setPlayHead (&ph);
        b.setPlayConfigDetails (0, 4, kRate, kBlock);
        b.prepareToPlay (kRate, kBlock);
        b.setStateInformation (blobA.getData(), (int) blobA.getSize());
        int bc = 0;
        settle (b, ph, bc);   // kit request + async APVTS sync, as in PanelTest
        uint32_t v = 0;
        check (waitSnapshot (b, ph, bc, snapB, v), "snapshot B after restore");
        check (snapB == snapA, "restored pattern set + song data identical to A");
        check (b.getPatternName (3) == "KICKA" && b.getPatternName (63) == "LAST",
               "pattern names restored");
        b.getStateInformation (blobB);
        b.releaseResources();
    }
    check (blobB.getSize() == blobA.getSize()
               && std::memcmp (blobB.getData(), blobA.getData(), blobA.getSize()) == 0,
           "re-saved state blob byte-identical to A");

    // v3 compat: truncate A's blob to the v3 body and flip the version byte.
    // The kit must restore; the pattern set must NOT change. Since Phase 4
    // (5) instances are isolated, C starts from the pristine set (NOT from
    // whatever A/B left behind — before Phase 4 the singleton made snapA
    // the only possible expectation); "untouched" is measured against C's
    // own pre-load snapshot.
    {
        juce::MemoryBlock blob3;
        blob3.append (blobA.getData(), 543);
        ((uint8_t*) blob3.getData())[0] = 3;
        ElfDrumLXRProcessor c;
        FakePlayHead ph; ph.playing = false;
        c.setPlayHead (&ph);
        c.setPlayConfigDetails (0, 4, kRate, kBlock);
        c.prepareToPlay (kRate, kBlock);
        int bc = 0;
        uint32_t v = 0;
        std::vector<uint8_t> snapC0;
        check (waitSnapshot (c, ph, bc, snapC0, v), "snapshot C before v3 load");
        check (snapC0 != snapA, "fresh instance does not inherit A's pattern set (Phase 4 isolation)");
        c.setStateInformation (blob3.getData(), (int) blob3.getSize());
        settle (c, ph, bc);
        check (c.currentKitBytes() == kitParams, "v3 load restores the kit");
        std::vector<uint8_t> snapC;
        check (waitSnapshot (c, ph, bc, snapC, v), "snapshot C after v3 load");
        check (snapC == snapC0, "v3 load leaves the pattern set untouched");
        c.releaseResources();
    }
    return failures ? 1 : 0;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::String mode = argc > 1 ? argv[1] : "";
    if (mode == "log") return modeLog();
    if (mode == "audio") return modeAudio();
    if (mode == "seqstate" && argc > 2)
        return modeSeqState (juce::File (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2])));
    std::fprintf (stderr, "usage: plugin-seq-test log|audio|seqstate <kit.SND>\n");
    return 2;
}
