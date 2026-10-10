// Tests for TextInput pointer-to-caret mapping and drag selection.
// These tests verify that clicking at specific X positions places the caret
// at the expected character boundary, and that drag selection works correctly.

#include "UI/Controls/TextField.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/ResolvedStyle.h"

#include "Rendering/Text/FontAtlas.h"
#include "RobotoTestFont.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Text;

namespace
{

// Compute the text base X used by TextInput for left-aligned text.
static float ComputeTextBaseX(float x, float padL)
{
    return x + padL;
}

    // Small test helper that exposes Field<T>::ComputePointerStyle for
    // verification. This lets tests call the protected static helper used by
    // TextFieldBase<T> without changing the production API surface.
    class PointerStyleTestField : public Field<std::string>
    {
    public:
        using Field<std::string>::Field;

        static ResolvedStyle InvokeComputePointerStyle(UIManager* owner, const UIElement* element)
        {
            return ComputePointerStyle(owner, element);
        }
    };

} // namespace

TEST(TextInputTests, IgnoresNonTextControlCharacters)
{
    TextInput tf;
    tf.SetValue("A");
    tf.OnFocusChanged(true);

    tf.OnChar(0x04u); // Ctrl+D on Windows can arrive as EOT through WM_CHAR.
    EXPECT_EQ(tf.GetValue(), "A");
    EXPECT_EQ(tf.GetCaretIndex(), 0);

    tf.OnChar(static_cast<unsigned int>('B'));
    EXPECT_EQ(tf.GetValue(), "BA");
}

// Test that clicking at each caret boundary position places caret correctly.
TEST(TextFieldPointerTests, ClickAtCaretBoundariesPlacesCaretCorrectly)
{
    auto atlas = UITesting::LoadRobotoAtlas();
    ASSERT_NE(atlas, nullptr)
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf");

    const std::string text = "0.0000000000000000"; // 18 characters
    const unsigned px = 16u;
    const float padL = 4.0f;
    const float padR = 4.0f;
    const float fieldX = 50.0f;
    const float fieldW = 200.0f;
    const float fieldY = 40.0f;
    const float fieldH = 24.0f;

    auto measure = atlas->MeasureText(text, px);
    const std::vector<float>& caretMap = measure.caretXByByte;

    // Verify caret map has expected size (text.size() + 1 entries)
    ASSERT_EQ(caretMap.size(), text.size() + 1);

    ResolvedStyle style{};
    style.Layout.Padding.Left = padL;
    style.Layout.Padding.Right = padR;
    style.Layout.Padding.Top = style.Layout.Padding.Bottom = 2.0f;
    style.Visual.FontSize = static_cast<float>(px);
    style.Visual.TextAlign = TextAlign::Left;
    style.Visual.Color = 0xFF000000u;

    float xBase = ComputeTextBaseX(fieldX, padL);

    // Test clicking at each caret position. Each iteration uses a fresh TextInput
    // to avoid triggering double-click detection (consecutive OnPointerDown calls
    // within 400ms and 20px would otherwise trigger SelectAll).
    for (size_t expectedCaret = 0; expectedCaret <= text.size(); ++expectedCaret)
    {
        SCOPED_TRACE(::testing::Message() << "expectedCaret=" << expectedCaret);

        TextInput tf;
        tf.SetId("pointer-test");
        tf.SetValue(text);
        tf.OnFocusChanged(true);

        float caretX = caretMap[expectedCaret];
        float mouseX = xBase + caretX;

        tf.OnPointerDown(mouseX, fieldY + fieldH / 2.0f,
                         fieldX, fieldY, fieldW, fieldH,
                         style, atlas.get());

        int actualCaret = tf.GetCaretIndex();
        int diff = std::abs(actualCaret - static_cast<int>(expectedCaret));

        EXPECT_EQ(diff, 0)
            << "Clicking at exact caret position " << expectedCaret
            << " (mouseX=" << mouseX << ", caretX=" << caretX << ")"
            << " placed caret at " << actualCaret;
    }
}

// Glyph-midpoint snapping is covered against the PAINTED caret positions in
// TextPenPlacementParityTests (ClickResolvesToTheCaretItLandsNearestInPainted-
// Space), which reads the origin back from the emitted primitives instead of
// reconstructing it here.

// Test drag selection from start to end of string.
TEST(TextFieldPointerTests, DragSelectionFromStartToEnd)
{
    auto atlas = UITesting::LoadRobotoAtlas();
    ASSERT_NE(atlas, nullptr)
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf");

    const std::string text = "0.0000000000000000";
    const unsigned px = 16u;
    const float padL = 4.0f;
    const float padR = 4.0f;
    const float fieldX = 50.0f;
    const float fieldW = 200.0f;
    const float fieldY = 40.0f;
    const float fieldH = 24.0f;

    auto measure = atlas->MeasureText(text, px);
    const std::vector<float>& caretMap = measure.caretXByByte;

    TextInput tf;
    tf.SetId("drag-test");
    tf.SetValue(text);
    tf.OnFocusChanged(true);

    ResolvedStyle style{};
    style.Layout.Padding.Left = padL;
    style.Layout.Padding.Right = padR;
    style.Layout.Padding.Top = style.Layout.Padding.Bottom = 2.0f;
    style.Visual.FontSize = static_cast<float>(px);
    style.Visual.TextAlign = TextAlign::Left;
    style.Visual.Color = 0xFF000000u;

    float xBase = ComputeTextBaseX(fieldX, padL);

    // Click at start
    float startMouseX = xBase + caretMap[0];
    tf.OnPointerDown(startMouseX, fieldY + fieldH / 2.0f,
                     fieldX, fieldY, fieldW, fieldH,
                     style, atlas.get());

    EXPECT_EQ(tf.GetCaretIndex(), 0);
    EXPECT_EQ(tf.GetSelectionStart(), 0);
    EXPECT_EQ(tf.GetSelectionEnd(), 0);

    // Drag to end
    float endMouseX = xBase + caretMap[text.size()];
    tf.OnPointerDrag(endMouseX, fieldY + fieldH / 2.0f,
                     fieldX, fieldY, fieldW, fieldH,
                     style, atlas.get());

    EXPECT_EQ(tf.GetCaretIndex(), static_cast<int>(text.size()));
    EXPECT_EQ(tf.GetSelectionStart(), 0);
    EXPECT_EQ(tf.GetSelectionEnd(), static_cast<int>(text.size()));
}

// Test drag selection beyond field edges.
TEST(TextFieldPointerTests, DragBeyondFieldEdgesSelectsAll)
{
    auto atlas = UITesting::LoadRobotoAtlas();
    ASSERT_NE(atlas, nullptr)
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf");

    const std::string text = "0.0000000000000000";
    const unsigned px = 16u;
    const float padL = 4.0f;
    const float padR = 4.0f;
    const float fieldX = 50.0f;
    const float fieldW = 80.0f; // Narrow field so text overflows
    const float fieldY = 40.0f;
    const float fieldH = 24.0f;

    TextInput tf;
    tf.SetId("edge-drag-test");
    tf.SetValue(text);
    tf.OnFocusChanged(true);

    ResolvedStyle style{};
    style.Layout.Padding.Left = padL;
    style.Layout.Padding.Right = padR;
    style.Layout.Padding.Top = style.Layout.Padding.Bottom = 2.0f;
    style.Visual.FontSize = static_cast<float>(px);
    style.Visual.TextAlign = TextAlign::Left;
    style.Visual.Color = 0xFF000000u;

    // Click at start (before field)
    float startMouseX = fieldX - 10.0f;
    tf.OnPointerDown(startMouseX, fieldY + fieldH / 2.0f,
                     fieldX, fieldY, fieldW, fieldH,
                     style, atlas.get());

    EXPECT_EQ(tf.GetCaretIndex(), 0) << "Clicking before field should place caret at 0";

    // Drag far beyond field right edge
    float endMouseX = fieldX + fieldW + 100.0f;
    tf.OnPointerDrag(endMouseX, fieldY + fieldH / 2.0f,
                     fieldX, fieldY, fieldW, fieldH,
                     style, atlas.get());

    EXPECT_EQ(tf.GetCaretIndex(), static_cast<int>(text.size()))
        << "Dragging beyond right edge should place caret at end";
    EXPECT_EQ(tf.GetSelectionStart(), 0);
    EXPECT_EQ(tf.GetSelectionEnd(), static_cast<int>(text.size()));
}
	
// NOTE: Similarly, an earlier regression test that probed glyph midpoints for
// a run of repeated zeros has been removed. The real-world bug it guarded
// against is now covered by the boundary tests above and the full
// UIManager-driven end-to-end tests in UIManagerOrderTests.

// Ensure that when a resolved style is present on a UIElement, the pointer
// style helper uses it directly instead of recomputing from CSS. This guards
// against regressions where pointer hit-testing would see a different
// fontSize than the one used by layout/rendering.
TEST(TextFieldPointerTests, ComputePointerStyleUsesResolvedStyleWhenAvailable)
{
    ResolvedStyle resolved{};
    resolved.Visual.FontSize = 14.0f;
    resolved.Visual.HasFontSize = true;

    TextInput tf;
    tf.SetId("resolved-style-test");
    tf.SetValue("0000000000");
    tf.OnFocusChanged(true);

    // Simulate UIManager having populated the resolved style during layout.
    tf.GetMutableResolvedStyle() = resolved;

    // Owner may be null here; ComputePointerStyle should still honor the
    // resolved style and not attempt to consult the stylesheet.
    ResolvedStyle pointerStyle = PointerStyleTestField::InvokeComputePointerStyle(nullptr, &tf);

    EXPECT_FLOAT_EQ(pointerStyle.Visual.FontSize, resolved.Visual.FontSize)
        << "Pointer style should use resolved fontSize from UIElement";
}

// Pointer routing does box-model arithmetic on Layout.Padding, so the copy it
// works from has to carry the USED length. A `padding: 10%` element whose solve
// resolved 40px must hand the pointer path 40, not 10 — otherwise hit-testing
// insets by 10 while glyph emission insets by 40 and the caret lands 30px off
// the character it was clicked on.
TEST(TextFieldPointerTests, ComputePointerStyleSubstitutesTheUsedPadding)
{
    TextInput tf;
    tf.SetId("used-padding-test");

    ResolvedStyle resolved{};
    resolved.Layout.Padding = Box4{10.0f, 10.0f, 10.0f, 10.0f};
    resolved.Layout.PaddingIsPercent = Box4b{true, true, true, true};
    tf.GetMutableResolvedStyle() = resolved;
    // What CommitLayoutRects would have committed: 10% of a 400px containing
    // block, on every edge (CSS resolves the vertical edges against width too).
    UILayoutAccess::SetLayoutPadding(tf, Box4{40.0f, 40.0f, 40.0f, 40.0f});

    ResolvedStyle pointerStyle = PointerStyleTestField::InvokeComputePointerStyle(nullptr, &tf);

    EXPECT_FLOAT_EQ(pointerStyle.Layout.Padding.Left, 40.0f);
    EXPECT_FLOAT_EQ(pointerStyle.Layout.Padding.Top, 40.0f);
    EXPECT_FLOAT_EQ(pointerStyle.Layout.Padding.Right, 40.0f);
    EXPECT_FLOAT_EQ(pointerStyle.Layout.Padding.Bottom, 40.0f);
    // The percent flags must not survive: a downstream reader that still
    // branched on them would treat the substituted length as a percentage.
    EXPECT_EQ(pointerStyle.Layout.PaddingIsPercent, Box4b{});
    // The element's own style is untouched — the substitution lives in the copy.
    EXPECT_TRUE(tf.GetResolvedStyle().Layout.PaddingIsPercent.Left);
    EXPECT_FLOAT_EQ(tf.GetResolvedStyle().Layout.Padding.Left, 10.0f);
}

// End-to-end style + pointer mapping check: verify that when a resolved
// style supplies the font size, TextInput::OnPointerDown uses that size for
// caret mapping and clicking at a caret boundary lands exactly on that
// caret index.
TEST(TextFieldPointerTests, ResolvedStyleFontSizeDrivesPointerCaretMapping)
{
    auto atlas = UITesting::LoadRobotoAtlas();
    ASSERT_NE(atlas, nullptr)
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf");

    const std::string text = "0000000000";
    const float padL = 4.0f;
    const float padR = 4.0f;
    const float fieldX = 50.0f;
    const float fieldW = 200.0f;
    const float fieldY = 40.0f;
    const float fieldH = 24.0f;

    ResolvedStyle resolved{};
    resolved.Layout.Padding.Left = padL;
    resolved.Layout.Padding.Right = padR;
    resolved.Layout.Padding.Top = resolved.Layout.Padding.Bottom = 2.0f;
    resolved.Visual.FontSize = 14.0f;
    resolved.Visual.HasFontSize = true;
    resolved.Visual.TextAlign = TextAlign::Left;
    resolved.Visual.Color = 0xFF000000u;

    const float px = resolved.Visual.FontSize;
    auto measure = atlas->MeasureText(text, px);
    const std::vector<float>& caretMap = measure.caretXByByte;
    ASSERT_EQ(caretMap.size(), text.size() + 1);

    TextInput tf2;
    tf2.SetId("resolved-style-pointer-map-test");
    tf2.SetValue(text);
    tf2.OnFocusChanged(true);

    tf2.GetMutableResolvedStyle() = resolved;
    // ComputePointerStyle substitutes the padding the SOLVE resolved, so a
    // fixture that hand-builds a style has to hand-build the layout side too —
    // a real element has both, and only the layout side is a used length.
    UILayoutAccess::SetLayoutPadding(tf2, Box4{2.0f, padR, 2.0f, padL});

    ResolvedStyle pointerStyle = PointerStyleTestField::InvokeComputePointerStyle(nullptr, &tf2);
    EXPECT_FLOAT_EQ(pointerStyle.Visual.FontSize, resolved.Visual.FontSize)
        << "Pointer style should reflect resolved fontSize";

    float xBase = ComputeTextBaseX(fieldX, padL);

	// Click at a couple of caret boundary positions (start and end) and verify
	// that we land exactly on those caret indices.
	for (size_t pass = 0; pass < 2; ++pass)
	{
	    const size_t expectedCaret = (pass == 0) ? 0u : text.size();
	    SCOPED_TRACE(::testing::Message() << "expectedCaret=" << expectedCaret);

	    float caretX = caretMap[expectedCaret];
	    float mouseX = xBase + caretX;

	    tf2.OnPointerDown(mouseX, fieldY + fieldH * 0.5f,
	                      fieldX, fieldY, fieldW, fieldH,
	                      pointerStyle, atlas.get());

	    EXPECT_EQ(tf2.GetCaretIndex(), static_cast<int>(expectedCaret))
	        << "Clicking at caret boundary " << expectedCaret
	        << " (mouseX=" << mouseX << ", caretX=" << caretX
	        << ") should map to that caret index when using resolved style.";
	}
}
