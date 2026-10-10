#pragma once

#include "Mathematics/Vector2.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace GameEngine::Mathematics
{

// How EarClip tells a degenerate corner from a real one, in the polygon's own units.
struct EarClipTolerances
{
    // Twice the area below which an ear is degenerate (a corner that runs straight on).
    float MinDoubledArea = 1.0e-12f;
    // Points closer than this are one point: they never block an ear.
    float SamePointDistance = 1.0e-5f;
};

namespace EarClipDetail
{

inline float Cross2(const Vector2& a, const Vector2& b) { return a.x * b.y - a.y * b.x; }

// Strictly inside a counter-clockwise triangle: a point on an edge is not.
inline bool StrictlyInsideTriangle(const Vector2& a, const Vector2& b, const Vector2& c, const Vector2& p,
                                   float minDoubledArea)
{
    return Cross2(b - a, p - a) > minDoubledArea && Cross2(c - b, p - b) > minDoubledArea &&
           Cross2(a - c, p - c) > minDoubledArea;
}

inline bool SamePoint(const Vector2& a, const Vector2& b, float distance)
{
    const Vector2 d = a - b;
    return d.x * d.x + d.y * d.y <= distance * distance;
}

// Whether the corner at polygon[i] is an ear: turning left by more than `minTurn` with no other
// polygon point strictly inside the triangle it cuts.
inline bool IsEar(std::span<const Vector2> points, const std::vector<std::uint32_t>& polygon, std::size_t i,
                  float minTurn, const EarClipTolerances& tolerances)
{
    const std::size_t count = polygon.size();
    const Vector2& a = points[polygon[(i + count - 1u) % count]];
    const Vector2& b = points[polygon[i]];
    const Vector2& c = points[polygon[(i + 1u) % count]];
    if (!(Cross2(b - a, c - b) > minTurn))
        return false;
    for (std::size_t k = 0; k < count; ++k)
    {
        const Vector2& p = points[polygon[k]];
        if (SamePoint(p, a, tolerances.SamePointDistance) || SamePoint(p, b, tolerances.SamePointDistance) ||
            SamePoint(p, c, tolerances.SamePointDistance))
            continue;
        if (StrictlyInsideTriangle(a, b, c, p, tolerances.MinDoubledArea))
            return false;
    }
    return true;
}

} // namespace EarClipDetail

// Ear-clips the counter-clockwise polygon `polygon` (indices into `points`, +y up) into
// counter-clockwise triangles appended to `out` as index triples: a polygon of n corners gives
// n - 2 triangles. The engine's one polygon triangulator.
//
// A corner where the outline runs straight on is clipped as a flat triangle once no true ear
// remains, rather than dropped, so every outline edge stays an edge of a triangle. A polygon
// that folds over itself (no ear clips) has its first corner clipped anyway so the loop ends; the
// fold has no area to cover. A hole is spliced in first (BridgeHole). O(n^3)
// at worst: meant for outlines of a few hundred corners.
inline void EarClip(std::span<const Vector2> points, std::vector<std::uint32_t> polygon,
                    std::vector<std::uint32_t>& out, const EarClipTolerances& tolerances = {})
{
    while (polygon.size() > 3u)
    {
        const std::size_t count = polygon.size();
        std::size_t ear = count;
        for (const float minTurn : {tolerances.MinDoubledArea, -tolerances.MinDoubledArea})
        {
            for (std::size_t i = 0; i < count && ear == count; ++i)
            {
                if (EarClipDetail::IsEar(points, polygon, i, minTurn, tolerances))
                    ear = i;
            }
            if (ear != count)
                break;
        }
        if (ear == count)
            ear = 0;
        out.insert(out.end(), {polygon[(ear + count - 1u) % count], polygon[ear], polygon[(ear + 1u) % count]});
        polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(ear));
    }
    if (polygon.size() == 3u)
        out.insert(out.end(), {polygon[0], polygon[1], polygon[2]});
}

// Splices the clockwise `hole` (indices into `points`) into the counter-clockwise `outline` it
// lies inside as a zero-width bridge, through the outline corner the hole's rightmost corner can
// see (Eberly's bridge), so EarClip triangulates the outline around the hole. Bridge several
// holes rightmost first. A hole no ray to its right meets the outline from is left out.
inline void BridgeHole(std::span<const Vector2> points, std::vector<std::uint32_t>& outline,
                       std::span<const std::uint32_t> hole, const EarClipTolerances& tolerances = {})
{
    std::size_t holeStart = 0;
    for (std::size_t i = 1; i < hole.size(); ++i)
    {
        if (points[hole[i]].x > points[hole[holeStart]].x)
            holeStart = i;
    }
    const Vector2 m = points[hole[holeStart]];

    // The nearest outline edge the ray from M towards +x crosses.
    float nearestX = std::numeric_limits<float>::max();
    std::size_t hitEdge = outline.size();
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        const Vector2& a = points[outline[i]];
        const Vector2& b = points[outline[(i + 1u) % outline.size()]];
        const bool crosses = (a.y <= m.y && b.y >= m.y) || (a.y >= m.y && b.y <= m.y);
        if (!crosses || a.y == b.y)
            continue;
        const float x = a.x + (m.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x >= m.x && x < nearestX)
        {
            nearestX = x;
            hitEdge = i;
        }
    }
    if (hitEdge == outline.size())
        return;

    const std::size_t edgeA = hitEdge;
    const std::size_t edgeB = (hitEdge + 1u) % outline.size();
    std::size_t visible = points[outline[edgeA]].x > points[outline[edgeB]].x ? edgeA : edgeB;
    const Vector2 hit(nearestX, m.y);
    // A reflex outline corner inside the triangle M, hit, candidate blocks the view; the one
    // nearest the ray's direction is then the visible one.
    const Vector2 candidate = points[outline[visible]];
    float bestAngle = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        if (i == visible)
            continue;
        const Vector2& p = points[outline[i]];
        const Vector2& previous = points[outline[(i + outline.size() - 1u) % outline.size()]];
        const Vector2& next = points[outline[(i + 1u) % outline.size()]];
        const bool reflex = EarClipDetail::Cross2(p - previous, next - p) <= 0.0f;
        if (!reflex)
            continue;
        if (!EarClipDetail::StrictlyInsideTriangle(m, hit, candidate, p, tolerances.MinDoubledArea) &&
            !EarClipDetail::StrictlyInsideTriangle(m, candidate, hit, p, tolerances.MinDoubledArea))
            continue;
        const Vector2 toP = p - m;
        const float angle = std::abs(std::atan2(toP.y, toP.x));
        if (angle < bestAngle)
        {
            bestAngle = angle;
            visible = i;
        }
    }

    std::vector<std::uint32_t> spliced;
    spliced.reserve(outline.size() + hole.size() + 2u);
    spliced.insert(spliced.end(), outline.begin(), outline.begin() + static_cast<std::ptrdiff_t>(visible) + 1);
    for (std::size_t k = 0; k <= hole.size(); ++k)
        spliced.push_back(hole[(holeStart + k) % hole.size()]);
    spliced.push_back(outline[visible]);
    spliced.insert(spliced.end(), outline.begin() + static_cast<std::ptrdiff_t>(visible) + 1, outline.end());
    outline = std::move(spliced);
}

} // namespace GameEngine::Mathematics
