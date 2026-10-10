#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Spline/SplineTypes.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Spline;
using namespace GameEngine::Mathematics;

namespace
{

SplineData MakeLinearSpline(Vector3 a, Vector3 b)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint(a);
    s.AddPoint(b);
    RebuildSplineCache(s);
    return s;
}

SplineData MakeLinear3Point(Vector3 a, Vector3 b, Vector3 c)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint(a);
    s.AddPoint(b);
    s.AddPoint(c);
    RebuildSplineCache(s);
    return s;
}

SplineData MakeCatmullRom4Point()
{
    SplineData s;
    s.Type = SplineType::CatmullRom;
    s.AddPoint({0, 0, 0});
    s.AddPoint({10, 0, 0});
    s.AddPoint({20, 0, 10});
    s.AddPoint({30, 0, 10});
    RebuildSplineCache(s);
    return s;
}

constexpr float32 kEps = 0.01f;

void ExpectNear(Vector3 a, Vector3 b, float32 tol = kEps)
{
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

} // namespace

// ---- Basic evaluation ----

TEST(SplineEvaluator, LinearEvaluateEndpoints)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    ExpectNear(EvaluatePosition(s, 0.0f), {0, 0, 0});
    ExpectNear(EvaluatePosition(s, 1.0f), {10, 0, 0});
}

TEST(SplineEvaluator, LinearEvaluateMidpoint)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    ExpectNear(EvaluatePosition(s, 0.5f), {5, 0, 0});
}

TEST(SplineEvaluator, Linear3PointMidSegment)
{
    auto s = MakeLinear3Point({0, 0, 0}, {10, 0, 0}, {10, 0, 10});

    // t=0.25 is midpoint of first segment (0→10 on X)
    ExpectNear(EvaluatePosition(s, 0.25f), {5, 0, 0});

    // t=0.75 is midpoint of second segment (10,0,0 → 10,0,10)
    ExpectNear(EvaluatePosition(s, 0.75f), {10, 0, 5});
}

TEST(SplineEvaluator, LinearFrameForward)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    auto frame = Evaluate(s, 0.5f);
    ExpectNear(frame.Forward, {1, 0, 0});
}

// ---- CatmullRom ----

TEST(SplineEvaluator, CatmullRomEndpoints)
{
    auto s = MakeCatmullRom4Point();
    // CatmullRom passes through control points.
    ExpectNear(EvaluatePosition(s, 0.0f), {0, 0, 0}, 0.1f);
    ExpectNear(EvaluatePosition(s, 1.0f), {30, 0, 10}, 0.1f);
}

TEST(SplineEvaluator, CatmullRomSmoothInterpolation)
{
    auto s = MakeCatmullRom4Point();
    // At the second control point (t ≈ 0.333), should be near (10, 0, 0).
    Vector3 p = EvaluatePosition(s, 1.0f / 3.0f);
    EXPECT_NEAR(p.x, 10.0f, 1.0f);
    EXPECT_NEAR(p.y, 0.0f, 0.1f);
}

// ---- Arc length ----

TEST(SplineEvaluator, ArcLengthLinear)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    EXPECT_NEAR(s.TotalArcLength, 10.0f, 0.1f);
}

TEST(SplineEvaluator, DistanceToParametricLinear)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    EXPECT_NEAR(DistanceToParametric(s, 5.0f), 0.5f, 0.02f);
    EXPECT_NEAR(DistanceToParametric(s, 0.0f), 0.0f, 0.01f);
    EXPECT_NEAR(DistanceToParametric(s, 10.0f), 1.0f, 0.01f);
}

TEST(SplineEvaluator, ParametricToDistanceLinear)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    EXPECT_NEAR(ParametricToDistance(s, 0.5f), 5.0f, 0.1f);
}

TEST(SplineEvaluator, EvaluateAtDistanceLinear)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    auto frame = EvaluateAtDistance(s, 5.0f);
    ExpectNear(frame.Position, {5, 0, 0}, 0.2f);
}

// ---- Closest point ----

TEST(SplineEvaluator, ClosestPointOnLine)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    auto result = FindClosestPoint(s, {5, 5, 0});
    EXPECT_NEAR(result.Position.x, 5.0f, 0.5f);
    EXPECT_NEAR(result.Position.y, 0.0f, 0.1f);
    EXPECT_NEAR(result.Distance, 5.0f, 0.5f);
}

TEST(SplineEvaluator, ClosestPointBeyondEnd)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    auto result = FindClosestPoint(s, {15, 0, 0});
    // Should snap to end of spline
    EXPECT_NEAR(result.Position.x, 10.0f, 0.5f);
}

// ---- Signed distance ----

TEST(SplineEvaluator, SignedDistanceInside)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    // Default radius is 5.0. Point at (5, 2, 0) is 2 units from spline, inside the 5-unit radius.
    float32 sd = SignedDistanceToSpline(s, {5, 2, 0});
    EXPECT_LT(sd, 0.0f); // Inside
}

TEST(SplineEvaluator, SignedDistanceOutside)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    // Point at (5, 10, 0) is 10 units from spline, outside the 5-unit radius.
    float32 sd = SignedDistanceToSpline(s, {5, 10, 0});
    EXPECT_GT(sd, 0.0f); // Outside
}

// ---- Ground-plane (XZ) queries ----
//
// The XZ variants exist for top-down footprints: the curve's altitude must not
// enter the distance, while its Y stays readable as data.

TEST(SplineEvaluator, ClosestPointXZIgnoresAltitudeAndReportsTheCurveY)
{
    // A path 20 units up. In 3-D nothing on the ground is near it; in XZ the
    // point directly below x = 5 sits on it.
    auto s = MakeLinearSpline({0, 20, 0}, {10, 20, 0});

    auto xz = FindClosestPointXZ(s, 5.0f, 0.0f);
    EXPECT_NEAR(xz.Distance, 0.0f, kEps);
    EXPECT_NEAR(xz.Position.x, 5.0f, 0.5f);
    EXPECT_NEAR(xz.Position.y, 20.0f, kEps) << "the XZ query dropped the curve's height";

    // The 3-D query is unchanged and still measures the full separation.
    EXPECT_NEAR(FindClosestPoint(s, {5, 0, 0}).Distance, 20.0f, 0.5f);
}

TEST(SplineEvaluator, SignedDistanceXZIsInsideTheFootprintOfAnElevatedPath)
{
    // Default radius is 5. (5, 2) is 2 units from the path in XZ — inside the
    // footprint — however high the path is authored.
    auto s = MakeLinearSpline({0, 20, 0}, {10, 20, 0});

    EXPECT_LT(SignedDistanceToSplineXZ(s, 5.0f, 2.0f), 0.0f);
    EXPECT_GT(SignedDistanceToSplineXZ(s, 5.0f, 9.0f), 0.0f); // 9 > radius 5
    EXPECT_GT(SignedDistanceToSpline(s, {5, 0, 2}), 0.0f);    // 3-D: 20 up, outside
}

TEST(SplineEvaluator, ClosestPointXZDoesNotCullTheNearestSegmentByAltitude)
{
    // Segment 2 runs entirely at y = 100 and is the XZ-nearest to (15, 0);
    // segments 0-1 are 9 units away in XZ but sit on the ground. A broad phase
    // that measured segment AABBs in 3-D would reject segment 2 (95 units up,
    // versus a running best of 9) and answer with the ground segment instead.
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({0, 0, 0});
    s.AddPoint({6, 0, 0});
    s.AddPoint({6, 100, 0});
    s.AddPoint({20, 100, 0});
    RebuildSplineCache(s);

    auto xz = FindClosestPointXZ(s, 15.0f, 0.0f);
    EXPECT_NEAR(xz.Distance, 0.0f, 0.3f);
    EXPECT_NEAR(xz.Position.x, 15.0f, 0.3f);
    EXPECT_NEAR(xz.Position.y, 100.0f, 0.3f) << "the broad phase culled the XZ-nearest segment";
}

TEST(SplineEvaluator, SignedDistanceXZIsNegativeInsideAnElevatedClosedLoop)
{
    // A closed loop 50 units up: everything it encloses is inside its footprint,
    // and the falloff band outside it is measured in XZ like the rest.
    SplineData s;
    s.Type = SplineType::Linear;
    s.Closed = true;
    s.AddPoint({-20, 50, -20});
    s.AddPoint({20, 50, -20});
    s.AddPoint({20, 50, 20});
    s.AddPoint({-20, 50, 20});
    RebuildSplineCache(s);

    EXPECT_LT(SignedDistanceToSplineXZ(s, 0.0f, 0.0f), 0.0f) << "loop interior read as outside";
    // Just past the edge + radius 5: outside, but by a measurable XZ margin
    // rather than the 50 units of altitude.
    const float32 outside = SignedDistanceToSplineXZ(s, 30.0f, 0.0f);
    EXPECT_GT(outside, 0.0f);
    EXPECT_NEAR(outside, 5.0f, 0.5f); // 10 from the edge, less the 5 radius
}

// ---- Edge cases ----

TEST(SplineEvaluator, InvalidSpline)
{
    SplineData s;
    s.Type = SplineType::Linear;
    // 0 points — should not crash.
    auto frame = Evaluate(s, 0.5f);
    EXPECT_EQ(frame.Radius, 0.0f);
}

TEST(SplineEvaluator, SinglePoint)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({5, 0, 0});
    // 1 point — invalid but should not crash.
    EXPECT_FALSE(s.IsValid());
}

TEST(SplineEvaluator, ClosedSpline)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.Closed = true;
    s.AddPoint({0, 0, 0});
    s.AddPoint({10, 0, 0});
    s.AddPoint({10, 0, 10});
    RebuildSplineCache(s);

    // 3 points closed = 3 segments. Last segment wraps from (10,0,10) back to (0,0,0).
    EXPECT_EQ(s.GetSegmentCount(), 3u);

    // At t=1.0 we should be back at the start (or near it).
    ExpectNear(EvaluatePosition(s, 0.0f), {0, 0, 0}, 0.1f);
}

// ---- SampleUniform ----

TEST(SplineEvaluator, SampleUniformCount)
{
    auto s = MakeLinearSpline({0, 0, 0}, {10, 0, 0});
    std::vector<SplineFrame> frames;
    SampleUniform(s, 11, frames);
    EXPECT_EQ(frames.size(), 11u);

    // First and last should be at endpoints.
    ExpectNear(frames.front().Position, {0, 0, 0}, 0.2f);
    ExpectNear(frames.back().Position, {10, 0, 0}, 0.2f);
}

// ---- Radius interpolation ----

TEST(SplineEvaluator, RadiusInterpolation)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({0, 0, 0}, 2.0f);
    s.AddPoint({10, 0, 0}, 8.0f);
    RebuildSplineCache(s);

    auto frame = Evaluate(s, 0.5f);
    EXPECT_NEAR(frame.Radius, 5.0f, 0.1f);
}

// ---- Channel sampling (facade over per-point Radius) ----

TEST(SplineEvaluator, SampleChannelWidthAtKnotsAndMidArc)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({0, 0, 0}, 2.0f);
    s.AddPoint({10, 0, 0}, 8.0f);
    RebuildSplineCache(s);

    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 0.0f, -1.0f), 2.0f, 0.05f);
    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 5.0f, -1.0f), 5.0f, 0.05f);
    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 10.0f, -1.0f), 8.0f, 0.05f);
    // Quarter-arc: linear between the knot values, not through the curve basis.
    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 2.5f, -1.0f), 3.5f, 0.05f);
}

TEST(SplineEvaluator, SampleChannelWidthMultiSegment)
{
    // Two 10-unit segments with radii 2 -> 4 -> 8: mid-arc of each segment
    // interpolates that segment's endpoints only.
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({0, 0, 0}, 2.0f);
    s.AddPoint({10, 0, 0}, 4.0f);
    s.AddPoint({20, 0, 0}, 8.0f);
    RebuildSplineCache(s);

    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 5.0f, -1.0f), 3.0f, 0.05f);
    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 10.0f, -1.0f), 4.0f, 0.05f);
    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 15.0f, -1.0f), 6.0f, 0.05f);
}

TEST(SplineEvaluator, SampleChannelClampsPastTheEnds)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({0, 0, 0}, 2.0f);
    s.AddPoint({10, 0, 0}, 8.0f);
    RebuildSplineCache(s);

    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, -5.0f, -1.0f), 2.0f, 0.05f);
    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth, 50.0f, -1.0f), 8.0f, 0.05f);
}

TEST(SplineEvaluator, SampleChannelUnknownNameReturnsFallback)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({0, 0, 0}, 2.0f);
    s.AddPoint({10, 0, 0}, 8.0f);
    RebuildSplineCache(s);

    EXPECT_EQ(SampleChannelAtDistance(s, HashStringId("speed"), 5.0f, 7.5f), 7.5f);
}

TEST(SplineEvaluator, SampleChannelInvalidSplineReturnsFallback)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({5, 0, 0}, 2.0f); // 1 point — invalid
    EXPECT_EQ(SampleChannelAtDistance(s, SplineChannels::kWidth, 0.0f, 3.25f), 3.25f);
}

// ---- Width envelope (the gizmo offsets by the sampled half-width at arc distances) ----

TEST(SplineEvaluator, EnvelopeHalfWidthMatchesLerpAtMidArc)
{
    // Two knots carrying the demo's authored taper 1.5 -> 3.0: the envelope
    // half-width at mid-arc is the lerp midpoint.
    {
        SplineData s;
        s.Type = SplineType::Linear;
        s.AddPoint({0, 0, 0}, 1.5f);
        s.AddPoint({12, 0, 0}, 3.0f);
        RebuildSplineCache(s);
        EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth,
                                            0.5f * s.TotalArcLength, -1.0f),
                    2.25f, 0.05f);
    }

    // Unequal segment lengths split parametric t from arc distance: knots at
    // x = 0, 2, 20 put mid-ARC 8 m into the second segment (fraction 8/18),
    // while parametric mid sits exactly on the middle knot. Sampling must
    // resolve distance through the arc-length LUT, not t = distance / length.
    {
        SplineData s;
        s.Type = SplineType::Linear;
        s.AddPoint({0, 0, 0}, 1.5f);
        s.AddPoint({2, 0, 0}, 2.25f);
        s.AddPoint({20, 0, 0}, 3.0f);
        RebuildSplineCache(s);
        const float32 expected = 2.25f + (3.0f - 2.25f) * (8.0f / 18.0f);
        EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth,
                                            0.5f * s.TotalArcLength, -1.0f),
                    expected, 0.05f);
    }
}

TEST(SplineEvaluator, EnvelopeWidthOnCurvedSplineSamplesByArcLength)
{
    // Genuinely curved CatmullRom with the 1.5 -> 3.0 taper. The knots mirror
    // about x = 10, so both segments have equal arc length and mid-arc lands
    // exactly on the middle knot — its authored half-width, reached only
    // through the arc-length LUT on a curve where arc and parametric diverge.
    SplineData s;
    s.Type = SplineType::CatmullRom;
    s.AddPoint({0, 0, 0}, 1.5f);
    s.AddPoint({10, 0, 10}, 2.25f);
    s.AddPoint({20, 0, 0}, 3.0f);
    RebuildSplineCache(s);

    // Longer than the straight chord run (2 * sqrt(200) ~ 28.28): the spline
    // actually bows, so the LUT is doing real work.
    EXPECT_GT(s.TotalArcLength, 28.5f);
    EXPECT_NEAR(SampleChannelAtDistance(s, SplineChannels::kWidth,
                                        0.5f * s.TotalArcLength, -1.0f),
                2.25f, 0.05f);

    // Quarter-arc sits strictly inside the first segment: strictly between its
    // endpoint half-widths (also fails if a sabotaged sampler clamps to knots).
    const float32 quarter = SampleChannelAtDistance(s, SplineChannels::kWidth,
                                                    0.25f * s.TotalArcLength, -1.0f);
    EXPECT_GT(quarter, 1.5f);
    EXPECT_LT(quarter, 2.25f);
}

// ---- Degenerate inputs (NaN guards) ----
//
// Three authorable degeneracies can hand glm::normalize a vector it cannot
// normalize and poison the frame: a tangent exactly parallel to the frame's
// world-up reference (a vertical spline), a zero raw tangent from coincident
// consecutive control points (a double-added point), and a raw tangent whose
// squared length is not finite, from control points outside the usable float
// range. The last one arrives two ways: an infinite component normalizes to
// NaN, while finite components whose squares merely overflow normalize to a
// zero vector that then collapses the whole basis.

namespace
{

void ExpectFiniteOrthonormalFrame(const SplineData& s, float32 t)
{
    const SplineFrame frame = Evaluate(s, t);
    const Vector3 f = frame.Forward;
    const Vector3 r = frame.Right;
    const Vector3 u = frame.Up;

    EXPECT_TRUE(std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z))
        << "Forward at t=" << t << " = (" << f.x << ", " << f.y << ", " << f.z << ")";
    EXPECT_TRUE(std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z))
        << "Right at t=" << t << " = (" << r.x << ", " << r.y << ", " << r.z << ")";
    EXPECT_TRUE(std::isfinite(u.x) && std::isfinite(u.y) && std::isfinite(u.z))
        << "Up at t=" << t << " = (" << u.x << ", " << u.y << ", " << u.z << ")";

    constexpr float32 kBasisTol = 1.0e-3f;
    EXPECT_NEAR(Vector3::Dot(f, f), 1.0f, kBasisTol) << "|Forward|^2 at t=" << t;
    EXPECT_NEAR(Vector3::Dot(r, r), 1.0f, kBasisTol) << "|Right|^2 at t=" << t;
    EXPECT_NEAR(Vector3::Dot(u, u), 1.0f, kBasisTol) << "|Up|^2 at t=" << t;
    EXPECT_NEAR(Vector3::Dot(f, r), 0.0f, kBasisTol) << "Forward.Right at t=" << t;
    EXPECT_NEAR(Vector3::Dot(f, u), 0.0f, kBasisTol) << "Forward.Up at t=" << t;
    EXPECT_NEAR(Vector3::Dot(r, u), 0.0f, kBasisTol) << "Right.Up at t=" << t;
}

// Sweeps parameters at and around the degenerate ones: for the coincident-point
// splines below, t = 0 and t = 1/3 land inside the zero-length segment while
// the rest land in the healthy one.
void ExpectFiniteOrthonormalSweep(const SplineData& s)
{
    for (float32 t : {0.0f, 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f})
        ExpectFiniteOrthonormalFrame(s, t);
}

} // namespace

TEST(SplineEvaluator, VerticalTangentLinearFrameIsFinite)
{
    // Straight up +Y: Cross(worldUp, forward) is exactly zero at every t.
    auto s = MakeLinearSpline({0, 0, 0}, {0, 10, 0});
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, VerticalTangentBezierFrameIsFinite)
{
    // Vertical handles on vertically stacked points: the derivative is +Y
    // (nonzero) for all t in [0,1].
    SplineData s;
    s.Type = SplineType::CubicBezier;
    s.AddPoint({0, 0, 0});
    s.AddPoint({0, 10, 0});
    s.SetPointTangents(0, {0, -3, 0}, {0, 3, 0});
    s.SetPointTangents(1, {0, -3, 0}, {0, 3, 0});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, VerticalTangentCatmullRomFrameIsFinite)
{
    // All points on the Y axis: the tangent is vertical (and nonzero) at every
    // sampled t.
    SplineData s;
    s.Type = SplineType::CatmullRom;
    s.AddPoint({0, 0, 0});
    s.AddPoint({0, 5, 0});
    s.AddPoint({0, 10, 0});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, CoincidentPointsLinearTangentIsFinite)
{
    // Double-added point: segment 0 has zero length, so its raw tangent is
    // exactly zero at every t.
    auto s = MakeLinear3Point({5, 0, 5}, {5, 0, 5}, {15, 0, 5});
    ExpectFiniteOrthonormalSweep(s);

    // Pin the fallback's IDENTITY, not just finiteness: a degenerate tangent
    // falls back to +Z by contract — changing the fallback axis must be a
    // deliberate edit, not a silent one.
    const SplineFrame frame = Evaluate(s, 0.0f);
    EXPECT_NEAR(frame.Forward.z, 1.0f, 1.0e-5f);
}

TEST(SplineEvaluator, NearVerticalTangentKeepsPrimaryFrame)
{
    // 1 degree off vertical: INSIDE the old dead-code constant's cone
    // (0.001 = sin^2 of ~1.8 degrees) but four orders of magnitude above the
    // numerical floor (1e-8), so the PRIMARY up reference must produce the
    // frame. Widening kMinFrameCrossLengthSq back toward 0.001 turns this
    // red — that is the pin.
    const float32 tilt = 1.0f * 3.14159265f / 180.0f;
    auto s = MakeLinear3Point({0, 0, 0},
                              {10.0f * std::sin(tilt), 10.0f * std::cos(tilt), 0},
                              {20.0f * std::sin(tilt), 20.0f * std::cos(tilt), 0});
    const SplineFrame frame = Evaluate(s, 0.5f);
    // Primary reference is world up: Right = normalize(Cross(up, forward)),
    // which for this tilt lies along -Z (LH, +Y up, forward tilted +X from +Y).
    EXPECT_NEAR(frame.Right.z, -1.0f, 1.0e-3f);
    EXPECT_NEAR(Vector3::Dot(frame.Right, frame.Right), 1.0f, 1.0e-3f);
}

TEST(SplineEvaluator, CoincidentPointsBezierTangentIsFinite)
{
    // Double-added point with default zero handles: all four control points of
    // segment 0 coincide, so the raw derivative is zero at every t. (Zero
    // handles also zero the derivative at the exact ends of segment 1.)
    SplineData s;
    s.Type = SplineType::CubicBezier;
    s.AddPoint({5, 0, 5});
    s.AddPoint({5, 0, 5});
    s.AddPoint({15, 0, 5});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, CoincidentPointsCatmullRomTangentIsFinite)
{
    // Clamped ends make segment 0's context p0 == p1 == p2, so its raw tangent
    // is (p3 - p2) * t * (1.5t - 1): exactly zero at t = 0, and vanishingly
    // small around local t = 2/3 (global t = 1/3).
    SplineData s;
    s.Type = SplineType::CatmullRom;
    s.AddPoint({5, 0, 5});
    s.AddPoint({5, 0, 5});
    s.AddPoint({15, 0, 5});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

namespace
{
constexpr float32 kInf = std::numeric_limits<float32>::infinity();

// Squaring this overflows fp32 (max ~3.4e38) while the value itself stays a
// perfectly ordinary float, so the raw tangent is finite but its squared
// length is not.
constexpr float32 kOverflowingCoord = 1.0e30f;
} // namespace

TEST(SplineEvaluator, InfiniteControlPointLinearTangentIsFinite)
{
    // An infinite coordinate makes the raw tangent infinite, so its squared
    // length is infinite too: a threshold test alone accepts that as "long
    // enough" and normalizes it into NaN.
    auto s = MakeLinear3Point({0, 0, 0}, {kInf, 0, 0}, {20, 0, 0});
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, InfiniteControlPointBezierTangentIsFinite)
{
    SplineData s;
    s.Type = SplineType::CubicBezier;
    s.AddPoint({0, 0, 0});
    s.AddPoint({kInf, 0, 0});
    s.SetPointTangents(0, {-3, 0, 0}, {3, 0, 0});
    s.SetPointTangents(1, {-3, 0, 0}, {3, 0, 0});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, InfiniteControlPointCatmullRomTangentIsFinite)
{
    SplineData s;
    s.Type = SplineType::CatmullRom;
    s.AddPoint({0, 0, 0});
    s.AddPoint({kInf, 0, 0});
    s.AddPoint({20, 0, 0});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, OverflowingControlPointLinearTangentIsFinite)
{
    // Finite coordinates, infinite squared length: normalizing yields a zero
    // vector rather than a NaN, which reads as a valid unit forward and then
    // collapses Right and Up into NaN inside the frame build.
    auto s = MakeLinear3Point({0, 0, 0},
                              {kOverflowingCoord, 0, 0},
                              {2.0f * kOverflowingCoord, 0, 0});
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, OverflowingControlPointBezierTangentIsFinite)
{
    SplineData s;
    s.Type = SplineType::CubicBezier;
    s.AddPoint({0, 0, 0});
    s.AddPoint({kOverflowingCoord, 0, 0});
    s.SetPointTangents(0, {-3, 0, 0}, {kOverflowingCoord, 0, 0});
    s.SetPointTangents(1, {-kOverflowingCoord, 0, 0}, {3, 0, 0});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluator, OverflowingControlPointCatmullRomTangentIsFinite)
{
    // The Catmull-Rom tangent is a scaled difference of the surrounding
    // points, so at these coordinates it overflows at mid-segment too, not
    // only at the ends.
    SplineData s;
    s.Type = SplineType::CatmullRom;
    s.AddPoint({0, 0, 0});
    s.AddPoint({kOverflowingCoord, 0, 0});
    s.AddPoint({2.0f * kOverflowingCoord, 0, 0});
    RebuildSplineCache(s);
    ExpectFiniteOrthonormalSweep(s);
}

// Control: coordinates far larger than any real scene, but whose squares stay
// inside fp32, so the raw tangent is perfectly healthy. Rejecting this would
// mean the finite test had started swallowing working input, and it is the
// only test here that must pass on both sides of the guard change.
TEST(SplineEvaluator, LargeButFiniteControlPointsKeepTheirFrame)
{
    constexpr float32 kLarge = 1.0e10f; // squared = 1e20, comfortably finite
    auto s = MakeLinear3Point({0, 0, 0}, {kLarge, 0, 0}, {2.0f * kLarge, 0, 0});
    ExpectFiniteOrthonormalSweep(s);

    const SplineFrame frame = Evaluate(s, 0.5f);
    EXPECT_NEAR(frame.Forward.x, 1.0f, 1.0e-5f);
}
