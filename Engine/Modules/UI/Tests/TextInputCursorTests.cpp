// The I-beam over editable text.
//
// UIManager resolves the cursor by walking up from the hovered element to the
// first ancestor with a non-Auto `cursor`, then invoking the cursor callback
// the platform layer maps onto an OS cursor. Pointer events over a typed field
// land on its *inner* editor, not on the field itself, so these tests pin that
// a rule on the editor is what actually reaches the callback — and that a rule
// on the outer field reaches it too, via the ancestor walk.
//
// What no unit test can cover is whether the shipped stylesheet declares the
// rule at all; that is an asset question, verified in the running editor.

#include "Rendering/Core/Device.h"
#include "UI/Controls/TextField.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

struct CursorFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    TextField* Field = nullptr;
    TextInput* Input = nullptr;
    std::optional<CursorStyle> LastCursor;

    bool Init(const std::string& cursorRules)
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto field = std::make_unique<TextField>();
        Field = field.get();
        field->SetId("field");
        root->AddChild(std::move(field));

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));
        Ui->SetCursorCallback([this](CursorStyle c) { LastCursor = c; });

        const auto css = std::filesystem::temp_directory_path() /
                         ("ui_text_cursor_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".css");
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 400px; height: 120px; }\n"
                 "#field { width: 200px; height: 26px; font-size: 14px; }\n"
                 ".field-editor { width: 100%; height: 100%; padding: 0 4px; }\n"
              << cursorRules << "\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Field->SetValue("editable");
        Rg = std::make_unique<UiRgHarness>(Dev);
        for (int i = 0; i < 4; ++i)
            Pump();

        Input = FindFirstTextInput(Field);
        return Input != nullptr;
    }

    void Pump()
    {
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    void HoverCentreOfField()
    {
        Ui->OnMouseMove(Field->GetLayoutX() + Field->GetLayoutWidth() * 0.5f,
                        Field->GetLayoutY() + Field->GetLayoutHeight() * 0.5f);
        Pump();
        Pump();
    }

    void HoverAway()
    {
        Ui->OnMouseMove(-500.0f, -500.0f);
        Pump();
        Pump();
    }

    ~CursorFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

#define REQUIRE_CURSOR_FIXTURE(fx, inited)                                            \
    do                                                                                \
    {                                                                                 \
        if (!(fx).Dev)                                                                \
            GTEST_SKIP() << "Device init failed";                                     \
        ASSERT_TRUE(inited);                                                          \
    } while (false)

} // namespace

// The pointer lands on the inner editor, so a rule targeting the editor is
// what the hover walk finds first.
TEST(TextInputCursorTests, CursorTextOnTheEditorReachesTheCursorCallback)
{
    CursorFixture fx;
    const bool inited = fx.Init("textinput { cursor: text; }");
    REQUIRE_CURSOR_FIXTURE(fx, inited);

    fx.HoverAway();
    fx.LastCursor.reset();

    fx.HoverCentreOfField();
    ASSERT_TRUE(fx.LastCursor.has_value())
        << "hovering editable text must drive the cursor callback";
    EXPECT_EQ(*fx.LastCursor, CursorStyle::Text);
}

// A rule on the outer field must reach the callback too: the walk goes up from
// the hovered inner editor through its ancestors.
TEST(TextInputCursorTests, CursorTextOnTheOuterFieldReachesTheCursorCallback)
{
    CursorFixture fx;
    const bool inited = fx.Init("#field { cursor: text; }");
    REQUIRE_CURSOR_FIXTURE(fx, inited);

    fx.HoverAway();
    fx.LastCursor.reset();

    fx.HoverCentreOfField();
    ASSERT_TRUE(fx.LastCursor.has_value());
    EXPECT_EQ(*fx.LastCursor, CursorStyle::Text);
}

// Without a rule the field reports the default arrow — this is the state the
// editor was in, and it is why no I-beam ever appeared over text fields.
TEST(TextInputCursorTests, WithoutARuleTheFieldReportsTheDefaultCursor)
{
    CursorFixture fx;
    const bool inited = fx.Init("");
    REQUIRE_CURSOR_FIXTURE(fx, inited);

    fx.HoverAway();
    fx.LastCursor.reset();

    fx.HoverCentreOfField();
    if (fx.LastCursor.has_value())
        EXPECT_EQ(*fx.LastCursor, CursorStyle::Auto);
}

// Moving off the text restores the default cursor rather than leaving the
// I-beam stuck on.
TEST(TextInputCursorTests, LeavingTheFieldRestoresTheDefaultCursor)
{
    CursorFixture fx;
    const bool inited = fx.Init("textinput { cursor: text; }");
    REQUIRE_CURSOR_FIXTURE(fx, inited);

    fx.HoverCentreOfField();
    ASSERT_TRUE(fx.LastCursor.has_value());
    ASSERT_EQ(*fx.LastCursor, CursorStyle::Text);

    fx.HoverAway();
    ASSERT_TRUE(fx.LastCursor.has_value());
    EXPECT_EQ(*fx.LastCursor, CursorStyle::Auto);
}
