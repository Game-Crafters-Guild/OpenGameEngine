#include "MarkupECS/MarkupRegionBoolean.h"

#include "MarkupECS/MarkupRegionArea.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/VectorOps.h"

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>

namespace GameEngine::MarkupECS
{

using Mathematics::Vector2;

namespace
{

// Decimal places the boolean keeps: a millimeter.
constexpr int kBooleanPrecision = 3;

// `ring` as a clipper path wound counter-clockwise, so the nonzero fill adds every operand
// whichever way it was authored.
Clipper2Lib::PathD CounterClockwisePath(std::span<const Vector2> ring)
{
    Clipper2Lib::PathD path;
    path.reserve(ring.size());
    for (const Vector2& point : ring)
        path.emplace_back(point.x, point.y);
    if (Mathematics::PolygonDoubledSignedArea(ring) < 0.0f)
        std::reverse(path.begin(), path.end());
    return path;
}

Clipper2Lib::PathD FootprintPath(const MarkupFootprint& footprint)
{
    if (!footprint.IsCircle())
        return CounterClockwisePath(footprint.Ring);
    Clipper2Lib::PathD path;
    path.reserve(kFootprintCircleCorners);
    for (std::size_t corner = 0; corner < kFootprintCircleCorners; ++corner)
    {
        const float angle = 2.0f * Mathematics::Pi * static_cast<float>(corner) / static_cast<float>(kFootprintCircleCorners);
        path.emplace_back(footprint.Center.x + footprint.Radius * std::cos(angle),
                          footprint.Center.y + footprint.Radius * std::sin(angle));
    }
    return path;
}

// `path` as a ring wound counter-clockwise (`outer`) or clockwise.
std::vector<Vector2> RingOf(const Clipper2Lib::PathD& path, bool outer)
{
    std::vector<Vector2> ring;
    ring.reserve(path.size());
    for (const Clipper2Lib::PointD& point : path)
        ring.emplace_back(static_cast<float>(point.x), static_cast<float>(point.y));
    if ((Mathematics::PolygonDoubledSignedArea(ring) > 0.0f) != outer)
        std::reverse(ring.begin(), ring.end());
    return ring;
}

// The outer rings under `node` as pieces with their holes, and the islands inside those holes.
void CollectPieces(const Clipper2Lib::PolyPathD& node, std::vector<MarkupAreaPiece>& pieces)
{
    for (const auto& outer : node)
    {
        MarkupAreaPiece& piece = pieces.emplace_back();
        piece.Outer = RingOf(outer->Polygon(), true);
        for (const auto& hole : *outer)
            piece.Holes.push_back(RingOf(hole->Polygon(), false));
        for (const auto& hole : *outer)
            CollectPieces(*hole, pieces);
    }
}

} // namespace

std::vector<MarkupAreaPiece> CombineRegionArea(std::span<const Vector2> base, std::span<const MarkupFootprint> includes,
                                               std::span<const MarkupFootprint> excludes)
{
    Clipper2Lib::PathsD subjects;
    subjects.reserve(includes.size() + 1);
    subjects.push_back(CounterClockwisePath(base));
    for (const MarkupFootprint& include : includes)
        subjects.push_back(FootprintPath(include));
    Clipper2Lib::PathsD clips;
    clips.reserve(excludes.size());
    for (const MarkupFootprint& exclude : excludes)
        clips.push_back(FootprintPath(exclude));

    Clipper2Lib::ClipperD clipper(kBooleanPrecision);
    clipper.AddSubject(subjects);
    clipper.AddClip(clips);
    Clipper2Lib::PolyTreeD tree;
    std::vector<MarkupAreaPiece> pieces;
    if (!clipper.Execute(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero, tree))
        return pieces;
    CollectPieces(tree, pieces);
    return pieces;
}

} // namespace GameEngine::MarkupECS
