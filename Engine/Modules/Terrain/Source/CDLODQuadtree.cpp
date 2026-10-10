#include "Terrain/CDLODQuadtree.h"
#include "Terrain/Heightfield.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace GameEngine::Terrain
{

uint32 CDLODQuadtree::GetNodesPerAxisAtLevel(uint32 level) const
{
    if (level >= m_NumLevels)
        return 0;
    return m_LevelSizes[level];
}

const CDLODQuadtree::Node& CDLODQuadtree::GetNode(uint32 level, uint32 x, uint32 z) const
{
    return m_Nodes[GetNodeIndex(level, x, z)];
}

uint32 CDLODQuadtree::GetNodeIndex(uint32 level, uint32 x, uint32 z) const
{
    assert(level < m_NumLevels);
    assert(x < m_LevelSizes[level] && z < m_LevelSizes[level]);
    return m_LevelOffsets[level] + z * m_LevelSizes[level] + x;
}

void CDLODQuadtree::AllocateLevels(uint32 numLevels)
{
    assert(numLevels > 0 && numLevels <= kMaxLODLevels);

    m_NumLevels = numLevels;
    m_LevelOffsets.resize(numLevels);
    m_LevelSizes.resize(numLevels);

    uint32 totalNodes = 0;
    for (uint32 level = 0; level < numLevels; ++level)
    {
        const uint32 nodesPerAxis = 1u << (numLevels - 1 - level);
        m_LevelSizes[level] = nodesPerAxis;
        m_LevelOffsets[level] = totalNodes;
        totalNodes += nodesPerAxis * nodesPerAxis;
    }

    m_Nodes.assign(totalNodes, Node{0.0f, 0.0f});
}

void CDLODQuadtree::SetNodeMinMax(uint32 level, uint32 x, uint32 z,
                                   float32 minHeight, float32 maxHeight)
{
    assert(level < m_NumLevels);
    assert(x < m_LevelSizes[level] && z < m_LevelSizes[level]);
    const uint32 idx = m_LevelOffsets[level] + z * m_LevelSizes[level] + x;
    m_Nodes[idx].MinHeight = minHeight;
    m_Nodes[idx].MaxHeight = maxHeight;
}

void CDLODQuadtree::BuildCoarseLevelsFromChildren()
{
    for (uint32 level = 1; level < m_NumLevels; ++level)
    {
        const uint32 nodesPerAxis = m_LevelSizes[level];
        const uint32 childLevel = level - 1;

        for (uint32 z = 0; z < nodesPerAxis; ++z)
        {
            for (uint32 x = 0; x < nodesPerAxis; ++x)
            {
                float32 minH = std::numeric_limits<float32>::max();
                float32 maxH = std::numeric_limits<float32>::lowest();

                for (uint32 cz = 0; cz < 2; ++cz)
                {
                    for (uint32 cx = 0; cx < 2; ++cx)
                    {
                        const uint32 childX = x * 2 + cx;
                        const uint32 childZ = z * 2 + cz;

                        if (childX < m_LevelSizes[childLevel] && childZ < m_LevelSizes[childLevel])
                        {
                            const Node& child = GetNode(childLevel, childX, childZ);
                            minH = std::min(minH, child.MinHeight);
                            maxH = std::max(maxH, child.MaxHeight);
                        }
                    }
                }

                const uint32 idx = m_LevelOffsets[level] + z * nodesPerAxis + x;
                m_Nodes[idx].MinHeight = minH;
                m_Nodes[idx].MaxHeight = maxH;
            }
        }
    }
}

void CDLODQuadtree::PatchRegion(const HeightfieldData& heightfield,
                                int32 minSampleX, int32 minSampleZ,
                                int32 maxSampleX, int32 maxSampleZ)
{
    if (m_Nodes.empty() || m_NumLevels == 0)
        return; // not built; the caller must Build() first

    const uint32 finestNodes = m_LevelSizes[0];
    const uint32 samplesPerNodeX = std::max(1u, (heightfield.GetWidth() - 1) / finestNodes);
    const uint32 samplesPerNodeZ = std::max(1u, (heightfield.GetHeight() - 1) / finestNodes);

    minSampleX = std::max(0, minSampleX);
    minSampleZ = std::max(0, minSampleZ);
    maxSampleX = std::max(minSampleX, maxSampleX);
    maxSampleZ = std::max(minSampleZ, maxSampleZ);

    // A finest node covers samples [n*spn, n*spn+spn] inclusive, so it shares
    // its edge samples with the adjacent node — an edit on a node boundary also
    // moves the neighbour's min/max. Expand the node span by one on each side;
    // re-scanning an unchanged node just recomputes the same value (idempotent).
    const int32 last = static_cast<int32>(finestNodes) - 1;
    const int32 nodeMinX = std::clamp(minSampleX / static_cast<int32>(samplesPerNodeX) - 1, 0, last);
    const int32 nodeMinZ = std::clamp(minSampleZ / static_cast<int32>(samplesPerNodeZ) - 1, 0, last);
    const int32 nodeMaxX = std::clamp(maxSampleX / static_cast<int32>(samplesPerNodeX) + 1, 0, last);
    const int32 nodeMaxZ = std::clamp(maxSampleZ / static_cast<int32>(samplesPerNodeZ) + 1, 0, last);

    for (int32 nz = nodeMinZ; nz <= nodeMaxZ; ++nz)
    {
        for (int32 nx = nodeMinX; nx <= nodeMaxX; ++nx)
        {
            float32 minH, maxH;
            heightfield.GetMinMax(nx * static_cast<int32>(samplesPerNodeX),
                                  nz * static_cast<int32>(samplesPerNodeZ),
                                  static_cast<int32>(samplesPerNodeX + 1),
                                  static_cast<int32>(samplesPerNodeZ + 1),
                                  minH, maxH);
            SetNodeMinMax(0, static_cast<uint32>(nx), static_cast<uint32>(nz), minH, maxH);
        }
    }

    BuildCoarseLevelsFromChildren();
}

bool CDLODQuadtree::TryGetGlobalHeightRange(uint32 heightfieldWidth, uint32 heightfieldHeight,
                                            float32& outMin, float32& outMax) const
{
    if (m_Nodes.empty() || m_NumLevels == 0 || heightfieldWidth < 2 || heightfieldHeight < 2)
        return false;

    const uint32 finestNodes = m_LevelSizes[0];
    const uint32 samplesPerNodeX = std::max(1u, (heightfieldWidth - 1) / finestNodes);
    const uint32 samplesPerNodeZ = std::max(1u, (heightfieldHeight - 1) / finestNodes);
    if (finestNodes * samplesPerNodeX != heightfieldWidth - 1 ||
        finestNodes * samplesPerNodeZ != heightfieldHeight - 1)
        return false; // finest level doesn't tile the field exactly -> root omits edge samples

    // Level (m_NumLevels - 1) is the single-node root covering the whole field.
    const Node& root = m_Nodes[m_LevelOffsets[m_NumLevels - 1]];
    outMin = root.MinHeight;
    outMax = root.MaxHeight;
    return true;
}

void CDLODQuadtree::Build(const HeightfieldData& heightfield, uint32 numLevels)
{
    AllocateLevels(numLevels);

    // Build finest level from heightfield
    {
        const uint32 finestNodes = m_LevelSizes[0];
        const uint32 samplesPerNodeX = std::max(1u, (heightfield.GetWidth() - 1) / finestNodes);
        const uint32 samplesPerNodeZ = std::max(1u, (heightfield.GetHeight() - 1) / finestNodes);
        BuildLevel(heightfield, 0, samplesPerNodeX, samplesPerNodeZ);
    }

    BuildCoarseLevelsFromChildren();
}

void CDLODQuadtree::BuildLevel(const HeightfieldData& heightfield, uint32 level,
                                uint32 samplesPerNodeX, uint32 samplesPerNodeZ)
{
    const uint32 nodesPerAxis = m_LevelSizes[level];

    for (uint32 z = 0; z < nodesPerAxis; ++z)
    {
        for (uint32 x = 0; x < nodesPerAxis; ++x)
        {
            const int32 startX = static_cast<int32>(x * samplesPerNodeX);
            const int32 startZ = static_cast<int32>(z * samplesPerNodeZ);

            float32 minH, maxH;
            heightfield.GetMinMax(startX, startZ,
                                  static_cast<int32>(samplesPerNodeX + 1),
                                  static_cast<int32>(samplesPerNodeZ + 1),
                                  minH, maxH);

            const uint32 idx = m_LevelOffsets[level] + z * nodesPerAxis + x;
            m_Nodes[idx].MinHeight = minH;
            m_Nodes[idx].MaxHeight = maxH;
        }
    }
}

} // namespace GameEngine::Terrain
