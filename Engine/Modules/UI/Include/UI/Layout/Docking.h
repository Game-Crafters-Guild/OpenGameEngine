#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{

class UIElement; // forward decl

// Desired docking target relative to another panel/area
enum class DockPosition
{
    Left,
    Right,
    Top,
    Bottom,
    Center
};

// Runtime state of a dockable panel
enum class DockState
{
    Docked,
    Floating,
    Tabbed
};

// Dock tree node. Either a leaf (tab stack of panels) or a split with two children.
class DockNode
{
  public:
    enum class RemoveActiveFallback
    {
        PreviousNeighbor,
        FirstTab
    };

    enum class Kind
    {
        Leaf,
        Split
    };

    DockNode() = default;

    // Leaf helpers
    static std::unique_ptr<DockNode> MakeLeaf();
    bool IsLeaf() const { return m_Kind == Kind::Leaf; }
    const std::vector<std::string>& GetTabs() const { return m_Tabs; }
    void AddTab(const std::string& panelId) { m_Tabs.push_back(panelId); }
    size_t GetActiveIndex() const { return m_ActiveIndex < m_Tabs.size() ? m_ActiveIndex : 0; }
    const std::string& GetActivePanelId() const;
    // Set the active tab by id without reordering; return true if found
    bool ActivateTab(const std::string& panelId);
    // Remove a tab by id from this leaf; return true if removed
    bool RemoveTab(const std::string& panelId, RemoveActiveFallback activeFallback = RemoveActiveFallback::PreviousNeighbor);

    // Split helpers
    static std::unique_ptr<DockNode> MakeSplit(DockPosition dir, float ratio = 0.5f);
    bool IsSplit() const { return m_Kind == Kind::Split; }
    DockPosition GetSplitDirection() const { return m_SplitDirection; }
    float GetSplitRatio() const { return m_SplitRatio; }
    DockNode* First() const { return m_First.get(); }
    DockNode* Second() const { return m_Second.get(); }
    void SetSplitRatio(float r) { m_SplitRatio = r; }

    // Min sizes for children (in pixels). Used to clamp ratios during resize.
    void SetMinChildSizes(float firstPx, float secondPx)
    {
        m_MinFirstPx = firstPx;
        m_MinSecondPx = secondPx;
    }
    float GetMinFirstPx() const { return m_MinFirstPx; }
    float GetMinSecondPx() const { return m_MinSecondPx; }

    // Mutators (for layout tools)
    void SetSplit(DockPosition dir, float ratio, std::unique_ptr<DockNode> first, std::unique_ptr<DockNode> second);

  private:
    Kind m_Kind = Kind::Leaf;

    // Leaf
    std::vector<std::string> m_Tabs; // panel IDs (order as authored)
    size_t m_ActiveIndex = 0;        // active tab index within m_Tabs

    // Split
    DockPosition m_SplitDirection = DockPosition::Left;
    float m_SplitRatio = 0.5f; // 0..1
    float m_MinFirstPx = 100.0f;
    float m_MinSecondPx = 100.0f;
    std::unique_ptr<DockNode> m_First;
    std::unique_ptr<DockNode> m_Second;
};

// Minimal docking manager: data-only, no rendering. Integrates with UI by storing panel contents.
class DockingManager
{
  public:
    struct LeafLayout
    {
        float x = 0.f, y = 0.f, width = 0.f, height = 0.f;
        std::vector<std::string> tabs; // panel IDs in this leaf (front = active)
    };

    struct NodeRect
    {
        float x = 0, y = 0, width = 0, height = 0;
        std::string path; // sequence of '0'/'1' from root
        bool isLeaf = true;
    };

    // Register a panel's root UI element (not owned). Caller owns element lifetime.
    void RegisterPanel(const std::string& id, UIElement* content)
    {
        m_Panels[id] = content;
    }

    UIElement* GetPanel(const std::string& id) const
    {
        auto it = m_Panels.find(id);
        return it == m_Panels.end() ? nullptr : it->second;
    }

    const std::unordered_map<std::string, UIElement*>& GetPanels() const { return m_Panels; }

    // Layout root (owned)
    const DockNode* GetRoot() const { return m_Root.get(); }
    DockNode* GetRoot() { return m_Root.get(); }
    // A preset/reset replaces the layout's identity, so this also drops the
    // session close locations remembered from the previous tree.
    void SetRoot(std::unique_ptr<DockNode> root);
    std::unique_ptr<DockNode> TakeRoot() { return std::move(m_Root); }
    bool IsEmpty() const;

    // Compute pixel rects for each leaf given a container size
    void ComputeLeafLayouts(float width, float height, std::vector<LeafLayout>& out) const;

    // Compute rects for all nodes (leaves and splits)
    void ComputeNodeRects(float width, float height, std::vector<NodeRect>& out) const;

    // Remove a panel tab by id and normalize the tree (prune empty leaves/splits)
    bool RemoveTab(const std::string& panelId);

    // Restore a panel to the leaf it most recently occupied before a normal
    // close. This state is intentionally session-only and is cleared by SetRoot
    // when a layout preset/reset replaces the current tree.
    bool RestoreLastClosedTab(const std::string& panelId);

    // Moving a dock tab out of a leaf should not leave the source leaf showing the
    // incidental neighbor that happened to sit left of the dragged tab. Prefer the
    // leaf's primary tab so dragging Animation out of the bottom group returns to Assets.
    bool RemoveTabForDockMove(const std::string& panelId);

    // Normalize the tree: remove empty leaves and collapse splits with a single child
    void Normalize();

    // Returns true if panelId is the active tab in the leaf that contains it (used to hide native overlays when tab is in background).
    bool IsPanelActiveTab(const std::string& panelId) const;

    // Activate a tab by panel id (bring to front in its leaf)
    bool ActivateTab(const std::string& panelId);

    // Editor-integration callbacks
    void SetUndockCallback(std::function<void(const std::string&)> cb) { m_OnRequestUndock = std::move(cb); }
    void RequestUndock(const std::string& panelId)
    {
        if (m_OnRequestUndock)
            m_OnRequestUndock(panelId);
    }

    // Minimum sizes API for a specific split path
    bool SetMinSizesByPath(const std::string& path, float minFirstPx, float minSecondPx);
    bool GetMinSizesByPath(const std::string& path, float& outFirstPx, float& outSecondPx) const;

    // Dock new panel at the root edge (wrap entire root into a split)
    bool DockToRoot(DockPosition edge, const std::string& panelId);

    // Split the subtree at path with a new panel along the given edge
    bool DockSplitSubtreeByPath(const std::string& path, DockPosition edge, const std::string& panelId);

    // Add panel as a tab into a leaf at the path. If the node at path is a split, targets its first leaf.
    bool DockAsTabInLeafByPath(const std::string& path, const std::string& panelId);

    // Reorder tabs within a leaf identified by path. fromIdx/toIdx are clamped to the tab count.
    bool ReorderTabInLeafByPath(const std::string& path, size_t fromIdx, size_t toIdx);

    // Update a split ratio by path from root; path is a sequence of '0'/'1' choosing first/second at each split
    bool SetSplitRatioByPath(const std::string& path, float ratio);

    // Find a node by path (sequence of '0'/'1' from root). Returns nullptr if path is invalid.
    const DockNode* FindNodeByPath(const std::string& path) const;

    // Find the leaf containing a panel by ID. Returns the leaf and its path (via output parameter).
    const DockNode* FindLeafContaining(const std::string& panelId, std::string& outPath) const;

  private:
    struct ClosedPanelPlacement
    {
        std::string leafPath;
        std::vector<std::string> sameLeafSiblings;
        std::vector<std::string> adjacentSubtreePanels;
        DockPosition recreateEdge = DockPosition::Center;
        float splitRatio = 0.5f;
    };

    // Produces the replacement for the subtree at `path`; receives nullptr when
    // the path runs past the live tree.
    using NodeTransform = std::function<std::unique_ptr<DockNode>(const DockNode*)>;

    static void CollectPanelIds(const DockNode* node, std::vector<std::string>& out);
    void RememberClosedPanelPlacement(const std::string& panelId);

    static bool ActivateInNode(DockNode* n, const std::string& panelId);
    static bool RemoveInNode(DockNode* n, const std::string& panelId, DockNode::RemoveActiveFallback activeFallback);
    static bool IsPanelActiveInNode(const DockNode* n, const std::string& panelId);
    static bool IsEmptyRecursive(const DockNode* n);
    const DockNode* FindLeafContainingRecursive(const DockNode* n, const std::string& panelId, std::string& path) const;
    void ComputeNode(const DockNode* n, float x, float y, float w, float h, std::vector<LeafLayout>& out) const;
    void ComputeNodeRectsRecursive(const DockNode* n, float x, float y, float w, float h,
                                   const std::string& path, std::vector<NodeRect>& out) const;
    static bool SetSplitRatioByPathRecursive(DockNode* n, const std::string& path, size_t i, float ratio);
    static bool SetMinSizesByPathRecursive(DockNode* n, const std::string& path, size_t i, float a, float b);
    static bool GetMinSizesByPathRecursive(const DockNode* n, const std::string& path, size_t i, float& outA, float& outB);
    std::unique_ptr<DockNode> CloneNormalized(const DockNode* n) const;
    std::unique_ptr<DockNode> CloneExact(const DockNode* n) const;
    std::unique_ptr<DockNode> CloneWithTransform(const DockNode* n, const std::string& path,
                                                 size_t i, const NodeTransform& transform) const;
    std::unique_ptr<DockNode> DockAsTabIntoFirstLeaf(const DockNode* sub, const std::string& panelId) const;

    std::unordered_map<std::string, UIElement*> m_Panels; // id -> content (not owned)
    std::unique_ptr<DockNode> m_Root;                     // layout tree
    std::unordered_map<std::string, ClosedPanelPlacement> m_LastClosedPlacements;

    // Integration hooks
    std::function<void(const std::string&)> m_OnRequestUndock;
};

} // namespace GameEngine
