#include "Spline/SplineEvaluator.h"
#include "Spline/SplineData.h"
#include "Mathematics/BezierCurve.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/VectorOps.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace GameEngine::Spline
{
namespace
{

using V3 = Mathematics::Vector3;

// Below this squared length a raw tangent has no usable direction, and
// normalizing it would yield NaN (glm::normalize is unguarded). Same epsilon as
// the editor placement code's NormalizedOrFallback.
constexpr float32 kMinTangentLengthSq = 1.0e-8f;

// For a unit forward, |Cross(up, forward)|^2 = sin^2(angle from up): below this
// threshold (within ~0.006 degrees of vertical) the frame switches to the
// alternate up reference. Kept at the numerical floor deliberately — the
// alternate basis does not agree with the primary at the boundary (up to a
// 180-degree roll pop), so the switch cone must stay as narrow as fp32
// allows; 1e-8 is a normal float and inversesqrt of it is exact.
constexpr float32 kMinFrameCrossLengthSq = 1.0e-8f;

// Get the 4 control points for a segment, handling closed spline wraparound.
void GetSegmentPoints(const SplineData& spline, uint32 segIndex,
                      V3& p0, V3& p1, V3& p2, V3& p3)
{
    const auto& pts = spline.Points;
    const uint32 n = static_cast<uint32>(pts.size());

    auto wrap = [n](int32 i) -> uint32 {
        return static_cast<uint32>(((i % static_cast<int32>(n)) + static_cast<int32>(n)) % static_cast<int32>(n));
    };

    if (spline.Type == SplineType::CubicBezier)
    {
        // Bezier: p0 = point[seg], p1 = point[seg] + tangentOut,
        //         p2 = point[seg+1] + tangentIn, p3 = point[seg+1]
        const uint32 i0 = spline.IsEffectivelyClosed() ? wrap(segIndex) : segIndex;
        const uint32 i1 = spline.IsEffectivelyClosed() ? wrap(segIndex + 1) : std::min(segIndex + 1, n - 1);
        p0 = pts[i0].Position;
        p1 = pts[i0].Position + pts[i0].TangentOut;
        p2 = pts[i1].Position + pts[i1].TangentIn;
        p3 = pts[i1].Position;
    }
    else // CatmullRom or Linear: need 4 context points
    {
        if (spline.IsEffectivelyClosed())
        {
            p0 = pts[wrap(static_cast<int32>(segIndex) - 1)].Position;
            p1 = pts[wrap(segIndex)].Position;
            p2 = pts[wrap(segIndex + 1)].Position;
            p3 = pts[wrap(segIndex + 2)].Position;
        }
        else
        {
            const int32 i = static_cast<int32>(segIndex);
            p0 = pts[static_cast<uint32>(std::max(i - 1, 0))].Position;
            p1 = pts[segIndex].Position;
            p2 = pts[std::min(segIndex + 1, n - 1)].Position;
            p3 = pts[static_cast<uint32>(std::min(i + 2, static_cast<int32>(n) - 1))].Position;
        }
    }
}

// Evaluate a single segment at local t in [0,1].
V3 EvaluateSegment(const SplineData& spline, uint32 segIndex, float32 t)
{
    V3 p0, p1, p2, p3;
    GetSegmentPoints(spline, segIndex, p0, p1, p2, p3);

    switch (spline.Type)
    {
    case SplineType::Linear:
        return p1 + (p2 - p1) * t;

    case SplineType::CubicBezier:
        return Math::CubicBezier(p0, p1, p2, p3, t);

    case SplineType::CatmullRom:
    default:
        return Math::CatmullRom(p0, p1, p2, p3, t);
    }
}

// Tangent of a single segment at local t. Always returns a finite unit vector
// (BuildFrame relies on this).
V3 EvaluateSegmentTangent(const SplineData& spline, uint32 segIndex, float32 t)
{
    V3 p0, p1, p2, p3;
    GetSegmentPoints(spline, segIndex, p0, p1, p2, p3);

    V3 tangent;
    switch (spline.Type)
    {
    case SplineType::Linear:
        tangent = p2 - p1;
        break;

    case SplineType::CubicBezier:
        tangent = Math::CubicBezierTangent(p0, p1, p2, p3, t);
        break;

    case SplineType::CatmullRom:
    default:
        tangent = Math::CatmullRomTangent(p0, p1, p2, p3, t);
        break;
    }

    // Coincident consecutive points (a double-added point) and zero Bezier
    // handles at an exact segment end give a zero raw tangent; the threshold
    // term catches those, and catches NaN with them because NaN compares false
    // against every bound. The finite term is what catches the other half:
    // garbage control points whose squared length is infinite pass a threshold
    // test honestly, and glm::normalize is unguarded, so they would come back
    // as NaN where a component was infinite and as a zero vector where finite
    // components merely overflowed when squared. The evaluator is stateless —
    // there is no previous frame's tangent to reuse — so fall back to the
    // engine's canonical +Z forward.
    const float32 tangentLenSq = V3::Dot(tangent, tangent);
    if (!std::isfinite(tangentLenSq) || tangentLenSq < kMinTangentLengthSq)
        return V3{0.0f, 0.0f, 1.0f};

    return tangent.Normalize();
}

// Interpolate radius between two points.
float32 LerpRadius(const SplineData& spline, uint32 segIndex, float32 t)
{
    const auto& pts = spline.Points;
    const uint32 n = static_cast<uint32>(pts.size());
    uint32 i0 = segIndex;
    uint32 i1 = spline.IsEffectivelyClosed() ? ((segIndex + 1) % n) : std::min(segIndex + 1, n - 1);
    return Math::Lerp(pts[i0].Radius, pts[i1].Radius, t);
}

float32 LerpRoll(const SplineData& spline, uint32 segIndex, float32 t)
{
    const auto& pts = spline.Points;
    const uint32 n = static_cast<uint32>(pts.size());
    uint32 i0 = segIndex;
    uint32 i1 = spline.IsEffectivelyClosed() ? ((segIndex + 1) % n) : std::min(segIndex + 1, n - 1);
    return Math::Lerp(pts[i0].Roll, pts[i1].Roll, t);
}

V3 LerpRotation(const SplineData& spline, uint32 segIndex, float32 t)
{
    const auto& pts = spline.Points;
    const uint32 n = static_cast<uint32>(pts.size());
    uint32 i0 = segIndex;
    uint32 i1 = spline.IsEffectivelyClosed() ? ((segIndex + 1) % n) : std::min(segIndex + 1, n - 1);
    return pts[i0].Rotation + (pts[i1].Rotation - pts[i0].Rotation) * t;
}

V3 LerpScale(const SplineData& spline, uint32 segIndex, float32 t)
{
    const auto& pts = spline.Points;
    const uint32 n = static_cast<uint32>(pts.size());
    uint32 i0 = segIndex;
    uint32 i1 = spline.IsEffectivelyClosed() ? ((segIndex + 1) % n) : std::min(segIndex + 1, n - 1);
    return pts[i0].Scale + (pts[i1].Scale - pts[i0].Scale) * t;
}

// Build an oriented frame from a forward direction and roll. `forward` must be
// a finite unit vector (EvaluateSegmentTangent guarantees it).
SplineFrame BuildFrame(const V3& position,
                       const V3& forward,
                       float32 radius,
                       float32 roll,
                       const V3& rotation,
                       const V3& scale)
{
    SplineFrame frame{};
    frame.Position = position;
    frame.Forward = forward;
    frame.Radius = radius;
    frame.Roll = roll;
    frame.Rotation = rotation;
    frame.Scale = scale;

    // Right vector: perpendicular to forward in the XZ plane. Test the raw
    // cross before normalizing — normalizing a zero cross yields NaN, and the
    // negated comparison keeps the fallback reachable even then (NaN
    // comparisons are false).
    V3 worldUp{0.0f, 1.0f, 0.0f};
    V3 right = V3::Cross(worldUp, forward);
    if (!(V3::Dot(right, right) >= kMinFrameCrossLengthSq))
    {
        // Forward is (near-)vertical: use the alternate up reference, whose
        // cross with a unit vertical forward is near unit length.
        V3 altUp{0.0f, 0.0f, 1.0f};
        right = V3::Cross(altUp, forward);
    }
    right = right.Normalize();

    V3 up = V3::Cross(forward, right);

    // Apply roll (rotate right and up around forward).
    if (std::abs(roll) > 0.001f)
    {
        float32 c = std::cos(roll);
        float32 s = std::sin(roll);
        V3 newRight = right * c + up * s;
        V3 newUp = up * c - right * s;
        right = newRight;
        up = newUp;
    }

    frame.Right = right;
    frame.Up = up;
    return frame;
}

// Decompose global t into segment index + local t.
void DecomposeT(const SplineData& spline, float32 globalT,
                uint32& outSegment, float32& outLocalT)
{
    const uint32 segCount = spline.GetSegmentCount();
    if (segCount == 0)
    {
        outSegment = 0;
        outLocalT = 0.0f;
        return;
    }

    globalT = std::clamp(globalT, 0.0f, 1.0f);
    float32 scaled = globalT * static_cast<float32>(segCount);
    outSegment = static_cast<uint32>(scaled);
    if (outSegment >= segCount)
        outSegment = segCount - 1;
    outLocalT = scaled - static_cast<float32>(outSegment);
}

// Which axes a spatial query measures. XZ projects onto the ground plane, so a
// curve's altitude never enters the distance — and the broad phase has to drop
// the same axis, or it would over-estimate a segment's distance and cull the
// one that is actually nearest in XZ.
enum class Metric
{
    ThreeD,
    XZ,
};

float32 DistSq(const V3& a, const V3& b, Metric metric)
{
    const float32 dx = a.x - b.x;
    const float32 dz = a.z - b.z;
    if (metric == Metric::XZ)
        return dx * dx + dz * dz;
    const float32 dy = a.y - b.y;
    return dx * dx + dy * dy + dz * dz;
}

// Squared distance from point to AABB.
float32 DistSqToAABB(const V3& p, const Mathematics::AABB& box, Metric metric)
{
    float32 dx = std::max(0.0f, std::max(box.min.x - p.x, p.x - box.max.x));
    float32 dz = std::max(0.0f, std::max(box.min.z - p.z, p.z - box.max.z));
    if (metric == Metric::XZ)
        return dx * dx + dz * dz;
    float32 dy = std::max(0.0f, std::max(box.min.y - p.y, p.y - box.max.y));
    return dx * dx + dy * dy + dz * dz;
}

// Closest point on the curve under `metric`. Subdivision approach: sample each
// segment at regular intervals, then refine the best candidate with bisection.
// Position and T always address the full 3-D curve; only the distance being
// minimized changes with the metric.
ClosestPointResult FindClosestImpl(const SplineData& spline, const V3& queryPos, Metric metric)
{
    ClosestPointResult best{};
    best.Distance = std::numeric_limits<float32>::max();

    if (!spline.IsValid())
        return best;

    const uint32 segCount = spline.GetSegmentCount();

    constexpr uint32 kSamplesPerSegment = 16;
    constexpr uint32 kRefinementSteps = 8;

    float32 globalBestDistSq = std::numeric_limits<float32>::max();

    for (uint32 seg = 0; seg < segCount; ++seg)
    {
        // Broad phase: skip segments whose AABB is farther than current best.
        if (seg < static_cast<uint32>(spline.SegmentBounds.size()))
        {
            float32 aabbDistSq = DistSqToAABB(queryPos, spline.SegmentBounds[seg], metric);
            if (aabbDistSq > globalBestDistSq)
                continue;
        }

        // Coarse sampling
        float32 localBestT = 0.0f;
        float32 localBestDistSq = std::numeric_limits<float32>::max();

        for (uint32 s = 0; s <= kSamplesPerSegment; ++s)
        {
            float32 lt = static_cast<float32>(s) / static_cast<float32>(kSamplesPerSegment);
            float32 distSq = DistSq(EvaluateSegment(spline, seg, lt), queryPos, metric);
            if (distSq < localBestDistSq)
            {
                localBestDistSq = distSq;
                localBestT = lt;
            }
        }

        // Bisection refinement around the best sample.
        float32 lo = std::max(0.0f, localBestT - 1.0f / static_cast<float32>(kSamplesPerSegment));
        float32 hi = std::min(1.0f, localBestT + 1.0f / static_cast<float32>(kSamplesPerSegment));

        for (uint32 r = 0; r < kRefinementSteps; ++r)
        {
            float32 mid1 = lo + (hi - lo) * 0.333f;
            float32 mid2 = lo + (hi - lo) * 0.667f;

            float32 sq1 = DistSq(EvaluateSegment(spline, seg, mid1), queryPos, metric);
            float32 sq2 = DistSq(EvaluateSegment(spline, seg, mid2), queryPos, metric);

            if (sq1 < sq2)
                hi = mid2;
            else
                lo = mid1;
        }

        float32 refinedT = (lo + hi) * 0.5f;
        V3 refinedPos = EvaluateSegment(spline, seg, refinedT);
        float32 refinedDistSq = DistSq(refinedPos, queryPos, metric);

        if (refinedDistSq < globalBestDistSq)
        {
            globalBestDistSq = refinedDistSq;
            float32 globalT = (static_cast<float32>(seg) + refinedT) / static_cast<float32>(segCount);
            best.T = globalT;
            best.Position = refinedPos;
            best.Distance = std::sqrt(refinedDistSq);
            best.SegmentIndex = seg;
        }
    }

    return best;
}

// Distance to the swept envelope under `metric`: distance to the path minus the
// radius interpolated at the closest point. `fillInterior` decides whether a
// closed spline's enclosed region counts as inside (a filled area) or only the
// swept band around the curve does (a ring).
float32 SignedDistanceImpl(const SplineData& spline, const V3& queryPos, Metric metric,
                           bool fillInterior)
{
    auto result = FindClosestImpl(spline, queryPos, metric);
    if (result.Distance >= std::numeric_limits<float32>::max() * 0.5f)
        return std::numeric_limits<float32>::max();

    // Interpolate radius at the closest point.
    uint32 seg;
    float32 lt;
    DecomposeT(spline, result.T, seg, lt);
    float32 radius = LerpRadius(spline, seg, lt);

    // For closed splines with a cached polygon, test whether the query point
    // is inside the enclosed area (XZ plane). Interior points get a negative
    // signed distance so the terrain modifier fills the entire region.
    if (fillInterior && spline.IsEffectivelyClosed() && !spline.ClosedPolygonXZ.empty())
    {
        if (Mathematics::PointInPolygon(Mathematics::Vector2(queryPos.x, queryPos.z), spline.ClosedPolygonXZ))
            return -(result.Distance + radius); // Always negative inside
    }

    // Positive = outside envelope, negative = inside radius band.
    return result.Distance - radius;
}

} // anonymous namespace

// ---- Public API ----

SplineFrame Evaluate(const SplineData& spline, float32 t)
{
    if (!spline.IsValid())
        return {};

    uint32 seg;
    float32 lt;
    DecomposeT(spline, t, seg, lt);

    V3 pos = EvaluateSegment(spline, seg, lt);
    V3 fwd = EvaluateSegmentTangent(spline, seg, lt);
    float32 radius = LerpRadius(spline, seg, lt);
    float32 roll = LerpRoll(spline, seg, lt);
    V3 rotation = LerpRotation(spline, seg, lt);
    V3 scale = LerpScale(spline, seg, lt);

    return BuildFrame(pos, fwd, radius, roll, rotation, scale);
}

Mathematics::Vector3 EvaluatePosition(const SplineData& spline, float32 t)
{
    if (!spline.IsValid())
        return {};

    uint32 seg;
    float32 lt;
    DecomposeT(spline, t, seg, lt);
    return EvaluateSegment(spline, seg, lt);
}

SplineFrame EvaluateAtDistance(const SplineData& spline, float32 distance)
{
    float32 t = DistanceToParametric(spline, distance);
    return Evaluate(spline, t);
}

float32 ParametricToDistance(const SplineData& spline, float32 t)
{
    if (spline.ArcLengthTable.empty() || spline.TotalArcLength <= 0.0f)
        return 0.0f;

    t = std::clamp(t, 0.0f, 1.0f);

    // Binary search for t in ArcLengthParams.
    const auto& params = spline.ArcLengthParams;
    const auto& table = spline.ArcLengthTable;
    auto it = std::lower_bound(params.begin(), params.end(), t);
    if (it == params.end())
        return spline.TotalArcLength;
    if (it == params.begin())
        return 0.0f;

    uint32 idx = static_cast<uint32>(it - params.begin());
    float32 frac = Math::InverseLerp(params[idx - 1], params[idx], t);
    return Math::Lerp(table[idx - 1], table[idx], frac);
}

float32 DistanceToParametric(const SplineData& spline, float32 distance)
{
    if (spline.ArcLengthTable.empty() || spline.TotalArcLength <= 0.0f)
        return 0.0f;

    distance = std::clamp(distance, 0.0f, spline.TotalArcLength);

    const auto& table = spline.ArcLengthTable;
    const auto& params = spline.ArcLengthParams;

    auto it = std::lower_bound(table.begin(), table.end(), distance);
    if (it == table.end())
        return 1.0f;
    if (it == table.begin())
        return 0.0f;

    uint32 idx = static_cast<uint32>(it - table.begin());
    float32 frac = Math::InverseLerp(table[idx - 1], table[idx], distance);
    return Math::Lerp(params[idx - 1], params[idx], frac);
}

float32 SampleChannelAtDistance(const SplineData& spline, StringId channel,
                                float32 distance, float32 fallback)
{
    if (channel != SplineChannels::kWidth || !spline.IsValid())
        return fallback;

    // kWidth is backed by the per-point Radius: distance -> parametric t via
    // the arc-length LUT, then a linear lerp between the segment's endpoint
    // radii (the same interpolation Evaluate uses for SplineFrame::Radius).
    const float32 t = DistanceToParametric(spline, distance);
    uint32 segment = 0;
    float32 localT = 0.0f;
    DecomposeT(spline, t, segment, localT);
    return LerpRadius(spline, segment, localT);
}

ClosestPointResult FindClosestPoint(const SplineData& spline,
                                    const Mathematics::Vector3& queryPos)
{
    return FindClosestImpl(spline, queryPos, Metric::ThreeD);
}

float32 SignedDistanceToSpline(const SplineData& spline,
                               const Mathematics::Vector3& queryPos)
{
    return SignedDistanceImpl(spline, queryPos, Metric::ThreeD, /*fillInterior=*/true);
}

ClosestPointResult FindClosestPointXZ(const SplineData& spline, float32 worldX, float32 worldZ)
{
    return FindClosestImpl(spline, V3(worldX, 0.0f, worldZ), Metric::XZ);
}

float32 SignedDistanceToSplineXZ(const SplineData& spline, float32 worldX, float32 worldZ)
{
    return SignedDistanceImpl(spline, V3(worldX, 0.0f, worldZ), Metric::XZ, /*fillInterior=*/true);
}

float32 SignedDistanceToSplineBandXZ(const SplineData& spline, float32 worldX, float32 worldZ)
{
    return SignedDistanceImpl(spline, V3(worldX, 0.0f, worldZ), Metric::XZ, /*fillInterior=*/false);
}

void RebuildSplineCache(SplineData& spline)
{
    const uint32 segCount = spline.GetSegmentCount();

    // Rebuild segment AABBs.
    spline.SegmentBounds.resize(segCount);
    constexpr uint32 kAABBSamples = 8;

    for (uint32 seg = 0; seg < segCount; ++seg)
    {
        V3 bmin{std::numeric_limits<float32>::max(),
                std::numeric_limits<float32>::max(),
                std::numeric_limits<float32>::max()};
        V3 bmax{-std::numeric_limits<float32>::max(),
                -std::numeric_limits<float32>::max(),
                -std::numeric_limits<float32>::max()};

        for (uint32 s = 0; s <= kAABBSamples; ++s)
        {
            float32 t = static_cast<float32>(s) / static_cast<float32>(kAABBSamples);
            V3 p = EvaluateSegment(spline, seg, t);
            bmin.x = std::min(bmin.x, p.x);
            bmin.y = std::min(bmin.y, p.y);
            bmin.z = std::min(bmin.z, p.z);
            bmax.x = std::max(bmax.x, p.x);
            bmax.y = std::max(bmax.y, p.y);
            bmax.z = std::max(bmax.z, p.z);
        }

        // Expand by max radius in this segment to account for swept volume.
        uint32 n = static_cast<uint32>(spline.Points.size());
        uint32 i0 = seg;
        uint32 i1 = spline.IsEffectivelyClosed() ? ((seg + 1) % n) : std::min(seg + 1, n - 1);
        float32 maxRadius = std::max(spline.Points[i0].Radius, spline.Points[i1].Radius);
        bmin.x -= maxRadius; bmin.y -= maxRadius; bmin.z -= maxRadius;
        bmax.x += maxRadius; bmax.y += maxRadius; bmax.z += maxRadius;

        spline.SegmentBounds[seg] = {bmin, bmax};
    }

    // Rebuild arc-length LUT.
    const uint32 totalSamples = spline.ArcLengthSamples;
    spline.ArcLengthTable.resize(totalSamples);
    spline.ArcLengthParams.resize(totalSamples);

    float32 cumLength = 0.0f;
    V3 prevPos = EvaluatePosition(spline, 0.0f);

    for (uint32 i = 0; i < totalSamples; ++i)
    {
        float32 t = static_cast<float32>(i) / static_cast<float32>(totalSamples - 1);
        V3 pos = EvaluatePosition(spline, t);

        if (i > 0)
        {
            V3 diff = pos - prevPos;
            cumLength += std::sqrt(V3::Dot(diff, diff));
        }

        spline.ArcLengthTable[i] = cumLength;
        spline.ArcLengthParams[i] = t;
        prevPos = pos;
    }

    spline.TotalArcLength = cumLength;

    // Build polygon cache for closed splines (used for interior point-in-polygon tests).
    // Scale sample count with segment count to maintain accuracy for complex shapes.
    // The effective-closed predicate already guarantees three segments here, so
    // the polygon can never be a zero-area A->B->A doubling.
    if (spline.IsEffectivelyClosed())
    {
        constexpr uint32 kSamplesPerSegment = 16;
        const uint32 kPolygonSamples = std::max(16u, segCount * kSamplesPerSegment);
        spline.ClosedPolygonXZ.resize(kPolygonSamples);
        for (uint32 i = 0; i < kPolygonSamples; ++i)
        {
            float32 t = static_cast<float32>(i) / static_cast<float32>(kPolygonSamples);
            V3 pos = EvaluatePosition(spline, t);
            spline.ClosedPolygonXZ[i] = Mathematics::Vector2(pos.x, pos.z);
        }
    }
    else
    {
        spline.ClosedPolygonXZ.clear();
    }

    spline.Dirty = false;
}

void SampleUniform(const SplineData& spline, uint32 sampleCount,
                   std::vector<SplineFrame>& outFrames)
{
    outFrames.clear();
    if (!spline.IsValid() || sampleCount == 0)
        return;

    outFrames.reserve(sampleCount);

    if (spline.TotalArcLength <= 0.0f)
    {
        // No arc-length data; fall back to uniform parametric sampling.
        for (uint32 i = 0; i < sampleCount; ++i)
        {
            float32 t = static_cast<float32>(i) / static_cast<float32>(sampleCount - 1);
            outFrames.push_back(Evaluate(spline, t));
        }
        return;
    }

    float32 spacing = spline.TotalArcLength / static_cast<float32>(sampleCount - 1);
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        float32 dist = static_cast<float32>(i) * spacing;
        outFrames.push_back(EvaluateAtDistance(spline, dist));
    }
}

} // namespace GameEngine::Spline
