// UIElement::PostAction called from a thread with no UI context: file-watch, job and download
// callbacks in the editor do exactly this. The action must reach the dispatcher of the manager
// that owns the element at the instant of the call and run on the next drain; when that
// manager has been destroyed — a torn-off panel's window closed — the action must be refused,
// never written into the dead manager's queue.
//
// The test thread plays the UI thread. Every worker is a real std::thread with no UI context,
// and every wait on one is bounded by kWorkerTimeout.
#include <gtest/gtest.h>

#include "UIRgTestHarness.h"

#include "UI/Controls/Mount.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"
#include "UI/UiCoalescedPost.h"
#include "UI/UiPostHandle.h"
#include "UI/UiPostTarget.h"
#include "Rendering/Core/Device.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <thread>

using namespace GameEngine;

namespace
{

constexpr std::chrono::seconds kWorkerTimeout{10};

// Runs `work` on a fresh thread and waits for it, at most kWorkerTimeout. A worker that has not
// finished by then is deadlocked on state the test frame owns, so the process aborts rather than
// return and leave that thread running against a destroyed frame.
void RunOnWorker(const std::function<void()>& work)
{
    std::promise<void> finished;
    std::future<void> done = finished.get_future();
    std::thread worker([&work, &finished]() {
        work();
        finished.set_value();
    });
    if (done.wait_for(kWorkerTimeout) != std::future_status::ready)
    {
        std::fprintf(stderr, "PostActionThreadingTests: worker did not finish within %lld s\n",
                     static_cast<long long>(kWorkerTimeout.count()));
        std::abort();
    }
    worker.join();
}

// root
//  +- mount  -> target (externally owned, so it can outlive the manager)
struct MountHost
{
    std::unique_ptr<UIManager> Manager;
    Mount* HostMount = nullptr;
};

MountHost MakeMountHost(Rendering::IDevice* device)
{
    MountHost host;
    host.Manager = std::make_unique<UIManager>(device);
    auto root = std::make_unique<UIElement>();
    auto mount = std::make_unique<Mount>();
    host.HostMount = mount.get();
    root->AddChild(std::move(mount));
    host.Manager->SetRoot(std::move(root));
    return host;
}

// A worker that posts through one element until told to stop. Executed is touched only by the
// posted actions, which run on the UI thread's drains; the rest is shared with the worker.
struct RacingPoster
{
    UIElement* Target = nullptr;
    int Executed = 0;
    std::atomic<int> Accepted{0};
    std::atomic<int> Refused{0};
    std::atomic<int> Attempts{0};
    std::atomic<bool> Stop{false};
};

void PostUntilStopped(RacingPoster& poster)
{
    const auto deadline = std::chrono::steady_clock::now() + kWorkerTimeout;
    while (!poster.Stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        int* executed = &poster.Executed;
        if (poster.Target->PostAction([executed]() { ++*executed; }))
            poster.Accepted.fetch_add(1, std::memory_order_relaxed);
        else
            poster.Refused.fetch_add(1, std::memory_order_relaxed);
        poster.Attempts.fetch_add(1, std::memory_order_release);
    }
}

// Waits, at most kWorkerTimeout, until the worker has made `count` attempts.
bool WaitForAttempts(const RacingPoster& poster, int count)
{
    const auto deadline = std::chrono::steady_clock::now() + kWorkerTimeout;
    while (poster.Attempts.load(std::memory_order_acquire) < count)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

} // namespace

TEST(PostActionThreadingTests, APostFromAWorkerRunsOnTheUiThreadAtTheNextDrain)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev.get());
    auto rootOwned = std::make_unique<UIElement>();
    UIElement* root = rootOwned.get();
    ui.SetRoot(std::move(rootOwned));

    const std::thread::id uiThread = std::this_thread::get_id();
    std::thread::id ranOn{};
    int runs = 0;
    bool posted = false;
    RunOnWorker([&]() {
        posted = root->PostAction([&runs, &ranOn]() {
            ++runs;
            ranOn = std::this_thread::get_id();
        });
    });

    EXPECT_TRUE(posted);
    EXPECT_EQ(runs, 0) << "a worker's post ran inline instead of waiting for the UI thread";

    ui.DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(ranOn, uiThread) << "the posted action ran off the UI thread";
}

TEST(PostActionThreadingTests, APostFromAWorkerAfterTheOwningManagerIsDestroyedIsDropped)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    auto panel = std::make_unique<UIElement>();
    int runs = 0;
    {
        MountHost window = MakeMountHost(dev.get());
        window.HostMount->SetTarget(panel.get());
        ASSERT_EQ(panel->GetOwnerManager(), window.Manager.get());

        // Positive control: while the manager lives, the same call from the same kind of
        // thread is accepted and runs, so the refusal below is caused by the destruction.
        bool postedWhileAlive = false;
        RunOnWorker([&]() { postedWhileAlive = panel->PostAction([&runs]() { ++runs; }); });
        ASSERT_TRUE(postedWhileAlive);
        window.Manager->DrainDeferredActionsOnce();
        ASSERT_EQ(runs, 1);
    }

    // The panel survives its window, which is what a worker holding the panel sees after a
    // floating window closes.
    bool posted = true;
    RunOnWorker([&]() { posted = panel->PostAction([&runs]() { ++runs; }); });
    EXPECT_FALSE(posted) << "a post to a destroyed manager must be refused, not queued";
    EXPECT_EQ(runs, 1);
}

// The interleaving a tear-off makes possible, pinned step by step: a worker loads the route
// while the first manager owns the element, the UI thread moves the element to a second manager
// and destroys the first, and only then does the worker post through what it loaded.
TEST(PostActionThreadingTests, ARouteLoadedBeforeAnOwnerChangeCannotReachTheDestroyedOwner)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    auto next = std::make_unique<UIManager>(dev.get());
    auto first = std::make_unique<UIManager>(dev.get());
    UI::UiPostTarget target;
    target.Set(UIManagerSharedDispatcher(*first), first.get());

    UI::UiPostTarget::Route loadedBeforeMove;
    RunOnWorker([&]() { loadedBeforeMove = target.Load(); });
    ASSERT_EQ(loadedBeforeMove.Owner, first.get());

    target.Set(UIManagerSharedDispatcher(*next), next.get());
    first.reset();

    int runsThroughStaleRoute = 0;
    int runsThroughCurrentRoute = 0;
    bool postedThroughStaleRoute = true;
    bool postedThroughCurrentRoute = false;
    UIManager* currentOwner = nullptr;
    RunOnWorker([&]() {
        postedThroughStaleRoute =
            loadedBeforeMove.Dispatcher->Post([&runsThroughStaleRoute]() { ++runsThroughStaleRoute; });
        const UI::UiPostTarget::Route current = target.Load();
        currentOwner = current.Owner;
        postedThroughCurrentRoute =
            current.Dispatcher->Post([&runsThroughCurrentRoute]() { ++runsThroughCurrentRoute; });
    });

    EXPECT_FALSE(postedThroughStaleRoute) << "a route to a destroyed manager accepted a post";
    EXPECT_TRUE(postedThroughCurrentRoute);
    EXPECT_EQ(currentOwner, next.get());

    next->DrainDeferredActionsOnce();
    EXPECT_EQ(runsThroughStaleRoute, 0);
    EXPECT_EQ(runsThroughCurrentRoute, 1);
}

// The same race with a live worker: it posts continuously while the UI thread tears the element
// back and forth between two managers and finally destroys the one it is left in. Every post
// the worker was told was accepted must run exactly once, and the worker must keep posting past
// the destruction so the refused path is exercised, not assumed.
TEST(PostActionThreadingTests, PostsRacingATearOffEitherRunOnceOrAreRefused)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    constexpr int kOwnerFlips = 64;
    constexpr int kPostsAfterDestruction = 256;

    auto panel = std::make_unique<UIElement>();
    MountHost docked = MakeMountHost(dev.get());
    MountHost floating = MakeMountHost(dev.get());
    floating.HostMount->SetTarget(panel.get());

    RacingPoster poster;
    poster.Target = panel.get();
    std::thread worker(PostUntilStopped, std::ref(poster));
    if (!WaitForAttempts(poster, 1))
    {
        poster.Stop.store(true, std::memory_order_release);
        worker.join();
        FAIL() << "the worker never started posting";
    }

    for (int flip = 0; flip < kOwnerFlips; ++flip)
    {
        MountHost& to = (flip % 2 == 0) ? docked : floating;
        to.HostMount->SetTarget(panel.get());
        docked.Manager->DrainDeferredActionsOnce();
        floating.Manager->DrainDeferredActionsOnce();
    }
    floating.HostMount->SetTarget(panel.get());
    docked.Manager->DrainDeferredActionsOnce();

    // Close the floating window with the panel still in it, while the worker is posting. Every
    // attempt that starts after reset() returns loads a route whose dispatcher is closed.
    floating.Manager.reset();
    const int attemptsAtDestruction = poster.Attempts.load(std::memory_order_acquire);
    const bool postedPastDestruction = WaitForAttempts(poster, attemptsAtDestruction + kPostsAfterDestruction);

    poster.Stop.store(true, std::memory_order_release);
    worker.join(); // bounded: PostUntilStopped checks Stop and its own deadline every iteration

    docked.Manager->DrainDeferredActionsOnce();

    ASSERT_TRUE(postedPastDestruction)
        << "the worker stopped before posting past the destruction; the race was not exercised";
    EXPECT_GT(poster.Refused.load(), 0) << "no post reached the destroyed manager's closed route";
    EXPECT_EQ(poster.Executed, poster.Accepted.load())
        << "an accepted post was lost (queued where nothing drains)";
}

TEST(PostActionThreadingTests, APostSafeActionFromAWorkerRunsOnlyWhileTheElementLives)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev.get());
    auto rootOwned = std::make_unique<UIElement>();
    UIElement* root = rootOwned.get();
    ui.SetRoot(std::move(rootOwned));
    auto kept = std::make_unique<UIElement>();
    auto removed = std::make_unique<UIElement>();
    UIElement* keptPtr = kept.get();
    UIElement* removedPtr = removed.get();
    root->AddChild(std::move(kept));
    root->AddChild(std::move(removed));

    int keptRuns = 0;
    int removedRuns = 0;
    bool keptPosted = false;
    bool removedPosted = false;
    RunOnWorker([&]() {
        keptPosted = keptPtr->PostSafeAction([&keptRuns]() { ++keptRuns; });
        removedPosted = removedPtr->PostSafeAction([&removedRuns]() { ++removedRuns; });
    });
    ASSERT_TRUE(keptPosted);
    ASSERT_TRUE(removedPosted);

    root->RemoveChild(removedPtr);
    ui.DrainDeferredActionsOnce();

    EXPECT_EQ(keptRuns, 1);
    EXPECT_EQ(removedRuns, 0) << "a safe action ran after its element was destroyed";
}

// A post handle is taken on the UI thread and used by a worker that never touches the element.
// It follows the element into another manager, so the worker keeps reaching it after the window
// it started in has closed.
TEST(PostActionThreadingTests, APostHandleFollowsItsElementAcrossATearOff)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    auto panel = std::make_unique<UIElement>();
    MountHost docked = MakeMountHost(dev.get());
    MountHost floating = MakeMountHost(dev.get());
    docked.HostMount->SetTarget(panel.get());
    const UI::UiPostHandle post = panel->GetPostHandle();

    int runs = 0;
    bool postedWhileDocked = false;
    RunOnWorker([&]() { postedWhileDocked = post.Post([&runs]() { ++runs; }); });
    ASSERT_TRUE(postedWhileDocked);
    docked.Manager->DrainDeferredActionsOnce();
    ASSERT_EQ(runs, 1) << "positive control: the handle reaches the first owner";

    floating.HostMount->SetTarget(panel.get());
    docked.Manager.reset();

    bool postedAfterTearOff = false;
    RunOnWorker([&]() { postedAfterTearOff = post.Post([&runs]() { ++runs; }); });
    EXPECT_TRUE(postedAfterTearOff) << "the handle did not follow the element to its new manager";
    floating.Manager->DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 2);
}

// The case a raw widget pointer cannot handle: the element is destroyed while a worker still
// holds a way to post to it. The handle refuses new posts and skips the one already queued.
TEST(PostActionThreadingTests, APostHandleOutlivesItsElementSafely)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev.get());
    auto rootOwned = std::make_unique<UIElement>();
    UIElement* root = rootOwned.get();
    ui.SetRoot(std::move(rootOwned));
    auto childOwned = std::make_unique<UIElement>();
    UIElement* child = childOwned.get();
    root->AddChild(std::move(childOwned));
    const UI::UiPostHandle post = child->GetPostHandle();

    int runs = 0;
    bool queuedBeforeDestruction = false;
    RunOnWorker([&]() { queuedBeforeDestruction = post.Post([&runs]() { ++runs; }); });
    ASSERT_TRUE(queuedBeforeDestruction);

    root->RemoveChild(child); // destroys it

    bool postedAfterDestruction = true;
    RunOnWorker([&]() { postedAfterDestruction = post.Post([&runs]() { ++runs; }); });
    EXPECT_FALSE(postedAfterDestruction) << "a handle to a destroyed element accepted a post";

    ui.DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 0) << "an action queued before the element was destroyed ran after it";

    const UI::UiPostHandle empty;
    EXPECT_FALSE(empty.Post([&runs]() { ++runs; }));
}

// The VCS-poll shape: a listener thread requests a refresh many times between two UI frames.
TEST(PostActionThreadingTests, ACoalescedPostRunsOncePerBurstOfWorkerRequests)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev.get());
    auto rootOwned = std::make_unique<UIElement>();
    UIElement* root = rootOwned.get();
    ui.SetRoot(std::move(rootOwned));

    int runs = 0;
    const UI::UiCoalescedPost refresh(root->GetPostHandle(), [&runs]() { ++runs; });
    constexpr int kBurst = 32;

    const size_t pendingBefore = ui.GetDispatcher()->PendingCount();
    RunOnWorker([refresh]() {
        for (int i = 0; i < kBurst; ++i)
            refresh.Request();
    });
    EXPECT_EQ(ui.GetDispatcher()->PendingCount(), pendingBefore + 1)
        << "a burst of requests queued more than one run";
    ui.DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 1);

    RunOnWorker([refresh]() { refresh.Request(); });
    ui.DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 2) << "a request after the run started was absorbed";
}

// The owner's teardown: Cancel stops a run that is already queued and every later request.
TEST(PostActionThreadingTests, ACancelledCoalescedPostNeverRunsAgain)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev.get());
    auto rootOwned = std::make_unique<UIElement>();
    UIElement* root = rootOwned.get();
    ui.SetRoot(std::move(rootOwned));

    int runs = 0;
    const UI::UiCoalescedPost refresh(root->GetPostHandle(), [&runs]() { ++runs; });
    const size_t pendingBefore = ui.GetDispatcher()->PendingCount();
    RunOnWorker([refresh]() { refresh.Request(); });
    ASSERT_EQ(ui.GetDispatcher()->PendingCount(), pendingBefore + 1)
        << "positive control: the request was queued";

    refresh.Cancel();
    RunOnWorker([refresh]() { refresh.Request(); });
    EXPECT_EQ(ui.GetDispatcher()->PendingCount(), pendingBefore + 1) << "a cancelled request was queued";
    ui.DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 0);
}

// A refused post must release the latch, or the first refusal would silence every later request.
TEST(PostActionThreadingTests, ACoalescedPostRefusedWhileDetachedPostsAgainOnceAttached)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev.get());
    auto rootOwned = std::make_unique<UIElement>();
    UIElement* root = rootOwned.get();
    ui.SetRoot(std::move(rootOwned));

    auto detached = std::make_unique<UIElement>();
    int runs = 0;
    const UI::UiCoalescedPost refresh(detached->GetPostHandle(), [&runs]() { ++runs; });
    const size_t pendingBefore = ui.GetDispatcher()->PendingCount();
    RunOnWorker([refresh]() { refresh.Request(); });
    EXPECT_EQ(ui.GetDispatcher()->PendingCount(), pendingBefore) << "a detached element's request was queued";

    root->AddChild(std::move(detached));
    RunOnWorker([refresh]() { refresh.Request(); });
    ui.DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 1) << "the refused request left the latch set";
}
