#include "Components/Transform.h"
#include "SplineLayout/FenceLayout.h"
#include "SplineLayout/PieceEntity.h"
#include "SplineLayout/SpanMitre.h"

#include "FenceLayoutCrestGoldens.h"
#include "GoldenUlps.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace GameEngine;
using SplineLayout::BuildFenceLayout;
using SplineLayout::CenterSample;
using SplineLayout::ChoosePieceAxis;
using SplineLayout::FenceCrest;
using SplineLayout::FenceLayoutParams;
using SplineLayout::FenceLayoutResult;
using SplineLayout::FencePieceBounds;
using SplineLayout::FenceReservation;
using SplineLayout::FenceSpan;
using SplineLayout::FenceStation;
using SplineLayout::TilePose;
using SplineLayout::PieceAxis;
using SplineLayout::PieceLength;
using SplineLayout::InvertPlacerWorld;
using SplineLayout::WriteParentLocalPose;
using SplineLayout::Tests::CrestGolden;
using SplineLayout::Tests::FloatBits;
using SplineLayout::Tests::kMaxGoldenUlps;
using SplineLayout::Tests::UlpDistance;
using V3 = GameEngine::Mathematics::Vector3;

namespace
{

// A straight, flat centerline along +Z, sampled every `length / (count - 1)`.
std::vector<CenterSample> StraightLineZ(float32 length, uint32 sampleCount)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        CenterSample s;
        s.Pos = V3(0.0f, 0.0f,
                   length * static_cast<float32>(i) / static_cast<float32>(sampleCount - 1u));
        s.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(s);
    }
    return center;
}

// A straight ramp: +Z with a constant grade.
std::vector<CenterSample> RampZ(float32 run, float32 rise, uint32 sampleCount)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(sampleCount - 1u);
        CenterSample s;
        s.Pos = V3(0.0f, rise * t, run * t);
        s.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(s);
    }
    return center;
}

// Flat along +Z but for a half-sine bump of `height` between `bumpFrom` and
// `bumpTo`: the draped centerline climbs over it, so the run is longer than the
// straight line between its ends.
std::vector<CenterSample> BumpedLineZ(float32 length, float32 bumpFrom, float32 bumpTo,
                                      float32 height, uint32 sampleCount)
{
    std::vector<CenterSample> center = StraightLineZ(length, sampleCount);
    for (CenterSample& sample : center)
    {
        const float32 t = (sample.Pos.z - bumpFrom) / (bumpTo - bumpFrom);
        if (t > 0.0f && t < 1.0f)
            sample.Pos.y = height * std::sin(Mathematics::Pi * t);
    }
    return center;
}

// An L: (0,0,0) -> (0,0,armZ) -> (armX,0,armZ), sampled at `step` metres.
std::vector<CenterSample> CorneredL(float32 armZ, float32 armX, float32 step)
{
    std::vector<CenterSample> center;
    const uint32 legZ = static_cast<uint32>(std::lround(armZ / step));
    const uint32 legX = static_cast<uint32>(std::lround(armX / step));
    for (uint32 i = 0; i <= legZ; ++i)
        center.push_back({V3(0.0f, 0.0f, static_cast<float32>(i) * step), V3(0, 1, 0), true});
    for (uint32 i = 1; i <= legX; ++i)
        center.push_back({V3(static_cast<float32>(i) * step, 0.0f, armZ), V3(0, 1, 0), true});
    return center;
}

// A closed square of `side` metres traversed p0->p1->p2->p3->p0, at `step`.
std::vector<CenterSample> ClosedSquare(float32 side, float32 step)
{
    const V3 corners[4] = {V3(0, 0, 0), V3(side, 0, 0), V3(side, 0, side), V3(0, 0, side)};
    std::vector<CenterSample> center;
    const uint32 perLeg = static_cast<uint32>(std::lround(side / step));
    for (uint32 leg = 0; leg < 4; ++leg)
    {
        const V3& a = corners[leg];
        const V3& b = corners[(leg + 1u) % 4u];
        for (uint32 i = 0; i < perLeg; ++i)
        {
            const float32 t = static_cast<float32>(i) / static_cast<float32>(perLeg);
            center.push_back({a + (b - a) * t, V3(0, 1, 0), true});
        }
    }
    center.push_back({corners[0], V3(0, 1, 0), true}); // closes the loop
    return center;
}

// A synthetic span: thin in X, long in Z. Convenient, and for a year the ONLY
// shape this file measured — which is why the fill could read a real kit
// piece's thickness as its length and no test noticed. Real-shaped fixtures
// live below (MeasuredKit*).
FencePieceBounds Piece(float32 length, float32 height = 1.0f)
{
    FencePieceBounds b;
    b.HalfExtents = V3(0.05f, height * 0.5f, length * 0.5f);
    b.Center = V3(0.0f, height * 0.5f, 0.0f);
    return b;
}

// ---- Real kit shapes -------------------------------------------------------
//
// Half-extents measured off the shipped FBX vertex data through the engine's
// own axis conversion (ModelAssetLoadFbx: source right/up/front onto -X/+Y/+Z).
// Every kit piece that runs anywhere runs along local X; the near-square ones
// are posts and slabs. This table is the blind spot the synthetic fixture above
// hid: a fixture built length-in-Z can never catch a consumer that measures Z.

// SM_Prop_Fence_01 (FantasyKingdom): 2.4707 x 1.6387 x 0.1567 m, centre-pivoted
// in XZ with a -0.52 m planting skirt. 15.8:1 — measured on the wrong axis, the
// piece fills the spline crosswise, 15.7 panels deep.
FencePieceBounds MeasuredKitPanel()
{
    FencePieceBounds b;
    b.HalfExtents = V3(1.2353f, 0.8194f, 0.0784f);
    b.Center = V3(0.0f, 0.2978f, 0.0f);
    return b;
}

// SM_Env_StoneWall_Pillar_01 (FantasyKingdom): 1.0066 x 1.1668 x 1.0043 m, a
// true standalone post. Near-square by construction — a post has no run
// direction, and its 1.002:1 must not be read as one.
FencePieceBounds MeasuredKitPost()
{
    FencePieceBounds b;
    b.HalfExtents = V3(0.5033f, 0.5834f, 0.5021f);
    b.Center = V3(0.0f, 0.5834f, 0.0f);
    return b;
}

// An identity placer: the emitted parent-local transform is then the world
// pose, so these tests read the piece's axes straight off the columns.
const Mathematics::Matrix4x4& IdentityPlacer()
{
    static const float32 kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const Mathematics::Matrix4x4 inverse = InvertPlacerWorld(kIdentity);
    return inverse;
}

float32 BasisDeterminant(const Components::Transform& t)
{
    const V3 c0(t.matrix[0], t.matrix[1], t.matrix[2]);
    const V3 c1(t.matrix[4], t.matrix[5], t.matrix[6]);
    const V3 c2(t.matrix[8], t.matrix[9], t.matrix[10]);
    return V3::Dot(c0, V3::Cross(c1, c2));
}

V3 Column(const Components::Transform& t, int index)
{
    return V3(t.matrix[index * 4], t.matrix[index * 4 + 1], t.matrix[index * 4 + 2]);
}

bool AnyValidationContains(const FenceLayoutResult& result, const char* needle)
{
    return std::any_of(result.Validation.begin(), result.Validation.end(),
                       [needle](const std::string& m)
                       { return m.find(needle) != std::string::npos; });
}

} // namespace

// ---- Per-run station walk -------------------------------------------------

// Authored points are mandatory stations, and the pitch fills between them.
// Three points on a 20 m line at pitch 2.5 give five stations per 10 m run,
// with the shared station at the corner emitted exactly once.
TEST(FenceLayout, AuthoredPointsAreStationsAndPitchFillsBetween)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.5f;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Stations.size(), 9u);
    for (uint32 i = 0; i < 9u; ++i)
    {
        EXPECT_NEAR(result.Stations[i].Pose.Base.z, 2.5f * static_cast<float32>(i), 1.0e-3f)
            << "station " << i;
        EXPECT_EQ(result.Stations[i].Index, i);
    }
    // Stations 0 and 4 open their runs; 8 closes the last one.
    EXPECT_TRUE(result.Stations[0].IsAuthoredPoint);
    EXPECT_TRUE(result.Stations[4].IsAuthoredPoint);
    EXPECT_TRUE(result.Stations[8].IsAuthoredPoint);
    EXPECT_FALSE(result.Stations[1].IsAuthoredPoint);
    EXPECT_TRUE(result.Validation.empty());
}

// A closed spline adds the last->first run and emits the shared station ONCE:
// four corners, four stations, no duplicate where the loop closes.
TEST(FenceLayout, ClosedSplineWrapsAndSharesTheFirstStationOnce)
{
    const auto center = ClosedSquare(10.0f, 0.5f);
    const float32 boundaries[5] = {0.0f, 20.0f, 40.0f, 60.0f, 80.0f};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.Closed = true;
    params.PostPitch = 12.0f; // longer than a side: two stations per run

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Stations.size(), 4u);
    for (const FenceStation& s : result.Stations)
        EXPECT_TRUE(s.IsAuthoredPoint);

    // Exactly one station at the loop's origin, not two.
    const auto atOrigin = std::count_if(
        result.Stations.begin(), result.Stations.end(), [](const FenceStation& s)
        { return std::abs(s.Pose.Base.x) < 1.0e-3f && std::abs(s.Pose.Base.z) < 1.0e-3f; });
    EXPECT_EQ(atOrigin, 1);

    // The wrap station is a two-run corner like every other one, so it takes
    // the bisector too — emitting it once must not mean yawing it by one run.
    // The last run arrives along -Z, run 0 leaves along +X.
    EXPECT_NEAR(result.Stations[0].Pose.Forward.x, 0.70710678f, 2.0e-3f);
    EXPECT_NEAR(result.Stations[0].Pose.Forward.y, 0.0f, 1.0e-3f);
    EXPECT_NEAR(result.Stations[0].Pose.Forward.z, -0.70710678f, 2.0e-3f);
}

// The shared corner station takes the BISECTOR of both runs' end directions.
// Each run's own chord would give the same physical post two different answers.
TEST(FenceLayout, SharedCornerStationTakesTheBisectorYaw)
{
    const auto center = CorneredL(10.0f, 10.0f, 0.5f);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 12.0f; // two stations per run, so index 1 IS the corner

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Stations.size(), 3u);

    // Run 0 travels +Z, run 1 travels +X: the bisector is 45 degrees between.
    const V3 corner = result.Stations[1].Pose.Forward;
    EXPECT_NEAR(corner.x, 0.70710678f, 2.0e-3f);
    EXPECT_NEAR(corner.y, 0.0f, 1.0e-3f);
    EXPECT_NEAR(corner.z, 0.70710678f, 2.0e-3f);

    // The runs' own ends keep their own chords.
    EXPECT_NEAR(result.Stations[0].Pose.Forward.z, 1.0f, 1.0e-3f);
    EXPECT_NEAR(result.Stations[2].Pose.Forward.x, 1.0f, 1.0e-3f);
}

// Coincident authored points: the run between them is skipped, the shared
// station is emitted once, and the pair is named rather than silently deduped.
TEST(FenceLayout, ZeroLengthRunIsSkippedAndReported)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[4] = {0.0f, 20.0f, 20.0f, 40.0f};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 12.0f;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_TRUE(AnyValidationContains(result, "coincident"));
    ASSERT_EQ(result.Stations.size(), 3u);
    EXPECT_NEAR(result.Stations[0].Pose.Base.z, 0.0f, 1.0e-3f);
    EXPECT_NEAR(result.Stations[1].Pose.Base.z, 10.0f, 1.0e-3f);
    EXPECT_NEAR(result.Stations[2].Pose.Base.z, 20.0f, 1.0e-3f);
}

// ---- Variable-length span fill -------------------------------------------

// Pool pieces need not share a length: the fill draws a sequence and one
// shared stretch closes the run exactly on both authored stations.
TEST(FenceLayout, MixedLengthPoolFillsARunExactlyAndWithinTheCap)
{
    const auto center = StraightLineZ(30.0f, 61u);
    const float32 boundaries[2] = {0.0f, 60.0f};
    const FencePieceBounds pieces[2] = {Piece(2.0f), Piece(3.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.4f;
    params.SpanMaxStretch = 1.25f;
    params.Seed = 7u;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_FALSE(result.Spans.empty());
    EXPECT_EQ(result.Stations.size(), result.Spans.size() + 1u);

    // Both pool lengths are actually drawn — this is the variable-length case,
    // not a uniform one wearing its clothes.
    const bool usesShort = std::any_of(result.Spans.begin(), result.Spans.end(),
                                       [](const FenceSpan& s) { return s.PoolSlot == 0u; });
    const bool usesLong = std::any_of(result.Spans.begin(), result.Spans.end(),
                                      [](const FenceSpan& s) { return s.PoolSlot == 1u; });
    EXPECT_TRUE(usesShort);
    EXPECT_TRUE(usesLong);

    // One shared stretch, inside the cap, and the spans cover the run exactly.
    float32 covered = 0.0f;
    for (const FenceSpan& span : result.Spans)
    {
        EXPECT_LE(span.LengthScale, params.SpanMaxStretch + 1.0e-3f);
        EXPECT_GE(span.LengthScale, 1.0f / params.SpanMaxStretch - 1.0e-3f);
        covered += pieces[span.PoolSlot].Length() * span.LengthScale;
    }
    EXPECT_NEAR(covered, 30.0f, 1.0e-2f);
    EXPECT_TRUE(result.Validation.empty());
}

// The count is seeded from the pool's own pieces, so it misses only when the
// DRAWN sequence differs from the pool's mean — which is exactly why the +-1
// search still earns its place with a mixed-length pool. Mean 4 m seeds two
// pieces for this 6 m run, the draw comes back 2 m + 2 m (1.50x, past the cap),
// and one more lands on 6 m exactly.
TEST(FenceLayout, CountAdjustsUpWhenTheDrawnFillMissesTheCap)
{
    const auto center = StraightLineZ(6.0f, 13u);
    const float32 boundaries[2] = {0.0f, 12.0f};
    const FencePieceBounds pieces[2] = {Piece(2.0f), Piece(6.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.4f; // deliberately unrelated: the pieces decide the count
    params.SpanMaxStretch = 1.25f;
    params.Seed = 1u;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_EQ(result.Spans.size(), 3u); // 3 x 2 m = 6 m, scale 1.0
    for (const FenceSpan& span : result.Spans)
        EXPECT_NEAR(span.LengthScale, 1.0f, 1.0e-3f);
    EXPECT_TRUE(result.Validation.empty());
}

// The count comes from the pool's piece lengths, never from PostPitch. The
// spec's castle case at stock settings: a 20 m wall run of 5 m segments with
// the default 2.4 m pitch. Seeded from the pitch this searched 7, 8 and 9
// pieces and shipped 7 at 0.571x; the four that land on 1.00x were never
// considered.
TEST(FenceLayout, FillCountComesFromPieceLengthNotPostPitch)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds pieces[1] = {Piece(5.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.4f; // the component default, and wrong for this kit
    params.SpanMaxStretch = 1.25f;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 4u);
    for (const FenceSpan& span : result.Spans)
        EXPECT_NEAR(span.LengthScale, 1.0f, 1.0e-4f);
    EXPECT_EQ(result.Stations.size(), 5u);
    EXPECT_TRUE(result.Validation.empty());
}

// The cap can be genuinely unsatisfiable (2 m piece, 3 m run, 1.25x cap has no
// integer count). The search stops after one attempt each way, the run is still
// built — a fence with a hole in it is not what the author asked for — and the
// miss is named.
TEST(FenceLayout, UnsatisfiableStretchIsReportedNotChased)
{
    const auto center = StraightLineZ(3.0f, 7u);
    const float32 boundaries[2] = {0.0f, 6.0f};
    const FencePieceBounds pieces[1] = {Piece(2.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 3.0f;
    params.SpanMaxStretch = 1.25f;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_FALSE(result.Spans.empty());
    // 2 pieces at 0.75x: a COMPRESSION, under the 0.800x floor. Reporting that
    // as "past the 1.250x cap" points the author at the wrong knob, so the
    // direction is pinned, not just the fact of a message.
    EXPECT_TRUE(AnyValidationContains(result, "under the 0.800x floor"));
    EXPECT_FALSE(AnyValidationContains(result, "past the"));
    // The search tries the nominal and its two neighbours and no more, so it
    // must not claim that no count whatsoever fits.
    EXPECT_TRUE(AnyValidationContains(result, "within one of the nominal"));
    EXPECT_EQ(result.Spans.size(), 2u);
    for (const FenceSpan& span : result.Spans)
        EXPECT_NEAR(span.LengthScale, 0.75f, 1.0e-3f);
    // The run still closes on both authored stations.
    EXPECT_NEAR(result.Stations.back().Pose.Base.z, 3.0f, 1.0e-3f);
}

// The other direction: a run too long for its nearest counts is genuinely past
// the cap, and must be worded that way.
TEST(FenceLayout, StretchPastTheCapIsReportedAsTheCapNotTheFloor)
{
    const auto center = StraightLineZ(2.6f, 7u);
    const float32 boundaries[2] = {0.0f, 6.0f};
    const FencePieceBounds pieces[1] = {Piece(2.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanMaxStretch = 1.25f;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 1u);
    EXPECT_NEAR(result.Spans[0].LengthScale, 1.3f, 1.0e-3f);
    EXPECT_TRUE(AnyValidationContains(result, "past the 1.250x cap"));
    EXPECT_FALSE(AnyValidationContains(result, "floor"));
}

// ---- Span frames ----------------------------------------------------------

// Racked spans pitch to follow the grade; stepped spans stay level at the
// LOWER station so no span floats off its downhill end.
TEST(FenceLayout, RackedAndSteppedSpansDifferOnAGrade)
{
    const auto center = RampZ(6.0f, 2.0f, 13u);
    const float32 boundaries[2] = {0.0f, 12.0f};
    const float32 runLength = std::sqrt(6.0f * 6.0f + 2.0f * 2.0f);
    const FencePieceBounds pieces[1] = {Piece(runLength)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = runLength; // exactly one span
    params.SpanPieces = pieces;

    params.SpanGrade = Components::SplineSpanGrade::Racked;
    const FenceLayoutResult racked = BuildFenceLayout(center, params);
    ASSERT_EQ(racked.Spans.size(), 1u);
    // Follows the grade: rise 2 over run 6 is a normalized +y of 2/sqrt(40).
    EXPECT_NEAR(racked.Spans[0].Pose.Forward.y, 2.0f / runLength, 1.0e-3f);
    EXPECT_NEAR(racked.Spans[0].Pose.Base.y, 1.0f, 1.0e-3f); // midpoint of 0 and 2

    params.SpanGrade = Components::SplineSpanGrade::Stepped;
    const FenceLayoutResult stepped = BuildFenceLayout(center, params);
    ASSERT_EQ(stepped.Spans.size(), 1u);
    EXPECT_NEAR(stepped.Spans[0].Pose.Forward.y, 0.0f, 1.0e-3f);
    EXPECT_NEAR(stepped.Spans[0].Pose.Base.y, 0.0f, 1.0e-3f); // the lower station
    // A level span between two stations 6 m apart in XZ is shorter than the
    // racked one, which spans the full 3D distance.
    EXPECT_LT(stepped.Spans[0].LengthScale, racked.Spans[0].LengthScale);

    // The stations themselves are unmoved by the grade mode.
    ASSERT_EQ(stepped.Stations.size(), racked.Stations.size());
    EXPECT_NEAR(stepped.Stations.back().Pose.Base.y, 2.0f, 1.0e-3f);
}

// A stepped riser taller than the piece that has to cover it leaves a gap, and
// that is reported rather than discovered in a screenshot.
TEST(FenceLayout, SteppedRiserTallerThanItsCoverIsReported)
{
    const auto center = RampZ(6.0f, 4.0f, 13u);
    const float32 boundaries[2] = {0.0f, 12.0f};
    const FencePieceBounds pieces[1] = {Piece(2.0f, 1.0f)}; // 1 m tall

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.0f;
    params.SpanGrade = Components::SplineSpanGrade::Stepped;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_TRUE(AnyValidationContains(result, "steps"));
}

// ---- Planting -------------------------------------------------------------

// The measured kits carry a planting skirt below the pivot, so the two plant
// modes are half a metre apart on the same bounds. PivotPlane lands the pivot
// on the ground; BoundsMin lifts until the lowest vertex touches, which is what
// floats a skirted mesh.
TEST(FenceLayout, PlantModeDistinguishesOnSkirtedBounds)
{
    const auto center = StraightLineZ(10.0f, 21u);
    const float32 boundaries[2] = {0.0f, 20.0f};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 12.0f;
    params.HasPostMesh = true;
    // Bounds min y = 0.24 - 0.76 = -0.52: the measured FantasyKingdom skirt.
    params.PostPiece.Center = V3(0.0f, 0.24f, 0.0f);
    params.PostPiece.HalfExtents = V3(0.1f, 0.76f, 0.1f);

    params.PlantMode = Components::SplinePlantMode::PivotPlane;
    const FenceLayoutResult planted = BuildFenceLayout(center, params);
    ASSERT_FALSE(planted.Stations.empty());
    EXPECT_NEAR(planted.Stations[0].Pose.Position.y, 0.0f, 1.0e-4f);
    EXPECT_NEAR(planted.Stations[0].Pose.Base.y, 0.0f, 1.0e-4f);

    params.PlantMode = Components::SplinePlantMode::BoundsMin;
    const FenceLayoutResult lifted = BuildFenceLayout(center, params);
    ASSERT_FALSE(lifted.Stations.empty());
    EXPECT_NEAR(lifted.Stations[0].Pose.Position.y, 0.52f, 1.0e-4f);
    EXPECT_NEAR(lifted.Stations[0].Pose.Base.y, 0.0f, 1.0e-4f);
}

// An empty PostPool is first-class: the stations still exist and still
// articulate the spans, and with no post mesh the pose sits on the centerline.
TEST(FenceLayout, EmptyPostPoolStillArticulatesSpans)
{
    const auto center = CorneredL(10.0f, 10.0f, 0.5f);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const FencePieceBounds pieces[1] = {Piece(2.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.0f;
    params.HasPostMesh = false;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 10u); // 5 per 10 m run at 2 m pieces
    for (const FenceStation& station : result.Stations)
    {
        EXPECT_NEAR(station.Pose.Position.x, station.Pose.Base.x, 1.0e-4f);
        EXPECT_NEAR(station.Pose.Position.y, station.Pose.Base.y, 1.0e-4f);
        EXPECT_NEAR(station.Pose.Position.z, station.Pose.Base.z, 1.0e-4f);
    }
}

// ---- Gap hold and burial --------------------------------------------------

// Hold-then-split: the gap patch runs on the WHOLE centerline before it is cut
// into runs. A run that opens inside the hole must inherit the altitude its
// neighbour ended on, not re-anchor to the first measured sample it contains.
TEST(FenceLayout, GapHoldRunsOnTheWholeCenterlineBeforeSplitting)
{
    auto center = StraightLineZ(20.0f, 41u);
    for (auto& s : center)
        s.Pos.y = 5.0f; // measured ground at 5 m
    // A hole spanning the second run's opening: samples 18..26 unmeasured, and
    // the ground on the far side sits 3 m lower.
    for (uint32 i = 27; i < 41u; ++i)
        center[i].Pos.y = 2.0f;
    for (uint32 i = 18; i <= 26u; ++i)
    {
        center[i].SurfaceValid = false;
        center[i].Pos.y = 100.0f; // authored fallback, deliberately absurd
    }

    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 12.0f;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Stations.size(), 3u);
    // The station at the run boundary sits inside the hole and must hold the
    // altitude the measured run BEFORE it ended on (5 m), never the 2 m on the
    // far side and never the 100 m authored fallback.
    EXPECT_NEAR(result.Stations[1].Pose.Base.y, 5.0f, 1.0e-3f);
    for (const FenceStation& s : result.Stations)
        EXPECT_LT(s.Pose.Base.y, 10.0f);
}

// Spans cast no rays, but the run's draped samples are already dense enough to
// notice the ground rising through a span's underside.
TEST(FenceLayout, GroundRisingThroughASpanIsReported)
{
    auto center = StraightLineZ(10.0f, 21u);
    center[10].Pos.y = 1.0f; // a bump the span cannot clear
    const float32 boundaries[2] = {0.0f, 20.0f};
    const FencePieceBounds pieces[1] = {Piece(10.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 10.0f; // one span across the whole run
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_TRUE(AnyValidationContains(result, "rises through"));

    // The same run without the bump reports nothing.
    center[10].Pos.y = 0.0f;
    const FenceLayoutResult flat = BuildFenceLayout(center, params);
    EXPECT_FALSE(AnyValidationContains(flat, "rises through"));
}

// ---- Determinism ----------------------------------------------------------

// Rebuild idempotence, asserted rather than eyeballed: identical inputs give
// byte-identical stations, spans and pool picks.
TEST(FenceLayout, RebuildIsIdempotentWithPoolsAndSeed)
{
    const auto center = CorneredL(12.0f, 8.0f, 0.5f);
    const float32 boundaries[3] = {0.0f, 24.0f, 40.0f};
    const FencePieceBounds pieces[3] = {Piece(2.0f), Piece(3.0f), Piece(1.5f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.4f;
    params.Seed = 4242u;
    params.SpanPieces = pieces;

    const FenceLayoutResult a = BuildFenceLayout(center, params);
    const FenceLayoutResult b = BuildFenceLayout(center, params);

    ASSERT_FALSE(a.Stations.empty());
    ASSERT_FALSE(a.Spans.empty());
    ASSERT_EQ(a.Stations.size(), b.Stations.size());
    ASSERT_EQ(a.Spans.size(), b.Spans.size());
    EXPECT_EQ(a.Validation, b.Validation);

    for (size_t i = 0; i < a.Stations.size(); ++i)
    {
        EXPECT_EQ(a.Stations[i].Pose.Position.x, b.Stations[i].Pose.Position.x);
        EXPECT_EQ(a.Stations[i].Pose.Position.y, b.Stations[i].Pose.Position.y);
        EXPECT_EQ(a.Stations[i].Pose.Position.z, b.Stations[i].Pose.Position.z);
        EXPECT_EQ(a.Stations[i].Index, b.Stations[i].Index);
    }
    for (size_t i = 0; i < a.Spans.size(); ++i)
    {
        EXPECT_EQ(a.Spans[i].PoolSlot, b.Spans[i].PoolSlot);
        EXPECT_EQ(a.Spans[i].LengthScale, b.Spans[i].LengthScale);
        EXPECT_EQ(a.Spans[i].Pose.Position.z, b.Spans[i].Pose.Position.z);
    }

    // A different seed reshuffles the picks but not the structure.
    params.Seed = 4243u;
    const FenceLayoutResult reseeded = BuildFenceLayout(center, params);
    std::vector<uint32> first, second;
    for (const FenceSpan& s : a.Spans)
        first.push_back(s.PoolSlot);
    for (const FenceSpan& s : reseeded.Spans)
        second.push_back(s.PoolSlot);
    EXPECT_NE(first, second);
}

// Every emitted frame is finite, and every one that is meant to be orthonormal
// is: a fence that produced a NaN basis would poison the transform hierarchy
// silently. Sheared spans are the ONE deliberate exception and they are held to
// the tighter contract instead — the shear is confined to Forward, and nothing
// else about the basis moves — so this stays the single place that answers what
// is true of an emitted frame.
TEST(FenceLayout, EveryEmittedFrameIsOrthonormalAndFinite)
{
    const auto center = ClosedSquare(9.0f, 0.5f);
    const float32 boundaries[5] = {0.0f, 18.0f, 36.0f, 54.0f, 72.0f};
    const FencePieceBounds pieces[2] = {Piece(2.0f), Piece(2.5f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.Closed = true;
    params.PostPitch = 2.4f;
    params.Seed = 11u;
    params.SpanPieces = pieces;

    const auto checkFinite = [](const SplineLayout::TilePose& p, const char* what)
    {
        EXPECT_TRUE(std::isfinite(p.Position.x) && std::isfinite(p.Position.y) &&
                    std::isfinite(p.Position.z))
            << what;
    };
    const auto checkPose = [&checkFinite](const SplineLayout::TilePose& p, const char* what)
    {
        EXPECT_NEAR(V3::Dot(p.Right, p.Right), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Up, p.Up), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Forward, p.Forward), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Right, p.Up), 0.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Right, p.Forward), 0.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Up, p.Forward), 0.0f, 1.0e-4f) << what;
        checkFinite(p, what);
    };
    // A sheared span's Forward is deliberately neither unit nor perpendicular to
    // Up. Everything else must survive it, and the determinant must stay
    // exactly 1: a shear moves no volume and mirrors nothing, so a piece that
    // came out inside-out or scaled would fail here rather than in a screenshot.
    const auto checkShearedPose = [&checkFinite](const SplineLayout::TilePose& p, const char* what)
    {
        EXPECT_NEAR(V3::Dot(p.Right, p.Right), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Up, p.Up), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Right, p.Up), 0.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Right, p.Forward), 0.0f, 1.0e-4f) << what;
        EXPECT_NEAR(V3::Dot(p.Right, V3::Cross(p.Up, p.Forward)), 1.0f, 1.0e-4f) << what;
        EXPECT_TRUE(std::isfinite(p.Forward.x) && std::isfinite(p.Forward.y) &&
                    std::isfinite(p.Forward.z))
            << what;
        checkFinite(p, what);
    };

    for (const Components::SplineSpanGrade grade :
         {Components::SplineSpanGrade::Racked, Components::SplineSpanGrade::Stepped,
          Components::SplineSpanGrade::Sheared})
    {
        params.SpanGrade = grade;
        const FenceLayoutResult result = BuildFenceLayout(center, params);
        ASSERT_FALSE(result.Stations.empty());
        ASSERT_FALSE(result.Spans.empty());

        for (const FenceStation& s : result.Stations)
            checkPose(s.Pose, "station");
        for (const FenceSpan& s : result.Spans)
        {
            if (grade == Components::SplineSpanGrade::Sheared)
                checkShearedPose(s.Pose, "sheared span");
            else
                checkPose(s.Pose, "span");
            EXPECT_TRUE(std::isfinite(s.LengthScale));
            EXPECT_GT(s.LengthScale, 0.0f);
        }
    }
}

// ---- Piece axis: which way a kit piece runs --------------------------------

// The reported defect: a 2.47 x 0.157 m panel filled the run 15.7 deep and
// crosswise, because the fill measured the piece's THICKNESS as its length.
// Length and orientation now come from one axis choice, so a 24.707 m run takes
// ten 2.4707 m panels at 1.00x — not 158 of them.
TEST(FenceLayout, MeasuredKitPanelFillsByItsLengthNotItsThickness)
{
    const auto center = StraightLineZ(24.707f, 249u);
    const float32 boundaries[2] = {0.0f, 248.0f};
    const FencePieceBounds pieces[1] = {MeasuredKitPanel()};

    // The default PostPitch is left in place deliberately: the fill must seed
    // from the pieces' own lengths, never from the pitch.
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_EQ(result.Spans.size(), 10u);
    EXPECT_EQ(result.Stations.size(), 11u);
    for (const FenceSpan& span : result.Spans)
        EXPECT_NEAR(span.LengthScale, 1.0f, 1.0e-2f);
    EXPECT_TRUE(result.Validation.empty());

    // What the same run measured on the thickness would have wanted.
    EXPECT_NEAR(24.707f / (pieces[0].HalfExtents.z * 2.0f), 157.6f, 0.5f);
}

// Orientation and measurement are one answer: the panel's long axis lands ON
// the direction of travel, carries the length scale, and the emitted basis is a
// yaw rather than a mirror (asymmetric kit pieces must not flip).
TEST(FenceLayout, MeasuredKitPanelIsLaidAlongTheSplineNotAcrossIt)
{
    const auto center = StraightLineZ(24.707f, 249u);
    const float32 boundaries[2] = {0.0f, 248.0f};
    const FencePieceBounds pieces[1] = {MeasuredKitPanel()};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_FALSE(result.Spans.empty());
    ASSERT_EQ(pieces[0].Axis(), PieceAxis::X);

    for (const FenceSpan& span : result.Spans)
    {
        Components::Transform t{};
        WriteParentLocalPose(t, IdentityPlacer(), span.Pose, pieces[0].Axis(), span.LengthScale);

        // Local +X runs downstream (+Z world here) and carries the stretch.
        const V3 localX = Column(t, 0);
        EXPECT_NEAR(localX.z, span.LengthScale, 1.0e-3f);
        EXPECT_NEAR(localX.x, 0.0f, 1.0e-3f);
        // Local +Z is the thickness: across travel, unscaled.
        const V3 localZ = Column(t, 2);
        EXPECT_NEAR(std::sqrt(V3::Dot(localZ, localZ)), 1.0f, 1.0e-3f);
        EXPECT_NEAR(std::abs(localZ.x), 1.0f, 1.0e-3f);
        // A yaw, never a mirror.
        EXPECT_NEAR(BasisDeterminant(t), 1.0f, 1.0e-3f);

        // The property the 15.7x violated: the world length the piece covers
        // along travel is the station gap it was fitted to.
        EXPECT_NEAR(span.LengthScale * pieces[0].Length(), 2.4707f, 1.0e-2f);
    }
}

// Structural pin, independent of any mesh: whichever axis a piece is measured
// on is the axis laid along travel and the only one the length scale reaches.
// Nothing in the emission may reach for a fixed axis.
TEST(FenceLayout, TheAxisMeasuredIsTheAxisLaidAlongTravel)
{
    constexpr float32 kScale = 3.0f;
    const V3 travel(0.0f, 0.0f, 1.0f);

    SplineLayout::TilePose pose;
    pose.Right = V3(1.0f, 0.0f, 0.0f);
    pose.Up = V3(0.0f, 1.0f, 0.0f);
    pose.Forward = travel;
    pose.Base = V3(0.0f, 0.0f, 0.0f);
    pose.Position = V3(0.0f, 0.0f, 0.0f);

    for (const PieceAxis axis : {PieceAxis::X, PieceAxis::Z})
    {
        Components::Transform t{};
        WriteParentLocalPose(t, IdentityPlacer(), pose, axis, kScale);

        // Column 0 is where local +X points, column 2 where local +Z points.
        const int measuredColumn = axis == PieceAxis::X ? 0 : 2;
        const int acrossColumn = axis == PieceAxis::X ? 2 : 0;
        const V3 measured = Column(t, measuredColumn);
        const V3 across = Column(t, acrossColumn);

        // The measured axis is the one on the travel direction...
        EXPECT_NEAR(V3::Dot(measured, travel), kScale, 1.0e-3f);
        // ...and the only one the length scale reaches.
        EXPECT_NEAR(std::sqrt(V3::Dot(measured, measured)), kScale, 1.0e-3f);
        EXPECT_NEAR(std::sqrt(V3::Dot(across, across)), 1.0f, 1.0e-3f);
        EXPECT_NEAR(V3::Dot(across, travel), 0.0f, 1.0e-3f);
        EXPECT_GT(BasisDeterminant(t), 0.0f);

        // And it is the axis PieceLength reads: a piece long only on `axis`
        // reports that extent as its length.
        V3 halfExtents(0.05f, 0.5f, 0.05f);
        (axis == PieceAxis::X ? halfExtents.x : halfExtents.z) = 1.5f;
        EXPECT_EQ(ChoosePieceAxis(halfExtents), axis);
        EXPECT_NEAR(PieceLength(halfExtents, axis), 3.0f, 1.0e-4f);
    }
}

// The measured kit table, straight off the shipped FBX. Spans and walls run
// along X; posts, towers and slabs are near-square and keep the Z convention —
// which is what leaves post and tile placement byte-identical.
TEST(FenceLayout, MeasuredKitPiecesResolveToTheAxisTheyWereModelledOn)
{
    struct Case
    {
        const char* Name;
        V3 HalfExtents;
        PieceAxis Expected;
    };
    const Case cases[] = {
        // Spans and walls: modelled running along X.
        {"SM_Prop_Fence_01", V3(1.2353f, 0.8194f, 0.0784f), PieceAxis::X},
        {"SM_Env_Fence_Wood_01", V3(1.2878f, 0.7130f, 0.0863f), PieceAxis::X},
        {"SM_Env_StoneWall_01", V3(2.5647f, 0.5447f, 0.3041f), PieceAxis::X},
        {"SM_Env_StoneWall_03", V3(0.6534f, 0.5353f, 0.2339f), PieceAxis::X},
        {"SM_Bld_Castle_Wall_01", V3(2.5000f, 2.5000f, 0.2500f), PieceAxis::X},
        {"SM_Bld_Railing_01", V3(1.3060f, 0.5500f, 0.1120f), PieceAxis::X},
        {"SM_Env_Hedge_03", V3(1.0452f, 0.6712f, 0.5600f), PieceAxis::X},
        // Posts: no run direction to read.
        {"SM_Env_StoneWall_Pillar_01", V3(0.5033f, 0.5834f, 0.5021f), PieceAxis::Z},
        {"SM_Bld_Castle_Wall_Tower_S_01", V3(1.2218f, 3.7620f, 1.2125f), PieceAxis::Z},
        {"SM_Bld_Railing_Pole_01", V3(0.1120f, 0.5500f, 0.1120f), PieceAxis::Z},
        // The footpath tile the placement recipe ships against: 2.62 x 2.38 m.
        // A 10% edge is modelling noise on a slab, not a run direction, and
        // reading it as one would yaw every placed path tile 90 degrees.
        {"SM_Env_Footpath_01", V3(1.3121f, 0.1333f, 1.1878f), PieceAxis::Z},
        // The one genuinely ambiguous SPAN measured: 1.045 x 1.031 m, an
        // amorphous hedge cube. The two answers differ by 1.4%, so the
        // near-square rule takes it and nothing visible turns on it.
        {"SM_Env_Hedge_04", V3(0.5227f, 0.6526f, 0.5156f), PieceAxis::Z},
    };

    for (const Case& c : cases)
    {
        FencePieceBounds bounds;
        bounds.HalfExtents = c.HalfExtents;
        EXPECT_EQ(bounds.Axis(), c.Expected) << c.Name;
        EXPECT_NEAR(bounds.Length(),
                    (c.Expected == PieceAxis::X ? c.HalfExtents.x : c.HalfExtents.z) * 2.0f,
                    1.0e-4f)
            << c.Name;
    }
}

// Posts are point-placed and near-square, so the axis rule must leave them
// exactly where the pre-fix code put them: local +X on Right, +Z on Forward.
TEST(FenceLayout, NearSquarePostKeepsTheZConventionAndItsPlacement)
{
    const auto center = StraightLineZ(12.0f, 61u);
    const float32 boundaries[2] = {0.0f, 60.0f};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 3.0f;
    params.HasPostMesh = true;
    params.PostPiece = MeasuredKitPost();

    ASSERT_EQ(params.PostPiece.Axis(), PieceAxis::Z);

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Stations.size(), 5u);
    for (const FenceStation& station : result.Stations)
    {
        Components::Transform t{};
        WriteParentLocalPose(t, IdentityPlacer(), station.Pose, params.PostPiece.Axis(), 1.0f);
        EXPECT_NEAR(V3::Dot(Column(t, 0), station.Pose.Right), 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(Column(t, 1), station.Pose.Up), 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(Column(t, 2), station.Pose.Forward), 1.0f, 1.0e-4f);
        EXPECT_NEAR(t.matrix[12], station.Pose.Position.x, 1.0e-4f);
        EXPECT_NEAR(t.matrix[13], station.Pose.Position.y, 1.0e-4f);
        EXPECT_NEAR(t.matrix[14], station.Pose.Position.z, 1.0e-4f);
    }
}

// A span whose two stations sit vertically above one another has no ground-plane
// direction at all; roll falls back to station A's frame instead of producing a
// zero basis.
TEST(FenceLayout, NearVerticalSpanFallsBackWithoutDegenerateBasis)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < 9u; ++i)
        center.push_back({V3(0.0f, static_cast<float32>(i) * 0.5f, 0.0f), V3(0, 1, 0), true});
    const float32 boundaries[2] = {0.0f, 8.0f};
    const FencePieceBounds pieces[1] = {Piece(4.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 4.0f;
    params.SpanPieces = pieces;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_FALSE(result.Spans.empty());
    for (const FenceSpan& s : result.Spans)
    {
        EXPECT_NEAR(V3::Dot(s.Pose.Right, s.Pose.Right), 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(s.Pose.Up, s.Pose.Up), 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(s.Pose.Forward, s.Pose.Forward), 1.0f, 1.0e-4f);
        // The fallback the comment above names: A's up, not merely a unit
        // basis. Deriving the up from the vertical forward instead would give a
        // horizontal up here — a finite, orthonormal, flat-lying panel.
        EXPECT_NEAR(s.Pose.Up.y, 1.0f, 1.0e-4f);
    }
}

// A run too long for the layout budget must come back short AND say so. The
// silent alternative — dropping the whole run on a size mismatch — is the one
// outcome every other rule in this file exists to prevent.
TEST(FenceLayout, RunPastThePieceBudgetIsTruncatedAndReported)
{
    const auto center = StraightLineZ(100.0f, 201u);
    const float32 boundaries[2] = {0.0f, 200.0f};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 0.5f; // 201 stations
    params.MaxPieces = 20u;  // deliberately far too small

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_FALSE(result.Stations.empty());
    EXPECT_LE(result.Stations.size(), 20u);
    EXPECT_FALSE(result.Validation.empty());
    EXPECT_TRUE(AnyValidationContains(result, "budget"));
}

// ---- Near-vertical spans on real ground: design section 4's roll rule ----
//
// The vertical-stack fixture above is the clean case. This is the one that
// shipped broken: on a near-vertical FACE the stations stack almost (not
// exactly) vertically, because arc length there is nearly all altitude. The
// span's forward comes out within a degree of vertical, and deriving its up
// from Cross(forward, Right) yields a HORIZONTAL up — the panel lies flat on
// the ground while its basis stays perfectly orthonormal and finite, so nothing
// downstream can tell.
//
// The threshold is a DEGENERACY escape, not a lean ceiling. Racked spans are
// meant to follow the grade however steep it gets, so these pin BOTH sides:
// racking survives 45 and 60 degrees joining its posts exactly, and the
// fallback only takes over near vertical.

namespace
{

// Ground climbing `rise` metres over `run` metres of horizontal travel, sampled
// densely so stations land ON the face rather than stepping across it.
std::vector<CenterSample> SteepFaceZ(float32 run, float32 rise, uint32 sampleCount)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(sampleCount - 1u);
        CenterSample s;
        s.Pos = V3(0.0f, rise * t, run * t);
        // Fence default is SlopeBlend 0, so stations stay world-up: this
        // fixture cannot be explained by the tile up-blend.
        s.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(s);
    }
    return center;
}

FencePieceBounds PanelPiece()
{
    FencePieceBounds piece;
    piece.Center = V3(0.0f, 0.6f, 0.0f);
    piece.HalfExtents = V3(0.06f, 0.6f, 1.2f); // 2.4 m long, 1.2 m tall
    return piece;
}

constexpr float32 kPiF = 3.14159265358979f;

float32 VectorLength(const V3& v) { return std::sqrt(V3::Dot(v, v)); }

FenceLayoutParams PanelRunParams(std::span<const float32> boundaries,
                                 const FencePieceBounds& piece)
{
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = std::span<const FencePieceBounds>(&piece, 1u);
    params.PostPiece = piece;
    params.HasPostMesh = true;
    return params;
}

// A ramp of the given grade, long enough to carry several spans.
std::vector<CenterSample> GradeRamp(float32 degrees, float32 run, uint32 sampleCount)
{
    return RampZ(run, run * std::tan(degrees * kPiF / 180.0f), sampleCount);
}

} // namespace

// Racking is a her-ruled behaviour: a racked span follows the grade it climbs,
// however steep. These two are the guard against a degeneracy escape quietly
// becoming a style limit — the same regression a 35-degree threshold produced,
// where a 45-degree run went level with its ends 0.83 m off both post bases.
TEST(FenceLayout, RackedSpansStillFollowSteepGradesAndJoinTheirPosts)
{
    const FencePieceBounds piece = PanelPiece();
    for (const float32 grade : {20.0f, 45.0f, 60.0f, 75.0f})
    {
        const auto center = GradeRamp(grade, 20.0f, 161u);
        const float32 boundaries[2] = {0.0f, 160.0f};
        const FenceLayoutResult result =
            BuildFenceLayout(center, PanelRunParams(boundaries, piece));
        ASSERT_FALSE(result.Spans.empty()) << grade << " degrees";
        EXPECT_EQ(result.RollBorrowedSpans, 0u)
            << grade << " degrees must not trip the degeneracy escape";

        for (const FenceSpan& span : result.Spans)
        {
            // Pitches to the grade rather than being held level.
            EXPECT_NEAR(span.Pose.Forward.y, std::sin(grade * kPiF / 180.0f), 0.02f)
                << grade << " degrees, span " << span.Index;
            EXPECT_NEAR(span.Pose.Up.y, std::cos(grade * kPiF / 180.0f), 0.02f)
                << grade << " degrees, span " << span.Index;
        }

        // ...and each span still reaches both of the posts it joins. The
        // regression this catches left the ends short by a third of the chord.
        ASSERT_GE(result.Stations.size(), result.Spans.size() + 1u) << grade << " degrees";
        for (size_t i = 0; i < result.Spans.size(); ++i)
        {
            const FenceSpan& span = result.Spans[i];
            const float32 half = 0.5f * span.LengthScale * piece.Length();
            const V3 tail = span.Pose.Base - span.Pose.Forward * half;
            const V3 head = span.Pose.Base + span.Pose.Forward * half;
            const V3& postA = result.Stations[i].Pose.Base;
            const V3& postB = result.Stations[i + 1u].Pose.Base;
            EXPECT_NEAR(VectorLength(tail - postA), 0.0f, 1.0e-3f)
                << grade << " degrees, span " << i << " tail misses its post";
            EXPECT_NEAR(VectorLength(head - postB), 0.0f, 1.0e-3f)
                << grade << " degrees, span " << i << " head misses its post";
        }
    }
}

TEST(FenceLayout, TheRollEscapeOnlyFiresNearVertical)
{
    const FencePieceBounds piece = PanelPiece();
    const float32 boundaries[2] = {0.0f, 160.0f};

    // Just under the threshold: still racked, still joining its posts.
    const auto shallow = GradeRamp(80.0f, 20.0f, 161u);
    const FenceLayoutResult below =
        BuildFenceLayout(shallow, PanelRunParams(boundaries, piece));
    ASSERT_FALSE(below.Spans.empty());
    EXPECT_EQ(below.RollBorrowedSpans, 0u);
    EXPECT_NEAR(below.Spans.front().Pose.Forward.y, std::sin(80.0f * kPiF / 180.0f), 0.02f);

    // Past it: the escape takes over and the run reports it.
    const auto steep = GradeRamp(88.0f, 20.0f, 161u);
    const FenceLayoutResult above = BuildFenceLayout(steep, PanelRunParams(boundaries, piece));
    ASSERT_FALSE(above.Spans.empty());
    EXPECT_GT(above.RollBorrowedSpans, 0u);
}

TEST(FenceLayout, ANearVerticalSpanStaysUprightInsteadOfLyingFlat)
{
    // The defect: a span between two vertically stacked posts derived a
    // HORIZONTAL up and the panel lay flat on the ground.
    const auto center = SteepFaceZ(0.4f, 12.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};
    const FencePieceBounds piece = PanelPiece();

    const FenceLayoutResult result = BuildFenceLayout(center, PanelRunParams(boundaries, piece));
    ASSERT_FALSE(result.Spans.empty());
    EXPECT_GT(result.RollBorrowedSpans, 0u) << "fixture must reach the degenerate case";

    for (const FenceSpan& span : result.Spans)
    {
        EXPECT_GT(span.Pose.Up.y, std::cos(36.0f * kPiF / 180.0f))
            << "span " << span.Index << " up = (" << span.Pose.Up.x << ", " << span.Pose.Up.y
            << ", " << span.Pose.Up.z << ")";
        EXPECT_NEAR(V3::Dot(span.Pose.Right, span.Pose.Right), 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(span.Pose.Up, span.Pose.Up), 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(span.Pose.Forward, span.Pose.Forward), 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(span.Pose.Right, span.Pose.Up), 0.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(span.Pose.Right, span.Pose.Forward), 0.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(span.Pose.Up, span.Pose.Forward), 0.0f, 1.0e-4f);
    }
}

// The bury check models a span's underside from the span's OWN pose. A span
// that borrowed its roll is level across a face its posts climb, so the
// post-to-post line the check used to assume would call it clear while most of
// the panel is inside the slope.
TEST(FenceLayout, BuryCheckSeesASpanThatBorrowedItsRoll)
{
    const auto center = SteepFaceZ(0.4f, 12.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};
    const FencePieceBounds piece = PanelPiece();

    const FenceLayoutResult result = BuildFenceLayout(center, PanelRunParams(boundaries, piece));
    ASSERT_GT(result.RollBorrowedSpans, 0u);
    EXPECT_TRUE(AnyValidationContains(result, "ground rises through the spans"))
        << "upright panels crossing the face must not pass the bury check silently";
}

TEST(FenceLayout, BuryCheckStillPassesACleanRackedRun)
{
    // Positive control for the test above: the same check on ground the spans
    // genuinely clear must stay quiet, or "buried" would mean nothing.
    const auto center = GradeRamp(20.0f, 20.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};
    const FencePieceBounds piece = PanelPiece();

    const FenceLayoutResult result = BuildFenceLayout(center, PanelRunParams(boundaries, piece));
    ASSERT_FALSE(result.Spans.empty());
    EXPECT_EQ(result.RollBorrowedSpans, 0u);
    EXPECT_FALSE(AnyValidationContains(result, "ground rises through the spans"));
}

// ---- The skirt a kit models below its own pivot ----------------------------

namespace
{

// The measured fence kit's panel: the pivot IS the plant line, with 0.5216 m of
// panel modelled below it and 1.1171 m above. Both figures are read off one
// SplineFence pool in a large island scene, whose 107 placed pieces carry two
// skirts (0.5216 m on 65, 0.5239 m on 42) — which is why the layout reads the
// skirt per piece and never as a constant.
FencePieceBounds SkirtedPanelPiece()
{
    FencePieceBounds piece;
    piece.Center = V3(0.0f, 0.29776f, 0.0f);
    piece.HalfExtents = V3(0.06f, 0.81937f, 1.2f); // 2.4 m long, 1.6387 m tall
    return piece;
}

// The same silhouette and the same footprint, modelled with its bottom ON the
// pivot instead of 0.5216 m under it. Length() and Axis() read HalfExtents
// only, so a run fills identically: the skirt is the one variable between this
// piece and SkirtedPanelPiece.
FencePieceBounds FlushPanelPiece()
{
    FencePieceBounds piece;
    piece.Center = V3(0.0f, 0.81937f, 0.0f);
    piece.HalfExtents = V3(0.06f, 0.81937f, 1.2f);
    return piece;
}

struct GroundBump
{
    float32 AtZ = 0.0f;
    float32 Height = 0.0f;
    float32 Width = 0.0f;
};

// A flat centerline along +Z carrying raised humps, each a raised cosine of the
// given crest height. The ground between humps is exactly y = 0, so a hump's
// crest height IS how far the ground stands above a level span's plant line.
//
// Put a crest MID-SPAN, and narrower than the station pitch. Stations conform
// to this same centerline, so a crest at a station lifts the station instead:
// the spans then rack over the hump, no ground stands above them, and the
// fixture silently tests nothing. With the default 2.4 m pitch on a 24 m run
// the stations land on multiples of 2.4, so the mid-span crests are 1.2, 3.6,
// 6.0, ... 10.8, and z = 12 is the trap.
std::vector<CenterSample> BumpedLineZ(float32 length, uint32 sampleCount,
                                      std::initializer_list<GroundBump> bumps)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        const float32 z = length * static_cast<float32>(i) / static_cast<float32>(sampleCount - 1u);
        float32 y = 0.0f;
        for (const GroundBump& bump : bumps)
        {
            const float32 d = std::abs(z - bump.AtZ);
            if (d < bump.Width * 0.5f)
                y = std::max(y,
                             bump.Height * 0.5f * (1.0f + std::cos(2.0f * kPiF * d / bump.Width)));
        }
        CenterSample s;
        s.Pos = V3(0.0f, y, z);
        s.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(s);
    }
    return center;
}

// No post mesh, which is what the measured fence kits ship: the station poses
// then sit exactly on the draped centerline and carry no piece geometry of
// their own, so the span piece is the only thing an arm varies.
FenceLayoutParams SkirtRunParams(std::span<const float32> boundaries,
                                 const FencePieceBounds& spanPiece)
{
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = std::span<const FencePieceBounds>(&spanPiece, 1u);
    params.HasPostMesh = false;
    return params;
}

struct BuryReport
{
    bool Found = false;
    uint32 Samples = 0;
    uint32 Span = 0;
    float32 Depth = 0.0f;
    float32 ArcMetres = 0.0f;
    float32 X = 0.0f;
    float32 Z = 0.0f;
};

// Reads the figures back out of the author-facing string. Asserting on what the
// author is actually TOLD is the point: a report that stops naming the worst
// offender fails here, where one asserting on an internal the message no longer
// prints would not.
BuryReport ParseBuryReport(const FenceLayoutResult& result)
{
    BuryReport report;
    for (const std::string& message : result.Validation)
    {
        if (message.find("ground rises through the spans") == std::string::npos)
            continue;
        const size_t runAt = message.find("run ");
        const size_t worstAt = message.find("worst at span ");
        if (runAt == std::string::npos || worstAt == std::string::npos)
            continue;
        uint32 from = 0;
        uint32 to = 0;
        if (std::sscanf(message.c_str() + runAt, "run %u->%u at %u sample(s)", &from, &to,
                        &report.Samples) != 3)
            continue;
        if (std::sscanf(message.c_str() + worstAt,
                        "worst at span %u, %f m deeper than the piece is planted for, %f m along "
                        "the run at (%f, %f)",
                        &report.Span, &report.Depth, &report.ArcMetres, &report.X, &report.Z) != 5)
            continue;
        report.Found = true;
        return report;
    }
    return report;
}

} // namespace

TEST(FenceLayout, GroundInsideTheKitsOwnSkirtIsNotBurial)
{
    // Ground standing 0.30 m over the plant line: past the 0.05 m tolerance,
    // and well inside the 0.5216 m of panel this kit models below its pivot.
    // The panel is still showing 0.82 m of itself, which is a fence set into a
    // bank — the thing the kit's skirt exists to do.
    const auto center = BumpedLineZ(24.0f, 241u, {{10.8f, 0.30f, 1.6f}});
    const float32 boundaries[2] = {0.0f, 240.0f};

    // Positive control: the same ground under a panel with NO skirt is burial,
    // which is what makes the silence below a decision rather than a fixture
    // that never reached the case.
    const FencePieceBounds flush = FlushPanelPiece();
    const FenceLayoutResult control = BuildFenceLayout(center, SkirtRunParams(boundaries, flush));
    ASSERT_FALSE(control.Spans.empty());
    ASSERT_TRUE(AnyValidationContains(control, "ground rises through the spans"))
        << "fixture must reach the case: a skirtless panel on this ground IS buried";

    const FencePieceBounds skirted = SkirtedPanelPiece();
    const FenceLayoutResult result = BuildFenceLayout(center, SkirtRunParams(boundaries, skirted));
    ASSERT_EQ(result.Spans.size(), control.Spans.size())
        << "the two arms must differ in the skirt and nothing else";
    EXPECT_FALSE(AnyValidationContains(result, "ground rises through the spans"))
        << "0.30 m of ground over the pivot is inside a 0.52 m skirt, by design";
}

TEST(FenceLayout, GroundPastTheSkirtIsStillBurial)
{
    // 0.80 m of ground over the plant line eats the whole 0.5216 m skirt and
    // 0.28 m of panel with it. Skirt awareness must not become blanket silence.
    const auto center = BumpedLineZ(24.0f, 241u, {{10.8f, 0.80f, 1.6f}});
    const float32 boundaries[2] = {0.0f, 240.0f};
    const FencePieceBounds skirted = SkirtedPanelPiece();

    const FenceLayoutResult result = BuildFenceLayout(center, SkirtRunParams(boundaries, skirted));
    ASSERT_FALSE(result.Spans.empty());
    ASSERT_TRUE(AnyValidationContains(result, "ground rises through the spans"))
        << "ground past the skirt is swallowing panel, and must still be reported";

    // And the depth reported is what is left AFTER the skirt is spent: 0.80 m
    // of ground less the 0.5216 m the piece is drawn to take. This is what pins
    // the allowance to the skirt itself — an allowance of twice the skirt, or
    // of half the panel, reports a different number here.
    const BuryReport report = ParseBuryReport(result);
    ASSERT_TRUE(report.Found);
    EXPECT_NEAR(report.Depth, 0.278f, 0.05f)
        << "the message must charge the ground only for what the skirt does not cover";
}

TEST(FenceLayout, BoundsMinPlantingIsAllowedNoSkirt)
{
    // BoundsMin stands a piece's lowest vertex ON the surface, so it declares
    // no burial to take and gets no allowance. The same 0.30 m bump a
    // PivotPlane fence absorbs is burial here, and a check that spent the
    // skirt regardless of plant mode would go blind to it.
    const auto center = BumpedLineZ(24.0f, 241u, {{10.8f, 0.30f, 1.6f}});
    const float32 boundaries[2] = {0.0f, 240.0f};
    const FencePieceBounds skirted = SkirtedPanelPiece();

    FenceLayoutParams params = SkirtRunParams(boundaries, skirted);
    params.PlantMode = Components::SplinePlantMode::BoundsMin;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_FALSE(result.Spans.empty());
    EXPECT_TRUE(AnyValidationContains(result, "ground rises through the spans"))
        << "a piece standing ON the surface has no skirt to be buried into";
}

TEST(FenceLayout, TheBuryReportNamesTheWorstStationNotTheFirst)
{
    // Two bumps, the shallow one FIRST along the run. A walk that stopped at
    // the first sample past tolerance would name the 0.30 m bump at z = 6; the
    // station an author has to add is at the 0.90 m bump at z = 18.
    const auto center = BumpedLineZ(24.0f, 241u, {{6.0f, 0.30f, 1.6f}, {18.0f, 0.90f, 1.6f}});
    const float32 boundaries[2] = {0.0f, 240.0f};
    const FencePieceBounds flush = FlushPanelPiece();

    const FenceLayoutResult result = BuildFenceLayout(center, SkirtRunParams(boundaries, flush));
    ASSERT_FALSE(result.Spans.empty());

    const BuryReport report = ParseBuryReport(result);
    ASSERT_TRUE(report.Found) << "the report must name the worst span, its depth and where it is";
    EXPECT_GE(report.Samples, 2u) << "both bumps stand over their spans";
    EXPECT_GT(report.Depth, 0.60f)
        << "0.30 m would mean the walk reported the FIRST bump, not the worst";
    EXPECT_LT(report.Depth, 1.10f) << "no sample on this ground stands a metre over its span";
    EXPECT_NEAR(report.ArcMetres, 18.0f, 2.0f) << "the deep bump sits 18 m along the run";
    EXPECT_NEAR(report.Z, 18.0f, 2.0f);
    EXPECT_NEAR(report.X, 0.0f, 0.01f) << "the centerline runs up +Z at x = 0";
    EXPECT_GT(report.Span, 4u) << "the shallow bump at z = 6 falls on span 2 or so";
}

// ---- Sheared spans: the grade taken as a shear, not a rotation --------------
//
// The defect these answer, measured on the shipped village: a Stepped run on
// steep ground levels each panel at its LOWER station, so the panel buries its
// uphill end and stands a step below its neighbour. What the eye catches is
// that step between neighbours rather than the depth itself — the worst run
// steps 0.655 m between two adjacent panels and reads as one of them sinking.
// Racked answers it by rotating the whole piece onto the grade, which leans the
// moulded-in posts with it and was rejected as the shipped look.
//
// Sheared keeps the posts plumb AND lands both ends on their own station, so
// the step is not reduced, it is zero by construction. These pin both halves:
// what the mode does, and that the two rejected modes really do the thing it
// fixes (a fixture where Stepped does not step would prove nothing).

namespace
{

// The grade every test in this section is measured at, steeper than the
// village's 29.74-degree worst so a margin that holds here holds there.
constexpr float32 kShearTestGradeDegrees = 20.0f;

float32 ShearTestGradeTangent()
{
    return std::tan(kShearTestGradeDegrees * kPiF / 180.0f);
}

// Where a span's two ends actually are: its own footprint walked along its own
// forward. For a sheared span that forward is not a unit vector, and using it
// raw is the point — the emitted matrix scales the piece by exactly this.
V3 SpanTail(const FenceSpan& span, const FencePieceBounds& piece)
{
    return span.Pose.Base - span.Pose.Forward * (0.5f * span.LengthScale * piece.Length());
}

V3 SpanHead(const FenceSpan& span, const FencePieceBounds& piece)
{
    return span.Pose.Base + span.Pose.Forward * (0.5f * span.LengthScale * piece.Length());
}

// The quantity the village report ranks its runs by: how far a panel's end
// stands above or below the end of the panel it meets. Zero is a continuous
// fence; anything else is the staircase.
float32 WorstAdjacentEndStep(const FenceLayoutResult& result, const FencePieceBounds& piece)
{
    float32 worst = 0.0f;
    for (size_t k = 0; k + 1u < result.Spans.size(); ++k)
    {
        if (result.Spans[k].Run != result.Spans[k + 1u].Run)
            continue; // a run boundary is a corner, not a neighbouring panel
        worst = std::max(worst, std::abs(SpanHead(result.Spans[k], piece).y -
                                         SpanTail(result.Spans[k + 1u], piece).y));
    }
    return worst;
}

} // namespace

// The whole point of the mode, in the two properties that define it: the piece
// stands plumb, and it reaches both of the stations it joins. Stepped is the
// disjoint control — it holds the first and fails the second, which is exactly
// the trade this mode exists to escape.
TEST(FenceLayout, ShearedSpansStandPlumbAndReachBothStations)
{
    const FencePieceBounds piece = PanelPiece();
    const auto center = GradeRamp(kShearTestGradeDegrees, 20.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};

    FenceLayoutParams params = PanelRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult sheared = BuildFenceLayout(center, params);
    ASSERT_FALSE(sheared.Spans.empty());
    ASSERT_GE(sheared.Stations.size(), sheared.Spans.size() + 1u);
    EXPECT_EQ(sheared.RollBorrowedSpans, 0u) << "20 degrees is nowhere near the degeneracy escape";

    for (size_t i = 0; i < sheared.Spans.size(); ++i)
    {
        const FenceSpan& span = sheared.Spans[i];
        // Plumb: the piece's up is world up, not the grade's normal. A rotated
        // span at this grade would read 0.940 here.
        EXPECT_NEAR(span.Pose.Up.y, 1.0f, 1.0e-4f) << "span " << i;
        EXPECT_NEAR(span.Pose.Right.y, 0.0f, 1.0e-4f) << "span " << i;
        // And the run leaves that plane by exactly the grade.
        EXPECT_NEAR(span.Pose.Forward.y, ShearTestGradeTangent(), 1.0e-3f) << "span " << i;

        // Both ends land on their own station, taken from the layout's stations
        // rather than re-derived from the span's own numbers.
        EXPECT_NEAR(VectorLength(SpanTail(span, piece) - sheared.Stations[i].Pose.Base), 0.0f,
                    1.0e-3f)
            << "span " << i << " tail misses its station";
        EXPECT_NEAR(VectorLength(SpanHead(span, piece) - sheared.Stations[i + 1u].Pose.Base), 0.0f,
                    1.0e-3f)
            << "span " << i << " head misses its station";
    }

    // The control, and the reason this fixture proves anything: the same run
    // Stepped stands just as plumb and misses its uphill station by the riser.
    params.SpanGrade = Components::SplineSpanGrade::Stepped;
    const FenceLayoutResult stepped = BuildFenceLayout(center, params);
    ASSERT_EQ(stepped.Spans.size(), sheared.Spans.size())
        << "the two arms must differ in the grade mode and nothing else";
    EXPECT_NEAR(stepped.Spans.front().Pose.Up.y, 1.0f, 1.0e-4f);
    EXPECT_GT(VectorLength(SpanHead(stepped.Spans.front(), piece) - stepped.Stations[1].Pose.Base),
              0.5f)
        << "a level panel cannot reach the higher of the two stations it joins";
}

// The measured defect itself: the step between the ends of neighbouring panels.
// Sheared drives it to zero by construction; Stepped is where the village's
// 0.655 m comes from.
TEST(FenceLayout, ShearedRunLeavesNoStepBetweenAdjacentPanels)
{
    const FencePieceBounds piece = PanelPiece();
    const auto center = GradeRamp(kShearTestGradeDegrees, 20.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};

    FenceLayoutParams params = PanelRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Stepped;
    const FenceLayoutResult stepped = BuildFenceLayout(center, params);
    ASSERT_GE(stepped.Spans.size(), 3u);
    // A 2.4 m panel on a 20-degree grade steps 0.87 m. The fixture reaches the
    // case, and by a margin over the village's worst 0.655 m.
    EXPECT_GT(WorstAdjacentEndStep(stepped, piece), 0.6f)
        << "fixture must reach the case: a stepped run on this grade IS a staircase";

    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult sheared = BuildFenceLayout(center, params);
    ASSERT_EQ(sheared.Spans.size(), stepped.Spans.size());
    EXPECT_NEAR(WorstAdjacentEndStep(sheared, piece), 0.0f, 1.0e-3f)
        << "sheared panels meet at their shared station, so there is no step to see";

    // Racked closes the step too — and this is why it is not the answer: it
    // buys it by leaning the posts, which is the look that was rejected.
    params.SpanGrade = Components::SplineSpanGrade::Racked;
    const FenceLayoutResult racked = BuildFenceLayout(center, params);
    ASSERT_EQ(racked.Spans.size(), stepped.Spans.size());
    EXPECT_NEAR(WorstAdjacentEndStep(racked, piece), 0.0f, 1.0e-3f);
    EXPECT_NEAR(racked.Spans.front().Pose.Up.y, std::cos(kShearTestGradeDegrees * kPiF / 180.0f),
                1.0e-3f)
        << "racked leans the piece, which is what sheared exists not to do";
}

// A sheared span's underside follows the ground it is planted along, so the
// uphill burial a stepped run pays simply is not there.
//
// This is also the check on the plant line itself. A sheared forward is NOT a
// unit vector, and a projection taken on it raw both scales the answer by that
// length and folds the sample's own height into the line: on this run it would
// under-allow the downhill half of every span by about 0.058 m and report a
// fence that stands exactly on its own ground as buried.
TEST(FenceLayout, ShearedSpansDoNotBuryTheirUphillEnd)
{
    const FencePieceBounds piece = SkirtedPanelPiece();
    const auto center = GradeRamp(kShearTestGradeDegrees, 24.0f, 241u);
    const float32 boundaries[2] = {0.0f, 240.0f};

    FenceLayoutParams params = SkirtRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Stepped;
    const FenceLayoutResult stepped = BuildFenceLayout(center, params);
    ASSERT_FALSE(stepped.Spans.empty());
    ASSERT_TRUE(AnyValidationContains(stepped, "ground rises through the spans"))
        << "fixture must reach the case: a level panel on a 20-degree grade buries its uphill end";

    // What the author is told, against the geometry: a 2.4 m panel steps 0.87 m
    // on this grade, of which the kit's own 0.5216 m skirt absorbs the first
    // part. The rest is panel being swallowed.
    const BuryReport steppedReport = ParseBuryReport(stepped);
    ASSERT_TRUE(steppedReport.Found);
    EXPECT_GT(steppedReport.Depth, 0.15f);

    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult sheared = BuildFenceLayout(center, params);
    ASSERT_EQ(sheared.Spans.size(), stepped.Spans.size())
        << "the two arms must differ in the grade mode and nothing else";
    EXPECT_FALSE(AnyValidationContains(sheared, "ground rises through the spans"))
        << "a span sheared onto the grade stands on it, at both ends and in between";
}

// The plant line's own red arm, with the skirt taken away so nothing can absorb
// the error. A sheared span on a clean ramp stands EXACTLY on its own ground —
// the drape between two stations is a straight line and so is the span — so a
// skirtless one must report nothing at any grade.
//
// A projection taken on the raw sheared forward rather than on its horizontal
// part misreads the line by k^2 times the sample's own height above the span's
// base, k being the grade's tangent. At 35 degrees that is 0.49 x 0.69 m =
// 0.34 m over the downhill half of every span, seven times the 0.05 m
// tolerance, and this run would report a fence standing on its own ground as
// buried on every span forever. The steep arms are here because the error grows
// as k^2: at 10 degrees the same mistake is 0.007 m and hides inside the noise.
TEST(FenceLayout, ShearedSpansOnCleanGroundReportNothingAtAnyGrade)
{
    const FencePieceBounds piece = FlushPanelPiece();
    const float32 boundaries[2] = {0.0f, 240.0f};

    for (const float32 grade : {10.0f, 20.0f, 35.0f, 50.0f})
    {
        const auto center = GradeRamp(grade, 24.0f, 241u);
        FenceLayoutParams params = SkirtRunParams(boundaries, piece);
        params.SpanGrade = Components::SplineSpanGrade::Sheared;
        const FenceLayoutResult result = BuildFenceLayout(center, params);
        ASSERT_FALSE(result.Spans.empty()) << grade << " degrees";
        EXPECT_EQ(result.RollBorrowedSpans, 0u) << grade << " degrees";
        EXPECT_FALSE(AnyValidationContains(result, "ground rises through the spans"))
            << grade << " degrees: a skirtless span sheared onto clean ground is not buried";
    }
}

// A bump under a sheared run must still be caught: shear awareness must not
// become blanket silence, which is the failure mode of every allowance change.
//
// The bump is deliberately SMALL and sits in the FIRST span. A station walk
// spaces stations by arc length, so a tall bump lengthens the polyline and drags
// every later station across it — the crest-mid-span trap the skirt fixtures
// document, and it costs the test its case: a station that climbs the bump
// leaves the span shearing over it with 0.08 m of ground proud instead of 0.30.
TEST(FenceLayout, ShearedSpansStillReportGroundThatStandsThroughThem)
{
    const FencePieceBounds piece = FlushPanelPiece();
    // Stations sit 2.4 m of arc apart, which is 2.26 m of z on this ramp, so the
    // first mid-span is z 1.13 and a 1.0 m wide bump there clears both stations
    // by half a metre even after its own 0.17 m of extra arc has moved them.
    auto center = GradeRamp(kShearTestGradeDegrees, 24.0f, 241u);
    for (CenterSample& sample : center)
    {
        const float32 d = std::abs(sample.Pos.z - 1.13f);
        if (d < 0.5f)
            sample.Pos.y += 0.30f * 0.5f * (1.0f + std::cos(2.0f * kPiF * d / 1.0f));
    }
    const float32 boundaries[2] = {0.0f, 240.0f};

    FenceLayoutParams params = SkirtRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_FALSE(result.Spans.empty());
    ASSERT_TRUE(AnyValidationContains(result, "ground rises through the spans"))
        << "a bump standing through a sheared span is still burial";

    const BuryReport report = ParseBuryReport(result);
    ASSERT_TRUE(report.Found);
    // The crest stands 0.30 m over the ramp, the span is sheared onto the ramp,
    // and this piece declares no skirt — so 0.30 m is what the author is owed.
    EXPECT_NEAR(report.Depth, 0.30f, 0.10f);
    EXPECT_NEAR(report.Z, 1.13f, 0.60f);
    EXPECT_EQ(report.Span, 0u) << "the bump is inside the run's first span";
}

// The escape from near-vertical spans is a property of the geometry, not of one
// grade mode: a shear rate that ran away would emit a piece stretched to the
// horizon, and it fails here instead.
TEST(FenceLayout, ShearedSpansTakeTheRollEscapeNearVertical)
{
    const FencePieceBounds piece = PanelPiece();
    const auto center = SteepFaceZ(0.4f, 12.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};

    FenceLayoutParams params = PanelRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_FALSE(result.Spans.empty());
    EXPECT_GT(result.RollBorrowedSpans, 0u) << "fixture must reach the degenerate case";

    for (const FenceSpan& span : result.Spans)
    {
        // The escape hands back an ORTHONORMAL frame, so a span that took it
        // carries no shear at all — which is the check that the rate never ran.
        EXPECT_NEAR(V3::Dot(span.Pose.Forward, span.Pose.Forward), 1.0f, 1.0e-4f)
            << "span " << span.Index;
        EXPECT_NEAR(V3::Dot(span.Pose.Up, span.Pose.Forward), 0.0f, 1.0e-4f)
            << "span " << span.Index;
        EXPECT_GT(span.Pose.Up.y, std::cos(36.0f * kPiF / 180.0f)) << "span " << span.Index;
        EXPECT_LT(span.LengthScale, 3.0f) << "span " << span.Index << " stretched off the face";
    }
}

// The emitted transform is what the renderer sees, and it is where "the posts
// stay plumb" is finally true or not. Read off the matrix columns rather than
// the pose that produced them.
TEST(FenceLayout, ShearedSpanEmitsAPlumbColumnAndAGradedRun)
{
    const FencePieceBounds piece = PanelPiece();
    const auto center = GradeRamp(kShearTestGradeDegrees, 20.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};
    const PieceAxis axis = piece.Axis();

    FenceLayoutParams params = PanelRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult sheared = BuildFenceLayout(center, params);
    ASSERT_FALSE(sheared.Spans.empty());

    Components::Transform emitted{};
    WriteParentLocalPose(emitted, IdentityPlacer(), sheared.Spans.front().Pose, axis,
                         sheared.Spans.front().LengthScale);

    // The piece's own local +Y — every post, picket and baluster in the mesh —
    // lands exactly on world up.
    const V3 up = Column(emitted, 1);
    EXPECT_NEAR(up.x, 0.0f, 1.0e-4f);
    EXPECT_NEAR(up.y, 1.0f, 1.0e-4f);
    EXPECT_NEAR(up.z, 0.0f, 1.0e-4f);

    // ...while the axis the piece runs along climbs at the grade.
    const V3 along = Column(emitted, axis == PieceAxis::X ? 0 : 2);
    const float32 alongGround = std::sqrt(along.x * along.x + along.z * along.z);
    ASSERT_GT(alongGround, 0.0f);
    EXPECT_NEAR(along.y / alongGround, ShearTestGradeTangent(), 1.0e-3f);

    // A shear moves no volume and mirrors nothing: the determinant is the
    // piece's own length stretch and nothing else.
    EXPECT_NEAR(BasisDeterminant(emitted), sheared.Spans.front().LengthScale, 1.0e-3f);

    // The control: racked emits the same run with its up column leaning off
    // vertical by the grade, which is the difference the mode exists for.
    params.SpanGrade = Components::SplineSpanGrade::Racked;
    const FenceLayoutResult racked = BuildFenceLayout(center, params);
    ASSERT_FALSE(racked.Spans.empty());
    Components::Transform rackedEmitted{};
    WriteParentLocalPose(rackedEmitted, IdentityPlacer(), racked.Spans.front().Pose, axis,
                         racked.Spans.front().LengthScale);
    EXPECT_NEAR(Column(rackedEmitted, 1).y, std::cos(kShearTestGradeDegrees * kPiF / 180.0f),
                1.0e-3f);
}

// Flat ground has no grade to shear, so the mode must be a no-op there: a run
// that moved on level ground would change every fence in the village that is
// not on a slope.
TEST(FenceLayout, ShearedMatchesSteppedOnLevelGround)
{
    const FencePieceBounds piece = PanelPiece();
    const auto center = StraightLineZ(20.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};

    FenceLayoutParams params = PanelRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Stepped;
    const FenceLayoutResult stepped = BuildFenceLayout(center, params);
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult sheared = BuildFenceLayout(center, params);

    ASSERT_FALSE(stepped.Spans.empty());
    ASSERT_EQ(sheared.Spans.size(), stepped.Spans.size());
    for (size_t i = 0; i < sheared.Spans.size(); ++i)
    {
        EXPECT_NEAR(sheared.Spans[i].LengthScale, stepped.Spans[i].LengthScale, 1.0e-5f) << i;
        EXPECT_NEAR(sheared.Spans[i].Pose.Forward.y, 0.0f, 1.0e-5f) << i;
        EXPECT_NEAR(VectorLength(sheared.Spans[i].Pose.Position - stepped.Spans[i].Pose.Position),
                    0.0f, 1.0e-4f)
            << i;
    }
}

// A kit that DOES ship a standalone post is the other half of "the posts": the
// stations are untouched by the grade mode, and a sheared span now meets each
// post at that post's own base instead of passing below it.
TEST(FenceLayout, ShearedSpansMeetAStandalonePostAtItsOwnBase)
{
    const FencePieceBounds panel = PanelPiece();
    const auto center = GradeRamp(kShearTestGradeDegrees, 20.0f, 161u);
    const float32 boundaries[2] = {0.0f, 160.0f};

    FenceLayoutParams params = PanelRunParams(boundaries, panel);
    params.PostPiece = MeasuredKitPost();
    params.HasPostMesh = true;
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    const FenceLayoutResult sheared = BuildFenceLayout(center, params);
    ASSERT_GE(sheared.Spans.size(), 2u);

    for (const FenceStation& station : sheared.Stations)
    {
        // Posts are planted, not sheared: their frames stay orthonormal and plumb.
        EXPECT_NEAR(station.Pose.Up.y, 1.0f, 1.0e-4f);
        EXPECT_NEAR(V3::Dot(station.Pose.Forward, station.Pose.Forward), 1.0f, 1.0e-4f);
    }
    for (size_t i = 0; i < sheared.Spans.size(); ++i)
    {
        EXPECT_NEAR(SpanTail(sheared.Spans[i], panel).y, sheared.Stations[i].Pose.Base.y, 1.0e-3f)
            << "span " << i << " meets its opening post above or below its base";
    }
}

// "The posts stay plumb" is the SHIPPED behaviour, not the invariant. What the
// mode actually promises is that the piece's up stays with its STATIONS — which
// is world up only because the fence default is SlopeBlend 0, "what a real fence
// does". Turn the stations onto the surface and the spans must follow them, or
// the panels would stand at an angle to the posts they join.
TEST(FenceLayout, ShearedSpansTakeTheirUpFromTheStationsNotFromWorldUp)
{
    const FencePieceBounds piece = PanelPiece();
    // A ramp carrying its TRUE surface normal, so blending toward it does
    // something: the default RampZ reports world up at every sample and cannot
    // reach this case.
    const float32 rise = 20.0f * std::tan(kShearTestGradeDegrees * kPiF / 180.0f);
    auto center = RampZ(20.0f, rise, 161u);
    const V3 slopeNormal = V3(0.0f, 20.0f, -rise) *
                           (1.0f / std::sqrt(20.0f * 20.0f + rise * rise));
    for (CenterSample& sample : center)
        sample.Normal = slopeNormal;
    const float32 boundaries[2] = {0.0f, 160.0f};

    FenceLayoutParams params = PanelRunParams(boundaries, piece);
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    params.AlignToSurfaceNormal = true;
    params.SlopeBlend = 1.0f; // posts lean all the way into the slope
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_GE(result.Spans.size(), 2u);
    ASSERT_GE(result.Stations.size(), result.Spans.size() + 1u);

    // The fixture must reach the case: these stations are NOT plumb.
    EXPECT_NEAR(result.Stations.front().Pose.Up.y,
                std::cos(kShearTestGradeDegrees * kPiF / 180.0f), 1.0e-3f)
        << "fixture must reach the case: SlopeBlend 1 has to lean the stations";

    for (size_t i = 0; i < result.Spans.size(); ++i)
    {
        const FenceSpan& span = result.Spans[i];
        const V3 stationUp = result.Stations[i].Pose.Up;
        // The span's up IS the stations' up, not world up.
        EXPECT_NEAR(V3::Dot(span.Pose.Up, stationUp), 1.0f, 1.0e-3f)
            << "span " << i << " left its stations' frame";
        EXPECT_LT(span.Pose.Up.y, 0.999f) << "span " << i << " snapped back to world up";
        // ...and the shear is still confined to it: Right perpendicular to both,
        // determinant still 1, so the piece is neither scaled nor mirrored.
        EXPECT_NEAR(V3::Dot(span.Pose.Right, span.Pose.Up), 0.0f, 1.0e-4f) << i;
        EXPECT_NEAR(V3::Dot(span.Pose.Right, span.Pose.Forward), 0.0f, 1.0e-4f) << i;
        EXPECT_NEAR(V3::Dot(span.Pose.Right, V3::Cross(span.Pose.Up, span.Pose.Forward)), 1.0f,
                    1.0e-4f)
            << i;
        // The invariant that does not care which way up is: both ends still
        // land on the stations they join.
        EXPECT_NEAR(VectorLength(SpanTail(span, piece) - result.Stations[i].Pose.Base), 0.0f,
                    1.0e-3f)
            << "span " << i << " tail";
        EXPECT_NEAR(VectorLength(SpanHead(span, piece) - result.Stations[i + 1u].Pose.Base), 0.0f,
                    1.0e-3f)
            << "span " << i << " head";
    }
}

TEST(FenceLayout, IndependentCompressionFloorKeepsLongRunsWithinStretchCap)
{
    const std::vector<CenterSample> center = {
        {V3(0,0,0),V3(0,1,0),true}, {V3(0,0,.75f),V3(0,1,0),true},
        {V3(0,0,8.16f),V3(0,1,0),true}};
    const float32 boundaries[] = {0,1,2};
    const FencePieceBounds pieces[] = {Piece(5.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = pieces;
    params.SpanMaxStretch = 1.25f;
    params.SpanMinScale = .15f;
    const auto result = BuildFenceLayout(center,params);
    EXPECT_TRUE(result.Validation.empty());
    ASSERT_EQ(result.Spans.size(),3u);
    EXPECT_NEAR(result.Spans[0].LengthScale,.15f,1e-4f);
    EXPECT_NEAR(result.Spans[1].LengthScale,.741f,1e-4f);
    EXPECT_NEAR(result.Spans[2].LengthScale,.741f,1e-4f);
    EXPECT_EQ(result.Spans[0].Run,0u);
    EXPECT_EQ(result.Spans[1].Run,1u);
    EXPECT_NEAR(result.Stations.back().Pose.Base.z,8.16f,1e-4f);
}

TEST(FenceLayout, ExplicitCompressionFailureNamesItsOwnFloor)
{
    const auto center = StraightLineZ(1.0f,3u);
    const float32 boundaries[] = {0,2};
    const FencePieceBounds pieces[] = {Piece(5.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = pieces;
    params.SpanMinScale = .5f;
    const auto result = BuildFenceLayout(center,params);
    EXPECT_TRUE(AnyValidationContains(result,"under the 0.500x floor"));
    EXPECT_TRUE(AnyValidationContains(result,"explicit compression limit"));
    EXPECT_FALSE(AnyValidationContains(result,"reciprocal"));
}

TEST(FenceLayout, InvalidCompressionFloorPreservesLegacyReciprocal)
{
    const auto center = StraightLineZ(3.0f,7u);
    const float32 boundaries[] = {0,6};
    const FencePieceBounds pieces[] = {Piece(2.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = pieces;
    params.SpanMaxStretch = 1.25f;
    for (const float floor : {0.0f,-1.0f,std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::quiet_NaN()})
    {
        params.SpanMinScale = floor;
        const auto result = BuildFenceLayout(center,params);
        EXPECT_TRUE(AnyValidationContains(result,"under the 0.800x floor"));
        EXPECT_TRUE(AnyValidationContains(result,"reciprocal"));
    }
    params.SpanMinScale = 2.0f;
    EXPECT_TRUE(AnyValidationContains(BuildFenceLayout(center,params),"under the 1.000x floor"));
}

// ---- Crest row: the layout does not move when the row is armed ------------

namespace
{

// FNV-1a over the BIT PATTERNS of every float a layout places and the raw value
// of every integer it keys them by. A handful of EXPECT_NEARs cannot say
// "nothing moved"; this can, and that is the whole claim the crest row makes
// about the layouts that came before it.
struct Fingerprint
{
    uint64 Value = 0xCBF29CE484222325ull;

    void MixBits(uint64 v)
    {
        for (int shift = 0; shift < 64; shift += 8)
        {
            Value ^= (v >> shift) & 0xFFull;
            Value *= 0x100000001B3ull;
        }
    }
    void MixFloat(float32 v)
    {
        uint32 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        MixBits(bits);
    }
    void MixVector(const V3& v)
    {
        MixFloat(v.x);
        MixFloat(v.y);
        MixFloat(v.z);
    }
    void MixPose(const TilePose& pose)
    {
        MixVector(pose.Position);
        MixVector(pose.Right);
        MixVector(pose.Up);
        MixVector(pose.Forward);
        MixVector(pose.Base);
        MixBits(pose.StationIndex);
    }
};

uint64 FingerprintStationsAndSpans(const FenceLayoutResult& result)
{
    Fingerprint print;
    print.MixBits(result.Stations.size());
    for (const FenceStation& station : result.Stations)
    {
        print.MixPose(station.Pose);
        print.MixBits(station.Index);
        print.MixBits(station.AuthoredPoint);
        print.MixBits(station.IsAuthoredPoint ? 1u : 0u);
    }
    print.MixBits(result.Spans.size());
    for (const FenceSpan& span : result.Spans)
    {
        print.MixPose(span.Pose);
        print.MixFloat(span.LengthScale);
        print.MixBits(span.PoolSlot);
        print.MixBits(span.Index);
        print.MixBits(span.Run);
        print.MixBits(span.OrdinalInRun);
        print.MixBits(span.IsGate ? 1u : 0u);
    }
    print.MixBits(result.RollBorrowedSpans);
    return print.Value;
}

// The crest row every identity fixture is rebuilt with: two crest lengths, a
// cap pool, a pitch the author typed and a reservation — everything the row can
// do, armed at once, so nothing it could perturb is left untried.
void ArmCrestRow(FenceLayoutParams& params)
{
    static const FencePieceBounds crests[2] = {Piece(1.0f), Piece(0.6f)};
    static const FencePieceBounds caps[1] = {Piece(0.4f)};
    static const FenceReservation reserved[1] = {{0u, 2.0f, 3.0f}};
    params.CrestPieces = crests;
    params.CapPieces = caps;
    params.Reservations = reserved;
    // Below the longest piece on purpose: the row's clamp report is then armed
    // too, so the message order this test pins has something to be about.
    params.CrestPitch = 0.5f;
}

// What an identity fixture is built with beyond its own geometry.
constexpr uint32 kArmNothing = 0u;
// ArmCrestRow: every crest-row feature at once.
constexpr uint32 kArmCrestRow = 1u << 0;
// A full, untouched override table and a gate pool: what a recipe with the
// override feature available but unused hands the layout.
constexpr uint32 kArmEmptyOverrideTable = 1u << 1;

void ArmFixture(FenceLayoutParams& params, uint32 arms)
{
    if ((arms & kArmCrestRow) != 0u)
        ArmCrestRow(params);
    if ((arms & kArmEmptyOverrideTable) != 0u)
    {
        static const Components::SplineFence untouched{};
        static const FencePieceBounds gates[1] = {Piece(3.0f)};
        params.SpanOverrides = untouched.Overrides;
        params.GatePieces = gates;
    }
}

FenceLayoutResult FixtureStraightPanels(uint32 arms)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const FencePieceBounds spans[1] = {MeasuredKitPanel()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.PostPiece = MeasuredKitPost();
    params.HasPostMesh = true;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureRampRacked(uint32 arms)
{
    const auto center = RampZ(20.0f, 4.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds spans[1] = {MeasuredKitPanel()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.SpanGrade = Components::SplineSpanGrade::Racked;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureRampSteppedOnBoundsMin(uint32 arms)
{
    const auto center = RampZ(20.0f, 4.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds spans[1] = {MeasuredKitPanel()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.SpanGrade = Components::SplineSpanGrade::Stepped;
    params.PlantMode = Components::SplinePlantMode::BoundsMin;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureRampSheared(uint32 arms)
{
    const auto center = RampZ(20.0f, 4.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds spans[1] = {MeasuredKitPanel()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.SpanGrade = Components::SplineSpanGrade::Sheared;
    params.AlignToSurfaceNormal = true;
    params.SlopeBlend = 1.0f;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureCorner(uint32 arms)
{
    const auto center = CorneredL(6.0f, 6.0f, 0.5f);
    const float32 boundaries[3] = {0.0f, 12.0f, 24.0f};
    const FencePieceBounds spans[1] = {Piece(2.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.PostPiece = MeasuredKitPost();
    params.HasPostMesh = true;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureClosedSquare(uint32 arms)
{
    const auto center = ClosedSquare(8.0f, 0.5f);
    const float32 boundaries[5] = {0.0f, 16.0f, 32.0f, 48.0f, 64.0f};
    const FencePieceBounds spans[1] = {Piece(4.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.Closed = true;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureStationsOnly(uint32 arms)
{
    const auto center = StraightLineZ(12.0f, 25u);
    const float32 boundaries[2] = {0.0f, 24.0f};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.PostPitch = 2.4f;
    params.PostPiece = MeasuredKitPost();
    params.HasPostMesh = true;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureMixedPoolSeeded(uint32 arms)
{
    const auto center = StraightLineZ(18.0f, 37u);
    const float32 boundaries[3] = {0.0f, 18.0f, 36.0f};
    const FencePieceBounds spans[3] = {Piece(1.5f), Piece(2.0f), Piece(3.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.Seed = 7u;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

FenceLayoutResult FixtureUnsatisfiableStretch(uint32 arms)
{
    const auto center = StraightLineZ(3.0f, 7u);
    const float32 boundaries[2] = {0.0f, 6.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.SpanMinScale = 0.5f;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

// A level arc of `radius` metres turning `degrees` toward +X from the origin,
// heading +Z, sampled every `step` metres of arc. Chords of a curve are shorter
// than the arc they cut, so a fill of whole walls closes each run at a stretch
// other than 1 — the geometry the owner's castle wall is built on.
std::vector<CenterSample> LevelArc(float32 radius, float32 degrees, float32 step)
{
    const float32 arc = radius * degrees * Mathematics::Pi / 180.0f;
    const uint32 steps = static_cast<uint32>(std::lround(arc / step));
    std::vector<CenterSample> center;
    for (uint32 i = 0; i <= steps; ++i)
    {
        const float32 angle = arc * static_cast<float32>(i) / static_cast<float32>(steps) / radius;
        center.push_back({V3(radius - radius * std::cos(angle), 0.0f, radius * std::sin(angle)),
                          V3(0, 1, 0), true});
    }
    return center;
}

// The castle kit's 5 m wall, measured off the editor's LocalBounds: 5 x 5 x
// 0.5 m, laid along local X and pivoted on its corner, not its centre.
FencePieceBounds CastleWall()
{
    FencePieceBounds b;
    b.HalfExtents = V3(2.5f, 2.5f, 0.25f);
    b.Center = V3(-2.5f, 2.5f, 0.0f);
    return b;
}

// A curved castle wall of three runs: whole 5 m walls over a 30 m, 150 degree
// arc, with authored points splitting it unevenly.
std::vector<CenterSample> CurvedCastleCenter() { return LevelArc(30.0f, 150.0f, 0.05f); }

void CurvedCastleBoundaries(const std::vector<CenterSample>& center, float32 (&out)[4])
{
    const float32 last = static_cast<float32>(center.size() - 1u);
    out[0] = 0.0f;
    out[1] = std::round(last * 0.37f);
    out[2] = std::round(last * 0.63f);
    out[3] = last;
}

FenceLayoutResult FixtureCurvedCastle(uint32 arms)
{
    const auto center = CurvedCastleCenter();
    float32 boundaries[4];
    CurvedCastleBoundaries(center, boundaries);
    const FencePieceBounds spans[1] = {CastleWall()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    ArmFixture(params, arms);
    return BuildFenceLayout(center, params);
}

struct IdentityFixture
{
    const char* Name;
    FenceLayoutResult (*Build)(uint32 arms);
    // Whether the armed build must actually lay a row. A fixture that quietly
    // laid none would pass every identity check while testing nothing.
    bool CarriesACrestRow;
    // The armed row, crest by crest.
    std::span<const CrestGolden> CrestRow;
};

const IdentityFixture kIdentityFixtures[] = {
    {"straight panels with posts", FixtureStraightPanels, true,
     SplineLayout::Tests::kStraightPanelsCrestRow},
    {"racked ramp", FixtureRampRacked, true, SplineLayout::Tests::kRampRackedCrestRow},
    {"stepped ramp planted on bounds-min", FixtureRampSteppedOnBoundsMin, true,
     SplineLayout::Tests::kRampSteppedOnBoundsMinCrestRow},
    {"sheared ramp on leaning stations", FixtureRampSheared, true,
     SplineLayout::Tests::kRampShearedCrestRow},
    {"cornered L with a bisector", FixtureCorner, true, SplineLayout::Tests::kCornerCrestRow},
    {"closed square", FixtureClosedSquare, true, SplineLayout::Tests::kClosedSquareCrestRow},
    {"stations only, no span pool", FixtureStationsOnly, false, {}},
    {"mixed span pool, seeded", FixtureMixedPoolSeeded, true,
     SplineLayout::Tests::kMixedPoolSeededCrestRow},
    {"run whose stretch cannot be satisfied", FixtureUnsatisfiableStretch, true,
     SplineLayout::Tests::kUnsatisfiableStretchCrestRow},
    {"curved castle wall, crest shorter than the wall", FixtureCurvedCastle, true,
     SplineLayout::Tests::kCurvedCastleCrestRow},
};

// One coordinate of a crest against its golden, within cross-architecture
// rounding.
void ExpectNearGolden(float32 actual, uint32 goldenBits, const char* fixture, size_t crest,
                      const char* what)
{
    EXPECT_LE(UlpDistance(FloatBits(actual), goldenBits), kMaxGoldenUlps)
        << fixture << ", crest " << crest << " " << what << ": " << actual << " vs golden "
        << std::bit_cast<float32>(goldenBits)
        << " is beyond cross-architecture rounding, so the row really moved";
}

void ExpectNearGolden(const V3& actual, const uint32 (&goldenBits)[3], const char* fixture,
                      size_t crest, const char* what)
{
    ExpectNearGolden(actual.x, goldenBits[0], fixture, crest, what);
    ExpectNearGolden(actual.y, goldenBits[1], fixture, crest, what);
    ExpectNearGolden(actual.z, goldenBits[2], fixture, crest, what);
}

} // namespace

// The crest row is decoration laid over a layout that is already solved:
// arming it must not move one station, one span, or one word the layout already
// said. Bit-for-bit on the poses, and every message the layout made before is
// still there, in order, before anything the row adds. Both builds run on one
// machine, so bit-equality between them is portable.
TEST(FenceLayout, EmptyCrestPoolLeavesEveryExistingFixtureByteIdentical)
{
    for (const IdentityFixture& fixture : kIdentityFixtures)
    {
        const FenceLayoutResult plain = fixture.Build(kArmNothing);
        const FenceLayoutResult armed = fixture.Build(kArmCrestRow);
        EXPECT_TRUE(plain.Crests.empty()) << fixture.Name;
        EXPECT_EQ(!armed.Crests.empty(), fixture.CarriesACrestRow) << fixture.Name;
        EXPECT_EQ(FingerprintStationsAndSpans(plain), FingerprintStationsAndSpans(armed))
            << fixture.Name;
        ASSERT_GE(armed.Validation.size(), plain.Validation.size()) << fixture.Name;
        for (size_t i = 0; i < plain.Validation.size(); ++i)
            EXPECT_EQ(armed.Validation[i], plain.Validation[i]) << fixture.Name;
    }
}

// The row each identity fixture lays is pinned crest by crest, so a change to
// how the row is solved has to name the fixture it moves and say why. Which
// cells the row keeps, and which piece stands in each, is exact; where it
// stands and which way it faces are compared in ULPs against values captured
// on x86-64 (GoldenUlps.h).
TEST(FenceLayout, TheArmedCrestRowOfEveryIdentityFixtureIsPinned)
{
    for (const IdentityFixture& fixture : kIdentityFixtures)
    {
        const FenceLayoutResult armed = fixture.Build(kArmCrestRow);
        EXPECT_EQ(armed.Crests.size(), fixture.CrestRow.size()) << fixture.Name;
        if (armed.Crests.size() != fixture.CrestRow.size())
            continue;
        for (size_t k = 0; k < armed.Crests.size(); ++k)
        {
            const FenceCrest& crest = armed.Crests[k];
            const CrestGolden& golden = fixture.CrestRow[k];
            EXPECT_EQ(crest.Index, golden.Index) << fixture.Name << ", crest " << k;
            EXPECT_EQ(crest.Run, golden.Run) << fixture.Name << ", crest " << k;
            EXPECT_EQ(crest.OrdinalInRun, golden.OrdinalInRun) << fixture.Name << ", crest " << k;
            EXPECT_EQ(crest.PoolSlot, golden.PoolSlot) << fixture.Name << ", crest " << k;
            EXPECT_EQ(crest.IsCap, golden.IsCap) << fixture.Name << ", crest " << k;
            // No identity fixture's crest shares its wall's length, so every row
            // here is on the pitch grid and every piece is whole.
            EXPECT_EQ(crest.LengthScale, 1.0f) << fixture.Name << ", crest " << k;
            ExpectNearGolden(crest.Pose.Position, golden.PositionBits, fixture.Name, k, "position");
            ExpectNearGolden(crest.Pose.Base, golden.BaseBits, fixture.Name, k, "base");
            ExpectNearGolden(crest.Pose.Forward, golden.ForwardBits, fixture.Name, k, "forward");
            ExpectNearGolden(crest.Pose.Up, golden.UpBits, fixture.Name, k, "up");
        }
    }
}

// ---- Crest row: cells ------------------------------------------------------

namespace
{

// The fixture the closed repeat-layout study and this recipe are compared on:
// one 10 m run under 5 m spans, a cell every 2 m, a 1 m crest piece. Its cells
// fall at 1, 3, 5, 7 and 9 m — the same centres that study's own tests assert.
struct CrestFixture
{
    std::vector<CenterSample> Center = StraightLineZ(10.0f, 21u);
    float32 Boundaries[2] = {0.0f, 20.0f};
    FencePieceBounds Spans[1] = {Piece(5.0f)};
    FencePieceBounds Crests[1] = {Piece(1.0f)};

    FenceLayoutParams Params() const
    {
        FenceLayoutParams params;
        params.RunBoundaries = Boundaries;
        params.SpanPieces = Spans;
        params.CrestPieces = Crests;
        params.CrestPitch = 2.0f;
        return params;
    }
};

// Cell identity in emission order. The caps carry a cell's ordinal too, so the
// row's own cells are what an ordinal comparison is about.
std::vector<uint32> CellOrdinals(const FenceLayoutResult& result)
{
    std::vector<uint32> out;
    for (const FenceCrest& crest : result.Crests)
    {
        if (!crest.IsCap)
            out.push_back(crest.Index);
    }
    return out;
}

// Where each cell stands along a fixture that runs down +Z from the origin.
std::vector<float32> CellCentres(const FenceLayoutResult& result)
{
    std::vector<float32> out;
    for (const FenceCrest& crest : result.Crests)
    {
        if (!crest.IsCap)
            out.push_back(crest.Pose.Position.z);
    }
    return out;
}

const FenceCrest* FindCell(const FenceLayoutResult& result, uint32 index)
{
    for (const FenceCrest& crest : result.Crests)
    {
        if (!crest.IsCap && crest.Index == index)
            return &crest;
    }
    return nullptr;
}

std::vector<FenceCrest> CapsOf(const FenceLayoutResult& result)
{
    std::vector<FenceCrest> out;
    for (const FenceCrest& crest : result.Crests)
    {
        if (crest.IsCap)
            out.push_back(crest);
    }
    return out;
}

// The crest fixture with a 2 m tower in the Post pool, 1 m either side of each
// station, and one crest piece of the given length at the given pitch.
FenceLayoutResult CrestRowBetweenTowers(float32 crestLength, float32 pitch, bool towers)
{
    CrestFixture fixture;
    fixture.Crests[0] = Piece(crestLength);
    FenceLayoutParams params = fixture.Params();
    params.CrestPitch = pitch;
    params.PostPiece.HalfExtents = V3(1.0f, 1.0f, 1.0f);
    params.PostPiece.Center = V3(0.0f, 1.0f, 0.0f);
    params.HasPostMesh = towers;
    return BuildFenceLayout(fixture.Center, params);
}

size_t EmptyCrestRowReports(const FenceLayoutResult& result)
{
    size_t count = 0;
    for (const std::string& message : result.Validation)
        count += message.find("crest row placed no crest pieces") != std::string::npos ? 1u : 0u;
    return count;
}

void ExpectSameVector(const V3& a, const V3& b, const char* what)
{
    EXPECT_EQ(a.x, b.x) << what;
    EXPECT_EQ(a.y, b.y) << what;
    EXPECT_EQ(a.z, b.z) << what;
}

// Bit-for-bit, not a tolerance: "this piece did not move" is an exact claim
// whenever the geometry under it did not change either, and a tolerance would
// pass a row that re-phased by less than it.
void ExpectSameCrest(const FenceCrest& a, const FenceCrest& b, const char* what)
{
    EXPECT_EQ(a.Index, b.Index) << what;
    EXPECT_EQ(a.OrdinalInRun, b.OrdinalInRun) << what;
    EXPECT_EQ(a.Run, b.Run) << what;
    EXPECT_EQ(a.PoolSlot, b.PoolSlot) << what;
    EXPECT_EQ(a.IsCap, b.IsCap) << what;
    ExpectSameVector(a.Pose.Position, b.Pose.Position, what);
    ExpectSameVector(a.Pose.Right, b.Pose.Right, what);
    ExpectSameVector(a.Pose.Up, b.Pose.Up, what);
    ExpectSameVector(a.Pose.Forward, b.Pose.Forward, what);
    ExpectSameVector(a.Pose.Base, b.Pose.Base, what);
}

// Cell ordinals within one run, which is what a reservation about a shared
// point is read by: the two runs it covers number their own cells.
std::vector<uint32> OrdinalsInRun(const FenceLayoutResult& result, uint32 run)
{
    std::vector<uint32> out;
    for (const FenceCrest& crest : result.Crests)
    {
        if (!crest.IsCap && crest.Run == run)
            out.push_back(crest.OrdinalInRun);
    }
    return out;
}

std::vector<FenceCrest> CrestsOfRun(const FenceLayoutResult& result, uint32 run)
{
    std::vector<FenceCrest> out;
    for (const FenceCrest& crest : result.Crests)
    {
        if (crest.Run == run)
            out.push_back(crest);
    }
    return out;
}

} // namespace

// The castle kit's defaults: 5 m battlements over 5 m walls, pitch left at 0.
// On a straight run the row registers to walls that stand at their own length,
// so it meets flush, every piece is whole, and the last cell closes exactly on
// the run's end rather than being lost to a rounding millimetre.
TEST(FenceLayout, StraightCastleRowRegistersFlushAtScaleOne)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(5.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Crests.size(), 4u);
    for (uint32 k = 0; k < 4u; ++k)
    {
        const FenceCrest& crest = result.Crests[k];
        EXPECT_EQ(crest.Index, k);
        EXPECT_EQ(crest.OrdinalInRun, k);
        EXPECT_EQ(crest.Run, 0u);
        EXPECT_FALSE(crest.IsCap);
        // Cell k is [5k, 5k+5) and the piece fills it: consecutive pieces meet.
        EXPECT_NEAR(crest.Pose.Position.z, 2.5f + 5.0f * static_cast<float32>(k), 1.0e-4f) << k;
        // On the top of the spans, which stand 1 m tall on the centerline.
        EXPECT_NEAR(crest.Pose.Position.y, 1.0f, 1.0e-4f) << k;
        EXPECT_NEAR(VectorLength(crest.Pose.Forward), 1.0f, 1.0e-5f) << k;
        EXPECT_NEAR(crest.LengthScale, 1.0f, 1.0e-6f) << k;
    }
    EXPECT_TRUE(result.Validation.empty());
}

// The fixtures of the closed repeat-layout study (PR 1558), asserted to its own
// expected ordinals and centres: the recipe's cell rule answers them without
// its solver, its interval type, its float64 or its signed ordinal.
TEST(FenceLayout, CrestRowMatchesRepeatLayoutOnSharedFixtures)
{
    const CrestFixture fixture;

    const FenceLayoutResult plain = BuildFenceLayout(fixture.Center, fixture.Params());
    EXPECT_EQ(CellOrdinals(plain), (std::vector<uint32>{0u, 1u, 2u, 3u, 4u}));
    for (uint32 k = 0; k < 5u; ++k)
        EXPECT_NEAR(CellCentres(plain)[k], 1.0f + 2.0f * static_cast<float32>(k), 1.0e-4f) << k;

    // A 0.2 m opening at 3.5 m: the cell centred at 3 m has its PIECE under it,
    // not its centre, and goes. The footprint is what a reservation is read
    // against.
    FenceLayoutParams params = fixture.Params();
    const FenceReservation narrow[1] = {{0u, 3.4f, 3.6f}};
    params.Reservations = narrow;
    const FenceLayoutResult cut = BuildFenceLayout(fixture.Center, params);
    EXPECT_EQ(CellOrdinals(cut), (std::vector<uint32>{0u, 2u, 3u, 4u}));
    EXPECT_EQ(FindCell(cut, 1u), nullptr);

    const FenceReservation touching[1] = {{0u, 3.5f, 4.5f}};
    params.Reservations = touching;
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{0u, 1u, 2u, 3u, 4u}));

    const FenceReservation nested[6] = {{0u, 4.0f, 6.0f},   {0u, 2.0f, 5.0f},
                                        {0u, 2.5f, 3.0f},   {0u, 4.0f, 6.0f},
                                        {0u, 20.0f, 30.0f}, {0u, -5.0f, -1.0f}};
    params.Reservations = nested;
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{0u, 3u, 4u}));
}

// Touching a reservation's edge is not standing under it: a cell that meets a
// tower's face exactly is kept, by the same millimetre that calls two authored
// points one corner.
TEST(FenceLayout, CellTouchingAReservationIsKept)
{
    const CrestFixture fixture;
    FenceLayoutParams params = fixture.Params();
    const FenceReservation touching[1] = {{0u, 3.5f, 4.5f}};
    params.Reservations = touching;
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{0u, 1u, 2u, 3u, 4u}));

    // Half a millimetre of overlap is the same contact measured on a float
    // axis, and is kept for the same reason.
    const FenceReservation grazing[1] = {{0u, 3.4995f, 4.5f}};
    params.Reservations = grazing;
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{0u, 1u, 2u, 3u, 4u}));

    // Two millimetres is a piece standing under a tower, and goes.
    const FenceReservation biting[1] = {{0u, 3.498f, 4.5f}};
    params.Reservations = biting;
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{0u, 2u, 3u, 4u}));
}

// Unsorted, overlapping and nested reservations answer as one merged stretch,
// and a zero-length one reserves nothing at all.
TEST(FenceLayout, OverlappingReservationsMergeAndZeroLengthOnesAreIgnored)
{
    const CrestFixture fixture;
    FenceLayoutParams params = fixture.Params();
    const FenceReservation nested[4] = {
        {0u, 4.0f, 6.0f}, {0u, 2.0f, 5.0f}, {0u, 2.5f, 3.0f}, {0u, 4.0f, 6.0f}};
    params.Reservations = nested;
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{0u, 3u, 4u}));

    const FenceReservation empty[3] = {{0u, 3.0f, 3.0f}, {0u, 5.0f, 5.0f}, {0u, 9.0f, 9.0f}};
    params.Reservations = empty;
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{0u, 1u, 2u, 3u, 4u}));
}

// Adding a gate — here the stretch of run its piece occupies — takes the cells
// under it away and leaves every other cell exactly where it stood, under the
// ordinal it already had and drawing the piece that ordinal already picked.
TEST(FenceLayout, AddedGateRemovesCellsWithoutRephasingSurvivors)
{
    const CrestFixture fixture;
    // A pool of two lengths and a seed, so a survivor that kept its place but
    // not its pick would still fail.
    const FencePieceBounds crests[2] = {Piece(1.0f), Piece(0.8f)};
    FenceLayoutParams params = fixture.Params();
    params.CrestPieces = crests;
    params.Seed = 3u;
    const FenceLayoutResult before = BuildFenceLayout(fixture.Center, params);
    ASSERT_EQ(before.Crests.size(), 5u);

    const FenceReservation gate[1] = {{0u, 2.0f, 6.0f}};
    params.Reservations = gate;
    const FenceLayoutResult after = BuildFenceLayout(fixture.Center, params);

    EXPECT_EQ(CellOrdinals(after), (std::vector<uint32>{0u, 3u, 4u}));
    for (const FenceCrest& crest : after.Crests)
    {
        const FenceCrest* original = FindCell(before, crest.Index);
        ASSERT_NE(original, nullptr) << crest.Index;
        ExpectSameCrest(crest, *original, "survivor of an added gate");
    }
}

// A station's own post reserves the run it stands on, so the row clears every
// tower automatically the moment a post mesh exists — no field, no clearance.
TEST(FenceLayout, PostFootprintsReserveTheCrestRow)
{
    const CrestFixture fixture;
    FenceLayoutParams params = fixture.Params();
    FencePieceBounds tower;
    tower.HalfExtents = V3(1.0f, 1.0f, 1.0f); // a 2 m tower, 1 m either side
    tower.Center = V3(0.0f, 1.0f, 0.0f);
    params.PostPiece = tower;
    params.HasPostMesh = true;

    // Stations stand at 0, 5 and 10 m, so the cells at 1, 5 and 9 m are under a
    // tower and the cells at 3 and 7 m are not.
    EXPECT_EQ(CellOrdinals(BuildFenceLayout(fixture.Center, params)),
              (std::vector<uint32>{1u, 3u}));
}

// A bump under a bay: the draped centerline climbs over it, the span between
// the bay's two posts does not. A crest piece stands on that span, so the row
// fills the span's chord — the same cells, at the same places, that a flat run
// between the same two posts gives — and no piece overlaps its neighbour, the
// tower it stands beside, or a caller's piece reserved at the closing end.
TEST(FenceLayout, CrestRowOverABumpFillsTheSpanItStandsOn)
{
    const auto flat = StraightLineZ(10.0f, 201u);
    const auto bumped = BumpedLineZ(10.0f, 3.0f, 7.0f, 2.0f, 201u);
    const float32 boundaries[2] = {0.0f, 200.0f};
    float32 drapedLength = 0.0f;
    for (size_t i = 1; i < bumped.size(); ++i)
        drapedLength += VectorLength(bumped[i].Pos - bumped[i - 1u].Pos);
    ASSERT_GT(drapedLength, 11.0f)
        << "fixture must reach the case: the draped run has to be longer than its chord";
    // One span over the whole run, flat or draped: the draped run is inside the
    // stretch cap of a 10 m piece.
    const FencePieceBounds spans[1] = {Piece(10.0f)};
    const FencePieceBounds crests[1] = {Piece(1.0f)};
    FencePieceBounds tower; // 2 m, 1 m either side of its station
    tower.HalfExtents = V3(1.0f, 1.0f, 1.0f);
    tower.Center = V3(0.0f, 1.0f, 0.0f);
    constexpr float32 kCrestHalf = 0.5f;
    constexpr float32 kTowerHalf = 1.0f;
    constexpr float32 kTouching = 1.0e-3f;
    // A caller's piece reaching 1.5 m back from the closing point, and no post
    // mesh. The reservation is measured back from the end of the span; measured
    // back from the end of the draped run, 1.85 m longer, it would cover no
    // cell at all.
    constexpr float32 kClosingReserve = 1.5f;
    const FenceReservation closing[1] = {{1u, -kClosingReserve, 0.0f}};

    struct Arm
    {
        const char* Name;
        bool Towers;
        std::span<const FenceReservation> Reserved;
        uint32 Cells;
        // How far the first and last crest's centre stand, at least, from the
        // opening and the closing station.
        float32 OpeningClearance;
        float32 ClosingClearance;
    };
    const Arm arms[] = {
        {"bare", false, {}, 10u, kCrestHalf, kCrestHalf},
        {"towers", true, {}, 8u, kTowerHalf + kCrestHalf, kTowerHalf + kCrestHalf},
        {"closing reservation", false, closing, 8u, kCrestHalf, kClosingReserve + kCrestHalf},
    };

    for (const Arm& arm : arms)
    {
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;
        params.PostPiece = tower;
        params.HasPostMesh = arm.Towers;
        params.Reservations = arm.Reserved;

        const FenceLayoutResult level = BuildFenceLayout(flat, params);
        const FenceLayoutResult draped = BuildFenceLayout(bumped, params);
        ASSERT_EQ(draped.Spans.size(), 1u) << arm.Name;
        ASSERT_EQ(draped.Stations.size(), 2u) << arm.Name;

        // The cell count and every place a flat run of the chord's length gives.
        EXPECT_EQ(level.Crests.size(), arm.Cells) << arm.Name;
        EXPECT_EQ(draped.Crests.size(), level.Crests.size()) << arm.Name;
        ASSERT_FALSE(draped.Crests.empty()) << arm.Name;
        for (size_t k = 0; k < std::min(draped.Crests.size(), level.Crests.size()); ++k)
        {
            const FenceCrest& crest = draped.Crests[k];
            EXPECT_EQ(crest.OrdinalInRun, level.Crests[k].OrdinalInRun) << arm.Name << " " << k;
            EXPECT_NEAR(VectorLength(crest.Pose.Position - level.Crests[k].Pose.Position), 0.0f,
                        1.0e-3f)
                << arm.Name << " " << k;
        }

        // No two neighbours overlap, measured along the span they stand on.
        for (size_t k = 1; k < draped.Crests.size(); ++k)
        {
            const FenceCrest& a = draped.Crests[k - 1u];
            const FenceCrest& b = draped.Crests[k];
            EXPECT_GE(V3::Dot(b.Pose.Base - a.Pose.Base, a.Pose.Forward),
                      2.0f * kCrestHalf - kTouching)
                << arm.Name << ": crest " << k - 1u << " overlaps crest " << k;
        }
        // Nor whatever stands at either end of the bay.
        const FenceCrest& first = draped.Crests.front();
        const FenceCrest& last = draped.Crests.back();
        EXPECT_GE(V3::Dot(first.Pose.Base - draped.Stations.front().Pose.Base, first.Pose.Forward),
                  arm.OpeningClearance - kTouching)
            << arm.Name;
        EXPECT_GE(V3::Dot(draped.Stations.back().Pose.Base - last.Pose.Base, last.Pose.Forward),
                  arm.ClosingClearance - kTouching)
            << arm.Name;
    }
}

// A stepped span is levelled, so on a grade it stands shorter than the run it
// covers by exactly the climb: 20 m of ground rising 4 m holds 20 one-metre
// pieces on its steps, not the 20.4 the ramp measures, and they butt.
TEST(FenceLayout, CrestRowOnSteppedSpansFillsTheirLevelledLength)
{
    const auto center = RampZ(20.0f, 4.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(1.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.SpanGrade = Components::SplineSpanGrade::Stepped;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_EQ(result.Crests.size(), 20u);
    ASSERT_FALSE(result.Crests.empty());
    EXPECT_NEAR(result.Crests.back().Pose.Base.z + 0.5f, 20.0f, 1.0e-3f);
    for (size_t k = 1; k < result.Crests.size(); ++k)
    {
        const FenceCrest& a = result.Crests[k - 1u];
        const FenceCrest& b = result.Crests[k];
        EXPECT_NEAR(V3::Dot(b.Pose.Base - a.Pose.Base, a.Pose.Forward), 1.0f, 1.0e-3f)
            << "crest " << k - 1u << " and crest " << k << " must butt";
    }
}

// A crest pool that yields not one cell anywhere, on a run long enough to hold
// one bare, is a dead end the author cannot see from the result: the wall
// simply has no row. It is reported once with the widest stretch a piece was
// offered and the piece length certain to fit it — half the stretch at Crest
// Pitch 0, because cells are laid from the run's opening station and a piece
// between half and the whole stretch fits or not by that phase alone.
TEST(FenceLayout, ACrestRowThatPlacesNothingSaysWhyOnce)
{
    constexpr float32 kPieceLength = 0.0f; // Crest Pitch 0: the piece's own length

    // Stations at 0, 5 and 10 m with a 1 m half-footprint leave 3 m clear, and
    // 1.50 m is what always fits it.
    const FenceLayoutResult tooLong = CrestRowBetweenTowers(3.5f, kPieceLength, true);
    ASSERT_TRUE(CellOrdinals(tooLong).empty()) << "fixture no longer reproduces an empty row";
    EXPECT_EQ(EmptyCrestRowReports(tooLong), 1u)
        << "one dead row is one message, however many runs it spans";
    EXPECT_TRUE(AnyValidationContains(tooLong, "the longest clear stretch is 3.00 m."));
    EXPECT_TRUE(AnyValidationContains(
        tooLong, "A crest piece no longer than half of it, 1.50 m, always fits; use one, or "
                 "clear the Post pool."));
    EXPECT_FALSE(AnyValidationContains(tooLong, "plant towers"))
        << "a hand-placed tower reserves nothing, so the row would run through it";

    // A piece shorter than the stretch but longer than half of it still misses
    // every cell here, so the report stands and names the same bound — a bound
    // the piece the author just tried does not meet.
    for (const float32 almost : {2.9f, 2.5f})
    {
        const FenceLayoutResult result = CrestRowBetweenTowers(almost, kPieceLength, true);
        EXPECT_TRUE(CellOrdinals(result).empty()) << almost;
        EXPECT_EQ(EmptyCrestRowReports(result), 1u) << almost;
        EXPECT_TRUE(AnyValidationContains(result, "no longer than half of it, 1.50 m")) << almost;
    }

    // The bound the report names places cells, and the report goes quiet.
    const FenceLayoutResult atTheBound = CrestRowBetweenTowers(1.5f, kPieceLength, true);
    EXPECT_FALSE(CellOrdinals(atTheBound).empty());
    EXPECT_EQ(EmptyCrestRowReports(atTheBound), 0u);

    // The control: the same pool on the same spline with no post to reserve the
    // run places cells and says nothing. Without it the assertions above would
    // pass just as well for a message that fires unconditionally.
    const FenceLayoutResult clear = CrestRowBetweenTowers(3.5f, kPieceLength, false);
    EXPECT_FALSE(CellOrdinals(clear).empty());
    EXPECT_EQ(EmptyCrestRowReports(clear), 0u);

    // And the line the message must not cross: a run shorter than its piece
    // places nothing whether or not posts stand on it, but nothing a post took
    // away would have held a cell — the wall shows the author why, and the pitch
    // ceiling has always been silent there.
    for (const bool towers : {false, true})
    {
        const FenceLayoutResult tooBig = CrestRowBetweenTowers(12.0f, kPieceLength, towers);
        EXPECT_TRUE(CellOrdinals(tooBig).empty()) << towers;
        EXPECT_EQ(EmptyCrestRowReports(tooBig), 0u) << towers;
    }
    // A Crest Pitch longer than the 10 m run.
    const FenceLayoutResult pitchCeiling = CrestRowBetweenTowers(1.0f, 20.0f, true);
    EXPECT_TRUE(CellOrdinals(pitchCeiling).empty());
    EXPECT_EQ(EmptyCrestRowReports(pitchCeiling), 0u);
}

// A typed Crest Pitch decides the bound once it is more than half the stretch:
// a cell holds its piece centred in its pitch, so the piece must leave the
// pitch's slack inside the stretch. A pitch the stretch cannot hold at all is
// named as the thing to lower.
TEST(FenceLayout, TheEmptyCrestRowReportNamesAPitchThatBinds)
{
    const FenceLayoutResult slack = CrestRowBetweenTowers(2.0f, 2.6f, true);
    ASSERT_TRUE(CellOrdinals(slack).empty()) << "fixture no longer reproduces an empty row";
    EXPECT_TRUE(AnyValidationContains(
        slack, "At a Crest Pitch of 2.60 m, a crest piece no longer than 0.40 m (the stretch "
               "less the pitch) always fits"));
    EXPECT_FALSE(CellOrdinals(CrestRowBetweenTowers(0.4f, 2.6f, true)).empty())
        << "the bound the report names must place a cell";

    const FenceLayoutResult noRoom = CrestRowBetweenTowers(3.0f, 6.0f, true);
    ASSERT_TRUE(CellOrdinals(noRoom).empty()) << "fixture no longer reproduces an empty row";
    EXPECT_TRUE(AnyValidationContains(
        noRoom, "Crest Pitch 6.00 m leaves no room for a piece in it; lower the pitch, or clear "
                "the Post pool."));
}

// A pitch typed below the longest piece in the pool would overlap the pieces.
// It is raised to the piece length and the author is told once, whatever the
// spline is made of.
TEST(FenceLayout, PitchBelowTheLongestPieceIsClampedAndReportedOnce)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(0.78f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CrestPitch = 0.5f;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    size_t reports = 0;
    for (const std::string& message : result.Validation)
        reports += message.find("Crest Pitch") != std::string::npos ? 1u : 0u;
    EXPECT_EQ(reports, 1u);
    EXPECT_TRUE(AnyValidationContains(
        result, "Crest Pitch 0.50 m is below the longest crest piece, 0.78 m"));
    // Raised to the piece's own length: the first cell stands at half of it.
    ASSERT_FALSE(result.Crests.empty());
    EXPECT_NEAR(result.Crests.front().Pose.Position.z, 0.39f, 1.0e-4f);
}

// A pool of mixed lengths pitches on the LONGEST piece so every pick fits its
// cell, and a shorter pick is centred in the cell it was drawn for rather than
// pulling the row out of phase.
TEST(FenceLayout, MixedLengthCrestPoolPitchesOnTheLongestPiece)
{
    const CrestFixture fixture;
    const FencePieceBounds crests[2] = {Piece(0.6f), Piece(1.0f)};
    FenceLayoutParams params = fixture.Params();
    params.CrestPieces = crests;
    params.CrestPitch = 0.0f;

    const FenceLayoutResult result = BuildFenceLayout(fixture.Center, params);
    ASSERT_EQ(result.Crests.size(), 10u);
    bool sawShort = false;
    bool sawLong = false;
    for (uint32 k = 0; k < 10u; ++k)
    {
        // Pitch 1 m from the longest piece: cell k is [k, k+1), centre k + 0.5,
        // whichever piece the pick drew.
        EXPECT_NEAR(result.Crests[k].Pose.Position.z, 0.5f + static_cast<float32>(k), 1.0e-4f)
            << k;
        sawShort = sawShort || result.Crests[k].PoolSlot == 0u;
        sawLong = sawLong || result.Crests[k].PoolSlot == 1u;
    }
    EXPECT_TRUE(sawShort) << "fixture must reach the case: the short piece has to be drawn";
    EXPECT_TRUE(sawLong) << "fixture must reach the case: the long piece has to be drawn";
}

// A pitch longer than the run gives no cell, no message and no noise: a short
// stretch between two towers legitimately carries no battlement.
TEST(FenceLayout, RunShorterThanAPitchTakesACapOrNothing)
{
    const auto center = StraightLineZ(1.5f, 7u);
    const float32 boundaries[2] = {0.0f, 6.0f};
    const FencePieceBounds spans[1] = {Piece(1.5f)};
    const FencePieceBounds crests[1] = {Piece(2.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;

    const FenceLayoutResult bare = BuildFenceLayout(center, params);
    EXPECT_TRUE(bare.Crests.empty());
    EXPECT_TRUE(bare.Validation.empty());

    // A cap that fits closes the whole stretch, flush against its opening.
    const FencePieceBounds caps[1] = {Piece(1.0f)};
    params.CapPieces = caps;
    const FenceLayoutResult capped = BuildFenceLayout(center, params);
    ASSERT_EQ(capped.Crests.size(), 1u);
    EXPECT_TRUE(capped.Crests.front().IsCap);
    EXPECT_EQ(capped.Crests.front().OrdinalInRun, 0u);
    EXPECT_NEAR(capped.Crests.front().Pose.Position.z, 0.5f, 1.0e-4f);
    // And it faces the edge it closes against, which is the one it stands on:
    // the same half-turn a cap takes when it closes the lower edge of a stretch
    // that does hold cells.
    EXPECT_NEAR(capped.Crests.front().Pose.Forward.z, -1.0f, 1.0e-5f);
    EXPECT_NEAR(capped.Crests.front().Pose.Right.x, -1.0f, 1.0e-5f);
    EXPECT_NEAR(V3::Dot(capped.Crests.front().Pose.Right,
                        V3::Cross(capped.Crests.front().Pose.Up,
                                  capped.Crests.front().Pose.Forward)),
                1.0f, 1.0e-5f);
    EXPECT_TRUE(capped.Validation.empty());

    // A cap longer than the stretch is an offer the run cannot take, and that
    // is not worth a message.
    const FencePieceBounds oversized[1] = {Piece(1.8f)};
    params.CapPieces = oversized;
    const FenceLayoutResult refused = BuildFenceLayout(center, params);
    EXPECT_TRUE(refused.Crests.empty());
    EXPECT_TRUE(refused.Validation.empty());
}

// ---- Crest row: caps -------------------------------------------------------

// Each remainder takes the longest cap that fits it, flush against the cells,
// so what the pool cannot close lies against the post that covers it.
TEST(FenceLayout, CapFillsTheRemainderWithTheLongestPieceThatFits)
{
    const CrestFixture fixture;
    const FencePieceBounds caps[3] = {Piece(0.3f), Piece(0.4f), Piece(0.6f)};
    FenceLayoutParams params = fixture.Params();
    params.CapPieces = caps;

    const FenceLayoutResult result = BuildFenceLayout(fixture.Center, params);
    EXPECT_EQ(CellOrdinals(result), (std::vector<uint32>{0u, 1u, 2u, 3u, 4u}));
    const std::vector<FenceCrest> caps2 = CapsOf(result);
    ASSERT_EQ(caps2.size(), 2u);
    // The cells hold [0.5, 9.5], so each remainder is 0.5 m: the 0.6 m cap is
    // too long, the 0.4 m one is the longest that fits, and 0.1 m is left
    // against the post at either end.
    EXPECT_EQ(caps2[0].PoolSlot, 1u);
    EXPECT_EQ(caps2[1].PoolSlot, 1u);
    EXPECT_NEAR(caps2[0].Pose.Position.z, 0.3f, 1.0e-4f);
    EXPECT_NEAR(caps2[1].Pose.Position.z, 9.7f, 1.0e-4f);
    // Emitted in run order, between which the row's own cells stand.
    EXPECT_TRUE(result.Crests.front().IsCap);
    EXPECT_TRUE(result.Crests.back().IsCap);
}

// A cap faces the edge it closes. At a run's start that is a half-turn about
// the span's up — the basis keeps its determinant, so an asymmetric end piece
// is turned around and never mirrored.
TEST(FenceLayout, CapFacesTheEdgeItClosesWithoutMirroring)
{
    const CrestFixture fixture;
    const FencePieceBounds caps[1] = {Piece(0.4f)};
    FenceLayoutParams params = fixture.Params();
    params.CapPieces = caps;

    const std::vector<FenceCrest> caps2 = CapsOf(BuildFenceLayout(fixture.Center, params));
    ASSERT_EQ(caps2.size(), 2u);
    const FenceCrest& opening = caps2[0];
    const FenceCrest& closing = caps2[1];

    EXPECT_NEAR(opening.Pose.Forward.z, -1.0f, 1.0e-5f);
    EXPECT_NEAR(opening.Pose.Right.x, -1.0f, 1.0e-5f);
    EXPECT_NEAR(opening.Pose.Up.y, 1.0f, 1.0e-5f);
    EXPECT_NEAR(closing.Pose.Forward.z, 1.0f, 1.0e-5f);
    EXPECT_NEAR(closing.Pose.Right.x, 1.0f, 1.0e-5f);
    for (const FenceCrest& cap : caps2)
    {
        EXPECT_NEAR(V3::Dot(cap.Pose.Right, V3::Cross(cap.Pose.Up, cap.Pose.Forward)), 1.0f,
                    1.0e-5f)
            << "a cap is turned around, never mirrored";
    }
}

// A cap answers to the cell it stands in, so the piece that closes a run end
// carries the ordinal of the partial cell that was dropped there.
TEST(FenceLayout, CapInheritsTheOrdinalOfItsCell)
{
    const auto center = StraightLineZ(9.0f, 19u);
    const float32 boundaries[2] = {0.0f, 18.0f};
    const FencePieceBounds spans[1] = {Piece(4.5f)};
    const FencePieceBounds crests[1] = {Piece(1.0f)};
    const FencePieceBounds caps[2] = {Piece(0.4f), Piece(1.2f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CapPieces = caps;
    params.CrestPitch = 2.0f;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    // Cell 4 reaches 9.5 m and is dropped whole; the 1.5 m left over takes the
    // 1.2 m cap, which stands in cell 4 and says so. The half-metre the 1 m
    // pieces leave inside cell 0 takes the 0.4 m cap, and answers to cell 0.
    EXPECT_EQ(CellOrdinals(result), (std::vector<uint32>{0u, 1u, 2u, 3u}));
    const std::vector<FenceCrest> placedCaps = CapsOf(result);
    ASSERT_EQ(placedCaps.size(), 2u);
    EXPECT_EQ(placedCaps[0].PoolSlot, 0u);
    EXPECT_EQ(placedCaps[0].OrdinalInRun, 0u);
    EXPECT_EQ(placedCaps[0].Index, 0u);
    EXPECT_NEAR(placedCaps[0].Pose.Position.z, 0.3f, 1.0e-4f);
    EXPECT_EQ(placedCaps[1].PoolSlot, 1u);
    EXPECT_EQ(placedCaps[1].OrdinalInRun, 4u);
    EXPECT_EQ(placedCaps[1].Index, 4u);
    EXPECT_NEAR(placedCaps[1].Pose.Position.z, 8.1f, 1.0e-4f);
}

// ---- Crest row: edits ------------------------------------------------------

// A tower planted at a node stands on both of the runs that meet there, and
// the run closing on the node is the half a caller cannot address by any
// distance it knows: the run's draped length is the layout's own arithmetic.
// One signed interval about the authored point says it.
TEST(FenceLayout, ReservationAboutASharedPointReachesBothRuns)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(1.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CrestPitch = 2.0f;

    // A 2 m tower at the shared point: one metre back into the run closing on
    // it, one metre forward into the run opening at it.
    const FenceReservation tower[1] = {{1u, -1.0f, 1.0f}};
    params.Reservations = tower;
    const FenceLayoutResult shared = BuildFenceLayout(center, params);
    EXPECT_EQ(OrdinalsInRun(shared, 0u), (std::vector<uint32>{0u, 1u, 2u, 3u}));
    EXPECT_EQ(OrdinalsInRun(shared, 1u), (std::vector<uint32>{1u, 2u, 3u, 4u}));

    // The last authored point of an open spline is a closing end and nothing
    // else, so only the negative side of its interval has a run to cover.
    const FenceReservation end[1] = {{2u, -1.0f, 0.0f}};
    params.Reservations = end;
    const FenceLayoutResult closing = BuildFenceLayout(center, params);
    EXPECT_EQ(OrdinalsInRun(closing, 0u), (std::vector<uint32>{0u, 1u, 2u, 3u, 4u}));
    EXPECT_EQ(OrdinalsInRun(closing, 1u), (std::vector<uint32>{0u, 1u, 2u, 3u}));

    // On a closed spline the first authored point is both ends of the loop, and
    // the one entry clears the last run's tail and the first run's head.
    const auto square = ClosedSquare(8.0f, 0.5f);
    const float32 loop[5] = {0.0f, 16.0f, 32.0f, 48.0f, 64.0f};
    const FencePieceBounds loopSpans[1] = {Piece(4.0f)};
    FenceLayoutParams closed;
    closed.RunBoundaries = loop;
    closed.Closed = true;
    closed.SpanPieces = loopSpans;
    closed.CrestPieces = crests;
    closed.CrestPitch = 2.0f;
    const FenceReservation corner[1] = {{0u, -1.0f, 1.0f}};
    closed.Reservations = corner;
    const FenceLayoutResult looped = BuildFenceLayout(square, closed);
    EXPECT_EQ(OrdinalsInRun(looped, 0u), (std::vector<uint32>{1u, 2u, 3u}));
    EXPECT_EQ(OrdinalsInRun(looped, 3u), (std::vector<uint32>{0u, 1u, 2u}));
    EXPECT_EQ(OrdinalsInRun(looped, 1u), (std::vector<uint32>{0u, 1u, 2u, 3u}));
}

// Trimming a run's far end changes how many cells it holds and nothing else:
// the survivors keep their ordinals and their phase from the run's opening
// station, and the run before them is untouched to the bit.
TEST(FenceLayout, FarEndTrimLeavesSurvivorsByteIdentical)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const auto trimmed = StraightLineZ(19.0f, 39u);
    const float32 trimmedBoundaries[3] = {0.0f, 20.0f, 38.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(1.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CrestPitch = 2.0f;
    const FenceLayoutResult before = BuildFenceLayout(center, params);

    FenceLayoutParams shorter = params;
    shorter.RunBoundaries = trimmedBoundaries;
    const FenceLayoutResult after = BuildFenceLayout(trimmed, shorter);

    const std::vector<FenceCrest> firstBefore = CrestsOfRun(before, 0u);
    const std::vector<FenceCrest> firstAfter = CrestsOfRun(after, 0u);
    ASSERT_EQ(firstBefore.size(), firstAfter.size());
    for (size_t i = 0; i < firstBefore.size(); ++i)
        ExpectSameCrest(firstAfter[i], firstBefore[i], "run before the trimmed one");

    // The trimmed run loses its last cell and re-phases none of the rest: each
    // survivor still stands at its own k * pitch from the run's opening
    // station, which the trim did not move.
    const std::vector<FenceCrest> lastBefore = CrestsOfRun(before, 1u);
    const std::vector<FenceCrest> lastAfter = CrestsOfRun(after, 1u);
    ASSERT_EQ(lastBefore.size(), 5u);
    ASSERT_EQ(lastAfter.size(), 4u);
    for (size_t i = 0; i < lastAfter.size(); ++i)
    {
        EXPECT_EQ(lastAfter[i].Index, lastBefore[i].Index) << i;
        EXPECT_EQ(lastAfter[i].OrdinalInRun, lastBefore[i].OrdinalInRun) << i;
        EXPECT_FLOAT_EQ(lastAfter[i].Pose.Position.z, lastBefore[i].Pose.Position.z) << i;
    }
}

// Adding or deleting an authored point at the end of a spline leaves every
// other run alone, down to the bit — including the pool piece each cell drew,
// because no earlier run's cell count changed.
TEST(FenceLayout, AddingOrDeletingAnAuthoredPointLeavesOtherRunsByteIdentical)
{
    const auto center = StraightLineZ(30.0f, 61u);
    const float32 twoRuns[3] = {0.0f, 20.0f, 40.0f};
    const float32 threeRuns[4] = {0.0f, 20.0f, 40.0f, 60.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[2] = {Piece(1.0f), Piece(0.8f)};

    FenceLayoutParams params;
    params.RunBoundaries = twoRuns;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CrestPitch = 2.0f;
    params.Seed = 11u;
    const FenceLayoutResult before = BuildFenceLayout(center, params);

    FenceLayoutParams extended = params;
    extended.RunBoundaries = threeRuns;
    const FenceLayoutResult after = BuildFenceLayout(center, extended);

    for (uint32 run = 0; run < 2u; ++run)
    {
        const std::vector<FenceCrest> was = CrestsOfRun(before, run);
        const std::vector<FenceCrest> is = CrestsOfRun(after, run);
        ASSERT_EQ(was.size(), is.size()) << run;
        for (size_t i = 0; i < was.size(); ++i)
            ExpectSameCrest(is[i], was[i], "run untouched by an added point");
    }
    EXPECT_EQ(CrestsOfRun(after, 2u).size(), 5u);
}

// Dragging the point a run OPENS at re-lays that run — the author moved its
// origin — and leaves the run before it standing on its own opening station.
TEST(FenceLayout, OpeningPointDragRelaysOnlyItsRun)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const float32 dragged[3] = {0.0f, 22.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(1.0f)};

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CrestPitch = 2.0f;
    const FenceLayoutResult before = BuildFenceLayout(center, params);

    FenceLayoutParams moved = params;
    moved.RunBoundaries = dragged;
    const FenceLayoutResult after = BuildFenceLayout(center, moved);

    // The first run still lays its cells from its own opening station at 0 m.
    const std::vector<FenceCrest> firstBefore = CrestsOfRun(before, 0u);
    const std::vector<FenceCrest> firstAfter = CrestsOfRun(after, 0u);
    ASSERT_EQ(firstBefore.size(), 5u);
    ASSERT_EQ(firstAfter.size(), 5u);
    for (size_t i = 0; i < firstAfter.size(); ++i)
    {
        EXPECT_EQ(firstAfter[i].OrdinalInRun, firstBefore[i].OrdinalInRun) << i;
        EXPECT_FLOAT_EQ(firstAfter[i].Pose.Position.z, firstBefore[i].Pose.Position.z) << i;
    }

    // The second run re-lays from where its opening station now stands: its
    // first cell is one half-pitch past 11 m, not past 10 m.
    const std::vector<FenceCrest> secondAfter = CrestsOfRun(after, 1u);
    ASSERT_FALSE(secondAfter.empty());
    EXPECT_NEAR(secondAfter.front().Pose.Position.z, 12.0f, 1.0e-4f);
    EXPECT_EQ(secondAfter.front().OrdinalInRun, 0u);
}

// ---- Crest row: limits -----------------------------------------------------

// A cell whose footprint closes exactly on a decimal pitch keeps its place:
// the float32 run axis is read with the same millimetre everything else here
// uses, so the last cell of an exactly divided run is never lost to rounding.
TEST(FenceLayout, DecimalPitchBoundariesKeepTheirCell)
{
    for (const float32 pitch : {0.3f, 0.5f, 0.6f, 1.5f, 2.4f})
    {
        const auto center = StraightLineZ(12.0f, 49u);
        const float32 boundaries[2] = {0.0f, 48.0f};
        const FencePieceBounds spans[1] = {Piece(6.0f)};
        const FencePieceBounds crests[1] = {Piece(pitch)};
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;

        const FenceLayoutResult result = BuildFenceLayout(center, params);
        const size_t expected = static_cast<size_t>(std::lround(12.0f / pitch));
        ASSERT_EQ(result.Crests.size(), expected) << pitch;
        EXPECT_NEAR(result.Crests.back().Pose.Position.z + pitch * 0.5f, 12.0f, 1.0e-3f) << pitch;
    }
}

// A cell's global ordinal is its place along the whole spline counted before
// anything removed it, so the run after a reserved one starts where it would
// have started with nothing reserved at all. Whatever the budget spends, and
// whatever a tower clears, the numbering is a property of the geometry.
TEST(FenceLayout, GlobalIndexCountsEveryCellOfTheRunBefore)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(0.5f)};
    // Nine pieces of structure -- four stations, four spans, the closing
    // station -- and room for three of the row.
    const FenceReservation gate[1] = {{0u, 0.0f, 9.5f}};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.Reservations = gate;
    params.MaxPieces = 12u;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Stations.size() + result.Spans.size(), 9u);
    ASSERT_EQ(result.Crests.size(), 3u);
    // The gate clears all of the first run but its last half metre, whose cell
    // is the twentieth of twenty and says so.
    EXPECT_EQ(result.Crests[0].Run, 0u);
    EXPECT_EQ(result.Crests[0].OrdinalInRun, 19u);
    EXPECT_EQ(result.Crests[0].Index, 19u);
    EXPECT_NEAR(result.Crests[0].Pose.Position.z, 9.75f, 1.0e-4f);
    // So the second run's first cell is the twenty-first, not the fourteenth
    // the piece budget would have counted to.
    EXPECT_EQ(result.Crests[1].Run, 1u);
    EXPECT_EQ(result.Crests[1].OrdinalInRun, 0u);
    EXPECT_EQ(result.Crests[1].Index, 20u);
    EXPECT_EQ(result.Crests[2].Index, 21u);
    EXPECT_TRUE(AnyValidationContains(result, "the rest of the crest row is not built"));
}

// The row is emitted after every station and span of the whole spline, so one
// shared budget cuts decoration before it cuts structure.
TEST(FenceLayout, CrestsAreEmittedAfterSpansUnderOneBudget)
{
    const CrestFixture fixture;
    FenceLayoutParams params = fixture.Params();
    params.MaxPieces = 5u; // 3 stations and 2 spans, and nothing left over

    const FenceLayoutResult result = BuildFenceLayout(fixture.Center, params);
    EXPECT_EQ(result.Stations.size(), 3u);
    EXPECT_EQ(result.Spans.size(), 2u);
    EXPECT_TRUE(result.Crests.empty());
    EXPECT_TRUE(AnyValidationContains(result, "the rest of the crest row is not built"));

    // Room for two cells: what fits is built and the cut is still reported,
    // rather than the whole row being voided over it.
    params.MaxPieces = 7u;
    const FenceLayoutResult partial = BuildFenceLayout(fixture.Center, params);
    EXPECT_EQ(partial.Stations.size(), 3u);
    EXPECT_EQ(partial.Spans.size(), 2u);
    EXPECT_EQ(CellOrdinals(partial), (std::vector<uint32>{0u, 1u}));
    EXPECT_TRUE(AnyValidationContains(partial, "the rest of the crest row is not built"));

    // A budget that structure itself cannot meet reports the cut once, as the
    // cut it is: the row never gets to speak over it.
    params.MaxPieces = 3u;
    const FenceLayoutResult starved = BuildFenceLayout(fixture.Center, params);
    EXPECT_EQ(starved.Stations.size() + starved.Spans.size(), 3u);
    EXPECT_TRUE(starved.Crests.empty());
    EXPECT_FALSE(AnyValidationContains(starved, "crest row"));
    EXPECT_TRUE(AnyValidationContains(starved, "the rest of the spline is not built"));
}

// The row stands on the tops of the spans, so a recipe that asks for one
// without a span pool is told rather than quietly given nothing.
TEST(FenceLayout, CrestRowWithoutSpansIsReported)
{
    const auto center = StraightLineZ(10.0f, 21u);
    const float32 boundaries[2] = {0.0f, 20.0f};
    const FencePieceBounds crests[1] = {Piece(1.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.CrestPieces = crests;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_TRUE(result.Crests.empty());
    EXPECT_TRUE(AnyValidationContains(result, "the span pool is empty"));
}

// ---- Crest row: frames -----------------------------------------------------

// A kit span is modelled with a planting skirt below its pivot, and the plant
// mode decides how much of that skirt is underground. The row stands on the
// mesh's TOP either way: on the measured panel that is 1.1172 m above the
// pivot, not the 1.6388 m the piece is authored tall.
TEST(FenceLayout, CrestSitsOnTheSpanMeshTopUnderEitherPlantMode)
{
    const auto center = StraightLineZ(10.0f, 21u);
    const float32 boundaries[2] = {0.0f, 20.0f};
    const FencePieceBounds spans[1] = {MeasuredKitPanel()};
    const FencePieceBounds crests[1] = {Piece(1.0f)};
    const float32 topAbovePivot = MeasuredKitPanel().Center.y + MeasuredKitPanel().HalfExtents.y;
    ASSERT_NEAR(topAbovePivot, 1.1172f, 1.0e-4f) << "fixture must carry a skirt";

    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CrestPitch = 2.0f;

    // PivotPlane plants the pivot on the ground and buries the skirt, so the
    // visible top is the geometry above the pivot and nothing else.
    params.PlantMode = Components::SplinePlantMode::PivotPlane;
    const FenceLayoutResult planted = BuildFenceLayout(center, params);
    ASSERT_FALSE(planted.Crests.empty());
    ASSERT_FALSE(planted.Spans.empty());
    EXPECT_NEAR(planted.Spans.front().Pose.Position.y, 0.0f, 1.0e-4f);
    for (const FenceCrest& crest : planted.Crests)
    {
        EXPECT_NEAR(crest.Pose.Position.y,
                    planted.Spans.front().Pose.Position.y + topAbovePivot, 1.0e-3f);
    }

    // BoundsMin lifts the piece until its lowest vertex rests on the ground, so
    // the same top stands a whole skirt higher. The row follows it.
    params.PlantMode = Components::SplinePlantMode::BoundsMin;
    const FenceLayoutResult lifted = BuildFenceLayout(center, params);
    ASSERT_EQ(lifted.Crests.size(), planted.Crests.size());
    EXPECT_NEAR(lifted.Spans.front().Pose.Position.y, MeasuredKitPanel().SkirtDepth(), 1.0e-4f);
    for (const FenceCrest& crest : lifted.Crests)
    {
        EXPECT_NEAR(crest.Pose.Position.y,
                    lifted.Spans.front().Pose.Position.y + topAbovePivot, 1.0e-3f);
    }
    // Which is the whole authored height above the ground, and half a metre
    // above where the planted row stands.
    EXPECT_NEAR(lifted.Crests.front().Pose.Position.y, MeasuredKitPanel().Height(), 1.0e-3f);
    EXPECT_NEAR(lifted.Crests.front().Pose.Position.y - planted.Crests.front().Pose.Position.y,
                MeasuredKitPanel().SkirtDepth(), 1.0e-3f);
}

// A crest piece on the pitch grid is whole on every grade, including the
// sheared one whose own forward carries the stretch that lands its span on the
// higher station: the row's basis stays orthonormal, so no piece of it is ever
// skewed or scaled. (A crest registered to its span takes that span's frame
// instead; CrestRegisteredToItsSpanClosesTheRowAsTheWallsDo pins it.)
TEST(FenceLayout, CrestPiecesStayRigidOnEveryGrade)
{
    for (const Components::SplineSpanGrade grade :
         {Components::SplineSpanGrade::Racked, Components::SplineSpanGrade::Stepped,
          Components::SplineSpanGrade::Sheared})
    {
        const auto center = RampZ(20.0f, 6.0f, 41u);
        const float32 boundaries[2] = {0.0f, 40.0f};
        const FencePieceBounds spans[1] = {Piece(5.0f)};
        const FencePieceBounds crests[1] = {Piece(2.0f)};
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;
        params.SpanGrade = grade;

        const FenceLayoutResult result = BuildFenceLayout(center, params);
        ASSERT_FALSE(result.Crests.empty());
        ASSERT_FALSE(result.Spans.empty());
        // One span piece on a straight ramp: every span shares one frame, so
        // the row can be held against the first of them.
        const FenceSpan& span = result.Spans.front();
        const V3 alongSpan = span.Pose.Forward * (1.0f / VectorLength(span.Pose.Forward));
        for (const FenceCrest& crest : result.Crests)
        {
            const TilePose& pose = crest.Pose;
            EXPECT_NEAR(VectorLength(pose.Forward), 1.0f, 1.0e-5f);
            EXPECT_NEAR(VectorLength(pose.Right), 1.0f, 1.0e-5f);
            EXPECT_NEAR(VectorLength(pose.Up), 1.0f, 1.0e-5f);
            EXPECT_NEAR(V3::Dot(pose.Right, pose.Up), 0.0f, 1.0e-5f);
            EXPECT_NEAR(V3::Dot(pose.Right, pose.Forward), 0.0f, 1.0e-5f);
            EXPECT_NEAR(V3::Dot(pose.Right, V3::Cross(pose.Up, pose.Forward)), 1.0f, 1.0e-5f);
            // The row runs along the span it stands on, whatever that span's
            // own frame is doing.
            EXPECT_NEAR(V3::Dot(pose.Forward, alongSpan), 1.0f, 1.0e-4f);
            // A sheared span's basis is deliberately skewed — its forward is
            // not perpendicular to its up — so a rigid piece laid along that
            // forward takes the closest frame to it and leans by the grade,
            // exactly as it does on a racked one. Where the span's own basis
            // is orthonormal, the row simply keeps it.
            if (grade != Components::SplineSpanGrade::Sheared)
                EXPECT_NEAR(V3::Dot(pose.Up, span.Pose.Up), 1.0f, 1.0e-4f);
        }
    }
}

// ---- Crest row: registered to the spans ------------------------------------

namespace
{

// The castle kit's 5 m battlement: the wall's length and depth, 1 m tall, laid
// along local X and corner-pivoted like the wall it stands on.
FencePieceBounds CastleBattlement()
{
    FencePieceBounds b;
    b.HalfExtents = V3(2.5f, 0.5f, 0.25f);
    b.Center = V3(-2.5f, 0.5f, 0.0f);
    return b;
}

// Where a wall's top stands above the base line its stations were planted on:
// the whole wall under the default PivotPlane planting.
constexpr float32 kCastleWallTop = 5.0f;

// The two ends of a piece laid along its pose's forward at `lengthScale`, on
// the line `lift` above the pose's base. Forward is taken as it is, unit or
// not: a sheared piece's forward carries its grade, and its ends are where the
// mesh's own ends land.
void PieceEnds(const TilePose& pose, float32 lift, float32 length, float32 lengthScale, V3& tail,
               V3& head)
{
    const V3 centre = pose.Base + pose.Up * lift;
    const V3 half = pose.Forward * (length * lengthScale * 0.5f);
    tail = centre - half;
    head = centre + half;
}

// How far a point stands from the nearest wall top of the row, each top taken
// as the segment between its two ends. This is the gap an author sees: a
// battlement end hanging off the wall it should stand on.
float32 DistanceToNearestWallTop(const FenceLayoutResult& result, const V3& point)
{
    float32 nearest = std::numeric_limits<float32>::max();
    for (const FenceSpan& span : result.Spans)
    {
        V3 tail;
        V3 head;
        PieceEnds(span.Pose, kCastleWallTop, CastleWall().Length(), span.LengthScale, tail, head);
        const V3 along = head - tail;
        const float32 t = std::clamp(V3::Dot(point - tail, along) / V3::Dot(along, along), 0.0f,
                                     1.0f);
        nearest = std::min(nearest, VectorLength(point - (tail + along * t)));
    }
    return nearest;
}

// Defined with the join-plane tests below.
V3 GroundBisector(const V3& a, const V3& b);
void ExpectEndOnJoinPlane(const TilePose& pose, float32 lengthScale,
                          const FencePieceBounds& piece, const SplineLayout::FenceEndPlane& start,
                          const SplineLayout::FenceEndPlane& end, bool atStart, const V3& station,
                          const V3& bisector, float32 tolerance, const char* what);

} // namespace

// A curved castle wall: whole 5 m walls closing each run at 0.97x and 1.02x,
// and 5 m battlements. Laid at their own pitch, the battlements' ends would
// stand up to 0.13 m off the walls here, and neighbours would meet at an angle
// over a wedge of bare wall top. Registered, each battlement is its wall's
// twin: the same frame, the same scale, and so the same two ends — the row
// closes wherever the walls do. The sheared ramp holds the same rule where a
// span's frame is skewed.
TEST(FenceLayout, CrestRegisteredToItsSpanClosesTheRowAsTheWallsDo)
{
    constexpr float32 kTouching = 1.0e-3f;
    const auto curve = CurvedCastleCenter();
    float32 curveBoundaries[4];
    CurvedCastleBoundaries(curve, curveBoundaries);
    const auto ramp = RampZ(20.0f, 4.0f, 41u);
    const float32 rampBoundaries[2] = {0.0f, 40.0f};

    struct Arm
    {
        const char* Name;
        const std::vector<CenterSample>* Center;
        std::span<const float32> Boundaries;
        Components::SplineSpanGrade Grade;
    };
    const Arm arms[] = {
        {"curved racked", &curve, curveBoundaries, Components::SplineSpanGrade::Racked},
        {"sheared ramp", &ramp, rampBoundaries, Components::SplineSpanGrade::Sheared},
    };

    const FencePieceBounds spans[1] = {CastleWall()};
    const FencePieceBounds crests[1] = {CastleBattlement()};
    for (const Arm& arm : arms)
    {
        FenceLayoutParams params;
        params.RunBoundaries = arm.Boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;
        params.SpanGrade = arm.Grade;
        const FenceLayoutResult result = BuildFenceLayout(*arm.Center, params);

        // The fixture has to reach the case: walls stretched off 1, or a frame
        // whose forward is skewed off unit length.
        bool reaches = false;
        for (const FenceSpan& span : result.Spans)
        {
            reaches = reaches || std::abs(span.LengthScale - 1.0f) > 0.01f ||
                      std::abs(VectorLength(span.Pose.Forward) - 1.0f) > 0.01f;
        }
        ASSERT_TRUE(reaches) << arm.Name;

        // No battlement end stands off the wall under it.
        for (const FenceCrest& crest : result.Crests)
        {
            V3 tail;
            V3 head;
            PieceEnds(crest.Pose, 0.0f, CastleBattlement().Length(), crest.LengthScale, tail, head);
            EXPECT_LE(DistanceToNearestWallTop(result, tail), kTouching)
                << arm.Name << ", crest " << crest.Index;
            EXPECT_LE(DistanceToNearestWallTop(result, head), kTouching)
                << arm.Name << ", crest " << crest.Index;
        }

        // One battlement per wall, numbered as the wall is.
        EXPECT_EQ(result.Crests.size(), result.Spans.size()) << arm.Name;
        if (result.Crests.size() != result.Spans.size())
            continue;
        size_t mitredJoints = 0;
        for (size_t k = 0; k < result.Crests.size(); ++k)
        {
            const FenceCrest& crest = result.Crests[k];
            const FenceSpan& span = result.Spans[k];
            EXPECT_FALSE(crest.IsCap) << arm.Name << " " << k;
            EXPECT_EQ(crest.Index, span.Index) << arm.Name << " " << k;
            EXPECT_EQ(crest.Run, span.Run) << arm.Name << " " << k;
            EXPECT_EQ(crest.OrdinalInRun, span.OrdinalInRun) << arm.Name << " " << k;
            EXPECT_EQ(crest.LengthScale, span.LengthScale) << arm.Name << " " << k;

            V3 crestTail;
            V3 crestHead;
            PieceEnds(crest.Pose, 0.0f, CastleBattlement().Length(), crest.LengthScale, crestTail,
                      crestHead);
            V3 wallTail;
            V3 wallHead;
            PieceEnds(span.Pose, kCastleWallTop, CastleWall().Length(), span.LengthScale, wallTail,
                      wallHead);
            EXPECT_LE(VectorLength(crestTail - wallTail), kTouching) << arm.Name << " " << k;
            EXPECT_LE(VectorLength(crestHead - wallHead), kTouching) << arm.Name << " " << k;

            // The joint closes: no bare wall top between neighbours and no
            // overlap past the touching millimetre, across authored points too.
            // A square join closes end to end. A mitred one (section 4c) closes
            // where both crests' cut faces lie on the one join plane through
            // the shared station.
            if (k + 1u < result.Crests.size())
            {
                const FenceCrest& next = result.Crests[k + 1u];
                V3 nextTail;
                V3 nextHead;
                PieceEnds(next.Pose, 0.0f, CastleBattlement().Length(), next.LengthScale, nextTail,
                          nextHead);
                if (!crest.End.Mitred)
                {
                    EXPECT_LE(VectorLength(nextTail - crestHead), kTouching) << arm.Name << " " << k;
                    continue;
                }
                ASSERT_TRUE(next.Start.Mitred) << arm.Name << " " << k;
                ++mitredJoints;
                const V3 station = result.Stations[k + 1u].Pose.Base;
                const V3 bisector =
                    GroundBisector(span.Pose.Forward, result.Spans[k + 1u].Pose.Forward);
                ExpectEndOnJoinPlane(crest.Pose, crest.LengthScale, CastleBattlement(),
                                     crest.Start, crest.End, false, station, bisector, kTouching,
                                     arm.Name);
                ExpectEndOnJoinPlane(next.Pose, next.LengthScale, CastleBattlement(), next.Start,
                                     next.End, true, station, bisector, kTouching, arm.Name);
            }
        }
        // The curve's bare authored points are mitred joints; the ramp has none.
        if (arm.Grade == Components::SplineSpanGrade::Racked)
            EXPECT_GT(mitredJoints, 0u) << arm.Name;
    }
}

namespace
{

// Two straight legs of `leg` metres meeting at an authored point, the second
// turned `degrees` toward +X from the first, sampled every `step` metres.
std::vector<CenterSample> TurnedCorner(float32 leg, float32 degrees, float32 step)
{
    const float32 turn = degrees * Mathematics::Pi / 180.0f;
    const V3 corner(0.0f, 0.0f, leg);
    const V3 second(std::sin(turn), 0.0f, std::cos(turn));
    const uint32 steps = static_cast<uint32>(std::lround(leg / step));
    std::vector<CenterSample> center;
    for (uint32 i = 0; i <= steps; ++i)
        center.push_back({V3(0.0f, 0.0f, step * static_cast<float32>(i)), V3(0, 1, 0), true});
    for (uint32 i = 1; i <= steps; ++i)
        center.push_back({corner + second * (step * static_cast<float32>(i)), V3(0, 1, 0), true});
    return center;
}

// A post of `width` metres square and `height` tall, centred on its station.
FencePieceBounds SquarePost(float32 width, float32 height)
{
    FencePieceBounds b;
    b.HalfExtents = V3(width * 0.5f, height * 0.5f, width * 0.5f);
    b.Center = V3(0.0f, height * 0.5f, 0.0f);
    return b;
}

} // namespace

// Posts stand at a registered row's joins, exactly where they stand at the
// walls': every station is the end of the two spans beside it and inside none.
// So a post covers a registered crest's join as it covers its wall's, and the
// row keeps one crest per wall with its ends on the wall's ends — a thin post,
// the castle kit's 2.444 m tower, and a 45 degree corner alike. Reserved
// against a post, every crest would be removed and caps would stand in its
// place.
TEST(FenceLayout, RegisteredCrestRowStandsBetweenItsPosts)
{
    constexpr float32 kTouching = 1.0e-3f;
    const auto curve = CurvedCastleCenter();
    float32 curveBoundaries[4];
    CurvedCastleBoundaries(curve, curveBoundaries);
    const auto corner = TurnedCorner(20.0f, 45.0f, 0.05f);
    const float32 cornerLast = static_cast<float32>(corner.size() - 1u);
    const float32 cornerBoundaries[3] = {0.0f, 400.0f, cornerLast};

    struct Arm
    {
        const char* Name;
        const std::vector<CenterSample>* Center;
        std::span<const float32> Boundaries;
        FencePieceBounds Post;
    };
    const Arm arms[] = {
        {"curve, 0.2 m post", &curve, curveBoundaries, SquarePost(0.2f, 6.0f)},
        {"curve, castle tower", &curve, curveBoundaries, SquarePost(2.444f, 8.0f)},
        {"45 degree corner, 0.2 m post", &corner, cornerBoundaries, SquarePost(0.2f, 6.0f)},
    };

    const FencePieceBounds spans[1] = {CastleWall()};
    const FencePieceBounds crests[1] = {CastleBattlement()};
    const FencePieceBounds caps[1] = {Piece(0.4f)};
    for (const Arm& arm : arms)
    {
        FenceLayoutParams params;
        params.RunBoundaries = arm.Boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;
        params.CapPieces = caps;
        params.PostPiece = arm.Post;
        params.HasPostMesh = true;
        const FenceLayoutResult result = BuildFenceLayout(*arm.Center, params);

        // A registered row leaves no remainder beside a post for a cap.
        EXPECT_TRUE(CapsOf(result).empty()) << arm.Name;
        EXPECT_EQ(result.Crests.size(), result.Spans.size()) << arm.Name;
        if (result.Crests.size() != result.Spans.size())
            continue;
        for (size_t k = 0; k < result.Crests.size(); ++k)
        {
            const FenceCrest& crest = result.Crests[k];
            const FenceSpan& span = result.Spans[k];
            EXPECT_EQ(crest.OrdinalInRun, span.OrdinalInRun) << arm.Name << " " << k;
            EXPECT_EQ(crest.LengthScale, span.LengthScale) << arm.Name << " " << k;
            V3 crestTail;
            V3 crestHead;
            PieceEnds(crest.Pose, 0.0f, CastleBattlement().Length(), crest.LengthScale, crestTail,
                      crestHead);
            V3 wallTail;
            V3 wallHead;
            PieceEnds(span.Pose, kCastleWallTop, CastleWall().Length(), span.LengthScale, wallTail,
                      wallHead);
            EXPECT_LE(VectorLength(crestTail - wallTail), kTouching) << arm.Name << " " << k;
            EXPECT_LE(VectorLength(crestHead - wallHead), kTouching) << arm.Name << " " << k;
        }
    }
}

// Registration is all or nothing per run, and only where the lengths agree
// with no air asked for. A run that fails either keeps the pitch grid for every
// cell: whole pieces at scale 1, centred at kP + P/2. The wall's scale is not a
// test: a crest follows its wall however far the fill had to stretch it.
TEST(FenceLayout, CrestRowKeepsThePitchGridUnlessEverySpanRegisters)
{
    const FencePieceBounds crests[1] = {Piece(5.0f)};

    // A wall past its stretch cap still registers: 7 m holds one 5 m wall at
    // 1.4x, over the 1.25x cap, and the crest stands on it at 1.4x rather than
    // whole at 2.5 m with 2 m of bare wall beside it.
    {
        const auto center = StraightLineZ(7.0f, 15u);
        const float32 boundaries[2] = {0.0f, 14.0f};
        const FencePieceBounds spans[1] = {Piece(5.0f)};
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;
        const FenceLayoutResult result = BuildFenceLayout(center, params);
        ASSERT_EQ(result.Spans.size(), 1u);
        ASSERT_GT(result.Spans.front().LengthScale, params.SpanMaxStretch);
        ASSERT_EQ(result.Crests.size(), 1u);
        EXPECT_EQ(result.Crests.front().LengthScale, result.Spans.front().LengthScale);
        EXPECT_NEAR(result.Crests.front().Pose.Position.z, 3.5f, 1.0e-4f);
    }

    // A pool of 5 m and 3 m walls: the 3 m walls do not share the crest's
    // length, so no wall of the run registers, the 5 m ones included.
    {
        const auto center = StraightLineZ(40.0f, 81u);
        const float32 boundaries[2] = {0.0f, 80.0f};
        const FencePieceBounds spans[2] = {Piece(5.0f), Piece(3.0f)};
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;
        const FenceLayoutResult result = BuildFenceLayout(center, params);
        bool drawsBoth[2] = {false, false};
        for (const FenceSpan& span : result.Spans)
            drawsBoth[span.PoolSlot] = true;
        ASSERT_TRUE(drawsBoth[0] && drawsBoth[1]) << "the run must mix both walls";
        ASSERT_FALSE(result.Crests.empty());
        for (const FenceCrest& crest : result.Crests)
        {
            EXPECT_EQ(crest.LengthScale, 1.0f) << crest.Index;
            EXPECT_NEAR(crest.Pose.Position.z,
                        2.5f + 5.0f * static_cast<float32>(crest.OrdinalInRun), 1.0e-3f)
                << crest.Index;
        }
    }

    // Air between the pieces: an author who typed a 6 m pitch over 5 m walls
    // asked for the pitch.
    {
        const auto center = StraightLineZ(20.0f, 41u);
        const float32 boundaries[2] = {0.0f, 40.0f};
        const FencePieceBounds spans[1] = {Piece(5.0f)};
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = spans;
        params.CrestPieces = crests;
        params.CrestPitch = 6.0f;
        const FenceLayoutResult result = BuildFenceLayout(center, params);
        ASSERT_EQ(result.Crests.size(), 3u);
        for (const FenceCrest& crest : result.Crests)
        {
            EXPECT_EQ(crest.LengthScale, 1.0f) << crest.Index;
            EXPECT_NEAR(crest.Pose.Position.z,
                        3.0f + 6.0f * static_cast<float32>(crest.OrdinalInRun), 1.0e-3f)
                << crest.Index;
        }
    }
}

// A registered row answers to reservations and caps exactly as the pitch grid
// does: a wall a reservation reaches takes no crest and keeps its ordinal, and
// each remainder beside the reservation takes the longest cap that fits, flush
// against the kept crests, facing the edge it closes.
TEST(FenceLayout, ReservedWallTakesNoRegisteredCrestAndCapsCloseBesideIt)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(5.0f)};
    const FencePieceBounds caps[1] = {Piece(1.0f)};
    const FenceReservation reserved[1] = {{0u, 6.0f, 7.0f}};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.CapPieces = caps;
    params.Reservations = reserved;

    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_EQ(CellOrdinals(result), (std::vector<uint32>{0u, 2u, 3u}));
    const std::vector<FenceCrest> capped = CapsOf(result);
    ASSERT_EQ(capped.size(), 2u);
    // The 1 m left between the first wall and the reservation, and the 3 m
    // between the reservation and the third wall: both in wall 1's cell.
    EXPECT_NEAR(capped[0].Pose.Position.z, 5.5f, 1.0e-4f);
    EXPECT_GT(capped[0].Pose.Forward.z, 0.0f);
    EXPECT_NEAR(capped[1].Pose.Position.z, 9.5f, 1.0e-4f);
    EXPECT_LT(capped[1].Pose.Forward.z, 0.0f);
    for (const FenceCrest& cap : capped)
    {
        EXPECT_EQ(cap.OrdinalInRun, 1u);
        EXPECT_EQ(cap.LengthScale, 1.0f);
    }

    // On compressed walls the span cells and the pitch grid part ways: 18 m of
    // run holds four 5 m walls at 0.9x, cells of 4.5 m. The cap closing the
    // 0.5 m left below a reservation at 9.5-10.5 m stands at 9.2 m, which is
    // in wall 2 (9-13.5 m) and would be in cell 1 on a 5 m grid.
    const auto compressed = StraightLineZ(18.0f, 37u);
    const float32 compressedBoundaries[2] = {0.0f, 36.0f};
    const FencePieceBounds shortCaps[1] = {Piece(0.4f)};
    const FenceReservation middle[1] = {{0u, 9.5f, 10.5f}};
    params.RunBoundaries = compressedBoundaries;
    params.CapPieces = shortCaps;
    params.Reservations = middle;
    const FenceLayoutResult squeezed = BuildFenceLayout(compressed, params);
    ASSERT_EQ(squeezed.Spans.size(), 4u);
    ASSERT_NEAR(squeezed.Spans.front().LengthScale, 0.9f, 1.0e-4f);
    EXPECT_EQ(CellOrdinals(squeezed), (std::vector<uint32>{0u, 1u, 3u}));
    const std::vector<FenceCrest> squeezedCaps = CapsOf(squeezed);
    ASSERT_EQ(squeezedCaps.size(), 2u);
    EXPECT_NEAR(squeezedCaps[0].Pose.Position.z, 9.2f, 1.0e-4f);
    EXPECT_EQ(squeezedCaps[0].OrdinalInRun, 2u);
    EXPECT_NEAR(squeezedCaps[1].Pose.Position.z, 13.3f, 1.0e-4f);
    EXPECT_EQ(squeezedCaps[1].OrdinalInRun, 2u);
}

// A registered row stands on its walls and the posts reserve nothing against
// it, so only a caller's reservation can empty it. The report names that and
// nothing else: a shorter crest piece would put the run on the pitch grid,
// where the posts do reserve and a stretch measured without them does not hold.
TEST(FenceLayout, AnEmptyRegisteredCrestRowNamesOnlyTheReservation)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds crests[1] = {Piece(5.0f)};
    const FenceReservation everyWall[4] = {
        {0u, 2.0f, 3.0f}, {0u, 7.0f, 8.0f}, {0u, 12.0f, 13.0f}, {0u, 17.0f, 18.0f}};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    params.PostPiece = SquarePost(0.2f, 6.0f);

    // The control: nothing reserved, one crest per wall and no report.
    params.HasPostMesh = true;
    const FenceLayoutResult clear = BuildFenceLayout(center, params);
    EXPECT_EQ(CellOrdinals(clear), (std::vector<uint32>{0u, 1u, 2u, 3u}));
    EXPECT_EQ(EmptyCrestRowReports(clear), 0u);

    params.Reservations = everyWall;
    for (const bool posts : {false, true})
    {
        params.HasPostMesh = posts;
        const FenceLayoutResult result = BuildFenceLayout(center, params);
        ASSERT_TRUE(CellOrdinals(result).empty()) << posts;
        EXPECT_EQ(EmptyCrestRowReports(result), 1u) << posts;
        EXPECT_TRUE(AnyValidationContains(
            result, "a reserved stretch reaches every wall they stand on; move what reserves "
                    "the run."))
            << posts;
        EXPECT_FALSE(AnyValidationContains(result, "always fits")) << posts;
        EXPECT_FALSE(AnyValidationContains(result, "clear the Post pool")) << posts;
    }
}

// A node several splines meet at faces one way in every one of their layouts:
// the caller that owns the network says which, and the recipe's own bisector
// yields to it.
TEST(FenceLayout, SharedFacingAtAnAuthoredPointOverridesTheBisector)
{
    const auto center = CorneredL(6.0f, 6.0f, 0.5f);
    const float32 boundaries[3] = {0.0f, 12.0f, 24.0f};
    const FencePieceBounds spans[1] = {Piece(2.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;

    const FenceLayoutResult bisected = BuildFenceLayout(center, params);
    const FenceStation& corner = bisected.Stations[3];
    ASSERT_TRUE(corner.IsAuthoredPoint);
    // The corner turns from +Z to +X, so its own answer is the 45 degree
    // bisector of the two.
    EXPECT_NEAR(corner.Pose.Forward.x, 0.70710678f, 1.0e-4f);
    EXPECT_NEAR(corner.Pose.Forward.z, 0.70710678f, 1.0e-4f);

    const V3 shared[3] = {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 1.0f), V3(0.0f, 0.0f, 0.0f)};
    params.AuthoredPointForwards = shared;
    const FenceLayoutResult forced = BuildFenceLayout(center, params);
    EXPECT_NEAR(forced.Stations[3].Pose.Forward.x, 0.0f, 1.0e-4f);
    EXPECT_NEAR(forced.Stations[3].Pose.Forward.z, 1.0f, 1.0e-4f);
    // A zero entry keeps the recipe's own answer, which for an end station is
    // its run's chord.
    EXPECT_NEAR(forced.Stations.front().Pose.Forward.z, 1.0f, 1.0e-4f);
}

// ---- Span joins: the mitre at bare joins (design section 4c) ---------------

namespace
{

using SplineGeometry::CutPlane;
using SplineLayout::FenceEndPlane;
using SplineLayout::kMitreLimitDegrees;
using SplineLayout::MitreEndCutPlane;
using SplineLayout::MitreShape;
using SplineLayout::MitreVariantPlan;
using SplineLayout::PlanMitreVariants;
using SplineLayout::QuantizeMitreShape;

// A mesh-local point through the transform the controller emits.
V3 LocalToWorld(const TilePose& pose, PieceAxis axis, float32 lengthScale, const V3& local)
{
    Components::Transform t{};
    WriteParentLocalPose(t, IdentityPlacer(), pose, axis, lengthScale);
    return Column(t, 0) * local.x + Column(t, 1) * local.y + Column(t, 2) * local.z +
           V3(t.matrix[12], t.matrix[13], t.matrix[14]);
}

V3 GroundBisector(const V3& a, const V3& b)
{
    const V3 groundA = V3(a.x, 0.0f, a.z).Normalize();
    const V3 groundB = V3(b.x, 0.0f, b.z).Normalize();
    return (groundA + groundB).Normalize();
}

// Walks one end's cut plane as the cut variant is built from its quantised
// key — its across extent at the piece's foot and a metre up — into the world,
// and asserts every point lies on the join plane: vertical through the
// station, perpendicular to `bisector`.
void ExpectEndOnJoinPlane(const TilePose& pose, float32 lengthScale,
                          const FencePieceBounds& piece, const FenceEndPlane& start,
                          const FenceEndPlane& end, bool atStart, const V3& station,
                          const V3& bisector, float32 tolerance, const char* what)
{
    const MitreShape shape = QuantizeMitreShape(lengthScale, start, end);
    const CutPlane plane = MitreEndCutPlane(piece, shape, atStart);
    const int along = piece.Axis() == PieceAxis::X ? 0 : 2;
    const int across = 2 - along;
    const float32 foot = piece.Center.y - piece.HalfExtents.y;
    for (const float32 side : {-1.0f, 0.0f, 1.0f})
    {
        for (const float32 height : {0.0f, std::min(piece.Height(), 1.0f)})
        {
            V3 local = piece.Center;
            (&local.x)[across] += side * piece.HalfThickness();
            local.y = foot + height;
            const float32* n = &plane.Normal.x;
            (&local.x)[along] = (plane.Offset - n[across] * (&local.x)[across] - n[1] * local.y) /
                                n[along];
            const V3 world = LocalToWorld(pose, piece.Axis(), lengthScale, local);
            EXPECT_NEAR(V3::Dot(world - station, bisector), 0.0f, tolerance)
                << what << ": side " << side << ", height " << height;
        }
    }
}

// Every span of a result ends square.
bool NoSpanIsMitred(const FenceLayoutResult& result)
{
    return std::none_of(result.Spans.begin(), result.Spans.end(), [](const FenceSpan& span)
                        { return span.Start.Mitred || span.End.Mitred; });
}

size_t MitredJoinCount(const FenceLayoutResult& result)
{
    return static_cast<size_t>(std::count_if(result.Spans.begin(), result.Spans.end(),
                                             [](const FenceSpan& span) { return span.End.Mitred; }));
}

// The castle wall laid on a level arc of `radius` metres with no posts, one
// run of `spans` whole walls.
FenceLayoutResult BareCastleArc(float32 radius, float32 degrees,
                                std::span<const FencePieceBounds> crests = {})
{
    const auto center = LevelArc(radius, degrees, 0.05f);
    const float32 boundaries[2] = {0.0f, static_cast<float32>(center.size() - 1u)};
    const FencePieceBounds spans[1] = {CastleWall()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.CrestPieces = crests;
    return BuildFenceLayout(center, params);
}

// Two legs meeting at a bare authored point, climbing `rise` metres over each.
std::vector<CenterSample> ClimbingCorner(float32 leg, float32 degrees, float32 rise, float32 step)
{
    std::vector<CenterSample> center = TurnedCorner(leg, degrees, step);
    const float32 total = 2.0f * leg;
    float32 walked = 0.0f;
    for (size_t i = 1; i < center.size(); ++i)
    {
        walked += step;
        center[i].Pos.y = rise * 2.0f * walked / total;
    }
    return center;
}

// Two 5 m legs meeting at bare authored point 1, the second turned `degrees`
// to the right, one span of `piece` on each.
FenceLayoutResult BareTurnedCorner(float32 degrees, const FencePieceBounds& piece)
{
    const auto center = TurnedCorner(5.0f, degrees, 0.05f);
    const float32 last = static_cast<float32>(center.size() - 1u);
    const float32 boundaries[3] = {0.0f, last * 0.5f, last};
    const FencePieceBounds spans[1] = {piece};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    return BuildFenceLayout(center, params);
}

} // namespace

// A post covers its join, so the mitre inside it would be a mesh for nothing:
// the same corner mitres with the post pool empty and stays square with it
// filled.
TEST(FenceLayout, CoveredJoinCarriesNoMitre)
{
    const auto center = CorneredL(6.0f, 6.0f, 0.5f);
    const float32 boundaries[3] = {0.0f, 12.0f, 24.0f};
    const FencePieceBounds spans[1] = {Piece(2.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    const FenceLayoutResult bare = BuildFenceLayout(center, params);
    EXPECT_EQ(MitredJoinCount(bare), 1u) << "the control: the bare corner mitres";

    params.PostPiece = MeasuredKitPost();
    params.HasPostMesh = true;
    const FenceLayoutResult posted = BuildFenceLayout(center, params);
    EXPECT_TRUE(NoSpanIsMitred(posted));
}

// On a curve no post covers, each interior join is cut on one vertical plane
// through the station, perpendicular to the bisector of the two chords; each
// span is laid longer by (t/2) tan(|turn|/2) at every mitred end and its
// LengthScale counts both; and the outer faces of the two spans meet on the
// plane, so the notch is closed.
TEST(FenceLayout, UncoveredArcJoinMeetsOnTheBisector)
{
    const FenceLayoutResult result = BareCastleArc(30.0f, 60.0f);
    const FencePieceBounds wall = CastleWall();
    const float32 half = wall.HalfThickness();
    ASSERT_GE(result.Spans.size(), 4u);
    ASSERT_EQ(result.Stations.size(), result.Spans.size() + 1u);
    EXPECT_FALSE(result.Spans.front().Start.Mitred);
    EXPECT_FALSE(result.Spans.back().End.Mitred);

    for (size_t k = 0; k + 1u < result.Spans.size(); ++k)
    {
        const FenceSpan& before = result.Spans[k];
        const FenceSpan& after = result.Spans[k + 1u];
        ASSERT_TRUE(before.End.Mitred) << k;
        ASSERT_TRUE(after.Start.Mitred) << k;
        const V3 station = result.Stations[k + 1u].Pose.Base;
        const V3 bisector = GroundBisector(before.Pose.Forward, after.Pose.Forward);
        ExpectEndOnJoinPlane(before.Pose, before.LengthScale, wall, before.Start, before.End, false,
                             station, bisector, 5e-4f, "closing end");
        ExpectEndOnJoinPlane(after.Pose, after.LengthScale, wall, after.Start, after.End, true,
                             station, bisector, 5e-4f, "opening end");
    }

    for (size_t k = 0; k < result.Spans.size(); ++k)
    {
        const FenceSpan& span = result.Spans[k];
        const float32 chord =
            VectorLength(result.Stations[k + 1u].Pose.Base - result.Stations[k].Pose.Base);
        float32 overhangs = 0.0f;
        for (const FenceEndPlane* end : {&span.Start, &span.End})
        {
            if (end->Mitred)
                overhangs += half * std::tan(0.5f * std::abs(end->TurnRadians));
        }
        EXPECT_NEAR(span.LengthScale * wall.Length(), chord + overhangs, 1e-4f) << "span " << k;
    }

    // The arc turns right, so each join's outside is its left: the outer
    // bottom corners of the two ends coincide on the plane.
    for (size_t k = 0; k + 1u < result.Spans.size(); ++k)
    {
        const FenceSpan& before = result.Spans[k];
        const FenceSpan& after = result.Spans[k + 1u];
        ASSERT_GT(before.End.TurnRadians, 0.0f);
        // X-laid wall: local +Z is the left of travel.
        const V3 closingCorner(wall.Center.x + wall.HalfExtents.x, 0.0f, half);
        const V3 openingCorner(wall.Center.x - wall.HalfExtents.x, 0.0f, half);
        const V3 a = LocalToWorld(before.Pose, wall.Axis(), before.LengthScale, closingCorner);
        const V3 b = LocalToWorld(after.Pose, wall.Axis(), after.LengthScale, openingCorner);
        EXPECT_LT(VectorLength(a - b), 5e-4f) << "join " << k << " still shows a notch";
    }
}

// Which way a join turns decides which face is outside: the planes' turns take
// the sign of the fence's own turn, positive to the right, on both ends.
TEST(FenceLayout, MitreSignFollowsTheTurn)
{
    const FenceLayoutResult right = BareCastleArc(30.0f, 60.0f);
    auto left = LevelArc(30.0f, 60.0f, 0.05f);
    for (CenterSample& sample : left)
        sample.Pos.x = -sample.Pos.x;
    const float32 boundaries[2] = {0.0f, static_cast<float32>(left.size() - 1u)};
    const FencePieceBounds spans[1] = {CastleWall()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    const FenceLayoutResult mirrored = BuildFenceLayout(left, params);
    ASSERT_EQ(right.Spans.size(), mirrored.Spans.size());
    for (size_t k = 0; k + 1u < right.Spans.size(); ++k)
    {
        EXPECT_GT(right.Spans[k].End.TurnRadians, 0.0f);
        EXPECT_GT(right.Spans[k + 1u].Start.TurnRadians, 0.0f);
        EXPECT_LT(mirrored.Spans[k].End.TurnRadians, 0.0f);
        EXPECT_LT(mirrored.Spans[k + 1u].Start.TurnRadians, 0.0f);
        EXPECT_NEAR(right.Spans[k].End.TurnRadians, -mirrored.Spans[k].End.TurnRadians, 1e-5f);
    }
}

// A bare right angle: both spans are cut on the 45 degree plane through the
// corner, each laid half its thickness longer (tan 45 = 1).
TEST(FenceLayout, ABareRightAngleIsCutOnItsBisector)
{
    const auto center = CorneredL(10.0f, 10.0f, 0.05f);
    const float32 boundaries[3] = {0.0f, 200.0f, 400.0f};
    const FencePieceBounds spans[1] = {CastleWall()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 4u);
    EXPECT_EQ(MitredJoinCount(result), 1u) << "the straight joins stay square";
    const FenceSpan& before = result.Spans[1];
    const FenceSpan& after = result.Spans[2];
    ASSERT_TRUE(before.End.Mitred);
    ASSERT_TRUE(after.Start.Mitred);
    EXPECT_NEAR(before.End.TurnRadians, Mathematics::Pi * 0.5f, 1e-4f);
    EXPECT_NEAR(before.LengthScale * 5.0f, 5.0f + 0.25f, 1e-4f);
    EXPECT_NEAR(after.LengthScale * 5.0f, 5.0f + 0.25f, 1e-4f);
    const V3 bisector = V3(1.0f, 0.0f, 1.0f).Normalize();
    ExpectEndOnJoinPlane(before.Pose, before.LengthScale, CastleWall(), before.Start, before.End,
                         false, V3(0.0f, 0.0f, 10.0f), bisector, 5e-4f, "closing end");
    ExpectEndOnJoinPlane(after.Pose, after.LengthScale, CastleWall(), after.Start, after.End, true,
                         V3(0.0f, 0.0f, 10.0f), bisector, 5e-4f, "opening end");
}

// Past the mitre limit a join keeps square ends and says where to plant a
// post; at the limit's edge the mitre still applies.
TEST(FenceLayout, ATurnPastTheMitreLimitStaysSquareAndIsReported)
{
    const FenceLayoutResult sharp = BareTurnedCorner(150.0f, CastleWall());
    ASSERT_EQ(sharp.Spans.size(), 2u);
    EXPECT_TRUE(NoSpanIsMitred(sharp));
    EXPECT_NEAR(sharp.Spans[0].LengthScale, 1.0f, 1e-5f) << "a square end is not laid longer";
    size_t reports = 0;
    for (const std::string& message : sharp.Validation)
    {
        if (message.find("mitre limit") == std::string::npos)
            continue;
        ++reports;
        EXPECT_NE(message.find("point 1 turns 150 degrees"), std::string::npos) << message;
        EXPECT_NE(message.find("plant a post"), std::string::npos) << message;
    }
    EXPECT_EQ(reports, 1u);

    const FenceLayoutResult within = BareTurnedCorner(kMitreLimitDegrees - 1.0f, CastleWall());
    EXPECT_EQ(MitredJoinCount(within), 1u);
    EXPECT_FALSE(AnyValidationContains(within, "mitre limit"));
    const FenceLayoutResult past = BareTurnedCorner(kMitreLimitDegrees + 1.0f, CastleWall());
    EXPECT_TRUE(NoSpanIsMitred(past));
    EXPECT_TRUE(AnyValidationContains(past, "mitre limit"));
}

// The notch floor: a turn whose square ends would leave under a millimetre
// between the outer faces is chord noise and stays square; one just over it
// is mitred.
TEST(FenceLayout, ANotchUnderAMillimetreStaysSquare)
{
    // Piece(): 0.1 m thick, so the notch is 0.1 * tan(turn / 2), and 1 mm of
    // it is a turn of 2 * atan(0.01) = 1.146 degrees.
    EXPECT_TRUE(NoSpanIsMitred(BareTurnedCorner(1.0f, Piece(5.0f))));
    EXPECT_EQ(MitredJoinCount(BareTurnedCorner(1.3f, Piece(5.0f))), 1u);
}

// A closed square with no posts mitres all four corners, the loop's shared
// station included, each on the same 90 degree turn.
TEST(FenceLayout, TheClosedSquareMitresItsFourCorners)
{
    const FenceLayoutResult result = FixtureClosedSquare(kArmNothing);
    ASSERT_EQ(result.Spans.size(), 8u);
    EXPECT_EQ(MitredJoinCount(result), 4u);
    for (const FenceSpan& span : result.Spans)
    {
        EXPECT_EQ(span.Start.Mitred, span.OrdinalInRun == 0u);
        EXPECT_EQ(span.End.Mitred, span.OrdinalInRun == 1u);
        if (span.End.Mitred)
            EXPECT_NEAR(std::abs(span.End.TurnRadians), Mathematics::Pi * 0.5f, 1e-4f);
    }
}

// A closed square of 4 m walls, two to a side, stopped by the piece budget
// after its first two sides (two stations and two spans each): the second
// side's closing wall and the first side's opening wall were mitred against
// walls that are never emitted, so both end square at their stations, laid at
// the plain fill's 1.0 like the square joins between them, while the corner
// the two emitted sides share stays mitred.
TEST(FenceLayout, AWallThePieceBudgetLeavesWithoutItsNeighbourEndsSquare)
{
    const auto center = ClosedSquare(8.0f, 0.5f);
    const float32 boundaries[5] = {0.0f, 16.0f, 32.0f, 48.0f, 64.0f};
    const FencePieceBounds spans[1] = {Piece(4.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.Closed = true;
    params.MaxPieces = 8u;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 4u);
    EXPECT_FALSE(result.Spans.front().Start.Mitred);
    EXPECT_FALSE(result.Spans.back().End.Mitred);
    EXPECT_NEAR(result.Spans.front().LengthScale, 1.0f, 1e-4f);
    EXPECT_NEAR(result.Spans.back().LengthScale, 1.0f, 1e-4f);
    EXPECT_TRUE(result.Spans[1].End.Mitred);
    EXPECT_TRUE(result.Spans[2].Start.Mitred);
}

// A crest registered to its span takes the span's two planes in its own frame,
// so the row closes at every join exactly as the walls do; a crest on the
// pitch grid never reaches a join and stays whole.
TEST(FenceLayout, ARegisteredCrestCarriesItsSpansPlanes)
{
    const FencePieceBounds battlements[1] = {CastleBattlement()};
    const FenceLayoutResult result =
        BareCastleArc(30.0f, 60.0f, battlements);
    ASSERT_EQ(result.Crests.size(), result.Spans.size());
    for (size_t k = 0; k < result.Crests.size(); ++k)
    {
        const FenceCrest& crest = result.Crests[k];
        const FenceSpan& span = result.Spans[k];
        EXPECT_EQ(crest.Start.Mitred, span.Start.Mitred);
        EXPECT_EQ(crest.End.Mitred, span.End.Mitred);
        EXPECT_EQ(crest.LengthScale, span.LengthScale);
        if (!crest.End.Mitred)
            continue;
        EXPECT_NEAR(crest.End.StationLocalY, -kCastleWallTop, 1e-5f);
        const V3 station = result.Stations[k + 1u].Pose.Base;
        const V3 bisector =
            GroundBisector(span.Pose.Forward, result.Spans[k + 1u].Pose.Forward);
        ExpectEndOnJoinPlane(crest.Pose, crest.LengthScale, CastleBattlement(), crest.Start,
                             crest.End, false, station, bisector, 5e-4f, "crest closing end");
        ExpectEndOnJoinPlane(result.Crests[k + 1u].Pose, result.Crests[k + 1u].LengthScale,
                             CastleBattlement(), result.Crests[k + 1u].Start,
                             result.Crests[k + 1u].End, true, station, bisector, 5e-4f,
                             "crest opening end");
    }

    // A 1 m battlement is shorter than the wall, so the row keeps its pitch
    // grid: whole pieces, never cut.
    const FencePieceBounds shortBattlements[1] = {Piece(1.0f)};
    const FenceLayoutResult grid = BareCastleArc(30.0f, 60.0f, shortBattlements);
    ASSERT_FALSE(grid.Crests.empty());
    for (const FenceCrest& crest : grid.Crests)
    {
        EXPECT_FALSE(crest.Start.Mitred || crest.End.Mitred);
        EXPECT_EQ(crest.LengthScale, 1.0f);
    }
}

// The fixtures that carry a post at every join, or run straight, keep square
// ends everywhere — which is what keeps their stations, spans and crest rows
// exactly as they were (the emitted span of a square-ended join is its chord
// pose, untouched). The bare closed square and the bare curved castle are the
// two that mitre. A bare straight run through an authored point needs no mitre
// either: its joins turn by nothing, and the notch the polyline's rounding
// leaves is far under the millimetre floor, so its walls stay at the fill's 1.0.
TEST(FenceLayout, FixturesWithPostsOrStraightRunsKeepSquareEnds)
{
    for (const IdentityFixture& fixture : kIdentityFixtures)
    {
        const std::string name = fixture.Name;
        const bool bareTurns = name == "closed square" ||
                               name == "curved castle wall, crest shorter than the wall";
        for (const uint32 arms : {kArmNothing, kArmCrestRow})
        {
            const FenceLayoutResult result = fixture.Build(arms);
            EXPECT_EQ(NoSpanIsMitred(result), !bareTurns) << fixture.Name;
            for (const FenceCrest& crest : result.Crests)
                EXPECT_FALSE(crest.Start.Mitred || crest.End.Mitred) << fixture.Name;
        }
    }

    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[3] = {0.0f, 20.0f, 40.0f};
    const FencePieceBounds spans[1] = {CastleWall()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    const FenceLayoutResult straight = BuildFenceLayout(center, params);
    ASSERT_EQ(straight.Spans.size(), 4u);
    EXPECT_TRUE(NoSpanIsMitred(straight));
    for (const FenceSpan& span : straight.Spans)
        EXPECT_NEAR(span.LengthScale, 1.0f, 1e-5f);
}


// ---- Span overrides: gates and pinned pieces --------------------------------

namespace
{

using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

// The castle kit's gate stands 1.47 times the lattice pitch (F0): a 7.35 m
// piece among 5 m walls, planted and pivoted as the walls are.
FencePieceBounds CastleGate()
{
    FencePieceBounds b;
    b.HalfExtents = V3(3.675f, 3.0f, 0.4f);
    b.Center = V3(-3.675f, 3.0f, 0.0f);
    return b;
}

// A straight run of `length` metres of 5 m castle walls down +Z, with the given
// overrides and gate pool, and an optional crest row.
FenceLayoutResult StraightCastleRun(float32 length, std::span<const SplineSpanOverride> overrides,
                                    std::span<const FencePieceBounds> gates,
                                    std::span<const FencePieceBounds> crests = {},
                                    float32 crestPitch = 0.0f,
                                    std::span<const FenceReservation> reserved = {})
{
    const uint32 samples = static_cast<uint32>(std::lround(length / 0.5f)) + 1u;
    const auto center = StraightLineZ(length, samples);
    const float32 boundaries[2] = {0.0f, static_cast<float32>(samples - 1u)};
    const FencePieceBounds spans[1] = {CastleWall()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.GatePieces = gates;
    params.SpanOverrides = overrides;
    params.CrestPieces = crests;
    params.CrestPitch = crestPitch;
    params.Reservations = reserved;
    return BuildFenceLayout(center, params);
}

const FenceSpan* SpanAtOrdinal(const FenceLayoutResult& result, uint32 run, uint32 ordinal)
{
    for (const FenceSpan& span : result.Spans)
    {
        if (span.Run == run && span.OrdinalInRun == ordinal)
            return &span;
    }
    return nullptr;
}

// A crest's footprint along Z on a +Z straight run: its base centred, its
// length scaled.
void CrestFootprintZ(const FenceCrest& crest, const FencePieceBounds& piece, float32& outLo,
                     float32& outHi)
{
    const float32 half = piece.Length() * crest.LengthScale * 0.5f;
    outLo = crest.Pose.Base.z - half;
    outHi = crest.Pose.Base.z + half;
}

} // namespace

// With no override set, a recipe that carries the whole override table and a
// gate pool lays out exactly as one that carries neither: every station, span,
// crest and message of every identity fixture, bit for bit.
TEST(FenceLayout, UntouchedOverrideTableLeavesEveryExistingFixtureByteIdentical)
{
    for (const IdentityFixture& fixture : kIdentityFixtures)
    {
        for (const uint32 arms : {kArmNothing, kArmCrestRow})
        {
            const FenceLayoutResult plain = fixture.Build(arms);
            const FenceLayoutResult armed = fixture.Build(arms | kArmEmptyOverrideTable);
            EXPECT_EQ(FingerprintStationsAndSpans(plain), FingerprintStationsAndSpans(armed))
                << fixture.Name;
            ASSERT_EQ(plain.Crests.size(), armed.Crests.size()) << fixture.Name;
            for (size_t k = 0; k < plain.Crests.size(); ++k)
            {
                ExpectSameCrest(armed.Crests[k], plain.Crests[k], fixture.Name);
                EXPECT_EQ(armed.Crests[k].LengthScale, plain.Crests[k].LengthScale) << fixture.Name;
            }
            EXPECT_EQ(armed.Validation, plain.Validation) << fixture.Name;
            for (const FenceSpan& span : armed.Spans)
                EXPECT_FALSE(span.IsGate) << fixture.Name;
        }
    }
}

// A gate draws its gate piece at the gate's own length: selected, never
// stretched to the wall it replaced. The run's one shared stretch closes the
// run around it, so the gate and the walls take the same scale and the last
// station still lands on the authored point.
TEST(FenceLayout, GateDrawsItsPieceAtItsOwnLengthAndTheRunClosesAroundIt)
{
    const FencePieceBounds gates[1] = {CastleGate()};
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const FenceLayoutResult result = StraightCastleRun(20.0f, overrides, gates);

    // 5 + 7.35 + 5 + 5 = 22.35 m of pieces close 20 m at 0.895.
    ASSERT_EQ(result.Spans.size(), 4u);
    const float32 stretch = 20.0f / 22.35f;
    for (const FenceSpan& span : result.Spans)
    {
        EXPECT_EQ(span.IsGate, span.OrdinalInRun == 1u) << span.OrdinalInRun;
        EXPECT_NEAR(span.LengthScale, stretch, 1e-4f) << span.OrdinalInRun;
    }
    EXPECT_EQ(result.Spans[1].PoolSlot, 0u);
    const float32 gateStart = result.Stations[1].Pose.Base.z;
    const float32 gateEnd = result.Stations[2].Pose.Base.z;
    EXPECT_NEAR(gateEnd - gateStart, 7.35f * stretch, 1e-3f);
    EXPECT_NEAR(result.Stations.back().Pose.Base.z, 20.0f, 1e-4f);
    EXPECT_TRUE(result.Validation.empty());
}

// A gate over an empty gate pool is an opening: the room of the wall it
// replaced, with nothing in it, and the ordinals after it do not move.
TEST(FenceLayout, GateOverAnEmptyGatePoolLeavesAnOpeningThatKeepsItsOrdinal)
{
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const FenceLayoutResult open = StraightCastleRun(20.0f, overrides, {});
    const FenceLayoutResult walled = StraightCastleRun(20.0f, {}, {});

    ASSERT_EQ(walled.Spans.size(), 4u);
    ASSERT_EQ(open.Spans.size(), 3u);
    EXPECT_EQ(SpanAtOrdinal(open, 0u, 1u), nullptr);
    for (const FenceSpan& span : open.Spans)
    {
        EXPECT_FALSE(span.IsGate);
        EXPECT_EQ(span.Index, span.OrdinalInRun) << "an opening renumbered the spans after it";
        const FenceSpan* same = SpanAtOrdinal(walled, 0u, span.OrdinalInRun);
        ASSERT_NE(same, nullptr);
        ExpectSameVector(span.Pose.Position, same->Pose.Position, "wall beside an opening");
    }
    EXPECT_EQ(open.Stations.size(), walled.Stations.size());
    EXPECT_TRUE(open.Validation.empty()) << "an empty gate pool is an opening, not a mistake";
}

// A gate's footprint is its span, and it reserves the crest row the way a
// caller's reservation over the same stretch does: no cell stands over the
// gate, and the row keeps exactly the cells that reservation would keep.
TEST(FenceLayout, GateFootprintReservesTheCrestRow)
{
    const FencePieceBounds gates[1] = {CastleGate()};
    const FencePieceBounds crests[1] = {Piece(1.0f)};
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const FenceLayoutResult gated = StraightCastleRun(20.0f, overrides, gates, crests, 2.0f);
    const float32 gateStart = gated.Stations[1].Pose.Base.z;
    const float32 gateEnd = gated.Stations[2].Pose.Base.z;

    ASSERT_FALSE(gated.Crests.empty());
    for (const FenceCrest& crest : gated.Crests)
    {
        float32 lo = 0.0f;
        float32 hi = 0.0f;
        CrestFootprintZ(crest, Piece(1.0f), lo, hi);
        EXPECT_FALSE(hi > gateStart + 1.0e-3f && lo < gateEnd - 1.0e-3f)
            << "crest " << crest.Index << " at [" << lo << ", " << hi << "] stands over the gate at ["
            << gateStart << ", " << gateEnd << "]";
    }

    // The control names the cells the gate must remove, by the mechanism that
    // already removes them: a caller's reservation over the gate's stretch, on
    // the same run with the same gate.
    const FenceReservation reserved[1] = {{0u, gateStart, gateEnd}};
    const FenceLayoutResult control =
        StraightCastleRun(20.0f, overrides, gates, crests, 2.0f, reserved);
    EXPECT_EQ(CellOrdinals(gated), CellOrdinals(control));
    for (const FenceCrest& crest : gated.Crests)
    {
        const FenceCrest* same = FindCell(control, crest.Index);
        if (!crest.IsCap && same)
            ExpectSameCrest(crest, *same, "cell beside a gate");
    }
}

// On a row registered to its walls the gate's cell is its span, and only that
// cell goes: a gate longer than the walls does not knock the run back to the
// pitch grid, so every wall keeps a crest at its own scale.
TEST(FenceLayout, GateOnARegisteredRunRemovesOnlyItsOwnCell)
{
    const FencePieceBounds gates[1] = {CastleGate()};
    const FencePieceBounds battlements[1] = {CastleBattlement()};
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const FenceLayoutResult result = StraightCastleRun(20.0f, overrides, gates, battlements);

    ASSERT_EQ(result.Spans.size(), 4u);
    std::vector<uint32> ordinals;
    for (const FenceCrest& crest : result.Crests)
    {
        ASSERT_FALSE(crest.IsCap);
        ordinals.push_back(crest.OrdinalInRun);
        const FenceSpan* wall = SpanAtOrdinal(result, 0u, crest.OrdinalInRun);
        ASSERT_NE(wall, nullptr);
        EXPECT_EQ(crest.LengthScale, wall->LengthScale) << "the run left its registration";
    }
    EXPECT_EQ(ordinals, (std::vector<uint32>{0u, 2u, 3u}));
}

// A registered row whose only wall is a gate places nothing, and the message
// that says a reservation took every wall is the one the author gets.
TEST(FenceLayout, AGateOnEveryRegisteredWallNamesTheReservation)
{
    const FencePieceBounds gates[1] = {CastleGate()};
    const FencePieceBounds battlements[1] = {CastleBattlement()};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    const FenceLayoutResult gated = StraightCastleRun(5.0f, overrides, gates, battlements);
    ASSERT_EQ(gated.Spans.size(), 1u);
    ASSERT_TRUE(gated.Spans[0].IsGate);
    EXPECT_TRUE(gated.Crests.empty());
    EXPECT_TRUE(AnyValidationContains(gated, "a reserved stretch reaches every wall"));

    // The control: the same run without the gate lays its crest and says nothing.
    const FenceLayoutResult walled = StraightCastleRun(5.0f, {}, gates, battlements);
    EXPECT_EQ(walled.Crests.size(), 1u);
    EXPECT_EQ(EmptyCrestRowReports(walled), 0u);
}

// A pinned span draws the slot its override names, and every other span keeps
// the pick it had: the seeded picks answer to the same ordinals either way.
TEST(FenceLayout, PinnedSpanDrawsItsSlotAndLeavesEveryOtherPickUnchanged)
{
    const auto center = StraightLineZ(24.0f, 49u);
    const float32 boundaries[2] = {0.0f, 48.0f};
    // Three pieces of one length, so the pin cannot change the count.
    const FencePieceBounds spans[3] = {Piece(2.0f), Piece(2.0f, 1.5f), Piece(2.0f, 2.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.Seed = 11u;
    const FenceLayoutResult seeded = BuildFenceLayout(center, params);
    ASSERT_EQ(seeded.Spans.size(), 12u);
    const uint32 pinnedSlot = (seeded.Spans[5].PoolSlot + 1u) % 3u;

    const SplineSpanOverride overrides[1] = {
        {0u, 5u, SplineSpanOverrideKind::ExplicitPiece, static_cast<uint8>(pinnedSlot)}};
    params.SpanOverrides = overrides;
    const FenceLayoutResult pinned = BuildFenceLayout(center, params);
    ASSERT_EQ(pinned.Spans.size(), seeded.Spans.size());
    for (size_t k = 0; k < pinned.Spans.size(); ++k)
    {
        EXPECT_EQ(pinned.Spans[k].PoolSlot, k == 5u ? pinnedSlot : seeded.Spans[k].PoolSlot) << k;
        EXPECT_FALSE(pinned.Spans[k].IsGate);
    }
    EXPECT_TRUE(pinned.Validation.empty());
}

// An override the layout cannot apply says so, once, and never vanishes: one
// past its run's fill, one on a point that opens no run, a gate or pin naming a
// slot its pool does not hold, and a second override on one span.
TEST(FenceLayout, OverridesTheLayoutCannotApplyAreReportedNeverDropped)
{
    const FencePieceBounds gates[1] = {CastleGate()};
    const SplineSpanOverride overrides[5] = {
        {0u, 9u, SplineSpanOverrideKind::Gate, 0u},
        {4u, 0u, SplineSpanOverrideKind::Gate, 0u},
        {0u, 1u, SplineSpanOverrideKind::Gate, 3u},
        {0u, 2u, SplineSpanOverrideKind::ExplicitPiece, 5u},
        {0u, 2u, SplineSpanOverrideKind::Gate, 0u},
    };
    const FenceLayoutResult result = StraightCastleRun(20.0f, overrides, gates);

    EXPECT_TRUE(AnyValidationContains(
        result, "a span override names span 9 of run 0->1, which has 4 span(s) — it places "
                "nothing; move it or remove it"));
    EXPECT_TRUE(AnyValidationContains(
        result, "a span override names point 4, which opens no run — it places nothing"));
    EXPECT_TRUE(AnyValidationContains(
        result, "the gate on span 1 of run 0->1 names Gate 4, but the Gate pool holds 1 — "
                "the span is left open"));
    EXPECT_TRUE(AnyValidationContains(
        result, "the explicit piece on span 2 of run 0->1 names Span 6, but the Span pool holds 1 — "
                "it keeps the fence's own pick"));
    EXPECT_TRUE(AnyValidationContains(
        result, "two span overrides name span 2 of run 0->1 — the first is used"));
    EXPECT_EQ(result.Validation.size(), 5u);

    // What each did: the missing gate piece left an opening, and the first of
    // the two overrides on span 2 won and kept the seeded wall.
    EXPECT_EQ(SpanAtOrdinal(result, 0u, 1u), nullptr);
    const FenceSpan* span2 = SpanAtOrdinal(result, 0u, 2u);
    ASSERT_NE(span2, nullptr);
    EXPECT_FALSE(span2->IsGate);
}

// A gate longer than its whole run cannot be solved by selection, and a stretch
// is never hidden: the run is built at the compression it needs and the fill's
// own report names it.
TEST(FenceLayout, GateLongerThanItsRunIsBuiltAtTheRequiredStretchAndReported)
{
    FencePieceBounds longGate = CastleGate();
    longGate.HalfExtents.x = 4.0f;
    longGate.Center.x = -4.0f;
    const FencePieceBounds gates[1] = {longGate};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    const FenceLayoutResult result = StraightCastleRun(5.0f, overrides, gates);

    ASSERT_EQ(result.Spans.size(), 1u);
    EXPECT_TRUE(result.Spans[0].IsGate);
    EXPECT_NEAR(result.Spans[0].LengthScale, 5.0f / 8.0f, 1e-4f);
    EXPECT_TRUE(AnyValidationContains(result, "needs 0.625x, under the 0.800x floor"));
}

// A gate is a span at a join no post covers: its end is cut on the join's
// bisector plane like any wall's, and the wall it meets is cut to match. An
// opening has nothing to meet, so the wall beside it keeps a square end.
TEST(FenceLayout, AGateIsMitredAtABareBendAndAnOpeningLeavesItsNeighbourSquare)
{
    const auto center = TurnedCorner(5.0f, 60.0f, 0.05f);
    const float32 last = static_cast<float32>(center.size() - 1u);
    const float32 boundaries[3] = {0.0f, last * 0.5f, last};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds gates[1] = {Piece(5.0f, 2.0f)};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.GatePieces = gates;
    params.SpanOverrides = overrides;
    const FenceLayoutResult gated = BuildFenceLayout(center, params);
    ASSERT_EQ(gated.Spans.size(), 2u);
    ASSERT_TRUE(gated.Spans[0].IsGate);
    EXPECT_TRUE(gated.Spans[0].End.Mitred);
    EXPECT_TRUE(gated.Spans[1].Start.Mitred);

    params.GatePieces = {};
    const FenceLayoutResult open = BuildFenceLayout(center, params);
    ASSERT_EQ(open.Spans.size(), 1u);
    EXPECT_FALSE(open.Spans[0].Start.Mitred);
    EXPECT_FALSE(open.Spans[0].End.Mitred);
}

// Each run reports how many spans its fill holds, an opening included, by the
// authored point opening it: what a point edit re-addresses overrides by.
TEST(FenceLayout, RunSpanCountsCountEachRunsFillOpeningsIncluded)
{
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const FenceLayoutResult open = StraightCastleRun(20.0f, overrides, {});
    ASSERT_EQ(open.Spans.size(), 3u);
    EXPECT_EQ(open.RunSpanCounts, (std::vector<uint32>{4u}));

    const FenceLayoutResult twoRuns = FixtureStraightPanels(kArmNothing);
    ASSERT_EQ(twoRuns.RunSpanCounts.size(), 2u);
    uint32 spans0 = 0;
    uint32 spans1 = 0;
    for (const FenceSpan& span : twoRuns.Spans)
        (span.Run == 0u ? spans0 : spans1)++;
    EXPECT_EQ(twoRuns.RunSpanCounts[0], spans0);
    EXPECT_EQ(twoRuns.RunSpanCounts[1], spans1);
}

// An override on a run whose two authored points coincide names a span that does
// not exist, and says so rather than vanishing with the run.
TEST(FenceLayout, OverridesOnACoincidentRunAreReported)
{
    const auto center = StraightLineZ(10.0f, 21u);
    const float32 boundaries[3] = {0.0f, 0.0f, 20.0f};
    const FencePieceBounds spans[1] = {CastleWall()};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.SpanOverrides = overrides;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    EXPECT_TRUE(AnyValidationContains(result, "are coincident"));
    EXPECT_TRUE(AnyValidationContains(
        result, "1 span override(s) on run 0->1 place nothing — the run has no length; move point 1 or "
                "remove them"));
}

// Only walls decide whether a run registers to its spans, and only a wall's slot
// indexes the Span pool: a gate's slot names a Gate pool piece. Here the Span
// pool holds a 5 m wall and a 4.9 m one, every wall is pinned to the 5 m piece,
// and the gate draws the second Gate piece — so a registration that read the
// gate's slot from the Span pool would find the 4.9 m wall and drop the row to
// the pitch grid. (With a one-piece Span pool the same mistake reads past the
// pool's end.)
TEST(FenceLayout, RegistrationReadsNoGateSlotFromTheSpanPool)
{
    const auto center = StraightLineZ(20.0f, 41u);
    const float32 boundaries[2] = {0.0f, 40.0f};
    FencePieceBounds shortWall = CastleWall();
    shortWall.HalfExtents.x = 2.45f;
    shortWall.Center.x = -2.45f;
    const FencePieceBounds spans[2] = {CastleWall(), shortWall};
    FencePieceBounds shortGate = CastleGate();
    shortGate.HalfExtents.x = 2.0f;
    shortGate.Center.x = -2.0f;
    const FencePieceBounds gates[2] = {CastleGate(), shortGate};
    const FencePieceBounds battlements[1] = {CastleBattlement()};
    const SplineSpanOverride overrides[4] = {
        {0u, 0u, SplineSpanOverrideKind::ExplicitPiece, 0u},
        {0u, 1u, SplineSpanOverrideKind::Gate, 1u},
        {0u, 2u, SplineSpanOverrideKind::ExplicitPiece, 0u},
        {0u, 3u, SplineSpanOverrideKind::ExplicitPiece, 0u},
    };
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.GatePieces = gates;
    params.SpanOverrides = overrides;
    params.CrestPieces = battlements;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 4u);
    ASSERT_TRUE(result.Spans[1].IsGate);
    EXPECT_EQ(result.Spans[1].PoolSlot, 1u);
    for (const FenceSpan& span : result.Spans)
        EXPECT_EQ(span.PoolSlot, span.IsGate ? 1u : 0u);
    std::vector<uint32> ordinals;
    for (const FenceCrest& crest : result.Crests)
    {
        ordinals.push_back(crest.OrdinalInRun);
        const FenceSpan* wall = SpanAtOrdinal(result, 0u, crest.OrdinalInRun);
        ASSERT_NE(wall, nullptr);
        EXPECT_EQ(crest.LengthScale, wall->LengthScale) << "the run left its registration";
    }
    EXPECT_EQ(ordinals, (std::vector<uint32>{0u, 2u, 3u}));
}

// A registered crest wider than its wall reaches across a bare join on the wall's
// side, and its span is laid longer to carry it; a gate stands in a reserved cell
// with no crest on it, so its side of the join takes no crest reach and the gate
// is laid exactly as it is with no crest row at all.
TEST(FenceLayout, AGateSideOfABareJoinTakesNoCrestReach)
{
    const auto center = TurnedCorner(5.0f, 60.0f, 0.05f);
    const float32 last = static_cast<float32>(center.size() - 1u);
    const float32 boundaries[3] = {0.0f, last * 0.5f, last};
    const FencePieceBounds spans[1] = {Piece(5.0f)};
    const FencePieceBounds gates[1] = {Piece(5.0f, 2.0f)};
    FencePieceBounds wideCrest = Piece(5.0f, 0.5f);
    wideCrest.HalfExtents.x = 0.6f;
    const FencePieceBounds crests[1] = {wideCrest};
    const SplineSpanOverride overrides[1] = {{1u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.GatePieces = gates;
    params.SpanOverrides = overrides;
    const FenceLayoutResult bare = BuildFenceLayout(center, params);
    params.CrestPieces = crests;
    const FenceLayoutResult crested = BuildFenceLayout(center, params);
    ASSERT_EQ(bare.Spans.size(), 2u);
    ASSERT_EQ(crested.Spans.size(), 2u);
    ASSERT_TRUE(crested.Spans[1].IsGate);
    ASSERT_TRUE(crested.Spans[1].Start.Mitred);
    EXPECT_EQ(crested.Spans[1].LengthScale, bare.Spans[1].LengthScale)
        << "the gate took a crest's reach it has no crest for";
    // The control: the wall on the other side does carry its crest's reach.
    EXPECT_GT(crested.Spans[0].LengthScale, bare.Spans[0].LengthScale);
}
