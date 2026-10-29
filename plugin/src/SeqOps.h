#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include "KitOps.h"   // kitops::Rng

extern "C" {
#include "engine.h"   // LxrStep + lxr_seq_snap_* decoded snapshot access
}

// Phase 5 host-side sequencer operations: pure functions over a decoded copy
// of ONE firmware pattern (PatternData). Nothing here invents a parallel
// sequencer model — PatternData is decoded from, and encoded back into, the
// firmware's own seq_patternSet through the Phase 3 wrapper paths (snapshot
// getters for reads, SeqCmd writes through the processor). All transforms are
// deterministic for a given seed.
namespace seqops {

inline constexpr int kTracks = 7;
inline constexpr int kSteps = 64;

struct PatternData
{
    LxrStep steps[kTracks][kSteps] {};
    bool main[kTracks][kSteps] {};
    uint8_t length[kTracks] {};    // normalised track lengths (stored 0 reads as 16)
    uint8_t shuffle = 0;           // firmware shuffle byte 0..127
    uint8_t kitNr = 0;

    bool operator== (const PatternData& o) const
    {
        if (shuffle != o.shuffle || kitNr != o.kitNr)
            return false;
        for (int t = 0; t < kTracks; ++t)
        {
            if (length[t] != o.length[t])
                return false;
            for (int s = 0; s < kSteps; ++s)
            {
                if (main[t][s] != o.main[t][s])
                    return false;
                const auto& a = steps[t][s];
                const auto& b = o.steps[t][s];
                if (a.volume != b.volume || a.prob != b.prob || a.note != b.note
                    || a.p1nr != b.p1nr || a.p1val != b.p1val
                    || a.p2nr != b.p2nr || a.p2val != b.p2val
                    || a.flam != b.flam || a.shift != b.shift)
                    return false;
            }
        }
        return true;
    }
    bool operator!= (const PatternData& o) const { return ! (*this == o); }
};

// Decode one pattern out of a pattern-set snapshot blob (the bytes
// lxr_seq_patternset_read produces; the processor's snapshot blob is exactly
// that, followed by song data).
inline void decode (const void* patternSetBytes, int pattern, PatternData& out)
{
    for (int t = 0; t < kTracks; ++t)
    {
        out.length[t] = (uint8_t) lxr_seq_snap_track_length (patternSetBytes, pattern, t);
        for (int s = 0; s < kSteps; ++s)
        {
            lxr_seq_snap_get_step (patternSetBytes, pattern, t, s, &out.steps[t][s]);
            out.main[t][s] = lxr_seq_snap_main_step (patternSetBytes, pattern, t, s) != 0;
        }
    }
    out.shuffle = (uint8_t) lxr_seq_snap_shuffle (patternSetBytes, pattern);
    out.kitNr = (uint8_t) lxr_seq_snap_kit_nr (patternSetBytes, pattern);
}

// A cleared step, matching the firmware's own clear defaults
// (seq_resetNote / seq_clearTrack in sequencer.c).
inline LxrStep clearedStep()
{
    LxrStep st {};
    st.volume = 127;               // decoded space: velocity only (wrapper masks bit 7)
    st.prob = 100;
    st.note = 60;
    st.p1nr = st.p2nr = LXR_SEQ_NO_AUTO;
    st.p1val = st.p2val = 0;
    st.flam = 0;
    st.shift = 0;
    return st;
}

inline PatternData clearedPattern()
{
    PatternData p {};
    for (int t = 0; t < kTracks; ++t)
    {
        p.length[t] = 16;
        for (int s = 0; s < kSteps; ++s)
            p.steps[t][s] = clearedStep();
    }
    return p;
}

// Bjorklund Euclidean distribution: `hits` pulses over `len` slots.
inline void euclid (bool* out, int len, int hits)
{
    for (int i = 0; i < len; ++i)
        out[i] = false;
    if (hits <= 0 || len <= 0)
        return;
    if (hits >= len)
    {
        for (int i = 0; i < len; ++i)
            out[i] = true;
        return;
    }
    int bucket = 0;
    for (int i = 0; i < len; ++i)
    {
        bucket += hits;
        if (bucket >= len)
        {
            bucket -= len;
            out[i] = true;
        }
    }
}

// Pattern generator. One 16-step bar is generated per track and tiled across
// the 64 slots (the firmware wraps per-track at length 16 by default, so the
// tiles beyond slot 15 only sound if the user lengthens the track — Remix and
// humanize then differentiate the bars). Density is per track (0..1):
//  - euclidean mode: exactly round(density * 16) hits, Bjorklund-spread;
//  - free mode: each step is an independent coin at the track's density,
//    with step 0 (the downbeat) always on for tracks with density > 0.
// Velocities are drawn 88..127, probability 100, note 60, no locks,
// flam/shift centred. Deterministic for a given seed.
inline PatternData generate (uint32_t seed, const float density[kTracks], bool euclideanMode)
{
    PatternData p = clearedPattern();
    kitops::Rng rng (seed);
    bool bar[kSteps / 4];
    for (int t = 0; t < kTracks; ++t)
    {
        const float d = density[t] < 0.0f ? 0.0f : (density[t] > 1.0f ? 1.0f : density[t]);
        if (euclideanMode)
        {
            euclid (bar, 16, (int) (d * 16.0f + 0.5f));
        }
        else
        {
            for (int s = 0; s < 16; ++s)
                bar[s] = rng.unit() < d;
            if (d > 0.0f)
                bar[0] = true;
        }
        for (int s = 0; s < kSteps; ++s)
        {
            const bool on = bar[s % 16];
            p.main[t][s] = on;
            if (on)
                p.steps[t][s].volume = (uint8_t) (88 + rng.below (40));
        }
    }
    return p;
}

// --- Remix transforms (all in place, all seed-deterministic) ---

// Rotate a track's full 64-slot grid (steps + main flags) by `shift` slots
// (positive = later). track < 0 rotates every track by the same amount.
inline void remixRotate (PatternData& p, int track, int shift)
{
    const int k = ((shift % kSteps) + kSteps) % kSteps;
    if (k == 0)
        return;
    for (int t = 0; t < kTracks; ++t)
    {
        if (track >= 0 && t != track)
            continue;
        LxrStep oldSteps[kSteps];
        bool oldMain[kSteps];
        std::memcpy (oldSteps, p.steps[t], sizeof oldSteps);
        std::memcpy (oldMain, p.main[t], sizeof oldMain);
        for (int s = 0; s < kSteps; ++s)
        {
            p.steps[t][(s + k) % kSteps] = oldSteps[s];
            p.main[t][(s + k) % kSteps] = oldMain[s];
        }
    }
}

// Probability thinning: each active step survives with probability `keep`;
// survivors' prob values are scaled by `keep` too (a step that was certain
// stays likely, a step that was a coin flip gets rarer). Deactivated steps
// keep their data (the firmware main-step bit is what silences them).
inline void remixThin (PatternData& p, uint32_t seed, float keep)
{
    if (keep < 0.0f) keep = 0.0f;
    if (keep > 1.0f) keep = 1.0f;
    kitops::Rng rng (seed);
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kSteps; ++s)
        {
            if (! p.main[t][s])
                continue;
            if (rng.unit() >= keep)
                p.main[t][s] = false;
            else
                p.steps[t][s].prob = (uint8_t) (int) ((float) p.steps[t][s].prob * keep + 0.5f);
        }
}

// Lock jitter: perturb every present parameter lock's value by a uniform
// delta in ±amount (clamped 0..127). Steps without locks are untouched.
inline void remixJitterLocks (PatternData& p, uint32_t seed, int amount)
{
    if (amount <= 0)
        return;
    kitops::Rng rng (seed);
    auto jit = [&rng, amount] (uint8_t v) -> uint8_t
    {
        int nv = (int) v + rng.below (2 * amount + 1) - amount;
        return (uint8_t) (nv < 0 ? 0 : (nv > 127 ? 127 : nv));
    };
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kSteps; ++s)
        {
            if (p.steps[t][s].p1nr != LXR_SEQ_NO_AUTO)
                p.steps[t][s].p1val = jit (p.steps[t][s].p1val);
            if (p.steps[t][s].p2nr != LXR_SEQ_NO_AUTO)
                p.steps[t][s].p2val = jit (p.steps[t][s].p2val);
        }
}

// --- Groove / humanize ---
// A groove template over one 16-step bar: per-step velocity scale and
// micro-timing shift (in firmware shift units, -7..+7). Extracted from a
// dropped MIDI file by Phase5.cpp, or built by hand.
struct GrooveTemplate
{
    float velScale[16];
    int shift[16];
    bool valid = false;
    GrooveTemplate() { for (int i = 0; i < 16; ++i) { velScale[i] = 1.0f; shift[i] = 0; } }
};

// Apply a groove at strength 0..1: velocities scale toward the template,
// step shift moves toward the template's shift. Deterministic (no RNG —
// the template carries the feel; `seed` is accepted for signature symmetry
// with the other transforms and intentionally unused).
inline void applyGroove (PatternData& p, const GrooveTemplate& g, float strength)
{
    if (! g.valid || strength <= 0.0f)
        return;
    if (strength > 1.0f)
        strength = 1.0f;
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kSteps; ++s)
        {
            if (! p.main[t][s])
                continue;
            const int bar16 = s % 16;
            const int vel = p.steps[t][s].volume & 0x7f;
            const float target = (float) vel * g.velScale[bar16];
            const int nv = (int) ((float) vel + (target - (float) vel) * strength + 0.5f);
            p.steps[t][s].volume = (uint8_t) (nv < 1 ? 1 : (nv > 127 ? 127 : nv));
            const int sh = (int) ((float) p.steps[t][s].shift
                                  + ((float) g.shift[bar16] - (float) p.steps[t][s].shift) * strength + 0.5f);
            p.steps[t][s].shift = (int8_t) (sh < -7 ? -7 : (sh > 7 ? 7 : sh));
        }
}

} // namespace seqops
