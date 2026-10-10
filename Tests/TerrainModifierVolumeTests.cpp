// Terrain modifier VOLUMES: one region entity owning the shape, falloff, master
// weight and priority, with shape-unaware effect components stacked on it
// (spline-modifier-tools-design.html §2a).
//
// The oracles here are of three kinds:
//
//  1. MIGRATION PARITY — the contract that makes the new model a replacement
//     rather than an addition. For each of the five migrated effects, a scene
//     authored the OLD way — the named property block a legacy scene file still
//     carries, put through the registered schema that converts it — and the SAME
//     content authored the NEW way (volume + effect) must produce a
//     BYTE-IDENTICAL bake. Compared with memcmp over the whole
//     heightfield/splatmap, so a one-LSB arithmetic reordering fails.
//
//     The old arm goes through the SCHEMA rather than constructing a pre-volume
//     component directly, because the schema conversion is what a legacy scene
//     actually depends on: no scene in this repo carries these blocks any more,
//     but a project's does, and this is the oracle that keeps such a scene
//     baking what it always baked. The FILE spelling has its own fixture —
//     Engine/Tests/Fixtures/TerrainLegacyModifierScene/LegacyTerrainModifiers.scene,
//     loaded by EngineSceneIOTests — which pins that the blocks still parse and
//     migrate; these oracles pin that the result bakes identically.
//
//  2. STACKING — that the effect order is real and reproducible. An
//     order-dependent pair (flatten-then-noise vs noise-then-flatten) must differ,
//     and each ordering must equal the two pre-volume modifiers whose priorities
//     express the same sequence.
//
//  3. SHAPE — that the volume owns the region: spline volumes reproduce the
//     Shape::Spline composition, SplinePath and SplineArea differ exactly where a
//     closed loop encloses something, the inward feather softens a filled rim, and
//     the sphere guards still refuse what has no sphere path.
//
// fBM evaluates to exactly zero at its integer lattice corners, so every
// "did something happen" assertion is over the SET of changed samples, never a
// single hand-picked one — a single sample can read unmodified no matter how well
// the volume resolved.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "Noise/FractalNoise2D.h"
#include "TerrainECS/Erosion/ErosionFilter.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

struct ScopedSplineService
{
    ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
    }
    ~ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
    }
};

constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;
constexpr uint32 kHeightmapDim = 129;
constexpr float32 kPlanetRadius = 2000.0f;

Terrain::TerrainConfig MakeTestConfig()
{
    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = kHeightmapDim;
    cfg.HeightmapHeight = kHeightmapDim;
    cfg.WorldSizeX = kWorldSize;
    cfg.WorldSizeZ = kWorldSize;
    cfg.HeightScale = kHeightScale;
    cfg.LODLevels = 4;
    return cfg;
}

// Mirrors TerrainExtractionSystem's creation path (base fill + quadtree +
// procedural splat), as TerrainRegionBakeTests and TerrainSplineModifierTests do.
TerrainHandle CreateBakedTerrain(TerrainService& svc)
{
    const TerrainHandle handle = svc.CreateTerrain(MakeTestConfig());
    auto* data = svc.GetTerrainData(handle);
    FillHeightfieldBaseRegion(data->Heightfield, Components::TerrainBaseSource::ProceduralNoise,
                              nullptr, 0, 0,
                              static_cast<int32>(kHeightmapDim) - 1,
                              static_cast<int32>(kHeightmapDim) - 1);
    data->MarkFullDirty();
    svc.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();
    return handle;
}

Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    Components::WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    return xf;
}

// `worldY` is the terrain entity's own translation — the offset every sample is read back
// through (`normalized * HeightScale + worldY`). Zero for every arm that compares normalized
// bytes; the world-height arms place it deliberately.
ECS::EntityHandle CreateTerrainEntity(ECS::World& world, TerrainHandle handle, float32 worldY)
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
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, worldY, 0.0f));
    return e;
}

ECS::EntityHandle CreatePlanetEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.Domain = Components::TerrainDomain::Spherical;
    terrain.PlanetRadius = kPlanetRadius;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

// A spline entity whose control points are authored in entity-local space, with
// the swept radius as the band half-width.
ECS::EntityHandle CreateSplineEntity(ECS::World& world, SplineECS::SplineService& svc,
                                     const std::vector<Mathematics::Vector3>& points,
                                     float32 radius, bool closed)
{
    const SplineECS::SplineHandle handle =
        svc.CreateSpline(Spline::SplineType::CatmullRom, closed);
    auto* data = svc.GetSplineData(handle);
    for (const auto& p : points)
        data->AddPoint(p, radius);
    svc.RebuildCache(handle);

    auto e = world.CreateEntity();
    Components::SplineComponent comp{};
    comp.SplineDataIndex = handle.Index();
    comp.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate<Components::SplineComponent>(e, comp);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

// Asymmetric procedural mask so a rotation/orientation mistake changes the bake.
std::vector<float32> MakeGradientMask(uint32 width, uint32 height)
{
    std::vector<float32> texels(static_cast<size_t>(width) * height);
    for (uint32 z = 0; z < height; ++z)
        for (uint32 x = 0; x < width; ++x)
        {
            const float32 u = static_cast<float32>(x) / static_cast<float32>(width - 1);
            const float32 v = static_cast<float32>(z) / static_cast<float32>(height - 1);
            texels[static_cast<size_t>(z) * width + x] = 0.15f + 0.7f * u * (0.3f + 0.7f * v);
        }
    return texels;
}

// The stamp mask GUID both arms of a parity pair seed, so the decoded content is
// identical and the only difference under test is the component authoring.
const GUID& StampMaskGuid()
{
    static const GUID guid("11111111-2222-3333-4444-555566667777");
    return guid;
}

// ---- Bake arms -------------------------------------------------------------
// One complete bake over its own terrain/spline/world, so two arms differing only
// in HOW the same content is authored can be compared without their service
// handles colliding, and the returned copies outlive the services.
struct BakeArm
{
    std::vector<float32> BaseHeights;
    std::vector<float32> BakedHeights;
    std::vector<uint8> BaseSplat;
    std::vector<uint8> BakedSplat;
};

// `author` receives the world and must create the modifier entities. It may also
// create the spline entity (the spline service is live).
BakeArm Bake(const std::function<void(ECS::World&, SplineECS::SplineService&)>& author,
             bool seedStampMask = false)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    BakeArm arm{};
    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    if (!data)
        return arm;
    const float32* base = data->Heightfield.GetRawSamples();
    arm.BaseHeights.assign(base, base + data->Heightfield.GetSampleCount());
    arm.BaseSplat = data->Splatmap;

    ECS::World world;
    CreateTerrainEntity(world, handle, 0.0f);
    author(world, splineSvc);

    TerrainModifierSystem system;
    if (seedStampMask)
        system.SeedDecodedStampMaskForTests(StampMaskGuid(), MakeGradientMask(33, 33), 33, 33);
    system.Update(world, 1.0f / 60.0f);

    const float32* baked = data->Heightfield.GetRawSamples();
    arm.BakedHeights.assign(baked, baked + data->Heightfield.GetSampleCount());
    arm.BakedSplat = data->Splatmap;
    return arm;
}

// A bake arm for the WORLD-HEIGHT contract: the terrain entity sits at `terrainWorldY`, and the
// samples come back already converted to world Y — `normalized * kHeightScale + terrainWorldY`,
// the composition every consumer of the field applies (CBT's surface, the editor height query,
// the collider).
//
// A separate arm rather than a flag on Bake, because the QUANTITY differs: BakeArm's normalized
// samples cannot express this contract at all. Two terrains at different Y that flatten to the
// same world height hold DIFFERENT normalized bytes by construction, so a normalized comparison
// would report a correct bake as a mismatch and an origin-blind one as a match.
std::vector<float32> BakeWorldHeights(
    const std::function<void(ECS::World&, SplineECS::SplineService&)>& author,
    float32 terrainWorldY)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    if (!data)
        return {};

    ECS::World world;
    CreateTerrainEntity(world, handle, terrainWorldY);
    author(world, splineSvc);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const float32* baked = data->Heightfield.GetRawSamples();
    std::vector<float32> worldHeights(data->Heightfield.GetSampleCount());
    for (std::size_t i = 0; i < worldHeights.size(); ++i)
        worldHeights[i] = baked[i] * kHeightScale + terrainWorldY;
    return worldHeights;
}

// A sphere bake arm. Answers "did the planet change" by scanning the sculpt store
// over a fan of directions spanning the modifier's footprint around +Y and
// returning the largest |offset| found.
//
// One direction would not do: fBM is exactly zero at its lattice corners AND at
// the tangent-plane origin, so the modifier's own centre direction reads
// unmodified for a noise effect however well it baked. The fan is the aggregate
// instrument, the sphere analogue of the changed-sample set on the planar side.
float32 BakeSphereMaxAbsOffset(const std::function<void(ECS::World&, SplineECS::SplineService&)>& author)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    ECS::World world;
    CreatePlanetEntity(world, handle);
    author(world, splineSvc);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // The footprints under test are <= ~150 m on a 2 km planet, so 0.08 rad of
    // angular half-extent covers them with room to spare.
    constexpr int32 kFan = 9;
    constexpr float32 kHalfAngle = 0.08f;
    float32 maxAbs = 0.0f;
    for (int32 iz = 0; iz < kFan; ++iz)
        for (int32 ix = 0; ix < kFan; ++ix)
        {
            const float32 ax = kHalfAngle * (2.0f * static_cast<float32>(ix) / (kFan - 1) - 1.0f);
            const float32 az = kHalfAngle * (2.0f * static_cast<float32>(iz) / (kFan - 1) - 1.0f);
            const float32 len = std::sqrt(ax * ax + 1.0f + az * az);
            const float32 h = terrainSvc.SampleSphereSculptHeight(ax / len, 1.0f / len, az / len, 0.0f);
            maxAbs = std::max(maxAbs, std::abs(h));
        }
    return maxAbs;
}

bool BytewiseEqual(const std::vector<float32>& a, const std::vector<float32>& b)
{
    return a.size() == b.size()
        && std::memcmp(a.data(), b.data(), a.size() * sizeof(float32)) == 0;
}

bool BytewiseEqual(const std::vector<uint8>& a, const std::vector<uint8>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// The SET of samples whose height moved. An aggregate is the honest instrument:
// the gradient-noise lattice is exactly zero at its integer corners, so any single
// hand-picked sample can read "unmodified" however well the shape resolved.
std::vector<std::size_t> ChangedSamples(const std::vector<float32>& before,
                                        const std::vector<float32>& after)
{
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] != after[i])
            changed.push_back(i);
    return changed;
}

void SampleWorldXZ(std::size_t index, float32& outX, float32& outZ)
{
    const float32 spacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    outX = static_cast<float32>(index % kHeightmapDim) * spacing - kWorldSize * 0.5f;
    outZ = static_cast<float32>(index / kHeightmapDim) * spacing - kWorldSize * 0.5f;
}

std::size_t SampleIndexAt(float32 worldX, float32 worldZ)
{
    const float32 spacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    const auto col = static_cast<std::size_t>((worldX + kWorldSize * 0.5f) / spacing + 0.5f);
    const auto row = static_cast<std::size_t>((worldZ + kWorldSize * 0.5f) / spacing + 0.5f);
    return row * kHeightmapDim + col;
}

// ---- Authoring helpers: the same content, two ways ------------------------

constexpr float32 kCircleRadius = 40.0f;
constexpr float32 kCircleFalloff = 12.0f;
constexpr float32 kModX = -20.0f;
constexpr float32 kModZ = 15.0f;
constexpr float32 kModY = 30.0f;

// A circle-shaped volume entity at the shared placement, with no effects yet.
ECS::EntityHandle CreateCircleVolume(ECS::World& world, float32 priority = 0.0f)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = kCircleRadius;
    vol.Falloff = kCircleFalloff;
    vol.Priority = priority;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(kModX, kModY, kModZ));
    return e;
}

// Author a pre-volume modifier the ONLY way a scene can still express one: as the
// named property block a legacy scene file carries, applied through the registered
// schema. The schema converts it to a volume + effect on the spot, so what these
// oracles compare is the MIGRATION's output against the same content authored
// directly — the contract that keeps a legacy scene baking what it always baked.
//
// The shape block is spelled from the same constants CreateCircleVolume uses, so
// the two arms cannot drift into describing different regions.
// 9 significant digits is the round-trip width for binary32: the text these
// oracles feed the schema must parse back to the SAME float the volume arm is
// built from, or a byte-identical bake comparison would fail on the formatting
// rather than on the migration.
std::string FormatF32(float32 v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return std::string(buf);
}

// Apply one legacy block to an existing entity. ApplyProperties is the grouped
// hook the scene loader uses, and the only one that sees a whole block at once.
void ApplyLegacyBlock(ECS::World& world, ECS::EntityHandle entity,
                      std::string_view componentName,
                      const std::vector<std::pair<std::string, std::string>>& block)
{
    Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = Scene::SceneSchemaRegistry::Find(componentName);
    ASSERT_NE(schema, nullptr) << componentName;

    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    pairs.reserve(block.size());
    for (const auto& kv : block)
        pairs.emplace_back(kv.first, kv.second);

    Scene::SceneLoadContext ctx{};
    std::string err;
    std::size_t failed = 0;
    ASSERT_TRUE(schema->ApplyProperties(world, entity, ctx, pairs, &err, &failed))
        << componentName << " property " << failed << ": " << err;
}

// The pre-volume common shape block, spelled to match CreateCircleVolume exactly.
std::vector<std::pair<std::string, std::string>> LegacyCircleShapeProps(float32 priority = 0.0f)
{
    return {{"shape", "0"}, // TerrainModifierShape::Circle
            {"radius", FormatF32(kCircleRadius)},
            {"recthalfx", FormatF32(kCircleRadius)},
            {"recthalfz", FormatF32(kCircleRadius)},
            {"falloff", FormatF32(kCircleFalloff)},
            {"priority", FormatF32(priority)}};
}

ECS::EntityHandle CreateMigratedLegacyModifier(
    ECS::World& world, std::string_view componentName,
    const std::vector<std::pair<std::string, std::string>>& props,
    float32 priority = 0.0f)
{
    auto e = world.CreateEntity();
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(kModX, kModY, kModZ));

    std::vector<std::pair<std::string, std::string>> block = LegacyCircleShapeProps(priority);
    block.insert(block.end(), props.begin(), props.end());
    ApplyLegacyBlock(world, e, componentName, block);
    return e;
}

Components::TerrainNoiseEffect SharedNoiseEffect()
{
    Components::TerrainNoiseEffect fx{};
    fx.Blend = Components::TerrainModifierBlend::Add;
    fx.Frequency = 6.0f;
    fx.Amplitude = 18.0f;
    fx.Octaves = 3;
    fx.Seed = 91u;
    fx.Lacunarity = 2.1f;
    fx.Persistence = 0.45f;
    return fx;
}

// The same noise, spelled as the property block a legacy scene carries. Folded
// from SharedNoiseEffect so the two arms cannot drift into different noise.
std::vector<std::pair<std::string, std::string>> SharedNoiseProps()
{
    const auto fx = SharedNoiseEffect();
    return {{"blend", std::to_string(static_cast<uint32>(fx.Blend))},
            {"frequency", FormatF32(fx.Frequency)},
            {"amplitude", FormatF32(fx.Amplitude)},
            {"octaves", std::to_string(fx.Octaves)},
            {"seed", std::to_string(fx.Seed)},
            {"lacunarity", FormatF32(fx.Lacunarity)},
            {"persistence", FormatF32(fx.Persistence)}};
}

// A straight band along +X at Z = 0 spanning x = -80..80, authored at height y.
std::vector<Mathematics::Vector3> StraightSplinePoints(float32 y)
{
    return {{-80.0f, y, 0.0f}, {-20.0f, y, 0.0f}, {20.0f, y, 0.0f}, {80.0f, y, 0.0f}};
}

// A closed loop enclosing the square [-50, 50]^2, authored at height y.
std::vector<Mathematics::Vector3> ClosedLoopPoints(float32 y)
{
    return {{-50.0f, y, -50.0f}, {50.0f, y, -50.0f}, {50.0f, y, 50.0f}, {-50.0f, y, 50.0f}};
}

} // namespace

// ---------------------------------------------------------------------------
// 1 · Migration parity — the same content, authored both ways, byte-identical
// ---------------------------------------------------------------------------

TEST(TerrainVolumeMigration, FlattenAbsoluteTargetMatchesLegacyModifier)
{
    constexpr float32 kTarget = 42.0f;

    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateMigratedLegacyModifier(w, "TerrainFlattenModifier",
                                     {{"targetheight", "42"}, {"useentityheight", "false"}});
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = kTarget;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    });

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty())
        << "the migrated arm baked nothing — the parity assertion below would be vacuous";
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights))
        << "volume + flatten effect is not byte-identical to the migrated flatten block";
}

TEST(TerrainVolumeMigration, FlattenToEntityHeightMatchesLegacyModifier)
{
    // The legacy block ignored targetHeight when useEntityHeight was set; the
    // effect expresses that as a zero offset from the volume's reference height.
    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateMigratedLegacyModifier(w, "TerrainFlattenModifier",
                                     {{"targetheight", "42"}, {"useentityheight", "true"}});
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = true;
        fx.TargetHeight = 0.0f;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    });

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights));
}

TEST(TerrainVolumeMigration, NoiseMatchesLegacyModifier)
{
    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateMigratedLegacyModifier(w, "TerrainNoiseModifier", SharedNoiseProps());
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
    });

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights));
}

TEST(TerrainVolumeMigration, StampWithMaskMatchesLegacyModifier)
{
    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateMigratedLegacyModifier(
            w, "TerrainStampModifier",
            {{"blend", std::to_string(static_cast<uint32>(Components::TerrainModifierBlend::Add))},
             {"heightscale", "22"},
             {"rotation", "37"},
             // The schema takes an asset slot as a quoted GUID string.
             {"stampasset", "\"" + StampMaskGuid().ToString() + "\""}});
    }, /*seedStampMask=*/true);

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainStampEffect fx{};
        fx.Blend = Components::TerrainModifierBlend::Add;
        fx.HeightScale = 22.0f;
        fx.Rotation = 37.0f;
        fx.StampAssetGuid.Set(StampMaskGuid());
        w.AddComponentImmediate<Components::TerrainStampEffect>(e, fx);
    }, /*seedStampMask=*/true);

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights));
}

TEST(TerrainVolumeMigration, PaintLayerMatchesLegacyModifier)
{
    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateMigratedLegacyModifier(w, "TerrainPaintLayerModifier",
                                     {{"layerindex", "2"},
                                      {"strength", "0.65"},
                                      {"replace", "true"}});
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainPaintLayerEffect fx{};
        fx.LayerIndex = 2;
        fx.Strength = 0.65f;
        fx.Replace = true;
        w.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, fx);
    });

    EXPECT_FALSE(BytewiseEqual(legacy.BaseSplat, legacy.BakedSplat))
        << "the legacy arm painted nothing — the parity assertion below would be vacuous";
    EXPECT_TRUE(BytewiseEqual(legacy.BakedSplat, volume.BakedSplat))
        << "volume + paint effect is not byte-identical to the paint layer modifier";
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights))
        << "a paint-only volume must leave the heightfield alone, exactly as PaintLayer does";
}

// The spline modifier is volume(SplinePath) + flatten, and its HeightOffset is the
// flatten effect's offset from the volume reference height (the spline's own Y).
TEST(TerrainVolumeMigration, SplineFlattenMatchesLegacySplineModifier)
{
    constexpr float32 kSplineY = 24.0f;
    constexpr float32 kOffset = -6.0f;
    constexpr float32 kFalloff = 7.0f;
    constexpr float32 kRadius = 12.0f;

    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(kSplineY), kRadius, false);
        ApplyLegacyBlock(w, e, "TerrainSplineModifier",
                         {{"falloff", FormatF32(kFalloff)},
                          {"flatten", "true"},
                          {"heightoffset", FormatF32(kOffset)}});
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(kSplineY), kRadius, false);
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::SplinePath;
        vol.Falloff = kFalloff;
        w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = true;
        fx.TargetHeight = kOffset;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    });

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights));
}

TEST(TerrainVolumeMigration, SplineOffsetModeMatchesLegacySplineModifier)
{
    constexpr float32 kSplineY = 24.0f;
    constexpr float32 kOffset = 9.0f;
    constexpr float32 kFalloff = 7.0f;
    constexpr float32 kRadius = 12.0f;

    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(kSplineY), kRadius, false);
        ApplyLegacyBlock(w, e, "TerrainSplineModifier",
                         {{"falloff", FormatF32(kFalloff)},
                          {"flatten", "false"},
                          {"heightoffset", FormatF32(kOffset)}});
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(kSplineY), kRadius, false);
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::SplinePath;
        vol.Falloff = kFalloff;
        w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        Components::TerrainHeightOffsetEffect fx{};
        fx.Offset = kOffset;
        fx.Blend = Components::TerrainModifierBlend::Add;
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
    });

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights));
}

// Shape::Spline on a shared-shape modifier (#832) is volume(SplinePath) + effect.
TEST(TerrainVolumeMigration, SplineShapedNoiseMatchesLegacyShapeSpline)
{
    constexpr float32 kFalloff = 4.0f;
    constexpr float32 kRadius = 10.0f;

    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(0.0f), kRadius, false);
        auto block = SharedNoiseProps();
        block.emplace_back("shape", "2"); // TerrainModifierShape::Spline
        block.emplace_back("falloff", FormatF32(kFalloff));
        ApplyLegacyBlock(w, e, "TerrainNoiseModifier", block);
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(0.0f), kRadius, false);
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::SplinePath;
        vol.Falloff = kFalloff;
        w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
    });

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights));
}

// Shape::Spline resolved through the XZ SDF, which FILLS a closed loop — so the
// exact migration of a spline-shaped modifier is SplineArea, not SplinePath.
// Pins the schema layer's mapping choice: with SplinePath, a closed-loop scene
// would silently lose its interior on load.
TEST(TerrainVolumeMigration, ClosedSplineShapeMigratesToSplineAreaNotPath)
{
    constexpr float32 kFalloff = 4.0f;
    constexpr float32 kRadius = 6.0f;

    auto legacyAuthor = [](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, ClosedLoopPoints(0.0f), kRadius, /*closed=*/true);
        auto block = SharedNoiseProps();
        block.emplace_back("shape", "2"); // TerrainModifierShape::Spline
        block.emplace_back("falloff", FormatF32(kFalloff));
        ApplyLegacyBlock(w, e, "TerrainNoiseModifier", block);
    };
    auto volumeAuthor = [](Components::TerrainVolumeShape shape) {
        return [shape](ECS::World& w, SplineECS::SplineService& svc) {
            const auto e = CreateSplineEntity(w, svc, ClosedLoopPoints(0.0f), kRadius, /*closed=*/true);
            Components::TerrainModifierVolume vol{};
            vol.Shape = shape;
            vol.Falloff = kFalloff;
            w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
            w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
        };
    };

    const BakeArm legacy = Bake(legacyAuthor);
    const BakeArm area = Bake(volumeAuthor(Components::TerrainVolumeShape::SplineArea));
    const BakeArm path = Bake(volumeAuthor(Components::TerrainVolumeShape::SplinePath));

    EXPECT_FALSE(ChangedSamples(legacy.BaseHeights, legacy.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, area.BakedHeights))
        << "SplineArea must reproduce Shape::Spline exactly on a closed loop";
    EXPECT_FALSE(BytewiseEqual(legacy.BakedHeights, path.BakedHeights))
        << "SplinePath equals Shape::Spline here — then the two shapes are one behaviour "
           "and the migration mapping is untested";
}

// ---------------------------------------------------------------------------
// 1b · Flatten's unit — TargetHeight is a WORLD Y, not a height above the terrain
// ---------------------------------------------------------------------------
//
// The discriminating case is a terrain that does not sit at Y = 0. Every other test in this
// file bakes at the origin, where the terrain's own translation is zero and an origin-blind
// bake is indistinguishable from a correct one. Here the SAME authored content is baked on a
// terrain at Y = 0 and on one translated in Y, and the flattened ground has to come out at
// the same WORLD height both times — the height the author typed.
//
// Each test carries its own positive control: the ground OUTSIDE the volume must move with
// the terrain by exactly the translation. Without it, an arm that baked nothing at all (or a
// harness that silently ignored the terrain's Y) would satisfy the equality vacuously.

namespace
{
// Sample indices deep inside the shared circle volume, where the shape ramp is exactly 1 and a
// Set blend lands precisely on the target. Kept clear of the falloff band so the assertion is
// about the TARGET, not about the ramp.
std::vector<std::size_t> VolumeCoreSamples()
{
    std::vector<std::size_t> core;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kHeightmapDim) * kHeightmapDim; ++i)
    {
        float32 x = 0.0f, z = 0.0f;
        SampleWorldXZ(i, x, z);
        const float32 dx = x - kModX;
        const float32 dz = z - kModZ;
        if (std::sqrt(dx * dx + dz * dz) < kCircleRadius - 10.0f)
            core.push_back(i);
    }
    return core;
}

// Samples far outside the circle (shape weight 0), which no effect touches.
std::vector<std::size_t> UntouchedSamples()
{
    std::vector<std::size_t> outside;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kHeightmapDim) * kHeightmapDim; ++i)
    {
        float32 x = 0.0f, z = 0.0f;
        SampleWorldXZ(i, x, z);
        const float32 dx = x - kModX;
        const float32 dz = z - kModZ;
        if (std::sqrt(dx * dx + dz * dz) > kCircleRadius + kCircleFalloff + 10.0f)
            outside.push_back(i);
    }
    return outside;
}

constexpr float32 kTerrainShiftY = 37.0f;
constexpr float32 kWorldHeightTolerance = 1e-3f;
} // namespace

TEST(TerrainFlattenWorldHeight, AbsoluteTargetLandsOnTheSameWorldHeightWhereverTheTerrainSits)
{
    constexpr float32 kTarget = 21.5f;

    const auto author = [](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = kTarget;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    };

    const std::vector<float32> atOrigin = BakeWorldHeights(author, 0.0f);
    const std::vector<float32> raised = BakeWorldHeights(author, kTerrainShiftY);
    ASSERT_EQ(atOrigin.size(), raised.size());

    const std::vector<std::size_t> core = VolumeCoreSamples();
    ASSERT_FALSE(core.empty()) << "no full-weight samples — every assertion below is vacuous";
    for (const std::size_t i : core)
    {
        EXPECT_NEAR(atOrigin[i], kTarget, kWorldHeightTolerance)
            << "flatten missed its target on a terrain at Y=0, sample " << i;
        EXPECT_NEAR(raised[i], kTarget, kWorldHeightTolerance)
            << "flatten landed " << (raised[i] - kTarget) << " m off on a terrain at Y="
            << kTerrainShiftY << " — TargetHeight is being read as a height ABOVE the terrain, "
               "not as a world Y (sample " << i << ")";
    }

    const std::vector<std::size_t> outside = UntouchedSamples();
    ASSERT_FALSE(outside.empty());
    for (const std::size_t i : outside)
        ASSERT_NEAR(raised[i] - atOrigin[i], kTerrainShiftY, kWorldHeightTolerance)
            << "unmodified ground did not move with the terrain — the two arms are not actually "
               "at different world Y, so the equality above proves nothing (sample " << i << ")";
}

TEST(TerrainFlattenWorldHeight, VolumeReferenceFlattenLandsOnTheSameWorldHeightWhereverTheTerrainSits)
{
    // useVolumeHeight resolves the volume's OWN world Y (kModY here) and adds the authored
    // offset — so it is a world Y too, and must be just as independent of where the terrain
    // sits. This is the mode the spline road/river content uses.
    constexpr float32 kOffset = 6.0f;

    const auto author = [](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = true;
        fx.TargetHeight = kOffset;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    };

    const std::vector<float32> atOrigin = BakeWorldHeights(author, 0.0f);
    const std::vector<float32> raised = BakeWorldHeights(author, kTerrainShiftY);
    ASSERT_EQ(atOrigin.size(), raised.size());

    const float32 expected = kModY + kOffset;
    const std::vector<std::size_t> core = VolumeCoreSamples();
    ASSERT_FALSE(core.empty());
    for (const std::size_t i : core)
    {
        EXPECT_NEAR(atOrigin[i], expected, kWorldHeightTolerance)
            << "volume-reference flatten missed the volume's own height + offset, sample " << i;
        EXPECT_NEAR(raised[i], expected, kWorldHeightTolerance)
            << "volume-reference flatten landed " << (raised[i] - expected)
            << " m off on a terrain at Y=" << kTerrainShiftY
            << " — the volume's world Y is being folded in twice (sample " << i << ")";
    }

    const std::vector<std::size_t> outside = UntouchedSamples();
    ASSERT_FALSE(outside.empty());
    for (const std::size_t i : outside)
        ASSERT_NEAR(raised[i] - atOrigin[i], kTerrainShiftY, kWorldHeightTolerance)
            << "unmodified ground did not move with the terrain (sample " << i << ")";
}

// ---------------------------------------------------------------------------
// 2 · Stacking — the order is real, reproducible, and equals the priority chain
// ---------------------------------------------------------------------------

namespace
{
// A circle volume carrying a flatten and a noise effect at the given stack
// orders, so the pair can be authored in either sequence.
void AuthorFlattenAndNoiseVolume(ECS::World& w, int32 flattenOrder, int32 noiseOrder)
{
    const auto e = CreateCircleVolume(w);
    Components::TerrainFlattenEffect flatten{};
    flatten.StackOrder = flattenOrder;
    flatten.UseVolumeHeight = false;
    flatten.TargetHeight = 42.0f;
    w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);

    auto noise = SharedNoiseEffect();
    noise.StackOrder = noiseOrder;
    w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, noise);
}
} // namespace

// The order-dependent pair. Flatten lerps toward an absolute target, so noise
// added BEFORE it is partly erased, while noise added AFTER it survives whole.
// If the stack order were ignored (or fixed by component type), these two bakes
// would be identical and this test fails.
TEST(TerrainVolumeStacking, FlattenThenNoiseDiffersFromNoiseThenFlatten)
{
    const BakeArm flattenFirst = Bake([](ECS::World& w, SplineECS::SplineService&) {
        AuthorFlattenAndNoiseVolume(w, /*flattenOrder=*/0, /*noiseOrder=*/1);
    });
    const BakeArm noiseFirst = Bake([](ECS::World& w, SplineECS::SplineService&) {
        AuthorFlattenAndNoiseVolume(w, /*flattenOrder=*/1, /*noiseOrder=*/0);
    });

    EXPECT_FALSE(ChangedSamples(flattenFirst.BaseHeights, flattenFirst.BakedHeights).empty());
    EXPECT_FALSE(BytewiseEqual(flattenFirst.BakedHeights, noiseFirst.BakedHeights))
        << "swapping StackOrder produced an identical bake — the stack order is not applied";
}

// Same stack, baked twice: the effect order must be a function of the data, not
// of ECS component iteration order or of which effect was added first.
TEST(TerrainVolumeStacking, StackOrderIsDeterministicAcrossBakes)
{
    const BakeArm first = Bake([](ECS::World& w, SplineECS::SplineService&) {
        AuthorFlattenAndNoiseVolume(w, 0, 1);
    });
    // Author the components in the opposite ADD order, same StackOrders.
    const BakeArm second = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        auto noise = SharedNoiseEffect();
        noise.StackOrder = 1;
        w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, noise);
        Components::TerrainFlattenEffect flatten{};
        flatten.StackOrder = 0;
        flatten.UseVolumeHeight = false;
        flatten.TargetHeight = 42.0f;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);
    });

    EXPECT_TRUE(BytewiseEqual(first.BakedHeights, second.BakedHeights))
        << "the stack order depends on component ADD order, not on StackOrder";
}

// The stack within a volume is the same sequencing the pre-volume model expressed
// with per-modifier Priority — so a migrated two-effect region is byte-identical
// to the two components it replaces.
TEST(TerrainVolumeStacking, StackOrderMatchesLegacyPriorityChain)
{
    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateMigratedLegacyModifier(w, "TerrainFlattenModifier",
                                     {{"targetheight", "42"}, {"useentityheight", "false"}},
                                     /*priority=*/0.0f);
        CreateMigratedLegacyModifier(w, "TerrainNoiseModifier", SharedNoiseProps(),
                                     /*priority=*/1.0f);
    });

    const BakeArm volume = Bake([](ECS::World& w, SplineECS::SplineService&) {
        AuthorFlattenAndNoiseVolume(w, /*flattenOrder=*/0, /*noiseOrder=*/1);
    });

    EXPECT_TRUE(BytewiseEqual(legacy.BakedHeights, volume.BakedHeights))
        << "a two-effect volume is not byte-identical to the two priority-ordered modifiers";
}

// Cross-volume ordering stays Priority, independent of the within-volume stack.
TEST(TerrainVolumeStacking, VolumePriorityOrdersVolumesAgainstEachOther)
{
    // Two overlapping volumes, each with one flatten to a different target. The
    // higher-priority one runs last and therefore dominates.
    auto author = [](float32 priorityA, float32 priorityB) {
        return [priorityA, priorityB](ECS::World& w, SplineECS::SplineService&) {
            const auto a = CreateCircleVolume(w, priorityA);
            Components::TerrainFlattenEffect fxA{};
            fxA.UseVolumeHeight = false;
            fxA.TargetHeight = 10.0f;
            w.AddComponentImmediate<Components::TerrainFlattenEffect>(a, fxA);

            const auto b = CreateCircleVolume(w, priorityB);
            Components::TerrainFlattenEffect fxB{};
            fxB.UseVolumeHeight = false;
            fxB.TargetHeight = 50.0f;
            w.AddComponentImmediate<Components::TerrainFlattenEffect>(b, fxB);
        };
    };

    const BakeArm aFirst = Bake(author(0.0f, 1.0f));
    const BakeArm bFirst = Bake(author(1.0f, 0.0f));

    const std::size_t center = SampleIndexAt(kModX, kModZ);
    ASSERT_LT(center, aFirst.BakedHeights.size());
    // At the shared centre both volumes have weight 1, so the last one wins outright.
    EXPECT_NEAR(aFirst.BakedHeights[center], 50.0f / kHeightScale, 1e-5f);
    EXPECT_NEAR(bFirst.BakedHeights[center], 10.0f / kHeightScale, 1e-5f);
}

// ---------------------------------------------------------------------------
// 3 · Shape — the volume owns the region
// ---------------------------------------------------------------------------

// A closed spline: SplineArea fills what the loop encloses, SplinePath does not.
// This is the whole reason the two shapes exist as distinct values.
TEST(TerrainVolumeShape, SplineAreaFillsClosedLoopInteriorAndSplinePathDoesNot)
{
    auto author = [](Components::TerrainVolumeShape shape) {
        return [shape](ECS::World& w, SplineECS::SplineService& svc) {
            const auto e = CreateSplineEntity(w, svc, ClosedLoopPoints(0.0f), 6.0f, /*closed=*/true);
            Components::TerrainModifierVolume vol{};
            vol.Shape = shape;
            vol.Falloff = 4.0f;
            w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
            Components::TerrainHeightOffsetEffect fx{};
            fx.Offset = 20.0f;
            w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
        };
    };

    const BakeArm area = Bake(author(Components::TerrainVolumeShape::SplineArea));
    const BakeArm path = Bake(author(Components::TerrainVolumeShape::SplinePath));

    const std::vector<std::size_t> areaChanged = ChangedSamples(area.BaseHeights, area.BakedHeights);
    const std::vector<std::size_t> pathChanged = ChangedSamples(path.BaseHeights, path.BakedHeights);
    ASSERT_FALSE(areaChanged.empty());
    ASSERT_FALSE(pathChanged.empty());

    // The area arm covers strictly more ground: the band plus the interior.
    EXPECT_GT(areaChanged.size(), pathChanged.size())
        << "SplineArea did not cover more than SplinePath on a closed loop";

    // A sample deep inside the loop, far from the band, discriminates directly.
    const std::size_t deepInside = SampleIndexAt(0.0f, 0.0f);
    ASSERT_LT(deepInside, area.BakedHeights.size());
    EXPECT_NE(area.BakedHeights[deepInside], area.BaseHeights[deepInside])
        << "SplineArea left the loop interior untouched";
    EXPECT_EQ(path.BakedHeights[deepInside], path.BaseHeights[deepInside])
        << "SplinePath filled the loop interior — it must stay a band";
}

// On an OPEN spline there is nothing to enclose, so the two shapes must agree
// bit for bit. (The negative control for the test above: it proves the
// difference there comes from the loop, not from the two shapes taking
// different code paths in general.)
TEST(TerrainVolumeShape, SplinePathAndSplineAreaAgreeOnOpenSpline)
{
    auto author = [](Components::TerrainVolumeShape shape) {
        return [shape](ECS::World& w, SplineECS::SplineService& svc) {
            const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(0.0f), 10.0f, false);
            Components::TerrainModifierVolume vol{};
            vol.Shape = shape;
            vol.Falloff = 4.0f;
            w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
            w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
        };
    };

    const BakeArm area = Bake(author(Components::TerrainVolumeShape::SplineArea));
    const BakeArm path = Bake(author(Components::TerrainVolumeShape::SplinePath));

    EXPECT_FALSE(ChangedSamples(path.BaseHeights, path.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(area.BakedHeights, path.BakedHeights));
}

// The inward feather: with it, the weight ramps up from the shape edge, so the
// rim moves LESS than the centre. Without it the interior is a flat 1.
TEST(TerrainVolumeShape, InwardFalloffFeathersTheInteriorRim)
{
    auto author = [](float32 falloffInward) {
        return [falloffInward](ECS::World& w, SplineECS::SplineService&) {
            auto e = w.CreateEntity();
            Components::TerrainModifierVolume vol{};
            vol.Shape = Components::TerrainVolumeShape::Circle;
            vol.Radius = kCircleRadius;
            vol.Falloff = 0.0f; // isolate the inward feather
            vol.FalloffInward = falloffInward;
            w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
            w.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
            Components::TerrainHeightOffsetEffect fx{};
            fx.Offset = 20.0f;
            w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
        };
    };

    const BakeArm hard = Bake(author(0.0f));
    const BakeArm feathered = Bake(author(20.0f));

    // Same footprint either way — the feather changes weights, not bounds.
    const std::vector<std::size_t> hardChanged = ChangedSamples(hard.BaseHeights, hard.BakedHeights);
    ASSERT_FALSE(hardChanged.empty());

    const std::size_t center = SampleIndexAt(0.0f, 0.0f);
    // 34 m from the centre: inside the 40 m radius, within the 20 m feather band.
    const std::size_t nearRim = SampleIndexAt(34.0f, 0.0f);
    ASSERT_LT(nearRim, hard.BakedHeights.size());

    const float32 hardRimDelta = hard.BakedHeights[nearRim] - hard.BaseHeights[nearRim];
    const float32 featheredRimDelta = feathered.BakedHeights[nearRim] - feathered.BaseHeights[nearRim];
    const float32 featheredCenterDelta = feathered.BakedHeights[center] - feathered.BaseHeights[center];

    EXPECT_GT(hardRimDelta, 0.0f);
    EXPECT_LT(featheredRimDelta, hardRimDelta)
        << "the inward feather did not reduce the rim contribution";
    EXPECT_NEAR(featheredCenterDelta, hardRimDelta, 1e-6f)
        << "the inward feather must leave the centre at full weight";
}

// Falloff and FalloffInward are the two halves of ONE ramp. Evaluated as two
// independent bands they disagree at the edge they share — the outward band
// reaching 1 exactly where the inward band starts from 0 — which cuts a groove
// ring with a raised skirt into an otherwise smooth plateau shoulder.
//
// The oracle is the ramp's largest ADJACENT-SAMPLE step over a fine sweep
// spanning both halves. A step is what a discontinuity is; a bound on it is the
// pin. Endpoint and monotonicity checks come along because a "continuous" ramp
// that never rises, or that peaks in the wrong place, would also pass a pure
// step bound.
TEST(TerrainVolumeShape, CombinedFalloffIsOneContinuousRampAcrossTheEdge)
{
    // The SplineVignette plateau's authored widths.
    constexpr float32 kFalloff = 6.0f;
    constexpr float32 kFalloffInward = 4.0f;

    // Fine enough that the bound below is far under any real step: smoothstep's
    // derivative peaks at 1.5/width, so a continuous ramp cannot exceed
    // 1.5 * step / width = 0.0015 per sample.
    constexpr float32 kStep = 0.01f;
    constexpr float32 kMaxAdjacentDelta = 0.002f;

    float32 previous = ShapeFalloffWeight(-kFalloff - 2.0f, kFalloff, kFalloffInward);
    float32 worstDelta = 0.0f;
    float32 worstAt = 0.0f;
    bool monotonic = true;
    for (float32 d = -kFalloff - 2.0f + kStep; d <= kFalloffInward + 2.0f; d += kStep)
    {
        const float32 w = ShapeFalloffWeight(d, kFalloff, kFalloffInward);
        EXPECT_GE(w, 0.0f);
        EXPECT_LE(w, 1.0f);
        if (w < previous - 1e-6f)
            monotonic = false;
        const float32 delta = std::abs(w - previous);
        if (delta > worstDelta)
        {
            worstDelta = delta;
            worstAt = d;
        }
        previous = w;
    }

    EXPECT_LT(worstDelta, kMaxAdjacentDelta)
        << "the weight steps by " << worstDelta << " at distFromEdge = " << worstAt
        << " m; the two falloff halves are not one continuous ramp";
    EXPECT_TRUE(monotonic) << "the ramp must rise monotonically from outside to inside";

    // The ramp's endpoints are the authored extents, and the edge itself is an
    // interior point of the ramp rather than either endpoint.
    EXPECT_FLOAT_EQ(ShapeFalloffWeight(-kFalloff, kFalloff, kFalloffInward), 0.0f);
    EXPECT_FLOAT_EQ(ShapeFalloffWeight(kFalloffInward, kFalloff, kFalloffInward), 1.0f);
    EXPECT_FLOAT_EQ(ShapeFalloffWeight(-kFalloff - 5.0f, kFalloff, kFalloffInward), 0.0f);
    EXPECT_FLOAT_EQ(ShapeFalloffWeight(kFalloffInward + 5.0f, kFalloff, kFalloffInward), 1.0f);

    const float32 atEdge = ShapeFalloffWeight(0.0f, kFalloff, kFalloffInward);
    EXPECT_GT(atEdge, 0.0f);
    EXPECT_LT(atEdge, 1.0f);
}

// Setting only ONE falloff must evaluate the expressions it always did, to the
// bit — the single-falloff scenes are not supposed to shift by an LSB. The
// oracle is the literal pre-existing formula, written out here so a rewrite of
// the shared helper that is merely CLOSE fails.
TEST(TerrainVolumeShape, SingleFalloffRampIsBitIdenticalToTheOriginalFormula)
{
    auto smooth = [](float32 t) { return t * t * (3.0f - 2.0f * t); };
    constexpr float32 kFalloff = 12.0f;
    constexpr float32 kFalloffInward = 7.0f;

    for (float32 d = -20.0f; d <= 20.0f; d += 0.013f)
    {
        // Outward only: rises to a flat interior that begins exactly at the edge.
        const float32 outwardExpected = d <= 0.0f
            ? smooth(std::clamp(1.0f + d / kFalloff, 0.0f, 1.0f))
            : 1.0f;
        EXPECT_EQ(ShapeFalloffWeight(d, kFalloff, 0.0f), outwardExpected)
            << "outward-only ramp changed at distFromEdge = " << d;

        // Inward only: a hard outer edge, rising inward from zero.
        const float32 inwardExpected = d <= 0.0f
            ? 0.0f
            : smooth(std::clamp(d / kFalloffInward, 0.0f, 1.0f));
        EXPECT_EQ(ShapeFalloffWeight(d, 0.0f, kFalloffInward), inwardExpected)
            << "inward-only ramp changed at distFromEdge = " << d;

        // Neither: a binary in/out mask, the edge itself reading outside.
        EXPECT_EQ(ShapeFalloffWeight(d, 0.0f, 0.0f), d <= 0.0f ? 0.0f : 1.0f)
            << "binary mask changed at distFromEdge = " << d;
    }
}

// The same defect end to end, in the quantity the user actually sees: the height
// profile crossing the rim of a volume that sets BOTH falloffs. The baked
// contribution is Offset * weight, so a step in the weight is a step in the
// terrain — the "goes down, then a sharp up again" the plateau showed.
TEST(TerrainVolumeShape, CombinedFalloffBakesARimWithNoStep)
{
    // Wide enough that the ramp spans ~20 heightmap samples at the 2 m spacing,
    // so a continuous rim is well resolved and a discontinuous one is unmissable.
    constexpr float32 kRadius = 40.0f;
    constexpr float32 kFalloff = 24.0f;
    constexpr float32 kFalloffInward = 16.0f;
    constexpr float32 kOffset = 20.0f;

    const BakeArm arm = Bake([](ECS::World& w, SplineECS::SplineService&) {
        auto e = w.CreateEntity();
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::Circle;
        vol.Radius = kRadius;
        vol.Falloff = kFalloff;
        vol.FalloffInward = kFalloffInward;
        w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        w.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
        Components::TerrainHeightOffsetEffect fx{};
        fx.Offset = kOffset;
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
    });
    ASSERT_FALSE(arm.BakedHeights.empty());

    // Walk outward along +X through the rim, from the flat interior to past the
    // outer extent. The modifier's contribution is the baked-minus-base delta,
    // which isolates it from the base fill's own relief.
    const float32 spacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    const float32 rampWidth = kFalloff + kFalloffInward;
    // Heightfield samples are NORMALISED by the terrain's height scale, so a
    // world-metre offset lands as offset / kHeightScale.
    const float32 fullWeightDelta = kOffset / kHeightScale;
    // smoothstep's peak slope is 1.5/width, scaled by the contribution, plus a
    // margin for the sampling grid not landing on the extrema.
    const float32 maxExpectedStep = fullWeightDelta * 1.5f * spacing / rampWidth * 1.5f;

    float32 previousDelta = 0.0f;
    float32 worstStep = 0.0f;
    float32 worstAtX = 0.0f;
    bool first = true;
    for (float32 x = 0.0f; x <= kRadius + kFalloff + 4.0f; x += spacing)
    {
        const std::size_t index = SampleIndexAt(x, 0.0f);
        ASSERT_LT(index, arm.BakedHeights.size());
        const float32 delta = arm.BakedHeights[index] - arm.BaseHeights[index];
        if (!first && std::abs(delta - previousDelta) > worstStep)
        {
            worstStep = std::abs(delta - previousDelta);
            worstAtX = x;
        }
        previousDelta = delta;
        first = false;
    }

    // The interior is at full offset and the outside is untouched — without this
    // a rim that baked nothing at all would trivially have no step.
    const float32 centerDelta =
        arm.BakedHeights[SampleIndexAt(0.0f, 0.0f)] - arm.BaseHeights[SampleIndexAt(0.0f, 0.0f)];
    const float32 outsideDelta =
        arm.BakedHeights[SampleIndexAt(kRadius + kFalloff + 4.0f, 0.0f)]
        - arm.BaseHeights[SampleIndexAt(kRadius + kFalloff + 4.0f, 0.0f)];
    EXPECT_NEAR(centerDelta, fullWeightDelta, 1e-6f)
        << "the interior must reach full weight";
    EXPECT_NEAR(outsideDelta, 0.0f, 1e-6f)
        << "past the outer extent the volume must contribute nothing";

    EXPECT_LT(worstStep, maxExpectedStep)
        << "the baked rim steps by " << worstStep << " (normalised) near x = " << worstAtX
        << " m, bound " << maxExpectedStep << ": the falloff bands are discontinuous";
}

TEST(TerrainVolumeShape, MasterWeightScalesTheWholeStack)
{
    auto author = [](float32 weight) {
        return [weight](ECS::World& w, SplineECS::SplineService&) {
            auto e = w.CreateEntity();
            Components::TerrainModifierVolume vol{};
            vol.Shape = Components::TerrainVolumeShape::Circle;
            vol.Radius = kCircleRadius;
            vol.Falloff = 0.0f;
            vol.Weight = weight;
            w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
            w.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
            Components::TerrainHeightOffsetEffect fx{};
            fx.Offset = 20.0f;
            w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
        };
    };

    const BakeArm full = Bake(author(1.0f));
    const BakeArm half = Bake(author(0.5f));
    const BakeArm zero = Bake(author(0.0f));

    const std::size_t center = SampleIndexAt(0.0f, 0.0f);
    const float32 fullDelta = full.BakedHeights[center] - full.BaseHeights[center];
    const float32 halfDelta = half.BakedHeights[center] - half.BaseHeights[center];

    EXPECT_GT(fullDelta, 0.0f);
    EXPECT_NEAR(halfDelta, fullDelta * 0.5f, 1e-6f);
    EXPECT_TRUE(BytewiseEqual(zero.BaseHeights, zero.BakedHeights))
        << "Weight = 0 must leave the terrain untouched";
}

// ---------------------------------------------------------------------------
// 4 · Structural — a volume with nothing to do does nothing
// ---------------------------------------------------------------------------

TEST(TerrainVolumeStructure, VolumeWithNoEffectsChangesNothing)
{
    const BakeArm arm = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateCircleVolume(w);
    });
    EXPECT_TRUE(BytewiseEqual(arm.BaseHeights, arm.BakedHeights));
    EXPECT_TRUE(BytewiseEqual(arm.BaseSplat, arm.BakedSplat));
}

TEST(TerrainVolumeStructure, DisabledVolumeAndDisabledEffectAreExcluded)
{
    const BakeArm disabledVolume = Bake([](ECS::World& w, SplineECS::SplineService&) {
        auto e = w.CreateEntity();
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::Circle;
        vol.Radius = kCircleRadius;
        w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        ECS::Entity(&w, e).SetEnabled<Components::TerrainModifierVolume>(false);
        w.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
        Components::TerrainHeightOffsetEffect fx{};
        fx.Offset = 20.0f;
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
    });
    EXPECT_TRUE(BytewiseEqual(disabledVolume.BaseHeights, disabledVolume.BakedHeights));

    const BakeArm disabledEffect = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainHeightOffsetEffect fx{};
        fx.Offset = 20.0f;
        fx.Enabled = false;
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
    });
    EXPECT_TRUE(BytewiseEqual(disabledEffect.BaseHeights, disabledEffect.BakedHeights));
}

// A spline-shaped volume whose entity has no SplineComponent has no region: it
// must be dropped (with a warning), never fall through to the rectangle branch.
TEST(TerrainVolumeStructure, SplineShapeWithoutSplineComponentIsDropped)
{
    const BakeArm arm = Bake([](ECS::World& w, SplineECS::SplineService&) {
        auto e = w.CreateEntity();
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::SplinePath;
        vol.RectHalfX = 60.0f;
        vol.RectHalfZ = 60.0f;
        w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        w.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
        Components::TerrainHeightOffsetEffect fx{};
        fx.Offset = 20.0f;
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
    });
    EXPECT_TRUE(BytewiseEqual(arm.BaseHeights, arm.BakedHeights));
}

// ---------------------------------------------------------------------------
// 5 · Sphere guards — supported effects bake, unsupported ones stay refused
// ---------------------------------------------------------------------------

namespace
{
// Place the volume on the planet surface along +Y so its world position is a real
// direction and radius (the planet is centred at the origin).
ECS::EntityHandle CreateSphereVolume(ECS::World& w, Components::TerrainVolumeShape shape)
{
    auto e = w.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = shape;
    vol.Radius = 120.0f;
    vol.RectHalfX = 120.0f;
    vol.RectHalfZ = 120.0f;
    vol.Falloff = 30.0f;
    w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    w.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, kPlanetRadius, 0.0f));
    return e;
}
} // namespace

// Positive control: without this, the two refusal oracles below would pass on a
// sphere bake that never runs at all.
TEST(TerrainVolumeSphere, NoiseEffectVolumeBakesOnAPlanet)
{
    const float32 maxOffset = BakeSphereMaxAbsOffset([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateSphereVolume(w, Components::TerrainVolumeShape::Circle);
        w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
    });
    EXPECT_GT(maxOffset, 0.0f) << "a circle volume with a noise effect baked nothing on the planet";
}

TEST(TerrainVolumeSphere, PaintOnlyVolumeBakesNothingOnAPlanet)
{
    const float32 maxOffset = BakeSphereMaxAbsOffset([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateSphereVolume(w, Components::TerrainVolumeShape::Circle);
        Components::TerrainPaintLayerEffect fx{};
        fx.LayerIndex = 1;
        fx.Strength = 1.0f;
        w.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, fx);
    });
    EXPECT_EQ(maxOffset, 0.0f) << "paint has no sphere splat layer yet — it must not bake height";
}

TEST(TerrainVolumeSphere, SplineShapedVolumeBakesNothingOnAPlanet)
{
    const float32 maxOffset = BakeSphereMaxAbsOffset([](ECS::World& w, SplineECS::SplineService& svc) {
        const auto e = CreateSplineEntity(w, svc, StraightSplinePoints(kPlanetRadius), 40.0f, false);
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::SplinePath;
        vol.Falloff = 20.0f;
        w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
    });
    EXPECT_EQ(maxOffset, 0.0f)
        << "the spline SDF is XZ-planar — a spline-shaped volume must not bake as a rectangle";
}

// Pooling is PLANAR-ONLY. A pool accumulates (weight * target) and (weight)
// across its members over a bake REGION and applies once at its highest-priority
// member's slot; the sphere bakes cube-face rects of a sculpt layer and has no
// such region to accumulate into. SphereModifierPlacement carries no blend for a
// flatten at all, so an Average one reaching the sphere would bake as a plain
// radial level — the silent wrong answer rather than an honest refusal.
//
// Both arms are the SAME volume and the same target, differing only in the blend,
// so the refusal cannot be a volume the sphere never resolved: the Set arm is the
// positive control and it must move the planet.
TEST(TerrainVolumeSphere, PooledFlattenBakesNothingOnAPlanet)
{
    constexpr float32 kPadRadialOffset = 30.0f;
    const auto bakeWithBlend = [](Components::TerrainModifierBlend blend) {
        return BakeSphereMaxAbsOffset([blend](ECS::World& w, SplineECS::SplineService&) {
            const auto e = CreateSphereVolume(w, Components::TerrainVolumeShape::Circle);
            Components::TerrainFlattenEffect fx{};
            // An absolute radial target, so the pad is a measurable offset from
            // the nominal surface rather than a level to where the volume already
            // sits (which would bake zero on both arms).
            fx.UseVolumeHeight = false;
            fx.TargetHeight = kPadRadialOffset;
            fx.Blend = blend;
            w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
        });
    };

    EXPECT_GT(bakeWithBlend(Components::TerrainModifierBlend::Set), 0.0f)
        << "positive control: the same circular flatten with a Set blend must bake on the planet, "
           "or the Average arm below proves nothing";
    EXPECT_EQ(bakeWithBlend(Components::TerrainModifierBlend::Average), 0.0f)
        << "a pooled flatten must be refused on a spherical terrain (warned at gather, naming the "
           "entity), not baked as an unblended radial level";
}


// ---------------------------------------------------------------------------
// 6 · The change gate — every input the gather READS must also wake it
// ---------------------------------------------------------------------------
//
// The gather resolves a volume from the volume component, the entity's
// WorldTransform and the effect components stacked on the same entity. The gate
// decides whether the gather runs at all, so any of those inputs changing while
// the gate sleeps is a lost edit: the terrain keeps the previous bake, and the
// only recovery is nudging something the gate does happen to watch.
//
// Every terrain scene is a volume scene now, migrated or authored: the entity
// carries the volume (a watched root) plus its effect columns and nothing else,
// so these tests exercise the only shape there is.
//
// Two instrument traps these tests are built around:
//
//  * The quiet-frame oracle comes first in every test. With the gate wedged open
//    (or a scene that never settles) every wake assertion below passes vacuously.
//  * Every frame here is a whole engine tick — systems, THEN one lifecycle swap,
//    the cadence EngineCore::Update drives. Swapping twice around an edit makes
//    the gate's swap-generation gap exceed 1, and the structural fallback then
//    wakes the gather no matter what the wake lists contain.

namespace
{
// The volume + one noise effect — what TerrainSceneSchemas leaves behind once a
// scene load migrates a legacy noise modifier.
ECS::EntityHandle CreateMigratedNoiseVolume(ECS::World& world)
{
    const auto e = CreateCircleVolume(world);
    world.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
    return e;
}

// One engine tick: systems run, then the lifecycle window swaps exactly once.
// Lifecycle events recorded between ticks are promoted by the NEXT swap, so a
// wake driven by Added<T>/Removed<T> lands two ticks after the edit.
void Tick(ECS::World& world, TerrainModifierSystem& system)
{
    system.Update(world, 1.0f / 60.0f);
    world.SwapLifecycleEvents();
}

// Run ticks until the gate's structural fallbacks (first gated run, lifecycle
// window, terrain-state hash and epoch baselines) are consumed.
uint64 SettleGate(ECS::World& world, TerrainModifierSystem& system)
{
    for (int i = 0; i < 6; ++i)
        Tick(world, system);
    return TerrainModifierSystem::GetGatherCountForTests();
}

// Settle, then confirm the scene is genuinely quiet before an edit is applied.
// Returns the settled count for the caller to compare against.
uint64 SettleAndRequireQuiet(ECS::World& world, TerrainModifierSystem& system)
{
    const uint64 settled = SettleGate(world, system);
    for (int i = 0; i < 8; ++i)
        Tick(world, system);
    EXPECT_EQ(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "idle ticks still ran the modifier gather in a volume scene — the wake "
           "assertions in this suite would pass vacuously";
    return TerrainModifierSystem::GetGatherCountForTests();
}

std::vector<float32> CopyHeights(const TerrainData& data)
{
    const float32* s = data.Heightfield.GetRawSamples();
    return std::vector<float32>(s, s + data.Heightfield.GetSampleCount());
}

// The scenery every gate test shares: a baked terrain, its entity, and the
// lifecycle-event subscriptions the engine installs on the primary world. Taking
// those from EnableTerrainModifierLifecycleEvents rather than hand-listing them
// also pins that the canonical subscription covers what the gate scans.
struct VolumeGateScene
{
    ScopedTerrainService TerrainScope;
    ScopedSplineService SplineScope;
    TerrainHandle Handle;
    TerrainData* Data = nullptr;
    ECS::World World;
    TerrainModifierSystem System;

    VolumeGateScene()
    {
        auto& svc = TerrainService::Get();
        Handle = CreateBakedTerrain(svc);
        Data = svc.GetTerrainData(Handle);
        EnableTerrainModifierLifecycleEvents(World);
        CreateTerrainEntity(World, Handle, 0.0f);
    }
};
} // namespace

// Control. Every test below compares against a settled count, so if a volume
// scene never goes quiet this suite proves nothing — and a gate "fixed" by
// waking unconditionally would satisfy all of them.
TEST(TerrainVolumeGate, IdleVolumeSceneStopsGathering)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);
    CreateMigratedNoiseVolume(scene.World);

    const uint64 settled = SettleGate(scene.World, scene.System);
    for (int i = 0; i < 8; ++i)
        Tick(scene.World, scene.System);
    EXPECT_EQ(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "idle ticks still ran the modifier gather in a volume scene";
}

// The headline escape: editing the volume's own region fields while the gate
// sleeps. Falloff is the field an inspector drag writes.
TEST(TerrainVolumeGate, VolumeFalloffEditWakesTheGatherAndRebakes)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);
    const auto volume = CreateMigratedNoiseVolume(scene.World);

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    auto* vol = scene.World.GetComponentForWrite<Components::TerrainModifierVolume>(volume);
    ASSERT_NE(vol, nullptr);
    vol->Falloff = kCircleFalloff * 2.5f;

    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "editing a volume's falloff did not wake the modifier gather";
    EXPECT_FALSE(BytewiseEqual(beforeEdit, CopyHeights(*scene.Data)))
        << "editing a volume's falloff did not re-bake the terrain";
}

// Moving the volume. The transform probe is scoped to the watched root
// archetypes, and the volume is one of them, so the move is seen through it.
TEST(TerrainVolumeGate, MovingAVolumeWakesTheGatherAndRebakes)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);
    const auto volume = CreateMigratedNoiseVolume(scene.World);

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    auto* xf = scene.World.GetComponentForWrite<Components::WorldTransform>(volume);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = kModX + 60.0f;

    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "moving a volume did not wake the modifier gather";
    EXPECT_FALSE(BytewiseEqual(beforeEdit, CopyHeights(*scene.Data)))
        << "moving a volume did not re-bake the terrain";
}

// Editing an effect. The effect column is stamped on the volume entity's chunk,
// but a Changed<TerrainModifierVolume> probe filters the VOLUME column, so the
// effect types need probes of their own.
TEST(TerrainVolumeGate, EffectFieldEditWakesTheGatherAndRebakes)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);
    const auto volume = CreateMigratedNoiseVolume(scene.World);

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    auto* fx = scene.World.GetComponentForWrite<Components::TerrainNoiseEffect>(volume);
    ASSERT_NE(fx, nullptr);
    fx->Amplitude *= 2.0f;

    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "editing an effect's amplitude did not wake the modifier gather";
    EXPECT_FALSE(BytewiseEqual(beforeEdit, CopyHeights(*scene.Data)))
        << "editing an effect's amplitude did not re-bake the terrain";
}

// Adding an effect to an existing volume. The entity changes archetype, which
// only a lifecycle event reports — hence two ticks, and no extra swap that would
// let the swap-generation fallback answer instead.
TEST(TerrainVolumeGate, AddingAnEffectWakesTheGatherAndRebakes)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);
    const auto volume = CreateCircleVolume(scene.World);

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    scene.World.AddComponentImmediate<Components::TerrainNoiseEffect>(volume, SharedNoiseEffect());
    Tick(scene.World, scene.System);
    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "adding an effect to a volume did not wake the modifier gather";
    EXPECT_FALSE(BytewiseEqual(beforeEdit, CopyHeights(*scene.Data)))
        << "adding an effect to a volume did not re-bake the terrain";
}

// Removing an effect. Removed<T> is the only detector: the entity leaves the
// archetype, so no Changed<> probe can see it.
TEST(TerrainVolumeGate, RemovingAnEffectWakesTheGatherAndRebakes)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);
    const auto volume = CreateMigratedNoiseVolume(scene.World);

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    scene.World.RemoveComponentImmediate<Components::TerrainNoiseEffect>(volume);
    Tick(scene.World, scene.System);
    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "removing an effect from a volume did not wake the modifier gather";
    EXPECT_FALSE(BytewiseEqual(beforeEdit, CopyHeights(*scene.Data)))
        << "removing an effect from a volume did not re-bake the terrain";
}

// Removing the volume itself must retire its footprint.
TEST(TerrainVolumeGate, RemovingAVolumeWakesTheGatherAndRebakes)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);
    const auto volume = CreateMigratedNoiseVolume(scene.World);

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    scene.World.RemoveComponentImmediate<Components::TerrainModifierVolume>(volume);
    Tick(scene.World, scene.System);
    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "removing a volume did not wake the modifier gather";
    EXPECT_FALSE(BytewiseEqual(beforeEdit, CopyHeights(*scene.Data)))
        << "removing a volume did not retire its footprint from the terrain";
}

// Editing a blend field must re-bake, which is a statement about HashModifierState and
// NOT about the change gate. The gate wakes on a Changed<TEffect> probe, so a dropped hash
// fold still wakes the GATHER — the bake then sees an unchanged hash and skips, leaving the
// terrain stale while every wake assertion above stays green. The second assertion in each
// arm below is what catches that, so the blend fields are covered the way the shape fields
// already are.
//
// The smoothing arms author SmoothMin first: BlendSmoothing is read by no other mode, so
// editing it under Set would be a legitimate no-op and the arm would fail for the wrong
// reason. The radius is 48 m of height rather than a token value because the polynomial
// clamps to the hard operator once |candidate - height| exceeds k — a 16 m radius leaves the
// noise and stamp candidates outside the band and their bakes byte-identical, correctly.
namespace
{
constexpr float32 kObservableSmoothingM = 48.0f;
// Above every base sample (the fill is normalized to [0,1]), so Min is a guaranteed no-op
// and Set a guaranteed flatten — the two are distinguishable without knowing the terrain's
// realised range.
constexpr float32 kAboveBandTargetHeight = kHeightScale;

template<typename TEffect, typename TMutate>
void ExpectBlendEditRebakes(const char* what, const TEffect& authored, TMutate mutate)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr) << what;
    const auto volume = CreateCircleVolume(scene.World);
    scene.World.AddComponentImmediate<TEffect>(volume, authored);

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    auto* fx = scene.World.GetComponentForWrite<TEffect>(volume);
    ASSERT_NE(fx, nullptr) << what;
    mutate(*fx);

    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << what << ": the edit did not wake the modifier gather";
    EXPECT_FALSE(BytewiseEqual(beforeEdit, CopyHeights(*scene.Data)))
        << what << ": the edit woke the gather but did not re-bake — the field is missing "
                   "from HashModifierState, so the terrain is now stale against the scene";
}
} // namespace

TEST(TerrainVolumeGate, BlendAndSmoothingEditsRebakeNotJustWakeTheGather)
{
    {
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = kAboveBandTargetHeight;
        fx.Blend = Components::TerrainModifierBlend::Set;
        ExpectBlendEditRebakes("flatten Blend", fx, [](Components::TerrainFlattenEffect& u) {
            u.Blend = Components::TerrainModifierBlend::Min;
        });
    }
    {
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = kAboveBandTargetHeight;
        fx.Blend = Components::TerrainModifierBlend::SmoothMin;
        fx.BlendSmoothing = 0.0f;
        ExpectBlendEditRebakes("flatten BlendSmoothing", fx,
                               [](Components::TerrainFlattenEffect& u) {
                                   u.BlendSmoothing = kObservableSmoothingM;
                               });
    }
    {
        Components::TerrainHeightOffsetEffect fx{};
        fx.Offset = kAboveBandTargetHeight;
        fx.Blend = Components::TerrainModifierBlend::SmoothMin;
        fx.BlendSmoothing = 0.0f;
        ExpectBlendEditRebakes("height-offset BlendSmoothing", fx,
                               [](Components::TerrainHeightOffsetEffect& u) {
                                   u.BlendSmoothing = kObservableSmoothingM;
                               });
    }
    {
        Components::TerrainNoiseEffect fx = SharedNoiseEffect();
        fx.Blend = Components::TerrainModifierBlend::SmoothMin;
        fx.BlendSmoothing = 0.0f;
        ExpectBlendEditRebakes("noise BlendSmoothing", fx, [](Components::TerrainNoiseEffect& u) {
            u.BlendSmoothing = kObservableSmoothingM;
        });
    }
    {
        // Null mask is the documented flat-white fallback, so this needs no asset manager.
        Components::TerrainStampEffect fx{};
        fx.Blend = Components::TerrainModifierBlend::SmoothMin;
        fx.BlendSmoothing = 0.0f;
        ExpectBlendEditRebakes("stamp BlendSmoothing", fx, [](Components::TerrainStampEffect& u) {
            u.BlendSmoothing = kObservableSmoothingM;
        });
    }
}

// The composition case end to end, on the arm the inspector authors by default: a mesa
// volume levels a plateau, a higher-priority pad drops onto it and cuts a notch with Set,
// and the pad's Blend is then edited to a union operator while the gate is quiet. The
// rebake must UNDO the pad's own notch — a union can only fill. ExpectBlendEditRebakes
// above proves such an edit re-bakes AT ALL; this proves the re-bake is CORRECT, which is
// a different claim and the one a stale-blend read would fail.
namespace
{
constexpr float32 kMesaY = 40.0f;      // plateau height (world units)
constexpr float32 kPadY = 20.0f;       // pad sits below it, so Set cuts
constexpr float32 kPadRadius = 20.0f;
constexpr float32 kMesaHalfExtent = 60.0f;

// Samples strictly inside the pad disc — where both volumes' weights are exactly 1.0,
// so the plateau value is the closed form with no falloff term in the way.
std::vector<std::size_t> PadCoreSamples()
{
    constexpr float32 kSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    std::vector<std::size_t> core;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kHeightmapDim) * kHeightmapDim; ++i)
    {
        float32 x = 0.0f, z = 0.0f;
        SampleWorldXZ(i, x, z);
        const float32 dx = x - kModX;
        const float32 dz = z - kModZ;
        if (std::sqrt(dx * dx + dz * dz) < kPadRadius - kSpacing)
            core.push_back(i);
    }
    return core;
}

// Count core samples cut below the plateau the mesa levelled.
std::size_t CountBelowPlateau(const std::vector<float32>& heights,
                              const std::vector<std::size_t>& core)
{
    constexpr float32 kPlateauNorm = kMesaY / kHeightScale;
    std::size_t below = 0;
    for (std::size_t i : core)
        if (heights[i] < kPlateauNorm - 1e-6f)
            ++below;
    return below;
}

// How the edit reaches the component. The two are NOT interchangeable for a change-gate
// test: the typed path takes a write grant through GetComponentForWrite, while the
// debug/MCP set_component path captures the component's bytes, patches them and applies
// the blob back. That is a different function, and it is the one an agent-driven edit
// travels — a gate that woke only for the typed path would leave every IPC-authored edit
// silently stale, so both are exercised.
enum class EditPath
{
    TypedWriteGrant,
    ComponentByteBlob,
};

// The set_component write reproduced exactly: capture, patch, apply back.
// Mirrors SetReflectedComponentFields (DebugHandlers.cpp:403-442).
void ApplyBlendViaComponentBytes(ECS::World& world, ECS::EntityHandle entity,
                                 Components::TerrainModifierBlend blend, float32 smoothing)
{
    const ECS::ComponentTypeId typeId =
        ECS::GetComponentTypeId<Components::TerrainFlattenEffect>();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(world.CaptureComponentBytes(entity, typeId, bytes));
    ASSERT_EQ(bytes.size(), sizeof(Components::TerrainFlattenEffect));

    Components::TerrainFlattenEffect patched{};
    std::memcpy(&patched, bytes.data(), sizeof(patched));
    patched.Blend = blend;
    patched.BlendSmoothing = smoothing;
    std::memcpy(bytes.data(), &patched, sizeof(patched));

    ASSERT_TRUE(world.ApplyComponentBytesImmediate(entity, typeId, bytes));
}

void ExpectPadBlendEditFillsItsOwnNotch(const char* what,
                                        Components::TerrainModifierBlend editedBlend,
                                        float32 editedSmoothing,
                                        EditPath path)
{
    const std::vector<std::size_t> core = PadCoreSamples();
    ASSERT_FALSE(core.empty()) << what;

    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr) << what;

    // The mesa: a rectangle volume levelling a plateau at its own entity Y.
    {
        auto e = scene.World.CreateEntity();
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::Rectangle;
        vol.RectHalfX = kMesaHalfExtent;
        vol.RectHalfZ = kMesaHalfExtent;
        vol.Priority = 30.0f;
        scene.World.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
        scene.World.AddComponentImmediate<Components::WorldTransform>(
            e, IdentityAt(kModX, kMesaY, kModZ));
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = true;
        scene.World.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    }

    // The pad: higher priority, so it applies AFTER the mesa and cuts into it.
    ECS::EntityHandle pad = scene.World.CreateEntity();
    {
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::Circle;
        vol.Radius = kPadRadius;
        vol.Priority = 61.0f;
        scene.World.AddComponentImmediate<Components::TerrainModifierVolume>(pad, vol);
        scene.World.AddComponentImmediate<Components::WorldTransform>(
            pad, IdentityAt(kModX, kPadY, kModZ));
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = true;
        fx.Blend = Components::TerrainModifierBlend::Set;
        scene.World.AddComponentImmediate<Components::TerrainFlattenEffect>(pad, fx);
    }

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);

    // Positive control: with Set the pad really does cut below the plateau, so
    // "nothing is below the plateau" after the edit is not vacuously true.
    const std::size_t notchBefore = CountBelowPlateau(CopyHeights(*scene.Data), core);
    ASSERT_GT(notchBefore, 0u)
        << what << ": the Set pad cut nothing into the mesa — the union assertion "
                   "below would pass on any implementation";

    if (path == EditPath::TypedWriteGrant)
    {
        auto* fx = scene.World.GetComponentForWrite<Components::TerrainFlattenEffect>(pad);
        ASSERT_NE(fx, nullptr) << what;
        fx->Blend = editedBlend;
        fx->BlendSmoothing = editedSmoothing;
    }
    else
    {
        ApplyBlendViaComponentBytes(scene.World, pad, editedBlend, editedSmoothing);
    }

    // The edit is the ONLY thing that changed: no transform touched, no component
    // added or removed. An effect-only edit is what an inspector field or an
    // IPC set_component produces, and it has to wake the gate on its own.
    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << what << ": the blend edit did not wake the modifier gather";
    EXPECT_EQ(CountBelowPlateau(CopyHeights(*scene.Data), core), 0u)
        << what << ": " << notchBefore << " samples were cut below the plateau and the "
        << "union edit left them cut — the rebake is not reading the effect's Blend";
}
} // namespace

TEST(TerrainVolumeGate, VolumeHeightFlattenBlendEditFillsItsOwnNotchOnRebake)
{
    for (const EditPath path : {EditPath::TypedWriteGrant, EditPath::ComponentByteBlob})
    {
        const char* via = path == EditPath::TypedWriteGrant ? " (typed write grant)"
                                                            : " (set_component byte blob)";
        ExpectPadBlendEditFillsItsOwnNotch((std::string("pad Blend -> Max") + via).c_str(),
                                           Components::TerrainModifierBlend::Max, 0.0f, path);
        ExpectPadBlendEditFillsItsOwnNotch((std::string("pad Blend -> SmoothMax") + via).c_str(),
                                           Components::TerrainModifierBlend::SmoothMax, 5.0f,
                                           path);
    }
}

// ---------------------------------------------------------------------------
// Blend dispatch — the surviving modes stay distinguishable at the bake
// ---------------------------------------------------------------------------

namespace
{
// A height-offset effect is the direct probe for the blend switch: it feeds the offset
// straight into BlendHeight with no shape sampling in between.
BakeArm BakeOffsetWithBlend(float32 offset, Components::TerrainModifierBlend blend)
{
    return Bake([offset, blend](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainHeightOffsetEffect fx{};
        fx.Offset = offset;
        fx.Blend = blend;
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
    });
}
} // namespace

// Deleting a blend value is only safe while the survivors still dispatch to distinct
// arithmetic. Subtract is exact-negated Add (IEEE sign flip and the h + (-y) == h - y
// identity are both exact), so that arm is a memcmp rather than a tolerance.
TEST(TerrainBlendDispatch, SetAddSubtractRemainDistinct)
{
    constexpr float32 kOffset = 9.0f;

    const BakeArm addPlus = BakeOffsetWithBlend(kOffset, Components::TerrainModifierBlend::Add);
    const BakeArm addMinus = BakeOffsetWithBlend(-kOffset, Components::TerrainModifierBlend::Add);
    const BakeArm subPlus = BakeOffsetWithBlend(kOffset, Components::TerrainModifierBlend::Subtract);
    const BakeArm setPlus = BakeOffsetWithBlend(kOffset, Components::TerrainModifierBlend::Set);

    ASSERT_FALSE(ChangedSamples(addPlus.BaseHeights, addPlus.BakedHeights).empty())
        << "the Add arm baked nothing — every comparison below would be vacuous";

    EXPECT_TRUE(BytewiseEqual(subPlus.BakedHeights, addMinus.BakedHeights))
        << "Subtract no longer mirrors Add about the base height";
    EXPECT_FALSE(BytewiseEqual(addPlus.BakedHeights, subPlus.BakedHeights))
        << "Add and Subtract collapsed onto the same arm";
    EXPECT_FALSE(BytewiseEqual(setPlus.BakedHeights, addPlus.BakedHeights))
        << "Set collapsed onto the Add arm — it must replace, not accumulate";
    EXPECT_FALSE(BytewiseEqual(setPlus.BakedHeights, subPlus.BakedHeights))
        << "Set collapsed onto the Subtract arm";
}

// The default arm is what a value outside the enum lands on, which is why removing
// Smooth preserved behavior. Pin it so a future switch rewrite cannot quietly make an
// unknown blend a no-op (or a Set) instead.
TEST(TerrainBlendDispatch, UnknownBlendBakesAsAdd)
{
    constexpr float32 kOffset = 9.0f;
    const auto unknown = static_cast<Components::TerrainModifierBlend>(3);

    const BakeArm add = BakeOffsetWithBlend(kOffset, Components::TerrainModifierBlend::Add);
    const BakeArm out = BakeOffsetWithBlend(kOffset, unknown);

    ASSERT_FALSE(ChangedSamples(add.BaseHeights, add.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(out.BakedHeights, add.BakedHeights))
        << "a blend value outside the enum no longer falls through to Add";
}

// ---------------------------------------------------------------------------
// Union operators — Min / Max / SmoothMin / SmoothMax (composition design §3.2)
//
// Every arm here uses a FLATTEN effect at an ABSOLUTE target: flatten is the shape
// both motivating cases have (valley carve, mountain-dome union), and an absolute
// target keeps the expected value a closed form with no volume-reference lookup in
// the way. Inside the circle's core the shape weight is exactly 1.0, so the expected
// value is computable in the test to the bit.
// ---------------------------------------------------------------------------

namespace
{
BakeArm BakeFlattenWithBlend(float32 targetHeight, Components::TerrainModifierBlend blend,
                             float32 smoothing = 0.0f)
{
    return Bake([targetHeight, blend, smoothing](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = targetHeight;
        fx.Blend = blend;
        fx.BlendSmoothing = smoothing;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    });
}

// Samples strictly inside the circle, where ComputeWeight returns exactly 1.0 — the
// only region whose expected value carries no falloff term. Half a sample spacing of
// margin keeps the boundary sample out of it.
std::vector<std::size_t> CircleCoreSamples()
{
    constexpr float32 kSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    std::vector<std::size_t> core;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kHeightmapDim) * kHeightmapDim; ++i)
    {
        float32 x = 0.0f, z = 0.0f;
        SampleWorldXZ(i, x, z);
        const float32 dx = x - kModX;
        const float32 dz = z - kModZ;
        if (std::sqrt(dx * dx + dz * dz) < kCircleRadius - 0.5f * kSpacing)
            core.push_back(i);
    }
    return core;
}

// A target midway through the base heights under the core, so the core provably holds
// ground on BOTH sides of the profile — without that, a cut operator has nothing to cut
// and a "never raises" assertion passes vacuously.
float32 MidCoreTargetHeight(const BakeArm& probe, const std::vector<std::size_t>& core)
{
    float32 lo = probe.BaseHeights[core.front()];
    float32 hi = lo;
    for (std::size_t i : core)
    {
        lo = std::min(lo, probe.BaseHeights[i]);
        hi = std::max(hi, probe.BaseHeights[i]);
    }
    return 0.5f * (lo + hi) * kHeightScale; // normalized -> world units
}
} // namespace

// SF15, the case the operator exists for: a valley profile cut across sloped ground.
// Flatten's lerp RAISES ground lying below the profile, so a valley bulges its downhill
// side; Min must never raise. The Set arm is the positive control — it proves the scene
// really does contain below-profile ground, without which "raised nothing" is vacuous.
TEST(TerrainBlendUnionOperators, MinFlattenCutsAndNeverRaisesBelowProfileGround)
{
    const std::vector<std::size_t> core = CircleCoreSamples();
    ASSERT_FALSE(core.empty());
    const BakeArm probe = BakeFlattenWithBlend(0.0f, Components::TerrainModifierBlend::Set);
    const float32 target = MidCoreTargetHeight(probe, core);

    const float32 targetNorm = target / kHeightScale;
    const BakeArm setArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Set);
    const BakeArm minArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Min);

    std::size_t setRaised = 0, minRaised = 0, minCut = 0, minOvershot = 0;
    for (std::size_t i = 0; i < setArm.BaseHeights.size(); ++i)
    {
        if (setArm.BakedHeights[i] > setArm.BaseHeights[i]) ++setRaised;
        if (minArm.BakedHeights[i] > minArm.BaseHeights[i]) ++minRaised;
        if (minArm.BakedHeights[i] < minArm.BaseHeights[i]) ++minCut;
        // A union never leaves the interval between its two operands. Accumulating
        // arithmetic does, and that is what separates an operator from a displacement.
        if (minArm.BakedHeights[i] < std::min(minArm.BaseHeights[i], targetNorm) - 1e-6f)
            ++minOvershot;
    }

    EXPECT_GT(setRaised, 0u)
        << "the Set arm raised nothing — the terrain has no below-profile ground here, "
           "so the Min assertion below would pass on any implementation";
    EXPECT_EQ(minRaised, 0u)
        << "Min raised " << minRaised << " samples — a valley on a slope still bulges downhill";
    EXPECT_GT(minCut, 0u) << "Min cut nothing — the operator is a no-op, not a cut";
    EXPECT_EQ(minOvershot, 0u)
        << "Min fell below min(height, target) at " << minOvershot << " samples — it accumulated";
}

// The mirror: Max unions, and never lowers ground already above the target.
TEST(TerrainBlendUnionOperators, MaxFlattenFillsAndNeverLowersAboveProfileGround)
{
    const std::vector<std::size_t> core = CircleCoreSamples();
    ASSERT_FALSE(core.empty());
    const BakeArm probe = BakeFlattenWithBlend(0.0f, Components::TerrainModifierBlend::Set);
    const float32 target = MidCoreTargetHeight(probe, core);

    const float32 targetNorm = target / kHeightScale;
    const BakeArm setArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Set);
    const BakeArm maxArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Max);

    std::size_t setLowered = 0, maxLowered = 0, maxFilled = 0, maxOvershot = 0;
    for (std::size_t i = 0; i < setArm.BaseHeights.size(); ++i)
    {
        if (setArm.BakedHeights[i] < setArm.BaseHeights[i]) ++setLowered;
        if (maxArm.BakedHeights[i] < maxArm.BaseHeights[i]) ++maxLowered;
        if (maxArm.BakedHeights[i] > maxArm.BaseHeights[i]) ++maxFilled;
        // The union bound. Without it this test passes on an implementation that simply
        // ADDS the target, which also never lowers — the trap a broken-arm run found.
        if (maxArm.BakedHeights[i] > std::max(maxArm.BaseHeights[i], targetNorm) + 1e-6f)
            ++maxOvershot;
    }

    EXPECT_GT(setLowered, 0u) << "the Set arm lowered nothing — the Max assertion would be vacuous";
    EXPECT_EQ(maxLowered, 0u) << "Max lowered " << maxLowered << " samples — it is not a union";
    EXPECT_GT(maxFilled, 0u) << "Max filled nothing — the operator is a no-op";
    EXPECT_EQ(maxOvershot, 0u)
        << "Max rose above max(height, target) at " << maxOvershot << " samples — it accumulated";
}

// Each operator against a hand-computed expectation, at weight == 1.0 where the closed
// form has no falloff term. Min/Max leave the untouched side BYTE-identical to the base
// (h + (h - h) * w is exactly h), which is the strongest statement available here.
TEST(TerrainBlendUnionOperators, OperatorArmsMatchHandComputedExpectationInTheCore)
{
    const std::vector<std::size_t> core = CircleCoreSamples();
    ASSERT_FALSE(core.empty());
    const BakeArm probe = BakeFlattenWithBlend(0.0f, Components::TerrainModifierBlend::Set);
    const float32 target = MidCoreTargetHeight(probe, core);
    const float32 targetNorm = target / kHeightScale;

    const BakeArm setArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Set);
    const BakeArm minArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Min);
    const BakeArm maxArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Max);

    std::size_t below = 0, above = 0;
    for (std::size_t i : core)
    {
        const float32 h = setArm.BaseHeights[i];
        // Set: the shipped flatten expression, evaluated the same way the bake does.
        EXPECT_FLOAT_EQ(setArm.BakedHeights[i], h + (targetNorm - h) * 1.0f) << "Set at " << i;
        EXPECT_FLOAT_EQ(minArm.BakedHeights[i], h + (std::min(h, targetNorm) - h) * 1.0f)
            << "Min at " << i;
        EXPECT_FLOAT_EQ(maxArm.BakedHeights[i], h + (std::max(h, targetNorm) - h) * 1.0f)
            << "Max at " << i;

        if (h < targetNorm)
        {
            ++below;
            // Untouched side of a cut: bit-identical, not merely close.
            EXPECT_EQ(minArm.BakedHeights[i], h) << "Min moved below-profile ground at " << i;
        }
        else if (h > targetNorm)
        {
            ++above;
            EXPECT_EQ(maxArm.BakedHeights[i], h) << "Max moved above-profile ground at " << i;
        }
    }
    EXPECT_GT(below, 0u) << "no core sample sits below the target — the Min rows proved nothing";
    EXPECT_GT(above, 0u) << "no core sample sits above the target — the Max rows proved nothing";
}

// A zero radius has no rounding to do, so the smooth operators must degenerate EXACTLY
// onto the hard ones. That pins the guard, and it is a memcmp rather than a tolerance.
TEST(TerrainBlendUnionOperators, SmoothOperatorsWithZeroRadiusEqualTheHardOperators)
{
    const std::vector<std::size_t> core = CircleCoreSamples();
    ASSERT_FALSE(core.empty());
    const BakeArm probe = BakeFlattenWithBlend(0.0f, Components::TerrainModifierBlend::Set);
    const float32 target = MidCoreTargetHeight(probe, core);

    const BakeArm minArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Min);
    const BakeArm maxArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Max);
    const BakeArm sMin =
        BakeFlattenWithBlend(target, Components::TerrainModifierBlend::SmoothMin, 0.0f);
    const BakeArm sMax =
        BakeFlattenWithBlend(target, Components::TerrainModifierBlend::SmoothMax, 0.0f);

    ASSERT_FALSE(ChangedSamples(minArm.BaseHeights, minArm.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(sMin.BakedHeights, minArm.BakedHeights))
        << "SmoothMin with a zero radius is not the hard Min";
    EXPECT_TRUE(BytewiseEqual(sMax.BakedHeights, maxArm.BakedHeights))
        << "SmoothMax with a zero radius is not the hard Max";
}

// With a real radius the smooth operators round the seam: they must differ from the hard
// pair, dip past it in the correct direction, and stay within the polynomial's k/4 bound.
TEST(TerrainBlendUnionOperators, SmoothOperatorsRoundTheSeamWithinTheRadiusBound)
{
    constexpr float32 kSmoothingM = 5.0f;
    const std::vector<std::size_t> core = CircleCoreSamples();
    ASSERT_FALSE(core.empty());
    const BakeArm probe = BakeFlattenWithBlend(0.0f, Components::TerrainModifierBlend::Set);
    const float32 target = MidCoreTargetHeight(probe, core);

    const BakeArm minArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Min);
    const BakeArm maxArm = BakeFlattenWithBlend(target, Components::TerrainModifierBlend::Max);
    const BakeArm sMin =
        BakeFlattenWithBlend(target, Components::TerrainModifierBlend::SmoothMin, kSmoothingM);
    const BakeArm sMax =
        BakeFlattenWithBlend(target, Components::TerrainModifierBlend::SmoothMax, kSmoothingM);

    EXPECT_FALSE(BytewiseEqual(sMin.BakedHeights, minArm.BakedHeights))
        << "SmoothMin with a 5 m radius collapsed onto the hard Min — the radius does nothing";
    EXPECT_FALSE(BytewiseEqual(sMax.BakedHeights, maxArm.BakedHeights))
        << "SmoothMax with a 5 m radius collapsed onto the hard Max";

    // The polynomial's maximum deviation from the hard operator is k/4, at the crossing.
    const float32 boundNorm = 0.25f * (kSmoothingM / kHeightScale) + 1e-6f;
    for (std::size_t i : core)
    {
        EXPECT_LE(sMin.BakedHeights[i], minArm.BakedHeights[i])
            << "SmoothMin rose above the hard Min at " << i;
        EXPECT_GE(sMin.BakedHeights[i], minArm.BakedHeights[i] - boundNorm)
            << "SmoothMin dipped more than k/4 below the hard Min at " << i;
        EXPECT_GE(sMax.BakedHeights[i], maxArm.BakedHeights[i])
            << "SmoothMax fell below the hard Max at " << i;
        EXPECT_LE(sMax.BakedHeights[i], maxArm.BakedHeights[i] + boundNorm)
            << "SmoothMax rose more than k/4 above the hard Max at " << i;
    }
}

namespace
{
// The volume-height arm of the same effect. Every union-operator arm above authors an
// ABSOLUTE target (UseVolumeHeight = false), but the component DEFAULTS the flag to true
// and the inspector authors it that way, so the arm the editor actually produces reaches
// BlendHeight through a different target expression: the volume's reference height (entity
// Y for a circle) plus TargetHeight as a signed OFFSET.
BakeArm BakeVolumeHeightFlattenWithBlend(float32 heightOffset,
                                         Components::TerrainModifierBlend blend,
                                         float32 smoothing = 0.0f)
{
    return Bake([heightOffset, blend, smoothing](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = true;
        fx.TargetHeight = heightOffset;
        fx.Blend = blend;
        fx.BlendSmoothing = smoothing;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    });
}
} // namespace

// The union operators on the arm the editor authors by default. A pad dropped on a mesa
// with Max or SmoothMax must FILL toward the target and never cut — that is the whole
// point of a union — and the Set arm is the positive control proving the mesa really does
// stand above the target, without which "cut nothing" passes on any implementation.
TEST(TerrainBlendUnionOperators, VolumeHeightFlattenHonorsUnionOperatorsAndNeverCuts)
{
    constexpr float32 kSmoothingM = 5.0f;
    const std::vector<std::size_t> core = CircleCoreSamples();
    ASSERT_FALSE(core.empty());

    // The target is kModY + offset, so pick the offset that lands it midway through the
    // core — ground provably on both sides, exactly as MidCoreTargetHeight guarantees.
    const BakeArm probe =
        BakeVolumeHeightFlattenWithBlend(0.0f, Components::TerrainModifierBlend::Set);
    const float32 offset = MidCoreTargetHeight(probe, core) - kModY;
    const float32 targetNorm = (kModY + offset) / kHeightScale;

    const BakeArm setArm =
        BakeVolumeHeightFlattenWithBlend(offset, Components::TerrainModifierBlend::Set);
    const BakeArm maxArm =
        BakeVolumeHeightFlattenWithBlend(offset, Components::TerrainModifierBlend::Max);
    const BakeArm smoothMaxArm = BakeVolumeHeightFlattenWithBlend(
        offset, Components::TerrainModifierBlend::SmoothMax, kSmoothingM);

    // The reference height and TargetHeight COMPOSE: the target is entity Y + offset, not
    // one or the other. A bake that dropped the offset would land the core on kModY.
    for (std::size_t i : core)
        EXPECT_FLOAT_EQ(setArm.BakedHeights[i], targetNorm)
            << "the volume-height target is not (reference height + TargetHeight) at " << i;

    std::size_t setLowered = 0, maxLowered = 0, maxFilled = 0, maxOvershot = 0,
                smoothMaxLowered = 0;
    for (std::size_t i = 0; i < setArm.BaseHeights.size(); ++i)
    {
        if (setArm.BakedHeights[i] < setArm.BaseHeights[i]) ++setLowered;
        if (maxArm.BakedHeights[i] < maxArm.BaseHeights[i]) ++maxLowered;
        if (maxArm.BakedHeights[i] > maxArm.BaseHeights[i]) ++maxFilled;
        if (smoothMaxArm.BakedHeights[i] < smoothMaxArm.BaseHeights[i]) ++smoothMaxLowered;
        if (maxArm.BakedHeights[i] > std::max(maxArm.BaseHeights[i], targetNorm) + 1e-6f)
            ++maxOvershot;
    }

    EXPECT_GT(setLowered, 0u)
        << "the Set arm cut nothing — no ground stands above the target here, so the "
           "union assertions below would be vacuous";
    EXPECT_EQ(maxLowered, 0u)
        << "Max cut " << maxLowered << " samples on the volume-height arm — the bake is not "
           "reading the effect's Blend on this path";
    EXPECT_EQ(smoothMaxLowered, 0u)
        << "SmoothMax cut " << smoothMaxLowered << " samples on the volume-height arm";
    EXPECT_GT(maxFilled, 0u) << "Max filled nothing — the operator is a no-op";
    EXPECT_EQ(maxOvershot, 0u)
        << "Max rose above max(height, target) at " << maxOvershot << " samples — it accumulated";
}

// Flatten's blend defaults to Set, which is the expression it hardcoded before it had a
// blend at all — so every scene authored without the field bakes byte-identically. The
// legacy pre-volume flatten is pinned to Set for the same reason and is the second,
// independent witness here.
TEST(TerrainBlendUnionOperators, FlattenDefaultBlendIsSetAndBakesIdentically)
{
    const BakeArm defaulted = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = 22.0f;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
    });
    const BakeArm explicitSet =
        BakeFlattenWithBlend(22.0f, Components::TerrainModifierBlend::Set);
    const BakeArm legacy = Bake([](ECS::World& w, SplineECS::SplineService&) {
        CreateMigratedLegacyModifier(w, "TerrainFlattenModifier",
                                     {{"targetheight", "22"}, {"useentityheight", "false"}});
    });

    ASSERT_FALSE(ChangedSamples(defaulted.BaseHeights, defaulted.BakedHeights).empty())
        << "the default-blend flatten baked nothing — the comparisons below are vacuous";
    EXPECT_EQ(Components::TerrainFlattenEffect{}.Blend, Components::TerrainModifierBlend::Set);
    EXPECT_TRUE(BytewiseEqual(defaulted.BakedHeights, explicitSet.BakedHeights))
        << "the flatten blend default is no longer Set";
    EXPECT_TRUE(BytewiseEqual(defaulted.BakedHeights, legacy.BakedHeights))
        << "the pre-volume flatten no longer bakes as the Set-blended effect";
}

// The operators reach the displacement effects too, not just flatten — the switch is
// shared, and a regression that wired only the flatten call site would pass everything
// above. Min against a raise must never raise.
TEST(TerrainBlendUnionOperators, MinReachesTheHeightOffsetEffect)
{
    const BakeArm addArm = BakeOffsetWithBlend(40.0f, Components::TerrainModifierBlend::Add);
    const BakeArm minArm = BakeOffsetWithBlend(40.0f, Components::TerrainModifierBlend::Min);

    ASSERT_FALSE(ChangedSamples(addArm.BaseHeights, addArm.BakedHeights).empty());
    EXPECT_FALSE(BytewiseEqual(minArm.BakedHeights, addArm.BakedHeights))
        << "Min on a height-offset effect fell through to the Add arm";

    std::size_t raised = 0;
    for (std::size_t i = 0; i < minArm.BaseHeights.size(); ++i)
        if (minArm.BakedHeights[i] > minArm.BaseHeights[i])
            ++raised;
    EXPECT_EQ(raised, 0u) << "Min raised " << raised << " samples on a height-offset effect";
}

// ---------------------------------------------------------------------------
// Paint replace — the falloff ring blends, it does not cut
//
// `Replace` composites the painted layer OVER the existing mix, and the volume's
// ramp weight is the replacement fraction: a paint authored with Falloff N gets
// an N-metre soft edge of the same character an additive paint gets.
//
// The ramp lies OUTSIDE the shape. ShapeFalloffWeight rises 0 -> 1 across
// [-Falloff, 0] in distance-from-edge, and with no inward feather the whole
// footprint sits at weight 1 — so the ring these tests read is the annulus
// between the rect and Falloff metres beyond it, and the rect itself is the
// full-replacement interior.
//
// Every arm pairs a BASELINE bake (the scene WITHOUT the replace volume) with
// the same scene plus it, and the oracles are invariants over that pair. That is
// deliberate: a test that recomputed ComputeWeight would fail as loudly for a
// deliberate ramp change as for the defect it exists to catch. Only
// ReplaceRingLerpsTheWholeMixTowardThePureLayer needs the ramp weight, and it
// reads it back out of the bake rather than recomputing it.
// ---------------------------------------------------------------------------

namespace
{

constexpr uint32 kReplaceLayer = 3;
constexpr float32 kReplaceHalf = 26.0f;
constexpr float32 kReplaceFalloff = 14.0f;
constexpr float32 kSplatSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
constexpr float32 kSplatOrigin = -kWorldSize * 0.5f;

// The volume under test: axis-aligned rect at the terrain centre, painting
// kReplaceLayer at full strength with Replace set. Priority 5 runs it after
// every other paint these arms author.
void AddReplaceRect(ECS::World& world)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Rectangle;
    vol.RectHalfX = kReplaceHalf;
    vol.RectHalfZ = kReplaceHalf;
    vol.Falloff = kReplaceFalloff;
    vol.Priority = 5.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));

    Components::TerrainPaintLayerEffect fx{};
    fx.LayerIndex = kReplaceLayer;
    fx.Strength = 1.0f;
    fx.Replace = true;
    world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, fx);
}

// A full-coverage base of material, as the shipped mechanism supplies it: a global
// volume carrying ONE unconditional rules row. Paint semantics are defined against
// the mix a texel already holds, so these tests need a terrain that HAS a mix —
// the unbaked base is all zero, and replacing nothing yields nothing. Priority is
// below the paints so it composites first, exactly as a scene-level rules volume
// does under a local paint stroke.
void AddBaseSurfaceRulesFill(ECS::World& world)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Global;
    vol.Priority = -1.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));

    Components::TerrainSurfaceRulesEffect rules{};
    rules.RuleCount = 1;
    rules.Rules[0].MaterialSlot = 0;
    rules.Rules[0].Strength = 1.0f;
    rules.Rules[0].ConditionCount = 0; // unconditional: full weight everywhere
    world.AddComponentImmediate<Components::TerrainSurfaceRulesEffect>(e, rules);
}

// An ordinary additive paint of the SAME layer, wide enough to cover the replace
// rect's whole ring: the deterministic stand-in for content that already carries
// the painted layer where a replace edge lands — a neighbouring paint, or a
// rules row that claims the same channel.
void AddUnderPaintCircle(ECS::World& world)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = 56.0f;
    vol.Falloff = 8.0f;
    vol.Priority = 0.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));

    Components::TerrainPaintLayerEffect fx{};
    fx.LayerIndex = kReplaceLayer;
    fx.Strength = 1.0f;
    fx.Replace = false;
    world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, fx);
}

float32 SplatTexelX(uint32 sx) { return kSplatOrigin + static_cast<float32>(sx) * kSplatSpacing; }
float32 SplatTexelZ(uint32 sz) { return kSplatOrigin + static_cast<float32>(sz) * kSplatSpacing; }

// distFromEdge for an unrotated square is Half - Chebyshev distance, so the
// Chebyshev radius is the one number both predicates below need.
float32 RectChebyshev(uint32 sx, uint32 sz)
{
    return std::max(std::abs(SplatTexelX(sx)), std::abs(SplatTexelZ(sz)));
}

bool IsRectInterior(uint32 sx, uint32 sz) { return RectChebyshev(sx, sz) < kReplaceHalf; }

bool IsRectRing(uint32 sx, uint32 sz)
{
    const float32 d = RectChebyshev(sx, sz);
    return d > kReplaceHalf && d < kReplaceHalf + kReplaceFalloff;
}

// The last texel row the ramp reaches before it hits zero. Beyond it the volume
// writes nothing at all, so a discontinuity in the bake shows up here as a
// delta-from-baseline that fails to approach zero.
//
// Half-open on purpose: texel centres land on exact multiples of the spacing, so
// a range open at both ends selects nothing at all on this grid.
bool IsOutermostRingRow(uint32 sx, uint32 sz)
{
    const float32 d = RectChebyshev(sx, sz);
    return d >= kReplaceHalf + kReplaceFalloff - kSplatSpacing && d < kReplaceHalf + kReplaceFalloff;
}

std::size_t SplatIndex(uint32 sx, uint32 sz, uint32 channel)
{
    return (static_cast<std::size_t>(sz) * kHeightmapDim + sx) * 4 + channel;
}

// Baseline / baked pair for the "replace lands on content already carrying its
// own layer" arrangement, which is the only one the defect shows up in.
struct ReplacePair
{
    BakeArm Baseline;
    BakeArm Baked;
};

ReplacePair BakeReplaceOverUnderPaint()
{
    ReplacePair pair{};
    pair.Baseline = Bake([](ECS::World& w, SplineECS::SplineService&) {
        AddBaseSurfaceRulesFill(w);
        AddUnderPaintCircle(w);
    });
    pair.Baked = Bake([](ECS::World& w, SplineECS::SplineService&) {
        AddBaseSurfaceRulesFill(w);
        AddUnderPaintCircle(w);
        AddReplaceRect(w);
    });
    return pair;
}

} // namespace

// The defect, stated as the invariant it breaks: painting a layer must never
// REMOVE weight from that layer. Writing the ramp value into the target channel
// absolutely (rather than lerping that channel toward pure like every other)
// drives the outer ring DOWN from whatever the surrounding mix held, to nearly
// zero where the ramp reaches its edge.
TEST(TerrainPaintReplace, PaintingALayerNeverReducesThatLayersWeight)
{
    const ReplacePair pair = BakeReplaceOverUnderPaint();
    ASSERT_FALSE(pair.Baked.BakedSplat.empty());
    ASSERT_EQ(pair.Baseline.BakedSplat.size(), pair.Baked.BakedSplat.size());

    std::size_t ringCarryingTheLayer = 0;
    std::size_t reduced = 0;
    int32 worstDrop = 0;
    uint32 worstX = 0, worstZ = 0;
    for (uint32 sz = 0; sz < kHeightmapDim; ++sz)
        for (uint32 sx = 0; sx < kHeightmapDim; ++sx)
        {
            const std::size_t idx = SplatIndex(sx, sz, kReplaceLayer);
            const int32 before = pair.Baseline.BakedSplat[idx];
            const int32 after = pair.Baked.BakedSplat[idx];
            if (IsRectRing(sx, sz) && before >= 32)
                ++ringCarryingTheLayer;
            // One truncated LSB is arithmetic; anything past that is removal.
            const int32 drop = before - after;
            if (drop > 1)
            {
                ++reduced;
                if (drop > worstDrop)
                {
                    worstDrop = drop;
                    worstX = sx;
                    worstZ = sz;
                }
            }
        }

    ASSERT_GE(ringCarryingTheLayer, 100u)
        << "the falloff ring never crossed content already carrying the painted layer — "
           "the assertion below would be vacuous";
    EXPECT_EQ(reduced, 0u)
        << reduced << " texels LOST weight in the layer being painted; worst drop " << worstDrop
        << " at (" << worstX << ", " << worstZ << ")";
}

// The staircase, measured. Outside the ramp the volume writes nothing, so the
// delta from baseline is exactly zero there; at the last texel row the ramp
// reaches, the delta must therefore be near zero too. A hard step at that
// boundary is precisely the texel-quantised edge a replace paint shows where it
// meets a neighbouring layer.
TEST(TerrainPaintReplace, ReplaceEdgeIsContinuousWhereTheRampReachesZero)
{
    const ReplacePair pair = BakeReplaceOverUnderPaint();
    ASSERT_FALSE(pair.Baked.BakedSplat.empty());

    // Across the outermost row the ramp is worth at most
    // smoothstep(kSplatSpacing / kReplaceFalloff) of a full channel; 20/255
    // leaves room for that plus truncation without admitting a step.
    constexpr int32 kMaxStep = 20;
    std::size_t rowTexels = 0;
    std::size_t stepped = 0;
    int32 worstStep = 0;
    uint32 worstX = 0, worstZ = 0, worstChannel = 0;
    for (uint32 sz = 0; sz < kHeightmapDim; ++sz)
        for (uint32 sx = 0; sx < kHeightmapDim; ++sx)
        {
            if (!IsOutermostRingRow(sx, sz))
                continue;
            ++rowTexels;
            for (uint32 i = 0; i < 4; ++i)
            {
                const std::size_t idx = SplatIndex(sx, sz, i);
                const int32 delta = std::abs(static_cast<int32>(pair.Baked.BakedSplat[idx])
                                           - static_cast<int32>(pair.Baseline.BakedSplat[idx]));
                if (delta > kMaxStep)
                {
                    ++stepped;
                    if (delta > worstStep)
                    {
                        worstStep = delta;
                        worstX = sx;
                        worstZ = sz;
                        worstChannel = i;
                    }
                }
            }
        }

    ASSERT_GE(rowTexels, 50u) << "the outermost ramp row resolved to almost no texels";
    EXPECT_EQ(stepped, 0u)
        << stepped << " channel steps at the ramp's zero crossing; worst " << worstStep
        << " on channel " << worstChannel << " at (" << worstX << ", " << worstZ << ")";
}

// Proportionality, pinned exactly. Channels the paint does not target are scaled
// by (1 - w), so the ramp weight can be READ BACK from one of them and used to
// predict the target channel — which must be the same lerp, from whatever weight
// the mix already held toward pure. Reading w out of the bake keeps this test
// independent of the ramp's shape.
TEST(TerrainPaintReplace, ReplaceRingLerpsTheWholeMixTowardThePureLayer)
{
    const ReplacePair pair = BakeReplaceOverUnderPaint();
    ASSERT_FALSE(pair.Baked.BakedSplat.empty());

    // Recovering w from an 8-bit channel of magnitude B costs up to 1/B; at
    // B >= 64 that is under 4/255 on the predicted target, plus its own LSB.
    constexpr int32 kMinProbeWeight = 64;
    constexpr int32 kTolerance = 6;
    std::size_t usable = 0;
    std::size_t mismatched = 0;
    int32 worstDelta = 0;
    uint32 worstX = 0, worstZ = 0;
    for (uint32 sz = 0; sz < kHeightmapDim; ++sz)
        for (uint32 sx = 0; sx < kHeightmapDim; ++sx)
        {
            if (!IsRectRing(sx, sz))
                continue;

            // The untargeted channel with the most baseline weight conditions the
            // recovery best.
            uint32 probe = 4;
            int32 probeBefore = kMinProbeWeight;
            for (uint32 i = 0; i < 4; ++i)
            {
                if (i == kReplaceLayer)
                    continue;
                const int32 before = pair.Baseline.BakedSplat[SplatIndex(sx, sz, i)];
                if (before > probeBefore)
                {
                    probeBefore = before;
                    probe = i;
                }
            }
            if (probe == 4)
                continue;

            const int32 probeAfter = pair.Baked.BakedSplat[SplatIndex(sx, sz, probe)];
            const float32 w = 1.0f - static_cast<float32>(probeAfter)
                                   / static_cast<float32>(probeBefore);
            if (w <= 0.0f || w >= 1.0f)
                continue; // the recovery only speaks inside the ramp
            ++usable;

            const std::size_t targetIdx = SplatIndex(sx, sz, kReplaceLayer);
            const float32 targetBefore = static_cast<float32>(pair.Baseline.BakedSplat[targetIdx]);
            const int32 expected = static_cast<int32>(targetBefore * (1.0f - w) + 255.0f * w);
            const int32 delta = std::abs(static_cast<int32>(pair.Baked.BakedSplat[targetIdx])
                                       - expected);
            if (delta > kTolerance)
            {
                ++mismatched;
                if (delta > worstDelta)
                {
                    worstDelta = delta;
                    worstX = sx;
                    worstZ = sz;
                }
            }
        }

    ASSERT_GE(usable, 100u)
        << "too few ring texels carried a usable probe channel — the assertion below is weak";
    EXPECT_EQ(mismatched, 0u)
        << mismatched << " of " << usable << " ring texels are not the ramp-proportional lerp; "
        << "worst delta " << worstDelta << " at (" << worstX << ", " << worstZ << ")";
}

// Splat weights are a partition of the surface: the terrain shader normalises
// after bilinear filtering, so a texel summing short under-contributes every
// layer it holds and reads as an edge. Replace must preserve the sum it found.
TEST(TerrainPaintReplace, ReplacedTexelsKeepTheirWeightSum)
{
    const ReplacePair pair = BakeReplaceOverUnderPaint();
    ASSERT_FALSE(pair.Baked.BakedSplat.empty());

    // 255 minus at most one truncated LSB per channel.
    constexpr int32 kMinSum = 251;
    constexpr int32 kMaxSum = 256;
    std::size_t ringTexels = 0;
    std::size_t offSum = 0;
    int32 worstSum = 1024;
    uint32 worstX = 0, worstZ = 0;
    for (uint32 sz = 0; sz < kHeightmapDim; ++sz)
        for (uint32 sx = 0; sx < kHeightmapDim; ++sx)
        {
            if (IsRectRing(sx, sz))
                ++ringTexels;
            int32 sum = 0;
            for (uint32 i = 0; i < 4; ++i)
                sum += pair.Baked.BakedSplat[SplatIndex(sx, sz, i)];
            if (sum < kMinSum || sum > kMaxSum)
            {
                ++offSum;
                if (sum < worstSum)
                {
                    worstSum = sum;
                    worstX = sx;
                    worstZ = sz;
                }
            }
        }

    ASSERT_GE(ringTexels, 100u) << "the falloff ring resolved to almost no texels";
    EXPECT_EQ(offSum, 0u)
        << offSum << " texels no longer sum to one; worst sum " << worstSum
        << " at (" << worstX << ", " << worstZ << ")";
}

// Control for the interior: the ramp is 1 across the whole footprint, so at full
// strength every texel inside the rect is the pure layer no matter what the mix
// held before — including where the earlier paint put its own weight there.
TEST(TerrainPaintReplace, FullStrengthInteriorFullyReplacesWhateverWasThere)
{
    const ReplacePair pair = BakeReplaceOverUnderPaint();
    ASSERT_FALSE(pair.Baked.BakedSplat.empty());

    std::size_t interior = 0;
    std::size_t interiorOverEarlierPaint = 0;
    std::size_t notPure = 0;
    for (uint32 sz = 0; sz < kHeightmapDim; ++sz)
        for (uint32 sx = 0; sx < kHeightmapDim; ++sx)
        {
            if (!IsRectInterior(sx, sz))
                continue;
            ++interior;
            if (pair.Baseline.BakedSplat[SplatIndex(sx, sz, kReplaceLayer)] >= 32)
                ++interiorOverEarlierPaint;
            bool pure = true;
            for (uint32 i = 0; i < 4; ++i)
            {
                const int32 expected = (i == kReplaceLayer) ? 255 : 0;
                if (pair.Baked.BakedSplat[SplatIndex(sx, sz, i)] != expected)
                    pure = false;
            }
            if (!pure)
                ++notPure;
        }

    ASSERT_GE(interior, 100u);
    ASSERT_GE(interiorOverEarlierPaint, 100u)
        << "the interior never covered the earlier paint — a weak control";
    EXPECT_EQ(notPure, 0u) << notPure << " full-strength interior texels kept foreign weights";
}

// Control for the ramp itself: the replace edge is not a binary in/out mask. The
// ring must carry a spread of intermediate values in the painted layer, which is
// what distinguishes "the ramp is applied but its base is wrong" from "replace
// skips the ramp entirely".
TEST(TerrainPaintReplace, ReplaceEdgeCarriesIntermediateWeights)
{
    const ReplacePair pair = BakeReplaceOverUnderPaint();
    ASSERT_FALSE(pair.Baked.BakedSplat.empty());

    std::size_t ringTexels = 0;
    std::size_t intermediate = 0;
    for (uint32 sz = 0; sz < kHeightmapDim; ++sz)
        for (uint32 sx = 0; sx < kHeightmapDim; ++sx)
        {
            if (!IsRectRing(sx, sz))
                continue;
            ++ringTexels;
            const int32 v = pair.Baked.BakedSplat[SplatIndex(sx, sz, kReplaceLayer)];
            if (v > 10 && v < 245)
                ++intermediate;
        }

    ASSERT_GE(ringTexels, 100u) << "the falloff ring resolved to almost no texels";
    EXPECT_GE(intermediate, ringTexels / 2)
        << "only " << intermediate << " of " << ringTexels
        << " ring texels carry an intermediate weight — the replace edge is a binary step";
}

// ---- Erosion block (design 5.3 c) ------------------------------------------
//
// The erosion filter is a point-local pure function of (x, z) evaluated inside
// the noise effect, so its oracles are golden-image-free: identity when
// disarmed, bit-exactness of the analytic-gradient basis, determinism when
// armed, and the single geometric claim that makes it erosion rather than more
// noise — gullies run ACROSS the gradient, not along it.

namespace
{

Components::TerrainNoiseEffect ErodedNoiseEffect(float32 strength)
{
    Components::TerrainNoiseEffect fx = SharedNoiseEffect();
    fx.ErosionStrength = strength;
    fx.ErosionOctaves = 3;
    fx.ErosionFrequency = 2.5f;
    fx.ErosionDetail = 0.5f;
    fx.ErosionGullyWeight = 0.6f;
    fx.ErosionEdgeRounding = 0.3f;
    fx.ErosionFade = 0.6f;
    return fx;
}

BakeArm BakeNoiseEffect(const Components::TerrainNoiseEffect& fx)
{
    return Bake([&fx](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, fx);
    });
}

} // namespace

// The compile-out. Every erosion knob set hard but the block disarmed: none of
// them may reach the bake, which is what lets the fields ship on the component
// without touching a single authored scene.
TEST(TerrainErosion, StrengthZeroLeavesTheNoiseByteIdentical)
{
    const BakeArm plain = BakeNoiseEffect(SharedNoiseEffect());

    Components::TerrainNoiseEffect armedFields = ErodedNoiseEffect(0.0f);
    armedFields.ErosionOctaves = 6;
    armedFields.ErosionGullyWeight = 1.0f;
    armedFields.ErosionFade = 1.0f;
    armedFields.ErosionFrequency = 5.0f;
    const BakeArm disarmed = BakeNoiseEffect(armedFields);

    ASSERT_FALSE(ChangedSamples(plain.BaseHeights, plain.BakedHeights).empty())
        << "the noise effect baked nothing — the identity below would be vacuous";
    EXPECT_TRUE(BytewiseEqual(plain.BakedHeights, disarmed.BakedHeights))
        << "erosion parameters reached the bake while ErosionStrength was 0";
}

// Strength > 0 routes the bake through the derivative-carrying evaluator; zero
// octaves means the filter itself contributes nothing. What is left is the
// assertion the whole design rests on: the analytic-gradient fBM reproduces
// FBMNoise2D's value expression for expression. A mismatch would mean arming
// erosion shifts the terrain before it carves anything.
TEST(TerrainErosion, ArmedWithZeroOctavesReproducesTheNoiseBitForBit)
{
    const BakeArm plain = BakeNoiseEffect(SharedNoiseEffect());

    Components::TerrainNoiseEffect armed = ErodedNoiseEffect(1.0f);
    armed.ErosionOctaves = 0;
    const BakeArm noGullies = BakeNoiseEffect(armed);

    ASSERT_FALSE(ChangedSamples(plain.BaseHeights, plain.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(plain.BakedHeights, noGullies.BakedHeights))
        << "the derivative-carrying fBM does not reproduce FBMNoise2D bit for bit";
}

TEST(TerrainErosion, ArmedErosionChangesHeightsDeterministicallyPerSeed)
{
    const BakeArm plain = BakeNoiseEffect(SharedNoiseEffect());
    const BakeArm eroded = BakeNoiseEffect(ErodedNoiseEffect(1.0f));
    const BakeArm again = BakeNoiseEffect(ErodedNoiseEffect(1.0f));

    Components::TerrainNoiseEffect otherSeed = ErodedNoiseEffect(1.0f);
    otherSeed.Seed = SharedNoiseEffect().Seed + 7u;
    const BakeArm reseeded = BakeNoiseEffect(otherSeed);

    EXPECT_FALSE(BytewiseEqual(plain.BakedHeights, eroded.BakedHeights))
        << "arming erosion changed nothing";
    EXPECT_TRUE(BytewiseEqual(eroded.BakedHeights, again.BakedHeights))
        << "two identical erosion bakes disagree — the filter is not deterministic";
    EXPECT_FALSE(BytewiseEqual(eroded.BakedHeights, reseeded.BakedHeights))
        << "the erosion pattern ignores the noise seed";
}

namespace
{

// Summed |difference| of the carved offset measured across the base gradient
// versus along it, over a grid of sloped samples.
//
// The differences are taken OF THE FILTER — a pure function of (x, z) — which is
// a different operation from differencing the accumulated heightfield, the
// mixed-stage read the design forbids and the reason the gradient is analytic.
struct GullyAnisotropy
{
    float64 Across = 0.0;
    float64 Along = 0.0;
    std::size_t Samples = 0;
};

GullyAnisotropy MeasureGullyAnisotropy(uint32 noiseOctaves, float32 baseFrequency,
                                       float32 erosionFrequency, float32 sampleSpacing,
                                       float32 step)
{
    using namespace GameEngine::Noise;
    using namespace GameEngine::TerrainECS::Erosion;

    ErosionParams params{};
    params.Strength = 1.0f;
    params.Octaves = 1u;       // one octave: no feedback rotating the stripes
    params.Frequency = erosionFrequency;
    params.Detail = 1.0f;
    params.GullyWeight = 0.0f; // isolate orientation from branching
    params.EdgeRounding = 1.0f;
    params.Fade = 0.0f;        // do not fade the signal being measured

    constexpr float32 kAmp = 1.0f;
    constexpr uint32 kSeed = 4242u;
    constexpr float32 kLacunarity = 2.0f;
    constexpr float32 kPersistence = 0.5f;

    auto plain = [&](float32 x, float32 z) {
        return FBMNoise2DWithDerivatives(x, z, baseFrequency, kAmp, noiseOctaves, kSeed,
                                         kLacunarity, kPersistence);
    };
    // The carved offset alone — subtracting the base removes the terrain's own
    // slope from both readings, leaving only what the filter added.
    auto carve = [&](float32 x, float32 z) {
        return ErodedFBMNoise2D(x, z, baseFrequency, kAmp, noiseOctaves, kSeed, kLacunarity,
                                kPersistence, params) - plain(x, z).Value;
    };

    GullyAnisotropy out{};
    for (int32 iz = 0; iz < 32; ++iz)
    {
        for (int32 ix = 0; ix < 32; ++ix)
        {
            const float32 x = sampleSpacing * static_cast<float32>(ix) + 1.3f;
            const float32 z = sampleSpacing * static_cast<float32>(iz) + 2.7f;
            const NoiseSample base = plain(x, z);
            const float32 len = std::sqrt(base.DValueDX * base.DValueDX +
                                          base.DValueDZ * base.DValueDZ);
            if (len < 1e-6f)
                continue; // flat ground carries no flow direction to test
            const float32 fx = base.DValueDX / len;
            const float32 fz = base.DValueDZ / len;

            out.Along += std::abs(carve(x + fx * step, z + fz * step) -
                                  carve(x - fx * step, z - fz * step));
            out.Across += std::abs(carve(x - fz * step, z + fx * step) -
                                   carve(x + fz * step, z - fx * step));
            ++out.Samples;
        }
    }
    return out;
}

} // namespace

// A gully is a channel that follows the flow, so the carved height varies across
// the slope and stays put along it.
//
// Two effects bound how anisotropic the result can measure. The stripe axis is
// recomputed from the gradient at every sample, so where the gradient turns
// inside the measurement stencil the axis turns with it; and the cell blend plus
// partial normalisation modulate the wave's AMPLITUDE isotropically, on the same
// cell scale the phase varies on — so that floor does not shrink by moving the
// frequencies apart. Both are properties of the filter, not of the instrument.
// Isotropic output would read 1.0; the gates sit well above that and well below
// what the construction actually delivers.
TEST(TerrainErosion, GulliesRunAcrossTheGradientNotAlongIt)
{
    // Base lattice 100 world units, gully wavelength 5: a smooth slope carrying
    // fine gullies, where the flow direction barely turns across a stencil.
    const GullyAnisotropy smooth = MeasureGullyAnisotropy(1u, 0.01f, 20.0f, 9.3f, 0.25f);
    ASSERT_GT(smooth.Samples, std::size_t{800}) << "too few sloped samples to conclude anything";
    ASSERT_GT(smooth.Across, 0.0) << "the filter carved nothing";
    EXPECT_GT(smooth.Across, smooth.Along * 3.0)
        << "gullies are not gradient-aligned on a smooth slope: across " << smooth.Across
        << " vs along " << smooth.Along;

    // Ordinary multi-octave terrain, where the finest base octave turns the flow
    // direction within the stencil and dilutes the margin. The orientation must
    // still survive: a regression that lost it entirely lands near 1.0.
    const GullyAnisotropy rough = MeasureGullyAnisotropy(3u, 0.05f, 3.0f, 2.9f, 0.3f);
    ASSERT_GT(rough.Samples, std::size_t{800});
    EXPECT_GT(rough.Across, rough.Along * 1.5)
        << "gullies lost their gradient alignment on multi-octave terrain: across "
        << rough.Across << " vs along " << rough.Along;
}

// Scene text is not a trusted input: the schema's ParseU32 accepts any uint32,
// and every octave count drives a loop that runs once per terrain texel. Four
// billion octaves is not a wrong-looking terrain, it is a bake that never
// returns — and because an armed erosion block forces the bake to CPU, there is
// no GPU timeout to end it either. The oracle is that an absurd count bakes the
// SAME BYTES as the clamp bound, which it can only do by clamping promptly.
TEST(TerrainErosion, AbsurdErosionOctaveCountClampsToTheBound)
{
    Components::TerrainNoiseEffect atBound = ErodedNoiseEffect(1.0f);
    atBound.ErosionOctaves = Components::kMaxNoiseOctaves;

    Components::TerrainNoiseEffect absurd = ErodedNoiseEffect(1.0f);
    absurd.ErosionOctaves = 4000000000u;

    const BakeArm bounded = BakeNoiseEffect(atBound);
    const BakeArm huge = BakeNoiseEffect(absurd);

    ASSERT_FALSE(ChangedSamples(bounded.BaseHeights, bounded.BakedHeights).empty())
        << "the eroded noise baked nothing — the identity below would be vacuous";
    EXPECT_TRUE(BytewiseEqual(bounded.BakedHeights, huge.BakedHeights))
        << "an out-of-range erosion octave count did not clamp to kMaxNoiseOctaves";
}

// The same class on the base fBM octave count, which predates the erosion block
// entirely: TerrainNoiseEffect.octaves reaches FBMNoise2D's per-texel loop by
// exactly the same untrusted path, with no clamp anywhere between.
TEST(TerrainErosion, AbsurdNoiseOctaveCountClampsToTheBound)
{
    Components::TerrainNoiseEffect atBound = SharedNoiseEffect();
    atBound.Octaves = Components::kMaxNoiseOctaves;

    Components::TerrainNoiseEffect absurd = SharedNoiseEffect();
    absurd.Octaves = 4000000000u;

    const BakeArm bounded = BakeNoiseEffect(atBound);
    const BakeArm huge = BakeNoiseEffect(absurd);

    ASSERT_FALSE(ChangedSamples(bounded.BaseHeights, bounded.BakedHeights).empty());
    EXPECT_TRUE(BytewiseEqual(bounded.BakedHeights, huge.BakedHeights))
        << "an out-of-range fBM octave count did not clamp to kMaxNoiseOctaves";
}

// ---------------------------------------------------------------------------
// 4 · The canonical effect list, as the authoring surfaces consume it
//
// ModifierEffectComponents is the one place the effect set is written down. The
// gather folds it directly; the scene loader, the Add Effect picker, the Add
// Component picker's exclusion and the inspector's section titles fold it
// through the helpers below. These oracles are what makes "registered in one
// place" true rather than aspirational — a consumer that quietly restates the
// list will disagree with them.
// ---------------------------------------------------------------------------

TEST(TerrainEffectRegistry, EveryCanonicalEffectIsRecognisedAndCarriesChrome)
{
    const auto effects = TerrainECS::TerrainEffectTypes();
    ASSERT_EQ(effects.size(), TerrainECS::kTerrainEffectTypeCount)
        << "the folded list disagrees with the count consumers static_assert against";

    for (const auto& effect : effects)
    {
        EXPECT_NE(effect.TypeId, 0u);
        EXPECT_TRUE(TerrainECS::IsTerrainEffectComponent(effect.TypeId));
        EXPECT_EQ(TerrainECS::FindTerrainEffectType(effect.TypeId), &effect);
        EXPECT_FALSE(effect.Title.empty()) << "an effect with no title renders a blank picker row";
        EXPECT_FALSE(effect.Description.empty());
    }
}

// The type ids the list must contain — spelled out here rather than folded, so
// that dropping or substituting a type in ModifierEffectComponents fails instead
// of silently redefining what "canonical" means. Adding one is meant to land
// here too: the count is the point, so the test is not named after it.
TEST(TerrainEffectRegistry, ListContainsExactlyTheCanonicalEffectComponents)
{
    const ECS::ComponentTypeId expected[] = {
        ECS::GetComponentTypeId<Components::TerrainFlattenEffect>(),
        ECS::GetComponentTypeId<Components::TerrainHeightOffsetEffect>(),
        ECS::GetComponentTypeId<Components::TerrainNoiseEffect>(),
        ECS::GetComponentTypeId<Components::TerrainStampEffect>(),
        ECS::GetComponentTypeId<Components::TerrainPaintLayerEffect>(),
        ECS::GetComponentTypeId<Components::TerrainSurfaceRulesEffect>(),
        ECS::GetComponentTypeId<Components::TerrainGroundClaimEffect>(),
        ECS::GetComponentTypeId<Components::TerrainGrassEffect>(),
    };

    const auto effects = TerrainECS::TerrainEffectTypes();
    ASSERT_EQ(effects.size(), std::size(expected));
    for (const ECS::ComponentTypeId id : expected)
        EXPECT_TRUE(TerrainECS::IsTerrainEffectComponent(id));
}

// The volume owns the region, not the stack, and the other modifier ROOTS are
// not effects either. A false positive here would pull a root out of the Add
// Component picker and into the Add Effect picker.
TEST(TerrainEffectRegistry, VolumeAndOtherRootsAreNotEffects)
{
    EXPECT_FALSE(TerrainECS::IsTerrainEffectComponent(
        ECS::GetComponentTypeId<Components::TerrainModifierVolume>()));
    EXPECT_FALSE(TerrainECS::IsTerrainEffectComponent(
        ECS::GetComponentTypeId<Components::TerrainSculptZone>()));
    EXPECT_FALSE(TerrainECS::IsTerrainEffectComponent(
        ECS::GetComponentTypeId<Components::TerrainPaintZone>()));
    EXPECT_FALSE(TerrainECS::IsTerrainEffectComponent(
        ECS::GetComponentTypeId<Components::Transform>()));
    EXPECT_EQ(TerrainECS::FindTerrainEffectType(0), nullptr);
}

// Add Effect stacks a new effect ON TOP: max + 1, counting every canonical type,
// which is the same rule the loader's migration uses for converted modifiers.
TEST(TerrainEffectRegistry, NextStackOrderIsOnePastTheHighestPresent)
{
    ECS::World w;
    const auto e = CreateCircleVolume(w);
    EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 0);

    Components::TerrainFlattenEffect flatten{};
    flatten.StackOrder = 4;
    w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);
    EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 5);

    // A LOWER order on a different type must not pull the next slot back down.
    Components::TerrainPaintLayerEffect paint{};
    paint.StackOrder = 1;
    w.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, paint);
    EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 5);

    Components::TerrainNoiseEffect noise{};
    noise.StackOrder = 9;
    w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, noise);
    EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 10);
}

// Add Effect and the Add Component presets both pick an effect by type id, so
// every canonical type must be addable that way and land at the stack order it
// was given — otherwise a picked effect either never arrives, or arrives at
// order 0 and silently applies before everything already there.
//
// ECS::ComponentFactory is deliberately NOT the path under test: the effects'
// reflection TU is in no build target, so they have no factory entry at all
// (asserted below, so this comment cannot rot silently).
TEST(TerrainEffectRegistry, EveryEffectIsAddableByTypeIdAtAGivenStackOrder)
{
    for (const auto& effect : TerrainECS::TerrainEffectTypes())
    {
        ECS::World w;
        const auto e = CreateCircleVolume(w);

        ASSERT_TRUE(TerrainECS::AddTerrainEffectDefault(w, e, effect.TypeId, 7))
            << "not addable by type id: " << effect.Title;
        EXPECT_TRUE(w.HasComponent(e, effect.TypeId))
            << "reported added but the entity does not carry it: " << effect.Title;

        // Read the order back through the same fold the bake orders the stack with.
        EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 8)
            << "the stack order did not land on " << effect.Title;

        // One component of a type per entity: a second add must decline.
        EXPECT_FALSE(TerrainECS::AddTerrainEffectDefault(w, e, effect.TypeId, 2))
            << "added a second " << effect.Title << " to one entity";
        EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 8)
            << "the declined add still rewrote StackOrder on " << effect.Title;

        // The type-erased remove the editor's Undo uses must reach it too.
        EXPECT_TRUE(w.RemoveComponentByTypeIdImmediate(e, effect.TypeId))
            << "Undo cannot remove " << effect.Title;
    }
}

// The reason the fold above exists rather than a ComponentFactory call. If this
// ever starts failing, an effect has gained a ComponentFactory entry
// and the Add Component picker's effect exclusion becomes load-bearing.
TEST(TerrainEffectRegistry, EffectsHaveNoComponentFactoryEntry)
{
    for (const auto& effect : TerrainECS::TerrainEffectTypes())
    {
        EXPECT_FALSE(ECS::ComponentFactory::Has(effect.TypeId))
            << effect.Title
            << " now has a factory: TerrainModifierEffectReflection.cpp reached a build target. "
               "Effects must stay out of the Add Component picker (InspectorPanel BuildSingles).";
    }
}

// A non-effect type id must not be silently accepted — that would make an Add
// Effect bug look like a successful add.
TEST(TerrainEffectRegistry, AddRejectsNonEffectTypes)
{
    ECS::World w;
    const auto e = CreateCircleVolume(w);
    EXPECT_FALSE(TerrainECS::AddTerrainEffectDefault(
        w, e, ECS::GetComponentTypeId<Components::TerrainModifierVolume>(), 2));
    EXPECT_FALSE(TerrainECS::AddTerrainEffectDefault(w, e, 0, 0));
}

// The inspector's drag-reorder renumbers a volume's whole stack by type id, so
// every canonical type must be stampable that way and land on the order it was
// given. Folded over the canonical list: an effect added there is covered here
// without an edit, which is the property the drag-reorder site now relies on.
TEST(TerrainEffectRegistry, EveryEffectIsStampableByTypeId)
{
    for (const auto& effect : TerrainECS::TerrainEffectTypes())
    {
        ECS::World w;
        const auto e = CreateCircleVolume(w);
        ASSERT_TRUE(TerrainECS::AddTerrainEffectDefault(w, e, effect.TypeId, 0));

        ASSERT_TRUE(TerrainECS::SetTerrainEffectStackOrder(w, e, effect.TypeId, 7))
            << "not stampable by type id: " << effect.Title;

        // Read back through the same fold the bake orders the stack with: with
        // one effect present, "next" is exactly one past the order just written.
        EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 8)
            << "the stamped order did not land on " << effect.Title;
    }
}

// Renumbering must move only the type it names. A stamp that reached its
// siblings would collapse a dragged stack onto one order.
TEST(TerrainEffectRegistry, StampWritesOnlyTheNamedEffect)
{
    ECS::World w;
    const auto e = CreateCircleVolume(w);

    Components::TerrainFlattenEffect flatten{};
    flatten.StackOrder = 0;
    w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);
    Components::TerrainNoiseEffect noise{};
    noise.StackOrder = 1;
    w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, noise);

    EXPECT_TRUE(TerrainECS::SetTerrainEffectStackOrder(
        w, e, ECS::GetComponentTypeId<Components::TerrainNoiseEffect>(), 0));
    EXPECT_TRUE(TerrainECS::SetTerrainEffectStackOrder(
        w, e, ECS::GetComponentTypeId<Components::TerrainFlattenEffect>(), 1));

    EXPECT_EQ(w.GetComponent<Components::TerrainNoiseEffect>(e)->StackOrder, 0);
    EXPECT_EQ(w.GetComponent<Components::TerrainFlattenEffect>(e)->StackOrder, 1);
}

// The drag-reorder walks every section on the entity and counts the stamps that
// took, so a type the entity does not carry — or one that is not an effect at
// all — must decline rather than consume a stack slot.
TEST(TerrainEffectRegistry, StampDeclinesForAbsentAndNonEffectTypes)
{
    ECS::World w;
    const auto e = CreateCircleVolume(w);

    EXPECT_FALSE(TerrainECS::SetTerrainEffectStackOrder(
        w, e, ECS::GetComponentTypeId<Components::TerrainFlattenEffect>(), 3))
        << "stamped an effect the entity does not carry";

    EXPECT_FALSE(TerrainECS::SetTerrainEffectStackOrder(
        w, e, ECS::GetComponentTypeId<Components::TerrainModifierVolume>(), 3));
    EXPECT_FALSE(TerrainECS::SetTerrainEffectStackOrder(
        w, e, ECS::GetComponentTypeId<Components::TerrainSculptZone>(), 3));
    EXPECT_FALSE(TerrainECS::SetTerrainEffectStackOrder(w, e, 0, 3));

    // A declined stamp must not have added the component on the way past.
    EXPECT_FALSE(w.HasComponent(e, ECS::GetComponentTypeId<Components::TerrainFlattenEffect>()));
    EXPECT_EQ(TerrainECS::NextEffectStackOrder(w, e), 0);
}

// ---------------------------------------------------------------------------
// 9 · Shape::Global — the scope with no footprint
// ---------------------------------------------------------------------------
//
// Built to terrain-authoring-design.html §5.1 (the shape row: "infinite
// footprint, weight 1 everywhere") plus its review finding SF-12, which is the
// part that collides with shipped machinery: the gather, dirty-rect diff,
// streamed-tile eligibility and GPU-bake check are all AABB-driven, so a global
// volume carries an INFINITE world AABB and every one of them includes it
// without a special case. That AABB reaches a float-to-int sample-index
// conversion, which is undefined outside int32 — ModifierSampleIndex is the
// clamp that makes an unbounded box legal there.
//
// The instrument for "it covered everything" is the changed-sample SET over the
// whole heightfield, for the same reason the rest of this file uses it: fBM is
// exactly zero at its lattice corners, so a single probe sample proves nothing.

namespace
{
// A global volume. Its entity sits far off the terrain, and its rect/circle
// extents are deliberately tiny relative to that distance: if any footprint
// maths survived, the terrain would be entirely outside it and nothing would
// bake.
ECS::EntityHandle CreateGlobalVolume(ECS::World& world, float32 priority = 0.0f,
                                     float32 weight = 1.0f)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Global;
    vol.Radius = 5.0f;
    vol.RectHalfX = 5.0f;
    vol.RectHalfZ = 5.0f;
    vol.Falloff = 3.0f;
    vol.Weight = weight;
    vol.Priority = priority;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(
        e, IdentityAt(5000.0f, 0.0f, -5000.0f));
    return e;
}

Components::TerrainHeightOffsetEffect OffsetEffect(float32 metres)
{
    Components::TerrainHeightOffsetEffect fx{};
    fx.Offset = metres;
    fx.Blend = Components::TerrainModifierBlend::Add;
    return fx;
}

std::size_t TotalSamples()
{
    return static_cast<std::size_t>(kHeightmapDim) * kHeightmapDim;
}
} // namespace

// The headline behaviour: a volume 5 km off the terrain with 5 m extents still
// reaches every sample. This is the planar form of the design's "a global volume
// affects a tile it does not geometrically overlap".
TEST(TerrainVolumeGlobal, GlobalVolumeReachesEverySampleFromOutsideTheTerrain)
{
    constexpr float32 kOffsetM = 8.0f;

    const BakeArm arm = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateGlobalVolume(w);
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, OffsetEffect(kOffsetM));
    });

    ASSERT_EQ(arm.BakedHeights.size(), TotalSamples());
    EXPECT_EQ(ChangedSamples(arm.BaseHeights, arm.BakedHeights).size(), TotalSamples())
        << "a global volume left samples untouched — its footprint is not global";

    // Weight 1 everywhere means the offset is the SAME at every sample, corners
    // included. A ramp of any kind shows up here as a spread.
    const float32 expected = kOffsetM / kHeightScale;
    for (std::size_t i = 0; i < arm.BakedHeights.size(); ++i)
        ASSERT_NEAR(arm.BakedHeights[i] - arm.BaseHeights[i], expected, 1e-6f)
            << "sample " << i << " received a partial weight — a global volume has no falloff";
}

// The negative half of "no footprint": the shape fields must not reach the bake
// at all. Two arms whose extents and falloff differ by two orders of magnitude
// have to be byte-identical.
//
// The Stamp effect is what gives this test teeth. HeightOffset never consults
// the extents, so an offset-only pair would agree even if the extents WERE live
// — true for the wrong reason. A stamp's mask projection is the one thing that
// reads Radius/RectHalf/Falloff, and its UV clamps to [0,1], so a live extent
// does not merely shift the mask: it extrudes the border texel across the whole
// terrain. Both arms carry one.
TEST(TerrainVolumeGlobal, ExtentsAndFalloffDoNotAffectAGlobalBake)
{
    constexpr float32 kOffsetM = 8.0f;

    auto author = [](float32 extent, float32 falloff) {
        return [extent, falloff](ECS::World& w, SplineECS::SplineService&) {
            auto e = w.CreateEntity();
            Components::TerrainModifierVolume vol{};
            vol.Shape = Components::TerrainVolumeShape::Global;
            vol.Radius = extent;
            vol.RectHalfX = extent;
            vol.RectHalfZ = extent;
            vol.Falloff = falloff;
            w.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
            w.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(kModX, kModY, kModZ));
            w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(
                e, OffsetEffect(kOffsetM));

            Components::TerrainStampEffect stamp{};
            stamp.Blend = Components::TerrainModifierBlend::Add;
            stamp.HeightScale = 22.0f;
            stamp.StackOrder = 1;
            stamp.StampAssetGuid.Set(StampMaskGuid());
            w.AddComponentImmediate<Components::TerrainStampEffect>(e, stamp);
        };
    };

    const BakeArm tiny = Bake(author(1.0f, 0.0f), /*seedStampMask=*/true);
    const BakeArm huge = Bake(author(400.0f, 250.0f), /*seedStampMask=*/true);

    EXPECT_FALSE(ChangedSamples(tiny.BaseHeights, tiny.BakedHeights).empty())
        << "neither arm baked anything — the equality below would be vacuous";
    EXPECT_TRUE(BytewiseEqual(tiny.BakedHeights, huge.BakedHeights))
        << "a global volume's Radius/RectHalf/Falloff changed its bake — they must be inert";
}

// The refusal stated as a value rather than as agreement between two arms: a
// stamp is a projection into a footprint and a global volume has none. Left in,
// the mask's UV clamp would paint its border texel over the entire world at full
// weight — with Radius, RectHalfX/Z and Falloff all hidden from the inspector for
// this shape, so nothing on screen would explain the result.
//
// A HeightOffset shares the volume, so the refusal is provably scoped to the
// stamp: the volume must still bake exactly as if the stamp were absent.
TEST(TerrainVolumeGlobal, StampEffectIsRefusedInsideAGlobalVolume)
{
    constexpr float32 kOffsetM = 8.0f;

    auto author = [](bool withStamp) {
        return [withStamp](ECS::World& w, SplineECS::SplineService&) {
            const auto e = CreateGlobalVolume(w);
            w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(
                e, OffsetEffect(kOffsetM));
            if (!withStamp)
                return;
            Components::TerrainStampEffect stamp{};
            stamp.Blend = Components::TerrainModifierBlend::Add;
            stamp.HeightScale = 22.0f;
            stamp.StackOrder = 1;
            stamp.StampAssetGuid.Set(StampMaskGuid());
            w.AddComponentImmediate<Components::TerrainStampEffect>(e, stamp);
        };
    };

    // Positive control: the same stamp on a CIRCLE volume must bake, or the
    // equality below would also hold for a stamp that is broken everywhere.
    const BakeArm circleStamp = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateCircleVolume(w);
        Components::TerrainStampEffect stamp{};
        stamp.Blend = Components::TerrainModifierBlend::Add;
        stamp.HeightScale = 22.0f;
        stamp.StampAssetGuid.Set(StampMaskGuid());
        w.AddComponentImmediate<Components::TerrainStampEffect>(e, stamp);
    }, /*seedStampMask=*/true);
    EXPECT_FALSE(ChangedSamples(circleStamp.BaseHeights, circleStamp.BakedHeights).empty())
        << "the stamp baked nothing even on a circle volume — this test proves nothing";

    const BakeArm withStamp = Bake(author(true), /*seedStampMask=*/true);
    const BakeArm without = Bake(author(false), /*seedStampMask=*/true);

    EXPECT_TRUE(BytewiseEqual(withStamp.BakedHeights, without.BakedHeights))
        << "the Stamp effect on a global volume reached the bake";

    // And the surviving effect is untouched: a flat offset at every sample, with
    // no trace of a mask gradient.
    const float32 expected = kOffsetM / kHeightScale;
    ASSERT_EQ(withStamp.BakedHeights.size(), TotalSamples());
    for (std::size_t i = 0; i < withStamp.BakedHeights.size(); ++i)
        ASSERT_NEAR(withStamp.BakedHeights[i] - withStamp.BaseHeights[i], expected, 1e-6f)
            << "sample " << i << " carries something other than the height offset";
}

// A global volume whose ONLY effect is a stamp has an empty stack after the
// refusal, so it is dropped like any other effectless volume rather than
// surviving as a modifier that dirties the world and does nothing.
TEST(TerrainVolumeGlobal, StampOnlyGlobalVolumeBakesNothingAtAll)
{
    const BakeArm arm = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateGlobalVolume(w);
        Components::TerrainStampEffect stamp{};
        stamp.Blend = Components::TerrainModifierBlend::Add;
        stamp.HeightScale = 22.0f;
        stamp.StampAssetGuid.Set(StampMaskGuid());
        w.AddComponentImmediate<Components::TerrainStampEffect>(e, stamp);
    }, /*seedStampMask=*/true);

    EXPECT_TRUE(BytewiseEqual(arm.BaseHeights, arm.BakedHeights))
        << "a stamp-only global volume changed the heightfield";
}

// The master weight is orthogonal to the shape, so it still scales the stack.
// Without this, "weight 1 everywhere" could be read as "ignore Weight too".
TEST(TerrainVolumeGlobal, MasterWeightStillScalesAGlobalVolume)
{
    constexpr float32 kOffsetM = 8.0f;

    const BakeArm full = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateGlobalVolume(w, 0.0f, 1.0f);
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, OffsetEffect(kOffsetM));
    });
    const BakeArm half = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateGlobalVolume(w, 0.0f, 0.5f);
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, OffsetEffect(kOffsetM));
    });

    ASSERT_EQ(full.BakedHeights.size(), TotalSamples());
    for (std::size_t i = 0; i < full.BakedHeights.size(); ++i)
    {
        ASSERT_NEAR(full.BakedHeights[i] - full.BaseHeights[i], kOffsetM / kHeightScale, 1e-6f);
        ASSERT_NEAR(half.BakedHeights[i] - half.BaseHeights[i], 0.5f * kOffsetM / kHeightScale,
                    1e-6f);
    }
}

namespace
{
// The ordering fixture: two SHAPE-scoped volumes and one global one, stacked by
// Priority so that only the global's position in the sequence decides the
// result.
//
//   circle P=0    HeightOffset Add +K
//   global P=gp   Flatten Set to T      (Set is what makes the order observable)
//   circle P=2    HeightOffset Add +M
//
// Inside the circle the three orderings are arithmetically distinct:
//   global between  ->  T + M
//   global last     ->  T
//   global first    ->  T + K + M
// Outside the circle only the global applies, so the height is T wherever the
// stack ordering put it.
constexpr float32 kOrderTargetM = 20.0f;  // T — the global's flatten target
constexpr float32 kOrderFirstM = 9.0f;    // K — the low-priority circle's offset
constexpr float32 kOrderLastM = 5.0f;     // M — the high-priority circle's offset
constexpr float32 kOrderRadius = 40.0f;

ECS::EntityHandle CreateHardCircleVolume(ECS::World& world, float32 priority)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = kOrderRadius;
    vol.Falloff = 0.0f;       // hard edge: weight is exactly 1 inside, 0 outside
    vol.FalloffInward = 0.0f;
    vol.Priority = priority;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

BakeArm BakeGlobalAtPriority(float32 globalPriority)
{
    return Bake([globalPriority](ECS::World& w, SplineECS::SplineService&) {
        const auto low = CreateHardCircleVolume(w, 0.0f);
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(
            low, OffsetEffect(kOrderFirstM));

        const auto global = CreateGlobalVolume(w, globalPriority);
        Components::TerrainFlattenEffect flatten{};
        flatten.UseVolumeHeight = false;
        flatten.TargetHeight = kOrderTargetM;
        flatten.Blend = Components::TerrainModifierBlend::Set;
        w.AddComponentImmediate<Components::TerrainFlattenEffect>(global, flatten);

        const auto high = CreateHardCircleVolume(w, 2.0f);
        w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(
            high, OffsetEffect(kOrderLastM));
    });
}
} // namespace

// The slice's load-bearing composition oracle: a global-scoped modifier is
// ordered by exactly the Priority machinery the shape-scoped ones use, and
// composes in that position rather than being appended, prepended, or applied on
// some separate pass.
TEST(TerrainVolumeGlobal, GlobalVolumeComposesInPriorityOrderBetweenTwoShapeVolumes)
{
    const BakeArm between = BakeGlobalAtPriority(1.0f);
    ASSERT_EQ(between.BakedHeights.size(), TotalSamples());

    // A sample well inside the circles, and one well outside them (the terrain
    // is 256 m across, the circles 40 m in radius at the origin).
    const std::size_t inside = SampleIndexAt(0.0f, 0.0f);
    const std::size_t outside = SampleIndexAt(-100.0f, -100.0f);
    ASSERT_LT(inside, between.BakedHeights.size());
    ASSERT_LT(outside, between.BakedHeights.size());

    EXPECT_NEAR(between.BakedHeights[inside], (kOrderTargetM + kOrderLastM) / kHeightScale, 1e-6f)
        << "inside the circles the global's flatten must land BETWEEN the two offsets";
    EXPECT_NEAR(between.BakedHeights[outside], kOrderTargetM / kHeightScale, 1e-6f)
        << "outside every shape only the global applies, so its target is the height";

    // The discriminator: moving the global to either end of the same stack has
    // to change the result. Without it, the assertions above would also pass on
    // an implementation that applied globals last (or first) unconditionally.
    const BakeArm last = BakeGlobalAtPriority(3.0f);
    const BakeArm first = BakeGlobalAtPriority(-1.0f);

    EXPECT_NEAR(last.BakedHeights[inside], kOrderTargetM / kHeightScale, 1e-6f);
    EXPECT_NEAR(first.BakedHeights[inside],
                (kOrderTargetM + kOrderFirstM + kOrderLastM) / kHeightScale, 1e-6f);

    EXPECT_FALSE(BytewiseEqual(between.BakedHeights, last.BakedHeights))
        << "the global volume's Priority did not change where it applied in the stack";
    EXPECT_FALSE(BytewiseEqual(between.BakedHeights, first.BakedHeights));

    // Outside every shape the ordering cannot matter — only the global is there.
    EXPECT_NEAR(last.BakedHeights[outside], kOrderTargetM / kHeightScale, 1e-6f);
    EXPECT_NEAR(first.BakedHeights[outside], kOrderTargetM / kHeightScale, 1e-6f);
}

// Effects inside a global volume still run in StackOrder — the second ordering
// level is untouched by the scope.
TEST(TerrainVolumeGlobal, EffectStackOrderStillDecidesInsideAGlobalVolume)
{
    constexpr float32 kTargetM = 12.0f;
    constexpr float32 kOffsetM = 7.0f;

    auto author = [](int32 flattenOrder, int32 offsetOrder) {
        return [flattenOrder, offsetOrder](ECS::World& w, SplineECS::SplineService&) {
            const auto e = CreateGlobalVolume(w);
            Components::TerrainFlattenEffect flatten{};
            flatten.UseVolumeHeight = false;
            flatten.TargetHeight = kTargetM;
            flatten.Blend = Components::TerrainModifierBlend::Set;
            flatten.StackOrder = flattenOrder;
            w.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);

            Components::TerrainHeightOffsetEffect offset = OffsetEffect(kOffsetM);
            offset.StackOrder = offsetOrder;
            w.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, offset);
        };
    };

    const BakeArm flattenFirst = Bake(author(0, 1));
    const BakeArm offsetFirst = Bake(author(1, 0));

    const std::size_t probe = SampleIndexAt(0.0f, 0.0f);
    ASSERT_LT(probe, flattenFirst.BakedHeights.size());

    // Flatten then offset: T + O. Offset then flatten: the flatten overwrites,
    // so the offset is lost entirely.
    EXPECT_NEAR(flattenFirst.BakedHeights[probe], (kTargetM + kOffsetM) / kHeightScale, 1e-6f);
    EXPECT_NEAR(offsetFirst.BakedHeights[probe], kTargetM / kHeightScale, 1e-6f);
    EXPECT_FALSE(BytewiseEqual(flattenFirst.BakedHeights, offsetFirst.BakedHeights))
        << "StackOrder stopped mattering inside a global volume";
}

// The splat half of the compose contract: paint is an ordinary effect, and a
// global volume carrying one paints every texel.
TEST(TerrainVolumeGlobal, PaintEffectInAGlobalVolumePaintsTheWholeSplatmap)
{
    const BakeArm arm = Bake([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateGlobalVolume(w);
        Components::TerrainPaintLayerEffect fx{};
        fx.LayerIndex = 1;
        fx.Strength = 1.0f;
        fx.Replace = true;
        w.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, fx);
    });

    ASSERT_FALSE(arm.BakedSplat.empty());
    EXPECT_FALSE(BytewiseEqual(arm.BaseSplat, arm.BakedSplat))
        << "a global paint effect wrote no splat texels";
    EXPECT_TRUE(BytewiseEqual(arm.BaseHeights, arm.BakedHeights))
        << "a paint-only global volume must not touch the heightfield";

    // Full-strength Replace over the whole world: every texel is the pure layer.
    for (std::size_t i = 0; i + 3 < arm.BakedSplat.size(); i += 4)
        ASSERT_EQ(arm.BakedSplat[i + 1], 255u)
            << "splat texel " << (i / 4) << " was not reached by the global paint";
}

// SF-12's failure mode in its planar form: the change gate must see a shape
// switch. Global resolves onto the same internal shape as Rectangle, so the
// geometry hash has to carry the scope explicitly — without that, this edit
// hashes identically to the previous bake and the terrain silently keeps it.
TEST(TerrainVolumeGate, SwitchingARectangleVolumeToGlobalRebakes)
{
    VolumeGateScene scene;
    ASSERT_NE(scene.Data, nullptr);

    auto volume = scene.World.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Rectangle;
    vol.RectHalfX = 30.0f;
    vol.RectHalfZ = 30.0f;
    vol.Falloff = 0.0f;
    scene.World.AddComponentImmediate<Components::TerrainModifierVolume>(volume, vol);
    scene.World.AddComponentImmediate<Components::WorldTransform>(
        volume, IdentityAt(0.0f, 0.0f, 0.0f));
    scene.World.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(
        volume, OffsetEffect(8.0f));

    const uint64 settled = SettleAndRequireQuiet(scene.World, scene.System);
    const std::vector<float32> beforeEdit = CopyHeights(*scene.Data);

    // The rectangle covered a 60 m square of a 256 m terrain, so switching to
    // Global has to move samples the rectangle never reached.
    auto* v = scene.World.GetComponentForWrite<Components::TerrainModifierVolume>(volume);
    ASSERT_NE(v, nullptr);
    v->Shape = Components::TerrainVolumeShape::Global;

    Tick(scene.World, scene.System);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "switching a volume's shape to Global did not wake the modifier gather";

    const std::vector<float32> afterEdit = CopyHeights(*scene.Data);
    EXPECT_FALSE(BytewiseEqual(beforeEdit, afterEdit))
        << "switching a rectangle volume to Global did not re-bake the terrain";

    // Not merely "something changed": the corner the rectangle never covered
    // must now carry the offset.
    const std::size_t corner = SampleIndexAt(-120.0f, -120.0f);
    ASSERT_LT(corner, afterEdit.size());
    EXPECT_NE(beforeEdit[corner], afterEdit[corner])
        << "a corner outside the old rectangle stayed at its pre-switch height";
}

// A global volume has no centre direction and no tangent frame, so it joins the
// sphere's unsupported set rather than baking as its authored rectangle.
TEST(TerrainVolumeSphere, GlobalShapedVolumeBakesNothingOnAPlanet)
{
    const float32 maxOffset = BakeSphereMaxAbsOffset([](ECS::World& w, SplineECS::SplineService&) {
        const auto e = CreateSphereVolume(w, Components::TerrainVolumeShape::Global);
        w.AddComponentImmediate<Components::TerrainNoiseEffect>(e, SharedNoiseEffect());
    });
    EXPECT_EQ(maxOffset, 0.0f)
        << "a global volume has no spherical cap — it must not bake as a rectangle";
}

// ---------------------------------------------------------------------------
// 10 · ModifierSampleIndex — the clamp an infinite AABB depends on
// ---------------------------------------------------------------------------

// The reason the helper exists: the bare cast it replaces is undefined for a
// global volume's bounds, and "undefined" in a release build is whatever the
// truncation instruction happens to leave in the register.
TEST(ModifierSampleIndex, InfiniteBoundsClampInsteadOfOverflowing)
{
    constexpr float32 inf = std::numeric_limits<float32>::infinity();
    EXPECT_EQ(ModifierSampleIndex(-inf, 0.0f, 2.0f), -kMaxModifierSampleIndex);
    EXPECT_EQ(ModifierSampleIndex(inf, 0.0f, 2.0f), kMaxModifierSampleIndex);

    // The bound leaves headroom for the "+ 1 + pad" the call sites add.
    EXPECT_LT(static_cast<int64>(kMaxModifierSampleIndex) + 16,
              static_cast<int64>(std::numeric_limits<int32>::max()));
}

// An unresolved spline leaves inverted bounds behind, which can subtract to NaN.
// NaN must land somewhere defined rather than propagating into an index.
TEST(ModifierSampleIndex, NanLandsOnTheLowBound)
{
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    EXPECT_EQ(ModifierSampleIndex(nan, 0.0f, 2.0f), -kMaxModifierSampleIndex);
}

// The clamp must not perturb any finite terrain: over the whole range a real
// heightfield can address, it truncates toward zero exactly as the bare cast did.
TEST(ModifierSampleIndex, FiniteCoordinatesTruncateExactlyLikeTheBareCast)
{
    constexpr float32 origin = -128.0f;
    constexpr float32 spacing = 2.0f;
    for (float32 world = -4096.0f; world <= 4096.0f; world += 0.25f)
    {
        const int32 bare = static_cast<int32>((world - origin) / spacing);
        ASSERT_EQ(ModifierSampleIndex(world, origin, spacing), bare) << "world = " << world;
    }
}
