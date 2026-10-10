// The deferred panel bind must be scheduled once, not once per layout pass.
//
// Editor panels bind their .uxml out of the layout pass: OnPostLayout posts a deferred
// action, and the action starts an asynchronous asset load. The latch that stops the next
// layout pass re-arming has to cover BOTH halves of that — the queued action and the load
// it starts. A latch cleared when the action runs, while the "applied" flag is set only in
// the load's completion callback, leaves a window in which every layout pass re-arms.
//
// The window is unbounded when the bind cannot succeed at all (an unresolvable guid, a
// rejected bind), which is the case this fixture reproduces: nothing here mounts the editor
// asset source, so the panels take their failure path. Clicks are handler-table
// subscriptions and registrations are additive, so a re-arming panel stacks one handler
// per pass and a single click fires once per pass that ran.
//
// WebPanel stands in for the shape. It is the smallest panel carrying it, and it carried the
// sharpest form: its loads were blocking future.get() calls, so a failing bind stalled the UI
// thread once per layout pass, against the rule SceneViewPanel.cpp states for the same idiom.
//
// GameViewPanel, UIDemoPanel and AssetBoundDockPanel join it here because the engine-wide
// latch fix did not reach them. Note what this fixture can and cannot show for each: with no
// editor asset source mounted, every panel below fails at guid resolution, BEFORE any load
// starts. That makes the re-arm property observable (a panel with no terminal failure state
// re-arms forever) but leaves blocking-vs-non-blocking loading unobservable here — no load
// runs at all. UIDemoPanel's and AssetBoundDockPanel's blocking future.get() calls are
// therefore pinned by reading and by a live editor check, not by this suite.

#include <gtest/gtest.h>

#include "Core/Engine.h"
#include "Panels/GameViewPanel.h"
#include "Panels/UIDemoPanel.h"
#include "Panels/WebPanel.h"
#include "UI/AssetBoundDockPanel.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"
#include "UI/UIManager.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::GameViewPanel;
using GameEngine::UIDemoPanel;
using GameEngine::UILayoutAccess;
using GameEngine::UIManager;
using GameEngine::WebPanel;

namespace
{
// Counts what the panel schedules. UIElement::PostAction prefers the thread-local UI
// context's dispatcher over the owning manager's, so installing one of these through
// UiContextScope intercepts every deferred action the panel posts.
class CountingDispatcher final : public GameEngine::UI::IUiDispatcher
{
  public:
    bool Post(std::function<void()> fn) override
    {
        ++m_PostCount;
        m_Queue.push_back(std::move(fn));
        return true;
    }

    void Drain() override
    {
        // Re-entrant posts land in the next drain, matching UiDispatcher's contract.
        std::vector<std::function<void()>> batch;
        batch.swap(m_Queue);
        for (auto& fn : batch)
            if (fn)
                fn();
    }

    size_t PendingCount() const override { return m_Queue.size(); }

    size_t PostCount() const { return m_PostCount; }

  private:
    std::vector<std::function<void()>> m_Queue;
    size_t m_PostCount = 0;
};

// A panel owned by a real UIManager, so GetOwnerManager() is non-null and OnPostLayout
// reaches the bind at all.
template <typename PanelT>
struct ManagedPanel
{
    UIManager Manager{nullptr};
    PanelT* Panel = nullptr;

    ManagedPanel()
    {
        auto owned = std::make_unique<PanelT>();
        Panel = owned.get();
        Manager.SetRoot(std::move(owned));
    }
};

using ManagedWebPanel = ManagedPanel<WebPanel>;

// AssetBoundDockPanel takes its asset source and paths from the panel type that derives
// it (the constructor is protected). This one names the same unmounted editor source the
// rest of the fixture relies on, so it takes the failure path like the others.
class LatchTestAssetBoundPanel final : public GameEngine::Editor::AssetBoundDockPanel
{
  public:
    LatchTestAssetBoundPanel()
        : AssetBoundDockPanel("Latch Test", "editor", "UI/panels/LatchTestPanel.uxml",
                              "UI/panels/LatchTestPanel.css")
    {
    }
};

struct ManagedAssetBoundPanel : ManagedPanel<LatchTestAssetBoundPanel>
{
    ManagedAssetBoundPanel()
    {
        // Unlike the other panels, this one treats a zero-sized rect as an inactive dock
        // tab and skips the bind entirely. Without a committed rect the positive control
        // below could never fire and the test would pass vacuously.
        UILayoutAccess::SetLastLayoutRect(*Panel, 0.0f, 0.0f, 320.0f, 240.0f);
    }
};

// The defect, stated as the property that fails without the latch: once the panel has made
// its bind attempt, further layout passes must schedule nothing. Asserted as "the post count
// stops growing" rather than as an exact total, so the arm holds whichever way the attempt
// resolved — failed outright, or still in flight behind an async load. Both are states in
// which a re-arm is wrong; only "applied" and "detached" may release the latch.
template <typename FixtureT>
void ExpectSettledPassesScheduleNoFurtherBinds(FixtureT& fixture, CountingDispatcher& dispatcher)
{
    // Positive control: the first pass really does schedule a bind. Without this, a panel
    // that never binds at all would satisfy the no-growth assertion vacuously.
    fixture.Panel->OnPostLayout();
    ASSERT_EQ(dispatcher.PostCount(), 1u)
        << "the first layout pass scheduled no bind, so this test proves nothing";
    dispatcher.Drain();

    // Let the attempt settle, then hold the count and keep laying out.
    for (int pass = 0; pass < 3; ++pass)
    {
        fixture.Panel->OnPostLayout();
        dispatcher.Drain();
    }
    const size_t settled = dispatcher.PostCount();

    for (int pass = 0; pass < 8; ++pass)
    {
        fixture.Panel->OnPostLayout();
        dispatcher.Drain();
    }

    EXPECT_EQ(dispatcher.PostCount(), settled)
        << "OnPostLayout re-armed the deferred bind after the attempt had settled: "
        << (dispatcher.PostCount() - settled) << " extra schedules across 8 passes";
}

// The same latch must not wedge the panel either. A bind attempt that resolves has to leave
// the panel in a terminal state rather than pending forever, which is what the count above
// would also show if the latch were simply never released — so this arm pins the other side:
// a panel that has settled reports no work still queued.
template <typename FixtureT>
void ExpectSettledPanelLeavesNothingQueued(FixtureT& fixture, CountingDispatcher& dispatcher)
{
    for (int pass = 0; pass < 4; ++pass)
    {
        fixture.Panel->OnPostLayout();
        dispatcher.Drain();
    }

    EXPECT_EQ(dispatcher.PendingCount(), 0u)
        << "the panel left deferred work queued after its bind attempt settled";
}

class PanelDeferredBindLatchTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        // The panels resolve their assets through the engine's AssetManager; the bind path
        // dereferences it before it can reach any latch decision.
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }
};
} // namespace

TEST_F(PanelDeferredBindLatchTests, SettledPassesScheduleNoFurtherBinds)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedWebPanel fixture;
    ExpectSettledPassesScheduleNoFurtherBinds(fixture, dispatcher);
}

TEST_F(PanelDeferredBindLatchTests, SettledPanelLeavesNothingQueued)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedWebPanel fixture;
    ExpectSettledPanelLeavesNothingQueued(fixture, dispatcher);
}

TEST_F(PanelDeferredBindLatchTests, GameViewPanelSettledPassesScheduleNoFurtherBinds)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedPanel<GameViewPanel> fixture;
    ExpectSettledPassesScheduleNoFurtherBinds(fixture, dispatcher);
}

TEST_F(PanelDeferredBindLatchTests, GameViewPanelSettledPanelLeavesNothingQueued)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedPanel<GameViewPanel> fixture;
    ExpectSettledPanelLeavesNothingQueued(fixture, dispatcher);
}

TEST_F(PanelDeferredBindLatchTests, UIDemoPanelSettledPassesScheduleNoFurtherBinds)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedPanel<UIDemoPanel> fixture;
    ExpectSettledPassesScheduleNoFurtherBinds(fixture, dispatcher);
}

TEST_F(PanelDeferredBindLatchTests, UIDemoPanelSettledPanelLeavesNothingQueued)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedPanel<UIDemoPanel> fixture;
    ExpectSettledPanelLeavesNothingQueued(fixture, dispatcher);
}

// AssetBoundDockPanel already failed terminally on an unresolvable guid before its loads
// went asynchronous, so this pair is a regression guard rather than a reproduction: it is
// what would catch the async conversion reintroducing the re-arm the other panels had.
TEST_F(PanelDeferredBindLatchTests, AssetBoundDockPanelSettledPassesScheduleNoFurtherBinds)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedAssetBoundPanel fixture;
    ExpectSettledPassesScheduleNoFurtherBinds(fixture, dispatcher);
}

TEST_F(PanelDeferredBindLatchTests, AssetBoundDockPanelSettledPanelLeavesNothingQueued)
{
    CountingDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope(&dispatcher, nullptr);

    ManagedAssetBoundPanel fixture;
    ExpectSettledPanelLeavesNothingQueued(fixture, dispatcher);
}
