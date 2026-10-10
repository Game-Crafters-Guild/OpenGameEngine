// Selection/caret state invariants for the single-line TextInput editor.
//
// These pin the *logical* contract of caret index and selection range —
// independent of fonts, DPI and rendering. TextInput models a selection as an
// anchor/head pair (m_SelectionStart is the anchor, m_SelectionEnd the head,
// both -1 when nothing is selected), so every assertion below states the exact
// pair a sequence of edits must produce.
//
// The invariant that ties them together, and that several of these tests exist
// to enforce: whenever the caret moves for a reason other than a shift-extend,
// the selection must be dropped. A selection left behind by a previous
// interaction is "stale" — its anchor no longer relates to where the caret is
// — and the next shift+arrow / shift+Home / shift+End would then extend from
// that stale anchor instead of from the caret.

#include "UI/Controls/TextField.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace GameEngine;

namespace
{

void PressKey(TextInput& input, int key, int mods = 0)
{
    input.OnKey(key, mods, nullptr);
}

void TypeAscii(TextInput& input, const std::string& text)
{
    for (char c : text)
        input.OnChar(static_cast<unsigned int>(static_cast<unsigned char>(c)));
}

// The selection the user actually sees: normalized [Begin, End), empty when
// the anchor/head pair is collapsed or unset. BuildTextInputOverlays applies
// exactly this rule — it gates on start != end, then takes min/max.
struct VisibleSelection
{
    int Begin = 0;
    int End = 0;
    bool Empty = true;
};

VisibleSelection VisibleSelectionOf(const TextInput& input)
{
    const int a = input.GetSelectionStart();
    const int b = input.GetSelectionEnd();
    if (a < 0 || b < 0 || a == b)
        return {};
    return {std::min(a, b), std::max(a, b), false};
}

::testing::AssertionResult HasSelection(const TextInput& input, int expectedBegin, int expectedEnd)
{
    const VisibleSelection sel = VisibleSelectionOf(input);
    if (sel.Empty)
    {
        return ::testing::AssertionFailure()
               << "expected selection [" << expectedBegin << ", " << expectedEnd
               << ") but nothing is selected (anchor=" << input.GetSelectionStart()
               << ", head=" << input.GetSelectionEnd() << ")";
    }
    if (sel.Begin != expectedBegin || sel.End != expectedEnd)
    {
        return ::testing::AssertionFailure()
               << "expected selection [" << expectedBegin << ", " << expectedEnd
               << ") but found [" << sel.Begin << ", " << sel.End << ")";
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult HasNoSelection(const TextInput& input)
{
    const VisibleSelection sel = VisibleSelectionOf(input);
    if (!sel.Empty)
    {
        return ::testing::AssertionFailure()
               << "expected no selection but found [" << sel.Begin << ", " << sel.End << ")";
    }
    return ::testing::AssertionSuccess();
}

} // namespace

// ---------------------------------------------------------------------------
// Select-all covers the FULL string
// ---------------------------------------------------------------------------

TEST(TextInputSelectionTests, CtrlASelectsEveryByteIncludingTheLast)
{
    const std::string text = "hello world";
    TextInput input;
    input.SetValue(text);

    PressKey(input, Input::kKeyCode_A, Input::kModControl);

    EXPECT_TRUE(HasSelection(input, 0, static_cast<int>(text.size())));
    EXPECT_EQ(input.GetCaretIndex(), static_cast<int>(text.size()))
        << "Ctrl+A leaves the caret at the head of the selection (the end)";
}

TEST(TextInputSelectionTests, SelectAllCoversEveryByteOfMultiByteText)
{
    // "a" + U+00E9 (2 bytes) + U+4E2D (3 bytes). Select-all is expressed in
    // bytes and must reach the final byte, not the final codepoint.
    const std::string text = "a\xC3\xA9\xE4\xB8\xAD";
    ASSERT_EQ(text.size(), 6u);

    TextInput input;
    input.SetValue(text);
    input.SelectAll();

    EXPECT_TRUE(HasSelection(input, 0, 6));
    EXPECT_EQ(input.GetCaretIndex(), 6);
}

TEST(TextInputSelectionTests, SelectAllOnEmptyValueLeavesNoSelection)
{
    TextInput input;
    input.SetValue("");
    input.SelectAll();

    EXPECT_TRUE(HasNoSelection(input));
    EXPECT_EQ(input.GetCaretIndex(), 0);
}

// ---------------------------------------------------------------------------
// Shift+arrow / shift+Home / shift+End anchor at the CURRENT caret
// ---------------------------------------------------------------------------

TEST(TextInputSelectionTests, ShiftRightFromCaretSelectsExactlyOneGlyph)
{
    TextInput input;
    input.SetValue("abcdef");
    PressKey(input, Input::kKeyCode_Home);
    ASSERT_EQ(input.GetCaretIndex(), 0);

    PressKey(input, Input::kKeyCode_Right, Input::kModShift);

    EXPECT_TRUE(HasSelection(input, 0, 1));
    EXPECT_EQ(input.GetCaretIndex(), 1);
}

TEST(TextInputSelectionTests, ShiftLeftThenShiftRightReturnsToEmptySelection)
{
    TextInput input;
    input.SetValue("abcdef");
    PressKey(input, Input::kKeyCode_End);
    ASSERT_EQ(input.GetCaretIndex(), 6);

    PressKey(input, Input::kKeyCode_Left, Input::kModShift);
    EXPECT_TRUE(HasSelection(input, 5, 6));

    // Shrinking back onto the anchor collapses the selection; the caret must
    // land back where it started.
    PressKey(input, Input::kKeyCode_Right, Input::kModShift);
    EXPECT_EQ(input.GetCaretIndex(), 6);
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, ShiftEndAfterTypingAnchorsAtTheCaretNotAStalePosition)
{
    // An earlier interaction can leave a *collapsed but valid* selection pair
    // behind — Ctrl+A on an empty value is the simplest way to produce one, a
    // plain click does the same. Typing then advances the caret. Shift+End
    // must extend from where the caret now is, so with the caret already at
    // the end nothing gets selected.
    TextInput input;
    input.SetValue("");
    PressKey(input, Input::kKeyCode_A, Input::kModControl); // anchor == head == 0
    ASSERT_EQ(input.GetSelectionStart(), 0);
    ASSERT_EQ(input.GetSelectionEnd(), 0);

    TypeAscii(input, "abc");
    ASSERT_EQ(input.GetValue(), "abc");
    ASSERT_EQ(input.GetCaretIndex(), 3);

    PressKey(input, Input::kKeyCode_End, Input::kModShift);
    EXPECT_EQ(input.GetCaretIndex(), 3);
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, ShiftHomeAfterTypingSelectsFromCaretBackToStart)
{
    TextInput input;
    input.SetValue("");
    PressKey(input, Input::kKeyCode_A, Input::kModControl);
    TypeAscii(input, "abcdef");
    ASSERT_EQ(input.GetCaretIndex(), 6);

    PressKey(input, Input::kKeyCode_Home, Input::kModShift);

    EXPECT_TRUE(HasSelection(input, 0, 6));
    EXPECT_EQ(input.GetCaretIndex(), 0);
}

TEST(TextInputSelectionTests, ShiftLeftAfterTypingSelectsTheJustTypedGlyph)
{
    TextInput input;
    input.SetValue("");
    PressKey(input, Input::kKeyCode_A, Input::kModControl);
    TypeAscii(input, "xyz");
    ASSERT_EQ(input.GetCaretIndex(), 3);

    PressKey(input, Input::kKeyCode_Left, Input::kModShift);

    EXPECT_TRUE(HasSelection(input, 2, 3));
    EXPECT_EQ(input.GetCaretIndex(), 2);
}

TEST(TextInputSelectionTests, ShiftArrowAfterBackspaceAnchorsAtTheCaret)
{
    TextInput input;
    input.SetValue("abcdef");
    input.SelectAll();
    PressKey(input, Input::kKeyCode_Backspace); // clears everything
    ASSERT_EQ(input.GetValue(), "");

    TypeAscii(input, "hi");
    ASSERT_EQ(input.GetCaretIndex(), 2);
    PressKey(input, Input::kKeyCode_Backspace); // "h", caret 1
    ASSERT_EQ(input.GetValue(), "h");
    ASSERT_EQ(input.GetCaretIndex(), 1);

    PressKey(input, Input::kKeyCode_Left, Input::kModShift);
    EXPECT_TRUE(HasSelection(input, 0, 1));
    EXPECT_EQ(input.GetCaretIndex(), 0);
}

TEST(TextInputSelectionTests, ShiftExtendKeepsTheAnchorAcrossMultipleSteps)
{
    TextInput input;
    input.SetValue("abcdefgh");
    PressKey(input, Input::kKeyCode_Home);

    for (int i = 0; i < 3; ++i)
        PressKey(input, Input::kKeyCode_Right, Input::kModShift);

    EXPECT_TRUE(HasSelection(input, 0, 3));
    EXPECT_EQ(input.GetCaretIndex(), 3);

    // Shrinking all the way back onto the anchor empties the selection.
    for (int i = 0; i < 3; ++i)
        PressKey(input, Input::kKeyCode_Left, Input::kModShift);
    EXPECT_EQ(input.GetCaretIndex(), 0);
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, ShiftExtendAfterCtrlAShrinksFromTheEnd)
{
    TextInput input;
    input.SetValue("abcdef");
    PressKey(input, Input::kKeyCode_A, Input::kModControl);
    ASSERT_EQ(input.GetCaretIndex(), 6);

    PressKey(input, Input::kKeyCode_Left, Input::kModShift);

    // Ctrl+A anchors at 0, so shift+left shrinks the head.
    EXPECT_TRUE(HasSelection(input, 0, 5));
    EXPECT_EQ(input.GetCaretIndex(), 5);
}

// ---------------------------------------------------------------------------
// Unshifted movement collapses the selection
// ---------------------------------------------------------------------------

TEST(TextInputSelectionTests, UnshiftedMovementDropsTheSelection)
{
    const int kMovementKeys[] = {Input::kKeyCode_Left, Input::kKeyCode_Right,
                                 Input::kKeyCode_Home, Input::kKeyCode_End};
    for (int key : kMovementKeys)
    {
        SCOPED_TRACE(::testing::Message() << "key=" << key);
        TextInput input;
        input.SetValue("abcdef");
        input.SelectAll();
        ASSERT_FALSE(VisibleSelectionOf(input).Empty);

        PressKey(input, key);
        EXPECT_TRUE(HasNoSelection(input));
    }
}

// ---------------------------------------------------------------------------
// Caret position after mutation
// ---------------------------------------------------------------------------

TEST(TextInputSelectionTests, TypingOverASelectionReplacesItAndCollapsesTheCaret)
{
    TextInput input;
    input.SetValue("abcdef");
    input.SelectAll();

    TypeAscii(input, "Z");

    EXPECT_EQ(input.GetValue(), "Z");
    EXPECT_EQ(input.GetCaretIndex(), 1) << "caret sits after the inserted text";
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, TypingLeavesNoSelectionBehind)
{
    // After an insert nothing is selected, so the anchor/head pair must not
    // keep reporting a range that the next shift-extend would anchor on.
    TextInput input;
    input.SetValue("ab");
    input.SelectAll();
    TypeAscii(input, "Q");
    ASSERT_EQ(input.GetValue(), "Q");

    TypeAscii(input, "R");
    EXPECT_EQ(input.GetValue(), "QR");
    EXPECT_EQ(input.GetCaretIndex(), 2);
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, BackspaceStepsBackOneCodepointNotOneByte)
{
    TextInput input;
    input.SetValue("a\xC3\xA9"); // 'a' + U+00E9
    ASSERT_EQ(input.GetValue().size(), 3u);
    PressKey(input, Input::kKeyCode_End);
    ASSERT_EQ(input.GetCaretIndex(), 3);

    PressKey(input, Input::kKeyCode_Backspace);

    EXPECT_EQ(input.GetValue(), "a");
    EXPECT_EQ(input.GetCaretIndex(), 1);
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, DeleteRemovesForwardCodepointAndKeepsTheCaret)
{
    TextInput input;
    input.SetValue("a\xC3\xA9z");
    PressKey(input, Input::kKeyCode_Home);
    PressKey(input, Input::kKeyCode_Right); // past 'a'
    ASSERT_EQ(input.GetCaretIndex(), 1);

    PressKey(input, Input::kKeyCode_Delete);

    EXPECT_EQ(input.GetValue(), "az");
    EXPECT_EQ(input.GetCaretIndex(), 1) << "forward delete never moves the caret";
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, DeletingASelectionPlacesTheCaretAtTheRangeStart)
{
    TextInput input;
    input.SetValue("abcdef");
    PressKey(input, Input::kKeyCode_Home);
    for (int i = 0; i < 4; ++i)
        PressKey(input, Input::kKeyCode_Right, Input::kModShift);
    ASSERT_EQ(input.GetCaretIndex(), 4);

    PressKey(input, Input::kKeyCode_Delete);

    EXPECT_EQ(input.GetValue(), "ef");
    EXPECT_EQ(input.GetCaretIndex(), 0);
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, BackspaceOnASelectionBehavesLikeDelete)
{
    TextInput input;
    input.SetValue("abcdef");
    PressKey(input, Input::kKeyCode_End);
    for (int i = 0; i < 2; ++i)
        PressKey(input, Input::kKeyCode_Left, Input::kModShift);
    ASSERT_EQ(input.GetCaretIndex(), 4);

    PressKey(input, Input::kKeyCode_Backspace);

    EXPECT_EQ(input.GetValue(), "abcd");
    EXPECT_EQ(input.GetCaretIndex(), 4);
    EXPECT_TRUE(HasNoSelection(input));
}

TEST(TextInputSelectionTests, BackspaceAtStartAndDeleteAtEndAreNoOps)
{
    TextInput input;
    input.SetValue("ab");
    PressKey(input, Input::kKeyCode_Home);
    PressKey(input, Input::kKeyCode_Backspace);
    EXPECT_EQ(input.GetValue(), "ab");
    EXPECT_EQ(input.GetCaretIndex(), 0);

    PressKey(input, Input::kKeyCode_End);
    PressKey(input, Input::kKeyCode_Delete);
    EXPECT_EQ(input.GetValue(), "ab");
    EXPECT_EQ(input.GetCaretIndex(), 2);
}

TEST(TextInputSelectionTests, EditingSurvivesTheValueShrinkingUnderTheCaret)
{
    TextInput input;
    input.SetValue("abcdef");
    PressKey(input, Input::kKeyCode_End);
    ASSERT_EQ(input.GetCaretIndex(), 6);

    // A programmatic value swap (formatting, data binding, undo) can shorten
    // the text without going through the editing path.
    input.SetValue("ab");

    // The next edit must not read or write out of range.
    TypeAscii(input, "X");
    EXPECT_EQ(input.GetValue(), "abX");
    EXPECT_EQ(input.GetCaretIndex(), 3);
}

// ---------------------------------------------------------------------------
// Word movement
// ---------------------------------------------------------------------------

TEST(TextInputSelectionTests, CtrlShiftRightSelectsToTheNextWordBoundary)
{
    TextInput input;
    input.SetValue("hello world");
    PressKey(input, Input::kKeyCode_Home);

    PressKey(input, Input::kKeyCode_Right, Input::kModControl | Input::kModShift);

    const VisibleSelection sel = VisibleSelectionOf(input);
    ASSERT_FALSE(sel.Empty);
    EXPECT_EQ(sel.Begin, 0);
    EXPECT_EQ(sel.End, input.GetCaretIndex());
    EXPECT_GT(input.GetCaretIndex(), 0);
    EXPECT_LE(input.GetCaretIndex(), 11);
}
