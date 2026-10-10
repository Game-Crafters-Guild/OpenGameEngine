#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "UI/UIElement.h"
#include "UI/Layout/Docking.h"
#include "UI/Layout/DockingHitTest.h"

namespace GameEngine {

class Mount;
class DockLeaf;
class Label;

enum class DockPatchFallbackReason : uint8_t
{
    None = 0,
    SourceLeafCollapsed,
    TargetPathInvalidated,
    FloatingTabbarTransition,
    MissingUINodes,
    TabVectorMismatch,
    EventDispatchActive
};

class DockspaceElement final : public UIElement {
public:
    DockspaceElement() = default;

    void BindModel(DockingManager* model) { m_Model = model; MarkDirty(VisualDirty | ChildrenDirty | LayoutDirty); }

    DockingManager* GetModel() const { return m_Model; }

    // The request is consumed by a heavy-pass poll in UIManager::Update; the
    // mark is what makes it visible to the idle/pointer gates — several call
    // sites mutate only DockingManager model objects (no UIElement dirt), and
    // without it an idle UI would never transfer the request.
    void RequestRebuildFromModel()
    {
        m_NeedsRebuild = true;
        MarkDirty(VisualDirty | ChildrenDirty | LayoutDirty);
    }
    bool ConsumeRebuildRequest() { bool v = m_NeedsRebuild; m_NeedsRebuild = false; return v; }

    bool RequestActivationPatch(const std::string& panelId);
    bool RequestTabReorder(const std::string& leafPath);
    bool RequestTabRemoval(const std::string& panelId);
    bool RequestTabAddition(const std::string& leafPath, const std::string& panelId);

    void RebuildFromModel();

    /** Invoked at the end of RebuildFromModel (e.g. editor can re-attach per-tab chrome). */
    void SetOnPostRebuild(std::function<void()> cb) { m_OnPostRebuild = std::move(cb); }

    /** Invoked when user right-clicks a dock tab (panelId, screenX, screenY). */
    void SetOnTabContextMenu(std::function<void(const std::string&, float, float)> cb) { m_OnTabContextMenu = std::move(cb); }
    bool FireTabContextMenu(const std::string& panelId, float x, float y)
    {
        if (!m_OnTabContextMenu)
            return false;
        m_OnTabContextMenu(panelId, x, y);
        return true;
    }
    bool m_NeedsRebuild = false;

    struct DockInstrumentation
    {
        uint32_t fullRebuilds = 0;
        uint32_t activationPatches = 0;
        uint32_t reorderPatches = 0;
        uint32_t tabRemovalPatches = 0;
        uint32_t tabAdditionPatches = 0;
        uint32_t patchFallbacks = 0;
        DockPatchFallbackReason lastFallbackReason = DockPatchFallbackReason::None;
    };
    const DockInstrumentation& GetInstrumentation() const { return m_Instrumentation; }

    void SetDropPreview(const DockDropTarget& target, float containerW, float containerH);
    void ClearDropPreview();
    void ResetLayoutDefault();
    void SyncSplitRatiosFromUI();

    bool ToggleMaximizeLeaf(const std::string& panelId);
    bool IsMaximized() const { return m_SavedRoot != nullptr; }

    void ShowDockDragGhost(const std::string& title, float mouseX, float mouseY, float tabW = 0.f, float tabH = 0.f);
    void UpdateDockDragGhostPosition(float mouseX, float mouseY);
    void SetDockDragGhostUndockMode(bool undocking);
    void HideDockDragGhost();

    // Accent color forwarded to dock drop overlays (ARGB: 0xAARRGGBB)
    void SetAccentColor(uint32_t argb) { m_AccentColor = argb; }

private:
    // Shared leaf-view resolver: finds UI elements for a leaf given a panelId.
    struct LeafView
    {
        DockLeaf* leaf = nullptr;
        UIElement* tabBar = nullptr;
        UIElement* content = nullptr;
        Mount* mount = nullptr;
        std::string leafPath;
    };
    LeafView ResolveLeafByPanelId(const std::string& panelId);
    LeafView ResolveLeafByPath(const std::string& leafPath);

    void ApplyActiveTabVisuals(const LeafView& view, const std::string& activePanelId);

    void BuildFromNode(const DockNode* n, UIElement* parent, const std::string& path);
    void SyncSplitRatiosRecursive(UIElement* el, const std::string& path);
    bool PatchActiveTabInLeaf(const std::string& panelId);

    void Fallback(DockPatchFallbackReason reason);

    DockingManager* m_Model = nullptr;
    DockInstrumentation m_Instrumentation{};
    std::function<void()> m_OnPostRebuild;
    std::function<void(const std::string&, float, float)> m_OnTabContextMenu;

    std::unique_ptr<DockNode> m_SavedRoot;
    uint32_t m_AccentColor = 0xFF3A8FFF;

    UIElement* m_DragGhost = nullptr; // Non-owning; child of root element
    Label* m_DragGhostLabel = nullptr;
    Label* m_DragGhostUndockLabel = nullptr;
    UIElement* m_DragGhostTab = nullptr;
    UIElement* m_DragGhostBody = nullptr;
};

} // namespace GameEngine
