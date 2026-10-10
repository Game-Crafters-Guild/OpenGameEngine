#pragma once

#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::MarkupECS
{

// A region's outline: its knots in order around the area, on the ground plane (x, z) in
// meters. The bounds are the drape's sample cap: one knot per 117 m on a 30 km boundary.
inline constexpr std::size_t kMinRegionKnots = 3;
inline constexpr std::size_t kMaxRegionKnots = 256;

// The refusal, naming the fix, for knots that cannot be a region's outline: fewer than
// kMinRegionKnots or more than kMaxRegionKnots, two edges that cross or touch (a figure
// eight), or no enclosed area (a mean width, twice the area over the perimeter, under 1 cm).
// Nullopt for a valid outline. Point numbers in the text are
// indices into `knots`.
std::optional<std::string> CheckRegionOutline(std::span<const Mathematics::Vector2> knots);

// The area a simple ring encloses, in square meters.
float32 RegionOutlineArea(std::span<const Mathematics::Vector2> ring);
// The closed ring's length, in meters.
float32 RegionOutlinePerimeter(std::span<const Mathematics::Vector2> ring);

// The distance from `point` to the closed ring's outline, in meters, whichever side it is on.
float32 RegionOutlineDistance(const Mathematics::Vector2& point, std::span<const Mathematics::Vector2> ring);

// The interior point farthest from the outline (its pole of inaccessibility), found to
// 0.5 m: where a region's label and pivot stand. Inside the ring even for a C-shaped or
// crescent outline, whose centroid falls outside. The ring's first point for fewer than
// three points. Bounded work for any ring: at most 64 x 64 seed cells and 100,000 refinements.
Mathematics::Vector2 RegionLabelPoint(std::span<const Mathematics::Vector2> ring);

// The evaluated ring as an agent reads it: simplified by Douglas-Peucker at 0.1 m, the
// tolerance doubled until at most kMaxRegionKnots points remain. A linear ring comes back
// as its knots.
std::vector<Mathematics::Vector2> DecimateRegionRing(std::span<const Mathematics::Vector2> ring);

} // namespace GameEngine::MarkupECS
