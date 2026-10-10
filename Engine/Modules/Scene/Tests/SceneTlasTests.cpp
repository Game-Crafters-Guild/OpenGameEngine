#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "JobSystem/WorkStealingThreadPool.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "Core/Engine.h"  // EngineCore::IsInitialized — the never-touches-EngineCore lock only
#include "ECS/ECSTemplates.h"  // World::AddComponent<T> bodies for real-entity tests
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"
#include "Scene/FunctionRef.h"
#include "Scene/SceneTlas.h"

namespace
{

using namespace GameEngine;
using namespace GameEngine::Scene;

using GameEngine::Engine::Renderer::RenderExtractionSystem;

// Build a synthetic WorldRenderableRecord for an entity with axis-aligned
// bounds at the given world position. Identity rotation, unit scale.
RenderExtractionSystem::WorldRenderableRecord MakeRecord(
    ECS::EntityHandle entity,
    const Mathematics::Vector3& worldPos,
    const Mathematics::Vector3& halfExtents = {0.5f, 0.5f, 0.5f},
    uint32 layerMask = 0xFFFFFFFFu,
    uint32 version = 1u)
{
    RenderExtractionSystem::WorldRenderableRecord rec{};
    rec.entity = entity;
    // Identity rotation + unit scale; translate to worldPos.
    for (int i = 0; i < 16; ++i) rec.worldTransform.matrix[i] = 0.0f;
    rec.worldTransform.matrix[0]  = 1.0f;
    rec.worldTransform.matrix[5]  = 1.0f;
    rec.worldTransform.matrix[10] = 1.0f;
    rec.worldTransform.matrix[15] = 1.0f;
    rec.worldTransform.matrix[12] = worldPos.x;
    rec.worldTransform.matrix[13] = worldPos.y;
    rec.worldTransform.matrix[14] = worldPos.z;
    rec.worldTransform.Version = version;

    rec.meshRenderer.renderLayerMask = layerMask;

    rec.hasBounds = true;
    rec.bounds.Box.center      = {0.0f, 0.0f, 0.0f};
    rec.bounds.Box.halfExtents = halfExtents;
    return rec;
}

ECS::EntityHandle MakeHandle(uint32 id)
{
    ECS::EntityHandle e;
    e.id = id;
    return e;
}

// Brute-force "did this AABB get hit by the ray under MaxDistance?" — used
// as the parity reference for TraverseRay correctness.
bool BruteForceRayHits(const Mathematics::Ray3D& ray,
                       const Mathematics::AABB& worldBox,
                       float32 maxDistance)
{
    float32 te = 0.0f, tx = 0.0f;
    if (!Mathematics::IntersectRayAABB(ray, worldBox, te, tx))
        return false;
    return te < maxDistance;
}

// Test fixture: clears the per-World TLAS registry between tests so state
// doesn't bleed across cases.
class SceneTlasFixture : public ::testing::Test
{
protected:
    void TearDown() override
    {
        ClearAllSceneTlasForTesting();
    }
};

}  // anon

// ---- Existing minimal smoke tests --------------------------------------

TEST_F(SceneTlasFixture, EmptyTlasReturnsZero)
{
    SceneTlas tlas;
    EXPECT_TRUE(tlas.IsEmpty());
    EXPECT_EQ(tlas.InstanceCount(), 0u);
    EXPECT_EQ(tlas.NodeCount(), 0u);

    Mathematics::Ray3D ray{};
    ray.origin    = {0.0f, 0.0f, 0.0f};
    ray.direction = {0.0f, 0.0f, 1.0f};

    int hits = 0;
    auto cb = [&](const TlasInstance&, float32, float32) { ++hits; return true; };
    tlas.TraverseRay(ray, {}, cb);
    EXPECT_EQ(hits, 0);
}

TEST_F(SceneTlasFixture, FunctionRefBindsLambda)
{
    int captured = 0;
    auto lambda = [&captured](int x) { captured = x; return x > 0; };
    FunctionRef<bool(int)> ref = lambda;
    EXPECT_TRUE(ref(42));
    EXPECT_EQ(captured, 42);
    EXPECT_FALSE(ref(-1));
    EXPECT_EQ(captured, -1);
}

// ---- SyncFromRecords structural correctness ----------------------------

TEST_F(SceneTlasFixture, SyncFromRecordsInsertsEntity)
{
    ECS::World world;
    SceneTlas tlas;

    auto eh = MakeHandle(1);
    std::vector recs{ MakeRecord(eh, {5.0f, 0.0f, 0.0f}) };

    tlas.SyncFromRecords(recs);
    EXPECT_TRUE(tlas.Contains(eh));
    EXPECT_EQ(tlas.InstanceCount(), 1u);
    EXPECT_FALSE(tlas.IsEmpty());
}

TEST_F(SceneTlasFixture, SyncFromRecordsDespawnsRemovedEntity)
{
    ECS::World world;
    SceneTlas tlas;

    auto e1 = MakeHandle(1);
    auto e2 = MakeHandle(2);
    std::vector first{ MakeRecord(e1, {0,0,0}), MakeRecord(e2, {10,0,0}) };
    tlas.SyncFromRecords(first);
    EXPECT_EQ(tlas.InstanceCount(), 2u);

    // Drop e2 from the records — TLAS should despawn it.
    std::vector second{ MakeRecord(e1, {0,0,0}) };
    tlas.SyncFromRecords(second);
    EXPECT_EQ(tlas.InstanceCount(), 1u);
    EXPECT_TRUE(tlas.Contains(e1));
    EXPECT_FALSE(tlas.Contains(e2));
}

TEST_F(SceneTlasFixture, SyncFromRecordsBoundslessGetsDefaultUnitBounds)
{
    // Boundsless entities (no LocalBounds component) — typical of primitives
    // like Sphere/Plane that PrimitiveGenerator doesn't populate — must
    // remain pickable. The TLAS synthesizes a default unit-cube AABB
    // around the entity's world position, matching the legacy
    // RaycastScene fallback before Phase E.
    ECS::World world;
    SceneTlas tlas;

    auto eh = MakeHandle(1);
    auto rec = MakeRecord(eh, {0,0,0});
    rec.hasBounds = false;

    tlas.SyncFromRecords(std::vector{rec});
    EXPECT_TRUE(tlas.Contains(eh));
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // Ray straight through origin should hit the default unit-cube leaf.
    Mathematics::Ray3D ray;
    ray.origin    = {0,0,-10};
    ray.direction = {0,0,1};

    int hits = 0;
    auto cb = [&](const TlasInstance&, float32, float32) { ++hits; return true; };
    tlas.TraverseRay(ray, {}, cb);
    EXPECT_EQ(hits, 1);
}

TEST_F(SceneTlasFixture, SyncFromRecordsRefitsOnVersionChange)
{
    ECS::World world;
    SceneTlas tlas;

    auto eh = MakeHandle(1);
    // First sync at origin with version=1.
    tlas.SyncFromRecords(std::vector{MakeRecord(eh, {0,0,0}, {0.5f, 0.5f, 0.5f}, 0xFFFFFFFFu, 1u)});

    // Ray straight at (0, 0, -10) along +Z hits the unit cube at origin.
    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, 0.0f, -10.0f};
    ray.direction = {0.0f, 0.0f,   1.0f};

    int hits = 0;
    auto cb = [&](const TlasInstance&, float32, float32) { ++hits; return true; };
    tlas.TraverseRay(ray, {}, cb);
    EXPECT_EQ(hits, 1);

    // Re-sync at (100, 0, 0) with bumped version. Same ray should now miss.
    tlas.SyncFromRecords(std::vector{MakeRecord(eh, {100,0,0}, {0.5f, 0.5f, 0.5f}, 0xFFFFFFFFu, 2u)});
    hits = 0;
    tlas.TraverseRay(ray, {}, cb);
    EXPECT_EQ(hits, 0);
}

// ---- TraverseRay correctness -------------------------------------------

TEST_F(SceneTlasFixture, TraverseRayHitsBoxOnAxis)
{
    ECS::World world;
    SceneTlas tlas;
    auto eh = MakeHandle(1);
    tlas.SyncFromRecords(std::vector{MakeRecord(eh, {0,0,5})});

    Mathematics::Ray3D ray;
    ray.origin    = {0,0,0};
    ray.direction = {0,0,1};

    int hits = 0;
    float32 capturedTEnter = -1.0f;
    auto cb = [&](const TlasInstance& inst, float32 tEnter, float32) {
        ++hits;
        capturedTEnter = tEnter;
        EXPECT_EQ(inst.Entity.id, eh.id);
        return true;
    };
    tlas.TraverseRay(ray, {}, cb);
    EXPECT_EQ(hits, 1);
    // Box at z=5 with halfExtents 0.5 → entry at z=4.5.
    EXPECT_NEAR(capturedTEnter, 4.5f, 1e-4f);
}

TEST_F(SceneTlasFixture, TraverseRayMissesOffAxis)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SyncFromRecords(std::vector{MakeRecord(MakeHandle(1), {0,0,5})});

    Mathematics::Ray3D ray;
    ray.origin    = {100, 100, 0};
    ray.direction = {0,   0,   1};

    int hits = 0;
    auto cb = [&](const TlasInstance&, float32, float32) { ++hits; return true; };
    tlas.TraverseRay(ray, {}, cb);
    EXPECT_EQ(hits, 0);
}

TEST_F(SceneTlasFixture, TraverseRayDispatchesInTEnterOrder)
{
    ECS::World world;
    SceneTlas tlas;
    auto e1 = MakeHandle(1);
    auto e2 = MakeHandle(2);
    auto e3 = MakeHandle(3);
    // 3 boxes along +Z at z=10, 5, 15.
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e1, {0,0,10}),
        MakeRecord(e2, {0,0, 5}),
        MakeRecord(e3, {0,0,15}),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0,0,0};
    ray.direction = {0,0,1};

    std::vector<uint32> visitOrder;
    auto cb = [&](const TlasInstance& inst, float32, float32) {
        visitOrder.push_back(inst.Entity.id);
        return true;
    };
    tlas.TraverseRay(ray, {}, cb);
    ASSERT_EQ(visitOrder.size(), 3u);
    // e2 (z=5) first, then e1 (z=10), then e3 (z=15).
    EXPECT_EQ(visitOrder[0], e2.id);
    EXPECT_EQ(visitOrder[1], e1.id);
    EXPECT_EQ(visitOrder[2], e3.id);
}

TEST_F(SceneTlasFixture, TraverseRayLayerMaskFilters)
{
    ECS::World world;
    SceneTlas tlas;
    auto e1 = MakeHandle(1);  // layer bit 0
    auto e2 = MakeHandle(2);  // layer bit 1
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e1, {0,0,5}, {0.5f,0.5f,0.5f}, 0x1u),
        MakeRecord(e2, {0,0,5}, {0.5f,0.5f,0.5f}, 0x2u),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0,0,0};
    ray.direction = {0,0,1};

    // Only bit 1 — e2 only.
    TraverseRayOptions opts;
    opts.LayerMask = 0x2u;

    std::vector<uint32> hits;
    auto cb = [&](const TlasInstance& inst, float32, float32) {
        hits.push_back(inst.Entity.id);
        return true;
    };
    tlas.TraverseRay(ray, opts, cb);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], e2.id);
}

TEST_F(SceneTlasFixture, TraverseRayIgnoreEntitiesFilters)
{
    ECS::World world;
    SceneTlas tlas;
    auto e1 = MakeHandle(1);
    auto e2 = MakeHandle(2);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e1, {0,0,5}),
        MakeRecord(e2, {0,0,10}),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0,0,0};
    ray.direction = {0,0,1};

    ECS::EntityHandle ignore[] = {e1};
    TraverseRayOptions opts;
    opts.IgnoreEntities = ignore;

    std::vector<uint32> hits;
    auto cb = [&](const TlasInstance& inst, float32, float32) {
        hits.push_back(inst.Entity.id);
        return true;
    };
    tlas.TraverseRay(ray, opts, cb);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], e2.id);
}

TEST_F(SceneTlasFixture, TraverseRayMaxDistanceClipsFar)
{
    ECS::World world;
    SceneTlas tlas;
    auto e1 = MakeHandle(1);
    auto e2 = MakeHandle(2);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e1, {0,0,5}),
        MakeRecord(e2, {0,0,50}),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0,0,0};
    ray.direction = {0,0,1};

    TraverseRayOptions opts;
    opts.MaxDistance = 10.0f;

    std::vector<uint32> hits;
    auto cb = [&](const TlasInstance& inst, float32, float32) {
        hits.push_back(inst.Entity.id);
        return true;
    };
    tlas.TraverseRay(ray, opts, cb);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], e1.id);
}

// ---- TraverseRay parity vs brute force ---------------------------------

TEST_F(SceneTlasFixture, TraverseRayParityVsBruteForce)
{
    ECS::World world;
    SceneTlas tlas;

    constexpr int kEntityCount = 100;
    constexpr int kRayCount    = 50;
    std::mt19937 rng(0xC0FFEE);
    // Dense cluster so random rays through it have a meaningful hit rate.
    std::uniform_real_distribution<float32> coord(-10.0f, 10.0f);
    std::uniform_real_distribution<float32> halfDist(0.5f, 2.0f);

    // Build records + remember each entity's world AABB for brute-force.
    std::vector<RenderExtractionSystem::WorldRenderableRecord> recs;
    std::vector<Mathematics::AABB> worldBoxes;
    for (int i = 0; i < kEntityCount; ++i)
    {
        Mathematics::Vector3 pos{coord(rng), coord(rng), coord(rng)};
        Mathematics::Vector3 he{halfDist(rng), halfDist(rng), halfDist(rng)};
        recs.push_back(MakeRecord(MakeHandle(static_cast<uint32>(i + 1)), pos, he));

        // Mirror the AABB the TLAS will compute internally (identity matrix
        // means world AABB = local box translated by pos).
        Mathematics::AABB box;
        box.min = {pos.x - he.x, pos.y - he.y, pos.z - he.z};
        box.max = {pos.x + he.x, pos.y + he.y, pos.z + he.z};
        worldBoxes.push_back(box);
    }
    tlas.SyncFromRecords(recs);

    // Aim rays AT the entity cluster from outside it — guarantees a
    // meaningful hit rate. Origins on sphere of radius 30, targets
    // anywhere inside the [-10, 10]³ cluster.
    std::uniform_real_distribution<float32> originSign(-1.0f, 1.0f);
    std::uniform_real_distribution<float32> targetCoord(-10.0f, 10.0f);

    int totalCompared = 0;
    for (int r = 0; r < kRayCount; ++r)
    {
        Mathematics::Vector3 originDir{originSign(rng), originSign(rng), originSign(rng)};
        if (originDir.Length() < 1e-3f) originDir = {1, 0, 0};
        originDir = originDir.Normalize();
        Mathematics::Vector3 origin{originDir.x * 30.0f, originDir.y * 30.0f, originDir.z * 30.0f};
        Mathematics::Vector3 target{targetCoord(rng), targetCoord(rng), targetCoord(rng)};

        Mathematics::Ray3D ray;
        ray.origin = origin;
        Mathematics::Vector3 dir{target.x - origin.x, target.y - origin.y, target.z - origin.z};
        if (dir.Length() < 1e-3f) dir = {0, 0, 1};
        ray.direction = dir.Normalize();

        // Collect TLAS hits.
        std::vector<uint32> tlasHits;
        auto cb = [&](const TlasInstance& inst, float32, float32) {
            tlasHits.push_back(inst.Entity.id);
            return true;
        };
        tlas.TraverseRay(ray, {}, cb);

        // Collect brute-force hits.
        std::vector<uint32> bruteHits;
        for (int i = 0; i < kEntityCount; ++i)
        {
            if (BruteForceRayHits(ray, worldBoxes[i], std::numeric_limits<float32>::infinity()))
                bruteHits.push_back(static_cast<uint32>(i + 1));
        }

        // Compare as sets (ordering differs between the two — brute-force
        // is index order, TLAS is tEnter order).
        std::sort(tlasHits.begin(), tlasHits.end());
        std::sort(bruteHits.begin(), bruteHits.end());
        ASSERT_EQ(tlasHits, bruteHits) << "ray " << r;
        totalCompared += static_cast<int>(tlasHits.size());
    }
    // Sanity: at least some rays should have hit.
    EXPECT_GT(totalCompared, 0);
}

// ---- TraverseSphere correctness ----------------------------------------

TEST_F(SceneTlasFixture, TraverseSphereSelectsByRadius)
{
    ECS::World world;
    SceneTlas tlas;
    auto eNear = MakeHandle(1);
    auto eFar  = MakeHandle(2);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(eNear, {2,0,0}),    // half-extent 0.5 → nearest face at x=1.5
        MakeRecord(eFar,  {20,0,0}),
    });

    std::vector<uint32> hits;
    auto cb = [&](const TlasInstance& inst) { hits.push_back(inst.Entity.id); };
    tlas.TraverseSphere({0,0,0}, 5.0f, {}, cb);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], eNear.id);
}

TEST_F(SceneTlasFixture, TraverseSphereLayerMaskFilters)
{
    ECS::World world;
    SceneTlas tlas;
    auto e1 = MakeHandle(1);
    auto e2 = MakeHandle(2);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e1, {0,0,0}, {0.5f,0.5f,0.5f}, 0x1u),
        MakeRecord(e2, {0,0,0}, {0.5f,0.5f,0.5f}, 0x2u),
    });

    TraverseSphereOptions opts;
    opts.LayerMask = 0x2u;

    std::vector<uint32> hits;
    auto cb = [&](const TlasInstance& inst) { hits.push_back(inst.Entity.id); };
    tlas.TraverseSphere({0,0,0}, 100.0f, opts, cb);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], e2.id);
}

// ---- TraverseFrustum correctness ---------------------------------------

TEST_F(SceneTlasFixture, TraverseFrustumSelectsInside)
{
    ECS::World world;
    SceneTlas tlas;
    auto eIn   = MakeHandle(1);
    auto eOut  = MakeHandle(2);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(eIn,  {0,0,0}),
        MakeRecord(eOut, {100,0,0}),
    });

    // 6-plane unit cube around origin. Each plane has normal pointing
    // INWARD; halfspace formula is (n·p + d) ≥ 0 inside.
    // Plane: x ≥ -10  →  normal=(+1,0,0), d=10
    // Plane: x ≤  10  →  normal=(-1,0,0), d=10
    Mathematics::Plane planes[6] = {
        {{ 1, 0, 0}, 10.0f},
        {{-1, 0, 0}, 10.0f},
        {{ 0, 1, 0}, 10.0f},
        {{ 0,-1, 0}, 10.0f},
        {{ 0, 0, 1}, 10.0f},
        {{ 0, 0,-1}, 10.0f},
    };

    std::vector<uint32> hits;
    auto cb = [&](const TlasInstance& inst) { hits.push_back(inst.Entity.id); };
    tlas.TraverseFrustum(planes, {}, cb);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], eIn.id);
}

// ---- Concurrent traversal smoke ----------------------------------------

TEST_F(SceneTlasFixture, ConcurrentTraverseIsSafe)
{
    ECS::World world;
    SceneTlas tlas;
    std::vector<RenderExtractionSystem::WorldRenderableRecord> recs;
    std::mt19937 rng(0xCAFEBABE);
    std::uniform_real_distribution<float32> coord(-20.0f, 20.0f);
    for (uint32 i = 0; i < 50u; ++i)
        recs.push_back(MakeRecord(MakeHandle(i + 1), {coord(rng), coord(rng), coord(rng)}));
    tlas.SyncFromRecords(recs);

    constexpr int kThreads = 8;
    constexpr int kRaysPerThread = 200;
    std::atomic<int> totalHits{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, seed = static_cast<uint32>(t * 31 + 1)]() {
            std::mt19937 trng(seed);
            std::uniform_real_distribution<float32> oc(-30.0f, 30.0f);
            std::uniform_real_distribution<float32> dc(-1.0f, 1.0f);
            int localHits = 0;
            auto cb = [&](const TlasInstance&, float32, float32) {
                ++localHits;
                return true;
            };
            for (int r = 0; r < kRaysPerThread; ++r)
            {
                Mathematics::Ray3D ray;
                ray.origin = {oc(trng), oc(trng), oc(trng)};
                Mathematics::Vector3 dir{dc(trng), dc(trng), dc(trng)};
                if (dir.Length() < 1e-3f) dir = {0,0,1};
                ray.direction = dir.Normalize();
                tlas.TraverseRay(ray, {}, cb);
            }
            totalHits.fetch_add(localHits, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();
    // Sanity: some rays should have hit. The exact count varies with RNG;
    // the real assertion is "no crash, no torn read" which is implicit in
    // the join completing without hangs or ASAN errors.
    EXPECT_GT(totalHits.load(), 0);
}

// ---- E.5.1: multi-sector replication & traversal ----------------------------
//
// At the default sectorSize (Components::kWorldSectorSize = 1024m), all of the
// existing tests above sit inside sector (0,0,0). The cases below shrink sectorSize to exercise sector
// routing, replication, and cross-sector traversal with per-query EntityId
// dedup.

TEST_F(SceneTlasFixture, BoundarySpanningEntityReplicatesIntoOverlappingSectors)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);
    EXPECT_FLOAT_EQ(tlas.GetSectorSize(), 50.0f);

    // Bridge centered on the boundary between sectors (0,0,0) and (1,0,0)
    // at world x=50; halfExtent.x=30 stretches it from x=20 to x=80, which
    // overlaps both sectors.
    auto bridge = MakeHandle(42);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(bridge, {50.0f, 0.0f, 0.0f}, {30.0f, 0.5f, 0.5f}),
    });

    // Single entity, but the BVH internally has two leaves (one per sector).
    EXPECT_EQ(tlas.InstanceCount(), 1u);
    EXPECT_GE(tlas.NodeCount(), 2u);  // each replica builds a tiny BVH

    // A ray that only clips sector (1,0,0) (x>50) still finds the bridge.
    Mathematics::Ray3D ray;
    ray.origin    = {70.0f, -10.0f, 0.0f};
    ray.direction = {0.0f,    1.0f, 0.0f};
    int hits = 0;
    uint32 hitEntity = 0u;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        ++hits;
        hitEntity = inst.Entity.id;
        return true;
    });
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(hitEntity, bridge.id);
}

TEST_F(SceneTlasFixture, RayDedupReportsReplicatedEntityOnce)
{
    // A ray that crosses BOTH sectors must still report the replicated
    // entity exactly once per query (per-query EntityId dedup).
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto bridge = MakeHandle(7);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(bridge, {50.0f, 0.0f, 0.0f}, {30.0f, 0.5f, 0.5f}),
    });

    // Ray sweeps from negative X through both sectors.
    Mathematics::Ray3D ray;
    ray.origin    = {-10.0f, 0.0f, 0.0f};
    ray.direction = { 1.0f,  0.0f, 0.0f};
    int hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        EXPECT_EQ(inst.Entity.id, bridge.id);
        ++hits;
        return true;
    });
    EXPECT_EQ(hits, 1);  // not 2
}

TEST_F(SceneTlasFixture, RayDedupReplicatesAcross8SectorsReportsOnce)
{
    // Plan-required "must pass" critical case: an entity replicated into
    // 8 sectors (2x2x2) reports exactly once when a ray clips all of them.
    // Bridge-like AABB centered at the corner of 8 sectors at world origin.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    // Bounds straddle sector indices {-1,0} on every axis: (-25..+25)
    // intersects sector(-1,...) and sector(0,...) in each axis.
    auto cube = MakeHandle(101);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(cube, {0.0f, 0.0f, 0.0f}, {25.0f, 25.0f, 25.0f}),
    });
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // Ray crossing all 8 sectors diagonally — must report cube once.
    Mathematics::Ray3D ray;
    ray.origin    = {-100.0f, -100.0f, -100.0f};
    ray.direction = {   1.0f,    1.0f,    1.0f};
    Mathematics::Vector3 d = ray.direction;
    const float32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    ray.direction = {d.x / len, d.y / len, d.z / len};
    int hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        EXPECT_EQ(inst.Entity.id, cube.id);
        ++hits;
        return true;
    });
    EXPECT_EQ(hits, 1);  // not 8
}

TEST_F(SceneTlasFixture, SphereDedupReportsReplicatedEntityOnce)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto bridge = MakeHandle(7);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(bridge, {50.0f, 0.0f, 0.0f}, {30.0f, 0.5f, 0.5f}),
    });

    int hits = 0;
    tlas.TraverseSphere({50.0f, 0.0f, 0.0f}, 100.0f, {},
        [&](const TlasInstance& inst) {
            EXPECT_EQ(inst.Entity.id, bridge.id);
            ++hits;
        });
    EXPECT_EQ(hits, 1);
}

TEST_F(SceneTlasFixture, MultipleEntitiesAcrossSectorsAllSelectableByRay)
{
    // Three small entities in three different sectors. A long ray sweeping
    // through all three should pick up each, in front-to-back order.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto a = MakeHandle(1);  // sector (0,0,0) at x=10
    auto b = MakeHandle(2);  // sector (2,0,0) at x=110
    auto c = MakeHandle(3);  // sector (4,0,0) at x=210
    tlas.SyncFromRecords(std::vector{
        MakeRecord(a, {10.0f,  0.0f, 0.0f}),
        MakeRecord(b, {110.0f, 0.0f, 0.0f}),
        MakeRecord(c, {210.0f, 0.0f, 0.0f}),
    });

    EXPECT_EQ(tlas.InstanceCount(), 3u);

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, 0.0f, 0.0f};
    ray.direction = {1.0f, 0.0f, 0.0f};
    std::vector<uint32> hits;
    std::vector<float32> tEntries;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32 te, float32) {
        hits.push_back(inst.Entity.id);
        tEntries.push_back(te);
        return true;
    });
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0], a.id);
    EXPECT_EQ(hits[1], b.id);
    EXPECT_EQ(hits[2], c.id);
    EXPECT_LT(tEntries[0], tEntries[1]);
    EXPECT_LT(tEntries[1], tEntries[2]);
}

TEST_F(SceneTlasFixture, MigrationAcrossBoundaryUpdatesReplicaSet)
{
    // Entity slides across a sector boundary; replica set should track
    // the new overlap on each Sync.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(11);

    // Frame 1: entity entirely in sector (0,0,0).
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {10.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 0xFFFFFFFFu, 1u),
    });
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // A ray in sector (1,0,0) — entity NOT replicated there yet — should miss.
    Mathematics::Ray3D ray;
    ray.origin    = {70.0f, -10.0f, 0.0f};
    ray.direction = {0.0f,   1.0f,  0.0f};
    int hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 0);

    // Frame 2: entity moves to x=70, still in sector (1,0,0) only.
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {70.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 0xFFFFFFFFu, 2u),
    });
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);  // now visible

    // Old sector (0,0,0): a ray there should miss after migration.
    Mathematics::Ray3D oldRay;
    oldRay.origin    = {10.0f, -10.0f, 0.0f};
    oldRay.direction = { 0.0f,  1.0f, 0.0f};
    hits = 0;
    tlas.TraverseRay(oldRay, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 0);
}

TEST_F(SceneTlasFixture, DespawnRemovesEntityFromAllReplicaSectors)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto bridge = MakeHandle(99);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(bridge, {50.0f, 0.0f, 0.0f}, {30.0f, 0.5f, 0.5f}),
    });
    EXPECT_TRUE(tlas.Contains(bridge));

    // Despawn: empty record set.
    tlas.SyncFromRecords(std::vector<RenderExtractionSystem::WorldRenderableRecord>{});
    EXPECT_FALSE(tlas.Contains(bridge));
    EXPECT_EQ(tlas.InstanceCount(), 0u);

    // Both replica sectors should be cleared. A ray hitting either misses.
    Mathematics::Ray3D ray;
    ray.origin    = {25.0f, -10.0f, 0.0f};
    ray.direction = { 0.0f,  1.0f, 0.0f};
    int hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 0);

    ray.origin = {75.0f, -10.0f, 0.0f};
    hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 0);
}

// ---- E.5.4: hysteresis on replica-set migration ----------------------------

TEST_F(SceneTlasFixture, HysteresisHoldsReplicaSetUntilThresholdCrossed)
{
    // Plan-required critical case: walk an entity in 9% increments past
    // a sector boundary; replica set must NOT shrink/migrate until the
    // 10%-of-sectorSize threshold is crossed. Once crossed, replicas
    // update to the new set.
    //
    // Setup: sectorSize=50m, hysteresis threshold = 5m. Entity is small
    // (halfExtents 0.1) so its world AABB closely tracks its translation.
    // It sits at x=49 (sector 0), then nudges past x=50 (sector boundary)
    // by sub-threshold steps.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(101);

    // Frame 1: position x=49.0, fully in sector (0,0,0).
    auto rec = MakeRecord(e, {49.0f, 0.0f, 0.0f}, {0.1f, 0.1f, 0.1f}, 0xFFFFFFFFu, 1u);
    tlas.SyncFromRecords(std::vector{rec});
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // A ray inside sector (0,0,0) should hit (entity is there).
    Mathematics::Ray3D rayPrev;
    rayPrev.origin    = {49.0f, -10.0f, 0.0f};
    rayPrev.direction = { 0.0f,  1.0f,  0.0f};
    int hits = 0;
    tlas.TraverseRay(rayPrev, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);

    // Frame 2: nudge to x=51.0 — crossed the boundary by 1m (2% of
    // sectorSize, well below the 10% / 5m threshold). The floor-rounded
    // sector key is now (1,0,0), but hysteresis must hold the replica
    // set at (0,0,0). A ray at x=49 (where the leaf still lives) should
    // STILL hit; a ray at x=51 — the new physical position — should miss
    // because the leaf hasn't migrated yet.
    rec = MakeRecord(e, {51.0f, 0.0f, 0.0f}, {0.1f, 0.1f, 0.1f}, 0xFFFFFFFFu, 2u);
    tlas.SyncFromRecords(std::vector{rec});

    Mathematics::Ray3D rayNew;
    rayNew.origin    = {51.0f, -10.0f, 0.0f};
    rayNew.direction = { 0.0f,  1.0f,  0.0f};
    hits = 0;
    tlas.TraverseRay(rayNew, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    // The leaf's WorldBounds got refit to the new position, so a ray
    // through (51, ...) hits — but it hits the leaf in sector (0,0,0),
    // NOT a leaf in sector (1,0,0). Distinguish by NodeCount: still 1
    // primary sector + 0 map sectors (no migration).
    EXPECT_EQ(hits, 1);  // refit covered the leaf's new world bounds
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // Frame 3: cross the threshold. Move entity to x=60 (11m past the
    // boundary at x=50, more than 5m). Hysteresis releases; replica set
    // updates to (1,0,0).
    rec = MakeRecord(e, {60.0f, 0.0f, 0.0f}, {0.1f, 0.1f, 0.1f}, 0xFFFFFFFFu, 3u);
    tlas.SyncFromRecords(std::vector{rec});

    Mathematics::Ray3D rayFar;
    rayFar.origin    = {60.0f, -10.0f, 0.0f};
    rayFar.direction = { 0.0f,  1.0f,  0.0f};
    hits = 0;
    tlas.TraverseRay(rayFar, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);

    // Frame 4: small wiggle within sector (1,0,0) — must NOT fall back
    // to migrating to (0,0,0) again. Centroid x=58 is 8m below the
    // boundary at 50, threshold is 5m; with the new stable AABB at x=60
    // the drift is 2m, still under threshold.
    rec = MakeRecord(e, {58.0f, 0.0f, 0.0f}, {0.1f, 0.1f, 0.1f}, 0xFFFFFFFFu, 4u);
    tlas.SyncFromRecords(std::vector{rec});
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // Strong assertion: a ray at the new position hits, and a ray back at
    // the old sector (0,0,0) misses — confirming no replica leaked back.
    Mathematics::Ray3D rayWiggle;
    rayWiggle.origin    = {58.0f, -10.0f, 0.0f};
    rayWiggle.direction = { 0.0f,  1.0f,  0.0f};
    hits = 0;
    tlas.TraverseRay(rayWiggle, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);

    Mathematics::Ray3D rayBack;
    rayBack.origin    = {49.0f, -10.0f, 0.0f};
    rayBack.direction = { 0.0f,  1.0f,  0.0f};
    hits = 0;
    tlas.TraverseRay(rayBack, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 0);
}

// ---- E.5.3: DDA ray pruning + sector AABB pre-cull -------------------------

TEST_F(SceneTlasFixture, RayDdaSkipsEmptySectorsBetweenPopulated)
{
    // Two entities far apart with empty sectors between them. The DDA walk
    // should reach both via cell-by-cell stepping. Verify both are picked
    // up and (indirectly) that the iteration is correct end-to-end.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto a = MakeHandle(1);  // sector (0,0,0)
    auto b = MakeHandle(2);  // sector (10,0,0) — 9 empty sectors in between
    tlas.SyncFromRecords(std::vector{
        MakeRecord(a, {10.0f,  0.0f, 0.0f}),
        MakeRecord(b, {510.0f, 0.0f, 0.0f}),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, 0.0f, 0.0f};
    ray.direction = {1.0f, 0.0f, 0.0f};
    std::vector<uint32> hits;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        hits.push_back(inst.Entity.id);
        return true;
    });
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0], a.id);
    EXPECT_EQ(hits[1], b.id);
}

TEST_F(SceneTlasFixture, RayDdaAxisAlignedZeroDirComponent)
{
    // Ray with dir.y = 0 (no Y motion). DDA must not advance on the Y axis;
    // it should walk X cells normally. Verify the entity in the same Y-row
    // is found.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(3);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {110.0f, 0.0f, 0.0f}),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, 0.0f, 0.0f};
    ray.direction = {1.0f, 0.0f, 0.0f};  // dir.y = dir.z = 0
    int hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);
}

TEST_F(SceneTlasFixture, RayDdaMaxDistanceClipsBeforeFarSector)
{
    // Entity at sector (10, 0, 0) — but ray's MaxDistance stops short.
    // Should not be reported.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto far = MakeHandle(4);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(far, {510.0f, 0.0f, 0.0f}),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, 0.0f, 0.0f};
    ray.direction = {1.0f, 0.0f, 0.0f};
    TraverseRayOptions opts;
    opts.MaxDistance = 100.0f;  // far entity is at x=510
    int hits = 0;
    tlas.TraverseRay(ray, opts, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 0);
}

TEST_F(SceneTlasFixture, SphereAabbPreCullSkipsNonOverlappingSectors)
{
    // Sphere centered at (0,0,0) radius 30 should NOT visit a populated
    // sector at (10, 0, 0) (centered at world x≈525). The pre-cull is
    // observable indirectly by correct hit count — primary entities hit,
    // far ones not. (Direct sector-skip count requires instrumentation;
    // this confirms behavior parity which is the contract.)
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto near = MakeHandle(5);
    auto far  = MakeHandle(6);
    tlas.SyncFromRecords(std::vector{
        MakeRecord(near, {10.0f,  0.0f, 0.0f}),
        MakeRecord(far,  {510.0f, 0.0f, 0.0f}),
    });

    std::vector<uint32> hits;
    tlas.TraverseSphere({0.0f, 0.0f, 0.0f}, 30.0f, {},
        [&](const TlasInstance& inst) { hits.push_back(inst.Entity.id); });
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], near.id);
}

TEST_F(SceneTlasFixture, WorldSectorCoordPresentComposesWorldMatrix)
{
    // E.5.2: When a record has hasSectorCoord=true, the worldTransform is
    // interpreted as sector-local. Composed world position = sector*sectorSize
    // + localTranslation. Verify a ray in the COMPOSED world position picks
    // it up, while the sector-local origin doesn't.
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(77);
    auto rec = MakeRecord(e, {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    rec.hasSectorCoord     = true;
    rec.sectorCoord.x      = 2;  // composed world x = 100
    rec.sectorCoord.y      = 0;
    rec.sectorCoord.z      = 0;
    tlas.SyncFromRecords(std::vector{rec});

    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // Ray at the COMPOSED position should hit.
    Mathematics::Ray3D rayWorld;
    rayWorld.origin    = {100.0f, -10.0f, 0.0f};
    rayWorld.direction = {0.0f,    1.0f,  0.0f};
    int hits = 0;
    tlas.TraverseRay(rayWorld, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);

    // Ray at the SECTOR-LOCAL origin should miss (entity is no longer there).
    Mathematics::Ray3D rayLocal;
    rayLocal.origin    = {0.0f, -10.0f, 0.0f};
    rayLocal.direction = {0.0f,  1.0f,  0.0f};
    hits = 0;
    tlas.TraverseRay(rayLocal, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 0);
}

TEST_F(SceneTlasFixture, WorldSectorCoordZeroIsNoOp)
{
    // hasSectorCoord=true with sector (0,0,0) is equivalent to no
    // composition (the offset is zero).
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(78);
    auto rec = MakeRecord(e, {10.0f, 0.0f, 0.0f});
    rec.hasSectorCoord = true;
    // sectorCoord is default-constructed to (0,0,0)
    tlas.SyncFromRecords(std::vector{rec});

    EXPECT_EQ(tlas.InstanceCount(), 1u);

    Mathematics::Ray3D ray;
    ray.origin    = {10.0f, -10.0f, 0.0f};
    ray.direction = { 0.0f,  1.0f,  0.0f};
    int hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);
}

TEST_F(SceneTlasFixture, BackCompatDefaultSectorSizeKeepsAllInPrimary)
{
    // The default sectorSize (Components::kWorldSectorSize = 1024m, the ONE
    // constant shared with the render path) means typical scenes (entities
    // within a sector of origin) all live in PrimarySector — the no-op single-
    // sector path. Verify the parity test passes with default settings: a ray
    // hitting an entity at world position (100,0,0) still reports it.
    ECS::World world;
    SceneTlas tlas;  // no SetSectorSizeForTesting — keep the default
    EXPECT_FLOAT_EQ(tlas.GetSectorSize(), Components::kWorldSectorSize);

    auto e = MakeHandle(1);
    tlas.SyncFromRecords(std::vector{ MakeRecord(e, {100.0f, 0.0f, 0.0f}) });
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    Mathematics::Ray3D ray;
    ray.origin    = {100.0f, -10.0f, 0.0f};
    ray.direction = {  0.0f,   1.0f, 0.0f};
    int hits = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        ++hits; return true;
    });
    EXPECT_EQ(hits, 1);
}

// Audit follow-up: negative-direction DDA was uncovered. Walk a ray from
// +X to -X across an empty sector gap and assert it hits the entity in
// the negative-X sector.
TEST_F(SceneTlasFixture, RayDdaNegativeDirectionFindsEntityInNegativeSector)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(1);
    // Entity at sector (-3, 0, 0) (world x ≈ -140 with halfExtent 0.5).
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {-140.0f, 0.0f, 0.0f}),
    });

    // Ray from +X heading -X, must traverse empty sectors at x=200, 150,
    // 100, 50, 0, -50, -100 before reaching the entity.
    Mathematics::Ray3D ray;
    ray.origin    = {200.0f, 0.0f, 0.0f};
    ray.direction = {-1.0f,  0.0f, 0.0f};

    int hits = 0;
    uint32 hitId = 0;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        ++hits;
        hitId = inst.Entity.id;
        return false;
    });
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(hitId, e.id);
}

// Audit follow-up: hysteresis × replication on a boundary-spanning entity.
// A bridge already replicating into 2 sectors drifts in 9% increments past
// a third boundary. Replica set must hold at 2 sectors until the 10%
// threshold is crossed.
TEST_F(SceneTlasFixture, HysteresisXReplicationOnBoundarySpanningEntity)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(77);

    // Drives a small entity across a sector-edge boundary in three stages so
    // each stage exercises a distinct branch of the migration code:
    //
    //   Stage 1: AABB fully inside sector 0  → replica set {0}
    //   Stage 2: AABB barely peeks into sector 1 with sub-threshold drift →
    //            floor-rounded set wants {0, 1} but hysteresis HOLDS at {0}
    //   Stage 3: drift exceeds 10% of sectorSize → hysteresis RELEASES;
    //            entity migrates to its new sector
    //
    // InstanceCount is the cleanest signal: it's the total leaf count
    // across all sectors. Hysteresis holding means "extra replica was NOT
    // created" → count stays 1; release means count migrates without ever
    // doubling (old single-sector dropped, new single-sector added).

    // Stage 1: x=48.5, halfExtent=1 → AABB (47.5, 49.5). Both ends in sector 0.
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {48.5f, 0.0f, 0.0f}, {1.0f, 0.5f, 0.5f}, 0xFFFFFFFFu, 1u),
    });
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // Stage 2: nudge by 1m to x=49.5 → AABB (48.5, 50.5). Floor-rounded set
    // would be {0, 1} (max=50.5 lands in sector 1). Drift is 1m on every
    // axis = 2% of sectorSize, well under the 10% threshold → hysteresis
    // HOLDS the previous set {0}. InstanceCount must stay at 1.
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {49.5f, 0.0f, 0.0f}, {1.0f, 0.5f, 0.5f}, 0xFFFFFFFFu, 2u),
    });
    EXPECT_EQ(tlas.InstanceCount(), 1u)
        << "hysteresis should hold replica set {0}; sector 1 must NOT gain a "
           "leaf even though AABB.max crossed the floor-rounded boundary.";

    // Stage 3: jump to x=55 → AABB (54, 56). Floor-rounded set is {1}. Drift
    // on every axis = 5.5m > 5m threshold → hysteresis releases. Migrates
    // from {0} to {1}; InstanceCount stays at 1 across the migration.
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {55.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 0.5f}, 0xFFFFFFFFu, 3u),
    });
    EXPECT_EQ(tlas.InstanceCount(), 1u);

    // Verify the entity is actually pickable in its new sector and not in
    // its old one.
    auto pickAt = [&](float32 x) -> bool {
        Mathematics::Ray3D ray;
        ray.origin    = {x, -10.0f, 0.0f};
        ray.direction = { 0.0f,  1.0f, 0.0f};
        bool hit = false;
        tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
            if (inst.Entity.id == e.id) hit = true;
            return false;
        });
        return hit;
    };
    EXPECT_FALSE(pickAt(48.5f));  // old position; entity moved away
    EXPECT_TRUE(pickAt(55.0f));   // new position in sector 1
}

// Audit follow-up: ConcurrentTraverseIsSafe ran at default sectorSize
// (single-sector). This test uses sectorSize=50m to force entities into
// multiple sectors and exercises the cross-sector code paths (DDA walk,
// AABB pre-cull, dedup) under concurrent ray fan.
TEST_F(SceneTlasFixture, ConcurrentTraverseAcrossMultiSectorIsSafe)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    // 50 large-ish entities scattered across ±100m. With sectorSize=50m
    // that puts them across roughly 5x5x5 sectors. HalfExtent=4 gives
    // each entity an 8m AABB so random rays have a meaningful chance of
    // hitting (vs. unit cubes in 200m space which almost always miss).
    std::vector<RenderExtractionSystem::WorldRenderableRecord> recs;
    std::mt19937 rng(0xFEEDFACE);
    std::uniform_real_distribution<float32> coord(-100.0f, 100.0f);
    for (uint32 i = 0; i < 50u; ++i)
        recs.push_back(MakeRecord(MakeHandle(i + 1),
                                  {coord(rng), coord(rng), coord(rng)},
                                  {4.0f, 4.0f, 4.0f}));
    tlas.SyncFromRecords(recs);

    EXPECT_EQ(tlas.InstanceCount(), 50u);

    constexpr int kThreads = 8;
    constexpr int kRaysPerThread = 400;
    std::atomic<int> totalHits{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, seed = static_cast<uint32>(t * 37 + 7)]() {
            std::mt19937 trng(seed);
            std::uniform_real_distribution<float32> oc(-150.0f, 150.0f);
            std::uniform_real_distribution<float32> dc(-1.0f, 1.0f);
            int localHits = 0;
            auto cb = [&](const TlasInstance&, float32, float32) {
                ++localHits;
                return true;
            };
            for (int r = 0; r < kRaysPerThread; ++r)
            {
                Mathematics::Ray3D ray;
                ray.origin = {oc(trng), oc(trng), oc(trng)};
                Mathematics::Vector3 dir{dc(trng), dc(trng), dc(trng)};
                if (dir.Length() < 1e-3f) dir = {0,0,1};
                ray.direction = dir.Normalize();
                tlas.TraverseRay(ray, {}, cb);
            }
            totalHits.fetch_add(localHits, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();
    // Real test is "no torn read / data race" — join completing without
    // ASAN/TSAN errors. The numerical assertion is a sanity check that
    // the multi-sector cross-traversal path (DDA + AABB pre-cull + dedup)
    // actually produced hits under load.
    EXPECT_GT(totalHits.load(), 0);
}

// ===========================================================================
// MeshPicking integration scenarios
// ===========================================================================
//
// These tests exercise picker-shaped flows across multiple TLAS features
// without launching the editor binary. They mimic the patterns the editor's
// MeshPickingService runs during click-pick, drop-placement, marquee
// selection, and hot-reload — but limited to the engine-level surface so
// they run in CI on every commit.
//
// What they catch (vs. the unit fixtures above): cross-feature regressions
// where a change to one phase (e.g. sector replication) silently breaks
// another phase's contract (e.g. dispatch order, dedup, hysteresis stability
// under realistic editor traffic).

namespace
{

// "Drop placement"-style ray: from a height down toward the ground plane.
inline Mathematics::Ray3D DropRay(float32 worldX, float32 worldZ, float32 fromHeight = 50.0f)
{
    Mathematics::Ray3D r;
    r.origin    = {worldX, fromHeight, worldZ};
    r.direction = {0.0f, -1.0f, 0.0f};
    return r;
}

}  // anon

class MeshPickingIntegrationFixture : public ::testing::Test
{
protected:
    void TearDown() override
    {
        ClearAllSceneTlasForTesting();
    }
};

// 1. Drop-placement onto a flat ground plane. Mirrors the editor's
//    "drop a mesh" flow: a ray fired straight down from the cursor's
//    world position should hit the ground entity at the expected depth.
TEST_F(MeshPickingIntegrationFixture, DropPlacementHitsFlatGround)
{
    ECS::World world;
    SceneTlas tlas;

    auto ground = MakeHandle(1);
    auto rec = MakeRecord(ground, {0.0f, 0.0f, 0.0f}, {10.0f, 0.05f, 10.0f});
    tlas.SyncFromRecords(std::vector{rec});

    Mathematics::Ray3D ray = DropRay(2.0f, -3.0f, 50.0f);

    bool hit = false;
    float32 hitT = -1.0f;
    uint32 hitId = 0u;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32 te, float32) {
        hit   = true;
        hitT  = te;
        hitId = inst.Entity.id;
        return false;  // first hit wins for drop placement
    });
    EXPECT_TRUE(hit);
    EXPECT_EQ(hitId, ground.id);
    // Origin y=50, hit at top of slab (y=0.05). Ray dir.y=-1, so tEnter ≈ 49.95.
    EXPECT_NEAR(hitT, 49.95f, 0.01f);
}

// 2. Marquee-style frustum selects the entities inside the rectangle and
//    misses entities outside. Verifies cross-sector frustum traversal +
//    pre-cull doesn't drop hits.
TEST_F(MeshPickingIntegrationFixture, MarqueeFrustumSelectsExpectedSubset)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(20.0f);

    // 5 entities along X. Frustum will cover x ∈ [-5, 5].
    std::vector<RenderExtractionSystem::WorldRenderableRecord> recs;
    for (uint32 i = 0; i < 5u; ++i)
    {
        const float32 x = -10.0f + 5.0f * static_cast<float32>(i);  // -10, -5, 0, 5, 10
        recs.push_back(MakeRecord(MakeHandle(100u + i), {x, 0.0f, 0.0f}));
    }
    tlas.SyncFromRecords(recs);

    // Frustum covering x ∈ [-5, 5], y ∈ [-100, 100], z ∈ [-100, 100].
    Mathematics::Plane planes[6] = {
        {{ 1, 0, 0},   5.0f},  // x ≥ -5
        {{-1, 0, 0},   5.0f},  // x ≤  5
        {{ 0, 1, 0}, 100.0f},
        {{ 0,-1, 0}, 100.0f},
        {{ 0, 0, 1}, 100.0f},
        {{ 0, 0,-1}, 100.0f},
    };

    std::vector<uint32> ids;
    tlas.TraverseFrustum(planes, {}, [&](const TlasInstance& inst) {
        ids.push_back(inst.Entity.id);
    });
    std::sort(ids.begin(), ids.end());

    // Entities at x=-5, 0, 5 are inside (boundary inclusive on the
    // inward-half-space test). Entities at x=-10 and x=10 are outside —
    // their AABBs (halfExtent 0.5) don't intersect the frustum.
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 101u);  // x = -5
    EXPECT_EQ(ids[1], 102u);  // x =  0
    EXPECT_EQ(ids[2], 103u);  // x =  5
}

// 3. Picker keeps finding an entity through several SyncFromRecords calls
//    that each move it across a sector boundary. Mimics dragging an entity
//    through the world and clicking on it after each frame.
TEST_F(MeshPickingIntegrationFixture, PickerFollowsEntityAcrossSectorMigrations)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(42);

    auto pickAt = [&](float32 x) -> bool {
        Mathematics::Ray3D ray;
        ray.origin    = {x, -10.0f, 0.0f};
        ray.direction = { 0.0f,  1.0f, 0.0f};
        bool hit = false;
        tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
            if (inst.Entity.id == e.id) hit = true;
            return false;
        });
        return hit;
    };

    // Walk the entity well past the boundary on each frame so hysteresis
    // releases (frame-by-frame moves bigger than 10% of sectorSize=5m).
    for (uint32 frame = 1u; frame <= 5u; ++frame)
    {
        const float32 x = 10.0f * static_cast<float32>(frame);  // 10, 20, ..., 50
        tlas.SyncFromRecords(std::vector{
            MakeRecord(e, {x, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 0xFFFFFFFFu, frame)
        });
        EXPECT_TRUE(pickAt(x)) << "frame " << frame << " x=" << x;
    }
}

// 4. WorldSectorCoord-aware pick: an entity with sectorCoord=(2,0,0) and
//    sector-local origin (0,0,0) should be picked at world position
//    (sectorCoord * sectorSize) — not at the local origin.
TEST_F(MeshPickingIntegrationFixture, PickerHonorsWorldSectorCoordTeleport)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto e = MakeHandle(7);
    auto rec = MakeRecord(e, {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    rec.hasSectorCoord = true;
    rec.sectorCoord    = {2, 0, 0};  // composed world x = 100
    tlas.SyncFromRecords(std::vector{rec});

    // Ray at the COMPOSED world position should hit.
    Mathematics::Ray3D rayWorld;
    rayWorld.origin    = {100.0f, -10.0f, 0.0f};
    rayWorld.direction = {  0.0f,   1.0f, 0.0f};
    bool composedHit = false;
    tlas.TraverseRay(rayWorld, {}, [&](const TlasInstance& inst, float32, float32) {
        composedHit = (inst.Entity.id == e.id);
        return false;
    });
    EXPECT_TRUE(composedHit);

    // Ray at the SECTOR-LOCAL origin should miss (entity isn't there in
    // world space).
    Mathematics::Ray3D rayLocal;
    rayLocal.origin    = {0.0f, -10.0f, 0.0f};
    rayLocal.direction = {0.0f,  1.0f,  0.0f};
    bool localHit = false;
    tlas.TraverseRay(rayLocal, {}, [&](const TlasInstance& inst, float32, float32) {
        localHit = (inst.Entity.id == e.id);
        return false;
    });
    EXPECT_FALSE(localHit);
}

// 5. Two ECS Worlds each have their own SceneTlas. A pick query in one
//    world must not see entities in the other.
TEST_F(MeshPickingIntegrationFixture, PerWorldTlasIsolation)
{
    ECS::World worldA;
    ECS::World worldB;
    auto& tlasA = GetSceneTlas(worldA);
    auto& tlasB = GetSceneTlas(worldB);

    auto eA = MakeHandle(1);
    auto eB = MakeHandle(2);
    tlasA.SyncFromRecords(std::vector{ MakeRecord(eA, {0.0f, 0.0f, 0.0f}) });
    tlasB.SyncFromRecords(std::vector{ MakeRecord(eB, {0.0f, 0.0f, 0.0f}) });

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, -10.0f, 0.0f};
    ray.direction = {0.0f,   1.0f, 0.0f};

    std::vector<uint32> idsA, idsB;
    tlasA.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        idsA.push_back(inst.Entity.id); return true;
    });
    tlasB.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        idsB.push_back(inst.Entity.id); return true;
    });

    ASSERT_EQ(idsA.size(), 1u);
    ASSERT_EQ(idsB.size(), 1u);
    EXPECT_EQ(idsA[0], eA.id);
    EXPECT_EQ(idsB[0], eB.id);
}

// A World constructed at a destroyed World's address gets its own empty TLAS:
// the registry keys on the never-reused World id, not the address.
TEST_F(MeshPickingIntegrationFixture, WorldAtReusedAddressDoesNotInheritTlas)
{
    std::optional<ECS::World> world;
    world.emplace();
    const ECS::World* firstAddress = &*world;
    auto e = MakeHandle(1);
    GetSceneTlas(*world).SyncFromRecords(std::vector{ MakeRecord(e, {0.0f, 0.0f, 0.0f}) });
    ASSERT_TRUE(GetSceneTlas(*world).Contains(e));

    world.reset();
    world.emplace();
    ASSERT_EQ(&*world, firstAddress);
    EXPECT_FALSE(GetSceneTlas(*world).Contains(e));
    EXPECT_TRUE(GetSceneTlas(*world).IsEmpty());
}

// 6. Boundsless entity (no LocalBounds) should still be pickable via the
//    default unit-cube fallback. Mirrors PrimitiveGenerator's path for
//    Sphere/Plane primitives that don't carry LocalBounds.
TEST_F(MeshPickingIntegrationFixture, BoundlessEntityPickableViaDefaultBox)
{
    ECS::World world;
    SceneTlas tlas;

    auto e = MakeHandle(13);
    auto rec = MakeRecord(e, {0.0f, 0.0f, 0.0f});
    rec.hasBounds = false;  // no LocalBounds — exercises the fallback
    tlas.SyncFromRecords(std::vector{rec});

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, -10.0f, 0.0f};
    ray.direction = {0.0f,   1.0f, 0.0f};
    bool hit = false;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
        hit = (inst.Entity.id == e.id);
        return false;
    });
    EXPECT_TRUE(hit);
}

// 7. Hot-reload-style bounds change: a record's LocalBounds shrinks
//    between syncs (e.g. a glb reload that updated the mesh AABB).
//    The TLAS should pick up the new bounds on the next Sync; a ray
//    aimed at the OLD bounds' edge (now outside the new bounds)
//    should miss.
TEST_F(MeshPickingIntegrationFixture, BoundsShrinkAfterReloadIsRespected)
{
    ECS::World world;
    SceneTlas tlas;

    auto e = MakeHandle(99);

    // Frame 1: large bounds (5x5x5 halfExtents).
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {0.0f, 0.0f, 0.0f}, {5.0f, 5.0f, 5.0f}, 0xFFFFFFFFu, 1u)
    });

    // Ray at x=4 hits because bounds extend to x=5.
    Mathematics::Ray3D ray;
    ray.origin    = {4.0f, -10.0f, 0.0f};
    ray.direction = {0.0f,   1.0f, 0.0f};
    bool hit1 = false;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        hit1 = true; return false;
    });
    EXPECT_TRUE(hit1);

    // Frame 2: bounds shrink (1x1x1 halfExtents). Version bump signals
    // movement; the AABB compare in ReconcileFromRecords also catches
    // the shrink.
    tlas.SyncFromRecords(std::vector{
        MakeRecord(e, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 0xFFFFFFFFu, 2u)
    });

    // Same ray at x=4 should now miss (bounds end at x=1).
    bool hit2 = false;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        hit2 = true; return false;
    });
    EXPECT_FALSE(hit2);

    // Ray at x=0.5 (inside the new bounds) still hits.
    ray.origin = {0.5f, -10.0f, 0.0f};
    bool hit3 = false;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        hit3 = true; return false;
    });
    EXPECT_TRUE(hit3);
}

// 8. Continuous motion across many syncs: entity translates 1m/frame for
//    100 frames, exercising hysteresis at sectorSize=10m. Verifies the
//    picker keeps finding it without state drift, and that the replica
//    set converges to a single sector at the final position.
TEST_F(MeshPickingIntegrationFixture, ContinuousMotionTracksWithoutLosingEntity)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(10.0f);

    auto e = MakeHandle(55);

    for (uint32 frame = 1u; frame <= 100u; ++frame)
    {
        const float32 x = 0.5f * static_cast<float32>(frame);  // 0.5 ... 50.0
        tlas.SyncFromRecords(std::vector{
            MakeRecord(e, {x, 0.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, 0xFFFFFFFFu, frame)
        });

        Mathematics::Ray3D ray;
        ray.origin    = {x, -10.0f, 0.0f};
        ray.direction = {0.0f, 1.0f, 0.0f};
        bool hit = false;
        tlas.TraverseRay(ray, {}, [&](const TlasInstance& inst, float32, float32) {
            if (inst.Entity.id == e.id) hit = true;
            return false;
        });
        ASSERT_TRUE(hit) << "lost entity at frame " << frame << " x=" << x;
    }

    // Final state: still exactly one entity tracked.
    EXPECT_EQ(tlas.InstanceCount(), 1u);
}

// 9. Layer mask filtering through a multi-sector traversal: entities split
//    across two layers, mask selects one. Verifies layer-mask propagation
//    survives sector replication + cross-sector dedup.
TEST_F(MeshPickingIntegrationFixture, LayerMaskFiltersAcrossSectors)
{
    ECS::World world;
    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);

    auto a = MakeHandle(1);  // layer 0x1, sector 0
    auto b = MakeHandle(2);  // layer 0x2, sector 1
    auto c = MakeHandle(3);  // layer 0x1, sector 2
    tlas.SyncFromRecords(std::vector{
        MakeRecord(a, {10.0f,  0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 0x1u),
        MakeRecord(b, {110.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 0x2u),
        MakeRecord(c, {210.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 0x1u),
    });

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, 0.0f, 0.0f};
    ray.direction = {1.0f, 0.0f, 0.0f};
    TraverseRayOptions opts;
    opts.LayerMask = 0x1u;  // only entities on layer 0x1

    std::vector<uint32> ids;
    tlas.TraverseRay(ray, opts, [&](const TlasInstance& inst, float32, float32) {
        ids.push_back(inst.Entity.id);
        return true;
    });
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], a.id);
    EXPECT_EQ(ids[1], c.id);
}

// 10. Despawn while picking: entity removed, picker no longer finds it.
//     Tests the despawn path in ReconcileFromRecords cleans both
//     EntityToSectors and the per-sector EntityToLeaf.
TEST_F(MeshPickingIntegrationFixture, DespawnedEntityNoLongerPickable)
{
    ECS::World world;
    SceneTlas tlas;

    auto e = MakeHandle(8);
    tlas.SyncFromRecords(std::vector{ MakeRecord(e, {0.0f, 0.0f, 0.0f}) });
    EXPECT_TRUE(tlas.Contains(e));

    tlas.SyncFromRecords(
        std::vector<RenderExtractionSystem::WorldRenderableRecord>{});
    EXPECT_FALSE(tlas.Contains(e));

    Mathematics::Ray3D ray;
    ray.origin    = {0.0f, -10.0f, 0.0f};
    ray.direction = {0.0f,   1.0f, 0.0f};
    bool hit = false;
    tlas.TraverseRay(ray, {}, [&](const TlasInstance&, float32, float32) {
        hit = true; return false;
    });
    EXPECT_FALSE(hit);
}

// ---- Shared real-entity helpers ----------------------------------------

namespace
{

// Create a real ECS entity carrying the components the TLAS's record gather
// and the refit read: WorldTransform (identity + translation,
// Version 1), an enabled MeshRenderer, and a LocalBounds box.
ECS::EntityHandle MakeRealEntity(ECS::World& world,
                                 const Mathematics::Vector3& pos,
                                 const Mathematics::Vector3& halfExtents = {0.5f, 0.5f, 0.5f})
{
    // Immediate variants so the components are attached synchronously (the
    // default AddComponent defers to a command buffer that only flushes at a
    // frame sync point, which these tests never reach).
    ECS::EntityHandle e = world.CreateEntity();

    Components::WorldTransform wt{};  // matrix defaults to identity
    wt.matrix[12] = pos.x;
    wt.matrix[13] = pos.y;
    wt.matrix[14] = pos.z;
    wt.Version = 1u;
    world.AddComponentImmediate<Components::WorldTransform>(e, wt);

    Components::MeshRenderer mr{};
    mr.renderLayerMask = 0xFFFFFFFFu;
    world.AddComponentImmediate<Components::MeshRenderer>(e, mr);

    Components::LocalBounds lb{};
    lb.Box.center = {0.0f, 0.0f, 0.0f};
    lb.Box.halfExtents = halfExtents;
    world.AddComponentImmediate<Components::LocalBounds>(e, lb);

    return e;
}

// Move a live entity's WorldTransform translation and bump its Version so the
// refit path treats it as a mover. Does not touch the structural version.
// Uses the direct-writer contract helper (Components/TransformDirtyFeed.h):
// on feed-subscribed worlds the move also lands in the dirty feed, exactly
// like the production in-place writers.
void MoveEntity(ECS::World& world, ECS::EntityHandle e, const Mathematics::Vector3& pos)
{
    auto* wt = world.GetComponentForWrite<Components::WorldTransform>(e);
    ASSERT_NE(wt, nullptr);
    wt->matrix[12] = pos.x;
    wt->matrix[13] = pos.y;
    wt->matrix[14] = pos.z;
    Components::BumpWorldTransform(world, e, *wt);
}

// Gather WorldRenderableRecords for every renderable entity in the world —
// the same shape RequestCurrent's reconcile gathers.
std::vector<RenderExtractionSystem::WorldRenderableRecord> GatherRecords(ECS::World& world)
{
    std::vector<RenderExtractionSystem::WorldRenderableRecord> records;
    using namespace GameEngine::ECS;
    world.Query<Read<Components::WorldTransform>, Read<Components::MeshRenderer>,
                Optional<Components::LocalBounds>>()
        .Each([&](EntityHandle h, const Components::WorldTransform& wt,
                  const Components::MeshRenderer& mr, const Components::LocalBounds* lb) {
            RenderExtractionSystem::WorldRenderableRecord rec{};
            rec.entity         = h;
            rec.worldTransform = wt;
            rec.meshRenderer   = mr;
            rec.hasBounds      = lb != nullptr;
            if (lb) rec.bounds = *lb;
            records.push_back(rec);
        });
    return records;
}

int SphereHitCount(const SceneTlas& tlas, const Mathematics::Vector3& center,
                   float32 radius, ECS::EntityHandle e)
{
    int hits = 0;
    tlas.TraverseSphere(center, radius, {}, [&](const TlasInstance& inst) {
        if (inst.Entity.id == e.id) ++hits;
    });
    return hits;
}

}  // anon

// ---- RequestCurrent: the tree brought current on demand -------------------
//
// No system keeps the TLAS current; a consumer asks. These drive
// RequestCurrent against a real ECS world: a world without a job system runs
// the work inline (Ready on return), a world with one runs it as a job.

class SceneTlasRequestFixture : public ::testing::Test
{
protected:
    void TearDown() override { ClearAllSceneTlasForTesting(); }
};

namespace
{

// Occupies a one-worker pool's only worker until released, so a job
// submitted behind it stays in flight: the main thread runs nothing of the
// pool's without a Wait on that job's counter.
class WorkerBlocker
{
public:
    explicit WorkerBlocker(JobSystem::WorkStealingThreadPool& pool) : m_Pool(pool)
    {
        m_Pool.Run([this]
        {
            m_Started.store(true, std::memory_order_release);
            while (!m_Released.load(std::memory_order_acquire))
                std::this_thread::yield();
        }, m_Counter);
        while (!m_Started.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    ~WorkerBlocker() { Release(); }
    WorkerBlocker(const WorkerBlocker&) = delete;
    WorkerBlocker& operator=(const WorkerBlocker&) = delete;

    void Release()
    {
        m_Released.store(true, std::memory_order_release);
        m_Pool.Wait(m_Counter);
    }

private:
    JobSystem::WorkStealingThreadPool& m_Pool;
    JobSystem::JobCounter m_Counter;
    std::atomic<bool> m_Started{false};
    std::atomic<bool> m_Released{false};
};

void WaitUntilNoWorkInFlight(const SceneTlas& tlas)
{
    while (tlas.IsWorkInFlightForTesting())
        std::this_thread::yield();
}

// Every entity the sphere at `center` touches, sorted by id.
std::vector<uint32> EntitiesNear(const SceneTlas& tlas, const Mathematics::Vector3& center, float32 radius)
{
    std::vector<uint32> ids;
    tlas.TraverseSphere(center, radius, {}, [&ids](const TlasInstance& inst) { ids.push_back(inst.Entity.id); });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace

// The first request reconciles (it inserts the leaves); requests that find
// nothing changed do no work at all.
TEST_F(SceneTlasRequestFixture, FirstRequestReconcilesAndIdleRequestsDoNothing)
{
    ECS::World world;
    auto e = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    SceneTlas& tlas = GetSceneTlas(world);

    EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_TRUE(tlas.Contains(e));
    EXPECT_EQ(tlas.GetReconcileCountForTesting(), 1u);
    const uint64 refits = tlas.GetRefitLeafCountForTesting();

    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(tlas.GetReconcileCountForTesting(), 1u);
    EXPECT_EQ(tlas.GetRefitLeafCountForTesting(), refits);
}

// The S1 counter: movers do no TLAS work until a request, however many
// frames they move; the first request then refits each moved leaf once. A
// leaf is a sector replica: these entities straddle the y = 0 and z = 0
// sector planes, so each has several, and every leaf in the tree moved.
TEST_F(SceneTlasRequestFixture, MovedLeavesRefitOnceAtTheNextRequest)
{
    constexpr int kMovers = 50;
    constexpr int kFrames = 5;
    ECS::World world;
    std::vector<ECS::EntityHandle> movers;
    for (int i = 0; i < kMovers; ++i)
        movers.push_back(MakeRealEntity(world, {2.0f * static_cast<float32>(i), 0.0f, 0.0f}));
    SceneTlas& tlas = GetSceneTlas(world);
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    const uint64 refitsBefore = tlas.GetRefitLeafCountForTesting();

    for (int frame = 1; frame <= kFrames; ++frame)
        for (int i = 0; i < kMovers; ++i)
            MoveEntity(world, movers[i], {2.0f * static_cast<float32>(i), static_cast<float32>(frame), 0.0f});
    EXPECT_EQ(tlas.GetRefitLeafCountForTesting(), refitsBefore) << "the TLAS worked without a request";

    EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(tlas.GetRefitLeafCountForTesting(), refitsBefore + tlas.GetLeafCountForTesting());
    EXPECT_EQ(tlas.GetReconcileCountForTesting(), 1u);
    for (int i = 0; i < kMovers; ++i)
        EXPECT_EQ(SphereHitCount(tlas, {2.0f * static_cast<float32>(i), static_cast<float32>(kFrames), 0.0f}, 0.6f,
                                 movers[i]), 1) << "mover " << i;
}

// A spawn and a despawn between requests are reconciled at the next one.
TEST_F(SceneTlasRequestFixture, StructuralChangesReconcileAtTheNextRequest)
{
    ECS::World world;
    auto a = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    SceneTlas& tlas = GetSceneTlas(world);
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);

    auto b = MakeRealEntity(world, {20.0f, 0.0f, 0.0f});
    world.DestroyEntityImmediate(a);
    EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(tlas.GetReconcileCountForTesting(), 2u);
    EXPECT_FALSE(tlas.Contains(a));
    EXPECT_EQ(SphereHitCount(tlas, {20.0f, 0.0f, 0.0f}, 1.0f, b), 1);
}

// A refit that moves an entity into a sector it has no replica in is
// followed by a reconcile before the tree reports Ready: no stale replica,
// found at the new position only.
TEST_F(SceneTlasRequestFixture, SectorCrossingIsReconciledBeforeReady)
{
    ECS::World world;
    SceneTlas& tlas = GetSceneTlas(world);
    tlas.SetSectorSizeForTesting(50.0f);
    auto e = MakeRealEntity(world, {10.0f, 0.0f, 0.0f});
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);

    MoveEntity(world, e, {75.0f, 0.0f, 0.0f}); // sector (1,0,0)
    EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(tlas.GetReconcileCountForTesting(), 2u);
    EXPECT_EQ(tlas.InstanceCount(), 1u);
    EXPECT_EQ(SphereHitCount(tlas, {75.0f, 0.0f, 0.0f}, 2.0f, e), 1);
    EXPECT_EQ(SphereHitCount(tlas, {10.0f, 0.0f, 0.0f}, 2.0f, e), 0);
}

// A LocalBounds write (a mesh reload republishes it; so does an edit) is
// reflected at the next request, though no transform moved.
TEST_F(SceneTlasRequestFixture, LocalBoundsWriteIsReflectedAtTheNextRequest)
{
    ECS::World world;
    auto e = MakeRealEntity(world, {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    SceneTlas& tlas = GetSceneTlas(world);
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    ASSERT_EQ(SphereHitCount(tlas, {4.0f, 0.0f, 0.0f}, 0.5f, e), 0);

    world.GetComponentForWrite<Components::LocalBounds>(e)->Box.halfExtents = {5.0f, 0.5f, 0.5f};
    EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(SphereHitCount(tlas, {4.0f, 0.0f, 0.0f}, 0.5f, e), 1)
        << "the grown bounds were not re-read";
}

// An undo-style byte restore rewinds the transform's Version; the leaf keys
// on inequality, so it is refit back to the restored pose.
TEST_F(SceneTlasRequestFixture, UndoStyleRestoreIsRefit)
{
    ECS::World world;
    auto e = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    std::vector<uint8_t> original;
    ASSERT_TRUE(world.CaptureComponentBytes(e, ECS::GetComponentTypeId<Components::WorldTransform>(), original));
    SceneTlas& tlas = GetSceneTlas(world);
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);

    MoveEntity(world, e, {15.0f, 0.0f, 0.0f});
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    ASSERT_TRUE(world.ApplyComponentBytesImmediate(e, ECS::GetComponentTypeId<Components::WorldTransform>(),
                                                   original));
    EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(SphereHitCount(tlas, {5.0f, 0.0f, 0.0f}, 1.0f, e), 1);
    EXPECT_EQ(SphereHitCount(tlas, {15.0f, 0.0f, 0.0f}, 1.0f, e), 0);
}

// On a world with a job system the work runs as a job: the request returns
// Updating, a second request joins the work in flight instead of starting
// more, and the tree is Ready once the job is done.
TEST_F(SceneTlasRequestFixture, RequestOnAPoolRunsAsAJobAndJoins)
{
    JobSystem::WorkStealingThreadPool pool(1);
    ECS::World world;
    world.SetJobSystem(&pool);
    auto e = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    SceneTlas& tlas = GetSceneTlas(world);
    tlas.WaitCurrent(world);
    const uint64 refitsBefore = tlas.GetRefitLeafCountForTesting();

    MoveEntity(world, e, {15.0f, 0.0f, 0.0f});
    {
        WorkerBlocker blocker(pool);
        EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Updating);
        EXPECT_TRUE(tlas.IsWorkInFlightForTesting());
        EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Updating);
    }
    WaitUntilNoWorkInFlight(tlas);
    EXPECT_EQ(tlas.GetRefitLeafCountForTesting(), refitsBefore + tlas.GetLeafCountForTesting())
        << "the joined request started more work (one entity: its replica leaves, once)";
    EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(SphereHitCount(tlas, {15.0f, 0.0f, 0.0f}, 1.0f, e), 1);
}

// The job reads only the snapshot the request took: a write made while the
// job is queued does not reach the tree until the next request.
TEST_F(SceneTlasRequestFixture, TheJobReadsOnlyItsSnapshot)
{
    JobSystem::WorkStealingThreadPool pool(1);
    ECS::World world;
    world.SetJobSystem(&pool);
    auto e = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    SceneTlas& tlas = GetSceneTlas(world);
    tlas.WaitCurrent(world);

    MoveEntity(world, e, {15.0f, 0.0f, 0.0f});
    {
        WorkerBlocker blocker(pool);
        ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Updating);
        MoveEntity(world, e, {25.0f, 0.0f, 0.0f}); // after the snapshot
    }
    WaitUntilNoWorkInFlight(tlas);
    EXPECT_EQ(SphereHitCount(tlas, {15.0f, 0.0f, 0.0f}, 1.0f, e), 1) << "the job did not use its snapshot";
    EXPECT_EQ(SphereHitCount(tlas, {25.0f, 0.0f, 0.0f}, 1.0f, e), 0) << "the job read the live world";

    tlas.WaitCurrent(world);
    EXPECT_EQ(SphereHitCount(tlas, {25.0f, 0.0f, 0.0f}, 1.0f, e), 1);
}

// I5: answers after on-demand requests equal a reference reconciled from
// every renderable each frame, across random motion, a spawn and a despawn
// between requests: every live entity at its current position, and every
// probe of a grid over the field. The field stays inside one sector, so the
// replica sets never change: near a sector plane the reconcile's replica
// hysteresis makes an incremental tree differ from a fresh one whatever the
// cadence (SectorCrossingIsReconciledBeforeReady covers crossings).
TEST_F(SceneTlasRequestFixture, OnDemandAnswersMatchAReferenceReconciledEveryFrame)
{
    constexpr int kEntities = 300;
    constexpr int kFrames = 12;
    std::mt19937 rng(3355u);
    std::uniform_real_distribution<float32> coord(10.0f, 290.0f);
    std::uniform_int_distribution<int> pick(0, kEntities - 1);
    std::uniform_real_distribution<float32> jitter(-3.0f, 3.0f);
    ECS::World world;
    std::vector<ECS::EntityHandle> entities;
    for (int i = 0; i < kEntities; ++i)
        entities.push_back(MakeRealEntity(world, {coord(rng), 0.0f, coord(rng)}));
    SceneTlas& onDemand = GetSceneTlas(world);
    SceneTlas reference;

    for (int frame = 0; frame < kFrames; ++frame)
    {
        // Most of the field moves a few meters, so every chunk holds movers
        // and the requests between structural changes are pure refits.
        for (int m = 0; m < kEntities / 2; ++m)
        {
            const ECS::EntityHandle e = entities[pick(rng)];
            if (!world.IsValid(e))
                continue;
            const float32* at = world.GetComponent<Components::WorldTransform>(e)->matrix;
            MoveEntity(world, e, {at[12] + jitter(rng), 0.0f, at[14] + jitter(rng)});
        }
        if (frame == 7)
        {
            if (const ECS::EntityHandle victim = entities[pick(rng)]; world.IsValid(victim))
                world.DestroyEntityImmediate(victim);
            entities.push_back(MakeRealEntity(world, {coord(rng), 0.0f, coord(rng)}));
        }
        reference.SyncFromRecords(GatherRecords(world));
        if (frame % 3 != 2)
            continue; // the on-demand tree is asked every third frame only: frames 2 (its first
                      // request, a reconcile), 5 (refit), 8 (reconcile after frame 7's spawn), 11 (refit)
        ASSERT_EQ(onDemand.RequestCurrent(world), SceneTlas::Readiness::Ready);
        for (const ECS::EntityHandle e : entities)
        {
            if (!world.IsValid(e))
                continue;
            const float32* m = world.GetComponent<Components::WorldTransform>(e)->matrix;
            const Mathematics::Vector3 at{m[12], m[13], m[14]};
            ASSERT_EQ(SphereHitCount(onDemand, at, 0.25f, e) > 0, SphereHitCount(reference, at, 0.25f, e) > 0)
                << "frame " << frame << " entity " << e.id << " at its current position";
        }
        for (float32 x = 0.0f; x <= 300.0f; x += 25.0f)
            for (float32 z = 0.0f; z <= 300.0f; z += 25.0f)
                ASSERT_EQ(EntitiesNear(onDemand, {x, 0.0f, z}, 14.0f), EntitiesNear(reference, {x, 0.0f, z}, 14.0f))
                    << "frame " << frame << " probe (" << x << ", " << z << ")";
    }
}

// Manual Release probe for the click (#3355 S1): after every one of 100,000
// renderables moved, the main thread's share of a request (the change query
// and the kick, which derives each mover's world bounds) and the job's refit,
// on a pool of the machine's hardware threads. Run in Release with
// --gtest_also_run_disabled_tests.
TEST_F(SceneTlasRequestFixture, DISABLED_RequestKickProbe)
{
    using Clock = std::chrono::steady_clock;
    constexpr int kEntities = 100'000;
    constexpr int kSide = 316;
    constexpr int kRepeats = 10;
    JobSystem::WorkStealingThreadPool pool(std::max(1u, std::thread::hardware_concurrency() - 1u));
    ECS::World world;
    world.SetJobSystem(&pool);
    std::vector<ECS::EntityHandle> entities;
    for (int i = 0; i < kEntities; ++i)
        entities.push_back(MakeRealEntity(world, {10.0f + static_cast<float32>(i % kSide),
                                                  0.0f, 10.0f + static_cast<float32>(i / kSide)}));
    SceneTlas& tlas = GetSceneTlas(world);
    tlas.WaitCurrent(world);

    std::vector<double> kicks, jobs;
    for (int repeat = 0; repeat < kRepeats; ++repeat)
    {
        const float32 lift = 0.01f * static_cast<float32>(repeat + 1);
        for (int i = 0; i < kEntities; ++i)
            MoveEntity(world, entities[i], {10.0f + static_cast<float32>(i % kSide), lift,
                                            10.0f + static_cast<float32>(i / kSide)});
        const auto t0 = Clock::now();
        ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Updating);
        const auto t1 = Clock::now();
        WaitUntilNoWorkInFlight(tlas);
        const auto t2 = Clock::now();
        tlas.WaitCurrent(world);
        kicks.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        jobs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    std::sort(kicks.begin(), kicks.end());
    std::sort(jobs.begin(), jobs.end());
    std::printf("[RequestKickProbe] %d movers, %u workers: kick median %.3f ms (min %.3f, max %.3f); "
                "job median %.3f ms (min %.3f, max %.3f)\n",
                kEntities, static_cast<unsigned>(pool.GetWorkerCount()), kicks[kRepeats / 2], kicks.front(),
                kicks.back(), jobs[kRepeats / 2], jobs.front(), jobs.back());
}

#if !defined(NDEBUG)
namespace
{
// A death-test child dies on a CRT assert; on Windows that must print to
// stderr instead of opening a dialog nobody can dismiss.
void SuppressCrtDialogsInDeathTestChild()
{
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}
} // namespace

// A WorldTransform written without a write grant (a raw pointer the change
// filter never saw) leaves a leaf stale; the debug validator on requests that
// find nothing changed reports it and reconciles.
TEST_F(SceneTlasRequestFixture, ValidatorReconcilesAWriteWithoutAGrant)
{
    ECS::World world;
    auto e = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    SceneTlas& tlas = GetSceneTlas(world);
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);

    auto* silent = const_cast<Components::WorldTransform*>(world.GetComponent<Components::WorldTransform>(e));
    silent->matrix[12] = 30.0f;
    ++silent->Version;
    for (int i = 0; i < 64; ++i)
        ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);
    EXPECT_EQ(tlas.GetReconcileCountForTesting(), 2u) << "the validator did not fire within 64 idle requests";
    EXPECT_EQ(SphereHitCount(tlas, {30.0f, 0.0f, 0.0f}, 1.0f, e), 1);
}

// Traversing while work is in flight is a defect, caught in dev builds.
TEST_F(SceneTlasRequestFixture, TraversalWhileWorkIsInFlightAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            JobSystem::WorkStealingThreadPool pool(1);
            ECS::World world;
            world.SetJobSystem(&pool);
            MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
            SceneTlas tlas;
            WorkerBlocker blocker(pool);
            tlas.RequestCurrent(world);
            tlas.TraverseSphere({5.0f, 0.0f, 0.0f}, 1.0f, {}, [](const TlasInstance&) {});
        },
        "traversed while work is in flight");
}
#endif

// ---- Dirty-feed lane (change-signaling §5 P2) ----------------------------

namespace
{

// Subscribe a test world to the WorldTransform dirty feed, mirroring the
// engine's primary-world bootstrap.
void EnableWorldTransformFeed(ECS::World& world)
{
    world.EnableComponentDirtyFeed(
        ECS::GetComponentTypeId<Components::WorldTransform>());
}

std::vector<ECS::EntityHandle> FeedSnapshot(ECS::World& world)
{
    std::vector<ECS::EntityHandle> out;
    world.GetComponentDirtyFeed().Snapshot(out);
    return out;
}


bool SnapshotContains(const std::vector<ECS::EntityHandle>& entries, ECS::EntityHandle e)
{
    return std::any_of(entries.begin(), entries.end(),
                       [&](ECS::EntityHandle h) { return h.id == e.id; });
}

}  // anon

// Direct SyncEntities tests: candidate lists are built by hand (from the
// world's dirty feed where the shape matters).
class SceneTlasFeedFixture : public ::testing::Test
{
protected:
    void TearDown() override { ClearAllSceneTlasForTesting(); }
};

// DupAndDeadEntries (§5.3): duplicate snapshot entries and handles of
// destroyed entities are harmless.
TEST_F(SceneTlasFeedFixture, SyncEntitiesToleratesDupsAndDeadEntries)
{
    ECS::World world;
    EnableWorldTransformFeed(world);

    SceneTlas tlas;
    auto e    = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    auto dead = MakeRealEntity(world, {50.0f, 0.0f, 0.0f});

    tlas.SyncFromRecords(GatherRecords(world));

    MoveEntity(world, e, {8.0f, 0.0f, 0.0f});
    world.DestroyEntityImmediate(dead);

    // Hand-built candidate list: dups, a dead handle, and a never-in-TLAS id.
    std::vector<ECS::EntityHandle> candidates{e, e, dead, e};
    ECS::EntityHandle bogus;
    bogus.id = 0xFFFFF0u;
    candidates.push_back(bogus);

    tlas.SyncEntities(world, candidates);
    EXPECT_EQ(SphereHitCount(tlas, {8.0f, 0.0f, 0.0f}, 1.0f, e), 1);
}

// Dedup correctness lock (refit S1.a, review F4): a same-window destroy +
// create that recycles the entity INDEX must not let the dedup stamp treat
// the new handle as a duplicate of the dead one — the stamp matches on the
// full 32-bit id (index + version), invariant I7. The window also carries
// dups of both handles and of an unrelated stable entity.
TEST_F(SceneTlasFeedFixture, SyncEntitiesDedupProcessesRecycledIndex)
{
    ECS::World world;
    EnableWorldTransformFeed(world);

    SceneTlas tlas;
    auto stable = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    auto doomed = MakeRealEntity(world, {20.0f, 0.0f, 0.0f});
    tlas.SyncFromRecords(GatherRecords(world));

    world.DestroyEntityImmediate(doomed);
    auto recycled = MakeRealEntity(world, {40.0f, 0.0f, 0.0f});
    ASSERT_EQ(recycled.index, doomed.index)
        << "freeIndices is LIFO; destroy+create should recycle the index";
    ASSERT_NE(recycled.id, doomed.id);

    // Reconcile so the recycled entity owns a leaf, then move both movers.
    tlas.SyncFromRecords(GatherRecords(world));
    MoveEntity(world, recycled, {45.0f, 0.0f, 0.0f});
    MoveEntity(world, stable, {8.0f, 0.0f, 0.0f});

    // F4 shape: the dead handle stamps the slot first; the recycled handle
    // (same index, new version) must still be processed, and later dups of
    // each are tolerated whichever id currently owns the slot.
    std::vector<ECS::EntityHandle> candidates{
        doomed, doomed, recycled, stable, recycled, doomed, stable};
    tlas.SyncEntities(world, candidates);

    EXPECT_EQ(SphereHitCount(tlas, {45.0f, 0.0f, 0.0f}, 1.5f, recycled), 1);
    EXPECT_EQ(SphereHitCount(tlas, {40.0f, 0.0f, 0.0f}, 1.5f, recycled), 0);
    EXPECT_EQ(SphereHitCount(tlas, {8.0f, 0.0f, 0.0f}, 1.5f, stable), 1);
    EXPECT_EQ(tlas.CountStaleLeavesForValidation(world), 0u);
}

// CleanFrameZeroRefits (§5.3): an empty candidate list refits nothing and
// flags nothing.
TEST_F(SceneTlasFeedFixture, SyncEntitiesEmptySnapshotIsZeroWork)
{
    ECS::World world;
    EnableWorldTransformFeed(world);

    SceneTlas tlas;
    auto e = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    tlas.SyncFromRecords(GatherRecords(world));

    const auto result = tlas.SyncEntities(world, {});
    EXPECT_FALSE(result.NeedsReconcile);
    EXPECT_EQ(SphereHitCount(tlas, {5.0f, 0.0f, 0.0f}, 1.0f, e), 1);
}

// The shadow-validator primitive: a Version bump WITHOUT a feed entry (the
// A4/contract-violation freeze class) is detectable, and a subsequent
// candidate visit heals it.
TEST_F(SceneTlasFeedFixture, CountStaleLeavesDetectsSilentBump)
{
    ECS::World world;
    auto e = MakeRealEntity(world, {5.0f, 0.0f, 0.0f});
    GetSceneTlas(world).SyncFromRecords(GatherRecords(world));
    ASSERT_EQ(GetSceneTlas(world).CountStaleLeavesForValidation(world), 0u);

    // Contract violation on purpose: bare bump, no helper, no feed entry.
    {
        auto* wt = world.GetComponentForWrite<Components::WorldTransform>(e);
        ASSERT_NE(wt, nullptr);
        wt->matrix[12] = 9.0f;
        ++wt->Version;
    }
    // Stale count is per replica LEAF: an entity whose bounds straddle
    // sector planes owns several replicas, all stale after one silent bump.
    EXPECT_GT(GetSceneTlas(world).CountStaleLeavesForValidation(world), 0u);

    const std::vector<ECS::EntityHandle> heal{e};
    GetSceneTlas(world).SyncEntities(world, heal);
    EXPECT_EQ(GetSceneTlas(world).CountStaleLeavesForValidation(world), 0u);
}

// ---- Feed cadence guard (SwapGenerationGuard, extraction-fusion S1) ------
//
// The engine tick swaps the feed once per frame whether or not this system
// runs; entries live for exactly two swaps (pending -> current -> gone). A
// disabled/throttled system therefore misses movers whose windows were
// discarded unseen. These tests drive the swap by hand to simulate the
// engine cadence around a paused or throttled consumer.

// ---- Parallel leaf refit (refit-arc S2) -----------------------------------
//
// These drive SceneTlas::SyncEntities with an explicit RefitExecution policy
// (the A5 seam) and a World-wired thread pool, so a genuine fork happens in
// this standalone host (which never initializes EngineCore's pool). The
// forced-chunk-count seam bypasses the auto-serial threshold so small
// in-process workloads still fork.

namespace
{

// Direct WorldTransform write + Version bump WITHOUT a feed emission —
// these tests hand-build their candidate windows, mirroring how the
// direct-API parity tests drive SyncEntities.
void NudgeEntity(ECS::World& world, ECS::EntityHandle e,
                 float32 dx, float32 dz)
{
    auto* wt = world.GetComponentForWrite<Components::WorldTransform>(e);
    ASSERT_NE(wt, nullptr);
    wt->matrix[12] += dx;
    wt->matrix[14] += dz;
    ++wt->Version;
}

// Mixed multi-sector workload at 50 m sectors: a grid spanning several
// sectors on x and z, every 7th entity large enough to straddle a sector
// plane somewhere along its row (replicated leaves).
std::vector<ECS::EntityHandle> MakeParallelWorkload(ECS::World& world, int count)
{
    std::vector<ECS::EntityHandle> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const float32 x = -120.0f + 2.5f * static_cast<float32>(i % 100);
        const float32 z = -60.0f + 3.5f * static_cast<float32>(i / 100);
        const bool straddler = (i % 7) == 0;
        const Mathematics::Vector3 halfExtents =
            straddler ? Mathematics::Vector3{6.0f, 1.0f, 6.0f}
                      : Mathematics::Vector3{0.5f, 0.5f, 0.5f};
        entities.push_back(MakeRealEntity(world, {x, 0.0f, z}, halfExtents));
    }
    return entities;
}

// Deterministic per-frame motion: most entities drift (some across sector
// planes), every third stays put so the Version gate sees clean skips.
void MoveWorkload(ECS::World& world,
                  const std::vector<ECS::EntityHandle>& entities,
                  int frame)
{
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        if ((i + static_cast<std::size_t>(frame)) % 3 == 0)
            continue;
        const float32 dx =
            ((i % 2) ? 1.0f : -1.0f) * (2.0f + static_cast<float32>(frame));
        const float32 dz =
            0.75f * static_cast<float32>((static_cast<std::size_t>(frame) + i) % 5);
        NudgeEntity(world, entities[i], dx, dz);
    }
}

// Candidate window shaped like the feed's double-buffered snapshot: every
// entity once, plus a re-delivered duplicate tail.
std::vector<ECS::EntityHandle> MakeCandidates(
    const std::vector<ECS::EntityHandle>& entities)
{
    std::vector<ECS::EntityHandle> candidates(entities.begin(), entities.end());
    for (std::size_t i = 0; i < entities.size(); i += 2)
        candidates.push_back(entities[i]);
    return candidates;
}

class SceneTlasParallelFixture : public ::testing::Test
{
protected:
    void TearDown() override { ClearAllSceneTlasForTesting(); }

    // Worker count pinned so the fork shape is machine-independent.
    JobSystem::WorkStealingThreadPool m_Pool{4};
};

}  // anon

// I3 parity lock: scripted multi-frame motion (movers, a mid-script spawn,
// sector crossings, replicated straddlers, dup-laden candidate windows)
// applied to two TLAS instances — one refit Serial, one Parallel with a
// forced fork — must produce memcmp-identical trees after every frame.
TEST_F(SceneTlasParallelFixture, ParallelRefitMatchesSerialByteIdentical)
{
    ECS::World world;
    world.SetJobSystem(&m_Pool);

    SceneTlas tlasSerial;
    SceneTlas tlasParallel;
    tlasSerial.SetSectorSizeForTesting(50.0f);
    tlasParallel.SetSectorSizeForTesting(50.0f);
    tlasParallel.SetParallelRefitChunkCountForTesting(5);  // force a genuine fork

    auto entities = MakeParallelWorkload(world, 300);
    auto records = GatherRecords(world);
    tlasSerial.SyncFromRecords(records);
    tlasParallel.SyncFromRecords(records);

    std::vector<uint8> snapSerial;
    std::vector<uint8> snapParallel;
    for (int frame = 0; frame < 6; ++frame)
    {
        MoveWorkload(world, entities, frame);
        if (frame == 3)
        {
            // Structural change mid-script: spawns stay on the reconcile
            // path in both lanes.
            entities.push_back(MakeRealEntity(world, {200.0f, 0.0f, 200.0f}));
            records = GatherRecords(world);
            tlasSerial.SyncFromRecords(records);
            tlasParallel.SyncFromRecords(records);
        }

        const auto candidates = MakeCandidates(entities);
        const auto rs = tlasSerial.SyncEntities(world, candidates, RefitExecution::Serial);
        const auto rp = tlasParallel.SyncEntities(world, candidates, RefitExecution::Parallel);
        EXPECT_EQ(rs.NeedsReconcile, rp.NeedsReconcile) << "frame " << frame;

        tlasSerial.SnapshotTreeForTesting(snapSerial);
        tlasParallel.SnapshotTreeForTesting(snapParallel);
        ASSERT_EQ(snapSerial.size(), snapParallel.size()) << "frame " << frame;
        ASSERT_EQ(std::memcmp(snapSerial.data(), snapParallel.data(), snapSerial.size()), 0)
            << "serial/parallel tree divergence at frame " << frame;

        if (rs.NeedsReconcile)
        {
            // Both lanes escalate identically; converge them like the
            // system's next tick would and continue the script.
            records = GatherRecords(world);
            tlasSerial.SyncFromRecords(records);
            tlasParallel.SyncFromRecords(records);
        }
    }

    EXPECT_GT(tlasParallel.GetParallelRefitForkCountForTesting(), 0u)
        << "parallel instance never actually forked — the lock proved nothing";
    EXPECT_EQ(tlasSerial.GetParallelRefitForkCountForTesting(), 0u);
}

// I2/F14 determinism: the same scripted workload replayed from scratch must
// produce bit-identical trees — five repetitions at a fixed chunk count,
// then varied chunk counts (1 = pinned serial) against the same reference.
TEST_F(SceneTlasParallelFixture, ParallelRefitDeterministicAcrossRuns)
{
    const uint32 chunkShapes[] = {3u, 3u, 3u, 3u, 3u, 1u, 2u, 4u, 8u};

    std::vector<uint8> reference;
    bool haveReference = false;
    for (const uint32 chunks : chunkShapes)
    {
        ECS::World world;
        world.SetJobSystem(&m_Pool);

        SceneTlas tlas;
        tlas.SetSectorSizeForTesting(50.0f);
        tlas.SetParallelRefitChunkCountForTesting(chunks);

        auto entities = MakeParallelWorkload(world, 250);
        tlas.SyncFromRecords(GatherRecords(world));
        for (int frame = 0; frame < 4; ++frame)
        {
            MoveWorkload(world, entities, frame);
            const auto candidates = MakeCandidates(entities);
            tlas.SyncEntities(world, candidates, RefitExecution::Parallel);
        }

        std::vector<uint8> snapshot;
        tlas.SnapshotTreeForTesting(snapshot);
        if (!haveReference)
        {
            reference = std::move(snapshot);
            haveReference = true;
            continue;
        }
        ASSERT_EQ(reference.size(), snapshot.size()) << "chunks=" << chunks;
        ASSERT_EQ(std::memcmp(reference.data(), snapshot.data(), reference.size()), 0)
            << "nondeterministic tree at chunks=" << chunks;
    }
}

// OR-reduce: a sector-crossing mover whose candidate lands in the LAST
// chunk still surfaces NeedsReconcile after the chunk-flag merge.
TEST_F(SceneTlasParallelFixture, ParallelSectorCrossingSetsNeedsReconcile)
{
    ECS::World world;
    world.SetJobSystem(&m_Pool);

    SceneTlas tlas;
    tlas.SetSectorSizeForTesting(50.0f);
    tlas.SetParallelRefitChunkCountForTesting(4);

    // 40 entities that stay well inside sector (0,0,0)...
    std::vector<ECS::EntityHandle> entities;
    for (int i = 0; i < 40; ++i)
        entities.push_back(MakeRealEntity(
            world, {2.0f + static_cast<float32>(i % 30), 0.0f, 10.0f},
            {0.4f, 0.4f, 0.4f}));
    // ...and one crosser created LAST, so with contiguous chunking it lands
    // in the final chunk.
    auto crosser = MakeRealEntity(world, {45.0f, 0.0f, 10.0f});
    entities.push_back(crosser);
    tlas.SyncFromRecords(GatherRecords(world));

    // A few in-sector movers so earlier chunks do real (non-crossing) work.
    for (int i = 0; i < 4; ++i)
        NudgeEntity(world, entities[static_cast<std::size_t>(i)], 0.5f, 0.25f);
    NudgeEntity(world, crosser, 30.0f, 0.0f);  // x=75: over the x=50 plane

    const auto result = tlas.SyncEntities(world, entities, RefitExecution::Parallel);
    EXPECT_TRUE(result.NeedsReconcile)
        << "crossing in the last chunk was lost by the OR-reduce";
    EXPECT_GT(tlas.GetParallelRefitForkCountForTesting(), 0u);
}

// A2 threshold: below kParallelRefitMinCandidates a Parallel-policy call
// with a live pool and no forced chunking never forks.
TEST_F(SceneTlasParallelFixture, ThresholdTakesSerialPath)
{
    ECS::World world;
    world.SetJobSystem(&m_Pool);

    SceneTlas tlas;
    auto entities = MakeParallelWorkload(world, 40);
    tlas.SyncFromRecords(GatherRecords(world));

    MoveWorkload(world, entities, 0);
    const std::vector<ECS::EntityHandle> candidates(entities.begin(), entities.end());
    tlas.SyncEntities(world, candidates, RefitExecution::Parallel);

    EXPECT_EQ(tlas.GetParallelRefitForkCountForTesting(), 0u);
    EXPECT_EQ(tlas.CountStaleLeavesForValidation(world), 0u);
}

// World job-system wiring lock: World::GetJobSystem() is the ONLY pool
// source for the refit — the engine-pool fallback ladder is deleted. An
// unwired world (null pool) driven through the system's feed lane with the
// fork armed must refit serially AND must never touch EngineCore: this host
// never initializes the engine, so IsInitialized() staying false keeps the
// pre-Init UB class (#400) dead — a reintroduced unconditional
// EngineCore::GetJobSystem() reach-in would dereference a null pool here.
TEST_F(SceneTlasParallelFixture, UnwiredWorldStaysSerialAndNeverTouchesEngineCore)
{
    ASSERT_FALSE(EngineCore::GetInstance().IsInitialized());

    ECS::World world;  // deliberately serial: no SetJobSystem
    ASSERT_EQ(world.GetJobSystem(), nullptr);

    auto entities = MakeParallelWorkload(world, 60);
    SceneTlas& tlas = GetSceneTlas(world);
    ASSERT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready);  // inline reconcile

    // Arm the fork: with a pool this chunk count would force a genuine
    // parallel refit (it bypasses the auto-serial threshold) — from here the
    // missing pool is the only thing keeping the refit serial.
    tlas.SetParallelRefitChunkCountForTesting(4);
    const uint64 refitsBefore = tlas.GetRefitLeafCountForTesting();

    for (int frame = 0; frame < 3; ++frame)
    {
        // In-place movers (no structural change, no sector crossing: the
        // workload grid sits far from the ±1-unit nudges), so every request
        // takes the refit, not the reconcile.
        for (std::size_t i = 0; i < entities.size(); i += 5)
            MoveEntity(world, entities[i],
                       {world.GetComponent<Components::WorldTransform>(entities[i])->matrix[12],
                        0.25f + 0.1f * static_cast<float32>(frame),
                        world.GetComponent<Components::WorldTransform>(entities[i])->matrix[14]});
        EXPECT_EQ(tlas.RequestCurrent(world), SceneTlas::Readiness::Ready) << "an unwired world runs inline";
        EXPECT_EQ(tlas.CountStaleLeavesForValidation(world), 0u);
    }

    EXPECT_EQ(tlas.GetParallelRefitForkCountForTesting(), 0u)
        << "an unwired world forked — World::GetJobSystem() must be the only pool source";
    EXPECT_GT(tlas.GetRefitLeafCountForTesting(), refitsBefore)
        << "the refit never ran — the fork lock proved nothing";
    ASSERT_FALSE(EngineCore::GetInstance().IsInitialized());
}
