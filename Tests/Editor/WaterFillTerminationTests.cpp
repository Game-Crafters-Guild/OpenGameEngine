// Where the filled water region STOPS, against her own IslandVignette terrain.
//
// The scene is rebuilt here rather than loaded: the same terrain size, base
// source and modifier stack the .scene file authors, baked by the same
// TerrainModifierSystem, and the same water spline and recipe. That makes the
// numbers this file prints the numbers the editor computes, and it makes the
// oracles below regressions against HER case rather than against a synthetic
// channel.
//
// Reference: an authored island scene, IslandVignette.falsepos-repro.scene, in a content project

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include "Placement/CenterlineSampling.h"
#include "Placement/SplineFillRebuild.h"
#include "TerrainECS/PlanarHeightQuery.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierBlend.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneTlas.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::float32;
using GameEngine::int32;
using GameEngine::uint32;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;

using GameEngine::Editor::BuildWaterFillRun;
using GameEngine::TerrainECS::PlanarHeightQuery;
using GameEngine::TerrainECS::ResolvePlanarHeightQuery;
using GameEngine::Editor::WaterFillOutcome;
using GameEngine::Editor::WaterFillRun;

namespace Components = GameEngine::Components;
namespace TerrainECS = GameEngine::TerrainECS;
namespace SG = GameEngine::SplineGeometry;
namespace SplineECS = GameEngine::SplineECS;

namespace
{

// ---- The scene, transcribed -------------------------------------------------

constexpr float32 kTerrainSizeX = 200.0f;
constexpr float32 kTerrainSizeZ = 200.0f;
constexpr float32 kTerrainHeightScale = 30.0f;
constexpr float32 kSamplesPerMeter = 2.0f;

// island_river_water: the seven authored control points, and the recipe fields
// the fill reads.
struct WaterPoint
{
    float32 X, Y, Z, Radius;
};
constexpr WaterPoint kWaterPoints[] = {
    {-4.0f, 8.067272f, -2.0f, 2.0f},  {-16.0f, 6.6f, 4.0f, 2.4f},
    {-24.0f, 5.0f, -8.0f, 2.8f},      {-38.0f, 3.4f, -2.0f, 3.3f},
    {-46.0f, 1.9f, 14.0f, 3.9f},      {-56.0f, 1.15f, 28.0f, 4.6f},
    {-64.0f, 0.95f, 42.0f, 5.4f},
};

// island_river_bed: the carve spline, on its own entity, offset -1.8 in Y.
constexpr WaterPoint kBedPoints[] = {
    {-4.0f, 8.3f, -2.0f, 2.0f}, {-16.0f, 6.6f, 4.0f, 2.4f},  {-24.0f, 5.0f, -8.0f, 2.8f},
    {-38.0f, 3.4f, -2.0f, 3.3f}, {-46.0f, 1.9f, 14.0f, 3.9f}, {-56.0f, 0.9f, 28.0f, 4.6f},
    {-64.0f, 0.3f, 42.0f, 5.4f},
};
constexpr float32 kBedEntityY = -1.8f;

constexpr float32 kVerticalOffset = 1.2f;
constexpr float32 kMaxHalfWidth = 12.0f;
constexpr float32 kEdgeDrop = 0.3f;

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

Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    Components::WorldTransform xf{};
    xf.matrix[0] = xf.matrix[5] = xf.matrix[10] = xf.matrix[15] = 1.0f;
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    return xf;
}

// One wet corner's row of the field dump.
struct Probe
{
    float32 X = 0.0f;
    float32 Z = 0.0f;
    float32 Waterline = 0.0f;
    float32 Ground = 0.0f;
    float32 Depth = 0.0f;
    float32 DistanceToRun = 0.0f;
};

// Everything one repro produces: the field, the grid it was solved on, and the
// production run built from the same inputs.
struct Repro
{
    std::vector<SG::SplineStripStation> Stations;
    std::vector<float32> Heights;
    SG::SplineGroundGrid Grid;
    SG::SplineFillResult Field;
    WaterFillRun Run;
};

class WaterFillTermination : public ::testing::Test
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

    void TearDown() override { GameEngine::Scene::ReleaseSceneTlas(m_World); }

    // ---- The terrain: base noise, then the scene's modifier stack ----------
    void BakeIslandTerrain()
    {
        const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
            kTerrainSizeX, kTerrainSizeZ, kTerrainHeightScale, kSamplesPerMeter);
        const GameEngine::TerrainECS::TerrainHandle handle =
            TerrainService::Get().CreateTerrain(config);
        ASSERT_NE(handle.Generation, 0u);
        auto* data = TerrainService::Get().GetTerrainData(handle);
        ASSERT_NE(data, nullptr);

        // baseSource = 2 in the scene, which is Flat: every metre of relief in
        // this island comes from the modifier stack below.
        GameEngine::TerrainECS::FillHeightfieldBaseRegion(
            data->Heightfield, Components::TerrainBaseSource::Flat, nullptr, 0, 0,
            static_cast<int32>(data->Heightfield.GetWidth()) - 1,
            static_cast<int32>(data->Heightfield.GetHeight()) - 1);
        data->MarkFullDirty();
        TerrainService::Get().RebuildQuadtree(handle);

        Components::Terrain terrain{};
        terrain.SizeX = kTerrainSizeX;
        terrain.SizeZ = kTerrainSizeZ;
        terrain.HeightScale = kTerrainHeightScale;
        terrain.Domain = Components::TerrainDomain::Planar;
        terrain.BaseSource = Components::TerrainBaseSource::Flat;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;
        const auto terrainEntity = m_World.CreateEntity();
        m_World.AddComponentImmediate(terrainEntity, terrain);
        m_World.AddComponentImmediate(terrainEntity, Components::WorldTransform{});

        AuthorModifiers();

        GameEngine::TerrainECS::TerrainModifierSystem system;
        system.Update(m_World, 1.0f / 60.0f);
    }

    void AuthorModifiers()
    {
        // island_dome
        {
            const auto e = m_World.CreateEntity();
            Components::TerrainModifierVolume volume{};
            volume.Shape = Components::TerrainVolumeShape::Circle;
            volume.Radius = 55.0f;
            volume.Falloff = 45.0f;
            volume.Priority = 0.0f;
            m_World.AddComponentImmediate(e, volume);
            Components::TerrainHeightOffsetEffect fx{};
            fx.Offset = 8.5f;
            fx.Blend = Components::TerrainModifierBlend::Add;
            m_World.AddComponentImmediate(e, fx);
            m_World.AddComponentImmediate(e, IdentityAt(0.0f, 0.0f, 0.0f));
        }
        // island_knoll
        {
            const auto e = m_World.CreateEntity();
            Components::TerrainModifierVolume volume{};
            volume.Shape = Components::TerrainVolumeShape::Circle;
            volume.Radius = 22.0f;
            volume.Falloff = 18.0f;
            volume.Priority = 1.0f;
            m_World.AddComponentImmediate(e, volume);
            Components::TerrainHeightOffsetEffect fx{};
            fx.Offset = 5.0f;
            fx.Blend = Components::TerrainModifierBlend::Add;
            m_World.AddComponentImmediate(e, fx);
            m_World.AddComponentImmediate(e, IdentityAt(28.0f, 0.0f, -18.0f));
        }
        // island_noise
        {
            const auto e = m_World.CreateEntity();
            Components::TerrainModifierVolume volume{};
            volume.Shape = Components::TerrainVolumeShape::Circle;
            volume.Radius = 70.0f;
            volume.Falloff = 30.0f;
            volume.Priority = 2.0f;
            m_World.AddComponentImmediate(e, volume);
            Components::TerrainNoiseEffect fx{};
            fx.Blend = Components::TerrainModifierBlend::Add;
            fx.Frequency = 5.0f;
            fx.Amplitude = 2.0f;
            fx.Octaves = 4u;
            fx.Seed = 5u;
            fx.Lacunarity = 2.0f;
            fx.Persistence = 0.5f;
            fx.ErosionStrength = 1.2f;
            fx.ErosionOctaves = 3u;
            fx.ErosionFrequency = 2.5f;
            fx.ErosionDetail = 0.5f;
            fx.ErosionGullyWeight = 0.6f;
            fx.ErosionEdgeRounding = 0.25f;
            fx.ErosionFade = 0.55f;
            m_World.AddComponentImmediate(e, fx);
            m_World.AddComponentImmediate(e, IdentityAt(0.0f, 0.0f, 0.0f));
        }
        // island_river_bed: a spline volume that flattens to its own curve
        {
            const auto e = m_World.CreateEntity();
            const SplineECS::SplineHandle handle =
                SplineECS::SplineService::Get().CreateSpline(GameEngine::Spline::SplineType::CatmullRom,
                                                             false);
            auto* data = SplineECS::SplineService::Get().GetSplineData(handle);
            for (const WaterPoint& p : kBedPoints)
                data->AddPoint(Vector3(p.X, p.Y, p.Z), p.Radius);
            SplineECS::SplineService::Get().RebuildCache(handle);

            Components::SplineComponent comp{};
            comp.SplineDataIndex = handle.Index();
            comp.SplineDataGeneration = handle.Generation();
            m_World.AddComponentImmediate(e, comp);

            Components::TerrainModifierVolume volume{};
            volume.Shape = Components::TerrainVolumeShape::SplinePath;
            volume.Radius = 7.0f;
            volume.Falloff = 12.0f;
            volume.Priority = 10.0f;
            m_World.AddComponentImmediate(e, volume);

            Components::TerrainFlattenEffect fx{};
            fx.UseVolumeHeight = true;
            fx.TargetHeight = 0.0f;
            fx.Blend = Components::TerrainModifierBlend::Set;
            m_World.AddComponentImmediate(e, fx);

            m_World.AddComponentImmediate(e, IdentityAt(0.0f, kBedEntityY, 0.0f));
        }
    }

    // ---- The water stations, exactly as the controller builds them ---------
    //
    // ConformMode is None in this scene, so the authored Y stands and only
    // VerticalOffset is added; frames come from the central difference over the
    // sampled polyline and Distance is walked in true 3-D length.
    static std::vector<SG::SplineStripStation> BuildWaterStations()
    {
        const SplineECS::SplineHandle handle =
            SplineECS::SplineService::Get().CreateSpline(GameEngine::Spline::SplineType::CatmullRom,
                                                         false);
        auto* data = SplineECS::SplineService::Get().GetSplineData(handle);
        for (const WaterPoint& p : kWaterPoints)
            data->AddPoint(Vector3(p.X, p.Y, p.Z), p.Radius);
        SplineECS::SplineService::Get().RebuildCache(handle);

        std::vector<GameEngine::Spline::SplineFrame> frames;
        GameEngine::Spline::SampleUniform(
            *data, GameEngine::Editor::CenterlineSampleCount(data->TotalArcLength), frames);

        std::vector<Vector3> centre;
        centre.reserve(frames.size());
        for (const GameEngine::Spline::SplineFrame& frame : frames)
            centre.push_back(Vector3(frame.Position.x, frame.Position.y + kVerticalOffset,
                                     frame.Position.z));

        std::vector<SG::SplineStripStation> stations(centre.size());
        float32 distance = 0.0f;
        const Vector3 up(0.0f, 1.0f, 0.0f);
        for (size_t i = 0; i < centre.size(); ++i)
        {
            if (i > 0u)
            {
                const Vector3 step = centre[i] - centre[i - 1u];
                distance += std::sqrt(Vector3::Dot(step, step));
            }
            const Vector3 ahead = centre[std::min(i + 1u, centre.size() - 1u)];
            const Vector3 behind = centre[i == 0u ? 0u : i - 1u];
            Vector3 forward = ahead - behind;
            const float32 forwardLength = std::sqrt(Vector3::Dot(forward, forward));
            forward = forwardLength > 1.0e-6f ? forward * (1.0f / forwardLength)
                                              : Vector3(0.0f, 0.0f, 1.0f);
            Vector3 right = Vector3::Cross(up, Vector3(forward.x, 0.0f, forward.z));
            const float32 rightLength = std::sqrt(Vector3::Dot(right, right));
            right = rightLength > 1.0e-6f ? right * (1.0f / rightLength)
                                          : Vector3(1.0f, 0.0f, 0.0f);

            stations[i].Position = centre[i];
            stations[i].Forward = forward;
            stations[i].Right = right;
            stations[i].Up = Vector3::Cross(forward, right);
            stations[i].Distance = distance;
        }
        return stations;
    }

    static Components::SplineExtrude WaterRecipe()
    {
        Components::SplineExtrude recipe{};
        recipe.WidthMode = Components::SplineExtrudeWidthMode::FitToBanks;
        recipe.MaxHalfWidth = kMaxHalfWidth;
        recipe.EdgeDrop = kEdgeDrop;
        recipe.VerticalOffset = kVerticalOffset;
        recipe.EndTaperMetres = 6.0f;
        recipe.TilesPerMetreU = 1.0f;
        recipe.TilesPerMetreV = 1.0f;
        return recipe;
    }

    static void Identity(float32 (&matrix)[16])
    {
        std::memset(matrix, 0, sizeof(matrix));
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    }

    // Rebuilds the ROI grid the same way BuildWaterFillRun does, so the field
    // solved here IS the field the production path solves. The diagnostics of
    // the two are compared in the repro test, which is what makes that claim
    // checkable rather than asserted.
    Repro BuildRepro(const Components::SplineExtrude& recipe)
    {
        Repro repro;
        repro.Stations = BuildWaterStations();

        const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(m_World);
        const float32 spacingX = terrain.LatticeSpacingX();
        const float32 spacingZ = terrain.LatticeSpacingZ();
        EXPECT_TRUE(terrain.IsValid());
        EXPECT_GT(spacingX, 0.0f);

        float32 minX = repro.Stations.front().Position.x;
        float32 maxX = minX;
        float32 minZ = repro.Stations.front().Position.z;
        float32 maxZ = minZ;
        for (const SG::SplineStripStation& station : repro.Stations)
        {
            minX = std::min(minX, station.Position.x);
            maxX = std::max(maxX, station.Position.x);
            minZ = std::min(minZ, station.Position.z);
            maxZ = std::max(maxZ, station.Position.z);
        }
        const float32 reach = std::max(0.0f, recipe.MaxHalfWidth);
        const float32 margin = reach + std::max(spacingX, spacingZ) * 2.0f;
        const float32 originX = terrain.LatticeOriginX();
        const float32 originZ = terrain.LatticeOriginZ();
        const int32 firstX = static_cast<int32>(std::floor((minX - margin - originX) / spacingX));
        const int32 lastX = static_cast<int32>(std::ceil((maxX + margin - originX) / spacingX));
        const int32 firstZ = static_cast<int32>(std::floor((minZ - margin - originZ) / spacingZ));
        const int32 lastZ = static_cast<int32>(std::ceil((maxZ + margin - originZ) / spacingZ));

        repro.Grid.OriginX = originX + static_cast<float32>(firstX) * spacingX;
        repro.Grid.OriginZ = originZ + static_cast<float32>(firstZ) * spacingZ;
        repro.Grid.SpacingX = spacingX;
        repro.Grid.SpacingZ = spacingZ;
        repro.Grid.CountX = static_cast<uint32>(std::max(lastX - firstX + 1, 0));
        repro.Grid.CountZ = static_cast<uint32>(std::max(lastZ - firstZ + 1, 0));

        repro.Heights.assign(static_cast<size_t>(repro.Grid.CountX) * repro.Grid.CountZ, 0.0f);
        for (uint32 cz = 0; cz < repro.Grid.CountZ; ++cz)
        {
            const float32 worldZ = repro.Grid.WorldZ(cz);
            for (uint32 cx = 0; cx < repro.Grid.CountX; ++cx)
            {
                const float32 worldX = repro.Grid.WorldX(cx);
                const uint32 index = repro.Grid.Index(cx, cz);
                if (!terrain.ContainsXZ(worldX, worldZ) ||
                    !terrain.SampleHeight(worldX, worldZ, repro.Heights[index]))
                    repro.Heights[index] = 1.0e9f;
            }
        }
        repro.Grid.Heights = repro.Heights;

        SG::SplineFillParams params;
        params.MaxHalfWidth = reach;
        params.EdgeDrop = std::max(0.0f, recipe.EdgeDrop);
        params.SeaLevelFloor = recipe.SeaLevelFloor;
        repro.Field = SG::BuildSplineFillField(repro.Stations, repro.Grid, params);

        float32 matrix[16];
        Identity(matrix);
        repro.Run = BuildWaterFillRun(m_World, repro.Stations, recipe, matrix, /*single terrain*/ nullptr);
        return repro;
    }

    // The escape elevation, recomputed here from the field's Distance and the
    // grid's heights alone: the lowest ground a drop must top on a path that
    // moves monotonically AWAY from the centreline until it leaves the corridor.
    //
    // Deliberately a second implementation -- a sort-free repeated relaxation
    // rather than the production single sweep -- so that agreeing with the
    // production field is evidence rather than tautology.
    static std::vector<float32> IndependentEscapeField(const Repro& repro, float32 reach)
    {
        const size_t count = repro.Field.Corners.size();
        const float32 unreachable = std::numeric_limits<float32>::infinity();
        std::vector<float32> escape(count, unreachable);
        for (uint32 i = 0; i < count; ++i)
            if (repro.Field.Corners[i].Distance > reach)
                escape[i] = repro.Heights[i];

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (uint32 cz = 0; cz < repro.Grid.CountZ; ++cz)
                for (uint32 cx = 0; cx < repro.Grid.CountX; ++cx)
                {
                    const uint32 index = repro.Grid.Index(cx, cz);
                    if (repro.Field.Corners[index].Distance > reach)
                        continue;
                    const float32 here = repro.Field.Corners[index].Distance;
                    float32 onward = unreachable;
                    const auto consider = [&](uint32 other)
                    {
                        if (repro.Field.Corners[other].Distance > here)
                            onward = std::min(onward, escape[other]);
                    };
                    if (cx > 0u)
                        consider(index - 1u);
                    if (cx + 1u < repro.Grid.CountX)
                        consider(index + 1u);
                    if (cz > 0u)
                        consider(index - repro.Grid.CountX);
                    if (cz + 1u < repro.Grid.CountZ)
                        consider(index + repro.Grid.CountX);
                    const float32 value = std::max(repro.Heights[index], onward);
                    if (value < escape[index])
                    {
                        escape[index] = value;
                        changed = true;
                    }
                }
        }
        return escape;
    }

    // The deepest wet corners: where the sheet stands furthest above the bed.
    static std::vector<Probe> DeepestWetCorners(const Repro& repro, size_t count)
    {
        std::vector<Probe> probes;
        for (uint32 cz = 0; cz < repro.Grid.CountZ; ++cz)
            for (uint32 cx = 0; cx < repro.Grid.CountX; ++cx)
            {
                const uint32 index = repro.Grid.Index(cx, cz);
                const SG::SplineFillCorner& corner = repro.Field.Corners[index];
                if (!corner.Wet)
                    continue;
                Probe probe;
                probe.X = repro.Grid.WorldX(cx);
                probe.Z = repro.Grid.WorldZ(cz);
                probe.Waterline = corner.Waterline;
                probe.Ground = repro.Heights[index];
                probe.Depth = corner.Waterline - repro.Heights[index];
                probe.DistanceToRun = corner.Distance;
                probes.push_back(probe);
            }
        std::sort(probes.begin(), probes.end(),
                  [](const Probe& a, const Probe& b) { return a.Depth > b.Depth; });
        if (probes.size() > count)
            probes.resize(count);
        return probes;
    }

    World m_World;
    ScopedTerrainService m_TerrainScope;
    ScopedSplineService m_SplineScope;
};

} // namespace

// The repro itself. Prints the field where her frame shows the defect and
// asserts the two defect signatures are ABSENT: water never stands over its own
// bank, and the region terminates on ground rather than at the corridor.
TEST_F(WaterFillTermination, HerVignetteHoldsOnlyWaterItsGroundCanHold)
{
    BakeIslandTerrain();
    const Repro repro = BuildRepro(WaterRecipe());

    ASSERT_EQ(repro.Run.Outcome, WaterFillOutcome::Built);
    ASSERT_GT(repro.Field.Diagnostics.WetCorners, 0u);

    // The hand-built grid must be the production grid, or nothing below is
    // about her scene.
    EXPECT_EQ(repro.Field.Diagnostics.WetCorners, repro.Run.Diagnostics.WetCorners);
    EXPECT_EQ(repro.Field.Diagnostics.OverBankCorners, repro.Run.Diagnostics.OverBankCorners);
    EXPECT_EQ(repro.Field.Diagnostics.DrySeeds, repro.Run.Diagnostics.DrySeeds);

    std::printf("\n[repro] grid %ux%u at %.6f m, origin (%.3f, %.3f)\n", repro.Grid.CountX,
                repro.Grid.CountZ, repro.Grid.SpacingX, repro.Grid.OriginX, repro.Grid.OriginZ);
    std::printf("[repro] stations=%zu  first=(%.2f, %.2f, %.2f)  last=(%.2f, %.2f, %.2f)\n",
                repro.Stations.size(), repro.Stations.front().Position.x,
                repro.Stations.front().Position.y, repro.Stations.front().Position.z,
                repro.Stations.back().Position.x, repro.Stations.back().Position.y,
                repro.Stations.back().Position.z);
    const SG::SplineFillDiagnostics& d = repro.Field.Diagnostics;
    std::printf("[repro] seeds=%u dry=%u wet=%u overBank=%u unseenBank=%u maxOverBank=%.3f "
                "maxDepth=%.3f medialStep=%.3f\n",
                d.Seeds, d.DrySeeds, d.WetCorners, d.OverBankCorners, d.UnseenBankCorners,
                d.MaxOverBankMetres, d.MaxDepthMetres, d.MedialStepMetres);

    std::printf("[repro] deepest wet corners (x, z, waterline, ground, depth, distToRun):\n");
    for (const Probe& probe : DeepestWetCorners(repro, 12))
        std::printf("        %8.3f %8.3f  %7.3f %8.3f %8.3f %7.3f\n", probe.X, probe.Z,
                    probe.Waterline, probe.Ground, probe.Depth, probe.DistanceToRun);

    // The region's own extent, against the corridor it is allowed to occupy.
    float32 wetMinX = 1.0e9f, wetMaxX = -1.0e9f, wetMinZ = 1.0e9f, wetMaxZ = -1.0e9f;
    float32 wetMinW = 1.0e9f, wetMaxW = -1.0e9f, maxDistance = 0.0f;
    for (uint32 cz = 0; cz < repro.Grid.CountZ; ++cz)
        for (uint32 cx = 0; cx < repro.Grid.CountX; ++cx)
        {
            const SG::SplineFillCorner& corner = repro.Field.Corners[repro.Grid.Index(cx, cz)];
            if (!corner.Wet)
                continue;
            wetMinX = std::min(wetMinX, repro.Grid.WorldX(cx));
            wetMaxX = std::max(wetMaxX, repro.Grid.WorldX(cx));
            wetMinZ = std::min(wetMinZ, repro.Grid.WorldZ(cz));
            wetMaxZ = std::max(wetMaxZ, repro.Grid.WorldZ(cz));
            wetMinW = std::min(wetMinW, corner.Waterline);
            wetMaxW = std::max(wetMaxW, corner.Waterline);
            maxDistance = std::max(maxDistance, corner.Distance);
        }
    std::printf("[repro] wet bbox x[%.2f, %.2f] z[%.2f, %.2f]  waterline[%.3f, %.3f]  "
                "maxDistToRun=%.3f (reach %.1f)  area=%.0f m2\n",
                wetMinX, wetMaxX, wetMinZ, wetMaxZ, wetMinW, wetMaxW, maxDistance, kMaxHalfWidth,
                static_cast<float32>(d.WetCorners) * repro.Grid.SpacingX * repro.Grid.SpacingZ);

    // Per-station containment: the bed under the station, the waterline over it,
    // and the LOWEST ground on the corridor boundary within one lattice cell of
    // this station's lateral line. That last number is the rim the water would
    // have to clear to leave the corridor -- below the waterline means the
    // corridor is holding water the ground would not.
    std::printf("[repro] station containment (arc, x, z, waterline, bedUnder, "
                "minGroundAtCorridorEdge):\n");
    for (size_t i = 0; i < repro.Stations.size(); i += 16u)
    {
        const SG::SplineStripStation& station = repro.Stations[i];
        const int32 sx = static_cast<int32>(
            std::lround((station.Position.x - repro.Grid.OriginX) / repro.Grid.SpacingX));
        const int32 sz = static_cast<int32>(
            std::lround((station.Position.z - repro.Grid.OriginZ) / repro.Grid.SpacingZ));
        const float32 bed = repro.Heights[repro.Grid.Index(static_cast<uint32>(sx),
                                                           static_cast<uint32>(sz))];
        float32 rim = 1.0e9f;
        for (int32 side = -1; side <= 1; side += 2)
            for (float32 offset = kMaxHalfWidth - 0.8f; offset <= kMaxHalfWidth + 0.8f;
                 offset += 0.39f)
            {
                const float32 px = station.Position.x + station.Right.x * offset *
                                                            static_cast<float32>(side);
                const float32 pz = station.Position.z + station.Right.z * offset *
                                                            static_cast<float32>(side);
                const int32 cx = static_cast<int32>(
                    std::lround((px - repro.Grid.OriginX) / repro.Grid.SpacingX));
                const int32 cz = static_cast<int32>(
                    std::lround((pz - repro.Grid.OriginZ) / repro.Grid.SpacingZ));
                if (cx < 0 || cz < 0 || cx >= static_cast<int32>(repro.Grid.CountX) ||
                    cz >= static_cast<int32>(repro.Grid.CountZ))
                    continue;
                rim = std::min(rim, repro.Heights[repro.Grid.Index(static_cast<uint32>(cx),
                                                                   static_cast<uint32>(cz))]);
            }
        std::printf("        %7.2f %8.2f %8.2f  %7.3f %8.3f %8.3f%s\n", station.Distance,
                    station.Position.x, station.Position.z, station.Position.y, bed, rim,
                    rim < station.Position.y ? "   <-- corridor holds water the ground would not"
                                             : "");
    }

    // ---- Oracle (a): nothing floats -----------------------------------------
    //
    // Checked against an escape field recomputed HERE, by a separate sweep with
    // its own ordering, so this is a cross-check and not a restatement of the
    // implementation.
    const std::vector<float32> escape = IndependentEscapeField(repro, kMaxHalfWidth);
    uint32 floating = 0;
    float32 worstFloat = 0.0f;
    for (uint32 i = 0; i < repro.Field.Corners.size(); ++i)
    {
        const SG::SplineFillCorner& corner = repro.Field.Corners[i];
        if (!corner.Wet)
            continue;
        const float32 over = corner.Waterline - escape[i];
        if (over > 1.0e-3f)
        {
            ++floating;
            worstFloat = std::max(worstFloat, over);
        }
    }
    EXPECT_EQ(floating, 0u) << floating << " wet corners stand above the bank they would have to "
                               "cross to leave, worst by " << worstFloat << " m";

    // ---- Oracle (b): the region ends on ground, never on the corridor -------
    //
    // A wet corner 4-adjacent to a corner OUTSIDE the corridor is the region
    // being cut by MaxHalfWidth. That is only honest where the ground out there
    // is at or above the water; otherwise it is a vertical face of water over
    // open ground, which is what she photographed.
    uint32 midAir = 0;
    float32 worstWall = 0.0f;
    for (uint32 cz = 0; cz < repro.Grid.CountZ; ++cz)
        for (uint32 cx = 0; cx < repro.Grid.CountX; ++cx)
        {
            const uint32 index = repro.Grid.Index(cx, cz);
            if (!repro.Field.Corners[index].Wet)
                continue;
            const float32 waterline = repro.Field.Corners[index].Waterline;
            // A corner whose own ground is at or above its waterline is the
            // EdgeDrop halo -- the region deliberately reaches that far into the
            // bank so the rim vertex buries instead of z-fighting -- and its
            // shoreline crossing lies INLAND of it. Only a corner with real
            // water on it can present a face.
            if (repro.Heights[index] >= waterline)
                continue;
            const auto outside = [&](uint32 other)
            {
                if (repro.Field.Corners[other].Distance <= kMaxHalfWidth)
                    return;
                const float32 wall = waterline - repro.Heights[other];
                if (wall > 1.0e-3f)
                {
                    ++midAir;
                    worstWall = std::max(worstWall, wall);
                }
            };
            if (cx > 0u)
                outside(index - 1u);
            if (cx + 1u < repro.Grid.CountX)
                outside(index + 1u);
            if (cz > 0u)
                outside(index - repro.Grid.CountX);
            if (cz + 1u < repro.Grid.CountZ)
                outside(index + repro.Grid.CountX);
        }
    EXPECT_EQ(midAir, 0u) << midAir << " corners end the region at the corridor with water "
                             "standing over lower ground, worst face " << worstWall << " m";
}

// The counter-hypothesis, quantitatively. If the GROUND were what stopped this
// region, doubling the corridor would barely move it; if the CORRIDOR is what
// stopped it, the region grows with the bound and keeps growing.
// Before the fix this region grew with MaxHalfWidth -- 14,821 wet corners at 12
// m, 17,467 at 18, 20,453 at 24 -- which is the proof that the corridor and not
// the ground was deciding the water's extent. A search bound must not be a shape
// parameter: widening it may only let the fill SEE more bank, never make more
// water stand.
TEST_F(WaterFillTermination, WideningTheCorridorNoLongerGrowsTheWater)
{
    BakeIslandTerrain();

    uint32 atTwelve = 0;
    uint32 atEighteen = 0;
    for (const float32 reach : {12.0f, 18.0f, 24.0f})
    {
        Components::SplineExtrude recipe = WaterRecipe();
        recipe.MaxHalfWidth = reach;
        const Repro repro = BuildRepro(recipe);
        const SG::SplineFillDiagnostics& d = repro.Field.Diagnostics;
        std::printf("[reach] MaxHalfWidth=%.0f  wet=%u  area=%.0f m2  overBank=%u unseenBank=%u  "
                    "maxDepth=%.3f\n",
                    reach, d.WetCorners,
                    static_cast<float32>(d.WetCorners) * repro.Grid.SpacingX * repro.Grid.SpacingZ,
                    d.OverBankCorners, d.UnseenBankCorners, d.MaxDepthMetres);
        if (reach == 12.0f)
        {
            atTwelve = d.WetCorners;
            ASSERT_GT(atTwelve, 0u);
            // Her authored reach really is too small for this run: 3,328 samples
            // are refused for want of a bank inside it, which is what the
            // UnseenBankCorners warning now tells her.
            EXPECT_GT(d.UnseenBankCorners, 0u);
            continue;
        }
        if (reach == 18.0f)
        {
            // Widening reveals banks that were outside the search, so the region
            // legitimately grows -- once. What must NOT happen is unbounded
            // growth with the knob.
            EXPECT_LT(d.UnseenBankCorners, 1000u)
                << "the banks are still outside an 18 m reach; the plateau below is not a plateau";
            atEighteen = d.WetCorners;
            continue;
        }
        EXPECT_LT(d.WetCorners, atEighteen * 11u / 10u)
            << "the region is still tracking MaxHalfWidth rather than the ground";
    }
}

// Prototype of the termination rule, computed here rather than in the fill, so
// the numbers can be read BEFORE the design commits to them.
//
// spill(corner) = the lowest rim a drop of water at that corner must clear to
// leave the corridor: the minimum over all paths to an escape corner of the
// maximum ground elevation along the path. Standard priority flood inward from
// the escape set. A waterline above spill() is water that would drain away.
TEST_F(WaterFillTermination, TheSpillElevationSaysWhereTheWaterWouldActuallyStand)
{
    BakeIslandTerrain();
    const Repro repro = BuildRepro(WaterRecipe());
    ASSERT_GT(repro.Field.Diagnostics.WetCorners, 0u);

    const size_t count = repro.Field.Corners.size();
    std::vector<float32> spill(count, std::numeric_limits<float32>::infinity());
    using Entry = std::pair<float32, uint32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    for (uint32 i = 0; i < count; ++i)
    {
        if (repro.Field.Corners[i].Distance <= kMaxHalfWidth)
            continue;
        spill[i] = repro.Heights[i];
        open.emplace(spill[i], i);
    }
    std::printf("\n[spill] escape corners seeded: %zu of %zu\n", open.size(), count);

    while (!open.empty())
    {
        const Entry entry = open.top();
        open.pop();
        if (entry.first > spill[entry.second])
            continue;
        const uint32 cx = entry.second % repro.Grid.CountX;
        const uint32 cz = entry.second / repro.Grid.CountX;
        const auto relax = [&](uint32 other)
        {
            const float32 candidate = std::max(entry.first, repro.Heights[other]);
            if (candidate >= spill[other])
                return;
            spill[other] = candidate;
            open.emplace(candidate, other);
        };
        if (cx > 0u)
            relax(entry.second - 1u);
        if (cx + 1u < repro.Grid.CountX)
            relax(entry.second + 1u);
        if (cz > 0u)
            relax(entry.second - repro.Grid.CountX);
        if (cz + 1u < repro.Grid.CountZ)
            relax(entry.second + repro.Grid.CountX);
    }

    uint32 overSpill = 0;
    uint32 survives = 0;
    float32 worstExcess = 0.0f;
    float32 worstX = 0.0f, worstZ = 0.0f, worstWater = 0.0f, worstSpill = 0.0f;
    for (uint32 cz = 0; cz < repro.Grid.CountZ; ++cz)
        for (uint32 cx = 0; cx < repro.Grid.CountX; ++cx)
        {
            const uint32 index = repro.Grid.Index(cx, cz);
            const SG::SplineFillCorner& corner = repro.Field.Corners[index];
            if (!corner.Wet)
                continue;
            const float32 capped = std::min(corner.Waterline, spill[index]);
            if (capped > repro.Heights[index])
                ++survives;
            const float32 excess = corner.Waterline - spill[index];
            if (excess > 0.0f)
            {
                ++overSpill;
                if (excess > worstExcess)
                {
                    worstExcess = excess;
                    worstX = repro.Grid.WorldX(cx);
                    worstZ = repro.Grid.WorldZ(cz);
                    worstWater = corner.Waterline;
                    worstSpill = spill[index];
                }
            }
        }
    std::printf("[spill] wet=%u  standing above their own spill=%u  survive the cap=%u\n",
                repro.Field.Diagnostics.WetCorners, overSpill, survives);
    std::printf("[spill] worst excess %.3f m at (%.2f, %.2f): waterline %.3f vs spill %.3f\n",
                worstExcess, worstX, worstZ, worstWater, worstSpill);

    // Along the run, what the cap would do to the surface at each station.
    std::printf("[spill] station (arc, x, z, waterline, bed, spill, cappedDepth):\n");
    for (size_t i = 0; i < repro.Stations.size(); i += 16u)
    {
        const SG::SplineStripStation& station = repro.Stations[i];
        const int32 sx = static_cast<int32>(
            std::lround((station.Position.x - repro.Grid.OriginX) / repro.Grid.SpacingX));
        const int32 sz = static_cast<int32>(
            std::lround((station.Position.z - repro.Grid.OriginZ) / repro.Grid.SpacingZ));
        const uint32 index = repro.Grid.Index(static_cast<uint32>(sx), static_cast<uint32>(sz));
        const float32 capped = std::min(station.Position.y, spill[index]);
        std::printf("        %7.2f %8.2f %8.2f  %7.3f %8.3f %8.3f %8.3f%s\n", station.Distance,
                    station.Position.x, station.Position.z, station.Position.y,
                    repro.Heights[index], spill[index], capped - repro.Heights[index],
                    capped - repro.Heights[index] <= 0.0f ? "   <-- station goes DRY" : "");
    }

    EXPECT_GT(overSpill, 0u) << "nothing stands above its spill elevation; there is nothing to cap";
}

// Does the fill read the same ground the renderer draws? The grid claims its
// corners ARE heightfield samples; this checks that claim against the raw store
// rather than trusting the comment.
TEST_F(WaterFillTermination, TheFillsGroundIsTheTerrainsOwnSamples)
{
    BakeIslandTerrain();

    const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(m_World);
    ASSERT_TRUE(terrain.IsValid());
    ASSERT_NE(terrain.Single, nullptr);

    const GameEngine::Terrain::HeightfieldData& field = terrain.Single->Heightfield;
    const uint32 width = field.GetWidth();
    const uint32 height = field.GetHeight();
    const float32 spacingX = terrain.LatticeSpacingX();
    const float32 spacingZ = terrain.LatticeSpacingZ();
    std::printf("\n[ground] heightfield %ux%u, spacing %.6f m, origin (%.3f, %.3f), "
                "originY=%.3f, heightScale=%.3f\n",
                width, height, spacingX, terrain.LatticeOriginX(), terrain.LatticeOriginZ(),
                terrain.OriginY, terrain.HeightScale);

    // Every lattice corner the fill would read, against the array read the
    // design says it is equivalent to.
    float32 worstDelta = 0.0f;
    float32 worstX = 0.0f, worstZ = 0.0f;
    for (uint32 zi = 0; zi < height; ++zi)
        for (uint32 xi = 0; xi < width; ++xi)
        {
            const float32 worldX = terrain.LatticeOriginX() + static_cast<float32>(xi) * spacingX;
            const float32 worldZ = terrain.LatticeOriginZ() + static_cast<float32>(zi) * spacingZ;
            float32 sampled;
            if (!terrain.SampleHeight(worldX, worldZ, sampled))
                continue;
            const float32 direct =
                terrain.OriginY + field.GetSample(xi, zi) * terrain.HeightScale;
            const float32 delta = std::abs(sampled - direct);
            if (delta > worstDelta)
            {
                worstDelta = delta;
                worstX = worldX;
                worstZ = worldZ;
            }
        }
    std::printf("[ground] worst |bilinear - array read| over %u corners: %.9f m at (%.3f, %.3f)\n",
                width * height, worstDelta, worstX, worstZ);

    // A millimetre is far below the shoreline's own sub-cell resolution; a
    // failure here would mean the fill is comparing its waterline against
    // interpolated ground the author never authored.
    EXPECT_LT(worstDelta, 1.0e-3f)
        << "the fill's ground sampling is not the terrain's own lattice samples";
}
