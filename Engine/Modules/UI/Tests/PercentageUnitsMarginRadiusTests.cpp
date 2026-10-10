// The last members of the cross-token unit family (#54), pinned from the outside.
//
// ParseFloatPx returns the numeric prefix of the string it is handed. Two
// longhand parsers took their NUMBER from that prefix — the first token — while
// sniffing their UNIT from the last character of the WHOLE declaration, so a
// second component silently retargeted the first component's unit:
//
//   margin-left: 10px 25%            -> Percent(10)  (number from `10px`, unit from `25%`)
//   margin-left: 25% 10px            -> Px(25)
//   border-top-left-radius: 10px 20% -> Percent(10)
//   border-top-left-radius: 20% 10px -> Px(20)
//
// Same defect, same shape, as `gap` and `font-size` (#878), the percentage
// border-radius (#873) and the per-side border shorthands (#875). The two
// properties resolve it differently because CSS grades the two-value form
// differently, and each test below says which rule it is pinning:
//
//   margin-left takes a SINGLE value (css-box-3 §4). A second token makes the
//   declaration invalid, so it is rejected whole and the cascaded value stands
//   (CSS 2.1 §4.2) — the same answer a browser gives.
//
//   border-top-left-radius takes `<length-percentage>{1,2}` (css-backgrounds-3
//   §5.1): horizontal radius then vertical, naming an ELLIPSE. This engine
//   stores BOTH radii per corner (UIStyle.h CornerRadius{X,Y}, UIPrimitive.h
//   `Radii`/`RadiiY`, sdf_functions.glsl per-corner semi-axes), so the pair is
//   represented and both components paint. The horizontal resolves against the
//   box width and the vertical against its height, per the same section.
//   This comment previously described a one-scalar collapse that dropped the
//   vertical; that collapse is gone, and the tests below assert the vertical
//   landing rather than naming a radius they discard.
//
// Every pin is laid-out geometry or an emitted primitive, never a resolved-style
// field: a stored number proves the cascade kept a value, not that Yoga placed a
// child or that the GPU got a corner.

#include "IsolatedUIFixture.h"

#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// Margin percentages are an exact float multiply against an integer containing
// block and radii are an exact multiply against an integer border box, so the
// arms below agree far inside this when they agree at all. Anything looser would
// let a 25%-read-as-25px arm through on a small box.
constexpr float kExactPx = 0.01f;

// Returned by value: Primitives() hands back a fresh vector, so a pointer into
// one dangles at the end of the call's full-expression.
bool TryFirstRect(const std::vector<UIPrimitive>& prims, UIPrimitive& out)
{
    for (const UIPrimitive& p : prims)
    {
        if (GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect)
        {
            out = p;
            return true;
        }
    }
    return false;
}

} // namespace

// --- margin longhands: one value, or the declaration is invalid --------------
//
// #row is 400px of content, so `margin-left: 25%` is a 100px offset and
// `margin-left: 10px` is 10. The two-value specimens each sit under an earlier
// `margin-left: 30px` rule, this engine's cascade being file order: 30 is the
// value that must survive a rejected declaration, and it is deliberately equal
// to none of the wrong answers — 40 (Percent(10) of 400), 25 (Px(25)), 10, or 0
// — so a salvaged numeric prefix cannot pass as a rejection.

TEST(PercentageMargin, ASecondTokenRejectsTheLonghandWhole)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="rowa"><uielement id="fwd"/></uielement>
  <uielement id="rowb"><uielement id="rev"/></uielement>
  <uielement id="rowc"><uielement id="keep"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#rowa, #rowb, #rowc { display: flex; flex-direction: row; width: 400px; height: 60px; flex-shrink: 0; }
#fwd, #rev, #keep { width: 40px; height: 40px; flex-shrink: 0; }
#fwd  { margin-left: 30px; }
#fwd  { margin-left: 10px 25%; }
#rev  { margin-left: 30px; }
#rev  { margin-left: 25% 10px; }
#keep { margin-left: 30px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument: the surviving rule is a plain 30px margin, and this is what it
    // places. If this moves, the fixture changed, not the parser.
    const float rowOrigin = fx.BorderBox("rowc").X;
    ASSERT_NEAR(fx.BorderBox("keep").X - rowOrigin, 30.0f, kExactPx);

    // `10px 25%` was Percent(10) = 40px off the 400px containing block.
    EXPECT_NEAR(fx.BorderBox("fwd").X - fx.BorderBox("rowa").X, 30.0f, kExactPx)
        << "a second token must invalidate the declaration, not lend it a unit";
    // `25% 10px` was Px(25).
    EXPECT_NEAR(fx.BorderBox("rev").X - fx.BorderBox("rowb").X, 30.0f, kExactPx)
        << "a second token must invalidate the declaration, not lose the first's unit";
}

// The single-value forms the fix must leave alone: a percentage still resolves
// against the containing block, and `auto` still reaches Yoga. Without these,
// rejecting everything would pass the test above.
TEST(PercentageMargin, SingleValueLonghandsKeepResolvingAsBefore)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="rowp"><uielement id="pct"/></uielement>
  <uielement id="rowx"><uielement id="px"/></uielement>
  <uielement id="rowu"><uielement id="autoed"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#rowp, #rowx, #rowu { display: flex; flex-direction: row; width: 400px; height: 60px; flex-shrink: 0; }
#pct, #px, #autoed { width: 40px; height: 40px; flex-shrink: 0; }
#pct    { margin-left: 25%; }
#px     { margin-left: 100px; }
#autoed { margin-left: auto; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // 25% of the 400px containing block is the declared 100px equivalent. The
    // two arms are compared to each other, so a shared constant cannot make the
    // percentage arm pass while the resolution is wrong.
    EXPECT_NEAR(fx.BorderBox("pct").X - fx.BorderBox("rowp").X,
                fx.BorderBox("px").X - fx.BorderBox("rowx").X, kExactPx);
    EXPECT_NEAR(fx.BorderBox("pct").X - fx.BorderBox("rowp").X, 100.0f, kExactPx);

    // `auto` absorbs the free space: 400 - 40 = 360.
    EXPECT_NEAR(fx.BorderBox("autoed").X - fx.BorderBox("rowu").X, 360.0f, kExactPx);
}

// --- corner radius longhands: two values, horizontal wins --------------------
//
// Every specimen is 200x200, so a percentage resolves against a 200px basis
// (UsedBorderRadius, min(W,H)) and 20% is 40px — four times the 10px arm and
// well under the shader's min(halfW,halfH) = 100 clamp, so nothing downstream
// can flatten a wrong number into a right one.

namespace
{

constexpr char kRadiusXml[] = R"(<uielement id="root">
  <uielement id="mixed"/>
  <uielement id="revmixed"/>
  <uielement id="samepair"/>
  <uielement id="pctone"/>
  <uielement id="pxone"/>
  <uielement id="junk"/>
  <uielement id="three"/>
</uielement>)";

// `junk` and `three` sit under an earlier 30px rule (cascade is file order); 30
// is equal to none of the salvageable prefixes — 10 or 1 — so a rejection cannot
// be confused with a partial parse.
constexpr char kRadiusCss[] = R"(
#root { display: flex; flex-direction: row; flex-wrap: wrap; width: 800px; height: 600px; }
#mixed, #revmixed, #samepair, #pctone, #pxone, #junk, #three {
  width: 200px; height: 200px; background-color: #ff0000;
}
#mixed    { border-top-left-radius: 10px 20%; }
#revmixed { border-top-left-radius: 20% 10px; }
#samepair { border-top-left-radius: 30px; }
#samepair { border-top-left-radius: 10px 20px; }
#pctone   { border-top-left-radius: 20%; }
#pxone    { border-top-left-radius: 10px; }
#junk     { border-top-left-radius: 30px; }
#junk     { border-top-left-radius: 10px oops; }
#three    { border-top-left-radius: 30px; }
#three    { border-top-left-radius: 1px 2px 3px; }
)";

// 20% of the 200px basis. The single-value arms are the instrument: they are the
// two units the two-value arms have to reproduce.
constexpr float kPercentRadiusPx = 40.0f;
constexpr float kPixelRadiusPx = 10.0f;

} // namespace

// `10px 20%` is the horizontal 10px and a vertical 20% of the 200px box = 40px —
// a 10x40 ellipse, which is now what the engine stores and paints. Each axis has
// to keep its OWN unit: what this must NOT do is what the last-character sniff
// did, read the number from `10px` and the unit from `20%`, producing Percent(10)
// and a 20px corner. The two axes carry different units here, which is exactly
// why neither can resolve before layout.
TEST(PercentageCornerRadius, TwoValueLonghandKeepsEachComponentsOwnUnit)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kRadiusXml, kRadiusCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    ASSERT_FLOAT_EQ(fx.BorderBox("mixed").W, 200.0f);
    ASSERT_FLOAT_EQ(fx.BorderBox("mixed").H, 200.0f);

    UIPrimitive control{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("pxone"), control)) << "no background rect emitted";
    ASSERT_FLOAT_EQ(control.Radii[0], kPixelRadiusPx) << "the px instrument moved";

    UIPrimitive rect{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("mixed"), rect)) << "no background rect emitted";
    EXPECT_FLOAT_EQ(rect.Radii[0], control.Radii[0])
        << "the horizontal 10px must survive with its own unit; Percent(10) of the "
           "200px basis would paint 20";
    EXPECT_NE(rect.Radii[0], 20.0f);
    EXPECT_FLOAT_EQ(rect.RadiiY[0], kPercentRadiusPx)
        << "the vertical 20% resolves against the height and lands on its own axis";
}

// The reverse order, where the sniff dropped a percentage instead of inventing
// one: `20% 10px` is a 40x10 ellipse, and the old parser produced Px(20). Paired
// with the arm above, this is what catches an axis swap — the two specimens are
// each other's transpose, so a parser that crossed the axes would pass one and
// fail the other.
TEST(PercentageCornerRadius, ReversedTwoValueLonghandKeepsEachComponentsOwnUnit)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kRadiusXml, kRadiusCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIPrimitive control{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("pctone"), control)) << "no background rect emitted";
    ASSERT_FLOAT_EQ(control.Radii[0], kPercentRadiusPx) << "the percentage instrument moved";

    UIPrimitive rect{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("revmixed"), rect)) << "no background rect emitted";
    EXPECT_FLOAT_EQ(rect.Radii[0], control.Radii[0])
        << "the horizontal 20% must keep its unit; Px(20) would paint 20, not 40";
    EXPECT_NE(rect.Radii[0], 20.0f);
    EXPECT_FLOAT_EQ(rect.RadiiY[0], kPixelRadiusPx)
        << "the vertical 10px lands on the vertical axis, not the horizontal one";
}

// Pinned so a later sweep cannot quietly turn the pair into a rejection: a valid
// two-value corner is ACCEPTED, and both components land. 30 on either axis would
// mean the declaration was dropped and the cascaded rule stood.
TEST(PercentageCornerRadius, ValidTwoValueLonghandIsAcceptedNotRejected)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kRadiusXml, kRadiusCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIPrimitive rect{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("samepair"), rect)) << "no background rect emitted";
    EXPECT_FLOAT_EQ(rect.Radii[0], kPixelRadiusPx)
        << "a two-value corner is accepted; 30 would mean the declaration was "
           "dropped and the cascaded rule stood";
    EXPECT_FLOAT_EQ(rect.RadiiY[0], 20.0f)
        << "the vertical 20px lands on the vertical axis";
}

// A component that is not a length, and a third component, are both invalid for
// a corner whose grammar stops at two — rejected whole, leaving the earlier 30px
// rule standing rather than applying whatever prefix could be salvaged (10, 1).
TEST(PercentageCornerRadius, UnreadableAndOverlongLonghandsLeaveTheCascadedRadiusStanding)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kRadiusXml, kRadiusCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIPrimitive junk{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("junk"), junk)) << "no background rect emitted";
    EXPECT_FLOAT_EQ(junk.Radii[0], 30.0f);
    EXPECT_FLOAT_EQ(junk.RadiiY[0], 30.0f) << "the surviving 30px rule is circular";

    UIPrimitive three{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("three"), three)) << "no background rect emitted";
    EXPECT_FLOAT_EQ(three.Radii[0], 30.0f);
    EXPECT_FLOAT_EQ(three.RadiiY[0], 30.0f) << "the surviving 30px rule is circular";
}

// The single-value forms the fix must leave alone, and the corners it must not
// reach: only the top-left rule was declared, so the other three corners stay at
// the initial 0.
TEST(PercentageCornerRadius, SingleValueLonghandsAndUndeclaredCornersAreUntouched)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kRadiusXml, kRadiusCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // A single value is a CIRCULAR corner: it duplicates into both semi-axes
    // rather than leaving the vertical one at zero, which would square the corner.
    UIPrimitive pct{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("pctone"), pct)) << "no background rect emitted";
    EXPECT_FLOAT_EQ(pct.Radii[0], kPercentRadiusPx);
    EXPECT_FLOAT_EQ(pct.RadiiY[0], kPercentRadiusPx);
    EXPECT_FLOAT_EQ(pct.Radii[1], 0.0f);
    EXPECT_FLOAT_EQ(pct.RadiiY[1], 0.0f);
    EXPECT_FLOAT_EQ(pct.Radii[2], 0.0f);
    EXPECT_FLOAT_EQ(pct.Radii[3], 0.0f);

    UIPrimitive px{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("pxone"), px)) << "no background rect emitted";
    EXPECT_FLOAT_EQ(px.Radii[0], kPixelRadiusPx);
    EXPECT_FLOAT_EQ(px.RadiiY[0], kPixelRadiusPx);
    EXPECT_FLOAT_EQ(px.Radii[1], 0.0f);
}
