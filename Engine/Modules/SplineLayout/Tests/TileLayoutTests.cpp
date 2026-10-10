#include "Components/Transform.h"
#include "SplineLayout/PieceAxis.h"
#include "SplineLayout/PieceEntity.h"
#include "SplineLayout/TileLayout.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using namespace GameEngine;
using GameEngine::SplineLayout::BuildTilePoses;
using GameEngine::SplineLayout::CenterSample;
using GameEngine::SplineLayout::ChoosePieceAxis;
using GameEngine::SplineLayout::HoldSurfaceAcrossGaps;
using GameEngine::SplineLayout::NormalizedOrFallback;
using GameEngine::SplineLayout::PieceAxis;
using GameEngine::SplineLayout::StationSurfaceProbe;
using GameEngine::SplineLayout::TileLayoutParams;
using GameEngine::SplineLayout::TilePose;
using V3 = GameEngine::Mathematics::Vector3;

namespace
{

std::vector<CenterSample> StraightLineZ(float32 length, uint32 sampleCount,
                                        V3 normal = V3(0.0f, 1.0f, 0.0f))
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        CenterSample s;
        s.Pos = V3(0.0f, 0.0f, length * static_cast<float32>(i) /
                                   static_cast<float32>(sampleCount - 1));
        s.Normal = normal;
        center.push_back(s);
    }
    return center;
}

void ExpectUnit(const V3& v, const char* what)
{
    EXPECT_NEAR(V3::Dot(v, v), 1.0f, 1.0e-4f) << what;
}

void ExpectOrthonormal(const TilePose& pose)
{
    ExpectUnit(pose.Right, "Right");
    ExpectUnit(pose.Up, "Up");
    ExpectUnit(pose.Forward, "Forward");
    EXPECT_NEAR(V3::Dot(pose.Right, pose.Up), 0.0f, 1.0e-4f);
    EXPECT_NEAR(V3::Dot(pose.Right, pose.Forward), 0.0f, 1.0e-4f);
    EXPECT_NEAR(V3::Dot(pose.Up, pose.Forward), 0.0f, 1.0e-4f);
}

} // namespace

// ---- Degenerate inputs ----

TEST(TileLayout, EmptyAndSingleSampleCenterlinesProduceNoPoses)
{
    TileLayoutParams params;
    EXPECT_TRUE(BuildTilePoses({}, params).empty());

    std::vector<CenterSample> one(1, CenterSample{V3(0, 0, 0), V3(0, 1, 0)});
    EXPECT_TRUE(BuildTilePoses(one, params).empty());
}

TEST(TileLayout, ZeroLengthCenterlineProducesNoPoses)
{
    std::vector<CenterSample> center(3, CenterSample{V3(5, 0, 5), V3(0, 1, 0)});
    TileLayoutParams params;
    EXPECT_TRUE(BuildTilePoses(center, params).empty());
}

TEST(TileLayout, VerticalCenterlineFallsBackToPlusZWithoutNaNs)
{
    // A centerline straight up has no ground-plane travel direction at all.
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < 5; ++i)
        center.push_back({V3(0.0f, static_cast<float32>(i), 0.0f), V3(0, 1, 0)});

    TileLayoutParams params;
    params.Spacing = 1.0f;
    const auto poses = BuildTilePoses(center, params);
    ASSERT_FALSE(poses.empty());
    for (const TilePose& pose : poses)
    {
        ExpectOrthonormal(pose);
        EXPECT_NEAR(pose.Forward.z, 1.0f, 1.0e-4f);
        EXPECT_TRUE(std::isfinite(pose.Position.x) && std::isfinite(pose.Position.y) &&
                    std::isfinite(pose.Position.z));
    }
}

// ---- Station generation ----

TEST(TileLayout, FitToLengthLandsStationsOnBothEnds)
{
    // Length 9, spacing 2: lround(4.5) + 1 = 6 stations, evenly spread 0..9.
    const auto center = StraightLineZ(9.0f, 10);
    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.Fit = Components::SplinePlacementFit::FitToLength;
    const auto poses = BuildTilePoses(center, params);
    ASSERT_EQ(poses.size(), 6u);
    EXPECT_NEAR(poses.front().Position.z, 0.0f, 1.0e-4f);
    EXPECT_NEAR(poses.back().Position.z, 9.0f, 1.0e-4f);
    // Uniform pitch of length / (count - 1).
    for (size_t i = 1; i < poses.size(); ++i)
        EXPECT_NEAR(poses[i].Position.z - poses[i - 1].Position.z, 9.0f / 5.0f, 1.0e-3f);
}

TEST(TileLayout, FixedPitchCutsThePartialStep)
{
    // Length 9, spacing 2: stations at 0,2,4,6,8 — the last partial step dies.
    const auto center = StraightLineZ(9.0f, 10);
    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.Fit = Components::SplinePlacementFit::FixedPitch;
    const auto poses = BuildTilePoses(center, params);
    ASSERT_EQ(poses.size(), 5u);
    for (size_t i = 0; i < poses.size(); ++i)
        EXPECT_NEAR(poses[i].Position.z, 2.0f * static_cast<float32>(i), 1.0e-3f);
}

TEST(TileLayout, MaxTilesClampsBothFitModes)
{
    const auto center = StraightLineZ(100.0f, 101);
    TileLayoutParams params;
    params.Spacing = 1.0f;
    params.MaxTiles = 4;

    params.Fit = Components::SplinePlacementFit::FitToLength;
    EXPECT_EQ(BuildTilePoses(center, params).size(), 4u);
    params.Fit = Components::SplinePlacementFit::FixedPitch;
    EXPECT_EQ(BuildTilePoses(center, params).size(), 4u);
}

// ---- Chord yaw ----

TEST(TileLayout, CornerStationYawsAlongTheChordNotTheSegmentTangent)
{
    // An L in the ground plane: +Z for 10 m, then +X for 10 m. The corner
    // station's forward must follow the chord across its own footprint
    // (bisecting the turn at 45 degrees), not either segment's tangent.
    std::vector<CenterSample> center;
    for (uint32 i = 0; i <= 10; ++i)
        center.push_back({V3(0.0f, 0.0f, static_cast<float32>(i)), V3(0, 1, 0)});
    for (uint32 i = 1; i <= 10; ++i)
        center.push_back({V3(static_cast<float32>(i), 0.0f, 10.0f), V3(0, 1, 0)});

    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.Fit = Components::SplinePlacementFit::FixedPitch;
    params.MeshBoundsHalfExtents = V3(1.0f, 0.5f, 1.0f);
    const auto poses = BuildTilePoses(center, params);

    // Station 10 m in sits exactly on the corner; its chord runs from 9 m to
    // 11 m along the path: (0,0,9) -> (1,0,10), i.e. 45 degrees right.
    ASSERT_GE(poses.size(), 6u);
    const TilePose& corner = poses[5];
    const float32 inv = 1.0f / std::sqrt(2.0f);
    EXPECT_NEAR(corner.Forward.x, inv, 1.0e-3f);
    EXPECT_NEAR(corner.Forward.z, inv, 1.0e-3f);
    // Straight-run stations are unaffected.
    EXPECT_NEAR(poses[2].Forward.z, 1.0f, 1.0e-3f);
    EXPECT_NEAR(poses[2].Forward.x, 0.0f, 1.0e-3f);
}

// ---- Surface alignment ----

TEST(TileLayout, SlopeBlendInterpolatesUpTowardTheSurfaceNormal)
{
    const float32 inv = 1.0f / std::sqrt(2.0f);
    const V3 slopeNormal(inv, inv, 0.0f); // 45-degree bank toward +X
    const auto center = StraightLineZ(10.0f, 11, slopeNormal);

    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.AlignToSurfaceNormal = true;
    // The blend is what this test names; the tilt ceiling is a separate rule
    // with its own tests, and the default 35 would hold this 45-degree bank.
    params.MaxTiltDegrees = 90.0f;

    params.SlopeBlend = 1.0f;
    for (const TilePose& pose : BuildTilePoses(center, params))
    {
        ExpectOrthonormal(pose);
        EXPECT_NEAR(pose.Up.x, inv, 1.0e-3f);
        EXPECT_NEAR(pose.Up.y, inv, 1.0e-3f);
    }

    params.SlopeBlend = 0.0f;
    for (const TilePose& pose : BuildTilePoses(center, params))
        EXPECT_NEAR(pose.Up.y, 1.0f, 1.0e-4f);

    params.AlignToSurfaceNormal = false;
    params.SlopeBlend = 1.0f;
    for (const TilePose& pose : BuildTilePoses(center, params))
        EXPECT_NEAR(pose.Up.y, 1.0f, 1.0e-4f);
}

// ---- Pivot compensation ----

TEST(TileLayout, CornerPivotedMeshIsFootprintCenteredAndBaseLifted)
{
    // A Synty-style corner pivot: bounds center (1.25, 0.5, 1.25) with half
    // extents (1.25, 0.5, 1.25) — base at y=0. On a straight +Z line the
    // origin must land at centerline - Right*1.25 - Forward*1.25, and the
    // base lift (halfExtents.y - center.y = 0) must vanish.
    const auto center = StraightLineZ(10.0f, 11);
    TileLayoutParams params;
    params.Spacing = 5.0f;
    params.MeshBoundsCenter = V3(1.25f, 0.5f, 1.25f);
    params.MeshBoundsHalfExtents = V3(1.25f, 0.5f, 1.25f);

    const auto poses = BuildTilePoses(center, params);
    ASSERT_EQ(poses.size(), 3u);
    const TilePose& mid = poses[1];
    EXPECT_NEAR(mid.Position.x, -1.25f, 1.0e-3f); // -Right * center.x
    EXPECT_NEAR(mid.Position.y, 0.0f, 1.0e-3f);   // base-pivoted: no lift
    EXPECT_NEAR(mid.Position.z, 5.0f - 1.25f, 1.0e-3f);

    // Center-pivoted in Y (center.y = 0): lift by halfExtents.y so the lowest
    // vertex lands on the surface.
    params.MeshBoundsCenter = V3(0.0f, 0.0f, 0.0f);
    const auto lifted = BuildTilePoses(center, params);
    ASSERT_EQ(lifted.size(), 3u);
    EXPECT_NEAR(lifted[1].Position.y, 0.5f, 1.0e-3f);
}

// ---- Conform-ray gaps ----
//
// Tile stations come from the 3D length of the draped polyline, so a sample
// that kept its authored altitude because no ray hit anything is measured as
// length. The reviewed case: a 40 m centerline authored 20 m above flat ground,
// dense-sampled at the controller's pinned step (82 samples), draped to y = 0
// everywhere except one interior miss. That one spike measures 79.02 m and
// doubles the tile count.

namespace
{

std::vector<CenterSample> DrapedWithGaps(const std::vector<size_t>& gapIndices, float32 groundY,
                                         bool markGapsUnmeasured)
{
    constexpr float32 kLength = 40.0f;
    constexpr uint32 kDenseSamples = 82; // CenterlineSampleCount(40 m)
    constexpr float32 kAuthoredAltitude = 20.0f;

    auto center = StraightLineZ(kLength, kDenseSamples);
    for (CenterSample& s : center)
        s.Pos.y = groundY;
    for (const size_t i : gapIndices)
    {
        center[i].Pos.y = kAuthoredAltitude;
        center[i].SurfaceValid = !markGapsUnmeasured;
    }
    return center;
}

} // namespace

TEST(TileLayout, OneConformGapDoesNotReparameteriseTheSpline)
{
    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.Fit = Components::SplinePlacementFit::FitToLength;

    const auto flat = BuildTilePoses(DrapedWithGaps({}, 0.0f, true), params);
    ASSERT_EQ(flat.size(), 21u); // 40 m / 2 m + 1

    // A spike the caller declares measured IS measured — 40.00 m of drape
    // becomes 79.02 m, and the whole spline re-counts.
    const auto measuredSpike = BuildTilePoses(DrapedWithGaps({40}, 0.0f, false), params);
    EXPECT_EQ(measuredSpike.size(), 41u);

    // Declared unmeasured, the same sample holds the altitude before it: the
    // length, the count and every station are the gapless drape's.
    const auto held = BuildTilePoses(DrapedWithGaps({40}, 0.0f, true), params);
    ASSERT_EQ(held.size(), flat.size());
    for (size_t i = 0; i < held.size(); ++i)
    {
        EXPECT_NEAR(held[i].Position.z, flat[i].Position.z, 1.0e-3f) << "station " << i;
        EXPECT_NEAR(held[i].Position.y, 0.0f, 1.0e-3f) << "station " << i;
    }
}

TEST(TileLayout, RunsOfConformGapsHoldTheNearestMeasuredAltitude)
{
    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.Fit = Components::SplinePlacementFit::FitToLength;

    constexpr float32 kGround = -1.5f;
    const auto flat = BuildTilePoses(DrapedWithGaps({}, kGround, true), params);
    ASSERT_EQ(flat.size(), 21u);

    // A leading gap has nothing before it, so it back-fills from the first
    // measured sample instead of keeping the authored altitude; trailing and
    // interior runs hold the measured run before them.
    const std::vector<std::vector<size_t>> gapRuns = {
        {0, 1, 2}, {79, 80, 81}, {30, 31, 32, 33}, {0, 40, 81}};
    for (const std::vector<size_t>& gaps : gapRuns)
    {
        const auto held = BuildTilePoses(DrapedWithGaps(gaps, kGround, true), params);
        ASSERT_EQ(held.size(), flat.size()) << "first gap index " << gaps.front();
        for (size_t i = 0; i < held.size(); ++i)
        {
            EXPECT_NEAR(held[i].Position.z, flat[i].Position.z, 1.0e-3f) << "station " << i;
            EXPECT_NEAR(held[i].Position.y, kGround, 1.0e-3f) << "station " << i;
        }
    }
}

TEST(TileLayout, ACenterlineWithNoMeasuredSampleKeepsItsAuthoredAltitudes)
{
    // Conform found nothing anywhere — no ground under the spline at all. The
    // authored altitudes are then the only surface there is, so holding must
    // not flatten them onto a measurement that does not exist.
    auto authored = StraightLineZ(40.0f, 82);
    for (CenterSample& s : authored)
        s.Pos.y = 0.25f * s.Pos.z;

    auto unmeasured = authored;
    for (CenterSample& s : unmeasured)
        s.SurfaceValid = false;

    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.Fit = Components::SplinePlacementFit::FitToLength;

    const auto measured = BuildTilePoses(authored, params);
    const auto held = BuildTilePoses(unmeasured, params);
    ASSERT_FALSE(held.empty());
    ASSERT_EQ(held.size(), measured.size());
    for (size_t i = 0; i < held.size(); ++i)
    {
        EXPECT_NEAR(held[i].Position.y, measured[i].Position.y, 1.0e-4f) << "station " << i;
        EXPECT_NEAR(held[i].Position.z, measured[i].Position.z, 1.0e-4f) << "station " << i;
    }
}

// ---- The shared conform-miss rule, directly ----

namespace
{

CenterSample GapSample(float32 z, float32 y, bool measured,
                       const V3& normal = V3(0.0f, 1.0f, 0.0f))
{
    CenterSample s;
    s.Pos = V3(0.0f, y, z);
    s.Normal = normal;
    s.SurfaceValid = measured;
    return s;
}

} // namespace

TEST(TileLayout, HoldSurfaceBackfillsALeadingGapFromTheFirstMeasured)
{
    const V3 groundN(0.0f, 0.6f, 0.8f);
    std::vector<CenterSample> line{GapSample(0.0f, 9.0f, false), GapSample(1.0f, 9.0f, false),
                                   GapSample(2.0f, 2.0f, true, groundN),
                                   GapSample(3.0f, 3.0f, true)};
    HoldSurfaceAcrossGaps(line);

    for (size_t i = 0; i < 2; ++i)
    {
        EXPECT_NEAR(line[i].Pos.y, 2.0f, 1.0e-6f) << "sample " << i;
        EXPECT_NEAR(line[i].Normal.y, groundN.y, 1.0e-6f) << "sample " << i;
        EXPECT_NEAR(line[i].Normal.z, groundN.z, 1.0e-6f) << "sample " << i;
    }
    // Measured samples, every XZ, and the flags themselves are untouched.
    EXPECT_NEAR(line[2].Pos.y, 2.0f, 1.0e-6f);
    EXPECT_NEAR(line[3].Pos.y, 3.0f, 1.0e-6f);
    for (size_t i = 0; i < line.size(); ++i)
        EXPECT_NEAR(line[i].Pos.z, static_cast<float32>(i), 1.0e-6f) << "sample " << i;
    EXPECT_FALSE(line[0].SurfaceValid);
    EXPECT_FALSE(line[1].SurfaceValid);
    EXPECT_TRUE(line[2].SurfaceValid);
}

TEST(TileLayout, HoldSurfaceInteriorGapHoldsTheRunBeforeIt)
{
    std::vector<CenterSample> line{GapSample(0.0f, 1.0f, true), GapSample(1.0f, 9.0f, false),
                                   GapSample(2.0f, 9.0f, false), GapSample(3.0f, 4.0f, true)};
    HoldSurfaceAcrossGaps(line);

    EXPECT_NEAR(line[0].Pos.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(line[1].Pos.y, 1.0f, 1.0e-6f); // holds the run before it, and
    EXPECT_NEAR(line[2].Pos.y, 1.0f, 1.0e-6f); // the hold cascades through the run
    EXPECT_NEAR(line[3].Pos.y, 4.0f, 1.0e-6f); // the next measurement is its own
}

TEST(TileLayout, HoldSurfaceTrailingGapHoldsTheLastMeasured)
{
    std::vector<CenterSample> line{GapSample(0.0f, 1.5f, true), GapSample(1.0f, 9.0f, false),
                                   GapSample(2.0f, 9.0f, false)};
    HoldSurfaceAcrossGaps(line);

    EXPECT_NEAR(line[1].Pos.y, 1.5f, 1.0e-6f);
    EXPECT_NEAR(line[2].Pos.y, 1.5f, 1.0e-6f);
}

TEST(TileLayout, HoldSurfaceAllMissLeavesTheAuthoredPolylineUntouched)
{
    std::vector<CenterSample> line{GapSample(0.0f, 5.0f, false), GapSample(1.0f, 6.0f, false),
                                   GapSample(2.0f, 7.0f, false)};
    const auto before = line;
    HoldSurfaceAcrossGaps(line);

    for (size_t i = 0; i < line.size(); ++i)
    {
        EXPECT_NEAR(line[i].Pos.y, before[i].Pos.y, 1.0e-6f) << "sample " << i;
        EXPECT_NEAR(line[i].Normal.y, before[i].Normal.y, 1.0e-6f) << "sample " << i;
        EXPECT_FALSE(line[i].SurfaceValid) << "sample " << i;
    }
}

// ---- Station surface probe ----

namespace
{

using GroundProfile = std::function<float32(float32 z)>;

// A centerline dense-sampled along +Z at the controller's pinned step and
// draped onto a ground profile — the polyline BuildTilePoses actually receives.
std::vector<CenterSample> DrapedAlongZ(float32 length, float32 step, const GroundProfile& ground)
{
    const uint32 count = static_cast<uint32>(length / step) + 1u;
    std::vector<CenterSample> center;
    center.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        const float32 z = static_cast<float32>(i) * step;
        center.push_back(CenterSample{V3(0.0f, ground(z), z), V3(0.0f, 1.0f, 0.0f)});
    }
    return center;
}

StationSurfaceProbe ProbeOf(const GroundProfile& ground)
{
    return [ground](const V3& at, float32& outAltitude, V3& outNormal)
    {
        outAltitude = ground(at.z);
        outNormal = V3(0.0f, 1.0f, 0.0f);
        return true;
    };
}

float32 MaxAltitudeError(const std::vector<TilePose>& poses, const GroundProfile& ground)
{
    float32 worst = 0.0f;
    for (const TilePose& pose : poses)
        worst = std::max(worst, std::fabs(pose.Position.y - ground(pose.Position.z)));
    return worst;
}

// A 0.30 m kerb, sited between the dense samples at z = 1.0 and z = 1.5.
float32 KerbGround(float32 z)
{
    return z < 1.3f ? 0.0f : 0.30f;
}

} // namespace

TEST(TileLayout, InterpolatedStationsCutAcrossAKerbAndTheProbeDoesNot)
{
    // The station at arc 1.3 lands at z = 1.2572, still short of the kerb, but
    // the chord it interpolates has already climbed 51.4% of the riser:
    // 0.1543 m of altitude the ground under that tile does not have.
    const GroundProfile kerb = KerbGround;
    const auto center = DrapedAlongZ(40.0f, 0.5f, kerb);

    TileLayoutParams params;
    params.Spacing = 1.3f; // off the dense grid, so stations interpolate
    params.Fit = Components::SplinePlacementFit::FixedPitch;

    const auto interpolated = BuildTilePoses(center, params);
    ASSERT_FALSE(interpolated.empty());
    EXPECT_NEAR(MaxAltitudeError(interpolated, kerb), 0.1543f, 2.0e-3f);

    params.Probe = ProbeOf(kerb);
    const auto probed = BuildTilePoses(center, params);
    EXPECT_LT(MaxAltitudeError(probed, kerb), 1.0e-4f);

    // The probe re-reads altitude only: it runs after the stations are fixed,
    // so the count and every station's XZ are untouched.
    ASSERT_EQ(probed.size(), interpolated.size());
    for (size_t i = 0; i < probed.size(); ++i)
    {
        EXPECT_NEAR(probed[i].Position.x, interpolated[i].Position.x, 1.0e-6f) << "station " << i;
        EXPECT_NEAR(probed[i].Position.z, interpolated[i].Position.z, 1.0e-6f) << "station " << i;
    }
}

TEST(TileLayout, InterpolatedStationsSagBelowSmoothGroundAndTheProbeDoesNot)
{
    // Nothing sharp: a rolling profile whose curvature alone leaves the chord
    // between dense samples up to ~0.014 m off the surface.
    const GroundProfile rolling = [](float32 z)
    {
        return 0.4f * std::sin(z * (2.0f * 3.14159265f / 6.0f));
    };
    const auto center = DrapedAlongZ(40.0f, 0.5f, rolling);

    TileLayoutParams params;
    params.Spacing = 1.3f;
    params.Fit = Components::SplinePlacementFit::FixedPitch;

    const auto interpolated = BuildTilePoses(center, params);
    ASSERT_FALSE(interpolated.empty());
    EXPECT_GT(MaxAltitudeError(interpolated, rolling), 0.005f);

    params.Probe = ProbeOf(rolling);
    EXPECT_LT(MaxAltitudeError(BuildTilePoses(center, params), rolling), 1.0e-4f);
}

TEST(TileLayout, StationProbeSuppliesTheNormalTheTileTiltsTo)
{
    const auto center = StraightLineZ(10.0f, 21);
    const float32 inv = 1.0f / std::sqrt(2.0f);

    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.AlignToSurfaceNormal = true;
    params.SlopeBlend = 1.0f;
    // Which normal the tile takes is the subject here, not how far it may lean:
    // the default 35-degree ceiling would hold this 45-degree bank.
    params.MaxTiltDegrees = 90.0f;
    params.Probe = [inv](const V3&, float32& outAltitude, V3& outNormal)
    {
        outAltitude = 3.0f;
        outNormal = V3(inv, inv, 0.0f); // 45-degree bank toward +X
        return true;
    };

    const auto poses = BuildTilePoses(center, params);
    ASSERT_FALSE(poses.empty());
    for (const TilePose& pose : poses)
    {
        ExpectOrthonormal(pose);
        EXPECT_NEAR(pose.Up.x, inv, 1.0e-3f);
        EXPECT_NEAR(pose.Up.y, inv, 1.0e-3f);
        EXPECT_NEAR(pose.Position.y, 3.0f, 1.0e-3f);
    }
}

TEST(TileLayout, AStationProbeMissLeavesThatTileOnTheInterpolatedCenterline)
{
    // A probe miss is a hole under one tile. It must cost that tile its
    // correction and nothing else — not the count, not its neighbours.
    const GroundProfile kerb = KerbGround;
    const auto center = DrapedAlongZ(40.0f, 0.5f, kerb);

    TileLayoutParams params;
    params.Spacing = 1.3f;
    params.Fit = Components::SplinePlacementFit::FixedPitch;
    const auto interpolated = BuildTilePoses(center, params);

    params.Probe = [](const V3&, float32&, V3&) { return false; };
    const auto allMissed = BuildTilePoses(center, params);
    ASSERT_EQ(allMissed.size(), interpolated.size());
    for (size_t i = 0; i < allMissed.size(); ++i)
        EXPECT_NEAR(allMissed[i].Position.y, interpolated[i].Position.y, 1.0e-6f)
            << "station " << i;

    params.Probe = [](const V3& at, float32& outAltitude, V3&)
    {
        if (at.z < 2.0f)
            return false; // the hole covers the kerb station
        outAltitude = KerbGround(at.z);
        return true;
    };
    const auto partial = BuildTilePoses(center, params);
    ASSERT_EQ(partial.size(), interpolated.size());
    EXPECT_NEAR(MaxAltitudeError(partial, kerb), 0.1543f, 2.0e-3f);
}

// ---- Caller-supplied stations, yaw and plant mode ----

// A caller that solved its own fill owns the stations outright: the walk cannot
// reproduce non-uniform spacing from any pitch, and structural recipes fill a
// run with pieces of different lengths.
TEST(TileLayout, CallerSuppliedStationDistancesWinOverSpacingAndFit)
{
    const auto center = StraightLineZ(10.0f, 21u);
    const float32 stations[4] = {0.0f, 1.0f, 6.5f, 10.0f};

    TileLayoutParams params;
    params.Spacing = 2.0f; // deliberately disagrees with the list
    params.Fit = Components::SplinePlacementFit::FitToLength;
    params.StationDistances = stations;

    const auto poses = BuildTilePoses(center, params);
    ASSERT_EQ(poses.size(), 4u);
    for (size_t i = 0; i < 4u; ++i)
    {
        EXPECT_NEAR(poses[i].Position.z, stations[i], 1.0e-3f) << "station " << i;
        EXPECT_NEAR(poses[i].Base.z, stations[i], 1.0e-3f) << "station " << i;
        ExpectOrthonormal(poses[i]);
    }
}

// Base is the conformed centerline point BEFORE footprint centering and
// planting move the pivot off it — the anchor structural recipes join.
TEST(TileLayout, BaseIsTheCenterlinePointAndPositionIsTheMovedPivot)
{
    const auto center = StraightLineZ(10.0f, 21u);
    TileLayoutParams params;
    params.Spacing = 5.0f;
    params.MeshBoundsCenter = V3(1.2f, 0.5f, 0.4f);
    params.MeshBoundsHalfExtents = V3(1.2f, 0.5f, 1.2f);

    const auto poses = BuildTilePoses(center, params);
    ASSERT_FALSE(poses.empty());
    for (const TilePose& pose : poses)
    {
        EXPECT_NEAR(pose.Base.x, 0.0f, 1.0e-4f);
        EXPECT_NEAR(pose.Base.y, 0.0f, 1.0e-4f);
        // BoundsMin (the default) lifts by halfExtents.y - center.y = 0.
        EXPECT_NEAR(pose.Position.y, 0.0f, 1.0e-4f);
        // Footprint centering pushes the pivot off the line in Right and Forward.
        EXPECT_NEAR(pose.Position.x, -1.2f, 1.0e-4f);
        EXPECT_NEAR(pose.Position.z - pose.Base.z, -0.4f, 1.0e-4f);
    }
}

// PivotPlane lands local y=0 on the conform hit and lets a planting skirt bury;
// BoundsMin lifts until the lowest vertex touches, which floats that same mesh
// by the depth of its skirt.
TEST(TileLayout, PlantModeChoosesWhichPlaneMeetsTheGround)
{
    const auto center = StraightLineZ(10.0f, 21u);
    TileLayoutParams params;
    params.Spacing = 5.0f;
    // Bounds min y = 0.24 - 0.76 = -0.52: a measured fence panel's skirt.
    params.MeshBoundsCenter = V3(0.0f, 0.24f, 0.0f);
    params.MeshBoundsHalfExtents = V3(0.1f, 0.76f, 0.1f);

    params.PlantMode = Components::SplinePlantMode::BoundsMin;
    const auto lifted = BuildTilePoses(center, params);
    ASSERT_FALSE(lifted.empty());
    EXPECT_NEAR(lifted[0].Position.y, 0.52f, 1.0e-4f);

    params.PlantMode = Components::SplinePlantMode::PivotPlane;
    const auto planted = BuildTilePoses(center, params);
    ASSERT_EQ(planted.size(), lifted.size());
    EXPECT_NEAR(planted[0].Position.y, 0.0f, 1.0e-4f);
}

// The proud-paving defect, in the arithmetic that causes it, on the two slabs it
// was measured on. Both are authored about a mid-plane, not a base: the pivot
// sits ~0.19 m above the lowest vertex and ~0.08 m below the top face. BoundsMin
// therefore lifts the WHOLE slab clear of the ground — a uniform side band that
// is the piece and not the terrain — while PivotPlane leaves only the part above
// the pivot standing, which is the flush stepping-stone read.
//
// The two heights are asserted against each other AND against their absolute
// values: a mode that did nothing at all would keep them equal, and a mode that
// merely differed would pass a comparison alone while planting at the wrong
// altitude.
TEST(TileLayout, MeasuredPavingSlabStandsItsFullThicknessProudUntilPivotPlane)
{
    struct Slab
    {
        const char* Name;
        // Local-space vertical extent as measured on the imported mesh.
        float32 MinY;
        float32 MaxY;
        // Footprint, which decides the piece axis; neither slab is long enough
        // in X to flip it off the Z convention.
        float32 HalfX;
        float32 HalfZ;
        // What is predicted to stand above the ground once PivotPlane is
        // authored, written independently of MinY/MaxY so the number the content
        // round is planned against is pinned rather than re-derived.
        float32 ExpectedProudMetres;
    };
    // SM_Env_Footpath_01: y -0.1908..+0.0758 (0.2666 thick), 2.6242 x 2.3756 m.
    // SM_Env_Footpath_02: y -0.1857..+0.0969 (0.2826 thick).
    const Slab slabs[] = {
        {"SM_Env_Footpath_01", -0.1908f, 0.0758f, 1.3121f, 1.1878f, 0.076f},
        {"SM_Env_Footpath_02", -0.1857f, 0.0969f, 1.3121f, 1.1878f, 0.097f},
    };

    for (const Slab& slab : slabs)
    {
        const auto center = StraightLineZ(10.0f, 21u);
        TileLayoutParams params;
        params.Spacing = 2.5f;
        params.MeshBoundsCenter = V3(0.0f, (slab.MinY + slab.MaxY) * 0.5f, 0.0f);
        params.MeshBoundsHalfExtents = V3(slab.HalfX, (slab.MaxY - slab.MinY) * 0.5f, slab.HalfZ);

        params.PlantMode = Components::SplinePlantMode::BoundsMin;
        const auto lifted = BuildTilePoses(center, params);
        ASSERT_FALSE(lifted.empty()) << slab.Name;

        params.PlantMode = Components::SplinePlantMode::PivotPlane;
        const auto planted = BuildTilePoses(center, params);
        ASSERT_EQ(planted.size(), lifted.size()) << slab.Name;

        // Ground is y = 0 along this centerline, so a pose's y IS its pivot's
        // height above ground, and pivot + MaxY is where the top face lands.
        EXPECT_NEAR(lifted[0].Position.y, -slab.MinY, 1.0e-4f)
            << slab.Name << ": BoundsMin no longer lands the lowest vertex on the surface";
        EXPECT_NEAR(lifted[0].Position.y + slab.MaxY, slab.MaxY - slab.MinY, 1.0e-4f)
            << slab.Name << ": the whole-thickness proud band is what this defect IS; without it "
                            "the PivotPlane assertion below proves nothing";

        EXPECT_NEAR(planted[0].Position.y, 0.0f, 1.0e-4f)
            << slab.Name << ": PivotPlane no longer lands local y=0 on the surface";
        // Tolerance is the reported figure's own rounding half-width (3 dp), not
        // a slack allowance: 0.076 stands for 0.0755..0.0765.
        EXPECT_NEAR(planted[0].Position.y + slab.MaxY, slab.ExpectedProudMetres, 5.0e-4f)
            << slab.Name << ": residual proud height moved; the content round is priced on it";

        EXPECT_NEAR(lifted[0].Position.y - planted[0].Position.y, -slab.MinY, 1.0e-4f)
            << slab.Name << ": PivotPlane sinks the slab by something other than its pivot depth";
    }
}

// A caller-supplied direction overrides the chord for that station only, and a
// zero entry means "no opinion, use the chord".
TEST(TileLayout, StationForwardOverridesReplaceOnlyTheStationsTheyName)
{
    const auto center = StraightLineZ(10.0f, 21u);
    const float32 stations[3] = {0.0f, 5.0f, 10.0f};
    const V3 forwards[3] = {V3(0, 0, 0), V3(1.0f, 0.0f, 1.0f), V3(0, 0, 0)};

    TileLayoutParams params;
    params.Spacing = 5.0f;
    params.StationDistances = stations;
    params.StationForwards = forwards;

    const auto poses = BuildTilePoses(center, params);
    ASSERT_EQ(poses.size(), 3u);
    EXPECT_NEAR(poses[0].Forward.z, 1.0f, 1.0e-4f);
    EXPECT_NEAR(poses[1].Forward.x, 0.70710678f, 1.0e-3f);
    EXPECT_NEAR(poses[1].Forward.z, 0.70710678f, 1.0e-3f);
    EXPECT_NEAR(poses[2].Forward.z, 1.0f, 1.0e-4f);
    for (const TilePose& pose : poses)
        ExpectOrthonormal(pose);
}

// ---- Which local axis the tile is laid along ------------------------------

// Tiles read the piece-axis rule the fence recipe reads, so the chord window
// and the footprint centering can never disagree about which way the slab runs.
// Every measured path tile is near-square (2.62 x 2.38 m) and therefore keeps
// the Z convention: the shipped tile strip is unchanged by the rule.
TEST(TileLayout, NearSquareTileKeepsTheZConventionForChordAndCentering)
{
    const auto center = StraightLineZ(10.0f, 21u);
    TileLayoutParams params;
    params.Spacing = 2.5f;
    // SM_Env_Footpath_01, measured: 2.6242 x 0.2666 x 2.3756 m, corner-pivoted.
    params.MeshBoundsCenter = V3(-1.2435f, -0.0575f, -1.2429f);
    params.MeshBoundsHalfExtents = V3(1.3121f, 0.1333f, 1.1878f);
    params.PlantMode = Components::SplinePlantMode::PivotPlane;

    ASSERT_EQ(ChoosePieceAxis(params.MeshBoundsHalfExtents), PieceAxis::Z);

    const auto poses = BuildTilePoses(center, params);
    ASSERT_FALSE(poses.empty());
    for (const TilePose& pose : poses)
    {
        // Centering still cancels center.x along Right and center.z along
        // Forward, which is what the Z convention means.
        EXPECT_NEAR(pose.Position.x, 1.2435f, 1.0e-4f);
        EXPECT_NEAR(pose.Position.z - pose.Base.z, 1.2429f, 1.0e-4f);
    }
}

// An X-long piece in a straight pool is laid along its long axis, and the chord
// window follows the same axis. Nothing in the shipped kits is shaped this way,
// but the two consumers must answer the question the same way or the fence bug
// has a second home.
TEST(TileLayout, LongTileIsCenteredAlongItsOwnAxes)
{
    const auto center = StraightLineZ(10.0f, 21u);
    TileLayoutParams params;
    params.Spacing = 5.0f;
    // Long in X, thin in Z, with a corner pivot on both.
    params.MeshBoundsCenter = V3(2.0f, 0.5f, 0.25f);
    params.MeshBoundsHalfExtents = V3(2.0f, 0.5f, 0.25f);
    params.PlantMode = Components::SplinePlantMode::PivotPlane;

    ASSERT_EQ(ChoosePieceAxis(params.MeshBoundsHalfExtents), PieceAxis::X);

    const auto poses = BuildTilePoses(center, params);
    ASSERT_FALSE(poses.empty());
    for (const TilePose& pose : poses)
    {
        // Local +X is on Forward (+Z world), local +Z on -Right (-X world), so
        // center.x is cancelled along travel and center.z across it.
        EXPECT_NEAR(pose.Position.z - pose.Base.z, -2.0f, 1.0e-4f);
        EXPECT_NEAR(pose.Position.x, 0.25f, 1.0e-4f);
    }
}

// NormalizedOrFallback is the shared normalize behind tile, fence and
// controller layout, so whatever it returns is what the poses are built from:
// it must convert garbage into the fallback rather than into NaN. Both
// non-finite forms defeat a plain `lenSq < eps` guard: a NaN squared length
// makes every comparison false and walks straight into the normalize, and an
// infinite one passes the guard honestly only to compute Inf * (1/Inf).

TEST(TileLayout, NormalizedOrFallbackRejectsNaNInput)
{
    const V3 fallback(0.0f, 0.0f, 1.0f);
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();

    const V3 allNaN = NormalizedOrFallback(V3(nan, nan, nan), fallback);
    EXPECT_FLOAT_EQ(allNaN.x, fallback.x);
    EXPECT_FLOAT_EQ(allNaN.y, fallback.y);
    EXPECT_FLOAT_EQ(allNaN.z, fallback.z);

    // One poisoned component is enough: the dot product spreads it.
    const V3 oneNaN = NormalizedOrFallback(V3(1.0f, nan, 0.0f), fallback);
    EXPECT_FLOAT_EQ(oneNaN.x, fallback.x);
    EXPECT_FLOAT_EQ(oneNaN.y, fallback.y);
    EXPECT_FLOAT_EQ(oneNaN.z, fallback.z);
}

TEST(TileLayout, NormalizedOrFallbackRejectsInfiniteInput)
{
    const V3 fallback(0.0f, 1.0f, 0.0f);
    const float32 inf = std::numeric_limits<float32>::infinity();

    const V3 positive = NormalizedOrFallback(V3(inf, 0.0f, 0.0f), fallback);
    EXPECT_FLOAT_EQ(positive.x, fallback.x);
    EXPECT_FLOAT_EQ(positive.y, fallback.y);
    EXPECT_FLOAT_EQ(positive.z, fallback.z);

    const V3 negative = NormalizedOrFallback(V3(0.0f, 0.0f, -inf), fallback);
    EXPECT_FLOAT_EQ(negative.x, fallback.x);
    EXPECT_FLOAT_EQ(negative.y, fallback.y);
    EXPECT_FLOAT_EQ(negative.z, fallback.z);
}

// A finite vector whose squared length overflows fp32 reaches the normalize as
// an infinite lenSq, so it fails for the same reason a literal Inf does.
TEST(TileLayout, NormalizedOrFallbackRejectsOverflowingMagnitude)
{
    const V3 fallback(1.0f, 0.0f, 0.0f);
    const V3 huge(1.0e30f, 1.0e30f, 1.0e30f);
    ASSERT_TRUE(std::isfinite(huge.x));

    const V3 result = NormalizedOrFallback(huge, fallback);
    EXPECT_FLOAT_EQ(result.x, fallback.x);
    EXPECT_FLOAT_EQ(result.y, fallback.y);
    EXPECT_FLOAT_EQ(result.z, fallback.z);
}

// Control: the working path must be untouched by the guard change.
TEST(TileLayout, NormalizedOrFallbackNormalizesHealthyInput)
{
    const V3 fallback(0.0f, 0.0f, 1.0f);

    const V3 axis = NormalizedOrFallback(V3(0.0f, 5.0f, 0.0f), fallback);
    EXPECT_NEAR(axis.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(axis.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(axis.z, 0.0f, 1.0e-6f);

    const V3 diagonal = NormalizedOrFallback(V3(3.0f, 0.0f, 4.0f), fallback);
    EXPECT_NEAR(diagonal.x, 0.6f, 1.0e-6f);
    EXPECT_NEAR(diagonal.z, 0.8f, 1.0e-6f);
    EXPECT_NEAR(V3::Dot(diagonal, diagonal), 1.0f, 1.0e-6f);
}

// Control: the degenerate-length contract the guard was written for.
TEST(TileLayout, NormalizedOrFallbackFallsBackOnZeroLength)
{
    const V3 fallback(0.0f, 0.0f, 1.0f);

    const V3 zero = NormalizedOrFallback(V3(0.0f, 0.0f, 0.0f), fallback);
    EXPECT_FLOAT_EQ(zero.x, fallback.x);
    EXPECT_FLOAT_EQ(zero.y, fallback.y);
    EXPECT_FLOAT_EQ(zero.z, fallback.z);

    // Just under the 1e-8 squared-length threshold.
    const V3 tiny = NormalizedOrFallback(V3(0.0f, 9.0e-5f, 0.0f), fallback);
    EXPECT_FLOAT_EQ(tiny.y, fallback.y);
    EXPECT_FLOAT_EQ(tiny.z, fallback.z);
}

// ---- Tilt ceiling on steep ground ----
//
// The artifact these pin: a path crossing a near-vertical crest folded into
// standing sheets. A tile's up came straight from the surface normal, and since
// Right is Cross(up, forward), an up leaning 88 degrees turns the tile's WIDTH
// axis skyward. Measured on the fixture below before the ceiling existed:
// up.y = 0.024 (an 88.6 degree lean), Right within 14 degrees of vertical, and
// an 88 degree frame step between the last flat station and the first on-face
// one. The XZ chord is NOT the mechanism — it collapses 44x here (2.40 m to
// 0.055 m) yet never falls back and never reverses, which the control test
// below pins by holding everything constant except the up-blend.

namespace
{

// A path along +Z whose GROUND climbs `rise` metres across a face `halfWidth`
// wide at z = 10 — the shape conform produces, with the steepness all in Y and
// none in the authored XZ. Normals are the heightfield gradient's.
//
// The ridge runs OBLIQUE to the path (its crest line drifts `kCrestObliquity`
// metres in z per metre of x), which is what ordinary terrain does and what the
// mechanism needs: Right is Cross(up, forward) = (up.y, -up.x, 0) for travel
// along +Z, so its verticality is set by the ratio of the normal's CROSS-slope
// component to its vertical one. On a near-vertical face the vertical component
// collapses and any cross-slope at all swings Right toward the sky. A ridge
// exactly square to the path has no cross-slope and hides the defect — the
// first draft of this fixture did, and passed while the editor showed sheets.
constexpr float32 kCrestObliquity = 0.28f;

std::vector<CenterSample> CrestCrossing(float32 halfWidth, float32 rise, uint32 count = 161u)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < count; ++i)
    {
        const float32 z = 20.0f * static_cast<float32>(i) / static_cast<float32>(count - 1u);
        const float32 u = (z - (10.0f - halfWidth)) / (2.0f * halfWidth);
        float32 y = rise;
        float32 dydz = 0.0f;
        if (u <= 0.0f)
        {
            y = 0.0f;
        }
        else if (u < 1.0f)
        {
            y = rise * (u * u * (3.0f - 2.0f * u));
            dydz = rise * (6.0f * u * (1.0f - u)) / (2.0f * halfWidth);
        }
        // Moving the crest line by dz = obliquity * dx moves the whole face, so
        // the lateral gradient is the along-path one times the obliquity.
        const float32 dydx = -dydz * kCrestObliquity;
        CenterSample s;
        s.Pos = V3(0.0f, y, z);
        s.Normal = NormalizedOrFallback(V3(-dydx, 1.0f, -dydz), V3(0, 1, 0));
        center.push_back(s);
    }
    return center;
}

float32 AngleDegrees(const V3& a, const V3& b)
{
    return std::acos(std::clamp(V3::Dot(a, b), -1.0f, 1.0f)) * 180.0f / 3.14159265358979f;
}

float32 TiltFromWorldUpDegrees(const V3& up) { return AngleDegrees(up, V3(0.0f, 1.0f, 0.0f)); }

TileLayoutParams SteepCrestParams(float32 maxTiltDegrees)
{
    TileLayoutParams params;
    params.Spacing = 2.0f;
    params.Fit = Components::SplinePlacementFit::FitToLength;
    params.AlignToSurfaceNormal = true;
    params.SlopeBlend = 1.0f;
    params.MaxTiltDegrees = maxTiltDegrees;
    params.MeshBoundsHalfExtents = V3(1.2f, 0.05f, 1.2f);
    return params;
}

} // namespace

TEST(TileLayout, SteepCrestWithoutTheCeilingStandsTilesOnEdge)
{
    // The mechanism's witness, kept as a test: 90 degrees is "no ceiling",
    // which is what the layout did before the ceiling existed.
    const auto center = CrestCrossing(0.2f, 12.0f);
    const auto poses = BuildTilePoses(center, SteepCrestParams(90.0f));
    ASSERT_GE(poses.size(), 8u);

    float32 worstTilt = 0.0f;
    for (const TilePose& pose : poses)
        worstTilt = std::max(worstTilt, TiltFromWorldUpDegrees(pose.Up));
    EXPECT_GT(worstTilt, 80.0f) << "fixture must actually present near-vertical ground";

    // The defect itself: a tile whose WIDTH axis stands up out of the ground
    // plane. Sign-agnostic — the cross product puts it up or down depending on
    // which side of the ridge the station is on, and either reads as a sheet.
    float32 mostVerticalRight = 0.0f;
    for (const TilePose& pose : poses)
        mostVerticalRight = std::max(mostVerticalRight, std::abs(pose.Right.y));
    EXPECT_GT(mostVerticalRight, std::cos(20.0f * 3.14159265358979f / 180.0f))
        << "unbounded blend must put some tile's width axis near vertical";
}

TEST(TileLayout, TiltCeilingHoldsTilesUprightOnASteepCrest)
{
    const auto center = CrestCrossing(0.2f, 12.0f);
    const auto poses = BuildTilePoses(center, SteepCrestParams(35.0f));
    ASSERT_GE(poses.size(), 8u);

    for (const TilePose& pose : poses)
    {
        ExpectOrthonormal(pose);
        EXPECT_LE(TiltFromWorldUpDegrees(pose.Up), 35.0f + 1.0e-2f);
        // Width axis stays near the ground plane rather than swinging skyward.
        EXPECT_LE(std::abs(pose.Right.y), std::sin(36.0f * 3.14159265358979f / 180.0f));
    }
}

TEST(TileLayout, TiltCeilingBoundsTheFrameStepBetweenAdjacentStations)
{
    // Frame continuity: no station's basis may jump away from its neighbour's.
    // Two stations may legitimately differ by twice the ceiling (one held at
    // +35, the next at -35 across a ridge line), which is the bound asserted.
    const auto center = CrestCrossing(0.2f, 12.0f);
    const auto poses = BuildTilePoses(center, SteepCrestParams(35.0f));
    ASSERT_GE(poses.size(), 8u);

    for (size_t i = 1; i < poses.size(); ++i)
    {
        EXPECT_LE(AngleDegrees(poses[i].Up, poses[i - 1].Up), 71.0f) << "up step at station " << i;
        EXPECT_LE(AngleDegrees(poses[i].Right, poses[i - 1].Right), 71.0f)
            << "right step at station " << i;
        // The property the brief named: adjacent right-vectors never reverse.
        EXPECT_GT(V3::Dot(poses[i].Right, poses[i - 1].Right), 0.0f)
            << "right reversed between stations " << i - 1 << " and " << i;
    }
}

TEST(TileLayout, TiltCeilingReportsWhatItHeldAndTheSteepestGroundSeen)
{
    const auto center = CrestCrossing(0.2f, 12.0f);
    TileLayoutParams params = SteepCrestParams(35.0f);
    GameEngine::SplineLayout::TiltClampReport report;
    params.OutTiltClamp = &report;
    const auto poses = BuildTilePoses(center, params);
    ASSERT_FALSE(poses.empty());

    EXPECT_GT(report.ClampedStations, 0u);
    EXPECT_LT(report.ClampedStations, poses.size())
        << "flat stations either side of the face must not be counted";
    // The report carries the ground's real steepness, not the held angle.
    EXPECT_GT(report.SteepestDegrees, 80.0f);
    EXPECT_NEAR(report.SteepestAt.z, 10.0f, 1.5f);
}

TEST(TileLayout, GentleSlopesArePoseIdenticalWithAndWithoutTheCeiling)
{
    // Ground within the ceiling must be untouched, bit for bit: the fix may not
    // move any tile on the ground the recipe already handled well.
    const auto center = CrestCrossing(6.0f, 2.0f); // ~9 degrees at its steepest
    const auto held = BuildTilePoses(center, SteepCrestParams(35.0f));
    const auto unheld = BuildTilePoses(center, SteepCrestParams(90.0f));
    ASSERT_EQ(held.size(), unheld.size());
    ASSERT_FALSE(held.empty());

    for (size_t i = 0; i < held.size(); ++i)
    {
        EXPECT_FLOAT_EQ(held[i].Up.x, unheld[i].Up.x) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].Up.y, unheld[i].Up.y) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].Up.z, unheld[i].Up.z) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].Right.x, unheld[i].Right.x) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].Right.y, unheld[i].Right.y) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].Right.z, unheld[i].Right.z) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].Position.y, unheld[i].Position.y) << "station " << i;
    }
}

TEST(TileLayout, TheChordCollapsesOnASteepFaceWithoutReversingTheFrame)
{
    // The control that falsifies the chord as the mechanism. Same path, same
    // 44x-collapsed XZ chord — only the up-blend is off. If the chord were the
    // cause the frame would scatter here too.
    const auto center = CrestCrossing(0.2f, 12.0f);
    TileLayoutParams params = SteepCrestParams(90.0f);
    params.AlignToSurfaceNormal = false;
    const auto poses = BuildTilePoses(center, params);
    ASSERT_GE(poses.size(), 8u);

    for (size_t i = 0; i < poses.size(); ++i)
    {
        ExpectOrthonormal(poses[i]);
        EXPECT_NEAR(poses[i].Up.y, 1.0f, 1.0e-4f);
        if (i == 0)
            continue;
        EXPECT_GT(V3::Dot(poses[i].Right, poses[i - 1].Right), 0.0f);
        EXPECT_LE(AngleDegrees(poses[i].Forward, poses[i - 1].Forward), 15.0f);
    }
}

TEST(TileLayout, ClampTiltFromWorldUpKeepsAzimuthAndPassesWhatIsAlreadyInside)
{
    using GameEngine::SplineLayout::ClampTiltFromWorldUp;
    const V3 worldUp(0.0f, 1.0f, 0.0f);

    // Inside the cone: returned unchanged.
    const V3 gentle = NormalizedOrFallback(V3(0.2f, 1.0f, 0.0f), worldUp);
    const V3 passed = ClampTiltFromWorldUp(gentle, 35.0f);
    EXPECT_FLOAT_EQ(passed.x, gentle.x);
    EXPECT_FLOAT_EQ(passed.y, gentle.y);

    // Outside: held at exactly the ceiling, leaning the same way.
    const V3 steep = NormalizedOrFallback(V3(1.0f, 0.05f, 0.0f), worldUp);
    const V3 heldSteep = ClampTiltFromWorldUp(steep, 35.0f);
    EXPECT_NEAR(TiltFromWorldUpDegrees(heldSteep), 35.0f, 1.0e-3f);
    EXPECT_GT(heldSteep.x, 0.0f) << "azimuth of the lean must survive the clamp";
    EXPECT_NEAR(heldSteep.z, 0.0f, 1.0e-5f);
    EXPECT_NEAR(V3::Dot(heldSteep, heldSteep), 1.0f, 1.0e-5f);

    // An upside-down normal names no azimuth; world up is the only answer.
    const V3 inverted = ClampTiltFromWorldUp(V3(0.0f, -1.0f, 0.0f), 35.0f);
    EXPECT_NEAR(inverted.y, 1.0f, 1.0e-5f);

    // A NaN ceiling must not become a NaN basis: std::clamp passes NaN through
    // (both comparisons false), so the guard is explicit, not incidental.
    const V3 nanCeiling =
        ClampTiltFromWorldUp(steep, std::numeric_limits<float32>::quiet_NaN());
    EXPECT_TRUE(std::isfinite(nanCeiling.x) && std::isfinite(nanCeiling.y) &&
                std::isfinite(nanCeiling.z));
    EXPECT_NEAR(TiltFromWorldUpDegrees(nanCeiling),
                Components::kDefaultSplineMaxTiltDegrees, 1.0e-3f);
}

// ---- Footprint centering survives the emitted rotation ----
//
// The layout cancels the mesh bounds centre along the piece's OWN axes, and the
// emitted transform puts it back through the same basis. The two terms must
// cancel at EVERY heading: resolving either one against world axes instead
// leaves a residue of R*c - c, which is zero due north and grows to metres as
// the run turns — a corner-pivoted 2.6 x 2.4 m kit tile carries |c| = 1.76 m,
// so the residue peaks past 3 m.
//
// The existing centering tests all run due north, where that residue vanishes
// identically, so none of them can see it.

namespace
{

// A straight, flat centerline running at `yawDeg` from +Z, rotating about +Y.
std::vector<CenterSample> StraightAtHeading(float32 yawDeg, float32 length, uint32 sampleCount)
{
    const float32 yaw = yawDeg * Mathematics::Pi / 180.0f;
    const V3 direction(std::sin(yaw), 0.0f, std::cos(yaw));
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        CenterSample s;
        s.Pos = direction * (length * static_cast<float32>(i) /
                             static_cast<float32>(sampleCount - 1u));
        s.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(s);
    }
    return center;
}

const Mathematics::Matrix4x4& IdentityPlacer()
{
    static const float32 kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const Mathematics::Matrix4x4 inverse = SplineLayout::InvertPlacerWorld(kIdentity);
    return inverse;
}

// Where the mesh's local bounds centre actually lands, read out of the emitted
// transform rather than recomputed — a re-derivation here could agree with a
// wrong layout.
V3 EmittedBoundsCenter(const Components::Transform& t, const V3& localCenter)
{
    Mathematics::Matrix4x4 world;
    std::memcpy(world.Data(), t.matrix, sizeof(t.matrix));
    return world.TransformPoint(localCenter);
}

} // namespace

TEST(TileLayout, FootprintCenterStaysOnTheCenterlineAtEveryHeading)
{
    // SM_Env_Footpath_01, measured: 2.6242 x 0.2666 x 2.3756 m, corner-pivoted.
    const V3 boundsCenter(-1.2435f, -0.0575f, -1.2429f);
    const V3 boundsHalf(1.3121f, 0.1333f, 1.1878f);
    const PieceAxis axis = ChoosePieceAxis(boundsHalf);
    ASSERT_EQ(axis, PieceAxis::Z);

    for (const float32 yawDeg : {0.0f, 30.0f, 60.0f, 90.0f, 100.0f, 135.0f, 180.0f, -96.0f})
    {
        const auto center = StraightAtHeading(yawDeg, 20.0f, 41u);
        TileLayoutParams params;
        params.Spacing = 2.309f;
        params.MeshBoundsCenter = boundsCenter;
        params.MeshBoundsHalfExtents = boundsHalf;

        const auto poses = BuildTilePoses(center, params);
        ASSERT_FALSE(poses.empty()) << "yaw " << yawDeg;

        for (const TilePose& pose : poses)
        {
            Components::Transform t{};
            SplineLayout::WriteParentLocalPose(t, IdentityPlacer(), pose, axis, 1.0f);
            const V3 delta = EmittedBoundsCenter(t, boundsCenter) - pose.Base;

            // Across travel and along it the centering must cancel outright.
            EXPECT_NEAR(V3::Dot(delta, pose.Right), 0.0f, 1.0e-3f)
                << "lateral drift at yaw " << yawDeg;
            EXPECT_NEAR(V3::Dot(delta, pose.Forward), 0.0f, 1.0e-3f)
                << "along-track drift at yaw " << yawDeg;
            // What remains is the plant lift alone: BoundsMin raises the mesh
            // until its lowest vertex meets the surface, so the centre sits one
            // half-height above the station.
            EXPECT_NEAR(V3::Dot(delta, pose.Up), boundsHalf.y, 1.0e-3f)
                << "plant lift at yaw " << yawDeg;
        }
    }
}
