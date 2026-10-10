// The water fill against a real terrain, through the editor-side entry point.
//
// The module tests cover the field and the mesh on synthetic grids; what can
// only be checked here is the wiring: that the ROI lands on the terrain's own
// lattice, that the recipe's fields reach the fill, and — the contract this
// slice exists to establish — that EndTaperMetres is IGNORED in this mode.
//
// The SINGLE-terrain arm fills its heightfield DIRECTLY rather than baking it
// from modifier volumes, and carries no procedural base, so the bed is exactly
// the shape those tests write. The TILED arm cannot: a tiled terrain's ground is
// composed from the modifier stack rather than read out of the tile store, so it
// authors its bed as volumes — see TiledFillProbe.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <span>
#include <vector>

#include "Placement/SplineFillRebuild.h"
#include "Placement/SplineSurfaceConform.h"

#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Scene/SceneTlas.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;

using GameEngine::Editor::BuildWaterFillRun;
using GameEngine::Editor::WaterFillOutcome;
using GameEngine::Editor::WaterFillRun;

namespace Components = GameEngine::Components;
namespace SG = GameEngine::SplineGeometry;

namespace
{

constexpr float kTerrainSize = 40.0f;
// One world metre per normalized unit, so a heightfield sample IS its altitude.
constexpr float kHeightScale = 1.0f;
constexpr float kSamplesPerMeter = 2.0f;

class SplineFillRebuildTests : public ::testing::Test
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
    }

    void TearDown() override
    {
        GameEngine::Scene::ReleaseSceneTlas(m_World);
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }

    template <typename BedFn>
    void CreateBedTerrain(float terrainSize, BedFn&& bed)
    {
        const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
            terrainSize, terrainSize, kHeightScale, kSamplesPerMeter);
        const GameEngine::TerrainECS::TerrainHandle handle =
            TerrainService::Get().CreateTerrain(config);
        ASSERT_NE(handle.Generation, 0u);

        auto* data = TerrainService::Get().GetTerrainData(handle);
        ASSERT_NE(data, nullptr);

        const uint32_t w = data->Heightfield.GetWidth();
        const uint32_t h = data->Heightfield.GetHeight();
        const float originX = -terrainSize * 0.5f;
        const float originZ = -terrainSize * 0.5f;
        for (uint32_t zi = 0; zi < h; ++zi)
        {
            const float v = static_cast<float>(zi) / static_cast<float>(h - 1u);
            const float worldZ = originZ + v * terrainSize;
            for (uint32_t xi = 0; xi < w; ++xi)
            {
                const float u = static_cast<float>(xi) / static_cast<float>(w - 1u);
                const float worldX = originX + u * terrainSize;
                data->Heightfield.SetSample(xi, zi, bed(worldX, worldZ));
            }
        }
        TerrainService::Get().RebuildQuadtree(handle);

        Components::Terrain terrain{};
        terrain.SizeX = terrainSize;
        terrain.SizeZ = terrainSize;
        terrain.HeightScale = kHeightScale;
        terrain.Domain = Components::TerrainDomain::Planar;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;

        m_Terrain = m_World.CreateEntity();
        m_World.AddComponentImmediate(m_Terrain, terrain);
        m_World.AddComponentImmediate(m_Terrain, Components::WorldTransform{});
    }

    // A V-channel running along Z: floor at -1, banks rising at 1:1 from |x| = 2.
    void CreateChannel()
    {
        CreateBedTerrain(kTerrainSize, [](float x, float)
        {
            const float across = std::abs(x) - 2.0f;
            return across <= 0.0f ? -1.0f : -1.0f + across;
        });
    }

    // A straight run down the channel at the given waterline altitude.
    static std::vector<SG::SplineStripStation> Run(float waterlineY)
    {
        std::vector<SG::SplineStripStation> stations;
        float distance = 0.0f;
        for (float z = -15.0f; z <= 15.0f; z += 0.5f)
        {
            SG::SplineStripStation station;
            station.Position = Vector3(0.0f, waterlineY, z);
            station.Forward = Vector3(0.0f, 0.0f, 1.0f);
            station.Right = Vector3(1.0f, 0.0f, 0.0f);
            station.Up = Vector3(0.0f, 1.0f, 0.0f);
            station.Distance = distance;
            stations.push_back(station);
            distance += 0.5f;
        }
        return stations;
    }

    static void Identity(GameEngine::float32 (&matrix)[16])
    {
        std::memset(matrix, 0, sizeof(matrix));
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    }

    WaterFillRun Build(const Components::SplineExtrude& recipe, float waterlineY = 0.0f)
    {
        GameEngine::float32 matrix[16];
        Identity(matrix);
        const std::vector<SG::SplineStripStation> stations = Run(waterlineY);
        return BuildWaterFillRun(m_World, stations, recipe, matrix, &m_ModifierSystem);
    }

    // Every vertex and index of every chunk, as bytes.
    static std::vector<unsigned char> Bytes(const WaterFillRun& run)
    {
        std::vector<unsigned char> bytes;
        const auto append = [&](const void* data, size_t size)
        {
            const auto* p = static_cast<const unsigned char*>(data);
            bytes.insert(bytes.end(), p, p + size);
        };
        for (const SG::SplineFillMesh& mesh : run.Chunks)
        {
            for (const SG::SplineVertex& v : mesh.Vertices)
                append(&v, sizeof(v));
            for (uint32_t i : mesh.Indices)
                append(&i, sizeof(i));
            append(&mesh.MinBounds, sizeof(mesh.MinBounds));
            append(&mesh.MaxBounds, sizeof(mesh.MaxBounds));
        }
        return bytes;
    }

    static Components::SplineExtrude WaterRecipe()
    {
        Components::SplineExtrude recipe{};
        recipe.WidthMode = Components::SplineExtrudeWidthMode::FitToBanks;
        recipe.MaxHalfWidth = 12.0f;
        recipe.EdgeDrop = 0.1f;
        return recipe;
    }

    World m_World;
    GameEngine::ECS::EntityHandle m_Terrain{};
    // The fill's ground source for a tiled terrain. Idle for the single-terrain
    // tests, which read their heightfield directly.
    GameEngine::TerrainECS::TerrainModifierSystem m_ModifierSystem;
};

} // namespace

TEST_F(SplineFillRebuildTests, ACarvedChannelFillsAgainstTheRealTerrain)
{
    CreateChannel();
    const WaterFillRun run = Build(WaterRecipe());

    EXPECT_EQ(run.Outcome, WaterFillOutcome::Built);
    EXPECT_GT(run.Diagnostics.WetCorners, 0u);
    EXPECT_EQ(run.Diagnostics.DrySeeds, 0u);

    bool anyGeometry = false;
    for (const SG::SplineFillMesh& mesh : run.Chunks)
        anyGeometry |= mesh.IsValid();
    EXPECT_TRUE(anyGeometry);

    // Waterline 0 over a 1:1 bank from |x| = 2 puts the shoreline near |x| = 3,
    // and the EdgeDrop tuck carries it a little past that. Nothing like the
    // 12 m reach: the GROUND stopped the water, so nothing was refused for
    // standing over a bank and nothing went looking past the corridor for one.
    EXPECT_EQ(run.Diagnostics.OverBankCorners, 0u);
    EXPECT_EQ(run.Diagnostics.UnseenBankCorners, 0u);
    for (const SG::SplineFillMesh& mesh : run.Chunks)
        for (const SG::SplineVertex& v : mesh.Vertices)
            EXPECT_LE(std::abs(v.Position.x), 3.3f);
}

// The contract this slice establishes. A region's end is already the shape the
// ground gives it, so pinching it would reintroduce the mismatch the fill
// removes — and "ignored" has to mean the bytes do not move.
TEST_F(SplineFillRebuildTests, EndTaperMetresIsIgnoredInFitToBanks)
{
    CreateChannel();

    Components::SplineExtrude untapered = WaterRecipe();
    untapered.EndTaperMetres = 0.0f;
    Components::SplineExtrude tapered = WaterRecipe();
    tapered.EndTaperMetres = 6.0f;

    const WaterFillRun a = Build(untapered);
    const WaterFillRun b = Build(tapered);
    ASSERT_EQ(a.Outcome, WaterFillOutcome::Built);
    ASSERT_EQ(b.Outcome, WaterFillOutcome::Built);

    EXPECT_EQ(Bytes(a), Bytes(b))
        << "EndTaperMetres moved the filled region; it is Channel-only by design";
}

// The positive control for the test above: the byte comparison must be able to
// FAIL, or "no change" proves nothing. A sea-level floor over this run raises
// the waterline and must move the surface.
TEST_F(SplineFillRebuildTests, TheSeaLevelFloorDoesChangeTheFilledSurface)
{
    CreateChannel();

    Components::SplineExtrude plain = WaterRecipe();
    Components::SplineExtrude floored = WaterRecipe();
    floored.SeaLevelFloor = 0.5f; // above the run's own waterline of 0

    const WaterFillRun a = Build(plain);
    const WaterFillRun b = Build(floored);
    ASSERT_EQ(a.Outcome, WaterFillOutcome::Built);
    ASSERT_EQ(b.Outcome, WaterFillOutcome::Built);

    EXPECT_NE(Bytes(a), Bytes(b))
        << "the floor did nothing, so the byte comparison above is not a real instrument";
    EXPECT_GT(b.Diagnostics.WetCorners, a.Diagnostics.WetCorners)
        << "a higher waterline must wet more of a V-channel";
}

TEST_F(SplineFillRebuildTests, TheDefaultSeaLevelFloorIsInert)
{
    CreateChannel();

    Components::SplineExtrude defaulted = WaterRecipe();
    Components::SplineExtrude explicitlyOff = WaterRecipe();
    explicitlyOff.SeaLevelFloor = Components::kNoSeaLevelFloor;

    EXPECT_EQ(Bytes(Build(defaulted)), Bytes(Build(explicitlyOff)));
}

TEST_F(SplineFillRebuildTests, ABedAboveTheWaterlineReportsDrySeedsRatherThanBuilding)
{
    // A flat plateau well above the run's waterline: nothing to fill.
    CreateBedTerrain(kTerrainSize, [](float, float) { return 5.0f; });

    const WaterFillRun run = Build(WaterRecipe());
    EXPECT_EQ(run.Outcome, WaterFillOutcome::NoRegion);
    EXPECT_GT(run.Diagnostics.DrySeeds, 0u);
    EXPECT_EQ(run.Diagnostics.WetCorners, 0u);
}

TEST_F(SplineFillRebuildTests, WithNoTerrainTheRunRefusesInsteadOfFillingNothing)
{
    // No terrain entity at all — the fill reads the heightfield, so there is
    // nothing to fill against.
    const WaterFillRun run = Build(WaterRecipe());
    EXPECT_EQ(run.Outcome, WaterFillOutcome::NoTerrain);
    EXPECT_TRUE(run.Chunks.empty());
}

TEST_F(SplineFillRebuildTests, TheRegionSitsOnTheTerrainsOwnSampleLattice)
{
    CreateChannel();
    const WaterFillRun run = Build(WaterRecipe());
    ASSERT_EQ(run.Outcome, WaterFillOutcome::Built);

    // 40 m across 81 samples is 0.5 m spacing, origin at -20. Interior vertices
    // must land exactly on that lattice; only shoreline vertices sit between.
    const float spacing = kTerrainSize / 80.0f;
    uint32_t onLattice = 0;
    uint32_t total = 0;
    for (const SG::SplineFillMesh& mesh : run.Chunks)
    {
        for (const SG::SplineVertex& v : mesh.Vertices)
        {
            ++total;
            const float gx = (v.Position.x + 20.0f) / spacing;
            const float gz = (v.Position.z + 20.0f) / spacing;
            if (std::abs(gx - std::round(gx)) < 1.0e-3f &&
                std::abs(gz - std::round(gz)) < 1.0e-3f)
                ++onLattice;
        }
    }
    ASSERT_GT(total, 0u);
    EXPECT_GT(onLattice, 0u) << "no vertex landed on a terrain sample; the ROI is not aligned";
}

// The two budgets are different guards over different quantities, and the whole
// point of keeping them apart is that they take OPPOSITE fixes. These pin that.
namespace
{

// A run whose XZ bounding box is `span` metres on a side, as an L: two legs
// meeting at a corner, so the BOX is square while the run itself is short. The
// window is sized from the box, which is exactly the asymmetry under test.
std::vector<SG::SplineStripStation> LShapedRun(float span)
{
    std::vector<SG::SplineStripStation> stations;
    float distance = 0.0f;
    const float half = span * 0.5f;
    for (float z = -half; z <= half; z += 5.0f)
    {
        SG::SplineStripStation station;
        station.Position = Vector3(-half, 0.0f, z);
        station.Forward = Vector3(0.0f, 0.0f, 1.0f);
        station.Right = Vector3(1.0f, 0.0f, 0.0f);
        station.Up = Vector3(0.0f, 1.0f, 0.0f);
        station.Distance = distance;
        stations.push_back(station);
        distance += 5.0f;
    }
    for (float x = -half + 5.0f; x <= half; x += 5.0f)
    {
        SG::SplineStripStation station;
        station.Position = Vector3(x, 0.0f, half);
        station.Forward = Vector3(1.0f, 0.0f, 0.0f);
        station.Right = Vector3(0.0f, 0.0f, -1.0f);
        station.Up = Vector3(0.0f, 1.0f, 0.0f);
        station.Distance = distance;
        stations.push_back(station);
        distance += 5.0f;
    }
    return stations;
}

} // namespace

TEST_F(SplineFillRebuildTests, AWindowTooLargeToReadRefusesBeforeItTouchesTheGround)
{
    CreateChannel();
    const Components::SplineExtrude recipe = WaterRecipe();
    GameEngine::float32 matrix[16];
    Identity(matrix);

    // 300 m of span at the terrain's 0.5 m lattice is ~601 corners per axis.
    const std::vector<SG::SplineStripStation> stations = LShapedRun(300.0f);
    const WaterFillRun run = BuildWaterFillRun(m_World, stations, recipe, matrix, &m_ModifierSystem);

    EXPECT_EQ(run.Outcome, WaterFillOutcome::SampleWindowTooLarge);
    EXPECT_GT(run.WindowCorners,
              static_cast<uint64_t>(GameEngine::Editor::kMaxFillWindowCorners));
    EXPECT_TRUE(run.Chunks.empty());

    // Refused before a height is read, so it must claim nothing about the water:
    // every field diagnostic is still at its default.
    EXPECT_EQ(run.Diagnostics.WetCorners, 0u);
    EXPECT_EQ(run.Diagnostics.Seeds, 0u);
    EXPECT_FALSE(run.Diagnostics.ExceededBudget)
        << "ExceededBudget is the FIELD's flood verdict; the field was never called";

    // The reported extent is the window's, so the warning quotes a real number:
    // the run's span plus the margin (reach plus two lattice cells) on each side.
    const float expected = 300.0f + 2.0f * (recipe.MaxHalfWidth + 1.0f);
    EXPECT_NEAR(run.WindowSizeX, expected, 2.0f);
    EXPECT_NEAR(run.WindowSizeZ, expected, 2.0f);
}

// The red arm for the message this refusal used to print. It told the author to
// reduce Max Half Width; the reach enters the window only through its margin, so
// collapsing the reach leaves the refusal exactly where it was.
TEST_F(SplineFillRebuildTests, TheWindowRefusalIsNotAnsweredByANarrowerCorridor)
{
    CreateChannel();
    GameEngine::float32 matrix[16];
    Identity(matrix);
    const std::vector<SG::SplineStripStation> stations = LShapedRun(300.0f);

    Components::SplineExtrude wide = WaterRecipe();
    wide.MaxHalfWidth = 12.0f;
    Components::SplineExtrude narrow = WaterRecipe();
    narrow.MaxHalfWidth = 0.1f;

    const WaterFillRun wideRun = BuildWaterFillRun(m_World, stations, wide, matrix, &m_ModifierSystem);
    const WaterFillRun narrowRun = BuildWaterFillRun(m_World, stations, narrow, matrix, &m_ModifierSystem);

    ASSERT_EQ(wideRun.Outcome, WaterFillOutcome::SampleWindowTooLarge);
    EXPECT_EQ(narrowRun.Outcome, WaterFillOutcome::SampleWindowTooLarge)
        << "a 120x narrower corridor still refuses: the reach is not what sized this window";

    // Under 20% off the window for a 120x reduction in reach, and still over
    // budget. Shortening the run is the only lever that moves this.
    const double shrink = 1.0 - static_cast<double>(narrowRun.WindowCorners) /
                                   static_cast<double>(wideRun.WindowCorners);
    EXPECT_LT(shrink, 0.20);
}

// The runaway-fill guard has to be REACHABLE, not merely present. Wet corners
// are a SUBSET of the window, so a water budget set to the window's bound is a
// guard no input can trip. This pins that it can: a basin that floods past the
// water budget from INSIDE a legal window still refuses.
//
// The band it lives in is what makes it a red arm — the flood here is over the
// field's own budget but under the window's, so handing the field the window
// budget instead would build this happily and the guard would be dead again.
TEST_F(SplineFillRebuildTests, TheWaterBudgetTripsInsideALegalWindow)
{
    // 512 m at 2 samples/m lands exactly on the renderer's 1025-sample ladder,
    // so the lattice is 0.5 m — the shipped island's own spacing.
    constexpr float kBasinTerrain = 512.0f;
    // The rim has to sit INSIDE the corridor, not merely outside the run. A bank
    // the fill cannot see is not a bank: water whose escape route leaves the
    // corridor before reaching the wall is refused as an UnseenBankCorner, so a
    // rim past the reach shrinks the region to the run's own extent instead of
    // dilating it. At 8 m pitch under a 12 m reach the corridor still covers
    // sqrt(12^2 - 4^2) = 11.3 m past the legs, which is where this wall goes.
    constexpr float kRim = 132.0f;
    CreateBedTerrain(kBasinTerrain, [](float x, float z)
    {
        const bool rim = std::abs(x) >= kRim || std::abs(z) >= kRim;
        return rim ? 10.0f : -1.0f;
    });

    // A serpentine at 8 m pitch under the recipe's 12 m reach: consecutive legs'
    // corridors overlap, so the basin is ONE connected region rather than a comb
    // of separate ones.
    std::vector<SG::SplineStripStation> stations;
    float distance = 0.0f;
    Vector3 previous{};
    bool forward = true;
    for (float z = -120.0f; z <= 120.0f; z += 8.0f)
    {
        for (float step = -123.0f; step <= 123.0f; step += 2.0f)
        {
            SG::SplineStripStation station;
            station.Position = Vector3(forward ? step : -step, 0.0f, z);
            station.Forward = Vector3(forward ? 1.0f : -1.0f, 0.0f, 0.0f);
            station.Right = Vector3(0.0f, 0.0f, forward ? -1.0f : 1.0f);
            station.Up = Vector3(0.0f, 1.0f, 0.0f);
            if (!stations.empty())
            {
                const Vector3 delta = station.Position - previous;
                distance += std::sqrt(delta.x * delta.x + delta.z * delta.z);
            }
            station.Distance = distance;
            previous = station.Position;
            stations.push_back(station);
        }
        forward = !forward;
    }

    GameEngine::float32 matrix[16];
    Identity(matrix);
    const WaterFillRun run = BuildWaterFillRun(m_World, stations, WaterRecipe(), matrix, &m_ModifierSystem);

    ASSERT_LE(run.WindowCorners, static_cast<uint64_t>(GameEngine::Editor::kMaxFillWindowCorners))
        << "this probe must sit INSIDE the window budget or it proves the wrong guard";
    EXPECT_EQ(run.Outcome, WaterFillOutcome::OverBudget)
        << "a basin this size floods past the water budget; the guard must fire";
    EXPECT_TRUE(run.Diagnostics.ExceededBudget);
    EXPECT_EQ(run.Diagnostics.WetCorners, 0u) << "over budget builds NOTHING, not a coarser fill";
    EXPECT_TRUE(run.Chunks.empty());
}

// Tiled-terrain arm. A tiled terrain streams tiles out behind the camera, so the
// fill does not read its tile store at all: it composes the ground from the
// modifier stack. The bed is therefore AUTHORED here, as a scene carries it — a
// Global flatten for the banks and a narrow rectangle for the channel. Writing
// samples into tile heightfields (as this fixture once did) would prove nothing:
// production never does it, and the composition would not see it.
class TiledFillProbe : public SplineFillRebuildTests
{
  protected:
    GameEngine::TerrainECS::TiledTerrainHandle m_Handle{};
    GameEngine::TerrainECS::TiledTerrainData* m_Tiled = nullptr;
    float m_BorderX = 0.0f;
    float m_CenterZ = 0.0f;
    GameEngine::ECS::EntityHandle m_ChannelModifier{};

    // The stations run at y = 0, so the floor sits below the waterline and the
    // banks above it. Both are authored in world Y, which is what Flatten takes.
    static constexpr float kBankY = 2.0f;
    static constexpr float kFloorY = -1.0f;
    static constexpr float kChannelHalfX = 2.0f;

    GameEngine::ECS::EntityHandle AddFlatten(float centerX, float centerZ,
                                             Components::TerrainVolumeShape shape, float halfX,
                                             float halfZ, float falloff, float targetY,
                                             float priority)
    {
        const auto e = m_World.CreateEntity();
        Components::TerrainModifierVolume vol{};
        vol.Shape = shape;
        vol.RectHalfX = halfX;
        vol.RectHalfZ = halfZ;
        vol.Radius = halfX;
        vol.Falloff = falloff;
        vol.Priority = priority;
        m_World.AddComponentImmediate(e, vol);

        Components::TerrainFlattenEffect fx{};
        fx.UseVolumeHeight = false;
        fx.TargetHeight = targetY;
        m_World.AddComponentImmediate(e, fx);

        Components::WorldTransform xf{};
        xf.matrix[12] = centerX;
        xf.matrix[14] = centerZ;
        m_World.AddComponentImmediate(e, xf);
        return e;
    }

    // `tilesToLoad` tiles of the pair are streamed in; the rest are absent from
    // the tile store entirely, which is what a camera parked elsewhere leaves.
    void CreateTiledChannel(int tilesToLoad)
    {
        auto& svc = TerrainService::Get();
        GameEngine::TerrainECS::TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 2048.0f;
        cfg.WorldSizeZ = 2048.0f;
        cfg.HeightScale = 1.0f;
        cfg.SamplesPerMeter = 1.0f;
        m_Handle = svc.CreateTiledTerrain(cfg);
        m_Tiled = svc.GetTiledTerrainData(m_Handle);
        ASSERT_NE(m_Tiled, nullptr);
        ASSERT_GE(m_Tiled->Config.TilesPerAxisX, 2u);

        m_BorderX = m_Tiled->WorldOriginX + m_Tiled->Config.TileWorldSize;
        m_CenterZ = m_Tiled->WorldOriginZ + m_Tiled->Config.TileWorldSize * 0.5f;

        for (int tx = 0; tx < tilesToLoad; ++tx)
        {
            auto* tile = svc.LoadTile(m_Handle, GameEngine::TerrainECS::TileCoord{tx, 0});
            ASSERT_NE(tile, nullptr);
            tile->LodState = GameEngine::TerrainECS::TileLodState::Full;
        }

        Components::Terrain terrain{};
        terrain.SizeX = cfg.WorldSizeX;
        terrain.SizeZ = cfg.WorldSizeZ;
        terrain.HeightScale = cfg.HeightScale;
        terrain.Domain = Components::TerrainDomain::Planar;
        terrain.TiledTerrainHandle = m_Handle.Index;
        terrain.TiledTerrainGeneration = m_Handle.Generation;
        const auto e = m_World.CreateEntity();
        m_World.AddComponentImmediate(e, terrain);
        m_World.AddComponentImmediate(e, Components::WorldTransform{});

        // Banks first (whole world, no edge), then the channel cut through them.
        AddFlatten(0.0f, 0.0f, Components::TerrainVolumeShape::Global, 0.0f, 0.0f, 0.0f, kBankY,
                   /*priority*/ 0.0f);
        m_ChannelModifier =
            AddFlatten(m_BorderX, m_CenterZ, Components::TerrainVolumeShape::Rectangle,
                       kChannelHalfX, /*halfZ*/ 400.0f, /*falloff*/ 1.0f, kFloorY,
                       /*priority*/ 1.0f);

        // One gather, so the fill composes against the stack this bake applied.
        m_ModifierSystem.Update(m_World, 1.0f / 60.0f);
    }

    WaterFillRun BuildAtBorder()
    {
        std::vector<SG::SplineStripStation> stations;
        float distance = 0.0f;
        for (float dz = -15.0f; dz <= 15.0f; dz += 0.5f)
        {
            SG::SplineStripStation s;
            s.Position = Vector3(m_BorderX, 0.0f, m_CenterZ + dz);
            s.Forward = Vector3(0.0f, 0.0f, 1.0f);
            s.Right = Vector3(1.0f, 0.0f, 0.0f);
            s.Up = Vector3(0.0f, 1.0f, 0.0f);
            s.Distance = distance;
            stations.push_back(s);
            distance += 0.5f;
        }
        GameEngine::float32 matrix[16];
        Identity(matrix);
        return BuildWaterFillRun(m_World, stations, WaterRecipe(), matrix, &m_ModifierSystem);
    }
};

TEST_F(TiledFillProbe, T1_FillCrossesATileBorderSeamlessly)
{
    CreateTiledChannel(/*tilesToLoad*/ 2);
    const WaterFillRun run = BuildAtBorder();

    ASSERT_EQ(run.Outcome, WaterFillOutcome::Built);
    EXPECT_EQ(run.Diagnostics.DrySeeds, 0u);

    bool left = false, right = false;
    for (const SG::SplineFillMesh& mesh : run.Chunks)
        for (const SG::SplineVertex& v : mesh.Vertices)
        {
            left |= v.Position.x < m_BorderX - 0.5f;
            right |= v.Position.x > m_BorderX + 0.5f;
            EXPECT_LE(std::abs(v.Position.x - m_BorderX), 4.5f)
                << "a vertex reached past the channel: the fill leaked at the tile seam";
        }
    EXPECT_TRUE(left) << "no water left of the border: the flood did not cross the tile seam";
    EXPECT_TRUE(right) << "no water right of the border: the flood did not cross the tile seam";
}

// A corridor straddling a tile the store does not hold must still build. Nothing
// brings in a tile the camera is not near, so a fill that waited for one would
// wait forever — at 8 km scale that is most of the world, and a river authored
// away from the camera would simply never appear.
TEST_F(TiledFillProbe, T2_ACorridorOverAnAbsentTileStillBuilds)
{
    CreateTiledChannel(/*tilesToLoad*/ 1);
    ASSERT_EQ(m_Tiled->Tiles.size(), 1u) << "the right tile must be absent, not merely Empty";
    const WaterFillRun run = BuildAtBorder();

    ASSERT_EQ(run.Outcome, WaterFillOutcome::Built);
    EXPECT_GT(run.Diagnostics.WetCorners, 0u);
    EXPECT_EQ(run.Diagnostics.DrySeeds, 0u);

    bool right = false;
    for (const SG::SplineFillMesh& mesh : run.Chunks)
        for (const SG::SplineVertex& v : mesh.Vertices)
            right |= v.Position.x > m_BorderX + 0.5f;
    EXPECT_TRUE(right) << "no water over the absent tile: the fill still stopped at residency";
}

// Residency must not change the WATER, not merely whether there is any. Same
// scene, same corridor, fully loaded versus streamed out — byte for byte.
TEST_F(TiledFillProbe, T3_TheWaterDoesNotDependOnWhichTilesAreResident)
{
    CreateTiledChannel(/*tilesToLoad*/ 2);
    const std::vector<unsigned char> resident = Bytes(BuildAtBorder());
    ASSERT_FALSE(resident.empty());

    // The camera moves away and the streamer frees both tiles' CPU data — the
    // exact call TileStreamingManager::UnloadDistantTiles makes.
    auto& svc = TerrainService::Get();
    svc.UnloadTile(m_Handle, GameEngine::TerrainECS::TileCoord{0, 0});
    svc.UnloadTile(m_Handle, GameEngine::TerrainECS::TileCoord{1, 0});
    ASSERT_TRUE(m_Tiled->Tiles.empty()) << "the unload did not actually free the tiles";

    const std::vector<unsigned char> streamedOut = Bytes(BuildAtBorder());
    EXPECT_EQ(resident, streamedOut)
        << "the water surface moved when the terrain streamed out from under it";
}

// An off-camera bed edit has to reach the fill. The surface digest folds per-tile
// heightfield versions, and those describe only RESIDENT tiles — with nothing
// streamed in, an edit moves no tile version at all. Unless the bake's own ground
// revision is folded, the digest sits still, the controller never re-runs the fill,
// and the water stays on a bed that moved beneath it until those tiles happen to
// arrive and retire its chunks in front of the camera.
TEST_F(TiledFillProbe, T4_AnOffCameraBedEditMovesTheSurfaceRevision)
{
    CreateTiledChannel(/*tilesToLoad*/ 0);
    ASSERT_TRUE(m_Tiled->Tiles.empty())
        << "no tile may be resident, or the digest has another way to notice";

    const uint64_t before =
        GameEngine::Editor::ConformSurfaceRevision(m_World, &m_ModifierSystem);
    const std::vector<unsigned char> waterBefore = Bytes(BuildAtBorder());
    ASSERT_FALSE(waterBefore.empty());

    // Deepen the channel. Nothing is resident, so no tile version can move.
    auto* fx = m_World.GetComponentForWrite<Components::TerrainFlattenEffect>(m_ChannelModifier);
    ASSERT_NE(fx, nullptr);
    fx->TargetHeight = kFloorY - 1.5f;
    m_ModifierSystem.Update(m_World, 1.0f / 60.0f);

    EXPECT_NE(GameEngine::Editor::ConformSurfaceRevision(m_World, &m_ModifierSystem), before)
        << "the digest did not notice a bed edit outside the streamed set";

    // Control: the edit really did move the water, so the digest above had
    // something to notice rather than being bumped by an edit that changed nothing.
    EXPECT_NE(waterBefore, Bytes(BuildAtBorder()))
        << "the deepened channel produced identical water — the edit never reached the fill";
}

// The chunk AABB the controller hands the culler is derived from the SAME
// vertices it hands the GPU, in the SAME space. The two are computed at
// different moments — the mesher's bounds are world space, the entity's must be
// the placer's local space — so a chunk drawn where its box is not is a
// standing risk rather than a hypothetical: it culls geometry that is on screen
// and the symptom is camera-dependent holes, which look like anything but a
// bounds bug.
//
// Exercised under a placer transform that is NOT the identity, because an
// identity placer cannot tell the two spaces apart.
TEST_F(SplineFillRebuildTests, EveryChunkBoxContainsItsOwnVertices)
{
    CreateChannel();

    // Translate, rotate a quarter turn about Y, and scale: local and world
    // disagree on every axis.
    GameEngine::float32 matrix[16];
    std::memset(matrix, 0, sizeof(matrix));
    matrix[0] = 0.0f;  matrix[2] = -2.0f;
    matrix[5] = 2.0f;
    matrix[8] = 2.0f;  matrix[10] = 0.0f;
    matrix[12] = 37.5f; matrix[13] = -4.25f; matrix[14] = 11.0f;
    matrix[15] = 1.0f;

    const std::vector<SG::SplineStripStation> stations = Run(0.0f);
    const WaterFillRun run = BuildWaterFillRun(m_World, stations, WaterRecipe(), matrix, &m_ModifierSystem);
    ASSERT_EQ(run.Outcome, GameEngine::Editor::WaterFillOutcome::Built);

    uint32_t checkedChunks = 0;
    for (const SG::SplineFillMesh& mesh : run.Chunks)
    {
        if (!mesh.IsValid())
            continue;
        ++checkedChunks;
        for (const SG::SplineVertex& v : mesh.Vertices)
        {
            EXPECT_GE(v.Position.x, mesh.MinBounds.x);
            EXPECT_GE(v.Position.y, mesh.MinBounds.y);
            EXPECT_GE(v.Position.z, mesh.MinBounds.z);
            EXPECT_LE(v.Position.x, mesh.MaxBounds.x);
            EXPECT_LE(v.Position.y, mesh.MaxBounds.y);
            EXPECT_LE(v.Position.z, mesh.MaxBounds.z);
        }
    }
    EXPECT_GT(checkedChunks, 0u) << "no chunk was checked: the oracle proved nothing";
}
