#include "SceneBvh/ThreadedBvhRefit.h"

namespace GameEngine::SceneBvh
{

using Mathematics::AABB;

ThreadedBvhRefit::ThreadedBvhRefit(ThreadedBvh& bvh)
    : m_Bvh(&bvh)
    , m_NodeCount(bvh.NodeCount())
{
    // Structural preconditions. A caller that changed topology instead of just
    // moving vertices lands here, which is exactly the "rebuild, don't refit"
    // signal: the partition encoded in Nodes no longer describes this geometry.
    const bool layoutValid = m_NodeCount > 0 &&
                             bvh.Nodes.size() % kThreadedBvhNodeStrideU32 == 0 &&
                             bvh.TriangleIndices.size() % kTriangleIndexStride == 0 &&
                             bvh.VertexData.size() % kVertexDataStrideFloats == 0 &&
                             bvh.TriangleMaterials.size() == bvh.TriangleCount();
    if (!layoutValid)
    {
        m_RebuildRequired = true;
        return;
    }

    m_Cursor = static_cast<int64>(m_NodeCount) - 1;
}

ThreadedBvhRefitStatus ThreadedBvhRefit::Fail()
{
    m_RebuildRequired = true;
    m_Cursor = -1;
    m_LeafActive = false;
    return ThreadedBvhRefitStatus::RebuildRequired;
}

ThreadedBvhRefitStatus ThreadedBvhRefit::Step(uint32 workBudget)
{
    if (m_RebuildRequired)
        return ThreadedBvhRefitStatus::RebuildRequired;
    if (m_Cursor < 0)
        return ThreadedBvhRefitStatus::Complete;

    const bool bounded = workBudget != kUnboundedRefitBudget;
    if (bounded && workBudget == 0)
        return ThreadedBvhRefitStatus::Budgeted;

    uint32* nodes = m_Bvh->Nodes.data();
    const uint32* triangleIndices = m_Bvh->TriangleIndices.data();
    const float32* vertexData = m_Bvh->VertexData.data();
    const size_t triangleIndexCount = m_Bvh->TriangleIndices.size();
    const size_t vertexDataCount = m_Bvh->VertexData.size();
    // Saturating: once the budget hits zero it stays there, so every later
    // check in this call reports exhausted instead of wrapping around.
    uint32 remaining = workBudget;
    const auto budgetExhausted = [&]() {
        if (!bounded)
            return false;
        if (remaining > 0)
            --remaining;
        return remaining == 0;
    };

    while (m_Cursor >= 0)
    {
        const uint32 index = static_cast<uint32>(m_Cursor);
        const uint32 leafWord = DecodeNodeLeafWord(nodes, index);

        if (!IsInteriorLeafWord(leafWord))
        {
            if (!m_LeafActive)
            {
                const uint32 triangleCount = DecodeLeafTriangleCount(leafWord);
                if (triangleCount == 0)
                {
                    // The builder never emits empty leaves; if one appears,
                    // keep its stale finite bounds rather than writing an
                    // inverted box that would poison every ancestor.
                    --m_Cursor;
                    ++m_ProcessedNodes;
                    if (budgetExhausted() && m_Cursor >= 0)
                        return ThreadedBvhRefitStatus::Budgeted;
                    continue;
                }
                m_LeafActive = true;
                m_LeafNextTriangle = DecodeLeafTriangleOffset(leafWord);
                m_LeafEndTriangle = m_LeafNextTriangle + triangleCount;
                m_LeafBounds = AABB::Empty();
            }

            while (m_LeafNextTriangle < m_LeafEndTriangle)
            {
                const size_t triangleBase =
                    static_cast<size_t>(m_LeafNextTriangle) * kTriangleIndexStride;
                if (triangleBase + 2u >= triangleIndexCount)
                    return Fail();

                for (uint32 corner = 0; corner < kTriangleIndexStride; ++corner)
                {
                    const size_t vertexBase =
                        static_cast<size_t>(triangleIndices[triangleBase + corner]) *
                        kVertexDataStrideFloats;
                    if (vertexBase + 2u >= vertexDataCount)
                        return Fail();

                    m_LeafBounds.Expand(Mathematics::Vector3(vertexData[vertexBase + 0],
                                                             vertexData[vertexBase + 1],
                                                             vertexData[vertexBase + 2]));
                }

                ++m_LeafNextTriangle;
                ++m_ProcessedTriangles;
                if (budgetExhausted() && m_LeafNextTriangle < m_LeafEndTriangle)
                    return ThreadedBvhRefitStatus::Budgeted;
            }

            EncodeNodeBounds(nodes, index, m_LeafBounds);
            m_LeafActive = false;
        }
        else
        {
            const uint32 left = index + 1u;
            if (left >= m_NodeCount)
                return Fail();

            // Pre-order plus "miss = first node after my subtree" means the left
            // child's escape link lands exactly on its right sibling.
            const uint32 right = DecodeNodeMissLink(nodes, left);
            if (!(right > left && right < m_NodeCount))
                return Fail();

            AABB bounds = DecodeNodeBounds(nodes, left);
            bounds.Expand(DecodeNodeBounds(nodes, right));
            EncodeNodeBounds(nodes, index, bounds);
        }

        --m_Cursor;
        ++m_ProcessedNodes;
        if (budgetExhausted() && m_Cursor >= 0)
            return ThreadedBvhRefitStatus::Budgeted;
    }

    m_Bvh->LocalBounds = DecodeNodeBounds(nodes, 0u);
    return ThreadedBvhRefitStatus::Complete;
}

} // namespace GameEngine::SceneBvh
