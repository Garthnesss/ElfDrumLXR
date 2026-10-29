#pragma once
#ifndef ELF_NO_EDITOR
 #define ELF_NO_EDITOR 0
#endif
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "EngineHost.h"

// ElfDrumLXR — Phase 1 plugin shell around the Phase 0 engine (see ARCHITECTURE.md).
// Threading model: the engine lives entirely on the audio thread (EngineHost). The message
// thread talks to it through (a) APVTS atomic parameter values, read every block by the
// parameter bridge, and (b) a single atomic kit-request pointer for whole-kit loads.
class ElfDrumLXRProcessor : public juce::AudioProcessor,
                            private juce::AsyncUpdater
{
public:
    static constexpr int kNumKitParams = 247;

    ElfDrumLXRProcessor();
    ~ElfDrumLXRProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return ! ELF_NO_EDITOR; }

    const juce::String getName() const override { return "ElfDrumLXR"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Initkit"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- message-thread API (editor, file layer) ---
    bool loadKitFile (const juce::File& file);          // .SND (255 B, or 309 B module kit)
    bool saveKitFile (const juce::File& file);          // writes name + current kit bytes (255 B)
    void triggerVoice (int voice);                      // 0..6 (6 = open hihat)
    juce::String getKitName();
    juce::AudioProcessorValueTreeState& getApvts() { return apvts; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static juce::String kitParamId (int i) { return "kit" + juce::String (i).paddedLeft ('0', 3); }

private:
    struct KitRequest
    {
        std::array<uint8_t, kNumKitParams> params {};
        std::array<uint8_t, kNumKitParams> target {};
        std::array<char, 8> name {};
        bool hasTarget = false;
        bool hasMorph = false;
        float morph = 0.0f;
    };

    void handleAsyncUpdate() override;
    void applyKitRequest (KitRequest& req);             // audio thread
    void bridgeParameters();                            // audio thread
    std::array<uint8_t, kNumKitParams> desiredKit();    // audio thread
    void postKitRequest (std::unique_ptr<KitRequest> req);

    juce::AudioProcessorValueTreeState apvts;
    std::array<std::atomic<float>*, kNumKitParams> kitParamPtrs {};
    std::atomic<float>* morphParamPtr = nullptr;

    EngineHost host;
    std::vector<float> scratch;   // sink for engine jacks the host didn't give us channels for

    // Editor trigger buttons -> audio thread, via a lock-free SPSC ring (message thread is
    // the only producer, processBlock the only consumer; EngineHost itself stays audio-only).
    struct Trigger { int voice, note, velocity; };
    juce::AbstractFifo triggerFifo { 1024 };
    std::array<Trigger, 1024> triggerRing {};

    // audio-thread-owned kit state
    std::array<uint8_t, kNumKitParams> baseKit {};
    std::array<uint8_t, kNumKitParams> morphTargetKit {};
    std::array<uint8_t, kNumKitParams> lastBaseKit {};
    std::array<uint8_t, kNumKitParams> lastSentKit {};
    float lastMorph = -1.0f;
    bool engineKitLoaded = false;

    std::atomic<KitRequest*> pendingRequest { nullptr };
    std::atomic<bool> apvtsSyncPending { false };
    std::atomic<bool> bridgeSuspended { false };

    juce::CriticalSection nameLock;
    std::array<char, 8> kitNameBytes {};
    void setKitNameBytes (const std::array<char, 8>& n);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElfDrumLXRProcessor)
};
