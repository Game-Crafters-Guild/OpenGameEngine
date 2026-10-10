#include "SplineGeometry/SplineProfile.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineGeometry
{
namespace
{

using V2 = Mathematics::Vector2;

// Two profile points closer than this are the same point. Sized well below any
// authorable dimension (a tenth of a millimetre) so a real cross-section is
// never collapsed, while an exactly-degenerate preset is.
constexpr float32 kCoincidentEpsilon = 1.0e-4f;

// Authored floats reach the generators unsanitized. A NaN dimension would
// propagate through every derived point and out into the mesh bounds, where a
// NaN AABB takes culling with it.
[[nodiscard]] float32 FiniteOr(float32 v, float32 fallback)
{
    return std::isfinite(v) ? v : fallback;
}

// Dimensions that name an extent are non-negative by construction; a negative
// one would mirror the profile and invert its winding.
[[nodiscard]] float32 NonNegative(float32 v)
{
    return std::max(0.0f, FiniteOr(v, 0.0f));
}

[[nodiscard]] bool Coincident(const V2& a, const V2& b)
{
    const float32 dx = a.x - b.x;
    const float32 dy = a.y - b.y;
    return (dx * dx + dy * dy) < (kCoincidentEpsilon * kCoincidentEpsilon);
}

// Drop consecutive duplicates, and for a closed loop the wrap pair too. A
// zero-length profile edge has no direction, so it has no normal and adds no V
// -- keeping one would divide by zero in both derivations.
void DropCoincidentPoints(std::vector<SplineProfilePoint>& points, bool closed)
{
    if (points.empty())
        return;

    std::vector<SplineProfilePoint> kept;
    kept.reserve(points.size());
    for (const SplineProfilePoint& p : points)
    {
        if (!std::isfinite(p.Position.x) || !std::isfinite(p.Position.y))
            continue;
        // A dropped duplicate must not take a crease with it: the surviving
        // point stands for both, so it inherits the flag.
        if (!kept.empty() && Coincident(kept.back().Position, p.Position))
        {
            kept.back().Hard = static_cast<uint8>(kept.back().Hard | p.Hard);
            continue;
        }
        kept.push_back(p);
    }
    if (closed && kept.size() >= 2u && Coincident(kept.front().Position, kept.back().Position))
    {
        kept.front().Hard = static_cast<uint8>(kept.front().Hard | kept.back().Hard);
        kept.pop_back();
    }
    points.swap(kept);
}

[[nodiscard]] SplineProfile Finalize(std::vector<SplineProfilePoint> points, bool closed)
{
    DropCoincidentPoints(points, closed);

    SplineProfile profile;
    profile.Points = std::move(points);
    // A "closed" polygon of fewer than three points is a line segment: closing
    // it would emit each edge twice, back to back, and cap a zero area.
    profile.Closed = closed && profile.Points.size() >= 3u;

    float32 widest = 0.0f;
    for (const SplineProfilePoint& p : profile.Points)
        widest = std::max(widest, std::abs(p.Position.x));
    profile.NominalHalfWidth = (widest > kCoincidentEpsilon) ? widest : 1.0f;
    return profile;
}

[[nodiscard]] SplineProfile BuildRectangle(const SplineProfileParams& params)
{
    const float32 halfWidth = NonNegative(params.Width) * 0.5f;
    const float32 height = NonNegative(params.Height);

    // Clockwise in (lateral, vertical): up the left face, right across the top,
    // down the right face, and the wrap edge closes the bottom. Every corner of
    // a wall is a crease, and the base stands on the ground.
    std::vector<SplineProfilePoint> points = {
        {V2{-halfWidth, 0.0f}, 1u, 1u},
        {V2{-halfWidth, height}, 1u, 0u},
        {V2{halfWidth, height}, 1u, 0u},
        {V2{halfWidth, 0.0f}, 1u, 1u},
    };
    return Finalize(std::move(points), /*closed=*/true);
}

[[nodiscard]] SplineProfile BuildBevel(const SplineProfileParams& params)
{
    const float32 halfWidth = NonNegative(params.Width) * 0.5f;
    const float32 edgeDrop = NonNegative(params.EdgeDrop);
    // Past half the width the two inset points would cross and mirror the
    // middle span; at exactly half they coincide and collapse to one.
    const float32 inset = std::min(NonNegative(params.EdgeInset), halfWidth);

    // Left to right, so the walkable span faces up. The outer points are open
    // ends and split their normals without needing a flag; the two inset points
    // are where the surface breaks into the drop, and those are creases.
    std::vector<SplineProfilePoint> points = {
        {V2{-halfWidth, -edgeDrop}, 0u},
        {V2{-halfWidth + inset, 0.0f}, 1u},
        {V2{halfWidth - inset, 0.0f}, 1u},
        {V2{halfWidth, -edgeDrop}, 0u},
    };
    return Finalize(std::move(points), /*closed=*/false);
}

[[nodiscard]] SplineProfile BuildCrown(const SplineProfileParams& params)
{
    const float32 halfWidth = NonNegative(params.Width) * 0.5f;
    const float32 crownRise = NonNegative(params.CrownRise);
    const float32 shoulderWidth = NonNegative(params.ShoulderWidth);
    const float32 shoulderDrop = NonNegative(params.ShoulderDrop);

    // Left to right. The camber apex stays smooth so the carriageway reads as
    // one curved surface; the two shoulder joins are creases.
    std::vector<SplineProfilePoint> points = {
        {V2{-halfWidth - shoulderWidth, -shoulderDrop}, 0u},
        {V2{-halfWidth, 0.0f}, 1u},
        {V2{0.0f, crownRise}, 0u},
        {V2{halfWidth, 0.0f}, 1u},
        {V2{halfWidth + shoulderWidth, -shoulderDrop}, 0u},
    };
    return Finalize(std::move(points), /*closed=*/false);
}

} // namespace

SplineProfile BuildProfile(const SplineProfileParams& params)
{
    switch (params.Shape)
    {
    case SplineProfileShape::Rectangle:
        return BuildRectangle(params);
    case SplineProfileShape::Bevel:
        return BuildBevel(params);
    case SplineProfileShape::Crown:
        return BuildCrown(params);
    case SplineProfileShape::Custom:
        break;
    }
    // Custom carries its points beside the params, not inside them.
    return SplineProfile{};
}

SplineProfile BuildCustomProfile(std::span<const SplineProfilePoint> points, bool closed)
{
    return Finalize(std::vector<SplineProfilePoint>(points.begin(), points.end()), closed);
}

} // namespace GameEngine::SplineGeometry
