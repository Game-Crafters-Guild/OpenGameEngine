// FormatFixed: fixed notation for display, trailing zeros trimmed, locale-independent.

#include "Types/FormatNumber.h"

#include <gtest/gtest.h>

#include <limits>

using GameEngine::FormatFixed;

TEST(FormatNumberTests, FormatFixedTrimsTrailingZerosAndNeverReadsMinusZero)
{
    EXPECT_EQ(FormatFixed(12.5, 3), "12.5");
    EXPECT_EQ(FormatFixed(3.0, 2), "3");
    EXPECT_EQ(FormatFixed(1.23456, 2), "1.23");
    EXPECT_EQ(FormatFixed(-4.25, 3), "-4.25");
    EXPECT_EQ(FormatFixed(1500.0, 0), "1500");
    EXPECT_EQ(FormatFixed(-0.0001, 3), "0");
    EXPECT_EQ(FormatFixed(std::numeric_limits<double>::infinity(), 3), "inf");
}
