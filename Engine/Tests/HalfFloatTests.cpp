#include "Mathematics/HalfFloat.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using GameEngine::Mathematics::HalfToFloat;

TEST(HalfFloat, ConvertsSignedZeros)
{
    EXPECT_EQ(HalfToFloat(0x0000u), 0.0f);
    EXPECT_FALSE(std::signbit(HalfToFloat(0x0000u)));
    EXPECT_EQ(HalfToFloat(0x8000u), 0.0f);
    EXPECT_TRUE(std::signbit(HalfToFloat(0x8000u)));
}

TEST(HalfFloat, ConvertsNormalsExactly)
{
    EXPECT_EQ(HalfToFloat(0x3C00u), 1.0f);
    EXPECT_EQ(HalfToFloat(0xC000u), -2.0f);
    EXPECT_EQ(HalfToFloat(0x3800u), 0.5f);
    EXPECT_EQ(HalfToFloat(0x7BFFu), 65504.0f);            // largest finite half
    EXPECT_EQ(HalfToFloat(0x0400u), std::ldexp(1.0f, -14)); // smallest normal
}

TEST(HalfFloat, KeepsSubnormalsExact)
{
    EXPECT_EQ(HalfToFloat(0x0001u), std::ldexp(1.0f, -24));  // smallest subnormal
    EXPECT_EQ(HalfToFloat(0x03FFu), std::ldexp(1023.0f, -24)); // largest subnormal
    EXPECT_EQ(HalfToFloat(0x8001u), -std::ldexp(1.0f, -24));
}

TEST(HalfFloat, KeepsInfinitiesAndNaN)
{
    EXPECT_EQ(HalfToFloat(0x7C00u), std::numeric_limits<float>::infinity());
    EXPECT_EQ(HalfToFloat(0xFC00u), -std::numeric_limits<float>::infinity());
    EXPECT_TRUE(std::isnan(HalfToFloat(0x7E00u)));
}
