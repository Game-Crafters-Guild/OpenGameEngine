#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineWall.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ECSTemplates.h"
#include "Placement/SplineWallSanitize.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <vector>

using namespace GameEngine;
using Components::MaterialRef;
using Components::SplineWall;
using Components::SplineWallCorner;
using Components::SplineWallGrade;
using Editor::SanitizeWallRecipe;
using Editor::WallRecipeValidation;

namespace
{

MaterialRef MaterialTestRef(uint8 ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0xC3;
    return MaterialRef(GUID(data));
}

} // namespace

// The recipe rides an ECS chunk column and is serialized whole. Its size is
// pinned exactly so a field cannot be added without the flip table and the
// sanitizer below learning about it.
TEST(SplineWallRecipe, EveryFieldParticipatesInEquality)
{
    const SplineWall base{};
    EXPECT_TRUE(base == SplineWall{});

    struct Case
    {
        const char* Name;
        void (*Flip)(SplineWall&);
    };
    static_assert(sizeof(SplineWall) == 44u,
                  "SplineWall changed size: update the flip table beside this assert and "
                  "SanitizeWallRecipe before re-baselining");
    const Case cases[] = {
        {"Thickness", [](SplineWall& w) { w.Thickness = 1.25f; }},
        {"Height", [](SplineWall& w) { w.Height = 4.5f; }},
        {"Grade", [](SplineWall& w) { w.Grade = SplineWallGrade::Stepped; }},
        {"Corner", [](SplineWall& w) { w.Corner = SplineWallCorner::Round; }},
        {"Material", [](SplineWall& w) { w.Material = MaterialTestRef(4); }},
        {"ConformMode",
         [](SplineWall& w) { w.ConformMode = Components::SplinePlacementConform::None; }},
        {"ConformTarget",
         [](SplineWall& w) { w.ConformTarget = Components::SplineConformTarget::TerrainOnly; }},
        {"CastShadows", [](SplineWall& w) { w.CastShadows = false; }},
        {"ReceiveShadows", [](SplineWall& w) { w.ReceiveShadows = false; }},
    };
    for (const Case& c : cases)
    {
        SplineWall flipped = base;
        c.Flip(flipped);
        EXPECT_FALSE(flipped == base) << c.Name << " does not participate in operator==";
    }
}

// Adding the component to a spline with nothing else set stands a 0.6 m by 3 m
// wall on the ground, racked, in the built-in material.
TEST(SplineWallRecipe, DefaultsDescribeAPlainWallOnTheFirstTry)
{
    const SplineWall wall{};
    EXPECT_FLOAT_EQ(wall.Thickness, 0.6f);
    EXPECT_FLOAT_EQ(wall.Height, 3.0f);
    EXPECT_EQ(wall.Grade, SplineWallGrade::Racked);
    EXPECT_TRUE(wall.Material.IsNull());
    EXPECT_EQ(wall.ConformMode, Components::SplinePlacementConform::Height);
    EXPECT_EQ(wall.ConformTarget, Components::SplineConformTarget::Scene);
    EXPECT_TRUE(wall.CastShadows);
    EXPECT_TRUE(wall.ReceiveShadows);
}

// A NaN would make the recipe unequal to itself and re-arm the rebuild every
// frame; a dimension below the minimum has no normals; an enumerator outside its
// enum names nothing. The sanitizer resolves all three, reports the dimensions
// it raised, and leaves a clean recipe untouched.
TEST(SplineWallRecipe, SanitizeResolvesPoisonAndReportsWhatItRaised)
{
    const SplineWall clean{};
    EXPECT_TRUE(SanitizeWallRecipe(clean) == clean);
    EXPECT_TRUE(WallRecipeValidation(clean).empty());

    SplineWall poisoned{};
    poisoned.Thickness = std::numeric_limits<float32>::quiet_NaN();
    poisoned.Height = std::numeric_limits<float32>::infinity();
    int32 badGrade = 7;
    std::memcpy(&poisoned.Grade, &badGrade, sizeof(badGrade));
    const SplineWall healed = SanitizeWallRecipe(poisoned);
    EXPECT_TRUE(healed == SanitizeWallRecipe(poisoned)) << "a sanitized recipe equals itself";
    EXPECT_FLOAT_EQ(healed.Thickness, clean.Thickness);
    EXPECT_FLOAT_EQ(healed.Height, clean.Height);
    EXPECT_EQ(healed.Grade, clean.Grade);
    EXPECT_TRUE(WallRecipeValidation(poisoned).empty()) << "a non-finite value is not a dimension";

    SplineWall flat{};
    flat.Thickness = 0.0f;
    flat.Height = 0.01f;
    const SplineWall raised = SanitizeWallRecipe(flat);
    EXPECT_FLOAT_EQ(raised.Thickness, Components::kSplineWallMinExtentMetres);
    EXPECT_FLOAT_EQ(raised.Height, Components::kSplineWallMinExtentMetres);
    const std::vector<std::string> lines = WallRecipeValidation(flat);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find("Thickness"), std::string::npos);
    EXPECT_NE(lines[1].find("Height"), std::string::npos);

    SplineWall huge{};
    huge.Thickness = 1.0e6f;
    huge.Height = 250.0f;
    const SplineWall lowered = SanitizeWallRecipe(huge);
    EXPECT_FLOAT_EQ(lowered.Thickness, Components::kSplineWallMaxExtentMetres);
    EXPECT_FLOAT_EQ(lowered.Height, Components::kSplineWallMaxExtentMetres);
    const std::vector<std::string> above = WallRecipeValidation(huge);
    ASSERT_EQ(above.size(), 2u);
    EXPECT_NE(above[0].find("Thickness is above"), std::string::npos) << above[0];
    EXPECT_NE(above[1].find("Height is above"), std::string::npos) << above[1];

    SplineWall badCorner{};
    badCorner.Corner = SplineWallCorner::Round;
    int32 unknownCorner = 9;
    std::memcpy(&badCorner.Corner, &unknownCorner, sizeof(unknownCorner));
    EXPECT_EQ(SanitizeWallRecipe(badCorner).Corner, SplineWall{}.Corner);
}

// The corner a wall starts with follows the spline it is added to: straight
// segments give Mitre, a curve gives Round. Adding the component through the
// factory (the Add Component menu's path) derives it from the spline's type at
// that moment, and every other field keeps its default.
TEST(SplineWallRecipe, AddingAWallTakesTheCornerFromTheSplinesType)
{
    const bool ownsService = !SplineECS::SplineService::IsInitialized();
    if (ownsService)
        SplineECS::SplineService::Initialize();
    auto& service = SplineECS::SplineService::Get();

    ECS::World world;
    const auto addWall = [&](Spline::SplineType type)
    {
        const SplineECS::SplineHandle handle = service.CreateSpline(type);
        const ECS::EntityHandle entity = world.CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        world.AddComponentImmediate(entity, spline);
        EXPECT_TRUE(ECS::ComponentFactory::Create(world, entity, ECS::GetComponentTypeId<SplineWall>()));
        return std::pair{entity, handle};
    };

    const auto [straight, straightHandle] = addWall(Spline::SplineType::Linear);
    const auto [curved, curvedHandle] = addWall(Spline::SplineType::CatmullRom);
    ASSERT_NE(world.GetComponent<SplineWall>(straight), nullptr);
    ASSERT_NE(world.GetComponent<SplineWall>(curved), nullptr);
    EXPECT_EQ(world.GetComponent<SplineWall>(straight)->Corner, SplineWallCorner::Mitre);
    EXPECT_EQ(world.GetComponent<SplineWall>(curved)->Corner, SplineWallCorner::Round);
    EXPECT_FLOAT_EQ(world.GetComponent<SplineWall>(curved)->Thickness, SplineWall{}.Thickness)
        << "only the corner is derived";

    service.DestroySpline(straightHandle);
    service.DestroySpline(curvedHandle);
    if (ownsService)
        SplineECS::SplineService::Shutdown();
}
