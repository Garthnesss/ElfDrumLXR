#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include "ParamMetadata.generated.h"

extern "C" {
#include "engine.h"   // lxr_morph_kit (firmware morph semantics, pure function)
}

// Phase 5 host-side kit operations: pure functions over the 247 kit bytes.
// Everything here works on the firmware data model only — results are applied
// to the engine through the processor's normal APVTS write path, exactly like
// a panel edit or the Phase 2 guarded randomise.
//
// Keep-set philosophy (shared with randomise, documented in STATUS Phase 2):
// parameters flagged kFlagRouting (the six per-voice AUDIO_OUT params +
// PAR_FX_ROUTING) or kFlagModTarget (the twelve mod-target selectors +
// PAR_MORPH_KIT_DEST) are never created or destroyed by mutation/breeding —
// in a breed child they always come from parent A (the "current" kit).
// Level params (kFlagLevel) are floored at 30% of range so a generated kit
// can never be silent by construction.
namespace kitops {

inline constexpr int kNumParams = 247;
using Kit = std::array<uint8_t, kNumParams>;

// Deterministic xorshift32 — same generator family as the engine shim's RNG.
// Seed 0 is remapped (xorshift's fixed point) so any user seed works.
struct Rng
{
    uint32_t s;
    explicit Rng (uint32_t seed) : s (seed != 0 ? seed : 0x9E3779B9u) {}
    uint32_t next()
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    // uniform in [0, n)
    int below (int n) { return n <= 1 ? 0 : (int) (next() % (uint32_t) n); }
    // uniform float in [0, 1)
    float unit() { return (float) (next() >> 8) * (1.0f / 16777216.0f); }
    bool coin() { return (next() & 1u) != 0; }
};

inline bool isKeepSet (int idx)
{
    return (elfmeta::kKitParams[idx].flags & (elfmeta::kFlagRouting | elfmeta::kFlagModTarget)) != 0;
}

inline bool isLevel (int idx)
{
    return (elfmeta::kKitParams[idx].flags & elfmeta::kFlagLevel) != 0;
}

// Mutation/breeding scope: the same scope as whole-kit randomise — the six
// voice slices + the FX page + PAR_VOICE_DECIMATION_ALL (the one
// performance-global that lives in the kit). System/Global params and
// PAR_NONE are out of scope.
inline bool inScope (int idx)
{
    const auto& m = elfmeta::kKitParams[idx];
    return m.voice < 6 || m.page == elfmeta::PageFX || idx == 114;
}

inline uint8_t flooredLevel (const elfmeta::KitParam& m, int v)
{
    const int floor_ = m.minValue + (int) (0.30f * (float) (m.maxValue - m.minValue));
    return (uint8_t) (v < floor_ ? floor_ : v);
}

// Guarded mutation (Patternarium-style): each in-scope, non-keep parameter is
// perturbed with probability == amount, by a delta drawn uniformly from
// ±amount × (full range), clamped to the parameter's range; level params are
// then floored. amount 0 returns the input unchanged; amount 1 re-rolls every
// mutable parameter within a full-range window around its current value.
// Deterministic for a given seed.
inline Kit mutate (const Kit& in, float amount, uint32_t seed)
{
    Kit out = in;
    if (amount <= 0.0f)
        return out;
    if (amount > 1.0f)
        amount = 1.0f;
    Rng rng (seed);
    for (int i = 0; i < kNumParams; ++i)
    {
        if (! inScope (i) || isKeepSet (i))
            continue;
        const auto& m = elfmeta::kKitParams[i];
        if (rng.unit() >= amount)
            continue;
        const int range = m.maxValue - m.minValue;
        if (range <= 0)
            continue;
        const int span = (int) (amount * (float) range);
        const int delta = rng.below (2 * span + 1) - span;
        int v = (int) in[i] + delta;
        if (v < m.minValue) v = m.minValue;
        if (v > m.maxValue) v = m.maxValue;
        out[i] = isLevel (i) ? flooredLevel (m, v) : (uint8_t) v;
    }
    return out;
}

enum class BreedMode { perParameter, perVoice };

// Breed two kits. Keep-set bytes always come from parent A. Out-of-scope
// bytes (System/Global) also come from A — the child is a variation of the
// current kit, not of the system setup. In perParameter mode every mutable
// byte is a coin flip between A and B; in perVoice mode each voice slice and
// the FX page is inherited whole from one parent. Unlike mutate/randomise
// (generative ops, whose written level values are floored), breed applies NO
// floor: every mutable byte in the child is exactly one parent's byte — a
// recombination of two real kits cannot silence a voice below what either
// parent chose. Deterministic for a given seed.
inline Kit breed (const Kit& a, const Kit& b, BreedMode mode, uint32_t seed)
{
    Kit out = a;
    Rng rng (seed);
    if (mode == BreedMode::perParameter)
    {
        for (int i = 0; i < kNumParams; ++i)
        {
            if (! inScope (i) || isKeepSet (i))
                continue;
            out[i] = rng.coin() ? b[i] : a[i];
        }
    }
    else
    {
        // One coin per voice slice (indices grouped by metadata voice),
        // one for the FX page, one for PAR_VOICE_DECIMATION_ALL.
        bool takeB[8] = {};   // voices 0..5, FX page, decimation
        for (int k = 0; k < 8; ++k)
            takeB[k] = rng.coin();
        for (int i = 0; i < kNumParams; ++i)
        {
            if (! inScope (i) || isKeepSet (i))
                continue;
            const auto& m = elfmeta::kKitParams[i];
            const int group = m.voice < 6 ? (int) m.voice : (m.page == elfmeta::PageFX ? 6 : 7);
            if (takeB[group])
                out[i] = b[i];
        }
    }
    return out;
}

// Morph XY composition (Phase 5 item 4). Two firmware morphs in a fixed,
// documented order: first toward target A by x, then from that result toward
// target B by y. Because lxr_morph_kit interpolates exactly at amount 0/255
// and always keeps the *source* kit's keep-set, the corners are exact:
//   (0,0) = base            (1,0) = morph(base, A, 1.0)   [single-target A]
//   (0,1) = morph(base, B, 1.0)   [single-target B]
//   (1,1) = B's morphable bytes over base's keep-set == the (0,1) result.
inline void morphXY (const Kit& base, const Kit& targetA, const Kit& targetB,
                     float x, float y, Kit& out)
{
    Kit stage = base;
    if (x > 0.001f)
        lxr_morph_kit (base.data(), targetA.data(),
                       (uint8_t) (int) (x * 255.0f + 0.5f), stage.data());
    if (y > 0.001f)
        lxr_morph_kit (stage.data(), targetB.data(),
                       (uint8_t) (int) (y * 255.0f + 0.5f), out.data());
    else
        out = stage;
}

//==============================================================================
// 16 Pitches tonal mode (Phase 5 item 8): quantise a played note to a scale.
// Scales by index (the APVTS pitch16Scale values): 0 chromatic, 1 major,
// 2 natural minor, 3 major pentatonic, 4 minor pentatonic. The result is the
// nearest scale tone (ties resolve DOWN), expressed as an absolute MIDI note
// around the played one; the processor turns it into a pitched voice hit
// (note 60 = the kit's own pitch).
inline int quantizeToScale (int note, int root, int scale)
{
    static const uint16_t kMasks[5] = {
        0xFFF,  // chromatic
        0xAB5,  // major:          0 2 4 5 7 9 11      (bits from the root)
        0x5AD,  // natural minor:  0 2 3 5 7 8 10
        0x295,  // major pent:     0 2 4 7 9
        0x4A9   // minor pent:     0 3 5 7 10
    };
    if (scale < 0 || scale > 4)
        scale = 0;
    const uint16_t mask = kMasks[scale];
    auto inScale = [&] (int n)
    {
        int pc = (n - root) % 12;
        if (pc < 0) pc += 12;
        return ((mask >> pc) & 1u) != 0;
    };
    if (inScale (note))
        return note;
    for (int d = 1; d <= 6; ++d)
    {
        if (inScale (note - d))
            return note - d;   // ties resolve down: check the lower neighbour first
        if (inScale (note + d))
            return note + d;
    }
    return note;
}

} // namespace kitops
