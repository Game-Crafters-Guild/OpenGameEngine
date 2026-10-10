#include "MarkupECS/MarkupRegionOutline.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"
#include "Spline/SplineUtility.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace GameEngine::MarkupECS
{

using Mathematics::Vector2;

namespace
{

// An outline whose mean width (twice its area over its perimeter) is under this, in meters,
// encloses nothing: collinear knots, or a sliver. The floor is relative to the outline's size,
// so a sliver far from the origin is refused as one at it is.
constexpr float32 kMinRegionMeanWidth = 0.01f;
// The label search seeds at most this many cells along the outline's longer side; a sliver's
// shorter side then takes ceil(short / long * kLabelSeedCells) cells, so the seed count is at
// most kLabelSeedCells squared whatever the outline's proportions or position.
constexpr int kLabelSeedCells = 64;
// How close the label point comes to the true pole of inaccessibility, in meters.
constexpr float32 kLabelPrecision = 0.5f;
// Bounds the label search after its seeds; a 30 km ring at 0.5 m needs far fewer cells.
constexpr int kMaxLabelCells = 100000;
// The Douglas-Peucker tolerance an agent's ring starts from, in meters.
constexpr float32 kRingTolerance = 0.1f;

float32 DistanceToSegment(const Vector2& p, const Vector2& a, const Vector2& b)
{
    const float32 abx = b.x - a.x;
    const float32 aby = b.y - a.y;
    const float32 lengthSquared = abx * abx + aby * aby;
    float32 t = lengthSquared > 0.0f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / lengthSquared : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float32 dx = p.x - (a.x + abx * t);
    const float32 dy = p.y - (a.y + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

float32 DistanceToRing(const Vector2& p, std::span<const Vector2> ring)
{
    float32 nearest = std::numeric_limits<float32>::max();
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        nearest = std::min(nearest, DistanceToSegment(p, ring[j], ring[i]));
    return nearest;
}

// The distance from `p` to the ring's outline, negative outside the ring.
float32 SignedDistanceToRing(const Vector2& p, std::span<const Vector2> ring)
{
    const float32 nearest = DistanceToRing(p, ring);
    return Mathematics::PointInPolygon(p, ring) ? nearest : -nearest;
}

// A square of the label search: its center, half size, the center's signed distance and
// the most any point in it can reach.
struct LabelCell
{
    Vector2 Center;
    float32 Half = 0.0f;
    float32 Distance = 0.0f;
    float32 Reach = 0.0f;
};

LabelCell MakeCell(const Vector2& center, float32 half, std::span<const Vector2> ring)
{
    const float32 distance = SignedDistanceToRing(center, ring);
    return LabelCell{center, half, distance, distance + half * std::sqrt(2.0f)};
}

// The area centroid, the search's first guess; the first point for a ring with no area.
// Summed relative to the first point, as PolygonDoubledSignedArea, for precision far out.
Vector2 AreaCentroid(std::span<const Vector2> ring)
{
    const Vector2 origin = ring.front();
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 doubledArea = 0.0f;
    for (std::size_t i = 2; i < ring.size(); ++i)
    {
        const Vector2 a(ring[i - 1].x - origin.x, ring[i - 1].y - origin.y);
        const Vector2 b(ring[i].x - origin.x, ring[i].y - origin.y);
        const float32 cross = a.x * b.y - b.x * a.y;
        x += (a.x + b.x) * cross;
        y += (a.y + b.y) * cross;
        doubledArea += cross;
    }
    if (doubledArea == 0.0f)
        return origin;
    return Vector2(origin.x + x / (3.0f * doubledArea), origin.y + y / (3.0f * doubledArea));
}

} // namespace

std::optional<std::string> CheckRegionOutline(std::span<const Vector2> knots)
{
    if (knots.size() < kMinRegionKnots || knots.size() > kMaxRegionKnots)
        return std::string("An outline needs 3 to 256 points, [x, z] in meters in order around the area; it has ") +
               std::to_string(knots.size());
    for (const Vector2& knot : knots)
    {
        if (!std::isfinite(knot.x) || !std::isfinite(knot.y))
            return std::string("The outline's points must be finite numbers");
    }
    const std::size_t count = knots.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t iNext = (i + 1) % count;
        // Edges that share a point (the neighbors, and the last with the first) meet by design.
        for (std::size_t j = i + 2; j < count; ++j)
        {
            const std::size_t jNext = (j + 1) % count;
            if (jNext == i)
                continue;
            if (Mathematics::SegmentsIntersect(knots[i], knots[iNext], knots[j], knots[jNext]))
                return "The outline crosses itself between points " + std::to_string(i) + "-" + std::to_string(iNext) +
                       " and " + std::to_string(j) + "-" + std::to_string(jNext) +
                       "; move the points so no two edges cross";
        }
    }
    const float32 area = RegionOutlineArea(knots);
    if (!(2.0f * area >= kMinRegionMeanWidth * RegionOutlinePerimeter(knots)))
        return std::string("The outline encloses no area; place the points around the area, not along a line");
    return std::nullopt;
}

float32 RegionOutlineArea(std::span<const Vector2> ring)
{
    if (ring.size() < 3)
        return 0.0f;
    return std::abs(Mathematics::PolygonDoubledSignedArea(ring)) * 0.5f;
}

float32 RegionOutlinePerimeter(std::span<const Vector2> ring)
{
    float32 length = 0.0f;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        length += std::hypot(ring[i].x - ring[j].x, ring[i].y - ring[j].y);
    return length;
}

float32 RegionOutlineDistance(const Vector2& point, std::span<const Vector2> ring)
{
    return ring.empty() ? 0.0f : DistanceToRing(point, ring);
}

Vector2 RegionLabelPoint(std::span<const Vector2> ring)
{
    if (ring.size() < 3)
        return ring.empty() ? Vector2() : ring.front();
    Vector2 min = ring.front();
    Vector2 max = ring.front();
    for (const Vector2& point : ring)
    {
        min = Vector2(std::min(min.x, point.x), std::min(min.y, point.y));
        max = Vector2(std::max(max.x, point.x), std::max(max.y, point.y));
    }
    const float32 width = max.x - min.x;
    const float32 depth = max.y - min.y;
    // Seeded by integer counts, never by stepping a float coordinate: a cell narrower than half
    // an ulp of the coordinate would never advance it.
    const float32 cellSize = std::max(std::min(width, depth), std::max(width, depth) / kLabelSeedCells);
    if (!(cellSize > 0.0f))
        return ring.front();

    const auto lowerReach = [](const LabelCell& a, const LabelCell& b) { return a.Reach < b.Reach; };
    std::priority_queue<LabelCell, std::vector<LabelCell>, decltype(lowerReach)> cells(lowerReach);
    const float32 half = cellSize * 0.5f;
    const int columns = std::clamp(static_cast<int>(std::ceil(width / cellSize)), 1, kLabelSeedCells);
    const int rows = std::clamp(static_cast<int>(std::ceil(depth / cellSize)), 1, kLabelSeedCells);
    for (int column = 0; column < columns; ++column)
    {
        for (int row = 0; row < rows; ++row)
            cells.push(MakeCell(Vector2(min.x + (static_cast<float32>(column) + 0.5f) * cellSize,
                                        min.y + (static_cast<float32>(row) + 0.5f) * cellSize),
                                half, ring));
    }
    LabelCell best = MakeCell(AreaCentroid(ring), 0.0f, ring);
    const LabelCell boundsCenter = MakeCell(Vector2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), 0.0f, ring);
    if (boundsCenter.Distance > best.Distance)
        best = boundsCenter;

    for (int visited = 0; !cells.empty() && visited < kMaxLabelCells; ++visited)
    {
        const LabelCell cell = cells.top();
        cells.pop();
        if (cell.Distance > best.Distance)
            best = cell;
        if (cell.Reach - best.Distance <= kLabelPrecision)
            continue;
        const float32 quarter = cell.Half * 0.5f;
        for (const float32 dx : {-quarter, quarter})
        {
            for (const float32 dy : {-quarter, quarter})
                cells.push(MakeCell(Vector2(cell.Center.x + dx, cell.Center.y + dy), quarter, ring));
        }
    }
    return best.Center;
}

std::vector<Vector2> DecimateRegionRing(std::span<const Vector2> ring)
{
    std::vector<Vector2> kept(ring.begin(), ring.end());
    if (ring.size() <= kMinRegionKnots)
        return kept;
    // Closed by repeating the first point, which Douglas-Peucker keeps at both ends.
    std::vector<Mathematics::Vector3> polyline;
    polyline.reserve(ring.size() + 1);
    for (const Vector2& point : ring)
        polyline.emplace_back(point.x, 0.0f, point.y);
    polyline.push_back(polyline.front());

    std::vector<uint32> indices;
    for (float32 tolerance = kRingTolerance;; tolerance *= 2.0f)
    {
        Spline::SimplifyPolylineIndices(polyline, tolerance, indices);
        indices.pop_back(); // the repeated first point
        if (indices.size() <= kMaxRegionKnots)
            break;
    }
    kept.clear();
    for (const uint32 index : indices)
        kept.push_back(ring[index]);
    return kept;
}

} // namespace GameEngine::MarkupECS
