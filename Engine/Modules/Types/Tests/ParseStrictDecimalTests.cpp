#include <Types/ParseNumber.h>

#include <gtest/gtest.h>

using GameEngine::ParseStrictDecimal;

namespace {

double Parsed(std::string_view text)
{
    double value = 0.0;
    EXPECT_TRUE(ParseStrictDecimal(text, value)) << "expected '" << text << "' to parse";
    return value;
}

bool Rejects(std::string_view text)
{
    double value = -12345.0;
    const bool ok = ParseStrictDecimal(text, value);
    // A refusal must leave the caller's value alone.
    EXPECT_TRUE(ok || value == -12345.0);
    return !ok;
}

} // namespace

TEST(ParseStrictDecimal, ReadsPlainDecimals)
{
    EXPECT_DOUBLE_EQ(Parsed("0"), 0.0);
    EXPECT_DOUBLE_EQ(Parsed("1.45"), 1.45);
    EXPECT_DOUBLE_EQ(Parsed("-2.5"), -2.5);
    EXPECT_DOUBLE_EQ(Parsed(".5"), 0.5);
    EXPECT_DOUBLE_EQ(Parsed("1e3"), 1000.0);
    EXPECT_DOUBLE_EQ(Parsed("-1.5E-2"), -0.015);
}

TEST(ParseStrictDecimal, TrimsSurroundingSpace)
{
    EXPECT_DOUBLE_EQ(Parsed("  3.5  "), 3.5);
    EXPECT_DOUBLE_EQ(Parsed("\t-1\n"), -1.0);
}

// strtod_l takes these and from_chars does not. The whole point of this
// function is that the two platforms answer the same way.
TEST(ParseStrictDecimal, RefusesWhatOnlyOnePlatformWouldTake)
{
    EXPECT_TRUE(Rejects("0x10"));
    EXPECT_TRUE(Rejects("-0X1p3"));
    EXPECT_TRUE(Rejects("inf"));
    EXPECT_TRUE(Rejects("-inf"));
    EXPECT_TRUE(Rejects("nan"));
    EXPECT_TRUE(Rejects("INFINITY"));
}

TEST(ParseStrictDecimal, RefusesALeadingPlusItCannotConsume)
{
    // One '+' is consumed, so the number behind it still reads.
    EXPECT_DOUBLE_EQ(Parsed("+7"), 7.0);
    EXPECT_TRUE(Rejects("++7"));
    EXPECT_TRUE(Rejects("+"));
}

TEST(ParseStrictDecimal, RefusesAnythingLeftOverAfterTheNumber)
{
    EXPECT_TRUE(Rejects("1.5px"));
    EXPECT_TRUE(Rejects("1.5 2.5"));
    EXPECT_TRUE(Rejects("12,5"));
    EXPECT_TRUE(Rejects("5%"));
}

TEST(ParseStrictDecimal, RefusesEmptyAndNonNumeric)
{
    EXPECT_TRUE(Rejects(""));
    EXPECT_TRUE(Rejects("   "));
    EXPECT_TRUE(Rejects("abc"));
    EXPECT_TRUE(Rejects("-"));
}

TEST(ParseStrictDecimal, RefusesAnOverflowRatherThanReturningInfinity)
{
    EXPECT_TRUE(Rejects("1e400"));
    EXPECT_TRUE(Rejects("-1e400"));
}
