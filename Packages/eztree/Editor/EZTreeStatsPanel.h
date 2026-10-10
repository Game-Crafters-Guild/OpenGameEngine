#pragma once

#include "UI/AssetBoundDockPanel.h"
#include "UI/UIManagerRef.h"

#include <cstdint>
#include <string>

namespace GameEngine
{
class Label;
class UIElement;
class UIManager;
}

namespace GameEngine::EZTreeEditor
{

// Tree Generator diagnostics panel, shipped by the eztree package's Editor
// module (EditorSDK phase 2 customer). Layout and stylesheet load from the
// package's own asset mount; content is live scene data: one row per EZTree
// entity with its seed/type and generated-mesh footprint, plus scene totals.
class EZTreeStatsPanel final : public Editor::AssetBoundDockPanel
{
public:
    EZTreeStatsPanel();
    ~EZTreeStatsPanel() override;

    void OnPostLayout() override;

protected:
    void OnLayoutBound() override;
    void OnLayoutReconciled() override;

private:
    void ResolveElements();
    void RefreshStats();
    // Refresh only when the tab is actually visible (skips the scene walk for
    // an inactive/zero-size tab). Driven by the manager's periodic refresh
    // tick so the panel stays current even while the editor is input-idle.
    void RefreshStatsIfVisible();

    // Resolved on bind and on every reconcile; null once a reconcile destroyed them.
    WeakRef<Label> m_SummaryLabel;
    WeakRef<UIElement> m_RowsContainer;
    std::string m_LastSnapshot; // rebuild rows only when the data changed
    // Periodic-refresh registration on the owning manager, captured at register
    // time so the destructor can unregister even after a detach; the reference
    // reads null once that manager is gone.
    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
};

} // namespace GameEngine::EZTreeEditor
