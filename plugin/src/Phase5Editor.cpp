#include "Phase5Editor.h"
#include "Phase5.h"

//==============================================================================
BlenderPage::BlenderPage (ElfDrumLXRProcessor& proc) : processor (proc)
{
    folder = voicebank::defaultFolder();

    for (int v = 0; v < 6; ++v)
        voiceBox.addItem (p5VoiceName (v), v + 1);
    voiceBox.setSelectedId (1, juce::dontSendNotification);
    voiceBox.onChange = [this] { rescan(); };
    addAndMakeVisible (voiceBox);

    presetList.setModel (this);
    presetList.setRowHeight (20);
    addAndMakeVisible (presetList);

    folderLabel.setText (folder.getFullPathName(), juce::dontSendNotification);
    folderLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (folderLabel);
    folderButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Voice preset folder", folder);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                if (fc.getResults().isEmpty()) return;
                folder = fc.getResult();
                folderLabel.setText (folder.getFullPathName(), juce::dontSendNotification);
                rescan();
            });
    };
    addAndMakeVisible (folderButton);
    rescanButton.onClick = [this] { rescan(); };
    addAndMakeVisible (rescanButton);

    applyButton.onClick = [this] { applySelected (false); };
    addAndMakeVisible (applyButton);
    auditionButton.onClick = [this] { applySelected (true); };
    addAndMakeVisible (auditionButton);

    saveName.setText ("MyVoice", juce::dontSendNotification);
    addAndMakeVisible (saveName);
    saveButton.onClick = [this] { saveCurrentVoice(); };
    addAndMakeVisible (saveButton);
    fromKitButton.onClick = [this] { extractFromKit(); };
    addAndMakeVisible (fromKitButton);

    statusLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (statusLabel);

    rescan();
}

void BlenderPage::rescan()
{
    const int voice = voiceBox.getSelectedId() - 1;
    presets.clear();
    for (auto& p : voicebank::scan (folder))
        if (p.voice == voice)
            presets.push_back (p);
    presetList.updateContent();
    statusLabel.setText (juce::String (presets.size()) + " preset(s) for " + p5VoiceName (voice)
                             + " in the library",
                         juce::dontSendNotification);
}

int BlenderPage::getNumRows() { return (int) presets.size(); }

void BlenderPage::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int) presets.size())
        return;
    if (selected)
        g.fillAll (juce::Colour (0xff3a4a5f));
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (13.0f));
    const auto& p = presets[(size_t) row];
    g.drawText (p.file.getFileNameWithoutExtension() + "   (from kit: " + p.sourceKitName + ")",
                6, 0, width - 12, height, juce::Justification::centredLeft);
}

void BlenderPage::listBoxItemDoubleClicked (int, const juce::MouseEvent&) { applySelected (true); }

void BlenderPage::applySelected (bool audition)
{
    const int row = presetList.getSelectedRow();
    if (row < 0 || row >= (int) presets.size())
    {
        statusLabel.setText ("Select a preset first", juce::dontSendNotification);
        return;
    }
    voiceslice::LxvData d;
    if (! voicebank::loadFile (presets[(size_t) row].file, d)
        || ! processor.applyVoiceSlice (d.voice, d.slice))
    {
        statusLabel.setText ("Preset failed to apply", juce::dontSendNotification);
        return;
    }
    statusLabel.setText ("Applied " + presets[(size_t) row].file.getFileName()
                             + " to " + p5VoiceName (d.voice),
                         juce::dontSendNotification);
    if (audition)
        processor.triggerVoice (d.voice);
}

void BlenderPage::saveCurrentVoice()
{
    const int voice = voiceBox.getSelectedId() - 1;
    juce::File out;
    if (voicebank::save (folder, saveName.getText(), voice, processor.currentKitBytes(),
                         processor.currentKitNameBytes(), out))
    {
        statusLabel.setText ("Saved " + out.getFileName(), juce::dontSendNotification);
        rescan();
    }
    else
        statusLabel.setText ("Save failed", juce::dontSendNotification);
}

void BlenderPage::extractFromKit()
{
    const int voice = voiceBox.getSelectedId() - 1;
    chooser = std::make_unique<juce::FileChooser> ("Kit to take the voice from", juce::File(), "*.SND;*.snd");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, voice] (const juce::FileChooser& fc)
        {
            if (fc.getResults().isEmpty()) return;
            std::vector<uint8_t> slice;
            std::array<char, 8> name {};
            if (! voicebank::sliceFromKitFile (fc.getResult(), voice, slice, name)
                || ! processor.applyVoiceSlice (voice, slice))
            {
                statusLabel.setText ("Could not extract that voice", juce::dontSendNotification);
                return;
            }
            statusLabel.setText (juce::String (p5VoiceName (voice)) + " taken from "
                                     + fc.getResult().getFileName() + " and applied",
                                 juce::dontSendNotification);
            processor.triggerVoice (voice);
        });
}

void BlenderPage::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (24);
    voiceBox.setBounds (top.removeFromLeft (140));
    top.removeFromLeft (8);
    folderButton.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (6);
    rescanButton.setBounds (top.removeFromLeft (80));
    r.removeFromTop (4);
    folderLabel.setBounds (r.removeFromTop (16));
    r.removeFromTop (6);
    auto bottom = r.removeFromBottom (110);
    presetList.setBounds (r);
    auto row = bottom.removeFromTop (26);
    applyButton.setBounds (row.removeFromLeft (100));
    row.removeFromLeft (6);
    auditionButton.setBounds (row.removeFromLeft (190));
    bottom.removeFromTop (6);
    row = bottom.removeFromTop (26);
    saveName.setBounds (row.removeFromLeft (180));
    row.removeFromLeft (6);
    saveButton.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (6);
    fromKitButton.setBounds (row.removeFromLeft (180));
    bottom.removeFromTop (6);
    statusLabel.setBounds (bottom.removeFromTop (18));
}

//==============================================================================
LibraryPage::LibraryPage (ElfDrumLXRProcessor& proc) : processor (proc)
{
    root = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("ElfDrumLXR");
    rootLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (rootLabel);
    rootButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("SD card / library root", root);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                if (fc.getResults().isEmpty()) return;
                root = fc.getResult();
                rescan();
            });
    };
    addAndMakeVisible (rootButton);
    rescanButton.onClick = [this] { rescan(); };
    addAndMakeVisible (rescanButton);

    treeList.setModel (this);
    treeList.setRowHeight (19);
    addAndMakeVisible (treeList);

    for (int i = 0; i < 64; ++i)
        slotBox.addItem ("Slot " + juce::String (i), i + 1);
    slotBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (slotBox);
    importButton.onClick = [this] { importRow (treeList.getSelectedRow()); };
    addAndMakeVisible (importButton);
    exportPackButton.onClick = [this] { exportSelectedPack(); };
    addAndMakeVisible (exportPackButton);
    importPackButton.onClick = [this] { importPack(); };
    addAndMakeVisible (importPackButton);
    authorEditor.setText ("", juce::dontSendNotification);
    authorEditor.setTextToShowWhenEmpty ("author (for pack manifest)", juce::Colours::grey);
    addAndMakeVisible (authorEditor);
    statusLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (statusLabel);
    rescan();
}

void LibraryPage::rescan()
{
    rootLabel.setText (root.getFullPathName(), juce::dontSendNotification);
    projects = cardlib::scanCard (root);
    rows.clear();
    for (int pi = 0; pi < (int) projects.size(); ++pi)
    {
        const auto& p = projects[(size_t) pi];
        rows.push_back ({ pi, -1, {}, p.name + "  (" + juce::String (p.kits.size()) + " kits, "
                                       + juce::String (p.patterns.size()) + " patterns, "
                                       + juce::String (p.songs.size()) + " songs)" });
        for (auto& f : p.kits)
            rows.push_back ({ pi, 0, f, "    KIT  " + f.getFileName() + "  —  " + cardlib::readKitName (f) });
        for (auto& f : p.patterns)
            rows.push_back ({ pi, 1, f, "    PAT  " + f.getFileName() });
        for (auto& f : p.songs)
            rows.push_back ({ pi, 2, f, "    SNG  " + f.getFileName() });
    }
    treeList.updateContent();
    statusLabel.setText (juce::String (projects.size()) + " project(s) found", juce::dontSendNotification);
}

int LibraryPage::getNumRows() { return (int) rows.size(); }

void LibraryPage::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int) rows.size())
        return;
    const auto& r = rows[(size_t) row];
    if (selected)
        g.fillAll (juce::Colour (0xff3a4a5f));
    g.setColour (r.kind < 0 ? juce::Colour (0xffffc873) : juce::Colours::white);
    g.setFont (juce::FontOptions (r.kind < 0 ? 13.5f : 12.5f, r.kind < 0 ? juce::Font::bold : juce::Font::plain));
    g.drawText (r.text, 6, 0, width - 12, height, juce::Justification::centredLeft);
}

void LibraryPage::listBoxItemDoubleClicked (int row, const juce::MouseEvent&) { importRow (row); }

void LibraryPage::importRow (int row)
{
    if (row < 0 || row >= (int) rows.size())
        return;
    const auto& r = rows[(size_t) row];
    const int slot = slotBox.getSelectedId() - 1;
    if (r.kind == 0)
    {
        // Kit import doubles as the in-context audition: the pattern keeps
        // playing, only the kit changes (the processor's normal load path).
        statusLabel.setText (processor.loadKitFile (r.file)
                                 ? "Loaded kit " + r.file.getFileName()
                                 : "Kit failed to load",
                             juce::dontSendNotification);
    }
    else if (r.kind == 1)
    {
        processor.seqLoadPatternFile (slot, r.file);
        statusLabel.setText ("Pattern " + r.file.getFileName() + " -> slot " + juce::String (slot)
                                 + " (applies on the audio thread)",
                             juce::dontSendNotification);
    }
    else if (r.kind == 2)
    {
        processor.seqLoadSongFile (slot, r.file);
        statusLabel.setText ("Song " + r.file.getFileName() + " -> slot " + juce::String (slot),
                             juce::dontSendNotification);
    }
}

void LibraryPage::exportSelectedPack()
{
    const int row = treeList.getSelectedRow();
    int projectIdx = -1;
    if (row >= 0 && row < (int) rows.size())
        projectIdx = rows[(size_t) row].project;
    if (projectIdx < 0)
    {
        statusLabel.setText ("Select a project (or one of its files) first", juce::dontSendNotification);
        return;
    }
    const auto dir = projects[(size_t) projectIdx].dir;
    chooser = std::make_unique<juce::FileChooser> ("Save pack", root.getChildFile (dir.getFileName() + ".zip"), "*.zip");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this, dir] (const juce::FileChooser& fc)
        {
            if (fc.getResults().isEmpty()) return;
            juce::String err;
            statusLabel.setText (cardlib::exportPack (dir, fc.getResult(), authorEditor.getText(), err)
                                     ? "Pack written: " + fc.getResult().getFileName()
                                     : "Export failed: " + err,
                                 juce::dontSendNotification);
        });
}

void LibraryPage::importPack()
{
    chooser = std::make_unique<juce::FileChooser> ("Import pack", juce::File(), "*.zip");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResults().isEmpty()) return;
            juce::File dir;
            juce::String err;
            if (cardlib::importPack (fc.getResult(), root, dir, err))
            {
                statusLabel.setText ("Imported project " + dir.getFileName(), juce::dontSendNotification);
                rescan();
            }
            else
                statusLabel.setText ("Import failed: " + err, juce::dontSendNotification);
        });
}

void LibraryPage::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (24);
    rootButton.setBounds (top.removeFromLeft (190));
    top.removeFromLeft (6);
    rescanButton.setBounds (top.removeFromLeft (80));
    r.removeFromTop (4);
    rootLabel.setBounds (r.removeFromTop (16));
    r.removeFromTop (6);
    auto bottom = r.removeFromBottom (88);
    treeList.setBounds (r);
    auto row = bottom.removeFromTop (26);
    slotBox.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (6);
    importButton.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (6);
    exportPackButton.setBounds (row.removeFromLeft (190));
    row.removeFromLeft (6);
    importPackButton.setBounds (row.removeFromLeft (120));
    bottom.removeFromTop (6);
    row = bottom.removeFromTop (26);
    authorEditor.setBounds (row.removeFromLeft (220));
    bottom.removeFromTop (6);
    statusLabel.setBounds (bottom.removeFromTop (18));
}

//==============================================================================
CreatePage::CreatePage (ElfDrumLXRProcessor& proc) : processor (proc)
{
    mutateLabel.setText ("Mutate: guarded drift of the current kit (routing / mod-targets / "
                         "morph-dest are never touched; levels floored). Amount = reach.",
                         juce::dontSendNotification);
    mutateLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (mutateLabel);
    mutateAmount.setRange (0.0, 1.0, 0.01);
    mutateAmount.setValue (0.25, juce::dontSendNotification);
    mutateAmount.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    addAndMakeVisible (mutateAmount);
    mutateSeed.setText ("42", juce::dontSendNotification);
    addAndMakeVisible (mutateSeed);
    mutateButton.onClick = [this] { doMutate(); };
    addAndMakeVisible (mutateButton);
    undoButton.onClick = [this]
    {
        processor.undoKitChange();
        statusLabel.setText ("Undone. Lineage depth: " + juce::String (processor.kitHistorySize()),
                             juce::dontSendNotification);
    };
    addAndMakeVisible (undoButton);

    breedLabel.setText ("Breed: coin-flip inheritance from kit B (per parameter, or per voice "
                        "with the toggle). Keep-set always comes from the current kit.",
                        juce::dontSendNotification);
    breedLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (breedLabel);
    loadBreedButton.onClick = [this] { loadBreedKit(); };
    addAndMakeVisible (loadBreedButton);
    breedKitLabel.setText ("(no kit B loaded)", juce::dontSendNotification);
    addAndMakeVisible (breedKitLabel);
    addAndMakeVisible (breedVoiceToggle);
    breedSeed.setText ("7", juce::dontSendNotification);
    addAndMakeVisible (breedSeed);
    breedButton.onClick = [this] { doBreed(); };
    addAndMakeVisible (breedButton);
    statusLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (statusLabel);

    // --- pattern generator / remix (item 5) ---
    patternLabel.setText ("Pattern: generate writes one 16-step bar per track (tiled over the "
                          "64 slots, track length 16); Remix transforms the pattern in place. "
                          "Density per track:",
                          juce::dontSendNotification);
    patternLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (patternLabel);
    for (int i = 0; i < 64; ++i)
        patternBox.addItem ("Pattern " + juce::String (i), i + 1);
    patternBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (patternBox);
    const double defaultDensity[7] = { 0.5, 0.25, 0.25, 0.5, 0.15, 0.35, 0.0 };
    for (int t = 0; t < 7; ++t)
    {
        densitySliders[t].setRange (0.0, 1.0, 0.01);
        densitySliders[t].setValue (defaultDensity[t], juce::dontSendNotification);
        densitySliders[t].setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 16);
        addAndMakeVisible (densitySliders[t]);
    }
    addAndMakeVisible (euclidToggle);
    genSeed.setText ("42", juce::dontSendNotification);
    addAndMakeVisible (genSeed);
    generateButton.onClick = [this] { doGenerate(); };
    addAndMakeVisible (generateButton);
    thinKeep.setRange (0.0, 1.0, 0.01);
    thinKeep.setValue (0.6, juce::dontSendNotification);
    thinKeep.setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 16);
    addAndMakeVisible (thinKeep);
    thinButton.onClick = [this]
    {
        const float keep = (float) thinKeep.getValue();
        const uint32_t seed = seedFromEditor (genSeed);
        withPattern ([keep, seed] (seqops::PatternData& p) { seqops::remixThin (p, seed, keep); },
                     "Thinned");
    };
    addAndMakeVisible (thinButton);
    rotateAmount.setText ("1", juce::dontSendNotification);
    addAndMakeVisible (rotateAmount);
    rotateButton.onClick = [this]
    {
        const int k = rotateAmount.getText().getIntValue();
        withPattern ([k] (seqops::PatternData& p) { seqops::remixRotate (p, -1, k); }, "Rotated");
    };
    addAndMakeVisible (rotateButton);
    jitterAmount.setText ("8", juce::dontSendNotification);
    addAndMakeVisible (jitterAmount);
    jitterButton.onClick = [this]
    {
        const int amt = jitterAmount.getText().getIntValue();
        const uint32_t seed = seedFromEditor (genSeed);
        withPattern ([amt, seed] (seqops::PatternData& p) { seqops::remixJitterLocks (p, seed, amt); },
                     "Locks jittered");
    };
    addAndMakeVisible (jitterButton);
    undoPatternButton.onClick = [this] { doUndoPattern(); };
    addAndMakeVisible (undoPatternButton);

    grooveLoadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Groove MIDI file", juce::File(), "*.mid;*.MID");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                if (fc.getResults().isEmpty()) return;
                juce::String err;
                if (::groove::extractFromMidi (fc.getResult(), groove, err))
                    grooveLabel.setText ("Groove: " + fc.getResult().getFileName(), juce::dontSendNotification);
                else
                    statusLabel.setText ("Groove failed: " + err, juce::dontSendNotification);
            });
    };
    addAndMakeVisible (grooveLoadButton);
    grooveLabel.setText ("(no groove loaded)", juce::dontSendNotification);
    addAndMakeVisible (grooveLabel);
    grooveStrength.setRange (0.0, 1.0, 0.01);
    grooveStrength.setValue (0.7, juce::dontSendNotification);
    grooveStrength.setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 16);
    addAndMakeVisible (grooveStrength);
    grooveApplyButton.onClick = [this]
    {
        if (! groove.valid)
        {
            statusLabel.setText ("Load a groove .mid first", juce::dontSendNotification);
            return;
        }
        const float strength = (float) grooveStrength.getValue();
        const auto g = groove;
        withPattern ([g, strength] (seqops::PatternData& p) { seqops::applyGroove (p, g, strength); },
                     "Groove applied to");
    };
    addAndMakeVisible (grooveApplyButton);
    startTimerHz (2);
}

CreatePage::~CreatePage() { stopTimer(); }

void CreatePage::timerCallback()
{
    // Keep a fresh pattern-set snapshot flowing so readPatternData (Remix /
    // undo source) is current.
    processor.requestSeqSnapshot();
}

int CreatePage::editPattern() const { return patternBox.getSelectedId() - 1; }

void CreatePage::withPattern (const std::function<void (seqops::PatternData&)>& fn, const juce::String& what)
{
    seqops::PatternData p;
    if (! processor.readPatternData (editPattern(), p))
    {
        statusLabel.setText ("No pattern snapshot yet — press play once or wait a moment",
                             juce::dontSendNotification);
        return;
    }
    patternUndo = p;
    hasPatternUndo = true;
    fn (p);
    processor.writePatternData (editPattern(), p);
    statusLabel.setText (what + " pattern " + juce::String (editPattern()), juce::dontSendNotification);
}

void CreatePage::doGenerate()
{
    float density[7];
    for (int t = 0; t < 7; ++t)
        density[t] = (float) densitySliders[t].getValue();
    seqops::PatternData current;
    if (processor.readPatternData (editPattern(), current))
    {
        patternUndo = current;
        hasPatternUndo = true;
    }
    processor.writePatternData (editPattern(),
                                seqops::generate (seedFromEditor (genSeed), density,
                                                  euclidToggle.getToggleState()));
    statusLabel.setText ("Generated pattern " + juce::String (editPattern())
                             + (euclidToggle.getToggleState() ? " (Euclidean)" : " (free)"),
                         juce::dontSendNotification);
}

void CreatePage::doUndoPattern()
{
    if (! hasPatternUndo)
    {
        statusLabel.setText ("Nothing to undo", juce::dontSendNotification);
        return;
    }
    processor.writePatternData (editPattern(), patternUndo);
    hasPatternUndo = false;
    statusLabel.setText ("Pattern edit undone", juce::dontSendNotification);
}

uint32_t CreatePage::seedFromEditor (const juce::TextEditor& ed) const
{
    return (uint32_t) ed.getText().getIntValue();
}

void CreatePage::doMutate()
{
    processor.mutateKit ((float) mutateAmount.getValue(), seedFromEditor (mutateSeed));
    statusLabel.setText ("Mutated (amount " + juce::String (mutateAmount.getValue(), 2)
                             + ", seed " + mutateSeed.getText() + "). Lineage depth: "
                             + juce::String (processor.kitHistorySize()),
                         juce::dontSendNotification);
}

void CreatePage::loadBreedKit()
{
    chooser = std::make_unique<juce::FileChooser> ("Kit B", juce::File(), "*.SND;*.snd");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResults().isEmpty()) return;
            juce::MemoryBlock data;
            auto f = fc.getResult();
            if (! f.loadFileAsData (data) || data.getSize() < 255)
            {
                statusLabel.setText ("Kit B failed to load", juce::dontSendNotification);
                return;
            }
            std::memcpy (breedKit.data(), (const uint8_t*) data.getData() + 8, breedKit.size());
            hasBreedKit = true;
            breedKitLabel.setText (f.getFileName(), juce::dontSendNotification);
        });
}

void CreatePage::doBreed()
{
    if (! hasBreedKit)
    {
        statusLabel.setText ("Load kit B first", juce::dontSendNotification);
        return;
    }
    processor.breedWithKit (breedKit, breedVoiceToggle.getToggleState(), seedFromEditor (breedSeed));
    statusLabel.setText ("Bred with " + breedKitLabel.getText() + ". Lineage depth: "
                             + juce::String (processor.kitHistorySize()),
                         juce::dontSendNotification);
}

void CreatePage::resized()
{
    auto r = getLocalBounds().reduced (10);
    mutateLabel.setBounds (r.removeFromTop (30));
    auto row = r.removeFromTop (26);
    mutateAmount.setBounds (row.removeFromLeft (260));
    row.removeFromLeft (6);
    mutateSeed.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (6);
    mutateButton.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (6);
    undoButton.setBounds (row.removeFromLeft (80));
    r.removeFromTop (14);
    breedLabel.setBounds (r.removeFromTop (30));
    row = r.removeFromTop (26);
    loadBreedButton.setBounds (row.removeFromLeft (170));
    row.removeFromLeft (6);
    breedKitLabel.setBounds (row.removeFromLeft (200));
    row = r.removeFromTop (26);
    breedVoiceToggle.setBounds (row.removeFromLeft (190));
    row.removeFromLeft (6);
    breedSeed.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (6);
    breedButton.setBounds (row.removeFromLeft (90));
    r.removeFromTop (14);
    statusLabel.setBounds (r.removeFromTop (18));
    r.removeFromTop (10);
    patternLabel.setBounds (r.removeFromTop (30));
    row = r.removeFromTop (22);
    patternBox.setBounds (row.removeFromLeft (120));
    row.removeFromLeft (8);
    for (int t = 0; t < 7; ++t)
    {
        densitySliders[t].setBounds (row.removeFromLeft (104));
        row.removeFromLeft (4);
    }
    row = r.removeFromTop (26);
    euclidToggle.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (4);
    genSeed.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (6);
    generateButton.setBounds (row.removeFromLeft (100));
    row.removeFromLeft (14);
    thinKeep.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (4);
    thinButton.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (14);
    rotateAmount.setBounds (row.removeFromLeft (44));
    row.removeFromLeft (4);
    rotateButton.setBounds (row.removeFromLeft (80));
    row.removeFromLeft (14);
    jitterAmount.setBounds (row.removeFromLeft (44));
    row.removeFromLeft (4);
    jitterButton.setBounds (row.removeFromLeft (100));
    row.removeFromLeft (14);
    undoPatternButton.setBounds (row.removeFromLeft (140));
    row = r.removeFromTop (26);
    grooveLoadButton.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (6);
    grooveLabel.setBounds (row.removeFromLeft (220));
    row.removeFromLeft (6);
    grooveStrength.setBounds (row.removeFromLeft (120));
    row.removeFromLeft (6);
    grooveApplyButton.setBounds (row.removeFromLeft (110));
}

//==============================================================================
// The XY pad: drag sets morphX (horizontal) and morphY (vertical, up = 1)
// through the APVTS parameters (the sliders below mirror it via attachments).
class PerformPage::XYPad : public juce::Component
{
public:
    explicit XYPad (ElfDrumLXRProcessor& p) : proc (p) {}
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1d22));
        g.setColour (juce::Colour (0xff3a4a5f));
        g.drawRect (getLocalBounds());
        const float x = proc.getApvts().getRawParameterValue ("morphX")->load();
        const float y = proc.getApvts().getRawParameterValue ("morphY")->load();
        const int px = (int) (x * (float) (getWidth() - 1));
        const int py = (int) ((1.0f - y) * (float) (getHeight() - 1));
        g.setColour (juce::Colour (0xff555f6e));
        g.drawLine (0.0f, (float) py, (float) getWidth(), (float) py);
        g.drawLine ((float) px, 0.0f, (float) px, (float) getHeight());
        g.setColour (juce::Colour (0xffffc873));
        g.fillEllipse ((float) px - 5.0f, (float) py - 5.0f, 10.0f, 10.0f);
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("A ->", 4, getHeight() - 14, 40, 12, juce::Justification::centredLeft);
        g.drawText ("B ^", 4, 2, 40, 12, juce::Justification::centredLeft);
    }
    void mouseDown (const juce::MouseEvent& e) override { setFromMouse (e); }
    void mouseDrag (const juce::MouseEvent& e) override { setFromMouse (e); }

private:
    void setFromMouse (const juce::MouseEvent& e)
    {
        const float x = juce::jlimit (0.0f, 1.0f, (float) e.x / (float) juce::jmax (1, getWidth() - 1));
        const float y = juce::jlimit (0.0f, 1.0f, 1.0f - (float) e.y / (float) juce::jmax (1, getHeight() - 1));
        if (auto* p = proc.getApvts().getParameter ("morphX"))
            p->setValueNotifyingHost (x);
        if (auto* p = proc.getApvts().getParameter ("morphY"))
            p->setValueNotifyingHost (y);
        repaint();
    }
    ElfDrumLXRProcessor& proc;
};

PerformPage::PerformPage (ElfDrumLXRProcessor& proc) : processor (proc)
{
    xyPad = std::make_unique<XYPad> (processor);
    addAndMakeVisible (*xyPad);
    addAndMakeVisible (xyOnToggle);
    xyOnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.getApvts(), "morphXY", xyOnToggle);
    for (auto* s : { &xyXSlider, &xyYSlider })
    {
        s->setRange (0.0, 1.0, 0.001);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
        addAndMakeVisible (*s);
    }
    xyXAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getApvts(), "morphX", xyXSlider);
    xyYAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getApvts(), "morphY", xyYSlider);
    xyALabel.setFont (juce::FontOptions (11.0f));
    xyBLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (xyALabel);
    addAndMakeVisible (xyBLabel);
    loadBButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Morph target B", juce::File(), "*.SND;*.snd");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                if (! fc.getResults().isEmpty())
                    processor.loadMorphTarget2File (fc.getResult());
            });
    };
    addAndMakeVisible (loadBButton);

    // --- macros / CTRL-ALL / TEMP (item 6) ---
    macroLabel.setText ("Macros: assign a slot (macro + slot + parameter + depth), then move "
                        "the macro knob. Values are anchored at assignment time.",
                        juce::dontSendNotification);
    macroLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (macroLabel);
    for (int m = 0; m < 4; ++m)
    {
        macroSliders[m].setRange (0.0, 1.0, 0.001);
        macroSliders[m].setValue (processor.getMacroPosition (m), juce::dontSendNotification);
        macroSliders[m].setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 16);
        macroSliders[m].onValueChange = [this, m] { processor.setMacroValue (m, (float) macroSliders[m].getValue()); };
        addAndMakeVisible (macroSliders[m]);
    }
    for (int m = 0; m < 4; ++m) macroSelBox.addItem ("Macro " + juce::String (m + 1), m + 1);
    for (int s = 0; s < 4; ++s) slotSelBox.addItem ("Slot " + juce::String (s + 1), s + 1);
    macroSelBox.setSelectedId (1, juce::dontSendNotification);
    slotSelBox.setSelectedId (1, juce::dontSendNotification);
    macroSelBox.onChange = [this] { refreshMacroEditor(); };
    slotSelBox.onChange = [this] { refreshMacroEditor(); };
    addAndMakeVisible (macroSelBox);
    addAndMakeVisible (slotSelBox);
    macroParamBox.addItem ("(none)", 1);
    for (int i = 0; i < 247; ++i)
        macroParamBox.addItem (elfmeta::kKitParams[i].displayName, i + 2);
    addAndMakeVisible (macroParamBox);
    macroDepth.setRange (-247.0, 247.0, 1.0);
    macroDepth.setValue (64.0, juce::dontSendNotification);
    macroDepth.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible (macroDepth);
    macroAssignButton.onClick = [this]
    {
        const int m = macroSelBox.getSelectedId() - 1;
        const int s = slotSelBox.getSelectedId() - 1;
        const int id = macroParamBox.getSelectedId();
        processor.setMacroAssignment (m, s, id <= 1 ? -1 : id - 2, (int) macroDepth.getValue());
        perfStatus.setText ("Assigned macro " + juce::String (m + 1) + " slot " + juce::String (s + 1),
                            juce::dontSendNotification);
    };
    addAndMakeVisible (macroAssignButton);
    refreshMacroEditor();

    ctrlLabel.setText ("CTRL-ALL: one offset on a parameter's per-voice equivalents; "
                        "releasing restores the kit exactly. TEMP: snapshot jump/return.",
                        juce::dontSendNotification);
    ctrlLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (ctrlLabel);
    for (int i = 0; i < 247; ++i)
        ctrlParamBox.addItem (elfmeta::kKitParams[i].displayName, i + 1);
    ctrlParamBox.setSelectedId (9, juce::dontSendNotification);   // Drum 1 Coarse
    addAndMakeVisible (ctrlParamBox);
    ctrlOffset.setRange (-64.0, 64.0, 1.0);
    ctrlOffset.setValue (0.0, juce::dontSendNotification);
    ctrlOffset.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 16);
    ctrlOffset.onValueChange = [this] { processor.setCtrlAllOffset ((int) ctrlOffset.getValue()); };
    addAndMakeVisible (ctrlOffset);
    ctrlBeginButton.onClick = [this]
    {
        if (ctrlBeginButton.getToggleState())
        {
            processor.beginCtrlAll (ctrlParamBox.getSelectedId() - 1);
            processor.setCtrlAllOffset ((int) ctrlOffset.getValue());
        }
        else
            processor.endCtrlAll();
    };
    addAndMakeVisible (ctrlBeginButton);
    tempStoreButton.onClick = [this]
    {
        processor.tempStore();
        perfStatus.setText ("TEMP stored", juce::dontSendNotification);
    };
    addAndMakeVisible (tempStoreButton);
    tempRecallButton.onClick = [this]
    {
        processor.tempRecall();
        perfStatus.setText ("TEMP recalled", juce::dontSendNotification);
    };
    addAndMakeVisible (tempRecallButton);
    perfStatus.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (perfStatus);

    // --- infinity / 16 Pitches / retro-capture (item 8) ---
    labLabel.setText ("Infinity evolves the kit while the sequencer runs. 16 Pitches plays one "
                      "tonal voice from any note, scale-locked (recordable into patterns via "
                      "retro-capture: play over the running sequencer, then commit).",
                      juce::dontSendNotification);
    labLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (labLabel);
    addAndMakeVisible (infinityToggle);
    infinityAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.getApvts(), "seqInfinity", infinityToggle);
    addAndMakeVisible (pitch16Toggle);
    pitch16Att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.getApvts(), "pitch16", pitch16Toggle);
    for (int v = 0; v < 4; ++v) pitchVoiceBox.addItem (p5VoiceName (v), v + 1);
    addAndMakeVisible (pitchVoiceBox);
    pitchVoiceAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.getApvts(), "pitch16Voice", pitchVoiceBox);
    for (auto* n : { "Chromatic", "Major", "Minor", "Major Pent", "Minor Pent" })
        pitchScaleBox.addItem (n, pitchScaleBox.getNumItems() + 1);
    addAndMakeVisible (pitchScaleBox);
    pitchScaleAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.getApvts(), "pitch16Scale", pitchScaleBox);
    for (auto* n : { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" })
        pitchRootBox.addItem (n, pitchRootBox.getNumItems() + 1);
    addAndMakeVisible (pitchRootBox);
    pitchRootAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.getApvts(), "pitch16Root", pitchRootBox);
    for (int i = 0; i < 64; ++i)
        retroPatternBox.addItem ("Pattern " + juce::String (i), i + 1);
    retroPatternBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (retroPatternBox);
    retroCommitButton.onClick = [this]
    {
        const int n = processor.retroCommit (retroPatternBox.getSelectedId() - 1);
        perfStatus.setText ("Retro-capture committed " + juce::String (n) + " step(s)",
                            juce::dontSendNotification);
    };
    addAndMakeVisible (retroCommitButton);
    retroClearButton.onClick = [this] { processor.clearRetro(); };
    addAndMakeVisible (retroClearButton);
    retroLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (retroLabel);
    startTimerHz (4);
}

PerformPage::~PerformPage() { stopTimer(); }

void PerformPage::timerCallback()
{
    xyALabel.setText ("X -> target A: " + processor.getMorphTargetName(), juce::dontSendNotification);
    xyBLabel.setText ("Y -> target B: " + processor.getMorphTarget2Name(), juce::dontSendNotification);
    retroLabel.setText ("Retro-capture: " + juce::String (processor.retroEventCount()) + " hit(s) buffered",
                        juce::dontSendNotification);
    processor.requestSeqSnapshot();   // keep retroCommit's pattern reads fresh
    xyPad->repaint();
}

void PerformPage::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto left = r.removeFromLeft (240);
    xyPad->setBounds (left.removeFromTop (240));
    left.removeFromTop (6);
    xyALabel.setBounds (left.removeFromTop (16));
    xyBLabel.setBounds (left.removeFromTop (16));
    r.removeFromLeft (12);
    auto row = r.removeFromTop (24);
    xyOnToggle.setBounds (row.removeFromLeft (420));
    xyXSlider.setBounds (r.removeFromTop (22).removeFromLeft (300));
    xyYSlider.setBounds (r.removeFromTop (22).removeFromLeft (300));
    r.removeFromTop (4);
    loadBButton.setBounds (r.removeFromTop (24).removeFromLeft (170));
    r.removeFromTop (16);
    macroLabel.setBounds (r.removeFromTop (28));
    row = r.removeFromTop (22);
    for (int m = 0; m < 4; ++m)
    {
        macroSliders[m].setBounds (row.removeFromLeft (150));
        row.removeFromLeft (8);
    }
    row = r.removeFromTop (24);
    macroSelBox.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (4);
    slotSelBox.setBounds (row.removeFromLeft (80));
    row.removeFromLeft (4);
    macroParamBox.setBounds (row.removeFromLeft (210));
    row.removeFromLeft (4);
    macroDepth.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (4);
    macroAssignButton.setBounds (row.removeFromLeft (100));
    r.removeFromTop (14);
    ctrlLabel.setBounds (r.removeFromTop (28));
    row = r.removeFromTop (24);
    ctrlParamBox.setBounds (row.removeFromLeft (210));
    row.removeFromLeft (4);
    ctrlOffset.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (4);
    ctrlBeginButton.setBounds (row.removeFromLeft (250));
    row = r.removeFromTop (26);
    tempStoreButton.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (6);
    tempRecallButton.setBounds (row.removeFromLeft (130));
    r.removeFromTop (8);
    perfStatus.setBounds (r.removeFromTop (18));
    r.removeFromTop (12);
    labLabel.setBounds (r.removeFromTop (30));
    row = r.removeFromTop (24);
    infinityToggle.setBounds (row.removeFromLeft (300));
    row.removeFromLeft (10);
    pitch16Toggle.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (4);
    pitchVoiceBox.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (4);
    pitchScaleBox.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (4);
    pitchRootBox.setBounds (row.removeFromLeft (60));
    row = r.removeFromTop (26);
    retroPatternBox.setBounds (row.removeFromLeft (120));
    row.removeFromLeft (6);
    retroCommitButton.setBounds (row.removeFromLeft (170));
    row.removeFromLeft (6);
    retroClearButton.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (10);
    retroLabel.setBounds (row.removeFromLeft (260));
}

//==============================================================================
// PerformPage: macros / CTRL-ALL / TEMP (item 6).
void PerformPage::refreshMacroEditor()
{
    const int m = macroSelBox.getSelectedId() - 1;
    const int s = slotSelBox.getSelectedId() - 1;
    if (m < 0 || s < 0)
        return;
    const auto a = processor.getMacroAssignment (m, s);
    macroParamBox.setSelectedId (a.param == 0xFF ? 1 : a.param + 2, juce::dontSendNotification);
    macroDepth.setValue (a.depth, juce::dontSendNotification);
}

//==============================================================================
ExportPage::ExportPage (ElfDrumLXRProcessor& proc) : processor (proc)
{
    infoLabel.setText ("Pattern -> MIDI: track = channel, locks become CC / NRPN events, "
                       "probability resolved by the documented deterministic policy (see "
                       "ARCHITECTURE). One-shots render at the engine's native 44,140 Hz. "
                       "The stems bundle needs the host to enable the stem buses.",
                       juce::dontSendNotification);
    infoLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (infoLabel);
    for (int i = 0; i < 64; ++i)
        patternBox.addItem ("Pattern " + juce::String (i), i + 1);
    patternBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (patternBox);
    dragMidiButton.onClick = [this] { dragPatternMidi(); };
    addAndMakeVisible (dragMidiButton);
    saveMidiButton.onClick = [this] { savePatternMidi(); };
    addAndMakeVisible (saveMidiButton);
    oneShotsButton.onClick = [this] { renderOneShots(); };
    addAndMakeVisible (oneShotsButton);
    stemsButton.onClick = [this] { renderStems(); };
    addAndMakeVisible (stemsButton);
    statusLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (statusLabel);
    startTimerHz (2);
}

ExportPage::~ExportPage() { stopTimer(); }

void ExportPage::timerCallback() { processor.requestSeqSnapshot(); }

void ExportPage::savePatternMidi()
{
    seqops::PatternData p;
    const int pat = patternBox.getSelectedId() - 1;
    if (! processor.readPatternData (pat, p))
    {
        statusLabel.setText ("No pattern snapshot yet", juce::dontSendNotification);
        return;
    }
    chooser = std::make_unique<juce::FileChooser> ("Save pattern MIDI",
                                                   juce::File ("pattern-" + juce::String (pat) + ".mid"), "*.mid");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this, p, pat] (const juce::FileChooser& fc)
        {
            if (fc.getResults().isEmpty()) return;
            statusLabel.setText (exportops::writePatternMidi (p, pat, fc.getResult())
                                     ? "Wrote " + fc.getResult().getFileName()
                                     : "MIDI write failed",
                                 juce::dontSendNotification);
        });
}

void ExportPage::dragPatternMidi()
{
    seqops::PatternData p;
    const int pat = patternBox.getSelectedId() - 1;
    if (! processor.readPatternData (pat, p))
    {
        statusLabel.setText ("No pattern snapshot yet", juce::dontSendNotification);
        return;
    }
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("ElfDrumLXR-pattern-" + juce::String (pat) + ".mid");
    if (! exportops::writePatternMidi (p, pat, tmp))
    {
        statusLabel.setText ("MIDI write failed", juce::dontSendNotification);
        return;
    }
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
    {
        container->performExternalDragDropOfFiles ({ tmp.getFullPathName() }, false, this);
        statusLabel.setText ("Dragging pattern " + juce::String (pat) + " as .mid", juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText ("External drag is unavailable in this host — use Save pattern .mid",
                             juce::dontSendNotification);
    }
}

void ExportPage::renderOneShots()
{
    chooser = std::make_unique<juce::FileChooser> ("One-shot output folder", juce::File());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                              | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResults().isEmpty()) return;
            juce::String err;
            statusLabel.setText (exportops::renderOneShots (processor, fc.getResult(), err)
                                     ? "One-shots written to " + fc.getResult().getFileName()
                                     : "One-shot render failed: " + err,
                                 juce::dontSendNotification);
        });
}

void ExportPage::renderStems()
{
    chooser = std::make_unique<juce::FileChooser> ("Stems bundle output folder", juce::File());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                              | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResults().isEmpty()) return;
            juce::String err;
            statusLabel.setText (exportops::renderStemsBundle (processor, fc.getResult(), err)
                                     ? "Stems bundle written to " + fc.getResult().getFileName()
                                     : "Stems render failed: " + err,
                                 juce::dontSendNotification);
        });
}

void ExportPage::resized()
{
    auto r = getLocalBounds().reduced (10);
    infoLabel.setBounds (r.removeFromTop (44));
    auto row = r.removeFromTop (26);
    patternBox.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (6);
    dragMidiButton.setBounds (row.removeFromLeft (170));
    row.removeFromLeft (6);
    saveMidiButton.setBounds (row.removeFromLeft (170));
    r.removeFromTop (10);
    row = r.removeFromTop (26);
    oneShotsButton.setBounds (row.removeFromLeft (250));
    row.removeFromLeft (6);
    stemsButton.setBounds (row.removeFromLeft (250));
    r.removeFromTop (10);
    statusLabel.setBounds (r.removeFromTop (18));
}
