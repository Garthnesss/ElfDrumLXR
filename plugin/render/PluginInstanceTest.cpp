/* plugin-instance-test — the Phase 4 (5) all-or-nothing gate for
   per-instance engines.

   Why partner-invariance is the criterion: the firmware's shared noise
   LFSR and voice states evolve with every generated block, so one
   instance's SECOND render in a process legitimately differs from its
   first (solo-vs-later comparisons are meaningless here — cross-process
   golden renders are the bit-exactness anchor, and they live in the
   other gates). What per-instance isolation must guarantee is narrower
   and stronger: with both instances starting from the pristine state,
   A's rendered output depends ONLY on A's kit, pattern set and inputs —
   never on the identity or activity of the instance it is interleaved
   with. So the gate renders A (fx-dist, drum pattern) interleaved 1:1
   with two DIFFERENT partners (flt-bp/snare pattern, then fx-verb/drum3
   pattern) from fresh processors each time. A's two renders must be
   bit-identical, all voices including noise; the partners' renders must
   differ from each other (sanity), and every pattern-set snapshot must
   be byte-stable and instance-distinct.
   Exit 0 and print PASS lines, or fail loudly. If the runtime swap does
   not engage on this platform the test fails (on Linux it must engage). */
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "../src/PluginProcessor.h"
#include "../src/EngineSwap.h"
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr double kRate = 44140.0;
constexpr int kBlock = 512;
int failures = 0;

#define CHECK(cond, msg) do { if (cond) std::printf ("PASS %s\n", msg); \
    else { std::printf ("FAIL %s\n", msg); ++failures; } } while (0)

struct Rig
{
    std::unique_ptr<ElfDrumLXRProcessor> proc;
    juce::AudioBuffer<float> buffer { 4, kBlock };
    juce::MidiBuffer midi;
    int64_t frame = 0;
};

void runBlocks (Rig& r, int n)
{
    for (int i = 0; i < n; ++i)
    {
        r.buffer.clear();
        r.midi.clear();
        r.proc->processBlock (r.buffer, r.midi);
        r.frame += kBlock;
    }
}

// One settle round on BOTH rigs, alternately — even loading is interleaved.
void settlePair (Rig& a, Rig& b)
{
    for (int i = 0; i < 3; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        runBlocks (a, 8);
        runBlocks (b, 8);
    }
}

void setMainSteps (Rig& r, int track, std::initializer_list<int> steps)
{
    ElfDrumLXRProcessor::SeqCmd cmd;
    cmd.type = ElfDrumLXRProcessor::SeqCmd::setMainStep;
    cmd.pattern = 0;
    cmd.track = (uint8_t) track;
    cmd.a = 1;
    for (int s : steps) { cmd.step = (uint8_t) s; r.proc->sendSeqCmd (cmd); }
}

std::vector<uint8_t> snapshot (Rig& r)
{
    r.proc->requestSeqSnapshot();
    runBlocks (r, 4);
    std::vector<uint8_t> blob;
    uint32_t ver = 0;
    if (! r.proc->copySeqSnapshot (blob, ver)) return {};
    return blob;
}

// Render the standard hit layout on r while the partner processes the
// same block sizes in silence, strictly alternating 1:1.
std::vector<float> renderHits (Rig& r, Rig& partner)
{
    static const int gmNotes[7] = { 36, 45, 50, 38, 39, 42, 46 };
    const int total = (int) (6 * 0.6 * kRate);
    std::vector<float> out;
    out.reserve ((size_t) total * 2);
    int nextHit = 0;
    const int64_t base = r.frame;
    while ((int) out.size() / 2 < total)
    {
        const int n = std::min (kBlock, total - (int) out.size() / 2);
        r.buffer.clear();
        r.midi.clear();
        while (nextHit < 7 && base + (int64_t) (nextHit * 0.6 * kRate) < r.frame + n)
        {
            const int off = (int) (base + (int64_t) (nextHit * 0.6 * kRate) - r.frame);
            if (0 <= off && off < n)
                r.midi.addEvent (juce::MidiMessage::noteOn (1, gmNotes[nextHit], 1.0f), off);
            ++nextHit;
        }
        juce::AudioBuffer<float> block (r.buffer.getArrayOfWritePointers(), 4, 0, n);
        r.proc->processBlock (block, r.midi);
        r.frame += n;
        for (int i = 0; i < n; ++i)
            { out.push_back (r.buffer.getSample (0, i) + r.buffer.getSample (2, i));
              out.push_back (r.buffer.getSample (1, i) + r.buffer.getSample (3, i)); }
        partner.buffer.clear();
        partner.midi.clear();
        juce::AudioBuffer<float> ob (partner.buffer.getArrayOfWritePointers(), 4, 0, n);
        partner.proc->processBlock (ob, partner.midi);
        partner.frame += n;
    }
    return out;
}

bool writeWav (const char* path, const std::vector<float>& interleaved)
{
    FILE* f = std::fopen (path, "wb");
    if (! f) return false;
    const uint32_t frames = (uint32_t) interleaved.size() / 2;
    const uint32_t bytes = frames * 4;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + bytes); std::fwrite ("WAVEfmt ", 1, 8, f);
    w32 (16); w16 (1); w16 (2); w32 ((uint32_t) kRate); w32 ((uint32_t) kRate * 4); w16 (4); w16 (16);
    std::fwrite ("data", 1, 4, f); w32 (bytes);
    for (float v : interleaved)
    {
        v = juce::jlimit (-1.0f, 1.0f, v);
        int16_t s = (int16_t) (v * 32767.0f);
        std::fwrite (&s, 2, 1, f);
    }
    std::fclose (f);
    return true;
}

struct PairResult
{
    std::vector<float> aOut, bOut;
    std::vector<uint8_t> aSnap, bSnap;
};

// One full run: fresh processors (both start from the pristine snapshot),
// interleaved settles, distinct patterns, snapshots, then A's hits while B
// idles interleaved, then B's hits while A idles interleaved.
PairResult runPair (const juce::File& kitA, const juce::File& kitB,
                    int bTrack, std::initializer_list<int> bSteps)
{
    Rig a, b;
    a.proc = std::make_unique<ElfDrumLXRProcessor>();
    b.proc = std::make_unique<ElfDrumLXRProcessor>();
    for (auto* r : { &a, &b })
    {
        r->proc->setPlayConfigDetails (0, 2, kRate, kBlock);
        r->proc->prepareToPlay (kRate, kBlock);
    }
    a.proc->loadKitFile (kitA);
    b.proc->loadKitFile (kitB);
    settlePair (a, b);
    setMainSteps (a, 0, { 0, 4, 8, 12 });
    setMainSteps (b, bTrack, bSteps);
    runBlocks (a, 4);
    runBlocks (b, 4);
    PairResult res;
    res.aSnap = snapshot (a);
    res.bSnap = snapshot (b);
    res.aOut = renderHits (a, b);
    res.bOut = renderHits (b, a);
    return res;
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 3) { std::fprintf (stderr, "usage: plugin_instance_test OUTDIR KITDIR\n"); return 2; }
    const juce::String outDir = argv[1];
    const juce::File kitDir (argv[2]);
    const juce::File kitDist = kitDir.getChildFile ("fx-dist.SND");
    const juce::File kitBp = kitDir.getChildFile ("flt-bp.SND");
    const juce::File kitVerb = kitDir.getChildFile ("fx-verb.SND");

    auto& swap = EngineSwap::instance();
    std::printf ("swap available: %s (%zu segments, %zu bytes)\n",
                 swap.available() ? "yes" : "NO",
                 swap.segments().size(), swap.totalBytes());
    CHECK (swap.available(), "swap engages on this platform");
    if (! swap.available()) return 1;

    auto run1 = runPair (kitDist, kitBp, 3, { 2, 6, 10, 14 });
    auto run2 = runPair (kitDist, kitVerb, 2, { 1, 5, 9, 13 });

    writeWav ((outDir + "/inst-run1A.wav").toRawUTF8(), run1.aOut);
    writeWav ((outDir + "/inst-run2A.wav").toRawUTF8(), run2.aOut);
    writeWav ((outDir + "/inst-run1B.wav").toRawUTF8(), run1.bOut);
    writeWav ((outDir + "/inst-run2B.wav").toRawUTF8(), run2.bOut);

    CHECK (! run1.aSnap.empty() && ! run1.bSnap.empty(), "snapshots fulfilled");
    CHECK (run1.aSnap != run1.bSnap, "run1: pattern sets differ between instances");
    CHECK (run1.aSnap == run2.aSnap, "A pattern set identical across partners");
    CHECK (run1.bSnap != run2.bSnap, "partner pattern sets differ across runs (sanity)");
    CHECK (run1.aOut == run2.aOut, "A render bit-identical across partners (all voices)");
    CHECK (run1.bOut != run2.bOut, "partner renders differ across runs (sanity)");

    std::printf ("%s (%d failures)\n", failures == 0 ? "INSTANCE TEST PASS" : "INSTANCE TEST FAIL", failures);
    return failures == 0 ? 0 : 1;
}
