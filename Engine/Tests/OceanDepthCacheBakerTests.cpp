#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/OceanDepthCacheBaker.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/PlanarHeightQuery.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace GameEngine;
namespace
{
class OceanSceneTerrainBake : public testing::Test
{
protected:
    ECS::World World;

    static void SetUpTestSuite()
    {
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = (std::filesystem::temp_directory_path() /
            ("ocean-terrain-bake-" + GUID::Generate().ToString())).string();
        std::filesystem::create_directories(config.WorkspaceDirectory);
        ASSERT_TRUE(engine.Initialize(config));
    }

    static void TearDownTestSuite() { EngineCore::GetInstance().Shutdown(); }

    void SetUp() override
    {
        if (!TerrainECS::TerrainService::TryGet())
            TerrainECS::TerrainService::Initialize();
        if (!SplineECS::SplineService::TryGet())
            SplineECS::SplineService::Initialize();
    }

    void TearDown() override
    {
        World.Clear();
        TerrainECS::TerrainService::Shutdown();
        SplineECS::SplineService::Shutdown();
    }

    static Components::WorldTransform At(float x, float y, float z)
    {
        Components::WorldTransform transform;
        transform.matrix[12] = x;
        transform.matrix[13] = y;
        transform.matrix[14] = z;
        return transform;
    }

    TerrainECS::TerrainData* AddTerrain(float sizeX, float sizeZ, float x = 0,
                                      float y = 0, float z = 0)
    {
        Terrain::TerrainConfig config;
        config.HeightmapWidth = config.HeightmapHeight = 65;
        config.WorldSizeX = sizeX;
        config.WorldSizeZ = sizeZ;
        config.HeightScale = 20;
        config.LODLevels = 4;
        auto& service = TerrainECS::TerrainService::Get();
        const auto handle = service.CreateTerrain(config);
        Components::Terrain terrain;
        terrain.SizeX = sizeX;
        terrain.SizeZ = sizeZ;
        terrain.HeightScale = config.HeightScale;
        terrain.BaseSource = Components::TerrainBaseSource::Flat;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;
        auto entity = World.CreateEntity();
        World.AddComponentImmediate(entity, terrain);
        World.AddComponentImmediate(entity, At(x, y, z));
        return service.GetTerrainData(handle);
    }

    Ocean::OceanDepthCacheAsset Bake(const Ocean::OceanDepthCacheBakeDesc& desc,
                                     uint32 expectedSources = 1)
    {
        Ocean::OceanDepthCacheAsset cache;
        Engine::Renderer::OceanSceneDepthBakeStats stats;
        Engine::Renderer::OceanSceneDepthBakeOptions options;
        std::string error;
        EXPECT_TRUE(Engine::Renderer::BakeOceanDepthCacheFromSceneMeshes(
            World, desc, options, cache, &stats, &error)) << error;
        EXPECT_EQ(stats.TerrainSources, expectedSources);
        EXPECT_EQ(stats.MeshSources, 0u);
        EXPECT_EQ(stats.SkippedTerrain, 0u);
        return cache;
    }
};

TEST_F(OceanSceneTerrainBake, CenteredComposedTerrainCoversNegativeWorldCoordinates)
{
    auto* data = AddTerrain(576, 576);
    ASSERT_NE(data, nullptr);
    TerrainECS::FillHeightfieldBaseRegion(data->Heightfield, Components::TerrainBaseSource::Flat,
                                         nullptr, 0, 0, 64, 64);
    data->MarkFullDirty();

    // Compose an ordinary global floor and a lower basin in the southwest using
    // the actual modifier owner, rather than supplying an already-correct bake source.
    Components::TerrainModifierVolume global;
    global.Shape = Components::TerrainVolumeShape::Global;
    Components::TerrainFlattenEffect floor;
    floor.UseVolumeHeight = false;
    floor.TargetHeight = 4;
    floor.Blend = Components::TerrainModifierBlend::Set;
    auto base = World.CreateEntity();
    World.AddComponentImmediate(base, global);
    World.AddComponentImmediate(base, floor);
    World.AddComponentImmediate(base, At(0, 0, 0));

    Components::TerrainModifierVolume circle;
    circle.Radius = 36;
    circle.Falloff = 0;
    circle.Priority = 1;
    floor.TargetHeight = 1;
    auto basin = World.CreateEntity();
    World.AddComponentImmediate(basin, circle);
    World.AddComponentImmediate(basin, floor);
    World.AddComponentImmediate(basin, At(-120, 0, -160));
    TerrainECS::TerrainModifierSystem modifiers;
    modifiers.Update(World, 1.f / 60);

    const auto query = TerrainECS::ResolvePlanarHeightQuery(World);
    float height = 0;
    ASSERT_TRUE(query.SampleHeight(-120, -160, height));
    ASSERT_NEAR(height, 1, 1e-4f);
    ASSERT_TRUE(query.SampleHeight(120, 160, height));
    ASSERT_NEAR(height, 4, 1e-4f);

    // The affected scene's real cache rectangle lies wholly inside [-288,288]^2.
    Ocean::OceanDepthCacheBakeDesc desc{1024, 1024, -224, -272, 432, 496, 10, 60000};
    const auto cache = Bake(desc);
    ASSERT_TRUE(cache.IsValid());
    const auto& depths = cache.GetDepths();
    EXPECT_EQ(std::count(depths.begin(), depths.end(), desc.DeepWaterDepth), 0);
    EXPECT_NEAR(*std::min_element(depths.begin(), depths.end()), 6, 1e-4f);
    EXPECT_NEAR(*std::max_element(depths.begin(), depths.end()), 9, 1e-4f);
    float depth = 0;
    ASSERT_TRUE(cache.SampleDepth(-120, -160, depth));
    EXPECT_NEAR(depth, 9, 1e-4f);
    ASSERT_TRUE(cache.SampleDepth(120, 160, depth));
    EXPECT_NEAR(depth, 6, 1e-4f);
}

TEST_F(OceanSceneTerrainBake, TranslatedRectangularTerrainPreservesHeightfieldCoordinates)
{
    auto* data = AddTerrain(96, 64, -32, 7, 11);
    ASSERT_NE(data, nullptr);
    for (uint32 z = 0; z < 65; ++z)
        for (uint32 x = 0; x < 65; ++x)
            data->Heightfield.SetSample(x, z, .1f + .2f * x / 64 + .35f * z / 64);

    // Include a border outside every side; it must remain the no-source sentinel.
    Ocean::OceanDepthCacheBakeDesc desc{100, 68, -82, -23, 100, 68, 32, 60000};
    const auto cache = Bake(desc);
    ASSERT_TRUE(cache.IsValid());
    unsigned mismatch = 0;
    for (uint32 z = 0; z < desc.Height; ++z)
        for (uint32 x = 0; x < desc.Width; ++x)
        {
            const float wx = desc.OriginX + (x + .5f) * desc.SizeX / desc.Width;
            const float wz = desc.OriginZ + (z + .5f) * desc.SizeZ / desc.Height;
            const bool inside = wx >= -80 && wx <= 16 && wz >= -21 && wz <= 43;
            const float expected = inside ? 32 - (7 + 20 *
                (.1f + .2f * (wx + 80) / 96 + .35f * (wz + 21) / 64)) : desc.DeepWaterDepth;
            mismatch += std::abs(cache.GetDepths()[z * desc.Width + x] - expected) > 1e-4f;
        }
    EXPECT_EQ(mismatch, 0u);
}

TEST_F(OceanSceneTerrainBake, TiledTerrainKeepsResidentTileCornerOrigins)
{
    auto& service = TerrainECS::TerrainService::Get();
    TerrainECS::TiledTerrainConfig config;
    config.WorldSizeX = config.WorldSizeZ = 576;
    config.HeightScale = 20;
    config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    auto* tiled = service.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    tiled->WorldOriginX = -300;
    tiled->WorldOriginZ = -160;
    auto* tile = service.LoadTile(handle, {0, 0});
    ASSERT_NE(tile, nullptr);
    for (uint32 z = 0; z < tile->Heightfield.GetHeight(); ++z)
        for (uint32 x = 0; x < tile->Heightfield.GetWidth(); ++x)
            tile->Heightfield.SetSample(x, z, .25f);
    Components::Terrain terrain;
    terrain.SizeX = terrain.SizeZ = 576;
    terrain.HeightScale = 20;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    auto entity = World.CreateEntity();
    World.AddComponentImmediate(entity, terrain);
    // Tile world corners are already resolved; this translation contributes only Y.
    World.AddComponentImmediate(entity, At(100, 3, 200));
    const float side = tiled->Config.TileWorldSize;
    Ocean::OceanDepthCacheBakeDesc desc{16, 16, -300, -160, side, side, 12, 60000};
    const auto cache = Bake(desc);
    ASSERT_TRUE(cache.IsValid());
    for (const float depth : cache.GetDepths())
        EXPECT_NEAR(depth, 4, 1e-4f);
}
} // namespace

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
