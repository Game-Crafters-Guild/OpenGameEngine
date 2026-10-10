#include <gtest/gtest.h>

#include <string>

#include "UI/Parsers/CSSParser.h"
#include "UI/TransitionSpec.h"
#include "UI/UIElement.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

// ---------------------------------------------------------------------------
// transition: property parsing
// ---------------------------------------------------------------------------

TEST(CSSTransitionParserTests, ParsesSingleTransition)
{
    const std::string css = ".t { transition: opacity 0.3s ease; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("t");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    ASSERT_EQ(rs.Transitions.Count(), 1u);
    rs.Transitions.ForEach([](const TransitionEntry& e) {
        EXPECT_EQ(e.Property, StylePropertyId::Opacity);
        EXPECT_NEAR(e.DurationSec, 0.3f, 0.001f);
        EXPECT_EQ(e.Easing, Math::EasingFunction::Ease);
    });
}

TEST(CSSTransitionParserTests, ParsesMultipleTransitions)
{
    const std::string css = ".t { transition: opacity 0.3s, background-color 0.5s ease-in-out; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("t");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    ASSERT_EQ(rs.Transitions.Count(), 2u);

    bool foundOpacity = false;
    bool foundBgColor = false;
    rs.Transitions.ForEach([&](const TransitionEntry& e) {
        if (e.Property == StylePropertyId::Opacity)
        {
            foundOpacity = true;
            EXPECT_NEAR(e.DurationSec, 0.3f, 0.001f);
        }
        if (e.Property == StylePropertyId::BackgroundColor)
        {
            foundBgColor = true;
            EXPECT_NEAR(e.DurationSec, 0.5f, 0.001f);
            EXPECT_EQ(e.Easing, Math::EasingFunction::EaseInOut);
        }
    });
    EXPECT_TRUE(foundOpacity);
    EXPECT_TRUE(foundBgColor);
}

TEST(CSSTransitionParserTests, ParsesTransitionAll)
{
    const std::string css = ".t { transition: all 0.5s; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("t");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    ASSERT_EQ(rs.Transitions.Count(), 1u);
    rs.Transitions.ForEach([](const TransitionEntry& e) {
        EXPECT_EQ(e.Property, kTransitionAll);
        EXPECT_NEAR(e.DurationSec, 0.5f, 0.001f);
    });
}

TEST(CSSTransitionParserTests, ParsesTransitionWithDelay)
{
    const std::string css = ".t { transition: opacity 0.3s ease 0.1s; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("t");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    ASSERT_EQ(rs.Transitions.Count(), 1u);
    rs.Transitions.ForEach([](const TransitionEntry& e) {
        EXPECT_EQ(e.Property, StylePropertyId::Opacity);
        EXPECT_NEAR(e.DurationSec, 0.3f, 0.001f);
        EXPECT_EQ(e.Easing, Math::EasingFunction::Ease);
        EXPECT_NEAR(e.DelaySec, 0.1f, 0.001f);
    });
}

TEST(CSSTransitionParserTests, ParsesTransitionNone)
{
    const std::string css = ".t { transition: none; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("t");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(rs.Transitions.IsEmpty());
}

TEST(CSSTransitionParserTests, ParsesMilliseconds)
{
    const std::string css = ".t { transition: opacity 300ms; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("t");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    ASSERT_EQ(rs.Transitions.Count(), 1u);
    rs.Transitions.ForEach([](const TransitionEntry& e) {
        EXPECT_EQ(e.Property, StylePropertyId::Opacity);
        EXPECT_NEAR(e.DurationSec, 0.3f, 0.001f);
    });
}

// ---------------------------------------------------------------------------
// box-shadow / glow parsing
// ---------------------------------------------------------------------------

TEST(CSSTransitionParserTests, ParsesBoxShadow)
{
    const std::string css = ".s { box-shadow: 2px 4px 8px #000000AA; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("s");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_NEAR(rs.Visual.ShadowOffsetX, 2.0f, 0.01f);
    EXPECT_NEAR(rs.Visual.ShadowOffsetY, 4.0f, 0.01f);
    EXPECT_NEAR(rs.Visual.ShadowSoftness, 8.0f, 0.01f);
    EXPECT_EQ(rs.Visual.ShadowColor, 0xAA000000u);
    EXPECT_FALSE(rs.Visual.ShadowInset);
}

TEST(CSSTransitionParserTests, ParsesInsetBoxShadow)
{
    const std::string css = ".s { box-shadow: inset 0 1px 2px rgba(0, 0, 0, 0.35); }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("s");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(rs.Visual.ShadowInset);
    EXPECT_NEAR(rs.Visual.ShadowOffsetX, 0.0f, 0.01f);
    EXPECT_NEAR(rs.Visual.ShadowOffsetY, 1.0f, 0.01f);
    EXPECT_NEAR(rs.Visual.ShadowSoftness, 2.0f, 0.01f);
}

TEST(CSSTransitionParserTests, ParsesTrailingInsetBoxShadow)
{
    const std::string css = ".s { box-shadow: 0 1px 2px #00000080 inset; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("s");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_TRUE(rs.Visual.ShadowInset);
    EXPECT_NEAR(rs.Visual.ShadowOffsetY, 1.0f, 0.01f);
}

TEST(CSSTransitionParserTests, ParsesGlow)
{
    const std::string css = ".g { glow: 6px #4488FFFF; }";
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));

    UIElement el;
    el.AddClass("g");
    ElementState st{};
    auto rs = CSSParser::ComputeStyleFor(el, sheet, st);

    EXPECT_NEAR(rs.Visual.GlowRadius, 6.0f, 0.01f);
    EXPECT_EQ(rs.Visual.GlowColor, 0xFF4488FFu);
}
