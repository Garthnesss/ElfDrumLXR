#include "PluginEditor.h"
#include "ParamMetadata.generated.h"
#include "SeqEditor.h"
#include "Phase5Editor.h"
#include "InitKit.h"
#include <algorithm>
#include <set>

namespace {
const char* kVoiceNames[7] = { "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "HiHat", "Open Hat" };
const char* kPageNames[8] = { "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "HiHat", "FX", "Global/Mix" };

juce::String kitNameFromHeader (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk() || in.getTotalLength() < 255)
        return {};
    char name[8] = {};
    if (in.read (name, 8) != 8)
        return {};
    return juce::String (name, 8).trimEnd();
}

//==============================================================================
// ComboBox <-> ranged parameter (JUCE has no attachment that maps a parameter
// with min != 0 onto combo indices; this one does, via ParameterAttachment's
// denormalised-value contract).
class ComboAttachment
{
public:
    ComboAttachment (juce::RangedAudioParameter& param, juce::ComboBox& box, int minValue)
        : attachment (param, [&box, minValue] (float v)
                      { box.setSelectedItemIndex (juce::roundToInt (v) - minValue,
                                                  juce::dontSendNotification); })
    {
        box.onChange = [this, &box, minValue]
        {
            const int idx = box.getSelectedItemIndex();
            if (idx >= 0)
                attachment.setValueAsCompleteGesture ((float) (minValue + idx));
        };
        attachment.sendInitialUpdate();
    }

private:
    juce::ParameterAttachment attachment;
};

class ToggleAttachment
{
public:
    ToggleAttachment (juce::RangedAudioParameter& param, juce::ToggleButton& button)
        : attachment (param, [&button] (float v)
                      { button.setToggleState (v >= 0.5f, juce::dontSendNotification); })
    {
        button.onClick = [this, &button]
        { attachment.setValueAsCompleteGesture (button.getToggleState() ? 1.0f : 0.0f); };
        attachment.sendInitialUpdate();
    }

private:
    juce::ParameterAttachment attachment;
};

//==============================================================================
// One kit parameter's control: name label + rotary / combo / toggle bound to
// the APVTS parameter through an attachment.
class ParamControl : public juce::Component
{
public:
    ParamControl (ElfDrumLXRProcessor& proc, const elfmeta::KitParam& meta)
    {
        nameLabel.setText (meta.name, juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (11.0f));
        nameLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (nameLabel);

        auto* param = proc.getApvts().getParameter (ElfDrumLXRProcessor::kitParamId (meta.index));
        jassert (param != nullptr);

        switch (meta.control)
        {
            case elfmeta::Control::Continuous:
            {
                active = Active::rotary;
                cellWidth = 86;
                slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 58, 14);
                const int off = meta.displayOffset;
                if (off != 0)
                {
                    slider.textFromValueFunction = [off] (double v)
                    { return juce::String (juce::roundToInt (v) + off); };
                    slider.valueFromTextFunction = [off] (const juce::String& t)
                    { return (double) (t.getIntValue() - off); };
                }
                slider.setDoubleClickReturnValue (
                    true, juce::jlimit ((int) meta.minValue, (int) meta.maxValue,
                                        (int) initkit::params[meta.index]));
                addAndMakeVisible (slider);
                sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                    proc.getApvts(), ElfDrumLXRProcessor::kitParamId (meta.index), slider);
                break;
            }
            case elfmeta::Control::Enum:
            case elfmeta::Control::ModTarget:
            {
                active = Active::combo;
                cellWidth = meta.control == elfmeta::Control::ModTarget ? 208 : 168;
                juce::StringArray items;
                for (int i = 0; i < meta.numValues; ++i)
                    items.add (meta.valueNames[i]);
                combo.addItemList (items, 1);
                addAndMakeVisible (combo);
                comboAttachment = std::make_unique<ComboAttachment> (*param, combo, meta.minValue);
                break;
            }
            case elfmeta::Control::Toggle:
            {
                active = Active::toggle;
                cellWidth = 104;
                addAndMakeVisible (toggle);
                toggleAttachment = std::make_unique<ToggleAttachment> (*param, toggle);
                break;
            }
        }
    }

    int desiredWidth() const { return cellWidth; }

    void resized() override
    {
        auto area = getLocalBounds();
        nameLabel.setBounds (area.removeFromTop (14));
        switch (active)
        {
            case Active::rotary: slider.setBounds (area); break;
            case Active::combo:  combo.setBounds (area.removeFromTop (24).reduced (2, 0)); break;
            case Active::toggle: toggle.setBounds (area.removeFromTop (24)); break;
        }
    }

private:
    enum class Active { rotary, combo, toggle };
    Active active = Active::rotary;
    int cellWidth = 86;
    juce::Label nameLabel;
    juce::Slider slider;
    juce::ComboBox combo;
    juce::ToggleButton toggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<ComboAttachment> comboAttachment;
    std::unique_ptr<ToggleAttachment> toggleAttachment;
};

//==============================================================================
// A firmware section (Osc, Filter, ...) as a titled box of flowed controls.
class SectionPanel : public juce::Component
{
public:
    explicit SectionPanel (const juce::String& title)
    {
        titleLabel.setText (title, juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);
    }

    void addControl (std::unique_ptr<ParamControl> c)
    {
        controls.push_back (std::move (c));
        addAndMakeVisible (controls.back().get());
    }

    int heightForWidth (int w) const
    {
        int x = 0, rows = 1;
        for (const auto& c : controls)
        {
            const int cw = c->desiredWidth();
            if (x > 0 && x + cw > w) { ++rows; x = 0; }
            x += cw + kGap;
        }
        return kTitleH + rows * kCellH + (rows - 1) * kGap + 6;
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff22252b));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (juce::Colour (0xff34383f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        titleLabel.setBounds (8, 2, getWidth() - 16, kTitleH - 4);
        int x = 6, y = kTitleH;
        for (const auto& c : controls)
        {
            const int cw = c->desiredWidth();
            if (x > 6 && x + cw > getWidth() - 4) { x = 6; y += kCellH + kGap; }
            c->setBounds (x, y, cw, kCellH);
            x += cw + kGap;
        }
    }

private:
    static constexpr int kTitleH = 22, kCellH = 92, kGap = 6;
    juce::Label titleLabel;
    std::vector<std::unique_ptr<ParamControl>> controls;
};

//==============================================================================
// A page: optional fixed bars (toolbar / mix panel) over a scrolling stack of
// sections.
class StackedPage : public juce::Component
{
public:
    StackedPage()
    {
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    void addFixed (std::unique_ptr<juce::Component> c, int height)
    {
        fixed.push_back ({ std::move (c), height });
        addAndMakeVisible (fixed.back().c.get());
    }

    void addSection (std::unique_ptr<SectionPanel> s)
    {
        sections.push_back (std::move (s));
        content.addAndMakeVisible (sections.back().get());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        for (auto& f : fixed)
        {
            f.c->setBounds (area.removeFromTop (f.h));
            area.removeFromTop (6);
        }
        viewport.setBounds (area);
        const int w = std::max (300, viewport.getWidth() - 16);
        int y = 0;
        for (auto& s : sections)
        {
            const int h = s->heightForWidth (w);
            s->setBounds (0, y, w, h);
            y += h + 8;
        }
        content.setSize (w, y);
    }

private:
    struct Fixed { std::unique_ptr<juce::Component> c; int h; };
    std::vector<Fixed> fixed;
    std::vector<std::unique_ptr<SectionPanel>> sections;
    juce::Viewport viewport;
    juce::Component content;
};

//==============================================================================
class VoiceToolbar : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void()> copy, paste, save, load, randomise;
    };

    explicit VoiceToolbar (Callbacks cb) : callbacks (std::move (cb))
    {
        copyButton.onClick = [this] { if (callbacks.copy) callbacks.copy(); };
        pasteButton.onClick = [this] { if (callbacks.paste) callbacks.paste(); };
        saveButton.onClick = [this] { if (callbacks.save) callbacks.save(); };
        loadButton.onClick = [this] { if (callbacks.load) callbacks.load(); };
        randomButton.onClick = [this] { if (callbacks.randomise) callbacks.randomise(); };
        for (auto* b : { &copyButton, &pasteButton, &saveButton, &loadButton, &randomButton })
            addAndMakeVisible (b);
    }

    void setPasteEnabled (bool e) { pasteButton.setEnabled (e); }

    void resized() override
    {
        auto area = getLocalBounds();
        for (auto* b : { &copyButton, &pasteButton, &saveButton, &loadButton, &randomButton })
        {
            b->setBounds (area.removeFromLeft (104));
            area.removeFromLeft (6);
        }
    }

    juce::TextButton copyButton { "Copy Voice" }, pasteButton { "Paste Voice" };
    juce::TextButton saveButton { "Save Voice..." }, loadButton { "Load Voice..." };
    juce::TextButton randomButton { "Randomise" };

private:
    Callbacks callbacks;
};

//==============================================================================
// Performance faders for the Global/Mix page.
class MixPanel : public juce::Component
{
public:
    explicit MixPanel (ElfDrumLXRProcessor& proc)
    {
        titleLabel.setText ("Mix — performance faders (live engine faders + master trim; saved with the session, not in kits)",
                            juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);
        for (int v = 0; v < 7; ++v)
        {
            auto& ch = chans[(size_t) v];
            ch.slider.setSliderStyle (juce::Slider::LinearVertical);
            ch.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 14);
            ch.slider.setDoubleClickReturnValue (true, 1.0);
            ch.label.setText (v < 6 ? kVoiceNames[v] : "Master", juce::dontSendNotification);
            ch.label.setFont (juce::FontOptions (11.0f));
            ch.label.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (ch.slider);
            addAndMakeVisible (ch.label);
            ch.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                proc.getApvts(), ElfDrumLXRProcessor::faderParamId (v), ch.slider);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff22252b));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (juce::Colour (0xff34383f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 4);
        titleLabel.setBounds (area.removeFromTop (18));
        const int w = area.getWidth() / 7;
        for (auto& ch : chans)
        {
            auto col = area.removeFromLeft (w);
            ch.label.setBounds (col.removeFromBottom (14));
            ch.slider.setBounds (col.reduced (6, 2));
        }
    }

private:
    struct Chan
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    juce::Label titleLabel;
    std::array<Chan, 7> chans;
};
} // namespace

//==============================================================================
ElfDrumLXRProcessorEditor::ElfDrumLXRProcessorEditor (ElfDrumLXRProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getDarkColourScheme());
    setLookAndFeel (&lookAndFeel);

    titleLabel.setText ("ElfDrumLXR", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);
    kitNameLabel.setFont (juce::FontOptions (15.0f));
    addAndMakeVisible (kitNameLabel);
    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setJustificationType (juce::Justification::right);
    addAndMakeVisible (statusLabel);

    loadButton.onClick = [this] { chooseKit (KitChoice::load); };
    saveButton.onClick = [this] { chooseKit (KitChoice::save); };
    initButton.onClick = [this] { processor.loadFactoryInit(); setStatus ("INIT kit loaded"); };
    randomKitButton.onClick = [this]
    {
        processor.randomiseParams (-1, (uint32_t) juce::Time::currentTimeMillis());
        setStatus ("Kit randomised (guarded)");
    };
    for (auto* b : { &loadButton, &saveButton, &initButton, &randomKitButton })
        addAndMakeVisible (b);

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
    morphTargetCaption.setText ("Morph target:", juce::dontSendNotification);
    addAndMakeVisible (morphTargetCaption);
    morphTargetLabel.setFont (juce::FontOptions (15.0f));
    addAndMakeVisible (morphTargetLabel);
    loadTargetButton.onClick = [this] { chooseKit (KitChoice::loadTarget); };
    addAndMakeVisible (loadTargetButton);

    // --- kit browser ---
    juce::PropertiesFile::Options opts;
    opts.applicationName = "ElfDrumLXR";
    opts.filenameSuffix = ".settings";
    opts.folderName = "ElfDrumLXR";
    opts.osxLibrarySubFolder = "Application Support";
    props = std::make_unique<juce::PropertiesFile> (opts);
    const auto savedFolder = props->getValue ("kitFolder");
    kitFolder = savedFolder.isNotEmpty()
        ? juce::File (savedFolder)
        : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
              .getChildFile ("ElfDrumLXR/kits");
    folderButton.onClick = [this] { chooseKitFolder(); };
    refreshButton.onClick = [this] { rescanKits(); };
    prevKitButton.onClick = [this] { stepKit (-1); };
    nextKitButton.onClick = [this] { stepKit (1); };
    addAndMakeVisible (folderButton);
    addAndMakeVisible (refreshButton);
    addAndMakeVisible (folderLabel);
    kitList.setModel (this);
    kitList.setRowHeight (20);
    addAndMakeVisible (kitList);
    addAndMakeVisible (prevKitButton);
    addAndMakeVisible (nextKitButton);
    folderLabel.setFont (juce::FontOptions (11.0f));
    rescanKits();

    // --- pages from the generated metadata, in panel (firmware) order ---
    std::array<StackedPage*, 8> pages {};
    for (int pg = 0; pg < 8; ++pg)
    {
        auto page = std::make_unique<StackedPage>();
        pages[pg] = page.get();
        if (pg < 6)
        {
            VoiceToolbar::Callbacks cb;
            cb.copy = [this, pg] { copyVoice (pg); };
            cb.paste = [this, pg] { pasteVoice (pg); };
            cb.save = [this, pg] { saveVoice (pg); };
            cb.load = [this, pg] { loadVoice (pg); };
            cb.randomise = [this, pg]
            {
                processor.randomiseParams (pg, (uint32_t) juce::Time::currentTimeMillis());
                setStatus (juce::String (kPageNames[pg]) + " randomised (guarded)");
            };
            auto bar = std::make_unique<VoiceToolbar> (std::move (cb));
            pasteButtons[pg] = &bar->pasteButton;
            page->addFixed (std::move (bar), 30);
        }
        else if (pg == elfmeta::PageGlobal)
        {
            page->addFixed (std::make_unique<MixPanel> (processor), 196);
        }
        else if (pg == elfmeta::PageFX)
        {
            auto hint = std::make_unique<juce::Label> (
                "fxHint",
                "P1-P4 by type — Dist: Drive/Color/Tone/Volume · Ring: Freq · Comp: Attack/Decay/Threshold/Gain · "
                "Delay: Time/Range/Tone/Feedback · Reverb: PreDelay/Decay/Tone/Reflection");
            hint->setFont (juce::FontOptions (11.0f));
            page->addFixed (std::move (hint), 18);
        }

        // sections in firmware order for this page
        juce::String currentSection;
        SectionPanel* section = nullptr;
        for (uint8_t idx8 : elfmeta::kPanelOrder)
        {
            const auto& meta = elfmeta::kKitParams[idx8];
            if (meta.page != pg)
                continue;
            if (section == nullptr || juce::String (meta.section) != currentSection)
            {
                currentSection = meta.section;
                auto s = std::make_unique<SectionPanel> (currentSection);
                section = s.get();
                page->addSection (std::move (s));
            }
            section->addControl (std::make_unique<ParamControl> (processor, meta));
        }

        tabs.addTab (kPageNames[pg], juce::Colour (0xff22252b), page.release(), true);
    }
    // Phase 3: the Sequencer tab (firmware pattern editor, see SeqEditor).
    tabs.addTab ("Sequencer", juce::Colour (0xff22252b), new SeqEditor (processor), true);
    // Phase 5: the bonus-programme pages (see Phase5Editor).
    tabs.addTab ("Blender", juce::Colour (0xff22252b), new BlenderPage (processor), true);
    tabs.addTab ("Library", juce::Colour (0xff22252b), new LibraryPage (processor), true);
    tabs.addTab ("Create", juce::Colour (0xff22252b), new CreatePage (processor), true);
    tabs.addTab ("Perform", juce::Colour (0xff22252b), new PerformPage (processor), true);
    tabs.addTab ("Export", juce::Colour (0xff22252b), new ExportPage (processor), true);
    addAndMakeVisible (tabs);

    kitNameLabel.setText (processor.getKitName(), juce::dontSendNotification);
    morphTargetLabel.setText (processor.getMorphTargetName(), juce::dontSendNotification);
    setStatus ("Panel: firmware menu layout (Phase 2)");

    setResizable (true, true);
    setResizeLimits (940, 620, 2600, 1800);
    setSize (1200, 820);
    startTimerHz (4);
}

ElfDrumLXRProcessorEditor::~ElfDrumLXRProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void ElfDrumLXRProcessorEditor::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

void ElfDrumLXRProcessorEditor::timerCallback()
{
    const auto name = processor.getKitName();
    if (kitNameLabel.getText() != name)
        kitNameLabel.setText (name, juce::dontSendNotification);
    const auto target = processor.getMorphTargetName();
    if (morphTargetLabel.getText() != target)
        morphTargetLabel.setText (target, juce::dontSendNotification);
    for (int v = 0; v < 6; ++v)
        if (pasteButtons[v] != nullptr)
            pasteButtons[v]->setEnabled (hasVoiceClipboard && voiceClipboard.voice == v);
}

//==============================================================================
void ElfDrumLXRProcessorEditor::chooseKit (KitChoice choice)
{
    const bool save = choice == KitChoice::save;
    chooser = std::make_unique<juce::FileChooser> (
        save ? "Save kit (.SND)"
             : (choice == KitChoice::loadTarget ? "Load morph target kit (.SND)" : "Load kit (.SND)"),
        kitFolder, "*.SND;*.snd");
    const int flags = save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                           : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (flags, [this, choice] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File()) return;
        if (choice == KitChoice::save)            processor.saveKitFile (file);
        else if (choice == KitChoice::loadTarget) processor.loadMorphTargetFile (file);
        else                                      processor.loadKitFile (file);
    });
}

//==============================================================================
// Kit browser
void ElfDrumLXRProcessorEditor::rescanKits()
{
    kitEntries.clear();
    currentKitRow = -1;
    if (kitFolder.isDirectory())
    {
        juce::Array<juce::File> files;
        kitFolder.findChildFiles (files, juce::File::findFiles, false, "*.SND");
        kitFolder.findChildFiles (files, juce::File::findFiles, false, "*.snd");
        std::set<juce::String> seen;
        std::vector<juce::File> uniq;
        for (const auto& f : files)
            if (seen.insert (f.getFullPathName()).second)
                uniq.push_back (f);
        std::sort (uniq.begin(), uniq.end(),
                   [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareNatural (b.getFileName()) < 0; });
        for (const auto& f : uniq)
            kitEntries.push_back ({ f, kitNameFromHeader (f) });
    }
    folderLabel.setText (kitFolder.getFileName(), juce::dontSendNotification);
    folderLabel.setTooltip (kitFolder.getFullPathName());
    kitList.updateContent();
}

void ElfDrumLXRProcessorEditor::chooseKitFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose kit folder", kitFolder, "");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (dir == juce::File()) return;
        kitFolder = dir;
        props->setValue ("kitFolder", kitFolder.getFullPathName());
        props->saveIfNeeded();
        rescanKits();
        setStatus (juce::String ((int) kitEntries.size()) + " kits in folder");
    });
}

int ElfDrumLXRProcessorEditor::getNumRows() { return (int) kitEntries.size(); }

void ElfDrumLXRProcessorEditor::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int) kitEntries.size())
        return;
    if (selected)
        g.fillAll (juce::Colour (0xff3a4a5f));
    const auto& e = kitEntries[(size_t) row];
    g.setColour (juce::Colour (0xffe8e8e8));
    g.setFont (juce::FontOptions (12.0f));
    const auto fileName = e.file.getFileName();
    const auto text = e.kitName.isNotEmpty() && e.kitName != fileName
        ? e.kitName + "  ·  " + fileName : fileName;
    g.drawText (text, 4, 0, width - 6, height, juce::Justification::centredLeft, true);
}

void ElfDrumLXRProcessorEditor::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    loadBrowserKit (row);
}

void ElfDrumLXRProcessorEditor::loadBrowserKit (int row)
{
    if (row < 0 || row >= (int) kitEntries.size())
        return;
    currentKitRow = row;
    kitList.selectRow (row, false, false);
    if (processor.loadKitFile (kitEntries[(size_t) row].file))
        setStatus ("Loaded " + kitEntries[(size_t) row].file.getFileName());
}

void ElfDrumLXRProcessorEditor::stepKit (int delta)
{
    const int n = (int) kitEntries.size();
    if (n == 0)
        return;
    int row = currentKitRow < 0 ? (delta > 0 ? 0 : n - 1)
                                : (currentKitRow + delta + n) % n;
    loadBrowserKit (row);
}

//==============================================================================
// Per-voice slices (copy/paste in memory; save/load via .lxv files)
void ElfDrumLXRProcessorEditor::applySliceToVoice (int voice, const std::vector<uint8_t>& slice)
{
    if (! processor.applyVoiceSlice (voice, slice))
    {
        setStatus ("Voice slice size mismatch — not applied");
        return;
    }
    setStatus (juce::String (kVoiceNames[voice]) + " voice applied");
}

void ElfDrumLXRProcessorEditor::copyVoice (int voice)
{
    const auto bytes = processor.currentKitBytes();
    voiceClipboard.voice = voice;
    voiceClipboard.slice = voiceslice::extract (bytes, voice);
    const auto name = processor.getKitName().toStdString();
    voiceClipboard.sourceKitName.fill (0);
    std::memcpy (voiceClipboard.sourceKitName.data(), name.data(),
                 std::min (name.size(), (size_t) 8));
    hasVoiceClipboard = true;
    setStatus (juce::String ("Copied ") + kVoiceNames[voice] + " voice");
}

void ElfDrumLXRProcessorEditor::pasteVoice (int voice)
{
    if (! hasVoiceClipboard || voiceClipboard.voice != voice)
        return;   // slices are voice-specific (different index sets per voice)
    applySliceToVoice (voice, voiceClipboard.slice);
}

juce::File voicesFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("ElfDrumLXR/voices");
    dir.createDirectory();
    return dir;
}

void ElfDrumLXRProcessorEditor::saveVoice (int voice)
{
    const auto bytes = processor.currentKitBytes();
    const auto slice = voiceslice::extract (bytes, voice);
    std::array<char, 8> name {};
    const auto kitName = processor.getKitName().toStdString();
    std::memcpy (name.data(), kitName.data(), std::min (kitName.size(), (size_t) 8));
    const auto data = voiceslice::serialiseLxv (voice, slice, name);
    const auto defaultName = processor.getKitName() + " "
        + juce::String (kVoiceNames[voice]).replaceCharacter (' ', '-') + ".lxv";
    chooser = std::make_unique<juce::FileChooser> ("Save voice (.lxv)", voicesFolder().getChildFile (defaultName), "*.lxv");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                          [data] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File()) return;
        file.replaceWithData (data.data(), data.size());
    });
}

void ElfDrumLXRProcessorEditor::loadVoice (int voice)
{
    chooser = std::make_unique<juce::FileChooser> ("Load voice (.lxv)", voicesFolder(), "*.lxv");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, voice] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File()) return;
        juce::MemoryBlock block;
        if (! file.loadFileAsData (block)) { setStatus ("Could not read " + file.getFileName()); return; }
        voiceslice::LxvData parsed;
        if (! voiceslice::parseLxv ((const uint8_t*) block.getData(), block.getSize(), parsed))
        {
            setStatus (file.getFileName() + ": not a valid .lxv for this firmware");
            return;
        }
        if (parsed.voice != voice)
        {
            setStatus (file.getFileName() + " holds a " + kVoiceNames[parsed.voice]
                       + " voice — open that page to load it");
            return;
        }
        applySliceToVoice (voice, parsed.slice);
    });
}

//==============================================================================
bool ElfDrumLXRProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".snd"))
            return true;
    return false;
}

void ElfDrumLXRProcessorEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    // Dropping onto the morph rows sets the morph target; anywhere else loads the kit.
    const bool asTarget = morphDropZone.contains (x, y);
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".snd"))
        {
            if (asTarget) processor.loadMorphTargetFile (juce::File (f));
            else          processor.loadKitFile (juce::File (f));
            break;
        }
}

void ElfDrumLXRProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1c1f24));
}

void ElfDrumLXRProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto row1 = area.removeFromTop (32);
    titleLabel.setBounds (row1.removeFromLeft (170));
    kitNameLabel.setBounds (row1.removeFromLeft (180));
    statusLabel.setBounds (row1);
    area.removeFromTop (4);

    auto row2 = area.removeFromTop (28);
    loadButton.setBounds (row2.removeFromLeft (104));
    row2.removeFromLeft (6);
    saveButton.setBounds (row2.removeFromLeft (104));
    row2.removeFromLeft (6);
    initButton.setBounds (row2.removeFromLeft (56));
    row2.removeFromLeft (6);
    randomKitButton.setBounds (row2.removeFromLeft (124));
    row2.removeFromLeft (16);
    morphLabel.setBounds (row2.removeFromLeft (48));
    morphSlider.setBounds (row2);
    area.removeFromTop (4);

    auto row3 = area.removeFromTop (28);
    morphTargetCaption.setBounds (row3.removeFromLeft (104));
    loadTargetButton.setBounds (row3.removeFromRight (140));
    row3.removeFromRight (8);
    morphTargetLabel.setBounds (row3);
    morphDropZone = row2.getUnion (row3).expanded (0, 2);
    area.removeFromTop (6);

    auto trigRow = area.removeFromTop (34);
    const int tw = (trigRow.getWidth() - 6 * 6) / 7;
    for (int v = 0; v < 7; ++v)
    {
        triggerButtons[v].setBounds (trigRow.removeFromLeft (tw));
        trigRow.removeFromLeft (6);
    }
    area.removeFromTop (8);

    auto browser = area.removeFromLeft (204);
    area.removeFromLeft (8);
    auto browserTop = browser.removeFromTop (26);
    folderButton.setBounds (browserTop.removeFromLeft (120));
    browserTop.removeFromLeft (4);
    refreshButton.setBounds (browserTop);
    folderLabel.setBounds (browser.removeFromTop (16));
    auto browserBottom = browser.removeFromBottom (26);
    prevKitButton.setBounds (browserBottom.removeFromLeft (98));
    browserBottom.removeFromLeft (4);
    nextKitButton.setBounds (browserBottom);
    kitList.setBounds (browser);

    tabs.setBounds (area);
}
