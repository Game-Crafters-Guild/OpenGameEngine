#pragma once

#include "UI/UIElement.h"
#include <functional>
#include <string>
#include <vector>

namespace GameEngine {

class ScrollView;

// Floating completion suggestion list anchored below the caret in the script editor.
class CompletionPopup : public UIElement {
public:
    CompletionPopup();

    // fontPx / rowHeight let the list match the script editor's current text size 1:1.
    void Show(const std::vector<std::string>& suggestions, float x, float y,
              float fontPx, float rowHeight);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    // Navigate the selection; wraps around. Returns false when the list is empty.
    bool MoveSelection(int delta);
    const std::string& GetSelected() const;

    void SetOnAccept(std::function<void(const std::string&)> cb) { m_OnAccept = std::move(cb); }

private:
    void RebuildItems(const std::vector<std::string>& items, float fontPx, float rowHeight);
    void UpdateSelectionStyle();
    void ScrollSelectionIntoView();

    ScrollView* m_ScrollView = nullptr;
    std::vector<std::string> m_Items;
    std::vector<UIElement*>  m_ItemElements; // raw ptrs, owned by the ScrollView content
    int   m_SelectedIndex = 0;
    bool  m_Visible = false;
    float m_RowHeight = 22.0f;
    std::function<void(const std::string&)> m_OnAccept;
};

} // namespace GameEngine
