// Text geometry measured against an external reference rather than against
// itself. Every expected number below was read out of Chrome rendering the
// same Roboto-Regular.ttf this test loads, at the same CSS pixel size, via
// Range.getClientRects()/CanvasRenderingContext2D.measureText — or, where the
// font tables are the authority, computed straight from head/hhea/OS2.
//
// Internal round-trip tests (TextInputGeometryTests and friends) cannot catch
// a convention that is wrong but consistently wrong: an em-size misread the
// same way everywhere still round-trips. These pin the engine to the outside
// world.
//
// Reference environment: Chrome 141, Roboto 2.001047 (unitsPerEm 2048,
// hhea ascent 2146 / descent -555 / lineGap 0, capHeight 1456, xHeight 1082).

#include "RobotoTestFont.h"

#include "Rendering/Common/Utils.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering::Text;
using GameEngine::UITesting::kUiAtlasPixelSize;
using GameEngine::UITesting::LoadRobotoAtlas;

namespace
{

// Roboto design-unit constants, read from the TTF tables. Ground truth for the
// "does font-size N produce N-ppem glyphs" question.
constexpr float kUnitsPerEm = 2048.0f;
constexpr float kCapHeightUnits = 1456.0f;
constexpr float kXHeightUnits = 1082.0f;

// Chrome quantises glyph advances to 1/64 px and the engine shapes at the
// atlas ppem before scaling to the render size, so the two agree to well
// inside a tenth of a pixel but never bit-exactly.
constexpr float kChromeAdvanceTolerancePx = 0.1f;

// Glyph extents derived from the font's own tables are exact in the engine —
// no rounding stage sits between the design units and the emitted quad.
constexpr float kDesignUnitTolerancePx = 0.01f;

class RobotoGeometry : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Atlas = LoadRobotoAtlas();
        if (!Atlas)
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";
    }

    std::vector<TextLayout::LineBreakInfo> Wrap(const std::string& text, float wrapWidth,
                                                float pixelSize = 16.0f)
    {
        StyledRun run{};
        run.Font = Atlas.get();
        run.PixelSize = pixelSize;
        run.Text = text;
        // 0 = 'normal': the font's own line height. (This argument used to be a
        // multiplier, where 1.0f meant the same thing; it is now an absolute box.)
        return TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), wrapWidth, 0.0f,
                                          TextLayout::WordBreak::Normal)
            .LineBreaks;
    }

    std::string LineText(const std::string& text, const TextLayout::LineBreakInfo& line) const
    {
        return text.substr(line.ByteStart, line.ByteEnd - line.ByteStart);
    }

    std::unique_ptr<FontAtlas> Atlas;
};

} // namespace

// --- Dimension 1: font size fidelity -----------------------------------------
// "font-size: N" must produce glyphs whose geometry is the font's design
// outline scaled by N/unitsPerEm. Cap height and x-height are the two extents
// that expose an em-size misinterpretation immediately: a font-size mapped to
// the em box, the win metrics or the typo metrics instead of the em square
// would put these off by several percent.

TEST_F(RobotoGeometry, CapHeightMatchesTheFontTableAtEveryRequestedPixelSize)
{
    for (float px : {12.0f, 16.0f, 20.0f, 24.0f, 32.0f, 48.0f})
    {
        FontAtlas::ShapeResult shaped;
        Atlas->ShapeText("H", px, shaped);
        ASSERT_EQ(shaped.glyphs.size(), 1u) << "px=" << px;
        const float expected = kCapHeightUnits / kUnitsPerEm * px;
        EXPECT_NEAR(shaped.glyphs[0].height, expected, kDesignUnitTolerancePx) << "px=" << px;
    }
}

TEST_F(RobotoGeometry, XHeightMatchesTheFontTableAtEveryRequestedPixelSize)
{
    for (float px : {12.0f, 16.0f, 20.0f, 24.0f, 32.0f, 48.0f})
    {
        FontAtlas::ShapeResult shaped;
        Atlas->ShapeText("x", px, shaped);
        ASSERT_EQ(shaped.glyphs.size(), 1u) << "px=" << px;
        const float expected = kXHeightUnits / kUnitsPerEm * px;
        EXPECT_NEAR(shaped.glyphs[0].height, expected, kDesignUnitTolerancePx) << "px=" << px;
    }
}

// The atlas is sampled at a fixed size, but geometry and advances retain the
// requested fractional size as a window moves through consecutive scale steps.
TEST_F(RobotoGeometry, ScaledFontSizesProduceDesignAccurateGlyphsAtTheDerivedPpem)
{
    for (float px : {16.25f, 16.5f, 19.8f, 19.95f, 20.0f, 20.05f, 20.2f, 35.75f})
    {
        FontAtlas::ShapeResult shaped;
        Atlas->ShapeText("H", px, shaped);
        ASSERT_EQ(shaped.glyphs.size(), 1u);
        EXPECT_NEAR(shaped.glyphs[0].height, kCapHeightUnits / kUnitsPerEm * px,
                    kDesignUnitTolerancePx) << "px=" << px;
    }
}

TEST_F(RobotoGeometry, FractionalResizeKeepsWidthsAndCaretPositionsProportional)
{
    const std::string text = "Text, borders, spacing and controls should keep the same proportions.";
    constexpr float baseSize = 20.0f;
    const auto base = Atlas->MeasureUtf8(text, baseSize);
    std::vector<float> baseCaret;
    ASSERT_TRUE(Atlas->BuildCaretMapUtf8(text, baseSize, baseCaret));
    for (int step = 0; step <= 100; ++step)
    {
        const float px = 19.5f + step * 0.01f;
        const float ratio = px / baseSize;
        const auto measured = Atlas->MeasureUtf8(text, px);
        EXPECT_NEAR(measured.width, base.width * ratio, 0.001f) << "px=" << px;
        FontAtlas::ShapeResult shaped;
        Atlas->ShapeText(text, px, shaped);
        EXPECT_NEAR(shaped.metrics.width, base.width * ratio, 0.001f) << "px=" << px;
        std::vector<float> caret;
        ASSERT_TRUE(Atlas->BuildCaretMapUtf8(text, px, caret));
        ASSERT_EQ(caret.size(), baseCaret.size());
        for (size_t i = 0; i < caret.size(); ++i)
            EXPECT_NEAR(caret[i], baseCaret[i] * ratio, 0.001f) << "px=" << px << " byte=" << i;
    }
}

TEST_F(RobotoGeometry, FractionalResizePreservesLineBreaksAtFixedLogicalWidth)
{
    StyledRun run{};
    run.Font = Atlas.get();
    run.Text = "Text, borders, spacing and controls should keep the same proportions.";
    run.PixelSize = 20.0f;
    const float logicalWidth = Atlas->MeasureUtf8(run.Text, run.PixelSize).width - 0.5f;
    const auto reference = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), logicalWidth);
    ASSERT_EQ(reference.LineCount, 2u);
    for (int step = 0; step <= 100; ++step)
    {
        const float scale = 0.975f + step * 0.0005f;
        run.PixelSize = 20.0f * scale;
        const auto result = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), logicalWidth * scale);
        ASSERT_EQ(result.LineCount, reference.LineCount) << "scale=" << scale;
        for (size_t line = 0; line < result.LineBreaks.size(); ++line)
        {
            EXPECT_EQ(result.LineBreaks[line].ByteStart, reference.LineBreaks[line].ByteStart);
            EXPECT_EQ(result.LineBreaks[line].ByteEnd, reference.LineBreaks[line].ByteEnd);
        }
    }
}

// --- Dimension 2: positioning fidelity ---------------------------------------

// Chrome's per-character caret positions for the same string, read with
// Range.getBoundingClientRect() at font-size 16. The A/V and T/a pairs carry
// GPOS kerning: an unkerned "AV" would place the second glyph at 10.4375
// (1336 design units) rather than 9.75, so this also proves kerning is applied.
TEST_F(RobotoGeometry, CaretPositionsMatchChromeIncludingKerning)
{
    const std::string text = "AVAWA Ta.";
    const float kChrome16[] = {0.0f,       9.75f,      19.34375f, 29.25f,    43.109375f,
                               53.546875f, 57.1875f,   65.859375f, 74.5625f, 78.765625f};
    const float kChrome32[] = {0.0f,      19.515625f, 38.703125f, 58.5f,     86.21875f,
                               107.09375f, 114.390625f, 131.71875f, 149.125f, 157.546875f};

    std::vector<float> caret;
    ASSERT_TRUE(Atlas->BuildCaretMapUtf8(text, 16.0f, caret));
    ASSERT_EQ(caret.size(), std::size(kChrome16));
    for (size_t i = 0; i < caret.size(); ++i)
        EXPECT_NEAR(caret[i], kChrome16[i], kChromeAdvanceTolerancePx) << "byte " << i;

    ASSERT_TRUE(Atlas->BuildCaretMapUtf8(text, 32.0f, caret));
    ASSERT_EQ(caret.size(), std::size(kChrome32));
    for (size_t i = 0; i < caret.size(); ++i)
        EXPECT_NEAR(caret[i], kChrome32[i], kChromeAdvanceTolerancePx) << "byte " << i;

    // The kerned pair really is kerned — guard against the reference table and
    // the engine agreeing only because both dropped GPOS.
    const float unkernedAdvanceA = 1336.0f / kUnitsPerEm * 32.0f;
    EXPECT_LT(caret[1], unkernedAdvanceA - 0.5f);
}

// Glyph quads carry fractional positions; nothing in the pipeline snaps a run
// to whole pixels. A snapping regression would make every advance an integer.
TEST_F(RobotoGeometry, GlyphPositionsAreFractionalNotPixelSnapped)
{
    FontAtlas::ShapeResult shaped;
    Atlas->ShapeText("Hamburgefonstiv", 16.0f, shaped);
    ASSERT_GT(shaped.glyphs.size(), 4u);

    bool sawFractional = false;
    for (const auto& glyph : shaped.glyphs)
    {
        if (std::abs(glyph.x - std::round(glyph.x)) > 0.01f)
        {
            sawFractional = true;
            break;
        }
    }
    EXPECT_TRUE(sawFractional) << "every glyph x landed on a whole pixel";
}

// --- Dimension 3: measurement fidelity ---------------------------------------

// MeasureUtf8 is what Yoga sizes labels from. Chrome's measureText on the same
// string is the external check that it is neither systematically wide nor
// narrow. The pangram is 43 characters, long enough that a per-glyph rounding
// error would show as a visible sum.
TEST_F(RobotoGeometry, MeasuredWidthsMatchChromeMeasureText)
{
    struct Case
    {
        const char* Text;
        float PixelSize;
        float ChromeWidth;
    };
    const Case cases[] = {
        {"H", 12.0f, 8.5546875f},
        {"H", 16.0f, 11.40625f},
        {"H", 24.0f, 17.109375f},
        {"H", 32.0f, 22.8125f},
        {"Hello", 16.0f, 36.7734375f},
        {"AVAWA", 16.0f, 53.546875f},
        {"Hamburgefonstiv", 16.0f, 123.4140625f},
        {"The quick brown fox jumps over the lazy dog", 12.0f, 236.794921875f},
        {"The quick brown fox jumps over the lazy dog", 16.0f, 315.7265625f},
        {"The quick brown fox jumps over the lazy dog", 24.0f, 473.58984375f},
        {"The quick brown fox jumps over the lazy dog", 32.0f, 631.453125f},
    };
    for (const auto& c : cases)
    {
        const float measured = Atlas->MeasureUtf8(c.Text, c.PixelSize).width;
        EXPECT_NEAR(measured, c.ChromeWidth, kChromeAdvanceTolerancePx)
            << "px=" << c.PixelSize << " \"" << c.Text << "\"";
    }
}

// Per-glyph rounding that compounds would show up as measured width drifting
// away from the sum of the caret-map steps. 200 characters is long enough for
// a 1/64px per-glyph error to accumulate past a pixel.
TEST_F(RobotoGeometry, LongStringMeasurementDoesNotDriftFromItsCaretMap)
{
    std::string text;
    while (text.size() < 200)
        text += "the quick brown fox ";
    text.resize(200);

    // Chrome's measureText for the same 200-character string.
    const struct
    {
        float PixelSize;
        float ChromeWidth;
    } cases[] = {{12.0f, 1074.7265625f}, {16.0f, 1432.96875f}, {24.0f, 2149.453125f}};

    for (const auto& c : cases)
    {
        std::vector<float> caret;
        ASSERT_TRUE(Atlas->BuildCaretMapUtf8(text, c.PixelSize, caret));
        const float measured = Atlas->MeasureUtf8(text, c.PixelSize).width;

        double stepSum = 0.0;
        for (size_t i = 1; i < caret.size(); ++i)
            stepSum += caret[i] - caret[i - 1];

        EXPECT_NEAR(measured, caret.back(), 0.001f) << "px=" << c.PixelSize;
        EXPECT_NEAR(static_cast<float>(stepSum), measured, 0.001f) << "px=" << c.PixelSize;
        // 0.25px over 200 characters is 0.00125px per glyph — no compounding.
        EXPECT_NEAR(measured, c.ChromeWidth, 0.25f) << "px=" << c.PixelSize;
    }
}

// --- Wrap points (issue #711 item 1) -----------------------------------------

// Line breaks for the same paragraph at the same CSS width, taken from Chrome
// via Range.getClientRects(). Widths are chosen away from sub-pixel ties: at a
// width equal to a candidate line's width to within 1/64px the two breakers
// can legitimately disagree.
TEST_F(RobotoGeometry, WrapPointsMatchChrome)
{
    struct Case
    {
        const char* Text;
        float Wrap;
        std::vector<const char*> ChromeLines;
    };
    const std::vector<Case> cases = {
        {"The quick brown fox jumps over the lazy dog",
         150.0f,
         {"The quick brown fox ", "jumps over the lazy ", "dog"}},
        {"The quick brown fox jumps over the lazy dog",
         200.0f,
         {"The quick brown fox jumps ", "over the lazy dog"}},
        {"one two", 40.0f, {"one ", "two"}},
        // A word longer than the line overflows rather than breaking, because
        // overflow-wrap defaults to normal.
        {"supercalifragilistic word", 60.0f, {"supercalifragilistic ", "word"}},
        // UAX #14 class HY: hyphens offer a break opportunity.
        {"e-mail well-known co-op", 60.0f, {"e-mail ", "well-", "known ", "co-op"}},
        {"a b c", 100.0f, {"a b c"}},
    };

    for (const auto& c : cases)
    {
        const auto lines = Wrap(c.Text, c.Wrap);
        ASSERT_EQ(lines.size(), c.ChromeLines.size())
            << "\"" << c.Text << "\" @" << c.Wrap;
        for (size_t i = 0; i < lines.size(); ++i)
            EXPECT_EQ(LineText(c.Text, lines[i]), std::string(c.ChromeLines[i]))
                << "\"" << c.Text << "\" @" << c.Wrap << " line " << i;
    }
}

// Chrome breaks after a hyphen-minus wherever one appears mid-string,
// including before a digit and at the very start, but never after a trailing
// hyphen with nothing to move down.
TEST_F(RobotoGeometry, HyphenBreakOpportunitiesMatchChrome)
{
    struct Case
    {
        const char* Text;
        std::vector<const char*> ChromeLines;
    };
    const std::vector<Case> cases = {
        {"well-known", {"well-", "known"}},
        {"co-op", {"co-", "op"}},
        {"a-5", {"a-", "5"}},
        {"5-6", {"5-", "6"}},
        {"-abc", {"-", "abc"}},
        {"abc-", {"abc-"}},
        {"x--y", {"x-", "-", "y"}},
        {"a-b-c", {"a-", "b-", "c"}},
        // No break opportunity: a solidus is not a break class in CSS text.
        {"a/b/c", {"a/b/c"}},
    };

    // 1px forces every available break to be taken.
    for (const auto& c : cases)
    {
        const auto lines = Wrap(c.Text, 1.0f);
        ASSERT_EQ(lines.size(), c.ChromeLines.size()) << "\"" << c.Text << "\"";
        for (size_t i = 0; i < lines.size(); ++i)
            EXPECT_EQ(LineText(c.Text, lines[i]), std::string(c.ChromeLines[i]))
                << "\"" << c.Text << "\" line " << i;
    }
}

// --- Per-line width and trailing whitespace (issue #711 item 4) ---------------

// The space a soft wrap consumes hangs past the wrap point: it stays inside
// the line's byte range so caret mapping is contiguous, but adds no width.
// Chrome's line boxes for the paragraph below end at the last glyph, not after
// the space.
TEST_F(RobotoGeometry, WrappedLineWidthsExcludeHangingTrailingSpace)
{
    const std::string text = "The quick brown fox jumps over the lazy dog";
    const auto lines = Wrap(text, 150.0f);
    ASSERT_EQ(lines.size(), 3u);

    // Chrome line-box widths at the same wrap width.
    const float kChromeLineWidths[] = {143.65625f, 137.03125f, 27.125f};
    for (size_t i = 0; i < lines.size(); ++i)
        EXPECT_NEAR(lines[i].Width, kChromeLineWidths[i], kChromeAdvanceTolerancePx)
            << "line " << i;

    // The byte range still covers the space, so consecutive lines tile the text.
    EXPECT_EQ(LineText(text, lines[0]), "The quick brown fox ");
    EXPECT_EQ(lines[0].ByteEnd, lines[1].ByteStart);
    EXPECT_EQ(lines[1].ByteEnd, lines[2].ByteStart);

    // The reported width is the content width, strictly less than the width of
    // the same range measured with its trailing space.
    const float withSpace = Atlas->MeasureUtf8("The quick brown fox ", 16.0f).width;
    EXPECT_LT(lines[0].Width, withSpace - 3.0f);
}

// A trailing space must not push the following word onto the next line early.
// Chrome keeps "a b " together for every container width above the width of
// "a b", even though "a b " itself is wider than the container.
TEST_F(RobotoGeometry, TrailingSpaceDoesNotForceAnEarlyWrap)
{
    const std::string text = "a b c";
    const float contentWidth = Atlas->MeasureUtf8("a b", 16.0f).width;
    const float withSpace = Atlas->MeasureUtf8("a b ", 16.0f).width;
    ASSERT_GT(withSpace, contentWidth + 1.0f);

    for (float wrap : {contentWidth + 0.1f, (contentWidth + withSpace) * 0.5f, withSpace - 0.1f})
    {
        const auto lines = Wrap(text, wrap);
        ASSERT_EQ(lines.size(), 2u) << "wrap=" << wrap;
        EXPECT_EQ(LineText(text, lines[0]), "a b ") << "wrap=" << wrap;
        EXPECT_EQ(LineText(text, lines[1]), "c") << "wrap=" << wrap;
        EXPECT_NEAR(lines[0].Width, contentWidth, 0.001f) << "wrap=" << wrap;
    }
}

// The block's reported width is the widest line's content width — what a
// shrink-to-fit box should size to. Chrome's max-content width for the wrapped
// paragraph is the same figure.
TEST_F(RobotoGeometry, BlockWidthIsTheWidestLineContentWidth)
{
    StyledRun run{};
    run.Font = Atlas.get();
    run.PixelSize = 16u;
    run.Text = "The quick brown fox jumps over the lazy dog";
    const auto result = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 150.0f,
                                                   0.0f, TextLayout::WordBreak::Normal);
    ASSERT_EQ(result.LineCount, 3u);
    EXPECT_NEAR(result.Metrics.width, 143.65625f, kChromeAdvanceTolerancePx);

    float widest = 0.0f;
    for (const auto& line : result.LineBreaks)
        widest = std::max(widest, line.Width);
    EXPECT_FLOAT_EQ(result.Metrics.width, widest);
}

// --- Block sizing (issue #711 item 3) ----------------------------------------

// Baseline-to-baseline distance is uniform and the block is exactly N of them,
// which is the structural half of the CSS rule (the other half — what one line
// box measures — is covered by MultilineTextLayoutTests).
TEST_F(RobotoGeometry, BlockHeightIsLineCountTimesBaselineToBaselineDistance)
{
    // Every line starts with the same letter, so comparing the first glyph of
    // consecutive lines compares line boxes rather than the ink height of
    // whichever character happened to land first.
    StyledRun run{};
    run.Font = Atlas.get();
    run.PixelSize = 16u;
    run.Text = "aa aa aa aa aa aa aa aa";

    for (float multiplier : {0.9f, 1.0f, 1.2f, 1.5f, 2.0f})
    {
        const float box = TextLayout::ResolveLineBoxPx(-multiplier, 16.0f, 1.0f);
        const auto result = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 70.0f,
                                                       box, TextLayout::WordBreak::Normal);
        ASSERT_EQ(result.LineCount, 3u) << "multiplier=" << multiplier;
        ASSERT_EQ(result.LineBreaks.size(), 3u);

        const float lineBox = result.Metrics.height / static_cast<float>(result.LineCount);
        const float firstTop = result.Glyphs[result.LineBreaks[0].GlyphStart].y;
        const float secondTop = result.Glyphs[result.LineBreaks[1].GlyphStart].y;
        const float thirdTop = result.Glyphs[result.LineBreaks[2].GlyphStart].y;

        EXPECT_NEAR(lineBox, box, 0.001f) << "multiplier=" << multiplier;
        EXPECT_NEAR(secondTop - firstTop, lineBox, 0.001f) << "multiplier=" << multiplier;
        EXPECT_NEAR(thirdTop - secondTop, lineBox, 0.001f) << "multiplier=" << multiplier;
    }
}

// Half-leading seats the font's ascent/descent box inside the line box, and
// keeps it seated when the line box is SHORTER than the font — CSS lifts the
// ink above the block rather than pinning it to the top.
//
// The leading is FLOORED, not split exactly. Blink takes
// floor(ascent + halfLeading) (FontHeight::AddLeading) and gives the leftover
// half pixel to the descent, which is the same rule ExactLineMetricsWithFace
// already applies to the leading that comes from the font's own lineGap —
// `ArialKeepsTheLineGapItsWinMetricsDoNotCover` below pins that half of it.
// A `line-height` declaration is the other source of leading and rounds the
// same way.
//
// Roboto 16px has a 21px font box and a 17px ascent, so these two line boxes
// straddle it in both directions and put the leading on a half pixel. Measured
// in Chrome 150, --force-device-scale-factor=1, as the top offset of a
// zero-size `vertical-align: baseline` probe inside the element:
//
//     line-height    15.75px   26.25px
//     first baseline      14        19
//
// An exact split would put them at 14.375 and 19.625, which is what this test
// asserted while the engine rounded only the lineGap leading. Neither value is
// one Chrome produces at any line-height sampled from 6px to 64px.
TEST_F(RobotoGeometry, HalfLeadingIsFlooredAndLiftsTheInkWhenTheLineBoxIsTighterThanTheFont)
{
    StyledRun run{};
    run.Font = Atlas.get();
    run.PixelSize = 16u;
    run.Text = "Hxg";

    const auto metrics = Atlas->GetFontLineMetrics(16.0f);
    const float fontBox = metrics.ascender + metrics.descender;
    ASSERT_NEAR(fontBox, 21.0f, 0.001f) << "the Chrome numbers below are for a 21px font box";
    ASSERT_NEAR(metrics.ascender, 17.0f, 0.001f);

    // 'H' sits cap-height above the baseline, so its quad top recovers the
    // baseline without consulting the layout code that is under test.
    const float capHeight = kCapHeightUnits / kUnitsPerEm * 16.0f;

    struct Case { float LineBox; float ChromeBaseline; };
    float baselineAtUnitSpacing = 0.0f;
    for (const Case c : {Case{15.75f, 14.0f}, Case{21.0f, 17.0f}, Case{26.25f, 19.0f}})
    {
        const auto result = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 0.0f,
                                                       c.LineBox, TextLayout::WordBreak::Normal);
        ASSERT_FALSE(result.Glyphs.empty());
        const float baseline = result.Glyphs[0].y + capHeight;
        EXPECT_NEAR(baseline, c.ChromeBaseline, 0.02f) << "lineBox=" << c.LineBox;
        // ...and the exact split is genuinely a different number here, so a
        // pass cannot be satisfied by both rules at once.
        if (c.LineBox != 21.0f)
            EXPECT_GT(std::abs(c.ChromeBaseline - (metrics.ascender + (c.LineBox - fontBox) * 0.5f)),
                      0.3f)
                << "lineBox=" << c.LineBox << " does not discriminate floor from an exact split";
        if (c.LineBox == 21.0f)
            baselineAtUnitSpacing = baseline;
    }

    // Tighter-than-font spacing pulls the ink up, it does not clamp at zero.
    const auto tight = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 0.0f,
                                                  fontBox * 0.75f, TextLayout::WordBreak::Normal);
    ASSERT_FALSE(tight.Glyphs.empty());
    EXPECT_LT(tight.Glyphs[0].y + capHeight, baselineAtUnitSpacing);
}

// --- Documented deviations ----------------------------------------------------

// Runs of whitespace are not collapsed: CSS `white-space: normal` renders
// "alpha  beta" as a single space, the engine renders both. Pinned so the
// deviation is visible and a future collapsing pass has a test to update
// rather than discovering the behaviour by eye.
TEST_F(RobotoGeometry, ConsecutiveSpacesAreNotCollapsed)
{
    const float twoSpaces = Atlas->MeasureUtf8("a  b", 16.0f).width;
    const float oneSpace = Atlas->MeasureUtf8("a b", 16.0f).width;
    const float space = Atlas->MeasureUtf8(" ", 16.0f).width;
    EXPECT_NEAR(twoSpaces - oneSpace, space, 0.01f);
    // Chrome collapses to the single-space width.
    EXPECT_NEAR(oneSpace, 21.640625f, kChromeAdvanceTolerancePx);
    EXPECT_NEAR(twoSpaces, 25.6015625f, kChromeAdvanceTolerancePx);
}

// The font line height comes from the design-unit tables scaled exactly to
// the render size — Roboto 2.001047: (2146 + 555) / 2048 = 1.31885 em, so
// 21.1016 at font-size 16 — and is independent of the atlas ppem. (It used to
// be read from FreeType's rounded size metrics at the atlas ppem: 21.6 with a
// 20px atlas, 21.5 with a 32px one.)
TEST_F(RobotoGeometry, NormalLineHeightMatchesChromeAtEverySizeAndAnyAtlasSize)
{
    auto atlas20 = LoadRobotoAtlas(20u);
    auto atlas32 = LoadRobotoAtlas(32u);
    ASSERT_TRUE(atlas20);
    ASSERT_TRUE(atlas32);

    // Chrome's used `line-height: normal` for this face, read off the same
    // Roboto-Regular.ttf via getBoundingClientRect at dpr 1. Blink rounds
    // ascent, descent and lineGap to whole device pixels independently and
    // sums them; Roboto's lineGap is 0, so this is round(asc) + round(desc).
    struct Case { float PixelSize; float ChromeNormal; };
    const Case cases[] = {
        {12.0f, 16.0f}, {13.0f, 18.0f}, {14.0f, 19.0f},
        {16.0f, 21.0f}, {19.0f, 25.0f}, {24.0f, 32.0f},
    };

    for (const auto& c : cases)
    {
        EXPECT_NEAR(atlas20->GetFontLineMetrics(c.PixelSize).height, c.ChromeNormal, 0.001f)
            << "px=" << c.PixelSize;
        // ...and it does not move with the atlas ppem, which is what the
        // FreeType size-metrics reading used to do.
        EXPECT_NEAR(atlas32->GetFontLineMetrics(c.PixelSize).height, c.ChromeNormal, 0.001f)
            << "px=" << c.PixelSize << " at atlas 32";
    }
}

// MeasureUtf8 and GetFontLineMetrics agree about how tall one line is — both
// come from the same exact design-unit metrics, so a label's per-line height
// does not change the moment it wraps. (They used to disagree by ~0.05em:
// FreeType's separately rounded `height` vs ascender+descender.)
TEST_F(RobotoGeometry, SingleLineAndMultiLineHeightDefinitionsAgree)
{
    // One definition of a line box, whichever entry point asks for it. These
    // used to disagree by ~0.05em, so a label's per-line height changed the
    // moment it wrapped.
    for (float px : {12.0f, 16.0f, 24.0f, 32.0f})
    {
        const float measured = Atlas->MeasureUtf8("Hxg", px).height;
        const float lineMetric = Atlas->GetFontLineMetrics(px).height;
        EXPECT_NEAR(measured, lineMetric, 0.001f) << "px=" << px;
        // Whole device pixels, per Blink's rounding.
        EXPECT_NEAR(lineMetric, std::round(lineMetric), 0.001f) << "px=" << px;
    }
}

// --- `normal` on faces whose metric tables disagree ---------------------------
//
// Roboto's hhea, OS/2 typographic and OS/2 usWin metrics are the same three
// numbers, so no Roboto expectation can tell apart the rules Blink actually
// applies. These faces disagree, and each falsifies a different wrong rule:
//
//   Consolas / Calibri  hhea carries a large lineGap that the usWin pair has
//                       already absorbed, so the totals agree but the baseline
//                       does not — reading hhea puts it 2px high at 16px.
//   Arial               the only one of the four whose lineGap survives into
//                       the line box, so it is the one that exercises the
//                       external-leading term and the floored half-leading.
//   Cascadia Code       sets fsSelection bit 7 (USE_TYPO_METRICS), so its
//                       usWin ascent must be ignored — honouring it would make
//                       every line 10.5% too tall.
//
// Every expected number was measured in Chrome 150 on Windows against the same
// system .ttf this test loads, at deviceScaleFactor 1: the height of a
// `line-height: normal` div and, inside it, the top offset of a zero-height
// inline-block with `vertical-align: baseline`.

namespace
{

// The expectations are absolute pixels tied to one font version, so a machine
// shipping a different Consolas must skip rather than fail. hhea identifies the
// version; the OS/2 tables have not drifted independently of it in these faces.
struct SystemFace
{
    const char* File;
    int Ascender;
    int Descender;
    int Height;
    // Faces outside the 2048-unit Windows norm are the ones that exercise the
    // design-unit scaling itself, so the em size is part of the identity check
    // rather than an assumption.
    int UnitsPerEm = 2048;
};

struct NormalLineBox
{
    float Px;
    float Height;
    float Baseline;
};

std::unique_ptr<FontAtlas> LoadSystemFace(const SystemFace& face, std::string& outWhySkipped)
{
    const char* windir = std::getenv("WINDIR");
    if (!windir)
    {
        outWhySkipped = "no WINDIR: system faces come from the Windows font directory";
        return nullptr;
    }
    const std::string path = std::string(windir) + "\\Fonts\\" + face.File;
    if (!Rendering::Utils::FileExists(path.c_str()))
    {
        outWhySkipped = path + " is not installed";
        return nullptr;
    }
    const std::vector<uint8_t> bytes = Rendering::Utils::ReadFile(path.c_str());
    auto atlas = std::make_unique<FontAtlas>();
    if (bytes.empty() || !atlas->LoadFontBytes(bytes.data(), bytes.size(), kUiAtlasPixelSize))
    {
        outWhySkipped = std::string(face.File) + " failed to load";
        return nullptr;
    }
    const auto info = atlas->GetFaceDebugInfo();
    if (!info || info->unitsPerEm != face.UnitsPerEm || info->ascender != face.Ascender ||
        info->descender != face.Descender || info->height != face.Height)
    {
        outWhySkipped = std::string(face.File) + " is a different version than the one measured";
        return nullptr;
    }
    return atlas;
}

void ExpectChromeNormalLineBoxes(FontAtlas& atlas, const std::vector<NormalLineBox>& rows)
{
    for (const auto& row : rows)
    {
        const auto lm = atlas.GetFontLineMetrics(row.Px);
        EXPECT_NEAR(lm.height, row.Height, 0.001f) << "height at px=" << row.Px;
        EXPECT_NEAR(lm.ascender, row.Baseline, 0.001f) << "baseline at px=" << row.Px;
        // The baseline is a position inside the box, not a fourth number.
        EXPECT_NEAR(lm.ascender + lm.descender, lm.height, 0.001f) << "px=" << row.Px;
    }
}

} // namespace

TEST(ChromeNormalLineBox, ConsolasIgnoresTheHheaLineGapItsWinMetricsAlreadyCover)
{
    // hhea 1521/-527 plus a 350-unit lineGap; usWin 1884/514. Same 2398-unit
    // total, split 363 units higher — reading hhea gives the right height at
    // most sizes and the wrong baseline at nearly all of them.
    std::string why;
    auto atlas = LoadSystemFace({"consola.ttf", 1521, -527, 2398}, why);
    if (!atlas)
        GTEST_SKIP() << why;

    ExpectChromeNormalLineBoxes(*atlas, {
        {8.0f, 9.0f, 7.0f},    {10.0f, 12.0f, 9.0f},  {11.0f, 13.0f, 10.0f},
        {12.0f, 14.0f, 11.0f}, {13.0f, 15.0f, 12.0f}, {14.0f, 17.0f, 13.0f},
        {16.0f, 19.0f, 15.0f}, {18.0f, 22.0f, 17.0f}, {20.0f, 23.0f, 18.0f},
        {24.0f, 28.0f, 22.0f}, {32.0f, 37.0f, 29.0f}, {48.0f, 56.0f, 44.0f},
    });
}

TEST(ChromeNormalLineBox, CalibriIgnoresTheHheaLineGapItsWinMetricsAlreadyCover)
{
    // The same shape as Consolas with a 452-unit gap, and here it moves the
    // height too: hhea rounds to a taller box than Chrome's at 12-18px.
    std::string why;
    auto atlas = LoadSystemFace({"calibri.ttf", 1536, -512, 2500}, why);
    if (!atlas)
        GTEST_SKIP() << why;

    ExpectChromeNormalLineBoxes(*atlas, {
        {8.0f, 10.0f, 8.0f},   {10.0f, 13.0f, 10.0f}, {11.0f, 13.0f, 10.0f},
        {12.0f, 14.0f, 11.0f}, {13.0f, 15.0f, 12.0f}, {14.0f, 17.0f, 13.0f},
        {16.0f, 19.0f, 15.0f}, {18.0f, 22.0f, 17.0f}, {20.0f, 24.0f, 19.0f},
        {24.0f, 29.0f, 23.0f}, {32.0f, 39.0f, 30.0f}, {48.0f, 59.0f, 46.0f},
    });
}

TEST(ChromeNormalLineBox, ArialKeepsTheLineGapItsWinMetricsDoNotCover)
{
    // usWin 1854/434 exactly equals hhea's extent, so all 67 units of hhea's
    // lineGap survive as external leading — the one face here with a non-zero
    // gap in the line box, and therefore the one that pins the floor: its
    // half-leading is a half pixel from 16px up.
    std::string why;
    auto atlas = LoadSystemFace({"arial.ttf", 1854, -434, 2355}, why);
    if (!atlas)
        GTEST_SKIP() << why;

    ExpectChromeNormalLineBoxes(*atlas, {
        {8.0f, 9.0f, 7.0f},    {10.0f, 11.0f, 9.0f},  {11.0f, 12.0f, 10.0f},
        {12.0f, 14.0f, 11.0f}, {13.0f, 15.0f, 12.0f}, {14.0f, 16.0f, 13.0f},
        {16.0f, 18.0f, 14.0f}, {18.0f, 21.0f, 16.0f}, {20.0f, 23.0f, 18.0f},
        {24.0f, 28.0f, 22.0f}, {32.0f, 37.0f, 29.0f}, {48.0f, 55.0f, 44.0f},
    });
}

TEST(ChromeNormalLineBox, CascadiaCodeUsesItsTypographicMetricsNotItsWinAscent)
{
    // fsSelection bit 7 is set and usWin ascent is 2226 against a typographic
    // 1900, so honouring usWin here would make every line 10.5% too tall: 21px
    // instead of 19 at 16px. (14% is the raw design-unit ratio, before the
    // per-term rounding this path applies.)
    std::string why;
    auto atlas = LoadSystemFace({"CascadiaCode.ttf", 1900, -480, 2380}, why);
    if (!atlas)
        GTEST_SKIP() << why;

    ExpectChromeNormalLineBoxes(*atlas, {
        {8.0f, 9.0f, 7.0f},    {10.0f, 11.0f, 9.0f},  {11.0f, 13.0f, 10.0f},
        {12.0f, 14.0f, 11.0f}, {13.0f, 15.0f, 12.0f}, {14.0f, 16.0f, 13.0f},
        {16.0f, 19.0f, 15.0f}, {18.0f, 21.0f, 17.0f}, {20.0f, 24.0f, 19.0f},
        {24.0f, 28.0f, 22.0f}, {32.0f, 38.0f, 30.0f}, {48.0f, 56.0f, 45.0f},
    });
}

// Cascadia Code sets USE_TYPO_METRICS but its sTypoLineGap is 0, so no
// expectation above can tell whether the typographic branch keeps that gap or
// drops it: both rules predict the same box at every Cascadia size. These two
// faces have a non-zero typographic gap, and separate the rules at every size
// listed — dropping the gap loses 3px at 16px on Bahnschrift and 7px on
// Gabriola. Gabriola is also the only expectation here whose em is not 2048,
// which is what pins the design-unit scaling rather than a 2048 constant.
TEST(ChromeNormalLineBox, TypographicFacesKeepTheirTypoLineGap)
{
    std::string why;
    auto bahnschrift = LoadSystemFace({"bahnschrift.ttf", 1626, -422, 2458}, why);
    if (!bahnschrift)
        GTEST_SKIP() << why;

    // typo 1626/-422 with a 410-unit gap; usWin 2036/422 would be both taller
    // and differently split.
    ExpectChromeNormalLineBoxes(*bahnschrift, {
        {8.0f, 10.0f, 7.0f},   {10.0f, 12.0f, 9.0f},  {11.0f, 13.0f, 10.0f},
        {12.0f, 14.0f, 11.0f}, {13.0f, 16.0f, 11.0f}, {14.0f, 17.0f, 12.0f},
        {16.0f, 19.0f, 14.0f}, {18.0f, 22.0f, 16.0f}, {20.0f, 24.0f, 18.0f},
        {24.0f, 29.0f, 21.0f}, {32.0f, 38.0f, 28.0f}, {48.0f, 58.0f, 43.0f},
    });

    auto gabriola = LoadSystemFace({"Gabriola.ttf", 2800, -1296, 6963, 4096}, why);
    if (!gabriola)
        GTEST_SKIP() << "the non-2048-em half of this test needs Gabriola: " << why;

    // 4096 units per em, typo 2800/-1296 and a 2867-unit gap — the largest
    // external leading of any face here, and over half the line box.
    ExpectChromeNormalLineBoxes(*gabriola, {
        {8.0f, 14.0f, 8.0f},   {10.0f, 17.0f, 10.0f}, {11.0f, 19.0f, 12.0f},
        {12.0f, 20.0f, 12.0f}, {13.0f, 22.0f, 13.0f}, {14.0f, 24.0f, 15.0f},
        {16.0f, 27.0f, 16.0f}, {18.0f, 31.0f, 18.0f}, {20.0f, 34.0f, 21.0f},
        {24.0f, 41.0f, 24.0f}, {32.0f, 54.0f, 33.0f}, {48.0f, 82.0f, 50.0f},
    });
}

// External leading is whatever the hhea line gap has left once the usWin pair
// has already covered it, and it never goes negative. Ink Free's gap (336
// units) is smaller than the 374 units its usWin pair adds over hhea's extent,
// so the subtraction is negative and has to clamp. Without the clamp the box
// loses a pixel from 14px up. Its em is 1000, not 2048.
TEST(ChromeNormalLineBox, ExternalLeadingClampsAtZeroWhenTheWinPairOvercoversTheGap)
{
    std::string why;
    auto atlas = LoadSystemFace({"Inkfree.ttf", 665, -199, 1200, 1000}, why);
    if (!atlas)
        GTEST_SKIP() << why;

    ExpectChromeNormalLineBoxes(*atlas, {
        {8.0f, 10.0f, 7.0f},   {10.0f, 12.0f, 9.0f},  {11.0f, 14.0f, 10.0f},
        {12.0f, 15.0f, 11.0f}, {13.0f, 16.0f, 12.0f}, {14.0f, 18.0f, 13.0f},
        {16.0f, 20.0f, 15.0f}, {18.0f, 22.0f, 16.0f}, {20.0f, 25.0f, 18.0f},
        {24.0f, 30.0f, 22.0f}, {32.0f, 39.0f, 29.0f}, {48.0f, 60.0f, 44.0f},
    });
}

// The mirror case, and the one a simplifying edit is most likely to lose: a
// usWin pair SHORTER than hhea's extent produces external leading even though
// hhea declares no line gap at all. Both faces have lineGap 0, so a rule that
// forwarded the gap unchanged — or dropped the term as "usually zero" — gives
// them no leading and a box up to 3px short.
TEST(ChromeNormalLineBox, WinMetricsShorterThanHheaBecomeExternalLeading)
{
    std::string why;
    auto gothic = LoadSystemFace({"GOTHIC.TTF", 2060, -451, 2511}, why);
    if (!gothic)
        GTEST_SKIP() << why;

    // usWin 1989/451 = 2440 against hhea's 2511 extent, so 71 units survive.
    ExpectChromeNormalLineBoxes(*gothic, {
        {8.0f, 10.0f, 8.0f},   {10.0f, 12.0f, 10.0f}, {11.0f, 13.0f, 11.0f},
        {12.0f, 15.0f, 12.0f}, {13.0f, 16.0f, 13.0f}, {14.0f, 17.0f, 14.0f},
        {16.0f, 21.0f, 16.0f}, {18.0f, 22.0f, 17.0f}, {20.0f, 24.0f, 19.0f},
        {24.0f, 29.0f, 23.0f}, {32.0f, 39.0f, 31.0f}, {48.0f, 60.0f, 48.0f},
    });

    auto frank = LoadSystemFace({"frank.ttf", 1505, -543, 2048}, why);
    if (!frank)
        GTEST_SKIP() << "the larger-leading half of this test needs FrankRuehl: " << why;

    // usWin 1462/442 = 1904 against a 2048 extent: 144 units of leading.
    ExpectChromeNormalLineBoxes(*frank, {
        {8.0f, 9.0f, 6.0f},    {10.0f, 10.0f, 7.0f},  {11.0f, 11.0f, 8.0f},
        {12.0f, 13.0f, 9.0f},  {13.0f, 13.0f, 9.0f},  {14.0f, 14.0f, 10.0f},
        {16.0f, 15.0f, 11.0f}, {18.0f, 18.0f, 13.0f}, {20.0f, 19.0f, 14.0f},
        {24.0f, 24.0f, 18.0f}, {32.0f, 32.0f, 24.0f}, {48.0f, 47.0f, 35.0f},
    });
}

// Chrome folds a display's scale factor into the used font size and rounds the
// line box there, so on a 150% display its line box is a whole number of DEVICE
// pixels and a fractional number of CSS ones. The engine rasterises at
// fontSize * contentScale for the same reason, which is why the rounding belongs
// at that size and not at the logical one: rounding logically would pin the box
// to a whole CSS pixel and land up to a device pixel from Chrome.
//
// Measured in Chrome launched with --force-device-scale-factor, converted back
// to device pixels. Both whole and fractional device sizes are listed: the
// rounding happens at the exact fractional device size, so a 13px font at 150%
// (19.5 device px) gets the same box Chrome does. The atlas ppem is still an
// integer, which costs up to 3% of glyph SIZE at those sizes (issue #729) but
// no longer costs a whole pixel of line box.
//
// The measurement method matters, and gets this wrong if chosen casually.
// Driving a headless Chrome with an emulated deviceScaleFactor (Playwright's
// context option, CDP Emulation.setDeviceMetricsOverride) does NOT reproduce a
// scaled display: emulation raises the raster scale but leaves the font
// instantiated at the CSS size, so the line box stays a whole number of CSS
// pixels at every ratio and the rounding looks logical. Only the browser-level
// --force-device-scale-factor flag re-instantiates the font at
// fontSize * scale, which is what a real 150% display does and what the engine
// does when it rasterises at fontSize * contentScale. Measuring the emulated
// path and concluding "Blink rounds in CSS pixels" fits that data perfectly and
// is wrong here: it moves every scaled-display box by up to a device pixel.
TEST(ChromeNormalLineBox, ScaledDisplayLineBoxIsWholeDevicePixelsLikeChrome)
{
    struct Case
    {
        float ContentScale;
        float LogicalPx;
        float DeviceHeight;
        float DeviceBaseline;
    };

    auto check = [](FontAtlas& atlas, const std::vector<Case>& cases)
    {
        for (const auto& c : cases)
        {
            // Exactly how UIManager sizes text for a scaled display — and the
            // point of the case: this is NOT truncated to a whole ppem.
            const float physicalPx = std::max(1.0f, (float)c.LogicalPx * c.ContentScale);
            const auto lm = atlas.GetFontLineMetrics(physicalPx);
            EXPECT_NEAR(lm.height, c.DeviceHeight, 0.001f)
                << "scale=" << c.ContentScale << " logical=" << c.LogicalPx;
            EXPECT_NEAR(lm.ascender, c.DeviceBaseline, 0.001f)
                << "scale=" << c.ContentScale << " logical=" << c.LogicalPx;
        }
    };

    auto roboto = LoadRobotoAtlas();
    ASSERT_TRUE(roboto) << "Staged Roboto-Regular.ttf not found next to the test executable";
    check(*roboto, {
        {1.5f, 8.0f, 16.0f, 13.0f},  {1.5f, 12.0f, 24.0f, 19.0f}, {1.5f, 16.0f, 32.0f, 25.0f},
        {1.5f, 20.0f, 39.0f, 31.0f}, {1.5f, 24.0f, 48.0f, 38.0f}, {1.5f, 32.0f, 63.0f, 50.0f},
        {2.0f, 8.0f, 21.0f, 17.0f},  {2.0f, 12.0f, 32.0f, 25.0f}, {2.0f, 16.0f, 43.0f, 34.0f},
        {2.0f, 20.0f, 53.0f, 42.0f}, {2.0f, 24.0f, 63.0f, 50.0f}, {2.0f, 32.0f, 84.0f, 67.0f},
        // Fractional device sizes. Every row here lands on a different whole
        // pixel than trunc(size * scale) does, which is the whole point.
        {1.25f, 15.0f, 25.0f, 20.0f}, {1.25f, 18.0f, 30.0f, 24.0f}, {1.25f, 19.0f, 31.0f, 25.0f},
        {1.25f, 21.0f, 35.0f, 28.0f}, {1.25f, 22.0f, 36.0f, 29.0f},
        {1.5f, 15.0f, 30.0f, 24.0f},  {1.5f, 17.0f, 34.0f, 27.0f},  {1.5f, 19.0f, 38.0f, 30.0f},
        {1.5f, 21.0f, 42.0f, 33.0f},
        {1.75f, 9.0f, 21.0f, 17.0f},  {1.75f, 13.0f, 30.0f, 24.0f}, {1.75f, 14.0f, 33.0f, 26.0f},
        {1.75f, 15.0f, 35.0f, 28.0f}, {1.75f, 17.0f, 39.0f, 31.0f}, {1.75f, 18.0f, 42.0f, 33.0f},
        {1.75f, 21.0f, 49.0f, 39.0f}, {1.75f, 26.0f, 60.0f, 48.0f},
    });

    std::string why;
    auto consolas = LoadSystemFace({"consola.ttf", 1521, -527, 2398}, why);
    if (!consolas)
        GTEST_SKIP() << "the non-zero-lineGap half of this test needs Consolas: " << why;
    check(*consolas, {
        {1.5f, 8.0f, 14.0f, 11.0f},  {1.5f, 12.0f, 22.0f, 17.0f}, {1.5f, 16.0f, 28.0f, 22.0f},
        {1.5f, 20.0f, 36.0f, 28.0f}, {1.5f, 24.0f, 42.0f, 33.0f}, {1.5f, 32.0f, 56.0f, 44.0f},
        {2.0f, 8.0f, 19.0f, 15.0f},  {2.0f, 12.0f, 28.0f, 22.0f}, {2.0f, 16.0f, 37.0f, 29.0f},
        {2.0f, 20.0f, 47.0f, 37.0f}, {2.0f, 24.0f, 56.0f, 44.0f}, {2.0f, 32.0f, 75.0f, 59.0f},
        // Fractional, including the 13px script face at both Windows steps.
        {1.25f, 11.0f, 16.0f, 13.0f}, {1.25f, 17.0f, 25.0f, 20.0f}, {1.25f, 18.0f, 27.0f, 21.0f},
        {1.25f, 19.0f, 28.0f, 22.0f}, {1.25f, 26.0f, 38.0f, 30.0f},
        {1.5f, 13.0f, 23.0f, 18.0f},  {1.5f, 15.0f, 27.0f, 21.0f},
        {1.75f, 11.0f, 23.0f, 18.0f}, {1.75f, 13.0f, 27.0f, 21.0f}, {1.75f, 14.0f, 29.0f, 23.0f},
        {1.75f, 19.0f, 39.0f, 31.0f}, {1.75f, 21.0f, 43.0f, 34.0f}, {1.75f, 26.0f, 53.0f, 42.0f},
    });

    // A 1000-unit em: the design-unit scaling, not just the rounding, has to be
    // right for the fractional size to land on Chrome's pixel.
    auto inkFree = LoadSystemFace({"Inkfree.ttf", 665, -199, 1200, 1000}, why);
    if (!inkFree)
        GTEST_SKIP() << "the non-2048-em half of this test needs Ink Free: " << why;
    check(*inkFree, {
        {1.25f, 11.0f, 18.0f, 13.0f}, {1.25f, 14.0f, 22.0f, 16.0f}, {1.25f, 15.0f, 23.0f, 17.0f},
        {1.25f, 19.0f, 30.0f, 22.0f}, {1.25f, 26.0f, 41.0f, 30.0f},
        {1.5f, 13.0f, 24.0f, 18.0f},  {1.5f, 19.0f, 35.0f, 26.0f},  {1.5f, 21.0f, 39.0f, 29.0f},
        {1.75f, 10.0f, 22.0f, 16.0f}, {1.75f, 11.0f, 24.0f, 18.0f}, {1.75f, 13.0f, 28.0f, 21.0f},
        {1.75f, 17.0f, 37.0f, 27.0f}, {1.75f, 18.0f, 39.0f, 29.0f}, {1.75f, 22.0f, 48.0f, 35.0f},
    });
}
