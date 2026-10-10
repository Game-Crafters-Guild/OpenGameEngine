#include "MarkupECS/MarkupRegionMesh.h"

#include "MarkupECS/MarkupRegionBoolean.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Triangulation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace GameEngine::MarkupECS
{

using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

// Outline samples closer than this are one sample.
constexpr float32 kSameSampleMeters = 1.0e-3f;
constexpr uint32 kNoTriangle = std::numeric_limits<uint32>::max();

Vector2 GroundXZ(const Vector3& point) { return Vector2(point.x, point.z); }

float32 LengthSquared(const Vector2& v) { return v.x * v.x + v.y * v.y; }

// The samples with near-duplicates merged, and a closed ring's repeated first point dropped.
std::vector<Vector3> MergeSamples(std::span<const Vector3> outline, bool closed)
{
    std::vector<Vector3> merged;
    merged.reserve(outline.size());
    for (const Vector3& point : outline)
    {
        if (!merged.empty() && LengthSquared(GroundXZ(point) - GroundXZ(merged.back())) <=
                                   kSameSampleMeters * kSameSampleMeters)
            continue;
        merged.push_back(point);
    }
    while (closed && merged.size() > 1 &&
           LengthSquared(GroundXZ(merged.back()) - GroundXZ(merged.front())) <= kSameSampleMeters * kSameSampleMeters)
        merged.pop_back();
    return merged;
}

// The lid's triangulation on the ground plane, relative to the first ring's first point (so a
// region far from the origin keeps its precision): the rings ear-clipped, then every edge longer
// than the limit split at its midpoint in both triangles that share it, so no triangle meets
// another at a T and the raised lid has no cracks.
class LidRefiner
{
  public:
    // `points` are the rings' samples at their `heights`, ring after ring (`ringEnds`, one past
    // each ring's last), `triangles` the ear-clipped kept corners.
    LidRefiner(std::vector<Vector2> points, std::vector<uint32> ringEnds, std::vector<uint32> triangles,
               std::vector<float32> heights)
        : m_Points(std::move(points)), m_Heights(std::move(heights)), m_RingEnds(std::move(ringEnds))
    {
        m_RingOf.resize(m_Points.size(), kNoRing);
        m_RingPosition.resize(m_Points.size(), 0.0f);
        for (uint32 ring = 0; ring < m_RingEnds.size(); ++ring)
        {
            for (uint32 i = RingStart(ring); i < m_RingEnds[ring]; ++i)
            {
                m_RingOf[i] = ring;
                m_RingPosition[i] = static_cast<float32>(i - RingStart(ring));
            }
        }
        for (std::size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const uint32 index = static_cast<uint32>(m_Triangles.size());
            m_Triangles.push_back({triangles[t], triangles[t + 1], triangles[t + 2]});
            for (int k = 0; k < 3; ++k)
                AddEdge(triangles[t + static_cast<std::size_t>(k)], triangles[t + static_cast<std::size_t>((k + 1) % 3)],
                        index);
        }
    }

    // Splits until no edge is longer than `maxEdge`, or the triangle cap is reached, by
    // longest-edge propagation (Rivara's LEPP): a triangle's longest edge is split only once it is
    // the longest edge of the triangle across it too, after that neighbour has been split along
    // its own. So the split never cuts a short edge of a sliver, and the triangle count stays near
    // the area over the limit squared.
    void Refine(float32 maxEdge)
    {
        const float32 limitSquared = maxEdge * maxEdge;
        for (std::size_t t = 0; t < m_Triangles.size() && m_Triangles.size() < kMaxLidTriangles; ++t)
        {
            while (m_Triangles.size() < kMaxLidTriangles)
            {
                const std::pair<uint32, uint32> edge = LongestEdge(static_cast<uint32>(t));
                if (LengthSquared(m_Points[edge.second] - m_Points[edge.first]) <= limitSquared)
                    break;
                const std::pair<uint32, uint32> terminal = TerminalEdge(static_cast<uint32>(t), edge);
                SplitEdge(terminal.first, terminal.second);
            }
        }
    }

    // Splits every outline edge at the ring samples it spans (the collinear ones left out of the ear
    // clip), so the lid's edge passes through every sample the walls stand on and the two meet
    // without a crack: the triangle on such an edge becomes a fan from its opposite corner (and a
    // fan triangle that still holds another such edge, at a corner of the area, fans again).
    void InsertSkippedSamples()
    {
        std::vector<std::array<uint32, 3>> refined;
        refined.reserve(m_Triangles.size() * 2);
        for (const std::array<uint32, 3>& triangle : m_Triangles)
            FanSkippedSamples(triangle, refined);
        m_Triangles = std::move(refined);
    }

    const std::vector<Vector2>& GetPoints() const { return m_Points; }
    // NaN for a point inside the area, which takes the ground sampler's height.
    const std::vector<float32>& GetHeights() const { return m_Heights; }
    std::vector<uint32> FlattenTriangles() const
    {
        std::vector<uint32> flat;
        flat.reserve(m_Triangles.size() * 3);
        for (const std::array<uint32, 3>& triangle : m_Triangles)
            flat.insert(flat.end(), triangle.begin(), triangle.end());
        return flat;
    }

  private:
    // A triangle's longest edge; ties go to the smaller edge key, so the triangles either side
    // of an edge agree on whether it is their longest.
    std::pair<uint32, uint32> LongestEdge(uint32 triangle) const
    {
        const std::array<uint32, 3>& corners = m_Triangles[triangle];
        std::pair<uint32, uint32> longest{corners[0], corners[1]};
        float32 longestSquared = -1.0f;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const uint32 a = corners[k];
            const uint32 b = corners[(k + 1) % 3];
            const float32 lengthSquared = LengthSquared(m_Points[b] - m_Points[a]);
            if (lengthSquared > longestSquared ||
                (lengthSquared == longestSquared && Key(a, b) < Key(longest.first, longest.second)))
            {
                longestSquared = lengthSquared;
                longest = {a, b};
            }
        }
        return longest;
    }

    // The end of the longest-edge path from `triangle`: across each longest edge to the neighbour
    // while the neighbour's longest edge is another one; the edge where the path stops is the
    // longest edge of both triangles that share it (or lies on the outline).
    std::pair<uint32, uint32> TerminalEdge(uint32 triangle, std::pair<uint32, uint32> edge) const
    {
        for (std::size_t step = 0; step < m_Triangles.size(); ++step)
        {
            const auto found = m_Edges.find(Key(edge.first, edge.second));
            if (found == m_Edges.end())
                return edge;
            const uint32 neighbour = found->second[0] == triangle ? found->second[1] : found->second[0];
            if (neighbour == kNoTriangle)
                return edge;
            const std::pair<uint32, uint32> next = LongestEdge(neighbour);
            if (Key(next.first, next.second) == Key(edge.first, edge.second))
                return edge;
            triangle = neighbour;
            edge = next;
        }
        return edge;
    }

    uint32 RingStart(uint32 ring) const { return ring == 0 ? 0u : m_RingEnds[ring - 1]; }
    uint32 RingSize(uint32 ring) const { return m_RingEnds[ring] - RingStart(ring); }

    // The position on ring `ring` of `point`, the midpoint of the ring's edge (u, v): the sample
    // segment between their positions it lies on, and how far along it.
    float32 RingPositionOn(uint32 ring, uint32 u, uint32 v, const Vector2& point) const
    {
        const uint32 start = RingStart(ring);
        const std::size_t count = RingSize(ring);
        float32 from = m_RingPosition[u];
        float32 to = m_RingPosition[v];
        if (to < from)
            std::swap(from, to);
        // The edge runs the short way round the ring.
        if (to - from > 0.5f * static_cast<float32>(count))
        {
            std::swap(from, to);
            to += static_cast<float32>(count);
        }
        float32 best = from;
        float32 bestDistance = std::numeric_limits<float32>::max();
        for (float32 segment = std::floor(from); segment < to; segment += 1.0f)
        {
            const Vector2& a = m_Points[start + static_cast<std::size_t>(segment) % count];
            const Vector2& b = m_Points[start + (static_cast<std::size_t>(segment) + 1) % count];
            const Vector2 ab = b - a;
            const float32 lengthSquared = LengthSquared(ab);
            const float32 s = lengthSquared > 0.0f
                                  ? std::clamp(((point.x - a.x) * ab.x + (point.y - a.y) * ab.y) / lengthSquared, 0.0f, 1.0f)
                                  : 0.0f;
            const float32 distance = LengthSquared(a + ab * s - point);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = std::fmod(segment + s, static_cast<float32>(count));
            }
        }
        return best;
    }

    // Ring `ring`'s height at a position on it, between its two samples.
    float32 RingHeightAt(uint32 ring, float32 position) const
    {
        const uint32 start = RingStart(ring);
        const std::size_t count = RingSize(ring);
        const std::size_t a = static_cast<std::size_t>(position) % count;
        const std::size_t b = (a + 1) % count;
        const float32 s = position - std::floor(position);
        return m_Heights[start + a] + (m_Heights[start + b] - m_Heights[start + a]) * s;
    }

    void FanSkippedSamples(const std::array<uint32, 3>& triangle, std::vector<std::array<uint32, 3>>& out) const
    {
        std::vector<uint32> samples;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const uint32 a = triangle[k];
            const uint32 b = triangle[(k + 1) % 3];
            const uint32 c = triangle[(k + 2) % 3];
            samples.clear();
            AppendSkippedSamples(a, b, samples);
            if (samples.empty())
                continue;
            uint32 previous = a;
            samples.push_back(b);
            for (const uint32 sample : samples)
            {
                FanSkippedSamples({previous, sample, c}, out);
                previous = sample;
            }
            return;
        }
        out.push_back(triangle);
    }

    // The ring's edge (a, b), when a and b lie on one ring and only one triangle has it: the
    // ring's index, else kNoRing (an edge inside the area, a bridge to a hole among them).
    uint32 RingOfEdge(uint32 a, uint32 b, const std::array<uint32, 2>& sides) const
    {
        return sides[1] == kNoTriangle && m_RingOf[a] != kNoRing && m_RingOf[a] == m_RingOf[b] ? m_RingOf[a]
                                                                                             : kNoRing;
    }

    // The ring samples strictly between ring edge (a, b)'s ends, in ring order, appended to
    // `out`; none for an edge inside the area.
    void AppendSkippedSamples(uint32 a, uint32 b, std::vector<uint32>& out) const
    {
        const auto found = m_Edges.find(Key(a, b));
        if (found == m_Edges.end())
            return;
        const uint32 ringIndex = RingOfEdge(a, b, found->second);
        if (ringIndex == kNoRing)
            return;
        const uint32 start = RingStart(ringIndex);
        const std::size_t count = RingSize(ringIndex);
        // The edge spans the short way round the ring, in either direction.
        const float32 ring = static_cast<float32>(count);
        const float32 forward = std::fmod(m_RingPosition[b] - m_RingPosition[a] + ring, ring);
        const bool reversed = forward > 0.5f * ring;
        const float32 from = reversed ? m_RingPosition[b] : m_RingPosition[a];
        const float32 to = from + (reversed ? ring - forward : forward);
        const std::size_t first = out.size();
        for (float32 sample = std::floor(from) + 1.0f; sample < to; sample += 1.0f)
            out.push_back(static_cast<uint32>(start + static_cast<std::size_t>(sample) % count));
        if (reversed)
            std::reverse(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
    }

    static uint64_t Key(uint32 a, uint32 b)
    {
        return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
    }

    void AddEdge(uint32 a, uint32 b, uint32 triangle)
    {
        auto [it, inserted] = m_Edges.try_emplace(Key(a, b), std::array<uint32, 2>{kNoTriangle, kNoTriangle});
        (it->second[0] == kNoTriangle ? it->second[0] : it->second[1]) = triangle;
    }

    void ReplaceEdgeTriangle(uint32 a, uint32 b, uint32 from, uint32 to)
    {
        std::array<uint32, 2>& sides = m_Edges[Key(a, b)];
        (sides[0] == from ? sides[0] : sides[1]) = to;
    }

    // Splits edge (u, v) at its midpoint in every triangle that has it; a midpoint on a ring (an
    // edge with one triangle) takes the height on the line between its ends, so the lid's edge
    // stays on the walls' top edge.
    void SplitEdge(uint32 u, uint32 v)
    {
        const auto found = m_Edges.find(Key(u, v));
        if (found == m_Edges.end())
            return;
        const std::array<uint32, 2> sides = found->second;
        m_Edges.erase(found);
        const uint32 ring = RingOfEdge(u, v, sides);
        const uint32 middle = static_cast<uint32>(m_Points.size());
        const Vector2 point = (m_Points[u] + m_Points[v]) * 0.5f;
        m_Points.push_back(point);
        m_RingOf.push_back(ring);
        if (ring != kNoRing)
        {
            // On a ring the point takes the ring's own height there, so the lid's edge stays on the
            // walls' top edge.
            const float32 position = RingPositionOn(ring, u, v, point);
            m_RingPosition.push_back(position);
            m_Heights.push_back(RingHeightAt(ring, position));
        }
        else
        {
            m_RingPosition.push_back(0.0f);
            m_Heights.push_back(std::numeric_limits<float32>::quiet_NaN());
        }
        for (const uint32 t : sides)
        {
            if (t == kNoTriangle)
                continue;
            const std::array<uint32, 3> triangle = m_Triangles[t];
            int k = 0;
            while (k < 3 && !((triangle[static_cast<std::size_t>(k)] == u && triangle[static_cast<std::size_t>((k + 1) % 3)] == v) ||
                              (triangle[static_cast<std::size_t>(k)] == v && triangle[static_cast<std::size_t>((k + 1) % 3)] == u)))
                ++k;
            const uint32 a = triangle[static_cast<std::size_t>(k)];
            const uint32 b = triangle[static_cast<std::size_t>((k + 1) % 3)];
            const uint32 c = triangle[static_cast<std::size_t>((k + 2) % 3)];
            const uint32 added = static_cast<uint32>(m_Triangles.size());
            m_Triangles[t] = {a, middle, c};
            m_Triangles.push_back({middle, b, c});
            ReplaceEdgeTriangle(b, c, t, added);
            AddEdge(a, middle, t);
            AddEdge(middle, b, added);
            AddEdge(middle, c, t);
            AddEdge(middle, c, added);
        }
    }

    // The ring of a point inside the area.
    static constexpr uint32 kNoRing = std::numeric_limits<uint32>::max();

    std::vector<Vector2> m_Points;
    std::vector<float32> m_Heights;
    std::vector<uint32> m_RingEnds;
    std::vector<uint32> m_RingOf;        // a point's ring, or kNoRing inside the area
    std::vector<float32> m_RingPosition; // on its ring: a sample's index, or the place between two
    std::vector<std::array<uint32, 3>> m_Triangles;
    std::unordered_map<uint64_t, std::array<uint32, 2>> m_Edges;
};

// The ring's corners: the samples a straight run between two kept samples would pass farther than
// kCornerTolerance from, so every sample left out lies on an edge between corners.
constexpr float32 kCornerTolerance = 1.0e-2f;

std::vector<uint32> RingCorners(std::span<const Vector2> ring)
{
    std::vector<uint32> corners{0};
    const std::size_t count = ring.size();
    std::size_t anchor = 0;
    while (anchor < count)
    {
        // The farthest sample `end` such that every sample between `anchor` and it lies on the
        // straight run between the two.
        std::size_t end = anchor + 1;
        while (end < count)
        {
            const std::size_t next = end + 1;
            const Vector2& a = ring[anchor];
            const Vector2& b = ring[next % count];
            const Vector2 ab = b - a;
            const float32 length = std::sqrt(LengthSquared(ab));
            bool straight = length > 0.0f;
            for (std::size_t k = anchor + 1; straight && k < next; ++k)
            {
                const Vector2 ak = ring[k] - a;
                straight = std::abs(ab.x * ak.y - ab.y * ak.x) / length <= kCornerTolerance;
            }
            if (!straight)
                break;
            end = next;
        }
        if (end >= count)
            break;
        corners.push_back(static_cast<uint32>(end));
        anchor = end;
    }
    return corners;
}

// One piece of the lid: the ground's outer ring and the holes inside it, as ring indices.
struct LidPiece
{
    uint32 Outer = 0;
    std::vector<uint32> Holes;
};

// The rightmost x of ring `ring` in `points` (ring after ring, `ringEnds`).
float32 RingRightmostX(const std::vector<Vector2>& points, const std::vector<uint32>& ringEnds, uint32 ring)
{
    float32 rightmost = -std::numeric_limits<float32>::max();
    for (uint32 i = ring == 0 ? 0u : ringEnds[ring - 1]; i < ringEnds[ring]; ++i)
        rightmost = std::max(rightmost, points[i].x);
    return rightmost;
}

// Ring `ring`'s corners (RingCorners) as indices into `points` (ring after ring, `ringEnds`).
std::vector<uint32> RingCornerIndices(const std::vector<Vector2>& points, const std::vector<uint32>& ringEnds,
                                      uint32 ring)
{
    const uint32 start = ring == 0 ? 0u : ringEnds[ring - 1];
    std::vector<uint32> corners = RingCorners(std::span<const Vector2>(points).subspan(start, ringEnds[ring] - start));
    for (uint32& corner : corners)
        corner += start;
    return corners;
}

// The lid over the ground's simple rings: each piece's outer ring (counter-clockwise from above)
// with its holes (clockwise) bridged in.
void BuildLid(const std::vector<LidPiece>& pieces, const MarkupGroundSampler& groundAt, MarkupRegionGround& ground)
{
    const Vector2 origin = ground.OutlineXZ.front();
    std::vector<Vector2> local;
    std::vector<float32> heights;
    local.reserve(ground.Outline.size());
    heights.reserve(ground.Outline.size());
    Vector2 min = origin;
    Vector2 max = origin;
    for (const Vector3& point : ground.Outline)
    {
        local.push_back(GroundXZ(point) - origin);
        heights.push_back(point.y);
        min = Vector2(std::min(min.x, point.x), std::min(min.y, point.z));
        max = Vector2(std::max(max.x, point.x), std::max(max.y, point.z));
    }
    // The ear clip takes the rings' corners only: a straight run of samples would clip into flat
    // slivers the split could never make well-shaped. The samples left out go back into the
    // ring edges after the split.
    std::vector<uint32> triangles;
    for (const LidPiece& piece : pieces)
    {
        std::vector<uint32> polygon = RingCornerIndices(local, ground.RingEnds, piece.Outer);
        std::vector<uint32> holes = piece.Holes;
        // Rightmost hole first, so a later bridge never crosses an earlier one.
        std::sort(holes.begin(), holes.end(), [&](uint32 a, uint32 b) {
            return RingRightmostX(local, ground.RingEnds, a) > RingRightmostX(local, ground.RingEnds, b);
        });
        for (const uint32 hole : holes)
            Mathematics::BridgeHole(local, polygon, RingCornerIndices(local, ground.RingEnds, hole));
        Mathematics::EarClip(local, std::move(polygon), triangles);
    }

    LidRefiner refiner(std::move(local), ground.RingEnds, std::move(triangles), std::move(heights));
    const float32 extent = std::max(max.x - min.x, max.y - min.y);
    refiner.Refine(std::max(kLidMinEdgeMeters, extent / kLidEdgeExtentDivisions));
    refiner.InsertSkippedSamples();

    std::vector<Vector2> rings2D;
    rings2D.reserve(ground.OutlineXZ.size());
    for (const Vector2& point : ground.OutlineXZ)
        rings2D.push_back(point - origin);
    const std::vector<Vector2>& points = refiner.GetPoints();
    const std::vector<float32>& pointHeights = refiner.GetHeights();
    ground.LidPoints.reserve(points.size());
    ground.LidEdgeDistance.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const Vector2 world = points[i] + origin;
        const bool interior = std::isnan(pointHeights[i]);
        ground.LidPoints.emplace_back(world.x, interior ? groundAt(world) : pointHeights[i], world.y);
        // The rings' own points and their edges' midpoints lie on a ring.
        float32 distance = 0.0f;
        if (interior)
        {
            distance = std::numeric_limits<float32>::max();
            for (std::size_t ring = 0; ring < ground.GetRingCount(); ++ring)
            {
                const uint32 start = ring == 0 ? 0u : ground.RingEnds[ring - 1];
                distance = std::min(distance, RegionOutlineDistance(points[i], std::span<const Vector2>(rings2D).subspan(
                                                                                   start, ground.RingEnds[ring] - start)));
            }
        }
        ground.LidEdgeDistance.push_back(distance);
    }
    ground.LidTriangles = refiner.FlattenTriangles();
}

// Sets the ground's rings and the bounds around them.
void SetRings(MarkupRegionGround& ground, std::vector<std::vector<Vector3>> rings)
{
    for (std::vector<Vector3>& ring : rings)
    {
        ground.Outline.insert(ground.Outline.end(), ring.begin(), ring.end());
        ground.RingEnds.push_back(static_cast<uint32>(ground.Outline.size()));
    }
    ground.OutlineXZ.reserve(ground.Outline.size());
    for (const Vector3& point : ground.Outline)
        ground.OutlineXZ.push_back(GroundXZ(point));
    if (ground.Outline.empty())
        return;
    Vector3 min = ground.Outline.front();
    Vector3 max = min;
    for (const Vector3& point : ground.Outline)
    {
        min = Vector3(std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z));
        max = Vector3(std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z));
    }
    ground.Center = (min + max) * 0.5f;
    ground.Top = max.y;
    for (const Vector3& point : ground.Outline)
        ground.Radius = std::max(ground.Radius, (point - ground.Center).Length());
}

// A closed base outline's merged samples `base` turned counter-clockwise from above, with the
// region's label point and whether the outline crosses itself read from them.
std::vector<Vector3> ReadClosedBase(std::vector<Vector3> base, const MarkupGroundSampler& groundAt,
                                    MarkupRegionGround& ground)
{
    std::vector<Vector2> ring2D;
    ring2D.reserve(base.size());
    for (const Vector3& point : base)
        ring2D.push_back(GroundXZ(point));
    if (Mathematics::PolygonDoubledSignedArea(ring2D) < 0.0f)
    {
        std::reverse(base.begin(), base.end());
        std::reverse(ring2D.begin(), ring2D.end());
    }
    const Vector2 label = RegionLabelPoint(ring2D);
    ground.LabelGround = Vector3(label.x, groundAt(label), label.y);
    // The drape caps the samples at kMaxRegionKnots, so the outline rule's count bound holds; a
    // crossing, or a sliver with no area, has no lid.
    ground.Crosses = CheckRegionOutline(ring2D).has_value();
    return base;
}

// `ring` sampled along the ground: its corners, and between them samples at most `spacing`
// apart, the spacing widened so a ring of any length adds at most kMaxRegionKnots; each at
// `groundAt`'s height, near-duplicates merged.
std::vector<Vector3> SampleAreaRing(std::span<const Vector2> ring, float32 spacing, const MarkupGroundSampler& groundAt)
{
    const float32 step = std::max(spacing, RegionOutlinePerimeter(ring) / static_cast<float32>(kMaxRegionKnots));
    std::vector<Vector3> samples;
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const Vector2 from = ring[i];
        const Vector2 to = ring[(i + 1) % ring.size()];
        const float32 length = std::sqrt(LengthSquared(to - from));
        const int steps = std::max(1, static_cast<int>(std::ceil(length / step)));
        for (int s = 0; s < steps; ++s)
        {
            const Vector2 point = from + (to - from) * (static_cast<float32>(s) / static_cast<float32>(steps));
            samples.emplace_back(point.x, groundAt(point), point.y);
        }
    }
    return MergeSamples(samples, true);
}

// The point halfway along an open polyline's length.
Vector3 HalfwayAlong(const std::vector<Vector3>& line)
{
    float32 remaining = 0.5f * PolylineLength(line);
    for (std::size_t i = 1; i < line.size(); ++i)
    {
        const float32 segment = (line[i] - line[i - 1]).Length();
        if (segment >= remaining && segment > 0.0f)
            return line[i - 1] + (line[i] - line[i - 1]) * (remaining / segment);
        remaining -= segment;
    }
    return line.back();
}

void AppendVertex(MarkupRegionMesh& mesh, const Vector3& position, float32 rim)
{
    mesh.Vertices.push_back(MarkupRegionVertex{position, rim});
}

// Reads the ground across a path's band at every sample (EdgeGround).
void SamplePathEdges(MarkupRegionGround& ground, const MarkupGroundSampler& groundAt)
{
    const std::vector<Vector3>& line = ground.Outline;
    ground.EdgeGround.reserve(line.size() * kPathEdgeOffsets.size() * 2);
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const Vector3 across = PathAcross(line, i);
        for (const float32 side : {-1.0f, 1.0f})
        {
            for (const float32 offset : kPathEdgeOffsets)
                ground.EdgeGround.push_back(groundAt(GroundXZ(line[i] + across * (side * offset))));
        }
    }
}

} // namespace

float32 PolylineLength(std::span<const Vector3> line)
{
    float32 length = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i)
        length += (line[i] - line[i - 1]).Length();
    return length;
}

Vector3 PathAcross(std::span<const Vector3> line, std::size_t i)
{
    const Vector3& previous = line[i == 0 ? 0 : i - 1];
    const Vector3& next = line[std::min(i + 1, line.size() - 1)];
    const Vector3 along = next - previous;
    return Vector3(along.z, 0.0f, -along.x).NormalizeOrZero();
}

float32 PathEdgeHeight(const MarkupRegionGround& ground, std::size_t i, float32 side, float32 halfWidth)
{
    constexpr std::size_t kOffsets = kPathEdgeOffsets.size();
    if (ground.EdgeGround.size() != ground.Outline.size() * kOffsets * 2)
        return ground.Outline[i].y;
    const float32* heights = ground.EdgeGround.data() + (i * 2 + (side > 0.0f ? 1 : 0)) * kOffsets;
    if (halfWidth <= kPathEdgeOffsets.front())
        return heights[0];
    for (std::size_t k = 1; k < kOffsets; ++k)
    {
        if (halfWidth <= kPathEdgeOffsets[k])
        {
            const float32 t = (halfWidth - kPathEdgeOffsets[k - 1]) / (kPathEdgeOffsets[k] - kPathEdgeOffsets[k - 1]);
            return heights[k - 1] + (heights[k] - heights[k - 1]) * t;
        }
    }
    return heights[kOffsets - 1];
}

std::span<const Vector3> MarkupRegionGround::GetRing(std::size_t ring) const
{
    const uint32 start = ring == 0 ? 0u : RingEnds[ring - 1];
    return std::span<const Vector3>(Outline).subspan(start, RingEnds[ring] - start);
}

std::span<const Vector2> MarkupRegionGround::GetRingXZ(std::size_t ring) const
{
    const uint32 start = ring == 0 ? 0u : RingEnds[ring - 1];
    return std::span<const Vector2>(OutlineXZ).subspan(start, RingEnds[ring] - start);
}

bool MarkupRegionGround::Contains(const Vector2& xz) const
{
    if (!Closed)
        return false;
    bool inside = false;
    for (std::size_t ring = 0; ring < GetRingCount(); ++ring)
        inside = inside != Mathematics::PointInPolygon(xz, GetRingXZ(ring));
    return inside;
}

MarkupRegionGround BuildMarkupRegionGround(std::span<const Vector3> outline, bool closed,
                                           const MarkupGroundSampler& groundAt)
{
    MarkupRegionGround ground;
    std::vector<Vector3> merged = MergeSamples(outline, closed);
    if (merged.empty())
        return ground;
    ground.Closed = closed && merged.size() >= kMinRegionKnots;
    if (!ground.Closed)
    {
        ground.LabelGround = HalfwayAlong(merged);
        SetRings(ground, {std::move(merged)});
        if (!closed)
            SamplePathEdges(ground, groundAt);
        return ground;
    }
    SetRings(ground, {ReadClosedBase(std::move(merged), groundAt, ground)});
    if (!ground.Crosses)
        BuildLid({LidPiece{}}, groundAt, ground);
    return ground;
}

MarkupRegionGround BuildMarkupRegionAreaGround(std::span<const Vector3> outline, std::span<const MarkupAreaPiece> area,
                                               float32 spacing, const MarkupGroundSampler& groundAt)
{
    MarkupRegionGround ground;
    std::vector<Vector3> base = MergeSamples(outline, true);
    if (base.size() < kMinRegionKnots)
        return BuildMarkupRegionGround(outline, true, groundAt);
    ground.Closed = true;
    ReadClosedBase(std::move(base), groundAt, ground);
    if (ground.Crosses)
        return BuildMarkupRegionGround(outline, true, groundAt);
    std::vector<std::vector<Vector3>> rings;
    std::vector<LidPiece> pieces;
    for (const MarkupAreaPiece& piece : area)
    {
        LidPiece& lid = pieces.emplace_back();
        lid.Outer = static_cast<uint32>(rings.size());
        rings.push_back(SampleAreaRing(piece.Outer, spacing, groundAt));
        for (const std::vector<Vector2>& hole : piece.Holes)
        {
            lid.Holes.push_back(static_cast<uint32>(rings.size()));
            rings.push_back(SampleAreaRing(hole, spacing, groundAt));
        }
    }
    SetRings(ground, std::move(rings));
    if (!pieces.empty())
        BuildLid(pieces, groundAt, ground);
    return ground;
}

MarkupRegionMesh BuildMarkupRegionMesh(const MarkupRegionGround& ground, float32 extrudeHeight)
{
    MarkupRegionMesh mesh;
    const Vector3 lift(0.0f, extrudeHeight, 0.0f);
    mesh.Label = ground.LabelGround + lift;
    if (ground.Outline.empty() || !ground.Closed)
        return mesh;

    // The rim coordinate is the distance to the top edge over the rim's width.
    const float32 rimWidth = std::max(kMarkupRegionRimWidthFraction * extrudeHeight, 1.0e-3f);
    const float32 bottomRim = extrudeHeight / rimWidth;
    mesh.Vertices.reserve(ground.Outline.size() * 6 + ground.LidTriangles.size());
    for (std::size_t r = 0; r < ground.GetRingCount(); ++r)
    {
        const std::span<const Vector3> ring = ground.GetRing(r);
        for (std::size_t i = 0; i < ring.size(); ++i)
        {
            // Quad from sample i to sample i + 1, wound to face out of its ring (out of the area on
            // a counter-clockwise outer ring, into the hole on a clockwise one): seen from outside
            // each triangle turns counter-clockwise, as markup_glow.vert winds a box's faces.
            const Vector3& from = ring[i];
            const Vector3& to = ring[(i + 1) % ring.size()];
            AppendVertex(mesh, to, bottomRim);
            AppendVertex(mesh, from + lift, 0.0f);
            AppendVertex(mesh, from, bottomRim);
            AppendVertex(mesh, to, bottomRim);
            AppendVertex(mesh, to + lift, 0.0f);
            AppendVertex(mesh, from + lift, 0.0f);
        }
    }
    mesh.WallVertexCount = mesh.Vertices.size();
    for (const uint32 index : ground.LidTriangles)
        AppendVertex(mesh, ground.LidPoints[index] + lift, ground.LidEdgeDistance[index] / rimWidth);
    return mesh;
}

} // namespace GameEngine::MarkupECS
