#pragma once
#include <cstdint>
// The firmware's INIT kit (factory/INIT.SND, from the LXR-02 initKit defaults), embedded so the
// plugin and the offline renderer start from the same kit the hardware boots with.
// Generated from factory/INIT.SND (name + 247 parameter bytes) — regenerate with:
//   python3 -c "import sys; d=open('factory/INIT.SND','rb').read(); print(len(d))"  (255 bytes)
namespace initkit {
inline const char name[] = "Initkit";
inline const uint8_t params[247] = {
    0, 0, 0, 0, 1, 0, 4, 0, 31, 63, 41, 63,
    58, 63, 45, 64, 127, 63, 50, 63, 0, 0, 0, 0,
    0, 0, 0, 127, 61, 0, 0, 0, 0, 56, 127, 127,
    127, 127, 127, 127, 127, 68, 104, 0, 0, 0, 0, 0,
    0, 0, 21, 0, 13, 0, 3, 0, 10, 0, 107, 0,
    5, 17, 46, 11, 4, 10, 0, 6, 0, 0, 26, 93,
    7, 14, 44, 14, 127, 81, 22, 10, 14, 15, 0, 0,
    0, 0, 0, 0, 127, 127, 127, 127, 127, 127, 63, 63,
    63, 0, 0, 63, 63, 63, 63, 63, 63, 63, 63, 63,
    127, 127, 127, 127, 127, 127, 127, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
};
} // namespace initkit
