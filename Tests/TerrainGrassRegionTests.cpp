#include <gtest/gtest.h>

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <filesystem>
#include <chrono>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

class TerrainGrassRegions : public testing::Test
{
protected:
    ECS::World World;
    TerrainModifierSystem System;
    TerrainHandle Handle{};
    ECS::EntityHandle TerrainEntity{};
    TerrainData* Data = nullptr;

    void SetUp() override
    {
        if (TerrainService::IsInitialized()) TerrainService::Shutdown();
        if (SplineECS::SplineService::IsInitialized()) SplineECS::SplineService::Shutdown();
        TerrainService::Initialize();
        SplineECS::SplineService::Initialize();
        EnableTerrainModifierLifecycleEvents(World);
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = config.HeightmapHeight = 65;
        config.WorldSizeX = config.WorldSizeZ = 128;
        config.HeightScale = 32;
        config.LODLevels = 3;
        Handle = TerrainService::Get().CreateTerrain(config);
        Data = TerrainService::Get().GetTerrainData(Handle);
        ASSERT_NE(Data, nullptr);
        const auto entity = World.CreateEntity();
        TerrainEntity = entity;
        Components::Terrain terrain{};
        terrain.SizeX = terrain.SizeZ = 128;
        terrain.HeightScale = 32;
        terrain.BaseSource = Components::TerrainBaseSource::Flat;
        terrain.TerrainDataHandle = Handle.Index;
        terrain.TerrainDataGeneration = Handle.Generation;
        World.AddComponentImmediate<Components::Terrain>(entity, terrain);
        World.AddComponentImmediate<Components::WorldTransform>(entity, {});
        Tick(); // establish the ordinary ground bake before measuring cosmetic edits
    }

    void TearDown() override
    {
        World.Clear();
        SplineECS::SplineService::Shutdown();
        TerrainService::Shutdown();
    }

    void Tick()
    {
        World.SwapLifecycleEvents();
        System.Update(World, 1.0f / 60.0f);
    }

    ECS::EntityHandle Region(Components::TerrainVolumeShape shape, float32 height,
                             float32 density, float32 x = 0, float32 priority = 0)
    {
        const auto entity = World.CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = shape;
        volume.Radius = volume.RectHalfX = volume.RectHalfZ = 12;
        volume.Falloff = 4;
        volume.Priority = priority;
        World.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);
        Components::TerrainGrassEffect effect{};
        effect.HeightScale = height; effect.DensityScale = density;
        World.AddComponentImmediate<Components::TerrainGrassEffect>(entity, effect);
        Components::WorldTransform transform{};
        transform.matrix[12] = x;
        World.AddComponentImmediate<Components::WorldTransform>(entity, transform);
        return entity;
    }

    ECS::EntityHandle GroundRegion(float32 height, float32 priority)
    {
        const auto entity = World.CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Global;
        volume.Priority = priority;
        World.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);
        Components::TerrainFlattenEffect effect{};
        effect.UseVolumeHeight = false;
        effect.TargetHeight = height;
        World.AddComponentImmediate<Components::TerrainFlattenEffect>(entity, effect);
        World.AddComponentImmediate<Components::WorldTransform>(entity, {});
        return entity;
    }

    uint8 At(float32 x, float32 z, uint32 channel = 0) const
    {
        if (!Data->GrassField.IsActive()) return 255;
        const auto ix = static_cast<uint32>((x + 64) / 2);
        const auto iz = static_cast<uint32>((z + 64) / 2);
        return Data->GrassField.Pixels[(iz * 65 + ix) * 2 + channel];
    }

    SplineECS::SplineHandle AddClosedSpline(ECS::EntityHandle entity)
    {
        auto& service = SplineECS::SplineService::Get();
        const auto spline = service.CreateSpline(Spline::SplineType::CatmullRom, true);
        for (const auto& p : std::array<Mathematics::Vector3, 4>{{{-24,0,-24},{24,0,-24},{24,0,24},{-24,0,24}}})
            service.GetSplineData(spline)->AddPoint(p, 3);
        service.RebuildCache(spline);
        Components::SplineComponent component{};
        component.SplineDataIndex = spline.Index();
        component.SplineDataGeneration = spline.Generation();
        World.AddComponentImmediate<Components::SplineComponent>(entity, component);
        return spline;
    }
};

TEST_F(TerrainGrassRegions, DefaultAndRepeatedNeutralEffectsAllocateNothing)
{
    Region(Components::TerrainVolumeShape::Global, 1, 1);
    Tick();
    EXPECT_TRUE(Data->GrassField.Initialized);
    EXPECT_TRUE(Data->GrassField.Pixels.empty());
    EXPECT_EQ(Data->GrassField.Pixels.capacity(), 0u);
    const auto version = Data->GrassField.Version;
    for (int frame = 0; frame < 4; ++frame) Tick();
    EXPECT_EQ(Data->GrassField.Version, version);
    EXPECT_FALSE(Data->GrassField.Dirty);
}

TEST_F(TerrainGrassRegions, OrderedTargetLerpLetsLaterNeutralRestoreBaseline)
{
    Region(Components::TerrainVolumeShape::Global, 0, 0.25f);
    const auto restoring = Region(Components::TerrainVolumeShape::Global, 1, 1, 0, 10);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(restoring)->Weight = 0.5f;
    Tick();
    EXPECT_EQ(At(0, 0), 128);
    EXPECT_EQ(At(0, 0, 1), 159);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(restoring)->Weight = 1;
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    World.GetComponentForWrite<Components::TerrainModifierVolume>(restoring)->Priority = -10;
    Tick();
    EXPECT_EQ(At(0, 0), 0);
    EXPECT_EQ(At(0, 0, 1), 64);
}

TEST_F(TerrainGrassRegions, MovedRegionRestoresOldBoundsAndIdleRetainsVersion)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0.2f, 0.4f, -30);
    Tick();
    EXPECT_EQ(At(-30, 0), 51);
    EXPECT_EQ(At(30, 0), 255);
    Data->GrassField.Dirty = false; // extraction consumed this version
    World.GetComponentForWrite<Components::WorldTransform>(region)->matrix[12] = 30;
    Tick();
    EXPECT_EQ(At(-30, 0), 255);
    EXPECT_EQ(At(30, 0), 51);
    EXPECT_GT(Data->GrassField.DirtyMinX, 0u);
    EXPECT_LT(Data->GrassField.DirtyMaxX, 65u);
    EXPECT_TRUE(Data->GrassField.Dirty);
    Data->GrassField.Dirty = false;
    const auto version = Data->GrassField.Version;
    Tick(); Tick();
    EXPECT_EQ(Data->GrassField.Version, version);
    EXPECT_FALSE(Data->GrassField.Dirty);
}

TEST_F(TerrainGrassRegions, DisableAndRemovalRestoreNeutralWithoutGroundDirtying)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    const auto groundVersion = Data->HeightfieldVersion;
    Data->SplatmapDirty = false;
    World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->Enabled = false;
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    EXPECT_FALSE(Data->SplatmapDirty);
    World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->Enabled = true;
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive());
    ECS::Entity(&World, region).SetEnabled<Components::TerrainModifierVolume>(false);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    ECS::Entity(&World, region).SetEnabled<Components::TerrainModifierVolume>(true);
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive());
    World.RemoveComponentImmediate<Components::TerrainGrassEffect>(region);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    EXPECT_FALSE(Data->SplatmapDirty);
}

TEST_F(TerrainGrassRegions, GrassEditOnMixedVolumePreservesHeightSplatAndNormals)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0.5f, 0.75f);
    Components::TerrainHeightOffsetEffect offset{};
    offset.Offset = 3;
    World.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(region, offset);
    Tick();
    const auto groundVersion = Data->HeightfieldVersion;
    const std::vector<float32> heights(Data->Heightfield.GetRawSamples(),
        Data->Heightfield.GetRawSamples() + Data->Heightfield.GetSampleCount());
    const auto splat = Data->Splatmap;
    const auto normals = Data->Normalmap;
    Data->SplatmapDirty = false;
    auto* effect = World.GetComponentForWrite<Components::TerrainGrassEffect>(region);
    effect->HeightScale = 0.1f;
    effect->DensityScale = 0.2f;
    effect->StackOrder = 12;
    Tick();
    EXPECT_EQ(At(0,0), 26);
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    EXPECT_TRUE(std::equal(heights.begin(), heights.end(), Data->Heightfield.GetRawSamples()));
    EXPECT_EQ(Data->Splatmap, splat);
    EXPECT_EQ(Data->Normalmap, normals);
    EXPECT_FALSE(Data->SplatmapDirty);
}

TEST_F(TerrainGrassRegions, StructuralGrassEditsPreserveExplicitGroundPriorityOrder)
{
    const auto a = GroundRegion(2, 0);
    const auto b = GroundRegion(6, 10);
    Tick();
    const auto groundVersion = Data->HeightfieldVersion;
    const std::vector<float32> heights(Data->Heightfield.GetRawSamples(),
        Data->Heightfield.GetRawSamples() + Data->Heightfield.GetSampleCount());
    const auto splat = Data->Splatmap;
    const auto normals = Data->Normalmap;
    Components::TerrainGrassEffect grass{};
    grass.HeightScale = .2f;
    for (const auto entity : {a, b})
    {
        World.AddComponentImmediate<Components::TerrainGrassEffect>(entity, grass);
        Tick();
        EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    }
    for (const auto entity : {b, a})
    {
        World.RemoveComponentImmediate<Components::TerrainGrassEffect>(entity);
        Tick();
        EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    }
    EXPECT_TRUE(std::equal(heights.begin(), heights.end(), Data->Heightfield.GetRawSamples()));
    EXPECT_EQ(Data->Splatmap, splat);
    EXPECT_EQ(Data->Normalmap, normals);
}

TEST_F(TerrainGrassRegions, StructuralGrassPresenceRetainsExistingEqualPriorityGroundTieSemantics)
{
    // Moving the first of three entities also swap-removes the last one from
    // its old archetype, making the tie-order change observable.
    const auto a = GroundRegion(2, 0);
    GroundRegion(4, 0);
    GroundRegion(6, 0);
    Tick();
    const auto before = Data->Heightfield.GetRawSamples()[0];
    World.AddComponentImmediate<Components::TerrainGrassEffect>(a, {});
    Tick();
    const auto afterAdd = Data->Heightfield.GetRawSamples()[0];
    ASSERT_NE(before, afterAdd) << "fixture must actually change the equal-priority archetype gather order";
    World.RemoveComponentImmediate<Components::TerrainGrassEffect>(a);
    Tick();
    EXPECT_NE(Data->Heightfield.GetRawSamples()[0], afterAdd)
        << "removal must also exercise the existing structural tie order";
    // Presence is a structural input to existing ground tie ordering. Consumers
    // may ignore grass scalar edits, but cannot omit this presence dependency.
}

TEST_F(TerrainGrassRegions, CircleRectangleAndBothSplineShapesUseExistingFootprints)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    EXPECT_EQ(At(0,0), 0);
    EXPECT_EQ(At(12,12), 255); // outside radius + feather
    World.GetComponentForWrite<Components::TerrainModifierVolume>(region)->Shape = Components::TerrainVolumeShape::Rectangle;
    Tick();
    EXPECT_LT(At(10,10), 255);
    AddClosedSpline(region);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(region)->Shape = Components::TerrainVolumeShape::SplineArea;
    Tick();
    EXPECT_EQ(At(0,0), 0);
    EXPECT_EQ(At(60,60), 255);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(region)->Shape = Components::TerrainVolumeShape::SplinePath;
    Tick();
    EXPECT_EQ(At(0,0), 255);
    EXPECT_GT(Data->GrassField.NonNeutralTexels, 0u);
}

TEST_F(TerrainGrassRegions, SplinePointEditRestoresOldAreaWithoutChangingGround)
{
    const auto region = Region(Components::TerrainVolumeShape::SplineArea, 0, 0);
    const auto handle = AddClosedSpline(region);
    Tick();
    ASSERT_EQ(At(0, 0), 0);
    const auto groundVersion = Data->HeightfieldVersion;
    const auto grassVersion = Data->GrassField.Version;
    auto& service = SplineECS::SplineService::Get();
    auto* spline = service.GetSplineData(handle);
    ASSERT_NE(spline, nullptr);
    for (uint32 i = 0; i < spline->Points.size(); ++i)
    {
        auto point = spline->Points[i].Position;
        point.x += 100;
        service.GetSplineData(handle)->SetPointPosition(i, point);
    }
    service.RebuildCache(handle);
    Tick();
    EXPECT_EQ(At(0, 0), 255);
    EXPECT_GT(Data->GrassField.Version, grassVersion);
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
}

TEST_F(TerrainGrassRegions, DisabledTerrainReentryClearsRegionRemovedWhileSuspended)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive());
    World.AddComponentImmediate<ECS::Disabled>(TerrainEntity, {});
    Tick();
    World.DestroyEntityImmediate(region);
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive()); // excluded owner retains its cache
    World.RemoveComponentImmediate<ECS::Disabled>(TerrainEntity);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
}

TEST_F(TerrainGrassRegions, UnrelatedRemovalReconcilesEqualPriorityStorageReorder)
{
    const auto far = Region(Components::TerrainVolumeShape::Circle, .7f, .7f, 500);
    Region(Components::TerrainVolumeShape::Circle, .2f, .2f);
    Region(Components::TerrainVolumeShape::Circle, .8f, .8f);
    Tick();
    const auto original = Data->GrassField.Pixels;
    World.DestroyEntityImmediate(far); // swap-remove can reorder the two survivors
    Tick();
    const auto incremental = Data->GrassField.Pixels;
    ASSERT_NE(original, incremental) << "fixture must actually reorder the overlapping survivors";
    TerrainModifierSystem fresh;
    fresh.Update(World, 1.0f/60.0f);
    EXPECT_EQ(Data->GrassField.Pixels, incremental);
}

TEST_F(TerrainGrassRegions, PlanetDoesNotAllocateRegionalGrass)
{
    World.GetComponentForWrite<Components::Terrain>(TerrainEntity)->Domain = Components::TerrainDomain::Spherical;
    Region(Components::TerrainVolumeShape::Global, 0, 0);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
}

TEST_F(TerrainGrassRegions, TiledMoveUsesCurrentAuthoredOriginBeforeExtraction)
{
    auto& service = TerrainService::Get();
    TiledTerrainConfig config{};
    config.WorldSizeX = config.WorldSizeZ = 128;
    config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    auto* tiled = service.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    const auto entity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = terrain.SizeZ = 128;
    terrain.TiledTerrainHandle = handle.Index; terrain.TiledTerrainGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(entity, terrain);
    World.AddComponentImmediate<Components::WorldTransform>(entity, {});
    Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    ASSERT_TRUE(tiled->GrassUnifiedField.IsActive());
    World.GetComponentForWrite<Components::WorldTransform>(entity)->matrix[12] = 256;
    // Do not pre-update service.WorldOrigin: actual extraction runs after modifiers.
    Tick();
    EXPECT_FALSE(tiled->GrassUnifiedField.IsActive());
}

TEST_F(TerrainGrassRegions, NonMultipleTiledSizeUsesTheRenderCoverageLattice)
{
    auto& service = TerrainService::Get();
    TiledTerrainConfig config{};
    config.WorldSizeX = 1500;
    config.WorldSizeZ = 1200;
    config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    auto* tiled = service.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    const auto entity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = 1500; terrain.SizeZ = 1200;
    terrain.TiledTerrainHandle = handle.Index; terrain.TiledTerrainGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(entity, terrain);
    World.AddComponentImmediate<Components::WorldTransform>(entity, {});
    Region(Components::TerrainVolumeShape::Circle, 0, .4f, 250);
    Tick();
    const auto extent = ComputeTiledRenderExtent(tiled->Config);
    ASSERT_EQ(extent.WorldSizeX, 2048);
    ASSERT_EQ(extent.WorldSizeZ, 2048);
    const auto& field = tiled->GrassUnifiedField;
    ASSERT_TRUE(field.IsActive());
    ASSERT_EQ(field.Width, 2049u);
    ASSERT_EQ(field.Height, 2049u);
    // Renderer origin is the requested centre minus half requested size;
    // subsequent UV spacing covers the complete tile lattice at one metre.
    const size_t centre = (600u * field.Width + 1000u) * 2;
    EXPECT_EQ(field.Pixels[centre], 0);
    EXPECT_EQ(field.Pixels[centre + 1], 102);
    EXPECT_EQ(field.Pixels[(600u * field.Width + 750u) * 2], 255);
}

TEST_F(TerrainGrassRegions, SchemaRoundtripKeepsBoundedTargetsAndRefusesNonfinite)
{
    Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = Scene::SceneSchemaRegistry::Find("TerrainGrassEffect");
    ASSERT_NE(schema, nullptr);
    const auto entity = World.CreateEntity();
    Scene::SceneLoadContext context{};
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "heightscale", "0.25", &error)) << error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "densityscale", "2", &error)) << error;
    EXPECT_FLOAT_EQ(World.GetComponent<Components::TerrainGrassEffect>(entity)->HeightScale, .25f);
    EXPECT_FLOAT_EQ(World.GetComponent<Components::TerrainGrassEffect>(entity)->DensityScale, 1);
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "heightscale", "nan", &error));
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "densityscale", "inf", &error));
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "unknown", "1", &error));
    EXPECT_FLOAT_EQ(Components::ClampTerrainGrassEffectScale(std::numeric_limits<float32>::quiet_NaN()), 1);
    EXPECT_FLOAT_EQ(Components::ClampTerrainGrassEffectScale(-1), 0);
    ECS::World authored;
    const auto source = authored.CreateEntity();
    authored.AddComponentImmediate<Components::TerrainGrassEffect>(source,
        *World.GetComponent<Components::TerrainGrassEffect>(entity));
    authored.AddComponentImmediate<Components::TerrainModifierVolume>(source, {});
    const auto file = std::filesystem::temp_directory_path() / ("GrassRegionScene-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(authored, file)) << Scene::GetLastSceneIOError().message;
    ECS::World loaded;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options; options.outDegradation = &degradation;
    const bool success = Scene::LoadSceneFromFile(loaded, file, options);
    std::error_code ignored; std::filesystem::remove(file, ignored);
    ASSERT_TRUE(success) << Scene::GetLastSceneIOError().message;
    EXPECT_FALSE(degradation.IsDegraded());
    uint32 found = 0;
    loaded.Query<ECS::Read<Components::TerrainGrassEffect>>()
        .Each([&](const Components::TerrainGrassEffect& effect) {
            ++found;
            EXPECT_FLOAT_EQ(effect.HeightScale, .25f);
            EXPECT_FLOAT_EQ(effect.DensityScale, 1);
        });
    EXPECT_EQ(found, 1u);
}

TEST_F(TerrainGrassRegions, TiledUnifiedComposesNeverStreamedGroundAndAtlasCoarseIsAnalytic)
{
    auto& service = TerrainService::Get();
    const auto region = Region(Components::TerrainVolumeShape::Global, .2f, .4f);
    for (const float32 sizeX : {2048.0f, 9216.0f})
    {
        TiledTerrainConfig config{};
        config.WorldSizeX = sizeX; config.WorldSizeZ = 1024;
        config.HeightScale = 32; config.SamplesPerMeter = 1;
        const auto handle = service.CreateTiledTerrain(config);
        auto* tiled = service.GetTiledTerrainData(handle);
        ASSERT_NE(tiled, nullptr);
        ASSERT_TRUE(tiled->Tiles.empty());
        const auto entity = World.CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = sizeX; terrain.SizeZ = 1024; terrain.HeightScale = 32;
        terrain.TiledTerrainHandle = handle.Index; terrain.TiledTerrainGeneration = handle.Generation;
        World.AddComponentImmediate<Components::Terrain>(entity, terrain);
        World.AddComponentImmediate<Components::WorldTransform>(entity, {});
        Tick();
        const auto& field = sizeX < 8193 ? tiled->GrassUnifiedField : tiled->GrassCoarseField;
        ASSERT_TRUE(field.IsActive());
        EXPECT_EQ(field.Pixels.front(), 51);
        EXPECT_EQ(field.Pixels.back(), 102);
        for (size_t i = 0; i < field.Pixels.size(); i += 2)
        {
            ASSERT_EQ(field.Pixels[i], 51);
            ASSERT_EQ(field.Pixels[i+1], 102);
        }
        EXPECT_TRUE(tiled->Tiles.empty());
        if (sizeX > 8193)
        {
            auto* tile = service.LoadTile(handle, {3,0});
            ASSERT_NE(tile, nullptr);
            tile->LodState = TileLodState::Full;
            Tick();
            ASSERT_TRUE(tile->GrassField.IsActive());
            EXPECT_EQ(tile->GrassField.Pixels.front(), 51);
            const auto version = tile->HeightfieldVersion;
            World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->HeightScale = .6f;
            Tick();
            EXPECT_EQ(tile->GrassField.Pixels.front(), 153);
            EXPECT_EQ(tile->HeightfieldVersion, version);
            auto* late = service.LoadTile(handle, {6,0});
            ASSERT_NE(late, nullptr);
            late->LodState = TileLodState::Full;
            Tick();
            ASSERT_TRUE(late->GrassField.IsActive());
            EXPECT_EQ(late->GrassField.Pixels.front(), 153)
                << "a previously nonresident tile must receive edits made before it arrived";
            EXPECT_EQ(tile->GrassField.Pixels.front(), 153);
        }
        World.DestroyEntityImmediate(entity);
    }
}

TEST(TerrainGrassField, TwoChannelAtlasPackingPreservesSharedEdgesAndApron)
{
    const auto geometry = MakeAtlasGeometry(5, 4, 2, 2);
    std::vector<uint8> samples(5 * 5 * 2);
    for (uint32 z = 0; z < 5; ++z)
        for (uint32 x = 0; x < 5; ++x)
        {
            samples[(z*5+x)*2] = static_cast<uint8>(x*30);
            samples[(z*5+x)*2+1] = static_cast<uint8>(z*40);
        }
    std::vector<uint8> full(geometry.AtlasDim * geometry.AtlasDim * 2), patch;
    PackTileBytesIntoSlot(full.data(), geometry, 0, samples.data(), 2);
    samples[0] = 99;
    const auto rect = PackTileEditRegionIntoSlot(patch, samples.data(), 2, geometry, 0, 0, 0, 0, 0);
    EXPECT_EQ(rect.DstTexelX, 0u);
    EXPECT_EQ(rect.DstTexelY, 0u);
    EXPECT_EQ(rect.Width, 2u);
    EXPECT_EQ(rect.Height, 2u);
    for (size_t i = 0; i < patch.size(); i += 2) EXPECT_EQ(patch[i], 99);
    const auto plainSlots = DeriveAtlasSlotCount(32, 32, 1025, kAtlasVramBudgetBytes);
    const auto grassSlots = DeriveAtlasSlotCount(32, 32, 1025, kAtlasVramBudgetBytes, 14);
    EXPECT_LE(grassSlots, plainSlots);
    const auto active = MakeAtlasGeometry(1025, grassSlots, 32, 32);
    EXPECT_LE(static_cast<uint64>(active.AtlasDim) * active.AtlasDim * 14, kAtlasVramBudgetBytes);
}
} // namespace
