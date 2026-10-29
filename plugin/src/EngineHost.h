#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace r8b { class CDSPResampler; }

// EngineHost owns the native LXR-02 engine (Phase 0 wrapper, engine/engine.h) and the rate
// contract from ARCHITECTURE.md: the engine always runs at its native 44140 Hz in 64-frame
// blocks; the four jack channels are resampled once, here, to the host rate with r8brain.
// All methods are AUDIO-THREAD ONLY (the engine is a single global state machine and is not
// thread-safe). The processor serialises every other thread's requests into this one.
class EngineHost
{
public:
    static constexpr double kEngineRate = 44140.0;
    static constexpr int kJacks = 4;

    EngineHost();
    ~EngineHost();

    // Prepare for playback at the given host rate. Inits the engine on first use.
    void prepare (double hostSampleRate);
    void release();

    // Resampler latency in host (output) samples, measured empirically at prepare().
    int getLatencySamples() const { return latencySamples; }

    // Kit operations (forwarded to the wrapper's single parameter-entry path).
    void loadKit (const uint8_t params[247]);
    void editKit (const uint8_t params[247]);
    // Performance faders (the hardware's performance-page faders, via the
    // engine wrapper's lxr_set_fader) + a host-side master trim applied to
    // the rendered output. Not kit parameters.
    void setFader (int voice, float gain);
    void setMasterGain (float gain) { masterGain = gain; }

    // Queue a voice hit to fire at an absolute host-sample position (>= current position).
    void queueHit (int voice, int note, int velocity, uint64_t atHostSample);

    // Feed raw non-note MIDI bytes to the firmware's own parser (midiParser_parseUartData).
    void midiBytes (const uint8_t* data, int len);

    // The firmware's per-voice MIDI configuration (globals seeded at engine init).
    uint8_t midiNote (int voice) const;
    uint8_t midiChannel (int voice) const;   // 0-based
    uint8_t globalChannel() const;           // as stored: 0 = "all channels"

    // Fill exactly numSamples per jack channel with host-rate audio. out[] = 4 channel pointers.
    void render (float* const* out, int numSamples);

    uint64_t getHostPosition() const { return hostPosition; }

    // Phase 3: the firmware's systick time base, for the processor's transport logic.
    uint32_t seqSystick() const;

private:
    struct Hit { uint64_t engineFrame; int voice, note, velocity; };
    void generateBlock();            // render one 64-frame engine block into the resamplers
    int measureLatency();            // impulse test on a scratch resampler

    double hostRate = 44100.0;
    bool prepared = false;
    int latencySamples = 0;
    float masterGain = 1.0f;         // host-side master trim (1.0 = unity, no-op)

    std::unique_ptr<r8b::CDSPResampler> resampler[kJacks];
    std::vector<float> fifo[kJacks];
    size_t fifoHead = 0;             // shared: all channels produce identical counts

    std::vector<Hit> pendingHits;    // sorted by engineFrame when consumed
    uint64_t hostPosition = 0;       // host samples rendered so far
    uint64_t engineFrames = 0;       // engine frames generated so far

    // Phase 3 sequencer pump anchor: engine frame 0 of this prepare() maps to
    // seqSystickBase on the firmware's free-running systick counter (4000 units/s).
    // The pump target for an engine-frame frontier F is
    //   seqSystickBase + llround((F - seqFrameBase) * 4000 / 44140)
    // — the same rule the gate (b) reference harness uses, which is what makes
    // plugin and reference trigger logs comparable to the unit.
    uint32_t seqSystickBase = 0;
    uint64_t seqFrameBase = 0;
};
