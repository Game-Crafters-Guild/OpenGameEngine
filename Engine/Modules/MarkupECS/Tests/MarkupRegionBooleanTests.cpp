// The boolean a region's display draws (CombineRegionArea): the base joined with its Include
// members and cut by its Exclude members.

#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionBoolean.h"
#include "MarkupECS/MarkupRegionOutline.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace GameEngine;
using MarkupECS::CombineRegionArea;
using MarkupECS::MarkupAreaPiece;
using MarkupECS::MarkupFootprint;
using Mathematics::Vector2;

namespace
{

std::vector<Vector2> Square(float minX, float minZ, float size)
{
    return {{minX, minZ}, {minX + size, minZ}, {minX + size, minZ + size}, {minX, minZ + size}};
}

MarkupFootprint RingFootprint(std::vector<Vector2> ring)
{
    MarkupFootprint footprint;
    footprint.Ring = std::move(ring);
    return footprint;
}

} // namespace

// An Exclude inside the base is a hole: one piece, its outer ring the base wound counter-clockwise
// (the base given clockwise), its hole the exclude wound clockwise.
TEST(MarkupRegionBoolean, AnExcludeInsideTheBaseIsAHole)
{
    std::vector<Vector2> base = Square(0.0f, 0.0f, 40.0f);
    std::reverse(base.begin(), base.end());
    const std::vector<MarkupFootprint> excludes{RingFootprint(Square(10.0f, 10.0f, 10.0f))};
    const std::vector<MarkupAreaPiece> pieces = CombineRegionArea(base, {}, excludes);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_NEAR(Mathematics::PolygonDoubledSignedArea(pieces[0].Outer), 2.0f * 1600.0f, 0.1f);
    ASSERT_EQ(pieces[0].Holes.size(), 1u);
    EXPECT_NEAR(Mathematics::PolygonDoubledSignedArea(pieces[0].Holes[0]), -2.0f * 100.0f, 0.1f);
}

// An Include that overlaps the base joins it into one ring; one apart from it is a piece of its
// own; a circle Exclude cuts a hole of kFootprintCircleCorners corners.
TEST(MarkupRegionBoolean, IncludesJoinOrStandApartAndACircleCutsAPolygonHole)
{
    const std::vector<MarkupFootprint> includes{RingFootprint(Square(30.0f, 10.0f, 20.0f)),
                                                RingFootprint(Square(100.0f, 0.0f, 10.0f))};
    MarkupFootprint pond;
    pond.Center = Vector2(15.0f, 20.0f);
    pond.Radius = 5.0f;
    const std::vector<MarkupFootprint> excludes{pond};
    const std::vector<MarkupAreaPiece> pieces = CombineRegionArea(Square(0.0f, 0.0f, 40.0f), includes, excludes);
    ASSERT_EQ(pieces.size(), 2u);
    const MarkupAreaPiece& joined = pieces[0].Outer.size() > 4 ? pieces[0] : pieces[1];
    const MarkupAreaPiece& apart = pieces[0].Outer.size() > 4 ? pieces[1] : pieces[0];
    EXPECT_NEAR(MarkupECS::RegionOutlineArea(joined.Outer), 1600.0f + 200.0f, 0.1f) << "the overlap counted twice";
    ASSERT_EQ(joined.Holes.size(), 1u);
    EXPECT_EQ(joined.Holes[0].size(), MarkupECS::kFootprintCircleCorners);
    EXPECT_NEAR(MarkupECS::RegionOutlineArea(apart.Outer), 100.0f, 0.1f);
    EXPECT_TRUE(apart.Holes.empty());
}
