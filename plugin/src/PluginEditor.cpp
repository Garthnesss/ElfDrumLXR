#include "PluginEditor.h"

namespace {
const char* kVoiceNames[7] = { "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "HiHat", "Open Hat" };
} // namespace

ElfDrumLXRProcessorEditor::ElfDrumLXRProcessorEditor (ElfDrumLXRProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    titleLabel.setText ("ElfDrumLXR", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    kitNameLabel.setText (processor.getKitName(), juce::dontSendNotification);
    kitNameLabel.setFont (juce::FontOptions (15.0f));
    addAndMakeVisible (kitNameLabel);

    loadButton.onClick = [this] { chooseKit (false); };
    saveButton.onClick = [this] { chooseKit (true); };
    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);

    for (int v = 0; v < 7; ++v)
    {
        triggerButtons[v].setButtonText (kVoiceNames[v]);
        triggerButtons[v].onClick = [this, v] { processor.triggerVoice (v); };
        addAndMakeVisible (triggerButtons[v]);
    }

    morphLabel.setText ("Morph", juce::dontSendNotification);
    addAndMakeVisible (morphLabel);
    morphSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    morphSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    addAndMakeVisible (morphSlider);
    morphAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getApvts(), "morph", morphSlider);

    setSize (560, 300);
    startTimerHz (4);
}

ElfDrumLXRProcessorEditor::~ElfDrumLXRProcessorEditor() { stopTimer(); }

void ElfDrumLXRProcessorEditor::timerCallback()
{
    const auto name = processor.getKitName();
    if (kitNameLabel.getText() != name)
        kitNameLabel.setText (name, juce::dontSendNotification);
}

void ElfDrumLXRProcessorEditor::chooseKit (bool save)
{
    chooser = std::make_unique<juce::FileChooser> (
        save ? "Save kit (.SND)" : "Load kit (.SND)",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.SND;*.snd");
    const int flags = save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                           : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (flags, [this, save] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File()) return;
        if (save) processor.saveKitFile (file);
        else      processor.loadKitFile (file);
    });
}

bool ElfDrumLXRProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".snd"))
            return true;
    return false;
}

void ElfDrumLXRProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".snd"))
        {
            processor.loadKitFile (juce::File (f));
            break;
        }
}

void ElfDrumLXRProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1c1f24));
}

void ElfDrumLXRProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto header = area.removeFromTop (34);
    titleLabel.setBounds (header.removeFromLeft (200));
    kitNameLabel.setBounds (header);
    area.removeFromTop (8);
    auto fileRow = area.removeFromTop (30);
    loadButton.setBounds (fileRow.removeFromLeft (130));
    fileRow.removeFromLeft (8);
    saveButton.setBounds (fileRow.removeFromLeft (130));
    area.removeFromTop (12);
    auto trigRow = area.removeFromTop (44);
    const int w = (trigRow.getWidth() - 6 * 6) / 7;
    for (int v = 0; v < 7; ++v)
    {
        triggerButtons[v].setBounds (trigRow.removeFromLeft (w));
        trigRow.removeFromLeft (6);
    }
    area.removeFromTop (16);
    auto morphRow = area.removeFromTop (28);
    morphLabel.setBounds (morphRow.removeFromLeft (60));
    morphSlider.setBounds (morphRow);
}
