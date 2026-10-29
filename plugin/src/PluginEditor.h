#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "VoiceSlice.h"

// Phase 2 panel editor. Layout follows the hardware's own menu structure via
// the generated metadata (ParamMetadata.generated.h): one tab per voice with
// its sections in firmware order, an FX tab, and a Global/Mix tab carrying
// the performance faders plus the kit-global parameters. Header: kit
// load/save/INIT, morph + explicit target, randomise, trigger buttons.
// Left strip: the .SND kit browser. All parameter widgets bind through
// APVTS attachments, so panel, host automation and state stay in sync.
class ElfDrumLXRProcessorEditor : public juce::AudioProcessorEditor,
                                  public juce::FileDragAndDropTarget,
                                  private juce::Timer,
                                  private juce::ListBoxModel
{
public:
    explicit ElfDrumLXRProcessorEditor (ElfDrumLXRProcessor&);
    ~ElfDrumLXRProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    enum class KitChoice { load, save, loadTarget };
    void timerCallback() override;
    void chooseKit (KitChoice choice);
    void setStatus (const juce::String& text);

    // kit browser (ListBoxModel)
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void rescanKits();
    void chooseKitFolder();
    void stepKit (int delta);
    void loadBrowserKit (int row);

    // per-voice actions (wired from the voice pages)
    void copyVoice (int voice);
    void pasteVoice (int voice);
    void saveVoice (int voice);
    void loadVoice (int voice);
    void applySliceToVoice (int voice, const std::vector<uint8_t>& slice);

    ElfDrumLXRProcessor& processor;

    juce::LookAndFeel_V4 lookAndFeel;   // dark colour scheme, set in the ctor
    std::array<juce::TextButton*, 6> pasteButtons {};   // per-voice Paste buttons (in the page toolbars)

    // header
    juce::Label titleLabel, kitNameLabel, statusLabel;
    juce::TextButton loadButton { "Load .SND" }, saveButton { "Save .SND" };
    juce::TextButton initButton { "INIT" }, randomKitButton { "Randomise Kit" };
    std::array<juce::TextButton, 7> triggerButtons;
    juce::Label morphLabel, morphTargetCaption, morphTargetLabel;
    juce::Slider morphSlider;
    juce::TextButton loadTargetButton { "Load target .SND" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> morphAttachment;
    juce::Rectangle<int> morphDropZone;
    std::unique_ptr<juce::FileChooser> chooser;

    // kit browser
    juce::TextButton folderButton { "Kit Folder..." }, refreshButton { "Refresh" };
    juce::Label folderLabel;
    juce::ListBox kitList;
    juce::TextButton prevKitButton { "< Prev" }, nextKitButton { "Next >" };
    struct BrowserEntry { juce::File file; juce::String kitName; };
    std::vector<BrowserEntry> kitEntries;
    juce::File kitFolder;
    int currentKitRow = -1;
    std::unique_ptr<juce::PropertiesFile> props;

    // pages
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    std::vector<std::unique_ptr<juce::Component>> tabPages;

    // voice clipboard (in-memory .lxv payload)
    voiceslice::LxvData voiceClipboard;
    bool hasVoiceClipboard = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElfDrumLXRProcessorEditor)
};
