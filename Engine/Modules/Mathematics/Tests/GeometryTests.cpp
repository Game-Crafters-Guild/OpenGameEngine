#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <vector>

using GameEngine::Mathematics::AABB;
using GameEngine::Mathematics::IntersectRaySphere;
using GameEngine::Mathematics::PointInPolygon;
using GameEngine::Mathematics::Ray3D;
using GameEngine::Mathematics::Vector2;
using GameEngine::Mathematics::Vector3;

namespace
{
Ray3D RayAlongZ(float originZ)
{
    Ray3D ray;
    ray.origin = Vector3(0.0f, 0.0f, originZ);
    ray.direction = Vector3(0.0f, 0.0f, 1.0f);
    return ray;
}
} // namespace

TEST(GeometryTests, AabbCornerIndexBitsSelectTheMaxOnEachAxis)
{
    const AABB box{Vector3(-1.0f, -2.0f, -3.0f), Vector3(4.0f, 5.0f, 6.0f)};
    const auto corners = box.Corners();
    for (int i = 0; i < 8; ++i)
    {
        EXPECT_EQ(corners[i].x, (i & 1) ? 4.0f : -1.0f) << "corner " << i;
        EXPECT_EQ(corners[i].y, (i & 2) ? 5.0f : -2.0f) << "corner " << i;
        EXPECT_EQ(corners[i].z, (i & 4) ? 6.0f : -3.0f) << "corner " << i;
    }
}

TEST(GeometryTests, RaySphereHitsTheNearSurfaceFromOutside)
{
    float t = -1.0f;
    ASSERT_TRUE(IntersectRaySphere(RayAlongZ(-10.0f), Vector3(0.0f, 0.0f, 0.0f), 2.0f, t));
    EXPECT_FLOAT_EQ(t, 8.0f);
}

TEST(GeometryTests, RaySphereHitsTheFarSurfaceFromInside)
{
    float t = -1.0f;
    ASSERT_TRUE(IntersectRaySphere(RayAlongZ(0.0f), Vector3(0.0f, 0.0f, 0.0f), 2.0f, t));
    EXPECT_FLOAT_EQ(t, 2.0f);
}

TEST(GeometryTests, RaySphereMissesASphereBesideOrBehindTheRay)
{
    float t = -1.0f;
    EXPECT_FALSE(IntersectRaySphere(RayAlongZ(-10.0f), Vector3(3.0f, 0.0f, 0.0f), 2.0f, t));
    EXPECT_FALSE(IntersectRaySphere(RayAlongZ(10.0f), Vector3(0.0f, 0.0f, 0.0f), 2.0f, t));
    EXPECT_FLOAT_EQ(t, -1.0f);
}

TEST(GeometryTests, PointInPolygonFollowsTheEvenOddRuleOnAConcaveOutline)
{
    // An L: the square [0,2]x[0,2] with its top-right quarter cut away.
    const std::vector<Vector2> outline = {
        {0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 2.0f}, {0.0f, 2.0f}};
    EXPECT_TRUE(PointInPolygon(Vector2(0.5f, 0.5f), outline));
    EXPECT_TRUE(PointInPolygon(Vector2(1.5f, 0.5f), outline));
    EXPECT_TRUE(PointInPolygon(Vector2(0.5f, 1.5f), outline));
    EXPECT_FALSE(PointInPolygon(Vector2(1.5f, 1.5f), outline));
    EXPECT_FALSE(PointInPolygon(Vector2(3.0f, 0.5f), outline));
    // Left of the outline: the ray to +x crosses two edges.
    EXPECT_FALSE(PointInPolygon(Vector2(-1.0f, 0.5f), outline));
}

TEST(GeometryTests, PointInPolygonBoundaryIsHalfOpenUnderEitherWinding)
{
    // The square [0,2]x[0,2]: the left and bottom edges are inside, the right and top edges
    // outside, so two polygons sharing an edge never both claim a point on it.
    const std::vector<Vector2> counterClockwise = {{0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}};
    const std::vector<Vector2> clockwise = {{0.0f, 0.0f}, {0.0f, 2.0f}, {2.0f, 2.0f}, {2.0f, 0.0f}};
    for (const std::vector<Vector2>* square : {&counterClockwise, &clockwise})
    {
        EXPECT_TRUE(PointInPolygon(Vector2(1.0f, 1.0f), *square));
        EXPECT_TRUE(PointInPolygon(Vector2(0.0f, 1.0f), *square));
        EXPECT_TRUE(PointInPolygon(Vector2(1.0f, 0.0f), *square));
        EXPECT_FALSE(PointInPolygon(Vector2(2.0f, 1.0f), *square));
        EXPECT_FALSE(PointInPolygon(Vector2(1.0f, 2.0f), *square));
        EXPECT_TRUE(PointInPolygon(Vector2(0.0f, 0.0f), *square));
        EXPECT_FALSE(PointInPolygon(Vector2(2.0f, 0.0f), *square));
        EXPECT_FALSE(PointInPolygon(Vector2(2.0f, 2.0f), *square));
        EXPECT_FALSE(PointInPolygon(Vector2(0.0f, 2.0f), *square));
    }

    // Inside the bounding box of a triangle but past its hypotenuse.
    const std::vector<Vector2> triangle = {{0.0f, 0.0f}, {4.0f, 0.0f}, {0.0f, 4.0f}};
    EXPECT_FALSE(PointInPolygon(Vector2(3.0f, 3.0f), triangle));
    EXPECT_FALSE(PointInPolygon(Vector2(2.0f, 2.0f), triangle));
}

TEST(GeometryTests, PointInPolygonDegenerateOutlinesContainNothing)
{
    EXPECT_FALSE(PointInPolygon(Vector2(0.0f, 0.0f), std::vector<Vector2>{}));
    EXPECT_FALSE(PointInPolygon(Vector2(0.0f, 0.0f), std::vector<Vector2>{{0.0f, 0.0f}}));
    EXPECT_FALSE(PointInPolygon(Vector2(1.0f, 1.0f), std::vector<Vector2>{{0.0f, 0.0f}, {2.0f, 2.0f}}));
    const std::vector<Vector2> collinear = {{0.0f, 0.0f}, {1.0f, 1.0f}, {2.0f, 2.0f}};
    EXPECT_FALSE(PointInPolygon(Vector2(1.0f, 1.0f), collinear));
    EXPECT_FALSE(PointInPolygon(Vector2(0.5f, 0.6f), collinear));
    const std::vector<Vector2> collapsed = {{1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}};
    EXPECT_FALSE(PointInPolygon(Vector2(1.0f, 1.0f), collapsed));
}

TEST(GeometryTests, AabbIntersectionIsTheOverlapAndEmptyWhenDisjoint)
{
    const AABB a{{0.0f, 0.0f, 0.0f}, {4.0f, 2.0f, 2.0f}};
    const AABB b{{1.0f, -1.0f, 1.0f}, {6.0f, 1.0f, 3.0f}};
    const AABB overlap = AABB::Intersection(a, b);
    EXPECT_FALSE(overlap.IsEmpty());
    EXPECT_FLOAT_EQ(overlap.min.x, 1.0f);
    EXPECT_FLOAT_EQ(overlap.min.y, 0.0f);
    EXPECT_FLOAT_EQ(overlap.min.z, 1.0f);
    EXPECT_FLOAT_EQ(overlap.max.x, 4.0f);
    EXPECT_FLOAT_EQ(overlap.max.y, 1.0f);
    EXPECT_FLOAT_EQ(overlap.max.z, 2.0f);

    // Apart on one axis only is still apart, and the result is Empty() itself,
    // which adds nothing when expanded into: an inverted finite box would pull
    // the box it is expanded into toward it.
    const AABB beside{{0.0f, 0.0f, 2.5f}, {4.0f, 2.0f, 3.0f}};
    EXPECT_TRUE(AABB::Intersection(a, beside).IsEmpty());
    AABB grown = b;
    grown.Expand(AABB::Intersection(a, beside));
    EXPECT_FLOAT_EQ(grown.min.x, b.min.x);
    EXPECT_FLOAT_EQ(grown.min.y, b.min.y);
    EXPECT_FLOAT_EQ(grown.min.z, b.min.z);
    EXPECT_FLOAT_EQ(grown.max.x, b.max.x);
    EXPECT_FLOAT_EQ(grown.max.y, b.max.y);
    EXPECT_FLOAT_EQ(grown.max.z, b.max.z);
    EXPECT_TRUE(AABB::Intersection(a, AABB::Empty()).IsEmpty());
    // Touching faces share a plane of points.
    const AABB touching{{4.0f, 0.0f, 0.0f}, {5.0f, 2.0f, 2.0f}};
    EXPECT_FALSE(AABB::Intersection(a, touching).IsEmpty());
}

TEST(GeometryTests, AabbInflateMovesEveryFaceOutAndKeepsAnEmptyBoxEmpty)
{
    AABB box{{0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f}};
    box.Inflate(Vector3(0.5f, 1.0f, 0.25f));
    EXPECT_FLOAT_EQ(box.min.x, -0.5f);
    EXPECT_FLOAT_EQ(box.min.y, -1.0f);
    EXPECT_FLOAT_EQ(box.min.z, -0.25f);
    EXPECT_FLOAT_EQ(box.max.x, 1.5f);
    EXPECT_FLOAT_EQ(box.max.y, 3.0f);
    EXPECT_FLOAT_EQ(box.max.z, 3.25f);

    AABB empty = AABB::Empty();
    empty.Inflate(Vector3(10.0f, 10.0f, 10.0f));
    EXPECT_TRUE(empty.IsEmpty());
}

TEST(GeometryTests, PolygonAreaIsSignedByWindingAndHalfTheDoubledSum)
{
    const std::vector<Vector2> counterClockwise = {{0.0f, 0.0f}, {4.0f, 0.0f}, {4.0f, 3.0f}, {0.0f, 3.0f}};
    const std::vector<Vector2> clockwise(counterClockwise.rbegin(), counterClockwise.rend());
    EXPECT_FLOAT_EQ(GameEngine::Mathematics::PolygonDoubledSignedArea(counterClockwise), 24.0f);
    EXPECT_FLOAT_EQ(GameEngine::Mathematics::PolygonDoubledSignedArea(clockwise), -24.0f);
}

// A 2 m square 30 km from the origin: absolute shoelace products there are 9e8 m², whose float
// rounding swamps the 4 m² the square holds.
TEST(GeometryTests, PolygonAreaFarFromTheOriginKeepsItsPrecision)
{
    const std::vector<Vector2> square = {{30000.0f, 30000.0f}, {30002.0f, 30000.0f}, {30002.0f, 30002.0f},
                                         {30000.0f, 30002.0f}};
    EXPECT_NEAR(GameEngine::Mathematics::PolygonDoubledSignedArea(square), 8.0f, 0.08f);
}

TEST(GeometryTests, SegmentsIntersectWhenTheyCrossOrTouchAndNotWhenApart)
{
    using GameEngine::Mathematics::SegmentsIntersect;
    EXPECT_TRUE(SegmentsIntersect({0.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}, {2.0f, 0.0f}));
    EXPECT_TRUE(SegmentsIntersect({0.0f, 0.0f}, {2.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 3.0f})); // a T
    EXPECT_TRUE(SegmentsIntersect({0.0f, 0.0f}, {2.0f, 0.0f}, {1.0f, 0.0f}, {3.0f, 0.0f})); // collinear overlap
    EXPECT_FALSE(SegmentsIntersect({0.0f, 0.0f}, {2.0f, 0.0f}, {3.0f, 0.0f}, {4.0f, 0.0f}));
    EXPECT_FALSE(SegmentsIntersect({0.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 1.0f}, {1.0f, 2.0f}));
}

TEST(GeometryTests, ConvexHullKeepsTheCornersCounterClockwiseAndDropsInnerAndCollinearPoints)
{
    const std::vector<Vector2> points = {{1.0f, 1.0f}, {0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f},
                                         {0.0f, 2.0f}, {1.0f, 0.0f}, {2.0f, 2.0f}};
    const std::vector<Vector2> hull = GameEngine::Mathematics::ConvexHull(points);
    ASSERT_EQ(hull.size(), 4u);
    const Vector2 expected[] = {{0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}};
    for (size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(hull[i].x, expected[i].x) << i;
        EXPECT_EQ(hull[i].y, expected[i].y) << i;
    }
    EXPECT_GT(GameEngine::Mathematics::PolygonDoubledSignedArea(hull), 0.0f);
}
