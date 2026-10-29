#include "Phase5.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"   // export render drivers (triggerVoice / processBlock)

//==============================================================================
namespace voicebank {

juce::File defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("ElfDrumLXR").getChildFile ("voices");
}

std::vector<Preset> scan (const juce::File& folder)
{
    std::vector<Preset> out;
    if (! folder.isDirectory())
        return out;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.lxv"))
    {
        voiceslice::LxvData d;
        if (! loadFile (f, d))
            continue;
        Preset p;
        p.file = f;
        p.voice = d.voice;
        p.sourceKitName = juce::String (d.sourceKitName.data(), 8).trimEnd();
        out.push_back (p);
    }
    std::sort (out.begin(), out.end(), [] (const Preset& a, const Preset& b)
    {
        if (a.voice != b.voice) return a.voice < b.voice;
        return a.file.getFileName() < b.file.getFileName();
    });
    return out;
}

bool save (const juce::File& folder, const juce::String& name, int voice,
           const kitops::Kit& kit, const std::array<char, 8>& sourceKitName,
           juce::File& outFile)
{
    if (voice < 0 || voice > 5)
        return false;
    if (! folder.createDirectory())
        return false;
    const auto slice = voiceslice::extract (kit, voice);
    const auto bytes = voiceslice::serialiseLxv (voice, slice, sourceKitName);
    outFile = folder.getChildFile (juce::File::createLegalFileName (name) + ".lxv");
    return outFile.replaceWithData (bytes.data(), bytes.size());
}

bool loadFile (const juce::File& file, voiceslice::LxvData& out)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data))
        return false;
    return voiceslice::parseLxv ((const uint8_t*) data.getData(), data.getSize(), out);
}

bool sliceFromKitFile (const juce::File& sndFile, int voice,
                       std::vector<uint8_t>& sliceOut,
                       std::array<char, 8>& kitNameOut)
{
    juce::MemoryBlock data;
    if (! sndFile.loadFileAsData (data) || data.getSize() < 255)
        return false;
    kitops::Kit kit;
    std::memcpy (kitNameOut.data(), data.getData(), 8);
    std::memcpy (kit.data(), (const uint8_t*) data.getData() + 8, kit.size());
    sliceOut = voiceslice::extract (kit, voice);
    return true;
}

} // namespace voicebank

//==============================================================================
namespace cardlib {
namespace {

bool hasExt (const juce::File& f, const char* ext)
{
    return f.getFileExtension().compareIgnoreCase (ext) == 0;
}

void collect (const juce::File& dir, Project& p)
{
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false))
    {
        if (hasExt (f, ".snd")) p.kits.push_back (f);
        else if (hasExt (f, ".pat")) p.patterns.push_back (f);
        else if (hasExt (f, ".sng")) p.songs.push_back (f);
    }
    auto byName = [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
    };
    std::sort (p.kits.begin(), p.kits.end(), byName);
    std::sort (p.patterns.begin(), p.patterns.end(), byName);
    std::sort (p.songs.begin(), p.songs.end(), byName);
}

bool isProjectFolderName (const juce::String& name)
{
    // PROJnn (the firmware's project folders); be lenient about case.
    if (name.length() < 5 || ! name.startsWithIgnoreCase ("PROJ"))
        return false;
    return name.substring (4).containsOnly ("0123456789");
}

juce::String fileType (const juce::File& f)
{
    if (hasExt (f, ".snd")) return "kit";
    if (hasExt (f, ".pat")) return "pattern";
    if (hasExt (f, ".sng")) return "song";
    return "file";
}

juce::String storedName (const juce::File& f)
{
    // The firmware file layouts all start with an 8-byte name.
    if (fileType (f) == "file")
        return {};
    juce::FileInputStream in (f);
    if (! in.openedOk())
        return {};
    char buf[8] = {};
    if (in.read (buf, 8) != 8)
        return {};
    return juce::String (buf, 8).trimEnd();
}

} // namespace

std::vector<Project> scanCard (const juce::File& root)
{
    std::vector<Project> out;
    if (! root.isDirectory())
        return out;
    std::vector<juce::File> subdirs;
    for (const auto& f : root.findChildFiles (juce::File::findDirectories, false))
        subdirs.push_back (f);
    std::sort (subdirs.begin(), subdirs.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
    });
    for (const auto& d : subdirs)
    {
        Project p;
        p.dir = d;
        p.name = d.getFileName();
        collect (d, p);
        if (isProjectFolderName (p.name) || ! p.empty())
            out.push_back (p);
    }
    Project loose;
    loose.dir = root;
    loose.name = "(loose files)";
    collect (root, loose);
    if (! loose.empty())
        out.push_back (loose);
    return out;
}

juce::String readKitName (const juce::File& sndFile)
{
    return storedName (sndFile);
}

bool exportPack (const juce::File& projectDir, const juce::File& zipOut,
                 const juce::String& author, juce::String& errorOut)
{
    if (! projectDir.isDirectory())
    {
        errorOut = "project folder not found";
        return false;
    }
    Project p;
    p.dir = projectDir;
    p.name = projectDir.getFileName();
    collect (projectDir, p);
    if (p.empty())
    {
        errorOut = "project folder holds no kits, patterns or songs";
        return false;
    }

    auto* manifest = new juce::DynamicObject();
    manifest->setProperty ("format", "ElfDrumLXR pack v1");
    manifest->setProperty ("project", p.name);
    manifest->setProperty ("author", author);
    manifest->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
    juce::Array<juce::var> files;
    std::vector<juce::File> all;
    for (auto& v : { &p.kits, &p.patterns, &p.songs })
        for (auto& f : *v)
            all.push_back (f);
    for (const auto& f : all)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("path", p.name + "/" + f.getFileName());
        entry->setProperty ("type", fileType (f));
        entry->setProperty ("name", storedName (f));
        entry->setProperty ("size", (juce::int64) f.getSize());
        files.add (juce::var (entry));
    }
    manifest->setProperty ("files", files);

    if (zipOut.existsAsFile())
        zipOut.deleteFile();
    const auto manifestJson = juce::JSON::toString (juce::var (manifest), true);
    juce::ZipFile::Builder builder;
    for (const auto& f : all)
        builder.addFile (f, 6, p.name + "/" + f.getFileName());
    builder.addEntry (new juce::MemoryInputStream (manifestJson.toRawUTF8(),
                                                   (size_t) manifestJson.getNumBytesAsUTF8(),
                                                   false),
                      6, "manifest.json", juce::Time::getCurrentTime());
    juce::FileOutputStream out (zipOut);
    if (! out.openedOk() || ! builder.writeToStream (out, nullptr))
    {
        errorOut = "could not write the pack zip";
        return false;
    }
    out.flush();
    return true;
}

bool importPack (const juce::File& zipFile, const juce::File& destRoot,
                 juce::File& projectDirOut, juce::String& errorOut)
{
    juce::ZipFile zip (zipFile);
    if (zip.getNumEntries() == 0)
    {
        errorOut = "not a readable zip";
        return false;
    }
    if (! destRoot.createDirectory())
    {
        errorOut = "destination folder not writable";
        return false;
    }
    juce::String topFolder;
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto* entry = zip.getEntry (i);
        const auto name = entry->filename;
        if (name.contains ("..") || name.startsWith ("/") || name.startsWith ("\\"))
        {
            errorOut = "pack contains an unsafe path: " + name;
            return false;
        }
        if (name == "manifest.json")
            continue;
        const auto firstSlash = name.indexOf ("/");
        if (firstSlash > 0 && topFolder.isEmpty())
            topFolder = name.substring (0, firstSlash);
        auto target = destRoot.getChildFile (name);
        if (entry->isSymbolicLink || name.endsWith ("/"))
            continue;
        target.getParentDirectory().createDirectory();
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        juce::FileOutputStream fout (target);
        if (in == nullptr || ! fout.openedOk() || ! fout.writeFromInputStream (*in, -1))
        {
            errorOut = "failed writing " + name;
            return false;
        }
        fout.flush();
    }
    if (topFolder.isEmpty())
    {
        errorOut = "pack holds no project folder";
        return false;
    }
    projectDirOut = destRoot.getChildFile (topFolder);
    return projectDirOut.isDirectory();
}

} // namespace cardlib

//==============================================================================
namespace exportops {
namespace {

bool probPasses (int pattern, int track, int step, int prob)
{
    if (prob >= 100) return true;
    if (prob <= 0) return false;
    // Deterministic per-step coin (splitmix-ish hash of the coordinates):
    // stable across runs, machines and exports.
    uint32_t h = (uint32_t) (pattern * 73856093) ^ (uint32_t) (track * 19349663)
               ^ (uint32_t) (step * 83492791) ^ 0x9E3779B9u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (int) (h % 100u) < prob;
}

bool writeWavFile (const juce::File& file, const juce::AudioBuffer<float>& buf, double rate)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (new juce::FileOutputStream (file), rate,
                             (unsigned) buf.getNumChannels(), 16, {}, 0));
    if (writer == nullptr)
        return false;
    return writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

const char* kVoiceFileNames[7] = { "Drum1", "Drum2", "Drum3", "Snare", "Cymbal", "HiHat", "OpenHat" };

} // namespace

std::vector<ExportedStep> exportedSteps (const seqops::PatternData& p, int patternIndex)
{
    std::vector<ExportedStep> out;
    for (int t = 0; t < seqops::kTracks; ++t)
        for (int s = 0; s < seqops::kSteps; ++s)
        {
            if (! p.main[t][s])
                continue;
            const auto& st = p.steps[t][s];
            if (! probPasses (patternIndex, t, s, st.prob))
                continue;
            ExportedStep e;
            e.track = t;
            e.step = s;
            e.note = st.note;
            e.vel = st.volume & 0x7f;
            if (st.p1nr != LXR_SEQ_NO_AUTO) e.locks.push_back ({ st.p1nr, st.p1val });
            if (st.p2nr != LXR_SEQ_NO_AUTO) e.locks.push_back ({ st.p2nr, st.p2val });
            out.push_back (e);
        }
    return out;
}

bool patternToMidi (const seqops::PatternData& p, int patternIndex, juce::MidiFile& out)
{
    out.clear();
    out.setTicksPerQuarterNote (96);
    const auto steps = exportedSteps (p, patternIndex);
    for (int t = 0; t < seqops::kTracks; ++t)
    {
        juce::MidiMessageSequence seq;
        const int chan = t + 1;
        seq.addEvent (juce::MidiMessage::endOfTrack(), 0);
        for (const auto& e : steps)
        {
            if (e.track != t)
                continue;
            const int tick = e.step * 24;
            for (const auto& lock : e.locks)
            {
                if (lock.first < 128)
                {
                    seq.addEvent (juce::MidiMessage::controllerEvent (chan, lock.first, lock.second), tick);
                }
                else
                {
                    seq.addEvent (juce::MidiMessage::controllerEvent (chan, 99, 0), tick);
                    seq.addEvent (juce::MidiMessage::controllerEvent (chan, 98, lock.first - 128), tick);
                    seq.addEvent (juce::MidiMessage::controllerEvent (chan, 6, lock.second), tick);
                }
            }
            seq.addEvent (juce::MidiMessage::noteOn (chan, e.note, (juce::uint8) e.vel), tick);
            seq.addEvent (juce::MidiMessage::noteOff (chan, e.note, (juce::uint8) 0), tick + 12);
        }
        seq.updateMatchedPairs();
        out.addTrack (seq);
    }
    return true;
}

bool writePatternMidi (const seqops::PatternData& p, int patternIndex, const juce::File& file)
{
    juce::MidiFile mf;
    if (! patternToMidi (p, patternIndex, mf))
        return false;
    juce::FileOutputStream out (file);
    if (! out.openedOk())
        return false;
    out.setPosition (0);
    out.truncate();
    mf.writeTo (out);
    out.flush();
    return true;
}

bool parseExportedMidi (const juce::File& file, std::vector<ExportedStep>& out)
{
    out.clear();
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return false;
    juce::MidiFile mf;
    if (! mf.readFrom (in))
        return false;
    if (mf.getTimeFormat() != 96)
        return false;
    for (int tr = 0; tr < mf.getNumTracks(); ++tr)
    {
        const auto* seq = mf.getTrack (tr);
        // Pending CC state at the current tick, resolved when a note lands.
        int nrpnMsb = -1, nrpnLsb = -1;
        std::vector<std::pair<int, int>> pendingLocks;
        int pendingTick = -1;
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            const int tick = (int) msg.getTimeStamp();
            if (tick != pendingTick)
            {
                pendingLocks.clear();
                nrpnMsb = nrpnLsb = -1;
                pendingTick = tick;
            }
            if (msg.isController())
            {
                const int num = msg.getControllerNumber(), val = msg.getControllerValue();
                if (num == 99) nrpnMsb = val;
                else if (num == 98) nrpnLsb = val;
                else if (num == 6 && nrpnMsb >= 0 && nrpnLsb >= 0)
                    pendingLocks.push_back ({ 128 + nrpnLsb, val });
                else if (num != 99 && num != 98)
                    pendingLocks.push_back ({ num, val });
            }
            else if (msg.isNoteOn())
            {
                ExportedStep e;
                e.track = msg.getChannel() - 1;
                e.step = (int) (msg.getTimeStamp() / 24.0 + 0.5);
                e.note = msg.getNoteNumber();
                e.vel = msg.getVelocity();
                e.locks = pendingLocks;
                out.push_back (e);
                pendingLocks.clear();
            }
        }
    }
    std::sort (out.begin(), out.end(), [] (const ExportedStep& a, const ExportedStep& b)
    {
        return a.track != b.track ? a.track < b.track : a.step < b.step;
    });
    return true;
}

bool renderOneShots (ElfDrumLXRProcessor& proc, const juce::File& dir, juce::String& errorOut)
{
    if (! dir.createDirectory())
    {
        errorOut = "cannot create the output folder";
        return false;
    }
    constexpr double rate = 44140.0;
    constexpr int blockSize = 512;
    const int gapBlocks = (int) (0.4 * rate) / blockSize;
    const int shotBlocks = (int) (1.2 * rate) / blockSize;
    juce::MidiBuffer midi;
    for (int v = 0; v < 7; ++v)
    {
        juce::AudioBuffer<float> silence (4, gapBlocks * blockSize);
        silence.clear();
        for (int i = 0; i < gapBlocks; ++i)
        {
            juce::AudioBuffer<float> b (silence.getArrayOfWritePointers(), 4, i * blockSize, blockSize);
            proc.processBlock (b, midi);
        }
        proc.triggerVoice (v);
        juce::AudioBuffer<float> shot (2, shotBlocks * blockSize);
        shot.clear();
        juce::AudioBuffer<float> raw (4, blockSize);
        for (int i = 0; i < shotBlocks; ++i)
        {
            raw.clear();
            proc.processBlock (raw, midi);
            for (int s = 0; s < blockSize; ++s)
            {
                shot.setSample (0, i * blockSize + s,
                                juce::jlimit (-1.0f, 1.0f, raw.getSample (0, s) + raw.getSample (2, s)));
                shot.setSample (1, i * blockSize + s,
                                juce::jlimit (-1.0f, 1.0f, raw.getSample (1, s) + raw.getSample (3, s)));
            }
        }
        if (! writeWavFile (dir.getChildFile (juce::String (kVoiceFileNames[v]) + ".wav"), shot, rate))
        {
            errorOut = "failed writing one-shot WAVs";
            return false;
        }
    }
    return true;
}

bool renderStemsBundle (ElfDrumLXRProcessor& proc, const juce::File& dir, juce::String& errorOut)
{
    if (! proc.isStemsEnabled())
    {
        errorOut = "stems are not enabled in this bus layout (the host must enable the stem buses)";
        return false;
    }
    if (! dir.createDirectory())
    {
        errorOut = "cannot create the output folder";
        return false;
    }
    constexpr double rate = 44140.0;
    constexpr int blockSize = 512;
    constexpr int channels = 16;
    const int totalFrames = (int) (7 * 0.6 * rate) + blockSize;
    juce::AudioBuffer<float> rendered (channels, totalFrames);
    rendered.clear();
    const int gmNotes[7] = { 36, 45, 50, 38, 39, 42, 46 };
    int written = 0, nextVoice = 0;
    while (written < totalFrames)
    {
        const int n = std::min (blockSize, totalFrames - written);
        juce::MidiBuffer midi;
        while (nextVoice < 7 && (int) (nextVoice * 0.6 * rate) < written + n)
        {
            const int offset = (int) (nextVoice * 0.6 * rate) - written;
            midi.addEvent (juce::MidiMessage::noteOn (1, gmNotes[nextVoice], (juce::uint8) 127), offset);
            ++nextVoice;
        }
        juce::AudioBuffer<float> block (rendered.getArrayOfWritePointers(), channels, written, n);
        proc.processBlock (block, midi);
        written += n;
    }
    static const char* names[8] = { "Main", "Out2", "Stem-Drum1", "Stem-Drum2", "Stem-Drum3",
                                    "Stem-Snare", "Stem-Cymbal", "Stem-HiHat" };
    for (int bus = 0; bus < 8; ++bus)
    {
        juce::AudioBuffer<float> stereo (2, totalFrames);
        stereo.copyFrom (0, 0, rendered, bus * 2, 0, totalFrames);
        stereo.copyFrom (1, 0, rendered, bus * 2 + 1, 0, totalFrames);
        if (! writeWavFile (dir.getChildFile (juce::String (names[bus]) + ".wav"), stereo, rate))
        {
            errorOut = "failed writing the stems bundle";
            return false;
        }
    }
    return true;
}

} // namespace exportops

//==============================================================================
namespace groove {

bool extractFromMidi (const juce::File& file, seqops::GrooveTemplate& out, juce::String& errorOut)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
    {
        errorOut = "cannot open the MIDI file";
        return false;
    }
    juce::MidiFile mf;
    if (! mf.readFrom (in))
    {
        errorOut = "not a readable MIDI file";
        return false;
    }
    const int tpq = mf.getTimeFormat();   // > 0: ticks per quarter note; < 0: SMPTE
    if (tpq <= 0)
    {
        errorOut = "MIDI file has no PPQ timebase (SMPTE files are unsupported)";
        return false;
    }
    const double stepTicks = tpq / 4.0;          // one 16th in file ticks
    double velSum[16] = {}, shiftSum[16] = {};
    int count[16] = {};
    for (int tr = 0; tr < mf.getNumTracks(); ++tr)
    {
        const auto* seq = mf.getTrack (tr);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            if (! msg.isNoteOn())
                continue;
            const double pos = msg.getTimeStamp();
            const long nearest = (long) (pos / stepTicks + 0.5);
            const int bin = (int) (nearest % 16);
            const double devTicks = pos - (double) nearest * stepTicks;
            // File ticks -> prescale ticks (24 per step) -> shift units (3 each).
            const double shiftUnits = devTicks * (24.0 / stepTicks) / 3.0;
            velSum[bin] += msg.getVelocity();
            shiftSum[bin] += shiftUnits;
            ++count[bin];
        }
    }
    int total = 0;
    for (int b = 0; b < 16; ++b)
        total += count[b];
    if (total == 0)
    {
        errorOut = "no note-ons in the MIDI file";
        return false;
    }
    out = seqops::GrooveTemplate();
    for (int b = 0; b < 16; ++b)
    {
        if (count[b] == 0)
            continue;
        out.velScale[b] = juce::jlimit (0.25f, 2.0f, (float) (velSum[b] / count[b] / 100.0));
        out.shift[b] = juce::jlimit (-7, 7, (int) (shiftSum[b] / count[b] + (shiftSum[b] >= 0 ? 0.5 : -0.5)));
    }
    out.valid = true;
    return true;
}

} // namespace groove
