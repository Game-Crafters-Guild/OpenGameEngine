#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Spline/SplineTypes.h"

#include <gtest/gtest.h>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Spline;
using namespace GameEngine::Mathematics;

namespace
{

SplineData MakeChain(uint32 count)
{
    SplineData s;
    for (uint32 i = 0; i < count; ++i)
    {
        s.AddPoint(Vector3(static_cast<float32>(i) * 10.0f, 0.0f, 0.0f),
                   1.0f + static_cast<float32>(i));
        s.SetPointRoll(i, 0.1f * static_cast<float32>(i + 1));
        s.SetPointRotation(i, Vector3(0.0f, 15.0f * static_cast<float32>(i), 0.0f));
        s.SetPointScale(i, Vector3(1.0f + static_cast<float32>(i), 1.0f, 1.0f));
    }
    return s;
}

} // namespace

// A spline needs two points to have a segment at all: removal that would breach
// the floor is refused outright rather than clamped to a degenerate one-point
// spline that IsValid() rejects downstream.
TEST(SplinePointRemoval, RefusesToDropBelowTwoPoints)
{
    SplineData s = MakeChain(2);

    EXPECT_FALSE(s.RemovePoint(0));
    EXPECT_EQ(s.Points.size(), 2u);
    EXPECT_TRUE(s.IsValid());

    EXPECT_FALSE(s.RemovePoint(1));
    EXPECT_EQ(s.Points.size(), 2u);
    EXPECT_TRUE(s.IsValid());
}

TEST(SplinePointRemoval, RefusesAnOutOfRangeIndex)
{
    SplineData s = MakeChain(4);

    EXPECT_FALSE(s.RemovePoint(4));
    EXPECT_FALSE(s.RemovePoint(99));
    EXPECT_EQ(s.Points.size(), 4u);
}

// A refused removal must not look like an edit: consumers poll Version to
// decide whether to rebuild, and a bump with no change is wasted work.
TEST(SplinePointRemoval, RefusedRemovalLeavesVersionUntouched)
{
    SplineData s = MakeChain(2);
    const uint64 versionBefore = s.Version;

    EXPECT_FALSE(s.RemovePoint(0));
    EXPECT_EQ(s.Version, versionBefore);

    SplineData four = MakeChain(4);
    const uint64 fourBefore = four.Version;
    EXPECT_FALSE(four.RemovePoint(4));
    EXPECT_EQ(four.Version, fourBefore);
}

TEST(SplinePointRemoval, SuccessfulRemovalBumpsVersionAndMarksDirty)
{
    SplineData s = MakeChain(4);
    RebuildSplineCache(s);
    s.Dirty = false;
    const uint64 versionBefore = s.Version;

    EXPECT_TRUE(s.RemovePoint(1));

    EXPECT_EQ(s.Points.size(), 3u);
    EXPECT_GT(s.Version, versionBefore);
    EXPECT_TRUE(s.Dirty);
}

// The surviving points keep their own channels and shift down by one; removal
// is an erase, not a rewrite.
TEST(SplinePointRemoval, SurvivorsKeepTheirChannels)
{
    SplineData s = MakeChain(4);
    const std::vector<SplineControlPoint> before = s.Points;

    ASSERT_TRUE(s.RemovePoint(1));
    ASSERT_EQ(s.Points.size(), 3u);

    const uint32 expectedSource[] = {0u, 2u, 3u};
    for (uint32 i = 0; i < 3; ++i)
    {
        const SplineControlPoint& got = s.Points[i];
        const SplineControlPoint& want = before[expectedSource[i]];
        EXPECT_FLOAT_EQ(got.Position.x, want.Position.x) << "Position " << i;
        EXPECT_FLOAT_EQ(got.Radius, want.Radius) << "Radius " << i;
        EXPECT_FLOAT_EQ(got.Roll, want.Roll) << "Roll " << i;
        EXPECT_FLOAT_EQ(got.Rotation.y, want.Rotation.y) << "Rotation " << i;
        EXPECT_FLOAT_EQ(got.Scale.x, want.Scale.x) << "Scale " << i;
    }
}

// Explicit Bezier handles belong to the points that carry them: erasing a
// neighbour must not rewrite the survivors' tangents.
TEST(SplinePointRemoval, BezierHandlesSurviveOnRemainingPoints)
{
    SplineData s = MakeChain(4);
    s.Type = SplineType::CubicBezier;
    for (uint32 i = 0; i < 4; ++i)
    {
        const float32 f = static_cast<float32>(i + 1);
        s.SetPointTangents(i, Vector3(-f, 0.0f, 0.0f), Vector3(f, 0.0f, 0.0f));
    }
    const std::vector<SplineControlPoint> before = s.Points;

    ASSERT_TRUE(s.RemovePoint(2));
    ASSERT_EQ(s.Points.size(), 3u);

    const uint32 expectedSource[] = {0u, 1u, 3u};
    for (uint32 i = 0; i < 3; ++i)
    {
        EXPECT_FLOAT_EQ(s.Points[i].TangentIn.x, before[expectedSource[i]].TangentIn.x) << i;
        EXPECT_FLOAT_EQ(s.Points[i].TangentOut.x, before[expectedSource[i]].TangentOut.x) << i;
    }
}

// Closed splines wrap segment N-1 back to point 0, so the segment count tracks
// the point count exactly and the ring stays evaluable after a removal.
TEST(SplinePointRemoval, ClosedLoopStaysConsistentAfterRemoval)
{
    SplineData s = MakeChain(4);
    s.Points[1].Position = Vector3(10.0f, 0.0f, 10.0f);
    s.Points[2].Position = Vector3(0.0f, 0.0f, 10.0f);
    s.Closed = true;
    RebuildSplineCache(s);
    ASSERT_EQ(s.GetSegmentCount(), 4u);

    ASSERT_TRUE(s.RemovePoint(0));
    EXPECT_EQ(s.Points.size(), 3u);
    EXPECT_TRUE(s.Closed);
    EXPECT_EQ(s.GetSegmentCount(), 3u);

    RebuildSplineCache(s);
    EXPECT_EQ(s.SegmentBounds.size(), 3u);
    EXPECT_GT(s.TotalArcLength, 0.0f);

    // The floor applies to closed splines too — two points is the last legal
    // point count, not a special case.
    ASSERT_TRUE(s.RemovePoint(0));
    EXPECT_EQ(s.Points.size(), 2u);
    EXPECT_FALSE(s.RemovePoint(0));
    EXPECT_EQ(s.Points.size(), 2u);

    // Two points cannot ring: the authored flag survives untouched while the
    // geometry generates as a single open segment.
    EXPECT_TRUE(s.Closed);
    EXPECT_FALSE(s.IsEffectivelyClosed());
    EXPECT_EQ(s.GetSegmentCount(), 1u);
}

// Removing the last point of an open spline shortens the curve from the end
// rather than reindexing around it.
TEST(SplinePointRemoval, RemovingAnEndpointShortensTheCurve)
{
    SplineData s = MakeChain(4);
    s.Type = SplineType::Linear;
    RebuildSplineCache(s);
    const float32 lengthBefore = s.TotalArcLength;
    ASSERT_GT(lengthBefore, 0.0f);

    ASSERT_TRUE(s.RemovePoint(3));
    RebuildSplineCache(s);

    EXPECT_EQ(s.Points.size(), 3u);
    EXPECT_EQ(s.GetSegmentCount(), 2u);
    EXPECT_LT(s.TotalArcLength, lengthBefore);
    EXPECT_FLOAT_EQ(s.Points.back().Position.x, 20.0f);
}
