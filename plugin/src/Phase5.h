#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cstdint>
#include <vector>
#include "KitOps.h"
#include "SeqOps.h"
#include "VoiceSlice.h"

class ElfDrumLXRProcessor;

// Phase 5 host-side feature layer (see ROADMAP Phase 5). Everything here
// works on the firmware data model — kit bytes, .lxv voice slices, firmware
// pattern data — and reaches the engine only through the processor's existing
// paths (APVTS writes, kit requests, SeqCmds). This file is the shared
// implementation used by both the editor pages and the headless gate harness
// (plugin_phase5_test), so what the harness proves is what the UI does.

namespace voicebank {

// One .lxv voice preset found on disk.
struct Preset
{
    juce::File file;
    int voice = -1;                 // 0..5
    juce::String sourceKitName;     // 8-byte .SND header name stored in the file
};

// The Phase 2 convention: voice presets live in ~/Documents/ElfDrumLXR/voices.
juce::File defaultFolder();

// All parseable .lxv files directly inside `folder`, sorted by voice then
// filename. Unparseable files are skipped (a .lxv whose slice length does not
// match the current metadata is from a different firmware revision).
std::vector<Preset> scan (const juce::File& folder);

// Serialise voice `voice` of `kit` as a .lxv named <name>.lxv in `folder`.
bool save (const juce::File& folder, const juce::String& name, int voice,
           const kitops::Kit& kit, const std::array<char, 8>& sourceKitName,
           juce::File& outFile);

// Parse a .lxv file (thin wrapper over voiceslice::parseLxv for file IO).
bool loadFile (const juce::File& file, voiceslice::LxvData& out);

// Extract one voice's slice bytes from a .SND kit file (255 B or 309 B).
bool sliceFromKitFile (const juce::File& sndFile, int voice,
                       std::vector<uint8_t>& sliceOut,
                       std::array<char, 8>& kitNameOut);

} // namespace voicebank

//==============================================================================
// SD-card project librarian (Phase 5 item 2). An LXR-02 SD card is a tree of
// PROJnn project folders holding .SND kits, .PAT patterns and .SNG songs in
// the firmware's own layouts. The librarian scans such a tree (or any folder
// shaped like one), and packs a project folder into a shareable zip with a
// JSON manifest (or unpacks one back). Import into the *plugin* goes through
// the processor's existing file paths (loadKitFile / seqLoadPatternFile /
// seqLoadSongFile) — this layer only moves files.
namespace cardlib {

struct Project
{
    juce::File dir;
    juce::String name;                     // folder name ("PROJ01", or "(loose files)")
    std::vector<juce::File> kits;          // .SND, sorted by filename
    std::vector<juce::File> patterns;      // .PAT
    std::vector<juce::File> songs;         // .SNG
    bool empty() const { return kits.empty() && patterns.empty() && songs.empty(); }
};

// Scan `root`: every immediate subfolder that is a PROJnn folder or directly
// holds kit/pattern/song files becomes a project; files loose in the root
// itself form a final "(loose files)" project when present.
std::vector<Project> scanCard (const juce::File& root);

// The 8-byte name stored in a .SND header ("" if unreadable).
juce::String readKitName (const juce::File& sndFile);

// Zip `projectDir` into `zipOut` (entries keep the project folder as their
// top level) with a manifest.json describing every file (path/type/name/
// size). Refuses to overwrite nothing — the caller picks the destination.
bool exportPack (const juce::File& projectDir, const juce::File& zipOut,
                 const juce::String& author, juce::String& errorOut);

// Unpack a pack zip into `destRoot`; projectDirOut receives the imported
// project folder. Entries with path traversal are rejected.
bool importPack (const juce::File& zipFile, const juce::File& destRoot,
                 juce::File& projectDirOut, juce::String& errorOut);

} // namespace cardlib

//==============================================================================
// Drag-out export (Phase 5 item 7). The pattern -> MIDI mapping is the
// plugin's own, documented here and in ARCHITECTURE.md:
//   track t -> MIDI channel t+1; step s -> tick s * 24 (tpq = 96, one step =
//   a 16th); note = step.note, velocity = step.volume (decoded 0..127),
//   note length = 12 ticks (half a step).
//   A step is written iff its main-step bit is set AND it passes the
//   deterministic probability policy hash(pattern, track, step) % 100 < prob
//   — the same pattern always exports the same file (probability resolved by
//   a fixed, documented coin, never by a live RNG).
//   Parameter locks become CC events at the step's tick, ahead of the note:
//   nr < 128 -> plain CC nr; nr >= 128 -> NRPN (CC99 = 0, CC98 = nr - 128,
//   CC6 = value), mirroring the firmware's CC/CC2 split the p-locks use.
namespace exportops {

struct ExportedStep
{
    int track = 0, step = 0, note = 60, vel = 127;
    std::vector<std::pair<int, int>> locks;   // (param nr, value)
    bool operator== (const ExportedStep& o) const
    {
        return track == o.track && step == o.step && note == o.note && vel == o.vel
               && locks == o.locks;
    }
};

// The steps a pattern exports, in (track, step) order — the single source of
// truth shared by the MIDI writer and the parse-back gate.
std::vector<ExportedStep> exportedSteps (const seqops::PatternData& p, int patternIndex);

bool patternToMidi (const seqops::PatternData& p, int patternIndex, juce::MidiFile& out);
bool writePatternMidi (const seqops::PatternData& p, int patternIndex, const juce::File& file);
// Parse a file written by writePatternMidi back into its exported step set.
bool parseExportedMidi (const juce::File& file, std::vector<ExportedStep>& out);

// Audio renders. Preconditions: the processor is prepared at 44,140 Hz;
// one-shots use the standard 4-channel layout, the stems bundle needs the
// 8-bus (16-channel) layout with stems enabled (as a multi-out host would
// arrange). Each writes WAV files into `dir` (created if needed).
bool renderOneShots (ElfDrumLXRProcessor& proc, const juce::File& dir, juce::String& errorOut);
bool renderStemsBundle (ElfDrumLXRProcessor& proc, const juce::File& dir, juce::String& errorOut);

} // namespace exportops

//==============================================================================
// Groove extraction (Phase 5 item 8): build a seqops::GrooveTemplate from a
// dropped MIDI file. Note-ons are binned to the nearest 16th of a bar; each
// bin's velocity scale is its mean velocity / 100 and its shift is the mean
// timing deviation, converted to firmware shift units (one step = 24
// prescale ticks; one shift unit = 3 ticks). Bins without notes keep the
// neutral value. Applying the template is seqops::applyGroove.
namespace groove {

bool extractFromMidi (const juce::File& file, seqops::GrooveTemplate& out, juce::String& errorOut);

} // namespace groove
