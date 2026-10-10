#pragma once

#include "UI/Controls/DockPanel.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace GameEngine
{

struct AssetLoadHandle;
class Checkbox;
class Toggle;
class Dropdown;

// Simple debug/demo panel that showcases basic UI controls (Checkbox, Toggle,
// Dropdown) inside the Editor. This is wired to F3 in EditorApplication to
// help exercise and visually validate the control library.
class UIDemoPanel : public DockPanel
{
  public:
    std::string_view DeclaredTabIconClass() const override { return "dock-ui-demo-icon"; }

    UIDemoPanel();
    ~UIDemoPanel() override;

    // Native dropdown instance used by the Editor to hook OS menus when
    // available. May be null if construction failed.
    Dropdown* GetNativeDropdown() const { return m_NativeDropdown; }

    void OnPostLayout() override;

  private:
    void BindFromAssetsDeferred();
    void MarkBindFailed(std::string_view reason);
    void RefreshControlPointers();
    // Loads and attaches stylesheet `index`, then chains to the next. Sequential because
    // attach order is the cascade order at equal specificity, which parallel loads would
    // leave to completion order.
    void AttachStylesheet(std::size_t index);

    Checkbox* m_Checkbox       = nullptr;
    Toggle*   m_Toggle         = nullptr;
    Dropdown* m_UiDropdown     = nullptr;
    Dropdown* m_NativeDropdown = nullptr;

    // Deferred layout bind. m_BindPending covers the whole attempt — the queued action AND
    // the async load it starts — so a bind in flight is not re-armed by every layout pass
    // while the .xml resolves. It is cleared only by an outcome: applied, failed, or
    // "panel detached, try again". m_BindFailed is terminal: retrying an unresolvable
    // editor-mount asset per frame never resolves it.
    bool m_BindApplied = false;
    bool m_BindPending = false;
    bool m_BindFailed = false;

    // The one load in flight. Stored so its callback is cancelled if the panel is
    // destroyed; the stylesheet chain replaces it per step, each after the previous
    // step's load has completed.
    std::unique_ptr<AssetLoadHandle> m_LoadHandle;
};

} // namespace GameEngine
