// A field's caption suffix describes the stored value ("172 (21 June)"), so it is hidden while the
// field is being edited. When the edit ends without changing the value the caption comes back at
// once; when it changes the value the caption waits for the host's new one. A unit suffix ("%")
// stays visible throughout.

#include "Input/KeyCodes.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;

namespace
{
TextInput* EditorOf(IntField& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (auto* editor = dynamic_cast<TextInput*>(child.get()))
            return editor;
    }
    return nullptr;
}

std::string SuffixText(IntField& field)
{
    for (const auto& child : field.GetChildren())
    {
        auto* label = dynamic_cast<Label*>(child.get());
        if (label && (label->HasClass("field-caption") || label->HasClass("field-suffix")))
            return label->GetText();
    }
    return "<no suffix label>";
}
} // namespace

TEST(FieldCaption, HiddenWhileEditingAndBackOnCommitOrBlur)
{
    IntField field;
    field.SetSuffixRole(SuffixRole::Caption);
    field.SetSuffix("(21 June)");
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(SuffixText(field), "(21 June)");

    field.OnFocusChanged(true);
    EXPECT_EQ(SuffixText(field), "");
    editor->OnChar('5');
    EXPECT_EQ(SuffixText(field), "");
    // The commit changed the value: the old caption stays hidden until the host brings the new one.
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(SuffixText(field), "");
    field.SetSuffix("(5 January)");
    EXPECT_EQ(SuffixText(field), "(5 January)");

    // Typing text the field refuses leaves the value as it was: the caption is back on commit.
    field.SetParseFunction([](const std::string&, int&, bool) { return false; });
    editor->OnChar('x');
    EXPECT_EQ(SuffixText(field), "");
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(SuffixText(field), "(5 January)");

    editor->OnChar('y');
    EXPECT_EQ(SuffixText(field), "");
    field.OnFocusChanged(false);
    EXPECT_EQ(SuffixText(field), "(5 January)");
}

// The previous value's caption never shows beside a newly committed value, even when the field
// loses focus before the host's refresh brings the new caption.
TEST(FieldCaption, AValueChangingCommitWaitsForTheNewCaption)
{
    IntField field;
    field.SetSuffixRole(SuffixRole::Caption);
    field.SetValue(172);
    field.SetSuffix("(21 June)");
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    field.OnFocusChanged(true);
    editor->SetValue("");
    editor->OnChar('1');
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    ASSERT_EQ(field.GetValue(), 1);
    EXPECT_EQ(SuffixText(field), "");
    field.OnFocusChanged(false);
    EXPECT_EQ(SuffixText(field), "");
    field.SetSuffix("(1 January)");
    EXPECT_EQ(SuffixText(field), "(1 January)");
}

TEST(FieldCaption, AUnitSuffixStaysVisibleWhileEditing)
{
    IntField field;
    field.SetSuffix("%");
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    field.OnFocusChanged(true);
    editor->OnChar('5');
    EXPECT_EQ(SuffixText(field), "%");
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(SuffixText(field), "%");
}

// Focus without an edit never commits; losing focus alone brings the caption back.
TEST(FieldCaption, BackOnBlurWithoutAnEdit)
{
    IntField field;
    field.SetSuffixRole(SuffixRole::Caption);
    field.SetSuffix("(21 June)");
    field.OnFocusChanged(true);
    EXPECT_EQ(SuffixText(field), "");
    field.OnFocusChanged(false);
    EXPECT_EQ(SuffixText(field), "(21 June)");
}

// A host that clamps the committed value back to the old one passes no new caption; the old one is
// right again and comes back when the field is set to that value.
TEST(FieldCaption, AClampBackToTheOldValueRestoresItsCaption)
{
    IntField field;
    field.SetSuffixRole(SuffixRole::Caption);
    field.SetValue(365);
    field.SetSuffix("(31 December)");
    field.SetOnValueChanged([&field](const int& value) {
        if (value > 365)
            field.SetValueWithoutNotify(365);
    });
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    field.OnFocusChanged(true);
    editor->SetValue("");
    editor->OnChar('4');
    editor->OnChar('0');
    editor->OnChar('0');
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(field.GetValue(), 365);
    EXPECT_EQ(SuffixText(field), "(31 December)");
}

// A typed value past an IntField's range commits the clamped value, the one a drag would reach, so
// the field shows what its host stores; a caption hidden for the edit comes back because the value
// returns to the one it describes.
TEST(FieldCaption, ATypedValuePastTheRangeCommitsTheClampedValue)
{
    IntField field;
    field.SetRange(1, 365);
    field.SetSuffixRole(SuffixRole::Caption);
    field.SetValue(365);
    field.SetSuffix("(31 December)");
    int committed = 0;
    field.SetOnValueChanged([&committed](const int& value) { committed = value; });
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    field.OnFocusChanged(true);
    editor->SetValue("");
    editor->OnChar('4');
    editor->OnChar('0');
    editor->OnChar('0');
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(field.GetValue(), 365);
    EXPECT_EQ(committed, 365);
    EXPECT_EQ(editor->GetValue(), "365");
    EXPECT_EQ(SuffixText(field), "(31 December)");
}

// Emptying a ranged field commits the field's empty value held to the range, not a value outside it.
TEST(FieldCaption, AnEmptiedRangedFieldCommitsAValueInsideTheRange)
{
    IntField field;
    field.SetRange(1, 365);
    field.SetValue(200);
    int committed = -1;
    field.SetOnValueChanged([&committed](const int& value) { committed = value; });
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    field.OnFocusChanged(true);
    editor->OnKey(Input::kKeyCode_End, 0, nullptr);
    editor->OnKey(Input::kKeyCode_Backspace, 0, nullptr);
    editor->OnKey(Input::kKeyCode_Backspace, 0, nullptr);
    editor->OnKey(Input::kKeyCode_Backspace, 0, nullptr);
    EXPECT_EQ(field.GetValue(), 1);
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(field.GetValue(), 1);
    EXPECT_EQ(committed, 1);
}
