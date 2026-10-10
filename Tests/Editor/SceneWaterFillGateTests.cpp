// Hold an AUTHORED SCENE FILE to the water-fit termination bar.
//
// WaterFillTerminationTests transcribes one scene into C++ and pins the fill's
// behaviour against it. This file is the other half: it LOADS a .scene, bakes
// its terrain through the real TerrainModifierSystem, and runs the production
// BuildWaterFillRun over every FitToBanks recipe the file contains. What it
// gates is therefore the AUTHORING, not the algorithm -- a scene whose river
// stands over its own bank fails here, and so does a scene whose key spelling
// silently dropped a component on load (#994), because a dropped component is a
// missing river rather than a parse error.
//
// Scene under test comes from GE_SCENE_WATER_GATE (an absolute .scene path).
// Without it every test skips: the scene this was written for lives in a
// separate content repo and staging it here would make the gate a copy of the
// content rather than a check on it.
//
//   GE_SCENE_WATER_GATE=<project>/Assets/Scenes/ComposedIsland.scene
//   WaterFillInvestigationTests.exe --gtest_filter=SceneWaterFillGate.*
//
// Optional knobs, so the same binary can gate a different bar:
//   GE_SCENE_WATER_GATE_MAX_OVERBANK      (default 0)
//   GE_SCENE_WATER_GATE_MAX_UNSEEN        (default 100)
//   GE_SCENE_WATER_GATE_SEA_Y             (default 3.0)  what counts as landmass
//   GE_SCENE_WATER_GATE_MAX_MEDIAN_SLOPE  (default 30)   degrees
//   GE_SCENE_WATER_GATE_MAX_P95_SLOPE     (default 55)   degrees

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
#include "Terrain/Heightfield.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::float32;
using GameEngine::int32;
using GameEngine::uint32;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;

using GameEngine::Editor::BuildWaterFillRun;
using GameEngine::Editor::CenterlineSampleCount;
using GameEngine::TerrainECS::PlanarHeightQuery;
using GameEngine::TerrainECS::ResolvePlanarHeightQuery;
using GameEngine::Editor::WaterFillOutcome;
using GameEngine::Editor::WaterFillRun;
using GameEngine::Editor::WorldCenterlineLength;

namespace Components = GameEngine::Components;
namespace TerrainECS = GameEngine::TerrainECS;
namespace SG = GameEngine::SplineGeometry;
namespace SplineECS = GameEngine::SplineECS;

namespace
{

const char* EnvOrNull(const char* name)
{
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

uint32 EnvUint(const char* name, uint32 fallback)
{
    const char* value = EnvOrNull(name);
    return value != nullptr ? static_cast<uint32>(std::strtoul(value, nullptr, 10)) : fallback;
}

float32 EnvFloat(const char* name, float32 fallback)
{
    const char* value = EnvOrNull(name);
    return value != nullptr ? std::strtof(value, nullptr) : fallback;
}

// Percentile of an ALREADY SORTED sample, by nearest rank.
float32 Percentile(const std::vector<float32>& sorted, float32 q)
{
    if (sorted.empty())
        return 0.0f;
    const size_t last = sorted.size() - 1u;
    return sorted[static_cast<size_t>(std::clamp(q, 0.0f, 1.0f) * static_cast<float32>(last))];
}

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

// One FitToBanks run found in the file, with the stations the controller would
// have handed the fill.
struct FittedRun
{
    EntityHandle Entity{};
    std::string Name;
    Components::SplineExtrude Recipe{};
    std::vector<SG::SplineStripStation> Stations;
    const GameEngine::Spline::SplineData* Data = nullptr;
    Matrix4x4 World;
};

std::string EntityLabel(World& world, EntityHandle e)
{
    if (const auto* name = world.GetComponent<Components::Name>(e))
        return std::string(name->View());
    return "entity " + std::to_string(e.id);
}

Vector3 NormalizedOr(const Vector3& v, const Vector3& fallback)
{
    const float32 length = std::sqrt(Vector3::Dot(v, v));
    return length > 1.0e-6f ? v * (1.0f / length) : fallback;
}

// The world-space stations SplineExtrudeController builds for a FitToBanks
// recipe, for the ConformMode::None path only.
//
// Deliberately NOT a copy of the whole controller: conforming recipes drape
// against scene geometry through the picking service, which a scene loaded
// without a renderer does not have. A FitToBanks recipe that conforms is
// therefore refused loudly below rather than measured against a silently
// different centreline.
std::vector<SG::SplineStripStation> BuildWorldStations(const GameEngine::Spline::SplineData& data,
                                                       const Components::SplineExtrude& recipe,
                                                       const Matrix4x4& worldMatrix)
{
    const Vector3 worldOrigin = worldMatrix.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    const float32 worldArcLength = WorldCenterlineLength(data.TotalArcLength, worldMatrix);

    std::vector<GameEngine::Spline::SplineFrame> frames;
    GameEngine::Spline::SampleUniform(data, CenterlineSampleCount(worldArcLength), frames);

    const Vector3 worldUp(0.0f, 1.0f, 0.0f);
    std::vector<Vector3> centre;
    centre.reserve(frames.size());
    for (const GameEngine::Spline::SplineFrame& frame : frames)
    {
        Vector3 pos = worldMatrix.TransformPoint(frame.Position);
        if (recipe.LateralOffset != 0.0f)
        {
            const Vector3 tangent =
                NormalizedOr(worldMatrix.TransformPoint(frame.Forward) - worldOrigin,
                             Vector3(0.0f, 0.0f, 1.0f));
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
        const Vector3 forward =
            NormalizedOr(ahead - behind, Vector3(0.0f, 0.0f, 1.0f));
        const Vector3 right = NormalizedOr(Vector3::Cross(worldUp, Vector3(forward.x, 0.0f, forward.z)),
                                           Vector3(1.0f, 0.0f, 0.0f));
        stations[i].Position = centre[i];
        stations[i].Forward = forward;
        stations[i].Right = right;
        stations[i].Up = Vector3::Cross(forward, right);
        stations[i].Distance = distance;
    }
    return stations;
}

// Rebuild the run's field on the run's own ROI, so the refused corners can be
// located. The grid construction mirrors BuildWaterFillRun exactly; the caller
// reconciles the resulting counters against the production run's, which is what
// makes "the same field" a checked claim rather than an asserted one.
struct RefusalField
{
    SG::SplineGroundGrid Grid;
    std::vector<float32> Heights;
    SG::SplineFillResult Field;
    bool Built = false;
};

RefusalField SolveRefusalField(const std::vector<SG::SplineStripStation>& stations,
                               const Components::SplineExtrude& recipe,
                               const TerrainECS::PlanarHeightQuery& terrain)
{
    RefusalField out;
    const float32 spacingX = terrain.LatticeSpacingX();
    const float32 spacingZ = terrain.LatticeSpacingZ();
    if (spacingX <= 0.0f || spacingZ <= 0.0f || stations.empty())
        return out;

    float32 minX = stations.front().Position.x, maxX = minX;
    float32 minZ = stations.front().Position.z, maxZ = minZ;
    for (const SG::SplineStripStation& s : stations)
    {
        minX = std::min(minX, s.Position.x);
        maxX = std::max(maxX, s.Position.x);
        minZ = std::min(minZ, s.Position.z);
        maxZ = std::max(maxZ, s.Position.z);
    }
    const float32 reach = std::max(0.0f, recipe.MaxHalfWidth);
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
        return out;

    out.Heights.assign(static_cast<size_t>(out.Grid.CountX) * out.Grid.CountZ, 0.0f);
    for (uint32 cz = 0; cz < out.Grid.CountZ; ++cz)
        for (uint32 cx = 0; cx < out.Grid.CountX; ++cx)
        {
            const float32 wx = out.Grid.WorldX(cx);
            const float32 wz = out.Grid.WorldZ(cz);
            const uint32 index = out.Grid.Index(cx, cz);
            if (!terrain.ContainsXZ(wx, wz) || !terrain.SampleHeight(wx, wz, out.Heights[index]))
                out.Heights[index] = 1.0e9f;
        }
    out.Grid.Heights = out.Heights;

    SG::SplineFillParams params;
    params.MaxHalfWidth = reach;
    params.EdgeDrop = std::max(0.0f, recipe.EdgeDrop);
    params.SeaLevelFloor = recipe.SeaLevelFloor;
    out.Field = SG::BuildSplineFillField(stations, out.Grid, params);
    out.Built = true;
    return out;
}

// The waterline floor the shipped fill actually applies. BuildSplineFillField
// lifts the authored level by kSeaLevelFloorLiftMetres so a mouth that merges
// with a sea sits ON it rather than IN its surface (SplineFillField.cpp, the
// `seaFloor` it derives once and clamps every waterline to). A recount reading
// Recipe.SeaLevelFloor raw would judge this field against a floor 2 cm below the
// one that built it. Safe when the floor is disabled: at kNoSeaLevelFloor's
// magnitude the addition is below one ULP.
float32 ShippedSeaFloor(const Components::SplineExtrude& recipe)
{
    return recipe.SeaLevelFloor + SG::kSeaLevelFloorLiftMetres;
}

// The level holding water up at a corner -- the rim it could escape over, or the
// sea when the sea is the higher of the two. Mirrors `held` in
// BuildSplineFillField.
float32 HeldAt(const SG::SplineFillCorner& corner, float32 seaFloor)
{
    return std::max(corner.Escape, seaFloor);
}

// Did the containment term refuse this corner? The guard below is the one in
// BuildSplineFillField's "what the containment term finally refused" loop,
// transcribed term for term -- including its skips, which are load-bearing: that
// loop runs AFTER the seal and only over corners that are still DRY, so a pocket
// the region enclosed and took back is not reported as a surface that stopped at
// a bank it did not stop at.
bool RefusedByContainment(const SG::SplineFillCorner& corner, float32 groundHeight,
                          float32 reach, float32 tuck, float32 seaFloor)
{
    if (corner.Wet != 0u || corner.Station == SG::kNoStation)
        return false;
    if (!(corner.Waterline - groundHeight + tuck > 0.0f) ||
        !(reach - corner.Distance > 0.0f) || HeldAt(corner, seaFloor) - corner.Waterline > 0.0f)
        return false;
    return true;
}

// Locate the refused corners: how far along the run they sit, how far out from
// the centreline, and the worst one's world position. Returns the INTERIOR
// count -- the refusals an author can act on.
//
// Past the first and last stations, distance-to-the-run grows ALONG the channel
// rather than across it, so the outward escape sweep looks up- and downstream
// for a rim. Downstream of a river's last station that rim is the sea, and
// there is no authoring change that puts one there: the refusal is the run
// ENDING, not the water misbehaving. Those corners live within MaxHalfWidth of
// an end, and are reported separately rather than counted against the bar.
struct RefusalTally
{
    uint32 Interior = 0;
    uint32 InteriorUnseen = 0;
};

RefusalTally ReportRefusals(const FittedRun& run, const TerrainECS::PlanarHeightQuery& terrain,
                            const SG::SplineFillDiagnostics& production)
{
    const RefusalField solved = SolveRefusalField(run.Stations, run.Recipe, terrain);
    if (!solved.Built)
    {
        std::printf("       [refusals] no field\n");
        return {};
    }
    // If these disagree, the attribution below is about a different field and
    // must not be read as a description of the run.
    //
    // Suppressing it is not enough: the tally this returns feeds the run's own
    // containment bars, and an empty one passes them no matter what the water is
    // doing. Grid drift has to RED the run rather than disarm it -- the mirror is
    // a transcription of BuildWaterFillRun's grid, and a transcription is exactly
    // the thing that goes stale when the shipped side moves.
    if (solved.Field.Diagnostics.OverBankCorners != production.OverBankCorners ||
        solved.Field.Diagnostics.WetCorners != production.WetCorners)
    {
        std::printf("       [refusals] FIELD MISMATCH (over %u vs %u, wet %u vs %u) -- "
                    "attribution suppressed\n",
                    solved.Field.Diagnostics.OverBankCorners, production.OverBankCorners,
                    solved.Field.Diagnostics.WetCorners, production.WetCorners);
        EXPECT_EQ(solved.Field.Diagnostics.OverBankCorners, production.OverBankCorners)
            << run.Name << ": the mirror grid refused a different number of corners than the run "
               "it mirrors, so the containment bars below have nothing to stand on";
        EXPECT_EQ(solved.Field.Diagnostics.WetCorners, production.WetCorners)
            << run.Name << ": the mirror grid wetted a different number of corners than the run "
               "it mirrors, so the containment bars below have nothing to stand on";
        return {};
    }

    const float32 reach = std::max(0.0f, run.Recipe.MaxHalfWidth);
    const float32 tuck = std::max(0.0f, run.Recipe.EdgeDrop);
    const float32 seaFloor = ShippedSeaFloor(run.Recipe);
    const float32 totalArc = run.Stations.back().Distance;
    constexpr uint32 kBuckets = 10u;
    uint32 byArc[kBuckets] = {};
    uint32 byLateral[kBuckets] = {};
    uint32 counted = 0;
    RefusalTally tally;
    float32 worst = 0.0f, worstX = 0.0f, worstZ = 0.0f, worstArc = 0.0f, worstLateral = 0.0f;

    for (uint32 cz = 0; cz < solved.Grid.CountZ; ++cz)
        for (uint32 cx = 0; cx < solved.Grid.CountX; ++cx)
        {
            const uint32 index = solved.Grid.Index(cx, cz);
            const SG::SplineFillCorner& corner = solved.Field.Corners[index];
            if (!RefusedByContainment(corner, solved.Heights[index],
                                      reach, tuck, seaFloor))
                continue;
            ++counted;
            if (corner.ArcDistance > reach && corner.ArcDistance < totalArc - reach)
            {
                ++tally.Interior;
                if (corner.EscapeUnseen != 0u)
                    ++tally.InteriorUnseen;
            }
            const float32 arcFraction =
                totalArc > 0.0f ? std::clamp(corner.ArcDistance / totalArc, 0.0f, 0.999f) : 0.0f;
            ++byArc[static_cast<uint32>(arcFraction * kBuckets)];
            const float32 lateralFraction =
                reach > 0.0f ? std::clamp(corner.Distance / reach, 0.0f, 0.999f) : 0.0f;
            ++byLateral[static_cast<uint32>(lateralFraction * kBuckets)];
            const float32 over = corner.Waterline - HeldAt(corner, seaFloor);
            // Seeded by the first refusal rather than by zero, and walked in
            // lattice order, so this picks the same corner out of a tie that
            // the fill's own ranking does -- the two are reconciled below.
            if (counted == 1u || over > worst)
            {
                worst = over;
                worstX = solved.Grid.WorldX(cx);
                worstZ = solved.Grid.WorldZ(cz);
                worstArc = corner.ArcDistance;
                worstLateral = corner.Distance;
            }
        }

    // The ROI as a picture: '#' wet, 'o' refused, ':' stamped but neither,
    // ' ' outside the corridor. A histogram says how much; this says where, and
    // a shape is what tells an author whether the trouble is the mouth, a bend
    // or one bank.
    if (EnvOrNull("GE_SCENE_WATER_GATE_MAP_FILL") != nullptr)
    {
        constexpr uint32 kCols = 116u;
        const uint32 cellX = std::max(1u, solved.Grid.CountX / kCols);
        const uint32 cellZ = std::max(1u, cellX * 2u);
        std::printf("       [shape] %u x %u cells of %.1f x %.1f m\n",
                    solved.Grid.CountX / cellX, solved.Grid.CountZ / cellZ,
                    cellX * solved.Grid.SpacingX, cellZ * solved.Grid.SpacingZ);
        for (uint32 bz = 0; bz + cellZ <= solved.Grid.CountZ; bz += cellZ)
        {
            std::printf("       [shape] ");
            for (uint32 bx = 0; bx + cellX <= solved.Grid.CountX; bx += cellX)
            {
                uint32 wet = 0, refused = 0, stamped = 0;
                for (uint32 z = bz; z < bz + cellZ; ++z)
                    for (uint32 x = bx; x < bx + cellX; ++x)
                    {
                        const uint32 index = solved.Grid.Index(x, z);
                        const SG::SplineFillCorner& c = solved.Field.Corners[index];
                        if (c.Station == SG::kNoStation)
                            continue;
                        ++stamped;
                        if (c.Wet != 0u)
                        {
                            ++wet;
                            continue;
                        }
                        if (RefusedByContainment(c, solved.Heights[index], reach, tuck, seaFloor))
                            ++refused;
                    }
                std::putchar(stamped == 0u ? ' ' : refused * 2u > stamped ? 'o'
                                                 : wet * 2u > stamped     ? '#'
                                                 : refused > 0u           ? '.'
                                                                          : ':');
            }
            std::printf("\n");
        }
    }

    std::printf("       [refusals] %u located of %u (%u interior, %u in the end caps); worst "
                "%.2f m at (%.1f, %.1f), arc %.0f m, %.1f m out\n",
                counted, production.OverBankCorners, tally.Interior, counted - tally.Interior,
                worst, worstX, worstZ, worstArc, worstLateral);
    std::printf("       [refusals] by arc tenth :");
    for (uint32 i = 0; i < kBuckets; ++i)
        std::printf(" %6u", byArc[i]);
    std::printf("\n       [refusals] by reach tenth:");
    for (uint32 i = 0; i < kBuckets; ++i)
        std::printf(" %6u", byLateral[i]);
    std::printf("\n");

    // The field parity check above says this is the same FIELD; this says the
    // predicate above is the same PREDICATE. Both are needed, and only the
    // second one moves when the shipped containment term changes underneath a
    // transcription of it -- which is exactly how a stale sea-level floor and a
    // missing post-seal skip sat here unnoticed, printing a wrong total beside
    // the right one.
    EXPECT_EQ(counted, production.OverBankCorners)
        << run.Name << ": the refusal recount disagrees with the field's own OverBankCorners, so "
           "the attribution below describes a different predicate than the one that ran";

    // The count alone is a blunt instrument for this: a floor that has drifted by
    // centimetres moves no corner across the predicate, but it moves every
    // overshoot it measures. The worst one is the term that shows it, so it is
    // the term worth pinning -- FLOAT_EQ because the two are the same expression
    // over the same corners and should differ only in the last bits, which is
    // orders below the drift this is here to catch.
    EXPECT_FLOAT_EQ(worst, production.MaxOverBankMetres)
        << run.Name << ": the recount's worst overshoot disagrees with the field's own "
           "MaxOverBankMetres -- it is measuring against a different waterline floor";

    // ...and WHERE it was measured, which is what the author-facing warning
    // quotes. The magnitude agreeing does not make the position right: the two
    // are read off different members, and a matching number beside the wrong
    // corner sends the author to the wrong stretch of river.
    EXPECT_FLOAT_EQ(worstX, production.WorstOverBank.X) << run.Name;
    EXPECT_FLOAT_EQ(worstZ, production.WorstOverBank.Z) << run.Name;
    EXPECT_FLOAT_EQ(worstArc, production.WorstOverBank.ArcDistance) << run.Name;
    return tally;
}

const char* OutcomeName(WaterFillOutcome outcome)
{
    switch (outcome)
    {
    case WaterFillOutcome::Built: return "Built";
    case WaterFillOutcome::NoTerrain: return "NoTerrain";
    case WaterFillOutcome::SampleWindowTooLarge: return "SampleWindowTooLarge";
    case WaterFillOutcome::OverBudget: return "OverBudget";
    case WaterFillOutcome::NoRegion: return "NoRegion";
    }
    return "?";
}

class SceneWaterFillGate : public ::testing::Test
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

    // Load, then make the world the same shape a running editor would: world
    // transforms derived, terrain data provisioned from the authored config, and
    // the modifier stack baked.
    void LoadAndBake()
    {
        const char* scenePath = EnvOrNull("GE_SCENE_WATER_GATE");
        ASSERT_NE(scenePath, nullptr);
        const std::filesystem::path path(scenePath);
        ASSERT_TRUE(std::filesystem::exists(path)) << "no such scene: " << scenePath;

        GameEngine::Scene::LoadOptions options;
        options.mode = GameEngine::Scene::LoadMode::Replace;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(m_World, path, options))
            << "scene failed to load: " << scenePath << " -- "
            << GameEngine::Scene::GetLastSceneIOError().message;
        m_World.ProcessCommands();

        // The Spline schema leaves its data Dirty (no arc-length LUT, no segment
        // bounds); SplineExtractionSystem is what rebuilds it every frame, and
        // both the modifier bake and the fill read that cache.
        GameEngine::SplineECS::SplineExtractionSystem splines;
        splines.Update(m_World, 0.0f);

        GameEngine::Engine::Renderer::TransformHierarchySystem hierarchy;
        hierarchy.Update(m_World, 0.0f);

        ProvisionTerrain();
        if (::testing::Test::HasFatalFailure())
            return;

        hierarchy.Update(m_World, 0.0f);

        GameEngine::TerrainECS::TerrainModifierSystem modifiers;
        modifiers.Update(m_World, 1.0f / 60.0f);
    }

    // The Terrain schema serializes configuration only; the data behind
    // TerrainDataHandle is provisioned at runtime by TerrainExtractionSystem,
    // which needs a device. This is that provisioning, and nothing else: the
    // same CreateTerrain + base fill the extraction system performs.
    void ProvisionTerrain()
    {
        std::vector<EntityHandle> terrains;
        m_World.Query<GameEngine::ECS::Read<Components::Terrain>>().Each(
            [&](EntityHandle e, const Components::Terrain&) { terrains.push_back(e); });
        ASSERT_EQ(terrains.size(), 1u) << "the gate expects exactly one Terrain in the scene";

        auto* terrain = m_World.GetComponentForWrite<Components::Terrain>(terrains.front());
        ASSERT_NE(terrain, nullptr);
        ASSERT_EQ(terrain->Domain, Components::TerrainDomain::Planar);

        const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
            terrain->SizeX, terrain->SizeZ, terrain->HeightScale, terrain->SamplesPerMeter);
        const GameEngine::TerrainECS::TerrainHandle handle =
            TerrainService::Get().CreateTerrain(config);
        ASSERT_NE(handle.Generation, 0u);
        auto* data = TerrainService::Get().GetTerrainData(handle);
        ASSERT_NE(data, nullptr);

        // A HeightmapAsset base has no decoded source here and degrades to Flat,
        // which would silently measure a different island than the editor bakes.
        ASSERT_NE(terrain->BaseSource, Components::TerrainBaseSource::HeightmapAsset)
            << "this gate cannot provision a heightmap base without the asset pipeline";

        GameEngine::TerrainECS::FillHeightfieldBaseRegion(
            data->Heightfield, terrain->BaseSource, nullptr, 0, 0,
            static_cast<int32>(data->Heightfield.GetWidth()) - 1,
            static_cast<int32>(data->Heightfield.GetHeight()) - 1);
        data->MarkFullDirty();
        TerrainService::Get().RebuildQuadtree(handle);

        terrain->TerrainDataHandle = handle.Index;
        terrain->TerrainDataGeneration = handle.Generation;

        std::printf("[scene] terrain %.0f x %.0f m, heightScale %.1f, %.2f samples/m -> %u x %u "
                    "lattice, baseSource %d\n",
                    terrain->SizeX, terrain->SizeZ, terrain->HeightScale, terrain->SamplesPerMeter,
                    data->Heightfield.GetWidth(), data->Heightfield.GetHeight(),
                    static_cast<int>(terrain->BaseSource));
    }

    // Every SplineExtrude in the world whose WidthMode is FitToBanks, with the
    // stations already built.
    std::vector<FittedRun> CollectFittedRuns()
    {
        std::vector<FittedRun> runs;
        auto* splines = SplineECS::SplineService::TryGet();
        if (splines == nullptr)
            return runs;

        std::vector<EntityHandle> candidates;
        m_World.Query<GameEngine::ECS::Read<Components::SplineExtrude>>().Each(
            [&](EntityHandle e, const Components::SplineExtrude&) { candidates.push_back(e); });

        for (EntityHandle e : candidates)
        {
            const auto* recipe = m_World.GetComponent<Components::SplineExtrude>(e);
            if (recipe == nullptr ||
                recipe->WidthMode != Components::SplineExtrudeWidthMode::FitToBanks)
                continue;

            const auto* comp = m_World.GetComponent<Components::SplineComponent>(e);
            if (comp == nullptr || !GameEngine::ECS::Entity(&m_World, e).IsEnabled<GameEngine::Components::SplineComponent>())
                continue;
            const auto* data = splines->GetSplineData(
                SplineECS::SplineHandle(comp->SplineDataIndex, comp->SplineDataGeneration));
            if (data == nullptr || !data->IsValid())
                continue;

            FittedRun run;
            run.Entity = e;
            run.Name = EntityLabel(m_World, e);
            run.Recipe = *recipe;
            run.Data = data;
            std::memset(run.World.Data(), 0, sizeof(float32) * 16);
            run.World.Data()[0] = run.World.Data()[5] = run.World.Data()[10] =
                run.World.Data()[15] = 1.0f;
            if (const auto* wt = m_World.GetComponent<Components::WorldTransform>(e))
                run.World = Matrix4x4::FromColumnMajor(wt->matrix);
            run.Stations = BuildWorldStations(*data, *recipe, run.World);
            runs.push_back(std::move(run));
        }
        return runs;
    }

    World m_World;
    ScopedTerrainService m_TerrainScope;
    ScopedSplineService m_SplineScope;
};

#define SKIP_WITHOUT_SCENE()                                                                       \
    do                                                                                             \
    {                                                                                              \
        if (EnvOrNull("GE_SCENE_WATER_GATE") == nullptr)                                           \
            GTEST_SKIP() << "set GE_SCENE_WATER_GATE to an absolute .scene path";                  \
    } while (false)

} // namespace

// The file parses AND materializes. A scene load tolerates an unknown field
// (warns, keeps the default) and preserves an unknown COMPONENT verbatim, so a
// misspelling is silent at load and shows up only as a component that is not
// there -- which is why this asserts the inventory rather than the return code.
TEST_F(SceneWaterFillGate, TheSceneLoadsAndCarriesItsAuthoredComponents)
{
    SKIP_WITHOUT_SCENE();
    LoadAndBake();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    uint32 entities = 0;
    uint32 volumes = 0;
    uint32 splineComponents = 0;
    uint32 extrudes = 0;
    m_World.Query<GameEngine::ECS::Read<Components::Transform>>().Each(
        [&](EntityHandle, const Components::Transform&) { ++entities; });
    m_World.Query<GameEngine::ECS::Read<Components::TerrainModifierVolume>>().Each(
        [&](EntityHandle, const Components::TerrainModifierVolume&) { ++volumes; });
    m_World.Query<GameEngine::ECS::Read<Components::SplineComponent>>().Each(
        [&](EntityHandle, const Components::SplineComponent&) { ++splineComponents; });
    m_World.Query<GameEngine::ECS::Read<Components::SplineExtrude>>().Each(
        [&](EntityHandle, const Components::SplineExtrude&) { ++extrudes; });

    std::printf("[scene] entities=%u modifierVolumes=%u splines=%u splineExtrudes=%u\n", entities,
                volumes, splineComponents, extrudes);

    EXPECT_GT(volumes, 0u) << "no TerrainModifierVolume survived the load";
    EXPECT_GT(splineComponents, 0u) << "no Spline survived the load";

    const std::vector<FittedRun> runs = CollectFittedRuns();
    std::printf("[scene] FitToBanks runs: %zu\n", runs.size());
    for (const FittedRun& run : runs)
        std::printf("        %-28s points=%zu arc=%.1f m  maxHalfWidth=%.1f verticalOffset=%.2f "
                    "edgeDrop=%.2f seaFloor=%.3f conform=%d\n",
                    run.Name.c_str(), run.Data->Points.size(), run.Data->TotalArcLength,
                    run.Recipe.MaxHalfWidth, run.Recipe.VerticalOffset, run.Recipe.EdgeDrop,
                    run.Recipe.SeaLevelFloor, static_cast<int>(run.Recipe.ConformMode));
    EXPECT_FALSE(runs.empty()) << "no FitToBanks SplineExtrude survived the load";
}

// The derivation table. A water spline's authored Y is only defensible against
// the ground the bake actually produced, so this prints that ground at every
// control point of every spline in the file -- bed splines included, because a
// bed's own carve target is what the water is derived from.
TEST_F(SceneWaterFillGate, GroundUnderEveryControlPoint)
{
    SKIP_WITHOUT_SCENE();
    LoadAndBake();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(m_World);
    ASSERT_TRUE(terrain.IsValid());

    auto* splines = SplineECS::SplineService::TryGet();
    ASSERT_NE(splines, nullptr);

    std::vector<EntityHandle> entities;
    m_World.Query<GameEngine::ECS::Read<Components::SplineComponent>>().Each(
        [&](EntityHandle e, const Components::SplineComponent&) { entities.push_back(e); });

    for (EntityHandle e : entities)
    {
        const auto* comp = m_World.GetComponent<Components::SplineComponent>(e);
        const auto* data = splines->GetSplineData(
            SplineECS::SplineHandle(comp->SplineDataIndex, comp->SplineDataGeneration));
        if (data == nullptr || !data->IsValid())
            continue;

        Matrix4x4 worldM;
        std::memset(worldM.Data(), 0, sizeof(float32) * 16);
        worldM.Data()[0] = worldM.Data()[5] = worldM.Data()[10] = worldM.Data()[15] = 1.0f;
        if (const auto* wt = m_World.GetComponent<Components::WorldTransform>(e))
            worldM = Matrix4x4::FromColumnMajor(wt->matrix);

        std::printf("\n[ground] %s (%zu points)\n", EntityLabel(m_World, e).c_str(),
                    data->Points.size());
        std::printf("         i        x        z   authoredY    bakedY     delta\n");
        for (size_t i = 0; i < data->Points.size(); ++i)
        {
            const Vector3 world = worldM.TransformPoint(data->Points[i].Position);
            float32 ground = 0.0f;
            const bool ok = terrain.SampleHeight(world.x, world.z, ground);
            std::printf("      %4zu %8.2f %8.2f  %9.3f %9.3f %9.3f%s\n", i, world.x, world.z,
                        world.y, ground, ground - world.y, ok ? "" : "   <-- off terrain");
        }
    }
}

// The island as a map: a coarse height grid to route a course on, and the
// height histogram that says where the sea belongs. Authoring a river against
// point probes alone is how a course ends up climbing a spur between two of
// them.
TEST_F(SceneWaterFillGate, ReliefMapAndSeaBand)
{
    SKIP_WITHOUT_SCENE();
    LoadAndBake();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(m_World);
    ASSERT_TRUE(terrain.IsValid());

    const float32 step = EnvUint("GE_SCENE_WATER_GATE_MAP_STEP", 16u) * 1.0f;
    const float32 minX = terrain.OriginX;
    const float32 minZ = terrain.OriginZ;
    const int32 countX = static_cast<int32>(terrain.SizeX / step) + 1;
    const int32 countZ = static_cast<int32>(terrain.SizeZ / step) + 1;

    std::printf("\n[map] %.0f m grid, rows are Z ascending, columns X ascending from "
                "(%.0f, %.0f)\n",
                step, minX, minZ);
    std::printf("[map] %6s", "z\\x");
    for (int32 ix = 0; ix < countX; ++ix)
        std::printf("%6.0f", minX + static_cast<float32>(ix) * step);
    std::printf("\n");
    for (int32 iz = 0; iz < countZ; ++iz)
    {
        const float32 worldZ = std::min(minZ + static_cast<float32>(iz) * step,
                                        minZ + terrain.SizeZ);
        std::printf("[map] %6.0f", worldZ);
        for (int32 ix = 0; ix < countX; ++ix)
        {
            const float32 worldX = std::min(minX + static_cast<float32>(ix) * step,
                                            minX + terrain.SizeX);
            float32 h = 0.0f;
            if (terrain.SampleHeight(worldX, worldZ, h))
                std::printf("%6.1f", h);
            else
                std::printf("%6s", "-");
        }
        std::printf("\n");
    }

    // The band the sea has to clear. Sampled on the same lattice the fill reads,
    // so "fraction below y" is the fraction of the island that y submerges.
    const uint32 width = terrain.Single != nullptr ? terrain.Single->Heightfield.GetWidth() : 0u;
    const uint32 height = terrain.Single != nullptr ? terrain.Single->Heightfield.GetHeight() : 0u;
    ASSERT_GT(width, 0u);
    std::vector<float32> samples;
    samples.reserve(static_cast<size_t>(width) * height);
    for (uint32 zi = 0; zi < height; ++zi)
        for (uint32 xi = 0; xi < width; ++xi)
            samples.push_back(terrain.OriginY +
                              terrain.Single->Heightfield.GetSample(xi, zi) * terrain.HeightScale);
    std::sort(samples.begin(), samples.end());
    std::printf("\n[band] %zu lattice samples: min %.2f  max %.2f\n", samples.size(),
                samples.front(), samples.back());
    for (const float32 q : {0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.7f, 0.9f, 0.99f})
        std::printf("[band] p%-4.0f %8.2f m\n", q * 100.0f,
                    samples[static_cast<size_t>(q * static_cast<float32>(samples.size() - 1))]);
    for (const float32 level : {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f})
    {
        const size_t below = static_cast<size_t>(
            std::lower_bound(samples.begin(), samples.end(), level) - samples.begin());
        std::printf("[band] sea at %4.1f m submerges %5.1f%% of the footprint\n", level,
                    100.0f * static_cast<float32>(below) / static_cast<float32>(samples.size()));
    }
}

// The other bar: the SURFACE the composition is delivered on. A scene can be
// right in plan and unusable in relief -- erosion tuned for a 200 m island turns
// a 512 m one into bristle -- and every water gate above stays green while it
// happens, because a sawtooth ridge holds a river exactly as well as a rolling
// one does. Nothing else here measures the ground as ground.
//
// Slope per LATTICE CELL, from the plane through its four corners: that plane's
// gradient direction IS its steepest direction, so atan|grad| is the cell's
// max-gradient slope. A cell counts as landmass when its centre stands above
// GE_SCENE_WATER_GATE_SEA_Y -- the sea floor is not ground anyone walks or looks
// at, and leaving it in would let a large shallow shelf pay for a bristling
// island.
//
// The max-EDGE slope reported beside it is the same cell measured across its
// four edges and two diagonals instead of through the fitted plane. It is never
// smaller, and the GAP between the two is the knife-edge term: a rolling
// hillside fits its plane and the pair agree, a one-cell sawtooth does not.
//
// The splat tally beside it is a SEPARATE bar, not a consequence of the slope
// one. Material placement comes from the terrain's authored SURFACE RULE rows,
// so what the tally shows is what those rows were told to do plus whatever the
// paint volumes intend -- it is not derived from slope by the bake itself. A
// scene carrying the shipped defaults does place rock and dirt on its steeper
// ground (measured in TerrainDefaultSurfaceRulesTests, in TerrainRegionBakeTests);
// a scene whose rules were deleted or re-authored may place neither, and that is
// an authoring fact about the scene, not a bake defect.
TEST_F(SceneWaterFillGate, TheLandAboveTheSeaRollsRatherThanBristles)
{
    SKIP_WITHOUT_SCENE();
    LoadAndBake();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(m_World);
    ASSERT_TRUE(terrain.IsValid());
    ASSERT_NE(terrain.Single, nullptr)
        << "the slope bar reads the baked lattice directly; there is no tiled path here";

    const float32 seaY = EnvFloat("GE_SCENE_WATER_GATE_SEA_Y", 3.0f);
    const float32 maxMedian = EnvFloat("GE_SCENE_WATER_GATE_MAX_MEDIAN_SLOPE", 30.0f);
    const float32 maxP95 = EnvFloat("GE_SCENE_WATER_GATE_MAX_P95_SLOPE", 55.0f);

    const GameEngine::Terrain::HeightfieldData& field = terrain.Single->Heightfield;
    const uint32 width = field.GetWidth();
    const uint32 height = field.GetHeight();
    ASSERT_GE(width, 2u);
    ASSERT_GE(height, 2u);
    const float32 spacingX = terrain.LatticeSpacingX();
    const float32 spacingZ = terrain.LatticeSpacingZ();
    ASSERT_GT(spacingX, 0.0f);
    ASSERT_GT(spacingZ, 0.0f);
    const float32 diagonal = std::sqrt(spacingX * spacingX + spacingZ * spacingZ);

    auto worldY = [&](uint32 xi, uint32 zi) {
        return terrain.OriginY + field.GetSample(xi, zi) * terrain.HeightScale;
    };

    const float32 toDegrees = 57.2957795130823f;
    std::vector<float32> planeSlope;
    std::vector<float32> edgeSlope;
    planeSlope.reserve(static_cast<size_t>(width - 1u) * (height - 1u));
    edgeSlope.reserve(planeSlope.capacity());
    // Splat weights of the same land cells, in splat channel order. The scene's
    // Terrain.layerAlbedoN decides what each channel DRAWS; the channel names below
    // are the shipped role convention (grass, rock, dirt, snow), not a claim about
    // which rule put weight there.
    const bool splatAligned = terrain.Single->SplatmapWidth == width &&
                              terrain.Single->SplatmapHeight == height &&
                              terrain.Single->Splatmap.size() ==
                                  static_cast<size_t>(width) * height * 4u;
    double splatSum[4] = {};
    uint32 splatDominant[4] = {};
    uint32 landCells = 0;
    uint32 allCells = 0;

    for (uint32 zi = 0; zi + 1u < height; ++zi)
        for (uint32 xi = 0; xi + 1u < width; ++xi)
        {
            const float32 h00 = worldY(xi, zi);
            const float32 h10 = worldY(xi + 1u, zi);
            const float32 h01 = worldY(xi, zi + 1u);
            const float32 h11 = worldY(xi + 1u, zi + 1u);
            ++allCells;
            if ((h00 + h10 + h01 + h11) * 0.25f <= seaY)
                continue;
            ++landCells;

            const float32 dhdx = ((h10 - h00) + (h11 - h01)) / (2.0f * spacingX);
            const float32 dhdz = ((h01 - h00) + (h11 - h10)) / (2.0f * spacingZ);
            planeSlope.push_back(std::atan(std::sqrt(dhdx * dhdx + dhdz * dhdz)) * toDegrees);

            const float32 worst = std::max({std::abs(h10 - h00) / spacingX,
                                            std::abs(h11 - h01) / spacingX,
                                            std::abs(h01 - h00) / spacingZ,
                                            std::abs(h11 - h10) / spacingZ,
                                            std::abs(h11 - h00) / diagonal,
                                            std::abs(h10 - h01) / diagonal});
            edgeSlope.push_back(std::atan(worst) * toDegrees);

            if (splatAligned)
            {
                const size_t texel = (static_cast<size_t>(zi) * width + xi) * 4u;
                uint32 best = 0;
                for (uint32 c = 0; c < 4u; ++c)
                {
                    const float32 weight =
                        static_cast<float32>(terrain.Single->Splatmap[texel + c]) / 255.0f;
                    splatSum[c] += weight;
                    if (terrain.Single->Splatmap[texel + c] >
                        terrain.Single->Splatmap[texel + best])
                        best = c;
                }
                ++splatDominant[best];
            }
        }

    ASSERT_GT(landCells, 0u) << "nothing stands above " << seaY << " m; there is no landmass to "
                                "measure (is the sea level right?)";
    std::sort(planeSlope.begin(), planeSlope.end());
    std::sort(edgeSlope.begin(), edgeSlope.end());

    const float32 cellArea = spacingX * spacingZ;
    std::printf("\n[slope] landmass %u of %u cells above %.1f m (%.0f m2 of %.0f m2), cell "
                "%.2f x %.2f m\n",
                landCells, allCells, seaY, static_cast<float32>(landCells) * cellArea,
                static_cast<float32>(allCells) * cellArea, spacingX, spacingZ);
    std::printf("[slope] plane-fit degrees: p05 %.1f  p25 %.1f  MEDIAN %.1f  p75 %.1f  p90 %.1f  "
                "P95 %.1f  p99 %.1f  max %.1f\n",
                Percentile(planeSlope, 0.05f), Percentile(planeSlope, 0.25f),
                Percentile(planeSlope, 0.50f), Percentile(planeSlope, 0.75f),
                Percentile(planeSlope, 0.90f), Percentile(planeSlope, 0.95f),
                Percentile(planeSlope, 0.99f), planeSlope.back());
    std::printf("[slope] max-edge   degrees: p05 %.1f  p25 %.1f  median %.1f  p75 %.1f  p90 %.1f  "
                "p95 %.1f  p99 %.1f  max %.1f\n",
                Percentile(edgeSlope, 0.05f), Percentile(edgeSlope, 0.25f),
                Percentile(edgeSlope, 0.50f), Percentile(edgeSlope, 0.75f),
                Percentile(edgeSlope, 0.90f), Percentile(edgeSlope, 0.95f),
                Percentile(edgeSlope, 0.99f), edgeSlope.back());

    // The histogram the bar is read off. Percentiles say where the mass sits;
    // the shape of the tail says whether the steep ground is a few cliffs or the
    // whole surface, and those are different authoring problems.
    constexpr float32 kBinDegrees = 5.0f;
    constexpr uint32 kBins = 18u; // 0..90
    uint32 bins[kBins] = {};
    for (const float32 s : planeSlope)
        bins[std::min<uint32>(kBins - 1u, static_cast<uint32>(s / kBinDegrees))] += 1u;
    std::printf("[slope] histogram (plane-fit, %% of landmass, cumulative):\n");
    uint32 running = 0;
    for (uint32 b = 0; b < kBins; ++b)
    {
        if (bins[b] == 0u)
            continue;
        running += bins[b];
        const float32 share = 100.0f * static_cast<float32>(bins[b]) /
                              static_cast<float32>(landCells);
        std::printf("[slope] %3.0f-%3.0f %7.2f%% %7.2f%%  %s\n", b * kBinDegrees,
                    (b + 1u) * kBinDegrees, share,
                    100.0f * static_cast<float32>(running) / static_cast<float32>(landCells),
                    std::string(static_cast<size_t>(share * 0.8f), '#').c_str());
    }

    if (splatAligned)
    {
        std::printf("[splat] over the same landmass, in channel order "
                    "(grass, rock, dirt, snow):\n");
        for (uint32 c = 0; c < 4u; ++c)
            std::printf("[splat] channel %u: mean weight %.3f, dominant on %5.1f%% of cells\n", c,
                        static_cast<float32>(splatSum[c] / landCells),
                        100.0f * static_cast<float32>(splatDominant[c]) /
                            static_cast<float32>(landCells));
    }
    else
    {
        std::printf("[splat] no splatmap at lattice resolution; composition not reported\n");
    }

    EXPECT_LT(Percentile(planeSlope, 0.50f), maxMedian)
        << "half the landmass is steeper than " << maxMedian
        << " degrees: this is bristle, not relief (reduce erosion strength/gully weight, or the "
           "amplitude of the finest noise volume)";
    EXPECT_LT(Percentile(planeSlope, 0.95f), maxP95)
        << "the steep tail reaches " << Percentile(planeSlope, 0.95f)
        << " degrees at p95: the surface is serrated even where it is not tall";
}

// The bar. Water may flow downstream; it may not stand over its own bank, and
// the region may not be decided by how far the search was allowed to look.
TEST_F(SceneWaterFillGate, EveryFittedRunHoldsTheWaterItsGroundCanHold)
{
    SKIP_WITHOUT_SCENE();
    LoadAndBake();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    const uint32 maxOverBank = EnvUint("GE_SCENE_WATER_GATE_MAX_OVERBANK", 0u);
    const uint32 maxUnseen = EnvUint("GE_SCENE_WATER_GATE_MAX_UNSEEN", 100u);

    std::vector<FittedRun> runs = CollectFittedRuns();
    ASSERT_FALSE(runs.empty());

    const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(m_World);
    ASSERT_TRUE(terrain.IsValid());

    for (const FittedRun& run : runs)
    {
        // A conforming recipe drapes against scene geometry this harness has no
        // picking service for; measuring it here would measure a different
        // centreline than the editor's.
        ASSERT_EQ(run.Recipe.ConformMode, Components::SplinePlacementConform::None)
            << run.Name << ": the gate only measures ConformMode=None runs";
        ASSERT_GE(run.Stations.size(), 2u) << run.Name;

        float32 matrix[16];
        std::memcpy(matrix, run.World.Data(), sizeof(matrix));
        // Single (untiled) terrain: wholly resident, so it needs no composed ground
        // source. A tiled scene would report NoTerrain here rather than pass quietly.
        const WaterFillRun built =
            BuildWaterFillRun(m_World, run.Stations, run.Recipe, matrix, nullptr);
        const SG::SplineFillDiagnostics& d = built.Diagnostics;

        uint32 vertices = 0;
        uint32 indices = 0;
        for (const SG::SplineFillMesh& chunk : built.Chunks)
        {
            vertices += static_cast<uint32>(chunk.Vertices.size());
            indices += static_cast<uint32>(chunk.Indices.size());
        }

        std::printf("\n[fill] %s -> %s\n", run.Name.c_str(), OutcomeName(built.Outcome));
        std::printf("       stations=%zu  seeds=%u dry=%u  wet=%u (%.0f m2)  chunks=%zu "
                    "verts=%u tris=%u\n",
                    run.Stations.size(), d.Seeds, d.DrySeeds, d.WetCorners,
                    static_cast<float32>(d.WetCorners) * terrain.LatticeSpacingX() *
                        terrain.LatticeSpacingZ(),
                    built.Chunks.size(), vertices, indices / 3u);
        // sealedPocket is reported beside overBank because they partition the
        // corners the containment term refused: the sealed ones were taken back,
        // the over-bank ones were not. A run whose banks dip inside the channel
        // shows it here long before it shows anywhere else.
        std::printf("       overBank=%u (worst %.3f m)  sealedPocket=%u  unseenBank=%u  "
                    "maxDepth=%.3f  medialStep=%.3f  rise=%.3f at arc %.1f\n",
                    d.OverBankCorners, d.MaxOverBankMetres, d.SealedPocketCorners,
                    d.UnseenBankCorners, d.MaxDepthMetres, d.MedialStepMetres,
                    d.WaterlineRiseMetres, d.WaterlineRiseAtArc);

        // Per-station containment, so a failure names WHERE rather than only how
        // much: the waterline, the ground under the station, and the lowest
        // ground on the corridor boundary beside it.
        std::printf("       station (arc, x, z, waterline, bedUnder, minRimAtReach):\n");
        const float32 reach = run.Recipe.MaxHalfWidth;
        const size_t stride = std::max<size_t>(1u, run.Stations.size() / 24u);
        for (size_t i = 0; i < run.Stations.size(); i += stride)
        {
            const SG::SplineStripStation& s = run.Stations[i];
            float32 bed = 0.0f;
            const bool bedRead = terrain.SampleHeight(s.Position.x, s.Position.z, bed);
            float32 rim = 1.0e9f;
            for (int32 side = -1; side <= 1; side += 2)
                for (float32 offset = reach - 1.0f; offset <= reach + 1.0f; offset += 0.5f)
                {
                    const float32 px = s.Position.x + s.Right.x * offset * static_cast<float32>(side);
                    const float32 pz = s.Position.z + s.Right.z * offset * static_cast<float32>(side);
                    float32 h = 0.0f;
                    if (terrain.SampleHeight(px, pz, h))
                        rim = std::min(rim, h);
                }
            std::printf("        %7.2f %8.2f %8.2f  %8.3f %9.3f %9.3f%s\n", s.Distance,
                        s.Position.x, s.Position.z, s.Position.y, bed, rim,
                        !bedRead                     ? "   <-- station off terrain"
                        : s.Position.y - bed <= 0.0f ? "   <-- DRY station"
                                                     : "");
        }

        // Every station the bed fails to get below, named. A count says how
        // many metres of run have no water in them; only the positions say why.
        if (d.DrySeeds > 0u)
        {
            std::printf("       first dry seed at (%.2f, %.2f, %.2f); dry stations:\n",
                        d.FirstDrySeed.x, d.FirstDrySeed.y, d.FirstDrySeed.z);
            uint32 shown = 0;
            for (const SG::SplineStripStation& s : run.Stations)
            {
                float32 g = 0.0f;
                if (!terrain.SampleHeight(s.Position.x, s.Position.z, g) || g < s.Position.y)
                    continue;
                if (shown++ < 20u)
                    std::printf("        arc %7.2f at (%8.2f, %8.2f)  waterline %8.3f  "
                                "ground %8.3f\n",
                                s.Distance, s.Position.x, s.Position.z, s.Position.y, g);
            }
            std::printf("        (%u stations total)\n", shown);
        }

        // The channel in section. A bank that is not there is invisible in a
        // count and obvious in a profile.
        {
            std::printf("       cross-sections (ground at metres left..right of the "
                        "centreline; * = below the waterline):\n");
            const size_t sectionStride = std::max<size_t>(1u, run.Stations.size() / 8u);
            for (size_t i = 0; i < run.Stations.size(); i += sectionStride)
            {
                const SG::SplineStripStation& s = run.Stations[i];
                std::printf("        arc %6.1f w=%7.3f |", s.Distance, s.Position.y);
                for (float32 offset = -run.Recipe.MaxHalfWidth;
                     offset <= run.Recipe.MaxHalfWidth + 0.01f; offset += 2.0f)
                {
                    float32 g = 0.0f;
                    const bool ok = terrain.SampleHeight(s.Position.x + s.Right.x * offset,
                                                         s.Position.z + s.Right.z * offset, g);
                    std::printf("%7.2f%c", ok ? g : 0.0f, (ok && g < s.Position.y) ? '*' : ' ');
                }
                std::printf("\n");
            }
        }

        // WHERE the refusals are. The run's counters say how many and how bad;
        // an author needs the arc position and the lateral distance, because
        // those two name different fixes -- a stretch of course to move, or a
        // reach to pull in. Rebuilding the field here costs one more solve and
        // is checked against the run's own counters below, so it cannot drift
        // into describing a different field.
        const RefusalTally refusals = ReportRefusals(run, terrain, d);

        EXPECT_EQ(built.Outcome, WaterFillOutcome::Built) << run.Name;
        EXPECT_EQ(d.DrySeeds, 0u) << run.Name << ": the bed is not below the waterline there "
                                     "(carve deeper, or lift VerticalOffset)";
        EXPECT_LE(refusals.Interior, maxOverBank)
            << run.Name << ": water stands over ground it would flow across along the run "
               "itself, worst overall by "
            << d.MaxOverBankMetres << " m (carve deeper, or lower VerticalOffset)";
        EXPECT_LE(refusals.InteriorUnseen, maxUnseen)
            << run.Name << ": along the run itself the banks are outside MaxHalfWidth (widen "
               "the reach); the run reports " << d.UnseenBankCorners << " over the whole field";
        EXPECT_LT(d.WaterlineRiseMetres, SG::kWaterlineRiseToleranceMetres)
            << run.Name << ": the waterline runs uphill at arc " << d.WaterlineRiseAtArc;
    }
}
