#include "Editor/Hierarchy/HierarchyNavigationBar.h"

#include "Editor/EditorTreeTitleIconVars.h"
#include "UI/Controls/ItemSizeSlider.h"

#include <cmath>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kStyleAssetPath = "UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.css";
constexpr float kTreeIconSizeStepPx = 1.0f;
// Sizes closer than this are the same size: the slider's step is a whole pixel.
constexpr float kSameSizeTolerancePx = 0.1f;
} // namespace

HierarchyNavigationBar::HierarchyNavigationBar()
{
    AddClass("hierarchy-navigation-bar");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    auto sizeSlider = std::make_unique<EditorUI::ItemSizeSlider>();
    m_SizeSlider = sizeSlider.get();
    m_SizeSlider->AddClass("hierarchy-size-slider");
    m_SizeSlider->SetMin(kMinEditorTreeIconSizePx);
    m_SizeSlider->SetMax(kMaxEditorHierarchyTreeIconSizePx);
    m_SizeSlider->SetStep(kTreeIconSizeStepPx);
    m_SizeSlider->SetOnValueChanging([this](const float& px) { ReportDrag(px); });
    m_SizeSlider->SetOnValueChanged([this](const float& px)
    {
        if (m_OnItemSizeChanged)
            m_OnItemSizeChanged(px);
    });
    m_SizeSlider->SetOnResizeGesture([this](float scrollY) { ApplyResizeGesture(scrollY); });
    AddChild(std::move(sizeSlider));
}

void HierarchyNavigationBar::SetItemSize(float px)
{
    if (m_ReportingDrag || std::fabs(m_SizeSlider->GetValue() - px) <= kSameSizeTolerancePx)
        return;
    m_SizeSlider->SetValueWithoutNotify(px);
}

void HierarchyNavigationBar::ReportDrag(float px)
{
    if (!m_OnItemSizeChanging)
        return;
    m_ReportingDrag = true;
    m_OnItemSizeChanging(px);
    m_ReportingDrag = false;
}

// The same step the gesture takes over the tree, from the size the slider shows.
void HierarchyNavigationBar::ApplyResizeGesture(float scrollY)
{
    const float current = m_SizeSlider->GetValue();
    const float resized = EditorTreeIconSizeAfterResizeGesture(current, scrollY, kMaxEditorHierarchyTreeIconSizePx);
    if (std::fabs(resized - current) > kSameSizeTolerancePx && m_OnItemSizeChanged)
        m_OnItemSizeChanged(resized);
}

void HierarchyNavigationBar::SetSearchBar(std::unique_ptr<UIElement> searchBar)
{
    if (!searchBar)
        return;
    searchBar->AddClass("hierarchy-navigation-bar-search");
    AddChild(std::move(searchBar));
}

void HierarchyNavigationBar::SetAboveHorizontalScrollbar(bool above)
{
    if (above)
        AddClass("hierarchy-navigation-bar-above-scrollbar");
    else
        RemoveClass("hierarchy-navigation-bar-above-scrollbar");
}

} // namespace GameEngine::Editor
