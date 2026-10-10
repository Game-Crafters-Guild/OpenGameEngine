#include <gtest/gtest.h>

#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/UIEvents.h"
#include "Input/KeyCodes.h"

using namespace GameEngine;

namespace {

void Send(UIElement& el, EventId id, float x, int button = 0)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

TextInput* FindEditor(FloatField& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (auto* editor = dynamic_cast<TextInput*>(child.get()))
            return editor;
    }
    return nullptr;
}

template <typename T>
T* FindChildByClass(UIElement& root, const char* className)
{
    if (root.HasClass(className))
        return dynamic_cast<T*>(&root);
    for (const auto& child : root.GetChildren())
    {
        if (!child)
            continue;
        if (T* found = FindChildByClass<T>(*child, className))
            return found;
    }
    return nullptr;
}

void Scrub(UIElement& el, float startX, float endX)
{
    Send(el, kEventMouseDown, startX);
    Send(el, kEventMouseMove, endX);
    Send(el, kEventMouseUp, endX);
}

} // namespace

TEST(FloatFieldDragToChangeTests, DisabledByDefault)
{
    FloatField field;
    EXPECT_FALSE(field.IsDragToChangeEnabled());
    EXPECT_FALSE(field.HasClass("float-field-drag-to-change"));

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    field.SetValue(1.0f);
    Scrub(*editor, 100.0f, 140.0f);
    EXPECT_FLOAT_EQ(field.GetValue(), 1.0f);
}

TEST(FloatFieldDragToChangeTests, DragPastThresholdScrubsValue)
{
    FloatField field;
    field.EnableDragToChange();
    field.EnableDragToChange();
    EXPECT_TRUE(field.IsDragToChangeEnabled());
    EXPECT_TRUE(field.HasClass("float-field-drag-to-change"));

    int changing = 0;
    int changed = 0;
    field.SetOnValueChanging([&](const float&) { ++changing; });
    field.SetOnValueChanged([&](const float&) { ++changed; });

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    field.SetValue(1.0f);
    Scrub(*editor, 100.0f, 120.0f);

    EXPECT_FLOAT_EQ(field.GetValue(), 1.6f);
    EXPECT_GE(changing, 1);
    EXPECT_EQ(changed, 1);
}

TEST(FloatFieldDragToChangeTests, ClickWithoutDragDoesNotChangeValue)
{
    FloatField field;
    field.EnableDragToChange();
    field.SetValue(2.5f);

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    Send(*editor, kEventMouseDown, 50.0f);
    Send(*editor, kEventMouseMove, 52.0f);
    Send(*editor, kEventMouseUp, 52.0f);
    EXPECT_FLOAT_EQ(field.GetValue(), 2.5f);
}

TEST(FloatFieldDragToChangeTests, RightMouseDragDoesNotScrubOrCapture)
{
    FloatField field;
    field.EnableDragToChange();
    field.SetValue(1.0f);

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);

    UIEvent down{};
    down.Id = kEventMouseDown;
    down.X = 100.0f;
    down.Button = Input::kMouseButton_Right;
    down.Target = editor;
    down.CurrentTarget = editor;
    editor->DispatchEvent(down);
    EXPECT_FALSE(down.Handled);
    EXPECT_EQ(down.CaptureRequested, nullptr);

    Send(*editor, kEventMouseMove, 140.0f, Input::kMouseButton_Right);
    Send(*editor, kEventMouseUp, 140.0f, Input::kMouseButton_Right);
    EXPECT_FLOAT_EQ(field.GetValue(), 1.0f);
}

TEST(Vector3FieldLabelDragTests, InspectorLabelsScrubByDefault)
{
    Vector3Field field;
    EXPECT_TRUE(field.IsLabelDragEnabled());

    auto* xLabel = FindChildByClass<Label>(field, "vector3-label-x");
    auto* xField = FindChildByClass<FloatField>(field, "vector3-component-x");
    ASSERT_NE(xLabel, nullptr);
    ASSERT_NE(xField, nullptr);
    EXPECT_TRUE(xLabel->HasClass("draggable-label"));
    EXPECT_FALSE(xField->IsDragToChangeEnabled());

    field.SetValue({1.0f, 2.0f, 3.0f});
    Scrub(*xLabel, 10.0f, 30.0f);
    EXPECT_FLOAT_EQ(field.GetValue().x, 3.0f);
    EXPECT_FLOAT_EQ(field.GetValue().y, 2.0f);
}

TEST(Vector3FieldLabelDragTests, GraphModeScrubsBoxesNotLabels)
{
    Vector3Field field;
    field.SetLabelDragEnabled(false);
    field.EnableComponentDragToChange();

    EXPECT_FALSE(field.IsLabelDragEnabled());

    auto* xLabel = FindChildByClass<Label>(field, "vector3-label-x");
    auto* xField = FindChildByClass<FloatField>(field, "vector3-component-x");
    ASSERT_NE(xLabel, nullptr);
    ASSERT_NE(xField, nullptr);
    EXPECT_FALSE(xLabel->HasClass("draggable-label"));
    EXPECT_TRUE(xField->IsDragToChangeEnabled());

    field.SetValue({1.0f, 2.0f, 3.0f});
    Scrub(*xLabel, 10.0f, 40.0f);
    EXPECT_FLOAT_EQ(field.GetValue().x, 1.0f);

    TextInput* editor = FindEditor(*xField);
    ASSERT_NE(editor, nullptr);
    Scrub(*editor, 100.0f, 120.0f);
    EXPECT_FLOAT_EQ(field.GetValue().x, 1.6f);
}

TEST(Vector3FieldLabelDragTests, DoubleClickResetsWhenLabelDragDisabled)
{
    Vector3Field field;
    field.SetLabelDragEnabled(false);
    field.SetDefaultValue({0.0f, 0.0f, 0.0f});
    field.SetValue({4.0f, 5.0f, 6.0f});

    auto* xLabel = FindChildByClass<Label>(field, "vector3-label-x");
    ASSERT_NE(xLabel, nullptr);
    Send(*xLabel, kEventMouseDown, 8.0f);
    Send(*xLabel, kEventMouseUp, 8.0f);
    Send(*xLabel, kEventMouseDown, 8.0f);
    EXPECT_FLOAT_EQ(field.GetValue().x, 0.0f);
    EXPECT_FLOAT_EQ(field.GetValue().y, 5.0f);
}

TEST(FloatFieldDragToChangeTests, DragClampsToValueRange)
{
    FloatField field;
    field.EnableDragToChange();
    field.SetValueRange(0.0f, 1.0f);
    field.SetValue(0.9f);

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    Scrub(*editor, 100.0f, 200.0f);
    EXPECT_FLOAT_EQ(field.GetValue(), 1.0f);

    field.ClearValueRange();
    field.SetValue(0.9f);
    Scrub(*editor, 100.0f, 200.0f);
    EXPECT_GT(field.GetValue(), 1.0f);
}

TEST(FloatFieldDragToChangeTests, TypedInputClampsToValueRange)
{
    FloatField field;
    field.SetValueRange(0.0f, 1.0f);
    field.SetValue(0.5f);

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    editor->SetValue("");
    editor->OnChar('5');
    EXPECT_FLOAT_EQ(field.GetValue(), 1.0f);
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_FLOAT_EQ(field.GetValue(), 1.0f);

    editor->SetValue("");
    editor->OnChar('-');
    editor->OnChar('3');
    EXPECT_FLOAT_EQ(field.GetValue(), 0.0f);
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_FLOAT_EQ(field.GetValue(), 0.0f);
}

// A host that installs its own parse function keeps the field's value range on typed input.
TEST(FloatFieldDragToChangeTests, TypedInputClampsToValueRangeUnderAHostParse)
{
    FloatField field;
    field.SetValueRange(0.0f, 1.0f);
    field.SetValue(0.5f);
    field.SetParseFunction([](const std::string& text, float& out, bool) { return FloatField::TryParseFloat(text, out); });

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    editor->SetValue("");
    editor->OnChar('5');
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_FLOAT_EQ(field.GetValue(), 1.0f);
}

// A focused field shows the exact stored value as a person types it: plain digits, with an exponent
// only for magnitudes no one types digit by digit.
TEST(FloatFieldFocusedTextTests, ShowsThePlainDigitsOfTheStoredValue)
{
    FloatField field;
    field.OnFocusChanged(true);
    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);

    field.SetValue(1e9f);
    EXPECT_EQ(editor->GetValue(), "1000000000");
    field.SetValue(0.0005f);
    EXPECT_EQ(editor->GetValue(), "0.0005");
    field.SetValue(-2.5f);
    EXPECT_EQ(editor->GetValue(), "-2.5");
    field.SetValue(3e20f);
    EXPECT_EQ(editor->GetValue(), "3e+20");
}
