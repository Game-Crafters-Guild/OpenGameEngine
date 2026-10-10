#pragma once

#include "UI/Controls/DockPanel.h"

#include <filesystem>
#include <memory>
#include <string>

namespace GameEngine
{
struct AssetLoadHandle;

namespace Editor
{

// A DockPanel whose content subtree binds a UXML layout (and optionally a CSS
// stylesheet) resolved from a named asset source. This is the mount-aware
// generalization of the editor's deferred panel-bind idiom (WebPanel,
// SceneViewPanel): binding waits for the first laid-out frame so an owner
// UIManager exists, then loads and applies the assets once. Package panels
// pass their package's mount alias so the UI ships with the package.
class AssetBoundDockPanel : public DockPanel
{
public:
    ~AssetBoundDockPanel() override;

    void OnPostLayout() override;

protected:
    AssetBoundDockPanel(const std::string& title, std::string assetSourceAlias,
                        std::filesystem::path layoutAssetPath,
                        std::filesystem::path styleAssetPath);

    // Called once, after the layout bound into this subtree (and any
    // stylesheet attached). Wire element pointers and event handlers here.
    virtual void OnLayoutBound() {}

    // Called after every later reconcile of the bound layout into this subtree
    // (kEventLayoutReconciled): a .uxml hot reload, or another panel binding the same
    // layout. Elements whose id or tag the new layout changed are destroyed and new
    // ones are created unwired, so re-resolve elements here (held as WeakRef) and wire
    // handlers only on element instances not wired before.
    virtual void OnLayoutReconciled() {}

    bool IsLayoutBound() const { return m_BindApplied; }

private:
    void BindFromAssets();
    void AttachStyle();
    void FinishBind();
    void MarkBindFailed(const char* what, const std::filesystem::path& path);

    std::string m_AssetSourceAlias;
    std::filesystem::path m_LayoutAssetPath;
    std::filesystem::path m_StyleAssetPath;

    // m_BindPending covers the whole attempt — the queued action AND the chained async
    // loads it starts — so a bind in flight is not re-armed by every layout pass. It is
    // cleared only by an outcome: applied, failed, or "panel detached, try again".
    bool m_BindApplied = false;
    bool m_BindPending = false;
    bool m_BindFailed = false;

    // The one load in flight. Stored so its callback is cancelled if the panel is
    // destroyed; the stylesheet step replaces it after the layout load has completed.
    std::unique_ptr<AssetLoadHandle> m_LoadHandle;
};

} // namespace Editor
} // namespace GameEngine
