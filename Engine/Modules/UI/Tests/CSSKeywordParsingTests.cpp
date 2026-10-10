// Keyword coverage for the per-property CSS value parsers.
//
// Two concerns live here:
//
//  1. align-items / align-self carry the full css-align-3 keyword set,
//     including `baseline`, and REJECT an unrecognised value instead of
//     substituting one. Rejection means the declaration is ignored and the
//     cascaded value survives (CSS 2.1 4.2).
//
//  2. Every other keyword parser ends in a terminal fall-through arm, and 624
//     shipped declarations reach one for a legitimate keyword. Those keywords
//     resolve correctly by accident, so they get no branch of their own here -
//     a branch returning the same value the arm returns is dead code. What did
//     need fixing is the set of legitimate keywords the arm resolves to the
//     WRONG value; those now have explicit branches ahead of it.
//
//     The arm itself is unchanged: an unrecognised value still resolves to it
//     rather than being rejected. #748 is what converts the arms to
//     rejections, and the coverage test at the bottom of this file is the net
//     that catches the 624 when it does.

#include <gtest/gtest.h>

#include <string>

#include "UI/Layout/YogaLayout.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIElement.h"

using namespace GameEngine;
using namespace GameEngine::UILayout;
using namespace GameEngine::UIParsing;

namespace
{

ResolvedStyle ComputeForClass(const std::string& css, const std::string& className)
{
    Stylesheet sheet{};
    EXPECT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    UIElement el;
    el.AddClass(className);
    ElementState state{};
    return CSSParser::ComputeStyleFor(el, sheet, state);
}

ResolvedStyle ComputeFor(const std::string& css)
{
    return ComputeForClass(css, "probe");
}

// Declares `prop` twice: a valid keyword first, then `bad`. If the second
// declaration is rejected the first survives; if it is accepted (or clobbers
// with a fall-through value) the cascade shows the second.
ResolvedStyle ComputeWithOverride(const std::string& prop, const std::string& good, const std::string& bad)
{
    return ComputeFor(".probe { " + prop + ": " + good + "; " + prop + ": " + bad + "; }\n");
}

// Top edge of each child after a real layout pass.
struct RowTops
{
    float Short;
    float Tall;
};

// Lays out a row of two leaves of differing height, driving the whole chain the
// keyword has to survive: CSS text -> cascade -> ResolvedStyle -> YogaAdapter ->
// geometry. `containerAlign` and `childAlign` are spliced in as declarations, so
// an empty string leaves the property undeclared.
RowTops LayOutRow(const std::string& containerAlign, const std::string& childAlign)
{
    const std::string css =
        ".row { display: flex; flex-direction: row; " + containerAlign + " }\n"
        ".short { width: 40px; height: 20px; " + childAlign + " }\n"
        ".tall { width: 40px; height: 50px; " + childAlign + " }\n";

    YGNodeRef root = YogaAdapter::CreateNode();
    YogaAdapter::ApplyStyle(root, ComputeForClass(css, "row"));

    YGNodeRef shortChild = YogaAdapter::CreateNode();
    YogaAdapter::ApplyStyle(shortChild, ComputeForClass(css, "short"));
    YGNodeRef tallChild = YogaAdapter::CreateNode();
    YogaAdapter::ApplyStyle(tallChild, ComputeForClass(css, "tall"));

    YGNodeInsertChild(root, shortChild, 0);
    YGNodeInsertChild(root, tallChild, 1);
    YogaAdapter::CalculateLayout(root, 200.0f, 100.0f);

    const RowTops tops{YGNodeLayoutGetTop(shortChild), YGNodeLayoutGetTop(tallChild)};

    YogaAdapter::DestroyNode(tallChild);
    YogaAdapter::DestroyNode(shortChild);
    YogaAdapter::DestroyNode(root);
    return tops;
}

} // namespace

// --- align-items / align-self: full keyword set + rejection -----------------

TEST(CSSKeywordParsingTests, AlignItemsMapsFullKeywordSet) {
    EXPECT_EQ(ComputeFor(".probe { align-items: baseline; }").Layout.AlignItems, AlignItems::Baseline);
    EXPECT_EQ(ComputeFor(".probe { align-items: normal; }").Layout.AlignItems, AlignItems::Stretch);
    EXPECT_EQ(ComputeFor(".probe { align-items: stretch; }").Layout.AlignItems, AlignItems::Stretch);
    EXPECT_EQ(ComputeFor(".probe { align-items: start; }").Layout.AlignItems, AlignItems::FlexStart);
    EXPECT_EQ(ComputeFor(".probe { align-items: self-start; }").Layout.AlignItems, AlignItems::FlexStart);
    EXPECT_EQ(ComputeFor(".probe { align-items: flex-start; }").Layout.AlignItems, AlignItems::FlexStart);
    EXPECT_EQ(ComputeFor(".probe { align-items: center; }").Layout.AlignItems, AlignItems::Center);
    EXPECT_EQ(ComputeFor(".probe { align-items: end; }").Layout.AlignItems, AlignItems::FlexEnd);
    EXPECT_EQ(ComputeFor(".probe { align-items: self-end; }").Layout.AlignItems, AlignItems::FlexEnd);
    EXPECT_EQ(ComputeFor(".probe { align-items: flex-end; }").Layout.AlignItems, AlignItems::FlexEnd);
}

TEST(CSSKeywordParsingTests, AlignSelfMapsFullKeywordSet) {
    EXPECT_EQ(ComputeFor(".probe { align-self: auto; }").Layout.AlignSelf, AlignItems::Auto);
    EXPECT_EQ(ComputeFor(".probe { align-self: baseline; }").Layout.AlignSelf, AlignItems::Baseline);
    EXPECT_EQ(ComputeFor(".probe { align-self: normal; }").Layout.AlignSelf, AlignItems::Stretch);
    EXPECT_EQ(ComputeFor(".probe { align-self: start; }").Layout.AlignSelf, AlignItems::FlexStart);
    EXPECT_EQ(ComputeFor(".probe { align-self: self-end; }").Layout.AlignSelf, AlignItems::FlexEnd);
    EXPECT_EQ(ComputeFor(".probe { align-self: center; }").Layout.AlignSelf, AlignItems::Center);
}

// The assertion that matters for #763: an unrecognised align value must not
// silently become FlexEnd (or anything else). The declaration is ignored, so
// the earlier declaration in the same rule still applies.
TEST(CSSKeywordParsingTests, AlignItemsRejectsUnrecognisedValueAndKeepsCascade) {
    EXPECT_EQ(ComputeWithOverride("align-items", "center", "bogus").Layout.AlignItems, AlignItems::Center);
    EXPECT_EQ(ComputeWithOverride("align-items", "center", "space-between").Layout.AlignItems, AlignItems::Center);
    EXPECT_EQ(ComputeWithOverride("align-items", "center", "12px").Layout.AlignItems, AlignItems::Center);
    // Nothing declared at all still lands on the initial value.
    EXPECT_EQ(ComputeFor(".probe { align-items: bogus; }").Layout.AlignItems, AlignItems::Stretch);
}

TEST(CSSKeywordParsingTests, AlignSelfRejectsUnrecognisedValueAndKeepsCascade) {
    EXPECT_EQ(ComputeWithOverride("align-self", "center", "bogus").Layout.AlignSelf, AlignItems::Center);
    EXPECT_EQ(ComputeFor(".probe { align-self: bogus; }").Layout.AlignSelf, AlignItems::Auto);
}

// --- baseline reaches layout ------------------------------------------------
//
// The tests above stop at ResolvedStyle, which cannot tell a keyword that
// drives layout from one the adapter drops on the floor. These run a real Yoga
// pass and assert the geometry, so a Baseline that never reaches
// YGNodeStyleSetAlignItems fails here while every parser test above still
// passes.
//
// A childless leaf with no baseline function reports its height as its
// baseline, so the tall child sets the line's ascent (50) and each child's top
// is 50 - its own height. Everything here is a plain box; text nodes carry a
// real baseline function (TextBaselineAlignmentTests).

TEST(CSSKeywordParsingTests, AlignItemsBaselineMovesChildrenInLayout) {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) {
        GTEST_SKIP() << "Yoga not available";
    }

    const RowTops baseline = LayOutRow("align-items: baseline;", "");
    EXPECT_NEAR(baseline.Short, 30.0f, 0.01f);
    EXPECT_NEAR(baseline.Tall, 0.0f, 0.01f);

    // Baseline is a distinct alignment, not a spelling of a neighbouring one.
    const RowTops flexStart = LayOutRow("align-items: flex-start;", "");
    const RowTops stretch = LayOutRow("align-items: stretch;", "");
    const RowTops flexEnd = LayOutRow("align-items: flex-end;", "");
    const RowTops center = LayOutRow("align-items: center;", "");
    EXPECT_NE(baseline.Short, flexStart.Short);
    EXPECT_NE(baseline.Short, stretch.Short);
    EXPECT_NE(baseline.Short, flexEnd.Short);
    EXPECT_NE(baseline.Short, center.Short);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(CSSKeywordParsingTests, AlignSelfBaselineMovesChildrenInLayout) {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) {
        GTEST_SKIP() << "Yoga not available";
    }

    // Both children opt in: with a single baseline-aligned child the line's
    // ascent is that child's own, so the alignment cannot be observed.
    const RowTops baseline = LayOutRow("", "align-self: baseline;");
    EXPECT_NEAR(baseline.Short, 30.0f, 0.01f);
    EXPECT_NEAR(baseline.Tall, 0.0f, 0.01f);

    const RowTops flexStart = LayOutRow("", "align-self: flex-start;");
    EXPECT_NE(baseline.Short, flexStart.Short);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

// A rejected value must leave layout on the cascaded value, not on whatever the
// old fall-through arm produced. On the pre-fix parser `bogus` became FlexEnd,
// which is geometrically distinct from the `center` it must preserve here.
TEST(CSSKeywordParsingTests, RejectedAlignValueLeavesLayoutOnTheCascadedValue) {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) {
        GTEST_SKIP() << "Yoga not available";
    }

    const RowTops rejected = LayOutRow("align-items: center; align-items: bogus;", "");
    const RowTops center = LayOutRow("align-items: center;", "");
    const RowTops flexEnd = LayOutRow("align-items: flex-end;", "");
    EXPECT_NEAR(rejected.Short, center.Short, 0.01f);
    EXPECT_NEAR(rejected.Tall, center.Tall, 0.01f);
    EXPECT_NE(rejected.Short, flexEnd.Short);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

// --- keywords that previously resolved to the WRONG value via a fall-through -

TEST(CSSKeywordParsingTests, ReverseFlowKeywordsKeepTheirAxis) {
    EXPECT_EQ(ComputeFor(".probe { flex-direction: row-reverse; }").Layout.FlexDirection, FlexDirection::Row);
    EXPECT_EQ(ComputeFor(".probe { flex-direction: column-reverse; }").Layout.FlexDirection, FlexDirection::Column);
    EXPECT_TRUE(ComputeFor(".probe { flex-wrap: wrap-reverse; }").Layout.FlexWrap);
}

TEST(CSSKeywordParsingTests, LogicalAndPhysicalAlignmentAliasesResolve) {
    EXPECT_EQ(ComputeFor(".probe { justify-content: end; }").Layout.JustifyContent, JustifyContent::FlexEnd);
    EXPECT_EQ(ComputeFor(".probe { justify-content: right; }").Layout.JustifyContent, JustifyContent::FlexEnd);
    EXPECT_EQ(ComputeFor(".probe { align-content: end; }").Layout.AlignContent, AlignContent::FlexEnd);
    EXPECT_EQ(ComputeFor(".probe { align-content: start; }").Layout.AlignContent, AlignContent::FlexStart);
    EXPECT_EQ(ComputeFor(".probe { text-align: end; }").Visual.TextAlign, TextAlign::Right);
}

TEST(CSSKeywordParsingTests, HidingAndClippingKeywordsResolve) {
    EXPECT_FALSE(ComputeFor(".probe { visibility: collapse; }").Visual.Visible);
    EXPECT_EQ(ComputeFor(".probe { overflow: overlay; }").Layout.Overflow, Overflow::Hidden);
}

TEST(CSSKeywordParsingTests, WrappingAndCursorAliasesResolve) {
    // `anywhere` is an overflow-wrap keyword only; word-break does not accept it.
    EXPECT_EQ(ComputeFor(".probe { overflow-wrap: anywhere; }").Visual.OverflowWrap,
              OverflowWrap::BreakWord);
    EXPECT_EQ(ComputeFor(".probe { word-break: anywhere; }").Visual.WordBreak, WordBreak::Normal);
    EXPECT_EQ(ComputeFor(".probe { cursor: nw-resize; }").Visual.Cursor, CursorStyle::NorthwestSoutheastResize);
    EXPECT_EQ(ComputeFor(".probe { cursor: se-resize; }").Visual.Cursor, CursorStyle::NorthwestSoutheastResize);
    EXPECT_EQ(ComputeFor(".probe { position: fixed; }").Layout.PositionType, PositionType::Absolute);
}

TEST(CSSKeywordParsingTests, TilingBackgroundRepeatKeywordsResolve) {
    EXPECT_EQ(ComputeFor(".probe { background-repeat: space; }").Visual.BackgroundImage.Repeat,
              BackgroundRepeat::Repeat);
    EXPECT_EQ(ComputeFor(".probe { background-repeat: round; }").Visual.BackgroundImage.Repeat,
              BackgroundRepeat::Repeat);
}

// --- the 624: keywords shipped CSS reaches a fall-through arm for -----------
// These pass on both sides of this change - they pin behaviour rather than
// prove it. Their job is to fail loudly in #748: each one is a keyword that
// only works because it lands on a terminal arm, so converting those arms to
// rejections breaks every one of them unless the keyword gets a real branch
// first. Declaration counts in shipped CSS at the time of writing are in the
// comment beside each line.

TEST(CSSKeywordParsingTests, ShippedKeywordsThatDependOnAFallThroughArm) {
    // 192
    EXPECT_EQ(ComputeFor(".probe { background-repeat: no-repeat; }").Visual.BackgroundImage.Repeat,
              BackgroundRepeat::NoRepeat);
    // 158
    EXPECT_EQ(ComputeFor(".probe { flex-direction: column; }").Layout.FlexDirection, FlexDirection::Column);
    // 63
    EXPECT_EQ(ComputeFor(".probe { position: relative; }").Layout.PositionType, PositionType::Relative);
    // 34
    EXPECT_EQ(ComputeFor(".probe { justify-content: flex-start; }").Layout.JustifyContent, JustifyContent::FlexStart);
    // 23
    EXPECT_EQ(ComputeFor(".probe { text-align: left; }").Visual.TextAlign, TextAlign::Left);
    // 21
    EXPECT_EQ(ComputeFor(".probe { cursor: default; }").Visual.Cursor, CursorStyle::Auto);
    // 19
    EXPECT_TRUE(ComputeFor(".probe { pointer-events: auto; }").Visual.PointerEvents);
    // 18
    EXPECT_EQ(ComputeFor(".probe { overflow: visible; }").Layout.Overflow, Overflow::Visible);
    // 13 - overflow-x/y deliberately keep auto/scroll visible, unlike the shorthand
    EXPECT_EQ(ComputeFor(".probe { overflow-y: auto; }").Layout.OverflowY, Overflow::Visible);
    // 22
    EXPECT_EQ(ComputeFor(".probe { border-style: solid; }").Visual.BorderStyle, BorderStyle::Solid);
    // 14
    EXPECT_FALSE(ComputeFor(".probe { flex-wrap: nowrap; }").Layout.FlexWrap);
    // 12
    EXPECT_EQ(ComputeFor(".probe { display: block; }").Layout.DisplayMode, DisplayMode::Block);
    // 11
    EXPECT_EQ(ComputeFor(".probe { white-space: normal; }").Visual.WhiteSpace, WhiteSpace::Normal);
    // 4
    EXPECT_TRUE(ComputeFor(".probe { visibility: visible; }").Visual.Visible);
    // 4
    EXPECT_EQ(ComputeFor(".probe { word-break: normal; }").Visual.WordBreak, WordBreak::Normal);
    // 1
    EXPECT_EQ(ComputeFor(".probe { font-style: normal; }").Visual.FontStyle, FontStyle::Normal);
    // 1 - `start` is LTR-correct only by accident; #763 wants direction read here
    EXPECT_EQ(ComputeFor(".probe { text-align: start; }").Visual.TextAlign, TextAlign::Left);
}

// Characterisation, not endorsement: outside align-items/align-self the
// terminal arm is still reached by garbage, so an invalid value still applies
// over the cascade. #748 is what turns these into rejections; when it lands,
// these expectations flip to "the earlier declaration survives".
TEST(CSSKeywordParsingTests, NonAlignParsersStillAcceptGarbageViaFallThrough) {
    EXPECT_EQ(ComputeWithOverride("background-repeat", "repeat", "bogus").Visual.BackgroundImage.Repeat,
              BackgroundRepeat::NoRepeat);
    EXPECT_EQ(ComputeWithOverride("flex-direction", "row", "bogus").Layout.FlexDirection, FlexDirection::Column);
    EXPECT_EQ(ComputeWithOverride("justify-content", "center", "bogus").Layout.JustifyContent,
              JustifyContent::FlexStart);
}

// --- baseline comes from a descendant, not from the box ---------------------

namespace
{

// A row exactly as tall as its tallest child, so the container leaves no slack
// for alignment to move anything into. `nestChild` gives the tall child a first
// child of its own, which is what gives it a baseline distinct from its height.
RowTops LayOutTightRow(const std::string& containerAlign, bool nestChild)
{
    const std::string css =
        ".row { display: flex; flex-direction: row; height: 40px; " + containerAlign + " }\n"
        ".short { width: 40px; height: 16px; }\n"
        ".tall { width: 40px; height: 40px; }\n"
        ".nested { width: 40px; height: 12px; }\n";

    YGNodeRef root = YogaAdapter::CreateNode();
    YogaAdapter::ApplyStyle(root, ComputeForClass(css, "row"));
    YGNodeRef shortChild = YogaAdapter::CreateNode();
    YogaAdapter::ApplyStyle(shortChild, ComputeForClass(css, "short"));
    YGNodeRef tallChild = YogaAdapter::CreateNode();
    YogaAdapter::ApplyStyle(tallChild, ComputeForClass(css, "tall"));

    YGNodeRef nested = nullptr;
    if (nestChild) {
        nested = YogaAdapter::CreateNode();
        YogaAdapter::ApplyStyle(nested, ComputeForClass(css, "nested"));
        YGNodeInsertChild(tallChild, nested, 0);
    }

    YGNodeInsertChild(root, shortChild, 0);
    YGNodeInsertChild(root, tallChild, 1);
    YogaAdapter::CalculateLayout(root, 200.0f, YGUndefined);

    const RowTops tops{YGNodeLayoutGetTop(shortChild), YGNodeLayoutGetTop(tallChild)};

    if (nested) {
        YogaAdapter::DestroyNode(nested);
    }
    YogaAdapter::DestroyNode(tallChild);
    YogaAdapter::DestroyNode(shortChild);
    YogaAdapter::DestroyNode(root);
    return tops;
}

} // namespace

// The layout tests above use a roomy container, where every alignment lands
// somewhere different. In a row no taller than its tallest child that stops
// being true, and the two shapes disagree about whether `baseline` is even
// observable - so both are pinned here.
TEST(CSSKeywordParsingTests, BaselineAlignsToDescendantBaselineInATightRow) {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) {
        GTEST_SKIP() << "Yoga not available";
    }

    // Leaf children: each one's baseline is its own bottom edge and the row has
    // no slack, so baseline and flex-end put both children in the same place.
    // This is why `align-items: baseline` looks inert on a row of plain boxes.
    const RowTops leafBaseline = LayOutTightRow("align-items: baseline;", false);
    const RowTops leafFlexEnd = LayOutTightRow("align-items: flex-end;", false);
    EXPECT_NEAR(leafBaseline.Short, leafFlexEnd.Short, 0.01f);
    EXPECT_NEAR(leafBaseline.Tall, leafFlexEnd.Tall, 0.01f);

    // Give the tall child a 12px first child and it adopts that child's
    // baseline (12) in place of its own height (40). The short leaf still
    // contributes 16 and so sets the line's ascent: short.top = 16 - 16 = 0,
    // tall.top = 16 - 12 = 4. No other YGAlign value produces this.
    const RowTops nestedBaseline = LayOutTightRow("align-items: baseline;", true);
    EXPECT_NEAR(nestedBaseline.Short, 0.0f, 0.01f);
    EXPECT_NEAR(nestedBaseline.Tall, 4.0f, 0.01f);

    const RowTops nestedFlexEnd = LayOutTightRow("align-items: flex-end;", true);
    EXPECT_NE(nestedBaseline.Short, nestedFlexEnd.Short);
    EXPECT_NE(nestedBaseline.Tall, nestedFlexEnd.Tall);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}
