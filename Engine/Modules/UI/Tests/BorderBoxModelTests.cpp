// `border-width` is part of the box model, so Yoga has to be told about it.
//
// CSS: the content box of an element is its border box inset by border AND
// padding, and children are laid out inside that content box. Yoga models this
// natively (YGNodeStyleSetBorder), but nothing in this engine called it — the
// widths lived on VisualStyle, which the layout solve never reads — so every
// child of a bordered container sat one border-width too far out on each edge
// and was two border-widths too wide on each axis.
//
// The numbers below are Chrome's for
//     box-sizing: border-box; width: 100px; padding: 10px; border: 5px
// which is the only box model this engine has: there is no `box-sizing`
// property, and Yoga's width is always the border box.
//
// The second test is the same defect seen from the text side. Wrap width is
// decided twice — once by the Yoga measure callback (which is handed the
// content width Yoga solved) and once by the renderer (ComputeContentBox,
// which always subtracted border). Those two agree only when Yoga knows the
// border, so a bordered wrapping label used to be measured against a box
// border-left + border-right wider than the one it was painted into, and Yoga
// sized it for fewer lines than the renderer went on to draw.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="box">
    <uielement id="child"/>
  </uielement>
</uielement>)";

// Authored with the `border` shorthand, which is what shipped CSS uses: the
// widths have to survive shorthand expansion into the per-edge longhands and
// land in LayoutInputs for the solve to see them.
constexpr char kCss[] = R"(
#root  { display: flex; flex-direction: column; width: 400px; height: 300px; }
#box   { width: 100px; padding: 10px; border: 5px solid #ff0000; }
#child { height: 20px; }
)";

// Chrome, devtools computed box model for #child inside #box.
constexpr float kChromeChildWidth = 70.0f;  // 100 - 2*padding - 2*border
constexpr float kChromeChildOffset = 15.0f; // padding + border

} // namespace

TEST(BorderBoxModel, BorderInsetsTheContentBoxLikeChrome)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, kCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const GameEngine::UIElement* box = fx.Element("box");
    const GameEngine::UIElement* child = fx.Element("child");
    ASSERT_NE(box, nullptr);
    ASSERT_NE(child, nullptr);

    // The widths must be on the layout inputs, not merely parsed: reading them
    // off VisualStyle is what let the solve ignore them.
    const GameEngine::ResolvedStyle* boxStyle = fx.Style("box");
    ASSERT_NE(boxStyle, nullptr);
    EXPECT_FLOAT_EQ(boxStyle->Layout.BorderWidth.Left, 5.0f);
    EXPECT_FLOAT_EQ(boxStyle->Layout.BorderWidth.Top, 5.0f);
    EXPECT_FLOAT_EQ(boxStyle->Layout.BorderWidth.Right, 5.0f);
    EXPECT_FLOAT_EQ(boxStyle->Layout.BorderWidth.Bottom, 5.0f);

    // Yoga's width is the border box, so the authored 100px is unchanged.
    EXPECT_FLOAT_EQ(box->GetLayoutWidth(), 100.0f);

    // Both numbers move together under the defect: without the border in the
    // solve the child is 80 wide at offset 10, so either assertion alone would
    // catch it and the pair pins the whole inset rather than one edge.
    EXPECT_FLOAT_EQ(child->GetLayoutWidth(), kChromeChildWidth);
    EXPECT_FLOAT_EQ(child->GetLayoutX() - box->GetLayoutX(), kChromeChildOffset);
    EXPECT_FLOAT_EQ(child->GetLayoutY() - box->GetLayoutY(), kChromeChildOffset);
}

// An auto-sized absolutely positioned container is sized by a post-pass of the
// engine's own (UIManager_Layout.cpp), not by Yoga: it takes the union of its
// children's committed rects and adds the trailing inset. The children's rects
// are measured from the container's BORDER-box origin, so once Yoga insets them
// by the border the trailing border has to be added back alongside the trailing
// padding — otherwise every fit-content popup ends up one border-width short on
// its right and bottom edges.
//
// Reachable in shipped CSS: `.dropdown-items.fit-content` is
// `position:absolute; width:auto; border-width:1px` (widgets.css:56,72), as is
// `.popover-panel` (widgets.css:86) when auto-sized.
namespace
{

constexpr char kAbsXml[] = R"(<uielement id="root">
  <uielement id="bordered"><uielement id="bchild"/></uielement>
  <uielement id="plain"><uielement id="pchild"/></uielement>
</uielement>)";

constexpr char kAbsCss[] = R"(
#root     { display: flex; flex-direction: column; width: 400px; height: 300px; }
#bordered { position: absolute; left: 0px; top: 0px; width: auto; height: auto;
            padding: 4px; border: 10px solid #ff0000; }
#plain    { position: absolute; left: 0px; top: 200px; width: auto; height: auto;
            padding: 4px; }
#bchild   { width: 120px; height: 20px; }
#pchild   { width: 120px; height: 20px; }
)";

// Chrome, `box-sizing: border-box` (the engine's only box model — Yoga's width
// is always the border box), getBoundingClientRect on the absolute container.
constexpr float kChromeAbsBorderedW = 148.0f; // 10 + 4 + 120 + 4 + 10
constexpr float kChromeAbsBorderedH = 48.0f;  // 10 + 4 +  20 + 4 + 10
// Control: identical but for the border. Its value is the same in both arms,
// so it fails only if the auto-size post-pass broke for reasons unrelated to
// the border term.
constexpr float kChromeAbsPlainW = 128.0f; // 4 + 120 + 4
constexpr float kChromeAbsPlainH = 28.0f;  // 4 +  20 + 4

} // namespace

TEST(BorderBoxModel, AutoSizedAbsoluteContainerKeepsItsTrailingBorder)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kAbsXml, kAbsCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const GameEngine::UIElement* plain = fx.Element("plain");
    ASSERT_NE(plain, nullptr);
    EXPECT_FLOAT_EQ(plain->GetLayoutWidth(), kChromeAbsPlainW);
    EXPECT_FLOAT_EQ(plain->GetLayoutHeight(), kChromeAbsPlainH);

    const GameEngine::UIElement* bordered = fx.Element("bordered");
    ASSERT_NE(bordered, nullptr);
    EXPECT_FLOAT_EQ(bordered->GetLayoutWidth(), kChromeAbsBorderedW);
    EXPECT_FLOAT_EQ(bordered->GetLayoutHeight(), kChromeAbsBorderedH);
}

// `border-style: none` computes `border-width` to zero (CSS Backgrounds 3
// §4.3), so an invisible border must not eat layout space. Resolving that at
// cascade time is what keeps the ~15 independent readers of BorderWidth — the
// Yoga inset, ComputeContentBox, text wrap widths, the clip inset, the painted
// ring — from having to agree by coincidence.
namespace
{

constexpr char kNoneXml[] = R"(<uielement id="root">
  <uielement id="solid"><uielement id="schild"/></uielement>
  <uielement id="nostyle"><uielement id="nchild"/></uielement>
  <uielement id="shorthand"><uielement id="hchild"/></uielement>
</uielement>)";

// Three arms on one cascade: an explicit style/width pair that must keep its
// inset, the same pair with `border-style: none`, and the shipped-CSS shape —
// a `border` shorthand carrying a width, later overridden by `border: none`.
// The shorthand emits no width longhand (CSSValueParsers `haveW`), so the 10px
// survives into the cascade and only the used-value rule can zero it.
constexpr char kNoneCss[] = R"(
#root      { display: flex; flex-direction: column; width: 400px; height: 300px; }
#solid     { width: 100px; border-width: 10px; border-style: solid; border-color: #ff0000; }
#nostyle   { width: 100px; border-width: 10px; border-style: none; }
#shorthand { width: 100px; border: 10px solid #ff0000; }
#shorthand { border: none; }
#schild    { height: 20px; }
#nchild    { height: 20px; }
#hchild    { height: 20px; }
)";

} // namespace

TEST(BorderBoxModel, BorderStyleNoneComputesWidthToZero)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kNoneXml, kNoneCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    // Control arm — a real border still insets. Chrome: computed
    // border-left-width 10px, child at offset 10, child width 80. Identical in
    // both arms, so it isolates the `none` rule from the border inset itself.
    const GameEngine::ResolvedStyle* solidStyle = fx.Style("solid");
    const GameEngine::UIElement* solid = fx.Element("solid");
    const GameEngine::UIElement* schild = fx.Element("schild");
    ASSERT_NE(solidStyle, nullptr);
    ASSERT_NE(solid, nullptr);
    ASSERT_NE(schild, nullptr);
    EXPECT_FLOAT_EQ(solidStyle->Layout.BorderWidth.Left, 10.0f);
    EXPECT_FLOAT_EQ(schild->GetLayoutWidth(), 80.0f);
    EXPECT_FLOAT_EQ(schild->GetLayoutX() - solid->GetLayoutX(), 10.0f);

    // Chrome, same cascade: computed border-left-width 0px, offset 0, width 100.
    for (const char* id : {"nostyle", "shorthand"})
    {
        const std::string childId = (std::string(id) == "nostyle") ? "nchild" : "hchild";
        const GameEngine::ResolvedStyle* style = fx.Style(id);
        const GameEngine::UIElement* box = fx.Element(id);
        const GameEngine::UIElement* child = fx.Element(childId);
        ASSERT_NE(style, nullptr) << id;
        ASSERT_NE(box, nullptr) << id;
        ASSERT_NE(child, nullptr) << id;

        EXPECT_FLOAT_EQ(style->Layout.BorderWidth.Left, 0.0f) << id;
        EXPECT_FLOAT_EQ(style->Layout.BorderWidth.Top, 0.0f) << id;
        EXPECT_FLOAT_EQ(style->Layout.BorderWidth.Right, 0.0f) << id;
        EXPECT_FLOAT_EQ(style->Layout.BorderWidth.Bottom, 0.0f) << id;
        EXPECT_FLOAT_EQ(child->GetLayoutWidth(), 100.0f) << id;
        EXPECT_FLOAT_EQ(child->GetLayoutX() - box->GetLayoutX(), 0.0f) << id;
        EXPECT_FLOAT_EQ(child->GetLayoutY() - box->GetLayoutY(), 0.0f) << id;
    }
}

// TextArea::OnPostLayout auto-grows a scrolled text area to the height of its
// own wrapped text. It sets a BORDER-box Height override so Yoga produces the
// rect, and compares against GetLayoutHeight, which is also the border box —
// so the border belongs in the sum next to the padding. ScriptTextArea's
// two copies of this arithmetic add it; the base class it derives from did not.
namespace
{

constexpr char kGrowXml[] = R"(<uielement id="root">
  <scrollview id="sv">
    <textarea id="bordered" value="The quick brown fox jumps over the lazy dog while the sleepy cat watches from a sunny window ledge nearby."/>
    <textarea id="plain" value="The quick brown fox jumps over the lazy dog while the sleepy cat watches from a sunny window ledge nearby."/>
  </scrollview>
</uielement>)";

// The two areas are given the same CONTENT width — 220 minus a 10px border
// each side, versus a bare 200 — so the same text wraps to the same number of
// lines in both. That is what makes the height difference attributable to the
// border term alone rather than to a different line count.
constexpr char kGrowCss[] = R"(
#root     { display: flex; flex-direction: column; width: 600px; height: 400px; }
#sv       { width: 600px; height: 400px; }
#bordered { font-family: Roboto; font-size: 16px; white-space: normal;
            padding: 0px; height: 20px; flex-shrink: 0;
            width: 220px; border: 10px solid #ff0000; }
#plain    { font-family: Roboto; font-size: 16px; white-space: normal;
            padding: 0px; height: 20px; flex-shrink: 0;
            width: 200px; }
)";

constexpr float kGrowBorderTotal = 20.0f; // top + bottom

} // namespace

TEST(BorderBoxModel, ScrolledTextAreaGrowsToABorderBoxHeight)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kGrowXml, kGrowCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    if (fx.ResolvedFontFamily("plain").find("Roboto") == std::string::npos)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const GameEngine::UIElement* bordered = fx.Element("bordered");
    const GameEngine::UIElement* plain = fx.Element("plain");
    ASSERT_NE(bordered, nullptr);
    ASSERT_NE(plain, nullptr);

    // Specimen guards — true in both arms. Without the grow actually firing
    // both areas would sit at their authored 20px and the difference below
    // would be 0 for a reason that has nothing to do with the border.
    ASSERT_GT(plain->GetLayoutHeight(), 20.0f);
    ASSERT_GT(bordered->GetLayoutHeight(), 20.0f);

    // The discriminator. Equal content widths wrap to equal line counts, so the
    // grown border boxes differ by exactly the border they enclose. Without the
    // border term both sums are the same line stack and the difference is 0.
    EXPECT_FLOAT_EQ(bordered->GetLayoutHeight() - plain->GetLayoutHeight(), kGrowBorderTotal);
}

// A border change has to re-run the solve. LayoutInputs::operator== is what
// gates the Yoga re-push (UIManager_Layout.cpp `PrevLayout != cs.Layout`), so
// a border width parked on VisualStyle would compare equal and the node would
// keep its old geometry however loudly the impact table called it a layout
// property.
TEST(BorderBoxModel, BorderWidthParticipatesInLayoutInputEquality)
{
    GameEngine::LayoutInputs a{};
    GameEngine::LayoutInputs b{};
    ASSERT_TRUE(a == b);

    b.BorderWidth.Left = 5.0f;
    EXPECT_FALSE(a == b);
}
