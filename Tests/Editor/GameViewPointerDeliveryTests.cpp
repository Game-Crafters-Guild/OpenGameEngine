#include <gtest/gtest.h>

#include "EditorPanelIds.h"
#include "Input/InputSystem.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Panels/GameViewPanel.h"
#include "UI/Controls/Button.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"

#include <limits>
#include <memory>
#include <vector>

namespace GameEngine
{
struct GameViewPanelTestAccess
{
    static void BindViewport(GameViewPanel& panel) { panel.RefreshElementPointers(); }
    // The layout-applied state a real .uxml bind leaves behind, without the editor
    // asset mount this fixture does not have.
    static void MarkLayoutApplied(GameViewPanel& panel) { panel.m_BindApplied = true; }
};
}

using namespace GameEngine;

namespace
{
class GameViewPointerDeliveryTests : public testing::Test
{
protected:
    GameUIHost Host{nullptr, nullptr, nullptr};
    UIManager Editor{nullptr};
    GameViewPanel* Panel = nullptr;
    UIElement* Viewport = nullptr;
    Button* Target = nullptr;
    int Clicks = 0;
    int AncestorPresses = 0;
    std::vector<bool> Edges;

    // The gameplay tree: one button under the viewport point Press() uses.
    void BuildHostTree()
    {
        auto root = std::make_unique<UIElement>();
        auto button = std::make_unique<Button>();
        Target = button.get();
        Target->SetId("target");
        Target->SetOnClick([this](auto&) { ++Clicks; });
        Target->RegisterEventHandler(kEventMouseDown, [this](auto&) { Edges.push_back(true); });
        Target->RegisterEventHandler(kEventMouseUp, [this](auto&) { Edges.push_back(false); });
        root->AddChild(std::move(button));
        Host.GetUIManager()->SetRoot(std::move(root));
        UILayoutAccess::SetLastLayoutRect(*Host.GetUIManager()->GetRootElement(), 0, 0, 800, 400);
        UILayoutAccess::SetLastLayoutRect(*Target, 200, 100, 100, 80);
    }

    // What UIHotReload raises on the panel after reconciling its .uxml.
    void AnnounceReconcile()
    {
        UIEvent reconciled{};
        reconciled.Id = kEventLayoutReconciled;
        reconciled.Target = Panel;
        reconciled.CurrentTarget = Panel;
        Panel->DispatchEvent(reconciled);
    }

    void SetUp() override
    {
        BuildHostTree();

        auto panel = std::make_unique<GameViewPanel>();
        Panel = panel.get();
        auto viewport = std::make_unique<UIElement>();
        Viewport = viewport.get();
        Viewport->SetId(EditorPanelIds::GameViewViewport);
        Panel->AddChild(std::move(viewport));
        Editor.SetRoot(std::move(panel));
        UILayoutAccess::SetLastLayoutRect(*Panel, 0, 0, 600, 400);
        UILayoutAccess::SetLastLayoutRect(*Viewport, 100, 50, 400, 200);
        GameViewPanelTestAccess::BindViewport(*Panel);
        Panel->SetPointerCallback([this](bool over, float x, float y, bool down, int mods) {
            GameUIHost::PointerState pointer{x * 800, y * 400, down, mods};
            Host.UpdatePointer(over ? &pointer : nullptr);
        });
        Panel->RegisterEventHandler(kEventMouseDown, [this](auto&) { ++AncestorPresses; });
    }

    void TearDown() override
    {
        // Destroy the source first and finish its cancellation while the sink is
        // alive. Production window teardown similarly owns the dispatcher/host.
        Editor.SetRoot(nullptr);
        Editor.GetDispatcher()->Drain();
    }

    void Press(float x = 225, float y = 120)
    {
        Editor.OnMouseMove(x, y);
        Editor.OnMouseButton(0, true);
    }
    void Release(float x = 225, float y = 120)
    {
        Editor.OnMouseMove(x, y);
        Editor.OnMouseButton(0, false);
    }
    void Send(EventId id)
    {
        UIEvent event;
        event.Id = id;
        event.X = 225;
        event.Y = 120;
        event.Button = 0;
        event.Target = Viewport;
        event.CurrentTarget = Viewport;
        Viewport->DispatchEvent(event);
        Editor.GetDispatcher()->Drain();
    }
};

TEST_F(GameViewPointerDeliveryTests, ShortClickAndOuterBubbleFinishWithoutAnyComposite)
{
    Press();
    EXPECT_EQ(AncestorPresses, 1) << "the inner manager must not overwrite the outer bubble chain";
    EXPECT_EQ(Edges, std::vector<bool>{true});
    Release();
    EXPECT_EQ(Clicks, 1);
    EXPECT_EQ(Edges, (std::vector<bool>{true, false}));
}

TEST_F(GameViewPointerDeliveryTests, RepeatedClicksPreserveEveryEdgeWhileRenderingIsSkipped)
{
    for (int i = 0; i < 12; ++i)
    {
        Press();
        Release();
    }
    EXPECT_EQ(Clicks, 12);
    EXPECT_EQ(AncestorPresses, 12);
    EXPECT_EQ(Edges.size(), 24u);
    Editor.GetDispatcher()->Drain();
    EXPECT_EQ(Clicks, 12) << "an empty drain cannot replay input";
}

TEST_F(GameViewPointerDeliveryTests, ReleaseModifiersReachTheHudBeforeTheNextFrame)
{
    std::vector<int> activations;
    Target->SetOnClick([&](UIEvent& event) { activations.push_back(event.Mods); });
    Editor.ReconcileModifierKeys(Input::kModControl);
    Press();
    Editor.ReconcileModifierKeys(Input::kModShift);
    Release();
    Editor.ReconcileModifierKeys(0);
    Press();
    Release();
    EXPECT_EQ(activations, (std::vector<int>{Input::kModShift, 0}));
}

TEST_F(GameViewPointerDeliveryTests, DeferredBridgeKeepsTheSnapshotWhenAnOuterHandlerChangesModifiers)
{
    int modifiers = -1;
    Target->SetOnClick([&](UIEvent& event) { modifiers = event.Mods; });
    Panel->RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { Editor.ReconcileModifierKeys(0); });
    Press();
    Editor.ReconcileModifierKeys(Input::kModShift | Input::kModControl);
    Release();
    EXPECT_EQ(Editor.GetModifierKeys(), 0);
    EXPECT_EQ(modifiers, Input::kModShift | Input::kModControl);
}

TEST_F(GameViewPointerDeliveryTests, CancellingClearsThePrivateManagersModifiers)
{
    Editor.ReconcileModifierKeys(Input::kModShift);
    Press();
    EXPECT_EQ(Host.GetUIManager()->GetModifierKeys(), Input::kModShift);
    Send(kEventMouseCancel);
    EXPECT_EQ(Host.GetUIManager()->GetModifierKeys(), 0);
    EXPECT_EQ(Clicks, 0);
}

TEST_F(GameViewPointerDeliveryTests, ReleaseUsesItsOwnPositionWithoutAMouseMoveEvent)
{
    Press();
    // UIManager receives the new position but no Update emits a MouseMove event
    // to the panel. Its MouseUp must normalize that edge's own coordinates.
    Release(350, 120);
    EXPECT_EQ(Clicks, 0);
    EXPECT_EQ(Edges, (std::vector<bool>{true, false}));
}

TEST_F(GameViewPointerDeliveryTests, CancelAndLeaveDisarmWithoutAnOrphanRelease)
{
    for (const auto event : {kEventMouseCancel, kEventMouseLeave})
    {
        Press();
        Send(event);
        Release();
        EXPECT_EQ(Clicks, 0);
        EXPECT_FALSE(Target->HasClass("pressed"));
    }
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewPointerDeliveryTests, HiddenPanelCancelsADeliveredPress)
{
    Press();
    ASSERT_TRUE(Target->HasClass("pressed")) << "nothing was delivered, so the cancel below proves nothing";
    Panel->OnMountVisibilityChanged(false);
    Editor.GetDispatcher()->Drain();
    Release();
    EXPECT_EQ(Clicks, 0);
    EXPECT_FALSE(Target->HasClass("pressed"));
}

TEST_F(GameViewPointerDeliveryTests, ReplacedTreeRequiresReleaseBeforeReplacementCanBeArmed)
{
    Press();
    BuildHostTree();
    GameUIHost::PointerState held{250, 140, true};
    Host.UpdatePointer(&held);
    Release();
    EXPECT_EQ(Clicks, 0);
    EXPECT_EQ(Edges, std::vector<bool>{true}) << "cancel must not emit a raw orphan MouseUp";
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewPointerDeliveryTests, DocumentResetCancelsWithoutWaitingForRendering)
{
    Press();
    Host.ResetBoundDocuments();
    Release();
    EXPECT_EQ(Clicks, 0);
    EXPECT_FALSE(Target->HasClass("pressed"));
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewPointerDeliveryTests, InvalidPointerCancelsTheGesture)
{
    Press();
    ASSERT_TRUE(Target->HasClass("pressed")) << "nothing was delivered, so the cancel below proves nothing";
    GameUIHost::PointerState invalid{std::numeric_limits<float>::quiet_NaN(), 140, true};
    Host.UpdatePointer(&invalid);
    Release();
    EXPECT_EQ(Clicks, 0);
    EXPECT_FALSE(Target->HasClass("pressed"));
}

TEST_F(GameViewPointerDeliveryTests, DetachedViewportOutlivingPanelHasNoHandlers)
{
    Press();
    Release();
    ASSERT_EQ(Clicks, 1) << "delivery is dead, so the detached-viewport assertion below is vacuous";
    Clicks = 0;
    auto detached = Panel->TakeChild(Viewport);
    ASSERT_TRUE(detached);
    Editor.SetRoot(nullptr);
    Editor.GetDispatcher()->Drain();
    UIEvent event;
    event.Id = kEventMouseDown;
    event.X = 225;
    event.Y = 120;
    event.Target = detached.get();
    event.CurrentTarget = detached.get();
    detached->DispatchEvent(event);
    Editor.GetDispatcher()->Drain();
    EXPECT_EQ(Clicks, 0);
}

TEST_F(GameViewPointerDeliveryTests, QueuedEdgesAreInvalidatedWhenViewportIsReplaced)
{
    UIEvent down;
    down.Id = kEventMouseDown;
    down.X = 225;
    down.Y = 120;
    down.Button = 0;
    down.Target = Viewport;
    down.CurrentTarget = Viewport;
    Viewport->DispatchEvent(down); // queue without draining the editor dispatcher
    auto oldViewport = Panel->TakeChild(Viewport);
    auto replacement = std::make_unique<UIElement>();
    Viewport = replacement.get();
    Viewport->SetId(EditorPanelIds::GameViewViewport);
    Panel->AddChild(std::move(replacement));
    UILayoutAccess::SetLastLayoutRect(*Viewport, 100, 50, 400, 200);
    GameViewPanelTestAccess::BindViewport(*Panel);
    Editor.GetDispatcher()->Drain();
    EXPECT_TRUE(Edges.empty());
    oldViewport->DispatchEvent(down);
    Editor.GetDispatcher()->Drain();
    EXPECT_TRUE(Edges.empty());
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewPointerDeliveryTests, QueuedPressCannotOutliveItsPanel)
{
    Press();
    Release();
    ASSERT_EQ(Clicks, 1) << "delivery is dead, so the queued-press assertions below are vacuous";
    Clicks = 0;
    Edges.clear();
    UIEvent down;
    down.Id = kEventMouseDown;
    down.X = 225;
    down.Y = 120;
    down.Button = 0;
    down.Target = Viewport;
    down.CurrentTarget = Viewport;
    Viewport->DispatchEvent(down);
    Editor.SetRoot(nullptr);
    Editor.GetDispatcher()->Drain();
    EXPECT_TRUE(Edges.empty());
    EXPECT_EQ(Clicks, 0);
}

TEST_F(GameViewPointerDeliveryTests, ViewportReplacementAtSameAddressGetsFreshHandlers)
{
    auto old = Panel->TakeChild(Viewport);
    ASSERT_TRUE(old);
    auto* storage = old.release();
    const auto oldId = storage->GetInstanceId();
    // Keep the allocation while ending the old element lifetime. This forces
    // allocator address reuse instead of hoping the ordinary heap chooses it.
    storage->~UIElement();
    Viewport = new (storage) UIElement();
    ASSERT_NE(Viewport->GetInstanceId(), oldId);
    Viewport->SetId(EditorPanelIds::GameViewViewport);
    Panel->AddChild(std::unique_ptr<UIElement>(Viewport));
    UILayoutAccess::SetLastLayoutRect(*Viewport, 100, 50, 400, 200);
    GameViewPanelTestAccess::BindViewport(*Panel);
    Editor.GetDispatcher()->Drain();
    EXPECT_EQ(Panel->GetViewportElement(), Viewport);
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewPointerDeliveryTests, DestroyingSourceTerminatesAnAlreadyDeliveredPress)
{
    Press();
    ASSERT_TRUE(Target->HasClass("pressed"));
    Editor.SetRoot(nullptr);
    Editor.GetDispatcher()->Drain();
    EXPECT_FALSE(Target->HasClass("pressed"));
    EXPECT_FALSE(Host.GetUIManager()->IsMouseCaptured());
    EXPECT_EQ(Clicks, 0);
}

TEST_F(GameViewPointerDeliveryTests, OwnerQuiescenceRevokesQueuedCallbacksBeforeSinkDestruction)
{
    Press();
    Release();
    ASSERT_EQ(Clicks, 1) << "delivery is dead, so the revoked-callback assertions below are vacuous";
    Clicks = 0;
    Edges.clear();
    UIEvent down;
    down.Id = kEventMouseDown;
    down.X = 225;
    down.Y = 120;
    down.Button = 0;
    down.Target = Viewport;
    down.CurrentTarget = Viewport;
    Viewport->DispatchEvent(down);
    Host.UpdatePointer(nullptr);
    Panel->ClearPointerCallback();
    Editor.SetRoot(nullptr);
    Editor.GetDispatcher()->Drain();
    EXPECT_TRUE(Edges.empty());
    EXPECT_EQ(Clicks, 0);
}

TEST_F(GameViewPointerDeliveryTests, ReconcileReresolvesTheViewportAndLayoutPassesDoNot)
{
    GameViewPanelTestAccess::MarkLayoutApplied(*Panel);
    auto replaced = Panel->TakeChild(Viewport);
    ASSERT_TRUE(replaced);
    auto fresh = std::make_unique<UIElement>();
    Viewport = fresh.get();
    Viewport->SetId(EditorPanelIds::GameViewViewport);
    Panel->AddChild(std::move(fresh));
    UILayoutAccess::SetLastLayoutRect(*Viewport, 100, 50, 400, 200);

    // No reconcile was announced, so the layout pass must not walk the subtree.
    for (int pass = 0; pass < 4; ++pass)
        Panel->OnPostLayout();
    EXPECT_EQ(Panel->GetViewportElement(), replaced.get())
        << "OnPostLayout re-resolved the viewport without a reconcile";

    AnnounceReconcile();
    EXPECT_EQ(Panel->GetViewportElement(), Viewport);
    Press();
    Release();
    EXPECT_EQ(Clicks, 1) << "the re-resolved viewport must deliver again";
}

TEST_F(GameViewPointerDeliveryTests, ReconcileKeepingTheViewportDoesNotWireItTwice)
{
    int forwards = 0;
    Panel->SetPointerCallback([&](bool, float, float, bool, int) { ++forwards; });
    Editor.GetDispatcher()->Drain();

    AnnounceReconcile();
    ASSERT_EQ(Panel->GetViewportElement(), Viewport) << "the reconcile kept the viewport, so it must stay resolved";
    forwards = 0;
    Send(kEventMouseMove);
    EXPECT_EQ(forwards, 1) << "the preserved viewport was wired again and forwards each event twice";
}

TEST_F(GameViewPointerDeliveryTests, RebindingSinkCancelsOldHostAndDoesNotReplayQueuedPress)
{
    Press();
    ASSERT_TRUE(Target->HasClass("pressed")) << "nothing reached the old host, so the rebind proves nothing";
    int newHostEdges = 0;
    Panel->SetPointerCallback([&](bool over, float, float, bool, int) {
        if (over)
            ++newHostEdges;
    });
    Editor.GetDispatcher()->Drain();
    EXPECT_FALSE(Target->HasClass("pressed"));
    EXPECT_EQ(Clicks, 0);
    EXPECT_EQ(newHostEdges, 0);
    Panel->SetPointerCallback({});
    Editor.GetDispatcher()->Drain();
}
}
