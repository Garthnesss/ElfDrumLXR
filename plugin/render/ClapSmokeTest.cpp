/* clap-smoke-test: a minimal, JUCE-free CLAP host — the Phase 4 CLAP gate.
   dlopen()s a .clap, walks clap_entry -> factory -> plugin, activates it at
   the engine's native 44140 Hz, feeds it the same GM note-on layout
   plugin-render uses (6 hits, 0.6 s apart; the plugin starts on its INIT
   kit), and writes the summed main outputs as a stereo WAV for comparison
   with the VST3-path render (tools/render-compare.py).
   Usage: clap-smoke-test PLUGIN.clap out.wav */
#include <clap/clap.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace {

// --- host callbacks (all no-ops; the wrapper degrades gracefully) -----------
const void* hostGetExtension (const clap_host_t*, const char*) { return nullptr; }
void hostRequestRestart (const clap_host_t*) {}
void hostRequestProcess (const clap_host_t*) {}
void hostRequestCallback (const clap_host_t*) {}

clap_host_t makeHost()
{
    clap_host_t h {};
    h.clap_version = CLAP_VERSION_INIT;
    h.host_data = nullptr;
    h.name = "ElfDrumLXR clap-smoke-test";
    h.vendor = "Elf";
    h.url = "";
    h.version = "0.1.0";
    h.get_extension = hostGetExtension;
    h.request_restart = hostRequestRestart;
    h.request_process = hostRequestProcess;
    h.request_callback = hostRequestCallback;
    return h;
}

// --- event list plumbing ------------------------------------------------------
struct EventList
{
    std::vector<const clap_event_header_t*> events;
    static uint32_t sizeFn (const clap_input_events_t* list)
    {
        return (uint32_t) ((EventList*) list->ctx)->events.size();
    }
    static const clap_event_header_t* getFn (const clap_input_events_t* list, uint32_t index)
    {
        return ((EventList*) list->ctx)->events[index];
    }
    static bool tryPushFn (const clap_output_events_t*, const clap_event_header_t*) { return true; }

    clap_input_events_t inEvents()
    {
        clap_input_events_t e {};
        e.ctx = this; e.size = sizeFn; e.get = getFn;
        return e;
    }
    clap_output_events_t outEvents()
    {
        clap_output_events_t e {};
        e.ctx = this; e.try_push = tryPushFn;
        return e;
    }
};

void addNoteOn (EventList& list, std::vector<clap_event_note_t>& storage,
                uint32_t time, int16_t key)
{
    clap_event_note_t ev {};
    ev.header.size = sizeof (ev);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_NOTE_ON;
    ev.header.flags = 0;
    ev.note_id = -1;
    ev.port_index = 0;
    ev.channel = 0;
    ev.key = key;
    ev.velocity = 1.0;
    storage.push_back (ev);
    list.events.push_back (&storage.back().header);
}

bool writeWav (const char* path, const std::vector<float>& L, const std::vector<float>& R, uint32_t rate)
{
    FILE* f = std::fopen (path, "wb");
    if (! f) return false;
    const uint32_t frames = (uint32_t) L.size();
    const uint32_t bytes = frames * 4;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + bytes); std::fwrite ("WAVEfmt ", 1, 8, f);
    w32 (16); w16 (1); w16 (2); w32 (rate); w32 (rate * 4); w16 (4); w16 (16);
    std::fwrite ("data", 1, 4, f); w32 (bytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c)
        {
            float v = c == 0 ? L[i] : R[i];
            v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
            int16_t s = (int16_t) (v * 32767.0f);
            std::fwrite (&s, 2, 1, f);
        }
    std::fclose (f);
    return true;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: clap-smoke-test PLUGIN.clap out.wav\n"); return 2; }
    const char* clapPath = argv[1];

    void* lib = dlopen (clapPath, RTLD_NOW | RTLD_LOCAL);
    if (! lib) { std::fprintf (stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    auto* entry = (const clap_plugin_entry_t*) dlsym (lib, "clap_entry");
    if (! entry) { std::fprintf (stderr, "clap_entry not exported\n"); return 1; }
    if (! clap_version_is_compatible (entry->clap_version))
    {
        std::fprintf (stderr, "incompatible clap version %u.%u.%u\n",
                      entry->clap_version.major, entry->clap_version.minor, entry->clap_version.revision);
        return 1;
    }
    if (! entry->init (clapPath)) { std::fprintf (stderr, "entry init failed\n"); return 1; }

    auto* factory = (const clap_plugin_factory_t*) entry->get_factory (CLAP_PLUGIN_FACTORY_ID);
    if (! factory) { std::fprintf (stderr, "no plugin factory\n"); return 1; }
    const uint32_t count = factory->get_plugin_count (factory);
    const clap_plugin_descriptor_t* desc = nullptr;
    for (uint32_t i = 0; i < count; ++i)
    {
        auto* d = factory->get_plugin_descriptor (factory, i);
        if (d && std::strcmp (d->id, "com.garth.elfdrumlxr") == 0) desc = d;
    }
    if (! desc) { std::fprintf (stderr, "plugin id not found (%u descriptors)\n", count); return 1; }
    std::printf ("descriptor: %s / %s (features:", desc->name, desc->id);
    for (int i = 0; desc->features && desc->features[i]; ++i) std::printf (" %s", desc->features[i]);
    std::printf (")\n");

    clap_host_t host = makeHost();
    const clap_plugin_t* plugin = factory->create_plugin (factory, &host, desc->id);
    if (! plugin) { std::fprintf (stderr, "create_plugin failed\n"); return 1; }
    if (! plugin->init (plugin)) { std::fprintf (stderr, "plugin init failed\n"); return 1; }

    // Audio ports: build process buffers from what the plugin reports.
    auto* ports = (const clap_plugin_audio_ports_t*) plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS);
    if (! ports) { std::fprintf (stderr, "no audio-ports extension\n"); return 1; }
    const uint32_t numIn = ports->count (plugin, true);
    const uint32_t numOut = ports->count (plugin, false);
    std::printf ("audio ports: %u in, %u out\n", numIn, numOut);
    struct PortBuf { clap_audio_buffer_t buf; std::vector<float> data[8]; float* ptrs[8]; };
    std::vector<PortBuf> inBufs (numIn), outBufs (numOut);
    auto setupPorts = [&] (std::vector<PortBuf>& bufs, bool isInput)
    {
        for (uint32_t p = 0; p < (uint32_t) bufs.size(); ++p)
        {
            clap_audio_port_info_t info {};
            if (! ports->get (plugin, p, isInput, &info)) { std::fprintf (stderr, "port get failed\n"); std::exit (1); }
            auto& pb = bufs[p];
            const uint32_t chs = info.channel_count > 8 ? 8 : info.channel_count;
            for (uint32_t c = 0; c < chs; ++c) { pb.data[c].assign (8192, 0.0f); pb.ptrs[c] = pb.data[c].data(); }
            pb.buf.data32 = pb.ptrs;
            pb.buf.data64 = nullptr;
            pb.buf.channel_count = chs;
            pb.buf.latency = 0;
            pb.buf.constant_mask = 0;
            std::printf ("  %s port %u: '%s' %u ch\n", isInput ? "in" : "out", p, info.name, info.channel_count);
        }
    };
    setupPorts (inBufs, true);
    setupPorts (outBufs, false);

    constexpr double kRate = 44140.0;
    constexpr uint32_t kBlock = 512;
    if (! plugin->activate (plugin, kRate, kBlock, 4096)) { std::fprintf (stderr, "activate failed\n"); return 1; }
    if (! plugin->start_processing (plugin)) { std::fprintf (stderr, "start_processing failed\n"); return 1; }
    std::printf ("latency (reported): %u samples\n",
                 plugin->get_extension (plugin, CLAP_EXT_LATENCY)
                     ? ((const clap_plugin_latency_t*) plugin->get_extension (plugin, CLAP_EXT_LATENCY))->get (plugin)
                     : 0);

    static const int gmNotes[7] = { 36, 45, 50, 38, 39, 42, 46 };
    const uint32_t totalFrames = (uint32_t) (6 * 0.6 * kRate);
    std::vector<float> outL, outR;
    outL.reserve (totalFrames); outR.reserve (totalFrames);
    std::vector<clap_audio_buffer_t> inArr (numIn), outArr (numOut);
    for (uint32_t p = 0; p < numIn; ++p) inArr[p] = inBufs[p].buf;
    for (uint32_t p = 0; p < numOut; ++p) outArr[p] = outBufs[p].buf;

    int nextVoice = 0;
    uint32_t written = 0;
    int64_t steady = 0;
    while (written < totalFrames)
    {
        const uint32_t n = std::min (kBlock, totalFrames - written);
        EventList events;
        std::vector<clap_event_note_t> storage;
        while (nextVoice < 6 && (uint32_t) (nextVoice * 0.6 * kRate) < written + n)
        {
            addNoteOn (events, storage, (uint32_t) (nextVoice * 0.6 * kRate) - written,
                       (int16_t) gmNotes[nextVoice]);
            ++nextVoice;
        }
        auto inEv = events.inEvents();
        auto outEv = events.outEvents();
        clap_process_t proc {};
        proc.steady_time = steady;
        proc.frames_count = n;
        proc.transport = nullptr;
        proc.audio_inputs = inArr.data();
        proc.audio_outputs = outArr.data();
        proc.audio_inputs_count = numIn;
        proc.audio_outputs_count = numOut;
        proc.in_events = &inEv;
        proc.out_events = &outEv;
        const clap_process_status status = plugin->process (plugin, &proc);
        if (status != CLAP_PROCESS_CONTINUE && status != CLAP_PROCESS_CONTINUE_IF_NOT_QUIET)
        {
            std::fprintf (stderr, "process returned %d at frame %u\n", (int) status, written);
            return 1;
        }
        // Sum output ports 0 and 1 (the two hardware pairs) like plugin-render does.
        for (uint32_t i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            for (uint32_t p = 0; p < std::min (numOut, 2u); ++p)
            {
                if (outBufs[p].buf.channel_count >= 1) l += outBufs[p].data[0][i];
                if (outBufs[p].buf.channel_count >= 2) r += outBufs[p].data[1][i];
            }
            outL.push_back (l);
            outR.push_back (r);
        }
        steady += n;
        written += n;
    }

    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    entry->deinit();
    dlclose (lib);

    if (! writeWav (argv[2], outL, outR, (uint32_t) kRate))
    {
        std::fprintf (stderr, "can't write %s\n", argv[2]);
        return 1;
    }
    static const char* names[6] = { "drum1", "drum2", "drum3", "snare", "cymbal", "hihat" };
    const int seg = (int) (0.6 * kRate);
    for (int v = 0; v < 6; ++v)
    {
        float peak = 0.0f;
        for (int i = v * seg; i < (v + 1) * seg && i < (int) outL.size(); ++i)
            peak = std::max (peak, std::max (std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i])));
        std::printf ("  %-7s peak %5d\n", names[v], (int) (peak * 32767.0f));
    }
    std::printf ("%s -> %s (3.6 s at %.0f Hz via CLAP)\n", clapPath, argv[2], kRate);
    return 0;
}
