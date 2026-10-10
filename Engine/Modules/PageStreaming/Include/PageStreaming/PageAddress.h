#pragma once

#include "Types/Types.h"

namespace GameEngine::PageStreaming
{

/// One page of a page store: a face, a pyramid level and the page's column and row in that level.
///
/// A planar terrain is face 0; a planet's base field has six faces. Level 0 is the finest; level N
/// samples the field every 2^N level-0 samples. X counts pages along +X and Z counts pages along +Z,
/// the axes of the field's rows (rows run along +X, the first row at the smallest Z).
struct PageAddress
{
    uint8 Face = 0;
    uint8 Level = 0;
    uint32 X = 0;
    uint32 Z = 0;

    /// The page one level coarser whose footprint holds this page's footprint.
    PageAddress Parent() const { return PageAddress{Face, static_cast<uint8>(Level + 1u), X / 2u, Z / 2u}; }

    bool operator==(const PageAddress&) const = default;
};

/// The Morton (Z-order) code of a page in its level: the bits of X in the even positions and the
/// bits of Z in the odd ones, so pages that are neighbors on the terrain are near each other in
/// the order. A store lays a level's pages out in this order.
uint64 PageMortonCode(uint32 x, uint32 z);

} // namespace GameEngine::PageStreaming
