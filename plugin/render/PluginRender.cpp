/* plugin-render: headless gate harness for the Phase 1 plugin. Instantiates the real
   ElfDrumLXR processor, loads a .SND kit, schedules the voice hits 0.6 s apart (the same
   layout as tools/lxr-render), renders offline at a chosen host rate, and writes a stereo
   WAV summed from both output buses — the same jack sum lxr-render writes, so the two
   WAVs compare segment by segment. The processor's reported latency is trimmed off the
   front so hit k lands in segment k.
   Usage: plugin-render KIT.SND out.wav [hostRate] [openhat] */
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "../src/PluginProcessor.h"

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc < 3)
    {
        std::fprintf (stderr, "usage: plugin-render KIT.SND out.wav [hostRate] [openhat]\n");
        return 2;
    }
    const juce::File kitFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const double rate = argc > 3 ? std::atof (argv[3]) : 44100.0;
    const int voices = (argc > 4 && juce::String (argv[4]) == "openhat") ? 7 : 6;
    const int blockSize = 512;

    ElfDrumLXRProcessor processor;
    processor.setPlayConfigDetails (0, 4, rate, blockSize);
    processor.prepareToPlay (rate, blockSize);
    if (! processor.loadKitFile (kitFile))
    {
        std::fprintf (stderr, "can't load %s\n", argv[1]);
        return 1;
    }

    const juce::int64 totalFrames = (juce::int64) (voices * 0.6 * rate) + blockSize;
    juce::AudioBuffer<float> rendered (4, (int) totalFrames);
    rendered.clear();
    juce::int64 written = 0;
    int nextVoice = 0;
    // Hits are scheduled as MIDI note-ons at exact sample offsets — the real DAW path
    // (stock firmware notes -> GM map in the processor). GM notes per voice:
    static const int gmNotes[7] = { 36, 45, 50, 38, 39, 42, 46 };
    while (written < totalFrames)
    {
        const int n = (int) std::min<juce::int64> (blockSize, totalFrames - written);
        juce::MidiBuffer midi;
        while (nextVoice < voices && (juce::int64) (nextVoice * 0.6 * rate) < written + n)
        {
            const int offset = (int) ((juce::int64) (nextVoice * 0.6 * rate) - written);
            midi.addEvent (juce::MidiMessage::noteOn (1, gmNotes[nextVoice], (juce::uint8) 127), offset);
            ++nextVoice;
        }
        juce::AudioBuffer<float> block (rendered.getArrayOfWritePointers(), 4, (int) written, n);
        processor.processBlock (block, midi);
        written += n;
    }
    processor.releaseResources();

    const int latency = processor.getLatencySamples();
    std::printf ("host rate %.0f, resampler latency %d samples\n", rate, latency);

    // stereo WAV = bus1 + bus2 jack sum, latency-trimmed
    const int frames = (int) (voices * 0.6 * rate);
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

    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (new juce::FileOutputStream (outFile), rate, 2, 16, {}, 0));
    if (writer == nullptr)
    {
        std::fprintf (stderr, "can't write %s\n", argv[2]);
        return 1;
    }
    writer->writeFromAudioSampleBuffer (stereo, 0, frames);

    static const char* names[7] = { "drum1", "drum2", "drum3", "snare", "cymbal", "hihat", "openhat" };
    const int seg = (int) (0.6 * rate);
    for (int v = 0; v < voices; ++v)
    {
        float peak = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = v * seg; i < (v + 1) * seg; ++i)
                peak = std::max (peak, std::abs (stereo.getSample (c, i)));
        std::printf ("  %-7s peak %5d\n", names[v], (int) (peak * 32767.0f));
    }
    std::printf ("%s -> %s (%.1f s at %.0f Hz)\n", argv[1], argv[2], frames / rate, rate);
    return 0;
}
