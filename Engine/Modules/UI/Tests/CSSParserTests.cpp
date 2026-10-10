#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "UI/Parsers/CSSParser.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "UI/UIElement.h"
#include "UI/StyleProperties.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Selectors/SelectorTypes.h"
#include "Types/StringId.h"
#include "../Source/UIAttributeAccess.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

TEST(UIElementIdTests, SameIdPreservesSubtreeRuleCaches)
{
    UIElement root;
    root.SetId("cell-7");
    auto child = std::make_unique<UIElement>();
    auto* childPtr = child.get();
    root.AddChild(std::move(child));
    root.m_RuleCacheValid = true;
    childPtr->m_RuleCacheValid = true;
    const auto hash = root.GetIdHash();

    root.SetId("cell-7");

    EXPECT_TRUE(root.m_RuleCacheValid);
    EXPECT_TRUE(childPtr->m_RuleCacheValid);
    EXPECT_EQ(root.GetIdHash(), hash);
}

TEST(UIElementIdTests, ChangedAndClearedIdsInvalidateSubtreeRuleCaches)
{
    UIElement root;
    root.SetId("before");
    auto child = std::make_unique<UIElement>();
    auto* childPtr = child.get();
    root.AddChild(std::move(child));
    for (const std::string& id : {std::string("after"), std::string()})
    {
        root.m_RuleCacheValid = true;
        childPtr->m_RuleCacheValid = true;
        root.SetId(id);
        EXPECT_FALSE(root.m_RuleCacheValid);
        EXPECT_FALSE(childPtr->m_RuleCacheValid);
        EXPECT_EQ(root.GetId(), id);
        EXPECT_EQ(root.GetIdHash(), id.empty() ? StringId{0} : HashStringId(id));
    }
}

TEST(UIElementIdTests, RepeatedEmptyIdPreservesRuleCache)
{
    UIElement element;
    element.m_RuleCacheValid = true;
    element.SetId("");
    EXPECT_TRUE(element.m_RuleCacheValid);
    EXPECT_EQ(element.GetIdHash(), StringId{0});
}

namespace
{
// Every flex assertion in this file is on a fixture that DECLARES flex-shrink,
// so an empty optional is a failure, not a default. Fold it to a value no
// expectation accepts rather than to the CSS initial 1, which would pass
// silently on a parser that stopped recording the declaration.
constexpr float kUndeclaredFlexShrink = -1.0f;
float DeclaredShrink(const ResolvedStyle& rs)
{
    return rs.Layout.FlexShrink.value_or(kUndeclaredFlexShrink);
}
} // namespace

TEST(CSSParserTests, ParsesBasicClassAndIdRules) {
    const std::string css =
        ".btn { width: 100px; height: 40px; }\n"
        "#root { opacity: 0.5; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    // Expect two rules parsed
    ASSERT_GE(sheet.Rules.size(), 2u);
}

TEST(CSSParserTests, ComputesStyleWithCascadeSpecificity) {
    const std::string css =
        "div { width: 50px; }\n"
        ".btn { width: 100px; }\n"
        "#root { width: 200px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState state{}; // default
    auto style = CSSParser::ComputeStyleFor(el, sheet, state);

    // The ID selector should win (highest specificity)
    EXPECT_TRUE(style.Layout.Width.IsPx());
    EXPECT_NEAR(style.Layout.Width.Value, 200.f, 0.01f);
}

// Pins the cross-stylesheet precedence contract: specificity is compared
// before sheet attach order; sheet order only breaks equal-specificity ties.
TEST(CSSParserTests, HigherSpecificityInEarlierSheetBeatsLaterSheet) {
    Stylesheet early{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".panel .btn { width: 200px; }\n", early)); // specificity (0,2,0)

    Stylesheet late{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".btn { width: 50px; }\n", late)); // specificity (0,1,0)

    auto parent = std::make_unique<UIElement>("parent");
    parent->AddClass("panel");
    auto child = std::make_unique<UIElement>("child");
    child->AddClass("btn");
    UIElement* el = child.get();
    parent->AddChild(std::move(child));

    ElementState state{};
    const std::vector<const Stylesheet*> sheets{&early, &late};
    auto style = CSSParser::ComputeStyleFor(*el, sheets, state);

    EXPECT_TRUE(style.Layout.Width.IsPx());
    EXPECT_NEAR(style.Layout.Width.Value, 200.f, 0.01f)
        << "a higher-specificity rule in an earlier sheet must beat a later "
           "sheet's lower-specificity rule";
}

TEST(CSSParserTests, EqualSpecificityLaterSheetWins) {
    Stylesheet early{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".btn { width: 50px; }\n", early));

    Stylesheet late{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".btn { width: 100px; }\n", late));

    UIElement el;
    el.AddClass("btn");

    ElementState state{};
    const std::vector<const Stylesheet*> sheets{&early, &late};
    auto style = CSSParser::ComputeStyleFor(el, sheets, state);

    EXPECT_TRUE(style.Layout.Width.IsPx());
    EXPECT_NEAR(style.Layout.Width.Value, 100.f, 0.01f)
        << "equal specificity across sheets: the later-attached sheet must win";
}

// C-10: specificity is a lexicographic (A, B, C) triple. The old flattened
// int (A*10000 + B*100 + C) gave each tier a radix of 100, so 100+ classes
// overflowed into the id tier and out-ranked a genuine #id rule.
TEST(CSSParserTests, SpecificityClassCountCannotReachIdTier) {
    std::string manyClasses;
    for (int i = 0; i < 120; ++i)
        manyClasses += ".c" + std::to_string(i);
    const std::string css = manyClasses + " { width: 100px; }\n#root { width: 200px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    for (int i = 0; i < 120; ++i)
        el.AddClass("c" + std::to_string(i));

    ElementState state{};
    auto style = CSSParser::ComputeStyleFor(el, sheet, state);

    EXPECT_TRUE(style.Layout.Width.IsPx());
    EXPECT_NEAR(style.Layout.Width.Value, 200.f, 0.01f)
        << "one id must out-rank any number of classes";
}

// C-10: re-adding an attached stylesheet must not change its cascade
// position (equal-specificity ties break on sheet order, so the old
// erase+push_back silently promoted the re-added sheet).
TEST(CSSParserTests, StylesheetReAddPreservesOrder) {
    auto a = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".btn { width: 50px; }\n", *a));
    auto b = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".btn { width: 100px; }\n", *b));

    UIElement el;
    el.AddStylesheet(a);
    el.AddStylesheet(b);
    el.AddStylesheet(a); // re-add: position-preserving no-op

    const auto& sheets = el.GetStylesheets();
    ASSERT_EQ(sheets.size(), 2u);
    EXPECT_EQ(sheets[0].get(), a.get());
    EXPECT_EQ(sheets[1].get(), b.get());
}

// C-10: block replacement (the hot-reload path) must not re-insert sheets
// that survive outside the replaced block — a shared @import owned by
// another attached style would otherwise duplicate at a shifted position.
TEST(CSSParserTests, ReplaceStylesheetBlockDedupesAgainstSurvivors) {
    auto shared = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".shared { width: 10px; }\n", *shared));
    auto oldOwn = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".a { width: 20px; }\n", *oldOwn));
    auto newOwn = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".a { width: 30px; }\n", *newOwn));

    UIElement el;
    el.AddStylesheet(shared); // survivor owned by "another style"
    el.AddStylesheet(oldOwn);

    // Reload replaces oldOwn's block; the fresh cascade list includes the
    // transitively imported `shared` again.
    el.ReplaceStylesheetBlock({oldOwn.get()}, {shared, newOwn});

    const auto& sheets = el.GetStylesheets();
    ASSERT_EQ(sheets.size(), 2u);
    EXPECT_EQ(sheets[0].get(), shared.get()) << "survivor keeps its original position";
    EXPECT_EQ(sheets[1].get(), newOwn.get());
}

TEST(CSSParserTests, ParsesBorderImageSliceAndRepeat) {
    const std::string css =
        ".frame { border-image-slice: 12 20 fill; border-image-repeat: round stretch; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("frame");
    ElementState state{};
    auto style = CSSParser::ComputeStyleFor(el, sheet, state);

    const auto& bi = style.Visual.BackgroundImage;
    EXPECT_TRUE(bi.HasBorderImage);
    // "12 20" shorthand -> top=bottom=12, right=left=20.
    EXPECT_NEAR(bi.BiSlice[0], 12.0f, 0.01f);
    EXPECT_NEAR(bi.BiSlice[1], 20.0f, 0.01f);
    EXPECT_NEAR(bi.BiSlice[2], 12.0f, 0.01f);
    EXPECT_NEAR(bi.BiSlice[3], 20.0f, 0.01f);
    EXPECT_TRUE(bi.BiSliceFill);
    EXPECT_EQ(bi.BiRepeatX, NineSliceFill::Round);
    EXPECT_EQ(bi.BiRepeatY, NineSliceFill::Stretch);
}

TEST(CSSParserTests, BorderImageSliceShorthandForms) {
    // One value -> all four sides; no `fill` keyword -> fill defaults off.
    {
        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(".a { border-image-slice: 8; }", sheet));
        UIElement el; el.AddClass("a");
        ElementState st{};
        auto s = CSSParser::ComputeStyleFor(el, sheet, st);
        EXPECT_TRUE(s.Visual.BackgroundImage.HasBorderImage);
        EXPECT_NEAR(s.Visual.BackgroundImage.BiSlice[0], 8.0f, 0.01f);
        EXPECT_NEAR(s.Visual.BackgroundImage.BiSlice[3], 8.0f, 0.01f);
        EXPECT_FALSE(s.Visual.BackgroundImage.BiSliceFill);
    }
    // Four values -> top, right, bottom, left in order.
    {
        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(".b { border-image-slice: 1 2 3 4; }", sheet));
        UIElement el; el.AddClass("b");
        ElementState st{};
        auto s = CSSParser::ComputeStyleFor(el, sheet, st);
        EXPECT_NEAR(s.Visual.BackgroundImage.BiSlice[0], 1.0f, 0.01f);
        EXPECT_NEAR(s.Visual.BackgroundImage.BiSlice[1], 2.0f, 0.01f);
        EXPECT_NEAR(s.Visual.BackgroundImage.BiSlice[2], 3.0f, 0.01f);
        EXPECT_NEAR(s.Visual.BackgroundImage.BiSlice[3], 4.0f, 0.01f);
    }
}

TEST(CSSParserTests, ZeroSliceStillEnablesTheOverride) {
    // A zero slice is an override, not an absent one: it is how an element replaces a
    // texture's intrinsic 9-slice with none, so hasBorderImage must be set even though
    // every inset is 0 (the renderer then reads the override as un-sliced).
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".z { border-image-slice: 0 fill; }", sheet));
    UIElement el; el.AddClass("z");
    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(s.Visual.BackgroundImage.HasBorderImage);
    for (float inset : s.Visual.BackgroundImage.BiSlice)
        EXPECT_FLOAT_EQ(inset, 0.0f);
    EXPECT_TRUE(s.Visual.BackgroundImage.BiSliceFill);
}

TEST(CSSParserTests, BorderImageRepeatAloneDoesNotEnableOverride) {
    // repeat without slice must NOT set hasBorderImage, so it can't clobber a
    // texture's intrinsic 9-slice.
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(".r { border-image-repeat: round; }", sheet));
    UIElement el; el.AddClass("r");
    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_FALSE(s.Visual.BackgroundImage.HasBorderImage);
}

TEST(CSSParserTests, BorderImageRepeatKeywordMatrix) {
    struct Case { const char* kw; NineSliceFill expected; };
    const Case cases[] = {
        {"stretch", NineSliceFill::Stretch},
        {"repeat",  NineSliceFill::Tile},
        {"space",   NineSliceFill::Tile},
        {"round",   NineSliceFill::Round},
    };
    for (const auto& c : cases) {
        Stylesheet sheet{};
        const std::string css = std::string(".r { border-image-repeat: ") + c.kw + "; }";
        ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet)) << c.kw;
        UIElement el; el.AddClass("r");
        ElementState st{};
        auto s = CSSParser::ComputeStyleFor(el, sheet, st);
        // Single value propagates to both axes.
        EXPECT_EQ(s.Visual.BackgroundImage.BiRepeatX, c.expected) << c.kw;
        EXPECT_EQ(s.Visual.BackgroundImage.BiRepeatY, c.expected) << c.kw;
    }
}

TEST(CSSParserTests, SpecificityIsPseudoUsesMostSpecificArgument)
{
    // :is(#id) has ID-level specificity, so it must override a later class rule.
    const std::string css =
        ":is(#root) { width: 200px; }\n"
        ".btn { width: 100px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState state{};
    auto style = CSSParser::ComputeStyleFor(el, sheet, state);
    EXPECT_TRUE(style.Layout.Width.IsPx());
    EXPECT_NEAR(style.Layout.Width.Value, 200.f, 0.01f);
}

TEST(CSSParserTests, SpecificityNotPseudoUsesMostSpecificArgument)
{
    // :not(#id) has ID-level specificity, so it must override a later class rule
    // when it matches.
    const std::string css =
        ":not(#nope) { width: 200px; }\n"
        ".btn { width: 100px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState state{};
    auto style = CSSParser::ComputeStyleFor(el, sheet, state);
    EXPECT_TRUE(style.Layout.Width.IsPx());
    EXPECT_NEAR(style.Layout.Width.Value, 200.f, 0.01f);
}


TEST(CSSParserTests, SpecificityWherePseudoIsAlwaysZero)
{
    // :where() always has zero specificity, so a later class rule should override it.
    const std::string css =
        ":where(#root) { width: 200px; }\n"
        ".btn { width: 100px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState state{};
    auto style = CSSParser::ComputeStyleFor(el, sheet, state);
    EXPECT_TRUE(style.Layout.Width.IsPx());
    EXPECT_NEAR(style.Layout.Width.Value, 100.f, 0.01f);
}

TEST(CSSParserTests, CheckedPseudoClassUsesElementState)
{
    const std::string css =
        ".checkbox { width: 10px; }\n"
        ".checkbox:checked { width: 100px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("checkbox");

    ElementState st{};
    st.Checked = false;
    auto sUnchecked = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(sUnchecked.Layout.Width.IsPx());
    EXPECT_NEAR(sUnchecked.Layout.Width.Value, 10.f, 0.01f);

    st.Checked = true;
    auto sChecked = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(sChecked.Layout.Width.IsPx());
    EXPECT_NEAR(sChecked.Layout.Width.Value, 100.f, 0.01f);
}


TEST(CSSParserTests, ParsesColorFunctionsRGBAndHSL)
{
    const std::string css =
        "#root { color: rgb(255,0,0); }\n"
        "#green { color: hsl(120, 100%, 50%); }\n"
        "#blue { color: rgba(0,0,255,1.0); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement elRoot; elRoot.SetId("root");
    UIElement elGreen; elGreen.SetId("green");
    UIElement elBlue; elBlue.SetId("blue");

    auto sRoot = CSSParser::ComputeStyleFor(elRoot, sheet, st);
    auto sGreen = CSSParser::ComputeStyleFor(elGreen, sheet, st);
    auto sBlue = CSSParser::ComputeStyleFor(elBlue, sheet, st);

    EXPECT_EQ(sRoot.Visual.Color, 0xFFFF0000u); // red
    EXPECT_EQ(sGreen.Visual.Color, 0xFF00FF00u); // green
    EXPECT_EQ(sBlue.Visual.Color, 0xFF0000FFu); // blue, alpha=1.0
}

TEST(CSSParserTests, MalformedColorValuesAreRejectedWhole)
{
    // A color the parser cannot read fully is rejected, not parsed from a
    // prefix or padded with defaults; the property then holds its initial white.
    for (const char* color : {"#12", "#ggg", "#1234567z", "rgb(1,2)", "rgb(1,2,3,4,5)",
                              "rgba(1,2,3,no)", "rgb(nan,2,3)", "rgb(1x,2,3)",
                              "hsl(no,50%,50%)", "rgb(1,2,3)x", "0x123456junk",
                              "0x100000000", "4294967296"})
    {
        SCOPED_TRACE(color);
        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(
            std::string("#text { color: ") + color + "; }", sheet));
        UIElement element;
        element.SetId("text");
        EXPECT_EQ(CSSParser::ComputeStyleFor(element, sheet, {}).Visual.Color, 0xFFFFFFFFu);
    }
}

TEST(CSSParserTests, ParsesNamedColors)
{
    const std::string css =
        "#mag { color: fuchsia; }\n"
        "#aqua { color: aqua; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement elMag; elMag.SetId("mag");
    UIElement elAqua; elAqua.SetId("aqua");

    auto sMag = CSSParser::ComputeStyleFor(elMag, sheet, st);
    auto sAqua = CSSParser::ComputeStyleFor(elAqua, sheet, st);

    EXPECT_EQ(sMag.Visual.Color, 0xFFFF00FFu); // fuchsia/magenta
    EXPECT_EQ(sAqua.Visual.Color, 0xFF00FFFFu); // aqua/cyan
}


TEST(CSSParserTests, ParsesBorderShorthand)
{
    const std::string css =
        "#box { border: 5px solid #112233; }\n"
        "#box2 { border: 2px red; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement elBox; elBox.SetId("box");
    UIElement elBox2; elBox2.SetId("box2");

    auto s1 = CSSParser::ComputeStyleFor(elBox, sheet, st);
    auto s2 = CSSParser::ComputeStyleFor(elBox2, sheet, st);

    EXPECT_NEAR(s1.Layout.BorderWidth.Top, 5.f, 0.01f);
    EXPECT_NEAR(s1.Layout.BorderWidth.Right, 5.f, 0.01f);
    EXPECT_NEAR(s1.Layout.BorderWidth.Bottom, 5.f, 0.01f);
    EXPECT_NEAR(s1.Layout.BorderWidth.Left, 5.f, 0.01f);
    EXPECT_EQ(s1.Visual.BorderColor.Top, 0xFF112233u);
    EXPECT_EQ(s1.Visual.BorderColor.Right, 0xFF112233u);
    EXPECT_EQ(s1.Visual.BorderColor.Bottom, 0xFF112233u);
    EXPECT_EQ(s1.Visual.BorderColor.Left, 0xFF112233u);

    EXPECT_NEAR(s2.Layout.BorderWidth.Top, 2.f, 0.01f);
    EXPECT_NEAR(s2.Layout.BorderWidth.Right, 2.f, 0.01f);
    EXPECT_NEAR(s2.Layout.BorderWidth.Bottom, 2.f, 0.01f);
    EXPECT_NEAR(s2.Layout.BorderWidth.Left, 2.f, 0.01f);
    EXPECT_EQ(s2.Visual.BorderColor.Top, 0xFFFF0000u); // red
    EXPECT_EQ(s2.Visual.BorderColor.Right, 0xFFFF0000u);
    EXPECT_EQ(s2.Visual.BorderColor.Bottom, 0xFFFF0000u);
    EXPECT_EQ(s2.Visual.BorderColor.Left, 0xFFFF0000u);
}

TEST(CSSParserTests, BorderWidthShorthandAndLonghandOverrides)
{
    const std::string css =
        "#a { border-width: 1px; }\n"
        "#b { border-width: 1px 2px; }\n"
        "#c { border-width: 1px 2px 3px; }\n"
        "#d { border-width: 1px 2px 3px 4px; }\n"
        "#e { border-width: 1px 2px 3px 4px; border-left-width: 9px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");
    UIElement c; c.SetId("c");
    UIElement d; d.SetId("d");
    UIElement e; e.SetId("e");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    auto sc = CSSParser::ComputeStyleFor(c, sheet, st);
    auto sd = CSSParser::ComputeStyleFor(d, sheet, st);
    auto se = CSSParser::ComputeStyleFor(e, sheet, st);

    EXPECT_NEAR(sa.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(sa.Layout.BorderWidth.Right, 1.f, 0.01f);
    EXPECT_NEAR(sa.Layout.BorderWidth.Bottom, 1.f, 0.01f);
    EXPECT_NEAR(sa.Layout.BorderWidth.Left, 1.f, 0.01f);

    EXPECT_NEAR(sb.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(sb.Layout.BorderWidth.Right, 2.f, 0.01f);
    EXPECT_NEAR(sb.Layout.BorderWidth.Bottom, 1.f, 0.01f);
    EXPECT_NEAR(sb.Layout.BorderWidth.Left, 2.f, 0.01f);

    EXPECT_NEAR(sc.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(sc.Layout.BorderWidth.Right, 2.f, 0.01f);
    EXPECT_NEAR(sc.Layout.BorderWidth.Bottom, 3.f, 0.01f);
    EXPECT_NEAR(sc.Layout.BorderWidth.Left, 2.f, 0.01f);

    EXPECT_NEAR(sd.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(sd.Layout.BorderWidth.Right, 2.f, 0.01f);
    EXPECT_NEAR(sd.Layout.BorderWidth.Bottom, 3.f, 0.01f);
    EXPECT_NEAR(sd.Layout.BorderWidth.Left, 4.f, 0.01f);

    EXPECT_NEAR(se.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(se.Layout.BorderWidth.Right, 2.f, 0.01f);
    EXPECT_NEAR(se.Layout.BorderWidth.Bottom, 3.f, 0.01f);
    EXPECT_NEAR(se.Layout.BorderWidth.Left, 9.f, 0.01f); // override
}

TEST(CSSParserTests, BorderColorShorthandAndLonghandOverrides)
{
    const std::string css =
        "#a { border-color: #111111; }\n"
        "#b { border-color: #111111 #222222; }\n"
        "#c { border-color: #111111 #222222 #333333; }\n"
        "#d { border-color: #111111 #222222 #333333 #444444; }\n"
        "#e { border-color: #111111 #222222 #333333 #444444; border-left-color: #999999; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");
    UIElement c; c.SetId("c");
    UIElement d; d.SetId("d");
    UIElement e; e.SetId("e");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    auto sc = CSSParser::ComputeStyleFor(c, sheet, st);
    auto sd = CSSParser::ComputeStyleFor(d, sheet, st);
    auto se = CSSParser::ComputeStyleFor(e, sheet, st);

    EXPECT_EQ(sa.Visual.BorderColor.Top, 0xFF111111u);
    EXPECT_EQ(sa.Visual.BorderColor.Right, 0xFF111111u);
    EXPECT_EQ(sa.Visual.BorderColor.Bottom, 0xFF111111u);
    EXPECT_EQ(sa.Visual.BorderColor.Left, 0xFF111111u);

    EXPECT_EQ(sb.Visual.BorderColor.Top, 0xFF111111u);
    EXPECT_EQ(sb.Visual.BorderColor.Right, 0xFF222222u);
    EXPECT_EQ(sb.Visual.BorderColor.Bottom, 0xFF111111u);
    EXPECT_EQ(sb.Visual.BorderColor.Left, 0xFF222222u);

    EXPECT_EQ(sc.Visual.BorderColor.Top, 0xFF111111u);
    EXPECT_EQ(sc.Visual.BorderColor.Right, 0xFF222222u);
    EXPECT_EQ(sc.Visual.BorderColor.Bottom, 0xFF333333u);
    EXPECT_EQ(sc.Visual.BorderColor.Left, 0xFF222222u);

    EXPECT_EQ(sd.Visual.BorderColor.Top, 0xFF111111u);
    EXPECT_EQ(sd.Visual.BorderColor.Right, 0xFF222222u);
    EXPECT_EQ(sd.Visual.BorderColor.Bottom, 0xFF333333u);
    EXPECT_EQ(sd.Visual.BorderColor.Left, 0xFF444444u);

    EXPECT_EQ(se.Visual.BorderColor.Top, 0xFF111111u);
    EXPECT_EQ(se.Visual.BorderColor.Right, 0xFF222222u);
    EXPECT_EQ(se.Visual.BorderColor.Bottom, 0xFF333333u);
    EXPECT_EQ(se.Visual.BorderColor.Left, 0xFF999999u); // override
}

TEST(CSSParserTests, BorderRadiusShorthandAndLonghandOverrides)
{
    const std::string css =
        "#r1 { border-radius: 4px; }\n"
        "#r2 { border-radius: 1px 2px / 8px 9px; }\n"
        "#r3 { border-radius: 1px 2px 3px; }\n"
        "#r4 { border-radius: 1px 2px 3px 4px; }\n"
        "#r5 { border-radius: 1px 2px; border-top-left-radius: 9px; }\n"
        "#r6 { border-top-left-radius: 10px 20px; }\n"
        "#r7 { border-radius: 5px; border-radius: 1px /; }\n"
        "#r8 { border-radius: 5px; border-radius: / 2px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement r1; r1.SetId("r1");
    UIElement r2; r2.SetId("r2");
    UIElement r3; r3.SetId("r3");
    UIElement r4; r4.SetId("r4");
    UIElement r5; r5.SetId("r5");
    UIElement r6; r6.SetId("r6");
    UIElement r7; r7.SetId("r7");
    UIElement r8; r8.SetId("r8");

    auto s1 = CSSParser::ComputeStyleFor(r1, sheet, st);
    auto s2 = CSSParser::ComputeStyleFor(r2, sheet, st);
    auto s3 = CSSParser::ComputeStyleFor(r3, sheet, st);
    auto s4 = CSSParser::ComputeStyleFor(r4, sheet, st);
    auto s5 = CSSParser::ComputeStyleFor(r5, sheet, st);
    auto s6 = CSSParser::ComputeStyleFor(r6, sheet, st);
    auto s7 = CSSParser::ComputeStyleFor(r7, sheet, st);
    auto s8 = CSSParser::ComputeStyleFor(r8, sheet, st);

    // One value is a circular corner: both semi-axes take it.
    EXPECT_NEAR(s1.Visual.BorderRadius.TopLeft.X, 4.f, 0.01f);
    EXPECT_NEAR(s1.Visual.BorderRadius.TopLeft.Y, 4.f, 0.01f);
    EXPECT_NEAR(s1.Visual.BorderRadius.TopRight.X, 4.f, 0.01f);
    EXPECT_NEAR(s1.Visual.BorderRadius.BottomRight.X, 4.f, 0.01f);
    EXPECT_NEAR(s1.Visual.BorderRadius.BottomLeft.X, 4.f, 0.01f);

    // `A / B`: the values before the slash are the horizontal radii and those
    // after it the vertical ones, each side fanning 1-4 values out to
    // tl/tr/br/bl on its own (css-backgrounds-3 §5.1). Both sides are 2-value
    // here, so each fans out as tl/br = first, tr/bl = second — and the two
    // sides differ, which is what distinguishes storing the vertical radii from
    // dropping them and from swapping the axes.
    EXPECT_NEAR(s2.Visual.BorderRadius.TopLeft.X, 1.f, 0.01f);
    EXPECT_NEAR(s2.Visual.BorderRadius.TopRight.X, 2.f, 0.01f);
    EXPECT_NEAR(s2.Visual.BorderRadius.BottomRight.X, 1.f, 0.01f);
    EXPECT_NEAR(s2.Visual.BorderRadius.BottomLeft.X, 2.f, 0.01f);
    EXPECT_NEAR(s2.Visual.BorderRadius.TopLeft.Y, 8.f, 0.01f);
    EXPECT_NEAR(s2.Visual.BorderRadius.TopRight.Y, 9.f, 0.01f);
    EXPECT_NEAR(s2.Visual.BorderRadius.BottomRight.Y, 8.f, 0.01f);
    EXPECT_NEAR(s2.Visual.BorderRadius.BottomLeft.Y, 9.f, 0.01f);

    // 3-value border-radius: tl=1, tr/bl=2, br=3
    EXPECT_NEAR(s3.Visual.BorderRadius.TopLeft.X, 1.f, 0.01f);
    EXPECT_NEAR(s3.Visual.BorderRadius.TopRight.X, 2.f, 0.01f);
    EXPECT_NEAR(s3.Visual.BorderRadius.BottomRight.X, 3.f, 0.01f);
    EXPECT_NEAR(s3.Visual.BorderRadius.BottomLeft.X, 2.f, 0.01f);

    // 4-value border-radius: tl=1, tr=2, br=3, bl=4
    EXPECT_NEAR(s4.Visual.BorderRadius.TopLeft.X, 1.f, 0.01f);
    EXPECT_NEAR(s4.Visual.BorderRadius.TopRight.X, 2.f, 0.01f);
    EXPECT_NEAR(s4.Visual.BorderRadius.BottomRight.X, 3.f, 0.01f);
    EXPECT_NEAR(s4.Visual.BorderRadius.BottomLeft.X, 4.f, 0.01f);

    // Longhand override
    EXPECT_NEAR(s5.Visual.BorderRadius.TopLeft.X, 9.f, 0.01f);
    EXPECT_NEAR(s5.Visual.BorderRadius.TopRight.X, 2.f, 0.01f);
    EXPECT_NEAR(s5.Visual.BorderRadius.BottomRight.X, 1.f, 0.01f);
    EXPECT_NEAR(s5.Visual.BorderRadius.BottomLeft.X, 2.f, 0.01f);

    // The two-value longhand: horizontal then vertical on one corner.
    EXPECT_NEAR(s6.Visual.BorderRadius.TopLeft.X, 10.f, 0.01f);
    EXPECT_NEAR(s6.Visual.BorderRadius.TopLeft.Y, 20.f, 0.01f);

    // A slash with an empty side — either side — is a malformed declaration and
    // is rejected whole (CSS 2.1 §4.2), so the cascaded 5px rule stands.
    EXPECT_NEAR(s7.Visual.BorderRadius.TopLeft.X, 5.f, 0.01f);
    EXPECT_NEAR(s7.Visual.BorderRadius.TopLeft.Y, 5.f, 0.01f);
    EXPECT_NEAR(s8.Visual.BorderRadius.TopLeft.X, 5.f, 0.01f);
    EXPECT_NEAR(s8.Visual.BorderRadius.TopLeft.Y, 5.f, 0.01f);
}

TEST(CSSParserTests, AdjacentAndGeneralSiblingCombinators)
{
    const std::string css =
        "label + .x { width: 10px; }\n"
        "label ~ .y { width: 20px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement root;
    auto a = std::make_unique<Label>();
    auto b = std::make_unique<UIElement>(); b->AddClass("x");
    auto c = std::make_unique<UIElement>(); c->AddClass("y");
    UIElement* bPtr = b.get();
    UIElement* cPtr = c.get();
    root.AddChild(std::move(a));
    root.AddChild(std::move(b));
    root.AddChild(std::move(c));

    ElementState st{};
    auto sb = CSSParser::ComputeStyleFor(*bPtr, sheet, st);
    auto sc = CSSParser::ComputeStyleFor(*cPtr, sheet, st);

    EXPECT_TRUE(sb.Layout.Width.IsPx());
    EXPECT_NEAR(sb.Layout.Width.Value, 10.f, 0.01f);
    EXPECT_TRUE(sc.Layout.Width.IsPx());
    EXPECT_NEAR(sc.Layout.Width.Value, 20.f, 0.01f);
}

TEST(CSSParserTests, AttributeSelectorVariants)
{
    const std::string css =
        "[data-role=assets] { width: 10px; }\n"
        "[data-tags~=browser] { height: 11px; }\n"
        "[data-lang|=en] { opacity: 0.5; }\n"
        "[data-name^=as] { border-width: 3px; }\n"
        "[data-name$=ts] { border-radius: 4px; }\n"
        "[data-name*=set] { padding: 6px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    UIAttributeAccess::SetAuthoredAttribute(el, "data-role", "assets", /*markDirty=*/false);
    UIAttributeAccess::SetAuthoredAttribute(el, "data-tags", "file browser panel", /*markDirty=*/false);
    UIAttributeAccess::SetAuthoredAttribute(el, "data-lang", "en-US", /*markDirty=*/false);
    UIAttributeAccess::SetAuthoredAttribute(el, "data-name", "assets", /*markDirty=*/false);

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 10.f, 0.01f);
    EXPECT_TRUE(s.Layout.Height.IsPx());
    EXPECT_NEAR(s.Layout.Height.Value, 11.f, 0.01f);
    EXPECT_NEAR(s.Visual.Opacity, 0.5f, 0.001f);
    EXPECT_NEAR(s.Layout.BorderWidth.Top, 3.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Right, 3.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Bottom, 3.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Left, 3.f, 0.01f);
    EXPECT_NEAR(s.Visual.BorderRadius.TopLeft.X, 4.f, 0.01f);
    EXPECT_NEAR(s.Visual.BorderRadius.TopLeft.Y, 4.f, 0.01f);
    EXPECT_NEAR(s.Visual.BorderRadius.TopRight.X, 4.f, 0.01f);
    EXPECT_NEAR(s.Visual.BorderRadius.BottomRight.X, 4.f, 0.01f);
    EXPECT_NEAR(s.Visual.BorderRadius.BottomLeft.X, 4.f, 0.01f);
    EXPECT_NEAR(s.Layout.Padding.Left, 6.f, 0.01f);
}

TEST(CSSParserTests, AttributeSelectorNamesAreCaseInsensitive)
{
    const std::string css =
        "[DATA-ROLE=assets] { width: 10px; }\n"
        "[ID=foo] { height: 12px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("foo");
    UIAttributeAccess::SetAuthoredAttribute(el, "Data-Role", "assets", /*markDirty=*/false);

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 10.f, 0.01f);
    EXPECT_TRUE(s.Layout.Height.IsPx());
    EXPECT_NEAR(s.Layout.Height.Value, 12.f, 0.01f);
}

TEST(CSSParserTests, ParsesEditorThemeSubset)
{
    // A minimal subset of Apps/Editor/Assets/UI/theme.css to validate parser behavior
    const std::string css =
        ".dockspace { display: flex; flex-direction: row; background: #0F1820; border: 1px solid #273340; }\n"
        ".tabbar { display: flex; gap: 6px; padding: 4px; }\n"
        ".tab { padding: 4px 8px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ASSERT_GE(sheet.Rules.size(), 2u);

    UIElement el;
    el.AddClass("dockspace");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    // Background color from #0F1820 (ARGB)
    EXPECT_EQ(s.Visual.BackgroundColor, 0xFF0F1820u);
    // Border width parsed
    EXPECT_NEAR(s.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Right, 1.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Bottom, 1.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Left, 1.f, 0.01f);
}


TEST(CSSParserTests, ParsesFullEditorThemeCSS)
{
    const std::string css =
        "#root, .dock-root { display: flex; flex-direction: row; gap: 8px; padding: 8px; }\n"
        ".panel { background-color: #1E2A35; border-color: #3A5366; border-width: 1px; border-radius: 6px; padding: 8px; color: #FFFFFF; }\n"
        ".scene { flex-grow: 1; }\n"
        ".dockspace { background-color: #0F1820; border-color: #2A3D4E; border-width: 1px; border-radius: 4px; font-size: 14px; color: #DEE6EE; font-family: \"Segoe UI\", Arial; }\n"
        ".dock-split.row { display: flex; flex-direction: row; gap: 0px; }\n"
        ".dock-split { flex-grow: 1; min-width: 0; min-height: 0; }\n"
        ".dock-split.col { display: flex; flex-direction: column; gap: 0px; }\n"
        ".dock-leaf { display: flex; flex-direction: column; flex-grow: 1; border-color: #243545; border-width: 1px; border-radius: 3px; }\n"
        ".tabbar { display: flex; flex-direction: row; gap: 6px; height: 28px; padding: 2px 6px; background-color: #18222E; }\n"
        ".tab { display: block; padding: 6px 10px; border-radius: 4px; background-color: #263545; color: #DEE6EE; cursor: pointer; }\n"
        ".tab:hover { background-color: #2E4154; }\n"
        ".tab.active { background-color: #375069; color: #FFFFFF; }\n"
        ".dock-content { flex-grow: 1; background-color: #0D141C; }\n"
        ".dockspace { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }\n"
        ".dock-leaf, .dock-content { min-width: 0; min-height: 0; }\n"
        ".pane { flex-basis: 0px; min-width: 0; min-height: 0; }\n"
        ".splitter.row { width: 4px; cursor: col-resize; background-color: #243545; }\n"
        ".splitter.col { height: 4px; cursor: row-resize; background-color: #243545; }\n"
        ".splitter.row:hover, .splitter.col:hover { background-color: #2E4154; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("dockspace");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_EQ(s.Visual.BackgroundColor, 0xFF0F1820u);
    EXPECT_NEAR(s.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Right, 1.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Bottom, 1.f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Left, 1.f, 0.01f);
}

TEST(CSSParserTests, ActivePseudoClassMatchesOnlyWhenStateActive)
{
    // :active is the standard CSS pseudo-class for the transient pressed
    // state (HTML and Unity UIToolkit semantics). It should only contribute
    // to the cascade when ElementState.Active is true.
    const std::string css =
        "#a { background-color: rgb(0, 0, 0); }\n"
        "#a:active { background-color: rgb(255, 0, 0); }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("a");

    ElementState idle{};
    auto sIdle = CSSParser::ComputeStyleFor(el, sheet, idle);
    EXPECT_EQ(sIdle.Visual.BackgroundColor, 0xFF000000u);

    ElementState active{};
    active.Active = true;
    auto sActive = CSSParser::ComputeStyleFor(el, sheet, active);
    EXPECT_EQ(sActive.Visual.BackgroundColor, 0xFFFF0000u);
}

TEST(CSSParserTests, DisplayInlineFlexIsTreatedAsFlex)
{
    const std::string css =
        "#a { display:inline-flex; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el;
    el.SetId("a");

    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_EQ(s.Layout.DisplayMode, DisplayMode::Flex);
}

TEST(CSSParserTests, PercentMarginsAndPadding)
{
    const std::string css =
        "#a { margin: 10% 5%; padding: 2% 4% 6% 8%; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("a");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_NEAR(s.Layout.Margin.Top, 10.f, 0.01f);
    EXPECT_TRUE(s.Layout.MarginIsPercent.Top);
    EXPECT_NEAR(s.Layout.Margin.Right, 5.f, 0.01f);
    EXPECT_TRUE(s.Layout.MarginIsPercent.Right);
    EXPECT_NEAR(s.Layout.Margin.Bottom, 10.f, 0.01f);
    EXPECT_TRUE(s.Layout.MarginIsPercent.Bottom);
    EXPECT_NEAR(s.Layout.Margin.Left, 5.f, 0.01f);
    EXPECT_TRUE(s.Layout.MarginIsPercent.Left);

    EXPECT_NEAR(s.Layout.Padding.Top, 2.f, 0.01f);
    EXPECT_TRUE(s.Layout.PaddingIsPercent.Top);
    EXPECT_NEAR(s.Layout.Padding.Right, 4.f, 0.01f);
    EXPECT_TRUE(s.Layout.PaddingIsPercent.Right);
    EXPECT_NEAR(s.Layout.Padding.Bottom, 6.f, 0.01f);
    EXPECT_TRUE(s.Layout.PaddingIsPercent.Bottom);
    EXPECT_NEAR(s.Layout.Padding.Left, 8.f, 0.01f);
    EXPECT_TRUE(s.Layout.PaddingIsPercent.Left);
}

TEST(CSSParserTests, RowAndColumnGapParsing)
{
    const std::string css =
        "#g { display:flex; row-gap: 7px; column-gap: 11px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("g");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_NEAR(s.Layout.RowGap, 7.f, 0.01f);
    EXPECT_NEAR(s.Layout.ColumnGap, 11.f, 0.01f);
}

// css-text-3 5.1 and 5.4: word-break and overflow-wrap are separate
// properties, both initially `normal`, and `word-wrap` aliases overflow-wrap.
// Each rule therefore leaves the OTHER property at its initial value.
TEST(CSSParserTests, ParsesWordBreakAndOverflowWrapIndependently)
{
    const std::string css =
        "#a { word-break: break-all; }\n"
        "#b { overflow-wrap: break-word; }\n"
        "#c { word-wrap: break-word; }\n"
        "#d { word-break: keep-all; }\n"
        "#e { overflow-wrap: anywhere; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");
    UIElement c; c.SetId("c");
    UIElement d; d.SetId("d");
    UIElement e; e.SetId("e");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    auto sc = CSSParser::ComputeStyleFor(c, sheet, st);
    auto sd = CSSParser::ComputeStyleFor(d, sheet, st);
    auto se = CSSParser::ComputeStyleFor(e, sheet, st);

    EXPECT_EQ(sa.Visual.WordBreak, WordBreak::BreakAll);
    EXPECT_EQ(sa.Visual.OverflowWrap, OverflowWrap::Normal);

    EXPECT_EQ(sb.Visual.OverflowWrap, OverflowWrap::BreakWord);
    EXPECT_EQ(sb.Visual.WordBreak, WordBreak::Normal);

    EXPECT_EQ(sc.Visual.OverflowWrap, OverflowWrap::BreakWord);
    EXPECT_EQ(sc.Visual.WordBreak, WordBreak::Normal);

    EXPECT_EQ(sd.Visual.WordBreak, WordBreak::KeepAll);
    EXPECT_EQ(sd.Visual.OverflowWrap, OverflowWrap::Normal);

    // `anywhere` is accepted; this engine does not distinguish its min-content
    // contribution from break-word's.
    EXPECT_EQ(se.Visual.OverflowWrap, OverflowWrap::BreakWord);
}

// The legacy `word-break: break-word` value, which css-text-3 defines as
// `word-break: normal` plus `overflow-wrap: anywhere`. It stays on the
// word-break property, and still reaches the line breaker as a break policy.
TEST(CSSParserTests, LegacyWordBreakBreakWordStaysOnWordBreak)
{
    const std::string css = "#a { word-break: break-word; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);

    EXPECT_EQ(sa.Visual.WordBreak, WordBreak::BreakWord);
    EXPECT_EQ(sa.Visual.OverflowWrap, OverflowWrap::Normal);
    EXPECT_EQ(ResolveTextBreakPolicy(sa.Visual.WordBreak, sa.Visual.OverflowWrap),
              WordBreak::BreakWord);
}

// The pair the editor ships (SettingsPanel.css:348-349), at the cascade level:
// one shared storage slot made these two declarations order-sensitive.
TEST(CSSParserTests, OverflowWrapSurvivesALaterWordBreakDeclaration)
{
    const std::string css =
        "#a { overflow-wrap: break-word; word-break: normal; }\n"
        "#b { word-break: normal; overflow-wrap: break-word; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");
    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);

    EXPECT_EQ(sa.Visual.OverflowWrap, OverflowWrap::BreakWord);
    EXPECT_EQ(sa.Visual.WordBreak, WordBreak::Normal);
    EXPECT_EQ(sb.Visual.OverflowWrap, sa.Visual.OverflowWrap);
    EXPECT_EQ(sb.Visual.WordBreak, sa.Visual.WordBreak);
}

TEST(CSSParserTests, ParsesWhiteSpaceValues)
{
    const std::string css =
        "#a { white-space: normal; }\n"
        "#b { white-space: nowrap; }\n"
        "#c { white-space: pre; }\n"
        "#d { white-space: pre-wrap; }\n"
        "#e { white-space: pre-line; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");
    UIElement c; c.SetId("c");
    UIElement d; d.SetId("d");
    UIElement e; e.SetId("e");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    auto sc = CSSParser::ComputeStyleFor(c, sheet, st);
    auto sd = CSSParser::ComputeStyleFor(d, sheet, st);
    auto se = CSSParser::ComputeStyleFor(e, sheet, st);

    EXPECT_EQ(sa.Visual.WhiteSpace, WhiteSpace::Normal);
    EXPECT_EQ(sb.Visual.WhiteSpace, WhiteSpace::NoWrap);
    EXPECT_EQ(sc.Visual.WhiteSpace, WhiteSpace::Pre);
    EXPECT_EQ(sd.Visual.WhiteSpace, WhiteSpace::Normal);
    EXPECT_EQ(se.Visual.WhiteSpace, WhiteSpace::Normal);
}

TEST(CSSParserTests, WhiteSpaceInheritance)
{
    const std::string css =
        "#parent { white-space: pre; }\n"
        "#child { }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement parent; parent.SetId("parent");
    UIElement child;  child.SetId("child");

    auto sp = CSSParser::ComputeStyleFor(parent, sheet, st);
    auto sc = CSSParser::ComputeStyleFor(child, std::vector<const Stylesheet*>{&sheet}, st, &sp);

    EXPECT_EQ(sp.Visual.WhiteSpace, WhiteSpace::Pre);
    EXPECT_EQ(sc.Visual.WhiteSpace, WhiteSpace::Pre);
}

TEST(CSSParserTests, WhiteSpaceNoWrap_DisablesWrapSemantic)
{
    // Verifies that white-space: nowrap sets the correct enum, and that
    // code checking this enum to suppress wrapping (e.g. Yoga measure,
    // TextArea::ComputeMetrics) would see the right value.
    // Guards against the scroll range overcount bug where Yoga measures
    // with wrapping but the renderer doesn't wrap.
    const std::string css =
        "#nowrap { white-space: nowrap; }\n"
        "#normal { white-space: normal; }\n"
        "#pre    { white-space: pre; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement elNoWrap; elNoWrap.SetId("nowrap");
    UIElement elNormal; elNormal.SetId("normal");
    UIElement elPre;    elPre.SetId("pre");

    auto sNoWrap = CSSParser::ComputeStyleFor(elNoWrap, sheet, st);
    auto sNormal = CSSParser::ComputeStyleFor(elNormal, sheet, st);
    auto sPre    = CSSParser::ComputeStyleFor(elPre, sheet, st);

    // nowrap and pre both suppress soft wrapping
    const bool noWrapDisablesWrap =
        (sNoWrap.Visual.WhiteSpace == WhiteSpace::NoWrap ||
         sNoWrap.Visual.WhiteSpace == WhiteSpace::Pre);
    EXPECT_TRUE(noWrapDisablesWrap)
        << "white-space: nowrap must disable soft wrapping for layout measurement";

    const bool preDisablesWrap =
        (sPre.Visual.WhiteSpace == WhiteSpace::NoWrap ||
         sPre.Visual.WhiteSpace == WhiteSpace::Pre);
    EXPECT_TRUE(preDisablesWrap)
        << "white-space: pre must disable soft wrapping for layout measurement";

    // normal does NOT disable wrapping
    const bool normalDisablesWrap =
        (sNormal.Visual.WhiteSpace == WhiteSpace::NoWrap ||
         sNormal.Visual.WhiteSpace == WhiteSpace::Pre);
    EXPECT_FALSE(normalDisablesWrap)
        << "white-space: normal must allow soft wrapping";
}

TEST(CSSParserTests, FlexBasisPercentAndMinMax)
{
    const std::string css =
        "#x { flex-basis: 50%; min-width: 20%; max-height: 120px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("x");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Layout.FlexBasis.IsPercent());
    EXPECT_NEAR(s.Layout.FlexBasis.Value, 50.f, 0.01f);
    EXPECT_TRUE(s.Layout.MinWidth.IsPercent());
    EXPECT_NEAR(s.Layout.MinWidth.Value, 20.f, 0.01f);
    EXPECT_TRUE(s.Layout.MaxHeight.IsPx());
    EXPECT_NEAR(s.Layout.MaxHeight.Value, 120.f, 0.01f);
}

TEST(CSSParserTests, FlexShorthandThreeValues)
{
    const std::string css =
        "#a { flex: 1 1 auto; }\n"
        "#b { flex: 0 0 200px; }\n"
        "#c { flex: 2 3 50%; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");
    UIElement c; c.SetId("c");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    EXPECT_NEAR(sa.Layout.FlexGrow, 1.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sa), 1.0f, 0.01f);
    EXPECT_TRUE(sa.Layout.FlexBasis.IsAuto());

    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    EXPECT_NEAR(sb.Layout.FlexGrow, 0.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sb), 0.0f, 0.01f);
    EXPECT_TRUE(sb.Layout.FlexBasis.IsPx());
    EXPECT_NEAR(sb.Layout.FlexBasis.Value, 200.0f, 0.01f);

    auto sc = CSSParser::ComputeStyleFor(c, sheet, st);
    EXPECT_NEAR(sc.Layout.FlexGrow, 2.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sc), 3.0f, 0.01f);
    EXPECT_TRUE(sc.Layout.FlexBasis.IsPercent());
    EXPECT_NEAR(sc.Layout.FlexBasis.Value, 50.0f, 0.01f);
}

TEST(CSSParserTests, FlexShorthandOneValue)
{
    const std::string css =
        "#a { flex: 1; }\n"
        "#b { flex: 0; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    EXPECT_NEAR(sa.Layout.FlexGrow, 1.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sa), 1.0f, 0.01f);
    EXPECT_TRUE(sa.Layout.FlexBasis.IsPx());
    EXPECT_NEAR(sa.Layout.FlexBasis.Value, 0.0f, 0.01f);

    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    EXPECT_NEAR(sb.Layout.FlexGrow, 0.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sb), 1.0f, 0.01f);
    EXPECT_TRUE(sb.Layout.FlexBasis.IsPx());
    EXPECT_NEAR(sb.Layout.FlexBasis.Value, 0.0f, 0.01f);
}

TEST(CSSParserTests, FlexShorthandTwoValues)
{
    const std::string css =
        "#a { flex: 1 0; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    EXPECT_NEAR(sa.Layout.FlexGrow, 1.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sa), 0.0f, 0.01f);
    EXPECT_TRUE(sa.Layout.FlexBasis.IsPx());
    EXPECT_NEAR(sa.Layout.FlexBasis.Value, 0.0f, 0.01f);
}

TEST(CSSParserTests, FlexShorthandKeywords)
{
    const std::string css =
        "#a { flex: none; }\n"
        "#b { flex: auto; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    EXPECT_NEAR(sa.Layout.FlexGrow, 0.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sa), 0.0f, 0.01f);
    EXPECT_TRUE(sa.Layout.FlexBasis.IsAuto());

    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    EXPECT_NEAR(sb.Layout.FlexGrow, 1.0f, 0.01f);
    EXPECT_NEAR(DeclaredShrink(sb), 1.0f, 0.01f);
    EXPECT_TRUE(sb.Layout.FlexBasis.IsAuto());
}

TEST(CSSParserTests, FlexShorthandOverridesLonghand)
{
    const std::string css =
        "#a { flex-grow: 5; flex: 1 1 auto; }\n"
        "#b { flex: 2 2 0; flex-grow: 7; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    EXPECT_NEAR(sa.Layout.FlexGrow, 1.0f, 0.01f);

    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    EXPECT_NEAR(sb.Layout.FlexGrow, 7.0f, 0.01f);
}

// --- flex shorthand vs css-flexbox-1 7.1.1 ----------------------------------
//
// Every expectation below is Chrome's computed flex-grow/flex-shrink/flex-basis
// for the same declaration, read from getComputedStyle under
// `chrome --headless=new --force-device-scale-factor=N`. The values are
// identical at dpr 1 and dpr 2 — the shorthand is a parse, so device scale
// cannot reach it; only the laid-out widths quantize differently, which
// FlexShorthandLayoutTests pins.

namespace
{

struct FlexExpectation
{
    const char* Declaration;
    float Grow;
    float Shrink;
    StyleLength::UnitType BasisUnit;
    float BasisValue;
};

// Resolves `flex: <decl>` on its own element and checks the three longhands.
void ExpectFlex(const FlexExpectation& e)
{
    const std::string css = std::string("#x { flex: ") + e.Declaration + "; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet)) << e.Declaration;

    ElementState st{};
    UIElement el;
    el.SetId("x");
    const auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_NEAR(s.Layout.FlexGrow, e.Grow, 0.001f) << "flex: " << e.Declaration;
    EXPECT_NEAR(DeclaredShrink(s), e.Shrink, 0.001f) << "flex: " << e.Declaration;
    EXPECT_EQ(s.Layout.FlexBasis.Unit, e.BasisUnit) << "flex: " << e.Declaration;
    if (e.BasisUnit != StyleLength::UnitType::Auto)
        EXPECT_NEAR(s.Layout.FlexBasis.Value, e.BasisValue, 0.001f) << "flex: " << e.Declaration;
}

constexpr auto kAuto = StyleLength::UnitType::Auto;
constexpr auto kPx = StyleLength::UnitType::Px;
constexpr auto kPercent = StyleLength::UnitType::Percent;

} // namespace

// The defect in #798: a lone <length> is the BASIS, not a grow factor.
// `flex: 100px` is `1 1 100px`; the unit is the whole discriminator, and
// `flex: 100` (no unit) still means grow.
TEST(CSSParserTests, FlexShorthandLoneLengthIsBasis)
{
    ExpectFlex({"100px", 1.0f, 1.0f, kPx, 100.0f});
    ExpectFlex({"0px", 1.0f, 1.0f, kPx, 0.0f});
    ExpectFlex({"50%", 1.0f, 1.0f, kPercent, 50.0f});
    // A percentage is a length, so `0%` is a basis while bare `0` is a grow
    // factor. The pair is the sharpest statement of the rule.
    ExpectFlex({"0%", 1.0f, 1.0f, kPercent, 0.0f});
    ExpectFlex({"0", 0.0f, 1.0f, kPx, 0.0f});
    ExpectFlex({"100", 100.0f, 1.0f, kPx, 0.0f});
    // Unit matching is case-insensitive.
    ExpectFlex({"100PX", 1.0f, 1.0f, kPx, 100.0f});
}

// Two values: the second is a shrink factor when bare and a basis when it
// carries a unit. `flex: 2 100px` is `2 1 100px`, NOT `flex-shrink: 100`.
TEST(CSSParserTests, FlexShorthandTwoValuesNumberThenLength)
{
    ExpectFlex({"2 100px", 2.0f, 1.0f, kPx, 100.0f});
    ExpectFlex({"2 3", 2.0f, 3.0f, kPx, 0.0f});
    ExpectFlex({"2 50%", 2.0f, 1.0f, kPercent, 50.0f});
    ExpectFlex({"1 auto", 1.0f, 1.0f, kAuto, 0.0f});
    ExpectFlex({"0 auto", 0.0f, 1.0f, kAuto, 0.0f});
}

// css-flexbox-1 uses `||`, so the basis may precede the factors:
// `flex: 100px 2` is `2 1 100px`, the same as `flex: 2 100px`.
TEST(CSSParserTests, FlexShorthandBasisMayComeFirst)
{
    ExpectFlex({"100px 2", 2.0f, 1.0f, kPx, 100.0f});
    ExpectFlex({"100px 2 3", 2.0f, 3.0f, kPx, 100.0f});
    ExpectFlex({"auto 1 2", 1.0f, 2.0f, kAuto, 0.0f});
    ExpectFlex({"auto 0 0", 0.0f, 0.0f, kAuto, 0.0f});
    ExpectFlex({"50% 2", 2.0f, 1.0f, kPercent, 50.0f});
    // Basis first, then a lone factor: the `0` is grow, so shrink stays 1.
    ExpectFlex({"0px 0", 0.0f, 1.0f, kPx, 0.0f});
    ExpectFlex({"100px 0", 0.0f, 1.0f, kPx, 100.0f});
}

// The two factors are one adjacent group, so a second bare number is shrink,
// never a basis — `flex: 2 0` is `2 0 0`, not `2 1 0`.
TEST(CSSParserTests, FlexShorthandFactorsAreAdjacent)
{
    ExpectFlex({"2 0", 2.0f, 0.0f, kPx, 0.0f});
    ExpectFlex({"0 0", 0.0f, 0.0f, kPx, 0.0f});
    ExpectFlex({"2 0 100px", 2.0f, 0.0f, kPx, 100.0f});
    ExpectFlex({"1 0 0", 1.0f, 0.0f, kPx, 0.0f});
}

// Unitless zero is both a valid <number> and a valid <length>, so once the
// factor group is closed a leftover `0` lands on the basis.
TEST(CSSParserTests, FlexShorthandUnitlessZeroCanBeBasis)
{
    ExpectFlex({"2 3 0", 2.0f, 3.0f, kPx, 0.0f});
    ExpectFlex({"1 1 0", 1.0f, 1.0f, kPx, 0.0f});
    ExpectFlex({"0 0 0", 0.0f, 0.0f, kPx, 0.0f});
    ExpectFlex({"1 1 0.0", 1.0f, 1.0f, kPx, 0.0f});
}

TEST(CSSParserTests, FlexShorthandPresets)
{
    // `none` is the one form the grammar cannot produce.
    ExpectFlex({"none", 0.0f, 0.0f, kAuto, 0.0f});
    // `auto` falls out of the general path: a lone basis keyword, factors
    // defaulting to 1.
    ExpectFlex({"auto", 1.0f, 1.0f, kAuto, 0.0f});
    ExpectFlex({"1", 1.0f, 1.0f, kPx, 0.0f});
    ExpectFlex({"1 1 auto", 1.0f, 1.0f, kAuto, 0.0f});
    ExpectFlex({"0 0 auto", 0.0f, 0.0f, kAuto, 0.0f});
}

// `initial` never reaches the shorthand expander — it fans out to the three
// longhands as a CSS-wide keyword and resolves to their initial values, which
// are Chrome's `0 1 auto`.
TEST(CSSParserTests, FlexShorthandInitialResolvesToInitialLonghands)
{
    ExpectFlex({"initial", 0.0f, 1.0f, kAuto, 0.0f});
}

// An invalid declaration is ignored so the cascaded value survives
// (CSS 2.1 4.2). Each case below is one Chrome rejects; the earlier
// declaration must still be the one in force.
TEST(CSSParserTests, FlexShorthandInvalidDeclarationIsIgnored)
{
    const char* kInvalid[] = {
        "2 3 4",           // a third factor: unitless non-zero is not a length
        "100px 200px",     // two bases
        "0 100px 0",       // basis between the factors, then a stray number
        "3 100px 4",       // same shape, non-zero leftover
        "-1",              // negative flex-grow
        "1 -1",            // negative flex-shrink
        "1 1 -5px",        // negative flex-basis
        "solid",           // not a number and not a basis keyword
        "none 2",          // `none` is only valid alone
        "2 none",          //
        "1 auto 2",        // factors split by the basis
        "initial 2",       // CSS-wide keyword mixed with a value
        "1 2 3 4",         // four components
        "0 0 0 0",         //
        "100px 2 3 4",     //
        "content",         // valid CSS, but StyleLength cannot represent it
    };

    for (const char* decl : kInvalid)
    {
        const std::string css =
            std::string("#x { flex: 4 5 60px; flex: ") + decl + "; }\n";
        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet)) << decl;

        ElementState st{};
        UIElement el;
        el.SetId("x");
        const auto s = CSSParser::ComputeStyleFor(el, sheet, st);

        EXPECT_NEAR(s.Layout.FlexGrow, 4.0f, 0.001f) << "flex: " << decl;
        EXPECT_NEAR(DeclaredShrink(s), 5.0f, 0.001f) << "flex: " << decl;
        EXPECT_EQ(s.Layout.FlexBasis.Unit, kPx) << "flex: " << decl;
        EXPECT_NEAR(s.Layout.FlexBasis.Value, 60.0f, 0.001f) << "flex: " << decl;
    }
}

// Known deviations from Chrome, pinned so they are visible rather than
// discovered. All three come from the engine having no CSS length-unit model at
// all — ParseLengthValue treats every unit suffix as px, for `width` exactly as
// for `flex` — so they are not specific to the shorthand and are not fixed here.
// What this fix does guarantee is that a unit suffix routes the value to the
// BASIS; which unit it was remains unmodelled.
TEST(CSSParserTests, FlexShorthandUnitHandlingDeviatesFromChrome)
{
    // Chrome rejects an unknown unit outright. The engine takes the numeric
    // prefix as px, which is what it does for `width: 100foo` too.
    ExpectFlex({"100foo", 1.0f, 1.0f, kPx, 100.0f});

    // Chrome resolves 100em against the 16px root font size to 1600px.
    ExpectFlex({"100em", 1.0f, 1.0f, kPx, 100.0f});

    // `100.` is not a valid CSS number, so Chrome drops the declaration;
    // strtof accepts it.
    ExpectFlex({"100.px", 1.0f, 1.0f, kPx, 100.0f});

    // An omitted basis is 0% in Chrome and 0px here. StyleLength can express
    // both, but every consumer resolves either to 0 on the main axis, and the
    // existing FlexShorthandOneValue test pins the px form.
    ExpectFlex({"1", 1.0f, 1.0f, kPx, 0.0f});
}

TEST(CSSParserTests, AlignContentDirectionAspectRatio)
{
    const std::string css =
        "#c { align-content: space-between; direction: rtl; aspect-ratio: 16/9; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("c");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    // Enum values are internal; use indirect checks
    // Ensure aspect ratio approx 1.7777
    EXPECT_NEAR(s.Layout.AspectRatio, 16.0f/9.0f, 0.001f);
}

TEST(CSSParserTests, LaterRulesOverrideSameSpecificity)
{
    const std::string css =
        ".tab { width: 10px; }\n"
        ".tab { width: 20px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("tab");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 20.f, 0.01f);
}

TEST(CSSParserTests, ImportantBeatsHigherSpecificityNormalDeclaration)
{
    // The class rule has lower specificity than the id rule, but the class
    // rule's !important must still win over the id rule.
    const std::string css =
        "#root { width: 200px; }\n"
        ".btn { width: 100px !important; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 100.f, 0.01f);
}

TEST(CSSParserTests, ImportantVsImportantUsesSpecificityAndOrder)
{
    // Both rules use !important: the higher-specificity (id) one must win
    // even though it appears first.
    const std::string css =
        "#root { width: 200px !important; }\n"
        ".btn { width: 100px !important; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 200.f, 0.01f);
}

TEST(CSSParserTests, ImportantStrippedFromColorValue)
{
    // Regression for the inspector.css note about "transparent !important":
    // the value must parse as the named color, not as a literal failed parse.
    const std::string css =
        ".btn { background-color: transparent !important; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    // transparent == 0x00000000
    EXPECT_EQ(s.Visual.BackgroundColor, 0x00000000u);
}

TEST(CSSParserTests, ImportantPropagatesThroughShorthand)
{
    // The shorthand `border` expands to multiple longhands; the !important
    // marker must apply to every longhand so a non-important longhand
    // override in a higher-specificity rule does not win.
    const std::string css =
        "#root { border-top-width: 5px; }\n"
        ".btn { border: 1px solid red !important; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_NEAR(s.Layout.Padding.Top, 0.f, 0.01f); // unrelated, just smoke
    EXPECT_NEAR(s.Layout.BorderWidth.Top, 1.f, 0.01f);
}

TEST(CSSParserTests, ImportantAppliesToInlineCustomVarConsumer)
{
    // Sanity: !important on a normal property inside a rule that uses a CSS
    // variable (DeferredDecl path) must still win over a higher-specificity
    // normal declaration.
    const std::string css =
        ":root { --w: 100px; }\n"
        "#root { width: 200px; }\n"
        ".btn { width: var(--w) !important; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 100.f, 0.01f);
}

TEST(CSSParserTests, ImportantOnCustomPropertyBeatsNormalCustomProperty)
{
    // Regression for audit finding B1: a low-specificity !important
    // declaration on a custom property (--foo) must beat a higher-specificity
    // normal declaration of the same variable.
    const std::string css =
        ".btn { --w: 100px !important; }\n"
        "#root { --w: 200px; }\n"
        ".btn { width: var(--w); }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 100.f, 0.01f);
}

TEST(CSSParserTests, ImportantOnCustomPropertyVsImportantUsesCascadeOrder)
{
    // Two !important declarations of the same custom property: the
    // higher-specificity one must win (id beats class).
    const std::string css =
        ".btn { --w: 100px !important; }\n"
        "#root { --w: 200px !important; }\n"
        ".btn { width: var(--w); }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.SetId("root");
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 200.f, 0.01f);
}

TEST(CSSParserTests, ImportantStrippedFromUnknownProperty)
{
    // Regression for audit finding B3: an unknown / unparseable property
    // declaration with !important must not pollute downstream parsing or
    // crash the cascade. The parser should still produce a valid stylesheet
    // and other rules in the same sheet should resolve correctly.
    const std::string css =
        ".btn { not-a-real-property: 5px !important; width: 50px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("btn");

    ElementState st{};
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 50.f, 0.01f);
}

TEST(CSSParserTests, InherentKeywordWithImportantWins)
{
    // CSS-wide keywords (inherit/initial/unset) and !important are
    // orthogonal. A child's `width: inherit !important` must beat a
    // higher-specificity normal `width: 200px` set on the child.
    const std::string css =
        "#parent { width: 50px; }\n"
        "#child.high-spec { width: 200px; }\n"
        ".child-class { width: inherit !important; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement parent;
    parent.SetId("parent");
    UIElement child;
    child.SetId("child");
    child.AddClass("high-spec");
    child.AddClass("child-class");

    ElementState st{};
    auto parentStyle = CSSParser::ComputeStyleFor(parent, sheet, st);
    std::vector<const Stylesheet*> sheets{&sheet};
    auto childStyle = CSSParser::ComputeStyleFor(child, sheets, st, &parentStyle);

    EXPECT_TRUE(childStyle.Layout.Width.IsPx());
    EXPECT_NEAR(childStyle.Layout.Width.Value, 50.f, 0.01f);
}

TEST(CSSParserTests, InlineStyleImportantMarkerStrippedFromStoredValue)
{
    // White-box: inline style="width: 50px !important" must store the parsed
    // 50px value, not store the literal marker. (Inline overrides currently
    // always win, so the importance bit is dropped — but the value must
    // still parse cleanly.)
    UIElement el;
    StyleOverrides ov;
    CSSParser::ParseInlineStyleToOverrides("width: 50px !important", ov);

    // The override should be present with the parsed numeric value.
    EXPECT_TRUE(ov.Has(StylePropertyId::Width));

    StyleOverrides ovEmpty;
    // A value that's only the marker should be skipped entirely.
    CSSParser::ParseInlineStyleToOverrides("width: !important", ovEmpty);
    EXPECT_FALSE(ovEmpty.Has(StylePropertyId::Width));
}

TEST(CSSParserTests, MultiStylesheetCascadeLaterSheetWins)
{
    const std::string css1 = ".tab { width: 10px; }\n";
    const std::string css2 = ".tab { width: 20px; }\n";

    Stylesheet sheet1{};
    Stylesheet sheet2{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css1, sheet1));
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css2, sheet2));

    UIElement el;
    el.AddClass("tab");

    ElementState st{};
    std::vector<const Stylesheet*> sheets;
    sheets.push_back(&sheet1);
    sheets.push_back(&sheet2);

    auto s = CSSParser::ComputeStyleFor(el, sheets, st);
    EXPECT_TRUE(s.Layout.Width.IsPx());
    EXPECT_NEAR(s.Layout.Width.Value, 20.f, 0.01f);
}


TEST(CSSParserTests, RootPseudoAndClassCascade)
{
	const std::string css =
	    ".root { padding: 4px; }\n"
	    ":root { padding: 12px; }\n";

	Stylesheet sheet{};
	ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

	// Root element with class "root" so both .root and :root match.
	UIElement root;
	root.AddClass("root");

	ElementState st{};
	auto s = CSSParser::ComputeStyleFor(root, sheet, st);

	// :root appears later in source and has the same specificity as .root,
	// so it should win for padding.
	EXPECT_NEAR(s.Layout.Padding.Left,   12.f, 0.01f);
	EXPECT_NEAR(s.Layout.Padding.Top,    12.f, 0.01f);
	EXPECT_NEAR(s.Layout.Padding.Right,  12.f, 0.01f);
	EXPECT_NEAR(s.Layout.Padding.Bottom, 12.f, 0.01f);
}


TEST(CSSParserTests, ParsesPercentageWidthAndHeight)
{
    const std::string css =
        "#a { width: 100%; }\n"
        "#b { height: 50%; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement elA; elA.SetId("a");
    UIElement elB; elB.SetId("b");

    auto sa = CSSParser::ComputeStyleFor(elA, sheet, st);
    auto sb = CSSParser::ComputeStyleFor(elB, sheet, st);

    EXPECT_TRUE(sa.Layout.Width.IsPercent());
    EXPECT_NEAR(sa.Layout.Width.Value, 100.0f, 0.001f);
    EXPECT_TRUE(sb.Layout.Height.IsPercent());
    EXPECT_NEAR(sb.Layout.Height.Value, 50.0f, 0.001f);
}


TEST(CSSParserTests, ParsesBackgroundImageAndSupportingProperties)
{
    const std::string css =
        "#bg { background-image: url(guid:00000000-0000-0000-0000-000000000001);\n"
        "      background-repeat: repeat-x;\n"
        "      background-size: 50% auto;\n"
        "      background-position: right bottom; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("bg");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Guid);
    EXPECT_FALSE(s.Visual.BackgroundImage.Source.Guid.IsNull());
    EXPECT_EQ(s.Visual.BackgroundImage.Repeat, BackgroundRepeat::RepeatX);
    EXPECT_EQ(s.Visual.BackgroundImage.SizeMode, BackgroundSizeMode::Explicit);
    EXPECT_TRUE(s.Visual.BackgroundImage.SizeXIsPercent);
    EXPECT_NEAR(s.Visual.BackgroundImage.SizeX, 50.0f, 0.001f);
    EXPECT_LT(s.Visual.BackgroundImage.SizeY, 0.0f);
    EXPECT_TRUE(s.Visual.BackgroundImage.PosXIsPercent);
    EXPECT_NEAR(s.Visual.BackgroundImage.PosX, 100.0f, 0.001f);
    EXPECT_TRUE(s.Visual.BackgroundImage.PosYIsPercent);
    EXPECT_NEAR(s.Visual.BackgroundImage.PosY, 100.0f, 0.001f);
}


TEST(CSSParserTests, ParsesBackgroundTintColorWithAlpha)
{
		    const std::string css =
		        "#bg { background-image: url(\"textures/icon.png\");\n"
		        "      background-tint: rgba(255,128,0,0.5); }\n";
		    Stylesheet sheet{};
		    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

		    ElementState st{};
		    UIElement el; el.SetId("bg");
		    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

		    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
		    EXPECT_TRUE(s.Visual.BackgroundImage.HasTint);
		    EXPECT_EQ(s.Visual.BackgroundImage.Tint, 0x80FF8000u); // A=0.5, RGB=(255,128,0)
		}

		TEST(CSSParserTests, ParsesBackgroundImageTintAlias)
		{
		    const std::string css =
		        "#bg { background-image: url(\"textures/icon_alias.png\");\n"
		        "      background-image-tint: rgba(10,20,30,0.25); }\n";
		    Stylesheet sheet{};
		    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

		    ElementState st{};
		    UIElement el; el.SetId("bg");
		    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

		    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
		    EXPECT_TRUE(s.Visual.BackgroundImage.HasTint);
		    EXPECT_EQ(s.Visual.BackgroundImage.Tint, 0x400A141Eu); // A=0.25, RGB=(10,20,30)
		}

		TEST(CSSParserTests, ParsesBackgroundImageTintTransparent)
		{
		    const std::string css =
		        "#bg { background-image: url(\"textures/icon.png\");\n"
		        "      background-image-tint: transparent; }\n";
		    Stylesheet sheet{};
		    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

		    ElementState st{};
		    UIElement el; el.SetId("bg");
		    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

		    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
		    EXPECT_TRUE(s.Visual.BackgroundImage.HasTint);
		    EXPECT_EQ(s.Visual.BackgroundImage.Tint, 0x00000000u);
		}


		TEST(CSSParserTests, ParsesBackgroundImagePathUrl)
{
    const std::string css =
        "#bg { background-image: url(\"textures/panel.png\"); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("bg");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("textures/panel.png"));
}

TEST(CSSParserTests, ParsesBackgroundImageAssetScheme)
{
    const std::string css =
        "#bg { background-image: url(asset:textures/panel.png); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("bg");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("asset:textures/panel.png"));
}

TEST(CSSParserTests, BackgroundImageColonAliasPrefix)
{
    // CSS `url("editor:Icons/Sphere.png")` should set sourceAlias=editor and
    // strip the prefix from value — lets editor stylesheets pin paths to the
    // editor mount even if a project asset shadows the same name.
    const std::string css =
        "#a { background-image: url(\"editor:Icons/Sphere.png\"); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("a");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("Icons/Sphere.png"));
    EXPECT_EQ(s.Visual.BackgroundImage.Source.SourceAlias, std::string("editor"));
}

TEST(CSSParserTests, BackgroundImageColonAliasRootedPath)
{
    // The rooted spelling `url("editor:/Icons/Sphere.png")` means "root of the
    // editor source". The parser must store a source-relative value: a kept
    // leading '/' would resolve at the drive root on Windows and read as a
    // genuine absolute path on POSIX, bypassing source resolution.
    const std::string css =
        "#a { background-image: url(\"editor:/Icons/Sphere.png\"); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("a");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("Icons/Sphere.png"));
    EXPECT_EQ(s.Visual.BackgroundImage.Source.SourceAlias, std::string("editor"));
}

TEST(CSSParserTests, BackgroundImageAtAliasPrefix)
{
    // CSS `url("@editor/Icons/Sphere.png")` is equivalent to the colon
    // form. Useful for paths whose first segment would otherwise look
    // like a Windows drive letter or a reserved scheme.
    const std::string css =
        "#a { background-image: url(\"@editor/Icons/Sphere.png\"); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("a");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("Icons/Sphere.png"));
    EXPECT_EQ(s.Visual.BackgroundImage.Source.SourceAlias, std::string("editor"));
}

TEST(CSSParserTests, ShorthandCssWideKeywordFanOut)
{
    // A CSS-wide keyword on a shorthand fans out to its longhand targets as
    // keyword-only properties resolved at cascade time.
    const std::string css =
        "#a { border: inherit; }\n"
        "#b { flex: initial; }\n"
        "#c { overflow: unset; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ASSERT_EQ(sheet.Rules.size(), 3u);

    const auto& border = sheet.Rules[0].Properties;
    ASSERT_EQ(border.size(), 9u);
    const StylePropertyId borderTargets[] = {
        StylePropertyId::BorderTopWidth, StylePropertyId::BorderRightWidth,
        StylePropertyId::BorderBottomWidth, StylePropertyId::BorderLeftWidth,
        StylePropertyId::BorderTopColor, StylePropertyId::BorderRightColor,
        StylePropertyId::BorderBottomColor, StylePropertyId::BorderLeftColor,
        StylePropertyId::BorderStyle};
    for (size_t i = 0; i < border.size(); ++i)
    {
        EXPECT_EQ(border[i].PropertyId, borderTargets[i]) << "index " << i;
        EXPECT_EQ(border[i].Keyword, StyleKeyword::Inherit) << "index " << i;
    }

    const auto& flex = sheet.Rules[1].Properties;
    ASSERT_EQ(flex.size(), 3u);
    EXPECT_EQ(flex[0].PropertyId, StylePropertyId::FlexGrow);
    EXPECT_EQ(flex[1].PropertyId, StylePropertyId::FlexShrink);
    EXPECT_EQ(flex[2].PropertyId, StylePropertyId::FlexBasis);
    for (const auto& p : flex)
        EXPECT_EQ(p.Keyword, StyleKeyword::Initial);

    const auto& overflow = sheet.Rules[2].Properties;
    ASSERT_EQ(overflow.size(), 3u);
    EXPECT_EQ(overflow[0].PropertyId, StylePropertyId::Overflow);
    EXPECT_EQ(overflow[1].PropertyId, StylePropertyId::OverflowX);
    EXPECT_EQ(overflow[2].PropertyId, StylePropertyId::OverflowY);
    for (const auto& p : overflow)
        EXPECT_EQ(p.Keyword, StyleKeyword::Unset);
}

TEST(CSSParserTests, BackgroundImageReservedSchemesNotEatenAsAlias)
{
    // Reserved URI schemes (asset, data, file, http, https) must NOT be
    // misinterpreted as asset-source aliases. Regression guard for the
    // alias-prefix detector added alongside source routing.
    const std::string css =
        "#a { background-image: url(\"asset:textures/p.png\"); }\n"
        "#b { background-image: url(\"data:image/png;base64,abc\"); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    {
        UIElement el; el.SetId("a");
        auto s = CSSParser::ComputeStyleFor(el, sheet, st);
        EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("asset:textures/p.png"));
        EXPECT_TRUE(s.Visual.BackgroundImage.Source.SourceAlias.empty());
    }
    {
        UIElement el; el.SetId("b");
        auto s = CSSParser::ComputeStyleFor(el, sheet, st);
        // data: URIs aren't a real path either, but the parser keeps them
        // intact so downstream can choose to handle or ignore them.
        EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("data:image/png;base64,abc"));
        EXPECT_TRUE(s.Visual.BackgroundImage.Source.SourceAlias.empty());
    }
}


TEST(CSSParserTests, UrlQuotingVariants)
{
    const std::string css =
        "#u1 { background-image: url(textures/a.png); }\n"
        "#u2 { background-image: url('textures/a.png'); }\n"
        "#u3 { background-image: url(\"textures/a.png\"); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement e1; e1.SetId("u1");
    UIElement e2; e2.SetId("u2");
    UIElement e3; e3.SetId("u3");

    auto s1 = CSSParser::ComputeStyleFor(e1, sheet, st);
    auto s2 = CSSParser::ComputeStyleFor(e2, sheet, st);
    auto s3 = CSSParser::ComputeStyleFor(e3, sheet, st);

    EXPECT_TRUE(s1.Visual.BackgroundImage.HasImage);
    EXPECT_TRUE(s2.Visual.BackgroundImage.HasImage);
    EXPECT_TRUE(s3.Visual.BackgroundImage.HasImage);

    EXPECT_EQ(s1.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s2.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s3.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);

    EXPECT_EQ(s1.Visual.BackgroundImage.Source.Value, std::string("textures/a.png"));
    EXPECT_EQ(s2.Visual.BackgroundImage.Source.Value, std::string("textures/a.png"));
    EXPECT_EQ(s3.Visual.BackgroundImage.Source.Value, std::string("textures/a.png"));
}

TEST(CSSParserTests, LeadingSlashNotStrippedInParse)
{
    const std::string css =
        "#bg { background-image: url(/textures/panel.png); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("bg");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(s.Visual.BackgroundImage.HasImage);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(s.Visual.BackgroundImage.Source.Value, std::string("/textures/panel.png"));
}


TEST(CSSParserTests, CustomPropertiesAndTypedGetters)
{
    const std::string css =
        "#e { --num: 12.5px; --pct: 33%; --col1: #336699; --col2: 0xAABBCCDD; --str: hello; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("e");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    // Number parsing strips px/% and converts to float
    ASSERT_TRUE(s.GetCustomNumber(HashStringId("--num")).has_value());
    EXPECT_NEAR(*s.GetCustomNumber(HashStringId("--num")), 12.5f, 0.001f);
    ASSERT_TRUE(s.GetCustomNumber(HashStringId("--pct")).has_value());
    EXPECT_NEAR(*s.GetCustomNumber(HashStringId("--pct")), 33.0f, 0.001f);

    // Colors: #RRGGBB -> 0xFFRRGGBB, 0xAARRGGBB passes through
    ASSERT_TRUE(s.GetCustomColor(HashStringId("--col1")).has_value());
    EXPECT_EQ(*s.GetCustomColor(HashStringId("--col1")), 0xFF336699u);
    ASSERT_TRUE(s.GetCustomColor(HashStringId("--col2")).has_value());
    EXPECT_EQ(*s.GetCustomColor(HashStringId("--col2")), 0xAABBCCDDu);

    // Strings
    ASSERT_TRUE(s.GetCustomString(HashStringId("--str")).has_value());
    EXPECT_EQ(*s.GetCustomString(HashStringId("--str")), std::string("hello"));

    // Typed access with defaults via value_or
    EXPECT_NEAR(s.GetCustomNumber(HashStringId("--num")).value_or(-1.0f), 12.5f, 0.001f);
    EXPECT_NEAR(s.GetCustomNumber(HashStringId("--pct")).value_or(-1.0f), 33.0f, 0.001f);
    EXPECT_EQ(s.GetCustomColor(HashStringId("--col1")).value_or(0u), 0xFF336699u);
    EXPECT_EQ(s.GetCustomColor(HashStringId("--missingColor")).value_or(0x12345678u), 0x12345678u);
    EXPECT_EQ(s.GetCustomString(HashStringId("--str")).value_or(std::string("def")), std::string("hello"));
    EXPECT_EQ(s.GetCustomString(HashStringId("--missingStr")).value_or(std::string("def")), std::string("def"));
}

// A typed getter substitutes var() in a custom property's value the way the cascade does for a
// real property that uses it: references resolve against the reading element's scope, inherited
// declarations included, so C++ reading a variable and CSS using it never disagree.
TEST(CSSParserTests, TypedGettersSubstituteVarReferences)
{
    const std::string css =
        "#parent { --token: #112233; --size: 7px; --alias: var(--token); }\n"
        "#child { --chain: var(--alias); --fallback: var(--missing, #445566); --broken: var(--missing);"
        " --length: var(--size); --text: var(--word); --word: hi; }\n"
        "#redefining { --token: #778899; border-top-color: var(--alias); }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    const std::vector<const Stylesheet*> sheets{&sheet};

    ElementState st{};
    UIElement parent; parent.SetId("parent");
    UIElement child; child.SetId("child");
    UIElement redefining; redefining.SetId("redefining");
    const ResolvedStyle parentStyle =
        CSSParser::ComputeStyleFor(parent, sheets, st, static_cast<const ResolvedStyle*>(nullptr));
    const ResolvedStyle childStyle = CSSParser::ComputeStyleFor(child, sheets, st, &parentStyle);
    const ResolvedStyle redefiningStyle = CSSParser::ComputeStyleFor(redefining, sheets, st, &parentStyle);

    EXPECT_EQ(childStyle.GetCustomColor(HashStringId("--alias")).value_or(0u), 0xFF112233u);
    EXPECT_EQ(childStyle.GetCustomColor(HashStringId("--chain")).value_or(0u), 0xFF112233u);
    EXPECT_EQ(childStyle.GetCustomColor(HashStringId("--fallback")).value_or(0u), 0xFF445566u);
    EXPECT_FALSE(childStyle.GetCustomColor(HashStringId("--broken")).has_value());
    EXPECT_NEAR(childStyle.GetCustomNumber(HashStringId("--length")).value_or(-1.0f), 7.0f, 0.001f);
    EXPECT_EQ(childStyle.GetCustomString(HashStringId("--text")).value_or(""), "hi");

    // Both children read the parent's one --alias entry. The redefining child's own real
    // property shows what the cascade makes of it; the getter must agree, and a value cached for
    // one child must not be served to the other.
    ASSERT_EQ(redefiningStyle.Visual.BorderColor.Top, 0xFF778899u);
    EXPECT_EQ(redefiningStyle.GetCustomColor(HashStringId("--alias")).value_or(0u), 0xFF778899u);
    EXPECT_EQ(childStyle.GetCustomColor(HashStringId("--alias")).value_or(0u), 0xFF112233u);
    EXPECT_EQ(redefiningStyle.GetCustomColor(HashStringId("--alias")).value_or(0u), 0xFF778899u);
}

// A typed colour getter reads every colour form a real colour property does, so a custom
// property a control paints from (a slider's track) takes a named colour or rgb() like
// background-color does.
TEST(CSSParserTests, TypedColorGetterReadsNamedAndFunctionalColors)
{
    const std::string css =
        "#e { --named: red; --functional: rgb(0, 255, 0); --alpha: rgba(0, 0, 255, 0.5);"
        " --hsl: hsl(120, 100%, 50%); --token: lime; --alias: var(--token); --bad: notacolor; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ElementState st{};
    UIElement el; el.SetId("e");
    const ResolvedStyle s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_EQ(s.GetCustomColor(HashStringId("--named")).value_or(0u), 0xFFFF0000u);
    EXPECT_EQ(s.GetCustomColor(HashStringId("--functional")).value_or(0u), 0xFF00FF00u);
    EXPECT_EQ(s.GetCustomColor(HashStringId("--alpha")).value_or(0u), 0x800000FFu);
    EXPECT_EQ(s.GetCustomColor(HashStringId("--hsl")).value_or(0u), 0xFF00FF00u);
    EXPECT_EQ(s.GetCustomColor(HashStringId("--alias")).value_or(0u), 0xFF00FF00u);
    EXPECT_FALSE(s.GetCustomColor(HashStringId("--bad")).has_value());
}

TEST(CSSParserTests, VarSubstitution_LonghandsAndInRuleOrder)
{
    const std::string css = R"(
        #e {
            color: var(--c);
            width: var(--w);
            --c: #336699;
            --w: 10px;
        }
    )";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("e");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_EQ(s.Visual.Color, 0xFF336699u);
    EXPECT_TRUE(s.Visual.HasColor);
    EXPECT_EQ(s.Layout.Width.Unit, StyleLength::UnitType::Px);
    EXPECT_NEAR(s.Layout.Width.Value, 10.f, 0.01f);
}

TEST(CSSParserTests, VarSubstitution_FallbackAndCycles)
{
    {
        const std::string css = R"(
            #e { color: var(--missing, #112233); }
        )";
        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

        ElementState st{};
        UIElement el; el.SetId("e");
        auto s = CSSParser::ComputeStyleFor(el, sheet, st);
        EXPECT_EQ(s.Visual.Color, 0xFF112233u);
    }

    {
        const std::string css = R"(
            #e {
                --a: var(--b);
                --b: var(--a);
                color: var(--a, #010203);
            }
        )";
        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

        ElementState st{};
        UIElement el; el.SetId("e");
        auto s = CSSParser::ComputeStyleFor(el, sheet, st);
        EXPECT_EQ(s.Visual.Color, 0xFF010203u);
    }
}

TEST(CSSParserTests, VarSubstitution_InBorderShorthand)
{
    const std::string css = R"(
        #e {
            --bw: 3px;
            --c: #112233;
            border: var(--bw) solid var(--c);
        }
    )";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("e");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_NEAR(s.Layout.BorderWidth.Top, 3.0f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Right, 3.0f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Bottom, 3.0f, 0.01f);
    EXPECT_NEAR(s.Layout.BorderWidth.Left, 3.0f, 0.01f);

    EXPECT_EQ(s.Visual.BorderColor.Top, 0xFF112233u);
    EXPECT_EQ(s.Visual.BorderColor.Right, 0xFF112233u);
    EXPECT_EQ(s.Visual.BorderColor.Bottom, 0xFF112233u);
    EXPECT_EQ(s.Visual.BorderColor.Left, 0xFF112233u);
}

TEST(CSSParserTests, CustomVarInheritsByDefaultAndVarUsesInheritedValue)
{
    const std::string css = R"(
        #parent { --c: #112233; }
        #child { color: var(--c); }
    )";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};

    UIElement parent; parent.SetId("parent");
    const auto parentStyle = CSSParser::ComputeStyleFor(parent, sheet, st);

    UIElement child; child.SetId("child");
    const auto childStyle = CSSParser::ComputeStyleFor(child, std::vector<const Stylesheet*>{&sheet}, st, &parentStyle);

    EXPECT_EQ(childStyle.Visual.Color, 0xFF112233u);
}

TEST(CSSParserTests, CustomVarStyleDeltaOverridesParticipateInVarResolution)
{
    const std::string css = R"(
        #e { color: var(--c); }
        #child { color: var(--c); }
    )";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("e");
    el.Overrides().SetCustom(HashStringId("--c"), "#112233");
    const auto parentStyle = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_EQ(parentStyle.Visual.Color, 0xFF112233u);

    UIElement child; child.SetId("child");
    const auto childStyle = CSSParser::ComputeStyleFor(child, std::vector<const Stylesheet*>{&sheet}, st, &parentStyle);
    EXPECT_EQ(childStyle.Visual.Color, 0xFF112233u);
}


TEST(CSSParserTests, ParsesDirectionRTLFlag)
{
    const std::string css =
        "#dir { direction: rtl; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement el; el.SetId("dir");
    auto s = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_EQ(s.Layout.Direction, Direction::RTL);
}

TEST(CSSParserTests, ParsesOrderAndPositionProperties)
{
    const std::string css =
        "#p { position: absolute; left: 10px; top: 20%; right: 5%; bottom: 4px; }\n"
        "#o { order: -2; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement elP; elP.SetId("p");
    UIElement elO; elO.SetId("o");

    auto sp = CSSParser::ComputeStyleFor(elP, sheet, st);
    auto so = CSSParser::ComputeStyleFor(elO, sheet, st);

    EXPECT_EQ(sp.Layout.PositionType, PositionType::Absolute);
    EXPECT_EQ(sp.Layout.PositionLeft.Unit, StyleLength::UnitType::Px);
    EXPECT_NEAR(sp.Layout.PositionLeft.Value, 10.f, 0.01f);
    EXPECT_EQ(sp.Layout.PositionTop.Unit, StyleLength::UnitType::Percent);
    EXPECT_NEAR(sp.Layout.PositionTop.Value, 20.f, 0.01f);
    EXPECT_EQ(sp.Layout.PositionRight.Unit, StyleLength::UnitType::Percent);
    EXPECT_NEAR(sp.Layout.PositionRight.Value, 5.f, 0.01f);
    EXPECT_EQ(sp.Layout.PositionBottom.Unit, StyleLength::UnitType::Px);
    EXPECT_NEAR(sp.Layout.PositionBottom.Value, 4.f, 0.01f);

    EXPECT_EQ(so.Layout.Order, -2);
}


TEST(CSSParserTests, ParsesZIndexIntegerFloatAndInvalid)
{
    const std::string css =
        "#a { z-index: 5; }\n"
        "#b { z-index: -3; }\n"
        "#c { z-index: 1.7; }\n"
        "#d { z-index: foo; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    ElementState st{};
    UIElement a; a.SetId("a");
    UIElement b; b.SetId("b");
    UIElement c; c.SetId("c");
    UIElement d; d.SetId("d");

    auto sa = CSSParser::ComputeStyleFor(a, sheet, st);
    auto sb = CSSParser::ComputeStyleFor(b, sheet, st);
    auto sc = CSSParser::ComputeStyleFor(c, sheet, st);
    auto sd = CSSParser::ComputeStyleFor(d, sheet, st);

    EXPECT_EQ(sa.Layout.ZIndex, 5);
    EXPECT_EQ(sb.Layout.ZIndex, -3);
    EXPECT_EQ(sc.Layout.ZIndex, 2);  // 1.7 rounded to nearest int
    EXPECT_EQ(sd.Layout.ZIndex, 0);  // invalid value falls back to default
}


TEST(CSSParserTests, PseudoDisabledEnabledMatchesUsingState)
{
    UIRegistration::RegisterBuiltInControls();

    const char* css = R"(
        uielement:disabled { background-color: #112233; }
        uielement:enabled  { background-color: #334455; }
    )";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el; el.SetId("x");

    // Enabled case
    ElementState st1{}; st1.Enabled = true; st1.Disabled = false;
    auto s1 = CSSParser::ComputeStyleFor(el, sheet, st1);
    EXPECT_EQ(s1.Visual.BackgroundColor, 0xFF334455u);

    // Disabled case
    ElementState st2{}; st2.Enabled = false; st2.Disabled = true;
    auto s2 = CSSParser::ComputeStyleFor(el, sheet, st2);
    EXPECT_EQ(s2.Visual.BackgroundColor, 0xFF112233u);
}

TEST(CSSParserTests, CssWideKeywordInheritCopiesNonInheritedWidthFromParent)
{
    const std::string css =
        "#p { width: 123px; }\n"
        "#c { width: inherit; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement parent;
    parent.SetId("p");
    UIElement child;
    child.SetId("c");

    ElementState st{};
    const auto parentStyle = CSSParser::ComputeStyleFor(parent, sheet, st);

    std::vector<const Stylesheet*> sheets;
    sheets.push_back(&sheet);
    const auto childStyle = CSSParser::ComputeStyleFor(child, sheets, st, &parentStyle);

    EXPECT_TRUE(childStyle.Layout.Width.IsPx());
    EXPECT_NEAR(childStyle.Layout.Width.Value, 123.f, 0.01f);
}

TEST(CSSParserTests, CssWideKeywordInitialResetsWidthToAuto)
{
    const std::string css =
        "#c { width: 100px; width: initial; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement child;
    child.SetId("c");

    ElementState st{};
    const auto s = CSSParser::ComputeStyleFor(child, sheet, st);
    EXPECT_TRUE(s.Layout.Width.IsAuto());
}

TEST(CSSParserTests, CssWideKeywordUnsetInheritsColorButResetsWidth)
{
    const std::string css =
        "#p { color: #112233; width: 321px; }\n"
        "#c1 { color: unset; }\n"
        "#c2 { width: unset; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement parent;
    parent.SetId("p");
    UIElement c1;
    c1.SetId("c1");
    UIElement c2;
    c2.SetId("c2");

    ElementState st{};
    const auto parentStyle = CSSParser::ComputeStyleFor(parent, sheet, st);

    std::vector<const Stylesheet*> sheets;
    sheets.push_back(&sheet);

    const auto s1 = CSSParser::ComputeStyleFor(c1, sheets, st, &parentStyle);
    EXPECT_EQ(s1.Visual.Color, parentStyle.Visual.Color);
    EXPECT_TRUE(s1.Visual.HasColor); // unset is specified (for inherited-by-default properties it acts like inherit)

    const auto s2 = CSSParser::ComputeStyleFor(c2, sheets, st, &parentStyle);
    EXPECT_TRUE(s2.Layout.Width.IsAuto()); // unset acts like initial for non-inherited properties
}

TEST(CSSParserTests, CssWideKeywordDefaultInheritanceColorWhenUnspecified)
{
    const std::string css =
        "#p { color: #123456; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement parent;
    parent.SetId("p");
    UIElement child;
    child.SetId("c"); // no matching rule sets color

    ElementState st{};
    const auto parentStyle = CSSParser::ComputeStyleFor(parent, sheet, st);

    std::vector<const Stylesheet*> sheets;
    sheets.push_back(&sheet);
    const auto childStyle = CSSParser::ComputeStyleFor(child, sheets, st, &parentStyle);

    EXPECT_EQ(childStyle.Visual.Color, parentStyle.Visual.Color);
    EXPECT_FALSE(childStyle.Visual.HasColor); // inherited-by-default, but not explicitly specified
}

TEST(CSSParserTests, CssWideKeywordInheritOnShorthandsBorderWidthAndMargin)
{
    const std::string css =
        "#p { border-width: 1px 2px 3px 4px; margin: 10% 5%; }\n"
        "#c { border-width: inherit; margin: inherit; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement parent;
    parent.SetId("p");
    UIElement child;
    child.SetId("c");

    ElementState st{};
    const auto parentStyle = CSSParser::ComputeStyleFor(parent, sheet, st);

    std::vector<const Stylesheet*> sheets;
    sheets.push_back(&sheet);
    const auto childStyle = CSSParser::ComputeStyleFor(child, sheets, st, &parentStyle);

    EXPECT_NEAR(childStyle.Layout.BorderWidth.Top, 1.f, 0.01f);
    EXPECT_NEAR(childStyle.Layout.BorderWidth.Right, 2.f, 0.01f);
    EXPECT_NEAR(childStyle.Layout.BorderWidth.Bottom, 3.f, 0.01f);
    EXPECT_NEAR(childStyle.Layout.BorderWidth.Left, 4.f, 0.01f);

    EXPECT_NEAR(childStyle.Layout.Margin.Top, 10.f, 0.01f);
    EXPECT_NEAR(childStyle.Layout.Margin.Right, 5.f, 0.01f);
    EXPECT_NEAR(childStyle.Layout.Margin.Bottom, 10.f, 0.01f);
    EXPECT_NEAR(childStyle.Layout.Margin.Left, 5.f, 0.01f);
    EXPECT_TRUE(childStyle.Layout.MarginIsPercent.Top);
    EXPECT_TRUE(childStyle.Layout.MarginIsPercent.Right);
    EXPECT_TRUE(childStyle.Layout.MarginIsPercent.Bottom);
    EXPECT_TRUE(childStyle.Layout.MarginIsPercent.Left);
}

TEST(CSSParserTests, CssWideKeywordInheritFontSizeDoesNotBecomeZero)
{
    const std::string css =
        "#p { font-size: 14px; }\n"
        "#c { font-size: inherit; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement parent;
    parent.SetId("p");
    UIElement child;
    child.SetId("c");

    ElementState st{};
    const auto parentStyle = CSSParser::ComputeStyleFor(parent, sheet, st);

    std::vector<const Stylesheet*> sheets;
    sheets.push_back(&sheet);
    const auto childStyle = CSSParser::ComputeStyleFor(child, sheets, st, &parentStyle);

    EXPECT_NEAR(childStyle.Visual.FontSize, 14.f, 0.01f);
    EXPECT_TRUE(childStyle.Visual.HasFontSize);
}

// ---------------------------------------------------------------------------
// CSS type selector matching via typeid (Approach 2 — no GetTagName).
// ---------------------------------------------------------------------------

TEST(CSSParserTests, TypeSelectorMatchesRegisteredElement)
{
    UIRegistration::RegisterBuiltInControls();

    const std::string css = "button { width: 42px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    Button btn;
    Label lbl;

    ElementState st{};
    auto sBtn = CSSParser::ComputeStyleFor(btn, sheet, st);
    auto sLbl = CSSParser::ComputeStyleFor(lbl, sheet, st);

    // "button" rule applies only to Button, not Label.
    EXPECT_TRUE(sBtn.Layout.Width.IsPx());
    EXPECT_NEAR(sBtn.Layout.Width.Value, 42.f, 0.01f);
    EXPECT_FALSE(sLbl.Layout.Width.IsPx());
}

TEST(CSSParserTests, TypeSelectorCombinedWithClass)
{
    UIRegistration::RegisterBuiltInControls();

    const std::string css =
        "button.primary { width: 100px; }\n"
        "label.primary  { width: 200px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    Button btn;
    btn.AddClass("primary");
    Label lbl;
    lbl.AddClass("primary");

    ElementState st{};
    auto sBtn = CSSParser::ComputeStyleFor(btn, sheet, st);
    auto sLbl = CSSParser::ComputeStyleFor(lbl, sheet, st);

    EXPECT_TRUE(sBtn.Layout.Width.IsPx());
    EXPECT_NEAR(sBtn.Layout.Width.Value, 100.f, 0.01f);
    EXPECT_TRUE(sLbl.Layout.Width.IsPx());
    EXPECT_NEAR(sLbl.Layout.Width.Value, 200.f, 0.01f);
}

TEST(CSSParserTests, TypeSelectorDoesNotMatchDifferentType)
{
    UIRegistration::RegisterBuiltInControls();

    const std::string css = "slider { opacity: 0.25; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    Button btn;
    Slider sl;

    ElementState st{};
    auto sBtn = CSSParser::ComputeStyleFor(btn, sheet, st);
    auto sSl  = CSSParser::ComputeStyleFor(sl, sheet, st);

    // Only Slider should get opacity 0.25.
    EXPECT_NEAR(sSl.Visual.LocalOpacity, 0.25f, 0.01f);
    EXPECT_NEAR(sBtn.Visual.LocalOpacity, 1.0f, 0.01f);
}

TEST(CSSParserTests, NthOfTypeUsesTypeidForSiblings)
{
    UIRegistration::RegisterBuiltInControls();

    // :first-of-type should match the first Button child, not the first child overall.
    const std::string css = "button:first-of-type { width: 77px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement root;
    auto lbl = std::make_unique<Label>();
    auto btn1 = std::make_unique<Button>();
    auto btn2 = std::make_unique<Button>();
    Button* btn1Ptr = btn1.get();
    Button* btn2Ptr = btn2.get();
    root.AddChild(std::move(lbl));
    root.AddChild(std::move(btn1));
    root.AddChild(std::move(btn2));

    ElementState st{};
    auto s1 = CSSParser::ComputeStyleFor(*btn1Ptr, sheet, st);
    auto s2 = CSSParser::ComputeStyleFor(*btn2Ptr, sheet, st);

    // btn1 is the first Button among siblings → matches.
    EXPECT_TRUE(s1.Layout.Width.IsPx());
    EXPECT_NEAR(s1.Layout.Width.Value, 77.f, 0.01f);
    // btn2 is NOT the first Button → should not match.
    EXPECT_FALSE(s2.Layout.Width.IsPx());
}

TEST(CSSParserTests, LastOfTypeUsesTypeidForSiblings)
{
    UIRegistration::RegisterBuiltInControls();

    const std::string css = "button:last-of-type { height: 33px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement root;
    auto btn1 = std::make_unique<Button>();
    auto btn2 = std::make_unique<Button>();
    auto lbl = std::make_unique<Label>();
    Button* btn1Ptr = btn1.get();
    Button* btn2Ptr = btn2.get();
    root.AddChild(std::move(btn1));
    root.AddChild(std::move(btn2));
    root.AddChild(std::move(lbl));

    ElementState st{};
    auto s1 = CSSParser::ComputeStyleFor(*btn1Ptr, sheet, st);
    auto s2 = CSSParser::ComputeStyleFor(*btn2Ptr, sheet, st);

    // btn2 is the last Button among siblings (Label follows, but it's a different type).
    EXPECT_FALSE(s1.Layout.Height.IsPx());
    EXPECT_TRUE(s2.Layout.Height.IsPx());
    EXPECT_NEAR(s2.Layout.Height.Value, 33.f, 0.01f);
}

// ---------------------------------------------------------------------------
// StringId-based tag matching tests
// ---------------------------------------------------------------------------

TEST(CSSParserTests, ParsedSelectorHasCorrectResolvedTagId)
{
    UIRegistration::RegisterBuiltInControls();

    const std::string css = "button { width: 10px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ASSERT_EQ(sheet.Rules.size(), 1u);

    const auto& compound = sheet.Rules[0].Selector.Terms.back().Selector;
    EXPECT_EQ(compound.Tag, "button");
    // ResolvedTagId must be the HashStringId of the canonical lowercase tag.
    EXPECT_EQ(compound.ResolvedTagId, HashStringId("button"));
    EXPECT_NE(compound.ResolvedTagId, StringId(0));
}

TEST(CSSParserTests, ParsedSelectorUnknownTagHasZeroResolvedTagId)
{
    const std::string css = "nonexistentwidget { width: 10px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ASSERT_EQ(sheet.Rules.size(), 1u);

    const auto& compound = sheet.Rules[0].Selector.Terms.back().Selector;
    EXPECT_EQ(compound.Tag, "nonexistentwidget");
    // Unknown tag must not resolve.
    EXPECT_EQ(compound.ResolvedTagId, StringId(0));
}

TEST(CSSParserTests, UnresolvedTagRuleDoesNotMatchAnyElement)
{
    UIRegistration::RegisterBuiltInControls();

    // "foobar" is not a registered element type. A rule targeting it must NOT
    // leak into matching other elements (the old universal-bucket bug).
    const std::string css =
        "foobar { width: 999px; }\n"
        ".real  { height: 50px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    Button btn;
    btn.AddClass("real");
    Label lbl;
    lbl.AddClass("real");
    UIElement el;
    el.AddClass("real");

    ElementState st{};
    auto sBtn = CSSParser::ComputeStyleFor(btn, sheet, st);
    auto sLbl = CSSParser::ComputeStyleFor(lbl, sheet, st);
    auto sEl  = CSSParser::ComputeStyleFor(el, sheet, st);

    // None of the elements should pick up width: 999px from the unknown tag.
    EXPECT_FALSE(sBtn.Layout.Width.IsPx());
    EXPECT_FALSE(sLbl.Layout.Width.IsPx());
    EXPECT_FALSE(sEl.Layout.Width.IsPx());

    // But the class rule must still apply.
    EXPECT_TRUE(sBtn.Layout.Height.IsPx());
    EXPECT_NEAR(sBtn.Layout.Height.Value, 50.f, 0.01f);
    EXPECT_TRUE(sLbl.Layout.Height.IsPx());
    EXPECT_NEAR(sLbl.Layout.Height.Value, 50.f, 0.01f);
}

TEST(CSSParserTests, UnresolvedTagWithPseudoDoesNotLeakToOtherElements)
{
    UIRegistration::RegisterBuiltInControls();

    // Regression test for the double-background bug: a rule like
    // "bogustag:checked { ... }" must not apply to checked elements of
    // other types when "bogustag" doesn't resolve.
    const std::string css = "bogustag:checked { opacity: 0.1; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    Toggle toggle;
    Checkbox cb;

    ElementState stChecked{};
    stChecked.Checked = true;

    auto sTgl = CSSParser::ComputeStyleFor(toggle, sheet, stChecked);
    auto sCb  = CSSParser::ComputeStyleFor(cb, sheet, stChecked);

    // Neither should match -- "bogustag" is not their type.
    EXPECT_NEAR(sTgl.Visual.LocalOpacity, 1.0f, 0.01f);
    EXPECT_NEAR(sCb.Visual.LocalOpacity, 1.0f, 0.01f);
}

TEST(CSSParserTests, TagSelectorWithCheckedPseudoOnlyMatchesCorrectType)
{
    UIRegistration::RegisterBuiltInControls();

    // "toggle:checked" should only style Toggle elements, not Checkbox.
    const std::string css =
        "toggle:checked { width: 200px; }\n"
        "checkbox:checked { width: 300px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    Toggle toggle;
    Checkbox cb;

    ElementState stChecked{};
    stChecked.Checked = true;

    auto sTgl = CSSParser::ComputeStyleFor(toggle, sheet, stChecked);
    auto sCb  = CSSParser::ComputeStyleFor(cb, sheet, stChecked);

    EXPECT_TRUE(sTgl.Layout.Width.IsPx());
    EXPECT_NEAR(sTgl.Layout.Width.Value, 200.f, 0.01f);
    EXPECT_TRUE(sCb.Layout.Width.IsPx());
    EXPECT_NEAR(sCb.Layout.Width.Value, 300.f, 0.01f);
}

TEST(CSSParserTests, TagSelectorAliasMatchesSameAsCanonical)
{
    UIRegistration::RegisterBuiltInControls();

    // "input" is a registered alias for "textfield". Rules using the alias
    // must match the same elements as rules using the canonical name.
    const std::string css =
        "input { width: 55px; }\n";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ASSERT_EQ(sheet.Rules.size(), 1u);

    const auto& compound = sheet.Rules[0].Selector.Terms.back().Selector;
    // The resolved id should equal the canonical tag's id.
    StringId canonicalId = UIRegistration::ElementFactoryRegistry::Instance().GetTagId("textfield");
    EXPECT_NE(canonicalId, StringId(0));
    EXPECT_EQ(compound.ResolvedTagId, canonicalId);
}

// ---------------------------------------------------------------------------
// :focus-within / :focus-visible support (lexbor overlay patch + matcher).
//
// Before the vendored lexbor overlay patch, lexbor's selector state machine
// routed :focus-within and :focus-visible into a "not supported" branch that
// failed the whole selector, so every rule using them was silently discarded
// and never reached the runtime Stylesheet. These pin that they now survive
// parsing and that the matcher evaluates them for both target and ancestor
// compounds.
// ---------------------------------------------------------------------------

namespace {
// True if any parsed rule's selector references the given pseudo kind.
bool SheetHasPseudoKind(const Stylesheet& sheet, PseudoClass::Kind kind) {
    for (const auto& rule : sheet.Rules)
        for (const auto& term : rule.Selector.Terms)
            for (const auto& pseudo : term.Selector.Pseudos)
                if (pseudo.PseudoKind == kind)
                    return true;
    return false;
}
} // namespace

TEST(CSSParserTests, FocusWithinAndFocusVisibleRulesSurviveParsing) {
    const std::string css =
        ".a:focus-within { width: 1px; }\n"
        ".b:focus-visible { width: 2px; }\n"
        ".c:focus { width: 3px; }\n"
        ".bar:focus-within .icon { width: 4px; }\n";

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    // All four rules must survive: the lexbor rejection used to drop the two
    // :focus-within and one :focus-visible rules, leaving only `.c:focus`.
    EXPECT_EQ(sheet.Rules.size(), 4u)
        << "focus-within/focus-visible rules must not be discarded by lexbor";
    EXPECT_TRUE(SheetHasPseudoKind(sheet, PseudoClass::Kind::FocusWithin));
    EXPECT_TRUE(SheetHasPseudoKind(sheet, PseudoClass::Kind::FocusVisible));
    EXPECT_TRUE(SheetHasPseudoKind(sheet, PseudoClass::Kind::Focus));
}

TEST(CSSParserTests, FocusWithinTargetFormUsesElementState) {
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".field { width: 10px; }\n"
        ".field:focus-within { width: 100px; }\n", sheet));

    UIElement el;
    el.AddClass("field");

    ElementState st{};
    st.FocusWithin = false;
    auto sBlur = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(sBlur.Layout.Width.IsPx());
    EXPECT_NEAR(sBlur.Layout.Width.Value, 10.f, 0.01f);

    st.FocusWithin = true;
    auto sFocus = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(sFocus.Layout.Width.IsPx());
    EXPECT_NEAR(sFocus.Layout.Width.Value, 100.f, 0.01f);
}

TEST(CSSParserTests, FocusWithinMatchesAncestorContainingFocus) {
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".bar:focus-within .icon { width: 100px; }\n", sheet));
    ASSERT_EQ(sheet.Rules.size(), 1u);

    auto bar = std::make_unique<UIElement>("bar");
    bar->AddClass("bar");
    auto icon = std::make_unique<UIElement>("icon");
    icon->AddClass("icon");
    UIElement* iconPtr = icon.get();
    bar->AddChild(std::move(icon));

    ElementState st{}; // the icon itself never gains focus state

    // No focus anywhere: the ancestor :focus-within compound must not match.
    auto sBlur = CSSParser::ComputeStyleFor(*iconPtr, sheet, st);
    EXPECT_FALSE(sBlur.Layout.Width.IsPx());

    // Ancestor .bar now contains focus (RebuildFocusWithinChain sets this bit
    // on the focused element and every ancestor).
    bar->m_InQueueFlags |= UIElement::InFocusChain;
    auto sFocus = CSSParser::ComputeStyleFor(*iconPtr, sheet, st);
    EXPECT_TRUE(sFocus.Layout.Width.IsPx());
    EXPECT_NEAR(sFocus.Layout.Width.Value, 100.f, 0.01f);

    // Blur: clearing the ancestor chain reverts the descendant.
    bar->m_InQueueFlags &= ~UIElement::InFocusChain;
    auto sBlur2 = CSSParser::ComputeStyleFor(*iconPtr, sheet, st);
    EXPECT_FALSE(sBlur2.Layout.Width.IsPx());
}

TEST(CSSParserTests, FocusMatchesAncestorFocusedElement) {
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".panel:focus .child { width: 100px; }\n", sheet));
    ASSERT_EQ(sheet.Rules.size(), 1u);

    auto panel = std::make_unique<UIElement>("panel");
    panel->AddClass("panel");
    auto child = std::make_unique<UIElement>("child");
    child->AddClass("child");
    UIElement* childPtr = child.get();
    panel->AddChild(std::move(child));

    ElementState st{};

    auto sBlur = CSSParser::ComputeStyleFor(*childPtr, sheet, st);
    EXPECT_FALSE(sBlur.Layout.Width.IsPx());

    // Ancestor .panel is the focused leaf.
    panel->m_InQueueFlags |= (UIElement::InFocusChain | UIElement::InFocusLeaf);
    auto sFocus = CSSParser::ComputeStyleFor(*childPtr, sheet, st);
    EXPECT_TRUE(sFocus.Layout.Width.IsPx());
    EXPECT_NEAR(sFocus.Layout.Width.Value, 100.f, 0.01f);

    panel->m_InQueueFlags &= ~(UIElement::InFocusChain | UIElement::InFocusLeaf);
    auto sBlur2 = CSSParser::ComputeStyleFor(*childPtr, sheet, st);
    EXPECT_FALSE(sBlur2.Layout.Width.IsPx());
}

TEST(CSSParserTests, FocusVisibleMatchesAncestorKeyboardFocusedElement) {
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".panel:focus-visible .child { width: 100px; }\n", sheet));
    ASSERT_EQ(sheet.Rules.size(), 1u);

    auto panel = std::make_unique<UIElement>("panel");
    panel->AddClass("panel");
    auto child = std::make_unique<UIElement>("child");
    child->AddClass("child");
    UIElement* childPtr = child.get();
    panel->AddChild(std::move(child));

    ElementState st{};

    // Mouse focus (leaf but not keyboard): :focus-visible must NOT match.
    panel->m_InQueueFlags |= (UIElement::InFocusChain | UIElement::InFocusLeaf);
    auto sMouse = CSSParser::ComputeStyleFor(*childPtr, sheet, st);
    EXPECT_FALSE(sMouse.Layout.Width.IsPx());

    // Keyboard focus: the InFocusVisibleLeaf bit is set on the leaf.
    panel->m_InQueueFlags |= UIElement::InFocusVisibleLeaf;
    auto sKey = CSSParser::ComputeStyleFor(*childPtr, sheet, st);
    EXPECT_TRUE(sKey.Layout.Width.IsPx());
    EXPECT_NEAR(sKey.Layout.Width.Value, 100.f, 0.01f);

    // Blur clears everything.
    panel->m_InQueueFlags &= ~(UIElement::InFocusChain | UIElement::InFocusLeaf |
                               UIElement::InFocusVisibleLeaf);
    auto sBlur = CSSParser::ComputeStyleFor(*childPtr, sheet, st);
    EXPECT_FALSE(sBlur.Layout.Width.IsPx());
}

// ---------------------------------------------------------------------------
// Custom-state (`:name`) pseudo-class support (lexbor overlay patch 0002 +
// matcher). Before the patch, lexbor's selector state machine failed the whole
// selector for any pseudo-class it did not recognize (unknown names like
// :loading / :my-state) OR routed into a "not supported" branch (:visited,
// :valid, :scope, ...) — either way the entire style rule was discarded and no
// pseudo node reached the runtime Stylesheet. These pin that custom states now
// survive parsing (as PseudoClass::Kind::Custom) and that the matcher evaluates
// them for both the target compound (via ElementState) and an ancestor compound
// (via UIElement::HasCustomState).
// ---------------------------------------------------------------------------

TEST(CSSParserTests, CustomStatePseudoRulesSurviveParsing) {
    const std::string css =
        ".a:loading { width: 1px; }\n"
        ".b:my-state .icon { width: 2px; }\n"
        ".c:visited { width: 3px; }\n"; // formerly "not supported" → now Custom

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    // All three rules must survive: lexbor used to drop the two unknown-name
    // rules (:loading, :my-state) and the known-but-"not supported" :visited.
    EXPECT_EQ(sheet.Rules.size(), 3u)
        << "custom-state / formerly-unsupported pseudo rules must not be discarded";
    EXPECT_TRUE(SheetHasPseudoKind(sheet, PseudoClass::Kind::Custom));

    // Names must resolve to the hashed lowercase state name. :visited, even
    // though lexbor knows it, now reaches the engine as a Custom pseudo too.
    bool sawLoading = false;
    bool sawVisited = false;
    for (const auto& rule : sheet.Rules)
        for (const auto& term : rule.Selector.Terms)
            for (const auto& pseudo : term.Selector.Pseudos)
                if (pseudo.PseudoKind == PseudoClass::Kind::Custom) {
                    if (pseudo.ResolvedCustomNameId == HashStringId("loading"))
                        sawLoading = true;
                    if (pseudo.ResolvedCustomNameId == HashStringId("visited"))
                        sawVisited = true;
                }
    EXPECT_TRUE(sawLoading)
        << ":loading must resolve to ResolvedCustomNameId == HashStringId(\"loading\")";
    EXPECT_TRUE(sawVisited)
        << ":visited must now parse as Custom, not be dropped by the removed switch";
}

TEST(CSSParserTests, CustomStateTargetFormUsesElementState) {
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".field { width: 10px; }\n"
        ".field:loading { width: 100px; }\n", sheet));

    UIElement el;
    el.AddClass("field");

    ElementState st{};
    auto sIdle = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(sIdle.Layout.Width.IsPx());
    EXPECT_NEAR(sIdle.Layout.Width.Value, 10.f, 0.01f);

    // The target compound reads the cascade's ElementState custom-state set.
    st.CustomStates.insert(HashStringId("loading"));
    auto sLoading = CSSParser::ComputeStyleFor(el, sheet, st);
    EXPECT_TRUE(sLoading.Layout.Width.IsPx());
    EXPECT_NEAR(sLoading.Layout.Width.Value, 100.f, 0.01f);
}

TEST(CSSParserTests, CustomStateMatchesAncestorViaElementState) {
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        "#panel:loading .spinner { width: 100px; }\n", sheet));
    ASSERT_EQ(sheet.Rules.size(), 1u);

    auto panel = std::make_unique<UIElement>();
    panel->SetId("panel");
    auto spinner = std::make_unique<UIElement>();
    spinner->AddClass("spinner");
    UIElement* spinnerPtr = spinner.get();
    panel->AddChild(std::move(spinner));

    ElementState st{}; // the spinner itself never carries the custom state

    // No custom state anywhere: the ancestor :loading compound must not match.
    auto sIdle = CSSParser::ComputeStyleFor(*spinnerPtr, sheet, st);
    EXPECT_FALSE(sIdle.Layout.Width.IsPx());

    // Ancestor #panel gains the custom state; the NON-target compound reads the
    // element's own set via UIElement::HasCustomState (the new matcher branch).
    panel->AddCustomState("loading");
    auto sLoading = CSSParser::ComputeStyleFor(*spinnerPtr, sheet, st);
    EXPECT_TRUE(sLoading.Layout.Width.IsPx());
    EXPECT_NEAR(sLoading.Layout.Width.Value, 100.f, 0.01f);

    // Removing it reverts the descendant.
    panel->RemoveCustomState("loading");
    auto sIdle2 = CSSParser::ComputeStyleFor(*spinnerPtr, sheet, st);
    EXPECT_FALSE(sIdle2.Layout.Width.IsPx());
}

// ---------------------------------------------------------------------------
// Property name -> invalidation impact
// ---------------------------------------------------------------------------
//
// A declaration holding var() is stored unparsed and classified later by NAME,
// so a shorthand has to answer for the longhands it expands to rather than for
// its own (often absent) slot. Every expectation below is derived from what the
// expansion writes, not from the table being queried.

TEST(CSSParserTests, ShorthandImpactFollowsItsExpansion) {
    struct Case {
        const char* Name;
        bool Layout;
        bool Paint;
        bool Subtree;
        const char* Why;
    };
    // outline draws outside the border box and is not in the box model at all;
    // flex-grow/shrink/basis are pure Yoga inputs with nothing to paint;
    // overflow-x/y toggle scrollbars (layout) as well as clipping (paint), and
    // overflow itself establishes the clip descendants are emitted against
    // (subtree); border writes both the widths (which text wrapping subtracts)
    // and the colours.
    const Case cases[] = {
        {"outline", false, true, false, "expands to outline-width/style/color"},
        {"flex", true, false, false, "expands to flex-grow/shrink/basis"},
        {"overflow", true, true, true, "expands to overflow-x/y; the clip reaches descendants"},
        {"border", true, true, false, "expands to the four widths, four colours and the style"},
        {"border-top", true, true, false, "expands to border-top-width/color"},
        {"border-right", true, true, false, "expands to border-right-width/color"},
        {"border-bottom", true, true, false, "expands to border-bottom-width/color"},
        {"border-left", true, true, false, "expands to border-left-width/color"},
        {"border-width", true, true, false, "expands to the four widths"},
        {"border-color", false, true, false, "expands to the four colours"},
        {"border-radius", false, true, false, "expands to the four corner radii"},
    };
    for (const Case& c : cases) {
        const StylePropertyImpact impact = CSSParser::PropertyImpactForName(c.Name);
        EXPECT_EQ(impact.Layout, c.Layout) << c.Name << ": " << c.Why;
        EXPECT_EQ(impact.Paint, c.Paint) << c.Name << ": " << c.Why;
        EXPECT_EQ(impact.Subtree, c.Subtree) << c.Name << ": " << c.Why;
    }
}

TEST(CSSParserTests, LonghandImpactMatchesItsOwnSlot) {
    EXPECT_TRUE(CSSParser::PropertyImpactForName("padding").Layout);
    EXPECT_FALSE(CSSParser::PropertyImpactForName("padding").Paint);
    EXPECT_FALSE(CSSParser::PropertyImpactForName("background-color").Layout);
    EXPECT_TRUE(CSSParser::PropertyImpactForName("background-color").Paint);
    EXPECT_FALSE(CSSParser::PropertyImpactForName("outline-offset").Layout);
    EXPECT_TRUE(CSSParser::PropertyImpactForName("outline-offset").Paint);
}

// {false,false} is "nothing is known about this name", which is a different
// statement from "this name changes nothing" -- callers escalate on it. Case is
// not part of the identity, so an author's `OUTLINE` classifies like `outline`.
TEST(CSSParserTests, UnknownPropertyNameCarriesNoImpactInformation) {
    for (const char* name : {"frobnicate", "", "-webkit-appearance"}) {
        const StylePropertyImpact impact = CSSParser::PropertyImpactForName(name);
        EXPECT_FALSE(impact.Layout) << name;
        EXPECT_FALSE(impact.Paint) << name;
    }
    EXPECT_EQ(CSSParser::PropertyImpactForName("OUTLINE").Paint,
              CSSParser::PropertyImpactForName("outline").Paint);
    EXPECT_EQ(CSSParser::PropertyImpactForName("OUTLINE").Layout,
              CSSParser::PropertyImpactForName("outline").Layout);
}

TEST(CSSParserTests, TextEffectsInvalidatePaintWithoutChangingLayout) {
    for (const char* name : {"text-shadow", "text-glow", "text-outline",
                            "text-shadow-offset-x", "text-shadow-offset-y",
                            "text-shadow-blur", "text-shadow-color",
                            "text-glow-radius", "text-glow-color",
                            "text-outline-width", "text-outline-color"}) {
        const StylePropertyImpact impact = CSSParser::PropertyImpactForName(name);
        EXPECT_FALSE(impact.Layout) << name;
        EXPECT_TRUE(impact.Paint) << name;
    }
}

TEST(CSSParserTests, TextShadowAcceptsSharpAndBlurredColorPositions) {
    for (const char* value : {"2px -3px #102030", "#102030 2px -3px",
                              "2px -3px 0px #102030", "#102030 2px -3px 0px"}) {
        SCOPED_TRACE(value);
        Stylesheet sheet;
        ASSERT_TRUE(CSSParser::ParseStylesFromString(
            std::string("#text { text-shadow: ") + value + "; }", sheet));
        UIElement element;
        element.SetId("text");
        const auto effect = CSSParser::ComputeStyleFor(element, sheet, {}).Visual.TextEffects;
        EXPECT_FLOAT_EQ(effect.ShadowOffsetX, 2);
        EXPECT_FLOAT_EQ(effect.ShadowOffsetY, -3);
        EXPECT_FLOAT_EQ(effect.ShadowBlur, 0);
        EXPECT_EQ(effect.ShadowColor, 0xff102030u);
    }
}

TEST(CSSParserTests, InvalidTextShadowsPreservePreviousDeclaration) {
    for (const char* value : {"inset 2px 3px #000", "2px 3px -1px #000",
                              "2px 3px 1px 4px #000", "2em 3px #000",
                              "2px 3px not-a-color"}) {
        SCOPED_TRACE(value);
        Stylesheet sheet;
        ASSERT_TRUE(CSSParser::ParseStylesFromString(
            std::string("#text { text-shadow: 4px 5px 2px #102030; text-shadow: ") + value + "; }", sheet));
        UIElement element;
        element.SetId("text");
        const auto effect = CSSParser::ComputeStyleFor(element, sheet, {}).Visual.TextEffects;
        EXPECT_FLOAT_EQ(effect.ShadowOffsetX, 4);
        EXPECT_FLOAT_EQ(effect.ShadowOffsetY, 5);
        EXPECT_FLOAT_EQ(effect.ShadowBlur, 2);
        EXPECT_EQ(effect.ShadowColor, 0xff102030u);
    }
}

// A text shadow without a color is valid CSS (the text color) but not a form
// the engine takes: it is ignored with a warning that names the fix.
TEST(CSSParserTests, IgnoredTextShadowWarnsWithTheAcceptedForm) {
    Logger::Log::Initialize({});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto warnings = std::make_shared<std::vector<std::string>>();
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [warnings](const Logger::LogMessage& msg) { warnings->emplace_back(msg.Message.c_str()); });
    Logger::Log::AddSink(std::move(sink));

    Stylesheet sheet;
    ASSERT_TRUE(CSSParser::ParseStylesFromString("#text { text-shadow: 1px 1px 2px; }", sheet));
    Logger::Log::Flush();
    sinkPtr->UnregisterCallback(callbackId);
    UIElement element;
    element.SetId("text");
    EXPECT_EQ(CSSParser::ComputeStyleFor(element, sheet, {}).Visual.TextEffects.ShadowColor, 0u);
    const auto named = std::count_if(warnings->begin(), warnings->end(), [](const std::string& w)
    {
        return w.find("'text-shadow: 1px 1px 2px' is ignored") != std::string::npos
            && w.find("text-shadow: <x>px <y>px [<blur>px] <color>") != std::string::npos;
    });
    EXPECT_EQ(named, 1);
}

TEST(CSSParserTests, MalformedTextEffectColorsPreservePreviousDeclaration) {
    for (const char* color : {"#12", "#ggg", "#1234567z", "rgb(1,2)",
                             "rgb(1,2,3,4,5)", "rgba(1,2,3,no)",
                             "rgb(nan,2,3)", "rgb(1x,2,3)", "hsl(no,50%,50%)",
                             "0x123456junk", "0x100000000", "4294967296"}) {
        SCOPED_TRACE(color);
        Stylesheet sheet;
        const std::string css = std::string("#text { text-shadow: 4px 5px 2px #102030; ") +
            "text-glow: 3px #102030; text-outline: 2px #102030; " +
            "text-shadow: 1px 1px " + color + "; text-glow: 1px " + color +
            "; text-outline: 1px " + color + "; }";
        ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
        UIElement element;
        element.SetId("text");
        const auto effect = CSSParser::ComputeStyleFor(element, sheet, {}).Visual.TextEffects;
        EXPECT_EQ(effect.ShadowColor, 0xff102030u);
        EXPECT_FLOAT_EQ(effect.ShadowOffsetX, 4);
        EXPECT_EQ(effect.GlowColor, 0xff102030u);
        EXPECT_FLOAT_EQ(effect.GlowRadius, 3);
        EXPECT_EQ(effect.OutlineColor, 0xff102030u);
        EXPECT_FLOAT_EQ(effect.OutlineWidth, 2);
    }
}

TEST(CSSParserTests, EachTextEffectPropertyParsesIntoTheResolvedStyle) {
    struct Case { const char* Declarations; UI::TextEffects Expected; };
    const Case cases[] = {
        {"text-shadow: 1px -2px 3px #102030;", {0xff102030u, 1, -2, 3}},
        {"text-glow: 4px #20304080;", {0, 0, 0, 0, 0x80203040u, 4}},
        {"text-outline: 1.5px #304050;", {0, 0, 0, 0, 0, 0, 0xff304050u, 1.5f}},
        {"text-shadow-offset-x: 5px; text-shadow-offset-y: 6px; text-shadow-blur: 7px;"
         " text-shadow-color: #405060;", {0xff405060u, 5, 6, 7}},
        {"text-glow-radius: 8px; text-glow-color: #506070;", {0, 0, 0, 0, 0xff506070u, 8}},
        {"text-outline-width: 9px; text-outline-color: #607080;", {0, 0, 0, 0, 0, 0, 0xff607080u, 9}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.Declarations);
        Stylesheet sheet;
        ASSERT_TRUE(CSSParser::ParseStylesFromString(std::string("#text { ") + c.Declarations + " }", sheet));
        UIElement element;
        element.SetId("text");
        const auto effect = CSSParser::ComputeStyleFor(element, sheet, {}).Visual.TextEffects;
        EXPECT_EQ(effect.ShadowColor, c.Expected.ShadowColor);
        EXPECT_FLOAT_EQ(effect.ShadowOffsetX, c.Expected.ShadowOffsetX);
        EXPECT_FLOAT_EQ(effect.ShadowOffsetY, c.Expected.ShadowOffsetY);
        EXPECT_FLOAT_EQ(effect.ShadowBlur, c.Expected.ShadowBlur);
        EXPECT_EQ(effect.GlowColor, c.Expected.GlowColor);
        EXPECT_FLOAT_EQ(effect.GlowRadius, c.Expected.GlowRadius);
        EXPECT_EQ(effect.OutlineColor, c.Expected.OutlineColor);
        EXPECT_FLOAT_EQ(effect.OutlineWidth, c.Expected.OutlineWidth);
    }
}

// Effect colours come from tokens: a var() inside each shorthand resolves at
// compute time like any other colour.
TEST(CSSParserTests, TextEffectColorsResolveThroughTokens) {
    const std::string css = R"(
        #text {
            --text_shadow_color: #102030;
            --text_glow_color: #20304080;
            --text_outline_color: #304050;
            text-shadow: 0px 2px 1px var(--text_shadow_color);
            text-glow: 6px var(--text_glow_color);
            text-outline: 1px var(--text_outline_color);
        }
    )";
    Stylesheet sheet;
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    UIElement element;
    element.SetId("text");
    const auto effect = CSSParser::ComputeStyleFor(element, sheet, {}).Visual.TextEffects;
    EXPECT_EQ(effect.ShadowColor, 0xff102030u);
    EXPECT_FLOAT_EQ(effect.ShadowOffsetY, 2);
    EXPECT_FLOAT_EQ(effect.ShadowBlur, 1);
    EXPECT_EQ(effect.GlowColor, 0x80203040u);
    EXPECT_FLOAT_EQ(effect.GlowRadius, 6);
    EXPECT_EQ(effect.OutlineColor, 0xff304050u);
    EXPECT_FLOAT_EQ(effect.OutlineWidth, 1);
}

// The shape a stylesheet uses: the tokens on an ancestor, the effect on the
// text element.
TEST(CSSParserTests, TextEffectColorsResolveThroughInheritedTokens) {
    const std::string css = R"(
        #parent { --text_glow_color: #20304080; --text_outline_color: #304050; }
        #child { text-glow: 6px var(--text_glow_color); text-outline: 1px var(--text_outline_color); }
    )";
    Stylesheet sheet;
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    const std::vector<const Stylesheet*> sheets{&sheet};
    ElementState state{};
    UIElement parent;
    parent.SetId("parent");
    UIElement child;
    child.SetId("child");
    const ResolvedStyle parentStyle =
        CSSParser::ComputeStyleFor(parent, sheets, state, static_cast<const ResolvedStyle*>(nullptr));
    const auto effect = CSSParser::ComputeStyleFor(child, sheets, state, &parentStyle).Visual.TextEffects;
    EXPECT_EQ(effect.GlowColor, 0x80203040u);
    EXPECT_FLOAT_EQ(effect.GlowRadius, 6);
    EXPECT_EQ(effect.OutlineColor, 0xff304050u);
    EXPECT_FLOAT_EQ(effect.OutlineWidth, 1);
}
