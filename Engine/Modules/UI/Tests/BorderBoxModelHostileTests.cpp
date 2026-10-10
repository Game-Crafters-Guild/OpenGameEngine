// Border box model under hostile shapes and dynamic restyle.
//
// Chrome ground truth measured with real
// `chrome.exe --headless=new --force-device-scale-factor=1` under
// `box-sizing: border-box`, which is the engine's only box model — Chrome's
// content-box default is not.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

using GameEngine::UITesting::IsolatedUIFixture;

// ---------------------------------------------------------------------------
// A pseudo-state rule that sets ONLY border-style changes the USED
// border-width, because border-style: none computes the width to zero. The
// impact table still classifies BorderStyle as paint-only
// (UIStyle.h, StylePropertyId::BorderStyle -> {Layout=false, Paint=true}), so
// nothing in the classification schedules a re-solve.
//
// What keeps it correct is the LayoutInputs diff: the Yoga re-push keys on
// PrevLayout != cs.Layout rather than on the impact flags, and BorderWidth
// lives in LayoutInputs. That is the whole load-bearing mechanism, so it gets
// an end-to-end test driving real pointer motion rather than a unit assertion.
//
// Reclassifying BorderStyle as layout-affecting is NOT the fix: m_StyleAnalysis
// is global per sheet-set, so it would force the layout-signature pass on every
// hover edge in any sheet containing a style-only rule.
//
// Chrome: solid arm child at +15/70, none arm child at +10/80.
// ---------------------------------------------------------------------------
namespace
{

constexpr char kHoverXml[] = R"(<uielement id="root">
  <uielement id="hbox"><uielement id="hchild"/></uielement>
  <uielement id="rbox"><uielement id="rchild"/></uielement>
</uielement>)";

// #hbox: hover REMOVES the border (solid -> none), style-only rule.
// #rbox: hover ADDS the border (none -> solid), style-only rule — the
// MovieRecorderPanel.css `border-style: solid` :hover shape.
constexpr char kHoverCss[] = R"(
#root  { display: flex; flex-direction: column; width: 400px; height: 300px; }
#hbox  { position: absolute; left: 0px; top: 0px; width: 100px; height: 50px;
         padding: 10px; border: 5px solid #ff0000; }
#hbox:hover { border-style: none; }
#rbox  { position: absolute; left: 200px; top: 0px; width: 100px; height: 50px;
         padding: 10px; border-width: 5px; border-style: none;
         border-color: #ff0000; }
#rbox:hover { border-style: solid; }
#hchild { height: 20px; }
#rchild { height: 20px; }
)";

} // namespace

TEST(BorderBoxModelHostile, HoverStyleOnlyFlipMovesChildren)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kHoverXml, kHoverCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const GameEngine::UIElement* hbox = fx.Element("hbox");
    const GameEngine::UIElement* hchild = fx.Element("hchild");
    const GameEngine::UIElement* rbox = fx.Element("rbox");
    const GameEngine::UIElement* rchild = fx.Element("rchild");
    ASSERT_NE(hbox, nullptr);
    ASSERT_NE(hchild, nullptr);
    ASSERT_NE(rbox, nullptr);
    ASSERT_NE(rchild, nullptr);

    // Baseline, no hover: hbox has a real border, rbox's is style:none.
    EXPECT_FLOAT_EQ(hchild->GetLayoutX() - hbox->GetLayoutX(), 15.0f);
    EXPECT_FLOAT_EQ(hchild->GetLayoutWidth(), 70.0f);
    EXPECT_FLOAT_EQ(rchild->GetLayoutX() - rbox->GetLayoutX(), 10.0f);
    EXPECT_FLOAT_EQ(rchild->GetLayoutWidth(), 80.0f);

    // Hover hbox: border-style:none must ZERO the used width -> child moves
    // out to the padding-only inset. Chrome: +10, 80 wide.
    fx.Manager().OnMouseMove(hbox->GetLayoutX() + 50.0f, hbox->GetLayoutY() + 25.0f);
    fx.Settle();
    EXPECT_FLOAT_EQ(hchild->GetLayoutX() - hbox->GetLayoutX(), 10.0f)
        << "hover border-style:none did not relayout (paint-only classification "
           "skipped the solve?)";
    EXPECT_FLOAT_EQ(hchild->GetLayoutWidth(), 80.0f);
    EXPECT_FLOAT_EQ(fx.Style("hbox")->Layout.BorderWidth.Left, 0.0f);

    // Hover rbox: border-style:solid must RESTORE the declared 5px -> child
    // moves in. Chrome: +15, 70 wide.
    fx.Manager().OnMouseMove(rbox->GetLayoutX() + 50.0f, rbox->GetLayoutY() + 25.0f);
    fx.Settle();
    EXPECT_FLOAT_EQ(rchild->GetLayoutX() - rbox->GetLayoutX(), 15.0f)
        << "hover border-style:solid did not relayout";
    EXPECT_FLOAT_EQ(rchild->GetLayoutWidth(), 70.0f);
    EXPECT_FLOAT_EQ(fx.Style("rbox")->Layout.BorderWidth.Left, 5.0f);
    // And hbox, no longer hovered, must have its border back.
    EXPECT_FLOAT_EQ(hchild->GetLayoutX() - hbox->GetLayoutX(), 15.0f)
        << "un-hover did not restore the border inset";
    EXPECT_FLOAT_EQ(hchild->GetLayoutWidth(), 70.0f);

    // Move to empty space: both at their base styles again.
    fx.Manager().OnMouseMove(399.0f, 299.0f);
    fx.Settle();
    EXPECT_FLOAT_EQ(hchild->GetLayoutX() - hbox->GetLayoutX(), 15.0f);
    EXPECT_FLOAT_EQ(rchild->GetLayoutX() - rbox->GetLayoutX(), 10.0f);
    EXPECT_FLOAT_EQ(rchild->GetLayoutWidth(), 80.0f);
}

// ---------------------------------------------------------------------------
// The ConvergePostLayout trailing-edge term under hostile shapes. An auto-sized
// absolute container is sized by the engine's own post-pass, not by Yoga: it
// unions its children's committed rects, which are measured from the
// container's BORDER-box origin, so the trailing border has to be added back
// alongside the trailing padding. Asymmetric widths are the discriminating
// case — they fail differently if the leading and trailing edges are confused.
//
// Chrome: asym 140x36, zero border 128x28, declared-width-under-style-none
// 128x28.
// ---------------------------------------------------------------------------
namespace
{

constexpr char kAbs2Xml[] = R"(<uielement id="root">
  <uielement id="asym"><uielement id="ac"/></uielement>
  <uielement id="zero"><uielement id="zc"/></uielement>
  <uielement id="none"><uielement id="nc"/></uielement>
</uielement>)";

constexpr char kAbs2Css[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#asym { position: absolute; left: 0px; top: 0px; width: auto; height: auto;
        padding: 4px; border-style: solid; border-color: #ff0000;
        border-width: 2px 4px 6px 8px; }
#zero { position: absolute; left: 0px; top: 60px; width: auto; height: auto;
        padding: 4px; border-width: 0px; border-style: solid; border-color: #ff0000; }
#none { position: absolute; left: 0px; top: 120px; width: auto; height: auto;
        padding: 4px; border-width: 10px; border-style: none; }
#ac { width: 120px; height: 20px; }
#zc { width: 120px; height: 20px; }
#nc { width: 120px; height: 20px; }
)";

} // namespace

TEST(BorderBoxModelHostile, AutoSizedAbsoluteHostileShapesMatchChrome)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kAbs2Xml, kAbs2Css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    // Asymmetric per-edge widths: leading edges (8 left, 2 top) arrive via the
    // children's Yoga inset, trailing edges (4 right, 6 bottom) via the
    // post-pass terms. Chrome: 140x36, child at +12,+6.
    const GameEngine::UIElement* asym = fx.Element("asym");
    const GameEngine::UIElement* ac = fx.Element("ac");
    ASSERT_NE(asym, nullptr);
    ASSERT_NE(ac, nullptr);
    EXPECT_FLOAT_EQ(ac->GetLayoutX() - asym->GetLayoutX(), 12.0f); // borderL 8 + pad 4
    EXPECT_FLOAT_EQ(ac->GetLayoutY() - asym->GetLayoutY(), 6.0f);  // borderT 2 + pad 4
    EXPECT_FLOAT_EQ(asym->GetLayoutWidth(), 140.0f);
    EXPECT_FLOAT_EQ(asym->GetLayoutHeight(), 36.0f);

    // Zero border: the trailing term must add exactly 0. Chrome: 128x28.
    const GameEngine::UIElement* zero = fx.Element("zero");
    ASSERT_NE(zero, nullptr);
    EXPECT_FLOAT_EQ(zero->GetLayoutWidth(), 128.0f);
    EXPECT_FLOAT_EQ(zero->GetLayoutHeight(), 28.0f);

    // Declared 10px width under border-style:none: used width 0 everywhere —
    // leading inset AND trailing term. Chrome: 128x28.
    const GameEngine::UIElement* none = fx.Element("none");
    const GameEngine::UIElement* nc = fx.Element("nc");
    ASSERT_NE(none, nullptr);
    ASSERT_NE(nc, nullptr);
    EXPECT_FLOAT_EQ(nc->GetLayoutX() - none->GetLayoutX(), 4.0f);
    EXPECT_FLOAT_EQ(none->GetLayoutWidth(), 128.0f);
    EXPECT_FLOAT_EQ(none->GetLayoutHeight(), 28.0f);
}

// Nested auto-sized absolutes. NOTE: the engine's post-pass unions ALL
// children including absolutely-positioned ones (Chrome's shrink-to-fit
// excludes abs descendants — a pre-existing, deliberate divergence that the
// dropdown design relies on), so the expectations here are the ENGINE'S own
// arithmetic, checked for internal consistency: the inner popup gets its
// trailing border, and the outer's union sees the grown inner rect plus the
// outer's own trailing border.
namespace
{

constexpr char kNestXml[] = R"(<uielement id="root">
  <uielement id="outer">
    <uielement id="inner"><uielement id="ic"/></uielement>
  </uielement>
</uielement>)";

constexpr char kNestCss[] = R"(
#root  { display: flex; flex-direction: column; width: 400px; height: 300px; }
#outer { position: absolute; left: 0px; top: 0px; width: auto; height: auto;
         padding: 3px; border: 5px solid #ff0000; }
#inner { position: absolute; left: 0px; top: 0px; width: auto; height: auto;
         padding: 4px; border: 10px solid #00ff00; }
#ic    { width: 120px; height: 20px; }
)";

} // namespace

TEST(BorderBoxModelHostile, NestedAutoSizedAbsolutesComposeTrailingBorders)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kNestXml, kNestCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const GameEngine::UIElement* outer = fx.Element("outer");
    const GameEngine::UIElement* inner = fx.Element("inner");
    const GameEngine::UIElement* ic = fx.Element("ic");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(ic, nullptr);

    // Inner sizes first (post-order): 10+4+120+4+10 x 10+4+20+4+10.
    EXPECT_FLOAT_EQ(inner->GetLayoutWidth(), 148.0f);
    EXPECT_FLOAT_EQ(inner->GetLayoutHeight(), 48.0f);
    // Its child is inset border+padding from ITS border-box origin.
    EXPECT_FLOAT_EQ(ic->GetLayoutX() - inner->GetLayoutX(), 14.0f);
    EXPECT_FLOAT_EQ(ic->GetLayoutY() - inner->GetLayoutY(), 14.0f);

    // Outer union: inner's committed rect (origin = wherever Yoga put the
    // absolute child, asserted separately below) + outer trailing pad 3 +
    // trailing border 5.
    const float innerOffX = inner->GetLayoutX() - outer->GetLayoutX();
    const float innerOffY = inner->GetLayoutY() - outer->GetLayoutY();
    EXPECT_FLOAT_EQ(outer->GetLayoutWidth(), innerOffX + 148.0f + 3.0f + 5.0f);
    EXPECT_FLOAT_EQ(outer->GetLayoutHeight(), innerOffY + 48.0f + 3.0f + 5.0f);
}

// Percent padding + border on an auto-sized absolute. Chrome (containing block
// 400px): padding 10% = 40px -> 210x110, child at +45,+45. Percentage paddings
// resolve against the containing block's WIDTH on all four edges, so the
// vertical term is 40 too — the trailing height inset is not 10% of 300.
namespace
{

constexpr char kPctXml[] = R"(<uielement id="root">
  <uielement id="pct"><uielement id="pc"/></uielement>
</uielement>)";

constexpr char kPctCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#pct  { position: absolute; left: 0px; top: 0px; width: auto; height: auto;
        padding: 10%; border: 5px solid #ff0000; }
#pc   { width: 120px; height: 20px; }
)";

} // namespace

TEST(BorderBoxModelHostile, PercentPaddingAutoSizedAbsoluteMatchesChrome)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kPctXml, kPctCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const GameEngine::UIElement* pct = fx.Element("pct");
    const GameEngine::UIElement* pc = fx.Element("pc");
    ASSERT_NE(pct, nullptr);
    ASSERT_NE(pc, nullptr);

    // Leading inset: border 5 + resolved padding 40 (10% of the 400px
    // containing block) = 45 on BOTH axes. Pins that the percent resolves at
    // all, and that the vertical edge resolves against width like Chrome's.
    EXPECT_FLOAT_EQ(pc->GetLayoutX() - pct->GetLayoutX(), 45.0f);
    EXPECT_FLOAT_EQ(pc->GetLayoutY() - pct->GetLayoutY(), 45.0f);
    // Chrome: 5 + 40 + 120 + 40 + 5 and 5 + 40 + 20 + 40 + 5.
    EXPECT_FLOAT_EQ(pct->GetLayoutWidth(), 210.0f);
    EXPECT_FLOAT_EQ(pct->GetLayoutHeight(), 110.0f);
}
