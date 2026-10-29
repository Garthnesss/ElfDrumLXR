#pragma once
#ifndef ELF_NO_EDITOR
 #define ELF_NO_EDITOR 0
#endif
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "EngineHost.h"
#include "ParamMetadata.generated.h"

extern "C" {
#include "engine.h"   // Phase 3 sequencer types (LxrStep) + wrapper API
}

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
    bool isStemsEnabled() const { return host.getStemsEnabled(); }  // Phase 4: any stem bus live in the current layout
    bool loadMorphTargetFile (const juce::File& file);  // set the morph target kit explicitly
    bool saveKitFile (const juce::File& file);          // writes name + current kit bytes (255 B)
    void loadFactoryInit();                             // factory INIT kit as a normal load
    void triggerVoice (int voice);                      // 0..6 (6 = open hihat)
    juce::String getKitName();
    juce::String getMorphTargetName();
    juce::AudioProcessorValueTreeState& getApvts() { return apvts; }

    // --- Phase 2: byte-level kit access for voice slices / randomise / tests ---
    // APVTS is the user-visible truth; these read and write it on the message
    // thread. Writes go through setValueNotifyingHost with the parameter's own
    // range conversion, so the audio-thread bridge replays the edit diff into
    // the engine exactly as for a panel edit.
    std::array<uint8_t, kNumKitParams> currentKitBytes() const;
    void writeParamValues (const int* indices, const uint8_t* values, int count);
    // Guarded randomise (guards documented in PluginProcessor.cpp): voice 0..5
    // for one voice, -1 for the whole kit. Deterministic for a given seed.
    void randomiseParams (int voice, uint32_t seed);
    static juce::String faderParamId (int voice) { return voice < 6 ? "fader" + juce::String (voice) : "faderMaster"; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static juce::String kitParamId (int i) { return "kit" + juce::String (i).paddedLeft ('0', 3); }

    // --- Phase 3: sequencer (message-thread API for the editor) ---
    // The engine is audio-thread only, so edits travel as absolute-value commands
    // through a lock-free SPSC ring (the same pattern as the trigger FIFO) and file
    // operations as heap requests (the same pattern as kit requests). Reads come
    // from a snapshot the audio thread fulfils on request, plus a per-block live
    // state struct. Nothing here touches the engine directly.
    struct SeqCmd
    {
        enum Type : uint8_t { setStep, setMainStep, setLength, setMute, setShuffle, setKitNr,
                              clearPattern, clearTrack, copyPattern, euklid, setRoll, setRollRate,
                              setSongEntry, setSongSlot, requestPattern, commitPattern };
        Type type = setStep;
        uint8_t pattern = 0, track = 0, step = 0;
        uint8_t a = 0, b = 0;        // euklid: a=steps b=rotation; songEntry: a=pos b=value
        int32_t value = 0;           // length / shuffle / dst pattern / roll rate idx / slot
        LxrStep stepData {};
    };
    void sendSeqCmd (const SeqCmd& cmd);

    struct SeqLive
    {
        int activePattern = 0, pendingPattern = 0, running = 0, songMode = 0;
        int songPos = 0, songSlot = 0, bpm = 120, extSync = 0, dawActive = 0;
        int stepIndex[7] {};         // per-track playhead (255 = before the first step)
        uint32_t systick = 0;
    };
    SeqLive getSeqLive();
    void requestSeqSnapshot();
    // Copy the latest fulfilled snapshot (pattern set bytes followed by song data).
    // Returns false if none has been fulfilled yet; version changes per fulfilment.
    bool copySeqSnapshot (std::vector<uint8_t>& blobOut, uint32_t& versionOut);

    void seqLoadPatternFile (int pattern, const juce::File& file);
    void seqSavePatternFile (int pattern, const juce::File& file);
    void seqLoadSongFile (int slot, const juce::File& file);
    void seqSaveSongFile (int slot, const juce::File& file);
    void seqExportSet (const juce::File& folder);   // pattern-NN.pat + song-NN.sng per slot
    void seqImportSet (const juce::File& folder);
    juce::String getPatternName (int pattern);
    void setPatternName (int pattern, const juce::String& name);
    juce::String getSongName (int slot);
    void setSongName (int slot, const juce::String& name);

private:
    struct SeqRequest
    {
        enum class Kind { restoreSet, loadPattern, savePattern, loadSong, saveSong, exportSet, importSet };
        Kind kind = Kind::restoreSet;
        juce::String path;
        int pattern = 0, slot = 0;
        // restoreSet payload:
        std::vector<uint8_t> patternSet, songData;
        std::array<std::array<char, 8>, 64> patNames {}, sngNames {};
        int activePattern = 0, songSlot = 0;
    };
    void postSeqRequest (std::unique_ptr<SeqRequest> req);
    void applySeqRequest (SeqRequest& req);         // audio thread
    void drainSeqCommands();                        // audio thread
    void bridgeSequencer (int numSamples);          // audio thread: transport + APVTS edges
    void fulfilSeqSnapshot();                       // audio thread
    struct KitRequest
    {
        std::array<uint8_t, kNumKitParams> params {};
        std::array<uint8_t, kNumKitParams> target {};
        std::array<char, 8> name {};
        std::array<char, 8> targetName {};
        bool hasTarget = false;
        bool hasMorph = false;
        bool targetOnly = false;   // loadMorphTargetFile: params/name describe the target
        float morph = 0.0f;
    };

    void handleAsyncUpdate() override;
    void applyKitRequest (KitRequest& req);             // audio thread
    void bridgeParameters();                            // audio thread
    void seqSeekToPpq (double ppq);                     // audio thread
    std::array<uint8_t, kNumKitParams> desiredKit();    // audio thread
    void postKitRequest (std::unique_ptr<KitRequest> req);

    juce::AudioProcessorValueTreeState apvts;
    std::array<std::atomic<float>*, kNumKitParams> kitParamPtrs {};
    std::atomic<float>* morphParamPtr = nullptr;
    std::array<std::atomic<float>*, 6> faderPtrs {};
    std::atomic<float>* masterFaderPtr = nullptr;
    std::array<float, 7> lastFaders {};   // audio thread: 0..5 voices, 6 master; -1 = unset

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

    // --- Phase 3 sequencer state ---
    std::atomic<float>* seqPlayPtr = nullptr;
    std::atomic<float>* seqPatternPtr = nullptr;
    std::atomic<float>* seqSongModePtr = nullptr;
    std::atomic<float>* seqTempoPtr = nullptr;
    std::atomic<float>* seqExtSyncPtr = nullptr;

    juce::AbstractFifo seqFifo { 2048 };
    std::array<SeqCmd, 2048> seqRing {};
    std::atomic<SeqRequest*> pendingSeqRequest { nullptr };

    juce::CriticalSection seqSnapLock;
    std::vector<uint8_t> seqSnapBlob;               // pattern set bytes + song data bytes
    uint32_t seqSnapVersionCounter = 0;             // guarded by seqSnapLock
    std::atomic<bool> seqSnapRequested { false };

    juce::CriticalSection seqLiveLock;
    SeqLive seqLiveState;

    juce::CriticalSection patNameLock;
    std::array<std::array<char, 8>, 64> patternNames {};
    std::array<std::array<char, 8>, 64> songNames {};

    // audio-thread transport bookkeeping
    bool seqWasPlaying = false;
    bool seqHaveAnchor = false;
    double seqAnchorPpq = 0.0;
    uint32_t seqAnchorSystick = 0;
    int lastSeqPatternParam = -1;
    bool lastSeqPlayParam = false, lastSeqSongParam = false, lastSeqExtParam = false;
    float lastSeqTempoParam = -1.0f;

    juce::CriticalSection nameLock;
    std::array<char, 8> kitNameBytes {};
    std::array<char, 8> morphTargetNameBytes {};
    void setKitNameBytes (const std::array<char, 8>& n);
    void setMorphTargetNameBytes (const std::array<char, 8>& n);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElfDrumLXRProcessor)
};
