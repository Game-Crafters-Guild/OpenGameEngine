#pragma once

// 64-bit GPU sort key for the sorted transparent path (transparency-scale
// S1/S2, design §2.6). Layout mirrors UE's Priority ▸ Distance ▸ MeshId
// bitfield:
//
//   bits [63:48]  priority   (16)  run draw slot (S2 drain): runs are the
//                                   per-(surface×blend) groups, slot-ordered
//                                   coarsely back-to-front, so the sort
//                                   partitions the array into contiguous runs
//   bits [47:16]  depthField (32)  monotonic in view depth, INVERTED so an
//                                   ascending integer sort is back-to-front
//   bits [15:0]   index      (16)  deterministic tiebreak — the DENSE per-view
//                                   record index (0..liveCount), NOT the
//                                   GPUScene instance index
//
// F4 (S1 review rider): a 16-bit tiebreak over GPUScene instance indices
// collides for instances 65536 apart at equal depth (unstable flicker). The
// runtime drain therefore feeds the RECORD index — dense per view, unique by
// construction, and bounded by kSortedTransparentSortCapacity — so every key
// is globally unique and the sorted order is total. The static_assert below
// locks the capacity under the 16-bit field.
//
// 64 bits (not 32) because a 16-bit priority reservation + a 16-bit index
// tiebreak would otherwise eat the depth mantissa and make equal-key order
// frame-nondeterministic. Stored as a uvec2 {Lo, Hi} so the sort compute needs
// no shaderInt64 (compare Hi then Lo). The GLSL mirror of this packing is
// sorted_transparent_drain.comp (ge_MakeSortedTransparentKey) — change both in
// the same commit; the drain device test cross-checks CPU vs GPU packing.

#include <bit>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{

// Element capacity of the single-workgroup GPU sort (shared-memory bitonic).
// Must match kCapacity in sorted_transparent_sort.comp AND
// sorted_transparent_drain.comp (power of two; 2048 * 12 B = 24 KiB shared).
// Views with more visible sorted transparents than this take the CPU-sorted
// drain path (identical draw stream, CPU key order) — see
// RenderServicesSortedTransparent.cpp.
inline constexpr uint32_t kSortedTransparentSortCapacity = 2048u;
static_assert((kSortedTransparentSortCapacity & (kSortedTransparentSortCapacity - 1u)) == 0u,
              "bitonic network requires a power-of-two capacity");
static_assert(kSortedTransparentSortCapacity <= 0x10000u,
              "record-index tiebreak is 16 bits; capacity must fit or keys collide (F4)");

struct SortedTransparentKey
{
    uint32_t Lo;
    uint32_t Hi;
};

// Monotonic IEEE-754 float -> uint32 transform: preserves ordering for all
// finite floats (including negatives) so an unsigned integer compare matches a
// float compare. Flip the sign bit for positives; flip all bits for negatives.
inline uint32_t FloatToSortableUint(float f)
{
    const uint32_t bits = std::bit_cast<uint32_t>(f);
    // Branchless: if sign bit set (negative) flip all bits, else flip only sign.
    const uint32_t mask = (bits & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u;
    return bits ^ mask;
}

// viewDepth = dot(boundingCenter - cameraPos, cameraForward); larger = farther.
// depthField is inverted so a plain ascending sort yields farthest-first
// (back-to-front). priority is the high field (draws with lower priority first);
// indexTiebreak breaks exact-depth ties deterministically.
inline SortedTransparentKey MakeSortedTransparentKey(float viewDepth, uint16_t priority,
                                                     uint16_t indexTiebreak)
{
    const uint32_t depthField = ~FloatToSortableUint(viewDepth); // farther -> smaller
    SortedTransparentKey key{};
    // key64 = (priority << 48) | (depthField << 16) | index
    key.Hi = (static_cast<uint32_t>(priority) << 16) | (depthField >> 16);
    key.Lo = ((depthField & 0xFFFFu) << 16) | static_cast<uint32_t>(indexTiebreak);
    return key;
}

// Strict weak ordering used by the sort (ascending): Hi dominates, Lo tiebreaks.
// Exposed for the CPU cross-check tests against the GPU compare.
inline bool SortedTransparentKeyLess(const SortedTransparentKey& a, const SortedTransparentKey& b)
{
    if (a.Hi != b.Hi)
        return a.Hi < b.Hi;
    return a.Lo < b.Lo;
}

} // namespace GameEngine::Engine::Renderer
