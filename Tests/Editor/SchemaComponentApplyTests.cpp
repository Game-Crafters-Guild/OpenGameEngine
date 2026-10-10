#include <gtest/gtest.h>

#include "DebugServer/SchemaComponentApply.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Scene/SceneSchemaRegistry.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"

#include <nlohmann/json.hpp>

#include <string>

namespace
{
using namespace GameEngine;
using json = nlohmann::json;

constexpr const char* kHeightmapGuid = "f49ad153-cf5c-44e0-9540-e6a8712871fb";

std::string ApplyTerrain(ECS::World& world, ECS::EntityHandle entity, const json& values)
{
    Scene::EnsureTerrainSceneSchemasRegistered();
    const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find("Terrain");
    EXPECT_NE(schema, nullptr);
    return Editor::ApplyComponentViaSchema(world, entity, "Terrain", *schema, values);
}

// set_component renders a JSON string as raw schema text, so the heightmap GUID reaches the schema
// as a bare token. It must bind the heightmap the way the quoted scene-file form does: it used to be
// refused ("terrainAsset must be a GUID string"), so a terrain could not be pointed at its heightmap
// over the debug port at all.
TEST(SchemaComponentApplyTests, TerrainHeightmapBindsFromABareGuid)
{
    ECS::World world;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, Components::Terrain{});

    const std::string refusal = ApplyTerrain(world, e, {{"baseSource", 1}, {"terrainAsset", kHeightmapGuid}});

    EXPECT_TRUE(refusal.empty()) << refusal;
    const auto* terrain = world.GetComponent<Components::Terrain>(e);
    ASSERT_NE(terrain, nullptr);
    EXPECT_EQ(terrain->BaseSource, Components::TerrainBaseSource::HeightmapAsset);
    EXPECT_EQ(terrain->TerrainAssetGuid.ToGuid(), GUID(kHeightmapGuid));
}

// The schema applies one key at a time, in the object's (alphabetical) key order. A refused key
// must not leave the keys before it applied: baseSource landing without its heightmap switched a
// noise terrain to a flat one and reported an error, so the terrain changed on a refused request.
TEST(SchemaComponentApplyTests, ARefusedKeyLeavesTheComponentAsItWas)
{
    ECS::World world;
    const ECS::EntityHandle e = world.CreateEntity();
    Components::Terrain authored{};
    authored.SizeX = 640.0f;
    world.AddComponentImmediate(e, authored);

    const std::string refusal =
        ApplyTerrain(world, e, {{"baseSource", 1}, {"sizeX", 2048.0f}, {"terrainAsset", "not-a-guid"}});

    EXPECT_FALSE(refusal.empty());
    const auto* terrain = world.GetComponent<Components::Terrain>(e);
    ASSERT_NE(terrain, nullptr);
    EXPECT_EQ(terrain->BaseSource, Components::TerrainBaseSource::ProceduralNoise);
    EXPECT_FLOAT_EQ(terrain->SizeX, 640.0f);
    EXPECT_TRUE(terrain->TerrainAssetGuid.IsNull());
}

// get_entity_components reports an unbound reference as "", and its output is meant to be written
// back as it is. The empty string must clear the reference, not be refused as an empty value.
TEST(SchemaComponentApplyTests, AnEmptyStringClearsTheHeightmap)
{
    ECS::World world;
    const ECS::EntityHandle e = world.CreateEntity();
    Components::Terrain bound{};
    bound.TerrainAssetGuid.Set(GUID(kHeightmapGuid));
    world.AddComponentImmediate(e, bound);

    const std::string refusal = ApplyTerrain(world, e, {{"terrainAsset", ""}});

    EXPECT_TRUE(refusal.empty()) << refusal;
    const auto* terrain = world.GetComponent<Components::Terrain>(e);
    ASSERT_NE(terrain, nullptr);
    EXPECT_TRUE(terrain->TerrainAssetGuid.IsNull());
}

// A Terrain key may write another component (the grass keys write TerrainGrass). A refused request
// leaves that one as it was too, so a client retrying after the refusal does not apply it twice.
TEST(SchemaComponentApplyTests, ARefusedKeyLeavesTheComponentsOtherKeysWroteAsTheyWere)
{
    ECS::World world;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, Components::Terrain{});

    const std::string refusal = ApplyTerrain(world, e, {{"grassBladeHeight", 7.5f}, {"terrainAsset", "not-a-guid"}});

    EXPECT_FALSE(refusal.empty());
    EXPECT_EQ(world.GetComponent<Components::TerrainGrass>(e), nullptr)
        << "the refused request left a TerrainGrass it added";
}

} // namespace
