#include "UI/Layout/Docking.h"

#include <algorithm>

namespace GameEngine
{

// --- DockNode ---

std::unique_ptr<DockNode> DockNode::MakeLeaf()
{
    return std::make_unique<DockNode>();
}

std::unique_ptr<DockNode> DockNode::MakeSplit(DockPosition dir, float ratio)
{
    auto n = std::make_unique<DockNode>();
    n->m_Kind = Kind::Split;
    n->m_SplitDirection = dir;
    n->m_SplitRatio = ratio;
    n->m_First = std::make_unique<DockNode>();
    n->m_Second = std::make_unique<DockNode>();
    return n;
}

const std::string& DockNode::GetActivePanelId() const
{
    static const std::string kEmpty;
    return m_Tabs.empty() ? kEmpty : m_Tabs[GetActiveIndex()];
}

bool DockNode::ActivateTab(const std::string& panelId)
{
    for (size_t i = 0; i < m_Tabs.size(); ++i)
    {
        if (m_Tabs[i] == panelId)
        {
            m_ActiveIndex = i;
            return true;
        }
    }
    return false;
}

bool DockNode::RemoveTab(const std::string& panelId, RemoveActiveFallback activeFallback)
{
    for (size_t i = 0; i < m_Tabs.size(); ++i)
    {
        if (m_Tabs[i] == panelId)
        {
            const std::string activeId = GetActivePanelId();
            const bool removedActive = (m_Tabs[i] == activeId);
            m_Tabs.erase(m_Tabs.begin() + static_cast<long long>(i));

            if (m_Tabs.empty())
            {
                m_ActiveIndex = 0;
            }
            else if (removedActive)
            {
                if (activeFallback == RemoveActiveFallback::FirstTab)
                    m_ActiveIndex = 0;
                else if (m_ActiveIndex >= m_Tabs.size())
                    m_ActiveIndex = m_Tabs.size() - 1;
            }
            else if (!activeId.empty())
            {
                (void)ActivateTab(activeId);
            }
            return true;
        }
    }
    return false;
}

void DockNode::SetSplit(DockPosition dir, float ratio, std::unique_ptr<DockNode> first, std::unique_ptr<DockNode> second)
{
    m_Kind = Kind::Split;
    m_SplitDirection = dir;
    m_SplitRatio = ratio;
    m_First = std::move(first);
    m_Second = std::move(second);
}

// --- DockingManager ---

void DockingManager::SetRoot(std::unique_ptr<DockNode> root)
{
    m_Root = std::move(root);
    // A preset/reset replaces the layout's identity. Session close
    // locations from the previous tree must not leak into the new one.
    m_LastClosedPlacements.clear();
}

bool DockingManager::IsEmpty() const
{
    return IsEmptyRecursive(m_Root.get());
}

void DockingManager::ComputeLeafLayouts(float width, float height, std::vector<LeafLayout>& out) const
{
    out.clear();
    if (!m_Root)
        return;
    ComputeNode(m_Root.get(), 0.f, 0.f, width, height, out);
}

void DockingManager::ComputeNodeRects(float width, float height, std::vector<NodeRect>& out) const
{
    out.clear();
    if (!m_Root)
        return;
    ComputeNodeRectsRecursive(m_Root.get(), 0.f, 0.f, width, height, "", out);
}

bool DockingManager::RemoveTab(const std::string& panelId)
{
    RememberClosedPanelPlacement(panelId);
    bool removed = RemoveInNode(m_Root.get(), panelId, DockNode::RemoveActiveFallback::PreviousNeighbor);
    if (removed)
        Normalize();
    return removed;
}

bool DockingManager::RestoreLastClosedTab(const std::string& panelId)
{
    if (ActivateTab(panelId))
        return false;

    const auto found = m_LastClosedPlacements.find(panelId);
    if (found == m_LastClosedPlacements.end())
        return false;
    const ClosedPanelPlacement placement = found->second;

    // Preferred case: at least one tab from the old stack still exists.
    for (const std::string& siblingId : placement.sameLeafSiblings)
    {
        std::string siblingPath;
        if (const DockNode* siblingLeaf = FindLeafContaining(siblingId, siblingPath))
        {
            if (siblingLeaf->IsLeaf() && DockAsTabInLeafByPath(siblingPath, panelId))
            {
                ActivateTab(panelId);
                m_LastClosedPlacements.erase(found);
                return true;
            }
        }
    }

    // If closing the only tab collapsed its split, reconstruct that split
    // around the nearest surviving sibling subtree.
    std::vector<std::string> livePaths;
    for (const std::string& siblingId : placement.adjacentSubtreePanels)
    {
        std::string path;
        if (FindLeafContaining(siblingId, path))
            livePaths.push_back(std::move(path));
    }
    if (!livePaths.empty())
    {
        std::string commonPath = livePaths.front();
        for (size_t i = 1; i < livePaths.size(); ++i)
        {
            size_t commonLength = 0;
            while (commonLength < commonPath.size() &&
                   commonLength < livePaths[i].size() &&
                   commonPath[commonLength] == livePaths[i][commonLength])
            {
                ++commonLength;
            }
            commonPath.resize(commonLength);
        }
        if (DockSplitSubtreeByPath(commonPath, placement.recreateEdge, panelId))
        {
            SetSplitRatioByPath(commonPath, placement.splitRatio);
            ActivateTab(panelId);
            m_LastClosedPlacements.erase(found);
            return true;
        }
    }

    // The surrounding layout may have changed after the close. Retain the
    // broad location when its old path still resolves to a live leaf.
    if (const DockNode* oldLeaf = FindNodeByPath(placement.leafPath);
        oldLeaf && oldLeaf->IsLeaf() && DockAsTabInLeafByPath(placement.leafPath, panelId))
    {
        ActivateTab(panelId);
        m_LastClosedPlacements.erase(found);
        return true;
    }
    return false;
}

bool DockingManager::RemoveTabForDockMove(const std::string& panelId)
{
    bool removed = RemoveInNode(m_Root.get(), panelId, DockNode::RemoveActiveFallback::FirstTab);
    if (removed)
        Normalize();
    return removed;
}

void DockingManager::Normalize()
{
    m_Root = CloneNormalized(m_Root.get());
}

bool DockingManager::IsPanelActiveTab(const std::string& panelId) const
{
    return IsPanelActiveInNode(m_Root.get(), panelId);
}

bool DockingManager::ActivateTab(const std::string& panelId)
{
    return ActivateInNode(m_Root.get(), panelId);
}

bool DockingManager::SetMinSizesByPath(const std::string& path, float minFirstPx, float minSecondPx)
{
    return SetMinSizesByPathRecursive(m_Root.get(), path, 0, minFirstPx, minSecondPx);
}

bool DockingManager::GetMinSizesByPath(const std::string& path, float& outFirstPx, float& outSecondPx) const
{
    return GetMinSizesByPathRecursive(m_Root.get(), path, 0, outFirstPx, outSecondPx);
}

bool DockingManager::DockToRoot(DockPosition edge, const std::string& panelId)
{
    auto newLeaf = DockNode::MakeLeaf();
    newLeaf->AddTab(panelId);
    if (!m_Root)
    {
        // Empty root becomes this leaf
        m_Root = std::move(newLeaf);
        return true;
    }
    // Wrap existing root according to edge
    std::unique_ptr<DockNode> old = std::move(m_Root);
    std::unique_ptr<DockNode> first, second;
    if (edge == DockPosition::Left || edge == DockPosition::Top)
    {
        first = std::move(newLeaf);
        second = std::move(old);
    }
    else
    { // Right/Bottom
        first = std::move(old);
        second = std::move(newLeaf);
    }
    auto split = std::make_unique<DockNode>();
    split->SetSplit(edge, 0.5f, std::move(first), std::move(second));
    m_Root = std::move(split);
    Normalize();
    return true;
}

bool DockingManager::DockSplitSubtreeByPath(const std::string& path, DockPosition edge, const std::string& panelId)
{
    auto transform = [&](const DockNode* sub) -> std::unique_ptr<DockNode>
    {
        auto newLeaf = DockNode::MakeLeaf();
        newLeaf->AddTab(panelId);
        // If target is null, just return the new leaf
        if (!sub)
            return newLeaf;
        // Clone subtree and wrap
        auto cloned = CloneExact(sub);
        std::unique_ptr<DockNode> first, second;
        if (edge == DockPosition::Left || edge == DockPosition::Top)
        {
            first = std::move(newLeaf);
            second = std::move(cloned);
        }
        else
        {
            first = std::move(cloned);
            second = std::move(newLeaf);
        }
        auto split = std::make_unique<DockNode>();
        split->SetSplit(edge, 0.5f, std::move(first), std::move(second));
        return split;
    };
    m_Root = CloneWithTransform(m_Root.get(), path, 0, transform);
    Normalize();
    return m_Root != nullptr;
}

bool DockingManager::DockAsTabInLeafByPath(const std::string& path, const std::string& panelId)
{
    auto transform = [&](const DockNode* sub) -> std::unique_ptr<DockNode>
    {
        // If sub is a leaf, clone and append tab; otherwise, descend to first child leaf
        if (!sub)
        {
            auto leaf = DockNode::MakeLeaf();
            leaf->AddTab(panelId);
            return leaf;
        }
        if (sub->IsLeaf())
        {
            auto leaf = CloneExact(sub);
            leaf->AddTab(panelId);
            return leaf;
        }
        // For splits: rebuild the same split, but append into its leftmost leaf
        auto a = DockAsTabIntoFirstLeaf(sub->First(), panelId);
        auto b = CloneExact(sub->Second());
        auto split = std::make_unique<DockNode>();
        split->SetSplit(sub->GetSplitDirection(), sub->GetSplitRatio(), std::move(a), std::move(b));
        return split;
    };
    m_Root = CloneWithTransform(m_Root.get(), path, 0, transform);
    Normalize();
    return m_Root != nullptr;
}

bool DockingManager::ReorderTabInLeafByPath(const std::string& path, size_t fromIdx, size_t toIdx)
{
    auto transform = [&](const DockNode* sub) -> std::unique_ptr<DockNode>
    {
        if (!sub)
            return nullptr;
        if (!sub->IsLeaf())
        {
            // Only reorder on leaves; if path points to a split, no-op
            return CloneExact(sub);
        }
        const auto& tabs = sub->GetTabs();
        if (tabs.empty())
            return CloneExact(sub);
        size_t n = tabs.size();
        if (fromIdx >= n)
            return CloneExact(sub);
        if (toIdx > n)
            toIdx = n; // allow inserting at end

        // Build new order
        std::vector<std::string> reordered;
        reordered.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (i != fromIdx)
                reordered.push_back(tabs[i]);
        }
        const std::string moved = tabs[fromIdx];
        // Adjust toIdx if removing an earlier element
        if (toIdx > fromIdx)
            toIdx -= 1;
        toIdx = std::min(toIdx, reordered.size());
        reordered.insert(reordered.begin() + static_cast<long long>(toIdx), moved);

        auto leaf = DockNode::MakeLeaf();
        for (const auto& id : reordered)
            leaf->AddTab(id);
        const std::string activeId = sub->GetActivePanelId();
        if (!activeId.empty())
            (void)leaf->ActivateTab(activeId);
        return leaf;
    };
    m_Root = CloneWithTransform(m_Root.get(), path, 0, transform);
    Normalize();
    return m_Root != nullptr;
}

bool DockingManager::SetSplitRatioByPath(const std::string& path, float ratio)
{
    return SetSplitRatioByPathRecursive(m_Root.get(), path, 0, ratio);
}

const DockNode* DockingManager::FindNodeByPath(const std::string& path) const
{
    const DockNode* n = m_Root.get();
    for (size_t i = 0; i < path.size() && n; ++i)
    {
        if (!n->IsSplit())
            return nullptr;
        n = (path[i] == '0') ? n->First() : n->Second();
    }
    return n;
}

const DockNode* DockingManager::FindLeafContaining(const std::string& panelId, std::string& outPath) const
{
    outPath.clear();
    return FindLeafContainingRecursive(m_Root.get(), panelId, outPath);
}

// --- Private helpers ---

void DockingManager::CollectPanelIds(const DockNode* node, std::vector<std::string>& out)
{
    if (!node)
        return;
    if (node->IsLeaf())
    {
        out.insert(out.end(), node->GetTabs().begin(), node->GetTabs().end());
        return;
    }
    CollectPanelIds(node->First(), out);
    CollectPanelIds(node->Second(), out);
}

void DockingManager::RememberClosedPanelPlacement(const std::string& panelId)
{
    std::string leafPath;
    const DockNode* leaf = FindLeafContaining(panelId, leafPath);
    if (!leaf)
        return;

    ClosedPanelPlacement placement;
    placement.leafPath = leafPath;
    for (const std::string& tabId : leaf->GetTabs())
        if (tabId != panelId)
            placement.sameLeafSiblings.push_back(tabId);

    if (placement.sameLeafSiblings.empty())
    {
        // Walk outward until a surviving sibling subtree is found. It is
        // enough to reconstruct the collapsed split on reopen.
        for (size_t depth = leafPath.size(); depth > 0; --depth)
        {
            const std::string parentPath = leafPath.substr(0, depth - 1);
            const DockNode* parent = FindNodeByPath(parentPath);
            if (!parent || !parent->IsSplit())
                continue;

            const bool targetWasFirst = leafPath[depth - 1] == '0';
            const DockNode* adjacent = targetWasFirst ? parent->Second() : parent->First();
            CollectPanelIds(adjacent, placement.adjacentSubtreePanels);
            if (placement.adjacentSubtreePanels.empty())
                continue;

            const DockPosition direction = parent->GetSplitDirection();
            const bool horizontal = direction == DockPosition::Left || direction == DockPosition::Right;
            placement.recreateEdge = targetWasFirst
                ? (horizontal ? DockPosition::Left : DockPosition::Top)
                : (horizontal ? DockPosition::Right : DockPosition::Bottom);
            placement.splitRatio = parent->GetSplitRatio();
            break;
        }
    }

    m_LastClosedPlacements[panelId] = std::move(placement);
}

bool DockingManager::ActivateInNode(DockNode* n, const std::string& panelId)
{
    if (!n)
        return false;
    if (n->IsLeaf())
        return n->ActivateTab(panelId);
    return ActivateInNode(n->First(), panelId) || ActivateInNode(n->Second(), panelId);
}

bool DockingManager::RemoveInNode(DockNode* n, const std::string& panelId, DockNode::RemoveActiveFallback activeFallback)
{
    if (!n)
        return false;
    if (n->IsLeaf())
        return n->RemoveTab(panelId, activeFallback);
    return RemoveInNode(n->First(), panelId, activeFallback) || RemoveInNode(n->Second(), panelId, activeFallback);
}

bool DockingManager::IsPanelActiveInNode(const DockNode* n, const std::string& panelId)
{
    if (!n)
        return false;
    if (n->IsLeaf())
    {
        const std::vector<std::string>& tabs = n->GetTabs();
        for (const auto& id : tabs)
        {
            if (id == panelId)
                return n->GetActivePanelId() == panelId;
        }
        return false;
    }
    return IsPanelActiveInNode(n->First(), panelId) || IsPanelActiveInNode(n->Second(), panelId);
}

bool DockingManager::IsEmptyRecursive(const DockNode* n)
{
    if (!n)
        return true;
    if (n->IsLeaf())
        return n->GetTabs().empty();
    return IsEmptyRecursive(n->First()) && IsEmptyRecursive(n->Second());
}

const DockNode* DockingManager::FindLeafContainingRecursive(const DockNode* n, const std::string& panelId, std::string& path) const
{
    if (!n)
        return nullptr;
    if (n->IsLeaf())
    {
        for (const auto& tab : n->GetTabs())
            if (tab == panelId)
                return n;
        return nullptr;
    }
    path.push_back('0');
    if (auto* r = FindLeafContainingRecursive(n->First(), panelId, path))
        return r;
    path.back() = '1';
    if (auto* r = FindLeafContainingRecursive(n->Second(), panelId, path))
        return r;
    path.pop_back();
    return nullptr;
}

void DockingManager::ComputeNode(const DockNode* n, float x, float y, float w, float h, std::vector<LeafLayout>& out) const
{
    if (!n)
        return;
    if (n->IsLeaf())
    {
        LeafLayout ll{};
        ll.x = x;
        ll.y = y;
        ll.width = w;
        ll.height = h;
        ll.tabs = n->GetTabs();
        out.push_back(std::move(ll));
        return;
    }
    // Split
    const float ratio = n->GetSplitRatio();
    if (n->GetSplitDirection() == DockPosition::Left || n->GetSplitDirection() == DockPosition::Right)
    {
        // Horizontal split: left/right
        float wA = w * ratio;
        float wB = w - wA;
        ComputeNode(n->First(), x, y, wA, h, out);
        ComputeNode(n->Second(), x + wA, y, wB, h, out);
    }
    else if (n->GetSplitDirection() == DockPosition::Top || n->GetSplitDirection() == DockPosition::Bottom)
    {
        // Vertical split: top/bottom
        float hA = h * ratio;
        float hB = h - hA;
        ComputeNode(n->First(), x, y, w, hA, out);
        ComputeNode(n->Second(), x, y + hA, w, hB, out);
    }
    else
    {
        // Center: treat as single region for now
        ComputeNode(n->First(), x, y, w, h, out);
    }
}

void DockingManager::ComputeNodeRectsRecursive(const DockNode* n, float x, float y, float w, float h,
                                               const std::string& path, std::vector<NodeRect>& out) const
{
    if (!n)
        return;
    NodeRect nr;
    nr.x = x;
    nr.y = y;
    nr.width = w;
    nr.height = h;
    nr.path = path;
    nr.isLeaf = n->IsLeaf();
    out.push_back(nr);
    if (!n->IsSplit())
        return;
    const float ratio = n->GetSplitRatio();
    if (n->GetSplitDirection() == DockPosition::Left || n->GetSplitDirection() == DockPosition::Right)
    {
        // Horizontal split: left/right
        float wA = w * ratio;
        float wB = w - wA;
        ComputeNodeRectsRecursive(n->First(), x, y, wA, h, path + "0", out);
        ComputeNodeRectsRecursive(n->Second(), x + wA, y, wB, h, path + "1", out);
    }
    else if (n->GetSplitDirection() == DockPosition::Top || n->GetSplitDirection() == DockPosition::Bottom)
    {
        // Vertical split: top/bottom
        float hA = h * ratio;
        float hB = h - hA;
        ComputeNodeRectsRecursive(n->First(), x, y, w, hA, path + "0", out);
        ComputeNodeRectsRecursive(n->Second(), x, y + hA, w, hB, path + "1", out);
    }
    else
    {
        // Center: treat as single region
        ComputeNodeRectsRecursive(n->First(), x, y, w, h, path + "0", out);
    }
}

bool DockingManager::SetSplitRatioByPathRecursive(DockNode* n, const std::string& path, size_t i, float ratio)
{
    if (!n)
        return false;
    if (!n->IsSplit())
        return false;
    if (i >= path.size())
    {
        n->SetSplitRatio(ratio);
        return true;
    }
    char c = path[i];
    DockNode* next = (c == '0') ? n->First() : n->Second();
    return SetSplitRatioByPathRecursive(next, path, i + 1, ratio);
}

bool DockingManager::SetMinSizesByPathRecursive(DockNode* n, const std::string& path, size_t i, float a, float b)
{
    if (!n)
        return false;
    if (!n->IsSplit())
        return false;
    if (i >= path.size())
    {
        n->SetMinChildSizes(a, b);
        return true;
    }
    char c = path[i];
    DockNode* next = (c == '0') ? n->First() : n->Second();
    return SetMinSizesByPathRecursive(next, path, i + 1, a, b);
}

bool DockingManager::GetMinSizesByPathRecursive(const DockNode* n, const std::string& path, size_t i, float& outA, float& outB)
{
    if (!n)
        return false;
    if (!n->IsSplit())
        return false;
    if (i >= path.size())
    {
        outA = n->GetMinFirstPx();
        outB = n->GetMinSecondPx();
        return true;
    }
    char c = path[i];
    const DockNode* next = (c == '0') ? n->First() : n->Second();
    return GetMinSizesByPathRecursive(next, path, i + 1, outA, outB);
}

std::unique_ptr<DockNode> DockingManager::CloneNormalized(const DockNode* n) const
{
    if (!n)
        return nullptr;
    if (n->IsLeaf())
    {
        const auto& tabs = n->GetTabs();
        if (tabs.empty())
            return nullptr;
        auto leaf = DockNode::MakeLeaf();
        for (const auto& id : tabs)
            leaf->AddTab(id);
        const std::string activeId = n->GetActivePanelId();
        if (!activeId.empty())
            (void)leaf->ActivateTab(activeId);
        return leaf;
    }
    // Split
    auto a = CloneNormalized(n->First());
    auto b = CloneNormalized(n->Second());
    if (!a && !b)
        return nullptr;
    if (!a)
        return b;
    if (!b)
        return a;
    auto split = std::make_unique<DockNode>();
    split->SetSplit(n->GetSplitDirection(), n->GetSplitRatio(), std::move(a), std::move(b));
    return split;
}

std::unique_ptr<DockNode> DockingManager::CloneExact(const DockNode* n) const
{
    if (!n)
        return nullptr;
    if (n->IsLeaf())
    {
        auto leaf = DockNode::MakeLeaf();
        for (const auto& id : n->GetTabs())
            leaf->AddTab(id);
        const std::string activeId = n->GetActivePanelId();
        if (!activeId.empty())
            (void)leaf->ActivateTab(activeId);
        return leaf;
    }
    auto a = CloneExact(n->First());
    auto b = CloneExact(n->Second());
    auto split = std::make_unique<DockNode>();
    split->SetSplit(n->GetSplitDirection(), n->GetSplitRatio(), std::move(a), std::move(b));
    return split;
}

std::unique_ptr<DockNode> DockingManager::DockAsTabIntoFirstLeaf(const DockNode* sub, const std::string& panelId) const
{
    if (!sub)
    {
        auto leaf = DockNode::MakeLeaf();
        leaf->AddTab(panelId);
        return leaf;
    }
    if (sub->IsLeaf())
    {
        auto leaf = CloneExact(sub);
        leaf->AddTab(panelId);
        return leaf;
    }
    auto a = DockAsTabIntoFirstLeaf(sub->First(), panelId);
    auto b = CloneExact(sub->Second());
    auto split = std::make_unique<DockNode>();
    split->SetSplit(sub->GetSplitDirection(), sub->GetSplitRatio(), std::move(a), std::move(b));
    return split;
}

std::unique_ptr<DockNode> DockingManager::CloneWithTransform(const DockNode* n, const std::string& path,
                                                             size_t i, const NodeTransform& transform) const
{
    if (!n)
    {
        // Missing subtree while descending; apply transform at this location
        return transform(nullptr);
    }
    if (i >= path.size())
    {
        return transform(n);
    }
    if (n->IsLeaf())
    {
        // Cannot descend further; apply transform here
        return transform(n);
    }
    const char c = path[i];
    if (c == '0')
    {
        auto a = CloneWithTransform(n->First(), path, i + 1, transform);
        auto b = CloneExact(n->Second());
        auto split = std::make_unique<DockNode>();
        split->SetSplit(n->GetSplitDirection(), n->GetSplitRatio(), std::move(a), std::move(b));
        return split;
    }
    else
    {
        auto a = CloneExact(n->First());
        auto b = CloneWithTransform(n->Second(), path, i + 1, transform);
        auto split = std::make_unique<DockNode>();
        split->SetSplit(n->GetSplitDirection(), n->GetSplitRatio(), std::move(a), std::move(b));
        return split;
    }
}

} // namespace GameEngine
