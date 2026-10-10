#include <gtest/gtest.h>

#include "Components/Rendering/LightPhotometry.h"
#include "SceneBvh/ThreadedBvhPool.h"
#include "Engine/Rendering/DDGIEmissive.h"
#include "Engine/Rendering/DDGIEmitterAreas.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "TestDeviceHelper.h"
#include "Engine/Rendering/DDGIMaterialUpdates.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/DDGISceneService.h"
#include "SceneBvh/MeshGeometryView.h"
#include "SceneBvh/ThreadedBvh.h"
#include "SceneBvh/ThreadedBvhBuilder.h"
#include "SceneBvh/TlasPacker.h"
#include "SceneBvh/UberMaterial.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Multi-mesh BLAS pooling — the piece of the DDGI software lane that Phase A's
// SceneBvhTests cannot reach, because ThreadedBvhBuilder only ever emits
// single-mesh, BLAS-local trees. Every assertion below re-derives its expected
// answer from the ORIGINAL per-mesh trees and compares against an independent
// walk of the POOLED arrays, in the same brute-force-parity style
// SceneBvhTests uses for single-mesh traversal.

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::SceneBvh;
using Mathematics::AABB;
using Mathematics::Vector3;

namespace
{

constexpr float32 kRayEpsilon    = 1.0e-4f;
constexpr float32 kParityEpsilon = 1.0e-4f;

std::string ReadShaderSource(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
        return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct TestMesh
{
    std::vector<float32> Positions;
    std::vector<uint32> Indices;

    MeshGeometryView Soup() const { return MeshGeometryView{Positions, {}, {}, Indices, {}}; }
};

// Heightfield patch offset to `origin`, so several of these occupy disjoint
// regions and a ray through one cannot accidentally hit another.
TestMesh MakeGridMesh(uint32 cells, uint32 seed, const Vector3& origin)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float32> heightDist(-0.4f, 0.4f);

    TestMesh mesh;
    const uint32 verts = cells + 1u;
    for (uint32 z = 0; z < verts; ++z)
    {
        for (uint32 x = 0; x < verts; ++x)
        {
            mesh.Positions.push_back(origin.x + static_cast<float32>(x));
            mesh.Positions.push_back(origin.y + heightDist(rng));
            mesh.Positions.push_back(origin.z + static_cast<float32>(z));
        }
    }
    for (uint32 z = 0; z < cells; ++z)
    {
        for (uint32 x = 0; x < cells; ++x)
        {
            const uint32 v0 = z * verts + x;
            mesh.Indices.insert(mesh.Indices.end(), {v0, v0 + verts, v0 + 1u});
            mesh.Indices.insert(mesh.Indices.end(), {v0 + 1u, v0 + verts, v0 + verts + 1u});
        }
    }
    return mesh;
}

TestMesh MakeRandomSoup(uint32 triangleCount, uint32 seed, const Vector3& origin)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float32> centreDist(-6.0f, 6.0f);
    std::uniform_real_distribution<float32> sizeDist(0.05f, 1.0f);

    TestMesh mesh;
    for (uint32 t = 0; t < triangleCount; ++t)
    {
        const Vector3 centre(origin.x + centreDist(rng), origin.y + centreDist(rng),
                             origin.z + centreDist(rng));
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

struct RayHit
{
    bool Hit         = false;
    float32 T        = 0.0f;
    uint32 Triangle  = 0;   // pooled triangle index
    uint32 Material  = 0;   // pooled material slot
};

bool RayTriangle(const Vector3& origin, const Vector3& direction, const Vector3& p0,
                 const Vector3& p1, const Vector3& p2, float32& outT)
{
    const Vector3 edge1 = p1 - p0;
    const Vector3 edge2 = p2 - p0;
    const Vector3 pvec  = Vector3::Cross(direction, edge2);
    const float32 det   = Vector3::Dot(edge1, pvec);
    if (std::abs(det) < 1.0e-12f)
        return false;

    const float32 invDet = 1.0f / det;
    const Vector3 tvec   = origin - p0;
    const float32 u      = Vector3::Dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;

    const Vector3 qvec = Vector3::Cross(tvec, edge1);
    const float32 v    = Vector3::Dot(direction, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    const float32 t = Vector3::Dot(edge2, qvec) * invDet;
    if (t <= kRayEpsilon)
        return false;
    outT = t;
    return true;
}

Vector3 PooledVertexPosition(const SceneBvh::PooledThreadedBvh& pool, uint32 vertex)
{
    const float32* record = &pool.VertexData[static_cast<size_t>(vertex) * kVertexDataStrideFloats];
    return Vector3(record[0], record[1], record[2]);
}

void PooledTriangleVertices(const SceneBvh::PooledThreadedBvh& pool, uint32 triangle, Vector3& p0, Vector3& p1,
                            Vector3& p2)
{
    const size_t base = static_cast<size_t>(triangle) * kTriangleIndexStride;
    p0 = PooledVertexPosition(pool, pool.TriangleIndices[base + 0]);
    p1 = PooledVertexPosition(pool, pool.TriangleIndices[base + 1]);
    p2 = PooledVertexPosition(pool, pool.TriangleIndices[base + 2]);
}

// Threaded walk of ONE pooled BLAS range — the exact loop ddgi_trace_sw.comp's
// inner while runs, with the range bounds standing in for the instance
// record's BlasRoot/BlasEnd.
RayHit TraversePooledRange(const SceneBvh::PooledThreadedBvh& pool, const SceneBvh::PooledBvhRange& range,
                           const Vector3& origin, const Vector3& direction)
{
    RayHit best;
    if (range.IsEmpty())
        return best;

    const uint32* nodes = pool.Nodes.data();
    const Vector3 invDirection(1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z);
    float32 bestT = std::numeric_limits<float32>::max();

    uint32 cursor          = range.NodeBegin;
    uint32 visits          = 0;
    const uint32 nodeSpan  = range.NodeEnd - range.NodeBegin;
    const uint32 visitCap  = nodeSpan * 2u + 8u;

    while (cursor < range.NodeEnd)
    {
        EXPECT_LT(visits++, visitCap) << "pooled walk failed to terminate";
        if (visits >= visitCap)
            break;

        const AABB bounds   = DecodeNodeBounds(nodes, cursor);
        const uint32 miss   = DecodeNodeMissLink(nodes, cursor);
        const uint32 leaf   = DecodeNodeLeafWord(nodes, cursor);

        float32 tNear = std::numeric_limits<float32>::lowest();
        float32 tFar  = std::numeric_limits<float32>::max();
        for (int axis = 0; axis < 3; ++axis)
        {
            const float32 t0 = (bounds.min[axis] - origin[axis]) * invDirection[axis];
            const float32 t1 = (bounds.max[axis] - origin[axis]) * invDirection[axis];
            tNear = std::max(tNear, std::min(t0, t1));
            tFar  = std::min(tFar, std::max(t0, t1));
        }
        if (!(tFar >= std::max(tNear, 0.0f) && tNear < bestT))
        {
            cursor = miss;
            continue;
        }
        if (IsInteriorLeafWord(leaf))
        {
            ++cursor;
            continue;
        }

        const uint32 offset = DecodeLeafTriangleOffset(leaf);
        const uint32 count  = DecodeLeafTriangleCount(leaf);
        for (uint32 i = 0; i < count; ++i)
        {
            const uint32 triangle = offset + i;
            Vector3 p0, p1, p2;
            PooledTriangleVertices(pool, triangle, p0, p1, p2);
            float32 t = 0.0f;
            if (RayTriangle(origin, direction, p0, p1, p2, t) && t < bestT)
            {
                bestT         = t;
                best.Hit      = true;
                best.T        = t;
                best.Triangle = triangle;
                best.Material = pool.TriangleMaterials[triangle];
            }
        }
        cursor = miss;
    }
    return best;
}

// Ground truth taken from the SOURCE tree, independent of any pooled array.
RayHit BruteForceSourceMesh(const ThreadedBvh& bvh, const Vector3& origin, const Vector3& direction,
                            uint32& outTiedCount)
{
    RayHit best;
    float32 bestT = std::numeric_limits<float32>::max();
    outTiedCount  = 0;
    for (uint32 triangle = 0; triangle < bvh.TriangleCount(); ++triangle)
    {
        const size_t base = static_cast<size_t>(triangle) * kTriangleIndexStride;
        const auto vertex = [&](size_t k) {
            const float32* r =
                &bvh.VertexData[static_cast<size_t>(bvh.TriangleIndices[base + k]) *
                                kVertexDataStrideFloats];
            return Vector3(r[0], r[1], r[2]);
        };
        float32 t = 0.0f;
        if (!RayTriangle(origin, direction, vertex(0), vertex(1), vertex(2), t))
            continue;
        if (t < bestT)
        {
            bestT         = t;
            best.Hit      = true;
            best.T        = t;
            best.Triangle = triangle;  // BLAS-LOCAL triangle index
        }
    }
    if (!best.Hit)
        return best;
    for (uint32 triangle = 0; triangle < bvh.TriangleCount(); ++triangle)
    {
        const size_t base = static_cast<size_t>(triangle) * kTriangleIndexStride;
        const auto vertex = [&](size_t k) {
            const float32* r =
                &bvh.VertexData[static_cast<size_t>(bvh.TriangleIndices[base + k]) *
                                kVertexDataStrideFloats];
            return Vector3(r[0], r[1], r[2]);
        };
        float32 t = 0.0f;
        if (RayTriangle(origin, direction, vertex(0), vertex(1), vertex(2), t) &&
            std::abs(t - bestT) <= kParityEpsilon)
            ++outTiedCount;
    }
    return best;
}

// Triangle base of mesh `index`, recomputed from the source trees rather than
// read back out of the pool, so the parity check has an independent expectation.
uint32 SourceTriangleBase(const std::vector<ThreadedBvh>& bvhs, size_t index)
{
    uint32 base = 0;
    for (size_t m = 0; m < index; ++m)
        base += bvhs[m].TriangleCount();
    return base;
}

uint32 SourceVertexBase(const std::vector<ThreadedBvh>& bvhs, size_t index)
{
    uint32 base = 0;
    for (size_t m = 0; m < index; ++m)
        base += bvhs[m].VertexCount();
    return base;
}

std::vector<ThreadedBvh> BuildScene(const std::vector<TestMesh>& meshes)
{
    std::vector<ThreadedBvh> bvhs;
    bvhs.reserve(meshes.size());
    for (const TestMesh& mesh : meshes)
        bvhs.push_back(ThreadedBvhBuilder::Build(mesh.Soup()));
    return bvhs;
}

std::vector<uint32> MaterialSlots(size_t count, uint32 first)
{
    std::vector<uint32> slots(count);
    for (size_t i = 0; i < count; ++i)
        slots[i] = first + static_cast<uint32>(i);
    return slots;
}

}  // namespace

// End-to-end pin: two plan slots over the SAME geometry with two distinct baked
// materials must survive pooling + packing as two distinct material records —
// the packed scene the software kernel reads, mirroring RebuildFromPlan.
TEST(DDGIPlanKeyTest, DistinctPlanSlotsBakeDistinctPackedMaterials)
{
    const TestMesh mesh = MakeGridMesh(4u, 71u, Vector3(0.0f, 0.0f, 0.0f));
    // Same geometry twice (a mesh instanced with two materials becomes two plan
    // meshes over identical geometry), each its own pool/material slot.
    const std::vector<ThreadedBvh> bvhs{ThreadedBvhBuilder::Build(mesh.Soup()),
                                        ThreadedBvhBuilder::Build(mesh.Soup())};
    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 0u));
    ASSERT_EQ(2u, pool.Ranges.size());

    UberMaterial redMat{};
    redMat.BaseColorR = 0.9f;
    redMat.BaseColorG = 0.1f;
    redMat.BaseColorB = 0.1f;
    UberMaterial blueMat{};
    blueMat.BaseColorR = 0.1f;
    blueMat.BaseColorG = 0.1f;
    blueMat.BaseColorB = 0.9f;
    const std::vector<UberMaterial> materials{redMat, blueMat};

    std::vector<TlasInstance> tlasInstances;
    for (size_t m = 0; m < bvhs.size(); ++m)
    {
        TlasInstance inst;
        inst.WorldFromLocal = Mathematics::Matrix4x4::Identity();
        inst.LocalBounds    = bvhs[m].LocalBounds;
        inst.BlasRoot       = pool.Ranges[m].NodeBegin;
        inst.BlasEnd        = pool.Ranges[m].NodeEnd;
        tlasInstances.push_back(inst);
    }

    const PackedTlas packed = TlasPacker::Pack(materials, tlasInstances);
    ASSERT_FALSE(packed.IsEmpty());
    ASSERT_EQ(2u, packed.MaterialCount);

    // Materials lead the packed buffer, kUberMaterialStrideFloats apart.
    const float32 mat0R = packed.Buffer[0 * kUberMaterialStrideFloats + 0];
    const float32 mat1R = packed.Buffer[1 * kUberMaterialStrideFloats + 0];
    EXPECT_FLOAT_EQ(0.9f, mat0R);
    EXPECT_FLOAT_EQ(0.1f, mat1R);
    EXPECT_NE(mat0R, mat1R) << "the two instances must carry distinct baked albedo";
}

// The premise DDGISceneService's transform-only update path rests on: a BLAS is
// LOCAL-space content, so moving an instance changes the packed instance and
// TLAS records and nothing else. If that ever stopped holding — a pooled node
// picking up a world-space term, a leaf offset varying with the transform — the
// cheap path would serve stale geometry from a pool it was entitled to reuse,
// which is a wrong image with no crash and no log line.
TEST(DDGIDynamicsTest, MovingAnInstanceLeavesEveryBlasArrayByteIdentical)
{
    const std::vector<TestMesh> meshes{MakeGridMesh(5u, 91u, Vector3(0.0f, 0.0f, 0.0f)),
                                       MakeRandomSoup(30u, 92u, Vector3(40.0f, 0.0f, 0.0f))};
    const std::vector<ThreadedBvh> bvhs = BuildScene(meshes);
    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 0u));
    ASSERT_EQ(bvhs.size(), pool.Ranges.size());

    // Re-pooling the same trees is what a full rebuild would do. Byte equality
    // against the first pool is what entitles the service to skip it.
    const SceneBvh::PooledThreadedBvh repooled = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 0u));
    EXPECT_EQ(pool.Nodes, repooled.Nodes);
    EXPECT_EQ(pool.TriangleIndices, repooled.TriangleIndices);
    EXPECT_EQ(pool.TriangleMaterials, repooled.TriangleMaterials);
    EXPECT_EQ(pool.VertexData, repooled.VertexData);

    const std::vector<UberMaterial> materials{UberMaterial{}};
    auto packAt = [&](const Mathematics::Matrix4x4& secondTransform)
    {
        std::vector<TlasInstance> instances;
        for (size_t m = 0; m < bvhs.size(); ++m)
        {
            TlasInstance inst;
            inst.WorldFromLocal = m == 1 ? secondTransform : Mathematics::Matrix4x4::Identity();
            inst.LocalBounds    = bvhs[m].LocalBounds;
            inst.BlasRoot       = pool.Ranges[m].NodeBegin;
            inst.BlasEnd        = pool.Ranges[m].NodeEnd;
            inst.MaterialSlot   = 0u;
            instances.push_back(inst);
        }
        return TlasPacker::Pack(materials, instances);
    };

    const PackedTlas rest = packAt(Mathematics::Matrix4x4::Identity());
    const PackedTlas moved = packAt(Mathematics::Matrix4x4::Translation(Vector3(12.0f, 3.0f, -7.0f)));
    ASSERT_FALSE(rest.IsEmpty());
    ASSERT_FALSE(moved.IsEmpty());

    // Same table shape, so the shader's uSwScene offsets survive a re-pack —
    // the service reuses them rather than re-deriving descriptors.
    EXPECT_EQ(rest.Buffer.size(), moved.Buffer.size());
    EXPECT_EQ(rest.InstanceBase, moved.InstanceBase);
    EXPECT_EQ(rest.TlasBase, moved.TlasBase);
    EXPECT_EQ(rest.TlasNodeCount, moved.TlasNodeCount);
    EXPECT_EQ(rest.InstanceCount, moved.InstanceCount);

    // The material table is upstream of the instance records and must not move.
    for (uint32 i = 0; i < rest.InstanceBase; ++i)
        EXPECT_FLOAT_EQ(rest.Buffer[i], moved.Buffer[i]) << "material float " << i;

    // ...and the move must actually have reached the packed instance records,
    // or this test would pass on a no-op.
    bool anyInstanceFloatMoved = false;
    for (uint32 i = rest.InstanceBase; i < rest.TlasBase; ++i)
        anyInstanceFloatMoved = anyInstanceFloatMoved || rest.Buffer[i] != moved.Buffer[i];
    EXPECT_TRUE(anyInstanceFloatMoved);
}

TEST(ThreadedBvhPoolTest, MismatchedMaterialSlotCountPoolsNothing)
{
    const std::vector<TestMesh> meshes{MakeGridMesh(4u, 1u, Vector3(0.0f, 0.0f, 0.0f))};
    const std::vector<ThreadedBvh> bvhs = BuildScene(meshes);
    const std::vector<uint32> slots{0u, 1u};

    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, slots);
    EXPECT_TRUE(pool.Nodes.empty());
    EXPECT_TRUE(pool.Ranges.empty());
}

TEST(ThreadedBvhPoolTest, RangesAndArraySizesAreRunningSumsOfTheSources)
{
    const std::vector<TestMesh> meshes{MakeGridMesh(6u, 11u, Vector3(0.0f, 0.0f, 0.0f)),
                                       MakeRandomSoup(40u, 12u, Vector3(50.0f, 0.0f, 0.0f)),
                                       MakeGridMesh(3u, 13u, Vector3(0.0f, 0.0f, 50.0f))};
    const std::vector<ThreadedBvh> bvhs = BuildScene(meshes);
    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 0u));

    ASSERT_EQ(bvhs.size(), pool.Ranges.size());
    uint32 expectedNodeBase = 0;
    for (size_t m = 0; m < bvhs.size(); ++m)
    {
        EXPECT_EQ(expectedNodeBase, pool.Ranges[m].NodeBegin) << "mesh " << m;
        EXPECT_EQ(expectedNodeBase + bvhs[m].NodeCount(), pool.Ranges[m].NodeEnd) << "mesh " << m;
        expectedNodeBase += bvhs[m].NodeCount();
    }

    EXPECT_EQ(static_cast<size_t>(expectedNodeBase) * kThreadedBvhNodeStrideU32, pool.Nodes.size());
    EXPECT_EQ(SourceTriangleBase(bvhs, bvhs.size()) * kTriangleIndexStride,
              pool.TriangleIndices.size());
    EXPECT_EQ(SourceTriangleBase(bvhs, bvhs.size()), pool.TriangleMaterials.size());
    EXPECT_EQ(static_cast<size_t>(SourceVertexBase(bvhs, bvhs.size())) * kVertexDataStrideFloats,
              pool.VertexData.size());
}

TEST(ThreadedBvhPoolTest, MissLinksAndLeafOffsetsAreRebasedPerMesh)
{
    const std::vector<TestMesh> meshes{MakeGridMesh(5u, 21u, Vector3(0.0f, 0.0f, 0.0f)),
                                       MakeGridMesh(7u, 22u, Vector3(40.0f, 0.0f, 0.0f)),
                                       MakeRandomSoup(30u, 23u, Vector3(0.0f, 40.0f, 0.0f))};
    const std::vector<ThreadedBvh> bvhs = BuildScene(meshes);
    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 5u));

    for (size_t m = 0; m < bvhs.size(); ++m)
    {
        const ThreadedBvh& source   = bvhs[m];
        const uint32 nodeBase       = pool.Ranges[m].NodeBegin;
        const uint32 triangleBase   = SourceTriangleBase(bvhs, m);
        const uint32 vertexBase     = SourceVertexBase(bvhs, m);
        ASSERT_FALSE(source.IsEmpty());

        for (uint32 n = 0; n < source.NodeCount(); ++n)
        {
            const uint32 pooledNode = nodeBase + n;
            // Bounds ride along untouched: they are local-space, and the
            // instance transform is applied at the TLAS level.
            const AABB sourceBounds = DecodeNodeBounds(source.Nodes.data(), n);
            const AABB pooledBounds = DecodeNodeBounds(pool.Nodes.data(), pooledNode);
            EXPECT_FLOAT_EQ(sourceBounds.min.x, pooledBounds.min.x);
            EXPECT_FLOAT_EQ(sourceBounds.max.z, pooledBounds.max.z);

            EXPECT_EQ(DecodeNodeMissLink(source.Nodes.data(), n) + nodeBase,
                      DecodeNodeMissLink(pool.Nodes.data(), pooledNode))
                << "mesh " << m << " node " << n;

            const uint32 sourceLeaf = DecodeNodeLeafWord(source.Nodes.data(), n);
            const uint32 pooledLeaf = DecodeNodeLeafWord(pool.Nodes.data(), pooledNode);
            if (IsInteriorLeafWord(sourceLeaf))
            {
                EXPECT_TRUE(IsInteriorLeafWord(pooledLeaf)) << "mesh " << m << " node " << n;
                continue;
            }
            EXPECT_FALSE(IsInteriorLeafWord(pooledLeaf)) << "mesh " << m << " node " << n;
            EXPECT_EQ(DecodeLeafTriangleOffset(sourceLeaf) + triangleBase,
                      DecodeLeafTriangleOffset(pooledLeaf));
            EXPECT_EQ(DecodeLeafTriangleCount(sourceLeaf), DecodeLeafTriangleCount(pooledLeaf));
        }

        for (uint32 i = 0; i < source.TriangleIndices.size(); ++i)
        {
            EXPECT_EQ(source.TriangleIndices[i] + vertexBase,
                      pool.TriangleIndices[static_cast<size_t>(triangleBase) * kTriangleIndexStride + i]);
        }
        for (uint32 t = 0; t < source.TriangleCount(); ++t)
        {
            // The mesh's GLOBAL uber-material slot, not the source tree's
            // all-zero per-triangle table.
            EXPECT_EQ(5u + static_cast<uint32>(m), pool.TriangleMaterials[triangleBase + t]);
        }
    }
}

TEST(ThreadedBvhPoolTest, EmptyMeshesGetEmptyRangesAndConsumeNoSpace)
{
    std::vector<ThreadedBvh> bvhs;
    bvhs.push_back(ThreadedBvhBuilder::Build(TestMesh{}.Soup()));  // empty
    bvhs.push_back(ThreadedBvhBuilder::Build(MakeGridMesh(4u, 31u, Vector3(0.0f, 0.0f, 0.0f)).Soup()));
    bvhs.push_back(ThreadedBvhBuilder::Build(TestMesh{}.Soup()));  // empty
    ASSERT_TRUE(bvhs[0].IsEmpty());
    ASSERT_FALSE(bvhs[1].IsEmpty());

    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 0u));
    EXPECT_TRUE(pool.Ranges[0].IsEmpty());
    EXPECT_TRUE(pool.Ranges[2].IsEmpty());
    EXPECT_EQ(0u, pool.Ranges[1].NodeBegin);
    EXPECT_EQ(bvhs[1].NodeCount(), pool.Ranges[1].NodeEnd);
    EXPECT_EQ(static_cast<size_t>(bvhs[1].NodeCount()) * kThreadedBvhNodeStrideU32,
              pool.Nodes.size());
}

// The load-bearing check: after rebasing, a walk of the POOLED arrays over a
// mesh's own range must find the same hit — at the same distance, on the same
// triangle, carrying that mesh's material — as brute force over the ORIGINAL
// tree. A wrong node base, triangle base or vertex base breaks this even when
// every size assertion above still passes.
TEST(ThreadedBvhPoolTest, PooledTraversalHitsTheSameTrianglesAsTheSourceTrees)
{
    const std::vector<TestMesh> meshes{MakeGridMesh(12u, 41u, Vector3(0.0f, 0.0f, 0.0f)),
                                       MakeGridMesh(9u, 42u, Vector3(100.0f, 0.0f, 0.0f)),
                                       MakeRandomSoup(120u, 43u, Vector3(0.0f, 0.0f, 100.0f)),
                                       MakeGridMesh(15u, 44u, Vector3(100.0f, 0.0f, 100.0f))};
    const std::vector<ThreadedBvh> bvhs = BuildScene(meshes);
    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 3u));
    ASSERT_EQ(bvhs.size(), pool.Ranges.size());

    std::mt19937 rng(9001u);
    std::uniform_real_distribution<float32> spanDist(-3.0f, 18.0f);
    std::uniform_real_distribution<float32> tiltDist(-0.3f, 0.3f);

    uint32 hitsChecked = 0;
    for (size_t m = 0; m < bvhs.size(); ++m)
    {
        const uint32 triangleBase = SourceTriangleBase(bvhs, m);
        // Each mesh sits at a known offset; aim rays down through its patch.
        const Vector3 meshOrigin(m == 1u || m == 3u ? 100.0f : 0.0f, 0.0f,
                                 m >= 2u ? 100.0f : 0.0f);
        for (uint32 i = 0; i < 250u; ++i)
        {
            const Vector3 origin(meshOrigin.x + spanDist(rng), 25.0f, meshOrigin.z + spanDist(rng));
            const Vector3 direction = Vector3(tiltDist(rng), -1.0f, tiltDist(rng)).Normalize();

            uint32 tiedCount        = 0;
            const RayHit reference  = BruteForceSourceMesh(bvhs[m], origin, direction, tiedCount);
            const RayHit traversed  = TraversePooledRange(pool, pool.Ranges[m], origin, direction);

            ASSERT_EQ(reference.Hit, traversed.Hit) << "mesh " << m << " ray " << i;
            if (!reference.Hit)
                continue;

            ++hitsChecked;
            EXPECT_NEAR(reference.T, traversed.T, kParityEpsilon) << "mesh " << m;
            EXPECT_EQ(3u + static_cast<uint32>(m), traversed.Material) << "mesh " << m;
            // Only well defined when one triangle uniquely owns the closest t
            // (a shared edge can tie), same caveat as SceneBvhTests'.
            if (tiedCount == 1)
                EXPECT_EQ(reference.Triangle + triangleBase, traversed.Triangle) << "mesh " << m;
        }
    }
    EXPECT_GT(hitsChecked, 0u) << "the fixture produced no hits — the parity check proved nothing";
}

// A pooled range must be watertight: rays aimed at one mesh's region must never
// be answered by another mesh's geometry, which is what a node base leaking
// across ranges would produce.
TEST(ThreadedBvhPoolTest, PooledRangesDoNotBleedIntoEachOther)
{
    const std::vector<TestMesh> meshes{MakeGridMesh(10u, 51u, Vector3(0.0f, 0.0f, 0.0f)),
                                       MakeGridMesh(10u, 52u, Vector3(0.0f, 8.0f, 0.0f))};
    const std::vector<ThreadedBvh> bvhs = BuildScene(meshes);
    const SceneBvh::PooledThreadedBvh pool = PoolThreadedBvhs(bvhs, MaterialSlots(bvhs.size(), 0u));

    // Two stacked patches: a downward ray through both hits the upper one
    // first, but a walk restricted to mesh 0's range must report the LOWER one.
    std::mt19937 rng(4711u);
    std::uniform_real_distribution<float32> spanDist(1.0f, 9.0f);
    uint32 checked = 0;
    for (uint32 i = 0; i < 200u; ++i)
    {
        const Vector3 origin(spanDist(rng), 30.0f, spanDist(rng));
        const Vector3 direction(0.0f, -1.0f, 0.0f);

        const RayHit lower = TraversePooledRange(pool, pool.Ranges[0], origin, direction);
        const RayHit upper = TraversePooledRange(pool, pool.Ranges[1], origin, direction);
        if (!lower.Hit || !upper.Hit)
            continue;

        ++checked;
        EXPECT_GT(lower.T, upper.T) << "mesh 0's range answered with the upper patch";
        EXPECT_EQ(0u, lower.Material);
        EXPECT_EQ(1u, upper.Material);
        EXPECT_GE(lower.Triangle, SourceTriangleBase(bvhs, 0));
        EXPECT_LT(lower.Triangle, SourceTriangleBase(bvhs, 1));
        EXPECT_GE(upper.Triangle, SourceTriangleBase(bvhs, 1));
        EXPECT_LT(upper.Triangle, SourceTriangleBase(bvhs, 2));
    }
    EXPECT_GT(checked, 0u) << "no ray hit both patches — the separation check proved nothing";
}

// ── Emissive units ─────────────────────────────────────────────────────────
//
// The software lane bakes emission into its uber-material record CPU-side while
// the hardware lane reads uParams18 in the kernel, so nothing structural forces
// the two to agree on units. Both must land on the RASTER composition
// (Surfaces/standard_pbr.glsl):
//
//     map * colour * (nits / referenceWhite)
//
// Dropping the divide is silent — the frame still renders, just with GI 203x
// hotter than the emitter that produced it — which is exactly the class of
// regression a pin test exists for.

TEST(DDGIEmissiveTest, AuthoredNitsAreFoldedByTheReferenceWhiteAnchor)
{
    const DDGIEmissive baked = ComputeDDGIEmissive(0.25f, 0.5f, 1.0f, 203.0f);

    // 203 nits IS the anchor, so it must come out as scene-linear 1.0 — the
    // whole point of the constant.
    EXPECT_FLOAT_EQ(0.25f, baked.Color[0]);
    EXPECT_FLOAT_EQ(0.5f, baked.Color[1]);
    EXPECT_FLOAT_EQ(1.0f, baked.Color[2]);

    // Away from the anchor the colour scales by nits / referenceWhite.
    constexpr float32 kNits = 812.0f;  // 4x the anchor
    const DDGIEmissive bright = ComputeDDGIEmissive(0.2f, 0.4f, 0.8f, kNits);
    const float32 expectedScale = kNits / Components::kReferenceWhiteNits;
    EXPECT_FLOAT_EQ(0.2f * expectedScale, bright.Color[0]);
    EXPECT_FLOAT_EQ(0.4f * expectedScale, bright.Color[1]);
    EXPECT_FLOAT_EQ(0.8f * expectedScale, bright.Color[2]);
}

// The GLSL half of the contract cannot be linked against, so the shader source
// is read as the artifact under test (the pattern
// DDGIGlossyAtlasLayoutTests/IblShaderContractTests use). Three things have to
// hold together or the two lanes drift apart in units:
//
//   * surface_io.glsl's anchor is numerically kReferenceWhiteNits;
//   * ddgi_hit_shade.glsl reaches that anchor by INCLUDING surface_io.glsl
//     rather than restating the number, so the raster path and the GI lanes
//     cannot be edited apart;
//   * the hardware kernel divides uParams18.w by it, which is the half of the
//     fix the C++ ComputeDDGIEmissive assertions above cannot see;
//   * the emissive map multiplies that colour, as on the raster surface.
TEST(DDGIEmissiveTest, BothTraceLanesFoldEmissionByTheSameShaderAnchor)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::filesystem::path shaders =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders";

    const std::string surfaceIo = ReadShaderSource(shaders / "Includes/surface_io.glsl");
    ASSERT_FALSE(surfaceIo.empty());
    const std::string anchor = "const float GE_EMISSION_PAPERWHITE_NITS = " +
                               std::to_string(static_cast<int>(Components::kReferenceWhiteNits)) + ".0;";
    EXPECT_NE(surfaceIo.find(anchor), std::string::npos)
        << "the shader emission anchor no longer matches kReferenceWhiteNits ("
        << Components::kReferenceWhiteNits << "); the software lane's CPU bake would fold "
        << "emission by a different constant than the raster surface and the hardware kernel";

    const std::string hitShade = ReadShaderSource(shaders / "Includes/ddgi_hit_shade.glsl");
    ASSERT_FALSE(hitShade.empty());
    EXPECT_NE(hitShade.find("#include \"surface_io.glsl\""), std::string::npos)
        << "ddgi_hit_shade.glsl must take the anchor from the raster path's own header, not "
        << "restate it — a restated copy is what lets the two drift";
    EXPECT_NE(hitShade.find("if (layer < -1.5)\n        return vec3(0.0);"), std::string::npos)
        << "a map that is assigned but not resident (DDGIMaterialMapAtlas::kAwaitedLayer) must emit nothing "
        << "in the trace lanes, as the raster surface's black awaited default does";
    EXPECT_NE(hitShade.find("return emissiveFactor * textureLod(ge_ddgiMapAtlas"), std::string::npos)
        << "the emissive map must multiply the emission colour in both trace lanes, as the raster "
        << "surface does";

    const std::string hwTrace = ReadShaderSource(shaders / "ddgi_trace_hw.comp");
    ASSERT_FALSE(hwTrace.empty());
    EXPECT_NE(hwTrace.find("GE_DDGIMatEmission(mat).w * (1.0 / GE_EMISSION_PAPERWHITE_NITS)"),
              std::string::npos)
        << "the hardware lane no longer folds authored nits to scene-linear; it would emit "
        << Components::kReferenceWhiteNits << "x the energy the raster surface shows";
#endif
}

// The raster half of the same composition: both standard surfaces multiply the emissive map by
// the emission colour and luminance (uParams18), the rule the trace lanes follow above. An
// additive line here would light a map's black texels with the colour.
TEST(DDGIEmissiveTest, StandardSurfacesMultiplyTheMapLikeTheTraceLanes)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::filesystem::path surfaces =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders/Surfaces";
    const std::pair<const char*, const char*> kSurfaces[] = {
        {"standard_pbr.glsl", "o.emissive = emissiveTex * Mat.uParams18.rgb * (Mat.uParams18.w"},
        {"standard_pbr_extended.glsl", "o.emissive = emissive * Mat.uParams18.rgb * (Mat.uParams18.w"},
    };
    for (const auto& [file, line] : kSurfaces)
    {
        const std::string source = ReadShaderSource(surfaces / file);
        ASSERT_FALSE(source.empty()) << file;
        EXPECT_NE(source.find(line), std::string::npos)
            << file << " no longer multiplies the emissive map by the emission colour and luminance";
    }
#endif
}

TEST(DDGIMaterialUpdates, PatchesChangedRecordsWithoutTouchingInstancesOrTlas)
{
    std::vector<UberMaterial> previous(6);
    for (size_t i = 0; i < previous.size(); ++i)
        previous[i].BaseColorR = 0.1f * static_cast<float>(i + 1);
    auto current = previous;
    current[1].BaseColorG = 0.2f;
    current[2].EmissiveR = 3.0f;
    current[4].Roughness = 0.25f;

    TlasInstance instance{};
    instance.LocalBounds = AABB{Vector3(-1, -1, -1), Vector3(1, 1, 1)};
    instance.BlasEnd = 1;
    instance.MaterialSlot = 2;
    const std::vector<TlasInstance> instances{instance};
    auto patched = TlasPacker::Pack(previous, instances);
    const auto expected = TlasPacker::Pack(current, instances);
    ASSERT_FALSE(patched.IsEmpty());
    const auto original = patched.Buffer;

    std::vector<DDGIMaterialUpdateRange> ranges;
    ASSERT_TRUE(PlanDDGIMaterialUpdates(previous, current, ranges));
    ASSERT_EQ(ranges.size(), 2u);
    size_t changedRecords = 0;
    for (const auto& range : ranges)
    {
        changedRecords += range.Count;
        std::memcpy(patched.Buffer.data() + range.First * kUberMaterialStrideFloats,
                    current.data() + range.First, range.Count * sizeof(UberMaterial));
    }
    EXPECT_EQ(changedRecords, 3u);
    EXPECT_EQ(patched.Buffer, expected.Buffer);
    EXPECT_TRUE(std::equal(original.begin() + patched.InstanceBase, original.end(),
                           patched.Buffer.begin() + patched.InstanceBase));

    // Every shading value and uv transform is patchable in place, and an
    // unchanged table plans no work.
    auto shaded = current;
    shaded[0].BaseColorB = 0.125f;
    shaded[0].Metalness = 0.4f;
    shaded[0].EmissiveG = 12.0f;
    shaded[0].UvRepeatX = 2.0f;
    shaded[0].UvOffsetY = 0.3f;
    shaded[0].AlphaTest = 0.5f;
    ASSERT_TRUE(PlanDDGIMaterialUpdates(current, shaded, ranges));
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].First, 0u);
    EXPECT_EQ(ranges[0].Count, 1u);
    EXPECT_TRUE(PlanDDGIMaterialUpdates(shaded, shaded, ranges));
    EXPECT_TRUE(ranges.empty());
}

TEST(DDGIMaterialUpdates, RejectsBindingChangesAndTableSplits)
{
    const std::vector<UberMaterial> previous(2);
    const std::array<float UberMaterial::*, 6> bindings{
        &UberMaterial::AlbedoMapLayer, &UberMaterial::NormalMapLayer,
        &UberMaterial::RoughnessMapLayer, &UberMaterial::MetalnessMapLayer,
        &UberMaterial::EmissiveMapLayer, &UberMaterial::AlphaMapLayer};
    for (const auto binding : bindings)
    {
        auto current = previous;
        current[0].BaseColorR = 0.2f;  // a preceding patch must not survive rejection
        current[1].*binding = 3.0f;
        std::vector<DDGIMaterialUpdateRange> ranges;
        EXPECT_FALSE(PlanDDGIMaterialUpdates(previous, current, ranges));
        EXPECT_TRUE(ranges.empty());
    }
    auto split = previous;
    split.emplace_back();
    std::vector<DDGIMaterialUpdateRange> ranges;
    EXPECT_FALSE(PlanDDGIMaterialUpdates(previous, split, ranges));
    EXPECT_TRUE(ranges.empty());
}

namespace
{
Mesh MakeEmitterQuad(float width, float height)
{
    Mesh mesh;
    mesh.Vertices.resize(4);
    const std::array<std::array<float, 3>, 4> positions{{
        {0, 0, 0}, {width, 0, 0}, {width, height, 0}, {0, height, 0}}};
    for (size_t i = 0; i < positions.size(); ++i)
        for (size_t axis = 0; axis < 3; ++axis)
            mesh.Vertices[i].Position[axis] = positions[i][axis];
    mesh.Indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}
}

TEST(DDGIEmitterArea, EqualAreaPanelsKeepEqualPowerDespiteDifferentBounds)
{
    const Mathematics::Matrix4x4 identity;
    EXPECT_FLOAT_EQ(ComputeDDGIEmitterSurfaceArea(MakeEmitterQuad(4, 4), identity), 16.0f);
    const float square = ComputeDDGIEmitterSurfaceArea(MakeEmitterQuad(1, 1), identity);
    const float strip = ComputeDDGIEmitterSurfaceArea(MakeEmitterQuad(8, 0.125f), identity);
    EXPECT_FLOAT_EQ(square, 1.0f);
    EXPECT_FLOAT_EQ(strip, square);
}

TEST(DDGIEmitterArea, ProjectedAreaIsTheDirectionAverageOfTheEmittingFaces)
{
    // Average the visible area of a unit panel over a dense uniform set of
    // view directions: one emitting face is seen from half the sphere, two
    // faces from all of it. The proxy's weight must match that average, or a
    // double-sided panel delivers half the power its hit-traced surface does.
    const float surfaceArea = ComputeDDGIEmitterSurfaceArea(MakeEmitterQuad(1, 1), Mathematics::Matrix4x4());
    constexpr int kDirections = 200000;
    double frontSum = 0.0;
    double bothSum = 0.0;
    for (int i = 0; i < kDirections; ++i)
    {
        // Uniform directions have a uniform cosine to the panel normal (+Z)
        // (Archimedes), and a flat panel's visible area depends on nothing else.
        const double z = 1.0 - (2.0 * i + 1.0) / kDirections;
        frontSum += std::max(z, 0.0);
        bothSum += std::abs(z);
    }
    const float frontAverage = static_cast<float>(surfaceArea * frontSum / kDirections);
    const float bothAverage = static_cast<float>(surfaceArea * bothSum / kDirections);
    EXPECT_NEAR(ComputeDDGIEmitterProjectedArea(surfaceArea, false), frontAverage, 1.0e-4f);
    EXPECT_NEAR(ComputeDDGIEmitterProjectedArea(surfaceArea, true), bothAverage, 1.0e-4f);
}

TEST(DDGIEmitterArea, MeasuresWorldSpaceTrianglesUnderAnyTransform)
{
    const auto mesh = MakeEmitterQuad(2, 3);
    Mathematics::Matrix4x4 transform;
    transform[0] = glm::vec4(2, 0, 1, 0);
    transform[1] = glm::vec4(1, 3, 0, 0);
    // The transformed edge basis has cross product (-3, 1, 6).
    const float expected = 6.0f * std::sqrt(46.0f);
    EXPECT_NEAR(ComputeDDGIEmitterSurfaceArea(mesh, transform), expected, 1.0e-4f);
    transform[3] = glm::vec4(1.0e8f, -1.0e8f, 1.0e8f, 1);
    EXPECT_NEAR(ComputeDDGIEmitterSurfaceArea(mesh, transform), expected, 1.0e-4f);
    transform[0] *= -1.0f;
    EXPECT_NEAR(ComputeDDGIEmitterSurfaceArea(mesh, transform), expected, 1.0e-4f);

    // Unindexed triangle lists measure the same; out-of-range and repeated
    // indices add nothing, and a flattening transform measures zero.
    const auto& indexed = mesh;
    Mesh unindexed;
    for (const auto index : indexed.Indices)
        unindexed.Vertices.push_back(indexed.Vertices[index]);
    const Mathematics::Matrix4x4 identity;
    EXPECT_FLOAT_EQ(ComputeDDGIEmitterSurfaceArea(unindexed, identity), 6.0f);
    auto extra = indexed;
    extra.Indices.insert(extra.Indices.end(), {0, 0, 0, 0, 1, 999});
    EXPECT_FLOAT_EQ(ComputeDDGIEmitterSurfaceArea(extra, identity), 6.0f);
    Mathematics::Matrix4x4 flattened;
    flattened[0] = glm::vec4(0);
    EXPECT_FLOAT_EQ(ComputeDDGIEmitterSurfaceArea(indexed, flattened), 0.0f);
}

TEST(DDGIEmitterArea, CachedAreasFollowTheirInstanceIndices)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    Rendering::GPUScene scene(device.get());
    ASSERT_TRUE(scene.Initialize(32, 32));
    Rendering::MeshGPURegistry registry;
    registry.Initialize(device.get());
    registry.SetGPUScene(&scene);
    const auto handle = registry.RegisterSubmesh({GUID::Generate(), 0}, MakeEmitterQuad(1, 1), true);
    ASSERT_TRUE(handle.IsValid());
    std::array<Rendering::GPUInstance, 4> instances{};
    for (auto& instance : instances)
        instance.meshIndex = registry.Find(handle)->gpuMeshIndex;
    instances[1].flags = Rendering::kInstanceFlagGIEmitter;
    instances[3].flags = Rendering::kInstanceFlagGIEmitter;
    instances[3].transform[0] *= 3.0f;
    DDGIEmitterAreas areas;
    areas.Update(registry, instances);
    EXPECT_FALSE(areas.GetSurfaceArea(0).has_value());
    EXPECT_FLOAT_EQ(areas.GetSurfaceArea(1).value_or(0.0f), 1.0f);
    EXPECT_FALSE(areas.GetSurfaceArea(2).has_value());
    EXPECT_FLOAT_EQ(areas.GetSurfaceArea(3).value_or(0.0f), 3.0f);

    // An emitter that stops emitting drops out; the others keep their areas.
    instances[1].flags = 0;
    instances[2].flags = Rendering::kInstanceFlagGIEmitter;
    areas.Update(registry, instances);
    EXPECT_FALSE(areas.GetSurfaceArea(1).has_value());
    EXPECT_FLOAT_EQ(areas.GetSurfaceArea(2).value_or(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(areas.GetSurfaceArea(3).value_or(0.0f), 3.0f);
    EXPECT_FALSE(areas.GetSurfaceArea(4).has_value());
    registry.Shutdown();
    scene.Shutdown();
}

TEST(DDGIEmitterArea, CacheDoesNotKeepUnregisteredGeometryAlive)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    Rendering::GPUScene scene(device.get());
    ASSERT_TRUE(scene.Initialize(32, 32));
    Rendering::MeshGPURegistry registry;
    registry.Initialize(device.get());
    registry.SetGPUScene(&scene);
    const Rendering::MeshGPUKey key{GUID::Generate(), 0};
    const auto handle = registry.RegisterSubmesh(key, MakeEmitterQuad(2, 3), true);
    ASSERT_TRUE(handle.IsValid());
    const auto* entry = registry.Find(handle);
    ASSERT_NE(entry, nullptr);
    const std::weak_ptr<const Mesh> geometry = entry->cpuMesh;
    Rendering::GPUInstance instance{};
    instance.meshIndex = entry->gpuMeshIndex;
    instance.flags = Rendering::kInstanceFlagGIEmitter;
    DDGIEmitterAreas areas;
    areas.Update(registry, std::span(&instance, 1));
    ASSERT_TRUE(areas.GetSurfaceArea(0).has_value());

    // The probe feature stops updating the cache while its light list is
    // full, so the cache must not be what keeps the geometry alive.
    registry.UnregisterSubmesh(key);
    EXPECT_TRUE(geometry.expired());
    registry.Shutdown();
    scene.Shutdown();
}

TEST(DDGIEmitterArea, CachedAreaTracksContentTransformRemovalAndRowReuse)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    Rendering::GPUScene scene(device.get());
    ASSERT_TRUE(scene.Initialize(32, 32));
    Rendering::MeshGPURegistry registry;
    registry.Initialize(device.get());
    registry.SetGPUScene(&scene);
    const Rendering::MeshGPUKey key{GUID::Generate(), 0};
    auto mesh = MakeEmitterQuad(2, 3);
    const auto handle = registry.RegisterSubmesh(key, mesh, true);
    ASSERT_TRUE(handle.IsValid());
    const auto* entry = registry.Find(handle);
    ASSERT_NE(entry, nullptr);
    Rendering::GPUInstance instance{};
    instance.meshIndex = entry->gpuMeshIndex;
    instance.flags = Rendering::kInstanceFlagGIEmitter;
    DDGIEmitterAreas areas;
    areas.Update(registry, std::span(&instance, 1));
    ASSERT_TRUE(areas.GetSurfaceArea(0).has_value());
    EXPECT_FLOAT_EQ(*areas.GetSurfaceArea(0), 6.0f);
    const auto unchangedRevision = registry.GetContentRevision();
    registry.RegisterSubmesh(key, mesh, true);
    EXPECT_EQ(registry.GetContentRevision(), unchangedRevision);
    for (int tick = 0; tick < 20; ++tick)
        areas.Update(registry, std::span(&instance, 1));
    EXPECT_FLOAT_EQ(*areas.GetSurfaceArea(0), 6.0f);

    instance.transform[0] *= 2.0f;
    areas.Update(registry, std::span(&instance, 1));
    EXPECT_FLOAT_EQ(*areas.GetSurfaceArea(0), 12.0f);
    instance.transform[3] = glm::vec4(100, -200, 300, 1);
    areas.Update(registry, std::span(&instance, 1));
    EXPECT_FLOAT_EQ(*areas.GetSurfaceArea(0), 12.0f);

    mesh = MakeEmitterQuad(4, 3);
    EXPECT_EQ(registry.RegisterSubmesh(key, mesh, true), handle);
    EXPECT_GT(registry.GetContentRevision(), unchangedRevision);
    areas.Update(registry, std::span(&instance, 1));
    EXPECT_FLOAT_EQ(*areas.GetSurfaceArea(0), 24.0f);
    registry.UnregisterSubmesh(key);
    areas.Update(registry, std::span(&instance, 1));
    EXPECT_FALSE(areas.GetSurfaceArea(0).has_value());

    const Rendering::MeshGPUKey replacement{GUID::Generate(), 0};
    const auto next = registry.RegisterSubmesh(replacement, MakeEmitterQuad(1, 1), true);
    instance.meshIndex = registry.Find(next)->gpuMeshIndex;
    areas.Update(registry, std::span(&instance, 1));
    ASSERT_TRUE(areas.GetSurfaceArea(0).has_value());
    EXPECT_FLOAT_EQ(*areas.GetSurfaceArea(0), 2.0f);
    const auto beforeRemoval = registry.GetContentRevision();
    registry.UnregisterModel(replacement.assetGuid);
    EXPECT_GT(registry.GetContentRevision(), beforeRemoval);
    areas.Update(registry, std::span(&instance, 1));
    EXPECT_FALSE(areas.GetSurfaceArea(0).has_value());
    registry.Shutdown();
    scene.Shutdown();
}
