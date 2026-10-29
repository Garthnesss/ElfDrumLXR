#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// Phase 1 minimal editor: product + kit name, .SND load/save, one trigger button per voice,
// the morph slider. The real panel (generated from the firmware's menu tables) is Phase 2.
class ElfDrumLXRProcessorEditor : public juce::AudioProcessorEditor,
                                  public juce::FileDragAndDropTarget,
                                  private juce::Timer
{
public:
    explicit ElfDrumLXRProcessorEditor (ElfDrumLXRProcessor&);
    ~ElfDrumLXRProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void chooseKit (bool save);

    ElfDrumLXRProcessor& processor;

    juce::Label titleLabel, kitNameLabel;
    juce::TextButton loadButton { "Load .SND" }, saveButton { "Save .SND" };
    std::array<juce::TextButton, 7> triggerButtons;
    juce::Slider morphSlider;
    juce::Label morphLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> morphAttachment;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElfDrumLXRProcessorEditor)
};
