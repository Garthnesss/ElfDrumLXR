/* plugin-morph-test: headless proof harness for the morph path + the .SND save round-trip.
   Modes:
     startup <outPrefix>
        Fresh processor, no kit ever loaded (Garth's reported case: INIT is both the
        current kit and — in the Phase 1 semantics — the morph target). Renders the
        6-hit sequence at morph 0 and morph 1; identical output proves the dead slider.
     twokit <targetKit.SND> <currentKit.SND> <outPrefix>
        Loads targetKit first, then currentKit (a file load makes the previously loaded
        kit the morph target), then renders at morph 0 / 0.25 / 0.5 / 0.75 / 1.
     target <targetKit.SND> <outPrefix>
        Garth's post-fix flow: fresh processor (current kit = INIT), set the morph
        target explicitly via loadMorphTargetFile, then sweep 0 / 0.5 / 1.
     save <kit.SND> <out.SND>
        Loads kit, pumps the message loop (APVTS sync), saves via saveKitFile, then
        byte-compares the saved file against the source kit (name bytes reported,
        parameter bytes 8..255 must match exactly) and reloads it.
   Renders at the engine's native 44,140 Hz, 6 hits 0.6 s apart via MIDI GM notes —
   the same layout as plugin-render / lxr-render, so WAVs compare segment by segment. */
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "../src/PluginProcessor.h"

namespace {
constexpr double kRate = 44140.0;
constexpr int kBlock = 512;
constexpr int kVoices = 6;
const int kGmNotes[7] = { 36, 45, 50, 38, 39, 42, 46 };
const char* kNames[7] = { "drum1", "drum2", "drum3", "snare", "cymbal", "hihat", "openhat" };

void pump (int ms = 60)
{
    // JUCE_MODAL_LOOPS_PERMITTED=1 is set for this harness target in CMakeLists.txt.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

void renderBlocks (ElfDrumLXRProcessor& proc, int numBlocks)
{
    juce::AudioBuffer<float> block (4, kBlock);
    juce::MidiBuffer midi;
    for (int i = 0; i < numBlocks; ++i)
    {
        block.clear();
        proc.processBlock (block, midi);
    }
}

/* Let a posted kit request land (processBlock applies it; the async APVTS sync needs
   the message loop pumped, as a DAW's message thread would). */
void settle (ElfDrumLXRProcessor& proc)
{
    renderBlocks (proc, 8);
    pump();
    renderBlocks (proc, 8);
    pump();
}

void setMorph (ElfDrumLXRProcessor& proc, float m)
{
    if (auto* p = proc.getApvts().getParameter ("morph"))
        p->setValueNotifyingHost (m);
}

/* Render the 6-hit sequence (processor must already be prepared + settled). */
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

void writeWav (const juce::File& file, const juce::AudioBuffer<float>& stereo)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (new juce::FileOutputStream (file), kRate, 2, 16, {}, 0));
    if (writer != nullptr)
        writer->writeFromAudioSampleBuffer (stereo, 0, stereo.getNumSamples());
}

void printPeaks (const juce::String& label, const juce::AudioBuffer<float>& stereo)
{
    const int seg = (int) (0.6 * kRate);
    juce::String line = label + ":";
    for (int v = 0; v < kVoices; ++v)
    {
        float peak = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = v * seg; i < (v + 1) * seg; ++i)
                peak = std::max (peak, std::abs (stereo.getSample (c, i)));
        line += " " + juce::String (kNames[v]) + "=" + juce::String ((int) (peak * 32767.0f));
    }
    std::printf ("%s\n", line.toRawUTF8());
}

int morphRun (ElfDrumLXRProcessor& proc, const juce::String& prefix,
              const std::vector<float>& steps)
{
    for (float m : steps)
    {
        setMorph (proc, m);
        renderBlocks (proc, 24);   // let the bridge push the (possibly) morphed kit edits
        pump (10);
        auto stereo = renderHits (proc);
        const int pct = (int) juce::roundToInt (m * 100.0f);
        writeWav (juce::File (prefix + "-" + juce::String (pct) + ".wav"), stereo);
        printPeaks ("morph " + juce::String (m, 2), stereo);
    }
    return 0;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: plugin-morph-test startup <prefix> | twokit <target.SND> <current.SND> <prefix> | target <target.SND> <prefix> | save <kit.SND> <out.SND>\n");
        return 2;
    }
    const juce::String mode (argv[1]);

    ElfDrumLXRProcessor processor;
    processor.setPlayConfigDetails (0, 4, kRate, kBlock);
    processor.prepareToPlay (kRate, kBlock);

    if (mode == "startup")
    {
        settle (processor);
        return morphRun (processor, argv[2], { 0.0f, 1.0f });
    }

    if (mode == "target" && argc >= 4)
    {
        settle (processor);
        if (! processor.loadMorphTargetFile (juce::File (argv[2]))) { std::fprintf (stderr, "can't load target %s\n", argv[2]); return 1; }
        settle (processor);
        std::printf ("current kit: %s, morph target: %s\n",
                     processor.getKitName().toRawUTF8(), processor.getMorphTargetName().toRawUTF8());
        return morphRun (processor, argv[3], { 0.0f, 0.5f, 1.0f });
    }

    if (mode == "twokit" && argc >= 5)
    {
        if (! processor.loadKitFile (juce::File (argv[2]))) { std::fprintf (stderr, "can't load target %s\n", argv[2]); return 1; }
        settle (processor);
        if (! processor.loadKitFile (juce::File (argv[3]))) { std::fprintf (stderr, "can't load current %s\n", argv[3]); return 1; }
        settle (processor);
        std::printf ("current kit: %s, morph steps follow\n", processor.getKitName().toRawUTF8());
        return morphRun (processor, argv[4], { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f });
    }

    if (mode == "save" && argc >= 4)
    {
        const juce::File src (argv[2]);
        const juce::File out (argv[3]);
        if (! processor.loadKitFile (src)) { std::fprintf (stderr, "can't load %s\n", argv[2]); return 1; }
        settle (processor);
        out.deleteFile();
        if (! processor.saveKitFile (out)) { std::fprintf (stderr, "save failed\n"); return 1; }

        juce::MemoryBlock a, b;
        src.loadFileAsData (a);
        out.loadFileAsData (b);
        std::printf ("sizes: source %d bytes, saved %d bytes\n", (int) a.getSize(), (int) b.getSize());
        if (a.getSize() < 255 || b.getSize() != 255) { std::printf ("SAVE TEST FAIL: bad size\n"); return 1; }
        const auto* pa = (const uint8_t*) a.getData();
        const auto* pb = (const uint8_t*) b.getData();
        std::printf ("name bytes: source '%s' saved '%s'\n",
                     juce::String ((const char*) pa, 8).toRawUTF8(),
                     juce::String ((const char*) pb, 8).toRawUTF8());
        int diffs = 0, first = -1;
        for (int i = 8; i < 255; ++i)
            if (pa[i] != pb[i]) { ++diffs; if (first < 0) first = i; }
        if (diffs == 0)
            std::printf ("SAVE TEST PASS: parameter bytes 8..255 identical\n");
        else
            std::printf ("SAVE TEST FAIL: %d parameter bytes differ (first at %d: %d -> %d)\n",
                         diffs, first, pa[first], pb[first]);

        // Reload the saved file and confirm the engine accepts it and sounds.
        if (! processor.loadKitFile (out)) { std::printf ("RELOAD FAIL\n"); return 1; }
        settle (processor);
        auto stereo = renderHits (processor);
        printPeaks ("reloaded saved kit", stereo);
        return diffs == 0 ? 0 : 1;
    }

    std::fprintf (stderr, "unknown mode\n");
    return 2;
}
