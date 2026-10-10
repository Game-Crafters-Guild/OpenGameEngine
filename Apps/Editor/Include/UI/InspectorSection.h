#pragma once

#include <string>
#include <functional>

#include "UI/UIElement.h"
#include "UI/UIEvents.h"

namespace GameEngine {

class EnableDot;
class Label;

// Reusable container for a single inspector "section" (e.g. a component).
// Owns a header (title) and a content root which inspectors can populate.
// Designed to be extended later with collapse / enable / remove actions.
class InspectorSection final : public UIElement
{
public:
    explicit InspectorSection(std::string title);
    ~InspectorSection() override = default;

    void SetTitle(const std::string& title);
    const std::string& GetTitle() const { return m_Title; }

    // Tooltip applied to the header row only (not the body content).
    void SetHeaderTooltip(std::string text);

    // Apply a CSS class controlling the header icon's background-image.
    // Passing an empty string clears any icon and collapses the icon slot.
    void SetHeaderIconClass(const std::string& cssClass);

    // Override the icon tint color (0xAARRGGBB). Pass 0 to reset to CSS default.
    void SetHeaderIconTint(uint32_t argb);

    // Set a direct image path (or "engine:<resourceName>") as the icon background.
    // Overrides the CSS class background-image with an inline style.
    void SetHeaderIconImagePath(const std::string& imagePath);

    // Assign a DOM id to the header icon element so async callbacks can locate it
    // via FindById. Returns the id passed in.
    const std::string& AssignHeaderIconId(const std::string& id);

    // Show a small right-aligned status badge in the header (e.g. "Not Loaded"),
    // inserted just before the options affordance. Empty text hides it.
    // An optional tooltip is applied to the badge itself.
    void SetHeaderBadge(const std::string& text, const std::string& tooltip = "");

    UIElement* GetContentRoot() const { return m_ContentRoot; }

    // Remove every inspector-built row from the body while keeping the section's
    // own chrome alive — the bottom collapse arrow is a body child and m_Chevron
    // must stay valid. Used by single-section rebuilds; a bare RemoveAllChildren
    // on the content root would leave the section pointing at freed children.
    void ClearContent();

    // Use a persistent leading disclosure arrow for nested component headers.
    void SetLeadingDisclosure();
    void SetCollapsed(bool collapsed);
    bool IsCollapsed() const { return m_Collapsed; }

    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_Enabled; }

    // Shows the dot's indeterminate state: the entities of a multi-selection disagree on the
    // component's state. The next SetEnabled clears it; a click on the dot switches to the
    // opposite of the state SetEnabled last set, so one click switches the whole selection to one
    // state, as a mixed Toggle does.
    void SetEnabledMixed();

    // The entity the component belongs to is off, itself or through an ancestor: the dot shows the
    // component's own state muted.
    void SetInactive(bool inactive);

    // A section that is part of this one's component (the Mesh Renderer's material slots): its body
    // reads as off whenever this section's does. Both sections are rebuilt together.
    void SetBodyStateFollower(InspectorSection* follower);

    // When false, the dot toggle is hidden and the section always appears enabled.
    void SetShowEnabledToggle(bool show);

    // Reserves the same horizontal space as the enabled dot (invisible, no interaction)
    // so headers align with sections that show the real dot.
    void SetEnabledTogglePlaceholder(bool placeholder);

    // Tooltip applied to the enabled dot only.
    void SetEnabledToggleTooltip(std::string text);

    // Called when the user clicks the dot to toggle enabled state.
    using EnabledChangedHandler = std::function<void(bool /*enabled*/)>;
    void SetOnEnabledChanged(EnabledChangedHandler cb) { m_OnEnabledChanged = std::move(cb); }

    // Ensure the user sees something even when no inspector populates content.
    void EnsureHasPlaceholderIfEmpty(const std::string& text = "(No inspector)");

    // Optional: callback when collapsed state changes.
    using CollapsedChangedHandler = std::function<void(InspectorSection&, bool /*collapsed*/)>;
    void SetOnCollapsedChanged(CollapsedChangedHandler cb) { m_OnCollapsedChanged = std::move(cb); }

    // Optional: right-click context menu on the header (component title row).
    // Coordinates are in UI-space (same as UIEvent.x/y).
    using HeaderContextMenuCallback = std::function<void(float /*x*/, float /*y*/)>;
    void SetOnHeaderContextMenu(HeaderContextMenuCallback cb) { m_OnHeaderContextMenu = std::move(cb); }

    // Optional: left-click callback on header mouse-up. Return true to consume
    // the click and prevent default collapse/expand toggling.
    using HeaderClickCallback = std::function<bool(int /*mods*/)>;
    void SetOnHeaderClick(HeaderClickCallback cb) { m_OnHeaderClick = std::move(cb); }

    // Optional: header left mouse-down (before drag arm / collapse arm). Return true to consume
    // the event (skip collapse-toggle + drag gesture for this press).
    using HeaderMouseDownCallback = std::function<bool(int /*mods*/)>;
    void SetOnHeaderMouseDown(HeaderMouseDownCallback cb) { m_OnHeaderMouseDown = std::move(cb); }

    // Section drag-reorder callbacks. Fired when the user drags the section header.
    using HeaderDragCallback = std::function<void(float /*x*/, float /*y*/)>;
    using HeaderDragEndCallback = std::function<void()>;
    void SetOnHeaderDragStarted(HeaderDragCallback cb) { m_OnHeaderDragStarted = std::move(cb); }
    void SetOnHeaderDragMoved(HeaderDragCallback cb) { m_OnHeaderDragMoved = std::move(cb); }
    void SetOnHeaderDragEnded(HeaderDragEndCallback cb) { m_OnHeaderDragEnded = std::move(cb); }

    void OnEvent(UIEvent& e) override;

private:
    bool HasContent() const;

private:
    void BuildUI();
    void ApplyEnabledPresentation(bool enabled);
    void SetBodyReadsOff(bool off);
    void RefreshEnabledDotPresentation();
    bool IsPointInHeader(float x, float y) const;
    bool IsPointInOptions(float x, float y) const;
    bool IsPointInCollapseArrow(float x, float y) const;

    std::string m_Title;
    Label* m_TitleLabel = nullptr;
    Label* m_HeaderBadge = nullptr;
    UIElement* m_ContentRoot = nullptr;
    UIElement* m_HeaderRow = nullptr;
    UIElement* m_HeaderChevron = nullptr;
    UIElement* m_LeadingDisclosure = nullptr;
    UIElement* m_Chevron = nullptr;
    EnableDot* m_Dot = nullptr;
    InspectorSection* m_BodyStateFollower = nullptr; // not owned
    UIElement* m_Icon = nullptr;
    UIElement* m_Options = nullptr;
    std::string m_IconClass;
    bool m_ShowEnabledToggle = true;
    bool m_EnabledTogglePlaceholder = false;
    bool m_Collapsed = false;
    bool m_Enabled = true;
    bool m_BodyReadsOff = false;
    bool m_Inactive = false;
    bool m_Armed = false;
    bool m_OptionsArmed = false;

    HeaderContextMenuCallback m_OnHeaderContextMenu;
    HeaderClickCallback m_OnHeaderClick;
    HeaderMouseDownCallback m_OnHeaderMouseDown;
    CollapsedChangedHandler m_OnCollapsedChanged;
    EnabledChangedHandler m_OnEnabledChanged;
    float m_DragArmedX = 0.0f;
    float m_DragArmedY = 0.0f;
    bool m_DragActive = false;
    HeaderDragCallback m_OnHeaderDragStarted;
    HeaderDragCallback m_OnHeaderDragMoved;
    HeaderDragEndCallback m_OnHeaderDragEnded;
};

} // namespace GameEngine


