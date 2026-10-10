#include <gtest/gtest.h>

#include "Rendering/Utils/CubeLutFileParser.h"

using namespace GameEngine::Rendering;

TEST(CubeLutFileParserTest, Parse3DOnly2CubeRedMajorOrder)
{
    const char* text = R"(LUT_3D_SIZE 2
LUT_3D_INPUT_RANGE 0.0 1.0
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_FALSE(r.Has1D);
    EXPECT_TRUE(r.Has3D);
    EXPECT_EQ(r.Size3D, 2u);
    EXPECT_FLOAT_EQ(r.In3DMin, 0.0f);
    EXPECT_FLOAT_EQ(r.In3DMax, 1.0f);
    ASSERT_EQ(r.Lut3DRgb.size(), 8u * 3u);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[0], 0.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[3], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[21], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[22], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[23], 1.0f);
}

TEST(CubeLutFileParserTest, Parse1DOnlyMinimal)
{
    const char* text = R"(LUT_1D_SIZE 2
0.1 0.2 0.3
0.4 0.5 0.6
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_TRUE(r.Has1D);
    EXPECT_FALSE(r.Has3D);
    EXPECT_EQ(r.Size1D, 2u);
    ASSERT_EQ(r.Lut1DRgb.size(), 6u);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[0], 0.1f);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[3], 0.4f);
}

TEST(CubeLutFileParserTest, ScalarDomainMinMaxSetsInputRange)
{
    const char* text = R"(DOMAIN_MIN -0.5 -0.5 -0.5
DOMAIN_MAX 1.5 1.5 1.5
LUT_3D_SIZE 2
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_FLOAT_EQ(r.In3DMin, -0.5f);
    EXPECT_FLOAT_EQ(r.In3DMax, 1.5f);
}

TEST(CubeLutFileParserTest, PerChannelDomainFailsExplicitly)
{
    const char* text = R"(DOMAIN_MIN 0.0 0.1 0.0
DOMAIN_MAX 1.0 1.0 1.0
LUT_3D_SIZE 2
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

    CubeLutParseResult r{};
    EXPECT_FALSE(ParseCubeLutFromText(text, r));
    EXPECT_FALSE(r.Error.empty());
}

TEST(CubeLutFileParserTest, ParseShaperPlus3DResolveSample)
{
    const char* text = R"(# Sample 3D cube file containing 1D shaper LUT and 3D LUT.
LUT_1D_SIZE 6
LUT_1D_INPUT_RANGE 0.0 1.0
LUT_3D_SIZE 3
LUT_3D_INPUT_RANGE 0.0 1.0
1.0 1.0 1.0
0.8 0.8 0.8
0.6 0.6 0.6
0.4 0.4 0.4
0.2 0.2 0.2
0.0 0.0 0.0
1.0 1.0 1.0
0.5 1.0 1.0
0.0 1.0 1.0
1.0 0.5 1.0
0.5 0.5 1.0
0.0 0.5 1.0
1.0 0.0 1.0
0.5 0.0 1.0
0.0 0.0 1.0
1.0 1.0 0.5
0.5 1.0 0.5
0.0 1.0 0.5
1.0 0.5 0.5
0.5 0.5 0.5
0.0 0.5 0.5
1.0 0.0 0.5
0.5 0.0 0.5
0.0 0.0 0.5
1.0 1.0 0.0
0.5 1.0 0.0
0.0 1.0 0.0
1.0 0.5 0.0
0.5 0.5 0.0
0.0 0.5 0.0
1.0 0.0 0.0
0.5 0.0 0.0
0.0 0.0 0.0
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_TRUE(r.Has1D);
    EXPECT_TRUE(r.Has3D);
    EXPECT_EQ(r.Size1D, 6u);
    EXPECT_EQ(r.Size3D, 3u);
    EXPECT_EQ(r.Lut1DRgb.size(), 18u);
    EXPECT_EQ(r.Lut3DRgb.size(), 27u * 3u);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[0], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[15], 0.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[0], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[78], 0.0f);
}

TEST(CubeLutFileParserTest, CommentAfterHeaderIsAllowed)
{
    const char* text = R"(LUT_3D_SIZE 2
LUT_3D_INPUT_RANGE 0.0 1.0
0 0 0
# allowed
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)";

    CubeLutParseResult r{};
    EXPECT_TRUE(ParseCubeLutFromText(text, r));
    EXPECT_TRUE(r.Ok);
}

TEST(CubeLutFileParserTest, Utf8BomIsAccepted)
{
    const char* text = "\xEF\xBB\xBFLUT_1D_SIZE 2\n0.1 0.2 0.3\n0.4 0.5 0.6\n";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_TRUE(r.Has1D);
    EXPECT_EQ(r.Size1D, 2u);
}
