#include <gtest/gtest.h>
#include <string>
#include <filesystem>
#include "Rendering/Text/FTUtils.h"
using GameEngine::Rendering::Text::FreeTypeLib;
using GameEngine::Rendering::Text::FreeTypeFace;

TEST(TextCoreMetrics, WidthIncreasesWithMoreCharacters)
{
#if !defined(GE_HAVE_FREETYPE)
    GTEST_SKIP() << "FreeType not available";
#else
    // Try a common Windows font; if not found, test will skip
    const char* windir = std::getenv("WINDIR");
    if (!windir) GTEST_SKIP() << "No WINDIR env";
    std::string fontsDir = std::string(windir) + "\\Fonts\\";
    std::string font = fontsDir + "segoeui.ttf";
    if (!std::filesystem::exists(font)) GTEST_SKIP() << "Font not found";

    FreeTypeLib lib; ASSERT_TRUE(lib.Init());
    FreeTypeFace face; ASSERT_TRUE(face.NewFace(lib.Lib(), font.c_str()));
    face.SetPixelSizes(32);

    auto measure = [&](const char* s){
        float w = 0.0f;
        for (const char* p = s; *p; ++p) {
            if (FT_Load_Char(face.Face(), static_cast<unsigned char>(*p), FT_LOAD_DEFAULT) == 0)
                w += face.Face()->glyph->advance.x / 64.0f;
        }
        return w;
    };
    float w1 = measure("Hi");
    float w2 = measure("Hiii");
    EXPECT_GT(w2, w1);
#endif
}

TEST(TextCoreMetrics, HeightRoughlyEqualsFontSize)
{
#if !defined(GE_HAVE_FREETYPE)
    GTEST_SKIP() << "FreeType not available";
#else
    const char* windir = std::getenv("WINDIR");
    if (!windir) GTEST_SKIP() << "No WINDIR env";
    std::string fontsDir = std::string(windir) + "\\Fonts\\";
    std::string font = fontsDir + "segoeui.ttf";
    if (!std::filesystem::exists(font)) GTEST_SKIP() << "Font not found";

    FreeTypeLib lib; ASSERT_TRUE(lib.Init());
    FreeTypeFace face; ASSERT_TRUE(face.NewFace(lib.Lib(), font.c_str()));
    face.SetPixelSizes(48);
    float h = 0.0f;
    if (face.Face()->size)
        h = face.Face()->size->metrics.height / 64.0f;
    else
        h = (face.Face()->ascender - face.Face()->descender) / 64.0f;
    EXPECT_GT(h, 30.0f);
    EXPECT_LT(h, 80.0f);
#endif
}

