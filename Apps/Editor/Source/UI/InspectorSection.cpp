#include "UI/InspectorSection.h"

#include "UI/Controls/EnableDot.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIStyle.h"

#include <vector>

namespace GameEngine {

class CollapseArrowElement : public UIElement {
public:
    CollapseArrowElement()
    {
        Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionRight, StyleLength::Px(8.0f))
            .Set(Style::PositionBottom, StyleLength::Px(-6.0f))
            .Set(Style::Width, StyleLength::Px(15.0f))
            .Set(Style::Height, StyleLength::Px(13.0f));
    }
};


InspectorSection::InspectorSection(std::string title)
    : m_Title(std::move(title))
{
    AddClass("inspector-section");
    SetFocusable(true);
    BuildUI();
    SetLeadingDisclosure();
}

void InspectorSection::SetTitle(const std::string& title)
{
    m_Title = title;
    if (m_TitleLabel)
    {
        m_TitleLabel->SetText(m_Title);
    }
}

void InspectorSection::SetHeaderTooltip(std::string text)
{
    if (m_TitleLabel)
        m_TitleLabel->SetTooltip(std::move(text));
}

void InspectorSection::SetHeaderIconClass(const std::string& cssClass)
{
    if (!m_Icon)
        return;
    if (!m_IconClass.empty())
        m_Icon->RemoveClass(m_IconClass);
    m_IconClass = cssClass;
    if (cssClass.empty())
    {
        m_Icon->AddClass("hidden");
    }
    else
    {
        m_Icon->AddClass(cssClass);
        m_Icon->RemoveClass("hidden");
    }
    m_Icon->MarkDirty(StyleDirty | VisualDirty);
}

void InspectorSection::SetHeaderIconTint(uint32_t argb)
{
    if (!m_Icon)
        return;
    if (argb == 0)
        m_Icon->Overrides().Reset(Style::BackgroundTint);
    else
        m_Icon->Overrides().Set(Style::BackgroundTint, argb);
    m_Icon->MarkDirty(StyleDirty | VisualDirty);
}

void InspectorSection::SetHeaderIconImagePath(const std::string& imagePath)
{
    if (!m_Icon || imagePath.empty())
        return;
    constexpr const char* kEnginePrefix = "engine:";
    constexpr size_t kEnginePrefixLen = 7;
    if (imagePath.rfind(kEnginePrefix, 0) == 0)
        UI::Layout::SetBackgroundResourceName(*m_Icon, imagePath.substr(kEnginePrefixLen));
    else
        UI::Layout::SetBackgroundPath(*m_Icon, imagePath);
    m_Icon->RemoveClass("hidden");
    m_Icon->MarkDirty(StyleDirty | VisualDirty);
}

const std::string& InspectorSection::AssignHeaderIconId(const std::string& id)
{
    if (m_Icon)
        m_Icon->SetId(id);
    return id;
}

void InspectorSection::SetHeaderBadge(const std::string& text, const std::string& tooltip)
{
    if (!m_HeaderRow)
        return;

    if (!m_HeaderBadge)
    {
        auto badge = std::make_unique<Label>();
        badge->AddClass("inspector-section-header-badge");
        m_HeaderBadge = badge.get();

        // Insert just before the options affordance so the badge sits on the
        // right of the title (the title flex-grows and pushes it over).
        const auto& children = m_HeaderRow->GetChildren();
        size_t insertIndex = children.size();
        for (size_t i = 0; i < children.size(); ++i)
        {
            if (children[i].get() == m_Options)
            {
                insertIndex = i;
                break;
            }
        }
        m_HeaderRow->InsertChild(insertIndex, std::move(badge));
    }

    m_HeaderBadge->SetText(text);
    m_HeaderBadge->SetTooltip(tooltip);
    if (text.empty())
    {
        m_HeaderBadge->AddClass("hidden");
        m_HeaderRow->RemoveClass("has-header-badge");
    }
    else
    {
        m_HeaderBadge->RemoveClass("hidden");
        m_HeaderRow->AddClass("has-header-badge");
    }
}

void InspectorSection::SetLeadingDisclosure()
{
    if (m_LeadingDisclosure || !m_HeaderRow) return;
    auto arrow = std::make_unique<UIElement>();
    m_LeadingDisclosure = arrow.get();
    arrow->AddClass("foldout-chevron");
    if (!m_Collapsed) arrow->AddClass("expanded");
    m_HeaderRow->InsertChild(1, std::move(arrow));
    AddClass("inspector-leading-disclosure");
}

void InspectorSection::SetCollapsed(bool collapsed)
{
    const bool previous = m_Collapsed;
    m_Collapsed = collapsed;
    if (m_LeadingDisclosure)
    {
        if (collapsed) m_LeadingDisclosure->RemoveClass("expanded");
        else m_LeadingDisclosure->AddClass("expanded");
    }
    if (!m_ContentRoot)
    {
        return;
    }

    // Check if section has content
    bool hasContent = HasContent();
    
    // Update header chevron state (only visible when collapsed and has content)
    if (m_HeaderChevron)
    {
        if (m_Collapsed && hasContent)
        {
            m_HeaderChevron->RemoveClass("hidden");
            m_HeaderChevron->RemoveClass("arrow-up");
            m_HeaderChevron->AddClass("arrow-down");
        }
        else
        {
            m_HeaderChevron->AddClass("hidden");
        }
    }
    
    // Update collapse arrow visibility (only visible when expanded and has content)
    if (m_Chevron)
    {
        if (m_Collapsed || !hasContent)
        {
            m_Chevron->AddClass("hidden");
        }
        else
        {
            m_Chevron->RemoveClass("hidden");
        }
    }
    
    // CSS handles hiding/showing the body. We still store state so future
    // actions (toggle button) can query it.
    if (m_Collapsed)
    {
        AddClass("inspector-section-collapsed");
        m_ContentRoot->AddClass("inspector-section-body-collapsed");
    }
    else
    {
        RemoveClass("inspector-section-collapsed");
        m_ContentRoot->RemoveClass("inspector-section-body-collapsed");
    }
    // Collapse toggles `inspector-section-body-collapsed`, which maps to `display:none` in the theme.
    // That prunes bodies from the Yoga tree; mark the full subtree so nested controls do not keep
    // stale layout/geometry. The subtree's LayoutDirty plus the structural prune drive the sibling
    // reflow on the next frame's deferred relayout -- no parent poke or RequestRelayout needed.
    MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);

    if (m_OnCollapsedChanged && previous != m_Collapsed)
    {
        m_OnCollapsedChanged(*this, m_Collapsed);
    }
}

void InspectorSection::SetEnabled(bool enabled)
{
    if (m_Dot)
        m_Dot->SetValueWithoutNotify(enabled);
    ApplyEnabledPresentation(enabled);
}

// The section's side of the component's state: an off section's title and body read as off.
void InspectorSection::ApplyEnabledPresentation(bool enabled)
{
    m_Enabled = enabled;
    if (m_Enabled)
        RemoveClass("disabled");
    else
        AddClass("disabled");
    SetBodyReadsOff(!m_Enabled);
    MarkDirty(LayoutDirty | VisualDirty);
}

void InspectorSection::SetEnabledMixed()
{
    // Neither on nor off: the title and body keep their on look and the dot turns into a dash.
    if (m_Dot)
        m_Dot->SetMixed();
    RemoveClass("disabled");
    SetBodyReadsOff(false);
    MarkDirty(LayoutDirty | VisualDirty);
}

void InspectorSection::SetBodyStateFollower(InspectorSection* follower)
{
    m_BodyStateFollower = follower;
    SetBodyReadsOff(m_BodyReadsOff);
}

void InspectorSection::SetBodyReadsOff(bool off)
{
    m_BodyReadsOff = off;
    for (InspectorSection* section : {this, m_BodyStateFollower})
    {
        if (!section || !section->m_ContentRoot)
            continue;
        if (off)
            section->m_ContentRoot->AddClass("inspector-section-body-disabled");
        else
            section->m_ContentRoot->RemoveClass("inspector-section-body-disabled");
        section->MarkDirty(LayoutDirty | VisualDirty);
    }
}

void InspectorSection::SetInactive(bool inactive)
{
    m_Inactive = inactive;
    RefreshEnabledDotPresentation();
}

void InspectorSection::SetEnabledToggleTooltip(std::string text)
{
    if (m_Dot)
        m_Dot->SetTooltip(std::move(text));
}

void InspectorSection::SetShowEnabledToggle(bool show)
{
    m_ShowEnabledToggle = show;
    if (!show)
        m_EnabledTogglePlaceholder = false;
    RefreshEnabledDotPresentation();
}

void InspectorSection::SetEnabledTogglePlaceholder(bool placeholder)
{
    m_EnabledTogglePlaceholder = placeholder;
    RefreshEnabledDotPresentation();
}

void InspectorSection::RefreshEnabledDotPresentation()
{
    if (!m_Dot)
        return;

    // A placeholder keeps the dot's footprint and nothing else: disabled, it takes no pointer and
    // no focus, and its class makes it invisible.
    const bool shown = m_EnabledTogglePlaceholder || m_ShowEnabledToggle;
    const bool live = shown && !m_EnabledTogglePlaceholder;
    if (shown)
        m_Dot->RemoveClass("hidden");
    else
        m_Dot->AddClass("hidden");
    if (m_EnabledTogglePlaceholder)
        m_Dot->AddClass("inspector-section-dot-placeholder");
    else
        m_Dot->RemoveClass("inspector-section-dot-placeholder");
    m_Dot->SetEnabled(live);
    m_Dot->SetFocusable(live);
    m_Dot->SetInactive(live && m_Inactive);

    MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
}

void InspectorSection::ClearContent()
{
    if (!m_ContentRoot)
        return;
    // Snapshot-then-remove (see UIElement::RemoveAllChildren), skipping the
    // section's own collapse arrow so m_Chevron stays valid.
    std::vector<UIElement*> snapshot;
    snapshot.reserve(m_ContentRoot->GetChildren().size());
    for (const auto& child : m_ContentRoot->GetChildren())
    {
        if (child && child.get() != m_Chevron)
            snapshot.push_back(child.get());
    }
    for (UIElement* child : snapshot)
        m_ContentRoot->RemoveChild(child);
}

void InspectorSection::EnsureHasPlaceholderIfEmpty(const std::string& text)
{
    if (!m_ContentRoot)
    {
        return;
    }
    // The collapse arrow is a body child (section chrome, not content); only
    // inspector-built rows count toward "has content".
    for (const auto& child : m_ContentRoot->GetChildren())
    {
        if (child && child.get() != m_Chevron)
            return;
    }

    auto l = std::make_unique<Label>();
    l->AddClass("inspector-section-empty");
    l->SetText(text.empty() ? "(No inspector)" : text);
    m_ContentRoot->AddChild(std::move(l));
}

void InspectorSection::BuildUI()
{
    // Header row - clickable to toggle collapse
    {
        auto headerRow = std::make_unique<UIElement>();
        headerRow->AddClass("inspector-section-header-row");
        m_HeaderRow = headerRow.get();

        // The component's enable dot (left side). Its tooltip opens above it, so it never covers
        // the first rows of the section it switches.
        auto dot = std::make_unique<EnableDot>();
        m_Dot = dot.get();
        dot->AddClass("inspector-section-dot");
        dot->SetTooltipPlacement(UIElement::TooltipPlacement::Above);
        dot->SetValueWithoutNotify(m_Enabled);
        dot->SetOnValueChanged([this](const bool& enabled)
                               {
            ApplyEnabledPresentation(enabled);
            if (m_OnEnabledChanged)
                m_OnEnabledChanged(enabled); });
        // The row brightens its title on hover as the collapse click's feedback; with the pointer on
        // a live dot a click switches instead, so the row takes a class that holds the title at rest.
        dot->RegisterEventHandler(kEventMouseEnter, [this](UIEvent&)
                                  {
            if (m_ShowEnabledToggle && !m_EnabledTogglePlaceholder)
                m_HeaderRow->AddClass("inspector-section-header-row-dot-hovered"); });
        dot->RegisterEventHandler(kEventMouseLeave, [this](UIEvent&)
                                  { m_HeaderRow->RemoveClass("inspector-section-header-row-dot-hovered"); });
        headerRow->AddChild(std::move(dot));

        // Component-type icon (between dot and title). Hidden by default until
        // a caller sets an icon class via SetHeaderIconClass().
        auto icon = std::make_unique<UIElement>();
        m_Icon = icon.get();
        icon->AddClass("inspector-section-icon");
        icon->AddClass("hidden");
        headerRow->AddChild(std::move(icon));

        // Title label
        auto title = std::make_unique<Label>();
        m_TitleLabel = title.get();
        title->AddClass("inspector-section-header");
        title->SetText(m_Title);
        headerRow->AddChild(std::move(title));

        // Header options icon (right side) used to open the component options popup.
        auto options = std::make_unique<UIElement>();
        m_Options = options.get();
        options->AddClass("button");
        options->AddClass("icon-button");
        options->AddClass("inspector-section-header-options");
        headerRow->AddChild(std::move(options));

        // Chevron arrow in header (right side) - only shown when collapsed
        auto headerChevron = std::make_unique<UIElement>();
        m_HeaderChevron = headerChevron.get();
        headerChevron->AddClass("inspector-section-header-chevron");
        if (m_Collapsed)
        {
            headerChevron->AddClass("arrow-down");
        }
        else
        {
            headerChevron->AddClass("hidden");
        }
        headerRow->AddChild(std::move(headerChevron));

        AddChild(std::move(headerRow));
    }

    // Content body root
    {
        auto body = std::make_unique<UIElement>();
        body->AddClass("inspector-section-body");
        m_ContentRoot = body.get();
        if (m_Collapsed)
        {
            body->AddClass("inspector-section-body-collapsed");
        }

        // Collapse arrow (bottom right) - only shown when expanded
        auto collapseArrow = std::make_unique<CollapseArrowElement>();
        m_Chevron = collapseArrow.get();
        collapseArrow->AddClass("inspector-section-collapse-arrow");
        collapseArrow->AddClass("arrow-up");
        if (m_Collapsed)
        {
            collapseArrow->AddClass("hidden");
        }
        body->AddChild(std::move(collapseArrow));

        AddChild(std::move(body));
    }
    RefreshEnabledDotPresentation();
}

bool InspectorSection::HasContent() const
{
    if (!m_ContentRoot)
        return false;
    
    // The collapse arrow is section chrome; anything else — including the
    // empty-state placeholder — is showable content, so an empty section stays
    // expandable to reveal its "(No properties)" message.
    for (const auto& child : m_ContentRoot->GetChildren())
    {
        if (child && child.get() != m_Chevron)
        {
            return true;
        }
    }

    return false;
}

void InspectorSection::OnEvent(UIEvent& e)
{
    // Check if section has content - if not, don't allow expanding
    bool hasContent = HasContent();
    
    bool onOptions = m_Options && IsPointInOptions(e.X, e.Y);
    // Check if click is on the collapse arrow (when expanded)
    bool onCollapseArrow = m_Chevron && !m_Chevron->HasClass("hidden") && IsPointInCollapseArrow(e.X, e.Y);

    if (m_Options)
    {
        if (e.Id == kEventMouseMove || e.Id == kEventMouseDown || e.Id == kEventMouseUp)
        {
            if (onOptions && !m_Options->HasClass("hover"))
                m_Options->AddClass("hover");
            else if (!onOptions && m_Options->HasClass("hover"))
                m_Options->RemoveClass("hover");
        }
        else if (e.Id == kEventMouseLeave && m_Options->HasClass("hover"))
        {
            m_Options->RemoveClass("hover");
        }
    }
    
    // Right-click header context menu (on mouse-up so the macOS NSMenu modal
    // loop sees a clean release-state and pops on the first click).
    if (e.Id == kEventMouseUp && e.Button == 1 && m_OnHeaderContextMenu)
    {
        if (IsPointInHeader(e.X, e.Y))
        {
            m_OnHeaderContextMenu(e.X, e.Y);
            e.Stop();
            return;
        }
    }

    // Press / drag logic only applies to left-click. Right-click (button 1) is
    // handled by the kEventMouseUp branch above for the context menu and must
    // not flip "pressed" CSS state on the header or capture the mouse.
    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        if (onOptions)
        {
            m_OptionsArmed = true;
            if (m_Options)
                m_Options->AddClass("pressed");
            e.Capture(this);
            e.Stop();
            return;
        }
        else if (onCollapseArrow)
        {
            m_Armed = true;
            if (m_Chevron)
                m_Chevron->AddClass("pressed");
            e.Capture(this);
            e.Stop();
            return;
        }
        else if (IsPointInHeader(e.X, e.Y))
        {
            if (m_OnHeaderMouseDown && m_OnHeaderMouseDown(e.Mods))
            {
                e.Stop();
                return;
            }
            // Only allow expanding if section has content
            if (!hasContent && m_Collapsed)
            {
                // Don't allow expanding empty sections
                return;
            }
            m_Armed = true;
            m_DragArmedX = e.X;
            m_DragArmedY = e.Y;
            m_DragActive = false;
            if (m_HeaderRow)
                m_HeaderRow->AddClass("pressed");
            e.Capture(this);
            e.Stop();
            return;
        }
    }

    if (e.Id == kEventMouseMove)
    {
        if (m_OptionsArmed)
        {
            const bool stillOnOptions = m_Options && IsPointInOptions(e.X, e.Y);
            if (m_Options)
            {
                if (stillOnOptions && !m_Options->HasClass("pressed"))
                    m_Options->AddClass("pressed");
                else if (!stillOnOptions && m_Options->HasClass("pressed"))
                    m_Options->RemoveClass("pressed");
            }
            e.Stop();
        }
        else if (m_Armed)
        {
            bool onArrow = m_Chevron && !m_Chevron->HasClass("hidden") && IsPointInCollapseArrow(e.X, e.Y);
            bool inHeader = IsPointInHeader(e.X, e.Y);

            // Detect drag threshold (5 px).
            const float ddx = e.X - m_DragArmedX;
            const float ddy = e.Y - m_DragArmedY;
            if (!m_DragActive && (ddx * ddx + ddy * ddy) > 25.0f)
            {
                m_DragActive = true;
                if (m_HeaderRow) m_HeaderRow->RemoveClass("pressed");
                if (m_OnHeaderDragStarted) m_OnHeaderDragStarted(e.X, e.Y);
            }
            else if (m_DragActive)
            {
                if (m_OnHeaderDragMoved) m_OnHeaderDragMoved(e.X, e.Y);
                e.Stop();
                return;
            }

            if (!m_DragActive)
            {
                if (m_Chevron && onArrow)
                {
                    if (!m_Chevron->HasClass("pressed"))
                        m_Chevron->AddClass("pressed");
                }
                else if (m_Chevron && m_Chevron->HasClass("pressed"))
                {
                    m_Chevron->RemoveClass("pressed");
                }

                if (m_HeaderRow && inHeader && !onArrow)
                {
                    if (!m_HeaderRow->HasClass("pressed"))
                        m_HeaderRow->AddClass("pressed");
                }
                else if (m_HeaderRow && m_HeaderRow->HasClass("pressed") && !inHeader && !onArrow)
                {
                    m_HeaderRow->RemoveClass("pressed");
                }
            }
            e.Stop();
        }
        return;
    }

    if (e.Id == kEventMouseUp)
    {
        if (m_OptionsArmed)
        {
            if (m_Options)
                m_Options->RemoveClass("pressed");
            if (onOptions && m_OnHeaderContextMenu)
                m_OnHeaderContextMenu(e.X, e.Y);
            e.Stop();
            m_OptionsArmed = false;
            return;
        }
        else if (m_Armed)
        {
            if (m_DragActive)
            {
                m_DragActive = false;
                if (m_OnHeaderDragEnded) m_OnHeaderDragEnded();
                if (m_HeaderRow) m_HeaderRow->RemoveClass("pressed");
                m_Armed = false;
                e.Stop();
                return;
            }

            // Check if section has content - if not, don't allow expanding
            hasContent = HasContent();

            if (m_Chevron)
                m_Chevron->RemoveClass("pressed");
            if (m_HeaderRow)
                m_HeaderRow->RemoveClass("pressed");

            if (onCollapseArrow)
            {
                SetCollapsed(true);
            }
            else if (IsPointInHeader(e.X, e.Y) && !onOptions)
            {
                if (m_OnHeaderClick && m_OnHeaderClick(e.Mods))
                {
                    e.Stop();
                    m_Armed = false;
                    return;
                }
                // Only allow expanding if section has content
                if (!hasContent && m_Collapsed)
                {
                    // Don't allow expanding empty sections
                    e.Stop();
                    m_Armed = false;
                    return;
                }
                SetCollapsed(!m_Collapsed);
            }
            e.Stop();
            m_Armed = false;
            return;
        }
    }

    UIElement::OnEvent(e);
}

bool InspectorSection::IsPointInHeader(float x, float y) const
{
    if (!m_HeaderRow)
        return false;
    float hx = m_HeaderRow->GetLayoutX();
    float hy = m_HeaderRow->GetLayoutY();
    float hw = m_HeaderRow->GetLayoutWidth();
    float hh = m_HeaderRow->GetLayoutHeight();
    return (x >= hx && y >= hy && x < (hx + hw) && y < (hy + hh));
}

bool InspectorSection::IsPointInOptions(float x, float y) const
{
    if (!m_Options)
        return false;
    float ox = m_Options->GetLayoutX();
    float oy = m_Options->GetLayoutY();
    float ow = m_Options->GetLayoutWidth();
    float oh = m_Options->GetLayoutHeight();
    return (x >= ox && y >= oy && x < (ox + ow) && y < (oy + oh));
}

bool InspectorSection::IsPointInCollapseArrow(float x, float y) const
{
    if (!m_Chevron || m_Chevron->HasClass("hidden"))
        return false;
    float ax = m_Chevron->GetLayoutX();
    float ay = m_Chevron->GetLayoutY();
    float aw = m_Chevron->GetLayoutWidth();
    float ah = m_Chevron->GetLayoutHeight();
    return (x >= ax && y >= ay && x < (ax + aw) && y < (ay + ah));
}


} // namespace GameEngine
