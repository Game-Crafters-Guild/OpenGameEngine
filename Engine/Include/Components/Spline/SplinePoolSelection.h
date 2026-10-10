#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <cstddef>

namespace GameEngine::Components
{

// Which pool a pick is drawn from. Salts the selection hash so roles sharing a
// station index draw independently: hash(Seed, role, stationIndex) %
// activePoolSize. Values are an on-disk-adjacent contract — every authored
// scene's picks reshuffle if one is renumbered, so APPEND ONLY.
enum class SplinePoolRole : uint32
{
    // SplinePlacement (surface tiles).
    Straight = 0,
    Curve = 1,
    Scatter = 2,
    // SplineFence (posts and spans).
    Post = 3,
    Span = 4,
    Gate = 5,
    // The crest row's cells. Caps are not a role: the piece that closes a
    // remainder is chosen by the length that fits it, never by a seeded pick.
    Crest = 6,
};

// Active entries are the leading non-empty slots. Entries after the first
// empty slot never select (the inspector compacts on edit; a hole from a
// hand-edited scene is reported by PoolHasHole, not silently reshuffled).
// Templated on the array bound because the recipes size their pools from their
// own kits — tiles at kSplinePoolCapacity, fences at kSplineFencePoolCapacity.
template <std::size_t kCapacity>
inline uint32 ActivePoolCount(const ModelRef (&pool)[kCapacity])
{
    uint32 count = 0;
    while (count < static_cast<uint32>(kCapacity) && !pool[count].IsNull())
        ++count;
    return count;
}

// True when a non-empty slot follows an empty one — authored data the
// selection contract ignores. Inspector validation surfaces it by name.
template <std::size_t kCapacity>
inline bool PoolHasHole(const ModelRef (&pool)[kCapacity])
{
    for (uint32 i = ActivePoolCount(pool); i < static_cast<uint32>(kCapacity); ++i)
    {
        if (!pool[i].IsNull())
            return true;
    }
    return false;
}

// splitmix64 finalizer: the fixed, implementation-independent integer mix the
// selection hash is built from. NOT std::hash — scenes must pick identically
// on every platform and toolchain, so the mix is pinned here (and by golden
// tests) rather than delegated to an unspecified library hash.
constexpr uint64 SplineSelectionMix(uint64 x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// The per-station pool pick: a pure function of (Seed, role, stationIndex) and
// nothing else — no iteration order, no entity id, no running RNG — so every
// rebuild reproduces the same picks and nudging knot N leaves stations before
// it byte-identical. stationIndex is the global tile ordinal along the spline.
// Returns an index in [0, activeCount); activeCount must be >= 1.
constexpr uint32 SplinePoolSelect(uint32 seed, SplinePoolRole role, uint32 stationIndex,
                                  uint32 activeCount)
{
    uint64 h = SplineSelectionMix(seed);
    h = SplineSelectionMix(h ^ static_cast<uint64>(role));
    h = SplineSelectionMix(h ^ static_cast<uint64>(stationIndex));
    return static_cast<uint32>(h % static_cast<uint64>(activeCount));
}

} // namespace GameEngine::Components
