#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <memory>

namespace GameEngine
{
namespace EditorUI
{
class ItemSizeSlider;
}

namespace Editor
{

// The Hierarchy's row of item size slider and panel search, pinned to the panel's bottom edge,
// or to its top edge when search bars sit at the top. The row, not the search inside it, is what
// that setting moves, so the panel registers the search with this row as its placement host
// (EditorSearchBars::RegisterHosted). The layout comes from
// UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.css, requested for this row's subtree.
//
// The slider sets the Hierarchy's tree icon size. The row reports sizes through two callbacks and
// is told the current size back; the panel owns the size.
class HierarchyNavigationBar : public UIElement
{
public:
    HierarchyNavigationBar();

    // The size the slider shows. Raises neither callback, and leaves the slider alone while it is
    // reporting a drag, so the drag keeps its thumb.
    void SetItemSize(float px);

    // While the slider drags: the size under the thumb.
    void SetOnItemSizeChanging(std::function<void(float)> callback) { m_OnItemSizeChanging = std::move(callback); }

    // A size to commit: the slider released, or the item resize gesture over the slider.
    void SetOnItemSizeChanged(std::function<void(float)> callback) { m_OnItemSizeChanged = std::move(callback); }

    // Takes the panel search (BuildPanelSearchBar's root) as the row's flexible second item.
    void SetSearchBar(std::unique_ptr<UIElement> searchBar);

    // Lifts the row above the tree's horizontal scrollbar while one is on screen.
    void SetAboveHorizontalScrollbar(bool above);

private:
    void ReportDrag(float px);
    void ApplyResizeGesture(float scrollY);

    EditorUI::ItemSizeSlider* m_SizeSlider = nullptr;
    std::function<void(float)> m_OnItemSizeChanging;
    std::function<void(float)> m_OnItemSizeChanged;
    bool m_ReportingDrag = false;
};

} // namespace Editor
} // namespace GameEngine
