#pragma once

#include "Types/Types.h"

#include <cstdint>
#include <cstring>

// Comparing a layout against values captured on another machine.
//
// Goldens are captured on x86-64 as float bit patterns. The layout runs
// sin/cos/normalize, which libm rounds differently per architecture, and a
// compiler may contract a multiply-add into one FMA on one target and not on
// another, so on arm64 a value computed by textually identical arithmetic lands
// a few ULP away. Bit-equality with a captured value is therefore a property of
// the capture machine, not of the layout: compare in ULPs.
namespace GameEngine::SplineLayout::Tests
{

// The observed cross-architecture spread (1-8 ULP) with headroom. A layout
// change a human could see moves a value by millimetres at least, which is
// thousands of ULPs at any coordinate a fixture reaches.
inline constexpr uint32 kMaxGoldenUlps = 64;

inline uint32 FloatBits(float32 value)
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// How many representable floats lie between two bit patterns. Both are mapped
// onto one ordered integer line first, so +0 and -0 are the same point and a
// pair on opposite sides of zero counts every float between them.
inline uint64 UlpDistance(uint32 aBits, uint32 bBits)
{
    constexpr uint32 kSignBit = 0x80000000u;
    auto ordered = [](uint32 bits) -> int64 {
        const int64 magnitude = static_cast<int64>(bits & ~kSignBit);
        return (bits & kSignBit) != 0u ? -magnitude : magnitude;
    };
    const int64 a = ordered(aBits);
    const int64 b = ordered(bBits);
    return static_cast<uint64>(a > b ? a - b : b - a);
}

} // namespace GameEngine::SplineLayout::Tests
