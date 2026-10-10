#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "Assets/AssetManager.h"
#include "Rendering/Core/Device.h"
#include "UIRgTestHarness.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UI/ResolvedStyle.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/DockspaceElement.h"

#include "UI/Controls/TextField.h"
#include "UI/Controls/TextArea.h"
#include "UI/Interaction/FocusIsInside.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Layout/Docking.h"
#include "UI/UIEvents.h"
#include "Input/KeyCodes.h"
#include "Input/InputSystem.h"

#ifndef GLFW_KEY_TAB
#define GLFW_KEY_TAB 258
#endif
#ifndef GLFW_PRESS
#define GLFW_PRESS 1
#endif
#ifndef GLFW_MOD_SHIFT
#define GLFW_MOD_SHIFT 0x0001
#endif
#ifndef GLFW_KEY_ENTER
#define GLFW_KEY_ENTER 257
#endif
#ifndef GLFW_KEY_SPACE
#define GLFW_KEY_SPACE 32
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace
{
// Sets an environment variable for one scope and puts the previous value back,
// so a test that needs an env-driven code path cannot leak it into the next
// test in the same process. A null value means "unset".
class ScopedEnvVar
{
  public:
    ScopedEnvVar(const char* name, const char* value) : m_Name(name)
    {
        if (const char* prev = std::getenv(name))
        {
            m_HadPrevious = true;
            m_Previous = prev;
        }
        Assign(value);
    }

    ~ScopedEnvVar() { Assign(m_HadPrevious ? m_Previous.c_str() : nullptr); }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  private:
    void Assign(const char* value)
    {
#if defined(_WIN32)
        // An empty value is how the CRT removes a variable.
        _putenv_s(m_Name.c_str(), value ? value : "");
#else
        if (value)
            setenv(m_Name.c_str(), value, 1);
        else
            unsetenv(m_Name.c_str());
#endif
    }

    std::string m_Name;
    std::string m_Previous;
    bool m_HadPrevious = false;
};
} // namespace

TEST(UIManagerFocusTests, AFocusedTextFieldConsumesOnlyTheChordsItImplements)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));
    ui.SetFocusById("field");

    // IsPrimaryShortcutModifier accepts Control on every platform.
    constexpr int shortcutModifier = Input::kModControl;

    // Select-all and the clipboard chords are the field's own editing gestures.
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_A, GLFW_PRESS, shortcutModifier));
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_C, GLFW_PRESS, shortcutModifier));
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_X, GLFW_PRESS, shortcutModifier));

    // Chords the field does not implement bubble, so the application's binding
    // for them still fires with the caret sitting in the field. This is what a
    // focused search box or rename field must do with Save.
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_S, GLFW_PRESS, shortcutModifier));
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_K, GLFW_PRESS, shortcutModifier));

    // The unmodified key is text, and stays with the field.
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_S, GLFW_PRESS, 0));

    // Alt splits by platform (Input::ComposesTextInput): on macOS Option
    // composes a character, so the keystroke is the field's; everywhere else
    // Alt+letter composes nothing — it is a chord (the default Frame All is
    // Alt+F) and bubbles to the application's binding for it.
#if defined(__APPLE__)
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_F, GLFW_PRESS, Input::kModAlt));
#else
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_F, GLFW_PRESS, Input::kModAlt));
#endif
}

namespace
{
// A panel that IS the behaviour for Ctrl/Cmd+S (a local Save): it acts, and
// says so, the way ScriptEditorPanel and the Animation window do — gated on the
// same shared focus scope they use, because the manager also offers the key to
// a merely-hovered panel.
class SavingPanel : public UIElement
{
  public:
    int SaveCount = 0;
    bool HandlesTheChord = true;

    void OnEvent(UIEvent& e) override
    {
        if (e.Id != kEventKeyDown || e.Key != Input::kKeyCode_S ||
            !Input::IsPrimaryShortcutModifier(e.Mods))
            return;
        if (!HandlesTheChord || !UI::FocusIsInside(*this))
            return;
        ++SaveCount;
        e.Stop();
    }
};
} // namespace

TEST(UIManagerFocusTests, AChordIsConsumedExactlyWhenAHandlerActsOnIt)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto panelOwned = std::make_unique<SavingPanel>();
    SavingPanel* panel = panelOwned.get();
    panel->SetId("panel");
    root->AddChild(std::move(panelOwned));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));
    ui.SetFocusById("panel");

    constexpr int shortcutModifier = Input::kModControl;

    // Handled: the panel's Save ran, so the chord is consumed here and the
    // application never sees it.
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_S, GLFW_PRESS, shortcutModifier));
    EXPECT_EQ(panel->SaveCount, 1);
    ui.OnKey(Input::kKeyCode_S, 0 /*release*/, shortcutModifier);

    // Declined: nothing acted, so the chord is not consumed and routes on to
    // the application's binding for it.
    panel->HandlesTheChord = false;
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_S, GLFW_PRESS, shortcutModifier));
    EXPECT_EQ(panel->SaveCount, 1);
}

// Hover is not focus. The manager offers an unhandled key to the hovered
// element as well, so a panel whose Save checks only the chord fires while the
// pointer simply rests on it — and consuming it there takes the application's
// binding for that keystroke away.
TEST(UIManagerFocusTests, AHoveredPanelWithoutFocusDoesNotClaimTheSaveChord)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UiRgHarness rg(dev.get());

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto panelOwned = std::make_unique<SavingPanel>();
    SavingPanel* panel = panelOwned.get();
    panel->SetId("panel");
    root->AddChild(std::move(panelOwned));
    auto fieldOwned = std::make_unique<TextField>();
    auto* field = fieldOwned.get();
    field->SetId("field");
    root->AddChild(std::move(fieldOwned));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "ui_hovered_panel_save.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#panel { width: 100px; height: 20px; }
#field { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    ASSERT_GT(panel->GetLayoutWidth(), 0.0f) << "panel must have a box to hover";
    DriveUiRender(ui, rg);

    ui.OnMouseMove(panel->GetLayoutX() + 5.0f, panel->GetLayoutY() + 5.0f);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);
    for (UIElement* p = ui.GetHoveredElement();; p = p->GetParent())
    {
        ASSERT_NE(p, nullptr) << "pointer must be over the panel";
        if (p == panel)
            break;
    }

    constexpr int shortcutModifier = Input::kModControl;

    // Focus elsewhere: the panel is hovered only, so it must decline and the
    // chord must route on to whatever the application bound to it.
    ui.SetFocusById("field");
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_S, GLFW_PRESS, shortcutModifier));
    EXPECT_EQ(panel->SaveCount, 0);
    ui.OnKey(Input::kKeyCode_S, 0 /*release*/, shortcutModifier);

    // Nothing focused at all — the state a click on a non-focusable surface
    // leaves behind — is the same answer.
    ui.ClearFocus();
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_S, GLFW_PRESS, shortcutModifier));
    EXPECT_EQ(panel->SaveCount, 0);
}

TEST(UIManagerFocusTests, ConsumptionFollowsWhatTheControlDoesWithTheKey)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    auto number = std::make_unique<FloatField>();
    number->SetId("number");
    root->AddChild(std::move(number));
    auto area = std::make_unique<TextArea>();
    area->SetId("area");
    root->AddChild(std::move(area));
    auto slider = std::make_unique<Slider>();
    slider->SetId("slider");
    root->AddChild(std::move(slider));
    auto toggle = std::make_unique<Toggle>();
    toggle->SetId("toggle");
    root->AddChild(std::move(toggle));
    // The panel-shaped case the whole design turns on: play parks focus on the
    // Game View viewport, a bare UIElement that does nothing with any key.
    auto viewport = std::make_unique<UIElement>();
    viewport->SetId("viewport");
    root->AddChild(std::move(viewport));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_W, GLFW_PRESS, 0)) << "nothing focused";

    // A text editor turns the keystroke into text, so the key is its own.
    for (const char* id : {"field", "number", "area"})
    {
        ui.SetFocusById(id);
        EXPECT_TRUE(ui.OnKey(Input::kKeyCode_W, GLFW_PRESS, 0)) << id;
    }

    // These hold focus and do nothing with W. Holding focus is not consuming:
    // the press stays available to whatever is behind the UI, which is what
    // keeps gameplay fed while a viewport or a toggle is focused.
    for (const char* id : {"slider", "toggle", "viewport"})
    {
        ui.SetFocusById(id);
        EXPECT_FALSE(ui.OnKey(Input::kKeyCode_W, GLFW_PRESS, 0)) << id;
    }

    // ...and the same controls do consume the keys they act on.
    ui.SetFocusById("slider");
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_Right, GLFW_PRESS, 0)) << "slider owns its axis";
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_Up, GLFW_PRESS, 0)) << "horizontal slider, vertical key";

    ui.SetFocusById("toggle");
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_Space, GLFW_PRESS, 0)) << "toggle owns Space";
}

TEST(UIManagerFocusTests, AHoveredFieldWithoutFocusLeavesTheKeyToWhateverIsBehindIt)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UiRgHarness rg(dev.get());

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto fieldOwned = std::make_unique<TextField>();
    auto* field = fieldOwned.get();
    field->SetId("field");
    field->SetValue("ab");
    root->AddChild(std::move(fieldOwned));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "ui_hovered_field_keys.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 200px; height: 40px; }
#field { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    // Park the pointer over the field without ever giving it focus.
    ui.OnMouseMove(field->GetLayoutX() + 2.0f, field->GetLayoutY() + 2.0f);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);
    ASSERT_TRUE(ui.GetFocusedElementId().empty()) << "hovering must not focus";

    // Characters follow focus, so a hovered field can never act on this key.
    // Claiming it anyway is how a hovered Inspector field silently eats the
    // movement keys of the game running behind it.
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_W, GLFW_PRESS, 0)) << "hovered, not focused";

    // Same rule for the editing keys that need no character at all: without
    // focus this field must not be quietly editing itself under the pointer.
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_Backspace, GLFW_PRESS, 0)) << "hovered, not focused";
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_Left, GLFW_PRESS, 0)) << "hovered, not focused";
    EXPECT_EQ(field->GetValue(), "ab") << "a hovered field must not edit";

    // Focus arrives, the pointer has not moved: now the keystroke is the
    // field's, and Backspace really does edit.
    ui.SetFocusById("field");
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_W, GLFW_PRESS, 0)) << "focused and hovered";
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_Backspace, GLFW_PRESS, 0)) << "focused and hovered";
}

TEST(UIManagerFocusTests, ATextAreaThatCannotMoveItsCaretDoesNotClaimTheNavigationKey)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto area = std::make_unique<TextArea>();
    area->SetId("area");
    root->AddChild(std::move(area));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));
    ui.SetFocusById("area");

    // Nothing has been laid out or had a font resolved, so line metrics are
    // unavailable and the caret cannot move. The area still owns the keys that
    // type, but a navigation key it cannot act on belongs to whatever is behind
    // it — a scrolling list, or the game.
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_W, GLFW_PRESS, 0)) << "typing is still the area's";
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_Up, GLFW_PRESS, 0));
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_Down, GLFW_PRESS, 0));
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_PageUp, GLFW_PRESS, 0));
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_PageDown, GLFW_PRESS, 0));
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_Home, GLFW_PRESS, 0));
    EXPECT_FALSE(ui.OnKey(Input::kKeyCode_End, GLFW_PRESS, 0));

    // Ctrl+Home/End need no metrics: they jump to the ends of the buffer.
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_Home, GLFW_PRESS, Input::kModControl));
    EXPECT_TRUE(ui.OnKey(Input::kKeyCode_End, GLFW_PRESS, Input::kModControl));
}

TEST(UIManagerInputTests, DebugHotkeysStayOutOfTheWayUntilTheEnvironmentAsksForThem)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    // The variable is read once per manager, at construction, so each arm needs
    // its own UIManager and the value must be in place before it is built.
    {
        ScopedEnvVar off("GE_UI_DEBUG_KEYS", nullptr);
        UIManager ui(dev.get());
        ui.SetRoot(std::make_unique<UIElement>());

        // Nothing focused, nothing hovered: a bare function key is nobody's
        // here, and must stay available to the editor and to the running game.
        EXPECT_FALSE(ui.OnKey(Input::kKeyCode_F4, GLFW_PRESS, 0));
        EXPECT_FALSE(ui.OnKey(Input::kKeyCode_F5, GLFW_PRESS, 0));
        EXPECT_FALSE(ui.OnKey(Input::kKeyCode_F12, GLFW_PRESS, 0));
        EXPECT_FALSE(ui.IsTextDebugOverlayEnabled()) << "F4 must not have toggled anything";
    }

    {
        ScopedEnvVar on("GE_UI_DEBUG_KEYS", "1");
        UIManager ui(dev.get());
        ui.SetRoot(std::make_unique<UIElement>());

        EXPECT_TRUE(ui.OnKey(Input::kKeyCode_F4, GLFW_PRESS, 0));
        EXPECT_TRUE(ui.OnKey(Input::kKeyCode_F5, GLFW_PRESS, 0));
        EXPECT_TRUE(ui.OnKey(Input::kKeyCode_F12, GLFW_PRESS, 0));
        EXPECT_TRUE(ui.IsTextDebugOverlayEnabled()) << "F4 toggles the overlay when opted in";
    }
}

TEST(UIManagerInputTests, ModifierReleaseIgnoresStalePlatformMask)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UIManager ui(dev.get());

    ui.OnKey(Input::kKeyCode_LeftControl, GLFW_PRESS, Input::kModControl);
    EXPECT_NE(ui.GetModifierKeys() & Input::kModControl, 0);

    // Linux may report the pre-release modifier mask on this callback.
    ui.OnKey(Input::kKeyCode_LeftControl, /*release=*/0, Input::kModControl);
    EXPECT_EQ(ui.GetModifierKeys() & Input::kModControl, 0);
}

TEST(UIManagerInputTests, ModifierReleasePreservesOtherPhysicalSide)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UIManager ui(dev.get());

    ui.OnKey(Input::kKeyCode_LeftControl, GLFW_PRESS, Input::kModControl);
    ui.OnKey(Input::kKeyCode_RightControl, GLFW_PRESS, Input::kModControl);
    ui.OnKey(Input::kKeyCode_LeftControl, /*release=*/0, Input::kModControl);
    EXPECT_NE(ui.GetModifierKeys() & Input::kModControl, 0);

    ui.OnKey(Input::kKeyCode_RightControl, /*release=*/0, Input::kModControl);
    EXPECT_EQ(ui.GetModifierKeys() & Input::kModControl, 0);
}

TEST(UIManagerInputTests, FocusLossResetClearsHeldModifiers)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UIManager ui(dev.get());

    ui.OnKey(Input::kKeyCode_LeftShift, Input::kKeyActionPress, Input::kModShift);
    ui.OnKey(Input::kKeyCode_LeftSuper, Input::kKeyActionPress, Input::kModShift | Input::kModSuper);
    EXPECT_EQ(ui.GetModifierKeys(), Input::kModShift | Input::kModSuper);

    ui.ResetModifierKeys();

    EXPECT_EQ(ui.GetModifierKeys(), 0);
}

TEST(UIManagerInputTests, FocusLossResetClearsMaskDerivedModifier)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UIManager ui(dev.get());

    // Shift was already down when this window took focus, so the manager only
    // learns it from the event mask - there is no physical Shift key event.
    ui.OnKey(Input::kKeyCode_A, Input::kKeyActionPress, Input::kModShift);
    ui.OnKey(Input::kKeyCode_A, Input::kKeyActionRelease, Input::kModShift);
    EXPECT_EQ(ui.GetModifierKeys(), Input::kModShift);

    // The window system synthesizes releases on focus loss only for keys it saw
    // pressed here, and Shift is not one of them: nothing clears the mask-derived
    // flag, and the next click would carry a phantom Shift.
    ui.ResetModifierKeys();

    EXPECT_EQ(ui.GetModifierKeys(), 0);
}

TEST(UIManagerInputTests, PointerMaskAdoptsModifierHeldBeforeFocus)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UIManager ui(dev.get());

    // Shift went down before this window took focus, so no key event reports it
    // and the platform sends no synthetic press. The click's mask is the only
    // chance to see it, or the Shift-click arrives as a plain click.
    EXPECT_EQ(ui.GetModifierKeys(), 0);
    ui.SyncModifierKeys(Input::kModShift);
    EXPECT_EQ(ui.GetModifierKeys(), Input::kModShift);

    // A later click with Shift no longer down clears it again.
    ui.SyncModifierKeys(0);
    EXPECT_EQ(ui.GetModifierKeys(), 0);
}

TEST(UIManagerInputTests, PointerMaskDoesNotClearPhysicallyHeldModifier)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UIManager ui(dev.get());

    // A key press this manager saw stays authoritative until its release: a
    // pointer mask that omits it must not drop it.
    ui.OnKey(Input::kKeyCode_LeftShift, Input::kKeyActionPress, Input::kModShift);
    ui.SyncModifierKeys(0);
    EXPECT_EQ(ui.GetModifierKeys(), Input::kModShift);

    ui.OnKey(Input::kKeyCode_LeftShift, Input::kKeyActionRelease, 0);
    EXPECT_EQ(ui.GetModifierKeys(), 0);
}

TEST(UIManagerInputTests, LiveMaskReleasesPhysicallyHeldModifierAndKeepsReportedOnes)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UIManager ui(dev.get());

    // A live platform query outranks the tracked key state: the Super release
    // was lost to the host, and the query says only Shift is down now.
    ui.OnKey(Input::kKeyCode_LeftSuper, Input::kKeyActionPress, Input::kModSuper);
    ui.OnKey(Input::kKeyCode_RightShift, Input::kKeyActionPress, Input::kModSuper | Input::kModShift);
    ui.ReconcileModifierKeys(Input::kModShift);
    EXPECT_EQ(ui.GetModifierKeys(), Input::kModShift);

    // The kept side is still the physical one: its own release clears it.
    ui.OnKey(Input::kKeyCode_RightShift, Input::kKeyActionRelease, 0);
    EXPECT_EQ(ui.GetModifierKeys(), 0);

    // A modifier the query reports without any key event becomes held, and a
    // later query without it releases it again.
    ui.ReconcileModifierKeys(Input::kModControl);
    EXPECT_EQ(ui.GetModifierKeys(), Input::kModControl);
    ui.ReconcileModifierKeys(0);
    EXPECT_EQ(ui.GetModifierKeys(), 0);
}

TEST(UIManagerOrderTests, OrderPropertyReordersAndUnsettingRestoresOriginal)
{
    // Build device + render graph (headless)
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());

    // Ensure controls are registered so <uielement> is recognized
    UIRegistration::RegisterBuiltInControls();

    // XML layout: root container with three children a,b,c
    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
        <uielement id='c' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    // Create temp dir + CSS files
    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css1 = tmpDir / "ui_order_1.css";
    const auto css2 = tmpDir / "ui_order_2.css";

    // CSS 1: container 300px wide; each child 100px; order: b first (-1), c second (1), a last (2)
    {
        std::ofstream f(css1);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b, #c { width: 100px; height: 10px; }
#b { order: -1; }
#c { order: 1; }
#a { order: 2; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css1.string()));

    // Render once to compute layout
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    UIElement* c = r->FindById("c");
    ASSERT_TRUE(a && b && c);

    float ax1 = a->GetLayoutX();
    float bx1 = b->GetLayoutX();
    float cx1 = c->GetLayoutX();

    // Expect order: b(0), c(100), a(200)
    EXPECT_NEAR(bx1, 0.f, 0.5f);
    EXPECT_NEAR(cx1, 100.f, 0.5f);
    EXPECT_NEAR(ax1, 200.f, 0.5f);

	    // Sanity-check computed order from resolved styles after CSS 1.
	    const auto& styleA1 = a->GetResolvedStyle();
	    const auto& styleB1 = b->GetResolvedStyle();
	    const auto& styleC1 = c->GetResolvedStyle();
	    EXPECT_EQ(styleA1.Layout.Order, 2);
	    EXPECT_EQ(styleB1.Layout.Order, -1);
	    EXPECT_EQ(styleC1.Layout.Order, 1);

	    // CSS 2: explicitly reset order back to default 0 -> DOM order a,b,c restored
    {
        std::ofstream f(css2);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
	#a, #b, #c { width: 100px; height: 10px; order: 0; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css2.string()));

    // Render again after stylesheet change
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

	    // After CSS 2 we expect all children to have order 0 from the later
	    // stylesheet so DOM order a,b,c is restored.
	    const auto& styleA2 = a->GetResolvedStyle();
	    const auto& styleB2 = b->GetResolvedStyle();
	    const auto& styleC2 = c->GetResolvedStyle();
	    EXPECT_EQ(styleA2.Layout.Order, 0);
	    EXPECT_EQ(styleB2.Layout.Order, 0);
	    EXPECT_EQ(styleC2.Layout.Order, 0);
	
    float ax2 = a->GetLayoutX();
    float bx2 = b->GetLayoutX();
    float cx2 = c->GetLayoutX();

    // Expect DOM order restored: a(0), b(100), c(200)
    EXPECT_NEAR(ax2, 0.f, 0.5f);
    EXPECT_NEAR(bx2, 100.f, 0.5f);
    EXPECT_NEAR(cx2, 200.f, 0.5f);
}

TEST(UIManagerOrderTests, OrderRespectedUnderRTLWithMirroredPositions)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
        <uielement id='c' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css1 = tmpDir / "ui_order_rtl_1.css";
    const auto css2 = tmpDir / "ui_order_rtl_2.css";

    // CSS #1: direction: rtl; same order assignments -> visual order b, c, a, but mirrored positions: b(200), c(100), a(0)
    {
        std::ofstream f(css1);
        f << R"(
#root { display: flex; width: 300px; height: 40px; direction: rtl; }
#a, #b, #c { width: 100px; height: 10px; }
#b { order: -1; }
#c { order: 1; }
#a { order: 2; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css1.string()));

	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    UIElement* c = r->FindById("c");
    ASSERT_TRUE(a && b && c);

    float ax1 = a->GetLayoutX();
    float bx1 = b->GetLayoutX();
    float cx1 = c->GetLayoutX();

    EXPECT_NEAR(bx1, 200.f, 1.0f);
    EXPECT_NEAR(cx1, 100.f, 1.0f);
    EXPECT_NEAR(ax1, 0.f, 1.0f);

	    // Sanity-check computed order from resolved styles after CSS #1.
	    const auto& styleA1 = a->GetResolvedStyle();
	    const auto& styleB1 = b->GetResolvedStyle();
	    const auto& styleC1 = c->GetResolvedStyle();
	    EXPECT_EQ(styleA1.Layout.Order, 2);
	    EXPECT_EQ(styleB1.Layout.Order, -1);
	    EXPECT_EQ(styleC1.Layout.Order, 1);

	    // CSS #2: keep direction: rtl but reset order to 0 -> DOM order a,b,c mirrored: a(200), b(100), c(0)
    {
        std::ofstream f(css2);
        f << R"(
#root { display: flex; width: 300px; height: 40px; direction: rtl; }
	#a, #b, #c { width: 100px; height: 10px; order: 0; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css2.string()));

	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

	    // After CSS #2 we expect all children to have order 0 from the later
	    // stylesheet so DOM order a,b,c is restored (mirrored by RTL).
	    const auto& styleA2 = a->GetResolvedStyle();
	    const auto& styleB2 = b->GetResolvedStyle();
	    const auto& styleC2 = c->GetResolvedStyle();
	    EXPECT_EQ(styleA2.Layout.Order, 0);
	    EXPECT_EQ(styleB2.Layout.Order, 0);
	    EXPECT_EQ(styleC2.Layout.Order, 0);
	
    float ax2 = a->GetLayoutX();
    float bx2 = b->GetLayoutX();
    float cx2 = c->GetLayoutX();

    EXPECT_NEAR(ax2, 200.f, 1.0f);
    EXPECT_NEAR(bx2, 100.f, 1.0f);
    EXPECT_NEAR(cx2, 0.f, 1.0f);
}

TEST(UIManagerOrderTests, OrderWithWrapReordersAcrossLines)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a'/>
        <uielement id='b'/>
        <uielement id='c'/>
        <uielement id='d'/>
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_order_wrap.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; flex-wrap: wrap; align-content: flex-start; width: 210px; height: 100px; }
#a, #b, #c, #d { width: 100px; height: 20px; }
#b { order: -1; }
#d { order: 0; }
#c { order: 1; }
#a { order: 2; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    UIElement* c = r->FindById("c");
    UIElement* d = r->FindById("d");
    ASSERT_TRUE(a && b && c && d);

    // Expect rows: (b,d) at y=0; (c,a) at y=20
    EXPECT_NEAR(b->GetLayoutX(), 0.f, 1.0f);
    EXPECT_NEAR(b->GetLayoutY(), 0.f, 1.0f);
    EXPECT_NEAR(d->GetLayoutX(), 100.f, 1.0f);
    EXPECT_NEAR(d->GetLayoutY(), 0.f, 1.0f);
    EXPECT_NEAR(c->GetLayoutX(), 0.f, 1.0f);
    EXPECT_NEAR(c->GetLayoutY(), 20.f, 1.0f);
    EXPECT_NEAR(a->GetLayoutX(), 100.f, 1.0f);
    EXPECT_NEAR(a->GetLayoutY(), 20.f, 1.0f);
}

TEST(UIManagerOrderTests, ColumnDirectionOrderAffectsVerticalPositions)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a'/>
        <uielement id='b'/>
        <uielement id='c'/>
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_order_column.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; flex-direction: column; width: 100px; height: 100px; }
#a, #b, #c { width: 50px; height: 10px; }
#b { order: -1; }
#c { order: 1; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    UIElement* c = r->FindById("c");
    ASSERT_TRUE(a && b && c);

    EXPECT_NEAR(b->GetLayoutY(), 0.f, 1.0f);
    EXPECT_NEAR(a->GetLayoutY(), 10.f, 1.0f);
    EXPECT_NEAR(c->GetLayoutY(), 20.f, 1.0f);
}

TEST(UIManagerOrderTests, NestedContainersOrderAppliedIndependently)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='p0'>
            <uielement id='a0'/>
            <uielement id='b0'/>
        </uielement>
        <uielement id='p1'>
            <uielement id='a1'/>
            <uielement id='b1'/>
        </uielement>
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_order_nested.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 400px; height: 100px; }
#p0, #p1 { display: flex; width: 200px; height: 40px; }
#a0, #b0, #a1, #b1 { width: 100px; height: 20px; }
#p1 { order: -1; }
#p0 { order: 1; }
#b0 { order: -1; }
#a0 { order: 1; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a0 = r->FindById("a0");
    UIElement* b0 = r->FindById("b0");
    UIElement* a1 = r->FindById("a1");
    UIElement* b1 = r->FindById("b1");
    ASSERT_TRUE(a0 && b0 && a1 && b1);

    // Inside p0: b0 then a0; Inside p1: a1 then b1 (check relative order within each parent)
    float dx_p0 = a0->GetLayoutX() - b0->GetLayoutX();
    EXPECT_NEAR(dx_p0, 100.f, 1.0f);
    float dx_p1 = b1->GetLayoutX() - a1->GetLayoutX();
    EXPECT_NEAR(dx_p1, 100.f, 1.0f);
}

static TextField* FindTextFieldById(UIElement* root, const std::string& id)
{
    if (!root)
        return nullptr;
    if (auto* tf = dynamic_cast<TextField*>(root))
    {
        if (root->GetId() == id)
            return tf;
    }
    const auto& ch = root->GetChildren();
    for (const auto& c : ch)
    {
        if (auto* tf = FindTextFieldById(c.get(), id))
            return tf;
    }
    return nullptr;
}

// Full-pipeline regression test: build a UIManager + TextField tree from XML and CSS,
// run a real Update(..) so Yoga/layout + resolved styles are populated, then drive a
// pointer click via UIManager's OnMouseMove/OnMouseButton. The inner TextInput should
// place its caret using the same fontSize and caret map that layout/rendering used.
TEST(UIManagerOrderTests, TextFieldPointerEndToEndUsesResolvedStyle)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <TextField id='field' value='0000000000' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_textfield_pointer_e2e.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 300px; height: 40px; align-items: center; }
#field { width: 200px; height: 24px; padding-left: 4px; padding-right: 4px; font-size: 14px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // First frame: compute layout, resolved styles, and font atlas.
    ui.Update(0.0f, /*interactive=*/true);

    Rendering::Text::FontAtlas* atlas = ui.GetDefaultFontAtlas();
    if (!atlas)
    {
        GTEST_SKIP() << "Font atlas not available for TextField pointer e2e test";
    }

    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);

    TextField* field = FindTextFieldById(rootEl, "field");
    ASSERT_NE(field, nullptr);

    TextInput* textInput = FindFirstTextInput(field);
    ASSERT_NE(textInput, nullptr);

    const auto& resolvedStyle = textInput->GetResolvedStyle();
    EXPECT_NEAR(resolvedStyle.Visual.FontSize, 14.0f, 0.25f);

    const std::string text = textInput->GetValue();
    ASSERT_EQ(text.size(), 10u);

    const float px = std::max(1.0f, resolvedStyle.Visual.FontSize);
    auto measure = atlas->MeasureText(text, px);
    const std::vector<float>& caretMap = measure.caretXByByte;
    ASSERT_EQ(caretMap.size(), text.size() + 1);

    float padL = resolvedStyle.Layout.Padding.Left;
    float xBase = textInput->GetLayoutX() + padL;
    float mouseY = textInput->GetLayoutY() + textInput->GetLayoutHeight() * 0.5f;

    // Click at the first and last caret positions using UIManager's pointer API and
    // ensure the inner TextInput ends up with the matching caret index. If pointer
    // style resolution diverges from the resolved layout style (e.g., wrong
    // fontSize), one of these will typically land on the wrong caret. The first
    // click also takes focus, and a free-text field keeps the caret a mouse press
    // placed rather than selecting all over it.
    for (size_t pass = 0; pass < 2; ++pass)
    {
        const size_t expectedCaret = (pass == 0) ? 0u : text.size();
        float caretX = caretMap[expectedCaret];
        float mouseX = xBase + caretX;

        ui.OnMouseMove(mouseX, mouseY);
        ui.OnMouseButton(0, true);
        ui.Update(0.0f, /*interactive=*/true);

        ui.OnMouseButton(0, false);
        ui.Update(0.0f, /*interactive=*/true);

        EXPECT_EQ(textInput->GetCaretIndex(), static_cast<int>(expectedCaret));
    }
}

TEST(UIManagerMountTests, MountedElementStyledAndRendered)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    // Build tree programmatically: root -> mount (targets 'btn')
    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    Mount* mount = new Mount();
    mount->SetId("m");
    std::unique_ptr<UIElement> mountOwned(mount);

    Button button;
    button.SetId("btn");
    mount->SetTarget(&button); // not owned by mount

    root->AddChild(std::move(mountOwned));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_mount_basic.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#m { width: 300px; height: 40px; }
#btn { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    EXPECT_NEAR(button.GetLayoutWidth(), 100.f, 1.0f);
    EXPECT_NEAR(button.GetLayoutX(), 0.f, 1.0f);
    EXPECT_NEAR(button.GetLayoutY(), 0.f, 1.0f);
}

TEST(UIManagerMountTests, EventsBubbleToMountedTarget)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    Mount* mount = new Mount();
    mount->SetId("m");
    std::unique_ptr<UIElement> mountOwned(mount);

    Button button;
    button.SetId("btn");
    int clicks = 0;
    button.RegisterEventHandler(kEventButtonClick, [&](UIEvent&)
                      { ++clicks; });
    mount->SetTarget(&button);

    root->AddChild(std::move(mountOwned));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_mount_events.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 200px; height: 40px; }
#m { width: 200px; height: 40px; }
#btn { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // First layout
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    // Simulate a click inside the mounted button
    float bx = button.GetLayoutX();
    float by = button.GetLayoutY();
    ui.OnMouseMove(bx + 5, by + 5);
    ui.OnMouseButton(0, true); // down
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);
    ui.OnMouseButton(0, false); // up
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);

    EXPECT_EQ(clicks, 1);
}

TEST(UIManagerMountTests, KeyboardFocusTraversesAcrossMountPortal)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    // Build: root -> mount + inline TextField; mount targets another TextField
    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto mount = std::make_unique<Mount>();
    mount->SetId("m");

    auto tfAOwned = std::make_unique<TextField>();
    auto* tfA = tfAOwned.get();
    tfA->SetId("tfA");
    tfA->AddClass("text");

    auto tfBOwned = std::make_unique<TextField>();
    auto* tfB = tfBOwned.get();
    tfB->SetId("tfB");
    tfB->AddClass("text");
    mount->SetTarget(tfB);

    root->AddChild(std::move(mount));
    root->AddChild(std::move(tfAOwned));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_mount_focus.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 300px; height: 50px; }
#m { width: 150px; height: 50px; }
#tfA, #tfB { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // Initial layout + build focus order
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    // Click tfA to focus it
    ui.OnMouseMove(tfA->GetLayoutX() + 2, tfA->GetLayoutY() + 2);
    ui.OnMouseButton(0, true);
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);
    ui.OnMouseButton(0, false);
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);

    // Press Tab → should move focus to mounted tfB
    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);

    // Type 'X' → should go to tfB
	    ui.OnChar('X');
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);

    EXPECT_EQ(tfA->GetValue(), "");
    EXPECT_EQ(tfB->GetValue(), "X");

    // Shift+Tab back to tfA and type 'Y'
	    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, GLFW_MOD_SHIFT);
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);
	    ui.OnChar('Y');
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);

    EXPECT_EQ(tfA->GetValue(), "Y");
}

TEST(UIManagerMountTests, KeyboardActivationOnMountedButton)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto mount = std::make_unique<Mount>();
    mount->SetId("m");

    Button btn;
    btn.SetId("btn");
    btn.AddClass("button");
    int clicks = 0;
    btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&)
                   { ++clicks; });
    mount->SetTarget(&btn);

    root->AddChild(std::move(mount));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_mount_btn_keys.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 200px; height: 50px; }
#m { width: 200px; height: 50px; }
#btn { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	
	    // Initial non-interactive layout pass
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

		// Focus via Tab (only focusable control is the mounted button). Tab focus
		// updates happen synchronously in OnKey, but we still run an interactive
		// update first so focus order is built, and then once more so any focus
		// side-effects are observed.
		ui.Update(0.0f, /*interactive=*/true);
		ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
	
		// Space activates the focused button inside the key dispatch itself.
		// The Update and render that follow only settle the frame around it.
		ui.OnKey(GLFW_KEY_SPACE, GLFW_PRESS, 0);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
	    EXPECT_EQ(clicks, 1);

		// Enter triggers click in the same way.
		ui.OnKey(GLFW_KEY_ENTER, GLFW_PRESS, 0);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
	    EXPECT_EQ(clicks, 2);
}

TEST(UIManagerScrollTests, ScrollEventRoutesToScrollViewAndTextField)
{
	    auto dev = MakeHeadlessDevice();
	    if (!dev)
	    {
	        GTEST_SKIP() << "Device init failed";
	    }
	    UiRgHarness rg(dev.get());
	    UIRegistration::RegisterBuiltInControls();

	    auto root = std::make_unique<UIElement>();
	    root->SetId("root");

	    auto scrollView = std::make_unique<ScrollView>();
	    auto* sv = scrollView.get();
	    scrollView->SetId("sv");

	    auto textField = std::make_unique<TextField>();
	    textField->SetId("tf");
	    textField->SetValue(std::string("Hello world this is a long line that should scroll"));

	    scrollView->AddContent(std::move(textField));
	    root->AddChild(std::move(scrollView));

	    UIManager ui(dev.get());
	    ui.SetRoot(std::move(root));

	    const auto tmpDir = std::filesystem::temp_directory_path();
	    const auto css = tmpDir / "ui_scroll_textfield.css";
	    {
	        std::ofstream f(css);
	        f << R"(
#root { display: flex; width: 200px; height: 40px; }
#sv { width: 200px; height: 40px; }
#tf { width: 400px; height: 20px; }
)";
	    }
	    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

	    // Focus the text field so caret/selection state is initialized
	    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    ui.Update(0.0f, /*interactive=*/true);
	    DriveUiRender(ui, rg);

	    // Move caret to the end by sending a series of characters
	    for (char c : std::string("!!!!"))
	    {
	        ui.OnChar(static_cast<unsigned int>(c));
	    DriveUiRender(ui, rg);
	    }

	    // A clipped layout rect reports no overflow, so the view takes its extent the
	    // way the controls that scroll horizontally do: explicitly (ScrollView.h's
	    // SetContentSize is authoritative over the Yoga rect).
	    sv->SetContentSize(400.0f, 20.0f);

	    // The wheel routes through UI.Scroll to the focused field and bubbles to the
	    // ScrollView. The field is wider than the view, so a vertical wheel with no
	    // vertical range remaps onto the horizontal axis and the view actually moves
	    // — which is the only thing that makes the view report the event handled.
	    EXPECT_GT(sv->GetContentWidth(), sv->GetViewportWidth())
	        << "content " << sv->GetContentWidth() << " vs viewport " << sv->GetViewportWidth()
	        << ": the view needs horizontal range for the remap to have somewhere to go";
	    EXPECT_TRUE(ui.OnScroll(0.0f, -1.0f));
	    EXPECT_GT(sv->GetScrollX(), 0.0f) << "the wheel must have moved the view";
}

// Scroll chaining, as Chrome and the CSS spec have it: a view consumes a wheel
// exactly when it moved, so one whose content fits leaves the tick to its
// ancestors and one with range takes it.
TEST(UIManagerScrollTests, AScrollViewConsumesAWheelExactlyWhenItMoved)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto ancestorOwned = std::make_unique<UIElement>();
    ancestorOwned->SetId("ancestor");
    auto* ancestor = ancestorOwned.get();
    auto scrollOwned = std::make_unique<ScrollView>();
    auto* scroll = scrollOwned.get();
    scroll->SetId("sv");
    auto contentOwned = std::make_unique<UIElement>();
    contentOwned->SetId("content");
    scroll->AddContent(std::move(contentOwned));
    ancestor->AddChild(std::move(scrollOwned));
    root->AddChild(std::move(ancestorOwned));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    int ancestorTicks = 0;
    ancestor->RegisterEventHandler(kEventScroll, [&ancestorTicks](UIEvent&) { ++ancestorTicks; });

    const auto css = std::filesystem::temp_directory_path() / "ui_scroll_chaining.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 200px; height: 200px; }
#ancestor { width: 200px; height: 200px; }
#sv { width: 200px; height: 100px; }
#content { width: 200px; height: 50px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
    ASSERT_GT(scroll->GetLayoutWidth(), 0.0f) << "the view must have a box to hover";
    scroll->SetContentSize(200.0f, 50.0f);

    ui.OnMouseMove(scroll->GetLayoutX() + 5.0f, scroll->GetLayoutY() + 5.0f);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);
    for (UIElement* p = ui.GetHoveredElement();; p = p->GetParent())
    {
        ASSERT_NE(p, nullptr) << "the pointer must be over the scroll view";
        if (p == scroll)
            break;
    }

    EXPECT_FALSE(ui.OnScroll(0.0f, -1.0f)) << "content that fits leaves the tick unconsumed";
    EXPECT_FLOAT_EQ(scroll->GetScrollY(), 0.0f) << "and nothing moved";
    EXPECT_EQ(ancestorTicks, 1) << "the tick must chain to the ancestor";

    scroll->SetContentSize(200.0f, 1000.0f);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);

    EXPECT_TRUE(ui.OnScroll(0.0f, -1.0f)) << "with range to move in, the view takes the tick";
    EXPECT_GT(scroll->GetScrollY(), 0.0f) << "and it moved";
    EXPECT_EQ(ancestorTicks, 1) << "a consumed tick must not also reach the ancestor";
}
		
		TEST(UIManagerBackgroundTests, BackgroundImageTintAliasResolvesInResolvedStyle)
		{
		    auto dev = MakeHeadlessDevice();
		    if (!dev)
		    {
		        GTEST_SKIP() << "Device init failed";
		    }
		    UiRgHarness rg(dev.get());
		    UIRegistration::RegisterBuiltInControls();
		
		    const std::string xml = R"(<uielement id='root'>
		        <uielement id='bg' />
		    </uielement>)";
		
		    std::unique_ptr<UIElement> root;
		    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
		
		    UIManager ui(dev.get());
		    ui.SetRoot(std::move(root));
		
		    const auto tmpDir = std::filesystem::temp_directory_path();
		    const auto css = tmpDir / "ui_background_tint_alias.css";
		    {
		        std::ofstream f(css);
		        f << R"(
		#root { display: flex; width: 100px; height: 100px; }
		#bg {
		    width: 100px;
		    height: 100px;
		    background-image: url("textures/icon.png");
		    background-image-tint: rgba(255,0,0,0.5);
		}
		)";
		    }
		    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
		
		    ui.Update(0.0f, /*interactive=*/false);
		    DriveUiRender(ui, rg);
		
		    UIElement* rootEl = ui.GetRootElement();
		    ASSERT_NE(rootEl, nullptr);
		    UIElement* bg = rootEl->FindById("bg");
		    ASSERT_NE(bg, nullptr);
		
		    const auto& st = bg->GetResolvedStyle();
		    EXPECT_TRUE(st.Visual.BackgroundImage.HasImage);
		    EXPECT_TRUE(st.Visual.BackgroundImage.HasTint);
		    EXPECT_EQ(st.Visual.BackgroundImage.Tint, 0x80FF0000u);
		}
		
		TEST(UIManagerBackgroundTests, BackgroundImageWithoutTintLeavesFlagFalse)
		{
		    auto dev = MakeHeadlessDevice();
		    if (!dev)
		    {
		        GTEST_SKIP() << "Device init failed";
		    }
		    UiRgHarness rg(dev.get());
		    UIRegistration::RegisterBuiltInControls();
		
		    const std::string xml = R"(<uielement id='root'>
		        <uielement id='bg' />
		    </uielement>)";
		
		    std::unique_ptr<UIElement> root;
		    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
		
		    UIManager ui(dev.get());
		    ui.SetRoot(std::move(root));
		
		    const auto tmpDir = std::filesystem::temp_directory_path();
		    const auto css = tmpDir / "ui_background_notint.css";
		    {
		        std::ofstream f(css);
		        f << R"(
		#root { display: flex; width: 100px; height: 100px; }
		#bg {
		    width: 100px;
		    height: 100px;
		    background-image: url("textures/icon.png");
		}
		)";
		    }
		    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
		
		    ui.Update(0.0f, /*interactive=*/false);
		    DriveUiRender(ui, rg);
		
		    UIElement* rootEl = ui.GetRootElement();
		    ASSERT_NE(rootEl, nullptr);
		    UIElement* bg = rootEl->FindById("bg");
		    ASSERT_NE(bg, nullptr);
		
		    const auto& st = bg->GetResolvedStyle();
		    EXPECT_TRUE(st.Visual.BackgroundImage.HasImage);
		    EXPECT_FALSE(st.Visual.BackgroundImage.HasTint);
		}
		
		TEST(UIManagerMountTests, HoverEventsRouteToMountedTarget)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto mount = std::make_unique<Mount>();
    mount->SetId("m");

    Button btn;
    btn.SetId("btn");
    size_t moves = 0;
    btn.RegisterEventHandler(kEventMouseMove, [&](UIEvent&)
                             { ++moves; });
    mount->SetTarget(&btn);

    root->AddChild(std::move(mount));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_mount_hover.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 300px; height: 60px; }
#m { width: 300px; height: 60px; }
#btn { width: 120px; height: 30px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    // Move inside button → should receive mousemove
	ui.OnMouseMove(btn.GetLayoutX() + 4, btn.GetLayoutY() + 4);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
    auto movesAfterEnter = moves;
    EXPECT_GT(movesAfterEnter, 0u);

    // Move far outside → button should not accumulate more moves
	ui.OnMouseMove(10000, 10000);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
    EXPECT_EQ(moves, movesAfterEnter);
}

TEST(UIManagerDockingIntegrationTests, DockedPanelsReceiveOwnerManager)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    // Build a docking model with a single leaf tab that hosts an Inspector
    // panel implemented as a UIElement containing a TextField.
    DockingManager dm;

    auto panel = std::make_unique<UIElement>();
    panel->SetId("InspectorPanel");
    auto tf = std::make_unique<TextField>();
    tf->SetId("inspectorField");
    TextField* tfRaw = tf.get();
    panel->AddChild(std::move(tf));
    UIElement* panelRaw = panel.get();

    dm.RegisterPanel("Inspector", panelRaw);
    auto rootNode = DockNode::MakeLeaf();
    rootNode->AddTab("Inspector");
    dm.SetRoot(std::move(rootNode));

    // Wire DockspaceElement into a UIManager tree. The dockspace does not own
    // the panel; it mounts it via Mount portals. RebuildFromModel must assign
    // the UIManager as owner to the panel tree so pointer routing works.
    UIManager ui(dev.get());
    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto dock = std::make_unique<DockspaceElement>();
    dock->SetId("dock");
    dock->BindModel(&dm);
    DockspaceElement* dockRaw = dock.get();
    root->AddChild(std::move(dock));

    ui.SetRoot(std::move(root));

    // At this point dock has an owner manager; rebuilding from the model
    // should propagate that owner to the external panel tree.
    dockRaw->RebuildFromModel();

    EXPECT_EQ(panel->GetOwnerManager(), &ui);
    EXPECT_EQ(tfRaw->GetOwnerManager(), &ui);
}

TEST(UIManagerFocusTests, TabIndexOrdersAndFocusableFlag)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto tf1 = std::make_unique<TextField>();
    tf1->SetId("tf1");
    tf1->SetTabIndex(3);
    auto tf2 = std::make_unique<TextField>();
    tf2->SetId("tf2");
    tf2->SetTabIndex(1);
    auto box = std::make_unique<UIElement>();
    box->SetId("box");
    box->SetFocusable(true); // tabindex=0
    auto tf3 = std::make_unique<TextField>();
    tf3->SetId("tf3");
    tf3->SetTabIndex(2);

    root->AddChild(std::move(tf1));
    root->AddChild(std::move(tf2));
    root->AddChild(std::move(box));
    root->AddChild(std::move(tf3));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    // Minimal CSS for layout sizes (not strictly required for focus order)
    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_focus_tabindex.css";
    {
        std::ofstream f(css);
        f << R"(#root { display: flex; width: 400px; height: 80px; } #tf1,#tf2,#tf3,#box { width: 80px; height: 20px; })";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // Build focus order
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    // Expected order: tf2 (1), tf3 (2), tf1 (3), then zeros in DOM order → box
    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("tf2"));

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("tf3"));

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("tf1"));

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("box"));
}

TEST(UIManagerFocusTests, DisabledAndHiddenSkippedInTabOrder)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto tf1 = std::make_unique<TextField>();
    tf1->SetId("tf1");
    auto tf2 = std::make_unique<TextField>();
    tf2->SetId("tf2");
    tf2->SetEnabled(false); // disabled
    auto tf3 = std::make_unique<TextField>();
    tf3->SetId("tf3"); // will be display:none via CSS
    auto tf4 = std::make_unique<TextField>();
    tf4->SetId("tf4");

    root->AddChild(std::move(tf1));
    root->AddChild(std::move(tf2));
    root->AddChild(std::move(tf3));
    root->AddChild(std::move(tf4));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_focus_hidden.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 400px; height: 80px; }
#tf1,#tf2,#tf3,#tf4 { width: 80px; height: 20px; }
#tf3 { display: none; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // Build focus order
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    // Tab should skip disabled tf2 and hidden tf3: order tf1 → tf4
    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("tf1"));

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("tf4"));
}

TEST(UIManagerFocusTests, TabIndexAndFocusableFromXml)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    const char* xml = R"(
<uielement id="root">
  <input id="a" tabindex="2" />
  <div id="b" focusable="true" />
  <input id="c" tabindex="1" />
</uielement>
)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_focus_xml.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 500px; height: 100px; }
#root > * { width: 80px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    // Expected tab order: c(1) → a(2) → b(0 focusable)
    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("c"));

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("a"));

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("b"));
}

TEST(UIManagerFocusTests, EnabledFromXmlAffectsFocusOrder)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    const char* xml = R"(
<uielement id="root">
  <input id="a" tabindex="1" enabled="false" />
  <input id="b" tabindex="2" />
</uielement>
)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_enabled_xml.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 400px; height: 80px; }
#root > * { width: 80px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

    // 'a' is disabled via XML, so Tab should focus 'b' first (tabindex=2)
    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
	    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("b"));
}

	TEST(UIManagerFocusTests, MouseClickOnTextFieldUsesFocusProxy)
	{
	    auto dev = MakeHeadlessDevice();
	    if (!dev)
	    {
	        GTEST_SKIP() << "Device init failed";
	    }
	    UiRgHarness rg(dev.get());
	    UIRegistration::RegisterBuiltInControls();

	    // Root with a single TextField. The field owns an internal TextInput editor
	    // which should delegate focus back to the field via UIElement's focus proxy.
	    auto root = std::make_unique<UIElement>();
	    root->SetId("root");

	    auto tfOwned = std::make_unique<TextField>();
	    auto* tf = tfOwned.get();
	    tf->SetId("tf");
	    tf->AddClass("text");
	    root->AddChild(std::move(tfOwned));

	    UIManager ui(dev.get());
	    ui.SetRoot(std::move(root));

	    const auto tmpDir = std::filesystem::temp_directory_path();
	    const auto css = tmpDir / "ui_focus_textinput_proxy.css";
	    {
	        std::ofstream f(css);
	        f << R"(
	#root { display: flex; width: 200px; height: 40px; }
	#tf { width: 120px; height: 20px; }
	)";
	    }
	    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

	    // Initial layout so TextField and its internal TextInput have positions.
	    ui.Update(0.0f, /*interactive=*/false);
	    DriveUiRender(ui, rg);

	    // Click inside the TextField. Hit-testing will report the deepest element
	    // (the internal TextInput), but focus assignment should resolve to the
	    // owning TextField via UIElement::ResolveFocusTarget / SetFocusProxy.
			ui.OnMouseMove(tf->GetLayoutX() + 2, tf->GetLayoutY() + 2);
			ui.Update(0.0f, /*interactive=*/true);
			DriveUiRender(ui, rg);
			ui.OnMouseButton(0, true);
			ui.Update(0.0f, /*interactive=*/true);
			DriveUiRender(ui, rg);
			ui.OnMouseButton(0, false);
			ui.Update(0.0f, /*interactive=*/true);
			DriveUiRender(ui, rg);

	    EXPECT_EQ(ui.GetFocusedElementId(), std::string("tf"));

			// Type a character; it should go to the TextField value even though the
			// actual editor is the internal TextInput. Character events are queued by
			// OnChar and consumed on the next interactive Update.
			ui.OnChar('A');
			ui.Update(0.0f, /*interactive=*/true);
			DriveUiRender(ui, rg);
	    EXPECT_EQ(tf->GetValue(), "A");
	}

TEST(UIManagerFocusTests, DragSelectingOutsideTextFieldRetainsFocus)
{
	auto dev = MakeHeadlessDevice();
	if (!dev)
	{
	    GTEST_SKIP() << "Device init failed";
	}
	UiRgHarness rg(dev.get());
	UIRegistration::RegisterBuiltInControls();

	// Root with a single TextField. We'll start a drag selection inside and
	// release the mouse outside the field; focus should remain on the field.
	auto root = std::make_unique<UIElement>();
	root->SetId("root");

	auto tfOwned = std::make_unique<TextField>();
	auto* tf = tfOwned.get();
	tf->SetId("tf");
	tf->AddClass("text");
	tf->SetValue(std::string("Drag selection test"));
	root->AddChild(std::move(tfOwned));

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));

	const auto tmpDir = std::filesystem::temp_directory_path();
	const auto css = tmpDir / "ui_focus_textinput_drag.css";
	{
	    std::ofstream f(css);
	    f << R"(
	#root { display: flex; width: 200px; height: 40px; }
	#tf { width: 120px; height: 20px; }
	)";
	}
	ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

	// Initial layout pass so layout rects are valid.
	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	const float xInside = tf->GetLayoutX() + 2.0f;
	const float yInside = tf->GetLayoutY() + tf->GetLayoutHeight() * 0.5f;
	const float xOutside = tf->GetLayoutX() + tf->GetLayoutWidth() + 50.0f;

		// Mouse down inside the text field to start selection (captures pointer).
		ui.OnMouseMove(xInside, yInside);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
		ui.OnMouseButton(0, true);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
	
		// Drag outside the field while still holding the mouse button.
		ui.OnMouseMove(xOutside, yInside);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);
	
		// Release the mouse button outside the field. Because capture started on
		// the field, focus should be preserved.
		ui.OnMouseButton(0, false);
		ui.Update(0.0f, /*interactive=*/true);
		DriveUiRender(ui, rg);

	EXPECT_EQ(ui.GetFocusedElementId(), std::string("tf"));
}

// C-10 (P4): re-adding an attached global stylesheet is a position-
// preserving no-op. The old contract moved it to the end — silently and
// permanently promoting it over every equal-specificity rule, differently
// per session depending on add order.
TEST(UIManagerStylesheetTests, GlobalReattachingStylesheetPreservesPrecedence)
{
	auto dev = MakeHeadlessDevice();
	if (!dev)
	{
	    GTEST_SKIP() << "Device init failed";
	}
	UiRgHarness rg(dev.get());
	UIRegistration::RegisterBuiltInControls();

	const std::string xml = R"(<uielement id='root'>
	    <uielement id='target'/>
	</uielement>)";

	std::unique_ptr<UIElement> root;
	ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));
	// The UI module's default stylesheet is already in place.
	const size_t initialSheets = ui.GetStylesheets().size();

	Stylesheet sheet1{};
	Stylesheet sheet2{};
	ASSERT_TRUE(CSSParser::ParseStylesFromString("#target { color: red; }\n", sheet1));
	ASSERT_TRUE(CSSParser::ParseStylesFromString("#target { color: blue; }\n", sheet2));

	StylesheetHandle h1 = std::make_shared<const Stylesheet>(sheet1);
	StylesheetHandle h2 = std::make_shared<const Stylesheet>(sheet2);

	ui.AddStylesheet(h1);
	ui.AddStylesheet(h2);
	EXPECT_EQ(ui.GetStylesheets().size(), initialSheets + 2u);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	UIElement* r = ui.GetRootElement();
	ASSERT_NE(r, nullptr);
	UIElement* target = r->FindById("target");
	ASSERT_NE(target, nullptr);
	const auto& style1 = target->GetResolvedStyle();
	EXPECT_EQ(style1.Visual.Color, 0xFF0000FFu);

	// Reattach the first stylesheet globally: position-preserving no-op —
	// the later-attached sheet must keep winning the equal-specificity tie.
	ui.AddStylesheet(h1);
	EXPECT_EQ(ui.GetStylesheets().size(), initialSheets + 2u);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	const auto& style2 = target->GetResolvedStyle();
	EXPECT_EQ(style2.Visual.Color, 0xFF0000FFu);
}

TEST(UIManagerStylesheetTests, LocalStylesheetReattachReordersForSubtree)
{
	auto dev = MakeHeadlessDevice();
	if (!dev)
	{
	    GTEST_SKIP() << "Device init failed";
	}
	UiRgHarness rg(dev.get());
	UIRegistration::RegisterBuiltInControls();

	const std::string xml = R"(<uielement id='root'>
	    <uielement id='container'>
	        <uielement id='target'/>
	    </uielement>
	</uielement>)";

	std::unique_ptr<UIElement> root;
	ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));
	// The UI module's default stylesheet is already in place.
	const size_t initialSheets = ui.GetStylesheets().size();

	Stylesheet sheet1{};
	Stylesheet sheet2{};
	ASSERT_TRUE(CSSParser::ParseStylesFromString("#target { color: red; }\n", sheet1));
	ASSERT_TRUE(CSSParser::ParseStylesFromString("#target { color: blue; }\n", sheet2));

	StylesheetHandle h1 = std::make_shared<const Stylesheet>(sheet1);
	StylesheetHandle h2 = std::make_shared<const Stylesheet>(sheet2);

	ui.AddStylesheet(h1);
	ui.AddStylesheet(h2);
	EXPECT_EQ(ui.GetStylesheets().size(), initialSheets + 2u);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	UIElement* r = ui.GetRootElement();
	ASSERT_NE(r, nullptr);
	UIElement* container = r->FindById("container");
	UIElement* target = r->FindById("target");
	ASSERT_TRUE(container && target);
	const auto& baseStyle = target->GetResolvedStyle();
	EXPECT_EQ(baseStyle.Visual.Color, 0xFF0000FFu);

	// Attach the first stylesheet at the container; it should be moved to the end
	// of the effective list for this subtree so red wins under 'container'.
	container->AddStylesheet(h1);
	EXPECT_EQ(container->GetStylesheets().size(), 1u);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	const auto& subtreeStyle = target->GetResolvedStyle();
	EXPECT_EQ(subtreeStyle.Visual.Color, 0xFFFF0000u);
}

TEST(UIManagerStylesheetTests, HotReloadRemovingPropertyRevertsToDefault)
{
	auto dev = MakeHeadlessDevice();
	if (!dev)
	{
	    GTEST_SKIP() << "Device init failed";
	}
	UiRgHarness rg(dev.get());
	UIRegistration::RegisterBuiltInControls();

	const std::string xml = R"(<uielement id='root'>
	    <uielement id='target'/>
	</uielement>)";

	std::unique_ptr<UIElement> root;
	ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));
	// The UI module's default stylesheet is already in place.
	const size_t initialSheets = ui.GetStylesheets().size();

	const auto tmpDir = std::filesystem::temp_directory_path();
	const auto css = tmpDir / "ui_hot_reload_remove_property.css";

	// Initial stylesheet sets a property.
	{
	    std::ofstream f(css);
	    f << "#target { background-color: red; }\n";
	}
	ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	EXPECT_EQ(ui.GetStylesheets().size(), initialSheets + 1u);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	UIElement* r = ui.GetRootElement();
	ASSERT_NE(r, nullptr);
	UIElement* target = r->FindById("target");
	ASSERT_NE(target, nullptr);
	const auto& s1 = target->GetResolvedStyle();
	EXPECT_EQ(s1.Visual.BackgroundColor, 0xFFFF0000u);

	// Hot reload: remove the property by replacing the rule.
	{
	    std::ofstream f(css);
	    f << "#other { background-color: blue; }\n";
	}
	ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
	// Must not stack duplicate stylesheets; precedence stays stable.
	EXPECT_EQ(ui.GetStylesheets().size(), initialSheets + 1u);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	const auto& s2 = target->GetResolvedStyle();
	EXPECT_EQ(s2.Visual.BackgroundColor, 0x00000000u);
}

TEST(UIManagerStylesheetTests, RootPaddingFromClassAndPseudoAffectsChildLayout)
{
	auto dev = MakeHeadlessDevice();
	if (!dev)
	{
	    GTEST_SKIP() << "Device init failed";
	}
	UIRegistration::RegisterBuiltInControls();

	// Root element with a single child. The root carries class "root" so both
	// .root and :root rules can apply; :root appears later and should win for
	// padding via the usual CSS cascade rules.
	const std::string xml = R"(<uielement id='root' class='root'>
	    <uielement id='child'/>
	</uielement>)";

	std::unique_ptr<UIElement> root;
	ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));

	const auto tmpDir = std::filesystem::temp_directory_path();
	const auto css = tmpDir / "ui_root_padding_class_pseudo.css";
	{
	    std::ofstream f(css);
	    f << R"(
#root { display: flex; width: 200px; height: 100px; }
	.root { padding: 4px; }
:root { padding: 12px; }
#child { width: 10px; height: 10px; }
)";
	}
	ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

	// Single non-interactive update is enough to build Yoga layout and resolved styles.
	ui.Update(0.0f, /*interactive=*/false);

	UIElement* rootEl = ui.GetRootElement();
	ASSERT_NE(rootEl, nullptr);
	UIElement* child = rootEl->FindById("child");
	ASSERT_NE(child, nullptr);

	const auto& rootStyle = rootEl->GetResolvedStyle();
	const auto& childStyle = child->GetResolvedStyle();

	EXPECT_NEAR(rootStyle.Layout.Padding.Left, 12.0f, 0.5f);
	EXPECT_NEAR(rootStyle.Layout.Padding.Top, 12.0f, 0.5f);
	EXPECT_NEAR(rootStyle.Layout.Padding.Right, 12.0f, 0.5f);
	EXPECT_NEAR(rootStyle.Layout.Padding.Bottom, 12.0f, 0.5f);

	EXPECT_NEAR(child->GetLayoutX(), rootStyle.Layout.Padding.Left, 0.5f);
	EXPECT_NEAR(child->GetLayoutY(), rootStyle.Layout.Padding.Top, 0.5f);

	EXPECT_NEAR(childStyle.Layout.Padding.Left, 0.0f, 0.5f);
	EXPECT_NEAR(childStyle.Layout.Padding.Top, 0.0f, 0.5f);
}

TEST(UIManagerStylesheetTests, CheckedPseudoClassTracksCheckedState)
{
	auto dev = MakeHeadlessDevice();
	if (!dev)
	{
	    GTEST_SKIP() << "Device init failed";
	}
	UiRgHarness rg(dev.get());
	UIRegistration::RegisterBuiltInControls();

	// Single checkbox; UIManager should populate ElementState.checked from control state.
	const std::string xml = R"(<uielement id='root'>
	    <Checkbox id='box' checked='true'/>
	</uielement>)";

	std::unique_ptr<UIElement> root;
	ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));

	const auto tmpDir = std::filesystem::temp_directory_path();
	const auto css = tmpDir / "ui_checked_pseudo.css";
	{
	    std::ofstream f(css);
	    f << R"(
#box { width: 10px; }
#box:checked { width: 100px; }
)";
	}
	ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	UIElement* r = ui.GetRootElement();
	ASSERT_NE(r, nullptr);
	UIElement* box = r->FindById("box");
	ASSERT_NE(box, nullptr);
	auto* checkbox = dynamic_cast<Checkbox*>(box);
	ASSERT_NE(checkbox, nullptr);

	const auto& styleChecked = box->GetResolvedStyle();
	EXPECT_TRUE(styleChecked.Layout.Width.IsPx());
	EXPECT_NEAR(styleChecked.Layout.Width.Value, 100.f, 0.5f);

	// Flip the state to false and ensure :checked no longer matches.
	checkbox->SetChecked(false);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	const auto& styleUnchecked = box->GetResolvedStyle();
	EXPECT_TRUE(styleUnchecked.Layout.Width.IsPx());
	EXPECT_NEAR(styleUnchecked.Layout.Width.Value, 10.f, 0.5f);
}
	

	TEST(UIManagerOrderTests, ZIndexControlsMouseDownTargetForOverlappingSiblings)
	{
		auto dev = MakeHeadlessDevice();
		if (!dev)
		{
			GTEST_SKIP() << "Device init failed";
		}
		UiRgHarness rg(dev.get());
		UIRegistration::RegisterBuiltInControls();

		// Two absolutely positioned children that fully overlap at the same rect.
		// Without z-index we expect the later DOM sibling (#upper) to receive
		// mouse events. After assigning a higher z-index to #lower we expect it
		// to become the mouse target instead.
		const std::string xml = R"(<uielement id='root'>
		    <uielement id='lower' />
		    <uielement id='upper' />
		</uielement>)";

		std::unique_ptr<UIElement> root;
		ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

		UIManager ui(dev.get());
		ui.SetRoot(std::move(root));

		const auto tmpDir = std::filesystem::temp_directory_path();
		const auto css1 = tmpDir / "ui_zindex_overlap_1.css";
		const auto css2 = tmpDir / "ui_zindex_overlap_2.css";

		// CSS 1: both children share the same absolute rect, no z-index.
		{
			std::ofstream f(css1);
			f << R"(
	#root { width: 200px; height: 100px; }
	#lower, #upper {
	    position: absolute;
	    left: 0px;
	    top: 0px;
	    width: 100px;
	    height: 40px;
	}
	)";
		}
		ASSERT_TRUE(ui.AttachStyleFromFile(css1.string()));

		// Initial layout.
		ui.Update(0.0f, /*interactive=*/false);
		DriveUiRender(ui, rg);

		UIElement* r = ui.GetRootElement();
		ASSERT_NE(r, nullptr);
		UIElement* lower = r->FindById("lower");
		UIElement* upper = r->FindById("upper");
		ASSERT_TRUE(lower && upper);

		// Target a point inside the overlapping rect.
		const float hitX = lower->GetLayoutX() + lower->GetLayoutWidth() * 0.5f;
		const float hitY = lower->GetLayoutY() + lower->GetLayoutHeight() * 0.5f;

		bool lowerClicked = false;
		bool upperClicked = false;
		lower->RegisterEventHandler(kEventMouseDown, [&lowerClicked](UIEvent&)
		{
			lowerClicked = true;
		});
		upper->RegisterEventHandler(kEventMouseDown, [&upperClicked](UIEvent&)
		{
			upperClicked = true;
		});

		// With no z-index, the later sibling (#upper) should win hit-testing.
			// Drive a full mouse move + click gesture across two Update() calls to match
			// the engine's expected input sequencing (hover established on move, then
			// MouseDown edge detected on the following frame).
			ui.OnMouseMove(hitX, hitY);
			ui.Update(0.0f, /*interactive=*/true);
			ui.OnMouseButton(0, true);
			ui.Update(0.0f, /*interactive=*/true);

		EXPECT_FALSE(lowerClicked);
		EXPECT_TRUE(upperClicked);

			// Release the mouse button so that a subsequent press is treated as a new
			// gesture. UIManager only fires MouseDown on the transition from
			// !m_PrevMouseDown -> m_MouseDown, so tests must emulate a full
			// press+release cycle between clicks.
			ui.OnMouseButton(0, false);
			ui.Update(0.0f, /*interactive=*/true);

		// Now attach a second stylesheet that raises #lower above #upper.
		{
			std::ofstream f(css2);
			f << R"(
	#lower { z-index: 10; }
	)";
		}
		ASSERT_TRUE(ui.AttachStyleFromFile(css2.string()));

		lowerClicked = false;
		upperClicked = false;

		ui.Update(0.0f, /*interactive=*/false);
		DriveUiRender(ui, rg);

		// After attaching the second stylesheet, verify that styles were recomputed
		// and that z-index has been updated on #lower but not #upper.
		const auto& styleLower2 = lower->GetResolvedStyle();
		const auto& styleUpper2 = upper->GetResolvedStyle();
		EXPECT_EQ(styleLower2.Layout.ZIndex, 10);
		EXPECT_EQ(styleUpper2.Layout.ZIndex, 0);
		
		ui.OnMouseMove(hitX, hitY);
		ui.OnMouseButton(0, true);
		ui.Update(0.0f, /*interactive=*/true);

		EXPECT_TRUE(lowerClicked);
		EXPECT_FALSE(upperClicked);
	}
	
// NOTE: This test exercises a simplified dropdown-like overlay scenario without
// involving the full Dropdown control implementation, to avoid test fragility
// around font/text measurement details. It still validates that an absolutely
// positioned popup with a higher z-index wins hit-testing over a background
// element that occupies the same screen space.
TEST(UIManagerOrderTests, DropdownLikePopupRespectsZIndexOverBackground)
	{
		auto dev = MakeHeadlessDevice();
		if (!dev)
		{
			GTEST_SKIP() << "Device init failed";
		}
	UiRgHarness rg(dev.get());

	// Simple tree with a dropdown-like container and an underlying background.
	// We model the structure the Dropdown would create, but using plain
	// <uielement> nodes with explicit sizes so the test doesn't depend on
	// text measurement.
	const std::string xml = R"(<uielement id='root'>
	    <uielement id='dd' class='dropdown'>
	        <uielement class='dropdown-items open'>
	            <uielement class='dropdown-item' id='item1' />
	            <uielement class='dropdown-item' id='item2' />
	            <uielement class='dropdown-item' id='item3' />
	        </uielement>
	    </uielement>
	    <uielement id='under' />
	</uielement>)";

		std::unique_ptr<UIElement> root;
		ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

		UIManager ui(dev.get());
		ui.SetRoot(std::move(root));

	// Attach a stylesheet that mirrors the Editor's dropdown styling in a
	// simplified form: the popup (.dropdown-items) is absolutely positioned
	// with a high z-index so it should render above #under. We give the popup
	// explicit size so we don't depend on Yoga/text sizing.
	Stylesheet sheet{};
	const std::string css = R"(
	#root {
		width: 300px;
		height: 200px;
		position: relative;
	}

	#dd, .dropdown {
		display: flex;
		flex-direction: column;
		position: relative;
		width: 120px;
	}

	.dropdown-items {
		display: flex;
		flex-direction: column;
		position: absolute;
		top: 100%;
		left: 0px;
		margin-top: 2px;
		background-color: #202020;
		z-index: 1000;
		width: 120px;
		height: 60px;
	}

	.dropdown-item {
		flex: 1;
	}

	#under {
		position: absolute;
		left: 0px;
		top: 0px;
		width: 300px;
		height: 200px;
		z-index: 0;
		background-color: #404040;
	}
			)";
		ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
		ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

	UIElement* r = ui.GetRootElement();
	ASSERT_NE(r, nullptr);
	UIElement* ddBase = r->FindById("dd");
	ASSERT_NE(ddBase, nullptr);
	UIElement* items = nullptr;
	for (const auto& ch : ddBase->GetChildren())
	{
		if (ch && ch->HasClass("dropdown-items"))
		{
			items = ch.get();
			break;
		}
	}
	ASSERT_NE(items, nullptr);

	// First layout + geometry pass.
	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	// The items container should exist, be visible, and have non-zero size.
		SCOPED_TRACE(::testing::Message()
		    << "dropdown-items layout: x=" << items->GetLayoutX()
		    << ", y=" << items->GetLayoutY()
		    << ", w=" << items->GetLayoutWidth()
		    << ", h=" << items->GetLayoutHeight()
		    << ", childCount=" << items->GetChildren().size());
		const auto& itemsStyle = items->GetResolvedStyle();
		EXPECT_EQ(itemsStyle.Layout.DisplayMode, DisplayMode::Flex);
		EXPECT_EQ(itemsStyle.Layout.PositionType, PositionType::Absolute);
		EXPECT_EQ(itemsStyle.Layout.ZIndex, 1000);
		EXPECT_GT(items->GetLayoutHeight(), 0.0f);
		EXPECT_GT(items->GetLayoutWidth(), 0.0f);

	// Target a point inside the popup, a bit below its top edge. This is
	// guaranteed to be within both #under and the popup itself.
	const float hitX = items->GetLayoutX() + items->GetLayoutWidth() * 0.5f;
	const float hitY = items->GetLayoutY() + items->GetLayoutHeight() * 0.25f;

		UIElement* under = r->FindById("under");
		ASSERT_NE(under, nullptr);
			SCOPED_TRACE(::testing::Message()
			    << "under layout: x=" << under->GetLayoutX()
			    << ", y=" << under->GetLayoutY()
			    << ", w=" << under->GetLayoutWidth()
			    << ", h=" << under->GetLayoutHeight());

	bool underClicked = false;
	bool popupClicked = false;
	under->RegisterEventHandler(kEventMouseDown, [&underClicked](UIEvent&)
	{
		underClicked = true;
	});
	items->RegisterEventHandler(kEventMouseDown, [&popupClicked](UIEvent&)
	{
		popupClicked = true;
	});

	ui.OnMouseMove(hitX, hitY);
	ui.Update(0.0f, /*interactive=*/true);
	ui.OnMouseButton(0, true);
	ui.Update(0.0f, /*interactive=*/true);

	// The popup overlay should win hit-testing over the background due to its
	// higher z-index.
	EXPECT_TRUE(popupClicked);
		EXPECT_FALSE(underClicked);
	}

TEST(UIManagerOrderTests, DropdownPopupClickDoesNotMoveUnderlyingSlider)
{
	auto dev = MakeHeadlessDevice();
	if (!dev)
	{
		GTEST_SKIP() << "Device init failed";
	}
	UiRgHarness rg(dev.get());

	UIRegistration::RegisterBuiltInControls();

	const std::string xml = R"(<uielement id='root'>
		<uielement class='settings-row'>
			<dropdown id='dd' class='settings-row-field' options='Off, 1x, 2x' />
		</uielement>
		<slider id='underSlider' />
	</uielement>)";

	std::unique_ptr<UIElement> root;
	ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));

	Stylesheet sheet{};
	const std::string css = R"(
	#root {
		width: 240px;
		height: 120px;
		position: relative;
	}

	.dropdown {
		position: relative;
		width: 140px;
	}

	.dropdown-header {
		height: 20px;
		padding: 0px;
	}

	/* Mirrors the Settings panel rule: the trigger must fill the value-column
	   width instead of shrinking the chevron next to the selected label. */
	.settings-row .dropdown.settings-row-field {
		align-items: stretch;
	}

	.settings-row .dropdown.settings-row-field .dropdown-header {
		align-self: stretch;
		width: auto;
	}

	.dropdown-items {
		display: none;
		position: absolute;
		top: 100%;
		left: 0px;
		width: 140px;
		background-color: #202020;
		z-index: 4000;
		pointer-events: auto;
	}

	.dropdown.open .dropdown-items,
	.dropdown-items.open {
		display: flex;
		flex-direction: column;
	}

	.dropdown-item {
		height: 20px;
		padding: 0px;
	}

	#underSlider {
		position: absolute;
		left: 0px;
		top: 20px;
		width: 140px;
		height: 80px;
	}
	)";
	ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
	ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

	UIElement* r = ui.GetRootElement();
	ASSERT_NE(r, nullptr);
	auto* dd = dynamic_cast<Dropdown*>(r->FindById("dd"));
	ASSERT_NE(dd, nullptr);
	auto* slider = dynamic_cast<Slider*>(r->FindById("underSlider"));
	ASSERT_NE(slider, nullptr);
	slider->SetMin(0.0f);
	slider->SetMax(100.0f);
	slider->SetValue(0.0f);

	ui.Update(0.0f, /*interactive=*/false);
	DriveUiRender(ui, rg);

	ASSERT_NE(dd->GetHeaderContainer(), nullptr);
	EXPECT_FLOAT_EQ(dd->GetHeaderContainer()->GetLayoutWidth(), dd->GetLayoutWidth());

	dd->OpenMenuUi();
	ui.Update(0.0f, /*interactive=*/true);
	DriveUiRender(ui, rg);

	ASSERT_TRUE(dd->IsMenuOpen());
	const float before = slider->GetValue();

	ui.OnMouseMove(50.0f, 45.0f);
	ui.Update(0.0f, /*interactive=*/true);
	ui.OnMouseButton(0, true);
	ui.Update(0.0f, /*interactive=*/true);

	EXPECT_EQ(dd->GetSelectedIndex(), 1);

	// Selecting an item closes the popup while the button is still held.
	// The dropdown must keep capture for the rest of the press so a small
	// pointer move cannot be reinterpreted as a drag on the slider below.
	ui.OnMouseMove(135.0f, 45.0f);
	ui.Update(0.0f, /*interactive=*/true);
	ui.OnMouseButton(0, false);
	ui.Update(0.0f, /*interactive=*/true);

	EXPECT_EQ(slider->GetValue(), before);
}
		
		// This test exercises the actual Dropdown control + Editor-style CSS using
		// an auto-sized, absolutely positioned popup. It guards against regressions
		// where the .dropdown-items container stretches to the full root height
		// instead of wrapping its menu items.
		TEST(UIManagerOrderTests, DropdownUiPopupAutoSizesToItemsContent)
		{
		    auto dev = MakeHeadlessDevice();
		    if (!dev)
		    {
		        GTEST_SKIP() << "Device init failed";
		    }
		    UiRgHarness rg(dev.get());
		
		    UIRegistration::RegisterBuiltInControls();
		
		    const std::string xml = R"(<uielement id='root' class='ui-demo-panel-root'>
		        <dropdown id='dd' class='dropdown-ui' options='Low, Medium, High' />
		    </uielement>)";
		
		    std::unique_ptr<UIElement> root;
		    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
		
		    UIManager ui(dev.get());
		    ui.SetRoot(std::move(root));
		
		    Stylesheet sheet{};
		    const std::string css = R"(
		#root {
		    display: flex;
		    flex-direction: column;
		    width: 400px;
		    height: 300px;
		}
		
		.dropdown {
		    display: flex;
		    flex-direction: column;
		    min-width: 140px;
		    max-width: 260px;
		    position: relative;
		}
		
		.dropdown-header {
		    padding: 4px 10px;
		    border-width: 1px;
		    border-radius: 4px;
		    border-color: #3A3A3A;
		    background-color: #202020;
		}
		
		.dropdown-items {
		    display: none;
		    position: absolute;
		    top: 100%;
		    left: 0px;
		    margin-top: 2px;
		    border-width: 1px;
		    border-radius: 4px;
		    border-color: #3A3A3A;
		    background-color: #202020;
		    z-index: 1000;
		}
		
		.dropdown.open .dropdown-items,
		.dropdown-items.open {
		    display: flex;
		    flex-direction: column;
		}
		
		.dropdown-item {
		    padding: 3px 10px;
		}
		)";
		    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
		    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));
		
		    UIElement* r = ui.GetRootElement();
		    ASSERT_NE(r, nullptr);
		    auto* dd = dynamic_cast<Dropdown*>(r->FindById("dd"));
		    ASSERT_NE(dd, nullptr);
		
		    // Initial layout to create header and items container.
		    ui.Update(0.0f, /*interactive=*/false);
		    DriveUiRender(ui, rg);
		
		    // Open the UI-mode popup.
		    dd->OpenMenuUi();
		
		    // Run another update so Yoga + post-layout convergence and auto-size pass run
		    // with the "open" classes applied.
		    ui.Update(0.0f, /*interactive=*/true);
		    DriveUiRender(ui, rg);
		
		    // Locate the items container created by Dropdown.
		    UIElement* items = nullptr;
		    for (const auto& ch : dd->GetChildren())
		    {
		        if (ch && ch->HasClass("dropdown-items"))
		        {
		            items = ch.get();
		            break;
		        }
		    }
		    ASSERT_NE(items, nullptr);
		
		    SCOPED_TRACE(::testing::Message()
		        << "root layout: x=" << r->GetLayoutX()
		        << ", y=" << r->GetLayoutY()
		        << ", w=" << r->GetLayoutWidth()
		        << ", h=" << r->GetLayoutHeight());
		    SCOPED_TRACE(::testing::Message()
		        << "dropdown-items layout: x=" << items->GetLayoutX()
		        << ", y=" << items->GetLayoutY()
		        << ", w=" << items->GetLayoutWidth()
		        << ", h=" << items->GetLayoutHeight()
		        << ", childCount=" << items->GetChildren().size());
		
		    const auto& itemsStyle = items->GetResolvedStyle();
		    EXPECT_EQ(itemsStyle.Layout.PositionType, PositionType::Absolute);
		    EXPECT_EQ(itemsStyle.Layout.DisplayMode, DisplayMode::Flex);
		    EXPECT_EQ(itemsStyle.Layout.ZIndex, 1000);
		
		    // The popup must have non-zero size, but it should not stretch to the
		    // full height of the root container. This catches regressions where Yoga
		    // gives an over-large height and the absolute auto-size pass fails to
		    // shrink it to the bounding box of its items.
		    EXPECT_GT(items->GetLayoutHeight(), 0.0f);
		    EXPECT_GT(items->GetLayoutWidth(), 0.0f);
		    EXPECT_LT(items->GetLayoutHeight(), r->GetLayoutHeight());
			}
		
			// Same as DropdownUiPopupAutoSizesToItemsContent, but opens the popup via the
			// header's MouseDown event path instead of calling OpenMenuUi() directly.
			// This mirrors how the control is used in the Editor and ensures that the
			// event-driven toggle + deferred item rebuild still result in a correctly
			// auto-sized absolute popup.
			TEST(UIManagerOrderTests, DropdownUiPopupAutoSizesToItemsContent_FromHeaderClick)
			{
			    auto dev = MakeHeadlessDevice();
			    if (!dev)
			    {
			        GTEST_SKIP() << "Device init failed";
			    }
			    UiRgHarness rg(dev.get());
		
			    UIRegistration::RegisterBuiltInControls();
		
			    const std::string xml = R"(<uielement id='root' class='ui-demo-panel-root'>
			        <dropdown id='dd' class='dropdown-ui' options='Low, Medium, High' />
			    </uielement>)";
		
			    std::unique_ptr<UIElement> root;
			    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
		
			    UIManager ui(dev.get());
			    ui.SetRoot(std::move(root));
		
			    Stylesheet sheet{};
			    const std::string css = R"(
			#root {
			    display: flex;
			    flex-direction: column;
			    width: 400px;
			    height: 300px;
			}
			
			.dropdown {
			    display: flex;
			    flex-direction: column;
			    min-width: 140px;
			    max-width: 260px;
			    position: relative;
			}
			
			.dropdown-header {
			    padding: 4px 10px;
			    border-width: 1px;
			    border-radius: 4px;
			    border-color: #3A3A3A;
			    background-color: #202020;
			}
			
			.dropdown-items {
			    display: none;
			    position: absolute;
			    top: 100%;
			    left: 0px;
			    margin-top: 2px;
			    border-width: 1px;
			    border-radius: 4px;
			    border-color: #3A3A3A;
			    background-color: #202020;
			    z-index: 1000;
			}
			
			.dropdown.open .dropdown-items,
			.dropdown-items.open {
			    display: flex;
			    flex-direction: column;
			}
			
			.dropdown-item {
			    padding: 3px 10px;
			}
			)";
			    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
			    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));
		
			    UIElement* r = ui.GetRootElement();
			    ASSERT_NE(r, nullptr);
			    auto* dd = dynamic_cast<Dropdown*>(r->FindById("dd"));
			    ASSERT_NE(dd, nullptr);
		
			    // Initial layout so the header and items container are created and measured.
			    ui.Update(0.0f, /*interactive=*/false);
			    DriveUiRender(ui, rg);
		
			    // Drive a click on the header label through UIManager's mouse input path.
			    Label* header = dd->GetHeaderLabel();
			    ASSERT_NE(header, nullptr);
			    const float hitX = header->GetLayoutX() + header->GetLayoutWidth() * 0.5f;
			    const float hitY = header->GetLayoutY() + header->GetLayoutHeight() * 0.5f;
		
			    ui.OnMouseMove(hitX, hitY);
			    ui.Update(0.0f, /*interactive=*/true);
			    ui.OnMouseButton(0, true);
			    ui.Update(0.0f, /*interactive=*/true);
			    DriveUiRender(ui, rg);
		
			    UIElement* items = dd->GetItemsContainer();
			    ASSERT_NE(items, nullptr);
		
			    SCOPED_TRACE(::testing::Message()
			        << "root layout: x=" << r->GetLayoutX()
			        << ", y=" << r->GetLayoutY()
			        << ", w=" << r->GetLayoutWidth()
			        << ", h=" << r->GetLayoutHeight());
			    SCOPED_TRACE(::testing::Message()
			        << "dropdown-items layout: x=" << items->GetLayoutX()
			        << ", y=" << items->GetLayoutY()
			        << ", w=" << items->GetLayoutWidth()
			        << ", h=" << items->GetLayoutHeight()
			        << ", childCount=" << items->GetChildren().size());
		
			    const auto& itemsStyle = items->GetResolvedStyle();
			    EXPECT_EQ(itemsStyle.Layout.PositionType, PositionType::Absolute);
			    EXPECT_EQ(itemsStyle.Layout.DisplayMode, DisplayMode::Flex);
			    EXPECT_EQ(itemsStyle.Layout.ZIndex, 1000);
		
			    EXPECT_GT(items->GetLayoutHeight(), 0.0f);
			    EXPECT_GT(items->GetLayoutWidth(), 0.0f);
			    EXPECT_LT(items->GetLayoutHeight(), r->GetLayoutHeight());
			}

			// Regression: when closing a dropdown via the event-driven header click path,
			// the popup should stop rendering immediately (same Update) and not linger
			// for a frame due to draw-style cache reuse.
			TEST(UIManagerOrderTests, DropdownCloseHidesPopupInSameFrame_NoGhost)
			{
			    auto dev = MakeHeadlessDevice();
			    if (!dev)
			    {
			        GTEST_SKIP() << "Device init failed";
			    }
			    UiRgHarness rg(dev.get());
			
			    UIRegistration::RegisterBuiltInControls();
			
			    const std::string xml = R"(<uielement id='root' class='ui-demo-panel-root'>
			        <dropdown id='dd' class='dropdown-ui' options='Low, Medium, High' />
			    </uielement>)";
			
			    std::unique_ptr<UIElement> root;
			    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
			
			    UIManager ui(dev.get());
			    ui.SetRoot(std::move(root));
			
			    Stylesheet sheet{};
			    const std::string css = R"(
			#root {
			    display: flex;
			    flex-direction: column;
			    width: 400px;
			    height: 300px;
			}
			
			.dropdown {
			    display: flex;
			    flex-direction: column;
			    min-width: 140px;
			    max-width: 260px;
			    position: relative;
			}
			
			.dropdown-header {
			    padding: 4px 10px;
			    border-width: 1px;
			    border-radius: 4px;
			    border-color: #3A3A3A;
			    background-color: #202020;
			}
			
			.dropdown-items {
			    display: none;
			    position: absolute;
			    top: 100%;
			    left: 0px;
			    margin-top: 2px;
			    border-width: 1px;
			    border-radius: 4px;
			    border-color: #3A3A3A;
			    background-color: #202020;
			    z-index: 1000;
			}
			
			.dropdown.open .dropdown-items,
			.dropdown-items.open {
			    display: flex;
			    flex-direction: column;
			}
			
			.dropdown-item {
			    padding: 3px 10px;
			}
			)";
			    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
			    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));
			
			    UIElement* r = ui.GetRootElement();
			    ASSERT_NE(r, nullptr);
			    auto* dd = dynamic_cast<Dropdown*>(r->FindById("dd"));
			    ASSERT_NE(dd, nullptr);
			
			    // Initial layout so the header and items container are created and measured.
			    ui.Update(0.0f, /*interactive=*/false);
			    DriveUiRender(ui, rg);
			
			    Label* header = dd->GetHeaderLabel();
			    ASSERT_NE(header, nullptr);
			    const float hitX = header->GetLayoutX() + header->GetLayoutWidth() * 0.5f;
			    const float hitY = header->GetLayoutY() + header->GetLayoutHeight() * 0.5f;
			
			    // Open via MouseDown.
			    ui.OnMouseMove(hitX, hitY);
			    ui.Update(0.0f, /*interactive=*/true);
			    ui.OnMouseButton(0, true);
			    ui.Update(0.0f, /*interactive=*/true);
			    DriveUiRender(ui, rg);
			    ui.OnMouseButton(0, false);
			    ui.Update(0.0f, /*interactive=*/true);
			
			    UIElement* items = dd->GetItemsContainer();
			    ASSERT_NE(items, nullptr);
			    const auto& itemsStyleOpen = items->GetResolvedStyle();
			    EXPECT_EQ(itemsStyleOpen.Layout.DisplayMode, DisplayMode::Flex);
			
			    // Close via MouseDown again. The popup should be hidden immediately in this Update.
			    ui.OnMouseButton(0, true);
			    ui.Update(0.0f, /*interactive=*/true);
			    DriveUiRender(ui, rg);
			
			    const auto& itemsStyleClosed = items->GetResolvedStyle();
			    EXPECT_EQ(itemsStyleClosed.Layout.DisplayMode, DisplayMode::None);
			
			    // Release mouse for cleanliness.
			    ui.OnMouseButton(0, false);
			    ui.Update(0.0f, /*interactive=*/true);
			}

			// Ensure that instantiating a live UI tree from a UILayoutAsset template does not
			// duplicate control-internal children (dropdown headers/items, toggle knobs, etc.).
			TEST(UIManagerOrderTests, ControlsFromUILayoutAssetTemplateDoNotDuplicateInternalChildren)
			{
			    auto dev = MakeHeadlessDevice();
			    if (!dev)
			    {
			        GTEST_SKIP() << "Device init failed";
			    }
			    UiRgHarness rg(dev.get());
			
			    UIRegistration::RegisterBuiltInControls();
			
			    const GUID guid = GUID::Generate();
			    auto asset = std::make_shared<UILayoutAsset>(guid, std::filesystem::path("dummy.xml"));
			
			    const std::string xml = R"(<uielement id='root'>
			        <toggle id='t' />
			        <dropdown id='dd' options='Low, Medium, High' />
			        <button id='btn' text='Hello' />
			    </uielement>)";
			
			    Vector<uint8> data;
			    data.reserve(xml.size());
			    for (char c : xml) data.push_back((uint8)c);
			    ASSERT_TRUE(asset->LoadFromData(data));
			
			    UIManager ui(dev.get());
			    ASSERT_TRUE(ui.LoadLayoutFromAsset(*asset));
			    ui.Update(0.0f, /*interactive=*/false);
			    DriveUiRender(ui, rg);
			
			    UIElement* root = ui.GetRootElement();
			    ASSERT_NE(root, nullptr);
			
			    // Toggle: exactly one knob child.
			    auto* t = dynamic_cast<Toggle*>(root->FindById("t"));
			    ASSERT_NE(t, nullptr);
			    int knobCount = 0;
			    for (const auto& ch : t->GetChildren())
			    {
			        if (ch && ch->HasClass("toggle-knob"))
			            ++knobCount;
			    }
			    EXPECT_EQ(knobCount, 1);
			    EXPECT_EQ(t->GetChildren().size(), (size_t)1);
			
			    // Dropdown: exactly one header + one items container.
			    auto* dd = dynamic_cast<Dropdown*>(root->FindById("dd"));
			    ASSERT_NE(dd, nullptr);
			    int headerCount = 0;
			    int itemsCount = 0;
			    for (const auto& ch : dd->GetChildren())
			    {
			        if (!ch) continue;
			        if (ch->HasClass("dropdown-header")) ++headerCount;
			        if (ch->HasClass("dropdown-items")) ++itemsCount;
			    }
			    EXPECT_EQ(headerCount, 1);
			    EXPECT_EQ(itemsCount, 1);
			
			    // Button: exactly one internal label child.
			    auto* btn = dynamic_cast<Button*>(root->FindById("btn"));
			    ASSERT_NE(btn, nullptr);
			    int labelCount = 0;
			    for (const auto& ch : btn->GetChildren())
			    {
			        if (ch && ch->HasClass("button-text"))
			            ++labelCount;
			    }
			    EXPECT_EQ(labelCount, 1);
			    EXPECT_EQ(btn->GetChildren().size(), (size_t)1);
			}

			// Smoke-test hot reload reconciliation via UIHotReload + AssetManager events: the existing
			// Dropdown instance should be preserved and internal children must not multiply.
			TEST(UIManagerOrderTests, HotReload_ReconcilesTemplateAndPreservesDropdownInstance)
			{
			    auto dev = MakeHeadlessDevice();
			    if (!dev)
			    {
			        GTEST_SKIP() << "Device init failed";
			    }
			    UiRgHarness rg(dev.get());
			
			    UIRegistration::RegisterBuiltInControls();
			
			    AssetManager assets;
			    const GUID guid = GUID::Generate();
			    auto asset = std::make_shared<UILayoutAsset>(guid, std::filesystem::path("dummy.xml"));
			
			    const std::string xml1 = R"(<uielement id='root'>
			        <dropdown id='dd' options='Low, Medium, High' />
			    </uielement>)";
			    Vector<uint8> data1;
			    data1.reserve(xml1.size());
			    for (char c : xml1) data1.push_back((uint8)c);
			    ASSERT_TRUE(asset->LoadFromData(data1));
			    assets.RegisterLoadedAsset(guid, asset);
			
			    UIManager ui(dev.get(), &assets);
			    ASSERT_TRUE(ui.LoadLayoutFromAsset(*asset));
			    ui.Update(0.0f, /*interactive=*/false);
			    DriveUiRender(ui, rg);
			
			    UIElement* root = ui.GetRootElement();
			    ASSERT_NE(root, nullptr);
			    auto* dd = dynamic_cast<Dropdown*>(root->FindById("dd"));
			    ASSERT_NE(dd, nullptr);
			    Dropdown* ddPtr = dd;
			
			    UIElement* itemsBefore = dd->GetItemsContainer();
			    ASSERT_NE(itemsBefore, nullptr);
			    EXPECT_EQ(itemsBefore->GetChildren().size(), (size_t)3);
			
			    // Reload template: add one more option.
			    const std::string xml2 = R"(<uielement id='root'>
			        <dropdown id='dd' options='Low, Medium, High, Ultra' />
			    </uielement>)";
			    Vector<uint8> data2;
			    data2.reserve(xml2.size());
			    for (char c : xml2) data2.push_back((uint8)c);
			    ASSERT_TRUE(asset->LoadFromData(data2));
			
			    // Dispatch a reload event and let UIManager pump UIHotReload.
			    assets.GetEventDispatcher().DispatchEvent(AssetEvents::AssetReloaded(guid, AssetType::UILayout, "dummy.xml"));
			    ui.Update(0.0f, /*interactive=*/false);
			    DriveUiRender(ui, rg);
			
			    auto* dd2 = dynamic_cast<Dropdown*>(ui.GetRootElement()->FindById("dd"));
			    ASSERT_NE(dd2, nullptr);
			    EXPECT_EQ(dd2, ddPtr);
			
			    // Still exactly one header + one items container.
			    int headerCount = 0;
			    int itemsCount = 0;
			    for (const auto& ch : dd2->GetChildren())
			    {
			        if (!ch) continue;
			        if (ch->HasClass("dropdown-header")) ++headerCount;
			        if (ch->HasClass("dropdown-items")) ++itemsCount;
			    }
			    EXPECT_EQ(headerCount, 1);
			    EXPECT_EQ(itemsCount, 1);
			
			    UIElement* itemsAfter = dd2->GetItemsContainer();
			    ASSERT_NE(itemsAfter, nullptr);
			    EXPECT_EQ(itemsAfter->GetChildren().size(), (size_t)4);
			}

			// Regression: absolute override rects must set position edges so Yoga
			// applies per-item offsets. Without this, virtualized controls like GridView and
			// TreeView stack all items at (0,0).
			TEST(UIManagerOrderTests, AbsoluteOverrideRectHonorsLeftTop)
			{
			    auto dev = MakeHeadlessDevice();
			    if (!dev)
			    {
			        GTEST_SKIP() << "Device init failed";
			    }
			    UiRgHarness rg(dev.get());
			
			    UIRegistration::RegisterBuiltInControls();
			
			    auto root = std::make_unique<UIElement>();
			    root->SetId("root");
			
			    // Container with two absolutely positioned children.
			    auto c1 = std::make_unique<UIElement>();
			    c1->SetId("c1");
			    UI::Layout::SetAbsolutePosition(*c1, Mathematics::Rect{10.0f, 20.0f, 30.0f, 40.0f});

			    auto c2 = std::make_unique<UIElement>();
			    c2->SetId("c2");
			    UI::Layout::SetAbsolutePosition(*c2, Mathematics::Rect{100.0f, 150.0f, 30.0f, 40.0f});
			
			    root->AddChild(std::move(c1));
			    root->AddChild(std::move(c2));
			
			    UIManager ui(dev.get());
			    ui.SetRoot(std::move(root));
			
			    // Give root a stable size.
			    Stylesheet sheet{};
			    const std::string css = R"(
			#root { width: 400px; height: 300px; position: relative; }
			)";
			    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
			    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));
			
			    ui.Update(0.0f, /*interactive=*/false);
			    DriveUiRender(ui, rg);
			
			    UIElement* r = ui.GetRootElement();
			    ASSERT_NE(r, nullptr);
			    UIElement* n1 = r->FindById("c1");
			    UIElement* n2 = r->FindById("c2");
			    ASSERT_NE(n1, nullptr);
			    ASSERT_NE(n2, nullptr);
			
			    EXPECT_NEAR(n1->GetLayoutX(), 10.0f, 0.5f);
			    EXPECT_NEAR(n1->GetLayoutY(), 20.0f, 0.5f);
			    EXPECT_NEAR(n2->GetLayoutX(), 100.0f, 0.5f);
			    EXPECT_NEAR(n2->GetLayoutY(), 150.0f, 0.5f);
			}

			TEST(UIManagerOrderTests, CssImport_OrderingAndManualInvalidation)
			{
			    auto dev = MakeHeadlessDevice();
			    if (!dev)
			    {
			        GTEST_SKIP() << "Device init failed";
			    }

			    // File-based style attach path exercises @import preprocessing without requiring Engine init.
			    UIManager ui(dev.get());

			    namespace fs = std::filesystem;
			    std::error_code ec;
			    fs::path dir = fs::temp_directory_path(ec);
			    if (ec)
			    {
			        GTEST_SKIP() << "No temp directory available";
			    }
			    dir /= "GameEngine_UiImportTests";
			    dir /= GUID::Generate().ToCompactString();
			    fs::create_directories(dir, ec);
			    ASSERT_FALSE(ec);

			    const fs::path bPath = dir / "b.css";
			    const fs::path aPath = dir / "a.css";

			    {
			        std::ofstream b(bPath);
			        ASSERT_TRUE(b.is_open());
			        b << "#e { color: #222222; }\n";
			    }
			    {
			        std::ofstream a(aPath);
			        ASSERT_TRUE(a.is_open());
			        a << "#e { color: #111111; }\n";
			        a << "@import \"b.css\";\n";
			    }

			    ASSERT_TRUE(ui.AttachStyleFromFile(aPath.string()));

			    std::vector<const Stylesheet*> sheets;
			    for (const auto& h : ui.GetStylesheets())
			    {
			        if (h)
			            sheets.push_back(h.get());
			    }
			    ASSERT_FALSE(sheets.empty());

			    ElementState st{};
			    UIElement el; el.SetId("e");
			    auto s1 = CSSParser::ComputeStyleFor(el, sheets, st, (const ResolvedStyle*)nullptr);
			    EXPECT_EQ(s1.Visual.Color, 0xFF222222u) << "@import should be expanded at its source location";

			    // Change the imported file and re-attach the master; expanded output should change.
			    {
			        std::ofstream b(bPath, std::ios::trunc);
			        ASSERT_TRUE(b.is_open());
			        b << "#e { color: #333333; }\n";
			    }

			    ASSERT_TRUE(ui.AttachStyleFromFile(aPath.string()));
			    auto s2 = CSSParser::ComputeStyleFor(el, sheets, st, (const ResolvedStyle*)nullptr);
			    EXPECT_EQ(s2.Visual.Color, 0xFF333333u);

			    // Cleanup best-effort.
			    fs::remove_all(dir, ec);
			}

// ---------------------------------------------------------------------------
// Cursor inheritance: CSS 'cursor' must survive the InheritProperties pass.
// Before the fix, hasCursor was never set to true so ResolveStyles always
// overwrote cursor with the parent's value (Auto).
// ---------------------------------------------------------------------------

TEST(UIManagerOrderTests, CSSCursorPropertySurvivesInheritance)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='parent'>
            <uielement id='child-pointer' />
            <uielement id='child-inherit' />
        </uielement>
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "ui_cursor_inherit.css";
    {
        std::ofstream f(css);
        f << R"(
#root   { width: 400px; height: 300px; }
#parent { width: 400px; height: 300px; cursor: crosshair; }
#child-pointer { width: 200px; height: 100px; cursor: pointer; }
#child-inherit { width: 200px; height: 100px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* parent = r->FindById("parent");
    UIElement* childPointer = r->FindById("child-pointer");
    UIElement* childInherit = r->FindById("child-inherit");
    ASSERT_TRUE(parent && childPointer && childInherit);

    EXPECT_EQ(parent->GetResolvedStyle().Visual.Cursor, CursorStyle::Crosshair)
        << "Parent should have crosshair cursor from CSS";
    EXPECT_EQ(childPointer->GetResolvedStyle().Visual.Cursor, CursorStyle::Pointer)
        << "Child with explicit cursor should keep pointer, not inherit parent's crosshair";
    EXPECT_EQ(childInherit->GetResolvedStyle().Visual.Cursor, CursorStyle::Crosshair)
        << "Child without cursor should inherit parent's crosshair";

    std::filesystem::remove(css);
}

// ---------------------------------------------------------------------------
// Splitter: verify GetHitTestBounds returns an expanded area beyond layout.
// ---------------------------------------------------------------------------

TEST(UIManagerOrderTests, SplitterHitTestBoundsAreExpandedBeyondLayout)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <pane id='left' weight='0.5' />
        <splitter id='split' class='splitter row' />
        <pane id='right' weight='0.5' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "ui_splitter_hit.css";
    {
        std::ofstream f(css);
        f << R"(
#root { width: 600px; height: 400px; flex-direction: row; }
#left, #right { flex-grow: 1; }
.splitter.row {
    width: 1px;
    cursor: col-resize;
    position: relative;
    z-index: 100;
    pointer-events: auto;
}
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* splitter = r->FindById("split");
    ASSERT_NE(splitter, nullptr);

    float layoutW = splitter->GetLayoutWidth();
    EXPECT_LE(layoutW, 2.0f) << "Splitter layout width should be small (1px CSS)";

    float hitX, hitY, hitW, hitH;
    splitter->GetHitTestBounds(hitX, hitY, hitW, hitH);
    EXPECT_GT(hitW, layoutW) << "Splitter hit width must be larger than layout width";
    EXPECT_GE(hitW, 6.0f) << "Splitter hit width should be at least 6px";

    std::filesystem::remove(css);
}

// ---------------------------------------------------------------------------
// Integration: cursor callback fires with correct style when hovering a
// splitter's expanded hit area.
// ---------------------------------------------------------------------------

TEST(UIManagerOrderTests, SplitterHoverSetsCursorViaCallback)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <pane id='left' weight='0.5' />
        <splitter id='split' class='splitter row' />
        <pane id='right' weight='0.5' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    CursorStyle lastCursor = CursorStyle::Auto;
    ui.SetCursorCallback([&](CursorStyle c) { lastCursor = c; });

    const auto css = std::filesystem::temp_directory_path() / "ui_splitter_cursor.css";
    {
        std::ofstream f(css);
        f << R"(
#root { width: 600px; height: 400px; flex-direction: row; }
#left, #right { flex-grow: 1; }
.splitter.row {
    width: 1px;
    cursor: col-resize;
    position: relative;
    z-index: 100;
    pointer-events: auto;
}
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* splitter = r->FindById("split");
    ASSERT_NE(splitter, nullptr);

    float splitCenterX = splitter->GetLayoutX() + splitter->GetLayoutWidth() * 0.5f;
    float splitCenterY = splitter->GetLayoutY() + splitter->GetLayoutHeight() * 0.5f;

    ui.OnMouseMove(splitCenterX, splitCenterY);
    ui.Update(0.0f, true);
    DriveUiRender(ui, rg);

    UIElement* hovered = ui.GetHoveredElement();
    EXPECT_EQ(hovered, splitter)
        << "Hovering the splitter center should make it the hovered element";
    EXPECT_EQ(lastCursor, CursorStyle::ColResize)
        << "Cursor callback should receive ColResize when hovering a .splitter.row";

    ui.OnMouseMove(10.0f, splitCenterY);
    ui.Update(0.0f, true);
    DriveUiRender(ui, rg);

    EXPECT_EQ(lastCursor, CursorStyle::Auto)
        << "Moving away from splitter should reset cursor to Auto";

    std::filesystem::remove(css);
}

// The two halves of the Handled rule, executed through the REAL parent-ward loop rather
// than asserted on a returned flag: a press on a Button whose OnEvent consumes it must
// reach BOTH of the button's own subscribers, and must still not reach the ancestor.
//
// Unit-level dispatch tests cannot cover the second half — they call DispatchEvent on one
// element and read e.Handled afterwards, which is the level the manager consults but not
// the manager consulting it. This drives real pointer input through UIManager so the
// same-element fan-out and the ancestor boundary are observed in one run.
TEST(UIManagerMountTests, SameElementFanoutDoesNotLeakIntoBubbling)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    UIElement* rootRaw = root.get();

    auto btnOwned = std::make_unique<Button>();
    btnOwned->SetId("btn");
    Button* btn = btnOwned.get();
    root->AddChild(std::move(btnOwned));

    int parentDowns = 0;
    int firstDown = 0;
    int secondDown = 0;
    rootRaw->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++parentDowns; });
    btn->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++firstDown; });
    btn->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++secondDown; });

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_bubble_boundary_fanout.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 200px; height: 40px; }
#btn { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    const float bx = btn->GetLayoutX();
    const float by = btn->GetLayoutY();
    ui.OnMouseMove(bx + 5, by + 5);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);

    EXPECT_EQ(firstDown, 1) << "first same-element subscriber did not run";
    EXPECT_EQ(secondDown, 1)
        << "second same-element subscriber starved by Button::OnEvent consuming the press";
    EXPECT_EQ(parentDowns, 0)
        << "the same-element fan-out leaked into parent-ward bubbling: the control consumed the "
           "press, so the ancestor must not see it";

    std::filesystem::remove(css);
}
