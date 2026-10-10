// The hover tooltip stays inside the window. A source that asks for a side
// (TooltipPlacement Right, Below) at the window's right or bottom edge gets the
// bubble on the opposite side when it fits there, and a bubble that fits on
// neither side is kept inside the window.

#include <gtest/gtest.h>

#include <string>

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"

using namespace GameEngine;

namespace
{

// A 400 x 300 window with one tooltip source in its bottom-right corner.
constexpr char kXml[] = R"(<uielement id="root"><uielement id="source"/></uielement>)";
constexpr char kCss[] = R"(
#root { width: 400px; height: 300px; }
#source { position: absolute; left: 340px; top: 260px; width: 50px; height: 30px; }
)";

// Fits to the left of and above the source, not to its right or below it.
constexpr char kShortText[] = "Move";
// Wider than the room on either side of the source.
constexpr char kWideText[] = "A tooltip wider than the room between its source and the window edge";

struct Placed
{
    float SourceX = 0.0f;
    float SourceY = 0.0f;
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

// Hovers the source past the hover delay. False when the fixture cannot run
// (`skipped`, the caller skips) or no bubble was placed.
bool HoverSource(UIElement::TooltipPlacement placement, const char* text, Placed& placed, bool& skipped)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    skipped = !fixture.DeviceAvailable();
    if (skipped || !built)
        return false;

    UIElement* source = fixture.Element("source");
    source->SetTooltip(text);
    source->SetTooltipPlacement(placement);
    fixture.Manager().OnMouseMove(source->GetLayoutX() + 5.0f, source->GetLayoutY() + 5.0f);
    // Past the 0.5 s hover delay, then frames for the bubble to measure and place.
    for (int frame = 0; frame < 4; ++frame)
        fixture.PumpSeconds(0.3f);
    const UIElement* bubble = fixture.Element("ui-tooltip-wrapper");
    if (!bubble || bubble->GetLayoutWidth() <= 0.0f)
        return false;
    placed = {source->GetLayoutX(), source->GetLayoutY(), bubble->GetLayoutX(), bubble->GetLayoutY(),
              bubble->GetLayoutWidth(), bubble->GetLayoutHeight()};
    return true;
}

void ExpectInsideTheWindow(const Placed& bubble)
{
    EXPECT_GE(bubble.X, 0.0f);
    EXPECT_GE(bubble.Y, 0.0f);
    EXPECT_LE(bubble.X + bubble.W, 400.0f) << "right edge";
    EXPECT_LE(bubble.Y + bubble.H, 300.0f) << "bottom edge";
}

} // namespace

TEST(TooltipPlacementTests, ARightTooltipAtTheWindowsRightEdgeFlipsLeft)
{
    Placed bubble;
    bool skipped = false;
    const bool shown = HoverSource(UIElement::TooltipPlacement::Right, kShortText, bubble, skipped);
    if (skipped)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(shown) << "no tooltip shown";
    ExpectInsideTheWindow(bubble);
    EXPECT_LE(bubble.X + bubble.W, bubble.SourceX) << "the bubble sits left of its source";
}

TEST(TooltipPlacementTests, ABelowTooltipAtTheWindowsBottomEdgeFlipsAbove)
{
    Placed bubble;
    bool skipped = false;
    const bool shown = HoverSource(UIElement::TooltipPlacement::Below, kShortText, bubble, skipped);
    if (skipped)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(shown) << "no tooltip shown";
    ExpectInsideTheWindow(bubble);
    EXPECT_LE(bubble.Y + bubble.H, bubble.SourceY) << "the bubble sits above its source";
}

// Neither side has room for the bubble: it is clamped inside the window instead.
TEST(TooltipPlacementTests, ARightTooltipThatFitsOnNeitherSideStaysInsideTheWindow)
{
    Placed bubble;
    bool skipped = false;
    const bool shown = HoverSource(UIElement::TooltipPlacement::Right, kWideText, bubble, skipped);
    if (skipped)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(shown) << "no tooltip shown";
    EXPECT_GT(bubble.W, bubble.SourceX - 16.0f - 6.0f) << "the text must be too wide for the left side";
    ExpectInsideTheWindow(bubble);
}
