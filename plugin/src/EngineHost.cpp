#include "EngineHost.h"
#include "EngineSwap.h"
#include "CDSPResampler.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

extern "C" {
#include "engine.h"   // Phase 0 wrapper (engine/engine.h)
}

// The engine is one global state machine: init exactly once per process, however many
// plugin instances exist. (Multi-instance state separation is a known Phase 1 limitation,
// inherited from the firmware's globals-everywhere design; see STATUS.md.)
static std::atomic<int> engineUsers { 0 };
static std::once_flag engineInitFlag;

// Phase 4 (5): the instance whose activation guard the calling thread holds.
static thread_local const EngineHost* tlsActiveHost = nullptr;

namespace {
// r8brain settings: 150 dB stop-band and a 2.5% transition band are far beyond what drum
// transients need, and keep the (linear-phase) latency moderate. Measured at prepare().
constexpr double kResamplerTransBand = 2.5;   // percent of Nyquist
constexpr double kResamplerAtten = 150.0;     // dB
constexpr int kBlock = LXR_BLOCK;             // 64 engine frames per render call

r8b::CDSPResampler* makeResampler (double src, double dst)
{
    return new r8b::CDSPResampler (src, dst, kBlock, kResamplerTransBand, kResamplerAtten,
                                   r8b::fprLinearPhase);
}
} // namespace

EngineHost::EngineHost() = default;

EngineHost::~EngineHost()
{
    auto& swap = EngineSwap::instance();
    std::lock_guard<std::mutex> g (swap.mutex);
    if (swap.owner == this) swap.owner = nullptr;   // live globals become unowned
    if (tlsActiveHost == this) tlsActiveHost = nullptr;
}

// --- Phase 4 (5): per-instance activation -----------------------------------
// The swap mutex is held for the whole activation: engine calls from two
// instances can interleave at guard boundaries but never mid-call, and the
// state switch is atomic with respect to every engine touch (they all take
// this guard — directly, or via the processor's processBlock guard).
EngineHost::Activation::~Activation()
{
    if (engaged)
    {
        tlsActiveHost = nullptr;
        EngineSwap::instance().mutex.unlock();
    }
}

EngineHost::Activation EngineHost::activation() const
{
    Activation a;
    if (tlsActiveHost == this) return a;            // nested: already live
    auto& swap = EngineSwap::instance();
    swap.mutex.lock();
    tlsActiveHost = this;
    a.engaged = true;
    if (swap.available() && swap.owner != this)
    {
        if (swap.owner != nullptr && swap.owner->blobValid)
            swap.saveTo (swap.owner->instanceBlob);
        if (blobValid)
            swap.loadFrom (instanceBlob);
        else if (swap.hasPristine())
        {
            instanceBlob = swap.pristine();        // start from post-init state
            swap.loadFrom (instanceBlob);
            blobValid = true;
        }
        else
        {
            // Virgin globals: no instance has ever owned them, so they ARE
            // this instance's (post-init) state. Capture them as pristine
            // for every later instance.
            swap.capturePristineIfNeeded();
        }
        swap.owner = const_cast<EngineHost*> (this);
    }
    return a;
}

void EngineHost::prepare (double hostSampleRate)
{
    hostRate = hostSampleRate;
    std::call_once (engineInitFlag, [] { lxr_init(); });
    auto act = activation();   // my state live (pristine capture on first use)
    ++engineUsers;

    latencySamples = measureLatency();
    for (int c = 0; c < kJacks; ++c)
    {
        resampler[c].reset (makeResampler (kEngineRate, hostRate));
        fifo[c].clear();
    }
    fifoHead = 0;
    // Stems latch here: prepare() is where every stream counter resets, so a
    // stem stream enabled now starts on the same engine frame as the mains.
    stemsEnabled = stemsWanted;
    for (int c = 0; c < kStemChannels; ++c)
    {
        if (stemsEnabled)
            stemResampler[c].reset (makeResampler (kEngineRate, hostRate));
        else
            stemResampler[c].reset();
        stemFifo[c].clear();
    }
    stemFifoHead = 0;
    pendingHits.clear();
    hostPosition = 0;
    engineFrames = 0;
    seqFrameBase = 0;
    seqSystickBase = lxr_seq_systick();
    prepared = true;
}

uint32_t EngineHost::seqSystick() const { auto act = activation(); return lxr_seq_systick(); }

void EngineHost::release()
{
    prepared = false;
    for (int c = 0; c < kJacks; ++c)
    {
        resampler[c].reset();
        fifo[c].clear();
    }
    for (int c = 0; c < kStemChannels; ++c)
    {
        stemResampler[c].reset();
        stemFifo[c].clear();
    }
    stemFifoHead = 0;
    pendingHits.clear();
    if (engineUsers > 0) --engineUsers;
}

int EngineHost::measureLatency()
{
    // Feed an impulse through a scratch resampler with the shipping settings and find the
    // first non-zero output sample. (CDSPResampler::getLatency() returns 0 in r8brain-free-src;
    // the delay lives in the stream position, so measure it.)
    std::unique_ptr<r8b::CDSPResampler> rs (makeResampler (kEngineRate, hostRate));
    double in[kBlock] = {};
    double* op = nullptr;
    uint64_t outPos = 0;
    for (int block = 0; block < 8192; ++block)
    {
        std::fill (std::begin (in), std::end (in), 0.0);
        if (block == 0) in[0] = 1.0;
        const int produced = rs->process (in, kBlock, op);
        for (int i = 0; i < produced; ++i)
            if (std::abs (op[i]) > 1e-9)
                return (int) (outPos + (uint64_t) i);
        outPos += (uint64_t) produced;
    }
    return 0; // passthrough (equal rates) or pathological; treat as no latency
}

void EngineHost::loadKit (const uint8_t params[247]) { auto act = activation(); lxr_load_params (params); }
void EngineHost::editKit (const uint8_t params[247]) { auto act = activation(); lxr_edit_params (params); }
void EngineHost::setFader (int voice, float gain) { auto act = activation(); lxr_set_fader (voice, gain); }

void EngineHost::queueHit (int voice, int note, int velocity, uint64_t atHostSample)
{
    const uint64_t frame = (uint64_t) std::llround ((double) atHostSample * kEngineRate / hostRate);
    pendingHits.push_back ({ frame, voice, note, velocity });
}

void EngineHost::midiBytes (const uint8_t* data, int len) { lxr_midi_bytes (data, len); }

uint8_t EngineHost::midiNote (int voice) const { auto act = activation(); return lxr_midi_note (voice); }
uint8_t EngineHost::midiChannel (int voice) const { auto act = activation(); return lxr_midi_channel (voice); }
uint8_t EngineHost::globalChannel() const { auto act = activation(); return lxr_global_channel(); }

void EngineHost::generateBlock()
{
    auto act = activation();   // the whole block renders under my state
    const uint64_t blockStart = engineFrames;
    const uint64_t blockEnd = blockStart + kBlock;

    // Phase 3: advance the firmware sequencer to this block's end frontier before
    // rendering it, so a step due inside the block sounds in the block (the same
    // relationship soundengine_process() has to its DMA block on hardware: seq_tick()
    // first, then the sample block). On hardware the systick counter free-runs from
    // power-on; here it is pumped in lockstep with generated engine frames.
    {
        const uint32_t target = seqSystickBase
            + (uint32_t) std::llround ((double) (blockEnd - seqFrameBase) * 4000.0 / kEngineRate);
        lxr_seq_pump (target);
    }

    // Fire every hit whose target frame falls inside this block, in time order.
    std::stable_sort (pendingHits.begin(), pendingHits.end(),
                      [] (const Hit& a, const Hit& b) { return a.engineFrame < b.engineFrame; });
    size_t fired = 0;
    for (const auto& h : pendingHits)
    {
        if (h.engineFrame >= blockEnd) break;
        lxr_note (h.voice, (uint8_t) h.note, (uint8_t) h.velocity);
        ++fired;
    }
    pendingHits.erase (pendingHits.begin(), pendingHits.begin() + (std::ptrdiff_t) fired);

    int16_t jack[kJacks][kBlock];
    lxr_render (jack);

    for (int c = 0; c < kJacks; ++c)
    {
        double in[kBlock];
        for (int i = 0; i < kBlock; ++i) in[i] = jack[c][i] * (1.0 / 32768.0);
        double* op = nullptr;
        const int produced = resampler[c]->process (in, kBlock, op);
        fifo[c].insert (fifo[c].end(), op, op + produced);
    }

    if (stemsEnabled)
    {
        // Patch 04's per-voice contribution buffers, deinterleaved and resampled
        // through the same r8brain settings as the mains (same stream latency).
        for (int v = 0; v < kStems; ++v)
        {
            const int16_t* stem = lxr_stem_data (v);
            for (int c = 0; c < 2; ++c)
            {
                double in[kBlock];
                for (int i = 0; i < kBlock; ++i) in[i] = stem[2 * i + c] * (1.0 / 32768.0);
                double* op = nullptr;
                const int produced = stemResampler[2 * v + c]->process (in, kBlock, op);
                stemFifo[2 * v + c].insert (stemFifo[2 * v + c].end(), op, op + produced);
            }
        }
    }
    engineFrames += kBlock;
}

void EngineHost::render (float* const* out, int numSamples)
{
    if (! prepared)
    {
        for (int c = 0; c < kJacks; ++c) std::fill (out[c], out[c] + numSamples, 0.0f);
        return;
    }

    while ((int) (fifo[0].size() - fifoHead) < numSamples)
        generateBlock();

    for (int c = 0; c < kJacks; ++c)
    {
        const float* src = fifo[c].data() + fifoHead;
        if (masterGain == 1.0f)
            std::copy (src, src + numSamples, out[c]);
        else
            for (int i = 0; i < numSamples; ++i) out[c][i] = src[i] * masterGain;
    }
    fifoHead += (size_t) numSamples;
    hostPosition += (uint64_t) numSamples;

    // Compact occasionally so the vectors don't grow without bound.
    if (fifoHead >= 65536)
    {
        for (int c = 0; c < kJacks; ++c)
            fifo[c].erase (fifo[c].begin(), fifo[c].begin() + (std::ptrdiff_t) fifoHead);
        fifoHead = 0;
    }
}

void EngineHost::renderStems (float* const* out, int numSamples)
{
    if (! prepared || ! stemsEnabled)
    {
        for (int c = 0; c < kStemChannels; ++c) std::fill (out[c], out[c] + numSamples, 0.0f);
        return;
    }

    // render() for this block already generated the engine blocks; the stem
    // FIFOs hold exactly as many samples as the main FIFOs did. Consume in
    // lockstep (same count, same master trim as the mains).
    for (int c = 0; c < kStemChannels; ++c)
    {
        const float* src = stemFifo[c].data() + stemFifoHead;
        if (masterGain == 1.0f)
            std::copy (src, src + numSamples, out[c]);
        else
            for (int i = 0; i < numSamples; ++i) out[c][i] = src[i] * masterGain;
    }
    stemFifoHead += (size_t) numSamples;

    if (stemFifoHead >= 65536)
    {
        for (int c = 0; c < kStemChannels; ++c)
            stemFifo[c].erase (stemFifo[c].begin(), stemFifo[c].begin() + (std::ptrdiff_t) stemFifoHead);
        stemFifoHead = 0;
    }
}
