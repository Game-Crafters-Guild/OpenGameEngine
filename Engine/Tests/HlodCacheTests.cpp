#include "Assets/HlodCache.h"
#include "Assets/ModelAsset.h"
#include "Scene/SceneValue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Hlod;
namespace fs = std::filesystem;

namespace {

GUID Guid(const char* s) { return GUID::Derive(GUID::Null(), s); }

BakedMemberRef MakeMember(const char* id, uint64 contentHash, const char* material) {
    BakedMemberRef m;
    m.StableId = Guid(id);
    m.MeshContentHash = contentHash;
    m.MaterialGuid = Guid(material);
    m.MeshHandleKey = 0x1234;
    for (int i = 0; i < 16; ++i) m.Transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    m.Transform[12] = 3.0f; m.Transform[13] = 4.0f; m.Transform[14] = 5.0f;
    m.ChosenLod = 2u;
    m.CastsShadow = 1u;
    return m;
}

BakedSubmesh MakeSubmesh(const char* material, uint32 verts, bool withColor, bool withUv1) {
    BakedSubmesh sm;
    sm.MaterialGuid = Guid(material);
    for (uint32 i = 0; i < verts; ++i) {
        Vertex v;
        v.Position[0] = static_cast<float>(i);
        v.Normal[2] = 1.0f;
        v.Tangent[0] = 1.0f; v.Tangent[3] = 1.0f;
        sm.Vertices.push_back(v);
    }
    // A valid triangle list over the vertices.
    for (uint32 i = 0; i + 2 < verts; ++i)
        sm.Indices.insert(sm.Indices.end(), {i, i + 1, i + 2});
    if (withColor)
        sm.Color0.assign(static_cast<size_t>(verts) * 4u, 0.5f);
    if (withUv1)
        sm.TexCoords1.assign(static_cast<size_t>(verts) * 2u, 0.25f);
    return sm;
}

HlodBakedScene MakeScene() {
    HlodBakedScene scene;
    scene.CellSize = 64.0f;
    scene.GridOrigin[0] = 1.0f; scene.GridOrigin[1] = 2.0f; scene.GridOrigin[2] = 3.0f;

    BakedCluster c0;
    c0.Cell = {1, -2, 3};
    c0.SphereCenter[0] = 10.0f; c0.SphereRadius = 5.5f;
    c0.BoundsMin[0] = 0.0f; c0.BoundsMax[0] = 20.0f;
    c0.Members = {MakeMember("e/0", 0xAA, "m/a"), MakeMember("e/1", 0xBB, "m/b")};
    c0.ClusterSourceHash = ComputeClusterSourceHash(c0.Members);
    c0.Submeshes = {MakeSubmesh("m/a", 6, /*color*/ true, /*uv1*/ false),
                    MakeSubmesh("m/b", 5, /*color*/ false, /*uv1*/ true)};

    BakedCluster c1;
    c1.Cell = {4, 0, 0};
    c1.SphereCenter[0] = 60.0f; c1.SphereRadius = 2.0f;
    c1.Members = {MakeMember("e/2", 0xCC, "m/c")};
    c1.ClusterSourceHash = ComputeClusterSourceHash(c1.Members);
    c1.Submeshes = {MakeSubmesh("m/c", 4, false, false)};

    scene.Clusters = {c0, c1};
    scene.Key.ConfigHash = 0xDEADBEEF;
    scene.Key.SourceHash = 0xC0FFEE;
    return scene;
}

bool SubmeshEqual(const BakedSubmesh& a, const BakedSubmesh& b) {
    if (a.MaterialGuid != b.MaterialGuid) return false;
    if (a.Vertices.size() != b.Vertices.size()) return false;
    if (std::memcmp(a.Vertices.data(), b.Vertices.data(), a.Vertices.size() * sizeof(Vertex)) != 0)
        return false;
    return a.Indices == b.Indices && a.Color0 == b.Color0 && a.TexCoords1 == b.TexCoords1;
}

class HlodCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        const uint64 tok = static_cast<uint64>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        m_Dir = fs::temp_directory_path() / ("hlodcache_" + std::to_string(tok));
        fs::create_directories(m_Dir);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }
    fs::path File() const { return m_Dir / "scene.gehlod"; }
    fs::path m_Dir;
};

std::string ReadAllBytes(const fs::path& p) {
    std::ifstream is(p, std::ios::binary);
    std::ostringstream ss;
    ss << is.rdbuf();
    return ss.str();
}

} // namespace

TEST_F(HlodCacheTest, RoundTripEqualsInMemory) {
    HlodBakedScene scene = MakeScene();
    ASSERT_TRUE(WriteHlodCache(File(), scene));

    HlodBakedScene loaded;
    EXPECT_EQ(ReadHlodCache(File(), nullptr, loaded), HlodCacheStatus::Hit);

    EXPECT_FLOAT_EQ(loaded.CellSize, scene.CellSize);
    EXPECT_EQ(loaded.Key.SourceHash, scene.Key.SourceHash);
    EXPECT_EQ(loaded.Key.ConfigHash, scene.Key.ConfigHash);
    ASSERT_EQ(loaded.Clusters.size(), scene.Clusters.size());
    for (size_t c = 0; c < scene.Clusters.size(); ++c) {
        const BakedCluster& a = scene.Clusters[c];
        const BakedCluster& b = loaded.Clusters[c];
        EXPECT_EQ(a.Cell, b.Cell);
        EXPECT_FLOAT_EQ(a.SphereRadius, b.SphereRadius);
        EXPECT_EQ(a.ClusterSourceHash, b.ClusterSourceHash);
        ASSERT_EQ(a.Members.size(), b.Members.size());
        for (size_t m = 0; m < a.Members.size(); ++m) {
            EXPECT_EQ(a.Members[m].StableId, b.Members[m].StableId);
            EXPECT_EQ(a.Members[m].MaterialGuid, b.Members[m].MaterialGuid);
            EXPECT_EQ(a.Members[m].ChosenLod, b.Members[m].ChosenLod);
        }
        ASSERT_EQ(a.Submeshes.size(), b.Submeshes.size());
        for (size_t s = 0; s < a.Submeshes.size(); ++s)
            EXPECT_TRUE(SubmeshEqual(a.Submeshes[s], b.Submeshes[s]));
    }
}

TEST_F(HlodCacheTest, ConfigHashMismatchRejected) {
    HlodBakedScene scene = MakeScene();
    ASSERT_TRUE(WriteHlodCache(File(), scene));

    const uint64 wrong = 0x2222;
    HlodBakedScene loaded;
    EXPECT_EQ(ReadHlodCache(File(), &wrong, loaded), HlodCacheStatus::KeyMismatch);
    EXPECT_TRUE(loaded.Clusters.empty());

    const uint64 right = scene.Key.ConfigHash;
    EXPECT_EQ(ReadHlodCache(File(), &right, loaded), HlodCacheStatus::Hit);
    EXPECT_EQ(loaded.Key.SourceHash, scene.Key.SourceHash);
}

// The S1 stale-proxy pin: a .gehlod whose ConfigHash was folded with a
// DIFFERENT kLodGeneratorVersion (a bake from before a cook-semantics bump)
// must be rejected by a reader expecting the current version's hash — this is
// the exact check that keeps v(N-1) proxy geometry from silently drawing
// beside v(N) members after a generator upgrade.
TEST_F(HlodCacheTest, WrongLodGeneratorVersionBakeIsRejected) {
    GridConfig config;
    config.CellSize = 64.0f;

    HlodBakedScene scene = MakeScene();
    scene.Key.ConfigHash =
        ComputeHlodConfigHash(config, kFbxImportGeometryVersion, kLodGeneratorVersion - 1u);
    ASSERT_TRUE(WriteHlodCache(File(), scene));

    const uint64 current = ComputeHlodConfigHash(config);
    HlodBakedScene loaded;
    EXPECT_EQ(ReadHlodCache(File(), &current, loaded), HlodCacheStatus::KeyMismatch)
        << "a bake from the previous LOD generator version was accepted";
    EXPECT_TRUE(loaded.Clusters.empty());

    // Same bake re-keyed with the current version: accepted.
    scene.Key.ConfigHash = current;
    ASSERT_TRUE(WriteHlodCache(File(), scene));
    EXPECT_EQ(ReadHlodCache(File(), &current, loaded), HlodCacheStatus::Hit);
}

TEST_F(HlodCacheTest, TruncatedBlobRejected) {
    HlodBakedScene scene = MakeScene();
    ASSERT_TRUE(WriteHlodCache(File(), scene));
    const std::string bytes = ReadAllBytes(File());

    // Truncate to half; must not Hit and must leave `loaded` empty.
    for (size_t cut : {bytes.size() / 2, bytes.size() - 4, size_t{8}}) {
        std::ofstream os(File(), std::ios::binary | std::ios::trunc);
        os.write(bytes.data(), static_cast<std::streamsize>(cut));
        os.close();
        HlodBakedScene loaded;
        const HlodCacheStatus s = ReadHlodCache(File(), nullptr, loaded);
        EXPECT_NE(s, HlodCacheStatus::Hit) << "cut=" << cut;
        EXPECT_TRUE(loaded.Clusters.empty());
    }
}

TEST_F(HlodCacheTest, WrongMagicIsFormatMismatch) {
    std::ofstream os(File(), std::ios::binary | std::ios::trunc);
    const uint32 garbage[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    os.write(reinterpret_cast<const char*>(garbage), sizeof(garbage));
    os.close();
    HlodBakedScene loaded;
    EXPECT_EQ(ReadHlodCache(File(), nullptr, loaded), HlodCacheStatus::FormatMismatch);
}

TEST_F(HlodCacheTest, OutOfBoundsIndexRejected) {
    HlodBakedScene scene = MakeScene();
    // Corrupt an index to reference a non-existent vertex.
    scene.Clusters[1].Submeshes[0].Indices[0] = 9999u;
    ASSERT_TRUE(WriteHlodCache(File(), scene));
    HlodBakedScene loaded;
    EXPECT_EQ(ReadHlodCache(File(), nullptr, loaded), HlodCacheStatus::Corrupt);
    EXPECT_TRUE(loaded.Clusters.empty());
}

TEST_F(HlodCacheTest, MissingFileReported) {
    HlodBakedScene loaded;
    EXPECT_EQ(ReadHlodCache(m_Dir / "nope.gehlod", nullptr, loaded), HlodCacheStatus::Missing);
}

// --- Bake-key sensitivity (C3) ---

TEST(HlodClusterSourceHash, PerturbingAnyMemberFieldChangesHash) {
    std::vector<BakedMemberRef> base = {MakeMember("e/0", 0xAA, "m/a"),
                                        MakeMember("e/1", 0xBB, "m/b")};
    const uint64 h0 = ComputeClusterSourceHash(base);

    auto perturbed = base;
    perturbed[0].Transform[12] += 1.0f; // moved member
    EXPECT_NE(ComputeClusterSourceHash(perturbed), h0);

    perturbed = base;
    perturbed[0].MeshContentHash ^= 0x1; // content changed
    EXPECT_NE(ComputeClusterSourceHash(perturbed), h0);

    perturbed = base;
    perturbed[0].MaterialGuid = Guid("m/reassigned"); // material reassignment (C3)
    EXPECT_NE(ComputeClusterSourceHash(perturbed), h0);

    perturbed = base;
    perturbed[0].MeshHandleKey ^= 0x1; // mesh handle / submesh selector
    EXPECT_NE(ComputeClusterSourceHash(perturbed), h0);

    perturbed = base;
    perturbed[0].ChosenLod += 1; // chosen LOD
    EXPECT_NE(ComputeClusterSourceHash(perturbed), h0);

    // Unrelated: re-hashing the untouched set is stable.
    EXPECT_EQ(ComputeClusterSourceHash(base), h0);
}

TEST(HlodClusterSourceHash, SubGridTransformNoiseDoesNotStale) {
    std::vector<BakedMemberRef> base = {MakeMember("e/0", 0xAA, "m/a")};
    const uint64 h0 = ComputeClusterSourceHash(base);
    auto jittered = base;
    jittered[0].Transform[12] += 1e-6f; // below the 1/4096 quantization grid
    EXPECT_EQ(ComputeClusterSourceHash(jittered), h0);
}

// A reload recomputes the C3 hash from WorldTransforms parsed back out of the
// scene file, and serialization drift lives in the world-ABSOLUTE translation
// (the cluster-origin subtraction cannot cancel it). The scene serializer writes
// shortest-exact (std::to_chars via FormatFloat3) and parses correctly-rounded
// (strtof), so the round trip must be bit-exact even at large world coordinates —
// otherwise an unmodified scene would reload permanently members-only.
TEST(HlodClusterSourceHash, StableAcrossSceneFloatRoundTripAtLargeCoordinates) {
    const float coords[] = {1500.0f,
                            1234.5678f,
                            -8191.03125f,
                            std::nextafterf(1500.0f, 2000.0f),
                            99999.984375f,
                            0.1f};
    for (float x : coords) {
        const std::string text = Scene::FormatFloat3(x, x * 0.5f, -x);
        Scene::Float3 back{};
        ASSERT_TRUE(Scene::ParseFloat3(text, back)) << text;
        EXPECT_EQ(std::bit_cast<uint32>(back.X), std::bit_cast<uint32>(x)) << text;
        EXPECT_EQ(std::bit_cast<uint32>(back.Y), std::bit_cast<uint32>(x * 0.5f)) << text;
        EXPECT_EQ(std::bit_cast<uint32>(back.Z), std::bit_cast<uint32>(-x)) << text;
    }

    // Hash level: a member translated ~1.5 km from the origin keeps an identical
    // cluster source hash after its translation makes the save→load round trip.
    std::vector<BakedMemberRef> base = {MakeMember("e/far", 0xAA, "m/a")};
    base[0].Transform[12] = 1500.03125f;
    base[0].Transform[13] = 12.7f;
    base[0].Transform[14] = -1499.4f;
    const uint64 h0 = ComputeClusterSourceHash(base);

    auto reloaded = base;
    const std::string text = Scene::FormatFloat3(
        base[0].Transform[12], base[0].Transform[13], base[0].Transform[14]);
    Scene::Float3 parsed{};
    ASSERT_TRUE(Scene::ParseFloat3(text, parsed));
    reloaded[0].Transform[12] = parsed.X;
    reloaded[0].Transform[13] = parsed.Y;
    reloaded[0].Transform[14] = parsed.Z;
    EXPECT_EQ(ComputeClusterSourceHash(reloaded), h0);
}

TEST(HlodConfigHash, PerturbingConfigChangesHash) {
    GridConfig base;
    const uint64 h0 = ComputeHlodConfigHash(base);

    GridConfig c = base; c.CellSize += 1.0f;
    EXPECT_NE(ComputeHlodConfigHash(c), h0);
    c = base; c.MaxInstancingRatio += 1.0f;
    EXPECT_NE(ComputeHlodConfigHash(c), h0);
    c = base; c.MinMembers += 1;
    EXPECT_NE(ComputeHlodConfigHash(c), h0);
    c = base; c.VbBudgetBytes += 1;
    EXPECT_NE(ComputeHlodConfigHash(c), h0);
    c = base; c.GridOrigin[1] += 1.0f;
    EXPECT_NE(ComputeHlodConfigHash(c), h0);

    EXPECT_EQ(ComputeHlodConfigHash(base), h0);
}

TEST(HlodConfigHash, FoldsFbxGeometryVersion) {
    // The proxy is merged from FBX-imported member meshes, so a loader-geometry
    // bump must invalidate stale .gehlod proxies. The default binds the current
    // salt; a different version must produce a different key.
    GridConfig base;
    const uint64 current = ComputeHlodConfigHash(base);
    EXPECT_EQ(ComputeHlodConfigHash(base, kFbxImportGeometryVersion), current);
    EXPECT_NE(ComputeHlodConfigHash(base, kFbxImportGeometryVersion + 1u), current);
}

TEST(HlodConfigHash, FoldsLodGeneratorVersion) {
    // The proxy bakes each member's coarsest COOKED LOD, so a .gelod cook-
    // semantics bump (kLodGeneratorVersion) must invalidate .gehlod bakes built
    // from the old cooks — otherwise a bake keyed only on model source + config
    // revalidates as Hit and keeps stale proxy geometry at far field.
    GridConfig base;
    const uint64 current = ComputeHlodConfigHash(base);
    EXPECT_EQ(ComputeHlodConfigHash(base, kFbxImportGeometryVersion, kLodGeneratorVersion),
              current);
    EXPECT_NE(ComputeHlodConfigHash(base, kFbxImportGeometryVersion, kLodGeneratorVersion + 1u),
              current);
}

TEST(HlodConfigHash, FoldsBakerVersion) {
    // kHlodBakerVersion marks a change in the proxy geometry the baker produces
    // for unchanged inputs. Without the fold, every proxy cooked by the previous
    // baker revalidates as Hit and the old geometry stays at far field.
    GridConfig base;
    const uint64 current = ComputeHlodConfigHash(base);
    EXPECT_EQ(ComputeHlodConfigHash(base, kFbxImportGeometryVersion, kLodGeneratorVersion,
                                    kHlodBakerVersion),
              current);
    EXPECT_NE(ComputeHlodConfigHash(base, kFbxImportGeometryVersion, kLodGeneratorVersion,
                                    kHlodBakerVersion + 1u),
              current);
}
