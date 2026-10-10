#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Placement/CenterlineSampling.h"
#include "Placement/SplineSurfaceConform.h"
#include "SceneView/SplineDrapePolylines.h"
#include "Scripting/ScriptsConfig.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "Scene/SceneTlas.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;

using GameEngine::Components::Terrain;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::Entity;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;

using GameEngine::Editor::ConformRayDown;
using GameEngine::Editor::WorldCenterlineLength;
using GameEngine::Editor::SceneTools::BuildSplineDrapedPolylines;
using GameEngine::Editor::SceneTools::kDrapeMaxSamples;
using GameEngine::Editor::SceneTools::kDrapeSampleStepMetres;
using GameEngine::Editor::SceneTools::kDrapeSurfaceLiftMetres;
using GameEngine::Editor::SceneTools::SplineDrapedPolylines;
using GameEngine::Components::SplineConformTarget;

namespace
{

// The lift is a decision, so pin its value here. Every assertion below states
// the drape RELATION (sample sits one lift above the conform hit) by
// referencing the constant on both sides, which makes those assertions blind
// to the value itself — they pass at any lift. This pin is what makes changing
// it a deliberate edit rather than a silent visual change.
static_assert(kDrapeSurfaceLiftMetres == 0.15f,
              "Drape surface lift changed. It exists to keep the depth-tested "
              "centerline and envelope edges off the ground plane without "
              "detaching them from it; the width band does NOT depend on it "
              "(it draws on the always-on-top overlay pass). Confirm in the "
              "editor, then update this pin.");

// Drape maths against a real conform ray: flat terrain centred at the origin
// at y = 0 (CreateTerrain leaves the heightmap zeroed), same setup as
// SplineToolTests / ScenePickTerrainInclusionTests.
class SplineDrapeTests : public ::testing::Test
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

    World* GetWorld() const
    {
        return EngineCore::GetInstance().EnsurePrimaryWorld();
    }

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

    // Drive every sample of an existing terrain to `normalizedHeight` and mark it dirty —
    // the observable a modifier bake leaves behind (TerrainModifierSystem's full-bake arm
    // ends in MarkFullDirty + RebuildQuadtree).
    static void RebakeTerrainToNormalizedHeight(GameEngine::TerrainECS::TerrainHandle handle,
                                                float normalizedHeight)
    {
        auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
        ASSERT_NE(terrainService, nullptr);
        auto* data = terrainService->GetTerrainData(handle);
        ASSERT_NE(data, nullptr);
        float* samples = data->Heightfield.GetMutableSamples();
        for (std::size_t i = 0; i < data->Heightfield.GetSampleCount(); ++i)
            samples[i] = normalizedHeight;
        data->MarkFullDirty();
        terrainService->RebuildQuadtree(handle);
    }

    // The same terrain, but with every sample driven to `normalizedHeight` and the quadtree
    // refreshed — what a modifier bake leaves behind, which never clamps to [0,1]. A negative
    // value is ground BELOW the terrain entity's origin, exactly what a lowering
    // TerrainHeightOffsetEffect produces.
    static void CreateTerrainAtNormalizedHeight(
        World& world, float normalizedHeight,
        GameEngine::TerrainECS::TerrainHandle* outHandle = nullptr)
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

        auto* data = terrainService->GetTerrainData(terrainHandle);
        ASSERT_NE(data, nullptr);
        float* samples = data->Heightfield.GetMutableSamples();
        for (std::size_t i = 0; i < data->Heightfield.GetSampleCount(); ++i)
            samples[i] = normalizedHeight;
        terrainService->RebuildQuadtree(terrainHandle);

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

        if (outHandle)
            *outHandle = terrainHandle;
    }

    // A straight 2-point Linear spline from (x0, y, 0) to (x1, y, 0) with a
    // 2 m half-width channel.
    static GameEngine::Spline::SplineData MakeStraightSpline(float x0, float x1, float y)
    {
        GameEngine::Spline::SplineData data;
        data.Type = GameEngine::Spline::SplineType::Linear;
        data.AddPoint(Vector3(x0, y, 0.0f), 2.0f);
        data.AddPoint(Vector3(x1, y, 0.0f), 2.0f);
        GameEngine::Spline::RebuildSplineCache(data);
        return data;
    }
};

TEST_F(SplineDrapeTests, CenterlineDrapesToTerrainWithLift)
{
    World* world = GetWorld();
    CreateFlatTerrain(*world);

    const float authoredY = 5.0f;
    const auto data = MakeStraightSpline(-5.0f, 5.0f, authoredY);
    ASSERT_GT(data.TotalArcLength, 0.0f);

    SplineDrapedPolylines out;
    BuildSplineDrapedPolylines(*world, data, Matrix4x4{}, {}, /*wantEdges=*/false, SplineConformTarget::Scene, out);

    ASSERT_GE(out.Center.size(), 2u);
    EXPECT_TRUE(out.Left.empty());
    EXPECT_TRUE(out.Right.empty());
    EXPECT_NEAR(out.Center.front().x, -5.0f, 1.0e-3f);
    EXPECT_NEAR(out.Center.back().x, 5.0f, 1.0e-3f);

    // Each sample must sit exactly one lift above what the shared conform ray
    // reports at that XZ — the same ray the builder casts, so this pins the
    // lift relationship without depending on terrain-picking precision.
    for (const Vector3& pt : out.Center)
    {
        Vector3 hit;
        ASSERT_TRUE(ConformRayDown(*world, Vector3(pt.x, authoredY, pt.z), {}, hit));
        EXPECT_NEAR(pt.y, hit.y + kDrapeSurfaceLiftMetres, 1.0e-4f);
        EXPECT_LT(pt.y, 1.0f); // and it really left the authored altitude
    }
}

TEST_F(SplineDrapeTests, ConformMissKeepsAuthoredAltitude)
{
    World* world = GetWorld();
    CreateFlatTerrain(*world);

    // Far outside the 20x20 terrain: every conform ray misses. All-miss is the
    // case the shared HoldSurfaceAcrossGaps rule leaves untouched — the
    // authored altitudes are the only surface there is — and no lift applies.
    const float authoredY = 5.0f;
    const auto data = MakeStraightSpline(1000.0f, 1010.0f, authoredY);

    SplineDrapedPolylines out;
    BuildSplineDrapedPolylines(*world, data, Matrix4x4{}, {}, /*wantEdges=*/false, SplineConformTarget::Scene, out);

    ASSERT_GE(out.Center.size(), 2u);
    for (const Vector3& pt : out.Center)
        EXPECT_NEAR(pt.y, authoredY, 1.0e-4f);
}

TEST_F(SplineDrapeTests, PartialGapHoldsNearestMeasuredAltitude)
{
    World* world = GetWorld();
    CreateFlatTerrain(*world);

    // Half on, half off the 20x20 terrain: samples past x = 10 miss. The drape
    // must hold the last measured altitude across the gap — the shared
    // HoldSurfaceAcrossGaps rule tile placement runs — not dive back to the
    // authored 5 m while the tiles hold measured ground.
    const float authoredY = 5.0f;
    const auto data = MakeStraightSpline(0.0f, 30.0f, authoredY);

    SplineDrapedPolylines out;
    BuildSplineDrapedPolylines(*world, data, Matrix4x4{}, {}, /*wantEdges=*/false, SplineConformTarget::Scene, out);
    ASSERT_GE(out.Center.size(), 2u);

    size_t hitCount = 0;
    size_t missCount = 0;
    bool haveMeasured = false;
    float lastMeasuredY = 0.0f;
    for (const Vector3& pt : out.Center)
    {
        Vector3 hit;
        if (ConformRayDown(*world, Vector3(pt.x, authoredY, pt.z), {}, hit))
        {
            ++hitCount;
            haveMeasured = true;
            lastMeasuredY = hit.y;
            EXPECT_NEAR(pt.y, hit.y + kDrapeSurfaceLiftMetres, 1.0e-4f) << "hit at x=" << pt.x;
        }
        else
        {
            ++missCount;
            // The spline starts over terrain, so every gap trails a measurement.
            ASSERT_TRUE(haveMeasured);
            EXPECT_NEAR(pt.y, lastMeasuredY + kDrapeSurfaceLiftMetres, 1.0e-4f)
                << "miss at x=" << pt.x << " must hold the measured altitude, not the authored "
                << authoredY;
        }
    }
    // The case only means something if both regimes were exercised.
    ASSERT_GT(hitCount, 0u);
    ASSERT_GT(missCount, 0u);
}

TEST_F(SplineDrapeTests, ScaledEntityScalesWidthAndDensity)
{
    World* world = GetWorld();
    CreateFlatTerrain(*world);

    // Local 10 m at y = 5 with the authored 2 m half-width; a uniform scale-2
    // entity makes that 20 world metres and a 4 m world half-width.
    const auto data = MakeStraightSpline(-5.0f, 5.0f, 5.0f);
    ASSERT_NEAR(data.TotalArcLength, 10.0f, 1.0e-3f);
    const Matrix4x4 worldM(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)));

    SplineDrapedPolylines out;
    BuildSplineDrapedPolylines(*world, data, worldM, {}, /*wantEdges=*/true, SplineConformTarget::Scene, out);

    // Sampling density follows WORLD length: ~20 m at the 1 m step is ~21
    // samples, where sampling the local arc against the world step gives 11.
    const auto expectedSamples = std::clamp<uint32_t>(
        static_cast<uint32_t>(WorldCenterlineLength(data.TotalArcLength, worldM) /
                              kDrapeSampleStepMetres) +
            1u,
        2u, kDrapeMaxSamples);
    EXPECT_EQ(out.Center.size(), expectedSamples);
    EXPECT_GE(out.Center.size(), 20u);
    ASSERT_EQ(out.Left.size(), out.Center.size());
    ASSERT_EQ(out.Right.size(), out.Center.size());

    // Travelling +X, right-of-travel is -Z; the local 2 m half-width channel
    // scales with the entity, so the edges sit at z = -/+ 4 world metres.
    for (size_t i = 0; i < out.Center.size(); ++i)
    {
        EXPECT_NEAR(out.Right[i].z, -4.0f, 1.0e-3f) << "sample " << i;
        EXPECT_NEAR(out.Left[i].z, 4.0f, 1.0e-3f) << "sample " << i;
    }
}

TEST_F(SplineDrapeTests, EdgesBuildOnlyWhenRequested)
{
    World* world = GetWorld();
    CreateFlatTerrain(*world);

    const auto data = MakeStraightSpline(-5.0f, 5.0f, 5.0f);

    SplineDrapedPolylines out;
    BuildSplineDrapedPolylines(*world, data, Matrix4x4{}, {}, /*wantEdges=*/true, SplineConformTarget::Scene, out);

    ASSERT_GE(out.Center.size(), 2u);
    ASSERT_EQ(out.Left.size(), out.Center.size());
    ASSERT_EQ(out.Right.size(), out.Center.size());

    // Travelling +X in this LH +Y-up engine, right-of-travel is -Z: the Right
    // edge offsets to z = -2, Left to z = +2 (the authored half-width).
    for (size_t i = 0; i < out.Center.size(); ++i)
    {
        EXPECT_NEAR(out.Right[i].z, -2.0f, 1.0e-3f);
        EXPECT_NEAR(out.Left[i].z, 2.0f, 1.0e-3f);
        // Both edges drape to the flat terrain like the center does.
        EXPECT_NEAR(out.Right[i].y, out.Center[i].y, 1.0e-3f);
        EXPECT_NEAR(out.Left[i].y, out.Center[i].y, 1.0e-3f);
    }
}

TEST_F(SplineDrapeTests, EdgeAllMissKeepsAuthoredAltitudeWhileCenterDrapes)
{
    World* world = GetWorld();
    CreateFlatTerrain(*world);

    // Centerline at z = 9 with a 4 m half-width: travelling +X, right-of-travel
    // is -Z, so the Right edge (z = 5) stays on the 20x20 terrain while the
    // Left edge (z = 13) hangs 3 m past it and every left conform ray misses.
    const float authoredY = 5.0f;
    GameEngine::Spline::SplineData data;
    data.Type = GameEngine::Spline::SplineType::Linear;
    data.AddPoint(Vector3(-5.0f, authoredY, 9.0f), 4.0f);
    data.AddPoint(Vector3(5.0f, authoredY, 9.0f), 4.0f);
    GameEngine::Spline::RebuildSplineCache(data);

    SplineDrapedPolylines out;
    BuildSplineDrapedPolylines(*world, data, Matrix4x4{}, {}, /*wantEdges=*/true, SplineConformTarget::Scene, out);

    ASSERT_GE(out.Center.size(), 2u);
    ASSERT_EQ(out.Left.size(), out.Center.size());
    ASSERT_EQ(out.Right.size(), out.Center.size());
    for (size_t i = 0; i < out.Center.size(); ++i)
    {
        // The measured polylines drape with the lift; the all-miss Left
        // polyline keeps its authored altitude and takes no lift. The miss
        // rule is per polyline — a shared "any measured" would invent ground
        // under an edge that measured none.
        EXPECT_LT(out.Center[i].y, 1.0f);
        EXPECT_LT(out.Right[i].y, 1.0f);
        EXPECT_NEAR(out.Left[i].y, authoredY, 1.0e-4f);
    }
}

TEST_F(SplineDrapeTests, WorldMatrixAppliesBeforeDrape)
{
    World* world = GetWorld();
    CreateFlatTerrain(*world);

    // Authored at y = 5 in entity space; the entity sits 2 m along +X. The
    // drape must transform first, then conform (still over the terrain).
    const auto data = MakeStraightSpline(-5.0f, 5.0f, 5.0f);
    const Matrix4x4 worldM = Matrix4x4::Translation(Vector3(2.0f, 0.0f, 0.0f));

    SplineDrapedPolylines out;
    BuildSplineDrapedPolylines(*world, data, worldM, {}, /*wantEdges=*/false, SplineConformTarget::Scene, out);

    ASSERT_GE(out.Center.size(), 2u);
    EXPECT_NEAR(out.Center.front().x, -3.0f, 1.0e-3f);
    EXPECT_NEAR(out.Center.back().x, 7.0f, 1.0e-3f);
    for (const Vector3& pt : out.Center)
        EXPECT_LT(pt.y, 1.0f); // draped, not floating at the authored 5 m
}

// ---- Conform over ground a modifier volume pushed outside the nominal band ----

// The reported defect, end to end at the conform layer: a lowering volume puts the ground two
// metres under the terrain entity's origin, and the conform ray must still find it. Pre-fix
// the pick's march slab stopped one metre below the origin, the cast missed, and the sample
// kept its authored altitude — a fence piece hanging in the air over visible ground.
TEST_F(SplineDrapeTests, ConformFindsGroundLoweredBelowTheTerrainOrigin)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateTerrainAtNormalizedHeight(*world, -2.0f); // HeightScale 1 -> ground at y = -2

    GameEngine::Editor::ConformMissTally tally;
    Vector3 hit;
    const Vector3 sample(0.0f, 5.0f, 0.0f); // authored well above the lowered ground
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, hit, nullptr, &tally))
        << "conform ray found no ground under a lowering volume";
    EXPECT_NEAR(hit.y, -2.0f, 0.05f);

    EXPECT_EQ(tally.Probes, 1u);
    EXPECT_EQ(tally.Misses, 0u);
}

// ---- The ground moving under finished pieces ----

// The defect on scene open: pieces conform once, against ground the
// modifier bake has not shaped yet, and nothing re-places them when it does. Both
// placement controllers gate their rebuild on the spline data, the transform and the
// recipe — none of which move when the terrain does — so the digest is what has to
// carry it. Assert the two halves TOGETHER: the ground the conform ray reports really
// moved, and the digest moved with it. Either half alone proves nothing.
TEST_F(SplineDrapeTests, ConformSurfaceRevisionMovesWhenABakeMovesTheGround)
{
    using GameEngine::Editor::ConformSurfaceRevision;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    GameEngine::TerrainECS::TerrainHandle handle{};
    CreateTerrainAtNormalizedHeight(*world, 0.0f, &handle);
    ASSERT_NE(handle.Generation, 0u);

    const Vector3 sample(0.0f, 5.0f, 0.0f);
    Vector3 before;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, before));
    const uint64_t revisionBefore = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);

    // What a lowering TerrainHeightOffsetEffect leaves behind on its first bake.
    RebakeTerrainToNormalizedHeight(handle, -2.0f);

    Vector3 after;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, after));
    EXPECT_NEAR(before.y, 0.0f, 0.05f);
    EXPECT_NEAR(after.y, -2.0f, 0.05f);
    ASSERT_GT(std::abs(before.y - after.y), 1.0f) << "the ground did not move; the case is void";

    EXPECT_NE(ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr), revisionBefore)
        << "ground moved by 2 m and the surface revision did not: a conformed piece would "
           "keep its pre-bake altitude with nothing to schedule a re-place";
}

// The cost side of the same mechanism, and the reason a plain frame counter or a clock
// would be wrong here: a digest that churns on quiet frames re-places every path in the
// scene once per settle window, forever.
TEST_F(SplineDrapeTests, ConformSurfaceRevisionIsQuiescentWhenNothingMoves)
{
    using GameEngine::Editor::ConformSurfaceRevision;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateTerrainAtNormalizedHeight(*world, 0.25f);

    const uint64_t first = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);
    // Casting rays is what a rebuild does; reading the ground must not move the digest.
    Vector3 hit;
    ASSERT_TRUE(ConformRayDown(*world, Vector3(0.0f, 5.0f, 0.0f), {}, hit));
    EXPECT_EQ(ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr), first);
    EXPECT_EQ(ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr), first);
}

// The heightfield version cannot carry this alone. A terrain with NO modifiers never
// bakes — the empty-modifier list pins the change hash to its 0 sentinel and
// TerrainModifierSystem::Update returns before touching the field — so its version
// stands still while moving the terrain entity moves every hit the pick reports:
// TerrainPicking derives the footprint origin from the translation and Size, and the
// altitude from originY and HeightScale. Same conjunction as the bake case: the ray's
// answer really moved, and the digest moved with it.
TEST_F(SplineDrapeTests, ConformSurfaceRevisionMovesWhenAModifierFreeTerrainIsMoved)
{
    using GameEngine::Editor::ConformSurfaceRevision;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    // Ground at y = 0.5 (HeightScale 1), clear of the entity origin so a vertical
    // move of the entity shows up in the hit altitude.
    CreateTerrainAtNormalizedHeight(*world, 0.5f);

    GameEngine::ECS::EntityHandle terrainEntity{};
    world->Query<GameEngine::ECS::Read<Terrain>, GameEngine::ECS::Read<WorldTransform>>().Each(
        [&](GameEngine::ECS::EntityHandle e, const Terrain&, const WorldTransform&)
        { terrainEntity = e; });
    ASSERT_TRUE(world->IsValid(terrainEntity));

    const Vector3 sample(0.0f, 20.0f, 0.0f);
    Vector3 before;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, before));
    const uint64_t revisionBefore = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);

    // Move the terrain entity 5 m up: not one heightfield sample changes, no bake
    // runs, and the ground under the sample is 5 m above what the pieces measured.
    auto* xf = world->GetComponentForWrite<WorldTransform>(terrainEntity);
    ASSERT_NE(xf, nullptr);
    xf->matrix[13] += 5.0f;

    Vector3 after;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, after));
    ASSERT_NEAR(after.y - before.y, 5.0f, 0.05f)
        << "the ground did not move with the entity; the case is void";

    EXPECT_NE(ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr), revisionBefore)
        << "a modifier-free terrain moved 5 m and the surface revision did not: its "
           "heightfield version never moves, so nothing would re-place the pieces";
}

// Identity, not just version: a terrain destroyed and re-created restarts its
// heightfield version at 0, and a second terrain arriving is new ground under any
// piece above it. Both must move the digest even though no existing version advanced.
TEST_F(SplineDrapeTests, ConformSurfaceRevisionTracksTerrainIdentity)
{
    using GameEngine::Editor::ConformSurfaceRevision;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    const uint64_t empty = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);
    CreateTerrainAtNormalizedHeight(*world, 0.0f);
    const uint64_t oneTerrain = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);
    EXPECT_NE(oneTerrain, empty) << "a terrain appearing is ground appearing";

    CreateTerrainAtNormalizedHeight(*world, 0.5f);
    EXPECT_NE(ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr), oneTerrain)
        << "a second terrain must move the digest";
}

// The tally is the diagnostic's whole substance, so pin its semantics directly: it counts
// every probe, counts only the misses, and keeps the FIRST missing sample as the witness
// (later misses must not overwrite it — the first is the one a reader navigates to).
TEST(ConformMissTallyTest, CountsProbesAndKeepsTheFirstMissAsWitness)
{
    GameEngine::Editor::ConformMissTally tally;
    tally.Record(true, Vector3(1.0f, 0.0f, 0.0f));
    tally.Record(false, Vector3(2.0f, 3.0f, 4.0f));
    tally.Record(false, Vector3(9.0f, 9.0f, 9.0f));
    tally.Record(true, Vector3(5.0f, 0.0f, 0.0f));

    EXPECT_EQ(tally.Probes, 4u);
    EXPECT_EQ(tally.Misses, 2u);
    EXPECT_NEAR(tally.FirstMiss.x, 2.0f, 1.0e-4f);
    EXPECT_NEAR(tally.FirstMiss.y, 3.0f, 1.0e-4f);
    EXPECT_NEAR(tally.FirstMiss.z, 4.0f, 1.0e-4f);
}

// The report is per-rebuild, and placement rebuilds can run every frame during a drag, so it
// must fire on a CHANGED miss count and stay quiet otherwise. The observable is the caller's
// dedupe state: it advances exactly when a report is emitted.
TEST(ConformMissTallyTest, ReportFiresOnChangedCountAndStaysQuietOtherwise)
{
    using GameEngine::Editor::ReportConformMisses;

    GameEngine::Editor::ConformMissTally clean;
    clean.Record(true, Vector3(0.0f, 0.0f, 0.0f));
    uint32_t lastReported = 0u;

    // Nothing missed: nothing to say, and the state stays put.
    ReportConformMisses(clean, /*entityId*/ 7u, lastReported);
    EXPECT_EQ(lastReported, 0u);

    GameEngine::Editor::ConformMissTally missed;
    missed.Record(false, Vector3(1.0f, 2.0f, 3.0f));
    ReportConformMisses(missed, 7u, lastReported);
    EXPECT_EQ(lastReported, 1u) << "a first miss must report";

    // Same count again (the steady state of a rebuilding scene): already reported.
    ReportConformMisses(missed, 7u, lastReported);
    EXPECT_EQ(lastReported, 1u);

    // The count moving is new information and reports again.
    missed.Record(false, Vector3(4.0f, 5.0f, 6.0f));
    ReportConformMisses(missed, 7u, lastReported);
    EXPECT_EQ(lastReported, 2u);

    // Recovering to zero clears the state so a later regression reports afresh.
    GameEngine::Editor::ConformMissTally recovered;
    recovered.Record(true, Vector3(0.0f, 0.0f, 0.0f));
    ReportConformMisses(recovered, 7u, lastReported);
    EXPECT_EQ(lastReported, 0u);
}

} // namespace

// ---- What the conform ray is allowed to land on ----

// Registers `entity` with the scene TLAS as a renderable so the picker's broad
// phase can reach it. Sync() only refits leaves it already knows about — spawns
// arrive through SyncFromRecords, which the system tick normally supplies.
namespace
{

void RegisterPickable(World& world, GameEngine::ECS::EntityHandle entity,
                      const Vector3& worldPos, const Vector3& halfExtents)
{
    using GameEngine::Engine::Renderer::RenderExtractionSystem;
    RenderExtractionSystem::WorldRenderableRecord rec{};
    rec.entity = entity;
    for (int i = 0; i < 16; ++i)
        rec.worldTransform.matrix[i] = 0.0f;
    rec.worldTransform.matrix[0] = 1.0f;
    rec.worldTransform.matrix[5] = 1.0f;
    rec.worldTransform.matrix[10] = 1.0f;
    rec.worldTransform.matrix[15] = 1.0f;
    rec.worldTransform.matrix[12] = worldPos.x;
    rec.worldTransform.matrix[13] = worldPos.y;
    rec.worldTransform.matrix[14] = worldPos.z;
    rec.worldTransform.Version = 1u;
    rec.hasBounds = true;
    rec.bounds.Box.center = {0.0f, 0.0f, 0.0f};
    rec.bounds.Box.halfExtents = halfExtents;

    std::vector<RenderExtractionSystem::WorldRenderableRecord> records{rec};
    GameEngine::Scene::GetSceneTlas(world).SyncFromRecords(records);
}

// A prop standing on the flat terrain, where a placed object would otherwise land
// on top of another object such as a cart instead of on the terrain. It
// carries no CPU mesh, so the picker answers for it through its bounds — which
// is also the path that returns BEFORE PickOptions' mesh/primitive filters are
// consulted, so it is the one a half-done terrain-only mode would leak through.
GameEngine::ECS::EntityHandle CreateCartOnTheGround(World& world, float halfHeight)
{
    Entity cart = world.Create();
    GameEngine::Components::MeshRenderer renderer{};
    cart.Set(renderer);
    GameEngine::Components::LocalBounds bounds{};
    bounds.Box.center = {0.0f, 0.0f, 0.0f};
    bounds.Box.halfExtents = {1.0f, halfHeight, 1.0f};
    cart.Set(bounds);

    WorldTransform xf{};
    xf.matrix[12] = 0.0f;
    xf.matrix[13] = halfHeight; // sitting ON the ground, top at 2*halfHeight
    xf.matrix[14] = 0.0f;
    cart.Set(xf);
    world.ProcessCommands();

    RegisterPickable(world, cart.GetHandle(), Vector3(0.0f, halfHeight, 0.0f),
                     Vector3(1.0f, halfHeight, 1.0f));
    return cart.GetHandle();
}

} // namespace

// The defect, at the conform layer: with the default whole-scene target a
// station standing under a prop takes the PROP's surface for ground and the
// piece is placed on its roof. Both halves are asserted together — Scene mode
// really does climb the cart, and TerrainOnly really does reach the ground —
// because either alone would pass on a mode that did nothing.
TEST_F(SplineDrapeTests, ConformTargetDecidesWhetherAPropCountsAsGround)
{
    using GameEngine::Components::SplineConformTarget;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateFlatTerrain(*world); // ground at y = 0
    constexpr float kHalfHeight = 1.0f;
    CreateCartOnTheGround(*world, kHalfHeight);

    const Vector3 sample(0.0f, 5.0f, 0.0f); // authored above both

    Vector3 sceneHit;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, sceneHit, nullptr, nullptr,
                               SplineConformTarget::Scene))
        << "the whole-scene conform found nothing at all";
    EXPECT_NEAR(sceneHit.y, kHalfHeight * 2.0f, 0.05f)
        << "Scene mode must land on the cart's roof — that IS the reported behaviour, and if it "
           "no longer does, the TerrainOnly assertion below stops proving anything";

    Vector3 terrainHit;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, terrainHit, nullptr, nullptr,
                               SplineConformTarget::TerrainOnly))
        << "terrain-only conform found no ground";
    EXPECT_NEAR(terrainHit.y, 0.0f, 0.05f)
        << "TerrainOnly must pass through the cart to the terrain under it";

    EXPECT_GT(sceneHit.y, terrainHit.y + 0.5f)
        << "the two modes returned the same surface, so the target is not reaching the picker";
}

// The drape takes its caller's target: a mark-up region's walls (TerrainOnly) stand on the
// ground under a prop, where the spline gizmo's centerline (Scene) rides on its roof.
TEST_F(SplineDrapeTests, DrapeConformsToTheTargetItIsGiven)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateFlatTerrain(*world); // ground at y = 0
    constexpr float kHalfHeight = 1.0f;
    CreateCartOnTheGround(*world, kHalfHeight);
    const auto data = MakeStraightSpline(-4.0f, 4.0f, 5.0f);

    const auto heightOverTheCart = [&](SplineConformTarget target) {
        SplineDrapedPolylines out;
        BuildSplineDrapedPolylines(*world, data, Matrix4x4{}, {}, /*wantEdges=*/false, target, out);
        const auto middle = std::min_element(out.Center.begin(), out.Center.end(),
                                             [](const Vector3& a, const Vector3& b) {
                                                 return std::abs(a.x) < std::abs(b.x);
                                             });
        return middle == out.Center.end() ? -1.0f : middle->y;
    };
    EXPECT_NEAR(heightOverTheCart(SplineConformTarget::Scene), kHalfHeight * 2.0f + kDrapeSurfaceLiftMetres, 0.05f);
    EXPECT_NEAR(heightOverTheCart(SplineConformTarget::TerrainOnly), kDrapeSurfaceLiftMetres, 0.05f);
}

// The default must remain whole-scene: an omitted argument has to behave
// exactly like an explicit Scene, or every recipe authored before the knob
// existed re-places itself on first load.
TEST_F(SplineDrapeTests, ConformTargetDefaultsToWholeScene)
{
    using GameEngine::Components::SplineConformTarget;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateFlatTerrain(*world);
    constexpr float kHalfHeight = 1.0f;
    CreateCartOnTheGround(*world, kHalfHeight);

    const Vector3 sample(0.0f, 5.0f, 0.0f);

    Vector3 defaulted;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, defaulted));
    Vector3 explicitScene;
    ASSERT_TRUE(ConformRayDown(*world, sample, {}, explicitScene, nullptr, nullptr,
                               SplineConformTarget::Scene));
    EXPECT_FLOAT_EQ(defaulted.y, explicitScene.y);
    EXPECT_NEAR(defaulted.y, kHalfHeight * 2.0f, 0.05f);
}

// Terrain-only conform still reports a genuine miss rather than silently
// succeeding: with the terrain removed there is nothing left it may hit, even
// though the cart is still standing there.
TEST_F(SplineDrapeTests, TerrainOnlyConformMissesWhenOnlyPropsAreUnderTheSample)
{
    using GameEngine::Components::SplineConformTarget;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    constexpr float kHalfHeight = 1.0f;
    CreateCartOnTheGround(*world, kHalfHeight); // no terrain in this world

    GameEngine::Editor::ConformMissTally tally;
    Vector3 hit;
    const Vector3 sample(0.0f, 5.0f, 0.0f);

    EXPECT_TRUE(ConformRayDown(*world, sample, {}, hit, nullptr, nullptr,
                               SplineConformTarget::Scene))
        << "positive control: the cart is pickable in Scene mode";

    EXPECT_FALSE(ConformRayDown(*world, sample, {}, hit, nullptr, &tally,
                                SplineConformTarget::TerrainOnly))
        << "terrain-only conform accepted a prop as ground";
    EXPECT_EQ(tally.Probes, 1u);
    EXPECT_EQ(tally.Misses, 1u);
}

// ---------------------------------------------------------------------------
// Surface READINESS over a live world.
//
// ConformSurfaceRevision answers "has the ground moved". It cannot answer "is
// the ground here yet": an unprovisioned terrain resolves no handle and folds no
// terms, so its digest is indistinguishable from a settled terrain's. That
// conflation is what let a recipe's first build commit against the props while
// the real ground was still arriving from the scene loader's resolve pump.
// ---------------------------------------------------------------------------

// The state a scene sits in for as long as its terrain asset takes to resolve:
// the Terrain entity exists and is enabled, its data handle names nothing yet.
// This is the window the gate exists for, so it is the one that must be legible.
TEST_F(SplineDrapeTests, ConformSurfaceStateIsProvisioningWhileTerrainDataIsUnresolved)
{
    using GameEngine::Editor::ConformSurfaceReadiness;
    using GameEngine::Editor::ConformSurfaceState;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    // A terrain entity exactly as the scene loader instantiates it, BEFORE
    // extraction provisions any data for it: enabled, sized, handles null.
    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = 20.0f;
    terrain.SizeZ = 20.0f;
    terrain.HeightScale = 1.0f;
    terrain.TerrainDataHandle = 0;
    terrain.TerrainDataGeneration = 0;
    terrain.TiledTerrainHandle = 0;
    terrain.TiledTerrainGeneration = 0;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    EXPECT_EQ(ConformSurfaceState(*world), ConformSurfaceReadiness::Provisioning)
        << "a declared terrain with no resolvable data must read as not-here-yet";
}

// The constraint that keeps the gate from becoming a blanket delay: a scene with
// no terrain has nothing coming, and its conforming recipes must place at once.
TEST_F(SplineDrapeTests, ConformSurfaceStateIsNoSurfaceWhenTheWorldHasNoTerrain)
{
    using GameEngine::Editor::ConformSurfaceReadiness;
    using GameEngine::Editor::ConformSurfaceState;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    EXPECT_EQ(ConformSurfaceState(*world), ConformSurfaceReadiness::NoSurface);
}

// The release edge, over a terrain that really is marchable.
TEST_F(SplineDrapeTests, ConformSurfaceStateIsReadyOnceTerrainDataResolves)
{
    using GameEngine::Editor::ConformSurfaceReadiness;
    using GameEngine::Editor::ConformSurfaceState;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);
    CreateFlatTerrain(*world);

    ASSERT_EQ(ConformSurfaceState(*world), ConformSurfaceReadiness::Ready);

    // Positive control: "Ready" has to mean a ray can actually land on it, not
    // merely that a handle resolved.
    Vector3 hit;
    EXPECT_TRUE(ConformRayDown(*world, Vector3(0.0f, 5.0f, 0.0f), {}, hit))
        << "the state says Ready, so the ground must be there to hit";
}

// WHY readiness is a separate question and not a finer digest.
//
// The digest is a CHANGE DETECTOR: it carries information only in the
// DIFFERENCE between two frames' values. It does move across the provision edge
// (the heightfield version joins the fold), so this is not a claim that the two
// states hash alike. The claim is that a difference is unusable by the build
// that matters — a recipe's FIRST build has no previous value to difference
// against, so from a single frame the digest cannot say whether the ground is
// there. Readiness answers that from one frame, which is the whole addition.
TEST_F(SplineDrapeTests, ReadinessAnswersFromOneFrameWhereTheDigestNeedsTwo)
{
    using GameEngine::Editor::ConformSurfaceReadiness;
    using GameEngine::Editor::ConformSurfaceRevision;
    using GameEngine::Editor::ConformSurfaceState;

    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    // A declared terrain with nothing provisioned behind it.
    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = 20.0f;
    terrain.SizeZ = 20.0f;
    terrain.HeightScale = 1.0f;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    const uint64_t digestUnprovisioned = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);
    ASSERT_EQ(ConformSurfaceState(*world), ConformSurfaceReadiness::Provisioning);

    // A ray confirms what readiness said: there is nothing under the sample.
    Vector3 hit;
    EXPECT_FALSE(ConformRayDown(*world, Vector3(0.0f, 5.0f, 0.0f), {}, hit))
        << "the state says Provisioning, so no ground should be hittable yet";

    // Now provision one, exactly as extraction would, and re-ask both.
    world->Clear();
    CreateFlatTerrain(*world);

    const uint64_t digestProvisioned = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);
    EXPECT_EQ(ConformSurfaceState(*world), ConformSurfaceReadiness::Ready);
    EXPECT_TRUE(ConformRayDown(*world, Vector3(0.0f, 5.0f, 0.0f), {}, hit))
        << "the state says Ready, so the ground must be there to hit";

    // Both digests are just numbers. Neither one, read alone, says which side of
    // the provision edge it came from — that is precisely the gap readiness
    // fills, and the reason the fix is not "compare digests harder".
    EXPECT_NE(digestUnprovisioned, digestProvisioned)
        << "sanity: provisioning does move the digest, so this is a claim about "
           "what a SINGLE reading can tell you, not about a hash collision";
}
