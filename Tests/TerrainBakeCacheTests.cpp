// The single terrain's baked-terrain cache (TerrainBakeCache.h): a hit is the
// fresh bake byte for byte, every input class moves the key, one terrain owns one
// artifact, a package stages only its scenes' artifacts, a malformed file is a
// miss, and the key and the bake output are pinned so a change to either needs
// kTerrainBakeFormatVersion.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainBakeCache.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZonePayload.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;
namespace fs = std::filesystem;

constexpr uint32 kDim = 129;
constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;

// Every input class of a single terrain's bake, at values a test changes one at a time.
struct SceneInputs
{
    float32 TerrainX = 0.0f;
    float32 WorldSize = kWorldSize;
    float32 HeightmapAmplitude = 1.0f;
    float32 FlattenTarget = 20.0f;
    float32 FlattenX = 30.0f;
    float32 SplineX = -100.0f;
    float32 MaskScale = 1.0f;
    float32 PayloadScale = 30.0f;
};

struct BakeResult
{
    std::vector<float32> Heights;
    std::vector<uint8> Splat;
    float32 SplatMinH = 0.0f;
    float32 SplatMaxH = 0.0f;
};

// Asset identities never reach the key (it hashes content), so any GUIDs serve.
const GUID kHeightmapGuid = GUID::Generate();
const GUID kMaskGuid = GUID::Generate();
const GUID kPayloadGuid = GUID::Generate();
const GUID kSceneGuid = GUID::Generate();
constexpr const char* kTerrainTag = "terrain";

// The scene identity a loaded terrain carries: its scene's GUID on the component and
// its scene tag on the entity.
void GiveSceneIdentity(ECS::World& world, ECS::EntityHandle terrainEntity)
{
    Components::SceneEntityTag tag{};
    std::strncpy(tag.value, kTerrainTag, sizeof(tag.value));
    world.AddComponentImmediate<Components::SceneEntityTag>(terrainEntity, tag);
    world.GetComponentForWrite<Components::Terrain>(terrainEntity)->BakeOriginScene = kSceneGuid;
}

struct TempDirectory
{
    fs::path Path = fs::temp_directory_path() / ("terrain-bake-" + GUID::Generate().ToString());
    ~TempDirectory()
    {
        std::error_code error;
        fs::remove_all(Path, error);
    }
};

// One open of a scene that carries every input class: a heightmap base, a flatten,
// a noise, a masked stamp, a paint layer, surface rules, a spline-path flatten and a
// sculpt zone, through the system's first (full) bake with `cache` configured. The
// session keeps its world, so a test can edit, update and save it.
struct BakeSession
{
    ECS::World world;
    TerrainModifierSystem system;
    TerrainData* data = nullptr;
    ECS::EntityHandle terrainEntity{};
    ECS::EntityHandle flattenEntity{};

    BakeSession(const SceneInputs& in, const TerrainBakeCacheConfig& cache, bool sceneIdentity = true,
                JobSystem::WorkStealingThreadPool* pool = nullptr)
        : world(pool)
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
        auto& svc = TerrainService::Get();
        svc.SetBakeCache(cache);

        Terrain::HeightfieldData heightmap(33, 33);
        heightmap.FillWithNoise(3.0f, in.HeightmapAmplitude, 4, 99);
        svc.SeedDecodedHeightmapForTests(kHeightmapGuid, heightmap);
        TerrainZonePayload payload;
        payload.Allocate(ZonePayloadFormat::SculptOffsetR32F, 17, 17);
        for (uint32 i = 0; i < 17u * 17u; ++i)
            payload.Offsets[i] = in.PayloadScale * static_cast<float32>(i % 17u) / 16.0f;
        svc.SeedZonePayloadForTests(kPayloadGuid, payload);

        Terrain::TerrainConfig config{};
        config.HeightmapWidth = kDim;
        config.HeightmapHeight = kDim;
        config.WorldSizeX = in.WorldSize;
        config.WorldSizeZ = in.WorldSize;
        config.HeightScale = kHeightScale;
        config.LODLevels = 4;
        const TerrainHandle handle = svc.CreateTerrain(config);
        data = svc.GetTerrainData(handle);

        EnableTerrainModifierLifecycleEvents(world);
        const auto addVolume = [&](float32 x, float32 z, Components::TerrainVolumeShape shape, float32 radius) {
            auto e = world.CreateEntity();
            Components::TerrainModifierVolume volume{};
            volume.Shape = shape;
            volume.Radius = radius;
            volume.Falloff = 8.0f;
            world.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
            Components::WorldTransform xf{};
            xf.matrix[12] = x;
            xf.matrix[14] = z;
            world.AddComponentImmediate<Components::WorldTransform>(e, xf);
            return e;
        };
        {
            auto e = world.CreateEntity();
            Components::Terrain terrain{};
            terrain.SizeX = in.WorldSize;
            terrain.SizeZ = in.WorldSize;
            terrain.HeightScale = kHeightScale;
            terrain.TerrainDataHandle = handle.Index;
            terrain.TerrainDataGeneration = handle.Generation;
            terrain.BaseSource = Components::TerrainBaseSource::HeightmapAsset;
            terrain.TerrainAssetGuid.Set(kHeightmapGuid);
            world.AddComponentImmediate<Components::Terrain>(e, terrain);
            Components::WorldTransform xf{};
            xf.matrix[12] = in.TerrainX;
            world.AddComponentImmediate<Components::WorldTransform>(e, xf);
            if (sceneIdentity)
                GiveSceneIdentity(world, e);
            terrainEntity = e;
        }
        {
            Components::TerrainFlattenEffect flatten{};
            flatten.TargetHeight = in.FlattenTarget;
            flattenEntity = addVolume(in.TerrainX + in.FlattenX, 40.0f, Components::TerrainVolumeShape::Circle, 25.0f);
            world.AddComponentImmediate(flattenEntity, flatten);
            Components::TerrainNoiseEffect noise{};
            noise.Amplitude = 5.0f;
            noise.Frequency = 8.0f;
            noise.Octaves = 3u;
            world.AddComponentImmediate(addVolume(in.TerrainX - 50.0f, 10.0f, Components::TerrainVolumeShape::Circle, 25.0f),
                                        noise);
            Components::TerrainStampEffect stamp{};
            stamp.HeightScale = 20.0f;
            stamp.StampAssetGuid.Set(kMaskGuid);
            world.AddComponentImmediate(addVolume(in.TerrainX + 60.0f, -60.0f, Components::TerrainVolumeShape::Circle, 25.0f),
                                        stamp);
            Components::TerrainPaintLayerEffect paint{};
            paint.LayerIndex = 1;
            paint.Strength = 0.8f;
            world.AddComponentImmediate(addVolume(in.TerrainX - 30.0f, -60.0f, Components::TerrainVolumeShape::Circle, 20.0f),
                                        paint);
            Components::TerrainSurfaceRulesEffect rules{};
            rules.RuleCount = 1;
            rules.Rules[0].MaterialSlot = 2;
            rules.Rules[0].Strength = 1.0f;
            rules.Rules[0].ConditionCount = 1;
            rules.Rules[0].Conditions[0].Kind = Components::TerrainRuleConditionKind::HeightNormalized;
            rules.Rules[0].Conditions[0].Min = 0.3f;
            rules.Rules[0].Conditions[0].Max = 0.9f;
            world.AddComponentImmediate(addVolume(0.0f, 0.0f, Components::TerrainVolumeShape::Global, 0.0f), rules);
        }
        {
            auto& splines = SplineECS::SplineService::Get();
            const SplineECS::SplineHandle spline = splines.CreateSpline(Spline::SplineType::CatmullRom, false);
            auto* points = splines.GetSplineData(spline);
            for (int i = 0; i < 4; ++i)
                points->AddPoint(Mathematics::Vector3(in.TerrainX + in.SplineX + 60.0f * static_cast<float32>(i), 0.0f,
                                                      80.0f - 20.0f * static_cast<float32>(i)),
                                 10.0f);
            splines.RebuildCache(spline);
            auto e = addVolume(0.0f, 0.0f, Components::TerrainVolumeShape::SplinePath, 0.0f);
            Components::SplineComponent component{};
            component.SplineDataIndex = spline.Index();
            component.SplineDataGeneration = spline.Generation();
            world.AddComponentImmediate<Components::SplineComponent>(e, component);
            Components::TerrainFlattenEffect pathFlatten{};
            pathFlatten.TargetHeight = 12.0f;
            world.AddComponentImmediate(e, pathFlatten);
        }
        {
            auto e = world.CreateEntity();
            Components::TerrainSculptZone zone{};
            zone.ExtentX = 40.0f;
            zone.ExtentZ = 40.0f;
            zone.Priority = 1000.0f;
            zone.PayloadRef.Set(kPayloadGuid);
            world.AddComponentImmediate<Components::TerrainSculptZone>(e, zone);
            Components::WorldTransform xf{};
            xf.matrix[12] = in.TerrainX + 80.0f;
            xf.matrix[14] = 60.0f;
            world.AddComponentImmediate<Components::WorldTransform>(e, xf);
        }

        std::vector<float32> mask(16u * 16u);
        for (uint32 i = 0; i < mask.size(); ++i)
            mask[i] = in.MaskScale * (0.15f + 0.7f * static_cast<float32>(i % 16u) / 15.0f);
        system.SeedDecodedStampMaskForTests(kMaskGuid, mask, 16, 16);
        Update();
    }

    ~BakeSession()
    {
        SplineECS::SplineService::Shutdown();
        TerrainService::Shutdown();
    }

    void Update() { system.Update(world, 1.0f / 60.0f); }

    BakeResult Result() const
    {
        BakeResult result;
        result.Heights.assign(data->Heightfield.GetRawSamples(),
                              data->Heightfield.GetRawSamples() + data->Heightfield.GetSampleCount());
        result.Splat = data->Splatmap;
        result.SplatMinH = data->SplatBakeMinH;
        result.SplatMaxH = data->SplatBakeMaxH;
        return result;
    }
};

// One open of the scene: the first (full) bake with `cache` configured.
BakeResult BakeScene(const SceneInputs& in, const TerrainBakeCacheConfig& cache)
{
    BakeSession session(in, cache);
    return session.Result();
}

// Every artifact under `directory`, at any depth.
std::vector<fs::path> Artifacts(const fs::path& directory)
{
    std::vector<fs::path> files;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(directory, error))
        if (entry.path().extension() == ".getbake")
            files.push_back(entry.path());
    return files;
}

// The key in an artifact's header (after the magic and the version).
uint64 StoredKey(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    uint64 key = 0;
    in.seekg(8);
    in.read(reinterpret_cast<char*>(&key), sizeof(key));
    return key;
}

// The key the bake of `in` is stored under.
uint64 KeyOf(const SceneInputs& in)
{
    TempDirectory dir;
    BakeScene(in, {dir.Path, true});
    const std::vector<fs::path> files = Artifacts(dir.Path);
    EXPECT_EQ(files.size(), 1u);
    return files.empty() ? 0u : StoredKey(files[0]);
}

// The procedural base with the effect kinds BakeScene's fixture lacks: a height
// offset, an eroded noise, a ground claim, grass and a paint zone.
BakeResult BakeProceduralScene()
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();
    auto& svc = TerrainService::Get();
    const GUID maskGuid = GUID::Generate();
    TerrainZonePayload mask;
    mask.Allocate(ZonePayloadFormat::PaintMaskR8, 17, 17);
    for (uint32 i = 0; i < 17u * 17u; ++i)
        mask.Mask[i] = static_cast<uint8>((i * 13u) % 256u);
    svc.SeedZonePayloadForTests(maskGuid, mask);

    Terrain::TerrainConfig config{};
    config.HeightmapWidth = kDim;
    config.HeightmapHeight = kDim;
    config.WorldSizeX = kWorldSize;
    config.WorldSizeZ = kWorldSize;
    config.HeightScale = kHeightScale;
    config.LODLevels = 4;
    const TerrainHandle handle = svc.CreateTerrain(config);
    auto* data = svc.GetTerrainData(handle);

    ECS::World world;
    EnableTerrainModifierLifecycleEvents(world);
    const auto addVolume = [&](float32 x, float32 z, float32 radius) {
        auto e = world.CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Circle;
        volume.Radius = radius;
        volume.Falloff = 8.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
        Components::WorldTransform xf{};
        xf.matrix[12] = x;
        xf.matrix[14] = z;
        world.AddComponentImmediate<Components::WorldTransform>(e, xf);
        return e;
    };
    {
        auto e = world.CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = kWorldSize;
        terrain.SizeZ = kWorldSize;
        terrain.HeightScale = kHeightScale;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;
        terrain.BaseSource = Components::TerrainBaseSource::ProceduralNoise;
        world.AddComponentImmediate<Components::Terrain>(e, terrain);
        world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    }
    {
        Components::TerrainHeightOffsetEffect offset{};
        offset.Offset = 6.0f;
        world.AddComponentImmediate(addVolume(40.0f, 30.0f, 30.0f), offset);
        Components::TerrainNoiseEffect eroded{};
        eroded.Amplitude = 8.0f;
        eroded.ErosionStrength = 0.6f;
        world.AddComponentImmediate(addVolume(-40.0f, -20.0f, 35.0f), eroded);
        const auto claimed = addVolume(-40.0f, -20.0f, 15.0f);
        Components::TerrainGroundClaimEffect claim{};
        world.AddComponentImmediate(claimed, claim);
        Components::TerrainGrassEffect grass{};
        grass.HeightScale = 0.5f;
        grass.DensityScale = 0.5f;
        world.AddComponentImmediate(addVolume(0.0f, 60.0f, 20.0f), grass);

        auto zoneEntity = world.CreateEntity();
        Components::TerrainPaintZone zone{};
        zone.ExtentX = 30.0f;
        zone.ExtentZ = 30.0f;
        zone.LayerIndex = 2;
        zone.PayloadRef.Set(maskGuid);
        world.AddComponentImmediate<Components::TerrainPaintZone>(zoneEntity, zone);
        Components::WorldTransform xf{};
        xf.matrix[12] = 60.0f;
        xf.matrix[14] = -50.0f;
        world.AddComponentImmediate<Components::WorldTransform>(zoneEntity, xf);
    }
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    BakeResult result;
    result.Heights.assign(data->Heightfield.GetRawSamples(),
                          data->Heightfield.GetRawSamples() + data->Heightfield.GetSampleCount());
    result.Splat = data->Splatmap;
    TerrainService::Shutdown();
    return result;
}

uint64 OutputHash(const BakeResult& bake)
{
    const uint64 heights = Hashing::Fnv1a64(bake.Heights.data(), bake.Heights.size() * sizeof(float32));
    return Hashing::Fnv1a64(bake.Splat.data(), bake.Splat.size(), heights);
}

// Writes raw bytes as a file, for the malformed-artifact cases.
void WriteBytes(const fs::path& file, const std::vector<uint8>& bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
}

// A valid kDim x kDim artifact's bytes under `key`.
std::vector<uint8> ValidArtifactBytes(const fs::path& scratch, uint64 key)
{
    TerrainBakeArtifact artifact;
    artifact.Width = kDim;
    artifact.Height = kDim;
    artifact.Heights.assign(static_cast<std::size_t>(kDim) * kDim, 1.0f);
    artifact.Splat.assign(static_cast<std::size_t>(kDim) * kDim * 4u, 7u);
    const fs::path file = scratch / "valid.getbake";
    EXPECT_TRUE(WriteTerrainBake(file, key, artifact));
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TerrainBakeCacheStatus ReadAs(const fs::path& file, uint64 key)
{
    TerrainBakeArtifact out;
    return ReadTerrainBake(file, key, kDim, kDim, out);
}

void ExpectSameBake(const BakeResult& a, const BakeResult& b)
{
    ASSERT_EQ(a.Heights.size(), b.Heights.size());
    EXPECT_EQ(0, std::memcmp(a.Heights.data(), b.Heights.data(), a.Heights.size() * sizeof(float32)));
    ASSERT_EQ(a.Splat.size(), b.Splat.size());
    EXPECT_EQ(0, std::memcmp(a.Splat.data(), b.Splat.data(), a.Splat.size()));
    EXPECT_EQ(a.SplatMinH, b.SplatMinH);
    EXPECT_EQ(a.SplatMaxH, b.SplatMaxH);
}

} // namespace

TEST(TerrainBakeCache, AHitIsByteIdenticalToAFreshBake)
{
    TempDirectory dir;
    const BakeResult uncached = BakeScene({}, {});
    const BakeResult stored = BakeScene({}, {dir.Path, true});
    ASSERT_EQ(Artifacts(dir.Path).size(), 1u) << "the miss must store its bake";
    EXPECT_EQ(Artifacts(dir.Path)[0], TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag));
    const BakeResult hit = BakeScene({}, {dir.Path, false});
    ExpectSameBake(uncached, stored);
    ExpectSameBake(uncached, hit);
}

TEST(TerrainBakeCache, AHitServesTheStoredArtifactInsteadOfBaking)
{
    TempDirectory dir;
    const BakeResult fresh = BakeScene({}, {dir.Path, true});
    const fs::path file = Artifacts(dir.Path).at(0);
    const uint64 key = StoredKey(file);
    TerrainBakeArtifact artifact;
    ASSERT_EQ(ReadTerrainBake(file, key, kDim, kDim, artifact), TerrainBakeCacheStatus::Hit);
    for (float32& height : artifact.Heights)
        height += 1.0f;
    ASSERT_TRUE(WriteTerrainBake(file, key, artifact));

    const BakeResult served = BakeScene({}, {dir.Path, false});
    ASSERT_EQ(served.Heights.size(), fresh.Heights.size());
    EXPECT_EQ(served.Heights[0], fresh.Heights[0] + 1.0f) << "a hit must apply the stored heights, not bake";
}

TEST(TerrainBakeCache, TheSameInputsGiveTheSameKey)
{
    EXPECT_EQ(KeyOf({}), KeyOf({}));
}

TEST(TerrainBakeCache, TheTerrainPlacementIsInTheKey)
{
    SceneInputs moved;
    moved.TerrainX = 16.0f;
    EXPECT_NE(KeyOf({}), KeyOf(moved));
}

TEST(TerrainBakeCache, TheTerrainSizeIsInTheKey)
{
    SceneInputs larger;
    larger.WorldSize = 300.0f;
    EXPECT_NE(KeyOf({}), KeyOf(larger));
}

TEST(TerrainBakeCache, TheBaseHeightmapContentIsInTheKey)
{
    SceneInputs taller;
    taller.HeightmapAmplitude = 1.5f;
    EXPECT_NE(KeyOf({}), KeyOf(taller));
}

TEST(TerrainBakeCache, AModifierParameterIsInTheKey)
{
    SceneInputs higher;
    higher.FlattenTarget = 21.0f;
    EXPECT_NE(KeyOf({}), KeyOf(higher));
}

TEST(TerrainBakeCache, TheSplineContentIsInTheKey)
{
    SceneInputs moved;
    moved.SplineX = -90.0f;
    EXPECT_NE(KeyOf({}), KeyOf(moved));
}

TEST(TerrainBakeCache, TheStampMaskContentIsInTheKey)
{
    SceneInputs fainter;
    fainter.MaskScale = 0.5f;
    EXPECT_NE(KeyOf({}), KeyOf(fainter));
}

TEST(TerrainBakeCache, TheZonePayloadContentIsInTheKey)
{
    SceneInputs deeper;
    deeper.PayloadScale = 31.0f;
    EXPECT_NE(KeyOf({}), KeyOf(deeper));
}

// One terrain owns one artifact: a later bake of the same terrain in another state
// replaces it, and the earlier state's key no longer reads as a hit.
TEST(TerrainBakeCache, StoresForOneTerrainReplaceItsOneArtifact)
{
    TempDirectory dir;
    BakeScene({}, {dir.Path, true});
    const uint64 firstKey = StoredKey(Artifacts(dir.Path).at(0));
    SceneInputs edited;
    edited.FlattenTarget = 21.0f;
    BakeScene(edited, {dir.Path, true});
    const std::vector<fs::path> files = Artifacts(dir.Path);
    ASSERT_EQ(files.size(), 1u) << "two stores for one terrain leave one artifact";
    EXPECT_NE(StoredKey(files[0]), firstKey) << "the stale key was replaced";
    EXPECT_EQ(ReadAs(files[0], firstKey), TerrainBakeCacheStatus::KeyMismatch);
}

// A terrain with no scene identity bakes uncached and leaves nothing behind.
TEST(TerrainBakeCache, ATerrainWithoutASceneIdentityIsNotCached)
{
    EXPECT_TRUE(TerrainBakeFile("cache", GUID::Null(), kTerrainTag).empty());
    EXPECT_TRUE(TerrainBakeFile("cache", kSceneGuid, "").empty());
}

// A package stages the artifacts of exactly the scenes it ships.
TEST(TerrainBakeCache, StagingCopiesOnlyTheShippedScenesArtifacts)
{
    TempDirectory cache;
    TempDirectory package;
    const GUID shipped = GUID::Generate();
    const GUID dropped = GUID::Generate();
    const std::vector<uint8> bytes = ValidArtifactBytes(cache.Path, 1u);
    WriteBytes(TerrainBakeFile(cache.Path, shipped, "a"), bytes);
    WriteBytes(TerrainBakeFile(cache.Path, shipped, "b"), bytes);
    WriteBytes(TerrainBakeFile(cache.Path, dropped, "a"), bytes);

    std::vector<fs::path> failures;
    const std::vector<GUID> scenes = {shipped};
    const auto staged = StageTerrainBakeArtifacts(cache.Path, scenes, package.Path, failures);
    EXPECT_TRUE(failures.empty());
    ASSERT_EQ(staged.size(), 1u);
    EXPECT_EQ(staged[0].Artifacts, 2u);
    std::vector<fs::path> copied = Artifacts(package.Path);
    std::sort(copied.begin(), copied.end());
    std::vector<fs::path> expected = {TerrainBakeFile(package.Path, shipped, "a"),
                                      TerrainBakeFile(package.Path, shipped, "b")};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(copied, expected);
}

// A malformed artifact answers its status without throwing or allocating from its header.
TEST(TerrainBakeCache, MalformedArtifactsAreMissesNotCrashes)
{
    TempDirectory dir;
    constexpr uint64 kKey = 0x1234u;
    const std::vector<uint8> valid = ValidArtifactBytes(dir.Path, kKey);
    const fs::path file = dir.Path / "probe.getbake";
    constexpr std::size_t kHeader = 32u;

    WriteBytes(file, valid);
    EXPECT_EQ(ReadAs(file, kKey), TerrainBakeCacheStatus::Hit) << "positive control";

    WriteBytes(file, std::vector<uint8>(valid.begin(), valid.begin() + kHeader));
    EXPECT_EQ(ReadAs(file, kKey), TerrainBakeCacheStatus::Corrupt) << "header only";

    std::vector<uint8> overflowing(valid.begin(), valid.begin() + kHeader);
    const uint32 huge = 0x80000000u;
    std::memcpy(overflowing.data() + 16, &huge, sizeof(huge));
    std::memcpy(overflowing.data() + 20, &huge, sizeof(huge));
    WriteBytes(file, overflowing);
    EXPECT_EQ(ReadAs(file, kKey), TerrainBakeCacheStatus::Corrupt) << "a header whose sizes overflow";

    WriteBytes(file, std::vector<uint8>(valid.begin(), valid.end() - 1));
    EXPECT_EQ(ReadAs(file, kKey), TerrainBakeCacheStatus::Corrupt) << "a truncated payload";

    WriteBytes(file, {});
    EXPECT_EQ(ReadAs(file, kKey), TerrainBakeCacheStatus::Corrupt) << "a zero-length file";

    std::vector<uint8> otherVersion = valid;
    const uint32 version = kTerrainBakeFormatVersion + 1u;
    std::memcpy(otherVersion.data() + 4, &version, sizeof(version));
    WriteBytes(file, otherVersion);
    EXPECT_EQ(ReadAs(file, kKey), TerrainBakeCacheStatus::FormatMismatch) << "another format version";
}

// A writer that died between its write and its rename leaves a temporary sibling;
// the next store of that terrain removes it.
TEST(TerrainBakeCache, AStoreRemovesTemporarySiblingsOfDeadWriters)
{
    TempDirectory dir;
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    const fs::path orphan = file.string() + ".1-1-0.tmp";
    WriteBytes(orphan, {1, 2, 3});
    fs::last_write_time(orphan, fs::last_write_time(orphan) - std::chrono::hours(24));
    BakeScene({}, {dir.Path, true});
    EXPECT_FALSE(fs::exists(orphan));
    EXPECT_TRUE(fs::exists(file));
}

// What a fresh open of the scene saved as `in` computes and bakes, with no cache.
struct FreshOpen
{
    uint64 Key = 0;
    BakeResult Bake;
};

FreshOpen FreshOpenOf(const SceneInputs& in)
{
    return {KeyOf(in), BakeScene(in, {})};
}

void ExpectArtifactIsFreshOpen(const fs::path& file, const FreshOpen& fresh)
{
    TerrainBakeArtifact artifact;
    ASSERT_EQ(ReadTerrainBake(file, fresh.Key, kDim, kDim, artifact), TerrainBakeCacheStatus::Hit)
        << "the stored key is the one a fresh open of the saved scene computes";
    ExpectSameBake({artifact.Heights, artifact.Splat, artifact.SplatMinH, artifact.SplatMaxH}, fresh.Bake);
}

// A region edit stores nothing; the save after it stores the in-memory result, which
// is the full bake of the saved state.
TEST(TerrainBakeCache, ASaveAfterARegionEditStoresTheFullBakeOfTheSavedState)
{
    TempDirectory dir;
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    {
        BakeSession session({}, {dir.Path, true});
        const uint64 opened = StoredKey(file);
        session.world.GetComponentForWrite<Components::TerrainFlattenEffect>(session.flattenEntity)->TargetHeight = 21.0f;
        session.Update();
        ASSERT_EQ(StoredKey(file), opened) << "the edit took the region path, which stores nothing";
        session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
        session.Update();
    }
    SceneInputs saved;
    saved.FlattenTarget = 21.0f;
    ExpectArtifactIsFreshOpen(file, FreshOpenOf(saved));
}

// A save during a deferred geometry drag stores nothing until the drag settles; the
// update that settles it stores the settled state.
TEST(TerrainBakeCache, ASaveDuringADeferredDragStoresTheSettledState)
{
    TempDirectory dir;
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    {
        BakeSession session({}, {dir.Path, true});
        const uint64 opened = StoredKey(file);
        auto& svc = TerrainService::Get();
        svc.SetInteractiveModifierEdit(TerrainService::InteractiveEditSource::TransformGizmo, true);
        session.world.GetComponentForWrite<Components::WorldTransform>(session.flattenEntity)->matrix[12] = 45.0f;
        session.Update();
        session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
        session.Update();
        EXPECT_EQ(StoredKey(file), opened) << "a preview mid-drag is not stored";
        svc.SetInteractiveModifierEdit(TerrainService::InteractiveEditSource::TransformGizmo, false);
        session.Update();
    }
    SceneInputs saved;
    saved.FlattenX = 45.0f;
    ExpectArtifactIsFreshOpen(file, FreshOpenOf(saved));
}

// The update that serves a save keys the terrain and hands its data to the store job,
// which copies the bake. The bake store channel is held so the test can write a sample
// after that update returns: the artifact carries the write, so the copy ran after it.
TEST(TerrainBakeCache, ASaveCopiesTheBakeOffTheUpdate)
{
    TempDirectory dir;
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    float32 probe = 0.0f;
    {
        BakeSession session({}, {dir.Path, true}, true, &pool);
        session.world.GetComponentForWrite<Components::TerrainFlattenEffect>(session.flattenEntity)->TargetHeight = 21.0f;
        session.Update();
        JobSystem::TaskHandle held = TerrainService::Get().BakeStoreChannel(pool).Submit(
            [released = release.get_future().share()]() { released.wait(); });
        session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
        session.Update();
        probe = session.data->Heightfield.GetRawSamples()[0] + 1000.0f;
        session.data->Heightfield.GetMutableSamples()[0] = probe;
        release.set_value();
        held.Wait();
    }
    TerrainBakeArtifact artifact;
    ASSERT_EQ(ReadTerrainBake(file, StoredKey(file), kDim, kDim, artifact), TerrainBakeCacheStatus::Hit);
    EXPECT_EQ(artifact.Heights[0], probe) << "the update that served the save copied the bake itself";
}

// With the store job still queued (the bake store channel held), the update after a save
// does not wait for it: it takes the copy itself, then bakes an edit made right after the
// save. The edit never reaches the artifact the save stored.
TEST(TerrainBakeCache, TheUpdateAfterASaveTakesTheCopyWhenTheStoreIsQueued)
{
    TempDirectory dir;
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    {
        BakeSession session({}, {dir.Path, true}, true, &pool);
        session.world.GetComponentForWrite<Components::TerrainFlattenEffect>(session.flattenEntity)->TargetHeight = 21.0f;
        session.Update();
        JobSystem::TaskHandle held = TerrainService::Get().BakeStoreChannel(pool).Submit(
            [released = release.get_future().share()]() { released.wait(); });
        session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
        session.Update();
        session.world.GetComponentForWrite<Components::TerrainFlattenEffect>(session.flattenEntity)->TargetHeight = 30.0f;
        std::future<void> next = std::async(std::launch::async, [&session]() { session.Update(); });
        const bool returned = next.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
        release.set_value();
        next.wait();
        held.Wait();
        EXPECT_TRUE(returned) << "the update after the save waited for the queued store";
    }
    SceneInputs saved;
    saved.FlattenTarget = 21.0f;
    ExpectArtifactIsFreshOpen(file, FreshOpenOf(saved));
}

// The store waits on the disk, not on a compute worker: with the pool's only worker held,
// a save's artifact is still written. (The open already stored the unedited bake; the save
// replaces it with the edited one.)
TEST(TerrainBakeCache, ASaveIsStoredWhileEveryComputeWorkerIsHeld)
{
    SceneInputs saved;
    saved.FlattenTarget = 21.0f;
    const uint64 savedKey = KeyOf(saved);
    TempDirectory dir;
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    bool storedWhileHeld = false;
    {
        BakeSession session({}, {dir.Path, true}, true, &pool);
        session.world.GetComponentForWrite<Components::TerrainFlattenEffect>(session.flattenEntity)->TargetHeight = 21.0f;
        session.Update();
        ASSERT_NE(StoredKey(file), savedKey) << "the edit was stored before the save";
        std::promise<void> workerHeld;
        JobSystem::TaskHandle held = pool.Submit(
            [&workerHeld, released = release.get_future().share()]()
            {
                workerHeld.set_value();
                released.wait();
            });
        workerHeld.get_future().wait();
        session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
        session.Update();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (StoredKey(file) != savedKey && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        storedWhileHeld = StoredKey(file) == savedKey;
        release.set_value();
        held.Wait();
    }
    EXPECT_TRUE(storedWhileHeld) << "the store waited for the held compute worker";
    ExpectArtifactIsFreshOpen(file, FreshOpenOf(saved));
}

// A second save with no edit between writes nothing.
TEST(TerrainBakeCache, ASaveWithNoEditWritesNothing)
{
    TempDirectory dir;
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    BakeSession session({}, {dir.Path, true});
    const auto old = fs::last_write_time(file) - std::chrono::hours(1);
    fs::last_write_time(file, old);
    session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
    session.Update();
    EXPECT_EQ(fs::last_write_time(file), old);
}

// A save prunes the saved scene's folder to its live terrains and leaves every other
// scene's folder alone.
TEST(TerrainBakeCache, ASavePrunesTheSceneFolderToItsLiveTerrains)
{
    TempDirectory dir;
    const fs::path deleted = TerrainBakeFile(dir.Path, kSceneGuid, "deleted-terrain");
    const fs::path otherScene = TerrainBakeFile(dir.Path, GUID::Generate(), kTerrainTag);
    const std::vector<uint8> bytes = ValidArtifactBytes(dir.Path, 1u);
    WriteBytes(deleted, bytes);
    WriteBytes(otherScene, bytes);
    {
        BakeSession session({}, {dir.Path, true});
        session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
        session.Update();
    }
    EXPECT_FALSE(fs::exists(deleted)) << "a terrain the scene no longer has";
    EXPECT_TRUE(fs::exists(TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag)));
    EXPECT_TRUE(fs::exists(otherScene)) << "another scene's artifact";
}

// Save As moves the terrains to the new scene's identity: its folder gets the
// artifact, and the old scene's stays its own.
TEST(TerrainBakeCache, SaveAsStoresUnderTheNewSceneAndKeepsTheOld)
{
    TempDirectory dir;
    const GUID newScene = GUID::Generate();
    const fs::path oldFile = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    const fs::path newFile = TerrainBakeFile(dir.Path, newScene, kTerrainTag);
    BakeSession session({}, {dir.Path, true});
    session.system.RequestBakeCacheStore(session.world, kSceneGuid, newScene);
    session.Update();
    EXPECT_EQ(session.world.GetComponent<Components::Terrain>(session.terrainEntity)->BakeOriginScene, newScene);
    ASSERT_TRUE(fs::exists(newFile));
    EXPECT_EQ(StoredKey(newFile), StoredKey(oldFile));
    EXPECT_TRUE(fs::exists(oldFile));
}

// A terrain added since the document was last saved has no identity yet; the save
// gives it the saved scene's.
TEST(TerrainBakeCache, ASaveStampsATerrainAddedSinceTheLastSave)
{
    TempDirectory dir;
    const GUID savedScene = GUID::Generate();
    BakeSession session({}, {dir.Path, true});
    session.world.GetComponentForWrite<Components::Terrain>(session.terrainEntity)->BakeOriginScene = GUID{};
    session.system.RequestBakeCacheStore(session.world, savedScene, savedScene);
    session.Update();
    EXPECT_EQ(session.world.GetComponent<Components::Terrain>(session.terrainEntity)->BakeOriginScene, savedScene);
    EXPECT_TRUE(fs::exists(TerrainBakeFile(dir.Path, savedScene, kTerrainTag)));
}

// A terrain from a subscene carries the subscene file's identity (kSceneGuid here). A
// save and a Save As of the parent document leave it there, store nothing under the
// parent, and a reopen of the subscene's terrain hits its artifact.
TEST(TerrainBakeCache, ASaveOfTheParentKeepsASubsceneTerrainsIdentity)
{
    TempDirectory dir;
    const GUID parentScene = GUID::Generate();
    const GUID savedAsScene = GUID::Generate();
    const fs::path file = TerrainBakeFile(dir.Path, kSceneGuid, kTerrainTag);
    {
        BakeSession session({}, {dir.Path, true});
        session.system.RequestBakeCacheStore(session.world, parentScene, parentScene);
        session.Update();
        session.system.RequestBakeCacheStore(session.world, parentScene, savedAsScene);
        session.Update();
        EXPECT_EQ(session.world.GetComponent<Components::Terrain>(session.terrainEntity)->BakeOriginScene, kSceneGuid);
    }
    ASSERT_EQ(Artifacts(dir.Path), std::vector<fs::path>{file}) << "nothing stored under the parent";
    const auto old = fs::last_write_time(file) - std::chrono::hours(1);
    fs::last_write_time(file, old);
    BakeSession reopen({}, {dir.Path, true});
    EXPECT_EQ(fs::last_write_time(file), old) << "the reopen hit: a miss would have stored its bake";
}

// A terrain with no scene tag has no artifact, at open or at save.
TEST(TerrainBakeCache, ASaveStoresNothingForATerrainWithoutATag)
{
    TempDirectory dir;
    BakeSession session({}, {dir.Path, true}, /*sceneIdentity*/ false);
    session.system.RequestBakeCacheStore(session.world, kSceneGuid, kSceneGuid);
    session.Update();
    EXPECT_TRUE(Artifacts(dir.Path).empty());
}

// Pins of the key (portable across processes and machines: it hashes authored
// values and content only) and of the bake output (an evaluator change that moves
// a texel). When either fails on purpose, bump kTerrainBakeFormatVersion and take
// the new values from the failure message.
TEST(TerrainBakeCache, TheKeyAndTheBakeOutputArePinnedToTheFormatVersion)
{
    constexpr uint64 kPinnedKey = 0xdcc535cfbbbe9a3full;
    constexpr uint64 kPinnedOutput = 0xf19e25b84ba58001ull;
    TempDirectory dir;
    const BakeResult bake = BakeScene({}, {dir.Path, true});
    const std::vector<fs::path> files = Artifacts(dir.Path);
    ASSERT_EQ(files.size(), 1u);
    const uint64 key = StoredKey(files[0]);
    const uint64 output = OutputHash(bake);
    EXPECT_EQ(key, kPinnedKey) << std::hex << "the bake key changed (now 0x" << key
                               << "): bump kTerrainBakeFormatVersion, then pin the new value";
    EXPECT_EQ(output, kPinnedOutput) << std::hex << "the bake output changed (now 0x" << output
                                     << "): bump kTerrainBakeFormatVersion, then pin the new value";
}

// The procedural base and the effect kinds the pin above lacks: a change to the base
// noise or to any of these evaluators changes this output.
TEST(TerrainBakeCache, TheProceduralBakeOutputIsPinnedToTheFormatVersion)
{
    constexpr uint64 kPinnedOutput = 0xe901d8350a11b30full;
    const uint64 output = OutputHash(BakeProceduralScene());
    EXPECT_EQ(output, kPinnedOutput) << std::hex << "the procedural bake output changed (now 0x" << output
                                     << "): bump kTerrainBakeFormatVersion, then pin the new value";
}
