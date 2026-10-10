#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <vector>

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"
#include "Mathematics/Vector3.h"
#include "MeshPicking/MeshBvh.h"

#include "Picking/MeshBvhDiskFormat.h"

#include "TestTempDir.h"

namespace
{

namespace fs = std::filesystem;
using GameEngine::float32;
using GameEngine::uint32;
using GameEngine::uint64;
using GameEngine::GUID;
using GameEngine::Mesh;
using GameEngine::Vertex;
using GameEngine::Mathematics::Vector3;
using GameEngine::MeshPicking::MeshBvh;
using GameEngine::MeshPicking::MeshView;
using GameEngine::MeshPicking::PickHit;
using namespace GameEngine::Editor::Picking;

// Per-test scratch directory, unique to the process and the test. The test
// fixture create-and-removes it so disk state from one test never bleeds into
// another, nor into the same test running in a concurrent ctest process.
class MeshBvhDiskFormatTest : public ::testing::Test
{
protected:
    fs::path CacheRoot;

    void SetUp() override
    {
        CacheRoot = GameEngine::TestUtils::MakeUniqueTempDirectory("GameEngine_MeshBvhDiskFormatTests");
        std::error_code ec;
        fs::create_directories(CacheRoot, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(CacheRoot, ec);
    }
};

// Build a deterministic Mesh of `triCount` random triangles. Same seed -> same
// geometry -> same geometry hash, which lets tests assert that mutating the
// source produces a different hash.
Mesh MakeRandomMesh(uint32 triCount, uint32 seed = 0xC0FFEE)
{
    Mesh m;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> coord(-1.0f, 1.0f);

    m.Vertices.resize(triCount * 3);
    m.Indices.reserve(triCount * 3);
    for (uint32 i = 0; i < triCount; ++i)
    {
        for (uint32 v = 0; v < 3; ++v)
        {
            Vertex& vert = m.Vertices[i * 3 + v];
            vert.Position[0] = coord(rng);
            vert.Position[1] = coord(rng);
            vert.Position[2] = coord(rng);
        }
        m.Indices.push_back(i * 3 + 0);
        m.Indices.push_back(i * 3 + 1);
        m.Indices.push_back(i * 3 + 2);
    }
    return m;
}

MeshView ViewOf(const Mesh& m)
{
    MeshView v;
    v.Positions    = reinterpret_cast<const Vector3*>(&m.Vertices[0].Position[0]);
    v.VertexStride = sizeof(Vertex);
    v.VertexCount  = static_cast<uint32>(m.Vertices.size());
    v.Indices      = m.Indices.data();
    v.IndexCount   = static_cast<uint32>(m.Indices.size());
    return v;
}

GUID MakeTestGuid(GameEngine::uint8 nonce = 1u)
{
    GUID::Data d{};
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = static_cast<GameEngine::uint8>(i ^ nonce);
    return GUID(d);
}

}  // namespace

TEST_F(MeshBvhDiskFormatTest, GeometryHashChangesOnAnyVertexEdit)
{
    Mesh a = MakeRandomMesh(50);
    Mesh b = a;
    b.Vertices[b.Vertices.size() / 2].Position[1] += 0.001f;

    EXPECT_NE(ComputeGeometryHash(ViewOf(a)), ComputeGeometryHash(ViewOf(b)));
}

// A later session rebuilds its view of the same source from scratch, and a build job reads a
// packed copy of the positions rather than the mesh's own vertices. The file written by one
// must load in the other: the key is the geometry, nothing about where or when it was read.
TEST_F(MeshBvhDiskFormatTest, APersistedBvhLoadsInAFreshSessionForTheSameSource)
{
    const Mesh firstSession = MakeRandomMesh(200);
    const auto firstView = ViewOf(firstSession);
    const MeshBvh original = MeshBvh::Build(firstView);
    const GUID guid = MakeTestGuid();
    ASSERT_TRUE(PersistBvh(CacheRoot, guid, 0, ComputeGeometryHash(firstView), original));

    const Mesh secondSession = MakeRandomMesh(200);
    std::vector<Vector3> packed;
    for (const Vertex& vertex : secondSession.Vertices)
        packed.emplace_back(vertex.Position[0], vertex.Position[1], vertex.Position[2]);
    MeshView packedView = ViewOf(secondSession);
    packedView.Positions = packed.data();
    packedView.VertexStride = sizeof(Vector3);

    MeshBvh restored;
    ASSERT_TRUE(TryLoadBvh(CacheRoot, guid, 0, ComputeGeometryHash(packedView), packedView, restored))
        << "the same source read again does not find its persisted BVH";
    std::vector<GameEngine::uint8> originalBytes;
    std::vector<GameEngine::uint8> restoredBytes;
    original.Serialize(originalBytes);
    restored.Serialize(restoredBytes);
    EXPECT_EQ(originalBytes, restoredBytes);
}

TEST_F(MeshBvhDiskFormatTest, PersistAndLoadRoundTrip)
{
    Mesh source = MakeRandomMesh(200);
    auto view = ViewOf(source);
    MeshBvh original = MeshBvh::Build(view);
    ASSERT_FALSE(original.IsEmpty());

    const GUID guid    = MakeTestGuid();
    const uint64 hash  = ComputeGeometryHash(view);

    ASSERT_TRUE(PersistBvh(CacheRoot, guid, /*submesh*/ 0, hash, original));

    MeshBvh restored;
    ASSERT_TRUE(TryLoadBvh(CacheRoot, guid, 0, hash, view, restored));
    EXPECT_EQ(original.TriangleCount(), restored.TriangleCount());

    PickHit hitO, hitR;
    const Vector3 origin(0.0f, 0.0f, -3.0f);
    const Vector3 dir(0.0f, 0.0f, 1.0f);
    EXPECT_EQ(original.Raycast(origin, dir, hitO), restored.Raycast(origin, dir, hitR));
}

TEST_F(MeshBvhDiskFormatTest, RejectsGeometryHashMismatch)
{
    Mesh source = MakeRandomMesh(50);
    auto view = ViewOf(source);
    MeshBvh bvh = MeshBvh::Build(view);
    const GUID guid = MakeTestGuid();

    ASSERT_TRUE(PersistBvh(CacheRoot, guid, 0, /*hash*/ 100ull, bvh));

    MeshBvh out;
    EXPECT_FALSE(TryLoadBvh(CacheRoot, guid, 0, /*hash*/ 200ull, view, out));
    EXPECT_TRUE(out.IsEmpty());
}

TEST_F(MeshBvhDiskFormatTest, RejectsCorruptedHeader)
{
    Mesh source = MakeRandomMesh(50);
    auto view = ViewOf(source);
    MeshBvh bvh = MeshBvh::Build(view);
    const GUID guid    = MakeTestGuid();
    const uint64 hash  = ComputeGeometryHash(view);

    ASSERT_TRUE(PersistBvh(CacheRoot, guid, 0, hash, bvh));

    // Locate the persisted file (one per submesh under <root>/MeshBvh/<guid>/).
    fs::path bvhDir = CacheRoot / "MeshBvh";
    ASSERT_TRUE(fs::exists(bvhDir));
    fs::path persistedFile;
    for (auto& sub : fs::recursive_directory_iterator(bvhDir))
    {
        if (sub.is_regular_file() && sub.path().extension() == ".meshbvh")
        {
            persistedFile = sub.path();
            break;
        }
    }
    ASSERT_FALSE(persistedFile.empty());

    // Corrupt the magic bytes (first 4 bytes of FileHeader).
    {
        std::fstream out(persistedFile, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(out.is_open());
        out.seekp(0);
        const uint32 garbage = 0xDEADBEEFu;
        out.write(reinterpret_cast<const char*>(&garbage), sizeof(garbage));
    }

    MeshBvh outBvh;
    EXPECT_FALSE(TryLoadBvh(CacheRoot, guid, 0, hash, view, outBvh));
}

TEST_F(MeshBvhDiskFormatTest, InvalidateRemovesPersistedFiles)
{
    Mesh source = MakeRandomMesh(50);
    auto view = ViewOf(source);
    MeshBvh bvh = MeshBvh::Build(view);
    const GUID guid = MakeTestGuid();
    const uint64 hash = ComputeGeometryHash(view);

    ASSERT_TRUE(PersistBvh(CacheRoot, guid, 0, hash, bvh));
    ASSERT_TRUE(PersistBvh(CacheRoot, guid, 1, hash, bvh));

    InvalidateBvhCacheFiles(CacheRoot, guid);

    MeshBvh out;
    EXPECT_FALSE(TryLoadBvh(CacheRoot, guid, 0, hash, view, out));
    EXPECT_FALSE(TryLoadBvh(CacheRoot, guid, 1, hash, view, out));
}

TEST_F(MeshBvhDiskFormatTest, MissingFileReturnsFalseWithoutError)
{
    Mesh source = MakeRandomMesh(50);
    auto view = ViewOf(source);

    MeshBvh out;
    // No persist call: file doesn't exist. Should be a clean false, not throw.
    EXPECT_FALSE(TryLoadBvh(CacheRoot, MakeTestGuid(), 0, ComputeGeometryHash(view), view, out));
}

TEST_F(MeshBvhDiskFormatTest, RejectsMutatedSourceMesh)
{
    // Persist with one source, then attempt to load with a *different* source
    // under the same key (forged by passing the original's hash). The root-AABB
    // sanity check inside
    // MeshBvh::Deserialize must still reject it because the rebuilt
    // triangles come from the new source and won't match the stamped root.
    Mesh original = MakeRandomMesh(50, /*seed*/ 1);
    auto viewOrig = ViewOf(original);
    MeshBvh bvh = MeshBvh::Build(viewOrig);
    const GUID guid = MakeTestGuid();
    const uint64 hash = ComputeGeometryHash(viewOrig);

    ASSERT_TRUE(PersistBvh(CacheRoot, guid, 0, hash, bvh));

    Mesh mutated = original;
    // Translate every vertex by a large delta: the root AABB shifts and the
    // geometry hash changes. The stale hash is passed to bypass that gate and
    // exercise the AABB check.
    for (auto& v : mutated.Vertices) v.Position[0] += 100.0f;
    auto viewMut = ViewOf(mutated);

    MeshBvh out;
    EXPECT_FALSE(TryLoadBvh(CacheRoot, guid, 0, hash, viewMut, out));
    EXPECT_TRUE(out.IsEmpty());
}
