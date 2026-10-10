// Pins the paint order between OverlayLayer ranks.
//
// Overlays escape their parent stacking context and are painted in a second
// pass, sorted by OverlayLayer value (UIManager_PrimitiveGen.cpp, phase 2). So
// the rank — not z-index, not tree order — decides which of two overlapping
// overlays the viewer sees. Nothing pinned that before: a rank could be
// reordered, or an element moved to a different one, and every existing test
// stayed green while a menu disappeared behind the popup it was opened over.
//
// Every case declares the element that must WIN first, so ordinary tree order
// would paint the loser on top of it. A pass therefore has exactly one
// explanation: the layer sort overrode tree order. TreeOrderDecidesWithinOneLayer
// is the instrument check — with both elements on the same rank the later
// sibling wins, which is what proves these tests can report either answer.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using GameEngine::UITesting::PixelAt;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using namespace GameEngine;

namespace
{

// Two identically-placed opaque squares. `first` is the earlier sibling, so
// without overlay ranks `second` paints over it.
constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="first"/>
  <uielement id="second"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { position: relative; width: 800px; height: 600px; background-color: #000000; }
#first { position: absolute; left: 200px; top: 150px; width: 200px; height: 200px; background-color: #FF0000; }
#second { position: absolute; left: 200px; top: 150px; width: 200px; height: 200px; background-color: #0000FF; }
)";

bool BuildOrSkip(IsolatedUIFixture& fx, std::string& why)
{
    if (fx.Build(1.0f, kXml, kCss))
        return true;
    why = fx.DeviceAvailable() ? ("fixture build failed: " + fx.Diagnostic()) : "no Vulkan device";
    return false;
}

// Which square the viewer sees at the shared centre. Red and blue are compared
// by channel dominance rather than exact values: the readback is encoded sRGB
// and the question here is which element won, not what the color pipeline did
// to it.
enum class Winner
{
    First,
    Second,
    Neither
};

Winner VisibleSquare(const IsolatedUIFixture& fx)
{
    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    if (px.empty())
        return Winner::Neither;

    const PhysicalRect box = fx.BorderBox("first");
    const Rgb c = PixelAt(px, static_cast<uint32_t>(box.X + box.W * 0.5f),
                          static_cast<uint32_t>(box.Y + box.H * 0.5f));

    constexpr int kChannelMargin = 40;
    if (c.R > c.B + kChannelMargin)
        return Winner::First;
    if (c.B > c.R + kChannelMargin)
        return Winner::Second;
    return Winner::Neither;
}

// Assigns the ranks and re-settles, since Build() cannot express them.
//
// The dirty mark is load-bearing. Primitive slots and their draw-order entries
// are persistent and reused frame to frame, and SetOverlayLayer only refreshes
// the subtree's overlay bit - it does not invalidate the bookkeeping that
// decides WHERE an element is drawn. Settling alone therefore re-renders the
// order captured on the first frame, when both squares were still ordinary
// content. Every production caller sets the layer before its element is first
// rendered, so this is a fixture concern rather than a defect.
void SetLayers(IsolatedUIFixture& fx, OverlayLayer firstLayer, OverlayLayer secondLayer)
{
    fx.Element("first")->SetOverlayLayer(firstLayer);
    fx.Element("second")->SetOverlayLayer(secondLayer);
    fx.Manager().GetRootElement()->MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty |
                                                   UIElement::VisualDirty);
    fx.Settle();
}

} // namespace

// The instrument check. Both squares on the same rank, so nothing overrides
// tree order and the later sibling must win. Without this, a harness that
// always answered "first" would pass every case below for the wrong reason.
TEST(OverlayLayerOrderTests, TreeOrderDecidesWithinOneLayer)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
        GTEST_SKIP() << why;

    SetLayers(fx, OverlayLayer::None, OverlayLayer::None);
    EXPECT_EQ(VisibleSquare(fx), Winner::Second) << "later sibling must paint over the earlier one";
}

// Deferral itself: an overlay is painted in the second pass, so it lands over
// ordinary content that came later in the tree.
TEST(OverlayLayerOrderTests, OverlayPaintsOverOrdinaryContent)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
        GTEST_SKIP() << why;

    SetLayers(fx, OverlayLayer::Tooltip, OverlayLayer::None);
    EXPECT_EQ(VisibleSquare(fx), Winner::First) << "an overlay paints after ordinary content";
}

// The reported defect: the camera-bookmark preview (Tooltip) painted over a
// context menu opened on the bookmark it belonged to.
TEST(OverlayLayerOrderTests, ContextMenuPaintsOverPanelPopup)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
        GTEST_SKIP() << why;

    SetLayers(fx, OverlayLayer::ContextMenu, OverlayLayer::Tooltip);
    EXPECT_EQ(VisibleSquare(fx), Winner::First) << "a context menu must not be covered by a panel popup";
}

// A menu can be opened from inside a modal — over a text field in a dialog, for
// one — so it has to outrank the surface it was opened over.
TEST(OverlayLayerOrderTests, ContextMenuPaintsOverModal)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
        GTEST_SKIP() << why;

    SetLayers(fx, OverlayLayer::ContextMenu, OverlayLayer::Modal);
    EXPECT_EQ(VisibleSquare(fx), Winner::First) << "a context menu must not be covered by a modal";
}

// The upper bound, so the menu's rank cannot be "fixed" later by pushing it to
// the top of the stack: a drag preview still paints over it.
TEST(OverlayLayerOrderTests, DragPreviewPaintsOverContextMenu)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
        GTEST_SKIP() << why;

    SetLayers(fx, OverlayLayer::DragPreview, OverlayLayer::ContextMenu);
    EXPECT_EQ(VisibleSquare(fx), Winner::First) << "a drag preview outranks a context menu";
}

// Unchanged by the context-menu rank, and pinned so it stays that way: panel
// popups still paint over dropdowns.
TEST(OverlayLayerOrderTests, PanelPopupPaintsOverDropdown)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
        GTEST_SKIP() << why;

    SetLayers(fx, OverlayLayer::Tooltip, OverlayLayer::Dropdown);
    EXPECT_EQ(VisibleSquare(fx), Winner::First) << "a panel popup outranks a dropdown";
}

// Pointer routing reads the same rank (UIManager_HoverAndEvents.cpp), so a menu
// that paints on top must also be the thing the pointer lands on — a visible
// menu you cannot click is the same defect wearing a different hat.
TEST(OverlayLayerOrderTests, ContextMenuTakesPointerOverModal)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
        GTEST_SKIP() << why;

    SetLayers(fx, OverlayLayer::ContextMenu, OverlayLayer::Modal);

    int menuEnter = 0;
    int modalEnter = 0;
    fx.Element("first")->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++menuEnter; });
    fx.Element("second")->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++modalEnter; });

    // OnMouseMove takes LOGICAL px; the squares share 200,150 -> 400,350.
    fx.Manager().OnMouseMove(300.0f, 250.0f);
    fx.StepFrame();

    EXPECT_EQ(menuEnter, 1);
    EXPECT_EQ(modalEnter, 0);
}
