#include <gtest/gtest.h>
#include <string>
#include "UI/StyleUtil.h"

using namespace GameEngine;
using namespace GameEngine::UIUtil;

TEST(StyleUtilTests, NormalizesAssetSchemeOnly)
{
    std::string a = NormalizeCssUrlPath("asset:textures/a.png");
    EXPECT_EQ(a, std::string("textures/a.png"));

    std::string b = NormalizeCssUrlPath("textures/a.png");
    EXPECT_EQ(b, std::string("textures/a.png"));
}

TEST(StyleUtilTests, DoesNotStripLeadingSlash)
{
    std::string p = NormalizeCssUrlPath("/textures/a.png");
    EXPECT_EQ(p, std::string("/textures/a.png"));
}

