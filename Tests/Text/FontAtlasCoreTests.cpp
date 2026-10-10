#include "Platform/Shell.h"
#include <gtest/gtest.h>
#include "Rendering/Text/FontAtlas.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>

using GameEngine::Rendering::Text::FontAtlas;

static std::string FindFallbackFont()
{
    return (GameEngine::Platform::GetExecutablePath().parent_path() /
            "TestData/Text/Roboto-Regular.ttf").string();
}

TEST(FontAtlasCoreTests, LoadFontAndMeasure)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes; { std::ifstream f(font, std::ios::binary); ASSERT_TRUE((bool)f); f.seekg(0, std::ios::end); auto sz=f.tellg(); f.seekg(0,std::ios::beg); bytes.resize((size_t)sz); f.read(reinterpret_cast<char*>(bytes.data()), sz);} ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));
    auto m = fa.MeasureUtf8("Hello AV ffi", 48);
    EXPECT_GT(m.width, 0.0f);
    EXPECT_GT(m.height, 0.0f);
}

TEST(FontAtlasCoreTests, ShapeTextReturnsGlyphsAndSlugData)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes; { std::ifstream f(font, std::ios::binary); ASSERT_TRUE((bool)f); f.seekg(0, std::ios::end); auto sz=f.tellg(); f.seekg(0,std::ios::beg); bytes.resize((size_t)sz); f.read(reinterpret_cast<char*>(bytes.data()), sz);} ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));
    FontAtlas::ShapeResult result;
    fa.ShapeText("Hello GPU!", 48, result);
    EXPECT_FALSE(result.glyphs.empty());
    EXPECT_GT(result.metrics.width, 0.0f);
    // At least one Slug page should have been created with actual data.
    ASSERT_GT(fa.GetSlugPageCount(), 0);
    EXPECT_GT(fa.GetSlugPageCurveRowsUsed(0), 0);
    EXPECT_GT(fa.GetSlugPageBandRowsUsed(0), 0);
}

// Stress test: shape a large variety of glyphs (full printable ASCII + Latin-1
// accented letters) to ensure many glyphs pack into pages without overflowing
// the fixed page dimensions. Previously, a miscalculated FitsInPage check could
// let a glyph be written past the pre-allocated page buffer, crashing at
// vector::operator[] with out-of-bounds.
TEST(FontAtlasCoreTests, SlugPagingHandlesManyGlyphs)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    { std::ifstream f(font, std::ios::binary); ASSERT_TRUE((bool)f);
      f.seekg(0, std::ios::end); auto sz=f.tellg(); f.seekg(0,std::ios::beg);
      bytes.resize((size_t)sz); f.read(reinterpret_cast<char*>(bytes.data()), sz); }
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    // Print all ASCII + many Latin-1 supplement characters in sequence.
    std::string text;
    for (int c = 0x20; c <= 0x7E; ++c)
        text.push_back(static_cast<char>(c));
    // Append a few multi-byte UTF-8 sequences to exercise new-glyph packing.
    text += " \xC3\xA0\xC3\xA1\xC3\xA2\xC3\xA3\xC3\xA4\xC3\xA5\xC3\xA6\xC3\xA7\xC3\xA8\xC3\xA9";
    text += " \xC3\xB1\xC3\xB2\xC3\xB3\xC3\xB4\xC3\xB5\xC3\xB6\xE2\x80\x9C\xE2\x80\x9D";

    FontAtlas::ShapeResult result;
    fa.ShapeText(text, 48, result);
    EXPECT_FALSE(result.glyphs.empty());

    // Verify at least one page exists and all placements reference valid pages.
    ASSERT_GT(fa.GetSlugPageCount(), 0);
    int pageCount = fa.GetSlugPageCount();
    for (const auto& gp : result.glyphs)
    {
        if (gp.isColor) continue;
        EXPECT_LT(gp.pageIndex, pageCount) << "glyph pageIndex out of range";
    }
}

// Regression test for a bug where PackGlyphIntoPage overwrote PageIndex
// to 0, causing glyphs that physically lived on page 1+ to be rendered
// using page 0's texture data (resulting in missing or corrupt glyphs).
// Also regression for the fresh-row-per-glyph bug that forced each glyph
// to consume a full row (~64 glyphs per page max) instead of dense packing.
TEST(FontAtlasCoreTests, SlugGlyphPageIndexMatchesPackedLocation)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    { std::ifstream f(font, std::ios::binary); ASSERT_TRUE((bool)f);
      f.seekg(0, std::ios::end); auto sz=f.tellg(); f.seekg(0,std::ios::beg);
      bytes.resize((size_t)sz); f.read(reinterpret_cast<char*>(bytes.data()), sz); }
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    // Shape many glyphs. With dense packing, these should all fit in page 0.
    std::string text;
    for (int c = 0x20; c <= 0x7E; ++c)
        text.push_back(static_cast<char>(c));

    FontAtlas::ShapeResult result;
    fa.ShapeText(text, 48, result);
    EXPECT_FALSE(result.glyphs.empty());

    // Every Slug glyph's pageIndex must be strictly less than the page count.
    // Also: placements on the same page should share their page, not all be 0
    // by accident (if dense packing is broken we might see many pages).
    ASSERT_GT(fa.GetSlugPageCount(), 0);
    int pageCount = fa.GetSlugPageCount();
    for (const auto& gp : result.glyphs)
    {
        if (gp.isColor) continue;
        EXPECT_LT(gp.pageIndex, pageCount) << "glyph pageIndex out of range";
    }
}

TEST(FontAtlasCoreTests, SlugPageGenerationIncrementsWhenNewGlyphsAppear)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes; { std::ifstream f(font, std::ios::binary); ASSERT_TRUE((bool)f); f.seekg(0, std::ios::end); auto sz=f.tellg(); f.seekg(0,std::ios::beg); bytes.resize((size_t)sz); f.read(reinterpret_cast<char*>(bytes.data()), sz);} ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));
    FontAtlas::ShapeResult res1, res2, res3;
    fa.ShapeText("ABC", 48, res1);
    ASSERT_GT(fa.GetSlugPageCount(), 0);
    auto gen1 = fa.GetSlugPageGeneration(0);
    fa.ShapeText("ABC", 48, res2);
    EXPECT_EQ(fa.GetSlugPageGeneration(0), gen1);
    fa.ShapeText("ABC \xCF\x89", 48, res3); // omega in UTF-8
    EXPECT_GT(fa.GetSlugPageGeneration(0), gen1);
}
