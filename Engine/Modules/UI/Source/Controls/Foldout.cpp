#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

namespace GameEngine
{

Foldout::Foldout()
{
    AddClass("foldout");
    // Asset-driven control styling: the UIManager will attach this to our subtree
    // when the element becomes owned by a manager.
    RequestSubtreeStyleAssetPath("UI/controls/Foldout.css", "editor");
    SetFocusable(true);
    EnsureStructure();
    UpdateExpandedState();
}

void Foldout::EnsureStructure()
{
    if (m_Header)
        return;

    // Header container (clickable area)
    auto header = std::make_unique<UIElement>();
    m_Header = header.get();
    m_Header->AddClass("foldout-header");

    // Chevron indicator
    auto chevron = std::make_unique<UIElement>();
    m_Chevron = chevron.get();
    m_Chevron->AddClass("foldout-chevron");
    m_Header->AddChild(std::move(chevron));

    // Icon placeholder (hidden by default until SetIconClass is called)
    auto icon = std::make_unique<UIElement>();
    m_Icon = icon.get();
    m_Icon->AddClass("foldout-icon");
    m_Icon->AddClass("hidden");
    m_Header->AddChild(std::move(icon));

    // Title label
    auto title = std::make_unique<Label>();
    m_TitleLabel = title.get();
    m_TitleLabel->AddClass("foldout-title");
    m_Header->AddChild(std::move(title));

    AddChild(std::move(header));

    // Content container for user-provided children
    auto content = std::make_unique<UIElement>();
    m_ContentContainer = content.get();
    m_ContentContainer->AddClass("foldout-content");
    AddChild(std::move(content));
}

void Foldout::SetTitle(const std::string& title)
{
    m_Title = title;
    EnsureStructure();
    if (m_TitleLabel)
        m_TitleLabel->SetText(title);
    MarkDirty(LayoutDirty | VisualDirty);
}

void Foldout::SetIconClass(const std::string& iconClass)
{
    if (!m_IconClass.empty() && m_Icon)
        m_Icon->RemoveClass(m_IconClass);

    m_IconClass = iconClass;
    EnsureStructure();

    if (m_Icon)
    {
        if (iconClass.empty())
        {
            m_Icon->AddClass("hidden");
        }
        else
        {
            m_Icon->RemoveClass("hidden");
            m_Icon->AddClass(iconClass);
        }
    }
    MarkDirty(LayoutDirty | VisualDirty);
}

void Foldout::SetExpanded(bool expanded)
{
    if (m_Expanded == expanded)
        return;

    m_Expanded = expanded;

    if (m_Expanded && m_LazyContentBuilder && !m_LazyContentBuilt)
    {
        m_LazyContentBuilt = true;
        EnsureStructure();
        // Move-out before invoking so any captured state (e.g. asset GUIDs,
        // closures over heavy ctx pointers) is released as soon as the
        // builder returns rather than living for the Foldout's lifetime.
        auto builder = std::move(m_LazyContentBuilder);
        m_LazyContentBuilder = nullptr;
        builder(m_ContentContainer);
    }

    UpdateExpandedState();

    if (m_OnExpandedChanged)
        m_OnExpandedChanged(*this, m_Expanded);
}

void Foldout::Toggle()
{
    SetExpanded(!m_Expanded);
}

void Foldout::UpdateExpandedState()
{
    EnsureStructure();

    if (m_Expanded)
    {
        RemoveClass("collapsed");
        AddClass("expanded");
        if (m_ContentContainer)
            m_ContentContainer->RemoveClass("hidden");
        if (m_Chevron)
        {
            m_Chevron->RemoveClass("collapsed");
            m_Chevron->AddClass("expanded");
        }
    }
    else
    {
        RemoveClass("expanded");
        AddClass("collapsed");
        if (m_ContentContainer)
            m_ContentContainer->AddClass("hidden");
        if (m_Chevron)
        {
            m_Chevron->RemoveClass("expanded");
            m_Chevron->AddClass("collapsed");
        }
    }

    // IMPORTANT:
    // Foldout collapse/expand is implemented by toggling the `hidden` class on the content container,
    // which maps to `display:none` in the Editor theme (see `theme/core.css`). `display:none` affects
    // layout tree structure (Yoga pruning) and must reliably trigger a relayout even when the only
    // change is a class toggle.
    //
    // We therefore mark the entire subtree as needing style+layout so nested foldouts and descendants
    // are correctly excluded/included and their cached geometry/layout doesn't linger. The subtree's
    // LayoutDirty plus the display:none structural prune drive the parent reflow on the next frame's
    // deferred relayout, so no explicit parent mark is needed.
    MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
}

bool Foldout::IsPointInHeader(float x, float y) const
{
    if (!m_Header)
        return false;
    float hx = m_Header->GetLayoutX();
    float hy = m_Header->GetLayoutY();
    float hw = m_Header->GetLayoutWidth();
    float hh = m_Header->GetLayoutHeight();
    return (x >= hx && y >= hy && x < (hx + hw) && y < (hy + hh));
}

void Foldout::AddChild(std::unique_ptr<UIElement> child)
{
    EnsureStructure();

    // Redirect user-provided children to the content container
    // Don't redirect internal structure elements (header and content container itself)
    if (child.get() != m_Header && child.get() != m_ContentContainer && m_ContentContainer)
    {
        m_ContentContainer->AddChild(std::move(child));
    }
    else
    {
        // For internal structure, use the base class AddChild
        UIElement::AddChild(std::move(child));
    }
}

void Foldout::RemoveChild(UIElement* child)
{
    // Check if the child is in the content container first
    if (m_ContentContainer)
    {
        const auto& contentChildren = m_ContentContainer->GetChildren();
        for (const auto& c : contentChildren)
        {
            if (c.get() == child)
            {
                m_ContentContainer->RemoveChild(child);
                return;
            }
        }
    }
    // Otherwise, try the base class (for internal structure)
    UIElement::RemoveChild(child);
}

std::unique_ptr<UIElement> Foldout::TakeChild(UIElement* child)
{
    // Check if the child is in the content container first
    if (m_ContentContainer)
    {
        const auto& contentChildren = m_ContentContainer->GetChildren();
        for (const auto& c : contentChildren)
        {
            if (c.get() == child)
                return m_ContentContainer->TakeChild(child);
        }
    }
    // Otherwise, try the base class (for internal structure)
    return UIElement::TakeChild(child);
}

void Foldout::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown)
    {
        if (e.Button != 0)
            return; // Only left-click toggles the foldout.
        if (IsPointInHeader(e.X, e.Y))
        {
            m_Armed = true;
            if (m_Header)
                m_Header->AddClass("pressed");
            e.Capture(this);
            e.Stop();
        }
        return;
    }

    if (e.Id == kEventMouseCancel)
    {
        // Press abandoned (pointer left the surface): disarm without toggling.
        if (m_Header)
            m_Header->RemoveClass("pressed");
        m_Armed = false;
        return;
    }

    if (e.Id == kEventMouseUp)
    {
        if (m_Armed)
        {
            if (m_Header)
                m_Header->RemoveClass("pressed");
            if (IsPointInHeader(e.X, e.Y))
                Toggle();
            e.Stop();
        }
        m_Armed = false;
        return;
    }
}

} // namespace GameEngine
