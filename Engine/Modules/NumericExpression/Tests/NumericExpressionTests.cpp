#include "NumericExpression/NumericExpression.h"

#include <gtest/gtest.h>

namespace GameEngine {
namespace
{

TEST(NumericExpressionTests, BasicArithmetic)
{
    double v = 0.0;
    ASSERT_TRUE(TryEvaluateNumericExpression("2+2", v));
    EXPECT_DOUBLE_EQ(v, 4.0);
    ASSERT_TRUE(TryEvaluateNumericExpression("3.5 * 2", v));
    EXPECT_DOUBLE_EQ(v, 7.0);
    ASSERT_TRUE(TryEvaluateNumericExpression("(1+2)*3", v));
    EXPECT_DOUBLE_EQ(v, 9.0);
}

TEST(NumericExpressionTests, BuiltInFunctionsAndConstants)
{
    double v = 0.0;
    ASSERT_TRUE(TryEvaluateNumericExpression("sqrt(16)", v));
    EXPECT_DOUBLE_EQ(v, 4.0);
    ASSERT_TRUE(TryEvaluateNumericExpression("sin(0)", v));
    EXPECT_NEAR(v, 0.0, 1e-9);
    ASSERT_TRUE(TryEvaluateNumericExpression("pi", v));
    EXPECT_NEAR(v, 3.141592653589793, 1e-12);
}

// TE_NAT_LOG: log is natural log; log10 remains base-10.
TEST(NumericExpressionTests, LogIsNaturalLog)
{
    double v = 0.0;
    ASSERT_TRUE(TryEvaluateNumericExpression("log(e)", v));
    EXPECT_NEAR(v, 1.0, 1e-9);
    ASSERT_TRUE(TryEvaluateNumericExpression("log10(100)", v));
    EXPECT_NEAR(v, 2.0, 1e-9);
}

// Default TinyExpr: exponentiation binds left-to-right (spreadsheet style).
TEST(NumericExpressionTests, PowAssociatesFromLeft)
{
    double v = 0.0;
    ASSERT_TRUE(TryEvaluateNumericExpression("2^3^2", v));
    EXPECT_DOUBLE_EQ(v, 64.0);
}

TEST(NumericExpressionTests, RejectsInvalidInput)
{
    double v = 0.0;
    EXPECT_FALSE(TryEvaluateNumericExpression("", v));
    EXPECT_FALSE(TryEvaluateNumericExpression("foo", v));
    EXPECT_FALSE(TryEvaluateNumericExpression("1/0", v));
}

TEST(NumericExpressionTests, FilterAllowsFunctionNames)
{
    const std::string filtered = FilterNumericExpressionInput("sin(pi/2)+@");
    EXPECT_EQ(filtered, "sin(pi/2)+");
}

} // namespace
} // namespace GameEngine
