// The colour picker dialog window's routing, driven without a live window.
//
// The dialog hosts UI and nothing else — no gameplay sink, no editor
// InputSystem — so its chain is one stage long. Two policies sit on top of that.
// The eyedropper: while its screen-wide grab is armed the pointer belongs to the
// grab's own monitors, and Escape is what ends it. A loaded UI replay scenario:
// the dialog hands its manager to no router, so the replay's synthetic stream is
// the only one that manager sees.
//
// These drive the config MakeColorPickerWindowInputConfig builds, through the
// same WindowInputRouter entry points BindBasicHandlers forwards to, over a real
// UIManager and real controls.

#include <gtest/gtest.h>

#include "ColorPicker/ColorPickerWindowInput.h"
#include "Core/WindowInputRouter.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace GameEngine;

namespace
{
struct PickerDialogFixture
{
    std::unique_ptr<Rendering::IDevice> device;
    std::unique_ptr<UIManager> ui;
    WindowInputRouterConfig config;

    // What the dialog answers for its eyedropper, recorded so a row can tell a
    // cancel that ran from one that did not.
    bool eyedropperActive = false;
    int cancels = 0;
    // What the dialog answers while a UI replay scenario is loaded.
    bool uiReplayActive = false;

    bool Init()
    {
        device = MakeHeadlessDevice();
        if (!device)
            return false;
        ui = std::make_unique<UIManager>(device.get());

        ColorPickerWindowInputSources sources;
        sources.getUi = [this]() { return ui.get(); };
        sources.isEyedropperActive = [this]() { return eyedropperActive; };
        sources.cancelEyedropper = [this]()
        {
            eyedropperActive = false;
            ++cancels;
        };
        sources.isUiReplayActive = [this]() { return uiReplayActive; };
        config = MakeColorPickerWindowInputConfig(/*window=*/nullptr, std::move(sources));
        return true;
    }
};

// The dialog's text entry: the hex and numeric fields the picker is edited
// through. Counts what reaches it so a row can assert delivery rather than an
// after-effect of it.
struct FieldProbe
{
    TextField* field = nullptr;
    int keys = 0;
    int text = 0;
};

void AttachFocusedField(PickerDialogFixture& f, FieldProbe& probe)
{
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto fieldOwned = std::make_unique<TextField>();
    probe.field = fieldOwned.get();
    probe.field->SetId("hex");
    root->AddChild(std::move(fieldOwned));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("hex");

    probe.field->RegisterEventHandler(kEventKeyDown, [&probe](UIEvent&) { ++probe.keys; });
    probe.field->RegisterEventHandler(kEventTextInput, [&probe](UIEvent&) { ++probe.text; });
}

// A button laid out at a known rect with the cursor already over it, so a routed
// press lands on a control that acts. Apply and Cancel are exactly this shape.
struct ButtonProbe
{
    Button* button = nullptr;
    int clicks = 0;
};

bool AttachHoveredButton(PickerDialogFixture& f, ButtonProbe& probe, const char* cssName)
{
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto buttonOwned = std::make_unique<Button>();
    probe.button = buttonOwned.get();
    probe.button->SetId("apply");
    root->AddChild(std::move(buttonOwned));
    f.ui->SetRoot(std::move(root));

    const TestUtils::ScopedTempDir cssDir{TestUtils::MakeUniqueTempDirectory("ColorPickerWindowInputTests")};
    const auto css = cssDir.Path() / cssName;
    {
        std::ofstream out(css);
        out << R"(
#root { display: flex; width: 200px; height: 40px; }
#apply { width: 100px; height: 20px; }
)";
    }
    if (!f.ui->AttachStyleFromFile(css.string()))
        return false;

    probe.button->RegisterEventHandler(kEventButtonClick, [&probe](UIEvent&) { ++probe.clicks; });

    f.ui->Update(0.0f, /*interactive=*/false);
    f.ui->OnMouseMove(probe.button->GetLayoutX() + 5.0f, probe.button->GetLayoutY() + 5.0f);
    f.ui->Update(0.0f, /*interactive=*/true);
    return probe.button->GetLayoutWidth() > 0.0f;
}
} // namespace

TEST(ColorPickerWindowInputTests, TypingReachesTheDialogsFocusedField)
{
    PickerDialogFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    FieldProbe probe;
    AttachFocusedField(f, probe);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_A, Input::kKeyActionPress, /*mods=*/0);
    EXPECT_TRUE(WindowInputRouter::RouteChar(f.config, 'a')) << "the focused field turns the character into an edit";

    EXPECT_EQ(probe.keys, 1);
    EXPECT_EQ(probe.text, 1);
}

TEST(ColorPickerWindowInputTests, ModifiedDragsReachTheDialogWithTheirModifiers)
{
    PickerDialogFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    ButtonProbe probe;
    ASSERT_TRUE(AttachHoveredButton(f, probe, "colorpicker_modified_drag.css"));

    // The mask a click carries is the only report of a modifier already held
    // when the dialog took focus, which is what a Shift- or Ctrl-modified drag on
    // the wheel and the sliders reads.
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, Input::kModShift);
    EXPECT_EQ(f.ui->GetModifierKeys(), Input::kModShift);

    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, Input::kModShift);
    EXPECT_EQ(probe.clicks, 1);
}

TEST(ColorPickerWindowInputTests, EscapeCancelsTheArmedEyedropperAndGoesNoFurther)
{
    PickerDialogFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    FieldProbe probe;
    AttachFocusedField(f, probe);
    f.eyedropperActive = true;

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Escape, Input::kKeyActionPress, /*mods=*/0);

    EXPECT_EQ(f.cancels, 1);
    EXPECT_FALSE(f.eyedropperActive);
    EXPECT_EQ(probe.keys, 0) << "the Escape that ended the grab is not also the field's";

    // With no grab to cancel the same Escape is nobody's but the UI's, which is
    // what keeps this row from passing on a chain that delivers nothing.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Escape, Input::kKeyActionPress, /*mods=*/0);
    EXPECT_EQ(f.cancels, 1);
    EXPECT_EQ(probe.keys, 1);
}

TEST(ColorPickerWindowInputTests, AnArmedEyedropperKeepsThePointerFromTheDialog)
{
    PickerDialogFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    ButtonProbe probe;
    ASSERT_TRUE(AttachHoveredButton(f, probe, "colorpicker_eyedropper_pointer.css"));

    f.eyedropperActive = true;
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);

    EXPECT_EQ(probe.clicks, 0) << "the click that commits a sample must not also press the control under it";

    // Disarmed, the same press is an ordinary click again.
    f.eyedropperActive = false;
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);
    EXPECT_EQ(probe.clicks, 1);
}

TEST(ColorPickerWindowInputTests, ALoadedReplayScenarioKeepsRealInputFromTheDialog)
{
    PickerDialogFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    ButtonProbe probe;
    ASSERT_TRUE(AttachHoveredButton(f, probe, "colorpicker_ui_replay.css"));

    f.uiReplayActive = true;
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, Input::kModShift);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, Input::kModShift);

    EXPECT_EQ(probe.clicks, 0) << "a replay owns this manager's stream; real input must reach no stage";
    EXPECT_EQ(f.ui->GetModifierKeys(), 0) << "a manager the router never received cannot have taken the mask";

    // With no scenario loaded the same press is the dialog's again, which is what
    // keeps this row from passing on a chain that delivers nothing.
    f.uiReplayActive = false;
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, Input::kModShift);
    EXPECT_EQ(f.ui->GetModifierKeys(), Input::kModShift);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, Input::kModShift);
    EXPECT_EQ(probe.clicks, 1);
}

TEST(ColorPickerWindowInputTests, LosingFocusClearsTheModifiersTheDialogWasHolding)
{
    PickerDialogFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftShift, Input::kKeyActionPress, Input::kModShift);
    ASSERT_EQ(f.ui->GetModifierKeys(), Input::kModShift);

    // Alt-tabbing away mid-drag synthesizes no release for a modifier derived
    // from an event mask, so without this leg the dialog holds Shift down for
    // every drag after the switch.
    WindowInputRouter::RouteFocusChange(f.config, /*focused=*/false);

    EXPECT_EQ(f.ui->GetModifierKeys(), 0);
}
