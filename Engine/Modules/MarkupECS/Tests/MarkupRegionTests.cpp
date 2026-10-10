// A region mark-up's outline rules (MarkupRegionOutline) and its area with members
// (MarkupRegionArea): the inside test the agent's markup_contains answers with.

#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionOutline.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Quaternion.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using Components::MarkupMemberMode;
using Components::MarkupRegion;
using Components::MarkupRegionMember;
using Mathematics::Vector2;
using Mathematics::Vector3;
using MarkupECS::CheckRegionMembers;
using MarkupECS::CheckRegionOutline;
using MarkupECS::MarkupRegionArea;

namespace
{

std::vector<Vector2> Square(float32 minX, float32 minZ, float32 size)
{
    return {{minX, minZ}, {minX + size, minZ}, {minX + size, minZ + size}, {minX, minZ + size}};
}

// A ring of `count` points on a circle, counter-clockwise.
std::vector<Vector2> Circle(float32 radius, std::size_t count)
{
    std::vector<Vector2> points;
    for (std::size_t i = 0; i < count; ++i)
    {
        const float32 angle = 6.2831853f * static_cast<float32>(i) / static_cast<float32>(count);
        points.emplace_back(radius * std::cos(angle), radius * std::sin(angle));
    }
    return points;
}

float32 DistanceToRing(const Vector2& p, const std::vector<Vector2>& ring)
{
    float32 nearest = std::numeric_limits<float32>::max();
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
    {
        const Vector2 a = ring[j];
        const Vector2 b = ring[i];
        const float32 abx = b.x - a.x;
        const float32 aby = b.y - a.y;
        const float32 t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / (abx * abx + aby * aby), 0.0f, 1.0f);
        nearest = std::min(nearest, std::hypot(p.x - (a.x + abx * t), p.y - (a.y + aby * t)));
    }
    return nearest;
}

class MarkupRegionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
    }

    void TearDown() override
    {
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
    }

    // A region mark-up whose knots are `knots` (entity-local x, z), placed at `origin`.
    ECS::EntityHandle AddRegion(const std::vector<Vector2>& knots, const Vector3& origin = Vector3(0.0f, 0.0f, 0.0f),
                                Spline::SplineType type = Spline::SplineType::Linear)
    {
        SplineECS::SplineService& splines = SplineECS::SplineService::Get();
        const SplineECS::SplineHandle handle = splines.CreateSpline(type, true);
        Spline::SplineData* data = splines.GetSplineData(handle);
        for (const Vector2& knot : knots)
            data->AddPoint(Vector3(knot.x, 0.0f, knot.y), 0.0f);
        Spline::RebuildSplineCache(*data);
        const ECS::EntityHandle region = AddMarkup(origin, Vector3(1.0f, 1.0f, 1.0f));
        World.AddComponentImmediate(region, MarkupRegion{});
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        World.AddComponentImmediate(region, spline);
        return region;
    }

    // A box (scale = its full extents) or a sphere (scale.x = its diameter) mark-up.
    ECS::EntityHandle AddVolume(Components::MarkupVolumeShape shape, const Vector3& center, const Vector3& scale)
    {
        const ECS::EntityHandle volume = AddMarkup(center, scale);
        World.AddComponentImmediate(volume, Components::MarkupVolume{shape, {}});
        return volume;
    }

    void SetMembers(ECS::EntityHandle region, std::initializer_list<MarkupRegionMember> members)
    {
        MarkupRegion component = *World.GetComponent<MarkupRegion>(region);
        component.MemberCount = static_cast<uint32>(members.size());
        std::copy(members.begin(), members.end(), component.Members);
        World.AddComponentImmediate(region, component);
    }

    const Spline::SplineData* SplineDataOf(ECS::EntityHandle region) const
    {
        const auto* spline = World.GetComponent<Components::SplineComponent>(region);
        return SplineECS::SplineService::Get().GetSplineData(
            SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    }

    bool Inside(ECS::EntityHandle region, float32 x, float32 z)
    {
        const std::optional<MarkupRegionArea> area = MarkupRegionArea::Read(World, region);
        EXPECT_TRUE(area.has_value());
        return area && area->Contains(Vector2(x, z));
    }

    ECS::World World;

  private:
    ECS::EntityHandle AddMarkup(const Vector3& position, const Vector3& scale)
    {
        const ECS::EntityHandle entity = World.CreateEntity();
        const Components::Transform transform =
            Components::Transform::FromTRS(position, Mathematics::Quaternion::Identity(), scale);
        Components::WorldTransform world{};
        std::copy(std::begin(transform.matrix), std::end(transform.matrix), std::begin(world.matrix));
        World.AddComponentImmediate(entity, transform);
        World.AddComponentImmediate(entity, world);
        World.AddComponentImmediate(entity, Components::Markup{});
        return entity;
    }

    bool m_OwnsSplines = false;
};

} // namespace

TEST(MarkupRegionOutline, AnOutlineHasThreeTo256Points)
{
    EXPECT_TRUE(CheckRegionOutline(std::vector<Vector2>{{0.0f, 0.0f}, {10.0f, 0.0f}}).has_value());
    EXPECT_FALSE(CheckRegionOutline(std::vector<Vector2>{{0.0f, 0.0f}, {10.0f, 0.0f}, {0.0f, 10.0f}}).has_value());
    EXPECT_FALSE(CheckRegionOutline(Circle(100.0f, 256)).has_value());
    const std::optional<std::string> tooMany = CheckRegionOutline(Circle(100.0f, 257));
    ASSERT_TRUE(tooMany.has_value());
    EXPECT_NE(tooMany->find("3 to 256 points"), std::string::npos) << *tooMany;
}

TEST(MarkupRegionOutline, AFigureEightIsRefusedNamingTheEdgesThatCross)
{
    // 0 -> 1 runs along the bottom, 2 -> 3 back along the top's diagonal: edges 1-2 and 3-0 cross.
    const std::vector<Vector2> bowtie = {{0.0f, 0.0f}, {10.0f, 0.0f}, {0.0f, 10.0f}, {10.0f, 10.0f}};
    const std::optional<std::string> refusal = CheckRegionOutline(bowtie);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("crosses itself between points 1-2 and 3-0"), std::string::npos) << *refusal;
}

TEST(MarkupRegionOutline, PointsAlongALineEncloseNoArea)
{
    const std::optional<std::string> refusal =
        CheckRegionOutline(std::vector<Vector2>{{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}});
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("encloses no area"), std::string::npos) << *refusal;
    const std::optional<std::string> sliver =
        CheckRegionOutline(std::vector<Vector2>{{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0001f}});
    ASSERT_TRUE(sliver.has_value());
    EXPECT_NE(sliver->find("encloses no area"), std::string::npos) << *sliver;
}

// A sliver 700 m long and 0.03 mm wide, a thousand meters from the origin: refused as
// enclosing no area, and its label point still found in bounded time (a float-stepped seed
// loop never advanced past x = 1000 on it).
TEST(MarkupRegionOutline, ASliverIsRefusedAndItsLabelSearchEnds)
{
    const std::vector<Vector2> sliver = {{1000.0f, 0.0f}, {1700.0f, 0.0f}, {1350.0f, 0.00003f}};
    const std::optional<std::string> refusal = CheckRegionOutline(sliver);
    EXPECT_NE(refusal.value_or("").find("encloses no area"), std::string::npos) << refusal.value_or("accepted");
    const Vector2 label = MarkupECS::RegionLabelPoint(sliver);
    EXPECT_GE(label.x, 1000.0f);
    EXPECT_LE(label.x, 1700.0f);
    // A thin but real strip, 1 km by 2 m, is an outline.
    EXPECT_FALSE(CheckRegionOutline(std::vector<Vector2>{{0.0f, 0.0f}, {1000.0f, 0.0f}, {1000.0f, 2.0f}, {0.0f, 2.0f}})
                     .has_value());
}

// A small region far out (a 2 m square 30 km from the origin) is an outline and measures 4 m²;
// its label point is inside it.
TEST(MarkupRegionOutline, ASmallOutlineFarFromTheOriginIsAcceptedAndMeasured)
{
    const std::vector<Vector2> well = Square(30000.0f, 30000.0f, 2.0f);
    EXPECT_FALSE(CheckRegionOutline(well).has_value()) << CheckRegionOutline(well).value_or("");
    EXPECT_NEAR(MarkupECS::RegionOutlineArea(well), 4.0f, 0.04f);
    EXPECT_TRUE(Mathematics::PointInPolygon(MarkupECS::RegionLabelPoint(well), well));
}

TEST(MarkupRegionOutline, AreaAndPerimeterAreInMeters)
{
    const std::vector<Vector2> field = Square(0.0f, 0.0f, 20.0f);
    EXPECT_FLOAT_EQ(MarkupECS::RegionOutlineArea(field), 400.0f);
    EXPECT_FLOAT_EQ(MarkupECS::RegionOutlinePerimeter(field), 80.0f);
}

// A C whose area centroid falls in its mouth: the label stands inside, in the thick back.
TEST(MarkupRegionOutline, TheLabelPointOfACShapeIsInsideIt)
{
    const std::vector<Vector2> letterC = {{0.0f, 0.0f},   {100.0f, 0.0f},  {100.0f, 20.0f}, {20.0f, 20.0f},
                                          {20.0f, 80.0f}, {100.0f, 80.0f}, {100.0f, 100.0f}, {0.0f, 100.0f}};
    ASSERT_FALSE(CheckRegionOutline(letterC).has_value());
    const Vector2 label = MarkupECS::RegionLabelPoint(letterC);
    EXPECT_TRUE(Mathematics::PointInPolygon(label, letterC)) << label.x << ", " << label.y;
    // The back is 20 m thick: the farthest point from the outline is 10 m in, within 0.5 m.
    EXPECT_GE(DistanceToRing(label, letterC), 9.5f);
}

TEST_F(MarkupRegionTest, TheDecimatedRingOfASmoothRegionStaysWithinATenthOfAMeter)
{
    const ECS::EntityHandle region = AddRegion(Circle(80.0f, 12), Vector3(0.0f, 0.0f, 0.0f), Spline::SplineType::CatmullRom);
    const std::vector<Vector2> tested = MarkupRegionArea::Read(World, region)->GetBase().Ring;
    const Spline::SplineData* data = SplineDataOf(region);
    ASSERT_NE(data, nullptr);
    EXPECT_LE(tested.size(), MarkupECS::kMaxRegionKnots);
    EXPECT_LT(tested.size(), data->ClosedPolygonXZ.size());
    for (const Vector2& point : data->ClosedPolygonXZ)
        EXPECT_LE(DistanceToRing(point, tested), 0.1f + 1e-3f);
}

// A linear outline is tested as its knots: one edge per knot, not the spline's 16 samples per
// segment, which kept the worst point test 16 times over its bound.
TEST_F(MarkupRegionTest, ALinearRegionsFootprintIsItsKnots)
{
    const ECS::EntityHandle region = AddRegion(Circle(100.0f, 256), Vector3(10.0f, 0.0f, 20.0f));
    const std::vector<Vector2> ring = MarkupRegionArea::Read(World, region)->GetBase().Ring;
    ASSERT_EQ(ring.size(), 256u);
    EXPECT_NEAR(ring[0].x, 110.0f, 1e-3f);
    EXPECT_NEAR(ring[0].y, 20.0f, 1e-3f);
}

TEST_F(MarkupRegionTest, ThePointTestIsTheBaseOutlinePlacedByTheWorldTransform)
{
    const ECS::EntityHandle forest = AddRegion(Square(-50.0f, -50.0f, 100.0f), Vector3(1000.0f, 30.0f, 0.0f));
    EXPECT_TRUE(Inside(forest, 1000.0f, 0.0f));
    EXPECT_TRUE(Inside(forest, 1049.0f, 49.0f));
    EXPECT_FALSE(Inside(forest, 0.0f, 0.0f));
    EXPECT_FALSE(Inside(forest, 1051.0f, 0.0f));
}

// A clearing cuts the forest, a box joins a field to it, a sphere high above cuts the circle
// beneath it: inside = (the base or an include) and not an exclude, heights ignored.
TEST_F(MarkupRegionTest, IncludesAddAndExcludesCutAndHeightIsIgnored)
{
    const ECS::EntityHandle forest = AddRegion(Square(0.0f, 0.0f, 200.0f));
    const ECS::EntityHandle clearing = AddRegion(Square(80.0f, 80.0f, 40.0f));
    const ECS::EntityHandle field =
        AddVolume(Components::MarkupVolumeShape::Box, Vector3(250.0f, 0.0f, 100.0f), Vector3(60.0f, 8.0f, 60.0f));
    const ECS::EntityHandle pond =
        AddVolume(Components::MarkupVolumeShape::Sphere, Vector3(30.0f, 500.0f, 30.0f), Vector3(20.0f, 20.0f, 20.0f));
    SetMembers(forest, {{clearing, MarkupMemberMode::Exclude}, {field, MarkupMemberMode::Include},
                        {pond, MarkupMemberMode::Exclude}});

    EXPECT_TRUE(Inside(forest, 10.0f, 150.0f));   // the base
    EXPECT_FALSE(Inside(forest, 100.0f, 100.0f)); // the clearing
    EXPECT_TRUE(Inside(forest, 260.0f, 100.0f));  // the field, outside the base
    EXPECT_FALSE(Inside(forest, 30.0f, 35.0f));   // under the pond, 500 m below it
    EXPECT_FALSE(Inside(forest, 300.0f, 300.0f)); // nowhere
}

// A member region lends its base outline only: its own clearing does not cut the region that
// includes it, so no list recurses.
TEST_F(MarkupRegionTest, AMemberRegionsOwnMembersDoNotApply)
{
    const ECS::EntityHandle village = AddRegion(Square(0.0f, 0.0f, 100.0f));
    const ECS::EntityHandle orchard = AddRegion(Square(100.0f, 0.0f, 100.0f));
    const ECS::EntityHandle well = AddRegion(Square(140.0f, 40.0f, 20.0f));
    SetMembers(orchard, {{well, MarkupMemberMode::Exclude}});
    SetMembers(village, {{orchard, MarkupMemberMode::Include}});

    EXPECT_FALSE(Inside(orchard, 150.0f, 50.0f));
    EXPECT_TRUE(Inside(village, 150.0f, 50.0f));
}

// A deleted member is skipped: its cut closes; the handle stays in the list for undo.
TEST_F(MarkupRegionTest, ADeletedMemberIsSkipped)
{
    const ECS::EntityHandle forest = AddRegion(Square(0.0f, 0.0f, 200.0f));
    const ECS::EntityHandle clearing = AddRegion(Square(80.0f, 80.0f, 40.0f));
    SetMembers(forest, {{clearing, MarkupMemberMode::Exclude}});
    ASSERT_FALSE(Inside(forest, 100.0f, 100.0f));

    World.DestroyEntityImmediate(clearing);
    EXPECT_TRUE(Inside(forest, 100.0f, 100.0f));
    const std::optional<MarkupRegionArea> area = MarkupRegionArea::Read(World, forest);
    ASSERT_EQ(area->GetMembers().size(), 1u);
    EXPECT_FALSE(area->GetMembers()[0].Footprint.has_value());
}

TEST_F(MarkupRegionTest, ABoxFootprintIsItsRotatedOutlineFromAbove)
{
    const ECS::EntityHandle box =
        AddVolume(Components::MarkupVolumeShape::Box, Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 4.0f, 20.0f));
    Components::Transform turned = Components::Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f), Mathematics::Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.78539816f),
        Vector3(20.0f, 4.0f, 20.0f));
    std::copy(std::begin(turned.matrix), std::end(turned.matrix),
              std::begin(World.GetComponentForWrite<Components::WorldTransform>(box)->matrix));
    const std::optional<MarkupECS::MarkupFootprint> footprint = MarkupECS::ReadMarkupFootprint(World, box);
    ASSERT_TRUE(footprint.has_value());
    EXPECT_EQ(footprint->Ring.size(), 4u);
    // A 20 m square turned 45 degrees reaches 14.1 m along the axes and not 10 m along the diagonal.
    EXPECT_TRUE(footprint->Contains(Vector2(13.5f, 0.0f)));
    EXPECT_FALSE(footprint->Contains(Vector2(8.0f, 8.0f)));

    // Tipped 90 degrees about x, a 20 x 4 x 2 box lies 4 m deep on the ground: its outline from
    // above takes the top corners as well as the bottom ones.
    Components::Transform tipped = Components::Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f), Mathematics::Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 1.5707963f),
        Vector3(20.0f, 4.0f, 2.0f));
    std::copy(std::begin(tipped.matrix), std::end(tipped.matrix),
              std::begin(World.GetComponentForWrite<Components::WorldTransform>(box)->matrix));
    const std::optional<MarkupECS::MarkupFootprint> lying = MarkupECS::ReadMarkupFootprint(World, box);
    ASSERT_TRUE(lying.has_value());
    EXPECT_TRUE(lying->Contains(Vector2(0.0f, 1.5f)));
    EXPECT_TRUE(lying->Contains(Vector2(0.0f, -1.5f)));
    EXPECT_FALSE(lying->Contains(Vector2(0.0f, 2.5f)));
}

TEST_F(MarkupRegionTest, AMemberListRefusesTheRegionItselfA33rdMemberARepeatAndAnEntityWithNoShape)
{
    const ECS::EntityHandle forest = AddRegion(Square(0.0f, 0.0f, 200.0f));
    const ECS::EntityHandle clearing = AddRegion(Square(80.0f, 80.0f, 40.0f));
    const ECS::EntityHandle rock = World.CreateEntity();

    const MarkupRegionMember self[] = {{forest, MarkupMemberMode::Exclude}};
    ASSERT_TRUE(CheckRegionMembers(World, forest, self).has_value());
    EXPECT_NE(CheckRegionMembers(World, forest, self)->find("cannot list itself"), std::string::npos);

    std::vector<MarkupRegionMember> many;
    for (uint32 i = 0; i < Components::kMaxRegionMembers; ++i)
        many.push_back({AddVolume(Components::MarkupVolumeShape::Sphere, Vector3(10.0f, 0.0f, 10.0f),
                                  Vector3(2.0f, 2.0f, 2.0f)),
                        MarkupMemberMode::Exclude});
    EXPECT_FALSE(CheckRegionMembers(World, forest, many).has_value());
    many.push_back({clearing, MarkupMemberMode::Exclude});
    ASSERT_TRUE(CheckRegionMembers(World, forest, many).has_value());
    EXPECT_NE(CheckRegionMembers(World, forest, many)->find("at most 32"), std::string::npos);

    const MarkupRegionMember twice[] = {{clearing, MarkupMemberMode::Exclude}, {clearing, MarkupMemberMode::Include}};
    EXPECT_TRUE(CheckRegionMembers(World, forest, twice).has_value());
    const MarkupRegionMember stone[] = {{rock, MarkupMemberMode::Exclude}};
    EXPECT_TRUE(CheckRegionMembers(World, forest, stone).has_value());
}
