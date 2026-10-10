#include "SplineGeometry/SplineProfile.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;

namespace
{

SplineProfileParams RectangleParams(float32 width, float32 height)
{
    SplineProfileParams p;
    p.Shape = SplineProfileShape::Rectangle;
    p.Width = width;
    p.Height = height;
    return p;
}

SplineProfileParams BevelParams(float32 width, float32 drop, float32 inset)
{
    SplineProfileParams p;
    p.Shape = SplineProfileShape::Bevel;
    p.Width = width;
    p.EdgeDrop = drop;
    p.EdgeInset = inset;
    return p;
}

// The winding rule the whole sweep rests on: outward is the edge direction
// rotated +90 degrees in (lateral, vertical). Checking it on the profile is
// what makes the mesh-level orientation test diagnosable when it fails.
Mathematics::Vector2 OutwardNormal(const Mathematics::Vector2& a, const Mathematics::Vector2& b)
{
    const float32 dx = b.x - a.x;
    const float32 dy = b.y - a.y;
    const float32 len = std::sqrt(dx * dx + dy * dy);
    return Mathematics::Vector2{-dy / len, dx / len};
}

} // namespace

TEST(SplineProfile, RectangleIsAClosedFourPointLoop)
{
    const SplineProfile profile = BuildProfile(RectangleParams(1.0f, 3.0f));

    ASSERT_TRUE(profile.IsValid());
    EXPECT_TRUE(profile.Closed);
    ASSERT_EQ(profile.Points.size(), 4u);
    EXPECT_FLOAT_EQ(profile.NominalHalfWidth, 0.5f);
    // Every corner of a wall is a crease, or the silhouette pillows.
    for (const SplineProfilePoint& p : profile.Points)
        EXPECT_NE(p.Hard, 0u);
}

TEST(SplineProfile, RectangleWindsSoEveryFacePointsAwayFromTheSolid)
{
    const SplineProfile profile = BuildProfile(RectangleParams(2.0f, 4.0f));
    ASSERT_EQ(profile.Points.size(), 4u);

    // The centroid is inside a convex closed profile, so an outward normal must
    // point away from it on every edge. This is the check that catches a
    // reversed point order, which would generate a wall visible only from
    // inside itself.
    Mathematics::Vector2 centroid{0.0f, 0.0f};
    for (const SplineProfilePoint& p : profile.Points)
        centroid = Mathematics::Vector2{centroid.x + p.Position.x * 0.25f,
                                        centroid.y + p.Position.y * 0.25f};

    for (size_t i = 0; i < profile.Points.size(); ++i)
    {
        const Mathematics::Vector2 a = profile.Points[i].Position;
        const Mathematics::Vector2 b = profile.Points[(i + 1u) % profile.Points.size()].Position;
        const Mathematics::Vector2 n = OutwardNormal(a, b);
        const Mathematics::Vector2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const float32 awayFromCentre =
            n.x * (mid.x - centroid.x) + n.y * (mid.y - centroid.y);
        EXPECT_GT(awayFromCentre, 0.0f) << "edge " << i << " faces inward";
    }
}

TEST(SplineProfile, BevelIsAnOpenStripOrderedLeftToRight)
{
    const SplineProfile profile = BuildProfile(BevelParams(4.0f, 0.2f, 0.3f));

    ASSERT_EQ(profile.Points.size(), 4u);
    EXPECT_FALSE(profile.Closed);
    EXPECT_FLOAT_EQ(profile.NominalHalfWidth, 2.0f);
    for (size_t i = 1; i < profile.Points.size(); ++i)
        EXPECT_GT(profile.Points[i].Position.x, profile.Points[i - 1].Position.x);

    // Left to right is what makes the walkable span face the sky.
    const Mathematics::Vector2 n =
        OutwardNormal(profile.Points[1].Position, profile.Points[2].Position);
    EXPECT_NEAR(n.x, 0.0f, 1e-5f);
    EXPECT_NEAR(n.y, 1.0f, 1e-5f);
}

TEST(SplineProfile, BevelWithNoDropOrInsetCollapsesToAFlatRibbon)
{
    // The water/decal case: a flat cross-section needs no new preset, because
    // the coincident-point cleanup turns a degenerate bevel into exactly the
    // two-point strip it describes.
    const SplineProfile profile = BuildProfile(BevelParams(6.0f, 0.0f, 0.0f));

    ASSERT_TRUE(profile.IsValid());
    ASSERT_EQ(profile.Points.size(), 2u);
    EXPECT_FALSE(profile.Closed);
    EXPECT_FLOAT_EQ(profile.Points[0].Position.x, -3.0f);
    EXPECT_FLOAT_EQ(profile.Points[1].Position.x, 3.0f);
    EXPECT_FLOAT_EQ(profile.Points[0].Position.y, 0.0f);
    EXPECT_FLOAT_EQ(profile.Points[1].Position.y, 0.0f);
}

TEST(SplineProfile, CrownNominalHalfWidthIncludesItsShoulders)
{
    SplineProfileParams p;
    p.Shape = SplineProfileShape::Crown;
    p.Width = 6.0f;
    p.CrownRise = 0.15f;
    p.ShoulderWidth = 1.0f;
    p.ShoulderDrop = 0.3f;

    const SplineProfile profile = BuildProfile(p);

    ASSERT_EQ(profile.Points.size(), 5u);
    EXPECT_FALSE(profile.Closed);
    // The width channel must scale the profile's WIDEST extent onto the band
    // the gizmo draws, or the geometry lands outside what the author can see.
    EXPECT_FLOAT_EQ(profile.NominalHalfWidth, 4.0f);
    EXPECT_FLOAT_EQ(profile.Points[2].Position.y, 0.15f);
}

TEST(SplineProfile, BevelInsetIsClampedToHalfTheWidth)
{
    // Past half the width the two inset points would cross and mirror the span;
    // at exactly half they coincide and collapse.
    const SplineProfile profile = BuildProfile(BevelParams(2.0f, 0.3f, 5.0f));

    ASSERT_TRUE(profile.IsValid());
    EXPECT_EQ(profile.Points.size(), 3u);
    EXPECT_FLOAT_EQ(profile.Points[1].Position.x, 0.0f);
}

TEST(SplineProfile, CustomProfileDropsCoincidentPointsAndInheritsTheirCreases)
{
    const std::vector<SplineProfilePoint> authored = {
        {Mathematics::Vector2{-1.0f, 0.0f}, 0u},
        {Mathematics::Vector2{-1.0f, 0.0f}, 1u}, // duplicate carrying a crease
        {Mathematics::Vector2{0.0f, 1.0f}, 0u},
        {Mathematics::Vector2{1.0f, 0.0f}, 0u},
    };

    const SplineProfile profile = BuildCustomProfile(authored, /*closed=*/false);

    ASSERT_EQ(profile.Points.size(), 3u);
    // A dropped duplicate must not take a crease with it.
    EXPECT_NE(profile.Points[0].Hard, 0u);
    EXPECT_FLOAT_EQ(profile.NominalHalfWidth, 1.0f);
}

TEST(SplineProfile, ADegenerateClosedProfileStopsClaimingToBeClosed)
{
    // A zero-thickness wall collapses to a line. Closing it would emit each
    // edge twice and cap a zero area.
    const SplineProfile profile = BuildProfile(RectangleParams(0.0f, 2.0f));

    EXPECT_FALSE(profile.Closed);
    EXPECT_LT(profile.Points.size(), 3u);
    // No lateral extent means lateral scaling has nothing to scale; reporting 1
    // keeps the width channel from dividing by zero.
    EXPECT_FLOAT_EQ(profile.NominalHalfWidth, 1.0f);
}

TEST(SplineProfile, NonFiniteDimensionsDoNotReachThePointList)
{
    // A NaN dimension would propagate into the mesh bounds, and a NaN AABB
    // takes culling with it.
    const SplineProfile profile =
        BuildProfile(RectangleParams(std::nanf(""), std::numeric_limits<float32>::infinity()));

    for (const SplineProfilePoint& p : profile.Points)
    {
        EXPECT_TRUE(std::isfinite(p.Position.x));
        EXPECT_TRUE(std::isfinite(p.Position.y));
    }
    EXPECT_TRUE(std::isfinite(profile.NominalHalfWidth));
}

TEST(SplineProfile, CustomShapeCarriesNoGeneratedPoints)
{
    SplineProfileParams p;
    p.Shape = SplineProfileShape::Custom;

    EXPECT_FALSE(BuildProfile(p).IsValid());
}
