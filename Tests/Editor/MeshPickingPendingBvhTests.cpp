// A pick never builds a large mesh's BVH on the calling thread unless it asks to wait. The
// first pick starts a job and answers with the mesh's bounds, marked pending; once the job
// publishes, picks hit triangles, and the published BVH is the one a synchronous build makes.
// A Scene View drop reports a point or a mesh that is only a pending stand-in, so the drop can
// refuse it, and an invalidated build never publishes.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

#include "Picking/MeshBvhCache.h"
#include "Picking/MeshPickingService.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "MeshPicking/MeshBvh.h"
#include "Scene/SceneTlas.h"
#include "Markups/MarkupTool.h"
#include "SceneView/SceneViewDropPlacement.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::GUID;
using GameEngine::Mesh;
using GameEngine::ECS::World;
using GameEngine::Editor::Picking::MeshBvhCache;
using GameEngine::Editor::Picking::PickableKind;
using GameEngine::Editor::Picking::PickOptions;
using GameEngine::Editor::Picking::RaycastScene;
using GameEngine::Mathematics::Ray3D;
using GameEngine::Mathematics::Vector3;

namespace Components = GameEngine::Components;
namespace MeshPicking = GameEngine::MeshPicking;

namespace
{
constexpr float kGridSize = 20.0f;
// 300 x 300 vertices, about 179k triangles: a build long enough to tell from a copy.
constexpr uint32_t kGridVertices = 300;

float SurfaceHeight(float x, float z)
{
    return 0.5f * std::sin(x * 1.3f) * std::cos(z * 1.1f);
}

// A bumpy square grid centred on the origin, heights within [-0.5, 0.5].
Mesh MakeGrid()
{
    Mesh mesh;
    mesh.Vertices.resize(kGridVertices * kGridVertices);
    for (uint32_t j = 0; j < kGridVertices; ++j)
    {
        for (uint32_t i = 0; i < kGridVertices; ++i)
        {
            const float x = (static_cast<float>(i) / (kGridVertices - 1) - 0.5f) * kGridSize;
            const float z = (static_cast<float>(j) / (kGridVertices - 1) - 0.5f) * kGridSize;
            auto& position = mesh.Vertices[j * kGridVertices + i].Position;
            position[0] = x;
            position[1] = SurfaceHeight(x, z);
            position[2] = z;
        }
    }
    for (uint32_t j = 0; j + 1 < kGridVertices; ++j)
    {
        for (uint32_t i = 0; i + 1 < kGridVertices; ++i)
        {
            const uint32_t a = j * kGridVertices + i;
            const uint32_t c = a + kGridVertices;
            mesh.Indices.insert(mesh.Indices.end(), {a, c, a + 1, a + 1, c, c + 1});
        }
    }
    return mesh;
}

MeshPicking::MeshView ViewOf(const Mesh& mesh)
{
    MeshPicking::MeshView view;
    view.Positions = reinterpret_cast<const Vector3*>(&mesh.Vertices[0].Position[0]);
    view.VertexStride = sizeof(GameEngine::Vertex);
    view.VertexCount = static_cast<uint32_t>(mesh.Vertices.size());
    view.Indices = mesh.Indices.data();
    view.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
    return view;
}

double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Straight down onto the grid.
Ray3D DownAt(float x, float z)
{
    Ray3D ray;
    ray.origin = Vector3(x, 50.0f, z);
    ray.direction = Vector3(0.0f, -1.0f, 0.0f);
    return ray;
}

// One grid model registered with the engine's asset manager and one entity rendering it. The
// engine is initialized because the picks reach the asset manager and the job system.
class MeshPickingPendingBvhTests : public ::testing::Test
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
        m_Model = GameEngine::MakeShared<GameEngine::ModelAsset>(m_Guid, "pending_bvh_grid.glb");
        m_Model->SetMeshesForTest({MakeGrid()});
        EngineCore::GetInstance().GetAssetManager().RegisterLoadedAsset(m_Guid, m_Model);

        m_Entity = m_World.CreateEntity();
        Components::MeshRenderer renderer{};
        renderer.modelAssetGuid.Set(m_Guid);
        m_World.AddComponentImmediate(m_Entity, renderer);
        m_World.AddComponentImmediate(m_Entity, Components::WorldTransform{});
        Components::LocalBounds bounds{};
        bounds.Box.center = Vector3(0.0f, 0.0f, 0.0f);
        bounds.Box.halfExtents = Vector3(kGridSize * 0.5f, 0.5f, kGridSize * 0.5f);
        m_World.AddComponentImmediate(m_Entity, bounds);
    }

    void TearDown() override
    {
        // Waits for a job still building the key, so none outlives the test.
        (void)MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid());
        MeshBvhCache::Instance().InvalidateAsset(m_Guid);
        GameEngine::Scene::ReleaseSceneTlas(m_World);
    }

    const Mesh& Grid() const { return m_Model->GetMesh(0); }

    PickOptions RestrictedToGrid() const
    {
        PickOptions options;
        options.RestrictToEntity = m_Entity;
        return options;
    }

    const GUID m_Guid = GUID::Generate();
    GameEngine::SharedPtr<GameEngine::ModelAsset> m_Model;
    World m_World;
    GameEngine::ECS::EntityHandle m_Entity{};
};
} // namespace

TEST_F(MeshPickingPendingBvhTests, TheFirstLookupIsPendingAtOnceAndPublishesTheSynchronousBvh)
{
    const auto buildStart = std::chrono::steady_clock::now();
    const MeshPicking::MeshBvh reference = MeshPicking::MeshBvh::Build(ViewOf(Grid()));
    const double buildMs = MillisecondsSince(buildStart);

    const auto lookupStart = std::chrono::steady_clock::now();
    const auto first = MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid());
    const double lookupMs = MillisecondsSince(lookupStart);
    EXPECT_TRUE(first.Pending) << "the lookup built the BVH on the calling thread";
    EXPECT_EQ(first.Bvh, nullptr);
    EXPECT_LT(lookupMs, buildMs / 4.0) << "lookup " << lookupMs << " ms against a " << buildMs << " ms build";

    const auto published = MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid());
    ASSERT_NE(published, nullptr);
    std::vector<uint8_t> publishedBytes;
    std::vector<uint8_t> referenceBytes;
    published->Serialize(publishedBytes);
    reference.Serialize(referenceBytes);
    EXPECT_EQ(publishedBytes, referenceBytes) << "the job's BVH differs from a synchronous build of the same mesh";

    const auto later = MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid());
    EXPECT_FALSE(later.Pending);
    EXPECT_EQ(later.Bvh, published);
}

TEST_F(MeshPickingPendingBvhTests, APickAnswersWithTheBoundsUntilTheBvhIsPublishedThenHitsTheSurface)
{
    const Ray3D ray = DownAt(3.1f, -2.7f);
    const auto whilePending = RaycastScene(ray, m_World, RestrictedToGrid());
    ASSERT_TRUE(whilePending.Hit);
    EXPECT_EQ(whilePending.Best.Kind, PickableKind::Bounds);
    EXPECT_NEAR(whilePending.Best.WorldPosition.y, 0.5f, 1e-4f) << "not on the top of the bounds";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const auto published = RaycastScene(ray, m_World, RestrictedToGrid());
    ASSERT_TRUE(published.Hit);
    EXPECT_EQ(published.Best.Kind, PickableKind::Mesh);
    EXPECT_NEAR(published.Best.WorldPosition.y, SurfaceHeight(3.1f, -2.7f), 0.01f);
}

TEST_F(MeshPickingPendingBvhTests, APickThatWaitsForTheBvhHitsTheSurfaceAtOnce)
{
    PickOptions options = RestrictedToGrid();
    options.WaitForMeshBvh = true;
    const auto pick = RaycastScene(DownAt(-4.3f, 6.2f), m_World, options);
    ASSERT_TRUE(pick.Hit);
    EXPECT_EQ(pick.Best.Kind, PickableKind::Mesh);
    EXPECT_NEAR(pick.Best.WorldPosition.y, SurfaceHeight(-4.3f, 6.2f), 0.01f);
}

TEST_F(MeshPickingPendingBvhTests, ADropPointOnAPendingMeshIsMarkedSoPlacementRefusesIt)
{
    const Ray3D ray = DownAt(3.1f, -2.7f);
    const auto whilePending = GameEngine::Editor::ResolveSceneViewDropPoint(ray, m_World);
    EXPECT_TRUE(whilePending.OnPendingMesh) << "a model would be placed on the bounds";
    EXPECT_NEAR(whilePending.Position.y, 0.5f, 1e-4f);

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const auto published = GameEngine::Editor::ResolveSceneViewDropPoint(ray, m_World);
    EXPECT_FALSE(published.OnPendingMesh);
    EXPECT_NEAR(published.Position.y, SurfaceHeight(3.1f, -2.7f), 0.01f);
}

// A region's knot is ground, never refused for a mesh whose picking is still being prepared: the
// Mark-up tool's Region click over a road's fresh mesh lands on the ground beneath (here no
// terrain, so the ground plane), where a drop would refuse it.
TEST_F(MeshPickingPendingBvhTests, ARegionClickOverAPendingMeshLandsOnTheGround)
{
    const Ray3D ray = DownAt(3.1f, -2.7f);
    ASSERT_TRUE(GameEngine::Editor::ResolveSceneViewDropPoint(ray, m_World).OnPendingMesh);
    const std::optional<Vector3> ground = GameEngine::Editor::ResolveMarkupRegionGround(ray, m_World);
    ASSERT_TRUE(ground.has_value()) << "the click was dropped";
    EXPECT_NEAR(ground->x, 3.1f, 1e-4f);
    EXPECT_NEAR(ground->y, 0.0f, 1e-4f);
    EXPECT_NEAR(ground->z, -2.7f, 1e-4f);
}

// Texture and material drops both assign to this query's mesh.
TEST_F(MeshPickingPendingBvhTests, ADropMeshStillPendingIsMarkedSoAssignmentRefusesIt)
{
    const Ray3D ray = DownAt(-4.3f, 6.2f);
    const auto whilePending = GameEngine::Editor::PickSceneViewDropMesh(ray, m_World);
    EXPECT_EQ(whilePending.Entity, m_Entity);
    EXPECT_TRUE(whilePending.Pending) << "a texture or material would be assigned through the bounds";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const auto published = GameEngine::Editor::PickSceneViewDropMesh(ray, m_World);
    EXPECT_EQ(published.Entity, m_Entity);
    EXPECT_FALSE(published.Pending);
}

// A renderer with no CPU mesh data has always answered with its bounds; that answer is final.
TEST_F(MeshPickingPendingBvhTests, ABoundsHitForAMeshWithoutCpuDataIsNotPending)
{
    const GameEngine::ECS::EntityHandle runtime = m_World.CreateEntity();
    Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(GUID::Generate());
    m_World.AddComponentImmediate(runtime, renderer);
    m_World.AddComponentImmediate(runtime, Components::WorldTransform{});
    m_World.AddComponentImmediate(runtime, Components::LocalBounds{});
    PickOptions options;
    options.RestrictToEntity = runtime;

    const auto pick = RaycastScene(DownAt(0.1f, 0.1f), m_World, options);
    ASSERT_TRUE(pick.Hit);
    EXPECT_EQ(pick.Best.Kind, PickableKind::Bounds);
    EXPECT_FALSE(pick.Best.MeshBvhPending);
}

TEST_F(MeshPickingPendingBvhTests, ABuildInvalidatedWhileItRunsPublishesNothingAndTheNextBuildUsesTheNewGeometry)
{
    const auto buildStart = std::chrono::steady_clock::now();
    (void)MeshPicking::MeshBvh::Build(ViewOf(Grid()));
    const auto buildTime = std::chrono::steady_clock::now() - buildStart;

    MeshBvhCache& cache = MeshBvhCache::Instance();
    const size_t sizeBefore = cache.Size();
    ASSERT_TRUE(cache.TryGet(m_Guid, 0, Grid()).Pending);
    cache.InvalidateAsset(m_Guid);
    Mesh raised = MakeGrid();
    for (GameEngine::Vertex& vertex : raised.Vertices)
        vertex.Position[1] += 5.0f;
    m_Model->SetMeshesForTest({raised});

    // Long past the invalidated job's own build; it must not have published.
    std::this_thread::sleep_for(buildTime * 4 + std::chrono::milliseconds(200));
    EXPECT_EQ(cache.Size(), sizeBefore) << "the invalidated build published its BVH";

    const auto next = cache.TryGet(m_Guid, 0, Grid());
    EXPECT_TRUE(next.Pending) << "the lookup after the invalidation did not start a new build";
    const auto rebuilt = cache.GetOrBuild(m_Guid, 0, Grid());
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_GT(rebuilt->RootBounds().min.y, 4.0f) << "the BVH was built from the geometry before the invalidation";
}

// The pick trusts a ready BVH: when it misses, the pick misses, with no per-triangle pass over
// the mesh after it. The cache key is seeded with a BVH of the grid moved far aside, so a ray
// onto the grid's real triangles misses that BVH; only a per-triangle pass would find them.
TEST_F(MeshPickingPendingBvhTests, AMissAgainstAReadyBvhIsTheAnswer)
{
    Mesh aside = MakeGrid();
    for (GameEngine::Vertex& vertex : aside.Vertices)
        vertex.Position[0] += 100.0f;
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, aside), nullptr);

    const auto pick = RaycastScene(DownAt(3.1f, -2.7f), m_World, RestrictedToGrid());
    EXPECT_FALSE(pick.Hit) << "the pick tested the mesh triangle by triangle after its BVH missed";
}

// A synchronous lookup never waits for the Background lane. A hover's lookup queued the key's
// build behind jobs that hold every compute worker; the synchronous lookup that follows (a
// spline conform's pick that waits for the BVH) builds the key itself and returns while the
// lane is still held, and the queued job, reached later, finds the build done and returns.
TEST_F(MeshPickingPendingBvhTests, AWaitingLookupBuildsAQueuedBuildItselfInsteadOfWaitingForTheLane)
{
    JobSystem::WorkStealingThreadPool& pool = EngineCore::GetInstance().GetJobSystem();
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    // Shared: the queued held jobs run after the test body returns.
    const auto held = std::make_shared<std::atomic<size_t>>(0);
    const size_t workers = pool.GetWorkerCount();
    // Every worker held, and more held jobs queued ahead of the build.
    for (size_t i = 0; i < workers + 2; ++i)
    {
        pool.EnqueueWork([held, released]()
                         {
                             held->fetch_add(1);
                             released.wait();
                         },
                         JobSystem::JobPriority::Background);
    }
    const auto heldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (held->load() < workers && std::chrono::steady_clock::now() < heldDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (held->load() != workers)
        release.set_value();
    ASSERT_EQ(held->load(), workers) << "the held jobs never occupied every worker";

    ASSERT_TRUE(MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid()).Pending);
    std::future<std::shared_ptr<const MeshPicking::MeshBvh>> lookup = std::async(
        std::launch::async, [this]() { return MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()); });
    const bool returnedWhileHeld = lookup.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
    release.set_value();
    const auto bvh = lookup.get();
    EXPECT_TRUE(returnedWhileHeld) << "the synchronous lookup waited for the jobs queued ahead of the build";
    ASSERT_NE(bvh, nullptr);
    EXPECT_FALSE(MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid()).Pending);
}
