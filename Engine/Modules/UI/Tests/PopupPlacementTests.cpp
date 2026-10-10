#include <gtest/gtest.h>

#include "UI/Layout/PopupPlacement.h"

namespace
{
using GameEngine::UI::Layout::ClampPopupToViewport;
using GameEngine::UI::Layout::PlaceSubmenuInViewport;
using GameEngine::UI::Layout::PopupRect;

TEST(PopupPlacementTests, ClampsRootPopupInsideRightAndBottomEdges)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const auto position =
        ClampPopupToViewport(viewport, 1250.0f, 780.0f, 240.0f, 300.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 1036.0f);
    EXPECT_FLOAT_EQ(position.Y, 496.0f);
}

TEST(PopupPlacementTests, ClampsRootPopupInsideLeftAndTopEdges)
{
    const PopupRect viewport{20.0f, 30.0f, 800.0f, 600.0f};
    const auto position =
        ClampPopupToViewport(viewport, -50.0f, -80.0f, 180.0f, 220.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 24.0f);
    EXPECT_FLOAT_EQ(position.Y, 34.0f);
}

TEST(PopupPlacementTests, KeepsSubmenuOnRightWhenItFits)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const PopupRect anchor{300.0f, 200.0f, 180.0f, 24.0f};
    const auto position =
        PlaceSubmenuInViewport(viewport, anchor, 220.0f, 260.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 480.0f);
    EXPECT_FLOAT_EQ(position.Y, 200.0f);
}

TEST(PopupPlacementTests, FlipsSubmenuLeftAtRightEdge)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const PopupRect anchor{1080.0f, 200.0f, 180.0f, 24.0f};
    const auto position =
        PlaceSubmenuInViewport(viewport, anchor, 220.0f, 260.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 860.0f);
    EXPECT_FLOAT_EQ(position.Y, 200.0f);
}

TEST(PopupPlacementTests, ClampsSubmenuAboveBottomEdge)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const PopupRect anchor{300.0f, 760.0f, 180.0f, 24.0f};
    const auto position =
        PlaceSubmenuInViewport(viewport, anchor, 220.0f, 260.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 480.0f);
    EXPECT_FLOAT_EQ(position.Y, 536.0f);
}

TEST(PopupPlacementTests, OversizedPopupPinsToViewportMargin)
{
    const PopupRect viewport{0.0f, 0.0f, 100.0f, 80.0f};
    const auto position =
        ClampPopupToViewport(viewport, 50.0f, 40.0f, 200.0f, 160.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 4.0f);
    EXPECT_FLOAT_EQ(position.Y, 4.0f);
}

} // namespace
