#include "Assets/MeshLODCache.h"
#include "AssetCore/GUID.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/MeshLODGeometry.h"
#include "Assets/ModelAsset.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace {

// A vertices-only mesh: N degenerate vertices, no LODs. Read validation needs
// the destination mesh to carry the LOD0 vertex count (PostLoad re-parses it
// before the cache lookup), so tests seed it here to mirror that.
Mesh MakeVertsOnly(uint32 vertexCount) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    mesh.Vertices.resize(vertexCount);
    return mesh;
}

// A mesh with hand-authored LOD index buffers over `vertexCount` vertices, so
// the pure-serialization path is exercised without depending on meshoptimizer.
Mesh MakeMeshWithLods(uint32 vertexCount,
                      Vector<Vector<uint32>> lods,
                      Vector<float> errors,
                      Vector<uint8> sloppy) {
    Mesh mesh = MakeVertsOnly(vertexCount);
    mesh.ExtraLODs = std::move(lods);
    mesh.ExtraLODErrors = std::move(errors);
    mesh.ExtraLODSloppy = std::move(sloppy);
    return mesh;
}

// N x N subdivided plane — a smooth, reducible surface for the meshopt-backed
// determinism gates (mirrors the fixture in MeshLODGeneratorTests).
Mesh MakeGridPlane(uint32 n) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    mesh.Vertices.reserve(static_cast<size_t>(n) * n);
    for (uint32 z = 0; z < n; ++z) {
        for (uint32 x = 0; x < n; ++x) {
            Vertex v;
            v.Position[0] = static_cast<float>(x);
            v.Position[2] = static_cast<float>(z);
            v.Normal[1] = 1.0f;
            mesh.Vertices.push_back(v);
        }
    }
    for (uint32 z = 0; z < n - 1; ++z) {
        for (uint32 x = 0; x < n - 1; ++x) {
            const uint32 i0 = z * n + x, i1 = z * n + x + 1;
            const uint32 i2 = (z + 1) * n + x, i3 = (z + 1) * n + x + 1;
            mesh.Indices.insert(mesh.Indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return mesh;
}

std::string ReadAllBytes(const fs::path& p) {
    std::ifstream is(p, std::ios::binary);
    std::ostringstream ss;
    ss << is.rdbuf();
    return ss.str();
}

void WritePrefix(const fs::path& p, const std::string& bytes, size_t count) {
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    os.write(bytes.data(), static_cast<std::streamsize>(count));
}

bool LodsEqual(const Mesh& a, const Mesh& b) {
    if (a.ExtraLODs.size() != b.ExtraLODs.size()) return false;
    if (a.ExtraLODErrors != b.ExtraLODErrors) return false;
    if (a.ExtraLODSloppy != b.ExtraLODSloppy) return false;
    for (size_t i = 0; i < a.ExtraLODs.size(); ++i)
        if (a.ExtraLODs[i] != b.ExtraLODs[i]) return false;
    return true;
}

class MeshLODCacheTest : public ::testing::Test {
protected:
    fs::path m_Dir;

    void SetUp() override {
        const std::string suffix =
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) +
            "_" + GUID::Generate().ToString();
        m_Dir = fs::temp_directory_path() / ("ge_lod_cache_" + suffix);
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
        fs::create_directories(m_Dir, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }
    fs::path File(const char* name) const { return m_Dir / name; }
};

const LodCacheKey kKey{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};

} // namespace

// --- Round-trip -----------------------------------------------------------

TEST_F(MeshLODCacheTest, SyntheticRoundTripPreservesEveryArray) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2, 3, 4, 5}, {0, 2, 4}},
                                      {0.021f, 0.077f}, {0u, 1u}));
    const fs::path file = File("rt.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, ComputeGeneratedLodHash(meshes), meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    ASSERT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Hit);
    EXPECT_TRUE(LodsEqual(meshes[0], dest[0]));
    ASSERT_EQ(dest[0].ExtraLODs.size(), 2u);
    EXPECT_EQ(dest[0].ExtraLODs[0], (Vector<uint32>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(dest[0].ExtraLODSloppy[1], 1u);
    EXPECT_FLOAT_EQ(dest[0].ExtraLODErrors[1], 0.077f);
}

TEST_F(MeshLODCacheTest, RewriteAfterReadIsByteIdentical) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(6, {{0, 1, 2}}, {0.03f}, {0u}));
    meshes.push_back(MakeMeshWithLods(10, {{0, 1, 2, 3, 4, 5}, {0, 4, 8}},
                                      {0.04f, 0.09f}, {0u, 0u}));
    const fs::path file1 = File("a.gelod");
    ASSERT_TRUE(WriteLodCache(file1, kKey, ComputeGeneratedLodHash(meshes), meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(6));
    dest.push_back(MakeVertsOnly(10));
    ASSERT_EQ(ReadLodCacheInto(file1, &kKey, dest), LodCacheStatus::Hit);

    const fs::path file2 = File("b.gelod");
    ASSERT_TRUE(WriteLodCache(file2, kKey, ComputeGeneratedLodHash(dest), dest));
    EXPECT_EQ(ReadAllBytes(file1), ReadAllBytes(file2));
}

TEST_F(MeshLODCacheTest, MixedModelKeepsSubmeshAlignment) {
    // A model where only some submeshes have LODs (skinned/too-small skipped).
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2, 3, 4, 5}}, {0.02f}, {0u}));
    meshes.push_back(MakeVertsOnly(4)); // no LODs
    meshes.push_back(MakeMeshWithLods(12, {{0, 6, 11}}, {0.05f}, {1u}));
    const fs::path file = File("mixed.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, ComputeGeneratedLodHash(meshes), meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    dest.push_back(MakeVertsOnly(4));
    dest.push_back(MakeVertsOnly(12));
    ASSERT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Hit);
    EXPECT_EQ(dest[0].ExtraLODs.size(), 1u);
    EXPECT_TRUE(dest[1].ExtraLODs.empty());
    EXPECT_EQ(dest[2].ExtraLODs.size(), 1u);
}

// --- Key / invalidation ---------------------------------------------------

TEST_F(MeshLODCacheTest, MissingFileIsMiss) {
    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(File("does-not-exist.gelod"), &kKey, dest),
              LodCacheStatus::Missing);
    EXPECT_TRUE(dest[0].ExtraLODs.empty());
}

TEST_F(MeshLODCacheTest, ExactKeyMatchIsHit) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path file = File("k.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    const LodCacheKey same = kKey;
    EXPECT_EQ(ReadLodCacheInto(file, &same, dest), LodCacheStatus::Hit);
}

TEST_F(MeshLODCacheTest, SourceEditIsKeyMismatch) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path file = File("src.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    const LodCacheKey editedSource{kKey.SourceHash ^ 0x1ull, kKey.ConfigHash};
    EXPECT_EQ(ReadLodCacheInto(file, &editedSource, dest), LodCacheStatus::KeyMismatch);
    EXPECT_TRUE(dest[0].ExtraLODs.empty()) << "a miss must not mutate the caller";
}

TEST_F(MeshLODCacheTest, ConfigChangeIsKeyMismatch) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path file = File("cfg.gelod");

    MeshLODConfig cfgA;
    MeshLODConfig cfgB;
    cfgB.TargetRatios[2] = 0.30f; // artist retuned a ratio
    const LodCacheKey keyA{kKey.SourceHash, ComputeLodConfigHash(cfgA, false, 0u)};
    const LodCacheKey keyB{kKey.SourceHash, ComputeLodConfigHash(cfgB, false, 0u)};
    ASSERT_NE(keyA.ConfigHash, keyB.ConfigHash);
    ASSERT_TRUE(WriteLodCache(file, keyA, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(file, &keyB, dest), LodCacheStatus::KeyMismatch);
}

TEST_F(MeshLODCacheTest, VersionBumpIsKeyMismatch) {
    // A meshopt / generator version bump folds into configHash and leaves the
    // config fields unchanged; an old blob's stored configHash then disagrees
    // with the new expected key. Modelled here as a configHash delta with the
    // same source hash — structurally the version-invalidation path.
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path file = File("ver.gelod");
    const LodCacheKey cookedUnderV1{kKey.SourceHash, 0xAAAA0001ull};
    const LodCacheKey expectedV2{kKey.SourceHash, 0xAAAA0002ull};
    ASSERT_TRUE(WriteLodCache(file, cookedUnderV1, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(file, &expectedV2, dest), LodCacheStatus::KeyMismatch);
}

TEST_F(MeshLODCacheTest, ConfigHashFoldsEveryField) {
    const MeshLODConfig base;
    const uint64 baseHash = ComputeLodConfigHash(base, false, 0u);

    MeshLODConfig count = base;
    count.LodCount = 3;
    MeshLODConfig ratios = base;
    ratios.TargetRatios[1] = 0.6f;
    MeshLODConfig errors = base;
    errors.TargetError[1] = 0.03f;
    MeshLODConfig border = base;
    border.BorderRule = MeshLODBorderRule::Free; // off-default so the fold shows
    MeshLODConfig normalW = base;
    normalW.NormalWeight = 0.75f;
    MeshLODConfig uvW = base;
    uvW.UvWeight = 0.75f;
    MeshLODConfig sloppy = base;
    sloppy.SloppyRatioThreshold = 0.2f;

    EXPECT_NE(baseHash, ComputeLodConfigHash(count, false, 0u));
    EXPECT_NE(baseHash, ComputeLodConfigHash(ratios, false, 0u));
    EXPECT_NE(baseHash, ComputeLodConfigHash(errors, false, 0u));
    EXPECT_NE(baseHash, ComputeLodConfigHash(border, false, 0u));
    EXPECT_NE(baseHash, ComputeLodConfigHash(normalW, false, 0u));
    EXPECT_NE(baseHash, ComputeLodConfigHash(uvW, false, 0u));
    EXPECT_NE(baseHash, ComputeLodConfigHash(sloppy, false, 0u));
    EXPECT_NE(baseHash, ComputeLodConfigHash(base, true, 0u)); // generateSkinned folds in
    EXPECT_EQ(baseHash, ComputeLodConfigHash(base, false, 0u)); // stable
}

// Authored admission decides the parsed submesh partition before any generated
// cache is applied, so a change to it must miss every .gelod cooked under the
// old rules. Without the fold a stale cache revalidates as Hit and the old
// partition survives the upgrade.
TEST_F(MeshLODCacheTest, ConfigHashFoldsAuthoredImportVersion) {
    const MeshLODConfig cfg;
    const uint64 current = ComputeLodConfigHash(cfg, false, 0u);

    EXPECT_EQ(ComputeLodConfigHash(cfg, false, 0u, kAuthoredLodImportVersion), current);
    EXPECT_NE(ComputeLodConfigHash(cfg, false, 0u, kAuthoredLodImportVersion + 1u), current);
}

// The parse-options hash (FBX / glTF / Blend loader options) folds into the
// key so a parse change invalidates stale LODs — but a 0 (formats with no
// parse options) leaves the hash byte-identical, so OBJ caches are not
// perturbed.
TEST_F(MeshLODCacheTest, ConfigHashFoldsParseOptionsConditionally) {
    const MeshLODConfig cfg;
    const uint64 noParse = ComputeLodConfigHash(cfg, false, 0u);
    const uint64 optsA = ComputeLodConfigHash(cfg, false, 0x1111222233334444ull);
    const uint64 optsB = ComputeLodConfigHash(cfg, false, 0x5555666677778888ull);

    EXPECT_NE(noParse, optsA) << "a present parse-options hash must change the key";
    EXPECT_NE(optsA, optsB) << "different parse options must give different keys";
    // Idempotent: the 0-sentinel path is stable (OBJ never invalidate).
    EXPECT_EQ(noParse, ComputeLodConfigHash(cfg, false, 0u));
    EXPECT_EQ(optsA, ComputeLodConfigHash(cfg, false, 0x1111222233334444ull));
}

// A cooked sidecar keyed under one FBX parse-options hash is a clean miss when
// the resolved options change (editor path: header key vs expected key).
TEST_F(MeshLODCacheTest, ParseOptionsChangeIsKeyMismatch) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path file = File("parseopts.gelod");

    const MeshLODConfig cfg;
    const LodCacheKey cookedKey{kKey.SourceHash, ComputeLodConfigHash(cfg, false, 0xAAAAull)};
    const LodCacheKey retunedKey{kKey.SourceHash, ComputeLodConfigHash(cfg, false, 0xBBBBull)};
    ASSERT_NE(cookedKey.ConfigHash, retunedKey.ConfigHash);
    ASSERT_TRUE(WriteLodCache(file, cookedKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(file, &retunedKey, dest), LodCacheStatus::KeyMismatch);
    EXPECT_TRUE(dest[0].ExtraLODs.empty());
}

// A packaged Player passes no expected key and trusts format + structure +
// bounds instead; the same blob validates.
TEST_F(MeshLODCacheTest, NullKeyTrustsHeaderStructureAndBounds) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2, 3, 4, 5}}, {0.02f}, {0u}));
    const fs::path file = File("player.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(file, nullptr, dest), LodCacheStatus::Hit);
    EXPECT_EQ(dest[0].ExtraLODs.size(), 1u);
}

// --- Structure mismatch ---------------------------------------------------

TEST_F(MeshLODCacheTest, MeshCountMismatchIsStructureMismatch) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path file = File("mc.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    dest.push_back(MakeVertsOnly(8)); // model now has 2 submeshes
    EXPECT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::StructureMismatch);
    EXPECT_TRUE(dest[0].ExtraLODs.empty());
}

TEST_F(MeshLODCacheTest, VertexCountMismatchIsStructureMismatch) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path file = File("vc.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(6)); // re-parsed geometry changed vertex count
    EXPECT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::StructureMismatch);
    EXPECT_TRUE(dest[0].ExtraLODs.empty());
}

// --- Corruption / A4 bounds -----------------------------------------------

TEST_F(MeshLODCacheTest, CorruptMagicIsRejected) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path good = File("good.gelod");
    ASSERT_TRUE(WriteLodCache(good, kKey, 0, meshes));
    std::string bytes = ReadAllBytes(good);
    bytes[0] = static_cast<char>(bytes[0] ^ 0xFF);
    const fs::path bad = File("badmagic.gelod");
    WritePrefix(bad, bytes, bytes.size());

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(bad, &kKey, dest), LodCacheStatus::FormatMismatch);
    EXPECT_TRUE(dest[0].ExtraLODs.empty());
}

TEST_F(MeshLODCacheTest, CorruptVersionIsRejected) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.02f}, {0u}));
    const fs::path good = File("good.gelod");
    ASSERT_TRUE(WriteLodCache(good, kKey, 0, meshes));
    std::string bytes = ReadAllBytes(good);
    bytes[4] = static_cast<char>(bytes[4] + 7); // bump the format version field
    const fs::path bad = File("badver.gelod");
    WritePrefix(bad, bytes, bytes.size());

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(bad, &kKey, dest), LodCacheStatus::FormatMismatch);
}

TEST_F(MeshLODCacheTest, TooManyLodsIsRejected) {
    // Author more levels than kMaxLODs: the read guard rejects the header count
    // before allocating anything.
    Vector<Vector<uint32>> lods;
    Vector<float> errors;
    Vector<uint8> sloppy;
    for (uint32 i = 0; i < MeshLODConfig::kMaxLODs + 1u; ++i) {
        lods.push_back({0, 1, 2});
        errors.push_back(0.02f);
        sloppy.push_back(0u);
    }
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, std::move(lods), std::move(errors), std::move(sloppy)));
    const fs::path file = File("toomany.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Corrupt);
    EXPECT_TRUE(dest[0].ExtraLODs.empty());
}

TEST_F(MeshLODCacheTest, OutOfRangeIndexIsRejected) {
    // An index == vertexCount is out of range; the write path trusts the
    // generator, so the read bounds check (A4) is the last line of defence
    // before a GPU upload.
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(4, {{0, 1, 4}}, {0.02f}, {0u})); // 4 >= 4
    const fs::path file = File("oob.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, 0, meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(4));
    EXPECT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Corrupt);
    EXPECT_TRUE(dest[0].ExtraLODs.empty());
}

TEST_F(MeshLODCacheTest, TruncationAtEveryOffsetNeverHitsNeverMutates) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(12, {{0, 1, 2, 3, 4, 5}, {0, 6, 11}},
                                      {0.02f, 0.08f}, {0u, 1u}));
    meshes.push_back(MakeMeshWithLods(8, {{0, 1, 2}}, {0.05f}, {0u}));
    const fs::path good = File("full.gelod");
    ASSERT_TRUE(WriteLodCache(good, kKey, 0, meshes));
    const std::string bytes = ReadAllBytes(good);
    ASSERT_GT(bytes.size(), 40u);

    // A process killed mid-write leaves a prefix of arbitrary length. Every
    // proper prefix must be rejected and must leave the caller's meshes intact.
    for (size_t len = 1; len < bytes.size(); ++len) {
        const fs::path partial = File("partial.gelod");
        WritePrefix(partial, bytes, len);

        Vector<Mesh> dest;
        dest.push_back(MakeVertsOnly(12));
        dest.push_back(MakeVertsOnly(8));
        const LodCacheStatus status = ReadLodCacheInto(partial, &kKey, dest);
        EXPECT_NE(status, LodCacheStatus::Hit) << "truncated at " << len << " bytes";
        EXPECT_TRUE(dest[0].ExtraLODs.empty() && dest[1].ExtraLODs.empty())
            << "truncation at " << len << " bytes mutated the caller";
    }
    // The full file is a hit.
    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(12));
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(good, &kKey, dest), LodCacheStatus::Hit);
}

// --- Determinism (F7 byte-identical gate, pinned meshopt) ------------------

TEST_F(MeshLODCacheTest, CookedIsByteIdenticalToFreshRegenerate) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    MeshLODConfig cfg;

    Mesh a = MakeGridPlane(64);
    Mesh b = MakeGridPlane(64);
    ASSERT_GT(GenerateMeshLODsInto(a, cfg), 1u);
    ASSERT_GT(GenerateMeshLODsInto(b, cfg), 1u);
    ASSERT_TRUE(LodsEqual(a, b)) << "meshopt output must be deterministic";

    Vector<Mesh> ma{a};
    Vector<Mesh> mb{b};
    const fs::path fa = File("fresh_a.gelod");
    const fs::path fb = File("fresh_b.gelod");
    const LodCacheKey key{123u, ComputeLodConfigHash(cfg, false, 0u)};
    ASSERT_TRUE(WriteLodCache(fa, key, ComputeGeneratedLodHash(ma), ma));
    ASSERT_TRUE(WriteLodCache(fb, key, ComputeGeneratedLodHash(mb), mb));
    EXPECT_EQ(ReadAllBytes(fa), ReadAllBytes(fb));
}

TEST_F(MeshLODCacheTest, CookedRoundTripEqualsRegenerate) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    MeshLODConfig cfg;

    Mesh generated = MakeGridPlane(64);
    ASSERT_GT(GenerateMeshLODsInto(generated, cfg), 1u);
    Vector<Mesh> cooked{generated};
    const fs::path file = File("gen.gelod");
    const LodCacheKey key{99u, ComputeLodConfigHash(cfg, false, 0u)};
    ASSERT_TRUE(WriteLodCache(file, key, ComputeGeneratedLodHash(cooked), cooked));

    // A fresh load parses LOD0 vertices, then the cache supplies the LODs.
    Mesh parsedOnly = MakeGridPlane(64);
    Vector<Mesh> dest{parsedOnly};
    ASSERT_EQ(ReadLodCacheInto(file, &key, dest), LodCacheStatus::Hit);
    EXPECT_TRUE(LodsEqual(generated, dest[0]));
}

// --- Packaging cook (ModelAsset::CookLODCache) -----------------------------

TEST_F(MeshLODCacheTest, ModelCookWritesConsumableSidecar) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    const MeshLODConfig cfg;

    ModelAsset model(GUID{}, "cook-fixture.glb");
    Vector<Mesh> meshes{MakeGridPlane(64)};
    model.SetMeshesForTest(meshes);

    const fs::path outFile = File("cooked.gelod");
    const uint64 bytes = model.CookLODCache(cfg, /*generateSkinned*/ false, outFile);
    ASSERT_GT(bytes, 0u);
    ASSERT_TRUE(fs::exists(outFile));

    // Cooked bytes match a fresh regenerate, and the Player consumption path
    // (null key — trusts the baked header + bounds) reads it as a hit.
    Mesh fresh = MakeGridPlane(64);
    ASSERT_GT(GenerateMeshLODsInto(fresh, cfg), 1u);
    Vector<Mesh> dest{MakeGridPlane(64)};
    ASSERT_EQ(ReadLodCacheInto(outFile, nullptr, dest), LodCacheStatus::Hit);
    EXPECT_TRUE(LodsEqual(fresh, dest[0]));

    // The cook's key agrees with what a runtime resolve would compute (source
    // hash 0 here — no LoadFromData — plus the resolved config hash).
    const LodCacheKey expected{0u, ComputeLodConfigHash(cfg, false, 0u)};
    Vector<Mesh> dest2{MakeGridPlane(64)};
    EXPECT_EQ(ReadLodCacheInto(outFile, &expected, dest2), LodCacheStatus::Hit);
}

TEST_F(MeshLODCacheTest, ModelCookWithNoLodsWritesNoFile) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    const MeshLODConfig cfg;

    // A skinned-only model with skinned generation off produces no LODs, so the
    // cook writes nothing and the Player stays LOD0-only for it.
    Mesh skinned = MakeGridPlane(32);
    skinned.Skinned = true;
    const size_t verts = skinned.Vertices.size();
    skinned.Joints0.assign(verts * 4, 0);
    skinned.Weights0.assign(verts * 4, 0.0f);
    for (size_t v = 0; v < verts; ++v)
        skinned.Weights0[v * 4] = 1.0f;
    ASSERT_TRUE(skinned.IsSkinned());

    ModelAsset model(GUID{}, "skinned-fixture.glb");
    Vector<Mesh> meshes{skinned};
    model.SetMeshesForTest(meshes);

    const fs::path outFile = File("skinned.gelod");
    EXPECT_EQ(model.CookLODCache(cfg, /*generateSkinned*/ false, outFile), 0u);
    EXPECT_FALSE(fs::exists(outFile));
}

// --- Phase C1: authored submeshes bypass the cook -------------------------

// An authored submesh is cooked as a zero-LOD authored entry; on read the
// reader leaves its parse-supplied chain intact (never clobbers it), while a
// generated sibling in the same file still round-trips.
TEST_F(MeshLODCacheTest, AuthoredSubmeshCookedAsZeroLodAndChainSurvivesRead) {
    Mesh authored = MakeVertsOnly(8);
    authored.ExtraLODVertices.push_back(Vector<Vertex>(4)); // own-vertex LOD1
    authored.ExtraLODs.push_back({0u, 1u, 2u});             // LOD-local indices
    authored.AuthoredLODs = true; // explicit provenance (import funnel stamps it)
    ASSERT_TRUE(authored.HasAuthoredLODs());

    Mesh generated = MakeMeshWithLods(8, {{0, 1, 2, 3, 4, 5}}, {0.03f}, {0u});

    Vector<Mesh> meshes{authored, generated};
    const fs::path file = File("authored.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, ComputeGeneratedLodHash(meshes), meshes));

    // dest mirrors PostLoad: the parse already populated dest[0]'s authored
    // chain; dest[1] is verts-only awaiting the cooked generated LODs.
    Vector<Mesh> dest;
    dest.push_back(authored);         // carries the authored chain
    dest.push_back(MakeVertsOnly(8)); // generated target
    ASSERT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Hit);

    // Authored submesh: chain preserved, not overwritten with zero LODs.
    EXPECT_TRUE(dest[0].HasAuthoredLODs());
    ASSERT_EQ(dest[0].ExtraLODs.size(), 1u);
    EXPECT_EQ(dest[0].ExtraLODs[0], (Vector<uint32>{0u, 1u, 2u}));
    // Generated sibling: applied from the cache.
    ASSERT_EQ(dest[1].ExtraLODs.size(), 1u);
    EXPECT_EQ(dest[1].ExtraLODs[0], (Vector<uint32>{0, 1, 2, 3, 4, 5}));
}

// A v1-header blob must fail the exact version gate rather than be mis-read: the
// authored byte v2 inserts would shift every subsequent submesh field.
TEST_F(MeshLODCacheTest, V1FormatFileMissesV2Reader) {
    const uint32 magic = 0x47454C44u; // "GELD"
    const uint32 v1 = 1u;
    std::string blob;
    blob.append(reinterpret_cast<const char*>(&magic), sizeof(magic));
    blob.append(reinterpret_cast<const char*>(&v1), sizeof(v1));
    const fs::path file = File("v1.gelod");
    WritePrefix(file, blob, blob.size());

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::FormatMismatch);
}

// --- Format v3: generated own-vertex shell levels -------------------------

namespace {

// A distinctive per-face shell block: 6 vertices with recognizable positions,
// normals, UVs, tangents, backing a 2-triangle LOD-local level.
Vector<Vertex> MakeShellBlock() {
    Vector<Vertex> block(6);
    for (size_t i = 0; i < block.size(); ++i) {
        Vertex& v = block[i];
        v.Position[0] = 10.0f + static_cast<float>(i);
        v.Position[1] = 0.5f;
        v.Position[2] = -3.0f;
        v.Normal[1] = 1.0f;
        v.TexCoords[0] = 0.125f * static_cast<float>(i);
        v.TexCoords[1] = 0.875f;
        v.Tangent[0] = 1.0f;
        v.Tangent[3] = -1.0f;
    }
    return block;
}

// Mixed chain: LOD1 index-only over the source vertices, LOD2 an own-vertex
// shell (LOD-local indices). Exactly what generator v4 cooks for a mesh whose
// mid level reduced watertight and whose far level went sloppy.
Mesh MakeMeshWithShellLevel(uint32 vertexCount) {
    Mesh mesh = MakeMeshWithLods(vertexCount, {{0u, 1u, 2u, 0u, 2u, 3u}, {}},
                                 {0.02f, 0.5f}, {0u, 1u});
    mesh.ExtraLODs[1] = {0u, 1u, 2u, 3u, 4u, 5u}; // LOD-local per-face iota
    mesh.ExtraLODVertices.resize(2);
    mesh.ExtraLODVertices[1] = MakeShellBlock();
    return mesh;
}

} // namespace

// Own-vertex shell levels round-trip byte-exactly: LOD-local indices, the
// vertex block, and the parallel error/sloppy markers all survive, and a
// rewrite of the read result is byte-identical to the original file.
TEST_F(MeshLODCacheTest, OwnVertexShellRoundTripsThroughCache) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithShellLevel(8));
    const fs::path file = File("shell.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, ComputeGeneratedLodHash(meshes), meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    ASSERT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Hit);

    // Cache structure validation needs only the base vertex count. Render
    // admission additionally requires an indexed base draw.
    Mesh drawable = dest[0];
    drawable.Indices = {0, 1, 2};
    const auto bounds = ResolveMeshLODGeometry(drawable);
    ASSERT_EQ(bounds.LevelCount, 3u);
    EXPECT_FLOAT_EQ(bounds.Bounds.center.x, 0.0f);
    EXPECT_FLOAT_EQ(bounds.Bounds.halfExtents.x, 15.0f);
    EXPECT_FLOAT_EQ(bounds.ReferenceBounds.Radius(), 0.0f);

    EXPECT_TRUE(LodsEqual(meshes[0], dest[0]));
    ASSERT_EQ(dest[0].ExtraLODVertices.size(), 2u);
    EXPECT_TRUE(dest[0].ExtraLODVertices[0].empty()) << "index-only level owns no block";
    const auto& src = meshes[0].ExtraLODVertices[1];
    const auto& got = dest[0].ExtraLODVertices[1];
    ASSERT_EQ(got.size(), src.size());
    EXPECT_EQ(std::memcmp(got.data(), src.data(), src.size() * sizeof(Vertex)), 0)
        << "shell vertex block must round-trip byte-exactly";
    EXPECT_TRUE(dest[0].HasOwnVertexLODs());
    EXPECT_FALSE(dest[0].HasAuthoredLODs()) << "cache-supplied shells are not authored";

    // Rewrite of the read result reproduces the file byte-for-byte.
    const fs::path file2 = File("shell2.gelod");
    ASSERT_TRUE(WriteLodCache(file2, kKey, ComputeGeneratedLodHash(dest), dest));
    EXPECT_EQ(ReadAllBytes(file), ReadAllBytes(file2));
}

// An all-index-only chain reads back with an EMPTY block array (the canonical
// form GenerateMeshLODsInto produces), never N empty blocks — the GPU content
// hash must agree between the generate and cache-hit paths.
TEST_F(MeshLODCacheTest, IndexOnlyChainReadsBackCanonicalEmptyBlockArray) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithLods(8, {{0u, 1u, 2u}}, {0.02f}, {0u}));
    const fs::path file = File("indexonly.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, ComputeGeneratedLodHash(meshes), meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    // Seed a stale block array to prove the apply clears it.
    dest[0].ExtraLODVertices.resize(3);
    ASSERT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Hit);
    EXPECT_TRUE(dest[0].ExtraLODVertices.empty());
}

// A4 for shells: a LOD-local index at-or-past its own block is corrupt, even
// when it would be in range for the submesh's larger vertex buffer.
TEST_F(MeshLODCacheTest, ShellIndexPastOwnBlockIsCorrupt) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithShellLevel(64)); // submesh has 64 verts
    meshes[0].ExtraLODs[1][5] = 6u; // block has 6 verts; 6 is OOB locally, <64 globally
    const fs::path file = File("oob.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, ComputeGeneratedLodHash(meshes), meshes));

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(64));
    EXPECT_EQ(ReadLodCacheInto(file, &kKey, dest), LodCacheStatus::Corrupt);
    EXPECT_TRUE(dest[0].ExtraLODs.empty()) << "a corrupt blob must not touch the mesh";
}

// A truncation anywhere inside the trailing vertex block must reject the file,
// not hand a short block to the GPU upload.
TEST_F(MeshLODCacheTest, TruncatedShellVertexBlockIsCorrupt) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMeshWithShellLevel(8));
    const fs::path file = File("trunc.gelod");
    ASSERT_TRUE(WriteLodCache(file, kKey, ComputeGeneratedLodHash(meshes), meshes));

    const std::string bytes = ReadAllBytes(file);
    const fs::path cut = File("cut.gelod");
    WritePrefix(cut, bytes, bytes.size() - sizeof(Vertex) / 2); // mid-vertex cut

    Vector<Mesh> dest;
    dest.push_back(MakeVertsOnly(8));
    EXPECT_EQ(ReadLodCacheInto(cut, &kKey, dest), LodCacheStatus::Corrupt);
}
