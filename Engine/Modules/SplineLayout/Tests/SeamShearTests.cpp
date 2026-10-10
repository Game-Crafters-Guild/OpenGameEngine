#include "SplineLayout/SeamShear.h"

#include "Mathematics/VectorOps.h"
#include "SplineLayout/PieceBasis.h"
#include "SplineLayout/TileLayout.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <vector>

using namespace GameEngine;
using GameEngine::SplineLayout::ApplySeamShear;
using GameEngine::SplineLayout::BuildTilePoses;
using GameEngine::SplineLayout::CenterSample;
using GameEngine::SplineLayout::ChoosePieceAxis;
using GameEngine::SplineLayout::ComputeSeamShearFactors;
using GameEngine::SplineLayout::MakePieceBasis;
using GameEngine::SplineLayout::PieceAxis;
using GameEngine::SplineLayout::PieceBasis;
using GameEngine::SplineLayout::TileLayoutParams;
using GameEngine::SplineLayout::TilePose;
using V3 = GameEngine::Mathematics::Vector3;

namespace
{

constexpr float32 kDegToRad = GameEngine::Mathematics::Pi / 180.0f;

// Forward heading `degrees` of yaw right of +Z (this LH +Y-up engine).
V3 YawForward(float32 degrees)
{
    const float32 r = degrees * kDegToRad;
    return V3(std::sin(r), 0.0f, std::cos(r));
}

// Right of travel for the same heading: +X when travelling +Z.
V3 YawRight(float32 degrees)
{
    const float32 r = degrees * kDegToRad;
    return V3(std::cos(r), 0.0f, -std::sin(r));
}

} // namespace

TEST(SeamShear, StraightPathProducesNoShear)
{
    const std::vector<V3> forwards(4, V3(0.0f, 0.0f, 1.0f));
    for (float32 s : ComputeSeamShearFactors(forwards, 8.0f))
        EXPECT_EQ(s, 0.0f);
}

TEST(SeamShear, ZeroCapDisables)
{
    const std::vector<V3> forwards = {YawForward(0.0f), YawForward(20.0f)};
    for (float32 s : ComputeSeamShearFactors(forwards, 0.0f))
        EXPECT_EQ(s, 0.0f);
}

TEST(SeamShear, SingleJointShearsBothNeighboursTowardTheBisector)
{
    const float32 theta = 10.0f;
    const float32 expected = std::tan(0.5f * theta * kDegToRad);

    // Right turn: earlier tile's left (outer) side shears forward => negative.
    const auto right = ComputeSeamShearFactors({YawForward(0.0f), YawForward(theta)}, 8.0f);
    ASSERT_EQ(right.size(), 2u);
    EXPECT_NEAR(right[0], -expected, 1.0e-5f);
    EXPECT_NEAR(right[1], expected, 1.0e-5f);

    // Left turn mirrors.
    const auto left = ComputeSeamShearFactors({YawForward(0.0f), YawForward(-theta)}, 8.0f);
    EXPECT_NEAR(left[0], expected, 1.0e-5f);
    EXPECT_NEAR(left[1], -expected, 1.0e-5f);
}

// The load-bearing geometry: at an isolated joint the two sheared end faces
// must MEET on the joint bisector. Built from first principles — two unit
// tiles (half-width 1, half-length 1) placed around the joint, sheared via
// Right' = Right + s * Forward, then their facing edge corners compared in
// world space. A wrong sign or magnitude leaves the wedge (or doubles it).
TEST(SeamShear, IsolatedJointClosesExactly)
{
    const float32 theta = 20.0f; // below the 45-degree cap given here
    const V3 joint(0.0f, 0.0f, 1.0f);

    const V3 f0 = YawForward(0.0f);
    const V3 r0 = YawRight(0.0f);
    const V3 c0(0.0f, 0.0f, 0.0f); // joint - f0 * halfLength

    const V3 f1 = YawForward(theta);
    const V3 r1 = YawRight(theta);
    const V3 c1 = joint + f1; // joint + f1 * halfLength

    const auto shear = ComputeSeamShearFactors({f0, f1}, 45.0f);
    ASSERT_EQ(shear.size(), 2u);
    const V3 r0s = r0 + f0 * shear[0];
    const V3 r1s = r1 + f1 * shear[1];

    for (float32 x : {-1.0f, 1.0f})
    {
        // Tile 0's end edge corner (local z = +1) vs tile 1's start edge
        // corner (local z = -1) at the same lateral offset.
        const V3 end0 = c0 + r0s * x + f0;
        const V3 start1 = c1 + r1s * x - f1;
        EXPECT_NEAR(end0.x, start1.x, 1.0e-5f) << "lateral " << x;
        EXPECT_NEAR(end0.y, start1.y, 1.0e-5f) << "lateral " << x;
        EXPECT_NEAR(end0.z, start1.z, 1.0e-5f) << "lateral " << x;
    }
}

TEST(SeamShear, CapBindsTheSkew)
{
    const float32 cap = 8.0f;
    const float32 capShear = std::tan(cap * kDegToRad);
    const auto shear = ComputeSeamShearFactors({YawForward(0.0f), YawForward(30.0f)}, cap);
    EXPECT_NEAR(shear[0], -capShear, 1.0e-5f);
    EXPECT_NEAR(shear[1], capShear, 1.0e-5f);
}

// Constant curvature: an interior tile's two joint demands are equal and
// opposite, so they cancel — the shear acts at a turn's entry and exit, and
// the mid-turn wedge remains (an affine shear cannot remove it).
TEST(SeamShear, ConstantCurvatureCancelsInteriorShear)
{
    const float32 theta = 15.0f;
    const std::vector<V3> forwards = {YawForward(0.0f), YawForward(theta), YawForward(2 * theta),
                                      YawForward(3 * theta)};
    const auto shear = ComputeSeamShearFactors(forwards, 8.0f);
    ASSERT_EQ(shear.size(), 4u);
    const float32 expected = std::tan(0.5f * theta * kDegToRad);
    EXPECT_NEAR(shear[0], -expected, 1.0e-5f);
    EXPECT_NEAR(shear[1], 0.0f, 1.0e-5f);
    EXPECT_NEAR(shear[2], 0.0f, 1.0e-5f);
    EXPECT_NEAR(shear[3], expected, 1.0e-5f);
}

// A reversal joint (yaw ~180 degrees) must produce a finite, capped factor,
// never a NaN that would poison the tile transform.
TEST(SeamShear, ReversalJointStaysFiniteAndCapped)
{
    const float32 capShear = std::tan(8.0f * kDegToRad);
    const auto shear = ComputeSeamShearFactors({YawForward(0.0f), YawForward(179.9f)}, 8.0f);
    for (float32 s : shear)
    {
        EXPECT_TRUE(std::isfinite(s));
        EXPECT_LE(std::fabs(s), capShear + 1.0e-6f);
    }
}

TEST(SeamShear, VerticalForwardCarriesNoYawDemand)
{
    const auto shear =
        ComputeSeamShearFactors({YawForward(0.0f), V3(0.0f, 1.0f, 0.0f), YawForward(30.0f)}, 8.0f);
    EXPECT_EQ(shear[0], 0.0f);
    EXPECT_EQ(shear[1], 0.0f);
    EXPECT_EQ(shear[2], 0.0f);
}

// A non-finite forward must take the same "no yaw demand" exit a vertical one
// takes. Nothing downstream can recover it: a NaN ground length makes the
// guard's comparison false, atan2 then yields NaN, and std::clamp passes NaN
// through untouched, so the factor escapes this public function poisoned.
TEST(SeamShear, NaNForwardCarriesNoYawDemand)
{
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    const auto shear =
        ComputeSeamShearFactors({YawForward(0.0f), V3(nan, 0.0f, nan), YawForward(30.0f)}, 8.0f);
    for (float32 s : shear)
        EXPECT_TRUE(std::isfinite(s));
    EXPECT_EQ(shear[0], 0.0f);
    EXPECT_EQ(shear[1], 0.0f);
    EXPECT_EQ(shear[2], 0.0f);
}

// An infinite forward is the quieter half: atan2 of two infinities is a
// perfectly finite 45 degrees, so an unguarded joint invents a real shear
// demand out of garbage instead of announcing itself as NaN.
TEST(SeamShear, InfiniteForwardCarriesNoYawDemand)
{
    const float32 inf = std::numeric_limits<float32>::infinity();
    const auto shear =
        ComputeSeamShearFactors({YawForward(0.0f), V3(inf, 0.0f, 0.0f), YawForward(30.0f)}, 8.0f);
    for (float32 s : shear)
        EXPECT_TRUE(std::isfinite(s));
    EXPECT_EQ(shear[0], 0.0f);
    EXPECT_EQ(shear[1], 0.0f);
    EXPECT_EQ(shear[2], 0.0f);
}

// ---- Applying the factors: does the joint actually close? ------------------
//
// ComputeSeamShearFactors above is well covered; APPLYING them was not, because
// the application lived in SplinePlacementController, a TU no test compiles.
// That gap let a wrong sign ship: the factors are derived from the WORLD
// forwards, so the sheared edge is Right + s * Forward for every piece, but the
// application signed the shear by which local axis the piece is laid on, which
// preserves the piece's LOCAL lean and opens the wedge instead of closing it.
//
// These pin the outcome, not the formula: two tiles abutting a joint must end
// up sharing their corners. Both axes assert closure, so whichever sign is
// wrong is convicted.

namespace
{

constexpr float32 kJointDegrees = 20.0f;
constexpr float32 kAlongHalf = 1.25f;  // half the tile's extent along travel
constexpr float32 kAcrossHalf = 1.0f;  // half its extent across travel

// Where the piece's across-travel axis points: Right for a Z-laid piece, and
// -Right for an X-laid one, which reaches the same world edge through its
// local +Z (Placement/PieceBasis.h).
V3 AcrossVector(const TilePose& pose, PieceAxis axis)
{
    const PieceBasis basis = MakePieceBasis(pose, axis);
    return axis == PieceAxis::X ? basis.LocalZ : basis.LocalX;
}

// Two tiles meeting at the origin across a kJointDegrees joint, each abutting
// it with its own end face. The along-travel axis is Forward for either piece
// axis, so placing them needs only the pose frame — no layout formula is
// borrowed, and the fixture states where the tiles ARE.
std::vector<TilePose> JointPair()
{
    std::vector<TilePose> poses(2);
    const V3 up(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 2; ++i)
    {
        const float32 heading = (i == 0) ? 0.0f : kJointDegrees;
        poses[i].Forward = YawForward(heading);
        poses[i].Right = YawRight(heading);
        poses[i].Up = up;
        // Tile 0 ends on the joint; tile 1 starts there.
        const float32 sign = (i == 0) ? -1.0f : 1.0f;
        poses[i].Base = poses[i].Forward * (sign * kAlongHalf);
        poses[i].Position = poses[i].Base;
    }
    return poses;
}

// Distance between the two tiles' corners on one side of the seam. Zero when
// the joint closes; the wedge width when it does not.
float32 SeamCornerGap(const std::vector<TilePose>& poses, PieceAxis axis, float32 side)
{
    const V3 a = poses[0].Position + poses[0].Forward * kAlongHalf +
                 AcrossVector(poses[0], axis) * (side * kAcrossHalf);
    const V3 b = poses[1].Position - poses[1].Forward * kAlongHalf +
                 AcrossVector(poses[1], axis) * (side * kAcrossHalf);
    const V3 d = a - b;
    return std::sqrt(V3::Dot(d, d));
}

std::vector<CenterSample> StraightLineZ(float32 length, uint32 sampleCount)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        CenterSample sample;
        sample.Pos = V3(0.0f, 0.0f,
                        length * static_cast<float32>(i) / static_cast<float32>(sampleCount - 1u));
        sample.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(sample);
    }
    return center;
}

} // namespace

// The closure instrument, with its own positive control: an unsheared joint
// must MEASURE its wedge (2 * halfWidth * sin(half the deflection)), or a
// fixture that closed for the wrong reason would pass the sheared assertion
// vacuously.
TEST(SeamShear, ShearedJointClosesForEitherPieceAxis)
{
    const float32 expectedWedge =
        2.0f * kAcrossHalf * std::sin(0.5f * kJointDegrees * kDegToRad);

    for (const PieceAxis axis : {PieceAxis::X, PieceAxis::Z})
    {
        const char* name = axis == PieceAxis::X ? "X-laid" : "Z-laid";

        const std::vector<TilePose> unsheared = JointPair();
        for (const float32 side : {-1.0f, 1.0f})
            EXPECT_NEAR(SeamCornerGap(unsheared, axis, side), expectedWedge, 1.0e-4f)
                << name << " control";

        std::vector<TilePose> poses = JointPair();
        const std::vector<V3> forwards = {poses[0].Forward, poses[1].Forward};
        const std::vector<float32> shear = ComputeSeamShearFactors(forwards, 45.0f);
        ASSERT_NE(shear[0], 0.0f) << name;
        ApplySeamShear(poses, shear, V3(0.0f, 0.0f, 0.0f), axis);

        for (const float32 side : {-1.0f, 1.0f})
            EXPECT_LT(SeamCornerGap(poses, axis, side), 1.0e-5f) << name << " side " << side;
    }
}

// The shear also has to carry the footprint-centering offset the layout wrote,
// and that offset reaches Position through a different local axis per piece
// axis — only one of which is Right. The invariant is that the layout's own
// centering still holds once the basis has sheared.
TEST(SeamShear, CenteringOffsetShearsWithTheAxisItWasWrittenOn)
{
    struct Case
    {
        const char* Name;
        V3 HalfExtents;
        PieceAxis Axis;
    };
    const Case cases[] = {
        {"Z-laid", V3(1.2f, 0.1f, 1.2f), PieceAxis::Z},
        {"X-laid", V3(2.0f, 0.1f, 0.3f), PieceAxis::X},
    };
    // A hostile corner pivot, off-centre on both horizontal axes.
    const V3 center(0.7f, 0.05f, -0.4f);
    constexpr float32 kShear = 0.35f;

    for (const Case& c : cases)
    {
        ASSERT_EQ(ChoosePieceAxis(c.HalfExtents), c.Axis) << c.Name;

        TileLayoutParams params;
        params.Spacing = 2.0f;
        params.MeshBoundsCenter = center;
        params.MeshBoundsHalfExtents = c.HalfExtents;
        // PivotPlane means no base lift, so the whole offset from the
        // centerline point is the footprint centering.
        params.PlantMode = Components::SplinePlantMode::PivotPlane;

        std::vector<TilePose> poses = BuildTilePoses(StraightLineZ(8.0f, 17u), params);
        ASSERT_GE(poses.size(), 2u) << c.Name;

        ApplySeamShear(poses, std::vector<float32>(poses.size(), kShear), center, c.Axis);

        for (const TilePose& pose : poses)
        {
            const PieceBasis basis = MakePieceBasis(pose, c.Axis);
            const V3 expected = pose.Base - basis.LocalX * center.x - basis.LocalZ * center.z;
            EXPECT_NEAR(pose.Position.x, expected.x, 1.0e-4f) << c.Name;
            EXPECT_NEAR(pose.Position.y, expected.y, 1.0e-4f) << c.Name;
            EXPECT_NEAR(pose.Position.z, expected.z, 1.0e-4f) << c.Name;
        }
    }
}
