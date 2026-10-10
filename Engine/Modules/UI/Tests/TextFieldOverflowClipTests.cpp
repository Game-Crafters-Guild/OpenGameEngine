#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "Rendering/Core/Device.h"
#include "UIRgTestHarness.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

// An element's own glyphs are emitted before its overflow clip is pushed for
// children, so containment of overflowing text relies on the self-clip path
// in EmitTextPrimitives. These tests pin that behavior for TextInput-backed
// fields: overflowing values clip to the editor's box (lazily allocating the
// element's clip slot), fitting values cost no slot, drains preserve the
// self-clip, and focused overflowing fields scroll to keep the caret visible.

namespace
{

class InspectableFloatField : public FloatField
{
public:
    const std::string& EditorText() const { return GetEditorText(); }
};

struct FieldFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    FloatField* Field = nullptr;
    FloatField* OtherField = nullptr;
    TextInput* Input = nullptr;

    bool Init(float value)
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto field = std::make_unique<FloatField>();
        Field = field.get();
        field->SetId("field");
        root->AddChild(std::move(field));
        auto other = std::make_unique<FloatField>();
        OtherField = other.get();
        other->SetId("other");
        root->AddChild(std::move(other));

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_textfield_overflow_clip.css";
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: row; width: 300px; height: 60px; }\n"
                 "#field, #other { width: 40px; height: 24px; font-size: 14px; }\n"
                 ".field-editor { width: 100%; height: 100%; padding: 0 4px; "
                 "border-width: 2px; border-color: #808080; }\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Field->SetValue(value);
        Rg = std::make_unique<UiRgHarness>(Dev);

        // Settle: first frames run layout solves + the full primitive regen.
        for (int i = 0; i < 3; ++i)
            Pump();

        Input = FindFirstTextInput(Field);
        return Input != nullptr;
    }

    void Pump()
    {
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    void Click(float x, float y)
    {
        Ui->OnMouseMove(x, y);
        Ui->OnMouseButton(0, true);
        Pump();
        Ui->OnMouseButton(0, false);
        Pump();
    }

    // All primitives currently baked for the inner TextInput.
    std::vector<UI::UIPrimitive> InputPrims() const
    {
        std::vector<UI::UIPrimitive> prims;
        for (uint16_t i = 0;; ++i)
        {
            const UI::UIPrimitive* p = Ui->PeekPrimitiveForTesting(*Input, i);
            if (!p)
                break;
            prims.push_back(*p);
        }
        return prims;
    }

    ~FieldFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

// "7.665336" is the Scene View camera popup value that originally spilled out
// of its 56px FloatField (std::log2(203.0f) at FormatFloat's 6-decimal default).
constexpr float kOverflowingValue = 7.665336f;

} // namespace

TEST(FloatFieldFormattingTests, FixedDecimalPlacesPreserveTrailingZeros)
{
    InspectableFloatField field;
    field.SetFixedDecimalPlaces(2);

    field.SetValue(0.0f);
    EXPECT_EQ(field.EditorText(), "0.00");
    field.SetValue(1.0f);
    EXPECT_EQ(field.EditorText(), "1.00");
    field.SetValue(-0.126f);
    EXPECT_EQ(field.EditorText(), "-0.13");
}

TEST(TextFieldOverflowClipTests, OverflowingValueClipsGlyphsToInputBox)
{
    FieldFixture fx;
    const bool inited = fx.Init(kOverflowingValue);
    if (!fx.Dev)
        GTEST_SKIP() << "Device init failed";
    ASSERT_TRUE(inited);
    if (!fx.Ui->GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";

    const auto prims = fx.InputPrims();
    size_t glyphCount = 0;
    for (const auto& p : prims)
    {
        if (UI::GetMode(p.ModeAndFlags) != UI::PrimitiveMode::Slug)
            continue;
        ++glyphCount;
        EXPECT_NE(fx.Input->m_ClipSlotIdx, UI::kNoClip)
            << "overflowing text must allocate the element's clip slot";
        EXPECT_EQ(UI::GetClipIndex(p.ModeAndFlags), fx.Input->m_ClipSlotIdx)
            << "every glyph must wear the self clip";
    }
    ASSERT_GT(glyphCount, 0u);

    // The mask is the TextInput's padding box (physical px == logical at
    // content scale 1): the editor's 2px border is excluded, so scrolled-out
    // glyph fragments can never paint over it.
    const UI::UIClipRect* cr = fx.Ui->PeekClipRectForTesting(fx.Input->m_ClipSlotIdx);
    ASSERT_NE(cr, nullptr);
    constexpr float kBorderPx = 2.0f;
    EXPECT_NEAR(cr->Rect[0], fx.Input->GetLayoutX() + kBorderPx, 0.6f);
    EXPECT_NEAR(cr->Rect[2], fx.Input->GetLayoutWidth() - 2.0f * kBorderPx, 0.6f);
    EXPECT_LE(cr->Rect[0] + cr->Rect[2],
              fx.Field->GetLayoutX() + fx.Field->GetLayoutWidth() + 0.6f)
        << "clip must not extend past the field";
}

TEST(TextFieldOverflowClipTests, FittingValueAllocatesNoClipSlot)
{
    FieldFixture fx;
    const bool inited = fx.Init(5.0f);
    if (!fx.Dev)
        GTEST_SKIP() << "Device init failed";
    ASSERT_TRUE(inited);
    if (!fx.Ui->GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";

    EXPECT_EQ(fx.Input->m_ClipSlotIdx, UI::kNoClip)
        << "fitting text must not spend a clip slot";

    size_t glyphCount = 0;
    for (const auto& p : fx.InputPrims())
    {
        if (UI::GetMode(p.ModeAndFlags) != UI::PrimitiveMode::Slug)
            continue;
        ++glyphCount;
        EXPECT_EQ(UI::GetClipIndex(p.ModeAndFlags), UI::kNoClip)
            << "fitting glyphs keep the ambient clip";
    }
    ASSERT_GT(glyphCount, 0u);
}

TEST(TextFieldOverflowClipTests, DrainPreservesSelfClipOnGlyphs)
{
    FieldFixture fx;
    const bool inited = fx.Init(kOverflowingValue);
    if (!fx.Dev)
        GTEST_SKIP() << "Device init failed";
    ASSERT_TRUE(inited);
    if (!fx.Ui->GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";
    ASSERT_NE(fx.Input->m_ClipSlotIdx, UI::kNoClip);

    // Visual-only drains re-emit the element without a clip stack; the
    // preserved-ambient patch must not clobber the glyphs' self clip, and
    // ambient primitives (caret) must stay on the ambient index (kNoClip in
    // this tree — no overflow:hidden ancestors).
    for (int drain = 1; drain <= 2; ++drain)
    {
        fx.Input->MarkDirty(UIElement::VisualDirty);
        fx.Pump();

        size_t glyphCount = 0;
        for (const auto& p : fx.InputPrims())
        {
            const bool isGlyph = UI::GetMode(p.ModeAndFlags) == UI::PrimitiveMode::Slug;
            if (isGlyph)
                ++glyphCount;
            const uint16_t expected = isGlyph ? fx.Input->m_ClipSlotIdx : UI::kNoClip;
            EXPECT_EQ(UI::GetClipIndex(p.ModeAndFlags), expected)
                << "after drain " << drain << (isGlyph ? " (glyph)" : " (overlay)");
        }
        ASSERT_GT(glyphCount, 0u) << "after drain " << drain;
    }
}

// Display shows Unity-width 7 significant digits; focus swaps to the full
// round-trip representation for editing and blur restores the display form,
// with the stored value bit-exact throughout.
TEST(TextFieldOverflowClipTests, DisplayPrecisionSwapsToFullWhileEditing)
{
    FieldFixture fx;
    const bool inited = fx.Init(203.123456f);
    if (!fx.Dev)
        GTEST_SKIP() << "Device init failed";
    ASSERT_TRUE(inited);
    if (!fx.Ui->GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";

    const float stored = fx.Field->GetValue();
    EXPECT_EQ(fx.Input->GetValue(), "203.1235") << "display: 7 significant digits";

    // Focus: the full round-trip representation appears for editing. Focus
    // events settle one frame after the click (select-on-up deferral).
    fx.Click(fx.Field->GetLayoutX() + fx.Field->GetLayoutWidth() * 0.5f,
             fx.Field->GetLayoutY() + fx.Field->GetLayoutHeight() * 0.5f);
    fx.Pump();
    EXPECT_EQ(fx.Input->GetValue(), "203.12346") << "editing: shortest exact round-trip";

    // Blur without editing: display form returns, stored value untouched.
    fx.Click(fx.OtherField->GetLayoutX() + fx.OtherField->GetLayoutWidth() * 0.5f,
             fx.OtherField->GetLayoutY() + fx.OtherField->GetLayoutHeight() * 0.5f);
    fx.Pump();
    EXPECT_EQ(fx.Input->GetValue(), "203.1235");
    EXPECT_EQ(fx.Field->GetValue(), stored) << "stored value is bit-exact";
}

// A self-clip written during a drain must intersect with (and stay parented
// to) the ambient ancestor clip. Regresses two drain bugs: the non-forked
// emit path not seeding the ambient, and the ambient being inferred from
// baked primitives instead of the emit-time stamp.
TEST(TextFieldOverflowClipTests, DrainInsideClippedContainerKeepsParentIntersection)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto wrap = std::make_unique<UIElement>();
        UIElement* wrapPtr = wrap.get();
        wrap->SetId("wrap");
        auto field = std::make_unique<FloatField>();
        FloatField* fieldPtr = field.get();
        field->SetId("field");
        wrap->AddChild(std::move(field));
        auto label = std::make_unique<Label>();
        Label* labelPtr = label.get();
        label->SetId("lbl");
        label->SetText("wwwwwwwwwwwwwwwwwwww");
        wrap->AddChild(std::move(label));
        root->AddChild(std::move(wrap));

        auto ui = std::make_unique<UIManager>(dev);
        ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_overflow_clip_nested.css";
        {
            std::ofstream f(css);
            f << "#root { display: flex; width: 300px; height: 80px; }\n"
                 "#wrap { display: flex; flex-direction: row; width: 120px; height: 40px; "
                 "overflow: hidden; }\n"
                 "#field { width: 40px; height: 24px; font-size: 14px; }\n"
                 "#lbl { width: 40px; height: 24px; font-size: 14px; overflow: hidden; "
                 "white-space: nowrap; }\n"
                 ".field-editor { width: 100%; height: 100%; padding: 0 4px; }\n";
        }
        ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));
        fieldPtr->SetValue(kOverflowingValue);

        UiRgHarness rg(dev);
        auto pump = [&]() {
            ui->Update(0.016f, /*interactive=*/true);
            DriveUiRender(*ui, rg);
        };
        for (int i = 0; i < 3; ++i)
            pump();
        if (!ui->GetDefaultFontAtlas())
            GTEST_SKIP() << "Font atlas not available";

        TextInput* input = FindFirstTextInput(fieldPtr);
        ASSERT_NE(input, nullptr);
        ASSERT_NE(wrapPtr->m_ClipSlotIdx, UI::kNoClip) << "container owns the children clip";
        ASSERT_NE(input->m_ClipSlotIdx, UI::kNoClip);
        ASSERT_NE(labelPtr->m_ClipSlotIdx, UI::kNoClip);

        auto checkParent = [&](const UIElement* el, const char* what, int drain) {
            const UI::UIClipRect* cr = ui->PeekClipRectForTesting(el->m_ClipSlotIdx);
            ASSERT_NE(cr, nullptr);
            EXPECT_EQ(cr->ParentIndex, wrapPtr->m_ClipSlotIdx)
                << what << " must stay parented to the container clip (drain " << drain << ")";
            const UI::UIClipRect* wrapCr = ui->PeekClipRectForTesting(wrapPtr->m_ClipSlotIdx);
            ASSERT_NE(wrapCr, nullptr);
            EXPECT_LE(cr->Rect[0] + cr->Rect[2], wrapCr->Rect[0] + wrapCr->Rect[2] + 0.6f)
                << what << " mask must stay intersected with the container (drain " << drain << ")";
        };
        checkParent(input, "input", 0);
        checkParent(labelPtr, "label", 0);

        // TextInput drains re-emit inline (UiThreadOnly); the Label drains
        // through the non-forked emit slice — both must keep the parentage.
        for (int drain = 1; drain <= 2; ++drain)
        {
            input->MarkDirty(UIElement::VisualDirty);
            labelPtr->MarkDirty(UIElement::VisualDirty);
            pump();
            checkParent(input, "input", drain);
            checkParent(labelPtr, "label", drain);
        }
    }
}

TEST(TextFieldOverflowClipTests, FocusedOverflowScrollsToCaretAndResetsOnBlur)
{
    FieldFixture fx;
    const bool inited = fx.Init(kOverflowingValue);
    if (!fx.Dev)
        GTEST_SKIP() << "Device init failed";
    ASSERT_TRUE(inited);
    if (!fx.Ui->GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";

    EXPECT_FLOAT_EQ(fx.Input->GetTextScrollX(), 0.0f) << "unfocused fields never scroll";

    // Focus the field: focus-gain selects all, placing the caret at the end
    // of the overflowing value, so the render pass must scroll it into view.
    fx.Click(fx.Field->GetLayoutX() + fx.Field->GetLayoutWidth() * 0.5f,
             fx.Field->GetLayoutY() + fx.Field->GetLayoutHeight() * 0.5f);
    fx.Input->SelectAll(); // caret to end regardless of click-caret placement
    fx.Input->MarkDirty(UIElement::VisualDirty);
    fx.Pump();
    EXPECT_GT(fx.Input->GetTextScrollX(), 0.0f)
        << "caret at the end of overflowing text must scroll into view";

    // Blur (focus the second field): the value snaps back to its start.
    fx.Click(fx.OtherField->GetLayoutX() + fx.OtherField->GetLayoutWidth() * 0.5f,
             fx.OtherField->GetLayoutY() + fx.OtherField->GetLayoutHeight() * 0.5f);
    fx.Pump();
    EXPECT_FLOAT_EQ(fx.Input->GetTextScrollX(), 0.0f) << "blur resets the scroll";
}
