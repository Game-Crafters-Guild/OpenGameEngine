// What the Scene View shows of a region mark-up: its display (walls and lid) through the gizmo
// and the glow's mesh path, nothing while hidden, and the ground it stands on.

#include "Markups/MarkupDrawList.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupGizmo.h"
#include "Markups/MarkupPathPort.h"
#include "Markups/MarkupRegionDisplay.h"
#include "Markups/MarkupRegionPort.h"
#include "Markups/MarkupRenderFeature.h"
#include "Markups/MarkupRequests.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Components/Markup/Markup.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "MarkupECS/MarkupService.h"
#include "Mathematics/Geometry.h"
#include "SceneView/SceneViewGizmos.h"
#include "Scripting/ScriptsConfig.h"
#include "SceneView/SplineDrapePolylines.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainService.h"
#include "TestEnvVar.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace GameEngine;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

constexpr Rendering::ViewId kViewId = 32;
constexpr float kHeight = 8.0f;

class MarkupRegionSceneViewTest : public ::testing::Test
{
  protected:
    // The drape's conform rays pick through the engine's scene picking.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ScriptsConfig scriptsConfig{};
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

    void SetUp() override
    {
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", (m_Root.Path() / "UserData").string().c_str());
        MarkupECS::MarkupService::Initialize();
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_Bridge = std::make_unique<Editor::MarkupEditorBridge>(
            m_Notifications, []() { return int64{1000}; }, []() { return std::optional<std::filesystem::path>(); });
    }

    void TearDown() override
    {
        TearDownTerrain();
        m_Bridge.reset();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
        MarkupECS::MarkupService::Shutdown();
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
    }

    // A linear region over `knots` (world x, z) standing at y = 0, with no terrain under it.
    ECS::EntityHandle CreateRegion(const std::vector<Vector2>& knots)
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        const Editor::RegionPlacement placement =
            Editor::NewRegionPlacement(knots, Components::WorldTransform{}.matrix, 0.0f);
        m_World.AddComponentImmediate(entity, placement.Local);
        m_World.AddComponentImmediate(entity, Components::Markup{});
        Editor::AddRegionParts(m_World, entity, placement.World.matrix, knots, Spline::SplineType::Linear, kHeight);
        (void)MarkupECS::MarkupService::Get().BeginMarkup(m_World, entity, Components::MarkupAuthor::User, 1000);
        return entity;
    }

    // A box mark-up `size` meters across (x, y, z) centered on `center`, as the tool places one.
    ECS::EntityHandle CreateBox(const Vector3& center, const Vector3& size)
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::Transform local{};
        local.matrix[0] = size.x;
        local.matrix[5] = size.y;
        local.matrix[10] = size.z;
        local.matrix[12] = center.x;
        local.matrix[13] = center.y;
        local.matrix[14] = center.z;
        Components::WorldTransform placed{};
        std::copy(std::begin(local.matrix), std::end(local.matrix), placed.matrix);
        m_World.AddComponentImmediate(entity, local);
        m_World.AddComponentImmediate(entity, placed);
        m_World.AddComponentImmediate(entity, Components::Markup{});
        m_World.AddComponentImmediate(entity, Components::MarkupVolume{});
        (void)MarkupECS::MarkupService::Get().BeginMarkup(m_World, entity, Components::MarkupAuthor::User, 1000);
        return entity;
    }

    // Lists `member` on `region` with `mode`.
    void AddMember(ECS::EntityHandle region, ECS::EntityHandle member, Components::MarkupMemberMode mode)
    {
        auto* component = m_World.GetComponentForWrite<Components::MarkupRegion>(region);
        component->Members[component->MemberCount++] = Components::MarkupRegionMember{member, mode};
    }

    // Whether any lid triangle of the region's display covers the ground-plane point (x, z).
    bool LidCovers(ECS::EntityHandle region, const Vector2& point)
    {
        const MarkupECS::MarkupRegionGround& ground = Display(region)->Ground;
        for (std::size_t t = 0; t + 2 < ground.LidTriangles.size(); t += 3)
        {
            std::vector<Vector2> triangle;
            for (std::size_t k = 0; k < 3; ++k)
            {
                const Vector3& corner = ground.LidPoints[ground.LidTriangles[t + k]];
                triangle.emplace_back(corner.x, corner.z);
            }
            if (Mathematics::PointInPolygon(point, triangle))
                return true;
        }
        return false;
    }

    void DrawFrame(const Vector3& camera = Vector3(0.0f, 80.0f, -150.0f))
    {
        Editor::SceneTools::ResetGizmoLineGroups(kViewId);
        Editor::SceneTools::ResetGizmoTriangleGroups(kViewId);
        Editor::SceneTools::GizmoRenderContext context(kViewId, kViewId, &camera);
        m_Gizmo.Draw(context, m_World, *m_Bridge, {}, camera, ++m_Frame);
    }

    static std::size_t TriangleCount()
    {
        std::size_t count = 0;
        if (const auto* groups = Editor::SceneTools::GetGizmoTriangleGroups(kViewId))
        {
            for (const auto& group : *groups)
                count += group.vertices.size() / 9;
        }
        return count;
    }

    static std::size_t LineCount()
    {
        std::size_t count = 0;
        if (const auto* groups = Editor::SceneTools::GetGizmoLineGroups(kViewId))
        {
            for (const auto& group : *groups)
                count += group.vertices.size() / 6;
        }
        return count;
    }

    const MarkupECS::MarkupRegionDisplayCache::Entry* Display(ECS::EntityHandle region)
    {
        return m_Bridge->RegionDisplaysOf(m_World).Find(region);
    }

    // A linear path through `points` (world) placed at its first point.
    ECS::EntityHandle CreatePath(const std::vector<Vector3>& points)
    {
        const ECS::EntityHandle path = m_World.CreateEntity();
        const Components::Transform placed = Editor::NewPathPlacement(points);
        m_World.AddComponentImmediate(path, placed);
        m_World.AddComponentImmediate(path, Components::Markup{});
        Editor::AddPathParts(m_World, path, placed.matrix, points, Spline::SplineType::Linear);
        (void)MarkupECS::MarkupService::Get().BeginMarkup(m_World, path, Components::MarkupAuthor::User, 1000);
        return path;
    }

    // A path's band stands this far above the ground at a sample `distance` meters from the camera:
    // 0.1 % of it, at least 0.05 m (MarkupGizmo's lift rule).
    static float PathLift(float distance) { return std::max(0.05f, 0.001f * distance); }

    // Every vertex of the gizmo's triangles this frame.
    static std::vector<Vector3> TriangleVertices()
    {
        std::vector<Vector3> vertices;
        if (const auto* groups = Editor::SceneTools::GetGizmoTriangleGroups(kViewId))
        {
            for (const auto& group : *groups)
            {
                for (std::size_t i = 0; i + 2 < group.vertices.size(); i += 3)
                    vertices.emplace_back(group.vertices[i], group.vertices[i + 1], group.vertices[i + 2]);
            }
        }
        return vertices;
    }

    // A 400 m terrain centered on the origin holding one cone, kConeHeight tall and kConeRadius
    // across its foot, around the origin; flat at y = 0 beyond it.
    static constexpr float kConeHeight = 300.0f;
    static constexpr float kConeRadius = 150.0f;
    static float Cone(float x, float z) { return std::max(0.0f, 1.0f - std::hypot(x, z) / kConeRadius); }
    void CreateConeTerrain() { CreateTerrainOf(kConeHeight, &Cone); }

    // A 400 m terrain centered on the origin, `heightScale` times `normalized` (0 to 1) high.
    void CreateTerrainOf(float heightScale, float (*normalized)(float x, float z))
    {
        m_TerrainHeightScale = heightScale;
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
        TerrainECS::TerrainService& terrains = TerrainECS::TerrainService::Get();
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = 65;
        config.HeightmapHeight = 65;
        config.WorldSizeX = 400.0f;
        config.WorldSizeZ = 400.0f;
        config.HeightScale = heightScale;
        config.LODLevels = 1;
        config.PatchGridSize = 8;
        m_Terrain = terrains.CreateTerrain(config);
        ASSERT_NE(m_Terrain.Generation, 0u);
        TerrainECS::TerrainData* data = terrains.GetTerrainData(m_Terrain);
        ASSERT_NE(data, nullptr);
        float* samples = data->Heightfield.GetMutableSamples();
        const uint32 width = data->Heightfield.GetWidth();
        for (uint32 row = 0; row < data->Heightfield.GetHeight(); ++row)
        {
            for (uint32 column = 0; column < width; ++column)
            {
                const float x = -200.0f + 400.0f * static_cast<float>(column) / static_cast<float>(width - 1);
                const float z = -200.0f + 400.0f * static_cast<float>(row) / static_cast<float>(width - 1);
                samples[row * width + column] = normalized(x, z);
            }
        }
        terrains.RebuildQuadtree(m_Terrain);
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = config.WorldSizeX;
        terrain.SizeZ = config.WorldSizeZ;
        terrain.HeightScale = config.HeightScale;
        terrain.TerrainDataHandle = m_Terrain.Index;
        terrain.TerrainDataGeneration = m_Terrain.Generation;
        m_World.AddComponentImmediate(entity, terrain);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
    }

    // The terrain's own height at world (x, z): its heightfield, bilinear, as the renderer draws it.
    float TerrainGroundAt(float x, float z) const
    {
        const TerrainECS::TerrainData* data = TerrainECS::TerrainService::Get().GetTerrainData(m_Terrain);
        return data->Heightfield.SampleBilinear((x + 200.0f) / 400.0f, (z + 200.0f) / 400.0f) * m_TerrainHeightScale;
    }

    void TearDownTerrain()
    {
        if (m_Terrain.Generation != 0u)
            TerrainECS::TerrainService::Get().DestroyTerrain(m_Terrain);
        m_Terrain = {};
    }

    TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("MarkupRegionSceneView")};
    Editor::EditorChangeNotifications m_Notifications;
    ECS::World m_World;
    std::unique_ptr<Editor::MarkupEditorBridge> m_Bridge;
    Editor::MarkupGizmo m_Gizmo;
    uint64 m_Frame = 0;
    bool m_OwnsSplines = false;
    TerrainECS::TerrainHandle m_Terrain{};
    float m_TerrainHeightScale = 0.0f;
};

const std::vector<Vector2> kSquare{{0.0f, 0.0f}, {40.0f, 0.0f}, {40.0f, 40.0f}, {0.0f, 40.0f}};

} // namespace

// The gizmo fills a region with its display's walls and lid and outlines its ring at the base and
// along the top edge; a hidden region draws nothing and leaves no group behind.
TEST_F(MarkupRegionSceneViewTest, ARegionDrawsItsDisplayAndNothingWhileHidden)
{
    const ECS::EntityHandle region = CreateRegion(kSquare);
    DrawFrame();
    const auto* display = Display(region);
    ASSERT_NE(display, nullptr);
    ASSERT_TRUE(display->Ground.Closed);
    const std::size_t samples = display->Ground.Outline.size();
    EXPECT_GE(samples, 150u) << "the drape samples the 160 m outline about every meter";
    EXPECT_EQ(display->Mesh.WallVertexCount, samples * 6u) << "one wall quad per drape sample";
    EXPECT_GT(display->Mesh.Vertices.size(), display->Mesh.WallVertexCount) << "no lid";
    EXPECT_EQ(TriangleCount(), display->Mesh.Vertices.size() / 3u);
    EXPECT_EQ(LineCount(), samples * 2u) << "the ring at the base and along the top edge";

    m_Bridge->SetHidden(m_World, std::span<const ECS::EntityHandle>(&region, 1), true);
    DrawFrame();
    EXPECT_EQ(TriangleCount(), 0u);
    EXPECT_EQ(LineCount(), 0u);
}

// The glow draws a region from its display's mesh: the mesh shape, every vertex of it.
TEST_F(MarkupRegionSceneViewTest, TheGlowDrawsARegionFromItsMesh)
{
    CreateRegion(kSquare);
    std::vector<Editor::MarkupDrawItem> items;
    std::vector<uint32> excludedScratch;
    const std::size_t bodies =
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, {}, Vector3(0.0f, 80.0f, -150.0f), nullptr, 1, items,
                                       excludedScratch);
    ASSERT_EQ(items.size(), 1u);
    ASSERT_NE(items[0].Region, nullptr);
    Editor::MarkupRenderFeature glow;
    std::vector<Editor::MarkupRenderFeature::Draw> draws;
    glow.BuildDraws(items, bodies, draws);
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].Rim[3], 2.0f);
    EXPECT_EQ(draws[0].VertexCount, items[0].Region->Mesh.Vertices.size());
    EXPECT_EQ(draws[0].Region, items[0].Region);
}

// A path draws its draped line and a band either side of it through the gizmo, a quad of band each
// side of the line and one line per pair of samples, and has no glow body.
TEST_F(MarkupRegionSceneViewTest, APathDrawsItsDrapedLineAndBandWithoutAGlowBody)
{
    const ECS::EntityHandle road = CreatePath({{0.0f, 0.0f, 0.0f}, {60.0f, 0.0f, 10.0f}, {120.0f, 0.0f, 0.0f}});

    DrawFrame();
    const auto* display = Display(road);
    ASSERT_NE(display, nullptr);
    EXPECT_FALSE(display->Ground.Closed);
    const std::size_t samples = display->Ground.Outline.size();
    EXPECT_GE(samples, 100u) << "the drape samples the 121 m path about every meter";
    EXPECT_EQ(TriangleCount(), (samples - 1) * 4u);
    EXPECT_EQ(LineCount(), samples - 1);

    std::vector<Editor::MarkupDrawItem> items;
    std::vector<uint32> excludedScratch;
    const std::size_t bodies =
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, {}, Vector3(0.0f, 80.0f, -150.0f), nullptr, 2, items,
                                       excludedScratch);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items[0].Path);
    Editor::MarkupRenderFeature glow;
    std::vector<Editor::MarkupRenderFeature::Draw> draws;
    glow.BuildDraws(items, bodies, draws);
    EXPECT_TRUE(draws.empty()) << "a path drew a glow body";
}

// Where the ground is below the sea, a region stands on the sea surface: an ocean at y = 5 lifts
// every wall's foot and the lid to it.
TEST_F(MarkupRegionSceneViewTest, ARegionStandsOnTheSeaWhereTheGroundIsBelowIt)
{
    const ECS::EntityHandle ocean = m_World.CreateEntity();
    Components::WorldTransform placed{};
    placed.matrix[13] = 5.0f;
    m_World.AddComponentImmediate(ocean, placed);
    m_World.AddComponentImmediate(ocean, Components::OceanSurface{});
    EXPECT_FLOAT_EQ(Editor::ResolveMarkupSeaLevel(m_World), 5.0f);

    const ECS::EntityHandle region = CreateRegion(kSquare);
    DrawFrame();
    const auto* display = Display(region);
    ASSERT_NE(display, nullptr);
    for (const Vector3& sample : display->Ground.Outline)
        EXPECT_GE(sample.y, 5.0f);
    for (const Vector3& point : display->Ground.LidPoints)
        EXPECT_GE(point.y, 5.0f);
}

// A region whose outline climbs a hill taller than the drape's 100 m lift stands on the hill: its
// walls' feet and its lid follow the terrain wherever it is, since the ground rays start above the
// terrain, not a fixed height above the outline (whose knots stand at the region's label point).
TEST_F(MarkupRegionSceneViewTest, ARegionOverAHillTallerThanTheRayLiftStandsOnTheHill)
{
    CreateConeTerrain();
    // Its edges cross the cone 60 m from the peak, 180 m up.
    const ECS::EntityHandle region =
        CreateRegion({{-60.0f, -60.0f}, {60.0f, -60.0f}, {60.0f, 60.0f}, {-60.0f, 60.0f}});
    DrawFrame();
    const auto* display = Display(region);
    ASSERT_NE(display, nullptr);
    float highest = 0.0f;
    for (const Vector3& sample : display->Ground.Outline)
    {
        const float ground = TerrainGroundAt(sample.x, sample.z);
        highest = std::max(highest, ground);
        EXPECT_NEAR(sample.y, ground + Editor::SceneTools::kDrapeSurfaceLiftMetres, 0.02f)
            << "a wall's foot off the ground at (" << sample.x << ", " << sample.z << ")";
    }
    EXPECT_GT(highest, 150.0f) << "the outline never climbs above the ray's old reach";
    // A lid point inside the area stands on the terrain's triangles, which on the cone's 6.25 m cells
    // differ from this bilinear reference by up to 0.07 m; well under the lift either way.
    for (const Vector3& point : display->Ground.LidPoints)
        EXPECT_NEAR(point.y, TerrainGroundAt(point.x, point.z) + Editor::SceneTools::kDrapeSurfaceLiftMetres, 0.1f)
            << "the lid off the ground at (" << point.x << ", " << point.z << ")";
}

// markup_get gives a region's groundRange, the lowest and highest heights it stands on, built for
// a region no view has drawn.
TEST_F(MarkupRegionSceneViewTest, GetGivesTheGroundRangeOfARegionNoViewDrew)
{
    CreateConeTerrain();
    const ECS::EntityHandle region =
        CreateRegion({{-60.0f, -60.0f}, {60.0f, -60.0f}, {60.0f, 60.0f}, {-60.0f, 60.0f}});
    ASSERT_EQ(Display(region), nullptr);
    Editor::UndoRedoService undo;
    Editor::MarkupRequestContext context;
    context.World = &m_World;
    context.Bridge = m_Bridge.get();
    context.Undo = &undo;
    context.Notifications = &m_Notifications;
    const nlohmann::json shape = Editor::GetMarkup(context, {{"entityId", region.id}})["markup"]["shape"];
    ASSERT_TRUE(shape.contains("groundRange")) << shape.dump();

    const auto* display = Display(region);
    ASSERT_NE(display, nullptr) << "the ground was not built";
    float lowest = std::numeric_limits<float>::max();
    float highest = std::numeric_limits<float>::lowest();
    for (const auto* points : {&display->Ground.Outline, &display->Ground.LidPoints})
    {
        for (const Vector3& point : *points)
        {
            lowest = std::min(lowest, point.y);
            highest = std::max(highest, point.y);
        }
    }
    EXPECT_FLOAT_EQ(shape["groundRange"]["min"].get<float>(), lowest);
    EXPECT_FLOAT_EQ(shape["groundRange"]["max"].get<float>(), highest);
    EXPECT_GT(highest, 150.0f);
}

// A path along a cross slope lies on it across its whole width: the band's edges stand on the
// ground beside the line, not at the line's height (where the downhill edge would float and the
// uphill one sink).
TEST_F(MarkupRegionSceneViewTest, APathBandLiesOnACrossSlope)
{
    // 100 m over the 400 m terrain along x: a 0.25 cross slope under a path running along z.
    CreateTerrainOf(100.0f, [](float x, float) { return (x + 200.0f) / 400.0f; });
    CreatePath({{0.0f, 50.0f, -15.0f}, {0.0f, 50.0f, 15.0f}});
    const Vector3 camera(0.0f, 60.0f, -40.0f); // every sample within 125 m: the band's 1.5 m minimum
    DrawFrame(camera);
    const std::vector<Vector3> band = TriangleVertices();
    ASSERT_FALSE(band.empty());
    float widest = 0.0f;
    for (const Vector3& vertex : band)
    {
        widest = std::max(widest, std::fabs(vertex.x));
        const Vector3 sample(0.0f, TerrainGroundAt(0.0f, vertex.z), vertex.z);
        EXPECT_NEAR(vertex.y - TerrainGroundAt(vertex.x, vertex.z), PathLift((sample - camera).Length()), 0.02f)
            << "a band vertex off the ground at (" << vertex.x << ", " << vertex.z << ")";
    }
    EXPECT_NEAR(widest, 1.5f, 0.01f) << "the band is not 1.5 m to either side";
}

// A path's band widens and lifts with each sample's own distance from the camera: 1.5 m to either
// side and 0.05 m up near it, 1.2 % and 0.1 % of the distance at the far end of the same path.
TEST_F(MarkupRegionSceneViewTest, APathBandWidensWithDistancePerSample)
{
    CreatePath({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1000.0f}});
    const Vector3 camera(0.0f, 2.0f, -5.0f);
    DrawFrame(camera);
    bool sawNear = false;
    bool sawFar = false;
    for (const Vector3& vertex : TriangleVertices())
    {
        if (std::fabs(vertex.x) < 1.0e-3f)
            continue; // the line's own vertices
        if (vertex.z < 1.0e-3f)
        {
            sawNear = true;
            EXPECT_NEAR(std::fabs(vertex.x), 1.5f, 1.0e-3f);
            EXPECT_NEAR(vertex.y, 0.05f, 1.0e-3f);
        }
        if (vertex.z > 1000.0f - 1.0e-3f)
        {
            sawFar = true;
            const float distance = (Vector3(0.0f, 0.0f, 1000.0f) - camera).Length();
            EXPECT_NEAR(std::fabs(vertex.x), 0.012f * distance, 0.01f) << "the far end keeps the near width";
            EXPECT_NEAR(vertex.y, 0.001f * distance, 0.01f) << "the far end's lift is not 0.1 % of its distance";
        }
    }
    EXPECT_TRUE(sawNear && sawFar);
}

// From about 500 m the band is about 6 m to either side, so its edges stand on the ground read
// between the stored offsets (4 and 10 m), not at the narrowest one: every band vertex still lies
// its lift above the ground under it on the cross slope.
TEST_F(MarkupRegionSceneViewTest, APathBandLiesOnACrossSlopeFromFarAway)
{
    CreateTerrainOf(100.0f, [](float x, float) { return (x + 200.0f) / 400.0f; });
    CreatePath({{0.0f, 50.0f, -15.0f}, {0.0f, 50.0f, 15.0f}});
    const Vector3 camera(0.0f, 60.0f, -515.0f);
    DrawFrame(camera);
    const std::vector<Vector3> band = TriangleVertices();
    ASSERT_FALSE(band.empty());
    float widest = 0.0f;
    for (const Vector3& vertex : band)
    {
        widest = std::max(widest, std::fabs(vertex.x));
        // The lift follows the distance of the line's sample the vertex belongs to.
        const Vector3 sample(0.0f, TerrainGroundAt(0.0f, vertex.z), vertex.z);
        EXPECT_NEAR(vertex.y - TerrainGroundAt(vertex.x, vertex.z), PathLift((sample - camera).Length()), 0.02f)
            << "a band vertex off the ground at (" << vertex.x << ", " << vertex.z << ")";
    }
    EXPECT_GT(widest, 5.5f) << "the band is not wider than the narrowest stored offset";
}

// A path over a ridge between its two knots stays on the ridge: the drape samples the ground about
// every meter, so the line and the band follow the rise and fall between the knots rather than the
// straight segment under them (up to 20 m off here). A guard: it holds at the change that adds it,
// whichever lift the band uses, so it bounds the band within a meter above the ground.
TEST_F(MarkupRegionSceneViewTest, APathOverARidgeBetweenTwoKnotsStaysOnIt)
{
    // A 20 m ridge across the path at x = 0 (a sample every meter over the 400 m terrain).
    CreateTerrainOf(20.0f, [](float x, float) { return std::max(0.0f, 1.0f - std::fabs(x) / 10.0f); });
    CreatePath({{-20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}});
    DrawFrame(Vector3(0.0f, 30.0f, -40.0f));
    const std::vector<Vector3> band = TriangleVertices();
    ASSERT_FALSE(band.empty());
    float highest = 0.0f;
    for (const Vector3& vertex : band)
    {
        const float ground = TerrainGroundAt(vertex.x, vertex.z);
        highest = std::max(highest, ground);
        EXPECT_GE(vertex.y - ground, 0.0f)
            << "a band vertex under the ridge at (" << vertex.x << ", " << vertex.z << ")";
        EXPECT_LE(vertex.y - ground, 1.0f)
            << "a band vertex off the ridge at (" << vertex.x << ", " << vertex.z << ")";
    }
    EXPECT_GT(highest, 19.0f) << "the band never reaches the ridge's top";
}

// An Exclude member cuts a hole in the region's display: a second ring around the hole, the walls
// standing on it facing into the hole, and no lid over it.
TEST_F(MarkupRegionSceneViewTest, AnExcludeMemberCutsAHoleInTheLidWithWallsInTheHole)
{
    const ECS::EntityHandle forest = CreateRegion(kSquare);
    const ECS::EntityHandle clearing = CreateRegion({{10.0f, 10.0f}, {20.0f, 10.0f}, {20.0f, 20.0f}, {10.0f, 20.0f}});
    AddMember(forest, clearing, Components::MarkupMemberMode::Exclude);
    DrawFrame();
    const auto* display = Display(forest);
    ASSERT_NE(display, nullptr);
    const MarkupECS::MarkupRegionGround& ground = display->Ground;
    ASSERT_EQ(ground.GetRingCount(), 2u) << "no ring around the clearing";
    for (const Vector2& point : ground.GetRingXZ(1))
    {
        const float toEdge = std::min({std::fabs(point.x - 10.0f), std::fabs(point.x - 20.0f), std::fabs(point.y - 10.0f),
                                       std::fabs(point.y - 20.0f)});
        EXPECT_LT(toEdge, 0.01f) << "a hole sample off the clearing's outline at (" << point.x << ", " << point.y << ")";
    }
    EXPECT_LT(Mathematics::PolygonDoubledSignedArea(ground.GetRingXZ(1)), 0.0f) << "the hole's walls face out of it";
    EXPECT_EQ(display->Mesh.WallVertexCount, ground.Outline.size() * 6u) << "a wall on every sample of both rings";
    EXPECT_FALSE(LidCovers(forest, Vector2(15.0f, 15.0f))) << "the lid covers the clearing";
    EXPECT_TRUE(LidCovers(forest, Vector2(5.0f, 5.0f)));
}

// An Include member extends the region's display: one ring around the base and the box together,
// the lid over both.
TEST_F(MarkupRegionSceneViewTest, AnIncludeMemberExtendsTheLidAndTheWalls)
{
    const ECS::EntityHandle forest = CreateRegion(kSquare);
    const ECS::EntityHandle grove = CreateBox(Vector3(50.0f, 0.0f, 20.0f), Vector3(30.0f, 8.0f, 10.0f));
    AddMember(forest, grove, Components::MarkupMemberMode::Include);
    DrawFrame();
    const auto* display = Display(forest);
    ASSERT_NE(display, nullptr);
    const MarkupECS::MarkupRegionGround& ground = display->Ground;
    ASSERT_EQ(ground.GetRingCount(), 1u);
    float farthest = 0.0f;
    for (const Vector2& point : ground.GetRingXZ(0))
        farthest = std::max(farthest, point.x);
    EXPECT_NEAR(farthest, 65.0f, 0.01f) << "the walls do not reach the box's far side";
    EXPECT_TRUE(LidCovers(forest, Vector2(60.0f, 20.0f))) << "the lid does not cover the box";
    EXPECT_TRUE(LidCovers(forest, Vector2(5.0f, 5.0f)));
}

// Moving a member rebuilds the display of the region that lists it, its hole following the box,
// and leaves a region that does not list it as it was.
TEST_F(MarkupRegionSceneViewTest, MovingAMemberRebuildsTheRegionsThatListItAndNoOther)
{
    const ECS::EntityHandle forest = CreateRegion(kSquare);
    const ECS::EntityHandle village = CreateRegion({{100.0f, 0.0f}, {140.0f, 0.0f}, {140.0f, 40.0f}, {100.0f, 40.0f}});
    const ECS::EntityHandle pond = CreateBox(Vector3(15.0f, 0.0f, 15.0f), Vector3(6.0f, 4.0f, 6.0f));
    AddMember(forest, pond, Components::MarkupMemberMode::Exclude);
    DrawFrame();
    ASSERT_NE(Display(forest), nullptr);
    ASSERT_NE(Display(village), nullptr);
    ASSERT_FALSE(LidCovers(forest, Vector2(15.0f, 15.0f)));
    const uint64 forestRevision = Display(forest)->MeshRevision;
    const uint64 villageRevision = Display(village)->MeshRevision;

    m_World.GetComponentForWrite<Components::WorldTransform>(pond)->matrix[12] = 25.0f;
    DrawFrame();
    EXPECT_NE(Display(forest)->MeshRevision, forestRevision) << "the region listing the box kept its old display";
    EXPECT_TRUE(LidCovers(forest, Vector2(15.0f, 15.0f))) << "the hole stayed where the box was";
    EXPECT_FALSE(LidCovers(forest, Vector2(25.0f, 15.0f))) << "no hole where the box is";
    EXPECT_EQ(Display(village)->MeshRevision, villageRevision) << "a region not listing the box was rebuilt";
}

// Deleting an Exclude member closes its hole; undo revives the member and reopens it.
TEST_F(MarkupRegionSceneViewTest, ADeletedMembersHoleClosesAndUndoReopensIt)
{
    const ECS::EntityHandle forest = CreateRegion(kSquare);
    const ECS::EntityHandle pond = CreateBox(Vector3(15.0f, 0.0f, 15.0f), Vector3(6.0f, 4.0f, 6.0f));
    AddMember(forest, pond, Components::MarkupMemberMode::Exclude);
    DrawFrame();
    ASSERT_NE(Display(forest), nullptr);
    ASSERT_FALSE(LidCovers(forest, Vector2(15.0f, 15.0f)));

    Editor::UndoRedoService undo;
    undo.Execute(std::make_unique<Editor::DeleteEntitiesCommand>("Delete", &m_World, &m_Notifications,
                                                                 std::vector<ECS::EntityHandle>{pond}));
    ASSERT_FALSE(m_World.IsValid(pond));
    DrawFrame();
    EXPECT_TRUE(LidCovers(forest, Vector2(15.0f, 15.0f))) << "the deleted member's hole stayed open";
    EXPECT_EQ(Display(forest)->Ground.GetRingCount(), 1u);

    undo.Undo();
    ASSERT_TRUE(m_World.IsValid(pond));
    DrawFrame();
    EXPECT_FALSE(LidCovers(forest, Vector2(15.0f, 15.0f))) << "undo did not reopen the hole";
    EXPECT_EQ(Display(forest)->Ground.GetRingCount(), 2u);
}

// An Exclude member draws no body at rest, only its outline, so the Forest's hole reads as a hole;
// selected it draws its body as any mark-up. A box that is no member draws its body at rest.
TEST_F(MarkupRegionSceneViewTest, AnExcludeMemberDrawsNoBodyAtRest)
{
    const ECS::EntityHandle forest = CreateRegion(kSquare);
    const ECS::EntityHandle pond = CreateBox(Vector3(15.0f, 0.0f, 15.0f), Vector3(6.0f, 4.0f, 6.0f));
    const ECS::EntityHandle shed = CreateBox(Vector3(80.0f, 0.0f, 15.0f), Vector3(6.0f, 4.0f, 6.0f));
    AddMember(forest, pond, Components::MarkupMemberMode::Exclude);
    const auto drawsBody = [&](ECS::EntityHandle entity, const Editor::MarkupHighlightState& highlight) {
        std::vector<Editor::MarkupDrawItem> items;
        std::vector<uint32> excludedScratch;
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, highlight, Vector3(0.0f, 80.0f, -150.0f), nullptr, ++m_Frame,
                                       items, excludedScratch);
        const auto item = std::find_if(items.begin(), items.end(),
                                       [entity](const Editor::MarkupDrawItem& drawn) { return drawn.Entity == entity; });
        return item != items.end() && item->DrawsBody();
    };
    EXPECT_FALSE(drawsBody(pond, {})) << "the Exclude member drew its body at rest";
    EXPECT_TRUE(drawsBody(shed, {}));
    Editor::MarkupHighlightState selected;
    selected.Selected = std::span<const ECS::EntityHandle>(&pond, 1);
    EXPECT_TRUE(drawsBody(pond, selected)) << "the selected Exclude member drew no body";
}
