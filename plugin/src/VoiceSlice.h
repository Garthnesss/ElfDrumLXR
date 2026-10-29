#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include "ParamMetadata.generated.h"

// Per-voice kit slices (Phase 5 "Kit Blender" groundwork, pulled into Phase 2).
// A voice's parameters are NOT a contiguous range in the kit — the firmware's
// ParamEnums interleaves the voices — so a slice is the index SET the generated
// metadata assigns to the voice, in ascending index order. Slices are taken
// from and applied to whole-kit byte arrays; applying goes through the normal
// APVTS parameter path in the processor (the bridge then CC-replays the edit
// diff into the engine, exactly like a panel edit).
//
// .lxv voice file format (self-describing, blender-ready):
//   bytes 0..3   magic "LXV1"
//   byte  4      voice id (0..5: Drum 1, Drum 2, Drum 3, Snare, Cymbal, HiHat)
//   byte  5      slice length N (metadata slice size for that voice)
//   bytes 6..6+N-1  slice bytes, in the metadata's ascending index order
//   bytes 6+N..6+N+7  source kit name (8 bytes, .SND header convention)
// A file whose N does not match the current metadata slice size for its voice
// is rejected (the metadata is regenerated from the firmware, so a mismatch
// means the file came from a different firmware revision).
namespace voiceslice {

inline const std::vector<int>& indicesForVoice (int voice)
{
    static const std::vector<std::vector<int>> table = []
    {
        std::vector<std::vector<int>> t (6);
        for (const auto& m : elfmeta::kKitParams)
            if (m.voice < 6)
                t[m.voice].push_back ((int) m.index);
        return t;
    }();
    static const std::vector<int> empty;
    return (voice >= 0 && voice < 6) ? table[(size_t) voice] : empty;
}

inline std::vector<uint8_t> extract (const std::array<uint8_t, 247>& kit, int voice)
{
    std::vector<uint8_t> out;
    for (int idx : indicesForVoice (voice))
        out.push_back (kit[(size_t) idx]);
    return out;
}

inline void splice (std::array<uint8_t, 247>& kit, int voice, const uint8_t* sliceBytes)
{
    const auto& idxs = indicesForVoice (voice);
    for (size_t i = 0; i < idxs.size(); ++i)
        kit[(size_t) idxs[i]] = sliceBytes[i];
}

inline constexpr char kLxvMagic[4] = { 'L', 'X', 'V', '1' };
inline constexpr int kLxvHeaderBytes = 6;
inline constexpr int kLxvNameBytes = 8;

inline std::vector<uint8_t> serialiseLxv (int voice, const std::vector<uint8_t>& slice,
                                          const std::array<char, 8>& sourceKitName)
{
    std::vector<uint8_t> out;
    out.insert (out.end(), kLxvMagic, kLxvMagic + 4);
    out.push_back ((uint8_t) voice);
    out.push_back ((uint8_t) slice.size());
    out.insert (out.end(), slice.begin(), slice.end());
    out.insert (out.end(), sourceKitName.begin(), sourceKitName.end());
    return out;
}

struct LxvData
{
    int voice = -1;
    std::vector<uint8_t> slice;
    std::array<char, 8> sourceKitName {};
};

inline bool parseLxv (const uint8_t* data, size_t size, LxvData& out)
{
    if (size < (size_t) (kLxvHeaderBytes + kLxvNameBytes))
        return false;
    if (std::memcmp (data, kLxvMagic, 4) != 0)
        return false;
    const int voice = data[4];
    const size_t n = data[5];
    if (voice < 0 || voice > 5)
        return false;
    if (n != indicesForVoice (voice).size())
        return false;   // slice from a different firmware/metadata revision
    if (size != (size_t) kLxvHeaderBytes + n + (size_t) kLxvNameBytes)
        return false;
    out.voice = voice;
    out.slice.assign (data + kLxvHeaderBytes, data + kLxvHeaderBytes + n);
    std::memcpy (out.sourceKitName.data(), data + kLxvHeaderBytes + n, kLxvNameBytes);
    return true;
}

} // namespace voiceslice
