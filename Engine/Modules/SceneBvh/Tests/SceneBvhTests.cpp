#include <gtest/gtest.h>

#include "JobSystem/WorkStealingThreadPool.h"
#include "SceneBvh/MeshGeometryView.h"
#include "SceneBvh/ThreadedBvh.h"
#include "SceneBvh/ThreadedBvhBuilder.h"
#include "SceneBvh/ThreadedBvhRefit.h"
#include "SceneBvh/TlasPacker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SceneBvh;
using Mathematics::AABB;
using Mathematics::Vector3;

namespace
{

constexpr float32 kRayEpsilon = 1.0e-4f;
constexpr float32 kParityEpsilon = 1.0e-4f;
constexpr float32 kBoundsEpsilon = 1.0e-4f;

// ── Test-side geometry ─────────────────────────────────────────────────────

struct TestMesh
{
    std::vector<float32> Positions;
    std::vector<uint32> Indices;

    MeshGeometryView Soup() const { return MeshGeometryView{Positions, {}, {}, Indices, {}}; }

    uint32 TriangleCount() const { return static_cast<uint32>(Indices.size() / 3u); }
};

// Heightfield grid: well-distributed triangles with real spatial structure, so
// the SAH has something to separate and traversal actually prunes.
TestMesh MakeGridMesh(uint32 cells, uint32 seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float32> heightDist(-0.4f, 0.4f);

    TestMesh mesh;
    const uint32 verts = cells + 1u;
    mesh.Positions.reserve(static_cast<size_t>(verts) * verts * 3u);
    for (uint32 z = 0; z < verts; ++z)
    {
        for (uint32 x = 0; x < verts; ++x)
        {
            mesh.Positions.push_back(static_cast<float32>(x));
            mesh.Positions.push_back(heightDist(rng));
            mesh.Positions.push_back(static_cast<float32>(z));
        }
    }
    for (uint32 z = 0; z < cells; ++z)
    {
        for (uint32 x = 0; x < cells; ++x)
        {
            const uint32 v0 = z * verts + x;
            const uint32 v1 = v0 + 1u;
            const uint32 v2 = v0 + verts;
            const uint32 v3 = v2 + 1u;
            mesh.Indices.insert(mesh.Indices.end(), {v0, v2, v1});
            mesh.Indices.insert(mesh.Indices.end(), {v1, v2, v3});
        }
    }
    return mesh;
}

// Unconnected triangles scattered through a box: no shared vertices, uneven
// sizes, and clusters the median split would handle worse than the SAH.
TestMesh MakeRandomSoup(uint32 triangleCount, uint32 seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float32> centreDist(-10.0f, 10.0f);
    std::uniform_real_distribution<float32> sizeDist(0.05f, 1.5f);

    TestMesh mesh;
    mesh.Positions.reserve(static_cast<size_t>(triangleCount) * 9u);
    mesh.Indices.reserve(static_cast<size_t>(triangleCount) * 3u);
    for (uint32 t = 0; t < triangleCount; ++t)
    {
        const Vector3 centre(centreDist(rng), centreDist(rng), centreDist(rng));
        const float32 size = sizeDist(rng);
        for (uint32 corner = 0; corner < 3u; ++corner)
        {
            std::uniform_real_distribution<float32> offsetDist(-size, size);
            mesh.Positions.push_back(centre.x + offsetDist(rng));
            mesh.Positions.push_back(centre.y + offsetDist(rng));
            mesh.Positions.push_back(centre.z + offsetDist(rng));
            mesh.Indices.push_back(t * 3u + corner);
        }
    }
    return mesh;
}

Vector3 VertexPosition(const ThreadedBvh& bvh, uint32 vertex)
{
    const float32* record = &bvh.VertexData[static_cast<size_t>(vertex) * kVertexDataStrideFloats];
    return Vector3(record[0], record[1], record[2]);
}

// ── Ray intersection ───────────────────────────────────────────────────────

struct RayHit
{
    bool Hit = false;
    float32 T = 0.0f;
    uint32 Triangle = 0;
};

// Moller-Trumbore, matching the reference traversal's acceptance rules.
bool RayTriangle(const Vector3& origin, const Vector3& direction, const Vector3& p0,
                 const Vector3& p1, const Vector3& p2, float32& outT)
{
    const Vector3 edge1 = p1 - p0;
    const Vector3 edge2 = p2 - p0;
    const Vector3 pvec = Vector3::Cross(direction, edge2);
    const float32 det = Vector3::Dot(edge1, pvec);
    if (std::abs(det) < 1.0e-12f)
        return false;

    const float32 invDet = 1.0f / det;
    const Vector3 tvec = origin - p0;
    const float32 u = Vector3::Dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;

    const Vector3 qvec = Vector3::Cross(tvec, edge1);
    const float32 v = Vector3::Dot(direction, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    const float32 t = Vector3::Dot(edge2, qvec) * invDet;
    if (t <= kRayEpsilon)
        return false;

    outT = t;
    return true;
}

void TriangleVertices(const ThreadedBvh& bvh, uint32 triangle, Vector3& p0, Vector3& p1,
                      Vector3& p2)
{
    const size_t base = static_cast<size_t>(triangle) * kTriangleIndexStride;
    p0 = VertexPosition(bvh, bvh.TriangleIndices[base + 0]);
    p1 = VertexPosition(bvh, bvh.TriangleIndices[base + 1]);
    p2 = VertexPosition(bvh, bvh.TriangleIndices[base + 2]);
}

RayHit BruteForceClosest(const ThreadedBvh& bvh, const Vector3& origin, const Vector3& direction,
                         uint32& outTiedCount)
{
    RayHit best;
    float32 bestT = std::numeric_limits<float32>::max();
    outTiedCount = 0;
    for (uint32 triangle = 0; triangle < bvh.TriangleCount(); ++triangle)
    {
        Vector3 p0, p1, p2;
        TriangleVertices(bvh, triangle, p0, p1, p2);
        float32 t = 0.0f;
        if (!RayTriangle(origin, direction, p0, p1, p2, t))
            continue;
        if (t < bestT)
        {
            bestT = t;
            best.Hit = true;
            best.T = t;
            best.Triangle = triangle;
        }
    }
    if (!best.Hit)
        return best;

    for (uint32 triangle = 0; triangle < bvh.TriangleCount(); ++triangle)
    {
        Vector3 p0, p1, p2;
        TriangleVertices(bvh, triangle, p0, p1, p2);
        float32 t = 0.0f;
        if (RayTriangle(origin, direction, p0, p1, p2, t) && std::abs(t - bestT) <= kParityEpsilon)
            ++outTiedCount;
    }
    return best;
}

// Walks the threaded node array exactly as the future traversal shader will:
// interior nodes descend to node + 1, everything else jumps to the miss link.
RayHit TraverseThreaded(const ThreadedBvh& bvh, const Vector3& origin, const Vector3& direction)
{
    RayHit best;
    const uint32 nodeCount = bvh.NodeCount();
    if (nodeCount == 0)
        return best;

    const uint32* nodes = bvh.Nodes.data();
    const Vector3 invDirection(1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z);
    float32 bestT = std::numeric_limits<float32>::max();

    uint32 cursor = 0;
    // A malformed tree must fail the test rather than hang it: every visit
    // either descends one node or jumps forward, so the walk is bounded.
    uint32 visits = 0;
    const uint32 visitLimit = nodeCount * 2u + 8u;

    while (cursor < nodeCount)
    {
        EXPECT_LT(visits++, visitLimit) << "threaded walk failed to terminate";
        if (visits >= visitLimit)
            break;

        const AABB bounds = DecodeNodeBounds(nodes, cursor);
        const uint32 miss = DecodeNodeMissLink(nodes, cursor);
        const uint32 leafWord = DecodeNodeLeafWord(nodes, cursor);

        float32 tNear = std::numeric_limits<float32>::lowest();
        float32 tFar = std::numeric_limits<float32>::max();
        for (int axis = 0; axis < 3; ++axis)
        {
            const float32 t0 = (bounds.min[axis] - origin[axis]) * invDirection[axis];
            const float32 t1 = (bounds.max[axis] - origin[axis]) * invDirection[axis];
            tNear = std::max(tNear, std::min(t0, t1));
            tFar = std::min(tFar, std::max(t0, t1));
        }

        if (!(tFar >= std::max(tNear, 0.0f) && tNear < bestT))
        {
            cursor = miss;
            continue;
        }

        if (IsInteriorLeafWord(leafWord))
        {
            ++cursor;
            continue;
        }

        const uint32 offset = DecodeLeafTriangleOffset(leafWord);
        const uint32 count = DecodeLeafTriangleCount(leafWord);
        for (uint32 i = 0; i < count; ++i)
        {
            const uint32 triangle = offset + i;
            Vector3 p0, p1, p2;
            TriangleVertices(bvh, triangle, p0, p1, p2);
            float32 t = 0.0f;
            if (RayTriangle(origin, direction, p0, p1, p2, t) && t < bestT)
            {
                bestT = t;
                best.Hit = true;
                best.T = t;
                best.Triangle = triangle;
            }
        }
        cursor = miss;
    }
    return best;
}

void ExpectTraversalMatchesBruteForce(const ThreadedBvh& bvh, const Vector3& origin,
                                      const Vector3& direction)
{
    uint32 tiedCount = 0;
    const RayHit reference = BruteForceClosest(bvh, origin, direction, tiedCount);
    const RayHit traversed = TraverseThreaded(bvh, origin, direction);

    ASSERT_EQ(reference.Hit, traversed.Hit);
    if (!reference.Hit)
        return;

    EXPECT_NEAR(reference.T, traversed.T, kParityEpsilon);
    // Two triangles sharing an edge can both be exactly at the closest t; the
    // index is only well defined when the winner is unique.
    if (tiedCount == 1)
        EXPECT_EQ(reference.Triangle, traversed.Triangle);
}

// ── Node-array inspection ──────────────────────────────────────────────────

// Exact AABB of every triangle under `node`, recomputed from vertex data
// without reusing any builder or refit code. A node's subtree occupies node
// indices [node, missLink).
AABB SubtreeGroundTruthBounds(const ThreadedBvh& bvh, uint32 node)
{
    const uint32* nodes = bvh.Nodes.data();
    const uint32 end = DecodeNodeMissLink(nodes, node);
    AABB bounds{{std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(),
                 std::numeric_limits<float32>::max()},
                {std::numeric_limits<float32>::lowest(), std::numeric_limits<float32>::lowest(),
                 std::numeric_limits<float32>::lowest()}};
    for (uint32 n = node; n < end; ++n)
    {
        const uint32 leafWord = DecodeNodeLeafWord(nodes, n);
        if (IsInteriorLeafWord(leafWord))
            continue;
        const uint32 offset = DecodeLeafTriangleOffset(leafWord);
        const uint32 count = DecodeLeafTriangleCount(leafWord);
        for (uint32 i = 0; i < count; ++i)
        {
            Vector3 p0, p1, p2;
            TriangleVertices(bvh, offset + i, p0, p1, p2);
            for (const Vector3& p : {p0, p1, p2})
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    bounds.min[axis] = std::min(bounds.min[axis], p[axis]);
                    bounds.max[axis] = std::max(bounds.max[axis], p[axis]);
                }
            }
        }
    }
    return bounds;
}

void ExpectBoundsNear(const AABB& actual, const AABB& expected, float32 epsilon)
{
    EXPECT_NEAR(actual.min.x, expected.min.x, epsilon);
    EXPECT_NEAR(actual.min.y, expected.min.y, epsilon);
    EXPECT_NEAR(actual.min.z, expected.min.z, epsilon);
    EXPECT_NEAR(actual.max.x, expected.max.x, epsilon);
    EXPECT_NEAR(actual.max.y, expected.max.y, epsilon);
    EXPECT_NEAR(actual.max.z, expected.max.z, epsilon);
}

// Shear plus a positional wave: changes every vertex, keeps topology, and is
// not a similarity transform, so the build-time partition genuinely goes stale.
void DeformPositions(std::vector<float32>& positions)
{
    for (size_t v = 0; v + 2 < positions.size(); v += 3)
    {
        const float32 x = positions[v + 0];
        const float32 y = positions[v + 1];
        const float32 z = positions[v + 2];
        positions[v + 0] = x * 1.7f + y * 0.35f;
        positions[v + 1] = y * 0.6f + std::sin(x * 0.9f) * 1.25f;
        positions[v + 2] = z * 0.8f - x * 0.15f;
    }
}

void ApplyPositionsToVertexData(ThreadedBvh& bvh, const std::vector<float32>& positions)
{
    for (uint32 v = 0; v < bvh.VertexCount(); ++v)
    {
        float32* record = &bvh.VertexData[static_cast<size_t>(v) * kVertexDataStrideFloats];
        record[0] = positions[static_cast<size_t>(v) * 3u + 0];
        record[1] = positions[static_cast<size_t>(v) * 3u + 1];
        record[2] = positions[static_cast<size_t>(v) * 3u + 2];
    }
}

Mathematics::Matrix4x4 MakeTransform(const Vector3& translation, const Vector3& scale)
{
    glm::mat4 m(1.0f);
    m[0][0] = scale.x;
    m[1][1] = scale.y;
    m[2][2] = scale.z;
    m[3][0] = translation.x;
    m[3][1] = translation.y;
    m[3][2] = translation.z;
    return Mathematics::Matrix4x4(m);
}

TlasInstance MakeInstance(const Vector3& translation, const Vector3& scale, uint32 blasRoot,
                          uint32 blasEnd)
{
    TlasInstance instance;
    instance.WorldFromLocal = MakeTransform(translation, scale);
    instance.LocalBounds = AABB{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    instance.BlasRoot = blasRoot;
    instance.BlasEnd = blasEnd;
    return instance;
}

} // namespace

// ── Bounds primitives the builders share ───────────────────────────────────

TEST(AabbTest, ExpandingEmptyByAPointYieldsThatPoint)
{
    AABB bounds = AABB::Empty();
    bounds.Expand(Vector3(1.0f, -2.0f, 3.0f));
    ExpectBoundsNear(bounds, AABB{{1.0f, -2.0f, 3.0f}, {1.0f, -2.0f, 3.0f}}, 0.0f);
}

TEST(AabbTest, ExpandingByAnEmptyBoxLeavesBoundsUnchanged)
{
    const AABB original{{-1.0f, 0.0f, 2.0f}, {4.0f, 5.0f, 6.0f}};
    AABB bounds = original;
    bounds.Expand(AABB::Empty());
    ExpectBoundsNear(bounds, original, 0.0f);
}

TEST(AabbTest, ExpandingByABoxTakesTheComponentWiseUnion)
{
    AABB bounds{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    bounds.Expand(AABB{{-2.0f, 0.5f, 0.5f}, {0.5f, 3.0f, 0.5f}});
    ExpectBoundsNear(bounds, AABB{{-2.0f, 0.0f, 0.0f}, {1.0f, 3.0f, 1.0f}}, 0.0f);
}

TEST(AabbTest, SurfaceAreaIsFullAreaAndZeroWhenEmpty)
{
    EXPECT_FLOAT_EQ((AABB{{0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f}}.SurfaceArea()), 22.0f);
    EXPECT_FLOAT_EQ(AABB::Empty().SurfaceArea(), 0.0f);
}

// ── Builder ────────────────────────────────────────────────────────────────

TEST(ThreadedBvhBuilderTest, EmptySoupProducesEmptyBvh)
{
    const MeshGeometryView empty;
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(empty);
    EXPECT_TRUE(bvh.IsEmpty());
    EXPECT_EQ(0u, bvh.NodeCount());
    EXPECT_EQ(0u, bvh.TriangleCount());
}

TEST(ThreadedBvhBuilderTest, OutOfRangeIndexIsRejected)
{
    const std::vector<float32> positions{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::vector<uint32> indices{0u, 1u, 7u};
    const MeshGeometryView soup{positions, {}, {}, indices, {}};

    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(soup);
    EXPECT_TRUE(bvh.IsEmpty());
}

TEST(ThreadedBvhBuilderTest, LeafTrianglesStayWithinCap)
{
    const TestMesh mesh = MakeRandomSoup(512u, 11u);
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    for (uint32 node = 0; node < bvh.NodeCount(); ++node)
    {
        const uint32 leafWord = DecodeNodeLeafWord(bvh.Nodes.data(), node);
        if (IsInteriorLeafWord(leafWord))
            continue;
        const uint32 count = DecodeLeafTriangleCount(leafWord);
        EXPECT_GT(count, 0u);
        EXPECT_LE(count, kMaxLeafTriangles);
        EXPECT_LE(DecodeLeafTriangleOffset(leafWord) + count, bvh.TriangleCount());
    }
}

TEST(ThreadedBvhBuilderTest, NodesArePreOrderWithValidMissLinks)
{
    const TestMesh mesh = MakeGridMesh(12u, 7u);
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    const uint32 nodeCount = bvh.NodeCount();
    const uint32* nodes = bvh.Nodes.data();

    // The root's subtree is the whole array, and every miss link must point
    // strictly forward and no further than one past the end.
    EXPECT_EQ(nodeCount, DecodeNodeMissLink(nodes, 0u));
    for (uint32 node = 0; node < nodeCount; ++node)
    {
        const uint32 miss = DecodeNodeMissLink(nodes, node);
        EXPECT_GT(miss, node);
        EXPECT_LE(miss, nodeCount);

        if (IsInteriorLeafWord(DecodeNodeLeafWord(nodes, node)))
        {
            // Left child is the next node; its escape link is the right child,
            // which must live inside this node's subtree.
            const uint32 left = node + 1u;
            ASSERT_LT(left, nodeCount);
            const uint32 right = DecodeNodeMissLink(nodes, left);
            EXPECT_GT(right, left);
            EXPECT_LT(right, miss);
        }
        else
        {
            EXPECT_EQ(node + 1u, miss);
        }
    }
}

TEST(ThreadedBvhBuilderTest, EveryTriangleAppearsInExactlyOneLeaf)
{
    const TestMesh mesh = MakeRandomSoup(400u, 3u);
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());
    ASSERT_EQ(mesh.TriangleCount(), bvh.TriangleCount());

    std::vector<uint32> coverage(bvh.TriangleCount(), 0u);
    for (uint32 node = 0; node < bvh.NodeCount(); ++node)
    {
        const uint32 leafWord = DecodeNodeLeafWord(bvh.Nodes.data(), node);
        if (IsInteriorLeafWord(leafWord))
            continue;
        const uint32 offset = DecodeLeafTriangleOffset(leafWord);
        for (uint32 i = 0; i < DecodeLeafTriangleCount(leafWord); ++i)
            ++coverage[offset + i];
    }
    for (uint32 triangle = 0; triangle < coverage.size(); ++triangle)
        EXPECT_EQ(1u, coverage[triangle]) << "triangle " << triangle;
}

TEST(ThreadedBvhBuilderTest, NodeBoundsEncloseTheirSubtrees)
{
    const TestMesh mesh = MakeRandomSoup(300u, 5u);
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    for (uint32 node = 0; node < bvh.NodeCount(); ++node)
        ExpectBoundsNear(DecodeNodeBounds(bvh.Nodes.data(), node),
                         SubtreeGroundTruthBounds(bvh, node), kBoundsEpsilon);
    ExpectBoundsNear(bvh.LocalBounds, SubtreeGroundTruthBounds(bvh, 0u), kBoundsEpsilon);
}

TEST(ThreadedBvhBuilderTest, BuildManyMatchesSerialBuilds)
{
    std::vector<TestMesh> meshes;
    for (uint32 i = 0; i < 8u; ++i)
        meshes.push_back(MakeRandomSoup(120u + i * 40u, 100u + i));

    std::vector<MeshGeometryView> soups;
    std::vector<ThreadedBvh> serial;
    for (const TestMesh& mesh : meshes)
    {
        soups.push_back(mesh.Soup());
        serial.push_back(ThreadedBvhBuilder::Build(mesh.Soup()));
    }

    JobSystem::WorkStealingThreadPool pool(4);
    std::vector<ThreadedBvh> parallel(soups.size());
    ThreadedBvhBuilder::BuildMany(&pool, soups, parallel);

    ASSERT_EQ(serial.size(), parallel.size());
    for (size_t i = 0; i < serial.size(); ++i)
    {
        EXPECT_EQ(serial[i].Nodes, parallel[i].Nodes) << "mesh " << i;
        EXPECT_EQ(serial[i].TriangleIndices, parallel[i].TriangleIndices) << "mesh " << i;
        EXPECT_EQ(serial[i].VertexData, parallel[i].VertexData) << "mesh " << i;
    }
}

TEST(ThreadedBvhBuilderTest, BuildManyWithoutPoolMatchesSerialBuilds)
{
    const TestMesh mesh = MakeGridMesh(6u, 21u);
    const std::vector<MeshGeometryView> soups{mesh.Soup(), mesh.Soup()};
    std::vector<ThreadedBvh> results(soups.size());
    ThreadedBvhBuilder::BuildMany(nullptr, soups, results);

    const ThreadedBvh reference = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(reference.IsEmpty());
    for (const ThreadedBvh& result : results)
        EXPECT_EQ(reference.Nodes, result.Nodes);
}

// ── Traversal parity ───────────────────────────────────────────────────────

TEST(ThreadedBvhTraversalTest, GridMeshMatchesBruteForce)
{
    const TestMesh mesh = MakeGridMesh(16u, 9u);
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    std::mt19937 rng(4242u);
    std::uniform_real_distribution<float32> planarDist(-2.0f, 18.0f);
    std::uniform_real_distribution<float32> tiltDist(-0.35f, 0.35f);
    for (uint32 i = 0; i < 400u; ++i)
    {
        const Vector3 origin(planarDist(rng), 6.0f, planarDist(rng));
        const Vector3 direction = Vector3(tiltDist(rng), -1.0f, tiltDist(rng)).Normalize();
        ExpectTraversalMatchesBruteForce(bvh, origin, direction);
    }
}

TEST(ThreadedBvhTraversalTest, RandomSoupMatchesBruteForce)
{
    const TestMesh mesh = MakeRandomSoup(600u, 13u);
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    std::mt19937 rng(777u);
    std::uniform_real_distribution<float32> originDist(-18.0f, 18.0f);
    std::uniform_real_distribution<float32> dirDist(-1.0f, 1.0f);
    for (uint32 i = 0; i < 400u; ++i)
    {
        const Vector3 origin(originDist(rng), originDist(rng), originDist(rng));
        Vector3 direction(dirDist(rng), dirDist(rng), dirDist(rng));
        if (direction.Length() < 0.2f)
            continue;
        direction = direction.Normalize();
        ExpectTraversalMatchesBruteForce(bvh, origin, direction);
    }
}

TEST(ThreadedBvhTraversalTest, RayMissingTheRootReportsNoHit)
{
    const TestMesh mesh = MakeGridMesh(8u, 31u);
    const ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    const RayHit hit =
        TraverseThreaded(bvh, Vector3(-100.0f, 50.0f, -100.0f), Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_FALSE(hit.Hit);
}

// ── Refit ──────────────────────────────────────────────────────────────────

TEST(ThreadedBvhRefitTest, RefitBoundsMatchSubtreeGroundTruth)
{
    TestMesh mesh = MakeGridMesh(14u, 17u);
    ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    DeformPositions(mesh.Positions);
    ApplyPositionsToVertexData(bvh, mesh.Positions);

    ThreadedBvhRefit refit(bvh);
    ASSERT_EQ(ThreadedBvhRefitStatus::Complete, refit.Step(kUnboundedRefitBudget));

    for (uint32 node = 0; node < bvh.NodeCount(); ++node)
        ExpectBoundsNear(DecodeNodeBounds(bvh.Nodes.data(), node),
                         SubtreeGroundTruthBounds(bvh, node), kBoundsEpsilon);
}

TEST(ThreadedBvhRefitTest, RefitRootBoundsMatchIndependentRebuild)
{
    TestMesh mesh = MakeGridMesh(14u, 19u);
    ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    DeformPositions(mesh.Positions);
    ApplyPositionsToVertexData(bvh, mesh.Positions);

    ThreadedBvhRefit refit(bvh);
    ASSERT_EQ(ThreadedBvhRefitStatus::Complete, refit.Step(kUnboundedRefitBudget));

    // A fresh build over the deformed vertices re-partitions, so node arrays are
    // not comparable element by element; the ROOT bounds are topology
    // independent and must agree exactly.
    const ThreadedBvh rebuilt = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(rebuilt.IsEmpty());
    ExpectBoundsNear(bvh.LocalBounds, rebuilt.LocalBounds, kBoundsEpsilon);
    ExpectBoundsNear(DecodeNodeBounds(bvh.Nodes.data(), 0u),
                     DecodeNodeBounds(rebuilt.Nodes.data(), 0u), kBoundsEpsilon);
}

TEST(ThreadedBvhRefitTest, RefitTraversalMatchesRebuiltTraversal)
{
    TestMesh mesh = MakeGridMesh(12u, 23u);
    ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    DeformPositions(mesh.Positions);
    ApplyPositionsToVertexData(bvh, mesh.Positions);

    ThreadedBvhRefit refit(bvh);
    ASSERT_EQ(ThreadedBvhRefitStatus::Complete, refit.Step(kUnboundedRefitBudget));

    std::mt19937 rng(31337u);
    std::uniform_real_distribution<float32> planarDist(-6.0f, 26.0f);
    std::uniform_real_distribution<float32> tiltDist(-0.3f, 0.3f);
    for (uint32 i = 0; i < 300u; ++i)
    {
        const Vector3 origin(planarDist(rng), 9.0f, planarDist(rng));
        const Vector3 direction = Vector3(tiltDist(rng), -1.0f, tiltDist(rng)).Normalize();
        ExpectTraversalMatchesBruteForce(bvh, origin, direction);
    }
}

TEST(ThreadedBvhRefitTest, BudgetedRefitMatchesUnboundedRefit)
{
    TestMesh mesh = MakeRandomSoup(250u, 41u);
    ThreadedBvh unbounded = ThreadedBvhBuilder::Build(mesh.Soup());
    ThreadedBvh budgeted = unbounded;
    ASSERT_FALSE(unbounded.IsEmpty());

    DeformPositions(mesh.Positions);
    ApplyPositionsToVertexData(unbounded, mesh.Positions);
    ApplyPositionsToVertexData(budgeted, mesh.Positions);

    ThreadedBvhRefit fullRefit(unbounded);
    ASSERT_EQ(ThreadedBvhRefitStatus::Complete, fullRefit.Step(kUnboundedRefitBudget));

    ThreadedBvhRefit steppedRefit(budgeted);
    ThreadedBvhRefitStatus status = ThreadedBvhRefitStatus::Budgeted;
    uint32 steps = 0;
    const uint32 stepLimit = fullRefit.ProcessedNodes() + fullRefit.ProcessedTriangles() + 16u;
    while (status == ThreadedBvhRefitStatus::Budgeted)
    {
        ASSERT_LT(steps++, stepLimit) << "budgeted refit failed to converge";
        status = steppedRefit.Step(1u);
    }
    ASSERT_EQ(ThreadedBvhRefitStatus::Complete, status);
    EXPECT_GT(steps, 1u) << "a one-unit budget must take more than one call";

    EXPECT_EQ(unbounded.Nodes, budgeted.Nodes);
    EXPECT_EQ(fullRefit.ProcessedNodes(), steppedRefit.ProcessedNodes());
    EXPECT_EQ(fullRefit.ProcessedTriangles(), steppedRefit.ProcessedTriangles());
}

TEST(ThreadedBvhRefitTest, ZeroBudgetMakesNoProgress)
{
    const TestMesh mesh = MakeRandomSoup(64u, 43u);
    ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    ThreadedBvhRefit refit(bvh);
    EXPECT_EQ(ThreadedBvhRefitStatus::Budgeted, refit.Step(0u));
    EXPECT_EQ(0u, refit.ProcessedNodes());
    EXPECT_EQ(0u, refit.ProcessedTriangles());
}

TEST(ThreadedBvhRefitTest, CorruptRightChildLinkRequiresRebuild)
{
    const TestMesh mesh = MakeRandomSoup(64u, 47u);
    ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());
    ASSERT_TRUE(IsInteriorLeafWord(DecodeNodeLeafWord(bvh.Nodes.data(), 0u)));

    // The root's left child is node 1; its miss link IS the right child, so
    // pushing that link past the end breaks the threaded-layout invariant.
    bvh.Nodes[1u * kThreadedBvhNodeStrideU32 + 6u] = bvh.NodeCount() + 1u;

    ThreadedBvhRefit refit(bvh);
    EXPECT_EQ(ThreadedBvhRefitStatus::RebuildRequired, refit.Step(kUnboundedRefitBudget));
}

TEST(ThreadedBvhRefitTest, OutOfRangeTriangleIndexRequiresRebuild)
{
    const TestMesh mesh = MakeRandomSoup(64u, 53u);
    ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    // A leaf triangle now points outside the vertex array: the refit cannot
    // produce bounds for it and must demand a rebuild instead of guessing.
    bvh.TriangleIndices[0] = bvh.VertexCount() + 100u;

    ThreadedBvhRefit refit(bvh);
    EXPECT_EQ(ThreadedBvhRefitStatus::RebuildRequired, refit.Step(kUnboundedRefitBudget));
}

TEST(ThreadedBvhRefitTest, TopologyChangeRequiresRebuild)
{
    const TestMesh mesh = MakeRandomSoup(64u, 59u);
    ThreadedBvh bvh = ThreadedBvhBuilder::Build(mesh.Soup());
    ASSERT_FALSE(bvh.IsEmpty());

    // Dropping a triangle is a connectivity edit, not a deformation: the stored
    // partition no longer describes this geometry.
    bvh.TriangleIndices.resize(bvh.TriangleIndices.size() - 1u);

    ThreadedBvhRefit refit(bvh);
    EXPECT_EQ(ThreadedBvhRefitStatus::RebuildRequired, refit.Step(kUnboundedRefitBudget));
}

TEST(ThreadedBvhRefitTest, RebuildRequiredIsSticky)
{
    ThreadedBvh empty;
    ThreadedBvhRefit refit(empty);
    EXPECT_EQ(ThreadedBvhRefitStatus::RebuildRequired, refit.Step(kUnboundedRefitBudget));
    EXPECT_EQ(ThreadedBvhRefitStatus::RebuildRequired, refit.Step(kUnboundedRefitBudget));
}

// ── TLAS packing ───────────────────────────────────────────────────────────

TEST(TlasPackerTest, EmptyInstanceListPacksNothing)
{
    const std::vector<UberMaterial> materials(2u);
    const PackedTlas packed = TlasPacker::Pack(materials, {});
    EXPECT_TRUE(packed.IsEmpty());
    EXPECT_EQ(0u, packed.TlasNodeCount);
}

TEST(TlasPackerTest, PackedLayoutMatchesStrides)
{
    const std::vector<UberMaterial> materials(3u);
    std::vector<TlasInstance> instances;
    for (uint32 i = 0; i < 9u; ++i)
        instances.push_back(MakeInstance(Vector3(static_cast<float32>(i) * 4.0f, 0.0f, 0.0f),
                                         Vector3(1.0f, 1.0f, 1.0f), i * 10u, i * 10u + 7u));

    const PackedTlas packed = TlasPacker::Pack(materials, instances);
    ASSERT_FALSE(packed.IsEmpty());

    EXPECT_EQ(3u, packed.MaterialCount);
    EXPECT_EQ(9u, packed.InstanceCount);
    EXPECT_EQ(3u * kUberMaterialStrideFloats, packed.InstanceBase);
    EXPECT_EQ(packed.InstanceBase + 9u * kUberMaterialStrideFloats, packed.TlasBase);
    EXPECT_EQ(packed.Buffer.size(),
              static_cast<size_t>(packed.TlasBase) + packed.TlasNodeCount * kTlasNodeStrideFloats);
    EXPECT_EQ(9u, packed.InstanceOrder.size());

    std::vector<uint32> seen(9u, 0u);
    for (uint32 source : packed.InstanceOrder)
    {
        ASSERT_LT(source, 9u);
        ++seen[source];
    }
    for (uint32 count : seen)
        EXPECT_EQ(1u, count);
}

TEST(TlasPackerTest, MaterialRecordsRoundTrip)
{
    std::vector<UberMaterial> materials(2u);
    materials[0].BaseColorR = 0.25f;
    materials[0].Roughness = 0.5f;
    materials[1].EmissiveG = 3.5f;
    materials[1].Side = 2.0f;
    materials[1].NirAlbedo = 0.55f;

    const std::vector<TlasInstance> instances{
        MakeInstance(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f), 0u, 5u)};
    const PackedTlas packed = TlasPacker::Pack(materials, instances);
    ASSERT_FALSE(packed.IsEmpty());

    EXPECT_FLOAT_EQ(0.25f, packed.Buffer[0]);
    EXPECT_FLOAT_EQ(0.5f, packed.Buffer[3]);
    EXPECT_FLOAT_EQ(3.5f, packed.Buffer[kUberMaterialStrideFloats + 8u]);
    EXPECT_FLOAT_EQ(2.0f, packed.Buffer[kUberMaterialStrideFloats + 22u]);
    EXPECT_FLOAT_EQ(0.55f, packed.Buffer[kUberMaterialStrideFloats + 25u]);
}

// A BLAS is shared by every instance of its mesh, so its per-triangle material
// table can only hold one answer. Two instances of ONE tree that shade with two
// materials are therefore the case the instance-record material slot exists for
// — and the case the software trace lane got wrong before it existed.
TEST(TlasPackerTest, TwoInstancesOfOneBlasCarryDistinctMaterialSlots)
{
    std::vector<UberMaterial> materials(2u);
    materials[0].BaseColorR = 0.25f;
    materials[1].BaseColorR = 0.75f;

    // Same BLAS range in both: one tree, two instances, two materials.
    constexpr uint32 kBlasRoot = 0u;
    constexpr uint32 kBlasEnd = 7u;
    std::vector<TlasInstance> instances{
        MakeInstance(Vector3(-8.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f), kBlasRoot, kBlasEnd),
        MakeInstance(Vector3(8.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f), kBlasRoot, kBlasEnd)};
    instances[0].MaterialSlot = 0u;
    instances[1].MaterialSlot = 1u;

    const PackedTlas packed = TlasPacker::Pack(materials, instances);
    ASSERT_FALSE(packed.IsEmpty());
    ASSERT_EQ(2u, packed.MaterialCount);
    ASSERT_EQ(2u, packed.InstanceCount);

    // Records live in TLAS slot order, so resolve each slot back to the caller's
    // instance before comparing.
    for (uint32 slot = 0; slot < packed.InstanceCount; ++slot)
    {
        const uint32 source = packed.InstanceOrder[slot];
        const float32* record = &packed.Buffer[packed.InstanceBase + slot * kUberMaterialStrideFloats];
        EXPECT_FLOAT_EQ(static_cast<float32>(kBlasRoot), record[12]);
        EXPECT_FLOAT_EQ(static_cast<float32>(kBlasEnd), record[13]);
        EXPECT_FLOAT_EQ(static_cast<float32>(instances[source].MaterialSlot), record[15]);

        // The slot must resolve to that instance's own material row.
        const float32 baseColorR =
            packed.Buffer[static_cast<size_t>(instances[source].MaterialSlot) *
                          kUberMaterialStrideFloats];
        EXPECT_FLOAT_EQ(materials[instances[source].MaterialSlot].BaseColorR, baseColorR);
    }

    const float32* first =
        &packed.Buffer[packed.InstanceBase + 0u * kUberMaterialStrideFloats];
    const float32* second =
        &packed.Buffer[packed.InstanceBase + 1u * kUberMaterialStrideFloats];
    EXPECT_NE(first[15], second[15]) << "both instances packed the same material slot";
}

// The default is "no instance material" — the traversal falls back to what the
// geometry carries, which is what every caller that shares one material per BLAS
// relies on.
TEST(TlasPackerTest, InstanceWithNoMaterialPacksTheDeferToGeometrySentinel)
{
    const std::vector<TlasInstance> instances{
        MakeInstance(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f), 0u, 5u)};
    ASSERT_EQ(kTlasInstanceMaterialFromGeometry, instances[0].MaterialSlot);

    const PackedTlas packed = TlasPacker::Pack({}, instances);
    ASSERT_FALSE(packed.IsEmpty());
    EXPECT_FLOAT_EQ(-1.0f, packed.Buffer[packed.InstanceBase + 15u]);
}

TEST(TlasPackerTest, InstanceRecordInvertsTheWorldTransform)
{
    const Vector3 translation(3.0f, -2.0f, 5.0f);
    const Vector3 scale(2.0f, 4.0f, 0.5f);
    const std::vector<TlasInstance> instances{MakeInstance(translation, scale, 12u, 34u)};
    const PackedTlas packed = TlasPacker::Pack({}, instances);
    ASSERT_FALSE(packed.IsEmpty());

    const float32* record = &packed.Buffer[packed.InstanceBase];
    EXPECT_FLOAT_EQ(12.0f, record[12]);
    EXPECT_FLOAT_EQ(34.0f, record[13]);
    EXPECT_FLOAT_EQ(1.0f, record[14]);

    // Apply the packed rows to a world point and expect the local point back.
    const Vector3 local(0.5f, -0.25f, 0.75f);
    const Vector3 world(local.x * scale.x + translation.x, local.y * scale.y + translation.y,
                        local.z * scale.z + translation.z);
    for (int row = 0; row < 3; ++row)
    {
        const float32 recovered = record[row * 4 + 0] * world.x + record[row * 4 + 1] * world.y +
                                  record[row * 4 + 2] * world.z + record[row * 4 + 3];
        EXPECT_NEAR(local[row], recovered, 1.0e-5f) << "row " << row;
    }
}

TEST(TlasPackerTest, MirroredInstanceGetsNegativeWindingSign)
{
    const std::vector<TlasInstance> instances{
        MakeInstance(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, -1.0f, 1.0f), 0u, 4u)};
    const PackedTlas packed = TlasPacker::Pack({}, instances);
    ASSERT_FALSE(packed.IsEmpty());
    EXPECT_FLOAT_EQ(-1.0f, packed.Buffer[packed.InstanceBase + 14u]);
}

TEST(TlasPackerTest, TlasLeavesCoverEverySlotExactlyOnce)
{
    std::mt19937 rng(64u);
    std::uniform_real_distribution<float32> positionDist(-30.0f, 30.0f);
    std::vector<TlasInstance> instances;
    for (uint32 i = 0; i < 37u; ++i)
        instances.push_back(MakeInstance(Vector3(positionDist(rng), positionDist(rng),
                                                 positionDist(rng)),
                                         Vector3(1.0f, 1.0f, 1.0f), 0u, 3u));

    const PackedTlas packed = TlasPacker::Pack({}, instances);
    ASSERT_FALSE(packed.IsEmpty());

    std::vector<uint32> coverage(packed.InstanceCount, 0u);
    for (uint32 node = 0; node < packed.TlasNodeCount; ++node)
    {
        const float32* record = &packed.Buffer[packed.TlasBase + node * kTlasNodeStrideFloats];
        const uint32 miss = static_cast<uint32>(record[6]);
        EXPECT_GT(miss, node);
        EXPECT_LE(miss, packed.TlasNodeCount);

        const uint32 count = static_cast<uint32>(record[8]);
        if (count == 0u)
        {
            EXPECT_LT(node + 1u, packed.TlasNodeCount);
            continue;
        }
        EXPECT_LE(count, kTlasLeafInstances);
        const uint32 offset = static_cast<uint32>(record[7]);
        ASSERT_LE(offset + count, packed.InstanceCount);
        for (uint32 i = 0; i < count; ++i)
            ++coverage[offset + i];
    }
    for (uint32 slot = 0; slot < coverage.size(); ++slot)
        EXPECT_EQ(1u, coverage[slot]) << "slot " << slot;
}

TEST(TlasPackerTest, TlasNodeBoundsEncloseTheirSubtrees)
{
    std::mt19937 rng(96u);
    std::uniform_real_distribution<float32> positionDist(-25.0f, 25.0f);
    std::uniform_real_distribution<float32> scaleDist(0.5f, 3.0f);
    std::vector<TlasInstance> instances;
    for (uint32 i = 0; i < 23u; ++i)
        instances.push_back(MakeInstance(
            Vector3(positionDist(rng), positionDist(rng), positionDist(rng)),
            Vector3(scaleDist(rng), scaleDist(rng), scaleDist(rng)), 0u, 3u));

    const PackedTlas packed = TlasPacker::Pack({}, instances);
    ASSERT_FALSE(packed.IsEmpty());

    // Per-slot world bounds, recomputed here rather than reused from the packer.
    std::vector<AABB> slotBounds(packed.InstanceCount);
    for (uint32 slot = 0; slot < packed.InstanceCount; ++slot)
    {
        const TlasInstance& instance = instances[packed.InstanceOrder[slot]];
        const glm::mat4& world = instance.WorldFromLocal.GetGLM();
        AABB bounds{{std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(),
                     std::numeric_limits<float32>::max()},
                    {std::numeric_limits<float32>::lowest(),
                     std::numeric_limits<float32>::lowest(),
                     std::numeric_limits<float32>::lowest()}};
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::vec4 local(corner & 1 ? instance.LocalBounds.max.x : instance.LocalBounds.min.x,
                                  corner & 2 ? instance.LocalBounds.max.y : instance.LocalBounds.min.y,
                                  corner & 4 ? instance.LocalBounds.max.z : instance.LocalBounds.min.z,
                                  1.0f);
            const glm::vec4 transformed = world * local;
            for (int axis = 0; axis < 3; ++axis)
            {
                bounds.min[axis] = std::min(bounds.min[axis], transformed[axis]);
                bounds.max[axis] = std::max(bounds.max[axis], transformed[axis]);
            }
        }
        slotBounds[slot] = bounds;
    }

    for (uint32 node = 0; node < packed.TlasNodeCount; ++node)
    {
        const float32* record = &packed.Buffer[packed.TlasBase + node * kTlasNodeStrideFloats];
        const uint32 end = static_cast<uint32>(record[6]);
        AABB expected{{std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(),
                       std::numeric_limits<float32>::max()},
                      {std::numeric_limits<float32>::lowest(),
                       std::numeric_limits<float32>::lowest(),
                       std::numeric_limits<float32>::lowest()}};
        for (uint32 n = node; n < end; ++n)
        {
            const float32* inner = &packed.Buffer[packed.TlasBase + n * kTlasNodeStrideFloats];
            const uint32 count = static_cast<uint32>(inner[8]);
            if (count == 0u)
                continue;
            const uint32 offset = static_cast<uint32>(inner[7]);
            for (uint32 i = 0; i < count; ++i)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    expected.min[axis] = std::min(expected.min[axis], slotBounds[offset + i].min[axis]);
                    expected.max[axis] = std::max(expected.max[axis], slotBounds[offset + i].max[axis]);
                }
            }
        }

        const AABB actual{{record[0], record[1], record[2]}, {record[3], record[4], record[5]}};
        ExpectBoundsNear(actual, expected, kBoundsEpsilon);
    }
}

TEST(TlasPackerTest, WorldBoundsEncloseEveryInstance)
{
    const std::vector<TlasInstance> instances{
        MakeInstance(Vector3(-5.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f), 0u, 3u),
        MakeInstance(Vector3(9.0f, 4.0f, -2.0f), Vector3(2.0f, 2.0f, 2.0f), 0u, 3u)};
    const PackedTlas packed = TlasPacker::Pack({}, instances);
    ASSERT_FALSE(packed.IsEmpty());

    // Unit cube at (-5, 0, 0)      -> [-6, -1, -1] .. [-4, 1, 1]
    // Cube scaled 2x at (9, 4, -2) -> [ 7,  2, -4] .. [11, 6, 0]
    ExpectBoundsNear(packed.WorldBounds, AABB{{-6.0f, -1.0f, -4.0f}, {11.0f, 6.0f, 1.0f}},
                     kBoundsEpsilon);

    // The root node covers the whole scene by construction.
    const float32* root = &packed.Buffer[packed.TlasBase];
    ExpectBoundsNear(AABB{{root[0], root[1], root[2]}, {root[3], root[4], root[5]}},
                     packed.WorldBounds, kBoundsEpsilon);
}
