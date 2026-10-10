// `margin: auto` is the flexbox idiom for pushing an item to the far edge and
// for centring a fixed-size box. It is not a length: an auto margin absorbs the
// line's remaining free space, and does it *before* justify-content gets to
// distribute anything (CSS Flexbox 1 §9.5 step 1 — if any auto margin exists on
// the main axis, justify-content is ignored on that line).
//
// Every expected number below was measured against real Chrome
// (chrome.exe --headless=new --force-device-scale-factor=N, box-sizing:
// border-box) at BOTH device scales, because Chrome quantises used lengths onto
// a 1/64 DEVICE-pixel grid: the dpr-2 answer is not the dpr-1 answer doubled.
// The f9 three-way split is the fixture that proves it — 33.328125 logical at
// dpr 1 (2133/64) against 33.3359375 at dpr 2 (4267/128), a full grid step
// apart.
//
// Two fixtures are controls, and both already agree with the broken engine:
// f5 (flex-grow consumes the free space first, so the auto margin resolves to
// 0) and f8 (negative free space, where an auto margin cannot go negative).
// They are what keeps the rest honest — every other expectation would also be
// met by an engine that simply handed free space to the item unconditionally.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"

#include "../Source/StyleInterpolation.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <string>
#include <variant>

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// Every container is a 400x100 flex box. `flex-shrink: 0` on the containers is
// not part of any fixture: the root is a column flex container and the stack of
// containers overflows it, so without it Yoga would shrink their heights and
// the cross-axis fixtures would be measuring a container Chrome never had.
constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="f1"><uielement id="f1a"/></uielement>
  <uielement id="f2"><uielement id="f2a"/></uielement>
  <uielement id="f3"><uielement id="f3a"/></uielement>
  <uielement id="f4"><uielement id="f4a"/><uielement id="f4b"/></uielement>
  <uielement id="f5"><uielement id="f5a"/><uielement id="f5b"/></uielement>
  <uielement id="f6"><uielement id="f6a"/></uielement>
  <uielement id="f7"><uielement id="f7a"/><uielement id="f7b"/></uielement>
  <uielement id="f8"><uielement id="f8a"/></uielement>
  <uielement id="f9"><uielement id="f9a"/><uielement id="f9b"/><uielement id="f9c"/></uielement>
  <uielement id="f10"><uielement id="f10a"/></uielement>
  <uielement id="f11"><uielement id="f11a"/></uielement>
  <uielement id="f12"><uielement id="f12a"/></uielement>
  <uielement id="f13"><uielement id="f13a"/></uielement>
  <uielement id="f14"><uielement id="f14a"/></uielement>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; }
#f1, #f2, #f3, #f4, #f5, #f6, #f7, #f8, #f9, #f10, #f11, #f12, #f13, #f14 {
  display: flex; width: 400px; height: 100px; flex-shrink: 0;
}

/* margin-left: auto pushes the item to the far edge */
#f1a { width: 100px; height: 50px; margin-left: auto; }

/* the `margin: 0 auto` centring idiom, via the shorthand */
#f2a { width: 100px; height: 50px; margin: 0 auto; }

/* cross-axis auto margins centre vertically inside a row */
#f3a { width: 100px; height: 60px; margin-top: auto; margin-bottom: auto; }

/* one auto margin each: the free space splits evenly between them */
#f4a { width: 100px; height: 50px; margin-left: auto; }
#f4b { width: 100px; height: 50px; margin-left: auto; }

/* CONTROL: flex-grow eats the free space first, so the auto margin gets 0 */
#f5a { width: 100px; height: 50px; margin-left: auto; }
#f5b { width: 100px; height: 50px; flex-grow: 1; }

/* an auto margin suppresses justify-content on the main axis */
#f6 { justify-content: center; }
#f6a { width: 100px; height: 50px; margin-left: auto; }

/* ...including space-between. The auto margin has to be on the LEADING edge to
   discriminate: `margin-right: auto` on the first of two items puts the pair
   exactly where space-between already would, so that arrangement is at parity
   with the broken engine too and proves nothing. */
#f7 { justify-content: space-between; }
#f7a { width: 100px; height: 50px; margin-left: auto; }
#f7b { width: 100px; height: 50px; }

/* negative free space: the auto margin resolves to 0, it never goes negative */
#f8 { width: 200px; }
#f8a { width: 300px; height: 50px; flex-shrink: 0; margin-left: auto; }

/* three-way split — the fixture that lands off the integer grid */
#f9a { width: 100px; height: 50px; margin-left: auto; }
#f9b { width: 100px; height: 50px; margin-left: auto; }
#f9c { width: 100px; height: 50px; margin-left: auto; }

/* column container: left/right auto margins centre on the CROSS axis */
#f10 { flex-direction: column; }
#f10a { width: 100px; height: 50px; margin-left: auto; margin-right: auto; }

/* column container: margin-top auto pushes down the MAIN axis */
#f11 { flex-direction: column; }
#f11a { width: 100px; height: 50px; margin-top: auto; }

/* auto on one side, a fixed length on the other */
#f12a { width: 100px; height: 50px; margin-left: auto; margin-right: 30px; }

/* a cross-axis auto margin overrides the container's align-items */
#f13 { align-items: flex-start; }
#f13a { width: 100px; height: 60px; margin-top: auto; margin-bottom: auto; }

/* ...and defeats the default stretch, so an auto-height item collapses to 0
   and its two auto margins split the whole 100px between them */
#f14a { width: 100px; margin-top: auto; margin-bottom: auto; }
)";

// Chrome, --force-device-scale-factor=1, offsets relative to the container's
// border box, in LOGICAL px (== device px at this scale).
struct ChromeOffset
{
    const char* Container;
    const char* Item;
    float X;
    float Y;
    float W;
    float H;
};

constexpr ChromeOffset kChromeScale1[] = {
    {"f1", "f1a", 300.0f, 0.0f, 100.0f, 50.0f},
    {"f2", "f2a", 150.0f, 0.0f, 100.0f, 50.0f},
    {"f3", "f3a", 0.0f, 20.0f, 100.0f, 60.0f},
    {"f4", "f4a", 100.0f, 0.0f, 100.0f, 50.0f},
    {"f4", "f4b", 300.0f, 0.0f, 100.0f, 50.0f},
    {"f5", "f5a", 0.0f, 0.0f, 100.0f, 50.0f},
    {"f5", "f5b", 100.0f, 0.0f, 300.0f, 50.0f},
    {"f6", "f6a", 300.0f, 0.0f, 100.0f, 50.0f},
    {"f7", "f7a", 200.0f, 0.0f, 100.0f, 50.0f},
    {"f7", "f7b", 300.0f, 0.0f, 100.0f, 50.0f},
    {"f8", "f8a", 0.0f, 0.0f, 300.0f, 50.0f},
    {"f9", "f9a", 33.328125f, 0.0f, 100.0f, 50.0f},
    {"f9", "f9b", 166.671875f, 0.0f, 100.0f, 50.0f},
    {"f9", "f9c", 300.0f, 0.0f, 100.0f, 50.0f},
    {"f10", "f10a", 150.0f, 0.0f, 100.0f, 50.0f},
    {"f11", "f11a", 0.0f, 50.0f, 100.0f, 50.0f},
    {"f12", "f12a", 270.0f, 0.0f, 100.0f, 50.0f},
    {"f13", "f13a", 0.0f, 20.0f, 100.0f, 60.0f},
    {"f14", "f14a", 0.0f, 50.0f, 100.0f, 0.0f},
};

// Chrome, --force-device-scale-factor=2, same offsets in DEVICE px. Identical
// to scale-1 doubled everywhere except the three-way split, where the finer
// grid lands a step away: 66.671875 device px, not 2 * 33.328125 = 66.65625.
constexpr ChromeOffset kChromeScale2[] = {
    {"f1", "f1a", 600.0f, 0.0f, 200.0f, 100.0f},
    {"f2", "f2a", 300.0f, 0.0f, 200.0f, 100.0f},
    {"f3", "f3a", 0.0f, 40.0f, 200.0f, 120.0f},
    {"f4", "f4a", 200.0f, 0.0f, 200.0f, 100.0f},
    {"f4", "f4b", 600.0f, 0.0f, 200.0f, 100.0f},
    {"f5", "f5a", 0.0f, 0.0f, 200.0f, 100.0f},
    {"f5", "f5b", 200.0f, 0.0f, 600.0f, 100.0f},
    {"f6", "f6a", 600.0f, 0.0f, 200.0f, 100.0f},
    {"f7", "f7a", 400.0f, 0.0f, 200.0f, 100.0f},
    {"f7", "f7b", 600.0f, 0.0f, 200.0f, 100.0f},
    {"f8", "f8a", 0.0f, 0.0f, 600.0f, 100.0f},
    {"f9", "f9a", 66.671875f, 0.0f, 200.0f, 100.0f},
    {"f9", "f9b", 333.328125f, 0.0f, 200.0f, 100.0f},
    {"f9", "f9c", 600.0f, 0.0f, 200.0f, 100.0f},
    {"f10", "f10a", 300.0f, 0.0f, 200.0f, 100.0f},
    {"f11", "f11a", 0.0f, 100.0f, 200.0f, 100.0f},
    {"f12", "f12a", 540.0f, 0.0f, 200.0f, 100.0f},
    {"f13", "f13a", 0.0f, 40.0f, 200.0f, 120.0f},
    {"f14", "f14a", 0.0f, 100.0f, 200.0f, 0.0f},
};

void ExpectChromeParity(float contentScale, const ChromeOffset* expected, size_t count)
{
    IsolatedUIFixture fx;
    if (!fx.Build(contentScale, kXml, kCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    for (size_t i = 0; i < count; ++i)
    {
        const ChromeOffset& e = expected[i];
        SCOPED_TRACE(testing::Message() << e.Item << " at contentScale " << contentScale);

        ASSERT_NE(fx.Element(e.Container), nullptr);
        ASSERT_NE(fx.Element(e.Item), nullptr);

        const PhysicalRect container = fx.BorderBox(e.Container);
        const PhysicalRect item = fx.BorderBox(e.Item);

        EXPECT_FLOAT_EQ(item.X - container.X, e.X);
        EXPECT_FLOAT_EQ(item.Y - container.Y, e.Y);
        EXPECT_FLOAT_EQ(item.W, e.W);
        EXPECT_FLOAT_EQ(item.H, e.H);
    }
}

} // namespace

TEST(MarginAutoChromeParity, AutoMarginsMatchChromeAtScale1)
{
    ExpectChromeParity(1.0f, kChromeScale1, std::size(kChromeScale1));
}

TEST(MarginAutoChromeParity, AutoMarginsMatchChromeAtScale2)
{
    ExpectChromeParity(2.0f, kChromeScale2, std::size(kChromeScale2));
}

// ---------------------------------------------------------------------------
// A KNOWN DIVERGENCE, pinned so it is visible and so it fails loudly the day it
// stops being true.
//
// CSS gives an absolutely positioned box auto margins too: with both insets
// pinned and a definite size, the leftover between them is what the auto
// margins divide. Chrome, measured on the same fixtures:
//     left:0 right:0 + margin: 0 auto      -> x = 150   (engine: 0)
//     left:0 right:0 + margin-left: auto   -> x = 300   (engine: 0)
//     top:0 bottom:0 + auto top and bottom -> y = 20    (engine: 0)
//
// The cause is Yoga (3.2.1), not this engine's push. Driving YGNodeStyleSet-
// MarginAuto directly on a raw absolute node produces the same 0, the node's
// margin unit reads back as YGUnitAuto, and an identically-configured child in
// the flex line does land at 300 — so the value reaches Yoga and Yoga's
// absolute layout is what ignores it. Fixing it means a Yoga change, which is
// out of scope for the flex-context defect this file exists for.
// ---------------------------------------------------------------------------

namespace
{

constexpr char kAbsoluteXml[] = R"(<uielement id="root">
  <uielement id="box"><uielement id="item"/></uielement>
</uielement>)";

constexpr char kAbsoluteCss[] = R"(
#root { display: flex; flex-direction: column; }
#box  { display: flex; width: 400px; height: 100px; flex-shrink: 0; }
#item { position: absolute; left: 0; right: 0; top: 0;
        width: 100px; height: 50px; margin: 0 auto; }
)";

} // namespace

TEST(MarginAutoKnownGaps, AbsolutePositionedAutoMarginsAreIgnoredByYoga)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kAbsoluteXml, kAbsoluteCss))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    // The cascade does its job — the flag is present on both edges.
    const GameEngine::ResolvedStyle* style = fx.Style("item");
    ASSERT_NE(style, nullptr);
    EXPECT_TRUE(style->Layout.MarginIsAuto.Left);
    EXPECT_TRUE(style->Layout.MarginIsAuto.Right);

    // Chrome puts this box at x = 150. Yoga leaves it on the leading inset.
    const PhysicalRect box = fx.BorderBox("box");
    const PhysicalRect item = fx.BorderBox("item");
    EXPECT_FLOAT_EQ(item.X - box.X, 0.0f);
}

// ---------------------------------------------------------------------------
// The cascade has to carry the Auto unit, not just the zero it evaluates to.
// `auto` and `0` produce the same Margin float; only the flag separates them,
// so a layout assertion alone would not say which layer dropped it.
// ---------------------------------------------------------------------------

namespace
{

GameEngine::ResolvedStyle ComputeStyle(const std::string& css, const std::string& id)
{
    GameEngine::Stylesheet sheet{};
    EXPECT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, sheet));
    GameEngine::UIParsing::ElementState state{};
    GameEngine::UIElement el;
    el.SetId(id);
    return GameEngine::UIParsing::CSSParser::ComputeStyleFor(el, sheet, state);
}

} // namespace

TEST(MarginAutoCascade, LonghandAutoReachesLayoutInputs)
{
    const auto s = ComputeStyle("#a { margin-left: auto; margin-top: 4px; }", "a");

    EXPECT_TRUE(s.Layout.MarginIsAuto.Left);
    EXPECT_FALSE(s.Layout.MarginIsPercent.Left);
    EXPECT_FALSE(s.Layout.MarginIsAuto.Top);
    EXPECT_FLOAT_EQ(s.Layout.Margin.Top, 4.0f);
    // The other two edges were never authored and must stay plain zeros.
    EXPECT_FALSE(s.Layout.MarginIsAuto.Right);
    EXPECT_FALSE(s.Layout.MarginIsAuto.Bottom);
}

TEST(MarginAutoCascade, ShorthandCarriesAutoPerEdge)
{
    const auto s = ComputeStyle("#a { margin: 0 auto; }", "a");

    EXPECT_FALSE(s.Layout.MarginIsAuto.Top);
    EXPECT_FALSE(s.Layout.MarginIsAuto.Bottom);
    EXPECT_TRUE(s.Layout.MarginIsAuto.Left);
    EXPECT_TRUE(s.Layout.MarginIsAuto.Right);
}

TEST(MarginAutoCascade, LaterDeclarationClearsAuto)
{
    // The flag is per-edge state that survives across declarations, so the
    // overriding length has to clear it or the element keeps an auto margin
    // nobody authored.
    const auto s = ComputeStyle("#a { margin-left: auto; } #a { margin-left: 12px; }", "a");

    EXPECT_FALSE(s.Layout.MarginIsAuto.Left);
    EXPECT_FLOAT_EQ(s.Layout.Margin.Left, 12.0f);
}

TEST(MarginAutoCascade, UnparseableMarginIsNotAnAutoMargin)
{
    // The generic length parser answers Auto for anything it cannot read, so
    // margin needs its own: a typo must not silently become a layout-shifting
    // auto margin.
    const auto s = ComputeStyle("#a { margin-left: bogus; }", "a");

    EXPECT_FALSE(s.Layout.MarginIsAuto.Left);
    EXPECT_FLOAT_EQ(s.Layout.Margin.Left, 0.0f);
}

TEST(MarginAutoCascade, PaddingAutoStaysZero)
{
    // `padding: auto` is not valid CSS. It must not acquire auto behaviour by
    // sharing the box parser with margin.
    const auto s = ComputeStyle("#a { padding: auto; padding-left: auto; }", "a");

    EXPECT_FLOAT_EQ(s.Layout.Padding.Top, 0.0f);
    EXPECT_FLOAT_EQ(s.Layout.Padding.Left, 0.0f);
    EXPECT_FALSE(s.Layout.PaddingIsPercent.Left);
}

TEST(MarginAutoCascade, LayoutInputsEqualitySeesTheAutoFlag)
{
    // LayoutInputs::operator== gates the Yoga re-push. Two inputs that differ
    // only in whether an edge is auto must compare unequal, or a transition
    // into or out of `margin: auto` leaves the previous solve on screen.
    GameEngine::LayoutInputs plain{};
    GameEngine::LayoutInputs autoLeft{};
    autoLeft.MarginIsAuto.Left = true;

    EXPECT_FALSE(plain == autoLeft);
}

// ---------------------------------------------------------------------------
// Transitions. CSS says `auto` is not an interpolable margin value: a
// transition with an auto endpoint is discrete, flipping at the halfway point
// rather than sliding. That is what the unit-mismatch branch of
// InterpolateProperty already does; these pin it for margins specifically and
// pin that the write-back carries the flag.
// ---------------------------------------------------------------------------

TEST(MarginAutoTransition, AutoEndpointFlipsDiscretely)
{
    const GameEngine::StyleValue px = GameEngine::StyleLength::Px(20.0f);
    const GameEngine::StyleValue autoLen = GameEngine::StyleLength::Auto();

    const auto early = GameEngine::InterpolateProperty(
        GameEngine::StylePropertyId::MarginLeft, px, autoLen, 0.25f);
    const auto late = GameEngine::InterpolateProperty(
        GameEngine::StylePropertyId::MarginLeft, px, autoLen, 0.75f);

    EXPECT_TRUE(std::get<GameEngine::StyleLength>(early).IsPx());
    EXPECT_FLOAT_EQ(std::get<GameEngine::StyleLength>(early).Value, 20.0f);
    EXPECT_TRUE(std::get<GameEngine::StyleLength>(late).IsAuto());
}

TEST(MarginAutoTransition, ReadAndWriteRoundTripTheAutoFlag)
{
    GameEngine::ResolvedStyle rs{};
    rs.Layout.MarginIsAuto.Left = true;

    const auto read = GameEngine::ReadProperty(rs, GameEngine::StylePropertyId::MarginLeft);
    EXPECT_TRUE(std::get<GameEngine::StyleLength>(read).IsAuto());

    // Writing a length back has to clear the flag, otherwise the tail of a
    // transition out of `auto` never stops being an auto margin.
    GameEngine::WriteProperty(rs, GameEngine::StylePropertyId::MarginLeft,
                              GameEngine::StyleLength::Px(7.0f));
    EXPECT_FALSE(rs.Layout.MarginIsAuto.Left);
    EXPECT_FLOAT_EQ(rs.Layout.Margin.Left, 7.0f);

    GameEngine::WriteProperty(rs, GameEngine::StylePropertyId::MarginLeft,
                              GameEngine::StyleLength::Auto());
    EXPECT_TRUE(rs.Layout.MarginIsAuto.Left);
}
