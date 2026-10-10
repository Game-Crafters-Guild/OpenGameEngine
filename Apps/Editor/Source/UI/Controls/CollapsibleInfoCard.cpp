#include "UI/Controls/CollapsibleInfoCard.h"

#include "UI/Controls/Label.h"
#include "UI/InfoCard.h"
#include "UI/UIEvents.h"

#include <memory>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kStyleAssetPath = "UI/controls/CollapsibleInfoCard/CollapsibleInfoCard.css";

// A laid-out text height above this means the copy wrapped, so the card is
// worth folding. Comfortably above one line of the card's 13px text and below
// two.
constexpr float kSingleRowHeightLimit = 24.0f;

// The collapsed line: the text's first sentence. The line is clipped where the
// right-aligned action begins, so the copy uses whatever width the card has
// rather than a fixed character budget.
std::string MakePreview(const std::string& text)
{
    return text.substr(0, text.find_first_of(".\n"));
}

} // namespace

CollapsibleInfoCard::CollapsibleInfoCard(const std::string& text)
    : m_FullText(text), m_PreviewText(MakePreview(text))
{
    StyleInfoCard(this);
    AddClass("editor-info-card-foldable");
    // The folding chrome's own sheet; the shared card look lives in the theme.
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    auto chevron = std::make_unique<UIElement>();
    chevron->AddClass("editor-info-card-chevron");
    m_Chevron = chevron.get();
    AddChild(std::move(chevron));

    auto line = std::make_unique<UIElement>();
    line->AddClass("editor-info-card-line");

    auto copy = std::make_unique<Label>();
    copy->AddClass("editor-info-card-copy");
    copy->AddClass("editor-info-card-text");
    m_Label = copy.get();
    line->AddChild(std::move(copy));

    auto toggle = std::make_unique<Label>();
    toggle->AddClass("editor-info-card-toggle");
    toggle->AddClass("editor-info-card-text");
    toggle->SetText("show more");
    m_Toggle = toggle.get();
    line->AddChild(std::move(toggle));

    AddChild(std::move(line));

    ApplyState();
}

void CollapsibleInfoCard::OnPostLayout()
{
    if (m_FoldabilityDecided)
        return;

    // Nothing measured yet (first pass, or a card in a collapsed subtree).
    if (GetLayoutWidth() <= 0.0f || m_Label->GetLayoutHeight() <= 0.0f)
        return;

    // Cards read expanded; the chevron is there to fold one away, not to make
    // the reader open it.
    m_FoldabilityDecided = true;
    const bool foldable = m_Label->GetLayoutHeight() > kSingleRowHeightLimit;
    if (foldable != m_Foldable)
        SetFoldable(foldable);
}

void CollapsibleInfoCard::ApplyState()
{
    // State classes live on the elements that style them, so a toggle never
    // depends on an ancestor-driven restyle.
    if (m_Expanded)
        RemoveClass("collapsed");
    else
        AddClass("collapsed");

    if (m_Foldable)
        m_Chevron->RemoveClass("hidden");
    else
        m_Chevron->AddClass("hidden");

    if (m_Expanded)
        m_Chevron->RemoveClass("collapsed");
    else
        m_Chevron->AddClass("collapsed");

    if (m_Expanded)
        m_Label->RemoveClass("clamped");
    else
        m_Label->AddClass("clamped");
    m_Label->SetText(m_Expanded ? m_FullText : m_PreviewText);

    UpdateToggleVisibility();
    MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
}

void CollapsibleInfoCard::SetText(const std::string& text)
{
    if (m_FullText == text)
        return;
    m_FullText = text;
    m_PreviewText = MakePreview(text);

    // Only an expanded card can be measured, so that is the only state in which
    // new copy re-opens the fold decision. The chevron stays until that
    // measurement says otherwise: a card whose copy changes every frame would
    // otherwise lose the chevron's width, and its wrapping, on each change. A
    // collapsed card keeps the chevron it has rather than losing it to a
    // measurement taken through the clamp.
    if (m_Expanded)
        m_FoldabilityDecided = false;
    ApplyState();
}

void CollapsibleInfoCard::SetFoldable(bool foldable)
{
    m_Foldable = foldable;
    ApplyState();
}

void CollapsibleInfoCard::SetExpanded(bool expanded)
{
    if (m_Expanded == expanded)
        return;
    m_Expanded = expanded;
    ApplyState();
}

// Only a collapsed card offers the action: expanded, the chevron is the way
// back and a second control would just repeat it.
void CollapsibleInfoCard::UpdateToggleVisibility()
{
    if (m_Foldable && !m_Expanded)
        m_Toggle->RemoveClass("hidden");
    else
        m_Toggle->AddClass("hidden");
}

void CollapsibleInfoCard::OnEvent(UIEvent& e)
{
    if (m_Foldable && e.Id == kEventMouseUp && e.Button == 0)
    {
        SetExpanded(!m_Expanded);
        e.Stop();
    }
}

} // namespace GameEngine::EditorUI
