#include "UI/Controls/TextArea.h"
#include "FixedScalePlatform.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "UI/ResolvedStyle.h"

#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include "RobotoTestFont.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Text;

namespace
{

class TextAreaLineBreakTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        atlas = UITesting::LoadRobotoAtlas();
        ASSERT_NE(atlas, nullptr)
            << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf");
    }

    std::vector<TextLayout::LineBreakInfo> ShapeLines(
        const std::string& text,
        float wrapWidth,
        float lineBoxPx = 0.0f, // "normal": the font's metric height
        TextLayout::WordBreak wb = TextLayout::WordBreak::BreakWord)
    {
        StyledRun run{};
        run.Font = atlas.get();
        run.PixelSize = kPx;
        run.Text = text;

        auto result = TextLayout::ShapeMultiline(
            std::span<const StyledRun>(&run, 1), wrapWidth, lineBoxPx, wb);

        auto lines = std::move(result.LineBreaks);

        if (!text.empty() && text.back() == '\n')
            lines.push_back({text.size(), text.size()});

        if (lines.empty())
            lines.push_back({0, text.size()});

        return lines;
    }

    float MeasureWidth(const std::string& text)
    {
        return atlas->MeasureText(text, kPx).metrics.width;
    }

    static constexpr unsigned kPx = 16u;
    std::unique_ptr<FontAtlas> atlas;
};

} // namespace

TEST(TextAreaInputTests, IgnoresNonTextControlCharacters)
{
    TextArea area;
    area.SetValue("A");
    area.SetSelection(1, 1);

    area.OnChar(0x04u); // Ctrl+D on Windows can arrive as EOT through WM_CHAR.
    EXPECT_EQ(area.GetValue(), "A");
    EXPECT_EQ(area.GetCaretIndex(), 1);

    area.OnChar(static_cast<unsigned int>('B'));
    EXPECT_EQ(area.GetValue(), "AB");
}

// Select all, copy and paste are the area's own shortcuts (the Mark-ups description and every
// other text area take them from here).
TEST(TextAreaInputTests, SelectAllCopyAndPasteAreTheAreasOwnShortcuts)
{
    UITesting::FixedScalePlatform platform(1.0f);
    TextArea area;
    area.SetValue("The village square");
    area.SetSelection(3, 3);
    ASSERT_TRUE(area.OnKey(Input::kKeyCode_A, Input::kModControl, &platform));
    EXPECT_EQ(area.GetSelectionStart(), 0);
    EXPECT_EQ(area.GetSelectionEnd(), 18);
    ASSERT_TRUE(area.OnKey(Input::kKeyCode_C, Input::kModControl, &platform));
    EXPECT_EQ(platform.GetClipboardText(), "The village square");

    area.SetSelection(18, 18);
    platform.SetClipboardText(", by the well");
    ASSERT_TRUE(area.OnKey(Input::kKeyCode_V, Input::kModControl, &platform));
    EXPECT_EQ(area.GetValue(), "The village square, by the well");
}

// In submit mode Enter submits and leaves the text as it is, and its newline char is absorbed;
// Shift+Enter breaks the line. Without the mode Enter breaks the line.
TEST(TextAreaInputTests, SubmitModeEnterSubmitsAndShiftEnterBreaksTheLine)
{
    TextArea area;
    int submitted = 0;
    area.SetOnSubmit([&submitted]() { ++submitted; });
    area.SetValue("Walls first");
    area.SetSelection(11, 11);

    ASSERT_TRUE(area.OnKey(Input::kKeyCode_Enter, 0, nullptr));
    EXPECT_TRUE(area.OnChar(13u));
    EXPECT_EQ(submitted, 1);
    EXPECT_EQ(area.GetValue(), "Walls first");

    ASSERT_TRUE(area.OnKey(Input::kKeyCode_Enter, Input::kModShift, nullptr));
    EXPECT_TRUE(area.OnChar(13u));
    EXPECT_EQ(submitted, 1);
    EXPECT_EQ(area.GetValue(), "Walls first\n");

    TextArea plain;
    plain.SetValue("a");
    plain.SetSelection(1, 1);
    ASSERT_TRUE(plain.OnKey(Input::kKeyCode_Enter, 0, nullptr));
    EXPECT_EQ(plain.GetValue(), "a\n");
}

// =============================================================================
// Line break correctness
// =============================================================================

TEST_F(TextAreaLineBreakTest, NoWrapping_NewlinesOnly)
{
    auto lines = ShapeLines("hello\nworld", 0.0f);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_EQ(lines[0].ByteEnd, 5u);
    EXPECT_EQ(lines[1].ByteStart, 6u);
    EXPECT_EQ(lines[1].ByteEnd, 11u);
}

TEST_F(TextAreaLineBreakTest, EmptyText_SingleLine)
{
    StyledRun run{};
    run.Font = atlas.get();
    run.PixelSize = kPx;
    run.Text = "";

    auto result = TextLayout::ShapeMultiline(
        std::span<const StyledRun>(&run, 1), 100.0f, 0.0f);

    // ShapeMultiline returns empty lineBreaks for empty text;
    // callers add a default entry
    EXPECT_TRUE(result.LineBreaks.empty());
}

TEST_F(TextAreaLineBreakTest, TrailingNewline_ExtraEmptyLine)
{
    auto lines = ShapeLines("hello\n", 0.0f);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_EQ(lines[0].ByteEnd, 5u);
    EXPECT_EQ(lines[1].ByteStart, 6u);
    EXPECT_EQ(lines[1].ByteEnd, 6u);
}

TEST_F(TextAreaLineBreakTest, WordBoundaryWrap)
{
    const std::string text = "hello world foo";
    float helloWidth = MeasureWidth("hello ");
    float helloWorldWidth = MeasureWidth("hello world");

    // Pick a width that fits "hello " but not "hello world"
    float wrapWidth = (helloWidth + helloWorldWidth) * 0.5f;
    ASSERT_GT(wrapWidth, helloWidth);
    ASSERT_LT(wrapWidth, helloWorldWidth);

    auto lines = ShapeLines(text, wrapWidth);
    ASSERT_GE(lines.size(), 2u);

    // First visual line should end at or before "hello "
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_LE(lines[0].ByteEnd, 11u);
    EXPECT_GE(lines[0].ByteEnd, 5u);
}

TEST_F(TextAreaLineBreakTest, ForcedCharacterBreak_NoSpaces)
{
    const std::string text = "abcdefghijklmnopqrstuvwxyz";
    float fullWidth = MeasureWidth(text);

    // Wrap at roughly half the text width
    float wrapWidth = fullWidth * 0.5f;
    auto lines = ShapeLines(text, wrapWidth);

    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_GT(lines[0].ByteEnd, 0u);
    EXPECT_LT(lines[0].ByteEnd, text.size());
    EXPECT_EQ(lines[1].ByteStart, lines[0].ByteEnd);
}

TEST_F(TextAreaLineBreakTest, MixedNewlinesAndWrapping)
{
    const std::string text = "short\nabcdefghijklmnopqrstuvwxyz\nend";
    float longLineWidth = MeasureWidth("abcdefghijklmnopqrstuvwxyz");

    float wrapWidth = longLineWidth * 0.5f;
    auto lines = ShapeLines(text, wrapWidth);

    // "short" on line 0, long line wraps to 2+ visual lines, "end" on last line
    ASSERT_GE(lines.size(), 4u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_EQ(lines[0].ByteEnd, 5u);

    // Last line should be "end"
    auto& lastLine = lines.back();
    EXPECT_EQ(lastLine.ByteEnd, text.size());
}

TEST_F(TextAreaLineBreakTest, ConsecutiveNewlines)
{
    auto lines = ShapeLines("a\n\nb", 0.0f);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_EQ(lines[0].ByteEnd, 1u);
    // Empty line between the two newlines
    EXPECT_EQ(lines[2].ByteStart, 3u);
    EXPECT_EQ(lines[2].ByteEnd, 4u);
}

// =============================================================================
// Line breaks match between overlay and renderer
// =============================================================================

TEST_F(TextAreaLineBreakTest, OverlayAndRendererLineBreaksAreIdentical)
{
    // This is the core test: selection overlay byte ranges must be the same
    // as those used by the text renderer, since both go through ShapeMultiline.
    const std::string text =
        "c:\\work\\sampleapp\\build\\win64-local-debugfast\\bin\\debug\\apps\\editor\\assets"
        "\\materials\\newsurface.glsl: error: #version: Desktop shaders for Vulkan SPIR-V "
        "require version 140 or higher";

    float narrowWidth = MeasureWidth(text) * 0.3f;

    StyledRun run{};
    run.Font = atlas.get();
    run.PixelSize = kPx;
    run.Text = text;

    auto result = TextLayout::ShapeMultiline(
        std::span<const StyledRun>(&run, 1), narrowWidth, 0.0f,
        TextLayout::WordBreak::BreakWord);

    ASSERT_GT(result.LineBreaks.size(), 1u);

    // Every line should start where the previous one ended
    for (size_t i = 1; i < result.LineBreaks.size(); ++i) {
        EXPECT_EQ(result.LineBreaks[i].ByteStart, result.LineBreaks[i - 1].ByteEnd)
            << "Gap between visual line " << (i - 1) << " and " << i;
    }

    // First line starts at 0, last line ends at text.size()
    EXPECT_EQ(result.LineBreaks.front().ByteStart, 0u);
    EXPECT_EQ(result.LineBreaks.back().ByteEnd, text.size());
}

// =============================================================================
// Selection byte ranges span visual lines correctly
// =============================================================================

TEST_F(TextAreaLineBreakTest, SelectionAcrossWrapBoundary)
{
    const std::string text = "abcdefghijklmnopqrstuvwxyz";
    float fullWidth = MeasureWidth(text);
    float wrapWidth = fullWidth * 0.4f;

    auto lines = ShapeLines(text, wrapWidth);
    ASSERT_GE(lines.size(), 2u);

    // Simulate selecting from byte 2 to byte at line 1
    size_t selStart = 2;
    size_t selEnd = lines[1].ByteStart + 3;

    // Find which visual lines the selection spans
    int lineA = -1, lineB = -1;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (selStart >= lines[i].ByteStart && selStart <= lines[i].ByteEnd)
            lineA = i;
        if (selEnd >= lines[i].ByteStart && selEnd <= lines[i].ByteEnd)
            lineB = i;
    }

    EXPECT_EQ(lineA, 0);
    EXPECT_EQ(lineB, 1);
    EXPECT_GT(lineB, lineA);
}

// =============================================================================
// Caret position on wrapped line
// =============================================================================

TEST_F(TextAreaLineBreakTest, CaretPositionOnWrappedLine)
{
    const std::string text = "abcdefghijklmnopqrstuvwxyz";
    float fullWidth = MeasureWidth(text);
    float wrapWidth = fullWidth * 0.4f;

    auto lines = ShapeLines(text, wrapWidth);
    ASSERT_GE(lines.size(), 2u);

    // Build caret map for the second visual line
    std::string lineText(text.data() + lines[1].ByteStart,
                          lines[1].ByteEnd - lines[1].ByteStart);
    std::vector<float> caretMap;
    atlas->BuildCaretMapUtf8(lineText, kPx, caretMap);

    // At the start of the line, caret x should be 0
    ASSERT_FALSE(caretMap.empty());
    EXPECT_FLOAT_EQ(caretMap[0], 0.0f);

    // Caret at position 3 within the line should have positive x
    size_t localByte = 3;
    if (localByte < caretMap.size()) {
        EXPECT_GT(caretMap[localByte], 0.0f);
    }
}

// =============================================================================
// No-wrap fast path consistency
// =============================================================================

// Verify that a simple newline scan (the no-wrap fast path in TextRenderCache)
// produces the same line breaks as ShapeMultiline with wrapWidth=0.
TEST_F(TextAreaLineBreakTest, NoWrapFastPath_MatchesShapeMultiline)
{
    const std::string text = "first\nsecond\nthird";
    auto shapeLines = ShapeLines(text, 0.0f);

    // The fast path scans for '\n' and produces:
    // [0,5), [6,12), [13,18)
    ASSERT_EQ(shapeLines.size(), 3u);
    EXPECT_EQ(shapeLines[0].ByteStart, 0u);
    EXPECT_EQ(shapeLines[0].ByteEnd, 5u);
    EXPECT_EQ(shapeLines[1].ByteStart, 6u);
    EXPECT_EQ(shapeLines[1].ByteEnd, 12u);
    EXPECT_EQ(shapeLines[2].ByteStart, 13u);
    EXPECT_EQ(shapeLines[2].ByteEnd, 18u);
}

TEST_F(TextAreaLineBreakTest, NoWrapFastPath_SingleLineNoNewline)
{
    auto lines = ShapeLines("hello world", 0.0f);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_EQ(lines[0].ByteEnd, 11u);
}

TEST_F(TextAreaLineBreakTest, NoWrapFastPath_OnlyNewlines)
{
    auto lines = ShapeLines("\n\n\n", 0.0f);
    // Three newlines produce: [0,0), [1,1), [2,2), [3,3) (trailing newline extra)
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_EQ(lines[0].ByteEnd, 0u);
    EXPECT_EQ(lines[3].ByteStart, 3u);
    EXPECT_EQ(lines[3].ByteEnd, 3u);
}

// =============================================================================
// Generic text shape cache key behavior
// =============================================================================

TEST_F(TextAreaLineBreakTest, ShapeCacheKey_ColorExcluded)
{
    // Shape the same text with different colors; glyph positions must be identical.
    FontAtlas::ShapeResult resultA, resultB;
    atlas->ShapeText("test", kPx, resultA, 0xFFFF0000u);
    atlas->ShapeText("test", kPx, resultB, 0xFF00FF00u);

    ASSERT_EQ(resultA.glyphs.size(), resultB.glyphs.size());
    for (size_t i = 0; i < resultA.glyphs.size(); ++i)
    {
        EXPECT_FLOAT_EQ(resultA.glyphs[i].x, resultB.glyphs[i].x);
        EXPECT_FLOAT_EQ(resultA.glyphs[i].y, resultB.glyphs[i].y);
        EXPECT_FLOAT_EQ(resultA.glyphs[i].width, resultB.glyphs[i].width);
        EXPECT_FLOAT_EQ(resultA.glyphs[i].height, resultB.glyphs[i].height);
    }
}

// =============================================================================
// NoWrap vs Wrap: line count and height consistency
// Guards against the scroll range overcount bug where Yoga measures with
// wrapping but the TextArea renders without wrapping, creating phantom
// vertical scroll space.
// =============================================================================

TEST_F(TextAreaLineBreakTest, NoWrap_FewerLinesThanWrapped)
{
    const std::string text =
        "a long line that should wrap when given a narrow width "
        "and keeps going with more words to force multiple wraps";

    float fullWidth = MeasureWidth(text);
    float narrowWidth = fullWidth * 0.3f;

    auto wrappedLines = ShapeLines(text, narrowWidth);
    auto noWrapLines = ShapeLines(text, 0.0f);

    EXPECT_EQ(noWrapLines.size(), 1u) << "No-wrap should produce exactly one line";
    EXPECT_GT(wrappedLines.size(), 1u) << "Narrow wrap should produce multiple lines";
    EXPECT_LT(noWrapLines.size(), wrappedLines.size());
}

TEST_F(TextAreaLineBreakTest, NoWrap_HeightNotExceedWrappedHeight)
{
    // Simulates the scroll range bug: if Yoga measures with wrapping, the
    // intrinsic height is (wrappedLines * lineAdvance). If the TextArea renders
    // without wrapping (fewer lines), the scroll content extends beyond visible
    // text. This test verifies no-wrap height <= wrapped height.
    const std::string text =
        "function calculateSum(a, b) { return a + b; } // a long line\n"
        "const result = calculateSum(42, 58);\n"
        "console.log(result);";

    float fullWidth = MeasureWidth(
        "function calculateSum(a, b) { return a + b; } // a long line");
    float narrowWidth = fullWidth * 0.4f;

    auto wrappedLines = ShapeLines(text, narrowWidth);
    auto noWrapLines = ShapeLines(text, 0.0f);

    EXPECT_EQ(noWrapLines.size(), 3u) << "Three real lines when not wrapping";
    EXPECT_GT(wrappedLines.size(), noWrapLines.size())
        << "Wrapping a long line should produce more visual lines";

    auto lm = atlas->GetFontLineMetrics(kPx);
    float lineAdvance = std::max(1.0f, lm.height); // "normal" line box

    float noWrapHeight = (float)noWrapLines.size() * lineAdvance;
    float wrappedHeight = (float)wrappedLines.size() * lineAdvance;

    EXPECT_LE(noWrapHeight, wrappedHeight)
        << "No-wrap content height must not exceed wrapped height; "
           "using wrapped height for scroll range causes phantom scroll space";
}

TEST_F(TextAreaLineBreakTest, NoWrap_MultilinePreservesNewlines)
{
    // Verify no-wrap mode still respects explicit newlines (only disables
    // soft wrapping). This is the expected behavior for white-space: nowrap.
    const std::string text = "line one\nline two\nline three\nline four";
    auto lines = ShapeLines(text, 0.0f);

    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0].ByteStart, 0u);
    EXPECT_EQ(lines[0].ByteEnd, 8u);
    EXPECT_EQ(lines[1].ByteStart, 9u);
    EXPECT_EQ(lines[1].ByteEnd, 17u);
    EXPECT_EQ(lines[2].ByteStart, 18u);
    EXPECT_EQ(lines[2].ByteEnd, 28u);
    EXPECT_EQ(lines[3].ByteStart, 29u);
    EXPECT_EQ(lines[3].ByteEnd, 38u);
}

// =============================================================================
// Binary search visualLineOfByte correctness
// Verifies the binary search over visual lines matches a linear scan for
// every byte position in a multi-line text.
// =============================================================================

TEST_F(TextAreaLineBreakTest, VisualLineOfByte_BinarySearchMatchesLinear)
{
    const std::string text = "aaa\nbbb\nccc\nddd\neee";
    auto vLines = ShapeLines(text, 0.0f);
    const int totalLines = static_cast<int>(vLines.size());
    ASSERT_EQ(totalLines, 5);

    // Linear scan reference implementation (always returns earliest matching line)
    auto linearSearch = [&](size_t byteIdx) -> int
    {
        for (int i = 0; i < totalLines; ++i)
        {
            if (byteIdx >= vLines[i].ByteStart && byteIdx <= vLines[i].ByteEnd)
                return i;
        }
        return std::max(0, totalLines - 1);
    };

    // Binary search (mirrors the production code in TextArea::OnGeneratePrimitives)
    auto binarySearch = [&](size_t byteIdx) -> int
    {
        int lo = 0, hi = totalLines - 1;
        while (lo <= hi)
        {
            int mid = lo + (hi - lo) / 2;
            if (byteIdx < vLines[mid].ByteStart)
                hi = mid - 1;
            else if (byteIdx > vLines[mid].ByteEnd)
                lo = mid + 1;
            else
            {
                if (mid > 0 &&
                    byteIdx == vLines[mid].ByteStart &&
                    byteIdx <= vLines[mid - 1].ByteEnd)
                {
                    return mid - 1;
                }
                return mid;
            }
        }
        return std::max(0, totalLines - 1);
    };

    // Check every byte position including one past the end
    for (size_t b = 0; b <= text.size(); ++b)
    {
        EXPECT_EQ(binarySearch(b), linearSearch(b))
            << "Mismatch at byte " << b;
    }
}

TEST_F(TextAreaLineBreakTest, VisualLineOfByte_BinarySearchWrappedText)
{
    const std::string text = "abcdefghijklmnopqrstuvwxyz 0123456789";
    float fullWidth = MeasureWidth(text);
    auto vLines = ShapeLines(text, fullWidth * 0.4f);
    const int totalLines = static_cast<int>(vLines.size());
    ASSERT_GE(totalLines, 2) << "Text should wrap at 40% width";

    auto linearSearch = [&](size_t byteIdx) -> int
    {
        for (int i = 0; i < totalLines; ++i)
        {
            if (byteIdx >= vLines[i].ByteStart && byteIdx <= vLines[i].ByteEnd)
                return i;
        }
        return std::max(0, totalLines - 1);
    };

    auto binarySearch = [&](size_t byteIdx) -> int
    {
        int lo = 0, hi = totalLines - 1;
        while (lo <= hi)
        {
            int mid = lo + (hi - lo) / 2;
            if (byteIdx < vLines[mid].ByteStart)
                hi = mid - 1;
            else if (byteIdx > vLines[mid].ByteEnd)
                lo = mid + 1;
            else
            {
                if (mid > 0 &&
                    byteIdx == vLines[mid].ByteStart &&
                    byteIdx <= vLines[mid - 1].ByteEnd)
                {
                    return mid - 1;
                }
                return mid;
            }
        }
        return std::max(0, totalLines - 1);
    };

    for (size_t b = 0; b <= text.size(); ++b)
    {
        EXPECT_EQ(binarySearch(b), linearSearch(b))
            << "Mismatch at byte " << b << " in wrapped text";
    }
}

// =============================================================================
// Glyph move pattern: shaping the same text twice doesn't crash or corrupt
// =============================================================================

TEST_F(TextAreaLineBreakTest, ShapeText_MoveAndReshape_NoCorruption)
{
    // Simulates the move pattern used in TextArea::EmitTextGlyphs:
    // 1. Move cached glyphs into scratchResult (recycling capacity)
    // 2. Shape into scratchResult (clears then fills)
    // 3. Move result back to cache
    const std::string text = "Hello, world!";
    FontAtlas::ShapeResult scratch;
    std::vector<FontAtlas::GlyphPlacement> cache;

    // First shape: populate cache
    atlas->ShapeText(text, kPx, scratch, 0xFFFFFFFFu);
    cache = std::move(scratch.glyphs);
    ASSERT_FALSE(cache.empty());
    size_t firstCount = cache.size();

    // Second shape: move cache into scratch, reshape, move back
    scratch.glyphs = std::move(cache);
    atlas->ShapeText(text, kPx, scratch, 0xFF000000u);
    cache = std::move(scratch.glyphs);

    ASSERT_EQ(cache.size(), firstCount)
        << "Same text should produce same glyph count after move+reshape";

    // Verify glyphs are valid (non-negative dimensions)
    for (const auto& gp : cache)
    {
        EXPECT_GE(gp.width, 0.0f);
        EXPECT_GE(gp.height, 0.0f);
    }
}

TEST_F(TextAreaLineBreakTest, ShapeText_MoveAndReshape_DifferentText)
{
    // Verify the move pattern works when the replacement text differs in length
    FontAtlas::ShapeResult scratch;
    std::vector<FontAtlas::GlyphPlacement> cache;

    // Shape short text first
    atlas->ShapeText("Hi", kPx, scratch, 0xFFFFFFFFu);
    cache = std::move(scratch.glyphs);
    ASSERT_FALSE(cache.empty());
    size_t shortCount = cache.size();

    // Now shape longer text via the move pattern
    scratch.glyphs = std::move(cache);
    atlas->ShapeText("A much longer string for testing", kPx, scratch, 0xFFFFFFFFu);
    cache = std::move(scratch.glyphs);

    EXPECT_GT(cache.size(), shortCount)
        << "Longer text should produce more glyphs";

    for (const auto& gp : cache)
    {
        EXPECT_GE(gp.width, 0.0f);
        EXPECT_GE(gp.height, 0.0f);
    }
}
