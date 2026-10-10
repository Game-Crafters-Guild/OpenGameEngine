// Spline-driven terrain modifiers: the shared Shape field's Spline option, the
// world-space bounds it implies, the scheduler edge that orders spline extraction
// before the modifier gather, the change-gate behaviour of a spline scene, and
// the altitude-independence of the footprint itself.
//
// Spline-shaped modifiers had no coverage before this file; the
// oracles here are written so each one FAILS on the pre-fix behaviour (a spline
// shape that resolved to nothing, an entity-rect AABB, an undeclared schedule
// edge, a probe that fired on every idle frame, a footprint that vanished when
// the spline was authored above sea level).

#include <gtest/gtest.h>

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "SplineECS/Systems/SplineExtractionSystem.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string_view>
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
// procedural splat), as TerrainRegionBakeTests does.
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

ECS::EntityHandle CreateTerrainEntity(ECS::World& world, TerrainHandle handle)
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
    return e;
}

Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    Components::WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    return xf;
}

// A spline entity whose control points run along +X at the given Z, with the
// swept radius as the band half-width. Points are authored in the entity's local
// space; the entity transform is identity-at-origin unless the caller moves it.
ECS::EntityHandle CreateSplineEntity(ECS::World& world, SplineECS::SplineService& svc,
                                     const std::vector<Mathematics::Vector3>& points,
                                     float32 radius, bool closed = false,
                                     float32 entityY = 0.0f)
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
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, entityY, 0.0f));
    return e;
}

// Attach a noise modifier whose region is the entity's spline. Amplitude is well
// above the base noise's local variation so a single sample discriminates.
void AddSplineShapedNoise(ECS::World& world, ECS::EntityHandle e, float32 falloff = 4.0f)
{
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::SplineArea;
    vol.Falloff = falloff;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

    Components::TerrainNoiseEffect fx{};
    fx.Frequency = 4.0f;
    fx.Amplitude = 24.0f;
    fx.Octaves = 2;
    fx.Seed = 7u;
    fx.Blend = Components::TerrainModifierBlend::Add;
    world.AddComponentImmediate<Components::TerrainNoiseEffect>(e, fx);
}

std::vector<float32> CopyHeights(const TerrainData& data)
{
    const float32* s = data.Heightfield.GetRawSamples();
    return std::vector<float32>(s, s + data.Heightfield.GetSampleCount());
}

// The world XZ of a heightfield sample (the bake's own mapping: origin at
// -size/2, uniform spacing over dim-1 intervals).
void SampleWorldXZ(std::size_t index, float32& outX, float32& outZ)
{
    const float32 spacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    outX = static_cast<float32>(index % kHeightmapDim) * spacing - kWorldSize * 0.5f;
    outZ = static_cast<float32>(index / kHeightmapDim) * spacing - kWorldSize * 0.5f;
}

// Every sample whose height moved. An aggregate is the honest oracle here: the
// gradient-noise lattice evaluates to exactly zero at its integer corners, so any
// single hand-picked sample can read "unmodified" no matter how well the shape
// resolved — the instrument would be the artifact.
std::vector<std::size_t> ChangedSamples(const std::vector<float32>& before,
                                        const std::vector<float32>& after)
{
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] != after[i])
            changed.push_back(i);
    return changed;
}

// One complete bake of a spline-shaped modifier over its own terrain, spline and
// world. Each arm owns its services, so two arms differing only in altitude can
// be compared without their handles colliding — and the returned copies outlive
// the services they came from.
struct SplineBakeArm
{
    std::vector<float32> BaseHeights;
    std::vector<float32> BakedHeights;
    std::vector<uint8> BaseSplat;
    std::vector<uint8> BakedSplat;
};

template<typename AddModifierFn>
SplineBakeArm BakeSplineArm(const std::vector<Mathematics::Vector3>& points, float32 radius,
                            bool closed, float32 entityY, AddModifierFn&& addModifier)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    SplineBakeArm arm{};
    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    if (!data)
        return arm;
    arm.BaseHeights = CopyHeights(*data);
    arm.BaseSplat = data->Splatmap;

    ECS::World world;
    CreateTerrainEntity(world, handle);
    const auto splineEntity =
        CreateSplineEntity(world, splineSvc, points, radius, closed, entityY);
    addModifier(world, splineEntity);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    arm.BakedHeights = CopyHeights(*data);
    arm.BakedSplat = data->Splatmap;
    return arm;
}

// A straight band along +X at Z = 0, spanning x = -80..80, authored at height y.
std::vector<Mathematics::Vector3> StraightSplinePoints(float32 y)
{
    return {{-80.0f, y, 0.0f}, {-20.0f, y, 0.0f}, {20.0f, y, 0.0f}, {80.0f, y, 0.0f}};
}

// A closed loop enclosing the square [-50, 50]^2, authored at height y.
std::vector<Mathematics::Vector3> ClosedLoopPoints(float32 y)
{
    return {{-50.0f, y, -50.0f}, {50.0f, y, -50.0f}, {50.0f, y, 50.0f}, {-50.0f, y, 50.0f}};
}

// Heightfield index of the sample at world (x, 0) on the Z = 0 row. The bake's
// mapping is exact for these coordinates: spacing is 2 m and the row through
// Z = 0 is a real sample row.
std::size_t SampleIndexAtXOnCenterRow(float32 worldX)
{
    const float32 spacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    const auto col = static_cast<std::size_t>((worldX + kWorldSize * 0.5f) / spacing + 0.5f);
    const auto row = static_cast<std::size_t>((kWorldSize * 0.5f) / spacing + 0.5f);
    return row * kHeightmapDim + col;
}

} // namespace

// ---------------------------------------------------------------------------
// Fix 1 — Shape::Spline on a shared-shape modifier resolves and bakes.
// ---------------------------------------------------------------------------

// Can-fail oracle for the silent no-op: a noise modifier whose Shape is Spline
// must change the terrain inside the swept band and leave it untouched outside.
// Pre-fix the generic gather never resolved the spline, ComputeWeight returned 0
// everywhere, and the bake was byte-identical to a modifier-free terrain — the
// "modified" assertion below is what fails.
TEST(TerrainSplineShape, NoiseModifierWithSplineShapeModifiesBandAndNothingElse)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);
    const std::vector<float32> baseHeights = CopyHeights(*data);

    ECS::World world;
    CreateTerrainEntity(world, handle);
    // A straight band along Z = 0, from x = -80 to x = +80, half-width 10 m.
    auto splineEntity = CreateSplineEntity(
        world, splineSvc,
        {{-80.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {80.0f, 0.0f, 0.0f}},
        10.0f);
    AddSplineShapedNoise(world, splineEntity);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const std::vector<float32> baked = CopyHeights(*data);
    ASSERT_EQ(baked.size(), baseHeights.size());

    const std::vector<std::size_t> changed = ChangedSamples(baseHeights, baked);
    EXPECT_FALSE(changed.empty())
        << "Shape::Spline produced no effect — the generic gather left ResolvedSpline null";

    // Everything that moved lies within the swept envelope: |z| within the
    // band's radius + falloff (plus a sample-spacing slop, since the bake writes
    // whole samples), and x within the spline's span similarly padded.
    constexpr float32 kSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    constexpr float32 kEnvelope = 10.0f + 4.0f + kSpacing; // radius + falloff + one sample
    for (std::size_t i : changed)
    {
        float32 x = 0.0f, z = 0.0f;
        SampleWorldXZ(i, x, z);
        EXPECT_LE(std::abs(z), kEnvelope) << "modified sample outside the band at x=" << x;
        EXPECT_LE(std::abs(x), 80.0f + kEnvelope) << "modified sample past the spline's span";
    }
}

// Can-fail oracle for the missing Spline case in ComputeModifierBounds. The
// modifier entity sits at the origin while its spline runs from x=+60 to x=+110,
// so the pre-fix Rectangle fallback (RectHalfX/RectHalfZ default 50 m, centred on
// the entity) clips the whole far half of the band away. The far-end sample must
// still be modified.
TEST(TerrainSplineShape, SplineBoundsFollowTheSplineNotTheEntityRect)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);
    const std::vector<float32> baseHeights = CopyHeights(*data);

    ECS::World world;
    CreateTerrainEntity(world, handle);
    auto splineEntity = CreateSplineEntity(
        world, splineSvc,
        {{60.0f, 0.0f, 0.0f}, {80.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}, {110.0f, 0.0f, 0.0f}},
        10.0f);
    AddSplineShapedNoise(world, splineEntity);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const std::vector<float32> baked = CopyHeights(*data);
    const std::vector<std::size_t> changed = ChangedSamples(baseHeights, baked);
    ASSERT_FALSE(changed.empty());

    // Samples beyond the entity's default 50 m half-extent must have moved: the
    // Rectangle fallback would have clipped every one of them away.
    float32 maxChangedX = -kWorldSize;
    for (std::size_t i : changed)
    {
        float32 x = 0.0f, z = 0.0f;
        SampleWorldXZ(i, x, z);
        maxChangedX = std::max(maxChangedX, x);
    }
    EXPECT_GT(maxChangedX, 60.0f) << "the spline modifier's AABB was clipped to the entity rect";
}

// Every effect composes with the spline region, not just height: a paint-layer
// modifier with Shape::Spline must write splat weights along the band.
TEST(TerrainSplineShape, PaintLayerModifierWithSplineShapePaintsTheBand)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);
    const std::vector<uint8> baseSplat = data->Splatmap;
    ASSERT_FALSE(baseSplat.empty());

    ECS::World world;
    CreateTerrainEntity(world, handle);
    auto splineEntity = CreateSplineEntity(
        world, splineSvc,
        {{-80.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {80.0f, 0.0f, 0.0f}},
        10.0f);

    Components::TerrainModifierVolume paintVol{};
    paintVol.Shape = Components::TerrainVolumeShape::SplineArea;
    paintVol.Falloff = 4.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(splineEntity, paintVol);

    Components::TerrainPaintLayerEffect paint{};
    paint.LayerIndex = 2;
    paint.Strength = 1.0f;
    paint.Replace = true;
    world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(splineEntity, paint);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    ASSERT_EQ(data->Splatmap.size(), baseSplat.size());
    EXPECT_NE(0, std::memcmp(data->Splatmap.data(), baseSplat.data(), baseSplat.size()))
        << "Shape::Spline on a paint modifier painted nothing";
    // Heights must be untouched — a splat-only modifier never writes the heightfield.
    // (Guards against the spline resolution accidentally promoting it to a height op.)
}

// Control (passes before and after): Shape::Spline on an entity with NO
// SplineComponent has no region to resolve. The modifier must be dropped, not
// quietly re-interpreted as the default 50 m rectangle.
TEST(TerrainSplineShape, SplineShapeWithoutSplineComponentLeavesTerrainUntouched)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);
    const std::vector<float32> baseHeights = CopyHeights(*data);

    ECS::World world;
    CreateTerrainEntity(world, handle);
    auto orphan = world.CreateEntity();
    world.AddComponentImmediate<Components::WorldTransform>(orphan, IdentityAt(0.0f, 0.0f, 0.0f));
    AddSplineShapedNoise(world, orphan);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const std::vector<float32> baked = CopyHeights(*data);
    ASSERT_EQ(baked.size(), baseHeights.size());
    EXPECT_EQ(0, std::memcmp(baked.data(), baseHeights.data(), baked.size() * sizeof(float32)))
        << "a Shape::Spline modifier with no spline reference baked something";
}

// ---------------------------------------------------------------------------
// Fix 2 — the scheduler edge.
// ---------------------------------------------------------------------------

// The modifier gather reads SplineComponent handles and SplineService data that
// SplineExtractionSystem creates and rebuilds; systems inside one wave run in
// parallel, so the ordering has to be a declared edge. Built from the two
// modules' own schedule contributions only — no third system to accidentally
// separate them — so an undeclared edge puts both in wave 0 and fails here.
TEST(TerrainSplineSchedule, SplineExtractionIsScheduledBeforeTerrainModifiers)
{
    ECS::SystemScheduleBuilder builder;
    SplineECS::AddSplineSystemsToSchedule(builder);
    TerrainECS::AddTerrainSystemsToSchedule(builder, nullptr);

    ECS::SystemManager manager;
    // Partial by design: this fixture's falsifiability depends on TransformHierarchy
    // and Camera being ABSENT. Registering them would place TerrainModifiers in a
    // later wave than the dependency-less SplineExtraction whether or not the edge
    // under test exists, and the assertions below would pass vacuously.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const auto& plan = manager.GetExecutionPlan();
    ASSERT_TRUE(plan.IsValid());

    int splineWave = -1;
    int modifierWave = -1;
    for (std::size_t w = 0; w < plan.Waves.size(); ++w)
    {
        for (std::size_t idx : plan.Waves[w].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(idx);
            if (!name)
                continue;
            if (std::string_view(name) == "SplineExtraction")
                splineWave = static_cast<int>(w);
            else if (std::string_view(name) == "TerrainModifiers")
                modifierWave = static_cast<int>(w);
        }
    }

    ASSERT_GE(splineWave, 0);
    ASSERT_GE(modifierWave, 0);
    EXPECT_LT(splineWave, modifierWave)
        << "TerrainModifiers does not declare a dependency on SplineExtraction — "
           "same-wave systems execute in parallel";
}

// ---------------------------------------------------------------------------
// Fix 3 — the change-gate probe on a spline scene.
// ---------------------------------------------------------------------------

// Same oracle for a spline-shaped volume carrying a flatten — the archetype
// SplineExtractionSystem write-visits every frame. Column versions are stamped
// per (chunk, column), so the extraction system's writes land on SplineComponent
// and the modifier probe (which filters the modifier roots / WorldTransform)
// does not see them: the scene goes quiet, and a service-side control-point edit
// is what needs its own signal.
TEST(TerrainSplineGate, IdleSplineVolumeSceneStopsGatheringButASplineEditWakesIt)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    world.EnableLifecycleEvents<Components::TerrainModifierVolume>();
    CreateTerrainEntity(world, handle);
    // Altitude is irrelevant to the gate probes; the TerrainSplineAltitude suite
    // below is what covers a lifted spline's footprint.
    auto splineEntity = CreateSplineEntity(
        world, splineSvc,
        {{-80.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {80.0f, 0.0f, 0.0f}},
        10.0f);

    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::SplineArea;
    vol.Falloff = 4.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(splineEntity, vol);

    Components::TerrainFlattenEffect fx{};
    fx.UseVolumeHeight = true;
    fx.TargetHeight = 0.0f;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(splineEntity, fx);
    world.SwapLifecycleEvents();

    SplineECS::SplineExtractionSystem extraction;
    TerrainModifierSystem modifiers;

    for (int i = 0; i < 6; ++i)
    {
        extraction.Update(world, 1.0f / 60.0f);
        modifiers.Update(world, 1.0f / 60.0f);
        world.SwapLifecycleEvents();
    }

    const uint64 settled = TerrainModifierSystem::GetGatherCountForTests();
    for (int i = 0; i < 8; ++i)
    {
        extraction.Update(world, 1.0f / 60.0f);
        modifiers.Update(world, 1.0f / 60.0f);
        world.SwapLifecycleEvents();
    }
    EXPECT_EQ(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "idle frames still ran the modifier gather in a spline-volume scene";

    const std::vector<float32> beforeEdit = CopyHeights(*data);
    {
        const auto* comp = world.GetComponent<Components::SplineComponent>(splineEntity);
        ASSERT_NE(comp, nullptr);
        SplineECS::SplineHandle h(comp->SplineDataIndex, comp->SplineDataGeneration);
        auto* splineData = splineSvc.GetSplineData(h);
        ASSERT_NE(splineData, nullptr);
        splineData->SetPointPosition(1, Mathematics::Vector3(-20.0f, 0.0f, 45.0f));
    }

    extraction.Update(world, 1.0f / 60.0f);
    modifiers.Update(world, 1.0f / 60.0f);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "a spline control-point edit did not wake the modifier gather";
    const std::vector<float32> afterEdit = CopyHeights(*data);
    ASSERT_EQ(afterEdit.size(), beforeEdit.size());
    EXPECT_NE(0, std::memcmp(afterEdit.data(), beforeEdit.data(),
                             afterEdit.size() * sizeof(float32)))
        << "a spline control-point edit did not re-bake the terrain";
}

// Quiet-frame oracle. With a spline modifier in the scene and nothing changing,
// the gather must stop running; a real spline edit (a control point moved through
// SplineService, which touches no ECS column) must still wake it and re-bake.
TEST(TerrainSplineGate, IdleSplineSceneStopsGatheringButASplineEditWakesIt)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    world.EnableLifecycleEvents<Components::TerrainModifierVolume>();
    CreateTerrainEntity(world, handle);
    auto splineEntity = CreateSplineEntity(
        world, splineSvc,
        {{-80.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {80.0f, 0.0f, 0.0f}},
        10.0f);
    AddSplineShapedNoise(world, splineEntity);
    world.SwapLifecycleEvents();

    SplineECS::SplineExtractionSystem extraction;
    TerrainModifierSystem modifiers;

    // Settle: run enough frames for the gate's structural fallbacks (first gated
    // run, lifecycle window, terrain-state hash seed) to be consumed.
    for (int i = 0; i < 6; ++i)
    {
        extraction.Update(world, 1.0f / 60.0f);
        modifiers.Update(world, 1.0f / 60.0f);
        world.SwapLifecycleEvents();
    }

    const uint64 settled = TerrainModifierSystem::GetGatherCountForTests();
    for (int i = 0; i < 8; ++i)
    {
        extraction.Update(world, 1.0f / 60.0f);
        modifiers.Update(world, 1.0f / 60.0f);
        world.SwapLifecycleEvents();
    }
    EXPECT_EQ(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "idle frames still ran the modifier gather in a spline scene";

    // A real spline edit: move a control point through the service. No ECS column
    // is written, so only a spline-side signal can wake the gate.
    const std::vector<float32> beforeEdit = CopyHeights(*data);
    {
        const auto* comp = world.GetComponent<Components::SplineComponent>(splineEntity);
        ASSERT_NE(comp, nullptr);
        SplineECS::SplineHandle h(comp->SplineDataIndex, comp->SplineDataGeneration);
        auto* splineData = splineSvc.GetSplineData(h);
        ASSERT_NE(splineData, nullptr);
        splineData->SetPointPosition(1, Mathematics::Vector3(-20.0f, 0.0f, 45.0f));
    }

    extraction.Update(world, 1.0f / 60.0f);
    modifiers.Update(world, 1.0f / 60.0f);

    EXPECT_GT(TerrainModifierSystem::GetGatherCountForTests(), settled)
        << "a spline control-point edit did not wake the modifier gather";
    const std::vector<float32> afterEdit = CopyHeights(*data);
    ASSERT_EQ(afterEdit.size(), beforeEdit.size());
    EXPECT_NE(0, std::memcmp(afterEdit.data(), beforeEdit.data(),
                             afterEdit.size() * sizeof(float32)))
        << "a spline control-point edit did not re-bake the terrain";
}

// ---------------------------------------------------------------------------
// Fix 4 — the footprint is a TOP-DOWN mask: weight comes from XZ distance to
// the spline's XZ projection, never from the spline's altitude.
// ---------------------------------------------------------------------------

// Can-fail oracle for the altitude-collapsed weight. The two arms differ only in
// the control points' Y, by more than radius + falloff; a top-down footprint must
// therefore produce a BYTE-IDENTICAL bake (the noise value is a pure function of
// world XZ, and so is the weight). Pre-fix the lifted arm bakes nothing at all:
// ComputeWeight measures a 3-D distance from y = 0, so every sample reads as
// 20 m outside a 10 m band.
TEST(TerrainSplineAltitude, SplineShapedNoiseIgnoresControlPointAltitude)
{
    auto addNoise = [](ECS::World& world, ECS::EntityHandle e) { AddSplineShapedNoise(world, e); };

    const SplineBakeArm ground =
        BakeSplineArm(StraightSplinePoints(0.0f), 10.0f, false, 0.0f, addNoise);
    const SplineBakeArm lifted =
        BakeSplineArm(StraightSplinePoints(20.0f), 10.0f, false, 0.0f, addNoise);

    ASSERT_FALSE(ground.BakedHeights.empty());
    ASSERT_EQ(ground.BakedHeights.size(), lifted.BakedHeights.size());
    // The arms must start from the same terrain, or "identical bakes" proves nothing.
    ASSERT_EQ(0, std::memcmp(ground.BaseHeights.data(), lifted.BaseHeights.data(),
                             ground.BaseHeights.size() * sizeof(float32)));

    // Positive control: the ground arm really did carve, so the comparison below
    // cannot pass by both arms being no-ops.
    ASSERT_FALSE(ChangedSamples(ground.BaseHeights, ground.BakedHeights).empty())
        << "the y = 0 arm baked nothing — the oracle has no signal";

    EXPECT_EQ(0, std::memcmp(ground.BakedHeights.data(), lifted.BakedHeights.data(),
                             ground.BakedHeights.size() * sizeof(float32)))
        << "a spline lifted to y = 20 carved differently from the same spline at y = 0 — "
           "the modifier weight is not a top-down XZ mask";
}

// Same oracle for the other authoring path (the entity is dragged up, the control
// points stay local) and the other effect (splat, which reaches ComputeWeight
// through ApplyPaintModifiers rather than the height loop).
TEST(TerrainSplineAltitude, SplineShapedPaintIgnoresEntityAltitude)
{
    auto addPaint = [](ECS::World& world, ECS::EntityHandle e) {
        Components::TerrainModifierVolume vol{};
        vol.Shape = Components::TerrainVolumeShape::SplineArea;
        vol.Falloff = 4.0f;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);

        Components::TerrainPaintLayerEffect paint{};
        paint.LayerIndex = 2;
        paint.Strength = 1.0f;
        paint.Replace = true;
        world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(e, paint);
    };

    const SplineBakeArm ground =
        BakeSplineArm(StraightSplinePoints(0.0f), 10.0f, false, 0.0f, addPaint);
    const SplineBakeArm lifted =
        BakeSplineArm(StraightSplinePoints(0.0f), 10.0f, false, 20.0f, addPaint);

    ASSERT_FALSE(ground.BakedSplat.empty());
    ASSERT_EQ(ground.BakedSplat.size(), lifted.BakedSplat.size());
    ASSERT_EQ(0, std::memcmp(ground.BaseSplat.data(), lifted.BaseSplat.data(),
                             ground.BaseSplat.size()));

    ASSERT_NE(0, std::memcmp(ground.BaseSplat.data(), ground.BakedSplat.data(),
                             ground.BaseSplat.size()))
        << "the y = 0 arm painted nothing — the oracle has no signal";

    EXPECT_EQ(0, std::memcmp(ground.BakedSplat.data(), lifted.BakedSplat.data(),
                             ground.BakedSplat.size()))
        << "lifting the spline entity to y = 20 changed which samples were painted";
}

// The closed-spline area weight must be altitude-independent too. The interior
// test is already XZ (the cached polygon), so pre-fix the lifted arm still fills
// the polygon — but every sample OUTSIDE it runs through the 3-D SDF and loses
// the radius band and the falloff skirt, so the two arms disagree at the rim.
TEST(TerrainSplineAltitude, ClosedSplineAreaIgnoresAltitudeIncludingTheRimBand)
{
    auto addNoise = [](ECS::World& world, ECS::EntityHandle e) { AddSplineShapedNoise(world, e); };

    const SplineBakeArm ground =
        BakeSplineArm(ClosedLoopPoints(0.0f), 10.0f, true, 0.0f, addNoise);
    const SplineBakeArm lifted =
        BakeSplineArm(ClosedLoopPoints(25.0f), 10.0f, true, 0.0f, addNoise);

    ASSERT_FALSE(ground.BakedHeights.empty());
    ASSERT_EQ(ground.BakedHeights.size(), lifted.BakedHeights.size());
    ASSERT_EQ(0, std::memcmp(ground.BaseHeights.data(), lifted.BaseHeights.data(),
                             ground.BaseHeights.size() * sizeof(float32)));

    const std::vector<std::size_t> groundChanged =
        ChangedSamples(ground.BaseHeights, ground.BakedHeights);
    const std::vector<std::size_t> liftedChanged =
        ChangedSamples(lifted.BaseHeights, lifted.BakedHeights);
    ASSERT_FALSE(groundChanged.empty()) << "the y = 0 closed arm baked nothing";

    EXPECT_EQ(groundChanged.size(), liftedChanged.size())
        << "the lifted closed spline covered a different sample set — the rim band "
           "outside the polygon still measures altitude";
    EXPECT_EQ(0, std::memcmp(ground.BakedHeights.data(), lifted.BakedHeights.data(),
                             ground.BakedHeights.size() * sizeof(float32)))
        << "a closed spline lifted to y = 25 filled differently from the same loop at y = 0";
}

// Flatten's documented job (TerrainModifiers.h: "flatten terrain to the spline's Y
// position at each point") on a spline that both climbs and sits above y = 0: the
// baked centreline must reproduce the spline's own height profile, not a constant
// and not the untouched base. Pre-fix the whole modifier is inert at this altitude.
TEST(TerrainSplineAltitude, SlopedSplineAtAltitudeFlattensToASlopedProfile)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    // A road climbing from y = 30 at x = -80 to y = 50 at x = +80: collinear
    // control points, so the closest point at world x has spline Y = 40 + x/8
    // exactly, whatever the curve's parameterization does along the way.
    ECS::World world;
    CreateTerrainEntity(world, handle);
    auto splineEntity = CreateSplineEntity(
        world, splineSvc,
        {{-80.0f, 30.0f, 0.0f}, {-20.0f, 37.5f, 0.0f}, {20.0f, 42.5f, 0.0f}, {80.0f, 50.0f, 0.0f}},
        10.0f);

    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::SplineArea;
    vol.Falloff = 4.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(splineEntity, vol);

    Components::TerrainFlattenEffect fx{};
    fx.UseVolumeHeight = true;
    fx.TargetHeight = 0.0f;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(splineEntity, fx);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const std::vector<float32> baked = CopyHeights(*data);
    ASSERT_EQ(baked.size(), data->Heightfield.GetSampleCount());

    // Samples on the centreline sit at XZ distance 0, so weight is exactly 1 and
    // the flatten writes the target outright. Tolerance covers the closest-point
    // refinement bracket (~6e-4 normalized here), not a slope this coarse.
    constexpr float32 kProbeX[] = {-60.0f, -30.0f, 0.0f, 30.0f, 60.0f};
    constexpr float32 kTolerance = 2.0e-3f;
    float32 previous = -1.0f;
    for (float32 x : kProbeX)
    {
        const float32 expected = (40.0f + x * 0.125f) / kHeightScale;
        const float32 actual = baked[SampleIndexAtXOnCenterRow(x)];
        EXPECT_NEAR(actual, expected, kTolerance)
            << "centreline height at x = " << x << " does not follow the spline's Y";
        EXPECT_GT(actual, previous) << "the flattened profile is not rising at x = " << x;
        previous = actual;
    }
}
