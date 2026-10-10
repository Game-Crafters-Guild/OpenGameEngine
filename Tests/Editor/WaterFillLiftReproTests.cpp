// The hole a lifted river opens: which constraint dried those cells?
//
// On ComposedIsland, raising Longwater Water's entity transform by 2.81 m opens
// an angular dry region MID-river, ringed by water on every side. The recipe
// does not conform, so the transform lift lands on the waterline one-for-one
// (SplineExtrudeController: station y = worldMatrix * point + VerticalOffset),
// which makes the defect reproducible from the scene file alone.
//
// A dry cell the water surrounds can be dried by exactly one of three terms,
// and they take three different fixes:
//   containment  Escape - Waterline <= 0   the bank it would drain over is low
//   corridor     MaxHalfWidth - Distance <= 0   it is further out than the reach
//   ground       phi + EdgeDrop <= 0       it is a real islet and must stay dry
// This file names which, rather than assuming.
//
// Scene comes from GE_WATER_LIFT_REPRO_SCENE (an absolute .scene path); without
// it every test skips, because the scene lives in a separate content repo and
// copying it here would make the regression a copy of the content.
//
//   GE_WATER_LIFT_REPRO_SCENE=<project>/Assets/Scenes/ComposedIsland.scene
//   WaterFillInvestigationTests.exe --gtest_filter=WaterFillLiftRepro.*

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Placement/CenterlineSampling.h"
#include "Placement/SplineFillRebuild.h"
#include "TerrainECS/PlanarHeightQuery.h"

#include "Components/Name.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Mathematics/Matrix4x4.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneTlas.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "SplineECS/Systems/SplineExtractionSystem.h"
#include "SplineGeometry/SplineFillField.h"
#include "Terrain/Heightfield.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::float32;
using GameEngine::int32;
using GameEngine::uint32;
using GameEngine::uint64;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;

using GameEngine::Editor::CenterlineSampleCount;
using GameEngine::TerrainECS::PlanarHeightQuery;
using GameEngine::TerrainECS::ResolvePlanarHeightQuery;
using GameEngine::Editor::WorldCenterlineLength;

namespace Components = GameEngine::Components;
namespace TerrainECS = GameEngine::TerrainECS;
namespace SG = GameEngine::SplineGeometry;
namespace SplineECS = GameEngine::SplineECS;

namespace
{

// Her repro, to the float: the y she typed into Longwater Water's transform.
constexpr float32 kLiftMetres = 2.8104186058044434f;
constexpr const char* kRiverName = "Longwater Water";

const char* EnvOrNull(const char* name)
{
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

Vector3 NormalizedOr(const Vector3& v, const Vector3& fallback)
{
    const float32 length = std::sqrt(Vector3::Dot(v, v));
    return length > 1.0e-6f ? v * (1.0f / length) : fallback;
}

// The world stations SplineExtrudeController builds for a FitToBanks recipe on
// the ConformMode::None path — which is the path this river takes. A conforming
// recipe drapes through the picking service, which a scene loaded without a
// renderer has not got, so this is refused rather than approximated.
std::vector<SG::SplineStripStation> BuildWorldStations(const GameEngine::Spline::SplineData& data,
                                                       const Components::SplineExtrude& recipe,
                                                       const Matrix4x4& worldMatrix)
{
    const Vector3 worldOrigin = worldMatrix.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    const float32 worldArc = WorldCenterlineLength(data.TotalArcLength, worldMatrix);

    std::vector<GameEngine::Spline::SplineFrame> frames;
    GameEngine::Spline::SampleUniform(data, CenterlineSampleCount(worldArc), frames);

    const Vector3 worldUp(0.0f, 1.0f, 0.0f);
    std::vector<Vector3> centre;
    centre.reserve(frames.size());
    for (const GameEngine::Spline::SplineFrame& frame : frames)
    {
        Vector3 pos = worldMatrix.TransformPoint(frame.Position);
        if (recipe.LateralOffset != 0.0f)
        {
            const Vector3 tangent = NormalizedOr(
                worldMatrix.TransformPoint(frame.Forward) - worldOrigin, Vector3(0.0f, 0.0f, 1.0f));
            const Vector3 travel =
                NormalizedOr(Vector3(tangent.x, 0.0f, tangent.z), Vector3(0.0f, 0.0f, 1.0f));
            const Vector3 right =
                NormalizedOr(Vector3::Cross(worldUp, travel), Vector3(1.0f, 0.0f, 0.0f));
            pos = pos + right * recipe.LateralOffset;
        }
        pos.y += recipe.VerticalOffset;
        centre.push_back(pos);
    }

    std::vector<SG::SplineStripStation> stations(centre.size());
    float32 distance = 0.0f;
    for (size_t i = 0; i < centre.size(); ++i)
    {
        if (i > 0u)
        {
            const Vector3 step = centre[i] - centre[i - 1u];
            distance += std::sqrt(Vector3::Dot(step, step));
        }
        const Vector3 ahead = centre[std::min(i + 1u, centre.size() - 1u)];
        const Vector3 behind = centre[i == 0u ? 0u : i - 1u];
        const Vector3 forward = NormalizedOr(ahead - behind, Vector3(0.0f, 0.0f, 1.0f));
        const Vector3 right = NormalizedOr(
            Vector3::Cross(worldUp, Vector3(forward.x, 0.0f, forward.z)), Vector3(1.0f, 0.0f, 0.0f));
        stations[i].Position = centre[i];
        stations[i].Forward = forward;
        stations[i].Right = right;
        stations[i].Up = Vector3::Cross(forward, right);
        stations[i].Distance = distance;
    }
    return stations;
}

// The ground grid BuildWaterFillRun builds for a run, rebuilt here so the field
// itself — every corner's Escape, Distance and Waterline — can be read back.
// Mirrors that function's region-of-interest construction exactly.
struct ReproField
{
    std::vector<float32> Heights;
    SG::SplineGroundGrid Grid;
    SG::SplineFillResult Field;
};

bool BuildReproField(World& world, const std::vector<SG::SplineStripStation>& stations,
                     const Components::SplineExtrude& recipe, ReproField& out)
{
    const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(world);
    const float32 spacingX = terrain.LatticeSpacingX();
    const float32 spacingZ = terrain.LatticeSpacingZ();
    if (!terrain.IsValid() || !(spacingX > 0.0f) || !(spacingZ > 0.0f))
        return false;

    const float32 reach = std::max(0.0f, recipe.MaxHalfWidth);
    float32 minX = stations.front().Position.x;
    float32 maxX = minX;
    float32 minZ = stations.front().Position.z;
    float32 maxZ = minZ;
    for (const SG::SplineStripStation& s : stations)
    {
        minX = std::min(minX, s.Position.x);
        maxX = std::max(maxX, s.Position.x);
        minZ = std::min(minZ, s.Position.z);
        maxZ = std::max(maxZ, s.Position.z);
    }
    const float32 margin = reach + std::max(spacingX, spacingZ) * 2.0f;
    const float32 originX = terrain.LatticeOriginX();
    const float32 originZ = terrain.LatticeOriginZ();
    const int32 firstX = static_cast<int32>(std::floor((minX - margin - originX) / spacingX));
    const int32 lastX = static_cast<int32>(std::ceil((maxX + margin - originX) / spacingX));
    const int32 firstZ = static_cast<int32>(std::floor((minZ - margin - originZ) / spacingZ));
    const int32 lastZ = static_cast<int32>(std::ceil((maxZ + margin - originZ) / spacingZ));

    out.Grid.OriginX = originX + static_cast<float32>(firstX) * spacingX;
    out.Grid.OriginZ = originZ + static_cast<float32>(firstZ) * spacingZ;
    out.Grid.SpacingX = spacingX;
    out.Grid.SpacingZ = spacingZ;
    out.Grid.CountX = static_cast<uint32>(std::max(lastX - firstX + 1, 0));
    out.Grid.CountZ = static_cast<uint32>(std::max(lastZ - firstZ + 1, 0));
    if (out.Grid.CountX < 2u || out.Grid.CountZ < 2u)
        return false;

    out.Heights.assign(static_cast<size_t>(out.Grid.CountX) * out.Grid.CountZ, 0.0f);
    for (uint32 cz = 0; cz < out.Grid.CountZ; ++cz)
    {
        const float32 worldZ = out.Grid.WorldZ(cz);
        for (uint32 cx = 0; cx < out.Grid.CountX; ++cx)
        {
            const float32 worldX = out.Grid.WorldX(cx);
            const uint32 index = out.Grid.Index(cx, cz);
            if (!terrain.ContainsXZ(worldX, worldZ) ||
                !terrain.SampleHeight(worldX, worldZ, out.Heights[index]))
                out.Heights[index] = 1.0e9f;
        }
    }
    out.Grid.Heights = out.Heights;

    SG::SplineFillParams params;
    params.MaxHalfWidth = reach;
    params.EdgeDrop = std::max(0.0f, recipe.EdgeDrop);
    params.SeaLevelFloor = recipe.SeaLevelFloor;
    out.Field = SG::BuildSplineFillField(stations, out.Grid, params);
    return true;
}

// Dry corners the wet region encloses on every side, split by the term that
// dried each one. Reachability is computed over dry corners from the grid
// boundary, exactly as the fill's own seal computes it.
struct PocketCensus
{
    uint32 Enclosed = 0;
    uint32 ByContainment = 0;
    uint32 ByCorridor = 0;
    uint32 ByGround = 0;
    Vector3 FirstEnclosed{};
};

PocketCensus CensusEnclosedDry(const SG::SplineFillResult& field, const SG::SplineGroundGrid& grid,
                               float32 edgeDrop, float32 reach)
{
    std::vector<GameEngine::uint8> reachable(field.Corners.size(), 0u);
    std::vector<uint32> stack;
    const auto visit = [&](uint32 index)
    {
        if (reachable[index] || field.Corners[index].Wet)
            return;
        reachable[index] = 1u;
        stack.push_back(index);
    };
    for (uint32 cx = 0; cx < grid.CountX; ++cx)
    {
        visit(grid.Index(cx, 0u));
        visit(grid.Index(cx, grid.CountZ - 1u));
    }
    for (uint32 cz = 0; cz < grid.CountZ; ++cz)
    {
        visit(grid.Index(0u, cz));
        visit(grid.Index(grid.CountX - 1u, cz));
    }
    while (!stack.empty())
    {
        const uint32 index = stack.back();
        stack.pop_back();
        const uint32 cx = index % grid.CountX;
        const uint32 cz = index / grid.CountX;
        if (cx > 0u)
            visit(index - 1u);
        if (cx + 1u < grid.CountX)
            visit(index + 1u);
        if (cz > 0u)
            visit(index - grid.CountX);
        if (cz + 1u < grid.CountZ)
            visit(index + grid.CountX);
    }

    PocketCensus census;
    for (uint32 index = 0; index < field.Corners.size(); ++index)
    {
        const SG::SplineFillCorner& corner = field.Corners[index];
        if (corner.Wet || reachable[index])
            continue;
        if (census.Enclosed == 0u)
            census.FirstEnclosed = Vector3(grid.WorldX(index % grid.CountX), corner.Waterline,
                                           grid.WorldZ(index / grid.CountX));
        ++census.Enclosed;
        // Order matters only for reporting: a corner can fail more than one.
        // Ground first, because a real islet is not a defect at all.
        if (!(corner.Waterline - grid.Heights[index] + edgeDrop > 0.0f))
            ++census.ByGround;
        else if (!(reach - corner.Distance > 0.0f))
            ++census.ByCorridor;
        else
            ++census.ByContainment;
    }
    return census;
}

class WaterFillLiftRepro : public ::testing::Test
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
        // The Spline schema resolves its points into the service AT LOAD, so an
        // uninitialised one fails the scene parse rather than degrading.
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
    }

    void TearDown() override
    {
        GameEngine::Scene::ReleaseSceneTlas(m_World);
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
    }

    // Load the scene, provision and bake its terrain, and lift the river by the
    // metres her repro used. Returns false when the scene is unavailable.
    bool LoadBakeAndLift(float32 liftMetres)
    {
        const char* scenePath = EnvOrNull("GE_WATER_LIFT_REPRO_SCENE");
        if (scenePath == nullptr)
            return false;
        const std::filesystem::path path(scenePath);
        EXPECT_TRUE(std::filesystem::exists(path)) << "no such scene: " << scenePath;
        if (!std::filesystem::exists(path))
            return false;

        GameEngine::Scene::LoadOptions options;
        options.mode = GameEngine::Scene::LoadMode::Replace;
        if (!GameEngine::Scene::LoadSceneFromFile(m_World, path, options))
        {
            const auto& err = GameEngine::Scene::GetLastSceneIOError();
            ADD_FAILURE() << "scene load failed: " << scenePath << " line " << err.line << ": "
                          << err.message;
            return false;
        }
        m_World.ProcessCommands();

        GameEngine::SplineECS::SplineExtractionSystem splines;
        splines.Update(m_World, 0.0f);

        GameEngine::Engine::Renderer::TransformHierarchySystem hierarchy;
        hierarchy.Update(m_World, 0.0f);

        if (!ProvisionTerrain())
            return false;
        hierarchy.Update(m_World, 0.0f);

        GameEngine::TerrainECS::TerrainModifierSystem modifiers;
        modifiers.Update(m_World, 1.0f / 60.0f);

        // Her edit: the river entity's own transform, which is what carries the
        // waterline on a recipe that does not conform.
        m_River = FindByName(kRiverName);
        EXPECT_TRUE(m_River.IsValid()) << "no entity named " << kRiverName;
        if (!m_River.IsValid())
            return false;
        if (liftMetres != 0.0f)
        {
            auto* transform = m_World.GetComponentForWrite<Components::Transform>(m_River);
            EXPECT_NE(transform, nullptr);
            if (transform == nullptr)
                return false;
            // Transform is a column-major matrix; column 3 is the translation.
            transform->matrix[13] += liftMetres;
        }
        hierarchy.Update(m_World, 0.0f);
        return true;
    }

    bool ProvisionTerrain()
    {
        std::vector<EntityHandle> terrains;
        m_World.Query<GameEngine::ECS::Read<Components::Terrain>>().Each(
            [&](EntityHandle e, const Components::Terrain&) { terrains.push_back(e); });
        EXPECT_EQ(terrains.size(), 1u);
        if (terrains.size() != 1u)
            return false;

        auto* terrain = m_World.GetComponentForWrite<Components::Terrain>(terrains.front());
        if (terrain == nullptr || terrain->Domain != Components::TerrainDomain::Planar)
            return false;
        EXPECT_NE(terrain->BaseSource, Components::TerrainBaseSource::HeightmapAsset)
            << "cannot provision a heightmap base without the asset pipeline";

        const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
            terrain->SizeX, terrain->SizeZ, terrain->HeightScale, terrain->SamplesPerMeter);
        const GameEngine::TerrainECS::TerrainHandle handle =
            TerrainService::Get().CreateTerrain(config);
        if (handle.Generation == 0u)
            return false;
        auto* data = TerrainService::Get().GetTerrainData(handle);
        if (data == nullptr)
            return false;

        GameEngine::TerrainECS::FillHeightfieldBaseRegion(
            data->Heightfield, terrain->BaseSource, nullptr, 0, 0,
            static_cast<int32>(data->Heightfield.GetWidth()) - 1,
            static_cast<int32>(data->Heightfield.GetHeight()) - 1);
        data->MarkFullDirty();
        TerrainService::Get().RebuildQuadtree(handle);
        terrain->TerrainDataHandle = handle.Index;
        terrain->TerrainDataGeneration = handle.Generation;
        return true;
    }

    EntityHandle FindByName(const char* wanted)
    {
        EntityHandle found{};
        m_World.Query<GameEngine::ECS::Read<Components::Name>>().Each(
            [&](EntityHandle e, const Components::Name& name)
            {
                // Name::value is a fixed char buffer: comparing it to a
                // pointer compiles and never matches.
                if (!found.IsValid() && std::strncmp(name.value, wanted, sizeof(name.value)) == 0)
                    found = e;
            });
        return found;
    }

    // The river's field, at whatever lift LoadBakeAndLift applied.
    bool RiverField(ReproField& out)
    {
        const auto* recipe = m_World.GetComponent<Components::SplineExtrude>(m_River);
        const auto* comp = m_World.GetComponent<Components::SplineComponent>(m_River);
        EXPECT_NE(recipe, nullptr);
        EXPECT_NE(comp, nullptr);
        if (recipe == nullptr || comp == nullptr)
            return false;
        EXPECT_EQ(recipe->ConformMode, Components::SplinePlacementConform::None)
            << "this repro only reproduces the non-conforming path";

        auto* splines = SplineECS::SplineService::TryGet();
        if (splines == nullptr)
            return false;
        const auto* data = splines->GetSplineData(
            SplineECS::SplineHandle(comp->SplineDataIndex, comp->SplineDataGeneration));
        EXPECT_NE(data, nullptr);
        if (data == nullptr || !data->IsValid())
            return false;

        Matrix4x4 worldMatrix;
        std::memset(worldMatrix.Data(), 0, sizeof(float32) * 16);
        worldMatrix.Data()[0] = worldMatrix.Data()[5] = worldMatrix.Data()[10] =
            worldMatrix.Data()[15] = 1.0f;
        if (const auto* wt = m_World.GetComponent<Components::WorldTransform>(m_River))
            worldMatrix = Matrix4x4::FromColumnMajor(wt->matrix);

        m_Recipe = *recipe;
        m_Stations = BuildWorldStations(*data, *recipe, worldMatrix);
        if (!BuildReproField(m_World, m_Stations, *recipe, out))
            return false;

        // The mirrored ROI above can drift from the production construction
        // silently; the production run is the tripwire. If these disagree, the
        // census below describes a field the editor does not build.
        const GameEngine::Editor::WaterFillRun run = GameEngine::Editor::BuildWaterFillRun(
            m_World, m_Stations, *recipe, worldMatrix.Data(), /*single terrain*/ nullptr);
        EXPECT_EQ(run.Outcome, GameEngine::Editor::WaterFillOutcome::Built);
        EXPECT_EQ(out.Field.Diagnostics.WetCorners, run.Diagnostics.WetCorners)
            << "mirrored field diverged from BuildWaterFillRun";
        EXPECT_EQ(out.Field.Diagnostics.OverBankCorners, run.Diagnostics.OverBankCorners)
            << "mirrored field diverged from BuildWaterFillRun";
        return true;
    }

    World m_World;
    EntityHandle m_River{};
    Components::SplineExtrude m_Recipe{};
    std::vector<SG::SplineStripStation> m_Stations;
};

} // namespace

// The repro itself, and the answer to "which term". Reports the census whether
// it passes or fails, because the number is the finding.
TEST_F(WaterFillLiftRepro, TheLiftedRiverEnclosesNoDryGround)
{
    if (!LoadBakeAndLift(kLiftMetres))
        GTEST_SKIP() << "repro scene unavailable (see failures above if any)";
    ReproField repro;
    ASSERT_TRUE(RiverField(repro));

    const PocketCensus census = CensusEnclosedDry(repro.Field, repro.Grid, m_Recipe.EdgeDrop,
                                                  m_Recipe.MaxHalfWidth);
    std::printf("[lift %.4f] grid %ux%u  stations=%zu  wet=%u  overBank=%u  sealed=%u\n",
                kLiftMetres, repro.Grid.CountX, repro.Grid.CountZ, m_Stations.size(),
                repro.Field.Diagnostics.WetCorners, repro.Field.Diagnostics.OverBankCorners,
                repro.Field.Diagnostics.SealedPocketCorners);
    std::printf("[lift] enclosed dry=%u  byContainment=%u  byCorridor=%u  byGround=%u "
                " first=(%.2f, %.2f)\n",
                census.Enclosed, census.ByContainment, census.ByCorridor, census.ByGround,
                census.FirstEnclosed.x, census.FirstEnclosed.z);

    EXPECT_EQ(census.ByContainment, 0u)
        << "water enclosing a cell it refused an escape to: the escape runs through the water";
    EXPECT_EQ(census.ByCorridor, 0u)
        << "water ringing a cell dried only by MaxHalfWidth: a dry ring inside a pool";
}

// The unlifted scene is the control: the same measurement at the authored
// height, so a non-zero census above is attributable to the lift.
TEST_F(WaterFillLiftRepro, TheAuthoredHeightIsTheControl)
{
    if (!LoadBakeAndLift(0.0f))
        GTEST_SKIP() << "repro scene unavailable (see failures above if any)";
    ReproField repro;
    ASSERT_TRUE(RiverField(repro));

    const PocketCensus census = CensusEnclosedDry(repro.Field, repro.Grid, m_Recipe.EdgeDrop,
                                                  m_Recipe.MaxHalfWidth);
    std::printf("[authored] wet=%u  overBank=%u  sealed=%u  enclosed dry=%u  byContainment=%u "
                " byCorridor=%u  byGround=%u\n",
                repro.Field.Diagnostics.WetCorners, repro.Field.Diagnostics.OverBankCorners,
                repro.Field.Diagnostics.SealedPocketCorners, census.Enclosed,
                census.ByContainment, census.ByCorridor, census.ByGround);

    EXPECT_EQ(census.ByContainment, 0u);
    EXPECT_EQ(census.ByCorridor, 0u);
}
