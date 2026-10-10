// The editor's terrain creation (create menu, create_terrain IPC) over the engine's
// TerrainECS::ProvisionTerrainEntity: the default surface-rules volume it spawns lands at the
// bottom of the hierarchy, and a refusal reaches the caller as text.

#include <gtest/gtest.h>

#include "Terrain/TerrainEntityProvisioning.h"

#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <string>

namespace
{

using namespace GameEngine;

TEST(EditorTerrainProvisioning, TheDefaultSurfaceRulesVolumeLandsBelowEverythingInTheScene)
{
    ECS::World world;
    const ECS::EntityHandle existing = world.CreateEntity();
    world.AddComponentImmediate(existing, Components::HierarchyOrder{7});

    const ECS::EntityHandle terrain = world.CreateEntity();
    const auto preset = CBTTerrainECS::MakeTerrainPreset(CBTTerrainECS::TerrainPreset::LargePlanar);
    EXPECT_EQ(Editor::ProvisionTerrainEntity(world, terrain, preset, Components::TerrainPlanetRelief{}), "");
    EXPECT_NE(world.GetComponent<Components::TerrainGrass>(terrain), nullptr);

    int volumes = 0;
    world.Query<ECS::Read<Components::TerrainSurfaceRulesEffect>>().Each(
        [&](ECS::EntityHandle volume, const Components::TerrainSurfaceRulesEffect&) {
            ++volumes;
            const auto* order = world.GetComponent<Components::HierarchyOrder>(volume);
            ASSERT_NE(order, nullptr) << "the volume takes a position in the hierarchy";
            EXPECT_GT(order->order, 7);
            EXPECT_EQ(world.GetComponent<Components::Name>(volume)->View(), "Terrain Surface Rules");
        });
    EXPECT_EQ(volumes, 1);
}

TEST(EditorTerrainProvisioning, ARefusedTerrainReturnsTheReasonAndAddsNothing)
{
    ECS::World world;
    const ECS::EntityHandle terrain = world.CreateEntity();
    auto config = CBTTerrainECS::MakeTerrainPreset(CBTTerrainECS::TerrainPreset::LargePlanar);
    config.SizeX = 1e30f;
    const std::string refusal = Editor::ProvisionTerrainEntity(world, terrain, config, Components::TerrainPlanetRelief{});
    EXPECT_NE(refusal.find("on each side"), std::string::npos) << refusal;
    EXPECT_EQ(world.GetComponent<Components::Terrain>(terrain), nullptr);
}

} // namespace
