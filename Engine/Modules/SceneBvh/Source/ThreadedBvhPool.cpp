#include "SceneBvh/ThreadedBvhPool.h"

#include "Logger/Logger.h"

namespace GameEngine::SceneBvh
{

using SceneBvh::kThreadedBvhNodeStrideU32;

PooledThreadedBvh PoolThreadedBvhs(std::span<const ThreadedBvh> meshes,
                                std::span<const uint32_t> meshMaterialSlots)
{
    PooledThreadedBvh pooled;
    if (meshes.size() != meshMaterialSlots.size())
    {
        Logger::Log::Error("PoolThreadedBvhs: {} meshes but {} material slots", meshes.size(),
                           meshMaterialSlots.size());
        return pooled;
    }

    pooled.Ranges.resize(meshes.size());

    size_t totalNodeWords = 0;
    size_t totalIndexWords = 0;
    size_t totalVertexFloats = 0;
    for (const ThreadedBvh& mesh : meshes)
    {
        totalNodeWords += mesh.Nodes.size();
        totalIndexWords += mesh.TriangleIndices.size();
        totalVertexFloats += mesh.VertexData.size();
    }
    pooled.Nodes.reserve(totalNodeWords);
    pooled.TriangleIndices.reserve(totalIndexWords);
    pooled.TriangleMaterials.reserve(totalIndexWords / SceneBvh::kTriangleIndexStride);
    pooled.VertexData.reserve(totalVertexFloats);

    uint32_t nodeBase   = 0;
    uint32_t triBase    = 0;
    uint32_t vertexBase = 0;
    bool ceilingHit     = false;

    for (size_t m = 0; m < meshes.size(); ++m)
    {
        const ThreadedBvh& mesh = meshes[m];
        if (mesh.IsEmpty())
            continue;

        const uint32_t nodeCount     = mesh.NodeCount();
        const uint32_t triangleCount = mesh.TriangleCount();
        const uint32_t vertexCount   = mesh.VertexCount();

        // Both ceilings are hard format limits, not tuning: pooled node
        // indices ship as f32 in the instance record (exact only to 2^24), and
        // a pooled triangle offset has to fit the leaf word's 24-bit field,
        // which EncodeLeafWord would otherwise silently mask into a wrong
        // triangle. Once one mesh overflows, every later mesh would too.
        if (static_cast<uint64_t>(nodeBase) + nodeCount > kThreadedBvhMaxNodes ||
            static_cast<uint64_t>(triBase) + triangleCount > kThreadedBvhMaxTriangles)
        {
            if (!ceilingHit)
            {
                ceilingHit = true;
                Logger::Log::Error(
                    "PoolThreadedBvhs: pool full at mesh {} ({} nodes, {} triangles pooled); "
                    "remaining meshes excluded",
                    m, nodeBase, triBase);
            }
            continue;
        }

        pooled.Ranges[m] = PooledBvhRange{nodeBase, nodeBase + nodeCount};

        const size_t nodeWriteBase = pooled.Nodes.size();
        pooled.Nodes.insert(pooled.Nodes.end(), mesh.Nodes.begin(), mesh.Nodes.end());
        for (uint32_t n = 0; n < nodeCount; ++n)
        {
            uint32_t* node = pooled.Nodes.data() + nodeWriteBase + n * kThreadedBvhNodeStrideU32;
            node[6] += nodeBase;
            const uint32_t leafWord = node[7];
            if (!SceneBvh::IsInteriorLeafWord(leafWord))
            {
                node[7] = SceneBvh::EncodeLeafWord(
                    SceneBvh::DecodeLeafTriangleOffset(leafWord) + triBase,
                    SceneBvh::DecodeLeafTriangleCount(leafWord));
            }
        }

        for (const uint32_t vertexIndex : mesh.TriangleIndices)
            pooled.TriangleIndices.push_back(vertexIndex + vertexBase);

        pooled.TriangleMaterials.insert(pooled.TriangleMaterials.end(), triangleCount,
                                        meshMaterialSlots[m]);

        pooled.VertexData.insert(pooled.VertexData.end(), mesh.VertexData.begin(),
                                 mesh.VertexData.end());

        nodeBase += nodeCount;
        triBase += triangleCount;
        vertexBase += vertexCount;
    }

    return pooled;
}

}  // namespace GameEngine::SceneBvh
