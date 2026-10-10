#include "TlasInternal.h"

namespace GameEngine::Scene::Internal
{

namespace
{

// Recompute a leaf-node's bounds from all its leaves.
AABB RecomputeLeafNodeBounds(const SectorState& sector, const TlasNode& node)
{
    AABB out = sector.Leaves[node.FirstLeaf].WorldBounds;
    for (uint32 i = 1; i < node.LeafCount; ++i)
        out.Expand(sector.Leaves[node.FirstLeaf + i].WorldBounds);
    return out;
}

}  // anon namespace

// Refit propagates updated leaf bounds up to the root. Called after dirty
// leaves' WorldBounds have been re-derived from their entities'
// WorldTransform * LocalBounds.
//
// Strategy: collect unique leaf-node indices for all dirty leaves, then
// for each one, recompute its node's bounds from all its leaves and walk
// the parent chain expanding ancestor bounds.
void RefitDirty(SectorState& sector)
{
    if (sector.Nodes.empty())
        return;

    auto& dirty = sector.DirtyLeavesScratch;
    if (dirty.empty())
        return;

    // 1. Update each dirty leaf's parent leaf-node to recompute bounds
    //    from the full leaf range. We may visit the same leaf-node from
    //    multiple dirty leaves; track which nodes have been refit using
    //    a visited bitmap stored in LeafSeenThisSyncScratch.
    auto& visited = sector.LeafSeenThisSyncScratch;
    visited.assign(sector.Nodes.size(), 0u);

    // Stage 1: refit leaf nodes containing dirty leaves.
    std::vector<uint32> dirtyLeafNodes;
    dirtyLeafNodes.reserve(dirty.size());
    for (uint32 leafIdx : dirty)
    {
        const uint32 nodeIdx = sector.Leaves[leafIdx].ParentNode;
        if (nodeIdx >= sector.Nodes.size()) continue;
        if (visited[nodeIdx]) continue;
        visited[nodeIdx] = 1u;

        sector.Nodes[nodeIdx].Bounds = RecomputeLeafNodeBounds(sector, sector.Nodes[nodeIdx]);
        dirtyLeafNodes.push_back(nodeIdx);
    }

    // Stage 2: walk parent chain from each dirty leaf-node to root,
    // expanding ancestor bounds. Re-use `visited` to avoid re-walking
    // shared ancestors (set bit 1 = leaf-refit-done; bit 2 = ancestor-walked).
    constexpr uint32 kAncestorWalkedBit = 2u;
    for (uint32 leafNodeIdx : dirtyLeafNodes)
    {
        uint32 cursor = sector.Nodes[leafNodeIdx].Parent;
        AABB   childBounds = sector.Nodes[leafNodeIdx].Bounds;
        // Update this leaf-node's siblings might have shrunk too — we
        // recompute the parent node by combining its two children.
        while (cursor != ~0u)
        {
            if (visited[cursor] & kAncestorWalkedBit)
            {
                // Another dirty branch already walked through here; the
                // ancestor's bounds will be re-validated by that walk.
                // But we still need to ensure our updated childBounds is
                // included. Recompute from both children.
            }
            visited[cursor] |= kAncestorWalkedBit;

            const auto& parent = sector.Nodes[cursor];
            const AABB& leftB  = sector.Nodes[parent.Left].Bounds;
            const AABB& rightB = sector.Nodes[parent.Right].Bounds;
            AABB        newB   = leftB;
            newB.Expand(rightB);

            // Early-out: if bounds didn't change, ancestors above didn't
            // change either.
            const auto& curB = sector.Nodes[cursor].Bounds;
            const bool same =
                curB.min.x == newB.min.x && curB.min.y == newB.min.y && curB.min.z == newB.min.z &&
                curB.max.x == newB.max.x && curB.max.y == newB.max.y && curB.max.z == newB.max.z;
            sector.Nodes[cursor].Bounds = newB;
            if (same)
                break;

            childBounds = newB;
            cursor      = parent.Parent;
        }
    }

    dirty.clear();
}

}  // namespace GameEngine::Scene::Internal
