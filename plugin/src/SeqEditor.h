#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// Phase 3 pattern editor (the "Sequencer" tab). Operates ONLY on the firmware's
// data model through the processor's sequencer API: reads come from the audio
// thread's pattern-set snapshots (decoded with the wrapper's snapshot accessors),
// writes go out as absolute-value SeqCmds. No parallel pattern model is kept —
// the local view is a decoded copy of the latest snapshot, refreshed on a timer
// and after every edit. Plain JUCE widgets, like the Phase 2 panel.
class SeqEditor : public juce::Component,
                  private juce::Timer
{
public:
    explicit SeqEditor (ElfDrumLXRProcessor& proc);
    ~SeqEditor() override;

    void resized() override;

private:
    class Grid;
    friend class Grid;

    void timerCallback() override;
    void rebuildFromSnapshot();
    void refreshDetail();
    void refreshSongEntry();
    void selectStep (int track, int step);
    void onCellClicked (int track, int step, bool toggle);
    void sendStepEdit();
    ElfDrumLXRProcessor::SeqCmd makeCmd (ElfDrumLXRProcessor::SeqCmd::Type type) const;

    ElfDrumLXRProcessor& processor;
    std::unique_ptr<Grid> grid;

    // transport row (APVTS attachments)
    juce::ToggleButton playButton { "Play" }, songButton { "Song" }, extSyncButton { "Ext Sync" };
    juce::Slider tempoSlider, patternPlaySlider;
    juce::Label tempoLabel, patternPlayLabel, liveLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playAtt, songAtt, extAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tempoAtt, patternPlayAtt;

    // pattern ops row
    juce::ComboBox editPatternBox, songSlotBox;
    juce::ToggleButton followButton { "Follow" };
    juce::TextButton copyButton { "Copy" }, pasteButton { "Paste" }, clearButton { "Clear" };
    juce::TextButton loadPatButton { "Load .PAT" }, savePatButton { "Save .PAT" };
    juce::TextButton exportButton { "Export Set..." }, importButton { "Import Set..." };
    juce::TextEditor nameEditor;
    juce::Label kitNrLabel;
    int copiedPattern = -1;

    // bar selector + per-track headers are drawn by Grid; track controls:
    std::array<juce::TextButton, 4> barButtons;
    struct TrackCtl
    {
        juce::Label name;
        juce::ToggleButton mute { "M" }, roll { "R" };
        juce::Slider length;
    };
    std::array<TrackCtl, 7> trackCtls;
    juce::ComboBox rollRateBox;
    juce::Label rollRateLabel;

    // step detail
    juce::Label detailLabel;
    juce::Slider velSlider, noteSlider, probSlider, flamSlider, shiftSlider;
    juce::Label noteNameLabel;
    juce::ComboBox p1Box, p2Box;
    juce::Slider p1ValSlider, p2ValSlider;
    juce::Label p1Label, p2Label;

    // shuffle + euklid + song rows
    juce::Slider shuffleSlider;
    juce::Label shuffleLabel;
    juce::Slider eukLenSlider, eukStepsSlider, eukRotSlider;
    juce::TextButton eukApplyButton { "Euklid Apply" };
    juce::Label eukLabel;
    juce::Slider songPosSlider;
    juce::ComboBox songEntryBox;
    juce::Label songLabel;
    std::unique_ptr<juce::FileChooser> chooser;

    // local decoded view of the latest snapshot (pattern set bytes)
    std::vector<uint8_t> snapBlob;
    uint32_t snapVersion = 0;
    bool haveSnap = false;
    LxrStep viewSteps[7][64] {};
    bool viewMain[7][64] {};
    int viewLength[7] {};
    int viewShuffle = 0, viewKitNr = 0;
    bool viewMute[7] {}, viewRoll[7] {};   // live engine state, editor-owned view

    int shownPattern = 0;
    int bar = 0;
    int selTrack = 0, selStep = 0;
    bool follow = true;
    bool updating = false;                 // feedback guard while refreshing widgets

    // playhead view (from the processor's per-block live state)
    int liveStep[7] {};
    bool liveRunning = false;
    int tickCount = 0;

    // song editor state
    int songSlot = 0;
    uint8_t viewSong[64] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqEditor)
};
