#include "SeqEditor.h"
#include "ParamMetadata.generated.h"

namespace {
// addAndMakeVisible over a heterogeneous widget pack (an initializer_list<Component*>
// fails to deduce across JUCE's button/slider class hierarchies).
template <typename... Cs>
void showAll (SeqEditor& owner, Cs&... cs)
{
    (owner.addAndMakeVisible (&cs), ...);
}
} // namespace

//==============================================================================
// The step grid: 7 track rows x 16 steps of the selected bar. Pure view — it
// reads the parent's decoded snapshot view and reports clicks back.
class SeqEditor::Grid : public juce::Component
{
public:
    explicit Grid (SeqEditor& owner) : owner (owner) {}

    void paint (juce::Graphics& g) override
    {
        const int rows = 7, cols = 16;
        const float w = (float) getWidth() / (float) cols;
        const float h = (float) getHeight() / (float) rows;
        g.fillAll (juce::Colour (0xff1a1d21));
        for (int trk = 0; trk < rows; ++trk)
        {
            for (int col = 0; col < cols; ++col)
            {
                const int step = owner.bar * 16 + col;
                auto cell = juce::Rectangle<float> (col * w + 1.0f, trk * h + 1.0f, w - 2.0f, h - 2.0f);
                const bool on = owner.viewMain[trk][step];
                const bool isPlayhead = owner.liveStep[trk] == step && owner.liveRunning;
                const bool beyondLen = step >= owner.viewLength[trk];
                juce::Colour fill = on ? juce::Colour (0xffd78a2e) : juce::Colour (0xff2b2f36);
                if (col % 4 == 0 && ! on) fill = juce::Colour (0xff343a42);
                if (beyondLen) fill = fill.withAlpha (0.35f);
                if (isPlayhead) fill = fill.brighter (0.7f);
                g.setColour (fill);
                g.fillRoundedRectangle (cell, 2.0f);
                if (trk == owner.selTrack && step == owner.selStep)
                {
                    g.setColour (juce::Colours::white);
                    g.drawRoundedRectangle (cell, 2.0f, 1.5f);
                }
                if (on && owner.viewSteps[trk][step].prob < 100)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.55f));
                    g.setFont (9.0f);
                    g.drawText (juce::String (owner.viewSteps[trk][step].prob), cell, juce::Justification::centredBottom);
                }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int col = juce::jlimit (0, 15, (int) ((float) e.x / (float) getWidth() * 16.0f));
        const int trk = juce::jlimit (0, 6, (int) ((float) e.y / (float) getHeight() * 7.0f));
        owner.onCellClicked (trk, owner.bar * 16 + col, ! e.mods.isRightButtonDown());
    }

private:
    SeqEditor& owner;
};

//==============================================================================
SeqEditor::SeqEditor (ElfDrumLXRProcessor& proc) : processor (proc)
{
    grid = std::make_unique<Grid> (*this);
    addAndMakeVisible (grid.get());

    // --- transport row ---
    playAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.getApvts(), "seqPlay", playButton);
    songAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.getApvts(), "seqSongMode", songButton);
    extAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.getApvts(), "seqExtSync", extSyncButton);
    tempoAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getApvts(), "seqTempo", tempoSlider);
    patternPlayAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getApvts(), "seqPattern", patternPlaySlider);
    tempoSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
    tempoSlider.setTextValueSuffix (" BPM");
    patternPlaySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 20);
    showAll (*this, playButton, songButton, extSyncButton, tempoSlider, patternPlaySlider,
             tempoLabel, patternPlayLabel, liveLabel);
    tempoLabel.setText ("Tempo", juce::dontSendNotification);
    patternPlayLabel.setText ("Play pattern", juce::dontSendNotification);

    // --- pattern ops row ---
    for (int p = 0; p < 64; ++p)
    {
        editPatternBox.addItem (juce::String (p).paddedLeft ('0', 2), p + 1);
        songSlotBox.addItem (juce::String (p).paddedLeft ('0', 2), p + 1);
    }
    editPatternBox.setSelectedId (1, juce::dontSendNotification);
    songSlotBox.setSelectedId (1, juce::dontSendNotification);
    followButton.setToggleState (true, juce::dontSendNotification);
    followButton.onClick = [this] { follow = followButton.getToggleState(); };
    editPatternBox.onChange = [this]
    {
        shownPattern = editPatternBox.getSelectedId() - 1;
        processor.requestSeqSnapshot();
        nameEditor.setText (processor.getPatternName (shownPattern), juce::dontSendNotification);
    };
    copyButton.onClick = [this] { copiedPattern = shownPattern; pasteButton.setEnabled (true); };
    pasteButton.onClick = [this]
    {
        if (copiedPattern < 0) return;
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::copyPattern);
        cmd.pattern = (uint8_t) copiedPattern;
        cmd.value = shownPattern;
        processor.sendSeqCmd (cmd);
    };
    pasteButton.setEnabled (false);
    clearButton.onClick = [this]
    {
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::clearPattern);
        cmd.pattern = (uint8_t) shownPattern;
        processor.sendSeqCmd (cmd);
    };
    loadPatButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load pattern (.PAT)", juce::File(), "*.pat;*.PAT");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File()) processor.seqLoadPatternFile (shownPattern, f);
            });
    };
    savePatButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Save pattern (.PAT)", juce::File(), "*.pat;*.PAT");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File()) processor.seqSavePatternFile (shownPattern, f);
            });
    };
    exportButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Export pattern set folder", juce::File());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File()) processor.seqExportSet (f);
            });
    };
    importButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import pattern set folder", juce::File());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File()) processor.seqImportSet (f);
            });
    };
    nameEditor.setText (processor.getPatternName (0), juce::dontSendNotification);
    nameEditor.onReturnKey = [this] { processor.setPatternName (shownPattern, nameEditor.getText()); };
    nameEditor.onFocusLost = [this] { processor.setPatternName (shownPattern, nameEditor.getText()); };
    showAll (*this, editPatternBox, followButton, copyButton, pasteButton, clearButton,
             loadPatButton, savePatButton, exportButton, importButton, nameEditor, kitNrLabel);

    // --- bar buttons + track controls ---
    for (int b = 0; b < 4; ++b)
    {
        barButtons[(size_t) b].setButtonText ("Bar " + juce::String (b + 1));
        barButtons[(size_t) b].onClick = [this, b]
        {
            bar = b;
            for (int i = 0; i < 4; ++i)
                barButtons[(size_t) i].setToggleState (i == b, juce::dontSendNotification);
            grid->repaint();
        };
        addAndMakeVisible (barButtons[(size_t) b]);
    }
    barButtons[0].setToggleState (true, juce::dontSendNotification);
    static const char* trackNames[7] = { "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "HiHat", "Open Hat" };
    for (int t = 0; t < 7; ++t)
    {
        auto& tc = trackCtls[(size_t) t];
        tc.name.setText (trackNames[t], juce::dontSendNotification);
        tc.length.setRange (1, 64, 1);
        tc.length.setValue (16, juce::dontSendNotification);
        tc.length.setTextBoxStyle (juce::Slider::TextBoxRight, false, 30, 18);
        tc.mute.onClick = [this, t]
        {
            viewMute[t] = trackCtls[(size_t) t].mute.getToggleState();
            auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setMute);
            cmd.track = (uint8_t) t;
            cmd.a = viewMute[t] ? 1 : 0;
            processor.sendSeqCmd (cmd);
        };
        tc.roll.onClick = [this, t]
        {
            viewRoll[t] = trackCtls[(size_t) t].roll.getToggleState();
            auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setRoll);
            cmd.track = (uint8_t) t;
            cmd.a = viewRoll[t] ? 1 : 0;
            processor.sendSeqCmd (cmd);
        };
        tc.length.onValueChange = [this, t]
        {
            if (updating) return;
            auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setLength);
            cmd.pattern = (uint8_t) shownPattern;
            cmd.track = (uint8_t) t;
            cmd.value = (int) trackCtls[(size_t) t].length.getValue();
            viewLength[t] = cmd.value;
            processor.sendSeqCmd (cmd);
            grid->repaint();
        };
        addAndMakeVisible (tc.name);
        addAndMakeVisible (tc.mute);
        addAndMakeVisible (tc.roll);
        addAndMakeVisible (tc.length);
    }
    for (int i = 0; i < 8; ++i)
        rollRateBox.addItem ("Roll rate " + juce::String (i + 1), i + 1);
    rollRateBox.setSelectedId (1, juce::dontSendNotification);
    rollRateBox.onChange = [this]
    {
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setRollRate);
        cmd.value = rollRateBox.getSelectedId() - 1;
        processor.sendSeqCmd (cmd);
    };
    rollRateLabel.setText ("Roll rate is global; R arms a track's roll (one-shot when stopped).", juce::dontSendNotification);
    rollRateLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (rollRateBox);
    addAndMakeVisible (rollRateLabel);

    // --- step detail ---
    auto setupSlider = [this] (juce::Slider& s, double lo, double hi)
    {
        s.setRange (lo, hi, 1);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 18);
        s.onValueChange = [this] { if (! updating) sendStepEdit(); };
        addAndMakeVisible (s);
    };
    setupSlider (velSlider, 0, 127);
    setupSlider (noteSlider, 0, 127);
    setupSlider (probSlider, 0, 100);
    setupSlider (flamSlider, 0, 5);
    setupSlider (shiftSlider, -7, 7);
    setupSlider (p1ValSlider, 0, 127);
    setupSlider (p2ValSlider, 0, 127);
    for (auto* box : { &p1Box, &p2Box })
    {
        box->addItem ("Off", 1);
        for (int i = 0; i < elfmeta::kNumKitParams; ++i)
            box->addItem (juce::String (i) + " " + elfmeta::kKitParams[i].displayName, i + 2);
        for (int i = 247; i <= 254; ++i)
            box->addItem ("Param " + juce::String (i), i + 2);
        box->onChange = [this] { if (! updating) sendStepEdit(); };
        addAndMakeVisible (box);
    }
    p1Label.setText ("Lock 1", juce::dontSendNotification);
    p2Label.setText ("Lock 2", juce::dontSendNotification);
    showAll (*this, detailLabel, noteNameLabel, p1Label, p2Label);

    // --- shuffle / euklid / song row ---
    shuffleSlider.setRange (0, 127, 1);
    shuffleSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 18);
    shuffleSlider.onValueChange = [this]
    {
        if (updating) return;
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setShuffle);
        cmd.pattern = (uint8_t) shownPattern;
        cmd.value = (int) shuffleSlider.getValue();
        viewShuffle = cmd.value;
        processor.sendSeqCmd (cmd);
    };
    shuffleLabel.setText ("Shuffle", juce::dontSendNotification);
    eukLenSlider.setRange (1, 16, 1);   eukLenSlider.setValue (16, juce::dontSendNotification);
    eukStepsSlider.setRange (1, 16, 1); eukStepsSlider.setValue (4, juce::dontSendNotification);
    eukRotSlider.setRange (0, 15, 1);
    for (auto* s : { &eukLenSlider, &eukStepsSlider, &eukRotSlider })
    {
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 30, 18);
        addAndMakeVisible (s);
    }
    eukLabel.setText ("Euklid (selected track)", juce::dontSendNotification);
    eukApplyButton.onClick = [this]
    {
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::euklid);
        cmd.pattern = (uint8_t) shownPattern;
        cmd.track = (uint8_t) selTrack;
        cmd.value = (int) eukLenSlider.getValue();
        cmd.a = (uint8_t) eukStepsSlider.getValue();
        cmd.b = (uint8_t) eukRotSlider.getValue();
        processor.sendSeqCmd (cmd);
    };
    songPosSlider.setRange (0, 63, 1);
    songPosSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 30, 18);
    songPosSlider.onValueChange = [this] { if (! updating) refreshSongEntry(); };
    songEntryBox.addItem ("End", 1);
    songEntryBox.addItem ("Loop", 2);
    for (int p = 0; p < 64; ++p)
        songEntryBox.addItem ("Pattern " + juce::String (p).paddedLeft ('0', 2), p + 3);
    songEntryBox.onChange = [this]
    {
        if (updating) return;
        const int id = songEntryBox.getSelectedId();
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setSongEntry);
        cmd.track = (uint8_t) songSlot;                 // track field carries the song slot
        cmd.a = (uint8_t) songPosSlider.getValue();
        cmd.b = (uint8_t) (id == 1 ? 64 : id == 2 ? 65 : id - 3);
        processor.sendSeqCmd (cmd);
        viewSong[(int) songPosSlider.getValue()] = cmd.b;
    };
    songSlotBox.onChange = [this]
    {
        songSlot = songSlotBox.getSelectedId() - 1;
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setSongSlot);
        cmd.value = songSlot;
        processor.sendSeqCmd (cmd);
        processor.requestSeqSnapshot();
        refreshSongEntry();
    };
    songLabel.setText ("Song", juce::dontSendNotification);
    showAll (*this, shuffleSlider, shuffleLabel, eukLabel, eukApplyButton,
             songPosSlider, songEntryBox, songLabel);

    processor.requestSeqSnapshot();
    startTimerHz (20);
}

SeqEditor::~SeqEditor() { stopTimer(); }

ElfDrumLXRProcessor::SeqCmd SeqEditor::makeCmd (ElfDrumLXRProcessor::SeqCmd::Type type) const
{
    ElfDrumLXRProcessor::SeqCmd cmd;
    cmd.type = type;
    return cmd;
}

//==============================================================================
void SeqEditor::onCellClicked (int track, int step, bool toggle)
{
    if (toggle)
    {
        const bool on = ! viewMain[track][step];
        viewMain[track][step] = on;
        auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setMainStep);
        cmd.pattern = (uint8_t) shownPattern;
        cmd.track = (uint8_t) track;
        cmd.step = (uint8_t) step;
        cmd.a = on ? 1 : 0;
        processor.sendSeqCmd (cmd);
    }
    selectStep (track, step);
    grid->repaint();
}

void SeqEditor::selectStep (int track, int step)
{
    selTrack = track;
    selStep = step;
    refreshDetail();
}

void SeqEditor::refreshDetail()
{
    updating = true;
    const auto& s = viewSteps[selTrack][selStep];
    velSlider.setValue (s.volume, juce::dontSendNotification);
    noteSlider.setValue (s.note, juce::dontSendNotification);
    probSlider.setValue (s.prob, juce::dontSendNotification);
    flamSlider.setValue (s.flam, juce::dontSendNotification);
    shiftSlider.setValue (s.shift, juce::dontSendNotification);
    p1ValSlider.setValue (s.p1val, juce::dontSendNotification);
    p2ValSlider.setValue (s.p2val, juce::dontSendNotification);
    p1Box.setSelectedId (s.p1nr == 0xff ? 1 : s.p1nr + 2, juce::dontSendNotification);
    p2Box.setSelectedId (s.p2nr == 0xff ? 1 : s.p2nr + 2, juce::dontSendNotification);
    noteNameLabel.setText (juce::MidiMessage::getMidiNoteName (s.note, true, true, 3),
                           juce::dontSendNotification);
    detailLabel.setText ("Track " + juce::String (selTrack + 1) + " · Step " + juce::String (selStep + 1)
                             + " (bar " + juce::String (selStep / 16 + 1) + ")",
                         juce::dontSendNotification);
    updating = false;
}

void SeqEditor::sendStepEdit()
{
    LxrStep s;
    s.volume = (uint8_t) velSlider.getValue();
    s.note = (uint8_t) noteSlider.getValue();
    s.prob = (uint8_t) probSlider.getValue();
    s.flam = (uint8_t) flamSlider.getValue();
    s.shift = (int8_t) shiftSlider.getValue();
    s.p1val = (uint8_t) p1ValSlider.getValue();
    s.p2val = (uint8_t) p2ValSlider.getValue();
    const int id1 = p1Box.getSelectedId(), id2 = p2Box.getSelectedId();
    s.p1nr = (uint8_t) (id1 <= 1 ? 0xff : id1 - 2);
    s.p2nr = (uint8_t) (id2 <= 1 ? 0xff : id2 - 2);
    viewSteps[selTrack][selStep] = s;
    auto cmd = makeCmd (ElfDrumLXRProcessor::SeqCmd::setStep);
    cmd.pattern = (uint8_t) shownPattern;
    cmd.track = (uint8_t) selTrack;
    cmd.step = (uint8_t) selStep;
    cmd.stepData = s;
    processor.sendSeqCmd (cmd);
    noteNameLabel.setText (juce::MidiMessage::getMidiNoteName (s.note, true, true, 3),
                           juce::dontSendNotification);
    grid->repaint();
}

void SeqEditor::refreshSongEntry()
{
    updating = true;
    const int pos = (int) songPosSlider.getValue();
    const int entry = viewSong[pos];
    songEntryBox.setSelectedId (entry == 64 ? 1 : entry == 65 ? 2 : entry + 3,
                                juce::dontSendNotification);
    updating = false;
}

//==============================================================================
void SeqEditor::rebuildFromSnapshot()
{
    if (! haveSnap)
        return;
    const uint8_t* set = snapBlob.data();
    const size_t psSize = lxr_seq_patternset_size();
    for (int trk = 0; trk < 7; ++trk)
    {
        viewLength[trk] = lxr_seq_snap_track_length (set, shownPattern, trk);
        for (int st = 0; st < 64; ++st)
        {
            lxr_seq_snap_get_step (set, shownPattern, trk, st, &viewSteps[trk][st]);
            viewMain[trk][st] = lxr_seq_snap_main_step (set, shownPattern, trk, st) != 0;
        }
    }
    viewShuffle = lxr_seq_snap_shuffle (set, shownPattern);
    viewKitNr = lxr_seq_snap_kit_nr (set, shownPattern);
    if (snapBlob.size() >= psSize + (size_t) (songSlot * 64 + 64))
        std::memcpy (viewSong, snapBlob.data() + psSize + (size_t) songSlot * 64, 64);

    updating = true;
    for (int t = 0; t < 7; ++t)
        trackCtls[(size_t) t].length.setValue (viewLength[t], juce::dontSendNotification);
    shuffleSlider.setValue (viewShuffle, juce::dontSendNotification);
    kitNrLabel.setText ("Pattern kit: " + juce::String (viewKitNr) + " (informational — patterns don't load kits)",
                        juce::dontSendNotification);
    updating = false;
    refreshDetail();
    refreshSongEntry();
    grid->repaint();
}

void SeqEditor::timerCallback()
{
    const auto live = processor.getSeqLive();
    for (int t = 0; t < 7; ++t)
        liveStep[t] = live.stepIndex[t];
    liveRunning = live.running != 0;

    juce::String mode;
    if (live.extSync) mode = "Ext sync (MIDI clock)";
    else if (live.dawActive) mode = live.running ? "DAW playing" : "DAW stopped";
    else mode = live.running ? "Internal playing" : "Internal stopped";
    liveLabel.setText (mode + " · pattern " + juce::String (live.activePattern).paddedLeft ('0', 2)
                           + (live.pendingPattern != live.activePattern
                                  ? " -> " + juce::String (live.pendingPattern).paddedLeft ('0', 2) : "")
                           + " · " + juce::String (live.bpm) + " BPM"
                           + (live.songMode ? " · song pos " + juce::String (live.songPos) : ""),
                       juce::dontSendNotification);

    if (follow && live.activePattern != shownPattern)
    {
        shownPattern = live.activePattern;
        editPatternBox.setSelectedId (shownPattern + 1, juce::dontSendNotification);
        nameEditor.setText (processor.getPatternName (shownPattern), juce::dontSendNotification);
        processor.requestSeqSnapshot();
    }

    if ((tickCount++ & 3) == 0)   // data sync at 5 Hz; playhead repaint every tick
    {
        std::vector<uint8_t> blob;
        uint32_t ver = 0;
        processor.requestSeqSnapshot();
        if (processor.copySeqSnapshot (blob, ver) && ver != snapVersion)
        {
            snapBlob = std::move (blob);
            snapVersion = ver;
            haveSnap = true;
            rebuildFromSnapshot();
        }
    }
    grid->repaint();
}

//==============================================================================
void SeqEditor::resized()
{
    auto area = getLocalBounds().reduced (6);
    const int rowH = 30;

    auto transport = area.removeFromTop (rowH);
    playButton.setBounds (transport.removeFromLeft (70));
    transport.removeFromLeft (6);
    songButton.setBounds (transport.removeFromLeft (70));
    transport.removeFromLeft (6);
    extSyncButton.setBounds (transport.removeFromLeft (86));
    transport.removeFromLeft (12);
    tempoLabel.setBounds (transport.removeFromLeft (46));
    tempoSlider.setBounds (transport.removeFromLeft (170));
    transport.removeFromLeft (12);
    patternPlayLabel.setBounds (transport.removeFromLeft (76));
    patternPlaySlider.setBounds (transport.removeFromLeft (150));
    transport.removeFromLeft (12);
    liveLabel.setBounds (transport);
    area.removeFromTop (4);

    auto ops = area.removeFromTop (rowH);
    ops.removeFromLeft (0);
    auto editLbl = ops.removeFromLeft (0); (void) editLbl;
    editPatternBox.setBounds (ops.removeFromLeft (64));
    ops.removeFromLeft (4);
    followButton.setBounds (ops.removeFromLeft (70));
    ops.removeFromLeft (8);
    nameEditor.setBounds (ops.removeFromLeft (110));
    ops.removeFromLeft (8);
    copyButton.setBounds (ops.removeFromLeft (58));
    pasteButton.setBounds (ops.removeFromLeft (58));
    clearButton.setBounds (ops.removeFromLeft (58));
    ops.removeFromLeft (8);
    loadPatButton.setBounds (ops.removeFromLeft (84));
    savePatButton.setBounds (ops.removeFromLeft (84));
    ops.removeFromLeft (8);
    exportButton.setBounds (ops.removeFromLeft (96));
    importButton.setBounds (ops.removeFromLeft (96));
    ops.removeFromLeft (8);
    kitNrLabel.setBounds (ops);
    area.removeFromTop (4);

    auto barRow = area.removeFromTop (24);
    auto barArea = barRow.removeFromRight (4 * 76);
    for (int b = 3; b >= 0; --b)
        barButtons[(size_t) b].setBounds (barArea.removeFromRight (72));
    rollRateBox.setBounds (barRow.removeFromLeft (130));
    barRow.removeFromLeft (6);
    rollRateLabel.setBounds (barRow);
    area.removeFromTop (4);

    const int gridH = 7 * 34;
    auto mainRow = area.removeFromTop (gridH);
    auto headers = mainRow.removeFromLeft (236);
    for (int t = 0; t < 7; ++t)
    {
        auto row = headers.removeFromTop (34);
        auto& tc = trackCtls[(size_t) t];
        tc.name.setBounds (row.removeFromLeft (56));
        tc.mute.setBounds (row.removeFromLeft (30));
        tc.roll.setBounds (row.removeFromLeft (30));
        tc.length.setBounds (row);
    }
    grid->setBounds (mainRow);
    area.removeFromTop (6);

    // step detail: two rows
    auto d1 = area.removeFromTop (26);
    detailLabel.setBounds (d1.removeFromLeft (190));
    d1.removeFromLeft (4);
    velSlider.setBounds (d1.removeFromLeft (120));
    noteSlider.setBounds (d1.removeFromLeft (120));
    noteNameLabel.setBounds (d1.removeFromLeft (44));
    probSlider.setBounds (d1.removeFromLeft (120));
    flamSlider.setBounds (d1.removeFromLeft (110));
    shiftSlider.setBounds (d1.removeFromLeft (110));
    area.removeFromTop (2);
    auto d2 = area.removeFromTop (26);
    p1Label.setBounds (d2.removeFromLeft (44));
    p1Box.setBounds (d2.removeFromLeft (210));
    p1ValSlider.setBounds (d2.removeFromLeft (110));
    d2.removeFromLeft (10);
    p2Label.setBounds (d2.removeFromLeft (44));
    p2Box.setBounds (d2.removeFromLeft (210));
    p2ValSlider.setBounds (d2.removeFromLeft (110));
    area.removeFromTop (6);

    auto bottom = area.removeFromTop (26);
    shuffleLabel.setBounds (bottom.removeFromLeft (52));
    shuffleSlider.setBounds (bottom.removeFromLeft (130));
    bottom.removeFromLeft (14);
    eukLabel.setBounds (bottom.removeFromLeft (130));
    eukLenSlider.setBounds (bottom.removeFromLeft (86));
    eukStepsSlider.setBounds (bottom.removeFromLeft (86));
    eukRotSlider.setBounds (bottom.removeFromLeft (86));
    eukApplyButton.setBounds (bottom.removeFromLeft (96));
    bottom.removeFromLeft (14);
    songLabel.setBounds (bottom.removeFromLeft (36));
    songSlotBox.setBounds (bottom.removeFromLeft (56));
    songPosSlider.setBounds (bottom.removeFromLeft (96));
    songEntryBox.setBounds (bottom.removeFromLeft (110));
}
