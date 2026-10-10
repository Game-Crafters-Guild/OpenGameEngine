// `align-items: baseline` must align GLYPH baselines, not box bottoms.
//
// WHY A ROW OF EQUAL-SIZED LABELS PROVES NOTHING: with no baseline function on
// a node, Yoga reports its measured HEIGHT as its baseline
// (yoga/algorithm/Baseline.cpp). Aligning on that aligns bottom edges — which
// is indistinguishable from aligning baselines exactly when every item on the
// line has the same font size and line-height, i.e. in every row a
// same-size test would build. It is also indistinguishable from `flex-end` in
// a row with no vertical slack. Every case below therefore mixes font sizes,
// line-heights or line counts, which is the only shape that separates the two.
//
// WHAT IS READ BACK: the emitted glyph primitive, inverted to the drawn
// baseline exactly as BaselineSnapTests does (see that file's header for the
// derivation) — not the Yoga node's top edge. Layout can only pass here by
// agreeing with where the renderer actually puts the ink.
//
// REFERENCE NUMBERS: Chrome 141 rendering the same staged Roboto-Regular.ttf,
// launched with --force-device-scale-factor (never Playwright's emulated
// deviceScaleFactor, which reports a dpr the layout did not use). Roboto
// 2.001047 reports ascent 2146 / descent 555 / lineGap 0 over a 2048 em, and
// Blink rounds each term to whole DEVICE pixels and sums them, so at scale 1:
//
//     32px   ascent 34   descent  9   height 43
//     12px   ascent 13   descent  3   height 16
//
// Every expectation below is one of these, measured at dpr 1 with a zero-size
// `vertical-align: baseline` probe inside each item (its top IS the baseline).
// Offsets are relative to the row's top:
//
//   row                          row.H  big.top  big.H  small.top  baselines
//   mixed sizes, baseline           43        0     43         21   34 / 34
//   mixed sizes, flex-end           43        0     43         27   34 / 40
//   big is two lines                86        0     86         21   34 / 34
//   big has line-height: 63px       63        0     63         31   44 / 44
//   small has padding-top: 6px      43        0     43         15   34 / 34
//   big wrapped, wrapper pad 5px    48        5     43         26   39 / 39
//
// The flex-end row is the tell: 27 and a 40px baseline are exactly what this
// engine produced for `baseline` before a baseline function existed. Aligning
// box bottoms IS flex-end, and it puts the small item's glyphs 6px low.

#include "IsolatedUIFixture.h"

#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// Roboto at content scale 1, from the tables above. Chrome's own numbers for
// the same face and sizes; the engine derives them independently.
constexpr float kBigAscentPx = 34.0f;
constexpr float kBigHeightPx = 43.0f;
constexpr float kSmallAscentPx = 13.0f;
constexpr float kSmallHeightPx = 16.0f;

// Glyph outlines land on the baseline exactly; the tolerance covers float
// accumulation through the cascade and the Yoga solve, nothing more.
constexpr float kExactPx = 0.01f;

constexpr char kTwoLabelXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="big">Hxg</label>
    <label id="small">Hxg</label>
  </uielement>
</uielement>)";

constexpr char kTwoLabelCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 32px; color: #ffffff; }
#small { font-family: Roboto; font-size: 12px; color: #ffffff; }
)";

// Same row, aligned on the container's default instead. The two land in
// different places only once baselines are real, so this is the control that
// makes the headline assertion falsifiable rather than a restatement of the
// layout the engine happens to produce.
constexpr char kFlexEndCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: flex-end; }
#big { font-family: Roboto; font-size: 32px; color: #ffffff; }
#small { font-family: Roboto; font-size: 12px; color: #ffffff; }
)";

// A two-line first item. CSS aligns on the FIRST line's baseline; `last
// baseline` is a separate keyword this engine does not support, and taking the
// last line here would drop the small label a whole line box.
constexpr char kMultiLineXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="big">Hxg
Hxg</label>
    <label id="small">Hxg</label>
  </uielement>
</uielement>)";

// Line-height widens the big item's line box symmetrically, so half-leading
// pushes its first baseline down by exactly half the extra height.
constexpr char kLineHeightCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 32px; line-height: 63px; color: #ffffff; }
#small { font-family: Roboto; font-size: 12px; color: #ffffff; }
)";

// Baseline recovered from one emitted Slug primitive: MakeSlugGlyph stores the
// undilated em-space bounds in UvRect and H spans them, so inverting the
// quad-top formula gives the drawn baseline. Pure readback of emitted
// geometry — see BaselineSnapTests.cpp for the full derivation.
float RecoveredBaseline(const GameEngine::UI::UIPrimitive& p)
{
    const float emSpan = p.UvRect[1] - p.UvRect[3];
    const float emScale = p.H / emSpan;
    return p.Y + p.UvRect[1] * emScale;
}

bool HasUsableEmSpan(const GameEngine::UI::UIPrimitive& p)
{
    return (p.UvRect[1] - p.UvRect[3]) > 1e-4f && p.H > 1e-4f;
}

// Baseline of the element's TOP line of glyphs, in absolute physical px.
// Multi-line labels emit every line, so the smallest baseline is the first
// one — the value CSS aligns on.
::testing::AssertionResult FirstBaseline(const IsolatedUIFixture& fx, const std::string& id,
                                         float& out)
{
    bool found = false;
    for (const auto& p : fx.Primitives(id, GameEngine::UI::PrimitiveMode::Slug))
    {
        if (!HasUsableEmSpan(p))
            continue;
        const float baseline = RecoveredBaseline(p);
        if (!found || baseline < out)
            out = baseline;
        found = true;
    }
    if (!found)
        return ::testing::AssertionFailure() << "no glyph with a usable em span for '" << id << "'";
    return ::testing::AssertionSuccess();
}

// Builds the fixture and fails the calling test unless every named label
// resolved to the Roboto the reference numbers were measured against.
::testing::AssertionResult BuildRow(IsolatedUIFixture& fx, float scale, const char* xml,
                                    const char* css, std::initializer_list<const char*> ids)
{
    const bool built = fx.Build(scale, xml, css);
    if (!fx.DeviceAvailable())
        return ::testing::AssertionFailure() << "skip:no device";
    if (!built)
        return ::testing::AssertionFailure() << fx.Diagnostic();
    for (const char* id : ids)
    {
        const std::string family = fx.ResolvedFontFamily(id);
        if (family.find("Roboto") == std::string::npos)
            return ::testing::AssertionFailure()
                   << id << " resolved to '" << family << "', not Roboto";
    }
    return ::testing::AssertionSuccess();
}

bool SkippedForDevice(const ::testing::AssertionResult& r)
{
    return !r && std::string(r.message()).find("skip:no device") != std::string::npos;
}

} // namespace

// THE headline property. Two items whose font sizes differ by 20px must draw
// their glyphs on ONE baseline. Without a baseline function the engine aligns
// the box bottoms and these land 6 physical px apart.
TEST(TextBaselineAlignment, GlyphBaselinesCoincideAcrossFontSizes)
{
    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kTwoLabelXml, kTwoLabelCss, {"big", "small"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    float bigBaseline = 0.0f;
    float smallBaseline = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "big", bigBaseline));
    ASSERT_TRUE(FirstBaseline(fx, "small", smallBaseline));

    EXPECT_NEAR(bigBaseline, smallBaseline, kExactPx);

    // ...and the absolute position matches Chrome, so "equal" cannot be
    // satisfied by both being equally wrong.
    const PhysicalRect row = fx.BorderBox("row");
    EXPECT_NEAR(bigBaseline - row.Y, kBigAscentPx, kExactPx);
    EXPECT_NEAR(smallBaseline - row.Y, kBigAscentPx, kExactPx);

    // Chrome puts the small item's border box 21px below the row top: the
    // difference of the two ascents, not of the two heights (27px).
    const PhysicalRect small = fx.BorderBox("small");
    EXPECT_NEAR(small.Y - row.Y, kBigAscentPx - kSmallAscentPx, kExactPx);
    EXPECT_NEAR(small.H, kSmallHeightPx, kExactPx);
    EXPECT_NEAR(fx.BorderBox("big").H, kBigHeightPx, kExactPx);

    // The row is as tall as ascent + the deepest descent below it, which for
    // these two is the big item's own box.
    EXPECT_NEAR(row.H, kBigHeightPx, kExactPx);
}

// The discriminator the box-bottom behaviour cannot pass: aligning bottoms IS
// `flex-end` in a row with no slack, so if `baseline` still meant "bottom"
// these two layouts would be identical.
TEST(TextBaselineAlignment, BaselineAndFlexEndDisagreeForMixedFontSizes)
{
    IsolatedUIFixture baselineFx;
    const auto baselineReady = BuildRow(baselineFx, 1.0f, kTwoLabelXml, kTwoLabelCss, {"big", "small"});
    if (SkippedForDevice(baselineReady))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(baselineReady);

    IsolatedUIFixture flexEndFx;
    ASSERT_TRUE(BuildRow(flexEndFx, 1.0f, kTwoLabelXml, kFlexEndCss, {"big", "small"}));

    const float baselineTop = baselineFx.BorderBox("small").Y - baselineFx.BorderBox("row").Y;
    const float flexEndTop = flexEndFx.BorderBox("small").Y - flexEndFx.BorderBox("row").Y;

    EXPECT_NEAR(baselineTop, kBigAscentPx - kSmallAscentPx, kExactPx);
    EXPECT_NEAR(flexEndTop, kBigHeightPx - kSmallHeightPx, kExactPx);
    EXPECT_GT(flexEndTop - baselineTop, 1.0f);
}

// Yoga and the font atlas work in physical px while CSS and layout work in
// logical px, and the baseline crosses that boundary twice. A scale that is not
// 1 is the only case that can catch a missing conversion.
//
// The small item's top is pinned to Chrome's own position, unrounded. Yoga
// rounds layout onto the grid YogaAdapter::SetPointScaleFactor sets, and that
// grid is the DEVICE pixel, so the position Chrome expresses as 20.8 logical px
// at content scale 1.25 is 26 whole device px on both sides and survives the
// round intact.
//
// Aligning box bottoms instead is out by the difference of the two descents —
// 5.6 logical px here, several times any rounding residue.
TEST(TextBaselineAlignment, BaselinesCoincideAtEveryContentScale)
{
    // Chrome's small-item top for this row, in logical px, at each device
    // scale factor (--force-device-scale-factor, not an emulated dpr).
    struct ScaleCase { float Scale; float ChromeSmallTopLogical; };
    const ScaleCase cases[] = {{1.25f, 20.8f}, {1.5f, 20.6666667f}, {2.0f, 21.0f}};

    for (const auto& c : cases)
    {
        IsolatedUIFixture fx;
        const auto ready = BuildRow(fx, c.Scale, kTwoLabelXml, kTwoLabelCss, {"big", "small"});
        if (SkippedForDevice(ready))
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(ready) << "scale " << c.Scale;

        float bigBaseline = 0.0f;
        float smallBaseline = 0.0f;
        ASSERT_TRUE(FirstBaseline(fx, "big", bigBaseline)) << "scale " << c.Scale;
        ASSERT_TRUE(FirstBaseline(fx, "small", smallBaseline)) << "scale " << c.Scale;

        const PhysicalRect row = fx.BorderBox("row");
        const PhysicalRect small = fx.BorderBox("small");
        // Yoga rounds onto Chrome's layout quantum of 1/64 device px and rounds
        // to nearest where Chrome floors, so one grid step is the floor on any
        // comparison against a Chrome coordinate.
        constexpr float kLayoutGridPx = 1.0f / 64.0f + 1.0e-4f;
        EXPECT_NEAR(small.Y - row.Y, c.ChromeSmallTopLogical * c.Scale, kLayoutGridPx)
            << "scale " << c.Scale;

        // Baselines snap to whole device pixels independently, so the small one
        // is never below the big one and never a full logical pixel above it.
        const float gap = bigBaseline - smallBaseline;
        EXPECT_GE(gap, 0.0f) << "scale " << c.Scale;
        EXPECT_LT(gap, c.Scale) << "scale " << c.Scale << " (gap " << gap
                                << " physical px, big " << bigBaseline << ", small "
                                << smallBaseline << ")";
    }
}

// CSS `baseline` is the FIRST line's. A two-line item that reported its last
// line would push the single-line item a whole line box further down.
TEST(TextBaselineAlignment, MultiLineItemAlignsOnItsFirstLine)
{
    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kMultiLineXml, kTwoLabelCss, {"big", "small"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    ASSERT_NEAR(fx.BorderBox("big").H, kBigHeightPx * 2.0f, kExactPx) << "expected two lines";

    float bigBaseline = 0.0f;
    float smallBaseline = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "big", bigBaseline));
    ASSERT_TRUE(FirstBaseline(fx, "small", smallBaseline));

    EXPECT_NEAR(bigBaseline, smallBaseline, kExactPx);
    // A multi-line block is top-aligned in its box, so the first baseline is
    // the ascent — not the ascent plus a line box, which aligning on the last
    // line would give. Chrome: row 86, small.top 21, both first baselines 34 —
    // the single-line row's numbers, with only the row taller.
    const PhysicalRect row = fx.BorderBox("row");
    EXPECT_NEAR(row.H, kBigHeightPx * 2.0f, kExactPx);
    EXPECT_NEAR(bigBaseline - row.Y, kBigAscentPx, kExactPx);
    EXPECT_NEAR(fx.BorderBox("small").Y - row.Y, kBigAscentPx - kSmallAscentPx, kExactPx);
}

// Half-leading is part of the baseline, not just of the box. `line-height:
// 63px` over a 43px font box adds 10px above the ink and 10 below, so the
// first baseline moves down by 10 and the small item follows it.
TEST(TextBaselineAlignment, DeclaredLineHeightMovesTheBaselineByHalfTheLeading)
{
    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kTwoLabelXml, kLineHeightCss, {"big", "small"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    constexpr float kDeclaredLineBoxPx = 63.0f;
    const float halfLeading = (kDeclaredLineBoxPx - kBigHeightPx) * 0.5f;

    float bigBaseline = 0.0f;
    float smallBaseline = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "big", bigBaseline));
    ASSERT_TRUE(FirstBaseline(fx, "small", smallBaseline));

    // Chrome: row 63, big 63, small.top 31, both baselines 44.
    const PhysicalRect row = fx.BorderBox("row");
    EXPECT_NEAR(fx.BorderBox("big").H, kDeclaredLineBoxPx, kExactPx);
    EXPECT_NEAR(row.H, kDeclaredLineBoxPx, kExactPx);
    EXPECT_NEAR(bigBaseline - row.Y, halfLeading + kBigAscentPx, kExactPx);
    EXPECT_NEAR(smallBaseline, bigBaseline, kExactPx);
    EXPECT_NEAR(fx.BorderBox("small").Y - row.Y,
                halfLeading + kBigAscentPx - kSmallAscentPx, kExactPx);
}

// A box taller than its line box is where this engine and CSS genuinely
// disagree, and the disagreement is older than baseline alignment: emission
// CENTRES a single line in the content box (UIManager_PrimitiveGen), while
// Chrome puts the line box at the content-box top. Measured at dpr 1 for a
// 12px item given height: 30px, Chrome keeps its baseline 13px below its own
// top and so places the item at 21; the engine draws the glyphs 7px lower, at
// (30 - 16) / 2 + 13 = 20, and so places the item at 14.
//
// The baseline function follows the ENGINE, because a baseline that disagreed
// with where the glyphs are drawn would defeat the whole feature — the items
// would be aligned on a line no ink sits on. Matching Chrome here means
// changing emission to top-align, which would move every single-line label in
// the editor and is a separate decision. What must hold either way, and is
// what this test exists for, is that the baselines still coincide.
TEST(TextBaselineAlignment, AnOverTallTextBoxAlignsOnTheLineTheEngineDraws)
{
    constexpr char kTallCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 32px; color: #ffffff; }
#small { font-family: Roboto; font-size: 12px; height: 30px; color: #ffffff; }
)";

    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kTwoLabelXml, kTallCss, {"big", "small"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    constexpr float kDeclaredHeightPx = 30.0f;
    const float centredBaseline =
        (kDeclaredHeightPx - kSmallHeightPx) * 0.5f + kSmallAscentPx;

    float bigBaseline = 0.0f;
    float smallBaseline = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "big", bigBaseline));
    ASSERT_TRUE(FirstBaseline(fx, "small", smallBaseline));

    EXPECT_NEAR(bigBaseline, smallBaseline, kExactPx);

    const PhysicalRect row = fx.BorderBox("row");
    const PhysicalRect small = fx.BorderBox("small");
    EXPECT_NEAR(small.H, kDeclaredHeightPx, kExactPx);
    EXPECT_NEAR(bigBaseline - row.Y, kBigAscentPx, kExactPx);
    // 14, not Chrome's 21: the difference is the 7px of centring, and it is
    // exactly the amount the glyphs moved down inside the box.
    EXPECT_NEAR(small.Y - row.Y, kBigAscentPx - centredBaseline, kExactPx);
}

// A non-text element keeps Yoga's own rule — first in-flow child's baseline,
// offset by that child's position — which is what CSS asks of a block
// container with line boxes. Wrapping the big label in a plain element must
// therefore not change where anything lands.
TEST(TextBaselineAlignment, WrappingAnItemInAPlainElementKeepsItsBaseline)
{
    constexpr char kWrappedXml[] = R"(<uielement id="root">
  <uielement id="row">
    <uielement id="wrapper"><label id="big">Hxg</label></uielement>
    <label id="small">Hxg</label>
  </uielement>
</uielement>)";
    constexpr char kWrappedCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#wrapper { display: flex; flex-direction: column; padding-top: 5px; }
#big { font-family: Roboto; font-size: 32px; color: #ffffff; }
#small { font-family: Roboto; font-size: 12px; color: #ffffff; }
)";

    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kWrappedXml, kWrappedCss, {"big", "small"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    float bigBaseline = 0.0f;
    float smallBaseline = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "big", bigBaseline));
    ASSERT_TRUE(FirstBaseline(fx, "small", smallBaseline));

    EXPECT_NEAR(bigBaseline, smallBaseline, kExactPx);
    // The wrapper's padding is part of the distance to the label's baseline, so
    // the whole line drops by it. Chrome: row 48, big.top 5, small.top 26,
    // both baselines 39.
    const PhysicalRect row = fx.BorderBox("row");
    EXPECT_NEAR(row.H, kBigHeightPx + 5.0f, kExactPx);
    EXPECT_NEAR(bigBaseline - row.Y, kBigAscentPx + 5.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("small").Y - row.Y,
                kBigAscentPx + 5.0f - kSmallAscentPx, kExactPx);
}

// Padding on the text element itself is inside the distance from its top edge
// to its baseline. Getting this wrong is invisible in every unpadded row and
// wrong in every padded one.
TEST(TextBaselineAlignment, PaddingOnATextItemShiftsItsOwnBaseline)
{
    constexpr char kPaddedCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 32px; color: #ffffff; }
#small { font-family: Roboto; font-size: 12px; padding-top: 6px; color: #ffffff; }
)";

    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kTwoLabelXml, kPaddedCss, {"big", "small"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    float bigBaseline = 0.0f;
    float smallBaseline = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "big", bigBaseline));
    ASSERT_TRUE(FirstBaseline(fx, "small", smallBaseline));

    EXPECT_NEAR(bigBaseline, smallBaseline, kExactPx);
    // The padded item's top edge is 6px higher than the unpadded one would be,
    // because its baseline is 6px further inside it. Chrome: row 43,
    // small.top 15, small.H 22, both baselines 34.
    const PhysicalRect row = fx.BorderBox("row");
    const PhysicalRect small = fx.BorderBox("small");
    EXPECT_NEAR(row.H, kBigHeightPx, kExactPx);
    EXPECT_NEAR(small.Y - row.Y, kBigAscentPx - kSmallAscentPx - 6.0f, kExactPx);
    EXPECT_NEAR(small.H, kSmallHeightPx + 6.0f, kExactPx);
    EXPECT_NEAR(bigBaseline - row.Y, kBigAscentPx, kExactPx);
}

// --- wrapped rows ------------------------------------------------------------
//
// A baseline is resolved per FLEX LINE, not per container. Everything above
// this point uses a single-line row, where a per-line baseline and a
// whole-container one are the same number, so none of it can see the wrapped
// path — and four shipped rules pair `align-items: baseline` with
// `flex-wrap: wrap`.
//
// Every item carries an explicit width so the wrap POINT is arithmetic: 90px
// items in a 200px row give two per line. Deciding it by text measurement
// instead would let a font-metric difference change which items share a line,
// and the comparison would be between two different layouts.
//
// Chrome 150, --force-device-scale-factor=1, same staged Roboto, offsets from
// the row's top:
//
//   W1 mixed sizes 32/12 | 24/16   row 75   tops 0, 21, 43, 51   baselines 34, 34, 68, 68
//   W2 mixed line-heights          row 62   tops 5, 0, 32, 36    baselines 22, 22, 53, 53
//   W4 two-line item on line 2     row 102  tops 0, 21, 60, 43   baselines 34, 34, 77, 77
//   W5 same items, NOWRAP          row 43   tops 0, 21, 9, 17    baselines 34 throughout

constexpr char kFourLabelXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg</label>
    <label id="b">Hxg</label>
    <label id="c">Hxg</label>
    <label id="d">Hxg</label>
  </uielement>
</uielement>)";

constexpr char kWrapSizesCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; flex-wrap: wrap; width: 200px; align-items: baseline; }
#a, #b, #c, #d { width: 90px; }
#a { font-family: Roboto; font-size: 32px; color: #ffffff; }
#b { font-family: Roboto; font-size: 12px; color: #ffffff; }
#c { font-family: Roboto; font-size: 24px; color: #ffffff; }
#d { font-family: Roboto; font-size: 16px; color: #ffffff; }
)";

// The control that makes the wrapped assertions falsifiable: the same four
// items at the same widths, in a row that may not wrap. One flex line cannot
// tell a per-line baseline from a per-container one, so the wrapped case must
// differ from this one or it is not exercising the wrapped path at all.
constexpr char kNoWrapSizesCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; flex-wrap: nowrap; width: 200px; align-items: baseline; }
#a, #b, #c, #d { width: 90px; }
#a { font-family: Roboto; font-size: 32px; color: #ffffff; }
#b { font-family: Roboto; font-size: 12px; color: #ffffff; }
#c { font-family: Roboto; font-size: 24px; color: #ffffff; }
#d { font-family: Roboto; font-size: 16px; color: #ffffff; }
)";

// Roboto at scale 1 for the two sizes the wrapped cases add.
constexpr float kAscent24Px = 25.0f;
constexpr float kAscent16Px = 17.0f;

TEST(TextBaselineAlignment, EachFlexLineOfAWrappedRowGetsItsOwnBaseline)
{
    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kFourLabelXml, kWrapSizesCss, {"a", "b", "c", "d"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    float ba = 0.0f, bb = 0.0f, bc = 0.0f, bd = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "a", ba));
    ASSERT_TRUE(FirstBaseline(fx, "b", bb));
    ASSERT_TRUE(FirstBaseline(fx, "c", bc));
    ASSERT_TRUE(FirstBaseline(fx, "d", bd));

    const PhysicalRect row = fx.BorderBox("row");
    ASSERT_NEAR(row.H, 75.0f, kExactPx) << "the row did not wrap into two lines as intended";

    // Within a line the glyphs share a baseline...
    EXPECT_NEAR(ba, bb, kExactPx);
    EXPECT_NEAR(bc, bd, kExactPx);
    // ...and the second line's is a whole line lower, not the same one.
    EXPECT_GT(bc - ba, 1.0f);

    // Chrome's absolute numbers, so "equal" cannot be satisfied by both lines
    // being equally wrong.
    EXPECT_NEAR(ba - row.Y, kBigAscentPx, kExactPx);
    EXPECT_NEAR(bc - row.Y, 68.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("a").Y - row.Y, 0.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("b").Y - row.Y, kBigAscentPx - kSmallAscentPx, kExactPx);
    EXPECT_NEAR(fx.BorderBox("c").Y - row.Y, 43.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("d").Y - row.Y, 68.0f - kAscent16Px, kExactPx);
    // c is the tallest on line 2 and starts it, so line 2's baseline is its own
    // ascent below the line top — the per-line resolution stated as arithmetic.
    EXPECT_NEAR(bc - fx.BorderBox("c").Y, kAscent24Px, kExactPx);
}

TEST(TextBaselineAlignment, AWrappedRowAndANonWrappingRowOfTheSameItemsDisagree)
{
    IsolatedUIFixture wrapFx;
    const auto ready = BuildRow(wrapFx, 1.0f, kFourLabelXml, kWrapSizesCss, {"a", "b", "c", "d"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    IsolatedUIFixture flatFx;
    ASSERT_TRUE(BuildRow(flatFx, 1.0f, kFourLabelXml, kNoWrapSizesCss, {"a", "b", "c", "d"}));

    float wrapC = 0.0f, flatC = 0.0f, wrapA = 0.0f, flatA = 0.0f;
    ASSERT_TRUE(FirstBaseline(wrapFx, "c", wrapC));
    ASSERT_TRUE(FirstBaseline(flatFx, "c", flatC));
    ASSERT_TRUE(FirstBaseline(wrapFx, "a", wrapA));
    ASSERT_TRUE(FirstBaseline(flatFx, "a", flatA));

    const float wrapRowY = wrapFx.BorderBox("row").Y;
    const float flatRowY = flatFx.BorderBox("row").Y;

    // Unwrapped, all four sit on one baseline at the tallest item's ascent.
    EXPECT_NEAR(flatFx.BorderBox("row").H, kBigHeightPx, kExactPx);
    EXPECT_NEAR(flatA - flatRowY, kBigAscentPx, kExactPx);
    EXPECT_NEAR(flatC - flatRowY, kBigAscentPx, kExactPx);

    // Wrapped, c has moved to a line of its own and its baseline with it.
    EXPECT_NEAR(wrapA - wrapRowY, kBigAscentPx, kExactPx);
    EXPECT_NEAR(wrapC - wrapRowY, 68.0f, kExactPx);
    EXPECT_GT((wrapC - wrapRowY) - (flatC - flatRowY), 1.0f);
}

TEST(TextBaselineAlignment, AWrappedLineWithMixedLineHeightsAlignsWithinThatLine)
{
    constexpr char kWrapLineHeightCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; flex-wrap: wrap; width: 200px; align-items: baseline; }
#a, #b, #c, #d { width: 90px; }
#a { font-family: Roboto; font-size: 16px; line-height: normal; color: #ffffff; }
#b { font-family: Roboto; font-size: 16px; line-height: 2; color: #ffffff; }
#c { font-family: Roboto; font-size: 16px; line-height: 30px; color: #ffffff; }
#d { font-family: Roboto; font-size: 16px; line-height: normal; color: #ffffff; }
)";

    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kFourLabelXml, kWrapLineHeightCss, {"a", "b", "c", "d"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    float ba = 0.0f, bb = 0.0f, bc = 0.0f, bd = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "a", ba));
    ASSERT_TRUE(FirstBaseline(fx, "b", bb));
    ASSERT_TRUE(FirstBaseline(fx, "c", bc));
    ASSERT_TRUE(FirstBaseline(fx, "d", bd));

    const PhysicalRect row = fx.BorderBox("row");
    ASSERT_NEAR(row.H, 62.0f, kExactPx) << "the row did not wrap into two lines as intended";

    EXPECT_NEAR(ba, bb, kExactPx);
    EXPECT_NEAR(bc, bd, kExactPx);
    // `line-height: 2` on a 16px font is a 32px box over a 21px font, so its
    // half-leading is 5.5 and floors to 5 — the item drops 5, not 5.5, and the
    // `normal` item beside it drops the same whole pixel to meet it.
    EXPECT_NEAR(ba - row.Y, 22.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("a").Y - row.Y, 5.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("b").Y - row.Y, 0.0f, kExactPx);
    // Line 2: a 30px box over the same 21px font gives 4.5, floored to 4.
    EXPECT_NEAR(bc - row.Y, 53.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("c").Y - row.Y, 32.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("d").Y - row.Y, 36.0f, kExactPx);
}

TEST(TextBaselineAlignment, AMultiLineItemOnASecondFlexLineAlignsOnItsFirstBaseline)
{
    constexpr char kWrapMultiXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg</label>
    <label id="b">Hxg</label>
    <label id="c">Hxg
Hxg</label>
    <label id="d">Hxg</label>
  </uielement>
</uielement>)";
    constexpr char kWrapMultiCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; flex-wrap: wrap; width: 200px; align-items: baseline; }
#a, #b, #c, #d { width: 90px; }
#a { font-family: Roboto; font-size: 32px; color: #ffffff; }
#b { font-family: Roboto; font-size: 12px; color: #ffffff; }
#c { font-family: Roboto; font-size: 16px; color: #ffffff; }
#d { font-family: Roboto; font-size: 32px; color: #ffffff; }
)";

    IsolatedUIFixture fx;
    const auto ready = BuildRow(fx, 1.0f, kWrapMultiXml, kWrapMultiCss, {"a", "b", "c", "d"});
    if (SkippedForDevice(ready))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(ready);

    float bc = 0.0f, bd = 0.0f;
    ASSERT_TRUE(FirstBaseline(fx, "c", bc));
    ASSERT_TRUE(FirstBaseline(fx, "d", bd));

    const PhysicalRect row = fx.BorderBox("row");
    const PhysicalRect c = fx.BorderBox("c");
    ASSERT_NEAR(row.H, 102.0f, kExactPx) << "the row did not wrap as intended";
    ASSERT_NEAR(c.H, 42.0f, kExactPx) << "c should be two 16px lines";

    // c's FIRST line is what line 2 aligns on. Taking its last would put its
    // baseline a whole 21px line box lower and drag d down with it.
    EXPECT_NEAR(bc, bd, kExactPx);
    EXPECT_NEAR(bc - row.Y, 77.0f, kExactPx);
    EXPECT_NEAR(bc - c.Y, kAscent16Px, kExactPx);
    EXPECT_NEAR(c.Y - row.Y, 60.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("d").Y - row.Y, 43.0f, kExactPx);
}

// `white-space` has nothing to do with vertical placement in CSS, and Chrome
// puts these two on the same pixel. The engine shapes wrapping and
// non-wrapping text through DIFFERENT code — ShapeMultiline bakes the leading
// into its glyph offsets, ShapeText sits on the bare ascender and has it
// applied at emission — so a rounding added to one path and not the other
// shows up here and nowhere else in this file.
TEST(TextBaselineAlignment, WhiteSpaceNowrapDoesNotMoveABaseline)
{
    constexpr char kWsXmlFmt[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="big">Hxg</label>
    <label id="small">Hxg</label>
  </uielement>
</uielement>)";

    struct Variant { const char* Name; const char* Css; float ChromeBaseline; };
    const Variant variants[] = {
        {"lh2 wrapping", R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 16px; line-height: 2; color: #ffffff; }
#small { font-family: Roboto; font-size: 16px; color: #ffffff; }
)", 22.0f},
        {"lh2 nowrap", R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 16px; line-height: 2; white-space: nowrap; color: #ffffff; }
#small { font-family: Roboto; font-size: 16px; color: #ffffff; }
)", 22.0f},
        {"lh25 wrapping", R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 16px; line-height: 25px; color: #ffffff; }
#small { font-family: Roboto; font-size: 16px; color: #ffffff; }
)", 19.0f},
        {"lh25 nowrap", R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#row { display: flex; flex-direction: row; align-items: baseline; }
#big { font-family: Roboto; font-size: 16px; line-height: 25px; white-space: nowrap; color: #ffffff; }
#small { font-family: Roboto; font-size: 16px; color: #ffffff; }
)", 19.0f},
    };

    for (const Variant& v : variants)
    {
        IsolatedUIFixture fx;
        const auto ready = BuildRow(fx, 1.0f, kWsXmlFmt, v.Css, {"big", "small"});
        if (SkippedForDevice(ready))
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(ready) << v.Name;

        float big = 0.0f, small = 0.0f;
        ASSERT_TRUE(FirstBaseline(fx, "big", big)) << v.Name;
        ASSERT_TRUE(FirstBaseline(fx, "small", small)) << v.Name;

        const PhysicalRect row = fx.BorderBox("row");
        EXPECT_NEAR(big, small, kExactPx) << v.Name;
        EXPECT_NEAR(big - row.Y, v.ChromeBaseline, kExactPx) << v.Name;
    }
}
