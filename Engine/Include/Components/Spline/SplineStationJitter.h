#pragma once

#include "Components/Spline/SplinePoolSelection.h"
#include "Types/Types.h"

namespace GameEngine::Components
{

// Which per-station random channel a draw comes from. Channels are salted apart
// so a station's draws are independent of one another: if yaw moved with
// spacing, every piece that slid forward would also turn the same way and the
// result would read as one coherent slither rather than as noise. Values are an
// on-disk-adjacent contract — every authored scene's layout reshuffles if one is
// renumbered, so APPEND ONLY.
enum class SplineJitterChannel : uint32
{
    Spacing = 0,
    Yaw = 1,
    Lateral = 2,
    Dropout = 3,
};

// Lifts the jitter channels clear of SplinePoolRole, which salts the same mix
// with its own small integers (SplinePoolSelection.h). Without the offset,
// channel 0 and role 0 would hash identically for a given seed and station, and
// a station's mesh pick would move in lockstep with its spacing wobble.
inline constexpr uint64 kSplineJitterChannelSalt = 0x5000ull;

// A per-station draw in [0, 1): a pure function of (seed, channel, station
// ordinal) and nothing else — no running RNG, no iteration order, no entity id
// — so a rebuild reproduces the layout exactly and nudging knot N leaves every
// station before it untouched. The station ordinal is the pre-dropout position
// along the spline, so removing a station does not reshuffle its neighbours.
//
// Built on the pinned splitmix64 mix the pool pick uses, so a placement has ONE
// determinism contract rather than two.
constexpr float32 SplineJitterUnit(uint32 seed, SplineJitterChannel channel, uint32 stationIndex)
{
    uint64 h = SplineSelectionMix(seed);
    h = SplineSelectionMix(h ^ (static_cast<uint64>(channel) + kSplineJitterChannelSalt));
    h = SplineSelectionMix(h ^ static_cast<uint64>(stationIndex));
    // Top 24 bits over 2^24: every quotient is exactly representable in float32,
    // so the draw is bit-identical on any toolchain that has IEEE floats.
    return static_cast<float32>(h >> 40) * (1.0f / 16777216.0f);
}

// The same draw mapped to [-1, 1) — the symmetric form the jitter knobs scale,
// so a knob of N metres means "up to N metres either way".
constexpr float32 SplineJitterSigned(uint32 seed, SplineJitterChannel channel, uint32 stationIndex)
{
    return SplineJitterUnit(seed, channel, stationIndex) * 2.0f - 1.0f;
}

} // namespace GameEngine::Components
