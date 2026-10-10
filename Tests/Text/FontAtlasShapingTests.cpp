#include "Platform/Shell.h"
#include "Rendering/Text/FontAtlas.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using GameEngine::Rendering::Text::FontAtlas;

static std::string FindFallbackFont()
{
    return (GameEngine::Platform::GetExecutablePath().parent_path() /
            "TestData/Text/Roboto-Regular.ttf").string();
}

static bool LoadFontBytes(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

TEST(FontAtlasShapingTests, KerningReducesSpacingForAV)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    auto mA = fa.MeasureUtf8("A", 48);
    auto mV = fa.MeasureUtf8("V", 48);
    auto mAV = fa.MeasureUtf8("AV", 48);

    EXPECT_LE(mAV.width, mA.width + mV.width + 1.0f);
}

TEST(FontAtlasShapingTests, LigatureMayReduceGlyphCountForFFI)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    FontAtlas::ShapeResult result;
    fa.ShapeText("ffi", 48, result);
    EXPECT_LE(result.glyphs.size(), (size_t)3);
    EXPECT_GT(result.glyphs.size(), 0u);
}

TEST(FontAtlasShapingTests, ArabicRTLProducesGlyphs)
{
    FontAtlas fa;
    auto font = (GameEngine::Platform::GetExecutablePath().parent_path() /
                 "TestData/Text/NotoSansArabic-Regular.ttf").string();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    const char* arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85";
    FontAtlas::ShapeResult result;
    fa.ShapeText(arabic, 48, result);
    EXPECT_GT(result.glyphs.size(), 0u);
}

TEST(FontAtlasShapingTests, LineMetricsStayInLockstepWithMeasureUtf8)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    // Render sizes are float px: the text stack measures at the fractional
    // device size rather than a rounded integer.
    const float kRenderSizes[] = {10.0f, 14.0f, 24.0f, 32.0f};
    const char* text = "0.000";

    for (float pixelSize : kRenderSizes)
    {
        auto metrics = fa.MeasureUtf8(text, pixelSize);
        auto lm = fa.GetFontLineMetrics(pixelSize);

        EXPECT_GT(metrics.ascender, 0.0f);
        EXPECT_GT(metrics.descender, 0.0f);
        EXPECT_GT(lm.ascender, 0.0f);
        EXPECT_GT(lm.descender, 0.0f);

        const float kEps = 1.0f;
        EXPECT_NEAR(metrics.ascender, lm.ascender, kEps);
        EXPECT_NEAR(metrics.descender, lm.descender, kEps);
        EXPECT_NEAR(metrics.baseline, lm.ascender, kEps);
    }
}

// Caret positions are compared only against the advance total, never against glyph ink:
// the two are not comparable in either direction. GlyphPlacement.x/.width is the rendered
// quad (screen-space, bearing included), while metrics.width and caretXByByte accumulate
// advances, so the gap between them is the trailing glyph's right side bearing -- a
// quantity a font is free to give either sign. On segoeui, ink falls 1.13px short of
// advance for the "0.000 gp" below at 24px (the trailing p has positive bearing), yet
// overhangs it by 0.72px for "f" at 48px; the overhang holds for every font
// FindFallbackFont can pick on Windows (arial 1.66px, verdana 1.55px at 48px) and reaches
// 9.27px on timesi. Whichever direction is asserted, some string and font pair falsifies it.
TEST(FontAtlasShapingTests, CaretMapAlignsWithMetrics)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    const float kPixelSize = 24.0f;
    const char* text = "0.000 gp";

    auto measure = fa.MeasureText(text, kPixelSize);
    FontAtlas::ShapeResult shaped;
    fa.ShapeText(text, kPixelSize, shaped);

    ASSERT_GT(shaped.glyphs.size(), 0u);
    ASSERT_FALSE(measure.caretXByByte.empty());

    const auto& caret = measure.caretXByByte;
    for (size_t i = 1; i < caret.size(); ++i)
    {
        EXPECT_LE(caret[i - 1], caret[i] + 1e-4f);
    }

    const float kWidthEps = 0.5f;
    EXPECT_NEAR(measure.metrics.width, caret.back(), kWidthEps);
}

#if defined(GE_HAVE_FREETYPE)
// Glyph metrics in font units, read straight from FreeType with the same
// convention the Slug bounds are built from (FT_LOAD_NO_SCALE, unhinted) — but
// read independently of ShapeText, so an expected placement never derives from
// the placement under test.
struct FontUnitGlyphMetrics
{
    long BearingX = 0; // ink xMin
    long Width = 0;    // ink xMax - xMin
    long Advance = 0;
    long InkRight() const { return BearingX + Width; }
    long Rsb() const { return Advance - InkRight(); } // right side bearing; either sign
};

static bool ReadFontUnitMetrics(FT_Face face, uint32_t codepoint, FontUnitGlyphMetrics& out)
{
    const FT_UInt gid = FT_Get_Char_Index(face, (FT_ULong)codepoint);
    if (gid == 0)
        return false;
    if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
        return false;
    out.BearingX = (long)face->glyph->metrics.horiBearingX;
    out.Width = (long)face->glyph->metrics.width;
    out.Advance = (long)face->glyph->metrics.horiAdvance;
    return true;
}
#endif

// The ink-quad/advance relation that IS invariant, per glyph: the ink quad sits
// at the glyph's caret position plus its bearing. In render pixels, with
// S = rasterPpem / unitsPerEm and rsb = advance - (bearingX + width):
//   inkLeft  == caret + bearingX * S
//   inkRight == caret + (advance - rsb) * S
// so |inkRight - caretEnd| is bounded by the glyph's own |rsb|, never by a
// tuned constant. Expected values come from FreeType font-unit metrics
// (FontUnitGlyphMetrics above); kerning cancels because ink placement never
// reads the advance. kPlacementTolPx covers float rounding only: caret map and
// pen accumulate the same 26.6 advances from the same hb_font, and the
// atlas-to-render scales include fractional sizes. The
// smallest |rsb| in this population is two orders of magnitude above it.
//
// The fonts are chosen so BOTH rsb signs are present ('f' overhangs its
// advance in every candidate; digits and 'p' leave a positive gap) — with one
// sign the bound could never fail in the direction ink-vs-advance comparisons
// historically got wrong, and the test would be vacuous.
TEST(FontAtlasShapingTests, GlyphPlacementSitsAtCaretPlusBearing)
{
#if !defined(GE_HAVE_FREETYPE)
    GTEST_SKIP() << "FreeType not available";
#else
    using GameEngine::Rendering::Text::FreeTypeFace;
    using GameEngine::Rendering::Text::FreeTypeLib;

    const std::string fontsDir = (GameEngine::Platform::GetExecutablePath().parent_path() /
                                  "TestData/Text").string() + "/";
    const char* kFontCandidates[] = {"Roboto-Regular.ttf", "Roboto-Italic.ttf"};

    const unsigned kAtlasPixelSize = 48;
    const float kRenderSizes[] = {24.0f, 24.5f, 37.25f, 48.0f};
    // ASCII and ligature-free (no f+f/i/l pairs), so glyphs map 1:1 to
    // non-space chars; "AV To" adds kerning pairs to stress caret agreement.
    const char* kFixtures[] = {"0.000 gp", "f", "AV To"};
    const float kPlacementTolPx = 1e-3f;

    bool sawNegativeRsb = false;
    bool sawPositiveRsb = false;
    int fontsLoaded = 0;

    for (const char* fontFile : kFontCandidates)
    {
        const std::string path = fontsDir + fontFile;
        if (!std::filesystem::exists(path))
            continue;
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(LoadFontBytes(path, bytes));
        FontAtlas fa;
        ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), kAtlasPixelSize));
        ++fontsLoaded;

        FreeTypeLib refLib;
        ASSERT_TRUE(refLib.Init());
        FreeTypeFace refFace;
        ASSERT_TRUE(refFace.NewMemoryFace(refLib.Lib(), bytes.data(), bytes.size()));
        ASSERT_GT(refFace.Face()->units_per_EM, 0);
        const double upem = (double)refFace.Face()->units_per_EM;

        for (float pixelSize : kRenderSizes)
        {
            // Quad geometry follows the fractional render size.
            const double S = (double)pixelSize / upem;

            int negCount = 0, posCount = 0;
            double rsbMinPx = 0.0, rsbMaxPx = 0.0;
            char rsbMinChar = ' ', rsbMaxChar = ' ';
            double maxAbsDeltaPx = 0.0;

            for (const char* text : kFixtures)
            {
                SCOPED_TRACE(std::string(fontFile) + " @" +
                             std::to_string((int)pixelSize) + "px \"" + text + "\"");
                auto measure = fa.MeasureText(text, pixelSize);
                FontAtlas::ShapeResult shaped;
                fa.ShapeText(text, pixelSize, shaped);

                const std::string_view sv(text);
                ASSERT_EQ(measure.caretXByByte.size(), sv.size() + 1);

                // One quad per non-space char: ShapeText emits no quad for
                // whitespace, and these fixtures shape without substitutions.
                // A mismatch here means a ligature or missing glyph, which
                // would break the quad-to-caret mapping below.
                std::vector<size_t> quadByteIndex;
                for (size_t b = 0; b < sv.size(); ++b)
                {
                    if (sv[b] != ' ')
                        quadByteIndex.push_back(b);
                }
                ASSERT_EQ(shaped.glyphs.size(), quadByteIndex.size());

                for (size_t k = 0; k < shaped.glyphs.size(); ++k)
                {
                    const auto& gp = shaped.glyphs[k];
                    const size_t b = quadByteIndex[k];
                    const char ch = sv[b];
                    FontUnitGlyphMetrics fu;
                    ASSERT_TRUE(ReadFontUnitMetrics(refFace.Face(), (uint32_t)(unsigned char)ch, fu));

                    const long rsb = fu.Rsb();
                    const double caret = measure.caretXByByte[b];
                    const double predictedLeft = caret + fu.BearingX * S;
                    const double predictedRight = caret + (fu.Advance - rsb) * S;
                    EXPECT_NEAR(gp.x, predictedLeft, kPlacementTolPx) << "glyph '" << ch << "'";
                    EXPECT_NEAR(gp.x + gp.width, predictedRight, kPlacementTolPx)
                        << "glyph '" << ch << "'";

                    const double deltaLeft = std::abs(gp.x - predictedLeft);
                    const double deltaRight = std::abs((gp.x + gp.width) - predictedRight);
                    maxAbsDeltaPx = std::max({maxAbsDeltaPx, deltaLeft, deltaRight});

                    const double rsbPx = rsb * S;
                    if (rsb < 0)
                        ++negCount;
                    else if (rsb > 0)
                        ++posCount;
                    sawNegativeRsb = sawNegativeRsb || rsb < 0;
                    sawPositiveRsb = sawPositiveRsb || rsb > 0;
                    if (rsbPx < rsbMinPx)
                    {
                        rsbMinPx = rsbPx;
                        rsbMinChar = ch;
                    }
                    if (rsbPx > rsbMaxPx)
                    {
                        rsbMaxPx = rsbPx;
                        rsbMaxChar = ch;
                    }
                }
            }
            std::printf("[ rsb      ] %-12s @%2.0fpx  neg=%d pos=%d  min=%+.4fpx('%c') "
                        "max=%+.4fpx('%c')  maxAbsDelta=%.6fpx\n",
                        fontFile, pixelSize, negCount, posCount, rsbMinPx, rsbMinChar,
                        rsbMaxPx, rsbMaxChar, maxAbsDeltaPx);
        }
    }
    ASSERT_GT(fontsLoaded, 0);
    EXPECT_TRUE(sawNegativeRsb) << "no negative right-side-bearing glyph measured";
    EXPECT_TRUE(sawPositiveRsb) << "no positive right-side-bearing glyph measured";
#endif
}
