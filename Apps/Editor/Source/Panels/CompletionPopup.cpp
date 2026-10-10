#include "Panels/CompletionPopup.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIEvents.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"

#include <algorithm>

namespace GameEngine {

static constexpr int   kMaxVisibleRows = 8;
static constexpr float kCharWidthFactor = 0.62f; // monospace advance ≈ 62% of font size
static constexpr float kPopupPaddingPx = 16.0f;  // horizontal item padding (8px each side)
static constexpr float kMinPopupWidthPx = 160.0f;
static constexpr float kMaxPopupWidthPx = 480.0f;

CompletionPopup::CompletionPopup()
{
    AddClass("completion-popup");
    SetOverlayLayer(OverlayLayer::Dropdown);
    UI::Layout::SetElementHidden(*this, true);

    auto scroll = std::make_unique<ScrollView>();
    scroll->Overrides()
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 1.0f);
    m_ScrollView = scroll.get();
    AddChild(std::move(scroll));
}

void CompletionPopup::Show(const std::vector<std::string>& suggestions, float x, float y,
                           float fontPx, float rowHeight)
{
    m_RowHeight = std::max(1.0f, rowHeight);
    RebuildItems(suggestions, fontPx, m_RowHeight);

    // Width: fit the longest suggestion at the editor font size, clamped.
    size_t longest = 0;
    for (const auto& s : suggestions)
        longest = std::max(longest, s.size());
    const float width = std::clamp(static_cast<float>(longest) * fontPx * kCharWidthFactor + kPopupPaddingPx,
                                   kMinPopupWidthPx, kMaxPopupWidthPx);

    const float height = std::min(static_cast<float>(suggestions.size()), static_cast<float>(kMaxVisibleRows)) * m_RowHeight;

    UI::Layout::SetElementHidden(*this, false);
    UI::Layout::SetAbsolutePosition(*this, Mathematics::Rect{x, y, width, height});
    if (m_ScrollView)
        m_ScrollView->SetScrollY(0.0f);
    m_Visible = true;
}

void CompletionPopup::Hide()
{
    if (!m_Visible)
        return;
    UI::Layout::SetElementHidden(*this, true);
    m_Visible = false;
}

bool CompletionPopup::MoveSelection(int delta)
{
    if (m_Items.empty())
        return false;
    const int n = static_cast<int>(m_Items.size());
    m_SelectedIndex = (m_SelectedIndex + delta + n) % n;
    UpdateSelectionStyle();
    ScrollSelectionIntoView();
    return true;
}

const std::string& CompletionPopup::GetSelected() const
{
    static const std::string kEmpty;
    if (m_Items.empty() || m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Items.size()))
        return kEmpty;
    return m_Items[m_SelectedIndex];
}

void CompletionPopup::RebuildItems(const std::vector<std::string>& items, float fontPx, float rowHeight)
{
    if (m_ScrollView)
        if (UIElement* content = m_ScrollView->GetViewport())
            for (UIElement* item : m_ItemElements)
                content->RemoveChild(item);
    m_ItemElements.clear();

    m_Items = items;
    m_SelectedIndex = 0;

    for (int i = 0; i < static_cast<int>(items.size()); ++i)
    {
        auto label = std::make_unique<Label>();
        label->AddClass("completion-item");
        label->SetText(items[i]);
        label->SetFocusable(false);
        // Match the editor font 1:1 and pin a uniform row height so scroll math is exact.
        label->Overrides()
            .Set(Style::FontSize, StyleLength::Px(fontPx))
            .Set(Style::Height, StyleLength::Px(rowHeight))
            .Set(Style::FlexShrink, 0.0f);

        const int idx = i;
        label->RegisterEventHandler(kEventMouseDown, [this, idx](UIEvent& e) {
            m_SelectedIndex = idx;
            UpdateSelectionStyle();
            if (m_OnAccept)
                m_OnAccept(m_Items[idx]);
            e.Stop();
        });

        m_ItemElements.push_back(label.get());
        if (m_ScrollView)
            m_ScrollView->AddContent(std::move(label));
        else
            AddChild(std::move(label));
    }

    UpdateSelectionStyle();
}

void CompletionPopup::UpdateSelectionStyle()
{
    for (int i = 0; i < static_cast<int>(m_ItemElements.size()); ++i)
    {
        if (i == m_SelectedIndex)
            m_ItemElements[i]->AddClass("completion-item-selected");
        else
            m_ItemElements[i]->RemoveClass("completion-item-selected");
    }
}

void CompletionPopup::ScrollSelectionIntoView()
{
    if (!m_ScrollView || m_SelectedIndex < 0)
        return;

    const float itemTop = static_cast<float>(m_SelectedIndex) * m_RowHeight;
    const float itemBottom = itemTop + m_RowHeight;
    const float viewportH = m_ScrollView->GetViewportHeight();
    const float scrollY = m_ScrollView->GetScrollY();

    if (itemTop < scrollY)
        m_ScrollView->SetScrollY(itemTop);
    else if (itemBottom > scrollY + viewportH)
        m_ScrollView->SetScrollY(itemBottom - viewportH);
}

} // namespace GameEngine
