/* plugin-stem-test: Phase 4 stem gate harness. Instantiates the real processor with
   ALL output buses enabled (2 mains + 6 stems = 16 channels), loads a kit, renders
   offline at the engine's native rate, and writes a 16-channel WAV:
     ch 0-1   Main (DAC1)      ch 2-3   Out 2 (DAC2)
     ch 4-5   Stem Drum 1      ch 6-7   Stem Drum 2     ch 8-9   Stem Drum 3
     ch 10-11 Stem Snare       ch 12-13 Stem Cymbal     ch 14-15 Stem HiHat
   Modes:
     plugin-stem-test all KIT.SND out.wav [rate]        6 hits, 0.6 s apart (plugin-render layout)
     plugin-stem-test one KIT.SND voice out.wav [rate]  one hit of voice 0..6 at t=0, 1.5 s
   Analysis lives in tools/stem-gate.sh / its python. */
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "../src/PluginProcessor.h"

namespace {
constexpr int kBlock = 512;
constexpr int kChannels = 16;
const int kGmNotes[7] = { 36, 45, 50, 38, 39, 42, 46 };

void pump (int ms = 60)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

void renderBlocks (ElfDrumLXRProcessor& proc, juce::AudioBuffer<float>& render, int numBlocks)
{
    juce::MidiBuffer midi;
    int written = 0;
    juce::AudioBuffer<float> full (kChannels, numBlocks * kBlock);
    full.clear();
    for (int i = 0; i < numBlocks; ++i)
    {
        juce::AudioBuffer<float> b (full.getArrayOfWritePointers(), kChannels, written, kBlock);
        proc.processBlock (b, midi);
        written += kBlock;
    }
    render.makeCopyOf (full);
}

void settle (ElfDrumLXRProcessor& proc)
{
    juce::AudioBuffer<float> sink;
    renderBlocks (proc, sink, 8);
    pump();
    renderBlocks (proc, sink, 8);
    pump();
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buf, double rate)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (new juce::FileOutputStream (file), rate, (unsigned) kChannels, 16, {}, 0));
    if (writer == nullptr) return false;
    return writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

void printPeaks (const juce::AudioBuffer<float>& buf)
{
    static const char* names[kChannels] = { "mainL", "mainR", "dac2L", "dac2R",
        "stemD1L", "stemD1R", "stemD2L", "stemD2R", "stemD3L", "stemD3R",
        "stemSnL", "stemSnR", "stemCyL", "stemCyR", "stemHhL", "stemHhR" };
    for (int c = 0; c < kChannels; ++c)
    {
        float peak = 0.0f;
        for (int i = 0; i < buf.getNumSamples(); ++i)
            peak = std::max (peak, std::abs (buf.getSample (c, i)));
        std::printf ("  ch%-2d %-8s peak %6d\n", c, names[c], (int) (peak * 32767.0f));
    }
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 4)
    {
        std::fprintf (stderr, "usage: plugin-stem-test all KIT out.wav [rate] | one KIT voice out.wav [rate]\n");
        return 2;
    }
    const juce::String mode (argv[1]);
    const juce::File kitFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const bool oneMode = mode == "one";
    const int voice = oneMode ? juce::String (argv[3]).getIntValue() : -1;
    const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (oneMode ? argv[4] : argv[3]));
    const double rate = std::atof (argv[oneMode ? 5 : 4] ? argv[oneMode ? 5 : 4] : "44140");
    if (! oneMode && mode != "all") { std::fprintf (stderr, "unknown mode\n"); return 2; }
    if (oneMode && (voice < 0 || voice > 6)) { std::fprintf (stderr, "voice 0..6\n"); return 2; }

    ElfDrumLXRProcessor processor;
    processor.setPlayConfigDetails (0, kChannels, rate, kBlock);
    {
        // setPlayConfigDetails alone leaves the auxiliary (stem) buses disabled;
        // enable all eight output buses explicitly, as a multi-out host would.
        juce::AudioProcessor::BusesLayout layout;
        for (int b = 0; b < 8; ++b) layout.outputBuses.add (juce::AudioChannelSet::stereo());
        if (! processor.setBusesLayout (layout))
        {
            std::fprintf (stderr, "processor rejected the 8-bus layout\n");
            return 1;
        }
    }
    processor.prepareToPlay (rate, kBlock);
    if (! processor.loadKitFile (kitFile))
    {
        std::fprintf (stderr, "can't load %s\n", argv[2]);
        return 1;
    }
    settle (processor);
    if (! processor.isStemsEnabled())
    {
        std::fprintf (stderr, "stems not enabled by layout\n");
        return 1;
    }

    const int totalFrames = oneMode ? (int) (1.5 * rate) : (int) (6 * 0.6 * rate) + kBlock;
    juce::AudioBuffer<float> rendered (kChannels, totalFrames);
    rendered.clear();
    int written = 0, nextVoice = 0;
    while (written < totalFrames)
    {
        const int n = std::min (kBlock, totalFrames - written);
        juce::MidiBuffer midi;
        if (oneMode)
        {
            if (written == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, kGmNotes[voice], (juce::uint8) 127), 0);
        }
        else
        {
            while (nextVoice < 6 && (int) (nextVoice * 0.6 * rate) < written + n)
            {
                const int offset = (int) (nextVoice * 0.6 * rate) - written;
                midi.addEvent (juce::MidiMessage::noteOn (1, kGmNotes[nextVoice], (juce::uint8) 127), offset);
                ++nextVoice;
            }
        }
        juce::AudioBuffer<float> block (rendered.getArrayOfWritePointers(), kChannels, written, n);
        processor.processBlock (block, midi);
        written += n;
    }
    processor.releaseResources();

    if (! writeWav (outFile, rendered, rate))
    {
        std::fprintf (stderr, "can't write %s\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }
    printPeaks (rendered);
    std::printf ("%s -> %s (%.1f s at %.0f Hz, %d ch)\n", argv[2],
                 outFile.getFullPathName().toRawUTF8(), totalFrames / rate, rate, kChannels);
    return 0;
}
