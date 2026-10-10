// Scene-view selection casts through RaycastSceneAll (it needs the whole front-to-back stack
// for click-through), while only RaycastScene carried a terrain branch. Terrain was therefore
// absent from every selection and hover pick regardless of PickOptions::IncludeTerrain, so
// clicking terrain in the scene view selected nothing. These lock both entry points on the
// same rule: IncludeTerrain decides, and the two agree about what the ray hit.

#include <gtest/gtest.h>

#include <vector>

#include "Picking/MeshPickingService.h"
#include "Picking/TerrainPicking.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneTlas.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::EngineCore;
using GameEngine::ApplicationConfig;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Ray3D;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;
using GameEngine::Editor::Picking::PickableKind;
using GameEngine::Editor::Picking::PickHit;
using GameEngine::Editor::Picking::PickOptions;
using GameEngine::Editor::Picking::RaycastScene;
using GameEngine::Editor::Picking::RaycastSceneAll;

namespace Components = GameEngine::Components;

namespace
{
constexpr float kTerrainSize = 64.0f;
constexpr float kHeightScale = 16.0f;

// A world holding one planar terrain centred at the origin, flat at y = 0 (CreateTerrain
// zero-fills the heightfield). The engine is initialized because the picking entry points
// reach AssetManager to attach their BVH-invalidation hook.
class ScenePickTerrainInclusionTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();

        const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
            kTerrainSize, kTerrainSize, kHeightScale, /*samplesPerMeter*/ 1.0f);
        const GameEngine::TerrainECS::TerrainHandle handle =
            TerrainService::Get().CreateTerrain(config);

        Components::Terrain terrain{};
        terrain.SizeX = kTerrainSize;
        terrain.SizeZ = kTerrainSize;
        terrain.HeightScale = kHeightScale;
        terrain.Domain = Components::TerrainDomain::Planar;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;

        m_Terrain = m_World.CreateEntity();
        m_World.AddComponentImmediate(m_Terrain, terrain);
        m_World.AddComponentImmediate(m_Terrain, Components::WorldTransform{});
    }

    void TearDown() override
    {
        GameEngine::Scene::ReleaseSceneTlas(m_World);
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }

    static Ray3D DownAtCentre()
    {
        Ray3D ray;
        ray.origin = Vector3(0.0f, 100.0f, 0.0f);
        ray.direction = Vector3(0.0f, -1.0f, 0.0f);
        return ray;
    }

    World m_World;
    GameEngine::ECS::EntityHandle m_Terrain{};
};
} // namespace

// Harness sanity twin: the fixture's terrain IS hittable through the entry point that always
// had a terrain branch. Without this, the RaycastSceneAll expectation below could fail (or
// pass) for reasons that have nothing to do with the missing branch.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneHitsTheTerrain)
{
    const auto result = RaycastScene(DownAtCentre(), m_World, PickOptions{});
    ASSERT_TRUE(result.Hit);
    EXPECT_EQ(result.Best.Kind, PickableKind::Terrain);
    EXPECT_EQ(result.Best.Entity.id, m_Terrain.id);
    EXPECT_NEAR(result.Best.WorldPosition.y, 0.0f, 0.1f);
}

// The defect: the selection path's entry point returned no hits at all over a terrain.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllHitsTheTerrain)
{
    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, PickOptions{});

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Kind, PickableKind::Terrain);
    EXPECT_EQ(hits[0].Entity.id, m_Terrain.id);
    EXPECT_NEAR(hits[0].WorldPosition.y, 0.0f, 0.1f);
    EXPECT_NEAR(hits[0].Distance, 100.0f, 0.2f);
}

// IncludeTerrain is what decides, so a caller that wants meshes only still gets meshes only.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllHonoursIncludeTerrainFalse)
{
    PickOptions options;
    options.IncludeTerrain = false;

    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, options);
    EXPECT_TRUE(hits.empty());
}

// A terrain-only query is a legitimate query: turning the mesh categories off must not
// short-circuit the cast before terrain is considered.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllServesATerrainOnlyQuery)
{
    PickOptions options;
    options.IncludeMeshes = false;
    options.IncludePrimitives = false;

    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, options);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Kind, PickableKind::Terrain);
}

// The two entry points answer the same question the same way — the nearest hit of the full
// stack is the single best hit.
TEST_F(ScenePickTerrainInclusionTests, BothEntryPointsAgreeOnTheNearestHit)
{
    const auto best = RaycastScene(DownAtCentre(), m_World, PickOptions{});
    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, PickOptions{});

    ASSERT_TRUE(best.Hit);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits.front().Entity.id, best.Best.Entity.id);
    EXPECT_EQ(hits.front().Kind, best.Best.Kind);
    EXPECT_NEAR(hits.front().Distance, best.Best.Distance, 1e-3f);
}

// A ray that leaves the footprint reports nothing, so the terrain branch is not a blanket
// "always one hit".
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllMissesOutsideTheFootprint)
{
    Ray3D ray;
    ray.origin = Vector3(kTerrainSize * 4.0f, 100.0f, 0.0f);
    ray.direction = Vector3(0.0f, -1.0f, 0.0f);

    std::vector<PickHit> hits;
    RaycastSceneAll(ray, m_World, hits, PickOptions{});
    EXPECT_TRUE(hits.empty());
}
