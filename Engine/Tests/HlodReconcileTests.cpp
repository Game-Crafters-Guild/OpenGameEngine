// HLOD scene reconciliation (design v0.2 §3.3 / F6, §3.4 stale->members). Maps
// baked cluster members (stable GUID) to live entities and flags clusters whose
// members vanished or whose live source hash drifted (material/mesh reassign or
// member move) as stale -> members-only fallback.

#include "Assets/HlodReconcile.h"
#include "Assets/HlodCache.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Hlod;

namespace {

GUID MemberGuid(int i) { return GUID::Derive(GUID::Null(), "member/" + std::to_string(i)); }
GUID MatGuid(int i) { return GUID::Derive(GUID::Null(), "mat/" + std::to_string(i)); }

// Build a baked cluster of `count` members at world positions along +X, cluster
// origin at (cx,0,0). The baked member transforms are cluster-relative (the
// baker's convention).
BakedCluster MakeCluster(int count, float cx) {
    BakedCluster bc;
    bc.SphereCenter[0] = cx;
    bc.SphereRadius = 32.0f;
    for (int i = 0; i < count; ++i) {
        BakedMemberRef ref;
        ref.StableId = MemberGuid(i);
        ref.MaterialGuid = MatGuid(i % 2);
        ref.MeshContentHash = 0x1000u + i;
        ref.MeshHandleKey = 0x2000u + i;
        ref.ChosenLod = 3u;
        // World X = cx + i; cluster-relative X = i.
        for (int k = 0; k < 16; ++k) ref.Transform[k] = (k % 5 == 0) ? 1.0f : 0.0f;
        ref.Transform[12] = static_cast<float>(i); // cluster-relative
        bc.Members.push_back(ref);
    }
    bc.ClusterSourceHash = ComputeClusterSourceHash(bc.Members);
    return bc;
}

// Live member state whose WORLD transform re-derives the baked cluster-relative
// one when the cluster origin (cx) is subtracted.
LiveMemberState LiveMember(GameEngine::ECS::EntityHandle e, const BakedMemberRef& baked, float cx) {
    LiveMemberState s;
    s.Entity = e;
    s.Resolved = baked;
    s.Resolved.Transform[12] = baked.Transform[12] + cx; // world = relative + origin
    return s;
}

GameEngine::ECS::EntityHandle Handle(uint32 id) {
    GameEngine::ECS::EntityHandle h{};
    h.id = id;
    return h;
}

} // namespace

TEST(HlodReconcile, AllMembersResolvedNotStale) {
    HlodBakedScene scene;
    const float cx = 100.0f;
    scene.Clusters.push_back(MakeCluster(4, cx));

    LiveMemberIndex live;
    for (int i = 0; i < 4; ++i)
        live[MemberGuid(i)] = LiveMember(Handle(50u + i), scene.Clusters[0].Members[i], cx);

    ReconcileResult r = ReconcileBakedScene(scene, live);
    ASSERT_EQ(r.Clusters.size(), 1u);
    EXPECT_EQ(r.Clusters[0].Members.size(), 4u);
    EXPECT_EQ(r.Clusters[0].MissingMembers, 0u);
    EXPECT_FALSE(r.Clusters[0].Stale);
    EXPECT_EQ(r.StaleClusters, 0u);
    EXPECT_EQ(r.Clusters[0].Members[0].id, 50u);
    EXPECT_FLOAT_EQ(r.Clusters[0].SphereCenter[0], cx);
}

TEST(HlodReconcile, MissingMemberMarksClusterStale) {
    HlodBakedScene scene;
    const float cx = 100.0f;
    scene.Clusters.push_back(MakeCluster(4, cx));

    LiveMemberIndex live;
    // Drop member 2 (e.g. deleted from the scene).
    for (int i = 0; i < 4; ++i) {
        if (i == 2)
            continue;
        live[MemberGuid(i)] = LiveMember(Handle(50u + i), scene.Clusters[0].Members[i], cx);
    }

    ReconcileResult r = ReconcileBakedScene(scene, live);
    ASSERT_EQ(r.Clusters.size(), 1u);
    EXPECT_EQ(r.Clusters[0].Members.size(), 3u);
    EXPECT_EQ(r.Clusters[0].MissingMembers, 1u);
    EXPECT_TRUE(r.Clusters[0].Stale);
    EXPECT_EQ(r.StaleClusters, 1u);
    EXPECT_EQ(r.TotalMissingMembers, 1u);
}

TEST(HlodReconcile, MaterialReassignmentMarksClusterStale) {
    HlodBakedScene scene;
    const float cx = 100.0f;
    scene.Clusters.push_back(MakeCluster(4, cx));

    LiveMemberIndex live;
    for (int i = 0; i < 4; ++i)
        live[MemberGuid(i)] = LiveMember(Handle(50u + i), scene.Clusters[0].Members[i], cx);
    // Reassign member 1's material (different GUID, same mesh) -> C3 stale.
    live[MemberGuid(1)].Resolved.MaterialGuid = MatGuid(99);

    ReconcileResult r = ReconcileBakedScene(scene, live);
    ASSERT_EQ(r.Clusters.size(), 1u);
    EXPECT_EQ(r.Clusters[0].MissingMembers, 0u); // all resolved
    EXPECT_TRUE(r.Clusters[0].Stale) << "material reassignment must perturb the cluster hash";
}

TEST(HlodReconcile, MemberMoveMarksClusterStale) {
    HlodBakedScene scene;
    const float cx = 100.0f;
    scene.Clusters.push_back(MakeCluster(4, cx));

    LiveMemberIndex live;
    for (int i = 0; i < 4; ++i)
        live[MemberGuid(i)] = LiveMember(Handle(50u + i), scene.Clusters[0].Members[i], cx);
    // Move member 3 in world space -> cluster-relative transform differs -> stale.
    live[MemberGuid(3)].Resolved.Transform[13] += 5.0f;

    ReconcileResult r = ReconcileBakedScene(scene, live);
    EXPECT_EQ(r.Clusters[0].MissingMembers, 0u);
    EXPECT_TRUE(r.Clusters[0].Stale) << "a member move must perturb the cluster hash";
}
