#include "Assets/HlodBaker.h"
#include "Assets/HlodCache.h"
#include "Assets/ModelAsset.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Hlod;

namespace {

// A unit triangle used as every member's geometry.
Mesh MakeBakerTriangle() {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    for (int i = 0; i < 3; ++i) {
        Vertex v;
        v.Position[0] = static_cast<float>(i);
        v.Normal[2] = 1.0f;
        v.Tangent[0] = 1.0f; v.Tangent[3] = 1.0f;
        mesh.Vertices.push_back(v);
    }
    mesh.Indices = {0, 1, 2};
    return mesh;
}

MemberInput Member(uint32 tag, float cx, float cy, float cz, uint64 meshHash,
                   const char* material) {
    MemberInput m;
    m.StableId = GUID::Derive(GUID::Null(), "hlod/bake/" + std::to_string(tag));
    m.MeshContentHash = meshHash;
    m.MaterialGuid = GUID::Derive(GUID::Null(), material);
    m.VertexCount = 3u;
    m.ChosenLod = 0u;
    m.CastsShadow = true;
    for (int i = 0; i < 16; ++i) m.WorldMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    m.WorldMatrix[12] = cx; m.WorldMatrix[13] = cy; m.WorldMatrix[14] = cz;
    m.WorldAabbMin[0] = cx - 0.5f; m.WorldAabbMin[1] = cy - 0.5f; m.WorldAabbMin[2] = cz - 0.5f;
    m.WorldAabbMax[0] = cx + 0.5f; m.WorldAabbMax[1] = cy + 0.5f; m.WorldAabbMax[2] = cz + 0.5f;
    return m;
}

GridConfig Config() {
    GridConfig c;
    c.CellSize = 1000.0f;
    c.MaxInstancingRatio = 4.0f;
    c.MinMembers = 8u;
    c.VbBudgetBytes = 1ull << 30;
    return c;
}

} // namespace

TEST(HlodBaker, BakesAdmittedClusterWithProxyGeometry) {
    Mesh tri = MakeBakerTriangle();
    std::vector<MemberInput> members;
    std::vector<const Mesh*> geometry;
    // 12 distinct meshes, 3 materials → admitted (r = 1).
    for (uint32 i = 0; i < 12; ++i) {
        members.push_back(Member(i, static_cast<float>(i), 0.0f, 0.0f, 0x1000 + i,
                                 ("m/" + std::to_string(i % 3)).c_str()));
        geometry.push_back(&tri);
    }

    BakeStats stats;
    HlodBakedScene scene = BakeScene(members, geometry, Config(), &stats);

    EXPECT_EQ(stats.ClusterCount, 1u);
    EXPECT_EQ(stats.AdmittedCount, 1u);
    ASSERT_EQ(scene.Clusters.size(), 1u);

    const BakedCluster& c = scene.Clusters[0];
    EXPECT_EQ(c.Members.size(), 12u);
    // 3 distinct materials → 3 proxy submeshes.
    EXPECT_EQ(c.Submeshes.size(), 3u);
    // Each material bucketed 4 members × 3 verts = 12 verts, 12 indices.
    for (const BakedSubmesh& sm : c.Submeshes) {
        EXPECT_EQ(sm.Vertices.size(), 12u);
        EXPECT_EQ(sm.Indices.size(), 12u);
    }
    EXPECT_NE(scene.Key.SourceHash, 0u);
    EXPECT_EQ(scene.Key.ConfigHash, ComputeHlodConfigHash(Config()));
}

TEST(HlodBaker, HighInstancingClusterProducesNoProxy) {
    Mesh tri = MakeBakerTriangle();
    std::vector<MemberInput> members;
    std::vector<const Mesh*> geometry;
    for (uint32 i = 0; i < 20; ++i) {
        members.push_back(Member(i, 1.0f, 0.0f, 0.0f, 0xAAAA, "m/shared")); // one mesh
        geometry.push_back(&tri);
    }
    BakeStats stats;
    HlodBakedScene scene = BakeScene(members, geometry, Config(), &stats);
    EXPECT_EQ(stats.SkippedHighInstancing, 1u);
    EXPECT_EQ(stats.AdmittedCount, 0u);
    EXPECT_TRUE(scene.Clusters.empty());
}

TEST(HlodBaker, InvalidOwnLevelColourRejectsEntireClusterBeforePublication) {
    Mesh bad = MakeBakerTriangle();
    bad.Color0.assign(12, 1.0f);
    bad.ExtraLODVertices = {bad.Vertices};
    bad.ExtraLODs = {{0, 1, 2}};
    bad.ExtraLODColor0 = {Vector<float>(11, 0.25f)};
    bad.AuthoredLODs = true;
    Mesh sibling = MakeBakerTriangle();
    std::vector<MemberInput> members{Member(0, 50, 50, 50, 1, "bad"), Member(1, 52, 50, 50, 2, "good")};
    members[0].ChosenLod = 1;
    std::vector<const Mesh*> geometry{&bad, &sibling};
    auto config = Config(); config.MinMembers = 2;
    BakeStats stats;
    auto rejected = BakeScene(members, geometry, config, &stats);
    EXPECT_EQ(stats.ClusterCount, 1u);
    EXPECT_EQ(stats.AdmittedCount, 0u);
    EXPECT_EQ(stats.ProxyVertexBytes, 0u);
    EXPECT_TRUE(rejected.Clusters.empty()) << "Neither an empty nor a partial proxy may replace both originals";
    EXPECT_EQ(rejected.Key.SourceHash, 0u);

    // Same cluster and admission inputs with repaired data must actually bake.
    bad.ExtraLODColor0[0].push_back(0.25f);
    auto accepted = BakeScene(members, geometry, config, &stats);
    ASSERT_EQ(accepted.Clusters.size(), 1u);
    EXPECT_EQ(stats.AdmittedCount, 1u);
    ASSERT_EQ(accepted.Clusters[0].Submeshes.size(), 2u);
    bool found = false;
    for (const auto& submesh : accepted.Clusters[0].Submeshes)
        if (submesh.MaterialGuid == members[0].MaterialGuid) {
            found = true;
            EXPECT_EQ(submesh.Color0, bad.ExtraLODColor0[0]);
        }
    EXPECT_TRUE(found);
    EXPECT_NE(accepted.Key.SourceHash, 0u);
}

TEST(HlodBaker, ClusterRelativeBakePreservesSmallVertexMagnitude) {
    Mesh tri = MakeBakerTriangle();
    std::vector<MemberInput> members;
    std::vector<const Mesh*> geometry;
    // Members far from origin (~10000): cluster-relative bake must keep baked
    // vertices near the cluster center, not at 10000.
    for (uint32 i = 0; i < 10; ++i) {
        members.push_back(Member(i, 10000.0f + static_cast<float>(i), 0.0f, 0.0f,
                                 0x1000 + i, ("m/" + std::to_string(i)).c_str()));
        geometry.push_back(&tri);
    }
    HlodBakedScene scene = BakeScene(members, geometry, Config(), nullptr);
    ASSERT_EQ(scene.Clusters.size(), 1u);
    const BakedCluster& c = scene.Clusters[0];
    // Cluster center is ~10004.5; baked vertices must be within a cell of it.
    for (const BakedSubmesh& sm : c.Submeshes)
        for (const Vertex& v : sm.Vertices)
            EXPECT_LT(std::abs(v.Position[0]), 100.0f);
    // The proxy origin (SphereCenter) carries the world offset.
    EXPECT_GT(c.SphereCenter[0], 9000.0f);
}

TEST(HlodBaker, FullBakeWriteReadRoundTrip) {
    Mesh tri = MakeBakerTriangle();
    std::vector<MemberInput> members;
    std::vector<const Mesh*> geometry;
    for (uint32 i = 0; i < 12; ++i) {
        members.push_back(Member(i, static_cast<float>(i), 0.0f, 0.0f, 0x1000 + i,
                                 ("m/" + std::to_string(i % 2)).c_str()));
        geometry.push_back(&tri);
    }
    HlodBakedScene scene = BakeScene(members, geometry, Config(), nullptr);

    const auto file = std::filesystem::temp_directory_path() / "hlod_baker_roundtrip.gehlod";
    ASSERT_TRUE(WriteHlodCache(file, scene));
    const uint64 configHash = scene.Key.ConfigHash;
    HlodBakedScene loaded;
    EXPECT_EQ(ReadHlodCache(file, &configHash, loaded), HlodCacheStatus::Hit);
    EXPECT_EQ(loaded.Key.SourceHash, scene.Key.SourceHash);
    ASSERT_EQ(loaded.Clusters.size(), 1u);
    EXPECT_EQ(loaded.Clusters[0].Submeshes.size(), 2u);
    EXPECT_EQ(loaded.Clusters[0].ClusterSourceHash, scene.Clusters[0].ClusterSourceHash);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
