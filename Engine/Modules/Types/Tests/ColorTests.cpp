#include "Types/Color.h"

#include <gtest/gtest.h>

namespace
{

using GameEngine::ColorHSV;
using GameEngine::ColorLinear;
using GameEngine::ColorSRGB;

TEST(ColorTests, LinearColorsCompareEqualOnlyWhenEveryChannelMatches)
{
    const ColorLinear color(0.1f, 0.2f, 0.3f, 0.4f);

    EXPECT_TRUE(color == ColorLinear(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_FALSE(color != ColorLinear(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.9f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.1f, 0.9f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.1f, 0.2f, 0.9f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.1f, 0.2f, 0.3f, 0.9f));
}

TEST(ColorTests, SrgbColorsCompareEqualOnlyWhenEveryChannelMatches)
{
    const ColorSRGB color(0.1f, 0.2f, 0.3f, 0.4f);

    EXPECT_TRUE(color == ColorSRGB(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_FALSE(color != ColorSRGB(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.9f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.1f, 0.9f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.1f, 0.2f, 0.9f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.1f, 0.2f, 0.3f, 0.9f));
}

TEST(ColorTests, HsvColorsCompareEqualOnlyWhenEveryChannelMatches)
{
    const ColorHSV color(120.0f, 0.2f, 0.3f, 0.4f);

    EXPECT_TRUE(color == ColorHSV(120.0f, 0.2f, 0.3f, 0.4f));
    EXPECT_FALSE(color != ColorHSV(120.0f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(240.0f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(120.0f, 0.9f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(120.0f, 0.2f, 0.9f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(120.0f, 0.2f, 0.3f, 0.9f));
}

} // namespace
