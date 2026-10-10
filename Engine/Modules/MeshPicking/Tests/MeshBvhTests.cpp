#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "Mathematics/Vector3.h"
#include "MeshPicking/MeshBvh.h"

namespace
{

using GameEngine::uint32;
using GameEngine::float32;
using GameEngine::Mathematics::Vector3;
using GameEngine::MeshPicking::MeshBvh;
using GameEngine::MeshPicking::MeshView;
using GameEngine::MeshPicking::PickHit;
using GameEngine::MeshPicking::RayMesh;
using GameEngine::MeshPicking::RayTriangle;

// Build a MeshView over tightly-packed Vector3 positions and a uint32 index list.
MeshView MakeView(const std::vector<Vector3>& positions, const std::vector<uint32>& indices)
{
    MeshView v;
    v.Positions    = positions.data();
    v.VertexStride = sizeof(Vector3);
    v.VertexCount  = static_cast<uint32>(positions.size());
    v.Indices      = indices.data();
    v.IndexCount   = static_cast<uint32>(indices.size());
    return v;
}

}

TEST(MeshPickingRayTriangle, HitsKnownTriangle)
{
    // Triangle in z=1 plane, axis-aligned.
    const Vector3 v0(0.0f, 0.0f, 1.0f);
    const Vector3 v1(1.0f, 0.0f, 1.0f);
    const Vector3 v2(0.0f, 1.0f, 1.0f);

    // Ray straight at (0.25, 0.25, 1) from the origin along +Z.
    const Vector3 origin(0.25f, 0.25f, 0.0f);
    const Vector3 dir   (0.0f,  0.0f,  1.0f);

    float32 t = 0.0f, u = 0.0f, v = 0.0f;
    ASSERT_TRUE(RayTriangle(origin, dir, v0, v1, v2, t, u, v));
    EXPECT_NEAR(t, 1.0f, 1e-5f);
    // Barycentrics: hit = v0 + u*(v1-v0) + v*(v2-v0). For (0.25,0.25,1):
    // 0.25 = 0 + u*1 + v*0  => u = 0.25
    // 0.25 = 0 + u*0 + v*1  => v = 0.25
    EXPECT_NEAR(u, 0.25f, 1e-5f);
    EXPECT_NEAR(v, 0.25f, 1e-5f);
}

TEST(MeshPickingRayTriangle, MissesOutsideTriangle)
{
    const Vector3 v0(0.0f, 0.0f, 1.0f);
    const Vector3 v1(1.0f, 0.0f, 1.0f);
    const Vector3 v2(0.0f, 1.0f, 1.0f);

    // Outside the triangle (negative x).
    const Vector3 origin(-0.5f, 0.5f, 0.0f);
    const Vector3 dir   (0.0f,  0.0f,  1.0f);

    float32 t = 0.0f, u = 0.0f, v = 0.0f;
    EXPECT_FALSE(RayTriangle(origin, dir, v0, v1, v2, t, u, v));
}

TEST(MeshPickingRayTriangle, RejectsBehindOrigin)
{
    const Vector3 v0(0.0f, 0.0f, -1.0f);
    const Vector3 v1(1.0f, 0.0f, -1.0f);
    const Vector3 v2(0.0f, 1.0f, -1.0f);

    const Vector3 origin(0.25f, 0.25f, 0.0f);
    const Vector3 dir   (0.0f,  0.0f,  1.0f);   // ray points away from triangle

    float32 t = 0.0f, u = 0.0f, v = 0.0f;
    EXPECT_FALSE(RayTriangle(origin, dir, v0, v1, v2, t, u, v));
}

TEST(MeshPickingRayMesh, EmptyMeshMisses)
{
    MeshView empty{};
    PickHit hit;
    EXPECT_FALSE(RayMesh(empty, Vector3(0,0,0), Vector3(0,0,1), hit));
}

TEST(MeshPickingRayMesh, ReturnsClosestHitOfMultipleTriangles)
{
    // Two parallel triangles at z=1 and z=2. Ray along +Z must hit the nearer one.
    std::vector<Vector3> verts = {
        {0,0,1}, {1,0,1}, {0,1,1},   // triangle 0 at z=1
        {0,0,2}, {1,0,2}, {0,1,2},   // triangle 1 at z=2
    };
    std::vector<uint32> idx = { 0,1,2,  3,4,5 };
    auto view = MakeView(verts, idx);

    PickHit hit;
    ASSERT_TRUE(RayMesh(view, Vector3(0.25f, 0.25f, 0.0f), Vector3(0,0,1), hit));
    EXPECT_EQ(hit.TriangleIndex, 0u);
    EXPECT_NEAR(hit.Distance, 1.0f, 1e-5f);
}

TEST(MeshBvh, BuiltBvhMatchesBruteForceOnRandomRays)
{
    // Mesh of 200 random triangles in a unit box.
    std::mt19937 rng(0xC0FFEE);
    std::uniform_real_distribution<float32> coord(-1.0f, 1.0f);

    constexpr uint32 kTriCount = 200;
    std::vector<Vector3> verts;
    verts.reserve(kTriCount * 3);
    std::vector<uint32> idx;
    idx.reserve(kTriCount * 3);
    for (uint32 i = 0; i < kTriCount; ++i)
    {
        verts.emplace_back(coord(rng), coord(rng), coord(rng));
        verts.emplace_back(coord(rng), coord(rng), coord(rng));
        verts.emplace_back(coord(rng), coord(rng), coord(rng));
        idx.push_back(i * 3 + 0);
        idx.push_back(i * 3 + 1);
        idx.push_back(i * 3 + 2);
    }
    auto view = MakeView(verts, idx);

    MeshBvh bvh = MeshBvh::Build(view);
    ASSERT_FALSE(bvh.IsEmpty());
    EXPECT_EQ(bvh.TriangleCount(), kTriCount);

    // 100 random rays through the bounding region.
    std::uniform_real_distribution<float32> origCoord(-3.0f, 3.0f);
    std::uniform_real_distribution<float32> dirCoord (-1.0f, 1.0f);

    int hitCount = 0;
    for (int i = 0; i < 100; ++i)
    {
        Vector3 origin(origCoord(rng), origCoord(rng), origCoord(rng));
        Vector3 dir   (dirCoord(rng),  dirCoord(rng),  dirCoord(rng));
        if (dir.Length() < 1e-3f) dir = Vector3(0, 0, 1);
        dir = dir.Normalize();

        PickHit hitBrute, hitBvh;
        const bool b = RayMesh(view, origin, dir, hitBrute);
        const bool v = bvh.Raycast(origin, dir, hitBvh);

        ASSERT_EQ(b, v) << "BVH and brute-force disagree on ray " << i;
        if (b)
        {
            ++hitCount;
            EXPECT_NEAR(hitBrute.Distance, hitBvh.Distance, 1e-4f) << "ray " << i;
            EXPECT_EQ(hitBrute.TriangleIndex, hitBvh.TriangleIndex) << "ray " << i;
        }
    }
    // Sanity: at least some rays should have hit.
    EXPECT_GT(hitCount, 0);
}

TEST(MeshBvh, RespectsCustomVertexStride)
{
    // Simulate an interleaved Vertex layout: position followed by 3 floats of padding.
    struct InterleavedVertex
    {
        Vector3 Position;
        float32 Padding[3];
    };

    std::vector<InterleavedVertex> verts(3);
    verts[0].Position = Vector3(0,0,1);
    verts[1].Position = Vector3(1,0,1);
    verts[2].Position = Vector3(0,1,1);
    std::vector<uint32> idx = {0, 1, 2};

    MeshView view;
    view.Positions    = &verts[0].Position;
    view.VertexStride = sizeof(InterleavedVertex);
    view.VertexCount  = 3;
    view.Indices      = idx.data();
    view.IndexCount   = 3;

    MeshBvh bvh = MeshBvh::Build(view);
    ASSERT_FALSE(bvh.IsEmpty());
    EXPECT_EQ(bvh.TriangleCount(), 1u);

    PickHit hit;
    ASSERT_TRUE(bvh.Raycast(Vector3(0.25f, 0.25f, 0.0f), Vector3(0,0,1), hit));
    EXPECT_NEAR(hit.Distance, 1.0f, 1e-5f);
    EXPECT_EQ(hit.TriangleIndex, 0u);
}

TEST(MeshBvh, RootBoundsCoverAllTriangles)
{
    std::vector<Vector3> verts = {
        {-2.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f},
    };
    std::vector<uint32> idx = {0, 1, 2};
    auto view = MakeView(verts, idx);

    MeshBvh bvh = MeshBvh::Build(view);
    ASSERT_FALSE(bvh.IsEmpty());

    auto bounds = bvh.RootBounds();
    EXPECT_LE(bounds.min.x, -2.0f);
    EXPECT_GE(bounds.max.x,  2.0f);
    EXPECT_LE(bounds.min.y,  0.0f);
    EXPECT_GE(bounds.max.y,  3.0f);
}

TEST(MeshBvh, SerializeDeserializeRoundTrip)
{
    // 200 random triangles — large enough to exercise BVH internal node
    // structure (not a trivial leaf-only tree).
    std::mt19937 rng(0xFEEDFACE);
    std::uniform_real_distribution<float32> coord(-1.0f, 1.0f);

    constexpr uint32 kTriCount = 200;
    std::vector<Vector3> verts;
    verts.reserve(kTriCount * 3);
    std::vector<uint32> idx;
    idx.reserve(kTriCount * 3);
    for (uint32 i = 0; i < kTriCount; ++i)
    {
        verts.emplace_back(coord(rng), coord(rng), coord(rng));
        verts.emplace_back(coord(rng), coord(rng), coord(rng));
        verts.emplace_back(coord(rng), coord(rng), coord(rng));
        idx.push_back(i * 3 + 0);
        idx.push_back(i * 3 + 1);
        idx.push_back(i * 3 + 2);
    }
    auto view = MakeView(verts, idx);

    MeshBvh original = MeshBvh::Build(view);
    ASSERT_FALSE(original.IsEmpty());

    std::vector<GameEngine::uint8> bytes;
    original.Serialize(bytes);
    ASSERT_GT(bytes.size(), 0u);

    MeshBvh restored;
    ASSERT_TRUE(restored.Deserialize(bytes, view));
    EXPECT_EQ(original.TriangleCount(), restored.TriangleCount());

    // 100 random rays must produce identical hits on both BVHs.
    std::uniform_real_distribution<float32> origCoord(-3.0f, 3.0f);
    std::uniform_real_distribution<float32> dirCoord (-1.0f, 1.0f);
    int hitCount = 0;
    for (int i = 0; i < 100; ++i)
    {
        Vector3 origin(origCoord(rng), origCoord(rng), origCoord(rng));
        Vector3 dir   (dirCoord(rng),  dirCoord(rng),  dirCoord(rng));
        if (dir.Length() < 1e-3f) dir = Vector3(0, 0, 1);
        dir = dir.Normalize();

        PickHit hitOrig, hitRestored;
        const bool hO = original.Raycast(origin, dir, hitOrig);
        const bool hR = restored.Raycast(origin, dir, hitRestored);
        ASSERT_EQ(hO, hR) << "ray " << i;
        if (hO)
        {
            ++hitCount;
            EXPECT_NEAR(hitOrig.Distance, hitRestored.Distance, 1e-4f);
            EXPECT_EQ(hitOrig.TriangleIndex, hitRestored.TriangleIndex);
        }
    }
    EXPECT_GT(hitCount, 0);
}

TEST(MeshBvh, DeserializeRejectsBadMagic)
{
    std::vector<Vector3> verts = {{0,0,1}, {1,0,1}, {0,1,1}};
    std::vector<uint32> idx = {0, 1, 2};
    auto view = MakeView(verts, idx);

    std::vector<GameEngine::uint8> bytes;
    MeshBvh::Build(view).Serialize(bytes);
    bytes[0] = 0xFF;  // corrupt magic

    MeshBvh restored;
    EXPECT_FALSE(restored.Deserialize(bytes, view));
    EXPECT_TRUE(restored.IsEmpty());
}

TEST(MeshBvh, DeserializeRejectsTruncatedBuffer)
{
    std::vector<Vector3> verts(600);
    std::vector<uint32> idx;
    for (uint32 i = 0; i < 200u; ++i) {
        verts[i*3 + 0] = Vector3(0,0,1);
        verts[i*3 + 1] = Vector3(1,0,1);
        verts[i*3 + 2] = Vector3(0,1,1);
        idx.push_back(i*3 + 0);
        idx.push_back(i*3 + 1);
        idx.push_back(i*3 + 2);
    }
    auto view = MakeView(verts, idx);

    std::vector<GameEngine::uint8> bytes;
    MeshBvh::Build(view).Serialize(bytes);
    bytes.resize(bytes.size() / 2);  // truncate

    MeshBvh restored;
    EXPECT_FALSE(restored.Deserialize(bytes, view));
}

TEST(MeshBvh, DeserializeRejectsStaleVersion)
{
    // A payload whose Version field doesn't match kSerializedVersion must
    // be rejected before the per-node memcpy could silently misinterpret
    // bytes laid out by a different node format.
    std::vector<Vector3> verts = {{0,0,1}, {1,0,1}, {0,1,1}};
    std::vector<uint32> idx = {0, 1, 2};
    auto view = MakeView(verts, idx);

    std::vector<GameEngine::uint8> bytes;
    MeshBvh::Build(view).Serialize(bytes);
    // Mutate the Version field (4 bytes after the 4-byte magic) to a
    // value that cannot match any real kSerializedVersion.
    GameEngine::uint32 staleVer = 999u;
    std::memcpy(bytes.data() + sizeof(GameEngine::uint32), &staleVer, sizeof(staleVer));

    MeshBvh restored;
    EXPECT_FALSE(restored.Deserialize(bytes, view));
    EXPECT_TRUE(restored.IsEmpty());
}

TEST(MeshBvh, BuildHandlesSingleTriangle)
{
    std::vector<Vector3> verts = {{0,0,1}, {1,0,1}, {0,1,1}};
    std::vector<uint32> idx = {0, 1, 2};
    auto view = MakeView(verts, idx);

    MeshBvh bvh = MeshBvh::Build(view);
    EXPECT_FALSE(bvh.IsEmpty());
    EXPECT_EQ(bvh.TriangleCount(), 1u);

    PickHit hit;
    EXPECT_TRUE(bvh.Raycast(Vector3(0.25f, 0.25f, 0), Vector3(0,0,1), hit));
    EXPECT_NEAR(hit.Distance, 1.0f, 1e-5f);
}

TEST(MeshBvh, BuildHandlesEmptyMesh)
{
    MeshView empty{};
    MeshBvh bvh = MeshBvh::Build(empty);
    EXPECT_TRUE(bvh.IsEmpty());
    EXPECT_EQ(bvh.TriangleCount(), 0u);

    // Round-trip an empty BVH through Serialize/Deserialize.
    std::vector<GameEngine::uint8> bytes;
    bvh.Serialize(bytes);
    MeshBvh restored;
    // Empty source MeshView is an explicit "no triangles to validate"
    // case. The Deserialize path should accept zero-count payload OR
    // reject when source is invalid; either way, restored ends empty.
    restored.Deserialize(bytes, empty);
    EXPECT_TRUE(restored.IsEmpty());
}

TEST(MeshBvh, RaycastSurvivesAxisAlignedRay)
{
    // Axis-aligned rays trigger the (slabMin - origin) * (1/dirZeroComponent)
    // = NaN edge case if we don't NaN-clamp the inverse direction.
    // Build a small scene and verify a ray with direction (0, 1, 0) —
    // i.e. zero X and Z components — doesn't produce stray hits or
    // crashes via NaN propagation.
    std::vector<Vector3> verts = {
        {-1, 0, -1}, {1, 0, -1}, {-1, 0, 1},  // floor triangle
        { 1, 0, -1}, {1, 0,  1}, {-1, 0, 1},
    };
    std::vector<uint32> idx = {0, 1, 2,  3, 4, 5};
    auto view = MakeView(verts, idx);
    MeshBvh bvh = MeshBvh::Build(view);

    // Ray from above pointing straight down. Direction has only Y; X and Z = 0.
    PickHit hit;
    EXPECT_TRUE(bvh.Raycast(Vector3(0, 5, 0), Vector3(0, -1, 0), hit));
    EXPECT_NEAR(hit.Distance, 5.0f, 1e-4f);
}

// Note: the conservative-quantization invariant (dequantized AABBs always
// enclose source AABBs) is covered statistically by
// BuiltBvhMatchesBruteForceOnRandomRays. A direct test would need to
// inspect private BvhNode8 state, which isn't worth the friend-class
// invasion. If a regression appears, the parity test will catch it
// because shrunken AABBs would silently miss-hit some random rays that
// brute-force still hits.
