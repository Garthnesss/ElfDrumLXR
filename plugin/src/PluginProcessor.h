#pragma once
#ifndef ELF_NO_EDITOR
 #define ELF_NO_EDITOR 0
#endif
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "EngineHost.h"
#include "KitOps.h"
#include "SeqOps.h"
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
    bool loadMorphTarget2File (const juce::File& file); // Phase 5: Morph XY target B
    juce::String getMorphTarget2Name();
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
    // Phase 5 (Kit Blender): apply one voice's slice bytes through the APVTS
    // path — the bridge CC-replays the edit diff into the engine, exactly
    // like a panel edit. Returns false on a voice/slice-size mismatch.
    bool applyVoiceSlice (int voice, const std::vector<uint8_t>& slice);
    std::array<char, 8> currentKitNameBytes() const;
    // Guarded randomise (guards documented in PluginProcessor.cpp): voice 0..5
    // for one voice, -1 for the whole kit. Deterministic for a given seed.
    void randomiseParams (int voice, uint32_t seed);
    // Phase 5 (item 3): guarded mutate / breed over the current kit (KitOps.h
    // semantics: same keep-set as randomise, level floor, seed-reproducible).
    // Both push the pre-change kit onto the lineage stack for undo.
    void mutateKit (float amount, uint32_t seed);
    void breedWithKit (const kitops::Kit& other, bool perVoice, uint32_t seed);
    void undoKitChange();
    int kitHistorySize() const { return (int) kitHistory.size(); }

    // --- Phase 5 (item 6): performance page state (message thread) ---
    // Macros: one knob drives up to 4 kit params. An assignment captures an
    // anchor (the param's value + the macro position at assignment time);
    // moving the macro writes anchor + depth * (pos - anchorPos), clamped to
    // the param's range — the documented delta the gate asserts.
    static constexpr int kNumMacros = 4, kMacroSlots = 4;
    struct MacroAssign { uint8_t param = 0xFF; int16_t depth = 64; };
    void setMacroAssignment (int macro, int slot, int paramIdx, int depth);
    MacroAssign getMacroAssignment (int macro, int slot) const;
    void setMacroValue (int macro, float pos);
    float getMacroPosition (int macro) const { return macroState[macro].position; }
    // CTRL-ALL: snapshot the kit, then apply one offset to a parameter's
    // per-voice equivalents (same metadata name+section across the voices).
    // endCtrlAll restores the snapshot byte-for-byte.
    static std::vector<int> ctrlAllParams (int paramIdx);
    void beginCtrlAll (int paramIdx);
    void setCtrlAllOffset (int offset);
    void endCtrlAll();
    bool ctrlAllActive() const { return ctrlAllOn; }
    // TEMP: jump to a stored kit snapshot and return, exactly.
    void tempStore();
    void tempRecall();
    bool hasTemp() const { return hasTempKit; }

    // --- Phase 5 (item 8): retro-capture ---
    // Note-ons played while the sequencer runs are recorded (pattern, track,
    // step, note, velocity) into a ring; retroCommit writes the events that
    // belong to `pattern` into that pattern through the normal write path.
    int retroEventCount() const { return (int) retroWrite.load(); }
    void clearRetro() { retroWrite.store (0); }
    int retroCommit (int pattern);   // returns how many steps were written
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

    // Phase 5 (item 5): decoded access to one pattern on the firmware data
    // model. Reads decode the latest fulfilled snapshot (request one first;
    // returns false if none exists yet). Writes go out as SeqCmds through
    // the same FIFO the Sequencer tab uses — the firmware pattern set stays
    // the only model; there is no shadow copy.
    bool readPatternData (int pattern, seqops::PatternData& out);
    void writePatternData (int pattern, const seqops::PatternData& data);

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
        // Phase 5: Morph XY target B (state restore / explicit load).
        std::array<uint8_t, kNumKitParams> target2 {};
        std::array<char, 8> target2Name {};
        bool hasTarget2 = false;
        bool target2Only = false;  // loadMorphTarget2File: params/name describe target B
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
    std::atomic<float>* morphXPtr = nullptr;    // Phase 5 Morph XY
    std::atomic<float>* morphYPtr = nullptr;
    std::atomic<float>* morphXYPtr = nullptr;
    std::atomic<float>* seqInfinityPtr = nullptr;   // Phase 5 item 8
    std::atomic<float>* pitch16Ptr = nullptr;
    std::atomic<float>* pitch16VoicePtr = nullptr;
    std::atomic<float>* pitch16ScalePtr = nullptr;
    std::atomic<float>* pitch16RootPtr = nullptr;
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
    std::array<uint8_t, kNumKitParams> morphTarget2Kit {};   // Phase 5: Morph XY target B
    std::array<uint8_t, kNumKitParams> lastBaseKit {};
    std::array<uint8_t, kNumKitParams> lastSentKit {};
    float lastMorph = -1.0f;
    float lastMorphX = -1.0f, lastMorphY = -1.0f;   // Phase 5 bridge change detection
    bool lastMorphXY = false;
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

    // Phase 5 lineage: pre-change kit snapshots for mutate/breed undo
    // (message thread only; capped, oldest dropped).
    std::vector<kitops::Kit> kitHistory;

    // Phase 5 performance state (message thread only).
    struct MacroState
    {
        MacroAssign slots[kMacroSlots];
        float position = 0.5f;
        int anchors[kMacroSlots] {};      // param value at assignment time
        float anchorPos = 0.5f;           // macro position at assignment time
    };
    std::array<MacroState, kNumMacros> macroState {};
    bool ctrlAllOn = false;
    kitops::Kit ctrlAllSnapshot {};
    std::vector<int> ctrlAllTargets;
    kitops::Kit tempKit {};
    bool hasTempKit = false;

    // Phase 5 item 8: infinity bookkeeping (audio thread counts, message
    // thread mutates) + the retro-capture ring (audio thread writes,
    // message thread reads; single-producer, index-ordered).
    int prevStep0 = -1;
    int infinityBars = 0;
    std::atomic<int> infinityPending { 0 };
    int infinityMutations = 0;   // message thread
    struct RetroEvent { uint8_t pattern = 0, track = 0, step = 0, note = 60, vel = 127; };
    std::array<RetroEvent, 512> retroRing {};
    std::atomic<uint32_t> retroWrite { 0 };

    juce::CriticalSection nameLock;
    std::array<char, 8> kitNameBytes {};
    std::array<char, 8> morphTargetNameBytes {};
    std::array<char, 8> morphTarget2NameBytes {};
    void setKitNameBytes (const std::array<char, 8>& n);
    void setMorphTargetNameBytes (const std::array<char, 8>& n);
    void setMorphTarget2NameBytes (const std::array<char, 8>& n);
    // The one desired-kit computation (audio thread): Morph XY composition
    // when morphXY is on, else the classic single-target morph at the given
    // legacy amount. Reads the XY atomics live.
    std::array<uint8_t, kNumKitParams> computeDesired (const std::array<uint8_t, kNumKitParams>& base,
                                                       float legacyMorph) const;
    void syncMorphLasts (float legacyMorph);   // audio thread: refresh lastMorph*

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElfDrumLXRProcessor)
};
