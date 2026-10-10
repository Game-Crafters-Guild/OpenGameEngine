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

// Points on the +X axis so a doubling ring shows up as a non-monotone x.
SplineData MakeAxisChain(uint32 count, bool closed)
{
    SplineData s;
    s.Type = SplineType::Linear;
    s.Closed = closed;
    for (uint32 i = 0; i < count; ++i)
        s.AddPoint(Vector3(static_cast<float32>(i) * 10.0f, 0.0f, 0.0f), 1.0f);
    return s;
}

} // namespace

// The predicate is the single place the "a ring needs three points" rule lives.
// Every geometry consumer asks it; nothing re-derives the rule locally.
TEST(SplineEffectiveClosed, PredicateTruthTable)
{
    SplineData onePointClosed;
    onePointClosed.Closed = true;
    onePointClosed.AddPoint(Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(onePointClosed.IsEffectivelyClosed());

    EXPECT_FALSE(MakeAxisChain(2, /*closed=*/false).IsEffectivelyClosed());
    EXPECT_FALSE(MakeAxisChain(3, /*closed=*/false).IsEffectivelyClosed());
    EXPECT_FALSE(MakeAxisChain(4, /*closed=*/false).IsEffectivelyClosed());

    EXPECT_FALSE(MakeAxisChain(2, /*closed=*/true).IsEffectivelyClosed());
    EXPECT_TRUE(MakeAxisChain(3, /*closed=*/true).IsEffectivelyClosed());
    EXPECT_TRUE(MakeAxisChain(4, /*closed=*/true).IsEffectivelyClosed());
}

// The authored flag is never consulted for geometry, and never mutated by it:
// a two-point loop reports Closed exactly as the user left it.
TEST(SplineEffectiveClosed, PredicateNeverMutatesTheAuthoredFlag)
{
    SplineData s = MakeAxisChain(2, /*closed=*/true);
    EXPECT_FALSE(s.IsEffectivelyClosed());
    EXPECT_TRUE(s.Closed);

    RebuildSplineCache(s);
    EXPECT_TRUE(s.Closed);
}

// Two points cannot enclose anything, so the wrap segment is not generated:
// one segment, A to B, exactly as an open spline of the same points.
TEST(SplineEffectiveClosed, TwoPointClosedGeneratesOneOpenSegment)
{
    SplineData closed = MakeAxisChain(2, /*closed=*/true);
    EXPECT_EQ(closed.GetSegmentCount(), 1u);

    SplineData open = MakeAxisChain(2, /*closed=*/false);
    EXPECT_EQ(closed.GetSegmentCount(), open.GetSegmentCount());
}

TEST(SplineEffectiveClosed, ThreePointClosedGeneratesThreeSegments)
{
    SplineData s = MakeAxisChain(3, /*closed=*/true);
    EXPECT_EQ(s.GetSegmentCount(), 3u);
}

// The consumer-level pin: the sampled polyline of a two-point closed spline
// advances from the first point to the last and stops. A generated ring would
// turn around at the far knot and retrace its own centreline.
TEST(SplineEffectiveClosed, TwoPointClosedPolylineDoesNotDoubleBack)
{
    SplineData s = MakeAxisChain(2, /*closed=*/true);
    RebuildSplineCache(s);

    std::vector<SplineFrame> frames;
    SampleUniform(s, 32, frames);
    ASSERT_EQ(frames.size(), 32u);

    for (size_t i = 1; i < frames.size(); ++i)
    {
        EXPECT_GE(frames[i].Position.x, frames[i - 1].Position.x - 1.0e-3f)
            << "sample " << i << " retraced toward the start";
    }

    EXPECT_NEAR(frames.front().Position.x, 0.0f, 0.2f);
    EXPECT_NEAR(frames.back().Position.x, 10.0f, 0.2f);
    EXPECT_NEAR(s.TotalArcLength, 10.0f, 0.2f);
}

// A degenerate ring would still cache a zero-area interior polygon and let
// point-in-polygon tests fire on it. Effective-open clears the cache instead.
TEST(SplineEffectiveClosed, TwoPointClosedCachesNoInteriorPolygon)
{
    SplineData s = MakeAxisChain(2, /*closed=*/true);
    RebuildSplineCache(s);
    EXPECT_TRUE(s.ClosedPolygonXZ.empty());

    SplineData ring = MakeAxisChain(3, /*closed=*/true);
    ring.SetPointPosition(2, Vector3(10.0f, 0.0f, 10.0f));
    RebuildSplineCache(ring);
    EXPECT_FALSE(ring.ClosedPolygonXZ.empty());
}

// The whole point of leaving the flag alone: the loop comes back by itself.
TEST(SplineEffectiveClosed, ReAddingAThirdPointRestoresTheRing)
{
    SplineData s = MakeAxisChain(3, /*closed=*/true);
    RebuildSplineCache(s);
    ASSERT_EQ(s.GetSegmentCount(), 3u);

    ASSERT_TRUE(s.RemovePoint(2));
    EXPECT_TRUE(s.Closed);
    EXPECT_FALSE(s.IsEffectivelyClosed());
    EXPECT_EQ(s.GetSegmentCount(), 1u);

    s.AddPoint(Vector3(10.0f, 0.0f, 10.0f));
    EXPECT_TRUE(s.IsEffectivelyClosed());
    EXPECT_EQ(s.GetSegmentCount(), 3u);
}
