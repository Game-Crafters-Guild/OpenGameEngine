#pragma once

#include "Mathematics/Vector2.h"

#include <cstddef>
#include <span>
#include <vector>

namespace GameEngine::MarkupECS
{

struct MarkupFootprint;

// One connected piece of a region's area on the ground plane (x, z), in meters: its outer ring,
// counter-clockwise seen from above (positive Mathematics::PolygonDoubledSignedArea), and the
// holes inside it, clockwise. No ring repeats its closing point; no two rings cross.
struct MarkupAreaPiece
{
    std::vector<Mathematics::Vector2> Outer;
    std::vector<std::vector<Mathematics::Vector2>> Holes;
};

// The corners of the ring that stands for a circle footprint (a sphere's) in the area.
inline constexpr std::size_t kFootprintCircleCorners = 32;

// A region's area as its display draws it: (the base ring ∪ the includes) − the excludes, the
// set MarkupRegionArea::Contains tests, a circle footprint taken as its kFootprintCircleCorners-gon
// inscribed. The pieces in no particular order; an island inside a hole is a piece of its own.
// Empty when the excludes cover everything. Exact to a millimeter. The engine's one polygon
// boolean (clipper2).
std::vector<MarkupAreaPiece> CombineRegionArea(std::span<const Mathematics::Vector2> base,
                                               std::span<const MarkupFootprint> includes,
                                               std::span<const MarkupFootprint> excludes);

} // namespace GameEngine::MarkupECS
