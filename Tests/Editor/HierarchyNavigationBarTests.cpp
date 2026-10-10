// The Hierarchy's navigation row reports tree icon sizes from its slider and is told the panel's
// size back. These tests drive the row's real slider: a plain wheel passes through for scrolling,
// the item resize gesture commits the next size and stops there, the panel's size moves the slider
// without echoing back, and a size the panel applies mid-drag does not yank the dragged thumb.

#include <gtest/gtest.h>

#include "Editor/EditorTreeTitleIconVars.h"
#include "Editor/Hierarchy/HierarchyNavigationBar.h"
#include "Input/InputSystem.h"
#include "UI/Controls/Slider.h"
#include "UI/UIEvents.h"

#include <vector>

using GameEngine::Slider;
using GameEngine::UIEvent;
using GameEngine::Editor::HierarchyNavigationBar;

namespace
{

constexpr float kStartSizePx = 40.0f;
constexpr float kWheelUpOneDetent = -30.0f;

Slider& SizeSliderOf(HierarchyNavigationBar& row)
{
    for (const auto& child : row.GetChildren())
    {
        if (child->HasClass("hierarchy-size-slider"))
            return static_cast<Slider&>(*child);
    }
    ADD_FAILURE() << "the row has no size slider";
    return static_cast<Slider&>(*row.GetChildren().front());
}

// Dispatches a wheel event at the slider and reports whether a handler stopped it.
bool WheelOver(Slider& slider, int mods)
{
    UIEvent e;
    e.Id = GameEngine::kEventScroll;
    e.Mods = mods;
    e.ScrollY = kWheelUpOneDetent;
    slider.DispatchEvent(e);
    return e.Handled;
}

struct RecordedSizes
{
    std::vector<float> Changing;
    std::vector<float> Changed;

    void Attach(HierarchyNavigationBar& row)
    {
        row.SetOnItemSizeChanging([this](float px) { Changing.push_back(px); });
        row.SetOnItemSizeChanged([this](float px) { Changed.push_back(px); });
    }
};

} // namespace

TEST(HierarchyNavigationBar, APlainWheelOverTheSliderPassesThrough)
{
    HierarchyNavigationBar row;
    row.SetItemSize(kStartSizePx);
    RecordedSizes sizes;
    sizes.Attach(row);

    EXPECT_FALSE(WheelOver(SizeSliderOf(row), /*mods=*/0)) << "a plain wheel was stopped at the slider";
    EXPECT_TRUE(sizes.Changed.empty());
    EXPECT_FLOAT_EQ(SizeSliderOf(row).GetValue(), kStartSizePx);
}

TEST(HierarchyNavigationBar, TheResizeGestureOverTheSliderCommitsTheNextSize)
{
    HierarchyNavigationBar row;
    row.SetItemSize(kStartSizePx);
    RecordedSizes sizes;
    sizes.Attach(row);

    // With no host matcher installed, the gesture is the primary shortcut modifier (Ctrl or Cmd).
    EXPECT_TRUE(WheelOver(SizeSliderOf(row), GameEngine::Input::kModControl))
        << "the gesture was not stopped at the slider, so it would also scroll";
    ASSERT_EQ(sizes.Changed.size(), 1u);
    EXPECT_FLOAT_EQ(sizes.Changed[0], GameEngine::EditorTreeIconSizeAfterResizeGesture(
                                          kStartSizePx, kWheelUpOneDetent, GameEngine::kMaxEditorHierarchyTreeIconSizePx));
    EXPECT_GT(sizes.Changed[0], kStartSizePx) << "wheel up must make items bigger";
    EXPECT_TRUE(sizes.Changing.empty());
}

TEST(HierarchyNavigationBar, ThePanelSizeMovesTheSliderWithoutEchoing)
{
    HierarchyNavigationBar row;
    RecordedSizes sizes;
    sizes.Attach(row);

    row.SetItemSize(kStartSizePx);
    EXPECT_FLOAT_EQ(SizeSliderOf(row).GetValue(), kStartSizePx);
    EXPECT_TRUE(sizes.Changing.empty());
    EXPECT_TRUE(sizes.Changed.empty());
}

TEST(HierarchyNavigationBar, ASizeAppliedMidDragDoesNotMoveTheDraggedThumb)
{
    constexpr float kDraggedToPx = 50.0f;
    constexpr float kPanelClampedPx = 20.0f;
    HierarchyNavigationBar row;
    row.SetItemSize(kStartSizePx);
    std::vector<float> committed;
    // The panel answers a drag by applying the size and telling the row its own value back.
    row.SetOnItemSizeChanging([&row](float) { row.SetItemSize(kPanelClampedPx); });
    row.SetOnItemSizeChanged([&committed](float px) { committed.push_back(px); });

    SizeSliderOf(row).SetValue(kDraggedToPx);
    EXPECT_FLOAT_EQ(SizeSliderOf(row).GetValue(), kDraggedToPx);
    ASSERT_EQ(committed.size(), 1u);
    EXPECT_FLOAT_EQ(committed[0], kDraggedToPx);
}
