#include "SceneBvh/ThreadedBvhBuilder.h"

#include "JobSystem/ParallelAlgorithms.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <numeric>

namespace GameEngine::SceneBvh
{

namespace
{

using Mathematics::AABB;
using Mathematics::Vector3;

uint32 WidestAxis(const AABB& bounds)
{
    const float32 dx = bounds.max.x - bounds.min.x;
    const float32 dy = bounds.max.y - bounds.min.y;
    const float32 dz = bounds.max.z - bounds.min.z;
    if (dx >= dy && dx >= dz)
        return 0u;
    return dy >= dz ? 1u : 2u;
}

} // namespace

ThreadedBvhBuilder::ThreadedBvhBuilder(const MeshGeometryView& soup)
    : m_Soup(soup)
{
}

ThreadedBvh ThreadedBvhBuilder::Build(const MeshGeometryView& soup)
{
    ThreadedBvhBuilder builder(soup);
    if (!builder.PrepareTriangles())
        return {};
    if (!builder.Partition())
        return {};
    return builder.Emit();
}

void ThreadedBvhBuilder::BuildMany(JobSystem::WorkStealingThreadPool* pool,
                                   std::span<const MeshGeometryView> soups,
                                   std::span<ThreadedBvh> outBvhs)
{
    if (soups.size() != outBvhs.size())
    {
        Logger::Log::Error("ThreadedBvhBuilder::BuildMany: {} soups but {} output slots",
                           soups.size(), outBvhs.size());
        return;
    }

    JobSystem::ParallelFor(
        pool, 0u, soups.size(),
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                outBvhs[i] = Build(soups[i]);
        },
        kMeshBuildBatchSize);
}

bool ThreadedBvhBuilder::PrepareTriangles()
{
    m_VertexCount = m_Soup.VertexCount();
    m_TriangleCount = m_Soup.TriangleCount();

    if (m_TriangleCount == 0 || m_VertexCount < 3)
        return false;

    if (m_TriangleCount >= kThreadedBvhMaxTriangles)
    {
        Logger::Log::Error(
            "ThreadedBvhBuilder: {} triangles exceeds the leaf word's 24-bit offset ceiling ({})",
            m_TriangleCount, kThreadedBvhMaxTriangles);
        return false;
    }

    // Optional attributes are all-or-nothing: a short span means the caller
    // built the soup wrong, and silently zeroing the tail would ship a mesh
    // with corrupt shading data rather than surfacing the mistake.
    if (!m_Soup.TriangleMaterials.empty() && m_Soup.TriangleMaterials.size() < m_TriangleCount)
    {
        Logger::Log::Error("ThreadedBvhBuilder: {} triangle materials for {} triangles",
                           m_Soup.TriangleMaterials.size(), m_TriangleCount);
        return false;
    }

    if (!m_Soup.Normals.empty() && m_Soup.Normals.size() < static_cast<size_t>(m_VertexCount) * 3u)
    {
        Logger::Log::Error("ThreadedBvhBuilder: {} normal floats for {} vertices",
                           m_Soup.Normals.size(), m_VertexCount);
        return false;
    }

    if (!m_Soup.TexCoords.empty() &&
        m_Soup.TexCoords.size() < static_cast<size_t>(m_VertexCount) * 2u)
    {
        Logger::Log::Error("ThreadedBvhBuilder: {} texcoord floats for {} vertices",
                           m_Soup.TexCoords.size(), m_VertexCount);
        return false;
    }

    m_TriangleBounds.resize(static_cast<size_t>(m_TriangleCount) * 6u);
    m_TriangleCentroids.resize(static_cast<size_t>(m_TriangleCount) * 3u);
    m_Order.resize(m_TriangleCount);
    std::iota(m_Order.begin(), m_Order.end(), 0u);

    for (uint32 tri = 0; tri < m_TriangleCount; ++tri)
    {
        AABB bounds = AABB::Empty();
        for (uint32 corner = 0; corner < 3u; ++corner)
        {
            const uint32 slot = tri * 3u + corner;
            const uint32 vertex = m_Soup.Indices.empty() ? slot : m_Soup.Indices[slot];
            if (vertex >= m_VertexCount)
            {
                Logger::Log::Error("ThreadedBvhBuilder: index {} points outside the {}-vertex soup",
                                   vertex, m_VertexCount);
                return false;
            }
            const float32* position = &m_Soup.Positions[static_cast<size_t>(vertex) * 3u];
            bounds.Expand(Vector3(position[0], position[1], position[2]));
        }

        const size_t boundsBase = static_cast<size_t>(tri) * 6u;
        m_TriangleBounds[boundsBase + 0] = bounds.min.x;
        m_TriangleBounds[boundsBase + 1] = bounds.min.y;
        m_TriangleBounds[boundsBase + 2] = bounds.min.z;
        m_TriangleBounds[boundsBase + 3] = bounds.max.x;
        m_TriangleBounds[boundsBase + 4] = bounds.max.y;
        m_TriangleBounds[boundsBase + 5] = bounds.max.z;

        const size_t centroidBase = static_cast<size_t>(tri) * 3u;
        m_TriangleCentroids[centroidBase + 0] = (bounds.min.x + bounds.max.x) * 0.5f;
        m_TriangleCentroids[centroidBase + 1] = (bounds.min.y + bounds.max.y) * 0.5f;
        m_TriangleCentroids[centroidBase + 2] = (bounds.min.z + bounds.max.z) * 0.5f;
    }

    return true;
}

ThreadedBvhBuilder::RangeBounds ThreadedBvhBuilder::ComputeRangeBounds(uint32 lo, uint32 hi) const
{
    RangeBounds result{AABB::Empty(), AABB::Empty()};
    for (uint32 slot = lo; slot < hi; ++slot)
    {
        const size_t tri = m_Order[slot];
        const float32* bounds = &m_TriangleBounds[tri * 6u];
        result.Node.Expand(AABB{{bounds[0], bounds[1], bounds[2]},
                                {bounds[3], bounds[4], bounds[5]}});
        const float32* centroid = &m_TriangleCentroids[tri * 3u];
        result.Centroid.Expand(Vector3(centroid[0], centroid[1], centroid[2]));
    }
    return result;
}

uint32 ThreadedBvhBuilder::ChooseSahSplit(uint32 lo, uint32 hi, const RangeBounds& bounds)
{
    const uint32 count = hi - lo;
    const float32 parentArea = bounds.Node.SurfaceArea();
    if (parentArea <= 0.0f)
        return hi;

    BinnedSahRange range;
    range.Order = std::span<const uint32>(m_Order).subspan(lo, count);
    range.Centroids = m_TriangleCentroids;
    range.TriangleBounds = m_TriangleBounds;
    range.CentroidBounds = bounds.Centroid;
    const BinnedSahCandidate candidate = FindBestBinnedSahSplit(range);
    if (!candidate.Found)
        return hi;

    // Full SAH: a split must beat the leaf, both costs in ray/triangle-test
    // units. The kernel's Cost is the un-normalised Al*Nl + Ar*Nr numerator.
    const float32 splitCost = kNodeTraversalCost + candidate.Cost / parentArea;
    if (splitCost >= static_cast<float32>(count))
        return hi;

    const auto begin = m_Order.begin();
    const auto middle = std::partition(begin + lo, begin + hi, [&](uint32 tri) {
        return BinnedSahBinOf(m_TriangleCentroids[static_cast<size_t>(tri) * 3u + candidate.Axis],
                              candidate.AxisMin, candidate.Scale) <= candidate.Bin;
    });

    const uint32 split = static_cast<uint32>(middle - begin);
    return (split == lo || split == hi) ? hi : split;
}

uint32 ThreadedBvhBuilder::MedianSplit(uint32 lo, uint32 hi, const AABB& centroidBounds)
{
    const uint32 axis = WidestAxis(centroidBounds);
    const uint32 mid = lo + (hi - lo) / 2u;
    const auto begin = m_Order.begin();

    // Tie-break on the source triangle id so coincident centroids still yield a
    // total order, making the tree shape reproducible across runs.
    std::nth_element(begin + lo, begin + mid, begin + hi, [&](uint32 lhs, uint32 rhs) {
        const float32 a = m_TriangleCentroids[static_cast<size_t>(lhs) * 3u + axis];
        const float32 b = m_TriangleCentroids[static_cast<size_t>(rhs) * 3u + axis];
        return a != b ? a < b : lhs < rhs;
    });
    return mid;
}

uint32 ThreadedBvhBuilder::AppendNode(const AABB& bounds)
{
    const uint32 index = static_cast<uint32>(m_Nodes.size() / kThreadedBvhNodeStrideU32);
    m_Nodes.resize(m_Nodes.size() + kThreadedBvhNodeStrideU32);
    EncodeNodeBounds(m_Nodes.data(), index, bounds);
    return index;
}

bool ThreadedBvhBuilder::Partition()
{
    // Explicit worklist rather than recursion: a pathological soup can drive the
    // partition thousands of levels deep, and a Finalize entry is exactly the
    // "patch my miss link once my subtree is complete" step.
    struct WorkItem
    {
        uint32 Lo = 0;
        uint32 Hi = 0;
        uint32 NodeIndex = 0;
        bool Finalize = false;
    };

    std::vector<WorkItem> work;
    work.push_back(WorkItem{0u, m_TriangleCount, 0u, false});
    const size_t expectedNodes = 2u * (m_TriangleCount / kExpectedLeafOccupancy + 1u);
    m_Nodes.reserve(expectedNodes * kThreadedBvhNodeStrideU32);

    while (!work.empty())
    {
        const WorkItem item = work.back();
        work.pop_back();

        const uint32 nodeCount = static_cast<uint32>(m_Nodes.size() / kThreadedBvhNodeStrideU32);
        if (item.Finalize)
        {
            m_Nodes[item.NodeIndex * kThreadedBvhNodeStrideU32 + 6u] = nodeCount;
            continue;
        }

        if (nodeCount >= kThreadedBvhMaxNodes)
        {
            Logger::Log::Error("ThreadedBvhBuilder: node count exceeds the 24-bit ceiling ({})",
                               kThreadedBvhMaxNodes);
            return false;
        }

        const RangeBounds bounds = ComputeRangeBounds(item.Lo, item.Hi);
        const uint32 index = AppendNode(bounds.Node);
        const uint32 count = item.Hi - item.Lo;

        uint32 split = item.Hi;
        if (count > kMaxLeafTriangles)
        {
            split = ChooseSahSplit(item.Lo, item.Hi, bounds);
            if (split == item.Hi)
                split = MedianSplit(item.Lo, item.Hi, bounds.Centroid);
        }

        if (split == item.Hi)
        {
            m_Nodes[index * kThreadedBvhNodeStrideU32 + 6u] = index + 1u; // escape = next slot
            m_Nodes[index * kThreadedBvhNodeStrideU32 + 7u] = EncodeLeafWord(item.Lo, count);
            continue;
        }

        m_Nodes[index * kThreadedBvhNodeStrideU32 + 7u] = kThreadedBvhInteriorLeafWord;
        // Right pushed before left so the left child pops next and lands at
        // index + 1 — the contract the traversal's "descend = cursor + 1" and
        // the refit's "left child's miss link is the right child" both rest on.
        work.push_back(WorkItem{0u, 0u, index, true});
        work.push_back(WorkItem{split, item.Hi, 0u, false});
        work.push_back(WorkItem{item.Lo, split, 0u, false});
    }

    return true;
}

ThreadedBvh ThreadedBvhBuilder::Emit()
{
    ThreadedBvh bvh;
    bvh.Nodes = std::move(m_Nodes);
    bvh.LocalBounds = DecodeNodeBounds(bvh.Nodes.data(), 0u);

    bvh.TriangleIndices.resize(static_cast<size_t>(m_TriangleCount) * kTriangleIndexStride);
    bvh.TriangleMaterials.resize(m_TriangleCount);
    for (uint32 slot = 0; slot < m_TriangleCount; ++slot)
    {
        const uint32 tri = m_Order[slot];
        for (uint32 corner = 0; corner < kTriangleIndexStride; ++corner)
        {
            const uint32 sourceSlot = tri * 3u + corner;
            bvh.TriangleIndices[slot * kTriangleIndexStride + corner] =
                m_Soup.Indices.empty() ? sourceSlot : m_Soup.Indices[sourceSlot];
        }
        bvh.TriangleMaterials[slot] =
            m_Soup.TriangleMaterials.empty() ? 0u : m_Soup.TriangleMaterials[tri];
    }

    const bool hasNormals = m_Soup.Normals.size() >= static_cast<size_t>(m_VertexCount) * 3u;
    const bool hasTexCoords = m_Soup.TexCoords.size() >= static_cast<size_t>(m_VertexCount) * 2u;
    bvh.VertexData.assign(static_cast<size_t>(m_VertexCount) * kVertexDataStrideFloats, 0.0f);
    for (uint32 vertex = 0; vertex < m_VertexCount; ++vertex)
    {
        float32* record = &bvh.VertexData[static_cast<size_t>(vertex) * kVertexDataStrideFloats];
        const float32* position = &m_Soup.Positions[static_cast<size_t>(vertex) * 3u];
        record[0] = position[0];
        record[1] = position[1];
        record[2] = position[2];
        if (hasNormals)
        {
            const float32* normal = &m_Soup.Normals[static_cast<size_t>(vertex) * 3u];
            record[3] = normal[0];
            record[4] = normal[1];
            record[5] = normal[2];
        }
        if (hasTexCoords)
        {
            const float32* uv = &m_Soup.TexCoords[static_cast<size_t>(vertex) * 2u];
            record[6] = uv[0];
            record[7] = uv[1];
        }
    }

    return bvh;
}

} // namespace GameEngine::SceneBvh
