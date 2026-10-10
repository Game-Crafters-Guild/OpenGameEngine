// The sky-tail defect, reproduced at test level.
//
// A fence run passing under an overhanging prop took that prop's surface for
// ground: its stations stood on the canopy, metres above the terrain. The
// engine had already settled what a conform ray may land on — SplineConformTarget,
// Scene or TerrainOnly — but the enum reached SplinePlacement only, so a fence
// recipe could not express the choice and was hard-wired to the whole scene.
//
// A fence is more exposed to a wrong hit than a tile path, and that is what
// these tests pin. A run's length is the 3D length of its DRAPED polyline
// (FenceLayout.cpp: PolylineLength over the sliced run), and the fill is solved
// from that length, so a prop overhead does not merely lift the stations
// beneath it: the climb up its side and back down lengthens the run, re-solves
// how many spans fill it, and moves every station in the run. One tree changes
// a whole fence.
//
// The drape loop below mirrors SplineFenceController::Rebuild for an identity
// placer transform — the same shape SplineExtrudeCoverageTests mirrors for its
// controller, and for the same reason: the controllers need render services to
// resolve their pools, while the geometry contract under test needs none.
//
// Both halves of every comparison are asserted. A mode that did nothing at all
// would pass either half alone.
//
// The extrude recipe gained the same field and is deliberately NOT covered by a
// twin of these tests. What such a test could reach — that ConformRayDown
// honours a target — is asserted here and in SplineDrapeTests already; what it
// could not reach is what these cannot reach either. None of the three spline
// controllers is compiled into this target (they need render services), so no
// test here catches a controller dropping its recipe's target: removing the
// argument at all three routed sites still compiles and still passes this whole
// suite. Duplicating the drape mirror for a second recipe would buy coverage of
// the mirror, not of the code. That gap is closed by making the target a
// REQUIRED parameter of a shared drape loop, which is filed rather than done
// here.

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "Mathematics/Vector3.h"
#include "Placement/CenterlineSampling.h"
#include "Placement/FenceLayout.h"
#include "Placement/SplineSurfaceConform.h"
#include "Scene/SceneTlas.h"
#include "Scripting/ScriptsConfig.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;

using GameEngine::Components::SplineConformTarget;
using GameEngine::Components::SplineFence;
using GameEngine::Components::Terrain;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::Entity;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Vector3;

using GameEngine::Editor::BuildFenceLayout;
using GameEngine::Editor::CenterlineSampleCount;
using GameEngine::Editor::CenterSample;
using GameEngine::Editor::ConformRayDown;
using GameEngine::Editor::FenceLayoutParams;
using GameEngine::Editor::FenceLayoutResult;
using GameEngine::Editor::FencePieceBounds;

namespace
{

// Terrain sits at y = 0 and the fence is authored on it, so a station's
// altitude IS its height above ground.
constexpr float kGroundY = 0.0f;
// The canopy's underside clears the fence; only its TOP is what a down-ray
// starting above the sample can reach first.
constexpr float kCanopyTopY = 8.0f;
// One span piece, 2 m along the path. Two of the measured fence kits' panels
// differ by 2 mm, which is why the layout solves a fill rather than dividing;
// a single length keeps the arithmetic here readable without changing the path
// under test.
constexpr float kPieceHalfLength = 1.0f;

} // namespace

// Same engine/terrain bring-up as SplineDrapeTests, which is where the shared
// conform ray is exercised; this suite adds the fence layout on top of it.
class FenceConformTargetTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            GameEngine::ScriptsConfig scriptsConfig{};
            scriptsConfig.disableClr = true;
            scriptsConfig.enableHotReload = false;
            scriptsConfig.enableAsyncHotReload = false;
            scriptsConfig.enableAutoProjectGeneration = false;
            engine.SetScriptsConfig(scriptsConfig);

            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        if (!GameEngine::TerrainECS::TerrainService::IsInitialized())
            GameEngine::TerrainECS::TerrainService::Initialize();
    }

    void SetUp() override
    {
        World* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
    }

    World* GetWorld() const { return EngineCore::GetInstance().EnsurePrimaryWorld(); }

    // Flat terrain at y = 0, 20 m square about the origin.
    static void CreateFlatTerrain(World& world)
    {
        auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
        ASSERT_NE(terrainService, nullptr);

        GameEngine::Terrain::TerrainConfig config{};
        config.HeightmapWidth = 9;
        config.HeightmapHeight = 9;
        config.WorldSizeX = 20.0f;
        config.WorldSizeZ = 20.0f;
        config.HeightScale = 1.0f;
        config.LODLevels = 1;
        config.PatchGridSize = 8;

        const auto terrainHandle = terrainService->CreateTerrain(config);
        ASSERT_NE(terrainHandle.Generation, 0u);

        Entity terrainEntity = world.Create();
        Terrain terrain{};
        terrain.SizeX = config.WorldSizeX;
        terrain.SizeZ = config.WorldSizeZ;
        terrain.HeightScale = config.HeightScale;
        terrain.TerrainDataHandle = terrainHandle.Index;
        terrain.TerrainDataGeneration = terrainHandle.Generation;
        terrainEntity.Set(terrain);
        terrainEntity.Set(WorldTransform{});
        world.ProcessCommands();
    }

    // A canopy overhanging the second half of the run: the Party Tree's shape in
    // miniature. It stands clear of the fence line and is reachable only from
    // above, so nothing but a down-ray that accepts scenery can land on it.
    static void CreateCanopyOver(World& world, float centerX, float halfX)
    {
        Entity canopy = world.Create();
        GameEngine::Components::MeshRenderer renderer{};
        canopy.Set(renderer);

        const float halfY = 2.0f;
        GameEngine::Components::LocalBounds bounds{};
        bounds.Box.center = {0.0f, 0.0f, 0.0f};
        bounds.Box.halfExtents = {halfX, halfY, 4.0f};
        canopy.Set(bounds);

        WorldTransform xf{};
        xf.matrix[12] = centerX;
        xf.matrix[13] = kCanopyTopY - halfY;
        xf.matrix[14] = 0.0f;
        canopy.Set(xf);
        world.ProcessCommands();

        using GameEngine::Engine::Renderer::RenderExtractionSystem;
        RenderExtractionSystem::WorldRenderableRecord rec{};
        rec.entity = canopy.GetHandle();
        for (int i = 0; i < 16; ++i)
            rec.worldTransform.matrix[i] = 0.0f;
        rec.worldTransform.matrix[0] = 1.0f;
        rec.worldTransform.matrix[5] = 1.0f;
        rec.worldTransform.matrix[10] = 1.0f;
        rec.worldTransform.matrix[15] = 1.0f;
        rec.worldTransform.matrix[12] = centerX;
        rec.worldTransform.matrix[13] = kCanopyTopY - halfY;
        rec.worldTransform.matrix[14] = 0.0f;
        rec.worldTransform.Version = 1u;
        rec.hasBounds = true;
        rec.bounds.Box.center = {0.0f, 0.0f, 0.0f};
        rec.bounds.Box.halfExtents = {halfX, halfY, 4.0f};

        std::vector<RenderExtractionSystem::WorldRenderableRecord> records{rec};
        GameEngine::Scene::GetSceneTlas(world).SyncFromRecords(records);
    }

    // A straight two-point fence line on the ground: one run, no corners, so
    // anything that moves the stations is the surface and not the layout.
    static GameEngine::Spline::SplineData MakeFenceLine(float x0, float x1)
    {
        GameEngine::Spline::SplineData data;
        data.Type = GameEngine::Spline::SplineType::Linear;
        data.AddPoint(Vector3(x0, kGroundY, 0.0f), 1.0f);
        data.AddPoint(Vector3(x1, kGroundY, 0.0f), 1.0f);
        GameEngine::Spline::RebuildSplineCache(data);
        return data;
    }

    struct BuiltFence
    {
        FenceLayoutResult Layout;
        float MaxStationHeightAboveGround = 0.0f;
        std::size_t SpanCount = 0;
    };

    // Mirrors SplineFenceController::Rebuild's drape + layout for an identity
    // placer transform: dense arc-length samples, one conform ray each at the
    // recipe's target, run boundaries at the authored points, then the same
    // per-station probe the controller installs.
    static BuiltFence BuildFence(World& world, const GameEngine::Spline::SplineData& data,
                                 const SplineFence& recipe)
    {
        const uint32_t denseCount = CenterlineSampleCount(data.TotalArcLength);
        std::vector<GameEngine::Spline::SplineFrame> frames;
        GameEngine::Spline::SampleUniform(data, denseCount, frames);
        EXPECT_GE(frames.size(), 2u);

        const Vector3 worldUp(0.0f, 1.0f, 0.0f);
        std::vector<CenterSample> center;
        center.reserve(frames.size());
        for (const GameEngine::Spline::SplineFrame& frame : frames)
        {
            CenterSample sample;
            sample.Pos = frame.Position;
            sample.Normal = worldUp;
            Vector3 hit;
            Vector3 hitNormal;
            if (ConformRayDown(world, sample.Pos, {}, hit, &hitNormal, nullptr,
                               recipe.ConformTarget))
                sample.Pos.y = hit.y;
            else
                sample.SurfaceValid = false;
            center.push_back(sample);
        }

        const float lastIndex = static_cast<float>(frames.size() - 1u);
        const uint32_t segmentCount = data.GetSegmentCount();
        std::vector<float> runBoundaries;
        for (uint32_t i = 0; i <= segmentCount; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(segmentCount);
            const float distance = GameEngine::Spline::ParametricToDistance(data, t);
            runBoundaries.push_back(
                std::min(std::max(distance * (lastIndex / data.TotalArcLength), 0.0f), lastIndex));
        }

        const FencePieceBounds span{{0.0f, 0.0f, 0.0f}, {kPieceHalfLength, 0.8f, 0.1f}};
        const std::vector<FencePieceBounds> spanPieces{span};

        FenceLayoutParams layout;
        layout.RunBoundaries = std::span<const float>(runBoundaries);
        layout.PostPitch = recipe.PostPitch;
        layout.SpanMaxStretch = recipe.SpanMaxStretch;
        layout.SpanGrade = recipe.SpanGrade;
        layout.PlantMode = recipe.PlantMode;
        layout.SpanPieces = std::span<const FencePieceBounds>(spanPieces);

        const SplineConformTarget target = recipe.ConformTarget;
        layout.Probe = [&world, target](const Vector3& at, float& outAltitude, Vector3& outNormal)
        {
            Vector3 hit;
            Vector3 hitNormal;
            if (!ConformRayDown(world, at, {}, hit, &hitNormal, nullptr, target))
                return false;
            outAltitude = hit.y;
            outNormal = hitNormal;
            return true;
        };

        BuiltFence built;
        built.Layout = BuildFenceLayout(center, layout);
        built.SpanCount = built.Layout.Spans.size();
        for (const GameEngine::Editor::FenceStation& station : built.Layout.Stations)
        {
            built.MaxStationHeightAboveGround =
                std::max(built.MaxStationHeightAboveGround, station.Pose.Base.y - kGroundY);
        }
        return built;
    }
};

// The defect and its cure in one assertion set. Scene mode really does stand the
// run on the canopy, and TerrainOnly really does reach the ground under it —
// either half alone would pass on a target that did nothing.
TEST_F(FenceConformTargetTests, AFenceUnderACanopyStandsOnItUntilTheAuthorSaysTerrainOnly)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateFlatTerrain(*world);
    CreateCanopyOver(*world, 4.0f, 3.0f); // covers x 1..7 of the run

    const GameEngine::Spline::SplineData line = MakeFenceLine(-8.0f, 8.0f);

    SplineFence scene{};
    scene.ConformTarget = SplineConformTarget::Scene;
    const BuiltFence rode = BuildFence(*world, line, scene);

    SplineFence terrainOnly{};
    terrainOnly.ConformTarget = SplineConformTarget::TerrainOnly;
    const BuiltFence hugged = BuildFence(*world, line, terrainOnly);

    ASSERT_GT(rode.SpanCount, 0u) << "the whole-scene fence built nothing at all";
    ASSERT_GT(hugged.SpanCount, 0u) << "the terrain-only fence built nothing at all";

    EXPECT_GT(rode.MaxStationHeightAboveGround, 5.0f)
        << "Scene mode must still stand the run on the canopy — that IS the reported behaviour, "
           "and if it no longer does, the terrain-only assertion below stops proving anything";

    EXPECT_LT(hugged.MaxStationHeightAboveGround, 0.05f)
        << "a terrain-only fence put a station " << hugged.MaxStationHeightAboveGround
        << " m above the ground: the conform target is not reaching the stations";
}

// The fence-specific blast radius: the prop does not merely lift the stations
// under it, it re-counts the run. This is what makes a wrong conform hit on a
// fence a whole-run defect rather than a local one.
TEST_F(FenceConformTargetTests, ACanopyOverheadChangesHowManySpansTheRunBuilds)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateFlatTerrain(*world);
    CreateCanopyOver(*world, 4.0f, 3.0f);

    const GameEngine::Spline::SplineData line = MakeFenceLine(-8.0f, 8.0f);

    SplineFence scene{};
    scene.ConformTarget = SplineConformTarget::Scene;
    SplineFence terrainOnly{};
    terrainOnly.ConformTarget = SplineConformTarget::TerrainOnly;

    const BuiltFence rode = BuildFence(*world, line, scene);
    const BuiltFence hugged = BuildFence(*world, line, terrainOnly);

    // 16 m of ground over 2 m pieces.
    EXPECT_EQ(hugged.SpanCount, 8u)
        << "the terrain-only run no longer fills its 16 m with 2 m pieces, so the count below "
           "is being compared against the wrong baseline";
    EXPECT_GT(rode.SpanCount, hugged.SpanCount)
        << "the climb up and down the canopy did not lengthen the run, so this test is no longer "
           "measuring the amplification it exists to pin";
}

// The control: with nothing overhanging, the two targets must agree exactly.
// Without this, a TerrainOnly that simply ignored the scene would pass the
// tests above while being wrong everywhere else.
TEST_F(FenceConformTargetTests, WithNothingOverheadBothTargetsBuildTheSameFence)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateFlatTerrain(*world);

    const GameEngine::Spline::SplineData line = MakeFenceLine(-8.0f, 8.0f);

    SplineFence scene{};
    scene.ConformTarget = SplineConformTarget::Scene;
    SplineFence terrainOnly{};
    terrainOnly.ConformTarget = SplineConformTarget::TerrainOnly;

    const BuiltFence a = BuildFence(*world, line, scene);
    const BuiltFence b = BuildFence(*world, line, terrainOnly);

    ASSERT_GT(a.SpanCount, 0u);
    EXPECT_EQ(a.SpanCount, b.SpanCount);
    ASSERT_EQ(a.Layout.Stations.size(), b.Layout.Stations.size());
    for (std::size_t i = 0; i < a.Layout.Stations.size(); ++i)
    {
        EXPECT_NEAR(a.Layout.Stations[i].Pose.Base.y, b.Layout.Stations[i].Pose.Base.y, 1.0e-4f)
            << "station " << i << " disagrees between the two targets over bare terrain";
    }
}

// The default is unchanged in every direction: a fence recipe that says nothing
// conforms to the whole scene, exactly as every fence authored before the field
// existed did. Changing this would silently re-place authored content on load.
TEST_F(FenceConformTargetTests, TheDefaultTargetIsStillTheWholeScene)
{
    const SplineFence fresh{};
    EXPECT_EQ(fresh.ConformTarget, SplineConformTarget::Scene);
}
