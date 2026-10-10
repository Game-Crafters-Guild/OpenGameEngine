// GameUIHost interactivity pins: the pointer must reach the host's UIManager
// and opt Update into interactive mode, so a HUD subtree receives hover
// (enter/leave) and press/release events — the play-mode input path the Player
// and editor Game View drive. Both entries are pinned here: the sampled one the
// editor's viewport leg calls (SetPointer, through the test seam) and the event
// entries a router calls when its window is the surface (the Player).
// Headless: geometry is declared through GameUIHost::RenderRG (the real
// per-frame build) but never executed.
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <gtest/gtest.h>

#include "GameUIHostTestAccess.h"

#include "Engine/GameUI/GameUIHost.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Engine/GameUI/SessionEvent.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ScrollView.h"
#include "Rendering/Core/Device.h"
#include "UIRgTestHarness.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

// ECS + AssetManager for the SyncFromWorld integration tests (layout swap + multi-doc).
// JobSystem types must precede the ECS Query/World templates (Query.h uses
// JobSystem::TaskHandle), mirroring GameUIHost.cpp's own include ordering.
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "ECS/ECS.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "Assets/AssetManager.h"
#include "Components/UI/UIDocument.h"
#include "AssetCore/AssetEvents.h" // AssetEvent/AssetEventType for the hot-reload drive
#include "AssetCore/GUID.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

TEST(GameUIHostTests, SetPointerDrivesInteractiveHoverAndClick)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    // assetManager/jobSystem are null: this test exercises only the interactive
    // input path (no document binding, which is what those collaborators serve).
    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><uielement id='btn' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_interactive.css";
    {
        std::ofstream f(css);
        f << "#root { position: relative; width: 200px; height: 200px; }\n"
             "#btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    // First layout + geometry, display-only (no pointer fed yet → non-interactive).
    // Geometry is built via the harness's exe-side UIManager::RenderRG (which has
    // the test shader-path resolver) on the SAME UIManager the host drives — the
    // host's own RenderRG runs in Engine.dll, whose Utils resolver this exe can't
    // reach. Interactivity (SetPointer/Update) is still driven through the host.
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    UIElement* btn = ui->GetRootElement()->FindById("btn");
    ASSERT_NE(btn, nullptr);
    int enter = 0, leave = 0, down = 0, up = 0;
    btn->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++enter; });
    btn->RegisterEventHandler(kEventMouseLeave, [&](UIEvent&) { ++leave; });
    btn->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++down; });
    btn->RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { ++up; });

    // Pointer fed but outside the button: opts into interactive, no hover yet.
    GameUIHostTestAccess::SetPointer(host, 5.0f, 5.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(enter, 0);

    // Hover the button center (80, 60): exactly one MouseEnter.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(enter, 1);
    EXPECT_EQ(leave, 0);

    // Press then release inside: MouseDown then MouseUp dispatched to the button.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(down, 1);
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(up, 1);

    // Move away: exactly one MouseLeave.
    GameUIHostTestAccess::SetPointer(host, 5.0f, 5.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(leave, 1);
}

// A press that the pointer walks out on must not become a click.
//
// Button::OnEvent captures on MouseDown ("receive MouseUp even if pointer leaves"),
// and UIManager::OnCursorEnter(false) deliberately PRESERVES the cursor position
// while captured — correct for the editor chrome, where the OS keeps delivering
// moves and the real release. The Game View viewport has no such stream: once the
// cursor is outside, this UIManager never hears from it again. So a synthesised
// release resolved at the last known position lands inside the armed button and
// activates it — the user let go somewhere else entirely, and the HUD button fired.
//
// The release edge must still reach the tree (controls clear drag state on MouseUp);
// what must not survive is its reading as a click-completing release.
TEST(GameUIHostTests, CursorLeavingSurfaceWhileArmedDoesNotClick)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><button id='btn' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_leave_armed.css";
    {
        std::ofstream f(css);
        f << "#root { position: relative; width: 200px; height: 200px; }\n"
             "#btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    auto* btn = dynamic_cast<Button*>(ui->GetRootElement()->FindById("btn"));
    ASSERT_NE(btn, nullptr) << "fixture must mount a real Button — the armed model under test";
    ASSERT_GT(btn->GetLayoutWidth(), 0.0f) << "button has no layout — the pointer could never hit it";

    int clicks = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });
    // A raw MouseUp subscriber stands in for the HUD scripts that bind one directly
    // (HpBarSystem, the session-click gate): position-blind, and it treats anything it
    // receives as "the player released this button".
    int mouseUps = 0;
    int cancels = 0;
    float cancelX = 0.0f, cancelY = 0.0f;
    btn->RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { ++mouseUps; });
    int buttonClicks = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++buttonClicks; });
    btn->RegisterEventHandler(kEventMouseCancel,
                              [&](UIEvent& e)
                              {
                                  ++cancels;
                                  cancelX = e.X;
                                  cancelY = e.Y;
                              });

    // Press over the button centre (80, 60) and hold: armed, pressed visual on.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    ASSERT_TRUE(btn->HasClass("pressed")) << "press did not arm the button — the rest proves nothing";
    ASSERT_EQ(clicks, 0);

    // Cursor leaves the host surface while still held. The last position UIManager
    // has is (80,60) — inside the button.
    GameUIHostTestAccess::PointerLeftSurface(host);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);

    EXPECT_EQ(clicks, 0) << "leaving the surface mid-press fired a phantom click at the stale "
                            "in-window pointer position";
    EXPECT_EQ(mouseUps, 0) << "the abandoned press was delivered as a real MouseUp — every "
                              "position-blind subscriber reads that as a click";
    EXPECT_EQ(buttonClicks, 0) << "UI.ButtonClick fired on a cancelled gesture — the activation "
                                  "path was reached without a release";
    EXPECT_EQ(cancels, 1) << "the gesture was never terminated, so armed controls keep their state";
    EXPECT_FALSE(btn->HasClass("pressed")) << "pressed visual latched after the pointer left";
    EXPECT_FALSE(ui->IsMouseCaptured()) << "the abandoned press still holds UIManager capture, so "
                                           "hit-testing stays overridden for every later event";

    // No release position exists, so nothing may find one on the cancel event.
    EXPECT_LT(cancelX, 0.0f) << "cancel carried a live X (" << cancelX
                             << ") — a control gating on 'inside my rect' would activate";
    EXPECT_LT(cancelY, 0.0f) << "cancel carried a live Y (" << cancelY << ")";

    // Positive control: the fixture CAN produce a click, so the assertions above are
    // not passing vacuously. A fresh press+release inside still activates exactly once.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 1) << "a normal click stopped working after a leave-cancelled press";
    EXPECT_EQ(buttonClicks, 1) << "UI.ButtonClick stopped firing for a real click";
}

// The damaging consequence of an unterminated gesture: UIManager's capture is still
// held by the button the pointer walked out on, and capture outranks hit-testing for
// both the press and the release target. Every other control in the HUD is therefore
// dead — the user's next click goes to the abandoned button instead.
TEST(GameUIHostTests, LeavingMidPressDoesNotStealLaterClicksFromOtherControls)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><button id='a' /><button id='b' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_leave_steal.css";
    {
        std::ofstream f(css);
        f << "#root { position: relative; width: 200px; height: 200px; }\n"
             "#a { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n"
             "#b { position: absolute; left: 40px; top: 120px; width: 80px; height: 40px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    auto* a = dynamic_cast<Button*>(ui->GetRootElement()->FindById("a"));
    auto* b = dynamic_cast<Button*>(ui->GetRootElement()->FindById("b"));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_GT(b->GetLayoutWidth(), 0.0f);

    int aClicks = 0, bClicks = 0;
    a->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++aClicks; });
    b->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++bClicks; });

    // Press and hold A (centre 80,60), then walk the pointer out of the surface.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    ASSERT_TRUE(a->HasClass("pressed"));
    GameUIHostTestAccess::PointerLeftSurface(host);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);

    EXPECT_FALSE(ui->IsMouseCaptured()) << "the abandoned press still holds UIManager capture";

    // Come back and click B — a complete, unambiguous gesture on a different control.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 140.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    GameUIHostTestAccess::SetPointer(host, 80.0f, 140.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    GameUIHostTestAccess::SetPointer(host, 80.0f, 140.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);

    EXPECT_EQ(bClicks, 1) << "the click landed somewhere other than the button under the pointer";
    EXPECT_EQ(aClicks, 0) << "the abandoned button consumed a gesture aimed at another control";
}

// The stale gesture must not revive when the cursor comes back over the button it was
// abandoned on: the user released the mouse outside the surface, where this host can
// neither see it nor be told about it.
TEST(GameUIHostTests, ReturningOverTheButtonAfterLeavingMidPressDoesNotClick)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><button id='btn' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_leave_return.css";
    {
        std::ofstream f(css);
        f << "#root { position: relative; width: 200px; height: 200px; }\n"
             "#btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    auto* btn = dynamic_cast<Button*>(ui->GetRootElement()->FindById("btn"));
    ASSERT_NE(btn, nullptr);
    ASSERT_GT(btn->GetLayoutWidth(), 0.0f);

    int clicks = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });

    // Press and hold over the button centre.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    ASSERT_TRUE(btn->HasClass("pressed"));

    // Leave the surface while held; the user releases the button out there, where
    // this host can neither see it nor be told about it.
    GameUIHostTestAccess::PointerLeftSurface(host);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 0) << "click fired on the leave frame itself";

    // Cursor comes back over the button, no button held.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 0) << "re-entering over the button completed a press the user "
                            "already released outside the surface";

    // And the gesture is genuinely over, not merely delayed again: an idle frame
    // over the button stays quiet.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 0);
}

// ---------------------------------------------------------------------------
// The pointer as events, which is how a host whose whole window is the surface
// receives it (the Player, through the router). The sampled entry above is
// differenced into exactly these calls, so both sets of rows pin one machine.
// ---------------------------------------------------------------------------

// The answer the router routes on: a press a HUD control acted on stops there
// and never reaches the game, and a press on bare HUD background does not.
TEST(GameUIHostTests, APressAHudControlActsOnAnswersConsumed)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><button id='btn' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_press_answer.css";
    {
        std::ofstream f(css);
        f << "#root { position: relative; width: 200px; height: 200px; }\n"
             "#btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    auto* btn = dynamic_cast<Button*>(ui->GetRootElement()->FindById("btn"));
    ASSERT_NE(btn, nullptr);
    int clicks = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });

    host.OnMouseMove({80.0f, 60.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_TRUE(host.OnMouseButton(Input::kMouseButton_Left, true, 0))
        << "the button under the pointer acted on the press";
    host.OnMouseButton(Input::kMouseButton_Left, false, 0);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 1) << "and the gesture completed as a click";

    // Bare background: nothing acts, so the press keeps travelling to the game.
    host.OnMouseMove({5.0f, 5.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_FALSE(host.OnMouseButton(Input::kMouseButton_Left, true, 0))
        << "a press on empty HUD background must not be claimed";
    host.OnMouseButton(Input::kMouseButton_Left, false, 0);
}

// G2: every button edge reaches the surface. A HUD that wants the right button
// (a context menu, a cancel) gets it like any other.
TEST(GameUIHostTests, EveryButtonReachesTheSurfaceNotOnlyThePrimary)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><uielement id='slot' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_every_button.css";
    {
        std::ofstream f(css);
        f << "#root { position: relative; width: 200px; height: 200px; }\n"
             "#slot { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    UIElement* slot = ui->GetRootElement()->FindById("slot");
    ASSERT_NE(slot, nullptr);
    int down = 0, up = 0;
    slot->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++down; });
    slot->RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { ++up; });

    host.OnMouseMove({80.0f, 60.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);

    host.OnMouseButton(Input::kMouseButton_Right, true, 0);
    host.OnMouseButton(Input::kMouseButton_Right, false, 0);
    EXPECT_EQ(down, 1) << "the right button never reached the HUD";
    EXPECT_EQ(up, 1);

    host.OnMouseButton(Input::kMouseButton_Middle, true, 0);
    host.OnMouseButton(Input::kMouseButton_Middle, false, 0);
    EXPECT_EQ(down, 2) << "the middle button never reached the HUD";
    EXPECT_EQ(up, 2);

    // The primary still carries the gesture the others do not: a press after
    // them arms and completes normally.
    host.OnMouseButton(Input::kMouseButton_Left, true, 0);
    host.OnMouseButton(Input::kMouseButton_Left, false, 0);
    EXPECT_EQ(down, 3);
    EXPECT_EQ(up, 3);
}

// G1: a HUD list that actually scrolled answers the wheel, which is what keeps
// the same tick from also driving the game's zoom.
TEST(GameUIHostTests, AWheelIsAnsweredByTheHudScrollableThatMoved)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto scrollOwned = std::make_unique<ScrollView>();
    auto* scroll = scrollOwned.get();
    scroll->SetId("scroll");
    auto contentOwned = std::make_unique<UIElement>();
    contentOwned->SetId("content");
    scroll->AddContent(std::move(contentOwned));
    root->AddChild(std::move(scrollOwned));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_wheel.css";
    {
        std::ofstream f(css);
        // Content taller than the viewport: an unscrollable list declines the
        // wheel, which would make the consumed arm vacuous.
        f << "#root { display: flex; width: 200px; height: 100px; }\n"
             "#scroll { width: 200px; height: 100px; overflow: scroll; }\n"
             "#content { width: 200px; height: 1000px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 100);
    host.OnMouseMove({scroll->GetLayoutX() + 10.0f, scroll->GetLayoutY() + 10.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 100);

    EXPECT_TRUE(host.OnScroll({0.0f, -1.0f})) << "the scrollable under the pointer took the wheel";
    EXPECT_GT(scroll->GetScrollY(), 0.0f) << "and it actually moved";
}

// The event path has to unwind a cancelled gesture the same way the sampled one
// does: the release the source eventually makes is not this surface's to act on.
TEST(GameUIHostTests, ACancelledGestureSwallowsTheReleaseThatEndsIt)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><button id='btn' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_cancel_event.css";
    {
        std::ofstream f(css);
        f << "#root { position: relative; width: 200px; height: 200px; }\n"
             "#btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    auto* btn = dynamic_cast<Button*>(ui->GetRootElement()->FindById("btn"));
    ASSERT_NE(btn, nullptr);
    int clicks = 0, downs = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });
    btn->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++downs; });

    host.OnMouseMove({80.0f, 60.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    ASSERT_TRUE(host.OnMouseButton(Input::kMouseButton_Left, true, 0));
    ASSERT_TRUE(btn->HasClass("pressed"));
    ASSERT_EQ(downs, 1);

    // The cursor leaves the window while the button is still held.
    host.CancelPointer(GameUIHost::PrimaryButtonState::Held);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);

    // Still holding, the source comes back over the control the cancel disarmed.
    // Hover follows the cursor again — but nothing the source does before it lets
    // go is this surface's, so a second button pressed during the abandoned
    // gesture must not re-arm the control.
    host.OnMouseMove({80.0f, 60.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_FALSE(host.OnMouseButton(Input::kMouseButton_Right, true, 0));
    EXPECT_EQ(downs, 1) << "a press during the cancelled gesture reached the HUD";
    host.OnMouseButton(Input::kMouseButton_Right, false, 0);

    EXPECT_FALSE(host.OnMouseButton(Input::kMouseButton_Left, false, 0))
        << "the release of a cancelled gesture is not the surface's to act on";
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 0) << "the abandoned press became a click";

    // And the surface is armed again for the next real gesture.
    host.OnMouseMove({80.0f, 60.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    host.OnMouseButton(Input::kMouseButton_Left, true, 0);
    host.OnMouseButton(Input::kMouseButton_Left, false, 0);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 1) << "the surface stayed deaf after the cancelled gesture ended";
}

// REPRO: mirrors the exact GameUIHost subtree path — empty root + a per-entity
// subtree child + an ELEMENT-level stylesheet (the AttachStyleToSubtreeFromAsset
// mechanism) carrying .slot:hover — and asserts the RESOLVED background flips on
// hover (not just that events fire). This isolates the reported bug: :hover style
// not applying to the game HUD.
TEST(GameUIHostTests, SubtreeHoverStyleApplies)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    // Empty root (what GameUIHost's ctor installs).
    ui->SetRoot(std::make_unique<UIElement>());

    // Per-entity subtree child, mirroring SyncFromWorld's st.Root + a .slot leaf.
    std::unique_ptr<UIElement> sub;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='sub'><uielement id='slot' class='slot' /></uielement>)", sub));
    UIElement* subRoot = sub.get();
    ui->GetRootElement()->AddChild(std::move(sub));

    // Element-level stylesheet on the subtree root (what AttachStyleToSubtreeFromAsset does).
    Stylesheet sheet;
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        "#sub { position: relative; width: 200px; height: 200px; }\n"
        ".slot { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; "
        "background-color: #112233; }\n"
        ".slot:hover { background-color: #ff0000; }\n",
        sheet));
    subRoot->AddStylesheet(std::make_shared<Stylesheet>(std::move(sheet)));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    UIElement* slot = ui->GetRootElement()->FindById("slot");
    ASSERT_NE(slot, nullptr);
    EXPECT_GT(slot->GetLayoutWidth(), 0.0f) << "slot has zero layout — subtree not laid out";

    const uint32_t base = slot->GetResolvedStyle().Visual.BackgroundColor;

    // Hover the slot center (80,60).
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    const uint32_t hovered = slot->GetResolvedStyle().Visual.BackgroundColor;

    EXPECT_NE(hovered, base) << "background did NOT change on hover (base=0x" << std::hex << base
                            << " hovered=0x" << hovered << ")";
}

// Companion for :active — the user's exact gesture: hover an element, THEN hold the
// mouse button. The pressed (already-hovered) element must re-cascade with :active
// and its resolved background must change. This isolates the press-on-hovered-element
// path (no hover transition on the press frame), which is where :active was dropped.
TEST(GameUIHostTests, SubtreeActiveStyleApplies)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    ui->SetRoot(std::make_unique<UIElement>());

    std::unique_ptr<UIElement> sub;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='sub'><uielement id='slot' class='slot' /></uielement>)", sub));
    UIElement* subRoot = sub.get();
    ui->GetRootElement()->AddChild(std::move(sub));

    Stylesheet sheet;
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        "#sub { position: relative; width: 200px; height: 200px; }\n"
        ".slot { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; "
        "background-color: #112233; }\n"
        ".slot:hover { background-color: #ff0000; }\n"
        ".slot:active { background-color: #00ff00; }\n",
        sheet));
    subRoot->AddStylesheet(std::make_shared<Stylesheet>(std::move(sheet)));

    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*ui, rg);

    UIElement* slot = ui->GetRootElement()->FindById("slot");
    ASSERT_NE(slot, nullptr);

    // 1) Hover the slot (no button) — establishes :hover, like the user did first.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    const uint32_t hovered = slot->GetResolvedStyle().Visual.BackgroundColor;

    // 2) HOLD the primary button while still over the slot — :active must win.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    const uint32_t active = slot->GetResolvedStyle().Visual.BackgroundColor;

    EXPECT_NE(active, hovered) << "background did NOT change on press/hold (active=0x" << std::hex
                              << active << " still hovered=0x" << hovered << ")";
}

// Pins that GameUIHost::Update's layout-size override drives the layout extent in
// the DEFAULT (non-fast-path) build — a HUD composited into a sub-window panel
// must lay out at the panel extent, not the swapchain. The headless device
// reports a 0x0 swapchain, so without the override the heavy path would fall back
// to its 1280x720 default; the override must win.
TEST(GameUIHostTests, LayoutSizeOverrideDrivesLayoutExtent)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><uielement id='fill' /></uielement>)", root));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_override.css";
    {
        std::ofstream f(css);
        f << "#root { width: 100%; height: 100%; }\n#fill { width: 100%; height: 100%; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    GameUIHostTestAccess::Update(host, 0.0f, 640, 480);

    UIElement* r = ui->GetRootElement();
    ASSERT_NE(r, nullptr);
    EXPECT_FLOAT_EQ(r->GetLayoutWidth(), 640.0f);
    EXPECT_FLOAT_EQ(r->GetLayoutHeight(), 480.0f);
}

// --- SyncFromWorld integration: real ECS world + AssetManager + .uxml assets ----
// These drive the production reconcile path (the prior tests hand-built subtrees).

static std::filesystem::path WriteTempUxml(const std::filesystem::path& dir, const char* name, const char* xml)
{
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream f(path);
    f << xml;
    return path;
}

// Register two distinct one-leaf layouts (leafA / leafB) and pre-warm them so the
// host's GetAsset poll resolves synchronously on the first SyncFromWorld.
static void SetupTwoLayouts(AssetManager& assets, const std::filesystem::path& root, GUID& outA, GUID& outB)
{
    const auto pathA = WriteTempUxml(root, "layout_a.uxml",
                                     "<UIElement id=\"docroot\" class=\"layout-a\"><UIElement id=\"leafA\" /></UIElement>\n");
    const auto pathB = WriteTempUxml(root, "layout_b.uxml",
                                     "<UIElement id=\"docroot\" class=\"layout-b\"><UIElement id=\"leafB\" /></UIElement>\n");
    auto& reg = assets.GetRegistry();
    reg.RegisterAsset(pathA);
    reg.RegisterAsset(pathB);
    outA = reg.GetAssetGUID(pathA);
    outB = reg.GetAssetGUID(pathB);
    assets.LoadAssetAsync(outA, AssetLoadPriority::Normal).get();
    assets.LoadAssetAsync(outB, AssetLoadPriority::Normal).get();
}

// REPRO: replacing the Layout reference on a live UIDocument must swap the rendered
// subtree. The bug: ReconcileChildren minted a fresh bindingId per layout, so the old
// layout's children (stamped with the prior id) were mistaken for runtime-added nodes
// and re-attached — leaving stale content behind. An empty (first) assign worked, so
// the symptom was "replace doesn't change the UI". The fix keeps the binding id stable
// per target across swaps.
TEST(GameUIHostTests, LayoutSwapReplacesSubtree)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_host_layoutswap";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);
    ASSERT_FALSE(guidA.IsNull());
    ASSERT_FALSE(guidB.IsNull());

    GameUIHost host(dev.get(), &assets, &pool);

    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(guidA);
    auto e = world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    UIElement* uiRoot = host.GetUIManager()->GetRootElement();
    ASSERT_NE(uiRoot, nullptr);
    ASSERT_NE(uiRoot->FindById("leafA"), nullptr) << "layout A did not bind on first assign";
    ASSERT_FALSE(uiRoot->GetChildren().empty());
    UIElement* container = uiRoot->GetChildren().front().get(); // the per-entity subtree root
    EXPECT_TRUE(container->HasClass("layout-a")) << "layout A root class not applied";

    // Replace the layout reference A -> B (the user's gesture).
    auto* live = e.GetForWrite<Components::UIDocument>();
    ASSERT_NE(live, nullptr);
    live->Layout.Set(guidB);

    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_EQ(uiRoot->FindById("leafA"), nullptr) << "stale layout A content survived the swap";
    EXPECT_NE(uiRoot->FindById("leafB"), nullptr) << "layout B did not bind after the swap";
    // The previous layout's root class must not linger (else stale CSS still matches).
    EXPECT_FALSE(container->HasClass("layout-a")) << "stale layout A root class survived the swap";
    EXPECT_TRUE(container->HasClass("layout-b")) << "layout B root class not applied after the swap";
}

// Multiple UIDocument entities each map to one direct child of the shared UI root
// (composed by SortOrder); disabling a document removes its subtree (mark-and-sweep).
TEST(GameUIHostTests, MultipleDocumentsCompose)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_host_multidoc";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);

    GameUIHost host(dev.get(), &assets, &pool);

    ECS::World world(&pool);
    Components::UIDocument doc1;
    doc1.Layout.Set(guidA);
    doc1.SortOrder = 0;
    Components::UIDocument doc2;
    doc2.Layout.Set(guidB);
    doc2.SortOrder = 10;
    world.Create<Components::UIDocument>(doc1);
    auto e2 = world.Create<Components::UIDocument>(doc2);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    UIElement* uiRoot = host.GetUIManager()->GetRootElement();
    ASSERT_NE(uiRoot, nullptr);
    EXPECT_EQ(uiRoot->GetChildren().size(), 2u) << "expected one subtree per UIDocument";
    EXPECT_NE(uiRoot->FindById("leafA"), nullptr) << "document 1 did not bind";
    EXPECT_NE(uiRoot->FindById("leafB"), nullptr) << "document 2 did not bind";

    // Disable document 2 -> its subtree is swept on the next sync.
    e2.SetEnabled<Components::UIDocument>(false);
    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_EQ(uiRoot->GetChildren().size(), 1u) << "disabled document's subtree was not removed";
    EXPECT_NE(uiRoot->FindById("leafA"), nullptr);
    EXPECT_EQ(uiRoot->FindById("leafB"), nullptr);
}

// RenderRG hands the composite a Clear when a Fullscreen document owns the target. That
// verdict has to wait for the document's layout to bind: a Fullscreen menu that is still
// loading has nothing to draw, so clearing on RenderMode alone blanks the frame for the
// duration of the load — and forever when the layout GUID names a missing or wrong-type
// asset, since LayoutFailed latches and content never arrives. Both arms are asserted so
// a predicate that simply never fires cannot pass this.
TEST(GameUIHostTests, FullscreenCoversTargetOnlyOnceItsLayoutIsBound)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_host_fs_readiness";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);

    // A CSS asset in the Layout slot never binds as a layout: pending leaves LayoutApplied
    // false, resolved latches LayoutFailed on the type mismatch. Not ready either way, so
    // the case is deterministic regardless of async load timing.
    const auto cssPath = root / "not_a_layout.css";
    {
        std::ofstream f(cssPath);
        f << "#x { width: 1px; }\n";
    }
    auto& reg = assets.GetRegistry();
    reg.RegisterAsset(cssPath);
    const GUID cssGuid = reg.GetAssetGUID(cssPath);
    ASSERT_FALSE(cssGuid.IsNull());

    GameUIHost host(dev.get(), &assets, &pool);
    ECS::World world(&pool);

    Components::UIDocument menu;
    menu.Layout.Set(cssGuid);
    menu.RenderMode = Components::UIRenderMode::Fullscreen;
    auto menuE = world.Create<Components::UIDocument>(menu);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_FALSE(GameUIHostTestAccess::AnyReadyFullscreenSubtree(host))
        << "a Fullscreen document whose layout never binds must not win the composite Clear — "
           "it would blank the frame with nothing to draw in its place";

    // Positive control: the same document with a layout that does bind must cover.
    menuE.GetForWrite<Components::UIDocument>()->Layout.Set(guidA);
    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_TRUE(GameUIHostTestAccess::AnyReadyFullscreenSubtree(host))
        << "a bound Fullscreen document must own the target";
}

// A Fullscreen document owns the screen, so Overlay (HUD) documents are hidden while
// one is ready. Without this a HUD with a higher SortOrder draws ON TOP of the menu
// that is supposed to have replaced it. The suppression is display:none on the
// host-owned container, so it must lift again the moment the menu goes away — here via
// the disable path, which removes the menu's subtree.
TEST(GameUIHostTests, ReadyFullscreenSuppressesOverlayDocuments)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_host_overlay_suppress";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);

    GameUIHost host(dev.get(), &assets, &pool);
    ECS::World world(&pool);

    Components::UIDocument hud; // Overlay HUD, deliberately the HIGHER SortOrder
    hud.Layout.Set(guidA);
    hud.SortOrder = 10;
    hud.RenderMode = Components::UIRenderMode::Overlay;
    Components::UIDocument menu;
    menu.Layout.Set(guidB);
    menu.SortOrder = 0;
    menu.RenderMode = Components::UIRenderMode::Fullscreen;
    const auto hudE = world.Create<Components::UIDocument>(hud);
    auto menuE = world.Create<Components::UIDocument>(menu);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    UIElement* hudRoot = host.FindDocumentRoot(hudE.GetHandle().id);
    UIElement* menuRoot = host.FindDocumentRoot(menuE.GetHandle().id);
    ASSERT_NE(hudRoot, nullptr);
    ASSERT_NE(menuRoot, nullptr);
    ASSERT_TRUE(GameUIHostTestAccess::AnyReadyFullscreenSubtree(host))
        << "fixture precondition: the menu's layout must have bound";
    EXPECT_EQ(hudRoot->Overrides().Get(Style::Display), std::optional<DisplayMode>(DisplayMode::None))
        << "an Overlay HUD must be hidden while a ready Fullscreen menu owns the screen";
    EXPECT_FALSE(menuRoot->Overrides().Get(Style::Display).has_value())
        << "the Fullscreen document itself must never be suppressed";

    // The menu goes away (disable -> its subtree is removed): the HUD comes back.
    menuE.SetEnabled<Components::UIDocument>(false);
    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_EQ(host.FindDocumentRoot(menuE.GetHandle().id), nullptr)
        << "fixture precondition: the disabled menu's subtree must be gone";
    EXPECT_FALSE(hudRoot->Overrides().Get(Style::Display).has_value())
        << "the Overlay HUD must be restored once no Fullscreen document is ready";
}

// CoversTargetOpaque is the verdict a host that OWNS its target acts on to stop
// producing what the menu hides (the Player drops the whole 3D scene on it). All three
// arms matter: a ready menu covers; a menu that cannot draw does not (skipping the
// scene for it would present black for the whole load, or forever on a bad GUID); and a
// host compositing onto a target it doesn't own never covers, however ready the menu
// is, because it composites with Load and the content behind it stays visible.
TEST(GameUIHostTests, CoversTargetOpaqueArms)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_host_covers_target";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);

    const auto cssPath = root / "not_a_layout.css";
    {
        std::ofstream f(cssPath);
        f << "#x { width: 1px; }\n";
    }
    auto& reg = assets.GetRegistry();
    reg.RegisterAsset(cssPath);
    const GUID cssGuid = reg.GetAssetGUID(cssPath);
    ASSERT_FALSE(cssGuid.IsNull());

    GameUIHost host(dev.get(), &assets, &pool);
    ECS::World world(&pool);

    Components::UIDocument menu;
    menu.Layout.Set(cssGuid); // never binds -> not ready
    menu.RenderMode = Components::UIRenderMode::Fullscreen;
    auto menuE = world.Create<Components::UIDocument>(menu);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_FALSE(host.CoversTargetOpaque())
        << "a Fullscreen menu that cannot draw must not let its host skip what it hides";

    menuE.GetForWrite<Components::UIDocument>()->Layout.Set(guidA);
    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_TRUE(host.CoversTargetOpaque()) << "a bound Fullscreen menu covers its host's target";

    // A host that composites onto a target it doesn't own never clears, so it never
    // covers — the same document set, the opposite verdict.
    GameUIHostTestAccess::SetNeverClearTarget(host, true);
    EXPECT_TRUE(GameUIHostTestAccess::AnyReadyFullscreenSubtree(host))
        << "fixture precondition: the menu is still ready; only target ownership changed";
    EXPECT_FALSE(host.CoversTargetOpaque())
        << "a host compositing onto a target it doesn't own must never report coverage";

    // And removing the menu un-covers a host that does own its target.
    GameUIHostTestAccess::SetNeverClearTarget(host, false);
    menuE.SetEnabled<Components::UIDocument>(false);
    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_FALSE(host.CoversTargetOpaque())
        << "coverage must lift when the menu goes away, or the scene never comes back";
}

// The readiness gate, from the suppression side. A Fullscreen menu that cannot draw
// must not take the HUD away: hiding the HUD the instant a menu entity appears leaves
// the player with a blank screen for the whole load, and forever on a bad layout GUID.
// The menu's Layout points at a CSS asset, which never binds as a layout whether the
// load is still pending (LayoutApplied false) or has resolved (LayoutFailed latches),
// so the not-ready arm is deterministic regardless of async timing.
TEST(GameUIHostTests, NotReadyFullscreenDoesNotSuppressOverlay)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_host_overlay_notready";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);

    const auto cssPath = root / "not_a_layout.css";
    {
        std::ofstream f(cssPath);
        f << "#x { width: 1px; }\n";
    }
    auto& reg = assets.GetRegistry();
    reg.RegisterAsset(cssPath);
    const GUID cssGuid = reg.GetAssetGUID(cssPath);
    ASSERT_FALSE(cssGuid.IsNull());

    GameUIHost host(dev.get(), &assets, &pool);
    ECS::World world(&pool);

    Components::UIDocument hud;
    hud.Layout.Set(guidA);
    hud.SortOrder = 10;
    hud.RenderMode = Components::UIRenderMode::Overlay;
    Components::UIDocument menu;
    menu.Layout.Set(cssGuid);
    menu.SortOrder = 0;
    menu.RenderMode = Components::UIRenderMode::Fullscreen;
    const auto hudE = world.Create<Components::UIDocument>(hud);
    auto menuE = world.Create<Components::UIDocument>(menu);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    UIElement* hudRoot = host.FindDocumentRoot(hudE.GetHandle().id);
    ASSERT_NE(hudRoot, nullptr);
    EXPECT_FALSE(hudRoot->Overrides().Get(Style::Display).has_value())
        << "an Overlay HUD must stay visible while the Fullscreen menu cannot draw";

    // Positive control: the same menu with a layout that binds does suppress, so a
    // predicate that simply never fires cannot pass this test.
    menuE.GetForWrite<Components::UIDocument>()->Layout.Set(guidB);
    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_EQ(hudRoot->Overrides().Get(Style::Display), std::optional<DisplayMode>(DisplayMode::None))
        << "the HUD must be suppressed once the menu's layout binds";
}

// An absolutely positioned auto-sized container (the post-layout auto-size
// pass) must shrink when a child goes display:none and grow back when it
// returns — on the main axis of either flex direction. Hidden children keep
// their last committed rect (CommitLayoutRects skips display:none subtrees),
// so the auto-size union has to skip them — otherwise the container stays
// stuck at its old size (the Scene View floating tool overlay bug, in both
// its vertical and bottom-docked horizontal orientations).
namespace
{
void RunAbsoluteAutoSizeHiddenChildCase(bool horizontal)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    UiRgHarness rg(dev.get());

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    ui->SetRoot(std::make_unique<UIElement>());

    std::unique_ptr<UIElement> sub;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='sub'>
             <uielement id='overlay' class='overlay'>
               <uielement class='item' />
               <uielement class='item' />
               <uielement id='last' class='item' />
             </uielement>
           </uielement>)",
        sub));
    UIElement* subRoot = sub.get();
    ui->GetRootElement()->AddChild(std::move(sub));

    const std::string css =
        std::string("#sub { position: relative; width: 400px; height: 400px; }\n"
                    ".overlay { position: absolute; left: 10px; top: 10px; display: flex; "
                    "flex-direction: ") +
        (horizontal ? "row" : "column") +
        "; padding: 4px; }\n"
        ".item { width: 28px; height: 28px; }\n";
    Stylesheet sheet;
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    subRoot->AddStylesheet(std::make_shared<Stylesheet>(std::move(sheet)));

    GameUIHostTestAccess::Update(host, 0.0f, 400, 400);
    DriveUiRender(*ui, rg);

    UIElement* overlay = ui->GetRootElement()->FindById("overlay");
    UIElement* last = ui->GetRootElement()->FindById("last");
    ASSERT_NE(overlay, nullptr);
    ASSERT_NE(last, nullptr);

    const auto mainAxisSize = [&]
    { return horizontal ? overlay->GetLayoutWidth() : overlay->GetLayoutHeight(); };

    const float fullSize = mainAxisSize();
    EXPECT_GE(fullSize, 3.0f * 28.0f) << "overlay did not content-size to its children";

    UI::Layout::SetElementHidden(*last, true);
    overlay->RequestRelayout();
    GameUIHostTestAccess::Update(host, 0.0f, 400, 400);
    DriveUiRender(*ui, rg);

    EXPECT_FLOAT_EQ(mainAxisSize(), fullSize - 28.0f)
        << "hidden child still holds the auto-sized container open";

    UI::Layout::SetElementHidden(*last, false);
    overlay->RequestRelayout();
    GameUIHostTestAccess::Update(host, 0.0f, 400, 400);
    DriveUiRender(*ui, rg);

    EXPECT_FLOAT_EQ(mainAxisSize(), fullSize)
        << "container did not grow back when the child was re-shown";
}
} // namespace

TEST(GameUIHostTests, AbsoluteAutoSizeShrinksWhenChildHidden)
{
    RunAbsoluteAutoSizeHiddenChildCase(/*horizontal=*/false);
}

TEST(GameUIHostTests, AbsoluteAutoSizeShrinksWhenChildHiddenHorizontal)
{
    RunAbsoluteAutoSizeHiddenChildCase(/*horizontal=*/true);
}

namespace
{
struct ScopedGameplayHost
{
    explicit ScopedGameplayHost(GameUIHost* host) { GameUI::SetHost(host); }
    ~ScopedGameplayHost() { GameUI::SetHost(nullptr); }
    ScopedGameplayHost(const ScopedGameplayHost&) = delete;
    ScopedGameplayHost& operator=(const ScopedGameplayHost&) = delete;
};
} // namespace

TEST(GameUIHostTests, GameplayUIPublishesRegisteredHost)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    EXPECT_EQ(GameUI::GetHost(), nullptr);
    EXPECT_EQ(GameUI::GetUIManager(), nullptr);
    EXPECT_EQ(GameUI::GetBindGeneration(), 0u);

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    {
        ScopedGameplayHost scope(&host);
        EXPECT_EQ(GameUI::GetHost(), &host);
        EXPECT_EQ(GameUI::GetUIManager(), host.GetUIManager());
        ASSERT_NE(GameUI::GetUIManager(), nullptr);
    }
    EXPECT_EQ(GameUI::GetHost(), nullptr);
    EXPECT_EQ(GameUI::GetUIManager(), nullptr);
}

TEST(GameUIHostTests, DestroyingRegisteredHostClearsGameplayUI)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto host = std::make_unique<GameUIHost>(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    GameUI::SetHost(host.get());
    ASSERT_EQ(GameUI::GetHost(), host.get());
    host.reset();
    EXPECT_EQ(GameUI::GetHost(), nullptr);
    EXPECT_EQ(GameUI::GetUIManager(), nullptr);
}

TEST(GameUIHostTests, DestroyingUnregisteredHostLeavesGameplayUI)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost gameplay(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ScopedGameplayHost scope(&gameplay);
    {
        GameUIHost preview(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
        EXPECT_EQ(GameUI::GetHost(), &gameplay);
    }
    EXPECT_EQ(GameUI::GetHost(), &gameplay);
}

TEST(GameUIHostTests, BindGenerationBumpsOnLayoutBindAndReset)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_bindgen";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);
    EXPECT_EQ(GameUI::GetBindGeneration(), 0u);

    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(guidA);
    const auto entity = world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    const uint64_t afterBind = GameUI::GetBindGeneration();
    EXPECT_GT(afterBind, 0u);
    EXPECT_NE(GameUI::FindDocumentRoot(entity.GetHandle().id), nullptr);
    EXPECT_NE(host.GetUIManager()->GetRootElement()->FindById("leafA"), nullptr);

    host.ResetBoundDocuments();
    EXPECT_GT(GameUI::GetBindGeneration(), afterBind);
    EXPECT_EQ(GameUI::FindDocumentRoot(entity.GetHandle().id), nullptr);
    EXPECT_EQ(host.GetUIManager()->GetRootElement()->FindById("leafA"), nullptr);

    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_NE(host.GetUIManager()->GetRootElement()->FindById("leafA"), nullptr);
}

TEST(GameUIHostTests, FindElementByIdScopesToDocument)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_findelement";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    GUID guidA, guidB;
    SetupTwoLayouts(assets, root, guidA, guidB);

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);

    ECS::World world(&pool);
    Components::UIDocument docA;
    docA.Layout.Set(guidA);
    Components::UIDocument docB;
    docB.Layout.Set(guidB);
    const auto entityA = world.Create<Components::UIDocument>(docA);
    const auto entityB = world.Create<Components::UIDocument>(docB);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_NE(GameUI::FindElementById(entityA.GetHandle().id, "leafA"), nullptr);
    EXPECT_EQ(GameUI::FindElementById(entityA.GetHandle().id, "leafB"), nullptr);
    EXPECT_NE(GameUI::FindElementById(entityB.GetHandle().id, "leafB"), nullptr);
    EXPECT_NE(GameUI::FindElementByIdAny("leafA"), nullptr);
    EXPECT_NE(GameUI::FindElementByIdAny("leafB"), nullptr);
}

// A .uxml edit reconciles the bound subtree through UIHotReload, NOT through
// SyncFromWorld: elements the new template no longer matches are destroyed and
// re-cloned, so every cached FindById pointer (and its click subscription) dies. The bind
// generation has to move for that too, or a gameplay binder polling it keeps a
// dangling element.
TEST(GameUIHostTests, BindGenerationBumpsOnLayoutHotReload)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_bindgen_hotreload";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));

    const auto path = WriteTempUxml(root, "hud_reload.uxml",
                                    "<uielement id='hud'><button id='take-damage' /></uielement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(path));
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());
    assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);

    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(guid);
    world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);
    ASSERT_NE(ui->GetRootElement()->FindById("take-damage"), nullptr) << "HUD did not bind";
    const uint64_t afterBind = GameUI::GetBindGeneration();

    // Rename the button in the .uxml: the reconcile destroys the old element
    // (template-owned, no longer matched by id) and clones a new one.
    {
        std::ofstream f(path);
        f << "<uielement id='hud'><button id='heal' /></uielement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guid, AssetType::UILayout, path.string()));

    // Host Update pumps UIHotReload; no further SyncFromWorld runs.
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);

    // Positive control: the reconcile really replaced the subtree contents.
    ASSERT_EQ(ui->GetRootElement()->FindById("take-damage"), nullptr)
        << "hot reload did not reconcile the bound subtree — the generation assert below would be vacuous";
    ASSERT_NE(ui->GetRootElement()->FindById("heal"), nullptr);

    EXPECT_GT(GameUI::GetBindGeneration(), afterBind)
        << "hot-reload reconcile invalidated cached elements without bumping the bind generation";
}

TEST(GameUIHostTests, NotifyGameplayStoppedDiscardsBoundDocumentsAndBumpsSession)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_playstop";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));

    const auto path = WriteTempUxml(root, "hud.uxml",
                                    "<uielement id='hud'><button id='take-damage' /></uielement>\n");
    auto& reg = assets.GetRegistry();
    reg.RegisterAsset(path);
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());
    assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);

    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(guid);
    world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    ASSERT_NE(GameUI::FindElementByIdAny("take-damage"), nullptr);

    const uint64_t session = GameUI::GetGameplaySession();
    const uint64_t gen = GameUI::GetBindGeneration();
    GameUI::NotifyGameplayStopped();
    EXPECT_GT(GameUI::GetGameplaySession(), session);
    EXPECT_GT(GameUI::GetBindGeneration(), gen);
    EXPECT_EQ(GameUI::FindElementByIdAny("take-damage"), nullptr)
        << "play-stop discards bound documents; the next SyncFromWorld rebuilds them";

    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_NE(GameUI::FindElementByIdAny("take-damage"), nullptr);
}

TEST(GameUIHostTests, NotifyGameplayStoppedRebuildDropsPlayHandlers)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_playstop_click";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));

    const auto path = WriteTempUxml(root, "hud.uxml",
                                    "<uielement id='hud'><button id='take-damage' /></uielement>\n");
    auto& reg = assets.GetRegistry();
    reg.RegisterAsset(path);
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());
    assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);

    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(guid);
    world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    auto* btn = dynamic_cast<Button*>(GameUI::FindElementByIdAny("take-damage"));
    ASSERT_NE(btn, nullptr);

    int clicks = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });
    btn->TriggerClick();
    EXPECT_EQ(clicks, 1);

    GameUI::NotifyGameplayStopped();
    GameUIHostTestAccess::SyncFromWorld(host, world);
    auto* rebuilt = dynamic_cast<Button*>(GameUI::FindElementByIdAny("take-damage"));
    ASSERT_NE(rebuilt, nullptr);
    rebuilt->TriggerClick();
    EXPECT_EQ(clicks, 1) << "rebuilt HUD must not keep the play-session click subscription";
}

// FindElementByIdAny is the scripting ABI's resolution primitive for entityId==0.
// THREE documents mount, each with a unique leaf plus a SHARED leaf id. Two documents
// would let a "return the first hit" regression pass roughly half the time (the
// unordered_map could iterate lowest-first by luck); three makes the lowest-entity
// tiebreak fail deterministically if it regresses. Also pins that the reported owner
// is the one a scoped lookup resolves the same element through, and that the
// out-param is untouched on a miss.
TEST(GameUIHostTests, FindElementByIdAnyResolvesSharedIdDeterministically)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_findany";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));

    // The layout root merges into the per-entity container on bind, so findable ids
    // must sit on child leaves (see MultipleDocumentsCompose).
    const auto uxmlA = WriteTempUxml(
        root, "findany_a.uxml",
        "<UIElement id=\"wrapA\"><UIElement id=\"only-a\" /><UIElement id=\"bar\" /></UIElement>\n");
    const auto uxmlB = WriteTempUxml(
        root, "findany_b.uxml",
        "<UIElement id=\"wrapB\"><UIElement id=\"only-b\" /><UIElement id=\"bar\" /></UIElement>\n");
    const auto uxmlC = WriteTempUxml(
        root, "findany_c.uxml",
        "<UIElement id=\"wrapC\"><UIElement id=\"only-c\" /><UIElement id=\"bar\" /></UIElement>\n");
    auto& reg = assets.GetRegistry();
    reg.RegisterAsset(uxmlA);
    reg.RegisterAsset(uxmlB);
    reg.RegisterAsset(uxmlC);
    const GUID guidA = reg.GetAssetGUID(uxmlA);
    const GUID guidB = reg.GetAssetGUID(uxmlB);
    const GUID guidC = reg.GetAssetGUID(uxmlC);
    assets.LoadAssetAsync(guidA, AssetLoadPriority::Normal).get();
    assets.LoadAssetAsync(guidB, AssetLoadPriority::Normal).get();
    assets.LoadAssetAsync(guidC, AssetLoadPriority::Normal).get();

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);

    ECS::World world(&pool);
    Components::UIDocument docA;
    docA.Layout.Set(guidA);
    Components::UIDocument docB;
    docB.Layout.Set(guidB);
    Components::UIDocument docC;
    docC.Layout.Set(guidC);
    const auto eA = world.Create<Components::UIDocument>(docA);
    const auto eB = world.Create<Components::UIDocument>(docB);
    const auto eC = world.Create<Components::UIDocument>(docC);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);

    const uint64_t idA = eA.GetHandle().id;
    const uint64_t idB = eB.GetHandle().id;
    const uint64_t idC = eC.GetHandle().id;

    // Unique id -> its owning document, reported through the out-param.
    uint64_t owner = 0;
    UIElement* onlyA = GameUI::FindElementByIdAny("only-a", &owner);
    ASSERT_NE(onlyA, nullptr) << "unique id only-a not found across documents";
    EXPECT_EQ(owner, idA);
    EXPECT_EQ(onlyA, GameUI::FindElementById(idA, "only-a")) << "any-search and scoped search disagree";

    // Absent id -> null, out-param untouched.
    owner = 0xABCDEF01ull;
    EXPECT_EQ(GameUI::FindElementByIdAny("no-such-id", &owner), nullptr);
    EXPECT_EQ(owner, 0xABCDEF01ull) << "outOwnerEntityId must not be written when nothing matches";

    // Shared id -> deterministic lowest-entity winner, and the reported owner must
    // resolve the identical element through the scoped lookup.
    const uint64_t expectedOwner = std::min({idA, idB, idC});
    owner = 0;
    UIElement* shared = GameUI::FindElementByIdAny("bar", &owner);
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(owner, expectedOwner) << "shared id must resolve to the lowest owning entity id";
    EXPECT_EQ(shared, GameUI::FindElementById(expectedOwner, "bar"))
        << "any-search winner must match the scoped lookup on the reported owner";
}

// Play-stop discards bound documents (NotifyGameplayStoppedDiscardsBoundDocumentsAndBumpsSession).
// The session gate still covers the case where the tree is somehow still alive
// (uxml hot-reload mid-play). After stop, SyncFromWorld rebuilds a clean tree with
// no leftover play callback.
//
// Driven through GameUI::Detail — the gate is no longer public C++ API (the
// scripting ABI is its only production consumer), and going through the ABI
// exports instead would bootstrap a whole Engine inside this suite.
TEST(GameUIHostTests, SessionEventGoesInertAfterPlayStops)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    UiRgHarness rg(dev.get());
    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_sessionclick";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));

    // Root classes land on the per-entity container (the layout root merges into it),
    // so size the HUD through a class, not the root's id.
    const auto uxml = WriteTempUxml(
        root, "session_hud.uxml",
        "<uielement class='hud-root'><button id='take-damage' class='btn' /></uielement>\n");
    const auto cssPath = root / "session_hud.css";
    {
        std::ofstream f(cssPath);
        f << ".hud-root { position: relative; width: 200px; height: 200px; }\n"
             ".btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(uxml));
    ASSERT_TRUE(reg.RegisterAsset(cssPath));
    const GUID layoutGuid = reg.GetAssetGUID(uxml);
    const GUID styleGuid = reg.GetAssetGUID(cssPath);
    ASSERT_FALSE(layoutGuid.IsNull());
    assets.LoadAssetAsync(layoutGuid, AssetLoadPriority::Normal).get();
    assets.LoadAssetAsync(styleGuid, AssetLoadPriority::Normal).get();

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);

    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(layoutGuid);
    doc.Style.Set(styleGuid);
    const auto entity = world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*host.GetUIManager(), rg);

    uint64_t owner = 0;
    UIElement* btn = GameUI::FindElementByIdAny("take-damage", &owner);
    ASSERT_NE(btn, nullptr);
    ASSERT_GT(btn->GetLayoutWidth(), 0.0f) << "button has no layout — the pointer could never hit it";
    EXPECT_EQ(owner, entity.GetHandle().id) << "owner out-param must name the document that matched";

    int clicks = 0;
    const uint64_t token =
        GameUI::Detail::RegisterSessionEvent(btn->GetInstanceId(), kEventButtonClick, [&clicks](const UIEvent&) { ++clicks; });
    ASSERT_NE(token, 0u) << "the session gate did not bind to the mounted button";
    const uint64_t registeredInstance = btn->GetInstanceId();

    // Press then release over the button centre — the production routing that emits MouseUp.
    const auto clickButton = [&]
    {
        GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
        GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
        GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
        GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    };

    clickButton();
    ASSERT_EQ(clicks, 1) << "handler must fire during the session that registered it";

    GameUI::NotifyGameplayStopped();
    GameUIHostTestAccess::SyncFromWorld(host, world);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    clickButton();
    EXPECT_EQ(clicks, 1) << "rebuilt HUD must not keep the play-session click";

    GameUI::Detail::UnregisterSessionEvent(registeredInstance, kEventButtonClick, token);
    clickButton();
    EXPECT_EQ(clicks, 1);
}

// The scripting-facing shape of the same escape: the session-click gate, which is what
// a HUD script binds through. It subscribes to UI.ButtonClick, so what protects it is
// that a cancelled gesture never reaches Button::TriggerClick — the only dispatcher of
// that event. This arm fails if cancellation is ever routed through the activation path.
TEST(GameUIHostTests, SessionEventDoesNotFireWhenTheCursorLeavesMidPress)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    UiRgHarness rg(dev.get());
    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_sessionclick_leave";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));

    const auto uxml = WriteTempUxml(
        root, "leave_hud.uxml",
        "<uielement class='hud-root'><button id='take-damage' class='btn' /></uielement>\n");
    const auto cssPath = root / "leave_hud.css";
    {
        std::ofstream f(cssPath);
        f << ".hud-root { position: relative; width: 200px; height: 200px; }\n"
             ".btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
    }
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(uxml));
    ASSERT_TRUE(reg.RegisterAsset(cssPath));
    const GUID layoutGuid = reg.GetAssetGUID(uxml);
    const GUID styleGuid = reg.GetAssetGUID(cssPath);
    ASSERT_FALSE(layoutGuid.IsNull());
    assets.LoadAssetAsync(layoutGuid, AssetLoadPriority::Normal).get();
    assets.LoadAssetAsync(styleGuid, AssetLoadPriority::Normal).get();

    GameUIHost host(dev.get(), &assets, &pool);
    ScopedGameplayHost scope(&host);

    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(layoutGuid);
    doc.Style.Set(styleGuid);
    world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    GameUIHostTestAccess::SyncFromWorld(host, world);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    DriveUiRender(*host.GetUIManager(), rg);

    auto* btn = dynamic_cast<Button*>(GameUI::FindElementByIdAny("take-damage"));
    ASSERT_NE(btn, nullptr);
    ASSERT_GT(btn->GetLayoutWidth(), 0.0f) << "button has no layout — the pointer could never hit it";

    int clicks = 0;
    const uint64_t token =
        GameUI::Detail::RegisterSessionEvent(btn->GetInstanceId(), kEventButtonClick, [&clicks](const UIEvent&) { ++clicks; });
    ASSERT_NE(token, 0u) << "the session gate did not bind to the mounted button";

    // Subscribe to the activation event directly too: the gate rides on it, so pinning
    // it here says exactly which layer holds the line.
    int buttonClicks = 0;
    btn->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++buttonClicks; });

    // Press over the button and hold, then take the cursor off the surface. The player
    // releases the mouse out there; this host is never told.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    GameUIHostTestAccess::PointerLeftSurface(host);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);

    EXPECT_EQ(clicks, 0) << "the HUD action fired for a press the player walked away from";
    EXPECT_EQ(buttonClicks, 0) << "UI.ButtonClick fired on a cancelled gesture — TriggerClick "
                                  "was reached without a release";

    // Positive control: the same registration still fires for a real press+release.
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, true);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    GameUIHostTestAccess::SetPointer(host, 80.0f, 60.0f, false);
    GameUIHostTestAccess::Update(host, 0.0f, 200, 200);
    EXPECT_EQ(clicks, 1) << "the session click stopped working after a cancelled press";
    EXPECT_EQ(buttonClicks, 1) << "UI.ButtonClick stopped firing for a real click";
}

TEST(GameUIHostTests, ReferenceModesKeepLayoutAndPointerAlignedAcrossResize)
{
    auto dev = MakeHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();
    GameUIHost host(dev.get(), nullptr, nullptr);
    auto* ui = host.GetUIManager();
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='root'><button id='scaled-button' /></uielement>)", root));
    ui->SetRoot(std::move(root));
    const auto css = std::filesystem::temp_directory_path() / "gameui_reference_height.css";
    {
        std::ofstream f(css);
        f << "#root { width: 100%; height: 100%; }\n"
             "#scaled-button { position: absolute; left: 100px; top: 200px; width: 200px; height: 48px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));
    auto* button = dynamic_cast<Button*>(ui->GetRootElement()->FindById("scaled-button"));
    ASSERT_NE(button, nullptr);
    int clicks = 0;
    button->SetOnClick([&](UIEvent&) { ++clicks; });
    for (const auto mode : {UI::UIScaleMode::Width, UI::UIScaleMode::Height,
                            UI::UIScaleMode::Fit, UI::UIScaleMode::Fill})
    {
        ui->SetScaleSettings({mode, 1600.0f, 800.0f});
        for (const uint32_t height : {400u, 800u, 1600u, 2400u})
        {
            const uint32_t width = height == 400 ? height * 3 : height;
            GameUIHostTestAccess::Update(host, 0.0f, width, height);
            const float x = static_cast<float>(width) / 1600.0f;
            const float y = static_cast<float>(height) / 800.0f;
            const float scale = mode == UI::UIScaleMode::Width ? x
                              : mode == UI::UIScaleMode::Height ? y
                              : mode == UI::UIScaleMode::Fit ? std::min(x, y) : std::max(x, y);
            EXPECT_FLOAT_EQ(ui->GetContentScale(), scale);
            EXPECT_NEAR(button->GetLayoutWidth(), 200.0f, 0.01f);
            EXPECT_NEAR(button->GetLayoutHeight(), 48.0f, 0.01f);
            const int before = clicks;
            host.OnMouseMove({150.0f * scale, 224.0f * scale});
            host.OnMouseButton(Input::kMouseButton_Left, true, 0);
            host.OnMouseButton(Input::kMouseButton_Left, false, 0);
            EXPECT_EQ(clicks, before + 1) << "height=" << height << " mode=" << static_cast<int>(mode);
        }
    }
    ui->SetScaleSettings({});
    GameUIHostTestAccess::Update(host, 0.0f, 1600, 1600);
    EXPECT_FLOAT_EQ(ui->GetContentScale(), 1.0f);
    std::filesystem::remove(css);
}

// The wheel carries no position of its own: it lands on whatever the last move
// hovered, and that move is divided by the content scale. Its delta is wheel
// steps, which UIManager turns into logical pixels, so one step scrolls the same
// logical distance at every scale.
TEST(GameUIHostTests, AWheelUnderAScaledPointerScrollsInLogicalPixels)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();
    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui, nullptr);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto scrollOwned = std::make_unique<ScrollView>();
    auto* scroll = scrollOwned.get();
    scroll->SetId("scroll");
    auto contentOwned = std::make_unique<UIElement>();
    contentOwned->SetId("content");
    scroll->AddContent(std::move(contentOwned));
    root->AddChild(std::move(scrollOwned));
    ui->SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "gameui_host_scaled_wheel.css";
    {
        std::ofstream f(css);
        f << "#root { display: flex; width: 200px; height: 100px; }\n"
             "#scroll { width: 200px; height: 100px; overflow: scroll; }\n"
             "#content { width: 200px; height: 1000px; }\n";
    }
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

    float eventX = -1.0f;
    float eventY = -1.0f;
    scroll->RegisterEventHandler(kEventScroll, [&](UIEvent& e) {
        eventX = e.X;
        eventY = e.Y;
    });

    // Height mode at twice the reference height: two physical pixels per logical one.
    ui->SetScaleSettings({UI::UIScaleMode::Height, 1600.0f, 800.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 400, 1600);
    ASSERT_FLOAT_EQ(ui->GetContentScale(), 2.0f);

    // (300, 180) physical is (150, 90) logical: inside the 200x100 view only
    // once divided, so an undivided pointer would leave the wheel unclaimed.
    host.OnMouseMove({300.0f, 180.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 400, 1600);
    EXPECT_TRUE(host.OnScroll({0.0f, -1.0f})) << "the scrollable under the scaled pointer took the wheel";
    EXPECT_FLOAT_EQ(eventX, 150.0f) << "the wheel event carries the logical pointer position";
    EXPECT_FLOAT_EQ(eventY, 90.0f);
    const float scaledStep = scroll->GetScrollY();
    EXPECT_GT(scaledStep, 0.0f);

    // Positive control at scale 1: the same step moves the view the same distance.
    ui->SetScaleSettings({});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 100);
    ASSERT_FLOAT_EQ(ui->GetContentScale(), 1.0f);
    host.OnMouseMove({150.0f, 90.0f});
    GameUIHostTestAccess::Update(host, 0.0f, 200, 100);
    EXPECT_TRUE(host.OnScroll({0.0f, -1.0f}));
    EXPECT_FLOAT_EQ(scroll->GetScrollY() - scaledStep, scaledStep)
        << "a wheel step is a logical distance, independent of the content scale";

    std::filesystem::remove(css);
}

TEST(GameUIHostTests, ReferenceModesRespondToAspectRatioChanges)
{
    UI::UIScaleSettings settings{UI::UIScaleMode::Width, 1600.0f, 800.0f};
    EXPECT_FLOAT_EQ(settings.Resolve(800, 800, 2.0f), 0.5f);
    settings.mode = UI::UIScaleMode::Height;
    EXPECT_FLOAT_EQ(settings.Resolve(800, 800, 2.0f), 1.0f);
    settings.mode = UI::UIScaleMode::Fit;
    EXPECT_FLOAT_EQ(settings.Resolve(800, 800, 2.0f), 0.5f);
    EXPECT_FLOAT_EQ(settings.Resolve(3200, 800, 2.0f), 1.0f);
    settings.mode = UI::UIScaleMode::Fill;
    EXPECT_FLOAT_EQ(settings.Resolve(800, 800, 2.0f), 1.0f);
    EXPECT_FLOAT_EQ(settings.Resolve(3200, 800, 2.0f), 2.0f);
    settings.referenceWidth = 0.0f;
    EXPECT_FLOAT_EQ(settings.Resolve(800, 800, 1.5f), 1.5f);
    settings.mode = UI::UIScaleMode::Platform;
    EXPECT_FLOAT_EQ(settings.Resolve(800, 800, 2.0f), 2.0f);
}

TEST(GameUIHostTests, ScaleConfigRoundTripsEveryMode)
{
    const auto path = std::filesystem::temp_directory_path() / ("ui-scale-" + GUID::Generate().ToString() + ".json");
    for (const auto mode : {UI::UIScaleMode::Platform, UI::UIScaleMode::Width, UI::UIScaleMode::Height,
                            UI::UIScaleMode::Fit, UI::UIScaleMode::Fill})
    {
        GameEngine::GameConfig config;
        config.uiScale = {mode, 1234.0f, 567.0f};
        ASSERT_TRUE(GameEngine::SaveGameConfig(path, config));
        const auto loaded = GameEngine::LoadGameConfig(path);
        EXPECT_EQ(loaded.uiScale.mode, mode);
        EXPECT_FLOAT_EQ(loaded.uiScale.referenceWidth, 1234.0f);
        EXPECT_FLOAT_EQ(loaded.uiScale.referenceHeight, 567.0f);
    }
    std::filesystem::remove(path);
}
