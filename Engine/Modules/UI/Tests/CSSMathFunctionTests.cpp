#include <gtest/gtest.h>
#include <string>

#include "UI/Parsers/CSSParser.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

// Direct parser coverage for the css-values-3 math functions (calc/min/max)
// that constant-fold in CSSValueParsers. Every case cascades a plain 40px
// first, so "rejected" is observable as the cascaded value standing — the
// same failure shape the length parsers give every unreadable declaration.
namespace
{

constexpr float kCascadedPx = 40.0f;

// Resolves `.t { width: 40px; } .t { width: <expr>; }` and returns the width.
StyleLength ResolveWidth(const std::string& expr)
{
    Stylesheet sheet{};
    const std::string css = ".t { width: 40px; } .t { width: " + expr + "; }";
    if (!CSSParser::ParseStylesFromString(css, sheet))
        return StyleLength::Auto();
    UIElement el;
    el.AddClass("t");
    const ElementState state{};
    return CSSParser::ComputeStyleFor(el, sheet, state).Layout.Width;
}

void ExpectFoldsToPx(const std::string& expr, float px)
{
    const StyleLength w = ResolveWidth(expr);
    ASSERT_TRUE(w.IsPx()) << expr;
    EXPECT_FLOAT_EQ(w.Value, px) << expr;
}

void ExpectFoldsToPercent(const std::string& expr, float pct)
{
    const StyleLength w = ResolveWidth(expr);
    ASSERT_TRUE(w.IsPercent()) << expr;
    EXPECT_FLOAT_EQ(w.Value, pct) << expr;
}

void ExpectRejected(const std::string& expr)
{
    const StyleLength w = ResolveWidth(expr);
    ASSERT_TRUE(w.IsPx()) << expr << " — a rejected declaration leaves the cascaded 40px";
    EXPECT_FLOAT_EQ(w.Value, kCascadedPx) << expr;
}

} // namespace

TEST(CSSMathFunctionTests, FoldsSameTypeArithmetic)
{
    ExpectFoldsToPx("calc(10px + 5px)", 15.0f);
    ExpectFoldsToPx("calc(10px - 4px)", 6.0f);
    ExpectFoldsToPx("calc(3 * 7px)", 21.0f);
    ExpectFoldsToPx("calc(7px * 3)", 21.0f);
    ExpectFoldsToPx("calc(9px / 3)", 3.0f);
    ExpectFoldsToPercent("calc(50% + 10%)", 60.0f);
    ExpectFoldsToPercent("min(75%, 50%)", 50.0f);
    ExpectFoldsToPx("max(1px, 2px, 3px)", 3.0f);
    ExpectFoldsToPx("min(24px, calc(8px * 3))", 24.0f);
    ExpectFoldsToPx("calc((4px + 2px) / 2)", 3.0f);
    ExpectFoldsToPx("calc(calc(2px + 1px) * 2)", 6.0f);
    ExpectFoldsToPx("calc(-2px + 5px)", 3.0f);
}

TEST(CSSMathFunctionTests, NewlineWhitespaceIsWhitespace)
{
    ExpectFoldsToPx("calc(2px\n + 3px)", 5.0f);
    ExpectFoldsToPx("min(9px,\r\n 4px)", 4.0f);
}

TEST(CSSMathFunctionTests, RejectsTypeMismatchesAndUnknownUnits)
{
    ExpectRejected("calc(50% - 4px)");
    ExpectRejected("min(1px, 50%)");
    ExpectRejected("calc(1 - 2px)");
    ExpectRejected("calc(2px * 3px)"); // length × length has no CSS type
    ExpectRejected("calc(1em + 2px)"); // unknown dimension cannot fold
    ExpectRejected("calc(1 + 2)");     // a number is not a length
    ExpectRejected("min(1, 2)");
}

TEST(CSSMathFunctionTests, RejectsDivisionByZeroAndByLength)
{
    ExpectRejected("calc(10px / 0)");
    ExpectRejected("calc(10px / 0.0)");
    ExpectRejected("calc(10px / 2px)");
}

TEST(CSSMathFunctionTests, PlusMinusRequireSurroundingWhitespace)
{
    // css-values-3: '+'/'-' need whitespace on both sides so "1-2px" stays a
    // dimension. An unspaced sum cannot fold and the declaration rejects.
    ExpectRejected("calc(2px-1px)");
    ExpectRejected("calc(2px+1px)");
    ExpectRejected("calc(2px -1px)");
    ExpectRejected("calc(2px- 1px)");
    // '*' and '/' carry no such rule.
    ExpectFoldsToPx("calc(2px*3)", 6.0f);
    ExpectFoldsToPx("calc(6px/2)", 3.0f);
}

TEST(CSSMathFunctionTests, MalformedInputRejectsWithoutCrashing)
{
    ExpectRejected("calc(1px))");      // trailing garbage
    ExpectRejected("calc()");
    ExpectRejected("calc( )");
    ExpectRejected("min()");
    ExpectRejected("max()");
    ExpectRejected("min(1px,)");
    ExpectRejected("min(, 1px)");
    ExpectRejected("calc(1px + )");
    ExpectRejected("calc(+ 1px + 2px)");
    ExpectRejected("calc(1px 2px)");
    ExpectRejected("calc(.)");
    ExpectRejected("calc(px)");
    ExpectRejected("min(1px 2px)");    // ',' is the argument separator
}

// An unclosed '(' is the one malformed value that costs the whole stylesheet:
// the '}' that would end the rule is swallowed by the function, so the file
// ends inside it and reads exactly like a half-written save. The parser rejects
// that outright rather than let a truncated file replace a live one.
TEST(CSSMathFunctionTests, UnbalancedOpenRejectsTheWholeStylesheet)
{
    Stylesheet sheet{};
    EXPECT_FALSE(CSSParser::ParseStylesFromString(".t { width: 40px; } .t { width: calc(1px + 2px; }",
                                                  sheet));
}

TEST(CSSMathFunctionTests, HugeNumbersDoNotCrash)
{
    // Overflow saturates rather than crashing; the exact value is not pinned.
    const StyleLength a = ResolveWidth("calc(340282346638528859811704183484516925440px * 10)");
    const StyleLength b = ResolveWidth("calc(1e30px * 1e30)");
    (void)a;
    (void)b;
    SUCCEED();
}

TEST(CSSMathFunctionTests, RecursionBoundRejectsDeepNestingButAllowsRealDepth)
{
    auto wrap = [](int depth)
    {
        std::string expr = "calc(";
        for (int i = 0; i < depth; ++i)
            expr += '(';
        expr += "1px";
        for (int i = 0; i < depth; ++i)
            expr += ')';
        expr += ')';
        return expr;
    };
    ExpectFoldsToPx(wrap(8), 1.0f);   // well inside the 32-frame bound
    ExpectRejected(wrap(64));         // over the bound: rejected, not a stack blowout
    ExpectRejected(wrap(4096));       // parenthesis bomb
}
