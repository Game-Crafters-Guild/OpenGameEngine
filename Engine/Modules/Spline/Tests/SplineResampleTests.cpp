#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Spline/SplineTypes.h"
#include "Spline/SplineUtility.h"

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Spline;
using namespace GameEngine::Mathematics;

namespace
{

// The editor resample pipeline: uniform frames, RDP on the sampled positions,
// point-list rewrite from the kept frames. Returns the kept indices so tests
// can map each rebuilt point back to its source arc distance.
std::vector<uint32> Resample(SplineData& data, float32 tolerance, uint32 sampleCount = 256)
{
    std::vector<SplineFrame> frames;
    SampleUniform(data, sampleCount, frames);
    EXPECT_GE(frames.size(), 2u);

    std::vector<Vector3> positions;
    positions.reserve(frames.size());
    for (const SplineFrame& frame : frames)
        positions.push_back(frame.Position);

    std::vector<uint32> kept;
    SimplifyPolylineIndices(positions, tolerance, kept);
    EXPECT_GE(kept.size(), 2u);

    std::vector<SplineFrame> keptFrames;
    keptFrames.reserve(kept.size());
    for (uint32 index : kept)
        keptFrames.push_back(frames[index]);

    data.ReplacePointsFromFrames(keptFrames);
    RebuildSplineCache(data);
    return kept;
}

void ExpectNear(Vector3 a, Vector3 b, float32 tol, const char* what)
{
    EXPECT_NEAR(a.x, b.x, tol) << what;
    EXPECT_NEAR(a.y, b.y, tol) << what;
    EXPECT_NEAR(a.z, b.z, tol) << what;
}

} // namespace

// A zigzag Linear spline with distinct per-point Radius, Roll, Rotation and
// Scale, resampled at a tolerance that keeps every corner: the rebuilt points
// must carry the authored profile, not a constant. Total length 30 with 256
// samples puts a sample exactly on each corner (10 and 20 are multiples of
// 30/255 * 85), so the kept points land on the authored knots.
TEST(SplineResample, ChannelsSurviveResample)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.AddPoint({0, 0, 0}, 2.0f);
    s.AddPoint({10, 0, 0}, 6.0f);
    s.AddPoint({10, 0, 10}, 3.0f);
    s.AddPoint({20, 0, 10}, 9.0f);
    const float32 rolls[] = {0.1f, -0.4f, 0.9f, 0.25f};
    const Vector3 rotations[] = {{0, 0, 0}, {30, 10, 0}, {0, 45, 5}, {15, 0, 60}};
    const Vector3 scales[] = {{1, 1, 1}, {2, 1, 3}, {1.5f, 2, 1}, {3, 3, 3}};
    for (uint32 i = 0; i < 4; ++i)
    {
        s.SetPointRoll(i, rolls[i]);
        s.SetPointRotation(i, rotations[i]);
        s.SetPointScale(i, scales[i]);
    }
    RebuildSplineCache(s);
    const SplineData authored = s;

    Resample(s, 0.05f);

    // Straight segments simplify away; the corners and endpoints survive.
    ASSERT_EQ(s.Points.size(), 4u);

    // The sample grid quantizes corner positions to ~0.12 m; channels lerp
    // over that error, so the tolerances are grid-scale, not exact.
    constexpr float32 kPosTol = 0.2f;
    constexpr float32 kChannelTol = 0.35f;
    for (uint32 i = 0; i < 4; ++i)
    {
        ExpectNear(s.Points[i].Position, authored.Points[i].Position, kPosTol, "Position");
        EXPECT_NEAR(s.Points[i].Radius, authored.Points[i].Radius, kChannelTol) << "Radius " << i;
        EXPECT_NEAR(s.Points[i].Roll, authored.Points[i].Roll, kChannelTol) << "Roll " << i;
        ExpectNear(s.Points[i].Rotation, authored.Points[i].Rotation, 3.0f, "Rotation");
        ExpectNear(s.Points[i].Scale, authored.Points[i].Scale, kChannelTol, "Scale");
    }

    // The historical failure mode: every radius flattened to one value.
    float32 minRadius = s.Points[0].Radius;
    float32 maxRadius = s.Points[0].Radius;
    for (const SplineControlPoint& p : s.Points)
    {
        minRadius = std::min(minRadius, p.Radius);
        maxRadius = std::max(maxRadius, p.Radius);
    }
    EXPECT_GT(maxRadius - minRadius, 1.0f) << "resample flattened the radius profile";
}

// On a curved spline the kept points fall between knots; each rebuilt point's
// Radius must match the width channel sampled at that point's arc distance on
// the ORIGINAL spline — interpolated preservation, not just knot copying.
TEST(SplineResample, CurvedSplinePreservesInterpolatedWidth)
{
    SplineData s;
    s.Type = SplineType::CatmullRom;
    s.AddPoint({0, 0, 0}, 1.5f);
    s.AddPoint({10, 0, 10}, 4.5f);
    s.AddPoint({20, 0, 0}, 2.0f);
    s.AddPoint({30, 0, 10}, 6.0f);
    RebuildSplineCache(s);
    const SplineData authored = s;

    constexpr uint32 kSampleCount = 256;
    const std::vector<uint32> kept = Resample(s, 0.1f, kSampleCount);
    ASSERT_GE(kept.size(), 4u) << "tolerance kept too few points to test interpolation";

    const float32 spacing = authored.TotalArcLength / static_cast<float32>(kSampleCount - 1);
    for (size_t i = 0; i < kept.size(); ++i)
    {
        const float32 distance = static_cast<float32>(kept[i]) * spacing;
        const float32 expected =
            SampleChannelAtDistance(authored, SplineChannels::kWidth, distance, -1.0f);
        EXPECT_NEAR(s.Points[i].Radius, expected, 0.05f) << "kept index " << kept[i];
    }
}

// Resampling a Bezier spline fabricates smooth tangent handles for the new
// points; zero handles would collapse every segment to a straight line.
TEST(SplineResample, BezierResampleFabricatesTangents)
{
    SplineData s;
    s.Type = SplineType::CubicBezier;
    s.AddPoint({0, 0, 0}, 2.0f);
    s.AddPoint({10, 0, 10}, 5.0f);
    s.AddPoint({20, 0, 0}, 3.0f);
    s.SetPointTangents(0, {0, 0, -3}, {0, 0, 3});
    s.SetPointTangents(1, {-3, 0, 0}, {3, 0, 0});
    s.SetPointTangents(2, {0, 0, 3}, {0, 0, -3});
    RebuildSplineCache(s);

    Resample(s, 0.1f);
    ASSERT_GE(s.Points.size(), 3u);

    for (size_t i = 1; i + 1 < s.Points.size(); ++i)
    {
        const Vector3& out = s.Points[i].TangentOut;
        const float32 lenSq = Vector3::Dot(out, out);
        EXPECT_GT(lenSq, 1.0e-4f) << "interior point " << i << " has a degenerate handle";
        ExpectNear(s.Points[i].TangentIn, out * -1.0f, 1.0e-4f, "TangentIn == -TangentOut");
    }
}

// The index-emitting simplifier is the primitive; the position form must be
// exactly its gather, with endpoints always kept and indices increasing.
TEST(SplineResample, SimplifyPolylineIndicesMatchesPositionForm)
{
    std::vector<Vector3> input;
    for (uint32 i = 0; i <= 40; ++i)
    {
        const float32 x = static_cast<float32>(i) * 0.5f;
        const float32 z = ((i / 10) % 2 == 0) ? static_cast<float32>(i % 10) : static_cast<float32>(10 - i % 10);
        input.push_back({x, 0.0f, z});
    }

    std::vector<uint32> kept;
    SimplifyPolylineIndices(input, 0.25f, kept);
    ASSERT_GE(kept.size(), 2u);
    EXPECT_EQ(kept.front(), 0u);
    EXPECT_EQ(kept.back(), 40u);
    for (size_t i = 1; i < kept.size(); ++i)
        EXPECT_LT(kept[i - 1], kept[i]);

    std::vector<Vector3> simplified;
    SimplifyPolyline(input, 0.25f, simplified);
    ASSERT_EQ(simplified.size(), kept.size());
    for (size_t i = 0; i < kept.size(); ++i)
        ExpectNear(simplified[i], input[kept[i]], 0.0f, "gathered position");
}
