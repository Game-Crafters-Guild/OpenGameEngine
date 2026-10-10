#include "Mathematics/Triangulation.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

using GameEngine::Mathematics::BridgeHole;
using GameEngine::Mathematics::EarClip;
using GameEngine::Mathematics::PointInPolygon;
using GameEngine::Mathematics::PolygonDoubledSignedArea;
using GameEngine::Mathematics::Vector2;

namespace
{

std::vector<std::uint32_t> Sequence(std::size_t count)
{
    std::vector<std::uint32_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0u);
    return indices;
}

float DoubledArea(const Vector2& a, const Vector2& b, const Vector2& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Every triangle wound counter-clockwise (or flat), their areas summing to the polygon's.
void ExpectCovers(const std::vector<Vector2>& points, const std::vector<std::uint32_t>& triangles,
                  float polygonDoubledArea)
{
    ASSERT_EQ(triangles.size() % 3u, 0u);
    float sum = 0.0f;
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const float area = DoubledArea(points[triangles[t]], points[triangles[t + 1]], points[triangles[t + 2]]);
        EXPECT_GE(area, -1.0e-5f) << "triangle " << t / 3 << " is wound clockwise";
        sum += area;
    }
    EXPECT_NEAR(sum, polygonDoubledArea, 1.0e-4f);
}

} // namespace

TEST(EarClip, AConvexQuadIsTwoCounterClockwiseTriangles)
{
    const std::vector<Vector2> square{{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    std::vector<std::uint32_t> triangles;
    EarClip(square, Sequence(square.size()), triangles);
    EXPECT_EQ(triangles.size(), 6u);
    ExpectCovers(square, triangles, PolygonDoubledSignedArea(square));
}

TEST(EarClip, AConcaveOutlineKeepsEveryTriangleInside)
{
    // An L: the reflex corner at (1, 1) must not be cut across.
    const std::vector<Vector2> shape{{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}};
    std::vector<std::uint32_t> triangles;
    EarClip(shape, Sequence(shape.size()), triangles);
    ASSERT_EQ(triangles.size(), 3u * (shape.size() - 2u));
    ExpectCovers(shape, triangles, PolygonDoubledSignedArea(shape));
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const Vector2 centroid = (shape[triangles[t]] + shape[triangles[t + 1]] + shape[triangles[t + 2]]) / 3.0f;
        EXPECT_TRUE(PointInPolygon(centroid, shape)) << "triangle " << t / 3 << " lies outside the L";
    }
}

TEST(EarClip, AStraightCornerStaysAnEdgeOfTheTriangles)
{
    // The mesh plane cut leaves a straight-on corner on every face it crosses; dropping it would
    // open the cap against the faces around it.
    // The last three corners left run straight along the top edge.
    const std::vector<Vector2> shape{{0, 0}, {2, 0}, {2, 1}, {1, 1}, {0, 1}};
    std::vector<std::uint32_t> triangles;
    EarClip(shape, Sequence(shape.size()), triangles);
    ASSERT_EQ(triangles.size(), 9u);
    std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        for (std::size_t k = 0; k < 3; ++k)
            edges.insert(std::minmax(triangles[t + k], triangles[t + (k + 1) % 3]));
    }
    for (std::uint32_t i = 0; i < shape.size(); ++i)
        EXPECT_TRUE(edges.count(std::minmax(i, static_cast<std::uint32_t>((i + 1) % shape.size()))))
            << "outline edge " << i << " is not an edge of any triangle";
    ExpectCovers(shape, triangles, PolygonDoubledSignedArea(shape));
}

TEST(EarClip, AHoleBridgedIntoTheOutlineIsLeftUncovered)
{
    // The plane cut's capped tube: a 4 x 4 square with a 2 x 2 hole, the clockwise hole spliced in
    // from its rightmost corner to the outline's (the bridge runs both ways along one edge).
    const std::vector<Vector2> points{{0, 0}, {4, 0}, {4, 4}, {0, 4}, {3, 1}, {1, 1}, {1, 3}, {3, 3}};
    const std::vector<std::uint32_t> bridged{0, 1, 2, 7, 4, 5, 6, 7, 2, 3};
    std::vector<std::uint32_t> triangles;
    EarClip(points, bridged, triangles);
    ExpectCovers(points, triangles, 2.0f * (16.0f - 4.0f));
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const Vector2 centroid = (points[triangles[t]] + points[triangles[t + 1]] + points[triangles[t + 2]]) / 3.0f;
        EXPECT_FALSE(centroid.x > 1.0f && centroid.x < 3.0f && centroid.y > 1.0f && centroid.y < 3.0f)
            << "triangle " << t / 3 << " covers the hole";
    }
}

TEST(EarClip, AFoldedOutlineEndsWithoutHanging)
{
    // A figure eight has no area to cover consistently; the clip must still end, with n - 2
    // triangles.
    const std::vector<Vector2> eight{{0, 0}, {2, 2}, {2, 0}, {0, 2}};
    std::vector<std::uint32_t> triangles;
    EarClip(eight, Sequence(eight.size()), triangles);
    EXPECT_EQ(triangles.size(), 6u);
}

TEST(EarClip, ASlitLeftWithFlatCornersOnlyIsClippedAtAFlatCorner)
{
    // A zero-width slit, as a bridge spliced in for a hole leaves: the outline runs from (4, 3)
    // down to (2, 1), back up to (3, 2), and later along the slit's line again from (4, 2) to
    // (0, 0). Once its true ears are clipped only flat corners remain, and the next ear is one of
    // them; any other corner would cut a triangle wound clockwise, across the outline.
    const std::vector<Vector2> slit{{4, 3}, {2, 1}, {3, 2}, {4, 2}, {0, 0}, {4, 0}, {4, 4}};
    std::vector<std::uint32_t> triangles;
    EarClip(slit, Sequence(slit.size()), triangles);
    EXPECT_EQ(triangles.size(), 3u * (slit.size() - 2u));
    ExpectCovers(slit, triangles, PolygonDoubledSignedArea(slit));
}

// A square with a square hole: the hole bridged in, the triangles cover the square less the hole
// and none lies over the hole.
TEST(EarClip, AnOutlineWithABridgedHoleLeavesTheHoleUncovered)
{
    const std::vector<Vector2> points{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {4, 4}, {4, 6}, {6, 6}, {6, 4}};
    std::vector<std::uint32_t> outline{0, 1, 2, 3};
    const std::vector<std::uint32_t> hole{4, 5, 6, 7}; // clockwise
    BridgeHole(points, outline, hole);
    EXPECT_EQ(outline.size(), 4u + 4u + 2u) << "the hole and the bridge's two repeated corners";
    std::vector<std::uint32_t> triangles;
    EarClip(points, std::move(outline), triangles);
    ExpectCovers(points, triangles, 2.0f * (100.0f - 4.0f));
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const Vector2 centroid = (points[triangles[t]] + points[triangles[t + 1]] + points[triangles[t + 2]]) * (1.0f / 3.0f);
        EXPECT_FALSE(centroid.x > 4.0f && centroid.x < 6.0f && centroid.y > 4.0f && centroid.y < 6.0f)
            << "triangle " << t / 3 << " covers the hole";
    }
}
