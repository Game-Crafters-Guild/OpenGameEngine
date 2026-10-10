// Routing policy of WindowInputRouter, driven without a live window: the
// Route* entry points take the config, so a test supplies a real UIManager and
// InputSystem and asserts where each event lands. The handlers those entry
// points back are one-line forwarders bound in BindBasicHandlers.
#include <gtest/gtest.h>

#include "Core/WindowInputRouter.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/VectorPrinting.h"
#include "Platform/Window.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Interaction/FocusIsInside.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace GameEngine;

namespace
{
struct RouterFixture
{
    std::unique_ptr<Rendering::IDevice> device;
    std::unique_ptr<UIManager> ui;
    Input::InputSystem input;
    WindowInputRouterConfig config;

    bool Init()
    {
        device = MakeHeadlessDevice();
        if (!device)
            return false;
        ui = std::make_unique<UIManager>(device.get());
        config.getUi = [this]() { return ui.get(); };
        config.getInput = [this]() { return &input; };
        return true;
    }

    // The play surface as the editor resolves it while a Game View tab is live:
    // one answer carrying every game-side leg, so no row can set them apart.
    void UsePlaySurface(WindowInputRouterConfig::PlaySurface surface)
    {
        config.getPlaySurface = [surface]() { return surface; };
    }

    // A surface with gameplay alone — no HUD documents, no viewport offset. That
    // is the Player's shape and the editor's before a game UI exists.
    void UseGameplaySink(Input::IRawInputSink& sink)
    {
        WindowInputRouterConfig::PlaySurface surface;
        surface.gameplaySink = &sink;
        UsePlaySurface(std::move(surface));
    }
};

// InputSystem collapses a stream into held/edge state, which cannot answer "did
// every event arrive, in order". This records the raw sequence instead, so a
// dropped or reordered event is visible rather than absorbed. It claims nothing,
// so the chain keeps propagating past it.
struct RecordingSink final : Input::IRawInputSink
{
    std::vector<std::string> events;

    bool OnKey(int key, int action, int /*mods*/) override
    {
        events.push_back("k" + std::to_string(key) + ":" + std::to_string(action));
        return false;
    }
    void OnChar(unsigned int codepoint) override
    {
        events.push_back("c" + std::to_string(codepoint));
    }
    void OnMouseMove(Mathematics::Vector2) override {}
    void OnMouseLeave() override {}
    void OnWindowFocus(bool focused) override { events.push_back(focused ? "f1" : "f0"); }
    bool OnMouseButton(int button, bool down, int /*mods*/) override
    {
        events.push_back("b" + std::to_string(button) + ":" + (down ? "1" : "0"));
        return false;
    }
    bool OnMouseScroll(Mathematics::Vector2 delta, int /*mods*/) override
    {
        events.push_back("s" + std::to_string(static_cast<int>(delta.x)) + ":" +
                         std::to_string(static_cast<int>(delta.y)));
        return false;
    }
    void OnGamepadState(int gamepadIndex, const float* axes, int axisCount, bool connected) override
    {
        const float leftX = (axes && axisCount > 0) ? axes[0] : 0.0f;
        events.push_back("gs" + std::to_string(gamepadIndex) + ":" + (connected ? "1" : "0") + ":" +
                         std::to_string(static_cast<int>(leftX * 100.0f)));
    }
    bool OnGamepadButton(int gamepadIndex, int button, bool down) override
    {
        events.push_back("g" + std::to_string(gamepadIndex) + ":" + std::to_string(button) + ":" +
                         (down ? "1" : "0"));
        return false;
    }
};

constexpr Input::ContextId kGameplayContext = Input::HashInput("Test.Gameplay");
constexpr Input::ActionId kGameplayAction = Input::HashInput("Test.Gameplay.Action");

// A game declares the keys it uses the way a real one does: action bindings in an
// enabled context. Every key bound here is claimed by that sink and stops there.
void ClaimByBinding(Input::InputSystem& sink, Input::KeyCode key)
{
    sink.BindKey(kGameplayContext, kGameplayAction, {Input::DeviceType::Keyboard, key, 1.0f});
    sink.PushContext(kGameplayContext);
}

// The other claim source: poll-style game code that reads a key every frame and
// registers nothing. The read happens on the frame's tick, so the claim is in
// place for the events of the frames that follow.
void ClaimByPolling(Input::InputSystem& sink, Input::KeyCode key)
{
    (void)sink.IsKeyDown(key);
}

// A root with one Button laid out at a known rect, used by the pointer rows.
// Returns the button, with the cursor already parked over it and hover resolved,
// so a routed press lands on a control that acts.
Button* AttachHoveredButton(RouterFixture& f, const char* cssName)
{
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto btnOwned = std::make_unique<Button>();
    Button* btn = btnOwned.get();
    btn->SetId("btn");
    root->AddChild(std::move(btnOwned));
    f.ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / cssName;
    {
        std::ofstream out(css);
        out << R"(
#root { display: flex; width: 200px; height: 40px; }
#btn { width: 100px; height: 20px; }
)";
    }
    if (!f.ui->AttachStyleFromFile(css.string()))
        return nullptr;

    f.ui->Update(0.0f, /*interactive=*/false);
    f.ui->OnMouseMove(btn->GetLayoutX() + 5.0f, btn->GetLayoutY() + 5.0f);
    f.ui->Update(0.0f, /*interactive=*/true);
    return btn;
}

} // namespace

TEST(WindowInputRoutingTests, FocusLossClearsUiModifiersAndReleasesHeldKeys)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftShift, Input::kKeyActionPress, Input::kModShift);
    f.input.Update(0.016f);
    EXPECT_EQ(f.ui->GetModifierKeys(), Input::kModShift);
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_LeftShift));

    WindowInputRouter::RouteFocusChange(f.config, /*focused=*/false);
    f.input.Update(0.016f);

    EXPECT_EQ(f.ui->GetModifierKeys(), 0);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_LeftShift));
}

TEST(WindowInputRoutingTests, FocusGainLeavesHeldStateAlone)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftControl, Input::kKeyActionPress, Input::kModControl);
    WindowInputRouter::RouteFocusChange(f.config, /*focused=*/true);

    EXPECT_EQ(f.ui->GetModifierKeys(), Input::kModControl);
}

TEST(WindowInputRoutingTests, ClickAdoptsTheModifierMaskItCarries)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // No key event ever reports a modifier held from before this window took
    // focus, so the click's mask has to reach the UI ahead of the button.
    EXPECT_EQ(f.ui->GetModifierKeys(), 0);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, Input::kModShift);

    EXPECT_EQ(f.ui->GetModifierKeys(), Input::kModShift);
}

TEST(WindowInputRoutingTests, WheelReconcilesModifierMaskBeforeDispatch)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // A click can be the first event to reveal a modifier that was held before
    // focus. A wheel while it remains physically held must retain that state,
    // even though this window never received the corresponding key press.
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, Input::kModControl);
    EXPECT_NE(f.ui->GetModifierKeys() & Input::kModControl, 0);

    WindowInputRouter::RouteScroll(
        f.config, /*dx=*/0.0f, /*dy=*/1.0f, /*mods=*/Input::kModControl);
    EXPECT_NE(f.ui->GetModifierKeys() & Input::kModControl, 0);

    // Once the physical key is released, a plain wheel clears the mask-derived
    // state instead of being mistaken for an Asset Browser zoom.
    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/1.0f, /*mods=*/0);

    EXPECT_EQ(f.ui->GetModifierKeys() & Input::kModControl, 0);
}

TEST(WindowInputRoutingTests, WheelLiveMaskReleasesAModifierWhoseKeyUpWasLost)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The window saw the Cmd press but its release went to the OS (Spotlight,
    // an app switch without a focus change). The live mask on the next wheel
    // says Cmd is up, so the wheel must not become the Cmd + wheel item resize gesture.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftSuper, Input::kKeyActionPress, Input::kModSuper);
    EXPECT_NE(f.ui->GetModifierKeys() & Input::kModSuper, 0);

    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/1.0f, /*mods=*/0);
    EXPECT_EQ(f.ui->GetModifierKeys(), 0);

    // A wheel while the key is genuinely held keeps it held.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftSuper, Input::kKeyActionPress, Input::kModSuper);
    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/1.0f, /*mods=*/Input::kModSuper);
    EXPECT_NE(f.ui->GetModifierKeys() & Input::kModSuper, 0);
}

TEST(WindowInputRoutingTests, ConsumedPressIsWithheldFromInputButReleaseAlwaysLands)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // A focused text field consumes ordinary key presses.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("field");

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_A, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_A));

    // The release is never consumed, so the input system always observes it and
    // cannot be left believing a key is still down.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_A, Input::kKeyActionRelease, 0);
    f.input.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_A));
}

TEST(WindowInputRoutingTests, AClaimedPressStopsAtTheGameplaySink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // A game that binds Escape is the reason Escape does not reach the editor's
    // Discard Change Review: the game claims it, not a policy that hides it.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_Escape);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Escape, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_Escape));
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_Escape));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Escape, Input::kKeyActionRelease, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_Escape));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_Escape));
}

TEST(WindowInputRoutingTests, AnUnclaimedPressFallsThroughToTheEditorInput)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The half that used to be impossible: the game binds W and nothing else, so
    // E is nobody's on the game side and keeps travelling to editor behaviour
    // even mid-play. The sink records nothing it declined.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_E, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_E));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_E));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_W));
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W));
}

TEST(WindowInputRoutingTests, EdgeQueriesAgreeWithTheChain)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Delivery is consumption: the sink's per-frame edge queries hold exactly the
    // presses that stopped at it. A press it declined — one the editor went on to
    // see — can never surface later as a WasKeyPressed edge.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_E, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);

    EXPECT_TRUE(runtime.WasKeyPressed(Input::kKeyCode_W));
    EXPECT_FALSE(f.input.WasKeyPressed(Input::kKeyCode_W));
    EXPECT_FALSE(runtime.WasKeyPressed(Input::kKeyCode_E));
    EXPECT_TRUE(f.input.WasKeyPressed(Input::kKeyCode_E));
}

TEST(WindowInputRoutingTests, APolledKeyIsClaimedFromTheFirstTick)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Poll-style game code registers nothing, so bindings cannot see it. Reading
    // the key is the claim, and a script polls on every tick — including the tick
    // before a human can have pressed anything.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);

    runtime.Update(0.016f);
    ClaimByPolling(runtime, Input::kKeyCode_Space);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Space, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_Space));
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_Space));
}

TEST(WindowInputRoutingTests, TheFirstPressBeforeAnyPollReachesTheEditorOnce)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The stated cost of poll-derived claims: a key read only inside a branch
    // that has not run yet is claimed by nobody, so that first press propagates
    // to editor behaviour once. It corrects itself as soon as the branch polls.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Space, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_Space));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_Space));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Space, Input::kKeyActionRelease, 0);
    ClaimByPolling(runtime, Input::kKeyCode_Space);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Space, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_Space));
}

TEST(WindowInputRoutingTests, UnclaimedModifierPressStillReachesEditorInput)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);

    // Shift is deliberately not an editor-shortcut modifier, so it is offered to
    // the game like any other key. This game does not claim it, so it lands on the
    // editor InputSystem — where Scene View "Move Faster" reads it.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftShift, Input::kKeyActionPress, Input::kModShift);
    f.input.Update(0.016f);
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_LeftShift));

    // A game that binds Shift for sprinting takes it, and the editor's own
    // Move Faster stops firing while it plays. The other Shift is used so this
    // half cannot read the held state the first half established.
    Input::InputSystem sprinting{Input::SinkRole::Gameplay};
    ClaimByBinding(sprinting, Input::kKeyCode_RightShift);
    f.UseGameplaySink(sprinting);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_RightShift, Input::kKeyActionPress, Input::kModShift);
    f.input.Update(0.016f);
    sprinting.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_RightShift));
    EXPECT_TRUE(sprinting.IsKeyDown(Input::kKeyCode_RightShift));
}

namespace
{
// A panel that IS the behaviour for Ctrl+S (a local Save, the Script Editor /
// Animation window shape): it acts, and says so. It gates on the same shared
// focus scope the real panels use — the key is offered to a hovered panel too,
// and a save there would eat the editor's binding from under the pointer.
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

constexpr Input::ContextId kEditorLikeContext = Input::HashInput("Test.EditorGlobal");
constexpr Input::ActionId kSaveSceneLikeAction = Input::HashInput("Test.EditorGlobal.Save");
constexpr Input::ActionId kCopyLikeAction = Input::HashInput("Test.EditorGlobal.Copy");
constexpr Input::ActionId kFrameAllLikeAction = Input::HashInput("Test.EditorGlobal.FrameAll");

// The editor's global chords, registered the way EditorApplication registers
// them: Ctrl+S (Save Scene), Ctrl+C (Copy) and Alt+F (Frame All).
void RegisterEditorChords(Input::InputSystem& input)
{
    Input::ActionBinding save{Input::DeviceType::Keyboard, Input::kKeyCode_S, 1.0f};
    save.requiredMods = Input::kModControl;
    input.BindKey(kEditorLikeContext, kSaveSceneLikeAction, save);

    Input::ActionBinding copy{Input::DeviceType::Keyboard, Input::kKeyCode_C, 1.0f};
    copy.requiredMods = Input::kModControl;
    input.BindKey(kEditorLikeContext, kCopyLikeAction, copy);

    Input::ActionBinding frameAll{Input::DeviceType::Keyboard, Input::kKeyCode_F, 1.0f};
    frameAll.requiredMods = Input::kModAlt;
    input.BindKey(kEditorLikeContext, kFrameAllLikeAction, frameAll);

    input.PushContext(kEditorLikeContext);
}
} // namespace

// The Script Editor / Animation shape: the focused panel IS the behaviour for
// the chord, so the editor action bound to the same keystroke must not also
// fire. One keystroke, one save.
TEST(WindowInputRoutingTests, APanelThatHandlesAChordStopsTheEditorAction)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    RegisterEditorChords(f.input);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto panelOwned = std::make_unique<SavingPanel>();
    SavingPanel* panel = panelOwned.get();
    panel->SetId("panel");
    root->AddChild(std::move(panelOwned));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("panel");

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_S, Input::kKeyActionPress, Input::kModControl);
    f.input.Update(0.016f);
    EXPECT_EQ(panel->SaveCount, 1);
    EXPECT_FALSE(f.input.WasActionTriggered(kSaveSceneLikeAction));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_S, Input::kKeyActionRelease, Input::kModControl);
    f.input.Update(0.016f);

    // The control arm, and the one that fails if the dispatcher ever forwards a
    // registered chord past the handled bit again: with the panel declining, the
    // same keystroke must reach the editor action.
    panel->HandlesTheChord = false;
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_S, Input::kKeyActionPress, Input::kModControl);
    f.input.Update(0.016f);
    EXPECT_EQ(panel->SaveCount, 1);
    EXPECT_TRUE(f.input.WasActionTriggered(kSaveSceneLikeAction));
}

// The other half of that rule, and the live defect it comes from: the pointer
// merely RESTS over the panel while focus is elsewhere. UIManager offers the
// key to the hovered element after the focused one declines, so a panel that
// checks only the chord saves — and its Stop() swallows Save Scene — without
// the user ever having put focus in it.
TEST(WindowInputRoutingTests, AHoveredPanelWithoutFocusLeavesTheChordToTheEditor)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    RegisterEditorChords(f.input);

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
    f.ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "router_hovered_panel.css";
    {
        std::ofstream out(css);
        out << R"(
#root { display: flex; width: 300px; height: 40px; }
#panel { width: 100px; height: 20px; }
#field { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(f.ui->AttachStyleFromFile(css.string()));

    f.ui->Update(0.0f, /*interactive=*/false);
    ASSERT_GT(panel->GetLayoutWidth(), 0.0f) << "panel must have a box to hover";
    f.ui->OnMouseMove(panel->GetLayoutX() + 5.0f, panel->GetLayoutY() + 5.0f);
    f.ui->Update(0.0f, /*interactive=*/true);
    for (UIElement* p = f.ui->GetHoveredElement();; p = p->GetParent())
    {
        ASSERT_NE(p, nullptr) << "pointer must be over the panel";
        if (p == panel)
            break;
    }

    // Focus lives in an unrelated field — the state left behind by clicking a
    // Hierarchy row before reaching for Save.
    f.ui->SetFocusById("field");

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_S, Input::kKeyActionPress, Input::kModControl);
    f.input.Update(0.016f);
    EXPECT_EQ(panel->SaveCount, 0) << "hovered is not focused: the panel must not save";
    EXPECT_TRUE(f.input.WasActionTriggered(kSaveSceneLikeAction)) << "Save Scene must still fire";
}

// The regression the old over-consumption + forwarding pair was hiding: an
// ordinary text field (entity rename, a search box) does not implement Save, so
// Ctrl+S must bubble out of it and reach the editor — while Ctrl+C, which the
// field DOES implement, stays with the field.
TEST(WindowInputRoutingTests, AFocusedTextFieldBubblesChordsItDoesNotImplement)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    RegisterEditorChords(f.input);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("field");

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_S, Input::kKeyActionPress, Input::kModControl);
    f.input.Update(0.016f);
    EXPECT_TRUE(f.input.WasActionTriggered(kSaveSceneLikeAction));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_S, Input::kKeyActionRelease, Input::kModControl);
    f.input.Update(0.016f);

    // Copy is the field's own gesture: it acts, so the editor's Copy must not
    // also fire on the same keystroke.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_C, Input::kKeyActionPress, Input::kModControl);
    f.input.Update(0.016f);
    EXPECT_FALSE(f.input.WasActionTriggered(kCopyLikeAction));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_C, Input::kKeyActionRelease, Input::kModControl);
    f.input.Update(0.016f);

    // Alt splits by platform (Input::ComposesTextInput). On macOS Option+F
    // composes a character, which IS the field's text, so the keystroke stays
    // with it; everywhere else Alt composes nothing, so Alt+F is a chord the
    // field cannot act on and the editor's Frame All must fire with the caret
    // sitting in the field.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_F, Input::kKeyActionPress, Input::kModAlt);
    f.input.Update(0.016f);
#if defined(__APPLE__)
    EXPECT_FALSE(f.input.WasActionTriggered(kFrameAllLikeAction));
#else
    EXPECT_TRUE(f.input.WasActionTriggered(kFrameAllLikeAction));
#endif
}

TEST(WindowInputRoutingTests, WindowWithoutARuntimeSinkKeepsHeldEditorActions)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The Scene View flight shape: an editor action held on an unmodified key.
    const Input::ContextId context = Input::HashInput("Test.SceneViewContext");
    Input::ActionDesc moveForward{};
    moveForward.id = Input::HashInput("Test.SceneView.MoveForward");
    moveForward.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_W, 1.0f});
    f.input.RegisterAction(context, moveForward);
    f.input.PushContext(context);

    // A window that is not showing the Game View resolves no runtime sink:
    // EditorApplication::GetPlaySurface matches the event's own window,
    // so a torn-off Game View cannot withhold keys typed into the main window.
    f.config.getPlaySurface = []() { return WindowInputRouterConfig::PlaySurface{}; };
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    EXPECT_TRUE(f.input.IsActionActive(moveForward.id));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionRelease, 0);
    f.input.Update(0.016f);
    ASSERT_FALSE(f.input.IsActionActive(moveForward.id));

    // The window that does show it offers the press to a game that binds W, which
    // takes it, and the editor action goes inactive — the deliberate behaviour
    // this scoping confines to that window.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    EXPECT_FALSE(f.input.IsActionActive(moveForward.id));
}

TEST(WindowInputRoutingTests, FocusedElementDoesNotStarveTheRuntimeSink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // UIManager reports a press as consumed whenever any element holds focus,
    // and play parks focus on the Game View viewport itself. Gating the runtime
    // forward on that flag would leave gameplay with no keys at all, so this
    // pins that a focused element does not stop the game receiving them.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto viewport = std::make_unique<UIElement>();
    viewport->SetId("GameViewViewport");
    root->AddChild(std::move(viewport));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("GameViewViewport");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_W));
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W));
}

TEST(WindowInputRoutingTests, FocusedTextFieldWithholdsGameplayPressFromTheRuntimeSink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Typing into an Inspector field while playing must not also walk the
    // character. The field is the element that turns the keystroke into an edit,
    // so it reaches neither editor actions nor the game.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("field");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_W));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_W));
}

TEST(WindowInputRoutingTests, ReleaseReachesTheRuntimeAfterAFieldTakesFocusMidHold)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto viewport = std::make_unique<UIElement>();
    viewport->SetId("GameViewViewport");
    root->AddChild(std::move(viewport));
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("GameViewViewport");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    ASSERT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W));

    // A field takes focus while the key is still physically held. The release is
    // never withheld, so the game cannot be left believing W is still down.
    f.ui->SetFocusById("field");
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionRelease, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_W));
}

TEST(WindowInputRoutingTests, FocusedNonTextControlDoesNotWithholdGameplayKeys)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // A Toggle acts on Space and on nothing else, so W is not its key: the game
    // keeps moving while a checkbox happens to hold focus.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto toggle = std::make_unique<Toggle>();
    toggle->SetId("toggle");
    root->AddChild(std::move(toggle));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("toggle");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W));
}

TEST(WindowInputRoutingTests, AHoveredUnfocusedTextFieldDoesNotStarveTheRuntimeSink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The live symptom this pins: the pointer rests over an Inspector field
    // while the game plays. Nothing is focused, so no character will ever reach
    // that field — and a key it cannot act on must not be taken from the game.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto fieldOwned = std::make_unique<TextField>();
    auto* field = fieldOwned.get();
    field->SetId("field");
    root->AddChild(std::move(fieldOwned));
    f.ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "router_hovered_field.css";
    {
        std::ofstream out(css);
        out << R"(
#root { display: flex; width: 200px; height: 40px; }
#field { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(f.ui->AttachStyleFromFile(css.string()));

    // Layout only: this binary has no shader path resolver, so no render is
    // driven here. Hover is decided by the interactive Update's hit test.
    f.ui->Update(0.0f, /*interactive=*/false);
    ASSERT_GT(field->GetLayoutWidth(), 0.0f) << "field must have a box to hover";
    f.ui->OnMouseMove(field->GetLayoutX() + 2.0f, field->GetLayoutY() + 2.0f);
    f.ui->Update(0.0f, /*interactive=*/true);
    // The hovered element is the field's inner editor, which is exactly the
    // element that used to claim the key: a typed field embeds a TextInput and
    // that child is what the pointer actually lands on.
    UIElement* hovered = f.ui->GetHoveredElement();
    ASSERT_NE(hovered, nullptr) << "pointer must be over the field";
    for (UIElement* p = hovered; ; p = p->GetParent())
    {
        ASSERT_NE(p, nullptr) << "hover landed outside the field";
        if (p == field)
            break;
    }
    ASSERT_TRUE(f.ui->GetFocusedElementId().empty()) << "hovering must not focus";

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W));

    // Focus lands on the same field the pointer never left, and the next press
    // is genuinely the field's.
    f.ui->SetFocusById("field");
    Input::InputSystem focusedRuntime{Input::SinkRole::Gameplay};
    ClaimByBinding(focusedRuntime, Input::kKeyCode_E);
    f.UseGameplaySink(focusedRuntime);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_E, Input::kKeyActionPress, 0);
    focusedRuntime.Update(0.016f);
    EXPECT_FALSE(focusedRuntime.IsKeyDown(Input::kKeyCode_E));
}

TEST(WindowInputRoutingTests, AConsumedReleaseStillReachesTheRuntimeSink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Consumption withholds presses, never releases. An element that consumes
    // key-ups would otherwise strand the key down in the game forever: the
    // press reached the sink, and the release that ends it never would.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto panel = std::make_unique<UIElement>();
    panel->SetId("panel");
    panel->RegisterEventHandler(kEventKeyUp, [](UIEvent& e) { e.Stop(); });
    root->AddChild(std::move(panel));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("panel");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    ASSERT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W)) << "precondition: the press reached the sink";

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionRelease, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_W)) << "a consumed release must still land";
}

TEST(WindowInputRoutingTests, TabThatMovesFocusIsConsumedByTheFocusMachinery)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Tab did something — it advanced focus — so it is consumed and stops there.
    // Consumption is an outcome, not a prediction, which is why this no longer
    // depends on when the router samples anything.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto toggle = std::make_unique<Toggle>();
    toggle->SetId("toggle");
    root->AddChild(std::move(toggle));
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("toggle");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_Tab);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Tab, Input::kKeyActionPress, 0);
    ASSERT_EQ(f.ui->GetFocusedElementId(), "field") << "focus really moved";

    runtime.Update(0.016f);
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_Tab));

    // The release is never withheld, so nothing is left holding Tab down.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Tab, Input::kKeyActionRelease, 0);
    runtime.Update(0.016f);
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_Tab));
}

TEST(WindowInputRoutingTests, TabWithNothingToFocusReachesTheRuntimeSink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The other half of the same rule: with no focusable to advance to, Tab
    // moved nothing, so the focus machinery does not claim it and the game gets
    // its key.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto viewport = std::make_unique<UIElement>();
    viewport->SetId("GameViewViewport");
    root->AddChild(std::move(viewport));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("GameViewViewport");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_Tab);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Tab, Input::kKeyActionPress, 0);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_Tab));
}

TEST(WindowInputRoutingTests, RapidSubFrameKeyStreamReachesTheSinkIntactAndInOrder)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Keys dispatch at their platform callback rather than being queued for the
    // next UIManager::Update, so a burst arriving between two frames cannot be
    // coalesced, reordered or dropped. Nothing here calls Update at all: every
    // event still has to land.
    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    std::vector<std::string> expected;
    for (int i = 0; i < 8; ++i)
    {
        const int key = Input::kKeyCode_A + i;
        WindowInputRouter::RouteKey(f.config, key, Input::kKeyActionPress, 0);
        WindowInputRouter::RouteKey(f.config, key, Input::kKeyActionRelease, 0);
        expected.push_back("k" + std::to_string(key) + ":" + std::to_string(Input::kKeyActionPress));
        expected.push_back("k" + std::to_string(key) + ":" + std::to_string(Input::kKeyActionRelease));
    }

    EXPECT_EQ(runtime.events, expected);
}

TEST(WindowInputRoutingTests, ConsumedAndUnconsumedKeysInterleaveWithoutLoss)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // A withheld press must not cost its release, and a focus change mid-stream
    // must not strand anything: every event is accounted for on exactly one side.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    auto viewport = std::make_unique<UIElement>();
    viewport->SetId("GameViewViewport");
    root->AddChild(std::move(viewport));
    f.ui->SetRoot(std::move(root));

    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    const std::string pressW = "k" + std::to_string(Input::kKeyCode_W) + ":" + std::to_string(Input::kKeyActionPress);
    const std::string releaseW =
        "k" + std::to_string(Input::kKeyCode_W) + ":" + std::to_string(Input::kKeyActionRelease);

    // Typed into the field: the press is the field's, the release still travels.
    f.ui->SetFocusById("field");
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionRelease, 0);

    // Same key, focus now parked on the viewport: both halves reach the game.
    f.ui->SetFocusById("GameViewViewport");
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionRelease, 0);

    EXPECT_EQ(runtime.events, (std::vector<std::string>{releaseW, pressW, releaseW}));
}

TEST(WindowInputRoutingTests, FocusedSliderConsumesItsAxisKeyButNotGameplayKeys)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The slider acts on its own axis, so that arrow is not the game's. W is not
    // the slider's, so the character still walks while a slider holds focus.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto slider = std::make_unique<Slider>();
    slider->SetId("slider");
    root->AddChild(std::move(slider));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("slider");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_Right);
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Right, Input::kKeyActionPress, 0);
    runtime.Update(0.016f);
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_Right));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W));
}

TEST(WindowInputRoutingTests, TypedCharacterDoesNotReachTheRuntimeSink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The key half is withheld by RouteKey; this is the character half of the
    // same keystroke, which previously reached the game unconditionally.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    f.ui->SetRoot(std::move(root));

    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    f.ui->SetFocusById("field");
    WindowInputRouter::RouteChar(f.config, 'w');
    EXPECT_TRUE(runtime.events.empty()) << "a typed character must not also reach the game";

    // With focus off any text control the character is nobody's, so it travels.
    f.ui->ClearFocus();
    WindowInputRouter::RouteChar(f.config, 'w');
    EXPECT_EQ(runtime.events, (std::vector<std::string>{"c" + std::to_string('w')}));
}

namespace
{
constexpr Input::ContextId kHostContext = Input::HashInput("Test.Host");
constexpr Input::ActionId kHostShortcut = Input::HashInput("Test.Host.Shortcut");

// A host shortcut the way the editor registers one: an action flagged
// hostReserved in the host's own InputSystem.
void ReserveForHost(Input::InputSystem& host, Input::KeyCode key, int mods)
{
    Input::ActionBinding binding{Input::DeviceType::Keyboard, key, 1.0f};
    binding.requiredMods = mods;
    host.RegisterAction(kHostContext, {kHostShortcut, {binding}, false, /*hostReserved=*/true});
}

WindowInputRouterConfig::PlaySurface GameplayOnly(Input::IRawInputSink& sink)
{
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameplaySink = &sink;
    return surface;
}
} // namespace

// A chord the host does not reserve is the game's like any other key, whether
// the game claims it by binding the chord or by polling its key code, and
// whichever modifier makes the chord.
TEST(WindowInputRoutingTests, UnreservedChordsReachTheGameplaySink)
{
    struct Modifier
    {
        int Mods;
        int Key;
    };
    const Modifier modifiers[] = {{Input::kModShift, Input::kKeyCode_LeftShift},
                                  {Input::kModControl, Input::kKeyCode_LeftControl},
                                  {Input::kModAlt, Input::kKeyCode_LeftAlt},
                                  {Input::kModSuper, Input::kKeyCode_LeftSuper}};
    for (const auto& [mods, modifierKey] : modifiers)
    {
        for (const bool claimByPolling : {false, true})
        {
            SCOPED_TRACE(testing::Message() << "mods=" << mods << " polling=" << claimByPolling);
            Input::InputSystem application;
            ReserveForHost(application, Input::kKeyCode_P, Input::kModControl);
            Input::InputSystem runtime{Input::SinkRole::Gameplay};
            if (claimByPolling)
            {
                runtime.IsKeyDown(Input::kKeyCode_A);
            }
            else
            {
                Input::ActionBinding binding{Input::DeviceType::Keyboard, Input::kKeyCode_A, 1.0f};
                binding.requiredMods = mods;
                runtime.BindKey(kGameplayContext, kGameplayAction, binding);
            }
            WindowInputRouterConfig config;
            config.getInput = [&] { return &application; };
            config.getPlaySurface = [&] { return GameplayOnly(runtime); };

            // The complete chord can arrive between frames, including the
            // modifier's release.
            WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionPress, mods);
            WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionRelease, mods);
            WindowInputRouter::RouteKey(config, modifierKey, Input::kKeyActionRelease, 0);
            application.Update(0.016f);
            runtime.Update(0.016f);
            if (claimByPolling)
                EXPECT_TRUE(runtime.WasKeyPressed(Input::kKeyCode_A));
            else
                EXPECT_TRUE(runtime.WasActionTriggered(kGameplayAction));
            EXPECT_FALSE(application.WasKeyPressed(Input::kKeyCode_A));
            EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_A));
        }
    }
}

TEST(WindowInputRoutingTests, HostReservedChordsSkipTheGameplaySink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The host's reservation wins even though the game claims the key.
    ReserveForHost(f.input, Input::kKeyCode_P, Input::kModControl);
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_P);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_P, Input::kKeyActionPress, Input::kModControl);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(f.input.WasActionTriggered(kHostShortcut));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_P));

    // The release carries no such rule: it reaches every sink, where a key the
    // sink never saw go down is a no-op.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_P, Input::kKeyActionRelease, Input::kModControl);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_P));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_P));
}

// A focused field that acts on the chord keeps it from the host's reservation
// and from the game; only the release travels on.
TEST(WindowInputRoutingTests, AChordTheUiActsOnReachesNeitherTheHostNorTheGame)
{
    UIManager ui{nullptr};
    auto root = std::make_unique<UIElement>();
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    ui.SetRoot(std::move(root));
    ui.SetFocusById("field");
    Input::InputSystem application;
    ReserveForHost(application, Input::kKeyCode_A, Input::kModControl);
    RecordingSink runtime;
    WindowInputRouterConfig config;
    config.getUi = [&] { return &ui; };
    config.getInput = [&] { return &application; };
    config.getPlaySurface = [&] { return GameplayOnly(runtime); };

    WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionPress, Input::kModControl);
    EXPECT_TRUE(runtime.events.empty());
    application.Update(0.016f);
    EXPECT_FALSE(application.WasActionTriggered(kHostShortcut));
    WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionRelease, 0);
    EXPECT_EQ(runtime.events, (std::vector<std::string>{"k65:0"}));
}

TEST(WindowInputRoutingTests, HostReservationGatesRepeatsButNeverStrandsAHeldGameplayKey)
{
    Input::InputSystem application;
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_A);
    WindowInputRouterConfig config;
    config.getInput = [&] { return &application; };
    config.getPlaySurface = [&] { return GameplayOnly(runtime); };
    WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionPress, Input::kModControl);
    runtime.Update(0.016f);
    ASSERT_TRUE(runtime.IsKeyDown(Input::kKeyCode_A));

    // The host reserves the chord mid-hold: its repeats go to the host, and
    // the release still reaches the game that saw the press.
    ReserveForHost(application, Input::kKeyCode_A, Input::kModControl);
    WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionRepeat, Input::kModControl);
    application.Update(0.016f);
    EXPECT_TRUE(application.IsKeyDown(Input::kKeyCode_A));
    WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionRelease, 0);
    WindowInputRouter::RouteFocusChange(config, false);
    application.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(application.IsKeyDown(Input::kKeyCode_A));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_A));
    EXPECT_TRUE(runtime.WasKeyReleased(Input::kKeyCode_A));
}

TEST(WindowInputRoutingTests, RoutingSurvivesAnAbsentUi)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // UI replay parks getUi at null so synthetic input owns the manager; the
    // input system must still receive events, and focus loss must not crash.
    f.config.getUi = []() -> UIManager* { return nullptr; };

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_A, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_A));

    WindowInputRouter::RouteFocusChange(f.config, /*focused=*/false);
    f.input.Update(0.016f);
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_A));
}

TEST(WindowInputRoutingTests, PlayPointerMapperRemapsRuntimeMouseOnly)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    Input::InputSystem runtime{Input::SinkRole::Gameplay};

    // The remap itself is production code (ClientToSurfaceLocal, what the mapper
    // in EditorApplication::GetPlaySurface calls with the resolved Game View's
    // origin). Only the viewport ORIGIN is supplied here, because resolving
    // the panel needs a docked editor; a lambda computing its own offsets would
    // replace the transform under test rather than exercise it.
    constexpr float kViewportLogicalX = 100.0f;
    constexpr float kViewportLogicalY = 40.0f;
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameplaySink = &runtime;
    surface.mapGameplayPointer = [&f](float clientX, float clientY, float& playX, float& playY) {
        WindowInputRouter::ClientToSurfaceLocal(/*window=*/nullptr, f.ui.get(), kViewportLogicalX, kViewportLogicalY,
                                                clientX, clientY, playX, playY);
    };
    f.UsePlaySurface(std::move(surface));

    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 150.0f, 90.0f);

    ASSERT_TRUE(f.input.IsPointerInWindow());
    ASSERT_TRUE(runtime.IsPointerInWindow());
    const Mathematics::Vector2 editorPosition = f.input.GetMousePosition();
    const Mathematics::Vector2 playPosition = runtime.GetMousePosition();
    EXPECT_FLOAT_EQ(editorPosition.x, 150.0f);
    EXPECT_FLOAT_EQ(editorPosition.y, 90.0f);
    EXPECT_FLOAT_EQ(playPosition.x, 50.0f);
    EXPECT_FLOAT_EQ(playPosition.y, 50.0f);
}

// A cursor that left the window is not in it. Every sink the window's moves
// reach has to stop reporting the pointer as in the window at the last point
// inside, or a game keeps edge-scrolling toward a pointer that is somewhere
// else entirely.
TEST(WindowInputRoutingTests, LeavingTheWindowTakesThePointerOutOfEverySink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 40.0f, 30.0f);
    ASSERT_TRUE(f.input.IsPointerInWindow());
    ASSERT_TRUE(runtime.IsPointerInWindow());
    ASSERT_EQ(f.input.GetMousePosition(), Mathematics::Vector2(40.0f, 30.0f));
    ASSERT_EQ(runtime.GetMousePosition(), Mathematics::Vector2(40.0f, 30.0f));

    WindowInputRouter::RouteCursorEnter(f.config, /*entered=*/true);
    EXPECT_TRUE(f.input.IsPointerInWindow());
    EXPECT_TRUE(runtime.IsPointerInWindow());

    WindowInputRouter::RouteCursorEnter(f.config, /*entered=*/false);
    EXPECT_FALSE(f.input.IsPointerInWindow());
    EXPECT_FALSE(runtime.IsPointerInWindow());
    EXPECT_EQ(f.input.GetMousePosition(), Mathematics::Vector2(40.0f, 30.0f));
    EXPECT_EQ(runtime.GetMousePosition(), Mathematics::Vector2(40.0f, 30.0f));

    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 50.0f, 60.0f);
    EXPECT_TRUE(f.input.IsPointerInWindow());
    EXPECT_TRUE(runtime.IsPointerInWindow());
    EXPECT_EQ(f.input.GetMousePosition(), Mathematics::Vector2(50.0f, 60.0f));
    EXPECT_EQ(runtime.GetMousePosition(), Mathematics::Vector2(50.0f, 60.0f));
}

// Focus is state every sink keeps: the game's sink hears it as the editor's
// does, so a game holds its edge scrolling while its window sits behind
// another, and a key it held is let go rather than left down until a release
// that never comes.
TEST(WindowInputRoutingTests, FocusChangesReachTheGameplaySinkAndTheAppInput)
{
    Input::InputSystem application;
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_A);
    WindowInputRouterConfig config;
    config.getInput = [&] { return &application; };
    config.getPlaySurface = [&] { return GameplayOnly(runtime); };
    EXPECT_FALSE(application.IsWindowFocused());
    EXPECT_FALSE(runtime.IsWindowFocused());

    WindowInputRouter::RouteFocusChange(config, /*focused=*/true);
    EXPECT_TRUE(application.IsWindowFocused());
    EXPECT_TRUE(runtime.IsWindowFocused());

    WindowInputRouter::RouteKey(config, Input::kKeyCode_A, Input::kKeyActionPress, 0);
    runtime.Update(0.016f);
    ASSERT_TRUE(runtime.IsKeyDown(Input::kKeyCode_A));

    WindowInputRouter::RouteFocusChange(config, /*focused=*/false);
    runtime.Update(0.016f);
    EXPECT_FALSE(application.IsWindowFocused());
    EXPECT_FALSE(runtime.IsWindowFocused());
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_A));
    EXPECT_TRUE(runtime.WasKeyReleased(Input::kKeyCode_A));
}

// A gameplay sink that is not an InputSystem is told the same thing, in order.
TEST(WindowInputRoutingTests, FocusChangesAreDeliveredToARecordingGameplaySink)
{
    RecordingSink runtime;
    WindowInputRouterConfig config;
    config.getPlaySurface = [&] { return GameplayOnly(runtime); };
    WindowInputRouter::RouteFocusChange(config, /*focused=*/true);
    WindowInputRouter::RouteFocusChange(config, /*focused=*/false);
    EXPECT_EQ(runtime.events, (std::vector<std::string>{"f1", "f0"}));
}

namespace
{
// GLFW's null platform keeps window focus as its own flag: a window created
// visible is focused and a hidden one is not, and nothing reaches the desktop.
// Real Platform::Windows can then be bound without taking focus from whatever
// the user is working in.
class NullPlatformSession
{
  public:
    NullPlatformSession()
    {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
        m_Initialized = Platform::Window::Initialize();
    }
    ~NullPlatformSession()
    {
        if (m_Initialized)
            Platform::Window::Terminate();
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
    }
    NullPlatformSession(const NullPlatformSession&) = delete;
    NullPlatformSession& operator=(const NullPlatformSession&) = delete;

    bool IsNullPlatform() const { return m_Initialized && glfwGetPlatform() == GLFW_PLATFORM_NULL; }

  private:
    bool m_Initialized = false;
};

bool CreateTestWindow(Platform::Window& window, bool visible)
{
    constexpr int kTestWindowExtent = 64;
    Platform::WindowDesc desc;
    desc.Title = "WindowInputRoutingTests";
    desc.Width = kTestWindowExtent;
    desc.Height = kTestWindowExtent;
    desc.StartHidden = !visible;
    return window.Create(desc);
}
} // namespace

// A window created with focus reports no gain to handlers bound after it, so
// binding is where every sink learns the window holds focus; without that, a
// Player launched focused would not edge-scroll until the user left the window
// and came back. Binding a window without focus says nothing, so it cannot
// unfocus a sink another window shares.
TEST(WindowInputRoutingTests, BindingAWindowReportsItsFocusToEverySink)
{
    NullPlatformSession platform;
    ASSERT_TRUE(platform.IsNullPlatform()) << "the null platform is what lets a test window hold focus";

    Input::InputSystem application;
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    WindowInputRouterConfig config;
    config.getInput = [&] { return &application; };
    config.getPlaySurface = [&] { return GameplayOnly(runtime); };

    Platform::Window hidden;
    ASSERT_TRUE(CreateTestWindow(hidden, /*visible=*/false));
    ASSERT_FALSE(hidden.IsFocused());
    config.window = &hidden;
    WindowInputRouter::BindBasicHandlers(config);
    EXPECT_FALSE(application.IsWindowFocused());
    EXPECT_FALSE(runtime.IsWindowFocused());

    Platform::Window focused;
    ASSERT_TRUE(CreateTestWindow(focused, /*visible=*/true));
    ASSERT_TRUE(focused.IsFocused());
    config.window = &focused;
    WindowInputRouter::BindBasicHandlers(config);
    EXPECT_TRUE(application.IsWindowFocused());
    EXPECT_TRUE(runtime.IsWindowFocused());

    Platform::Window secondHidden;
    ASSERT_TRUE(CreateTestWindow(secondHidden, /*visible=*/false));
    config.window = &secondHidden;
    WindowInputRouter::BindBasicHandlers(config);
    EXPECT_TRUE(application.IsWindowFocused()) << "binding an unfocused window must not unfocus a shared sink";
    EXPECT_TRUE(runtime.IsWindowFocused());
}

// Every pointer move a tool injects (the debug server's move_pointer, clicks,
// drags) enters RouteMouseMove, so the router is where the desktop cursor must
// stay out of it: the move is delivered to the sinks and the platform cursor,
// which on a real desktop is under the user's hand, stays where it was.
TEST(WindowInputRoutingTests, ARoutedMoveLeavesThePlatformCursorWhereItIs)
{
    NullPlatformSession platform;
    ASSERT_TRUE(platform.IsNullPlatform()) << "the null platform is what lets a test window hold focus";

    Platform::Window window;
    ASSERT_TRUE(CreateTestWindow(window, /*visible=*/true));
    ASSERT_TRUE(window.IsFocused());

    // The instrument: GLFW ignores a cursor move unless the window has focus, so
    // an unfocused window would read the same position whatever the router did.
    glfwSetCursorPos(window.GetGLFWHandle(), 7.0, 9.0);
    float cursorX = 0.0f;
    float cursorY = 0.0f;
    window.GetCursorClientPosition(cursorX, cursorY);
    ASSERT_EQ(cursorX, 7.0f);
    ASSERT_EQ(cursorY, 9.0f);

    Input::InputSystem application;
    WindowInputRouterConfig config;
    config.window = &window;
    config.getInput = [&] { return &application; };
    WindowInputRouter::BindBasicHandlers(config);

    WindowInputRouter::RouteMouseMove(config, &window, 40.0f, 30.0f);

    window.GetCursorClientPosition(cursorX, cursorY);
    EXPECT_EQ(cursorX, 7.0f) << "a routed move must not move the platform cursor";
    EXPECT_EQ(cursorY, 9.0f);
    const std::optional<Mathematics::Vector2> delivered = application.GetMousePosition();
    ASSERT_TRUE(delivered);
    EXPECT_EQ(delivered->x, 40.0f);
    EXPECT_EQ(delivered->y, 30.0f);
}

TEST(WindowInputRoutingTests, ABareModifierPressIsOfferedToTheGameplaySink)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The platform stamps a modifier's own bit on the press of that modifier, so
    // the modifier mask alone reads a bare Ctrl as a chord. A chord is a modifier
    // plus another key: a game that reads Ctrl (crouch) must receive its press.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByPolling(runtime, Input::kKeyCode_LeftControl);
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftControl, Input::kKeyActionPress, Input::kModControl);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_LeftControl));
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_LeftControl));

    // A game that reads no modifier leaves it to the editor, where Scene View
    // "Move Faster" and the chord masks are read.
    Input::InputSystem quiet{Input::SinkRole::Gameplay};
    f.UseGameplaySink(quiet);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftAlt, Input::kKeyActionPress, Input::kModAlt);
    f.input.Update(0.016f);
    quiet.Update(0.016f);
    EXPECT_FALSE(quiet.IsKeyDown(Input::kKeyCode_LeftAlt));
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_LeftAlt));
}

TEST(WindowInputRoutingTests, APausedGameplaySinkHandsItsKeysToTheEditor)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The editor action the game's key takes while it is running.
    const Input::ContextId context = Input::HashInput("Test.SceneViewContext");
    Input::ActionDesc moveForward{};
    moveForward.id = Input::HashInput("Test.SceneView.MoveForward");
    moveForward.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_W, 1.0f});
    f.input.RegisterAction(context, moveForward);
    f.input.PushContext(context);

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    ClaimByBinding(runtime, Input::kKeyCode_W);
    f.UseGameplaySink(runtime);

    // Paused, the game consumes nothing: its bound key fires the editor action
    // instead, and a key it polls is not its either.
    runtime.SetClaimsSuspended(true);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(f.input.IsActionActive(moveForward.id));
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_W));

    ClaimByPolling(runtime, Input::kKeyCode_Space);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Space, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_FALSE(runtime.IsKeyDown(Input::kKeyCode_Space));
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_Space));

    // Resuming restores the game's claim on the very same key.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionRelease, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    ASSERT_FALSE(f.input.IsActionActive(moveForward.id));

    runtime.SetClaimsSuspended(false);
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    f.input.Update(0.016f);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsKeyDown(Input::kKeyCode_W));
    EXPECT_FALSE(f.input.IsActionActive(moveForward.id));
}

// --- Pointer routing -------------------------------------------------------
// Mouse buttons obey the rule keys already do: the UI answers first, and a
// press it consumed belongs to that control alone.

TEST(WindowInputRoutingTests, ConsumedMousePressIsWithheldFromInputAndRuntime)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Button* btn = AttachHoveredButton(f, "router_consumed_press.css");
    ASSERT_NE(btn, nullptr);
    ASSERT_GT(btn->GetLayoutWidth(), 0.0f) << "the button needs a box to be pressed on";

    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    f.input.Update(0.016f);

    EXPECT_FALSE(f.input.IsMouseButtonDown(0))
        << "a press the UI acted on must not also fire editor mouse actions";
    EXPECT_TRUE(runtime.events.empty())
        << "a press the UI acted on must not also reach the game";
}

TEST(WindowInputRoutingTests, MouseReleaseAlwaysLandsEvenWhenItsPressWasConsumed)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Button* btn = AttachHoveredButton(f, "router_release_always.css");
    ASSERT_NE(btn, nullptr);

    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);

    // Under-forwarding a release is the stuck-button bug class: both sinks see
    // it whatever the press did, and an unmatched release is a no-op for both.
    ASSERT_EQ(runtime.events.size(), 1u) << "the release must reach the game even so";
    EXPECT_EQ(runtime.events[0], "b0:0");
}

TEST(WindowInputRoutingTests, UnconsumedMousePressReachesBothSinks)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Button* btn = AttachHoveredButton(f, "router_unconsumed_press.css");
    ASSERT_NE(btn, nullptr);

    // Park the pointer off the button, on bare root, and re-resolve hover: now
    // nothing acts on the press, which is how a click in the viewport reaches
    // the game.
    f.ui->OnMouseMove(btn->GetLayoutX() + 150.0f, btn->GetLayoutY() + 5.0f);
    f.ui->Update(0.0f, /*interactive=*/true);

    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    f.input.Update(0.016f);

    EXPECT_TRUE(f.input.IsMouseButtonDown(0));
    ASSERT_EQ(runtime.events.size(), 1u) << "an unconsumed press must reach the game";
    EXPECT_EQ(runtime.events[0], "b0:1");
}

TEST(WindowInputRoutingTests, RawButtonStateHookSeesConsumedPressesToo)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Button* btn = AttachHoveredButton(f, "router_post_hook.css");
    ASSERT_NE(btn, nullptr);

    // onMouseButtonPost mirrors physical button state for the host. Gating it on
    // consumption would leave the host believing a held button is up, so it is
    // deliberately outside the withholding rule.
    std::vector<std::string> posted;
    f.config.onMouseButtonPost = [&posted](int button, bool pressed) {
        posted.push_back("b" + std::to_string(button) + ":" + (pressed ? "1" : "0"));
    };

    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);

    ASSERT_EQ(posted.size(), 2u);
    EXPECT_EQ(posted[0], "b0:1");
    EXPECT_EQ(posted[1], "b0:0");
}

// --- Pointer grabbed outside the chain -------------------------------------
// A screen-wide sampler previews the pixel under the cursor wherever it goes and
// takes the click that commits it. While it holds the pointer, no stage here may
// act on one: the click meant for a pixel must not also press the control under
// the cursor, and the position it is tracking is not this window's to follow.

TEST(WindowInputRoutingTests, AGrabbedPointerReachesNoStage)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Button* btn = AttachHoveredButton(f, "router_pointer_grab.css");
    ASSERT_NE(btn, nullptr);
    int clicks = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&clicks](UIEvent&) { ++clicks; });

    RecordingSink runtime;
    f.UseGameplaySink(runtime);
    bool grabbed = true;
    f.config.isPointerGrabbed = [&grabbed]() { return grabbed; };

    const Mathematics::Vector2 before = f.input.GetMousePosition();
    const Mathematics::Vector2 target(320.0f, 240.0f);
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, target.x, target.y);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);
    WindowInputRouter::RouteScroll(f.config, 0.0f, 1.0f, /*mods=*/0);
    f.input.Update(0.016f);

    EXPECT_EQ(f.input.GetMousePosition(), before);
    EXPECT_FALSE(f.input.IsPointerInWindow()) << "a grabbed move must not place the pointer in the window";
    EXPECT_EQ(clicks, 0);
    EXPECT_FALSE(f.input.IsMouseButtonDown(0));
    EXPECT_TRUE(runtime.events.empty());

    // Released, the same events are ordinary again — which is what keeps the
    // arm above from passing on a chain that delivers nothing.
    grabbed = false;
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, target.x, target.y);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);
    WindowInputRouter::RouteScroll(f.config, 0.0f, 1.0f, /*mods=*/0);

    EXPECT_EQ(f.input.GetMousePosition(), target);
    EXPECT_TRUE(f.input.IsPointerInWindow());
    EXPECT_EQ(clicks, 1);
    ASSERT_FALSE(runtime.events.empty());
    EXPECT_EQ(runtime.events.back(), "s0:1") << "the wheel travels the chain again once the grab is released";
}

TEST(WindowInputRoutingTests, AGrabbedPointerDoesNotWithholdKeysOrCharacters)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto field = std::make_unique<TextField>();
    field->SetId("field");
    root->AddChild(std::move(field));
    f.ui->SetRoot(std::move(root));
    f.config.isPointerGrabbed = []() { return true; };

    // A character typed while a grab is held still reaches the focused control.
    f.ui->SetFocusById("field");
    EXPECT_TRUE(WindowInputRouter::RouteChar(f.config, 'w'));

    // And a key nobody consumes still reaches the app InputSystem. Escape is how
    // a grab is cancelled, so the keyboard leg cannot be gated on one.
    f.ui->ClearFocus();
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Escape, Input::kKeyActionPress, /*mods=*/0);
    f.input.Update(0.016f);
    EXPECT_TRUE(f.input.IsKeyDown(Input::kKeyCode_Escape));
}

// --- Scroll routing --------------------------------------------------------
// OnScroll reports honestly whether a scrollable moved, and the router acts on
// that answer: a tick nothing moved keeps travelling.

TEST(WindowInputRoutingTests, ScrollAScrollViewConsumedStopsAtTheUi)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Content taller than the viewport, so there is somewhere to scroll to and
    // the wheel genuinely moves it — an unscrollable ScrollView would decline
    // and make this arm vacuous.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto scrollOwned = std::make_unique<ScrollView>();
    auto* scroll = scrollOwned.get();
    scroll->SetId("scroll");
    auto contentOwned = std::make_unique<UIElement>();
    contentOwned->SetId("content");
    // AddContent, not AddChild: a plain child is a sibling of the clip viewport
    // and gives the view no range, which would leave this arm's control vacuous.
    scroll->AddContent(std::move(contentOwned));
    root->AddChild(std::move(scrollOwned));
    f.ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "router_scroll_consumed.css";
    {
        std::ofstream out(css);
        out << R"(
#root { display: flex; width: 200px; height: 100px; }
#scroll { width: 200px; height: 100px; overflow: scroll; }
#content { width: 200px; height: 1000px; }
)";
    }
    ASSERT_TRUE(f.ui->AttachStyleFromFile(css.string()));

    f.ui->Update(0.0f, /*interactive=*/false);
    f.ui->OnMouseMove(scroll->GetLayoutX() + 10.0f, scroll->GetLayoutY() + 10.0f);
    f.ui->Update(0.0f, /*interactive=*/true);

    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    // Positive control first: the wheel really is consumed by this tree, so the
    // absence below is the router honoring an answer rather than no answer.
    ASSERT_TRUE(f.ui->OnScroll(0.0f, -1.0f))
        << "control: this ScrollView must actually consume a wheel, or the arm is vacuous";

    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/-1.0f, /*mods=*/0);

    EXPECT_TRUE(runtime.events.empty())
        << "a wheel a scrollable moved must not also drive gameplay zoom behind it";
}

TEST(WindowInputRoutingTests, UnconsumedScrollReachesBothSinks)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // No root at all: nothing can consume a wheel, which is the viewport case.
    RecordingSink runtime;
    f.UseGameplaySink(runtime);

    ASSERT_FALSE(f.ui->OnScroll(0.0f, -1.0f))
        << "control: an empty tree must decline the wheel";

    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/-1.0f, /*mods=*/0);

    ASSERT_EQ(runtime.events.size(), 1u) << "a declined wheel must reach the game";
    EXPECT_EQ(runtime.events[0].rfind("s", 0), 0u);
}

// The gameplay stage on the pointer path, which is what S3's sink-return seam
// buys for the mouse: a click the game claims stops before the editor's own
// InputSystem, so a click in the viewport does not also fire an editor tool
// behind it. A claim from polling is used here because it needs no action-map
// setup — the same claim source the key rows exercise.
TEST(WindowInputRoutingTests, MousePressTheGameClaimsStopsBeforeEditorInput)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    // No UI tree: nothing in chrome can act, so the press reaches the game
    // stage, which is the stage under test.
    Input::InputSystem gameplay(Input::SinkRole::Gameplay);
    f.UseGameplaySink(gameplay);

    // Control first: with nothing claimed the press falls through to the editor,
    // so the absence below is a claim being honoured rather than a press that
    // never arrived.
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/1, /*pressed=*/true, /*mods=*/0);
    f.input.Update(0.016f);
    ASSERT_TRUE(f.input.IsMouseButtonDown(1))
        << "control: an unclaimed press must reach the editor InputSystem";
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/1, /*pressed=*/false, /*mods=*/0);
    f.input.Update(0.016f);

    // Now the game polls button 0 — the way poll-style game code claims it.
    (void)gameplay.IsMouseButtonDown(0);

    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);

    EXPECT_TRUE(gameplay.IsMouseButtonDown(0)) << "the game must have taken the press it claims";
    EXPECT_FALSE(f.input.IsMouseButtonDown(0))
        << "a click the game claimed must not also reach the editor InputSystem";

    // The release lands everywhere regardless, so neither side is left holding a
    // button down.
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);
    EXPECT_FALSE(gameplay.IsMouseButtonDown(0));
    EXPECT_FALSE(f.input.IsMouseButtonDown(0));
}

// A game whose only use of Ctrl is a Ctrl+Click binding never records Ctrl as
// held: a bare Ctrl press matches no binding, so the gameplay sink declines it
// and never sees it again. The click therefore has to carry the platform's live
// modifier mask, or the chord can never be judged and the binding is dead in
// editor play.
TEST(WindowInputRoutingTests, ChordedMousePressIsClaimedFromTheEventMods)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Input::InputSystem gameplay(Input::SinkRole::Gameplay);
    f.UseGameplaySink(gameplay);

    Input::ActionBinding ctrlClick{Input::DeviceType::Mouse, /*code=*/0, 1.0f};
    ctrlClick.requiredMods = Input::kModControl;
    gameplay.BindKey(kGameplayContext, kGameplayAction, ctrlClick);
    gameplay.PushContext(kGameplayContext);

    // The bare Ctrl the user holds first: nothing is bound to it, so the game
    // declines it and its own key state stays empty. This is the precondition
    // that makes the mask the only source of truth for the chord.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftControl, /*action=*/1,
                                /*mods=*/Input::kModControl);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);
    ASSERT_FALSE(gameplay.IsKeyDown(Input::kKeyCode_LeftControl))
        << "precondition: the game must NOT be holding Ctrl itself";

    // What the game took is read off the EDITOR sink, never by polling the game's
    // own button state: a poll is itself a claim source, so asking the gameplay
    // sink whether it holds button 0 would manufacture the very claim under test
    // and the chord arm would pass with the modifier mask thrown away.

    // Control: an unmodified click matches no binding, so it declines and the
    // editor gets it. Without this arm the claim below could pass vacuously.
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true, /*mods=*/0);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);
    EXPECT_TRUE(f.input.IsMouseButtonDown(0))
        << "control: a bare click matches no Ctrl+Click binding, so it must reach the editor";
    EXPECT_FALSE(gameplay.WasActionTriggered(kGameplayAction))
        << "control: an unmodified click must not fire a Ctrl-chorded action";
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false, /*mods=*/0);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);

    // The chord itself: the click carries Ctrl, so the binding matches.
    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/true,
                                        /*mods=*/Input::kModControl);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);

    EXPECT_FALSE(f.input.IsMouseButtonDown(0))
        << "the click the game claimed must not also reach the editor InputSystem";
    // Claiming is not enough on its own: the bound action has to actually fire,
    // which is the half that reads the modifier snapshot rather than the claim.
    EXPECT_TRUE(gameplay.WasActionTriggered(kGameplayAction))
        << "the claimed chord must also drive its action";

    WindowInputRouter::RouteMouseButton(f.config, /*button=*/0, /*pressed=*/false,
                                        /*mods=*/Input::kModControl);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);
}

// The wheel has the same hole: a Ctrl+Wheel axis binding is judged from the
// modifier snapshot, and the game never saw Ctrl go down. The editor sink reads
// the wheel through an unmodified axis of its own, so what the game took is
// again read off the editor rather than polled from the game.
TEST(WindowInputRoutingTests, ChordedScrollIsClaimedFromTheEventMods)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }
    Input::InputSystem gameplay(Input::SinkRole::Gameplay);
    f.UseGameplaySink(gameplay);

    Input::ActionDesc zoom{};
    zoom.id = kGameplayAction;
    zoom.isAxis = true;
    Input::ActionBinding ctrlWheel{Input::DeviceType::Mouse, Input::kMouseScrollAxisY, 1.0f};
    ctrlWheel.requiredMods = Input::kModControl;
    zoom.bindings.push_back(ctrlWheel);
    gameplay.RegisterAction(kGameplayContext, zoom);
    gameplay.PushContext(kGameplayContext);

    constexpr Input::ContextId kEditorContext = Input::HashInput("Test.Editor");
    constexpr Input::ActionId kEditorWheel = Input::HashInput("Test.Editor.Wheel");
    Input::ActionDesc editorWheel{};
    editorWheel.id = kEditorWheel;
    editorWheel.isAxis = true;
    editorWheel.bindings.push_back({Input::DeviceType::Mouse, Input::kMouseScrollAxisY, 1.0f});
    f.input.RegisterAction(kEditorContext, editorWheel);
    f.input.PushContext(kEditorContext);

    // The bare Ctrl the user holds first, which the game declines.
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftControl, /*action=*/1,
                                /*mods=*/Input::kModControl);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);

    // Control: an unmodified wheel matches no Ctrl+Wheel binding, so the editor
    // gets it and the game's axis stays flat.
    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/1.0f, /*mods=*/0);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);
    EXPECT_FLOAT_EQ(f.input.GetActionState(kEditorWheel).value, 1.0f)
        << "control: a bare wheel matches no Ctrl+Wheel binding, so it must reach the editor";
    EXPECT_FLOAT_EQ(gameplay.GetActionState(kGameplayAction).value, 0.0f)
        << "control: an unmodified wheel must not drive a Ctrl-chorded axis";

    // The chord itself: the wheel carries Ctrl, so the binding matches, the
    // editor never sees it, and the axis actually moves.
    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/1.0f, /*mods=*/Input::kModControl);
    f.input.Update(0.016f);
    gameplay.Update(0.016f);
    EXPECT_FLOAT_EQ(f.input.GetActionState(kEditorWheel).value, 0.0f)
        << "the wheel the game claimed must not also reach the editor InputSystem";
    EXPECT_FLOAT_EQ(gameplay.GetActionState(kGameplayAction).value, 1.0f)
        << "the claimed chord must also drive its axis";
}

// ---------------------------------------------------------------------------
// The play surface's own UI as a chain stage (S4).
//
// The chain a key walks on a window hosting a running Game View:
//   1 app pre-hooks -> 2 chrome UI -> 3 the play surface's HUD ->
//   4 gameplay (by claims) -> 5 the editor InputSystem.
// These rows pin the order, which is the whole arbitration: no stage declares a
// priority, so "a focused editor field beats a focused HUD field beats the game"
// has to be a fact about who is asked first.
// ---------------------------------------------------------------------------

namespace
{
// A HUD the way a bound UIDocument mounts one: a text field as a direct child of
// the host's root. The counters record the key and text events that actually
// reach it, so a row can tell "the HUD declined" from "the HUD was never asked".
struct HudSurface
{
    std::unique_ptr<GameUIHost> host;
    TextField* field = nullptr;
    int keyEvents = 0;
    int textEvents = 0;

    void Focus() { host->GetUIManager()->SetFocusById("chat"); }
    UIManager* Ui() { return host->GetUIManager(); }
};

bool MakeHud(HudSurface& hud, Rendering::IDevice* device)
{
    hud.host = std::make_unique<GameUIHost>(device, /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = hud.host->GetUIManager();
    if (!ui)
        return false;
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto field = std::make_unique<TextField>();
    field->SetId("chat");
    hud.field = field.get();
    root->AddChild(std::move(field));
    ui->SetRoot(std::move(root));
    hud.field->RegisterEventHandler(kEventKeyDown, [&hud](UIEvent&) { ++hud.keyEvents; });
    hud.field->RegisterEventHandler(kEventTextInput, [&hud](UIEvent&) { ++hud.textEvents; });
    return true;
}

// A HUD whose control is a button, with the cursor parked over it and hover
// resolved — the state a player leaves behind by moving the mouse across the
// HUD. The click count records whether the button actually fired.
struct HudButtonSurface
{
    std::unique_ptr<GameUIHost> host;
    Button* button = nullptr;
    int clicks = 0;

    UIManager* Ui() { return host->GetUIManager(); }
};

bool MakeHoveredHudButton(HudButtonSurface& hud, Rendering::IDevice* device, const char* cssName)
{
    hud.host = std::make_unique<GameUIHost>(device, /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = hud.host->GetUIManager();
    if (!ui)
        return false;
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto btnOwned = std::make_unique<Button>();
    hud.button = btnOwned.get();
    hud.button->SetId("damage-btn");
    root->AddChild(std::move(btnOwned));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / cssName;
    {
        std::ofstream out(css);
        out << R"(
#hud-root { display: flex; width: 200px; height: 40px; }
#damage-btn { width: 100px; height: 20px; }
)";
    }
    if (!ui->AttachStyleFromFile(css.string()))
        return false;

    ui->Update(0.0f, /*interactive=*/false);
    ui->OnMouseMove(hud.button->GetLayoutX() + 5.0f, hud.button->GetLayoutY() + 5.0f);
    ui->Update(0.0f, /*interactive=*/true);
    hud.button->RegisterEventHandler(kEventButtonClick, [&hud](UIEvent&) { ++hud.clicks; });
    return true;
}

// The surface the editor resolves while play is live and the Game View is the
// window's active tab: HUD and gameplay together, from one liveness answer.
void UseLivePlaySurface(RouterFixture& f, HudSurface& hud, Input::IRawInputSink& gameplay)
{
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameUi = hud.host.get();
    surface.gameplaySink = &gameplay;
    f.UsePlaySurface(std::move(surface));
}
} // namespace

// The slice's reason for existing: typing into a HUD chat box must not also walk
// the character. W is a key the game claims, and the focused HUD field takes it
// first because stage 3 is asked before stage 4.
TEST(WindowInputRoutingTests, AFocusedHudFieldTakesTheKeyTheGameClaims)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudSurface hud;
    ASSERT_TRUE(MakeHud(hud, f.device.get()));
    Input::InputSystem gameplay{Input::SinkRole::Gameplay};
    ClaimByBinding(gameplay, Input::kKeyCode_W);
    UseLivePlaySurface(f, hud, gameplay);

    hud.Focus();
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    gameplay.Update(0.016f);

    EXPECT_EQ(hud.keyEvents, 1) << "the focused HUD field must be offered the key";
    EXPECT_FALSE(gameplay.IsKeyDown(Input::kKeyCode_W))
        << "a key the HUD took must not also walk the character";
    EXPECT_FALSE(f.input.IsKeyDown(Input::kKeyCode_W)) << "and must not reach the editor either";
}

// The other half, and what makes a HUD usable at all: with nothing in the HUD
// focused the surface is transparent and W still walks.
TEST(WindowInputRoutingTests, AnUnfocusedHudLeavesTheGameplayKeyAlone)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudSurface hud;
    ASSERT_TRUE(MakeHud(hud, f.device.get()));
    Input::InputSystem gameplay{Input::SinkRole::Gameplay};
    ClaimByBinding(gameplay, Input::kKeyCode_W);
    UseLivePlaySurface(f, hud, gameplay);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    gameplay.Update(0.016f);

    EXPECT_EQ(hud.keyEvents, 0) << "nothing focused: no HUD element acts";
    EXPECT_TRUE(gameplay.IsKeyDown(Input::kKeyCode_W)) << "the game still gets its movement key";
}

// The same half, for the state a HUD is in most of the time: nothing focused and
// the cursor simply resting on a button. Chrome offers a key with no focus to
// the hovered element — and a Button acts on Space — so on a game surface the
// jump would fire the button instead of the jump. Hover is not focus here.
TEST(WindowInputRoutingTests, AHoveredUnfocusedHudButtonLeavesTheGameplayKeyAlone)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudButtonSurface hud;
    ASSERT_TRUE(MakeHoveredHudButton(hud, f.device.get(), "router_hud_hovered_button.css"));
    ASSERT_EQ(hud.Ui()->GetHoveredElement(), hud.button)
        << "precondition: the cursor rests on the HUD button";
    ASSERT_TRUE(hud.Ui()->GetFocusedElementId().empty()) << "precondition: nothing is focused";

    Input::InputSystem gameplay{Input::SinkRole::Gameplay};
    ClaimByBinding(gameplay, Input::kKeyCode_Space);
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameUi = hud.host.get();
    surface.gameplaySink = &gameplay;
    f.UsePlaySurface(std::move(surface));

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_Space, Input::kKeyActionPress, 0);
    gameplay.Update(0.016f);

    EXPECT_EQ(hud.clicks, 0) << "a HUD button the cursor merely rests on must not fire on Space";
    EXPECT_TRUE(gameplay.IsKeyDown(Input::kKeyCode_Space)) << "the jump belongs to the game";
}

// Decided by order alone: chrome is asked first, so an editor field left focused
// keeps its typing even while a HUD field is also focused. There is no
// arbitration API, and the HUD is never even asked.
TEST(WindowInputRoutingTests, AFocusedChromeFieldOutranksAFocusedHudField)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto chromeField = std::make_unique<TextField>();
    chromeField->SetId("inspector-field");
    root->AddChild(std::move(chromeField));
    f.ui->SetRoot(std::move(root));

    HudSurface hud;
    ASSERT_TRUE(MakeHud(hud, f.device.get()));
    Input::InputSystem gameplay{Input::SinkRole::Gameplay};
    UseLivePlaySurface(f, hud, gameplay);

    // Both hold a text focus — the editor field from an earlier click in the
    // Inspector, the chat box from an earlier click in the viewport.
    hud.Focus();
    f.ui->SetFocusById("inspector-field");

    WindowInputRouter::RouteChar(f.config, 'w');
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);

    EXPECT_EQ(hud.textEvents, 0) << "chrome answered first, so the HUD never sees the character";
    EXPECT_EQ(hud.keyEvents, 0) << "nor its key";
}

// A character and the key that produced it are one keystroke: both stop at the
// same stage, or the game receives the text of an edit the HUD already made.
TEST(WindowInputRoutingTests, TypedTextStopsAtTheHudBeforeTheGame)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudSurface hud;
    ASSERT_TRUE(MakeHud(hud, f.device.get()));
    RecordingSink gameplay;
    UseLivePlaySurface(f, hud, gameplay);

    hud.Focus();
    EXPECT_TRUE(WindowInputRouter::RouteChar(f.config, 'w'));
    EXPECT_EQ(hud.textEvents, 1);
    EXPECT_TRUE(gameplay.events.empty()) << "a character the HUD took must not also reach the game";

    // Focus off the HUD and the character is nobody's, so it travels again.
    hud.Ui()->ClearFocus();
    EXPECT_FALSE(WindowInputRouter::RouteChar(f.config, 'w'));
    EXPECT_EQ(gameplay.events, (std::vector<std::string>{"c" + std::to_string('w')}));
}

// Liveness is one answer for the whole surface. A window whose Game View is not
// the active tab resolves no surface, so its HUD is never offered the keystroke
// — a torn-off Game View cannot eat text typed into the main window.
TEST(WindowInputRoutingTests, ADeadPlaySurfaceOffersItsHudNothing)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudSurface hud;
    ASSERT_TRUE(MakeHud(hud, f.device.get()));
    hud.Focus();

    f.config.getPlaySurface = []() { return WindowInputRouterConfig::PlaySurface{}; };

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    WindowInputRouter::RouteChar(f.config, 'w');

    EXPECT_EQ(hud.keyEvents, 0);
    EXPECT_EQ(hud.textEvents, 0);
}

// Releases are never consumed, at this stage as at every other: a game holding W
// when the chat box takes focus must still be told the key came up.
//
// The HUD element here acts on key-up and says so — a hold-to-talk button
// letting go. An ordinary text field declines releases, so without such an
// element the router's release rule is never the reason this passes.
TEST(WindowInputRoutingTests, AReleaseReachesTheGameEvenWhenAHudElementActsOnIt)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudSurface hud;
    ASSERT_TRUE(MakeHud(hud, f.device.get()));
    hud.field->RegisterEventHandler(kEventKeyUp, [](UIEvent& ev) { ev.Stop(); });
    Input::InputSystem gameplay{Input::SinkRole::Gameplay};
    ClaimByBinding(gameplay, Input::kKeyCode_W);
    UseLivePlaySurface(f, hud, gameplay);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionPress, 0);
    gameplay.Update(0.016f);
    ASSERT_TRUE(gameplay.IsKeyDown(Input::kKeyCode_W)) << "precondition: the game holds W";

    hud.Focus();
    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_W, Input::kKeyActionRelease, 0);
    gameplay.Update(0.016f);
    EXPECT_FALSE(gameplay.IsKeyDown(Input::kKeyCode_W)) << "a withheld release is a key stuck down";
}

// Focus loss ends every stage's held state, the HUD included. Its window stops
// hearing keys, so a modifier released elsewhere would stay held here and turn
// the next plain keystroke into a chord.
TEST(WindowInputRoutingTests, FocusLossResetsTheHudsHeldModifiers)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudSurface hud;
    ASSERT_TRUE(MakeHud(hud, f.device.get()));
    Input::InputSystem gameplay{Input::SinkRole::Gameplay};
    UseLivePlaySurface(f, hud, gameplay);

    WindowInputRouter::RouteKey(f.config, Input::kKeyCode_LeftControl, Input::kKeyActionPress,
                                Input::kModControl);
    ASSERT_NE(hud.Ui()->GetModifierKeys() & Input::kModControl, 0)
        << "precondition: the HUD saw the press";

    WindowInputRouter::RouteFocusChange(f.config, /*focused=*/false);
    EXPECT_EQ(hud.Ui()->GetModifierKeys() & Input::kModControl, 0);
}

// ---------------------------------------------------------------------------
// The play surface's POINTER, for a router that is its source: a window whose
// whole client area is the surface, which is the Player. The editor's Game View
// is pointed by the chrome element that owns its rect instead, and the last row
// here is the guard that keeps this router out of that leg.
// ---------------------------------------------------------------------------

namespace
{
// A HUD with one button at a known rect, laid out but with no pointer fed yet:
// these rows deliver the pointer through the router, which is the thing under
// test.
bool MakeHudButton(HudButtonSurface& hud, Rendering::IDevice* device, const char* cssName)
{
    hud.host = std::make_unique<GameUIHost>(device, /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = hud.host->GetUIManager();
    if (!ui)
        return false;
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto btnOwned = std::make_unique<Button>();
    hud.button = btnOwned.get();
    hud.button->SetId("damage-btn");
    root->AddChild(std::move(btnOwned));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / cssName;
    {
        std::ofstream out(css);
        out << R"(
#hud-root { position: relative; width: 400px; height: 400px; }
#damage-btn { position: absolute; left: 100px; top: 100px; width: 100px; height: 40px; }
)";
    }
    if (!ui->AttachStyleFromFile(css.string()))
        return false;
    ui->Update(0.0f, /*interactive=*/false);
    hud.button->RegisterEventHandler(kEventButtonClick, [&hud](UIEvent&) { ++hud.clicks; });
    return true;
}

// The Player's config: no chrome bound on this window at all (the slot stays
// reserved, Q4), the HUD pointed by this router, and the game's own InputSystem
// as the last stage.
void UsePlayerSurface(RouterFixture& f, GameUIHost& hud, float toSurfacePixels)
{
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameUi = &hud;
    surface.mapGameUiPointer = [toSurfacePixels](float clientX, float clientY, float& uiX, float& uiY)
    {
        uiX = clientX * toSurfacePixels;
        uiY = clientY * toSurfacePixels;
    };
    f.UsePlaySurface(std::move(surface));
    f.config.getUi = nullptr;
}

// Hover resolves on the host's own frame tick — the one the Player's render loop
// drives — and a press can only land on a control once it has.
void SettleHud(GameUIHost& hud)
{
    hud.GetUIManager()->Update(0.0f, /*interactive=*/true);
}
} // namespace

// The slice's reason for existing on the pointer side: in the Player a click on
// a HUD button also fired the game, because nothing between them consumed.
TEST(WindowInputRoutingTests, AClickTheHudTakesIsNotAlsoTheGames)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudButtonSurface hud;
    ASSERT_TRUE(MakeHudButton(hud, f.device.get(), "router_player_hud_click.css"));
    UsePlayerSurface(f, *hud.host, /*toSurfacePixels=*/1.0f);

    // Control first: a press on bare HUD background is claimed by nobody and
    // reaches the game, so the absence below is consumption rather than a press
    // that never arrived.
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 10.0f, 10.0f);
    SettleHud(*hud.host);
    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/true, 0);
    f.input.Update(0.016f);
    ASSERT_TRUE(f.input.IsMouseButtonDown(Input::kMouseButton_Left))
        << "control: an unclaimed press must reach the game's InputSystem";
    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/false, 0);
    f.input.Update(0.016f);

    // Now over the HUD button, which acts on the press.
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 150.0f, 120.0f);
    SettleHud(*hud.host);
    ASSERT_EQ(hud.Ui()->GetHoveredElement(), hud.button)
        << "the router's move never reached the HUD";

    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/true, 0);
    f.input.Update(0.016f);
    EXPECT_FALSE(f.input.IsMouseButtonDown(Input::kMouseButton_Left))
        << "a press the HUD acted on must not also reach the game";

    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/false, 0);
    SettleHud(*hud.host);
    EXPECT_EQ(hud.clicks, 1) << "the HUD button never completed its click";
    f.input.Update(0.016f);
    EXPECT_FALSE(f.input.IsMouseButtonDown(Input::kMouseButton_Left))
        << "the release is never consumed, and must leave no button held anywhere";
}

// A click's modifier mask is live platform state and the only report of a
// modifier held since before the window took focus, so the HUD takes it from
// the click as chrome does, not only from the key presses it saw.
TEST(WindowInputRoutingTests, AHudClickCarriesTheModifiersHeldAtTheClick)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudButtonSurface hud;
    ASSERT_TRUE(MakeHudButton(hud, f.device.get(), "router_player_hud_mods.css"));
    UsePlayerSurface(f, *hud.host, /*toSurfacePixels=*/1.0f);
    int clickMods = -1;
    hud.button->RegisterEventHandler(kEventButtonClick, [&clickMods](UIEvent& e) { clickMods = e.Mods; });

    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 150.0f, 120.0f);
    SettleHud(*hud.host);
    ASSERT_EQ(hud.Ui()->GetHoveredElement(), hud.button);
    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/true, Input::kModShift);
    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/false, Input::kModShift);
    ASSERT_EQ(hud.clicks, 1);
    EXPECT_EQ(clickMods, Input::kModShift) << "a Shift-click on the HUD arrived without Shift";

    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/true, 0);
    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/false, 0);
    ASSERT_EQ(hud.clicks, 2);
    EXPECT_EQ(clickMods, 0) << "a plain click inherited the previous click's Shift";
}

// The pointer arrives in the surface's own pixels, not the window's: the HUD is
// laid out at its composite target, which on a HiDPI display is the framebuffer
// and not the client rect.
TEST(WindowInputRoutingTests, TheHudIsPointedInItsOwnPixels)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudButtonSurface hud;
    ASSERT_TRUE(MakeHudButton(hud, f.device.get(), "router_player_hud_scale.css"));
    UsePlayerSurface(f, *hud.host, /*toSurfacePixels=*/2.0f);

    // Client (75, 60) is twice that in surface pixels — inside the button at
    // (100,100)-(200,140). Unmapped it would land far outside it.
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 75.0f, 60.0f);
    SettleHud(*hud.host);
    EXPECT_EQ(hud.Ui()->GetHoveredElement(), hud.button)
        << "the client point was not mapped into the surface's pixels";

    // And a client point whose mapped position is outside the button is outside
    // it: the mapping is applied, not merely present.
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 150.0f, 120.0f);
    SettleHud(*hud.host);
    EXPECT_NE(hud.Ui()->GetHoveredElement(), hud.button);
}

// G1 for the Player: a HUD list that actually scrolled takes the wheel, so the
// same tick does not also drive the game behind it.
TEST(WindowInputRoutingTests, AWheelTheHudTakesIsNotAlsoTheGames)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    auto host = std::make_unique<GameUIHost>(f.device.get(), /*assetManager=*/nullptr,
                                             /*jobSystem=*/nullptr);
    UIManager* hudUi = host->GetUIManager();
    ASSERT_NE(hudUi, nullptr);
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto scrollOwned = std::make_unique<ScrollView>();
    auto* scroll = scrollOwned.get();
    scroll->SetId("hud-list");
    auto contentOwned = std::make_unique<UIElement>();
    contentOwned->SetId("content");
    scroll->AddContent(std::move(contentOwned));
    root->AddChild(std::move(scrollOwned));
    hudUi->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "router_player_hud_wheel.css";
    {
        std::ofstream out(css);
        // Content taller than the list, so the wheel genuinely moves it.
        out << R"(
#hud-root { display: flex; width: 200px; height: 100px; }
#hud-list { width: 200px; height: 100px; overflow: scroll; }
#content { width: 200px; height: 1000px; }
)";
    }
    ASSERT_TRUE(hudUi->AttachStyleFromFile(css.string()));
    hudUi->Update(0.0f, /*interactive=*/false);

    // The game's raw sink sits behind the HUD, so a consumed wheel is absent
    // from it as well as from the app InputSystem.
    RecordingSink game;
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameUi = host.get();
    surface.gameplaySink = &game;
    surface.mapGameUiPointer = [](float clientX, float clientY, float& uiX, float& uiY)
    {
        uiX = clientX;
        uiY = clientY;
    };
    f.UsePlaySurface(std::move(surface));
    f.config.getUi = nullptr;

    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 20.0f, 20.0f);
    SettleHud(*host);
    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/-1.0f, /*mods=*/0);

    EXPECT_GT(scroll->GetScrollY(), 0.0f) << "the HUD list never received the wheel";
    EXPECT_TRUE(game.events.empty()) << "a wheel the HUD took must not also drive the game";
}

// A HUD floats over the world and lets the pointer through, so for most of the
// screen nothing is hovered. Chrome hands such a tick to the focused element, or
// to the first focusable one when nothing is focused; a game surface must not
// inherit that, or a list the cursor is nowhere near takes the game's zoom.
TEST(WindowInputRoutingTests, AWheelOverBareGameBackgroundIsNotTheHudsToTake)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    auto host = std::make_unique<GameUIHost>(f.device.get(), /*assetManager=*/nullptr,
                                             /*jobSystem=*/nullptr);
    UIManager* hudUi = host->GetUIManager();
    ASSERT_NE(hudUi, nullptr);
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto scrollOwned = std::make_unique<ScrollView>();
    auto* scroll = scrollOwned.get();
    scroll->SetId("hud-list");
    auto contentOwned = std::make_unique<UIElement>();
    contentOwned->SetId("content");
    // The focusable item is what the chrome fallback aims at, and bubbling from
    // it reaches the list: without the gate this is the path that steals the tick.
    auto itemOwned = std::make_unique<Button>();
    itemOwned->SetId("hud-item");
    contentOwned->AddChild(std::move(itemOwned));
    scroll->AddContent(std::move(contentOwned));
    root->AddChild(std::move(scrollOwned));
    hudUi->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "router_player_hud_wheel_world.css";
    {
        std::ofstream out(css);
        out << R"(
#hud-root { display: flex; width: 200px; height: 100px; }
#hud-list { width: 200px; height: 100px; overflow: scroll; }
#content { width: 200px; height: 1000px; }
#hud-item { width: 100px; height: 20px; }
)";
    }
    ASSERT_TRUE(hudUi->AttachStyleFromFile(css.string()));
    hudUi->Update(0.0f, /*interactive=*/false);

    RecordingSink game;
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameUi = host.get();
    surface.gameplaySink = &game;
    surface.mapGameUiPointer = [](float clientX, float clientY, float& uiX, float& uiY)
    {
        uiX = clientX;
        uiY = clientY;
    };
    f.UsePlaySurface(std::move(surface));
    f.config.getUi = nullptr;

    // Control: over the list the wheel is the HUD's, so the list is demonstrably
    // scrollable and the absence below is the routing rule rather than a dead fixture.
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 20.0f, 20.0f);
    SettleHud(*host);
    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/-1.0f, /*mods=*/0);
    ASSERT_GT(scroll->GetScrollY(), 0.0f) << "control: the HUD list must take a wheel over itself";
    ASSERT_TRUE(game.events.empty()) << "control: a wheel the HUD took is not also the game's";

    // Off the HUD: nothing hovered, nothing focused - the tick belongs to the game.
    const float parked = scroll->GetScrollY();
    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 400.0f, 400.0f);
    SettleHud(*host);
    ASSERT_EQ(hudUi->GetHoveredElement(), nullptr)
        << "the pointer must be off the HUD for this to be the world's tick";

    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/-1.0f, /*mods=*/0);
    EXPECT_FLOAT_EQ(scroll->GetScrollY(), parked)
        << "a wheel over the world moved a HUD list the cursor was nowhere near";
    EXPECT_EQ(game.events.size(), 1u) << "the game never received the wheel aimed at it";
}

// The other half of the rule: a HUD list whose content fits cannot move, so the
// tick is not its to take and the game behind it still gets to zoom.
TEST(WindowInputRoutingTests, AWheelOverAHudListThatCannotMoveIsStillTheGames)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    auto host = std::make_unique<GameUIHost>(f.device.get(), /*assetManager=*/nullptr,
                                             /*jobSystem=*/nullptr);
    UIManager* hudUi = host->GetUIManager();
    ASSERT_NE(hudUi, nullptr);
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto scrollOwned = std::make_unique<ScrollView>();
    auto* scroll = scrollOwned.get();
    scroll->SetId("hud-list");
    auto contentOwned = std::make_unique<UIElement>();
    contentOwned->SetId("content");
    scroll->AddContent(std::move(contentOwned));
    root->AddChild(std::move(scrollOwned));
    hudUi->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "router_player_hud_wheel_fits.css";
    {
        std::ofstream out(css);
        // Content shorter than the list: there is nowhere for the wheel to take it.
        out << R"(
#hud-root { display: flex; width: 200px; height: 100px; }
#hud-list { width: 200px; height: 100px; overflow: scroll; }
#content { width: 200px; height: 50px; }
)";
    }
    ASSERT_TRUE(hudUi->AttachStyleFromFile(css.string()));
    hudUi->Update(0.0f, /*interactive=*/false);
    scroll->SetContentSize(200.0f, 50.0f);

    RecordingSink game;
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameUi = host.get();
    surface.gameplaySink = &game;
    surface.mapGameUiPointer = [](float clientX, float clientY, float& uiX, float& uiY)
    {
        uiX = clientX;
        uiY = clientY;
    };
    f.UsePlaySurface(std::move(surface));
    f.config.getUi = nullptr;

    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 20.0f, 20.0f);
    SettleHud(*host);
    for (UIElement* p = hudUi->GetHoveredElement();; p = p->GetParent())
    {
        ASSERT_NE(p, nullptr) << "the pointer must be over the HUD list for this to be its tick";
        if (p == scroll)
            break;
    }

    WindowInputRouter::RouteScroll(f.config, /*dx=*/0.0f, /*dy=*/-1.0f, /*mods=*/0);
    EXPECT_FLOAT_EQ(scroll->GetScrollY(), 0.0f) << "a list with no range has nowhere to move";
    EXPECT_EQ(game.events.size(), 1u)
        << "a tick the HUD could not take must still reach the game";
}

// The editor's Game View keeps its own pointer leg: its surface names no mapping
// into the HUD's pixels, because the chrome element that owns the viewport rect
// is the only stage that knows whether the cursor is over it. This router must
// stay out of that leg — two streams in two coordinate spaces is the regression.
TEST(WindowInputRoutingTests, AnEmbeddedSurfaceIsNotPointedByTheRouter)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    HudButtonSurface hud;
    ASSERT_TRUE(MakeHudButton(hud, f.device.get(), "router_embedded_hud.css"));

    Input::InputSystem gameplay{Input::SinkRole::Gameplay};
    (void)gameplay.IsMouseButtonDown(Input::kMouseButton_Left); // poll-claim, as game code does
    WindowInputRouterConfig::PlaySurface surface;
    surface.gameUi = hud.host.get();
    surface.gameplaySink = &gameplay;
    f.UsePlaySurface(std::move(surface));

    WindowInputRouter::RouteMouseMove(f.config, /*window=*/nullptr, 150.0f, 120.0f);
    SettleHud(*hud.host);
    EXPECT_EQ(hud.Ui()->GetHoveredElement(), nullptr)
        << "an embedded surface must not be hovered from the window's pointer";

    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/true, 0);
    gameplay.Update(0.016f);
    EXPECT_TRUE(gameplay.IsMouseButtonDown(Input::kMouseButton_Left))
        << "the press must still reach the game, which claims it";

    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/false, 0);
    SettleHud(*hud.host);
    EXPECT_EQ(hud.clicks, 0) << "the router clicked a HUD its chrome was already pointing";
}

// --- Gamepad: the poll's edges walk the same chain a key does ---

namespace
{

constexpr int kPad = 0;
constexpr int kPadButtonA = static_cast<int>(Input::GamepadButton::A);
constexpr int kPadButtonB = static_cast<int>(Input::GamepadButton::B);

// The pad the router reports as present, with its sticks centred.
void RouteConnectedPad(RouterFixture& f, float leftX = 0.0f)
{
    float axes[Input::kGamepadAxisCount] = {};
    axes[static_cast<int>(Input::GamepadAxis::LeftX)] = leftX;
    WindowInputRouter::RouteGamepadState(f.config, kPad, axes, Input::kGamepadAxisCount, /*connected=*/true);
}

} // namespace

TEST(WindowInputRoutingTests, GamepadButtonTheGameClaimsNeverReachesTheEditorInput)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Editor play mode: the game reads the button every frame, which is how
    // poll-style game code declares it.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);
    RouteConnectedPad(f);
    (void)runtime.IsGamepadButtonDown(kPad, Input::GamepadButton::A);

    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonA, /*down=*/true);
    EXPECT_TRUE(runtime.IsGamepadButtonDown(kPad, Input::GamepadButton::A));
    EXPECT_FALSE(f.input.IsGamepadButtonDown(kPad, Input::GamepadButton::A))
        << "a button the game took is not also the editor's";
}

TEST(WindowInputRoutingTests, GamepadButtonTheGameIgnoresReachesTheEditorInput)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);
    RouteConnectedPad(f);

    // Nothing binds or polls B, so it keeps travelling — the same rule that lets
    // a key the game ignores still fire editor behaviour mid-play.
    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonB, /*down=*/true);
    EXPECT_FALSE(runtime.IsGamepadButtonDown(kPad, Input::GamepadButton::B));
    EXPECT_TRUE(f.input.IsGamepadButtonDown(kPad, Input::GamepadButton::B));
}

TEST(WindowInputRoutingTests, GamepadStateAndBothButtonHalvesArriveInOrder)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    RecordingSink gameplay;
    f.UseGameplaySink(gameplay);
    RouteConnectedPad(f);

    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonA, /*down=*/true);
    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonA, /*down=*/false);

    // The pad announces itself before its buttons speak, and both halves of a
    // press arrive: a release the router withheld is the stuck-button bug.
    const std::vector<std::string> expected{"gs0:1:0", "g0:0:1", "g0:0:0"};
    EXPECT_EQ(gameplay.events, expected);

    // This sink consumes nothing, so the editor's InputSystem saw the same pair.
    EXPECT_FALSE(f.input.IsGamepadButtonDown(kPad, Input::GamepadButton::A));
    EXPECT_TRUE(f.input.IsGamepadConnected(kPad));
}

TEST(WindowInputRoutingTests, GamepadStateReachesTheGameAndTheEditorAlike)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);
    RouteConnectedPad(f, /*leftX=*/0.5f);

    // A stick is state, like a pointer position: nothing consumes it, so both the
    // game reading it and the editor's camera controller see the same pad.
    EXPECT_TRUE(runtime.IsGamepadConnected(kPad));
    EXPECT_FLOAT_EQ(runtime.GetGamepadAxis(kPad, Input::GamepadAxis::LeftX), 0.5f);
    EXPECT_TRUE(f.input.IsGamepadConnected(kPad));
    EXPECT_FLOAT_EQ(f.input.GetGamepadAxis(kPad, Input::GamepadAxis::LeftX), 0.5f);
}

TEST(WindowInputRoutingTests, GamepadEdgesReachTheRuntimeSinkWithNoUiInTheWay)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // The slice's reason for existing: with the pad routed through the chain, the
    // play session's own sink hears a controller. Its button is bound the way an
    // action-mapped game binds one.
    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    runtime.BindKey(kGameplayContext, kGameplayAction, {Input::DeviceType::Gamepad, kPadButtonA, 1.0f});
    runtime.PushContext(kGameplayContext);
    f.UseGameplaySink(runtime);
    RouteConnectedPad(f);

    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonA, /*down=*/true);
    runtime.Update(0.016f);
    EXPECT_TRUE(runtime.IsActionActive(kGameplayAction));
    EXPECT_FALSE(f.input.IsGamepadButtonDown(kPad, Input::GamepadButton::A));
}

TEST(WindowInputRoutingTests, GamepadButtonReachesTheEditorInputWithNoPlaySurface)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Edit mode: no play surface, so the editor's own InputSystem ends the chain
    // and takes every press it is delivered.
    RouteConnectedPad(f);
    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonA, /*down=*/true);
    EXPECT_TRUE(f.input.IsGamepadButtonDown(kPad, Input::GamepadButton::A));

    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonA, /*down=*/false);
    EXPECT_FALSE(f.input.IsGamepadButtonDown(kPad, Input::GamepadButton::A));
}

TEST(WindowInputRoutingTests, GamepadEdgesSkipTheSurfaceUiAndChromeUi)
{
    RouterFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // A pad carries no pointer and no focus, and no UI element answers for one,
    // so a focused chrome text field cannot swallow a controller button the way
    // it swallows typing.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto fieldOwned = std::make_unique<TextField>();
    TextField* field = fieldOwned.get();
    field->SetId("field");
    root->AddChild(std::move(fieldOwned));
    f.ui->SetRoot(std::move(root));
    f.ui->SetFocusById("field");

    Input::InputSystem runtime{Input::SinkRole::Gameplay};
    f.UseGameplaySink(runtime);
    RouteConnectedPad(f);
    (void)runtime.IsGamepadButtonDown(kPad, Input::GamepadButton::A);

    WindowInputRouter::RouteGamepadButton(f.config, kPad, kPadButtonA, /*down=*/true);
    EXPECT_TRUE(runtime.IsGamepadButtonDown(kPad, Input::GamepadButton::A));
    EXPECT_TRUE(field->GetValue().empty()) << "a gamepad button is not typing";
}
