#pragma once

#include <optional>
#include <string>
#include <string_view>
#include "UI/UIElement.h"

namespace GameEngine {

// Generic, reusable dockable panel control. Editor-specific panels (Hierarchy, Inspector, etc.)
// will compose/derive from this, but DockPanel itself is UI-agnostic.
class DockPanel : public UIElement {
public:
    explicit DockPanel(const std::string& title)
      : m_Title(title)
  {
      AddClass("panel");
      AddClass(title);
  }

    const std::string& GetTitle() const { return m_Title; }
    void SetTitle(const std::string& title) { m_Title = title; MarkDirty(VisualDirty); }

    /**
     * CSS icon class this panel type declares for its tab (e.g. "alert-circle-icon").
     * Empty means the type declares none. An instance can still override it either
     * way — see SetTabIcon.
     *
     * Resolved by GetTabIcon(), never from a constructor: a virtual called while
     * DockPanel's constructor runs would dispatch to this base, not the override.
     */
    virtual std::string_view DeclaredTabIconClass() const { return {}; }

    // CSS icon class shown in the tab: the instance override if one was set,
    // otherwise the type's declaration. Empty means no icon.
    std::string_view GetTabIcon() const
    {
        return m_IconOverride ? std::string_view{*m_IconOverride} : DeclaredTabIconClass();
    }

    // Overrides DeclaredTabIconClass() for this instance. An empty class is a
    // real override meaning "no icon", not a request to fall back to the
    // declaration — that is how markup spells explicit suppression.
    void SetTabIcon(const std::string& icon) { m_IconOverride = icon; }

    // Whether user-facing panel listings (Window menu, hamburger panel menu,
    // Universal Search) offer this panel. A delisted panel stays registered and
    // programmatically openable by id — it is only hidden from the listings.
    bool IsListedInPanelMenus() const { return m_ListedInPanelMenus; }
    void SetListedInPanelMenus(bool listed) { m_ListedInPanelMenus = listed; }

    /** If true, the dock content wrapper gets overflow: visible so children (e.g. popups) can draw outside. */
    virtual bool WantsDockContentOverflowVisible() const { return false; }

    // Called when the primary pointer presses an inactive tab, one frame before
    // mouse-up commits activation. Render-backed panels can use this lead time
    // to prepare on-demand data without making hidden panels run continuously.
    virtual void OnDockTabActivationArmed(float /*contentWidth*/, float /*contentHeight*/) {}

private:
    std::string m_Title;
    std::optional<std::string> m_IconOverride;
    bool m_ListedInPanelMenus = true;
};

} // namespace GameEngine
