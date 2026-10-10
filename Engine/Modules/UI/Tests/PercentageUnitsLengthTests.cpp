// The widest member of the cross-token unit family (#889), pinned from outside
// the parser.
//
// ParseFloatPx returns the numeric prefix of the string it is handed. The
// length-longhand parser took its NUMBER from that prefix — the first token —
// while sniffing its UNIT from the last character of the WHOLE declaration, so a
// second component silently retargeted the first component's unit:
//
//   width: 10px 25%       -> Percent(10)  (number from `10px`, unit from `25%`)
//   width: 25% 10px       -> Px(25)
//   padding-left: 10px 25% -> Percent(10)
//   left: 10px 25%        -> Percent(10)
//
// Same defect, same shape, as `gap` and `font-size` (#878) and the margin and
// corner-radius longhands (#54). One parser backs `width`, `height`,
// `min-width`, `min-height`, `padding-*`, `left`, `top`, `right`, `bottom` and
// `flex-basis`, plus `max-width`/`max-height` through ParseMaxLengthValue, so
// the whole family is one function's worth of fix.
//
// Each of these properties takes a SINGLE component (css-sizing-3 §5, css-box-3
// §4, css-position-3 §3). A second token makes the declaration invalid, so it is
// rejected whole and the cascaded value stands (CSS 2.1 §4.2) — the same answer
// a browser gives, and the answer the four sibling parsers already give.
//
// Rejection is also the new answer for a component that cannot be read at all
// (`fit-content`, unknown functions, a `calc()` that cannot fold). This parser
// used to hand back
// StyleLength::Auto() there, which is not "ignore the declaration": Auto IS a
// value, it is the initial value of every property in the set, so a typo behaved
// as `width: initial` and threw away an earlier rule. For padding it was worse
// than that — padding has no `auto`, so CSSParser's applyBoxEdge collapses Auto
// to zero and an unreadable declaration silently zeroed a cascaded padding. The
// specimens below pin the cascaded value surviving in both cases.
//
// Every pin is laid-out geometry, never a resolved-style field: a stored number
// proves the cascade kept a value, not that Yoga sized or placed a box.

#include "IsolatedUIFixture.h"

#include <gtest/gtest.h>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// Every expected number below is an integer px against an integer containing
// block, so the arms agree far inside this when they agree at all. Anything
// looser would let a 25%-read-as-25px arm through on a small box.
constexpr float kExactPx = 0.01f;

// The root's content box, which is the containing block every percentage in this
// file resolves against: 25% is 200px and Percent(10) is 80px.
constexpr float kContainingBlockPx = 800.0f;
constexpr float kPercentWidthPx = 200.0f; // 25% of 800
constexpr float kPixelWidthPx = 10.0f;

// The value every rejected declaration must fall back to. Deliberately equal to
// none of the wrong answers — 80 (Percent(10) of 800), 25 (Px(25)), 200 (25%),
// 10, or 0 (auto/unset) — so a salvaged numeric prefix cannot pass as a
// rejection.
constexpr float kCascadedPx = 30.0f;

constexpr char kWidthXml[] = R"(<uielement id="root">
  <uielement id="wfwd"/>
  <uielement id="wrev"/>
  <uielement id="wjunk"/>
  <uielement id="wkeep"/>
  <uielement id="wpx"/>
  <uielement id="wpct"/>
  <uielement id="wauto"/>
  <uielement id="wempty"/>
  <uielement id="wvarempty"/>
</uielement>)";

// The cascade is file order in this engine, so each two-rule specimen states its
// surviving value first and the declaration under test second.
constexpr char kWidthCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#wfwd, #wrev, #wjunk, #wkeep, #wpx, #wpct, #wauto { height: 20px; flex-shrink: 0; }
#wfwd  { width: 30px; }
#wfwd  { width: 10px 25%; }
#wrev  { width: 30px; }
#wrev  { width: 25% 10px; }
#wjunk { width: 30px; }
#wjunk { width: fit-content; }
#wkeep { width: 30px; }
#wpx   { width: 10px; }
#wpct  { width: 25%; }
#wauto { width: 30px; }
#wauto { width: auto; }
#wempty { width: 30px; }
#wempty { width:; }
#wvarempty { --empty-width: ; width: 30px; }
#wvarempty { width: var(--empty-width); }
)";

} // namespace

// --- width: the number and the unit must come from the same token ------------

TEST(PercentageLength, ASecondTokenRejectsTheWidthLonghandWhole)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kWidthXml, kWidthCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument: the surviving rule is a plain 30px width, and this is the box
    // it makes. If this moves, the fixture changed, not the parser.
    ASSERT_NEAR(fx.BorderBox("wkeep").W, kCascadedPx, kExactPx);
    ASSERT_NEAR(fx.BorderBox("root").W, kContainingBlockPx, kExactPx);

    // `10px 25%` was Percent(10) = 80px of the 800px containing block.
    EXPECT_NEAR(fx.BorderBox("wfwd").W, kCascadedPx, kExactPx)
        << "a second token must invalidate the declaration, not lend it a unit";
    // `25% 10px` was Px(25).
    EXPECT_NEAR(fx.BorderBox("wrev").W, kCascadedPx, kExactPx)
        << "a second token must invalidate the declaration, not lose the first's unit";
}

// The contract this fix changes, stated on its own: an unreadable component is
// now rejected rather than resolved to Auto. `fit-content` is valid CSS the
// engine cannot represent; CSS 2.1 4.2 says drop the declaration, so the 30px
// rule above it stands. The old Auto() fallback would have content-sized the box
// to 0 instead, discarding a value the author did write.
TEST(PercentageLength, AnUnreadableComponentLeavesTheCascadedWidthStanding)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kWidthXml, kWidthCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.BorderBox("wjunk").W, kCascadedPx, kExactPx)
        << "an unrepresentable value must be ignored, not substituted with auto";
}

// The single-value forms the fix must leave alone. Without these, rejecting
// every declaration would pass both tests above: `auto` in particular is a real
// value here — it is what the element falls back to when nothing sizes it — and
// it has to keep overriding the 30px rule that precedes it.
TEST(PercentageLength, SingleValueWidthsKeepResolvingAsBefore)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kWidthXml, kWidthCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.BorderBox("wpx").W, kPixelWidthPx, kExactPx);
    EXPECT_NEAR(fx.BorderBox("wpct").W, kPercentWidthPx, kExactPx);

    // An empty element sized by its content is 0 wide, which is neither the 30px
    // it would keep on a rejection nor any percentage of the containing block.
    EXPECT_NEAR(fx.BorderBox("wauto").W, 0.0f, kExactPx)
        << "`auto` is a value for this property, not the parser's failure mode";
}

// An empty declaration value — written literally or produced by a var() chain
// that substitutes to nothing — tokenizes to zero components and is rejected
// like any other malformed declaration, so the cascaded rule stands. The old
// Auto() fallback would have content-sized both boxes to 0. (A literal
// `width:;` may also be dropped upstream by the stylesheet tokenizer; either
// way the observable contract is the same: the 30px rule survives.)
TEST(PercentageLength, AnEmptyValueLeavesTheCascadedWidthStanding)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kWidthXml, kWidthCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.BorderBox("wempty").W, kCascadedPx, kExactPx)
        << "an empty value must be ignored, not substituted with auto";
    EXPECT_NEAR(fx.BorderBox("wvarempty").W, kCascadedPx, kExactPx)
        << "a var() substituting to empty must be ignored, not substituted with auto";
}

// --- padding-left: a different applier, the same parser ----------------------
//
// Padding reaches ResolvedStyle through CSSParser's applyBoxEdge, which collapses
// an Auto unit to zero because CSS has no `padding: auto`. That is what made the
// old fallback destructive here, so the rejection is pinned on this property
// separately rather than assumed from `width`.

namespace
{

constexpr char kPaddingXml[] = R"(<uielement id="root">
  <uielement id="pfwd"><uielement id="pfwdkid"/></uielement>
  <uielement id="pjunk"><uielement id="pjunkkid"/></uielement>
  <uielement id="pref"><uielement id="prefkid"/></uielement>
</uielement>)";

constexpr char kPaddingCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#pfwd, #pjunk, #pref { display: flex; flex-direction: row; width: 400px; height: 40px; flex-shrink: 0; }
#pfwdkid, #pjunkkid, #prefkid { width: 20px; height: 20px; flex-shrink: 0; }
#pfwd  { padding-left: 30px; }
#pfwd  { padding-left: 10px 25%; }
#pjunk { padding-left: 30px; }
#pjunk { padding-left: blorp(1px); }
#pref  { padding-left: 10px; }
)";

} // namespace

TEST(PercentageLength, PaddingLonghandRejectionKeepsTheCascadedPadding)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kPaddingXml, kPaddingCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument: a plain 10px padding offsets the child by 10.
    ASSERT_NEAR(fx.BorderBox("prefkid").X - fx.BorderBox("pref").X, kPixelWidthPx, kExactPx);

    // `10px 25%` was Percent(10) — some multiple of the box, never 10 and never 30.
    EXPECT_NEAR(fx.BorderBox("pfwdkid").X - fx.BorderBox("pfwd").X, kCascadedPx, kExactPx)
        << "a second token must invalidate the declaration, not lend it a unit";

    // An unknown function is unreadable; the old Auto() fallback reached
    // applyBoxEdge and zeroed the edge, wiping the 30px rule that a browser
    // leaves standing. (calc() no longer qualifies as junk — it folds.)
    EXPECT_NEAR(fx.BorderBox("pjunkkid").X - fx.BorderBox("pjunk").X, kCascadedPx, kExactPx)
        << "an unreadable padding must be ignored, not collapsed to zero";
}

// --- left, and max-width through ParseMaxLengthValue -------------------------

namespace
{

constexpr char kOtherXml[] = R"(<uielement id="root">
  <uielement id="host">
    <uielement id="lfwd"/>
    <uielement id="lpct"/>
  </uielement>
  <uielement id="mfwd"/>
  <uielement id="mnone"/>
  <uielement id="mref"/>
</uielement>)";

// `#host` is 400px wide and padding-free, so a percentage `left` resolves
// against 400: 25% is 100px and the old Percent(10) was 40px.
constexpr char kOtherCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#host { position: relative; width: 400px; height: 100px; flex-shrink: 0; }
#lfwd, #lpct { position: absolute; top: 0px; width: 20px; height: 20px; }
#lfwd  { left: 30px; }
#lfwd  { left: 10px 25%; }
#lpct  { left: 25%; }
#mfwd, #mnone, #mref { width: 300px; height: 20px; flex-shrink: 0; }
#mfwd  { max-width: 200px; }
#mfwd  { max-width: 10px 25%; }
#mnone { max-width: 200px; }
#mnone { max-width: none; }
#mref  { max-width: 200px; }
)";

constexpr float kHostWidthPx = 400.0f;
constexpr float kPercentLeftPx = 100.0f; // 25% of 400
constexpr float kMaxWidthPx = 200.0f;
constexpr float kDeclaredWidthPx = 300.0f;

} // namespace

TEST(PercentageLength, ASecondTokenRejectsThePositionLonghandWhole)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kOtherXml, kOtherCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument: the percentage arm proves `left` resolves against #host at all,
    // and names the basis the wrong answer would have used.
    ASSERT_NEAR(fx.BorderBox("host").W, kHostWidthPx, kExactPx);
    ASSERT_NEAR(fx.BorderBox("lpct").X - fx.BorderBox("host").X, kPercentLeftPx, kExactPx);

    // `10px 25%` was Percent(10) = 40px of the 400px host.
    EXPECT_NEAR(fx.BorderBox("lfwd").X - fx.BorderBox("host").X, kCascadedPx, kExactPx)
        << "a second token must invalidate the declaration, not lend it a unit";
}

// max-width routes through ParseMaxLengthValue, which handles `none` itself and
// delegates everything else. Both halves are pinned: the delegation inherits the
// rejection, and `none` still reaches the layout as "no maximum".
TEST(PercentageLength, MaxWidthInheritsTheRejectionAndStillAcceptsNone)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kOtherXml, kOtherCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument: a plain 200px maximum clamps the declared 300px width.
    ASSERT_NEAR(fx.BorderBox("mref").W, kMaxWidthPx, kExactPx);

    // `10px 25%` was Percent(10) = 80px of the 800px containing block; an Auto()
    // fallback would have removed the maximum and left the full 300.
    EXPECT_NEAR(fx.BorderBox("mfwd").W, kMaxWidthPx, kExactPx)
        << "a second token must invalidate the declaration, not lend it a unit";

    EXPECT_NEAR(fx.BorderBox("mnone").W, kDeclaredWidthPx, kExactPx)
        << "`none` is a value for this property, not the parser's failure mode";
}
