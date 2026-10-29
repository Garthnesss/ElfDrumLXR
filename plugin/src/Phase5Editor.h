#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "Phase5.h"

// Phase 5 editor pages (tabs added next to the Phase 2 panel + Sequencer tab
// in PluginEditor). Plain JUCE widgets in the panel's style. Every action
// goes through the shared Phase 5 layer (Phase5.h) and the processor's
// existing paths — the same code the headless gate harness exercises.

inline const char* p5VoiceName (int v)
{
    static const char* names[7] = { "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "HiHat", "Open Hat" };
    return names[v < 0 ? 0 : (v > 6 ? 6 : v)];
}

//==============================================================================
// Kit Blender + per-voice preset library (Phase 5 item 1): browse .lxv voice
// presets by voice type, audition/apply a slice into the current kit, save
// the current voice, or pull a voice straight out of any .SND kit file.
class BlenderPage : public juce::Component,
                    private juce::ListBoxModel
{
public:
    explicit BlenderPage (ElfDrumLXRProcessor& proc);
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void rescan();
    void applySelected (bool audition);
    void saveCurrentVoice();
    void extractFromKit();

    ElfDrumLXRProcessor& processor;
    juce::ComboBox voiceBox;
    juce::ListBox presetList;
    juce::Label folderLabel, statusLabel;
    juce::TextButton folderButton { "Folder..." }, rescanButton { "Rescan" };
    juce::TextButton applyButton { "Apply" }, auditionButton { "Audition (apply + play)" };
    juce::TextEditor saveName;
    juce::TextButton saveButton { "Save current voice" };
    juce::TextButton fromKitButton { "Voice from kit .SND..." };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File folder;
    std::vector<voicebank::Preset> presets;   // filtered to the selected voice

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BlenderPage)
};

//==============================================================================
// SD-card project librarian (Phase 5 item 2): browse a card-shaped folder
// tree (PROJnn folders of .SND/.PAT/.SNG), import kits/patterns/songs into
// the plugin through the processor's own file paths, and export/import
// shareable packs (zip + manifest). Clicking a kit also auditions it in
// context — the running pattern keeps playing (item 8's audition flow).
class LibraryPage : public juce::Component,
                    private juce::ListBoxModel
{
public:
    explicit LibraryPage (ElfDrumLXRProcessor& proc);
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void rescan();
    void importRow (int row);
    void exportSelectedPack();
    void importPack();

    // kind: -1 = project header, 0 = kit, 1 = pattern, 2 = song
    struct Row { int project = -1; int kind = -1; juce::File file; juce::String text; };
    ElfDrumLXRProcessor& processor;
    juce::Label rootLabel, statusLabel;
    juce::TextButton rootButton { "Card / Library Folder..." }, rescanButton { "Rescan" };
    juce::ListBox treeList;
    juce::ComboBox slotBox;      // destination pattern/song slot for imports
    juce::TextButton importButton { "Import selected" };
    juce::TextButton exportPackButton { "Export project as pack..." };
    juce::TextButton importPackButton { "Import pack..." };
    juce::TextEditor authorEditor;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File root;
    std::vector<cardlib::Project> projects;
    std::vector<Row> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryPage)
};

//==============================================================================
// Creation page (Phase 5 item 3 + item 5): guarded Mutate/Breed over the
// current kit with a lineage undo, and the pattern generator / Remix
// transforms over the firmware pattern model.
class CreatePage : public juce::Component,
                   private juce::Timer
{
public:
    explicit CreatePage (ElfDrumLXRProcessor& proc);
    ~CreatePage() override;
    void resized() override;

private:
    void timerCallback() override;
    void doMutate();
    void doBreed();
    void loadBreedKit();
    uint32_t seedFromEditor (const juce::TextEditor& ed) const;

    // pattern generator / remix: read the decoded pattern from the latest
    // snapshot, transform it purely (SeqOps.h), write it back through the
    // processor's SeqCmd path. The pre-edit pattern is kept for undo.
    int editPattern() const;
    void withPattern (const std::function<void (seqops::PatternData&)>& fn, const juce::String& what);
    void doGenerate();
    void doUndoPattern();

    ElfDrumLXRProcessor& processor;
    juce::Label mutateLabel, breedLabel, statusLabel;
    juce::Slider mutateAmount;
    juce::TextEditor mutateSeed, breedSeed;
    juce::TextButton mutateButton { "Mutate" }, undoButton { "Undo" };
    juce::TextButton loadBreedButton { "Load kit B (.SND)..." };
    juce::Label breedKitLabel;
    juce::ToggleButton breedVoiceToggle { "Per-voice inheritance" };
    juce::TextButton breedButton { "Breed" };
    std::unique_ptr<juce::FileChooser> chooser;
    kitops::Kit breedKit {};
    bool hasBreedKit = false;

    juce::Label patternLabel;
    juce::ComboBox patternBox;
    std::array<juce::Slider, 7> densitySliders;
    juce::ToggleButton euclidToggle { "Euclidean" };
    juce::TextEditor genSeed;
    juce::TextButton generateButton { "Generate" };
    juce::Slider thinKeep;
    juce::TextButton thinButton { "Thin" };
    juce::TextEditor rotateAmount, jitterAmount;
    juce::TextButton rotateButton { "Rotate" }, jitterButton { "Jitter locks" };
    juce::TextButton undoPatternButton { "Undo pattern edit" };
    seqops::PatternData patternUndo {};
    bool hasPatternUndo = false;
    // groove extraction + humanize (item 8)
    juce::TextButton grooveLoadButton { "Load groove .mid..." };
    juce::Label grooveLabel;
    juce::Slider grooveStrength;
    juce::TextButton grooveApplyButton { "Apply groove" };
    seqops::GrooveTemplate groove;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CreatePage)
};

//==============================================================================
// Performance page (Phase 5 items 4 + 6): the Morph XY pad (two firmware
// morph targets composed in the documented order), assignable macros,
// CTRL-ALL with instant restore, and TEMP snapshot jump/return.
class PerformPage : public juce::Component,
                    private juce::Timer
{
public:
    explicit PerformPage (ElfDrumLXRProcessor& proc);
    ~PerformPage() override;
    void resized() override;

private:
    class XYPad;
    void timerCallback() override;

    ElfDrumLXRProcessor& processor;
    std::unique_ptr<XYPad> xyPad;
    juce::ToggleButton xyOnToggle { "Morph XY on (bypasses the single Morph slider)" };
    juce::Slider xyXSlider, xyYSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> xyOnAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> xyXAtt, xyYAtt;
    juce::Label xyALabel, xyBLabel;
    juce::TextButton loadBButton { "Load target B .SND..." };
    std::unique_ptr<juce::FileChooser> chooser;

    // macros (item 6): 4 knobs x 4 assignments, edited one slot at a time.
    juce::Label macroLabel;
    std::array<juce::Slider, 4> macroSliders;
    juce::ComboBox macroSelBox, slotSelBox, macroParamBox;
    juce::Slider macroDepth;
    juce::TextButton macroAssignButton { "Assign slot" };
    void refreshMacroEditor();
    // CTRL-ALL + TEMP
    juce::Label ctrlLabel;
    juce::ComboBox ctrlParamBox;
    juce::Slider ctrlOffset;
    juce::ToggleButton ctrlBeginButton { "CTRL-ALL active (release restores)" };
    juce::TextButton tempStoreButton { "TEMP: store kit" }, tempRecallButton { "TEMP: recall" };
    juce::Label perfStatus;
    // infinity + 16 Pitches + retro-capture (item 8)
    juce::Label labLabel;
    juce::ToggleButton infinityToggle { "Infinity (mutate a little every 4 bars)" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> infinityAtt;
    juce::ToggleButton pitch16Toggle { "16 Pitches" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> pitch16Att;
    juce::ComboBox pitchVoiceBox, pitchScaleBox, pitchRootBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> pitchVoiceAtt, pitchScaleAtt, pitchRootAtt;
    juce::ComboBox retroPatternBox;
    juce::TextButton retroCommitButton { "Commit retro-capture" }, retroClearButton { "Clear" };
    juce::Label retroLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerformPage)
};

//==============================================================================
// Export page (Phase 5 item 7): drag a pattern out as a .mid file (or save
// it), render the current kit's voices as one-shot WAVs, and render a stems
// bundle. The rendering/writing code is exportops (Phase5.h) — the same code
// the gate harness proves.
class ExportPage : public juce::Component,
                   private juce::Timer
{
public:
    explicit ExportPage (ElfDrumLXRProcessor& proc);
    ~ExportPage() override;
    void resized() override;

private:
    void timerCallback() override;
    void savePatternMidi();
    void dragPatternMidi();
    void renderOneShots();
    void renderStems();

    ElfDrumLXRProcessor& processor;
    juce::Label infoLabel, statusLabel;
    juce::ComboBox patternBox;
    juce::TextButton dragMidiButton { "Drag pattern .mid out" };
    juce::TextButton saveMidiButton { "Save pattern .mid..." };
    juce::TextButton oneShotsButton { "Render kit one-shots (7 WAVs)..." };
    juce::TextButton stemsButton { "Render stems bundle (8 WAVs)..." };
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportPage)
};
