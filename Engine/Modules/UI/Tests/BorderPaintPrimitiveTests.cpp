// The paint side of the border box model, pinned at the PRIMITIVE level.
//
// BorderBoxModelTests proves the LAYOUT numbers (child rects, LayoutInputs
// equality). Nothing there proves the renderer draws those numbers: a correct
// layout rect emitted at the wrong origin, or a ring emitted with the wrong
// widths, would pass every layout assertion and still paint wrong. These tests
// read the emitted UIPrimitives instead.
//
// Ground truth is real Chrome (chrome.exe --headless=new, device scale 1) for
//     box-sizing: border-box; width: 100px; height: 80px;
//     padding: 10px; border: 5px solid red; background: green;  + child
// measured via getBoundingClientRect and per-pixel screenshot samples:
//   - child rect relative to the box: (15, 15, 70, 20)   [border + padding in]
//   - ring: a 5px band at the border-box edge (pixel (2,2) is border color,
//     (7,7) is background — the background shows through the padding zone)
//   - text: content origin inset border + padding from the border box
//
// The engine's Rect primitive draws its ring inset inside prim rect edges and
// its fill inside the ring (ui_sdf.frag: inner = elemRect + bw), so the
// Chrome-equivalent emission is: prim rect == the border box SNAPPED to the
// device grid (round per edge), BorderWidths == the CSS widths in physical px
// snapped to whole device pixels (floor, min 1) — Chrome paints boxes on the
// device grid even though layout keeps the 1/64 fractions (BorderEdgeSnapTests
// pins the snap rules against real Chrome). Children's own primitives land at
// the content box, snapped the same way.
//
// Scales 1.0 and 1.5 both run: ComputeContentBox works in physical px
// (logical * cs) while Yoga solves in logical px, so a border inset applied in
// the wrong space is invisible at 100% and only a fractional scale catches it.

#include "IsolatedUIFixture.h"

#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="box">
    <uielement id="child"/>
  </uielement>
  <label id="tbox" text="Hello"/>
</uielement>)";

// #tbox uses a 25px border: a glyph-origin assertion needs slack for ink
// dilation and per-font bearing, so its discriminator must dwarf the
// tolerance. A dropped border moves the origin by 25*cs — an order of
// magnitude above the 3px slack — where a 1-2px border would drown in it.
constexpr char kCss[] = R"(
#root  { display: flex; flex-direction: column; width: 400px; height: 300px; }
#box   { width: 100px; height: 80px; padding: 10px; border: 5px solid #ff0000;
         background-color: #00ff00; }
#child { height: 20px; background-color: #0000ff; }
#tbox  { width: 200px; padding: 10px; border: 25px solid #ff0000;
         background-color: #00ff00; font-family: Roboto; font-size: 16px; }
)";

// Chrome's numbers for the #box specimen, in logical px.
constexpr float kBorder = 5.0f;
constexpr float kChildInset = 15.0f; // border + padding
constexpr float kChildWidth = 70.0f; // 100 - 2*15
constexpr float kChildHeight = 20.0f;

// #tbox insets, logical px.
constexpr float kTextBorder = 25.0f;
constexpr float kTextInset = 35.0f; // border + padding

// Glyph quads are AA-dilated and carry the font's left/top bearing, so the ink
// edge sits near — not at — the content origin. Both assertions using this
// tolerance discriminate a 25*cs error.
constexpr float kGlyphSlackPx = 3.0f;

// Ink top of a 16px first line sits within this many logical px of the content
// top for any real face (ascent minus cap height plus half-leading).
constexpr float kFirstLineInkDepthPx = 12.0f;

} // namespace

class BorderPaintPrimitives : public ::testing::TestWithParam<float>
{
};

INSTANTIATE_TEST_SUITE_P(Scales, BorderPaintPrimitives, ::testing::Values(1.0f, 1.5f));

TEST_P(BorderPaintPrimitives, RingBackgroundAndChildMatchChrome)
{
    const float cs = GetParam();

    IsolatedUIFixture fx;
    if (!fx.Build(cs, kXml, kCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const PhysicalRect boxBB = fx.BorderBox("box");
    ASSERT_GT(boxBB.W, 0.0f);

    // --- The bordered box's own primitive: rect at the border box, ring of the
    // CSS widths, background fill, border color. One Rect carries all of it.
    const auto boxRects = fx.Primitives("box", PrimitiveMode::Rect);
    ASSERT_EQ(boxRects.size(), 1u);
    const UIPrimitive& bp = boxRects[0];

    EXPECT_FLOAT_EQ(bp.X, boxBB.X);
    EXPECT_FLOAT_EQ(bp.Y, boxBB.Y);
    EXPECT_FLOAT_EQ(bp.W, 100.0f * cs);
    EXPECT_FLOAT_EQ(bp.H, 80.0f * cs);

    // Ring widths L, T, R, B in physical px, snapped to whole device pixels
    // (7.5 paints as 7 — Chrome floors painted border thickness).
    const float ringW = std::floor(kBorder * cs);
    EXPECT_FLOAT_EQ(bp.BorderWidths[0], ringW);
    EXPECT_FLOAT_EQ(bp.BorderWidths[1], ringW);
    EXPECT_FLOAT_EQ(bp.BorderWidths[2], ringW);
    EXPECT_FLOAT_EQ(bp.BorderWidths[3], ringW);

    EXPECT_EQ(bp.FillColor, GameEngine::UI::PackFromARGB(0xFF00FF00u));
    EXPECT_EQ(bp.BorderColor, GameEngine::UI::PackFromARGB(0xFFFF0000u));

    // --- The child's own primitive lands at the parent's content box: Chrome
    // puts it at +15,+15 and 70 wide. Asserted on the EMITTED rect, not the
    // layout rect, so a generator that repositions or re-insets children
    // cannot pass on layout numbers alone.
    const auto childRects = fx.Primitives("child", PrimitiveMode::Rect);
    ASSERT_EQ(childRects.size(), 1u);
    const UIPrimitive& cp = childRects[0];

    // The child's paint rect snaps per edge, so its inset and extent are the
    // differences of snapped edges (22.5 -> 23 at cs 1.5), while its layout
    // rect keeps the fraction.
    const float childX = std::round(kChildInset * cs);
    const float childY = std::round(kChildInset * cs);
    EXPECT_FLOAT_EQ(cp.X - bp.X, childX);
    EXPECT_FLOAT_EQ(cp.Y - bp.Y, childY);
    EXPECT_FLOAT_EQ(cp.W, std::round((kChildInset + kChildWidth) * cs) - childX);
    EXPECT_FLOAT_EQ(cp.H, std::round((kChildInset + kChildHeight) * cs) - childY);

    // And inside the ring — the drawn band must not overlap the child's fill.
    EXPECT_GE(cp.X, bp.X + bp.BorderWidths[0]);
    EXPECT_GE(cp.Y, bp.Y + bp.BorderWidths[1]);
    EXPECT_LE(cp.X + cp.W, bp.X + bp.W - bp.BorderWidths[2]);
    EXPECT_LE(cp.Y + cp.H, bp.Y + bp.H - bp.BorderWidths[3]);
}

TEST_P(BorderPaintPrimitives, OwnTextStartsAtTheContentBox)
{
    const float cs = GetParam();

    IsolatedUIFixture fx;
    if (!fx.Build(cs, kXml, kCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    // The Chrome-derived origin below is font-independent to within
    // kGlyphSlackPx, but no atlas at all means no glyphs to measure.
    const auto glyphs = fx.Primitives("tbox", PrimitiveMode::Slug);
    if (glyphs.empty())
        GTEST_SKIP() << "no font atlas resolved for tbox (resolved family: '"
                     << fx.ResolvedFontFamily("tbox") << "')";

    const auto tboxRects = fx.Primitives("tbox", PrimitiveMode::Rect);
    ASSERT_EQ(tboxRects.size(), 1u);
    const UIPrimitive& tp = tboxRects[0];
    EXPECT_FLOAT_EQ(tp.BorderWidths[0], std::floor(kTextBorder * cs));

    float inkLeft = glyphs[0].X;
    float inkTop = glyphs[0].Y;
    for (const UIPrimitive& g : glyphs)
    {
        inkLeft = std::min(inkLeft, g.X);
        inkTop = std::min(inkTop, g.Y);
    }

    // Chrome: the first glyph's box starts at the content-box origin — border
    // box + border + padding. A paint side that insets by padding only sits
    // 25*cs to the left; one that double-insets sits 25*cs to the right.
    const float contentLeft = tp.X + kTextInset * cs;
    const float contentTop = tp.Y + kTextInset * cs;
    EXPECT_NEAR(inkLeft, contentLeft, kGlyphSlackPx * cs)
        << "cs=" << cs << " tbox.X=" << tp.X;
    EXPECT_GE(inkTop, contentTop - kGlyphSlackPx * cs) << "cs=" << cs;
    EXPECT_LE(inkTop, contentTop + kFirstLineInkDepthPx * cs) << "cs=" << cs;

    // Auto height is a border-box height: one 16px line plus 2*35 of insets.
    // A solve that forgets the border comes out ~50*cs short.
    EXPECT_GE(tp.H, (2.0f * kTextInset + 12.0f) * cs) << "cs=" << cs;
}

// The paint side reads the padding the SOLVE resolved, so a percentage padding
// and the pixel length it resolves to are indistinguishable downstream.
//
// Chrome (headless, device scale 1 and 2 — identical), containing block 400px:
//     width: 200px; padding: 10%; border: 25px      -> 200x149, text at (65,65)
//     width: 200px; padding: 40px; border: 25px     -> 200x149, text at (65,65)
// with computed padding 40px on all four edges in both arms (percentages
// resolve against the containing block's WIDTH, including the vertical edges).
//
// The px arm is the control: it holds the font, the border and the box size
// fixed, so a difference between the two arms can only be the percentage's
// resolution. Reading the style input instead put the pct arm's ink 40*cs to
// the left of the px arm's.
namespace
{

constexpr char kPctXml[] = R"(<uielement id="root">
  <label id="pctbox" text="Hello"/>
  <label id="pxbox" text="Hello"/>
</uielement>)";

constexpr char kPctCss[] = R"(
#root   { display: flex; flex-direction: column; width: 400px; height: 300px; }
#pctbox { width: 200px; padding: 10%; border: 25px solid #ff0000;
          background-color: #00ff00; font-family: Roboto; font-size: 16px; }
#pxbox  { width: 200px; padding: 40px; border: 25px solid #ff0000;
          background-color: #00ff00; font-family: Roboto; font-size: 16px; }
)";

// Chrome, logical px: 10% of the 400px containing block.
constexpr float kPctResolvedPadding = 40.0f;
constexpr float kPctTextInset = 65.0f; // border 25 + padding 40

struct InkOrigin { float Left, Top; };

InkOrigin GlyphInkOrigin(const std::vector<UIPrimitive>& glyphs)
{
    InkOrigin ink{glyphs[0].X, glyphs[0].Y};
    for (const UIPrimitive& g : glyphs)
    {
        ink.Left = std::min(ink.Left, g.X);
        ink.Top = std::min(ink.Top, g.Y);
    }
    return ink;
}

} // namespace

TEST_P(BorderPaintPrimitives, PercentPaddingPaintsLikeTheLengthItResolvesTo)
{
    const float cs = GetParam();

    IsolatedUIFixture fx;
    if (!fx.Build(cs, kPctXml, kPctCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const auto pctGlyphs = fx.Primitives("pctbox", PrimitiveMode::Slug);
    const auto pxGlyphs = fx.Primitives("pxbox", PrimitiveMode::Slug);
    if (pctGlyphs.empty() || pxGlyphs.empty())
        GTEST_SKIP() << "no font atlas resolved (resolved family: '"
                     << fx.ResolvedFontFamily("pctbox") << "')";

    // Layout first: both boxes are 200 wide and the solve inset the children by
    // 40 on every edge. Without this the ink comparison below could agree by
    // both arms being wrong in the same way.
    const PhysicalRect pctBB = fx.BorderBox("pctbox");
    const PhysicalRect pxBB = fx.BorderBox("pxbox");
    EXPECT_FLOAT_EQ(pctBB.W, 200.0f * cs);
    EXPECT_FLOAT_EQ(pctBB.H, pxBB.H);

    const GameEngine::UIElement* pctEl = fx.Element("pctbox");
    ASSERT_NE(pctEl, nullptr);
    EXPECT_FLOAT_EQ(pctEl->GetLayoutPadding().Left, kPctResolvedPadding);
    EXPECT_FLOAT_EQ(pctEl->GetLayoutPadding().Top, kPctResolvedPadding);
    EXPECT_FLOAT_EQ(pctEl->GetLayoutPadding().Right, kPctResolvedPadding);
    EXPECT_FLOAT_EQ(pctEl->GetLayoutPadding().Bottom, kPctResolvedPadding);
    // The style side still carries the raw percentage — that is the input the
    // paint side must NOT be reading.
    const GameEngine::ResolvedStyle* pctStyle = fx.Style("pctbox");
    ASSERT_NE(pctStyle, nullptr);
    EXPECT_TRUE(pctStyle->Layout.PaddingIsPercent.Left);
    EXPECT_FLOAT_EQ(pctStyle->Layout.Padding.Left, 10.0f);

    // Ink: same offset from each box's own border box, and at Chrome's 65.
    const InkOrigin pctInk = GlyphInkOrigin(pctGlyphs);
    const InkOrigin pxInk = GlyphInkOrigin(pxGlyphs);
    EXPECT_NEAR(pctInk.Left - pctBB.X, pxInk.Left - pxBB.X, 0.01f) << "cs=" << cs;
    EXPECT_NEAR(pctInk.Top - pctBB.Y, pxInk.Top - pxBB.Y, 0.01f) << "cs=" << cs;
    EXPECT_NEAR(pctInk.Left, pctBB.X + kPctTextInset * cs, kGlyphSlackPx * cs) << "cs=" << cs;
    EXPECT_GE(pctInk.Top, pctBB.Y + (kPctTextInset - kGlyphSlackPx) * cs) << "cs=" << cs;
    EXPECT_LE(pctInk.Top, pctBB.Y + (kPctTextInset + kFirstLineInkDepthPx) * cs) << "cs=" << cs;
}
