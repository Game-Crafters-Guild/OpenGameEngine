// Wiring idempotency for the Scene View toolbar.
//
// The scene view pushes its controller into the toolbar every frame, and the UI
// runs the wiring pass on every layout. Both must leave already-bound buttons
// alone: the click-handler table is additive, so a pass that re-binds stacks one
// handler per frame and a single click fires once per pass that ran.

#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/SceneViewToolbar.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <cstdint>
#include <memory>
#include <string>

using GameEngine::Button;
using GameEngine::kEventButtonClick;
using GameEngine::SceneViewToolbar;
using GameEngine::UIEventHandlerAccess;
using GameEngine::UIManager;
using GameEngine::Editor::SceneViewSettings;

namespace
{
Button* AddButton(SceneViewToolbar& toolbar, const std::string& id)
{
    auto button = std::make_unique<Button>();
    button->SetId(id);
    Button* raw = button.get();
    toolbar.AddChild(std::move(button));
    return raw;
}

// Every id SceneViewToolbar::WireControls asks for. The wiring pass skips itself only
// once ALL of them have resolved, so a fixture that wants the skip gate armed has to
// supply the whole set — a partially populated toolbar never arms it.
constexpr const char* kWiredButtonIds[] = {
    "ToolbarSpacer1", "GizmoToggle",         "GridToggle",       "SnapToggle",
    "View2DToggle",   "PickModeToggle",      "ProjectionToggle", "RotationGizmoToggle",
    "ToolOverlayToggle", "PostProcessToggle", "CameraSettingsBtn",
};

// A toolbar owned by a real UIManager, so GetOwnerManager() is non-null and the
// tree-structure generation the skip gate reads actually moves. The bare-toolbar tests
// below cannot exercise that gate at all: with no owning manager the pass never skips.
struct ManagedToolbar
{
    UIManager Manager{nullptr};
    SceneViewToolbar* Toolbar = nullptr;

    ManagedToolbar()
    {
        auto owned = std::make_unique<SceneViewToolbar>();
        Toolbar = owned.get();
        Manager.SetRoot(std::move(owned));
    }

    void PopulateAll()
    {
        for (const char* id : kWiredButtonIds)
            AddButton(*Toolbar, id);
    }

    // Drives layout passes until the wiring pass has resolved everything and armed its
    // skip gate. Two are enough by construction (pass 1 registers the ids, pass 2
    // resolves them); a third is spent to make the armed state the steady state.
    void RunPassesUntilArmed()
    {
        Toolbar->OnPostLayout();
        Toolbar->OnPostLayout();
        Toolbar->OnPostLayout();
    }
};
} // namespace

// The per-frame controller push must not touch handlers that are already bound.
//
// The trap: an observer that installs its own handler and counts its own firings CANNOT
// see a re-bind. Registrations are additive, so a second wiring pass adds the toolbar's
// handler alongside the observer's and the observer still fires exactly once. Count the
// button's subscriptions instead -- that is the quantity a re-bind actually changes.
TEST(SceneViewToolbarWiringTests, SceneControllerPushDoesNotRebindWiredButtons)
{
    SceneViewToolbar toolbar;
    Button* gizmoToggle = AddButton(toolbar, "GizmoToggle");

    toolbar.OnPostLayout(); // initial wiring pass

    // Positive control: without it, a pass that wired nothing at all would satisfy the
    // "still exactly one" assertion below by holding at zero.
    //
    // EXPECT_GE, not ASSERT_EQ, and both halves matter. Non-fatal so the discriminating
    // assertion below ALWAYS executes -- a fatal control aborts the arm on a stacked
    // fixture and the stacking count is never reported. >= 1 so a stacked fixture fails
    // on the stacking assertion under its own message rather than here under a
    // "never wired" one.
    EXPECT_GE(UIEventHandlerAccess::HandlerCount(*gizmoToggle, kEventButtonClick), 1u)
        << "the first pass never wired the button, so the re-bind assertion proves nothing";

    // What SceneViewPanel does every frame, plus the layout passes that follow it.
    toolbar.SetSceneController(nullptr);
    toolbar.OnPostLayout();
    toolbar.OnPostLayout();

    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*gizmoToggle, kEventButtonClick), 1u)
        << "the toolbar re-bound an already-wired button; on the additive handler table "
           "that stacks one subscription per pass and a single click fires N times";
}

// UXML children bind asynchronously, so the first wiring pass can run against an
// empty toolbar. Buttons that mount afterwards must still get wired.
TEST(SceneViewToolbarWiringTests, ButtonMountedAfterFirstPassIsWired)
{
    const bool savedExactPickMode = SceneViewSettings::Get().GetExactPickMode();

    SceneViewToolbar toolbar;
    toolbar.OnPostLayout(); // no children yet

    Button* pickModeToggle = AddButton(toolbar, "PickModeToggle");
    toolbar.OnPostLayout();

    // PickModeToggle's handler is the only observable proof it got bound: it flips
    // the exact-pick setting.
    pickModeToggle->TriggerClick();
    EXPECT_NE(SceneViewSettings::Get().GetExactPickMode(), savedExactPickMode)
        << "a button that mounted after the first wiring pass was never wired";

    SceneViewSettings::Get().SetExactPickMode(savedExactPickMode);
}

// The #1084 late-mount hazard re-asked under a real UIManager, which is the only
// configuration in which the skip gate can arm at all.
//
// Note what this arm does NOT prove. The gate never arms here — arming requires every
// wanted id to have resolved, and this toolbar is empty — so no pass is ever skipped
// and the assertion cannot fail by stranding. That is not a gap in the test but the
// arming rule doing its job: "the gate skipped a pass on which a wanted button was
// still missing" is unreachable by construction. What this arm holds down is that a
// toolbar under a manager wires a late mount at all, on the path where GetOwnerManager()
// is non-null and the generation is live. ReplacedButtonIsRewiredAfterTheSkipGateArms
// below is the arm that exercises an armed gate.
TEST(SceneViewToolbarWiringTests, LateMountedButtonIsWiredWithAnOwningManager)
{
    const bool savedExactPickMode = SceneViewSettings::Get().GetExactPickMode();

    ManagedToolbar fixture;
    fixture.RunPassesUntilArmed(); // no children yet

    Button* pickModeToggle = AddButton(*fixture.Toolbar, "PickModeToggle");
    fixture.Toolbar->OnPostLayout();

    pickModeToggle->TriggerClick();
    EXPECT_NE(SceneViewSettings::Get().GetExactPickMode(), savedExactPickMode)
        << "a button that mounted after the wiring pass settled was never wired";

    SceneViewSettings::Get().SetExactPickMode(savedExactPickMode);
}

// The case the generation gate is actually armed for: every wanted id has resolved, so
// passes are being skipped — and then a button is replaced. Removal and re-add both
// bump the generation, so the replacement must be wired rather than inheriting the
// wired-instance stamp of the element it replaced.
TEST(SceneViewToolbarWiringTests, ReplacedButtonIsRewiredAfterTheSkipGateArms)
{
    const bool savedExactPickMode = SceneViewSettings::Get().GetExactPickMode();

    ManagedToolbar fixture;
    fixture.PopulateAll();
    fixture.RunPassesUntilArmed();

    // Positive control: the fixture really did wire the original button. Without this a
    // broken pass would make the assertion below pass for the wrong reason.
    Button* original = dynamic_cast<Button*>(fixture.Toolbar->FindById("PickModeToggle"));
    ASSERT_NE(original, nullptr);
    original->TriggerClick();
    ASSERT_NE(SceneViewSettings::Get().GetExactPickMode(), savedExactPickMode)
        << "the fixture never wired the original button, so the replacement arm proves nothing";
    SceneViewSettings::Get().SetExactPickMode(savedExactPickMode);

    const std::uint64_t originalInstanceId = original->GetInstanceId();
    fixture.Toolbar->RemoveChild(original);
    Button* replacement = AddButton(*fixture.Toolbar, "PickModeToggle");
    // Compared by instance id, not by address: the allocator hands the replacement the
    // block the original just freed, so the two are routinely the same pointer. That
    // address reuse is exactly why the wired-once guard keys on the instance id.
    ASSERT_NE(replacement->GetInstanceId(), originalInstanceId);
    fixture.Toolbar->OnPostLayout();

    replacement->TriggerClick();
    EXPECT_NE(SceneViewSettings::Get().GetExactPickMode(), savedExactPickMode)
        << "a replacement button was never wired — the skip gate stranded it";

    SceneViewSettings::Get().SetExactPickMode(savedExactPickMode);
}

// Idempotency under the armed gate: once everything is wired, further passes must bind
// nothing at all. Eight passes, so a per-pass leak shows up as eight extra subscriptions
// rather than one and cannot be mistaken for an off-by-one.
TEST(SceneViewToolbarWiringTests, SettledPassesRebindNothing)
{
    ManagedToolbar fixture;
    fixture.PopulateAll();
    fixture.RunPassesUntilArmed();

    Button* gizmoToggle = dynamic_cast<Button*>(fixture.Toolbar->FindById("GizmoToggle"));
    ASSERT_NE(gizmoToggle, nullptr);

    // Positive control: the fixture wired it at all, so "still exactly one" below is a
    // preserved binding rather than an absent one. Non-fatal and >= 1 for the same two
    // reasons as the arm above: the 8-pass stacking assertion must always run, and a
    // fixture that already stacked must fail under the stacking message, not this one.
    EXPECT_GE(UIEventHandlerAccess::HandlerCount(*gizmoToggle, kEventButtonClick), 1u)
        << "the fixture never wired GizmoToggle, so this arm proves nothing";

    for (int pass = 0; pass < 8; ++pass)
    {
        fixture.Toolbar->SetSceneController(nullptr);
        fixture.Toolbar->OnPostLayout();
    }

    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*gizmoToggle, kEventButtonClick), 1u)
        << "a settled wiring pass re-bound an already-wired button: one extra "
           "subscription per pass, so a single click would fire once per pass";
}
