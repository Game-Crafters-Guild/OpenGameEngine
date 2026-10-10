// Lifetime contract for EditorVersionControlService status subscriptions:
//   * a Subscription may outlive its service — the editor destroys the service
//     during OnShutdown and the panels that subscribed to it only later, so
//     unsubscribing from a dead service is the normal path, not a misuse;
//   * Shutdown() (which InitializeForProject runs on every project switch)
//     must NOT drop subscriptions — the same panels observe the next project;
//   * Reset() stops delivery.
//
// Compiles the editor's EditorVersionControlService.cpp directly, as the
// first-detect gate tests in this target do.

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "VersionControl/EditorVersionControlService.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ed = GameEngine::Editor;

namespace
{

class SubscriptionFakeIntegration final : public GameEngine::IVCSIntegration
{
public:
    bool Initialize(const std::filesystem::path&, const std::filesystem::path&) override
    {
        return true;
    }
    void Shutdown() override { Repository = false; }
    bool IsAvailable() const override { return true; }
    bool IsRepository() const override { return Repository; }
    GameEngine::VCSFileStatus GetFileStatus(const std::filesystem::path&) override
    {
        return GameEngine::VCSFileStatus::Clean;
    }
    bool Add(const std::filesystem::path&) override { return true; }
    bool Remove(const std::filesystem::path&) override { return true; }
    bool Move(const std::filesystem::path&, const std::filesystem::path&) override { return true; }
    bool Commit(const std::string&) override { return true; }
    bool Update() override { return true; }
    bool Revert(const std::filesystem::path&) override { return true; }
    bool IsIgnored(const std::filesystem::path&) override { return false; }
    void RefreshStatus(const std::filesystem::path&) override {}
    std::filesystem::path GetRepositoryRoot() const override { return {}; }
    std::string GetCurrentBranch() const override { return {}; }
    std::vector<GameEngine::VCSLogEntry> GetLog(const std::filesystem::path&, int) override
    {
        return {};
    }
    std::filesystem::path GetExecutable() const override { return {}; }
    void SetStatusChangedCallback(StatusChangedCallback) override {}

    bool Repository = false;
    // The service hands the provider a "status changed" callback; holding it
    // lets a test raise a status change the way a poll thread would.
    std::function<void()> StatusChanged;
};

std::filesystem::path MakeSubscriptionWorkspace(const std::string& name,
                                                const std::string& marker)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ge_vcs_subscription_tests" / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / marker);
    return root;
}

ed::EditorVcsProviderDescriptor MakeSubscriptionDescriptor(
    const std::string& typeId, SubscriptionFakeIntegration& integration, std::string markerDir)
{
    ed::EditorVcsProviderDescriptor descriptor;
    descriptor.TypeId = typeId;
    descriptor.DisplayName = typeId;
    descriptor.DetectionOrder = 0;
    descriptor.Detect = [marker = std::move(markerDir)](const std::filesystem::path& root) {
        std::error_code ec;
        return std::filesystem::is_directory(root / marker, ec);
    };
    descriptor.Integration = [&integration]() -> GameEngine::IVCSIntegration& {
        return integration;
    };
    descriptor.Initialize = [&integration](const std::filesystem::path&,
                                           std::function<void()> onStatusChanged) {
        integration.StatusChanged = std::move(onStatusChanged);
        integration.Repository = true;
        return true;
    };
    return descriptor;
}

} // namespace

// The editor's OnShutdown destroys the VCS service, then destroys the panels
// holding subscriptions to it. Unsubscribing at that point must not reach into
// the freed service — that is the shutdown SIGABRT this contract exists to
// prevent.
//
// What this pins is the contract, NOT memory safety: the predecessor took a
// mutex in freed memory, which is undefined behaviour that this assertion would
// happily pass. Only an ASan build (vs2026-x64-local-asan) turns that into a
// failure. The structural guarantee is what makes the contract hold — the
// registry lives in a shared_ptr the subscription only weakly references — and
// that is what a reader should check when this test goes green.
TEST(EditorVcsSubscriptionLifetime, SubscriptionOutlivingTheServiceIsInert)
{
    int calls = 0;
    GameEngine::EditorVersionControlService::Subscription subscription;
    {
        GameEngine::EditorVersionControlService service;
        subscription = service.AddListener([&calls]() { ++calls; });
    }

    // Unsubscribing from a destroyed service is a no-op, not a use-after-free.
    subscription.Reset();
    EXPECT_EQ(calls, 0);
}

// Shutdown() also runs on the project-switch path, where the subscribed panels
// outlive the switch and must keep observing.
TEST(EditorVcsSubscriptionLifetime, SubscriptionsSurviveProjectReinitialization)
{
    const std::filesystem::path root =
        MakeSubscriptionWorkspace("reinit", ".subscriptionMarker");

    static SubscriptionFakeIntegration integration;
    integration = SubscriptionFakeIntegration{};

    GameEngine::EditorVersionControlService service;
    ed::EditorVcsProviderRegistry::Get().RegisterProvider(MakeSubscriptionDescriptor(
        "vcsSubscriptionTest.reinit", integration, ".subscriptionMarker"));
    service.NotifyProvidersReady();

    int calls = 0;
    GameEngine::EditorVersionControlService::Subscription subscription =
        service.AddListener([&calls]() { ++calls; });

    // InitializeForProject runs Shutdown() internally before re-detecting.
    service.InitializeForProject(root);
    ASSERT_TRUE(integration.StatusChanged)
        << "provider must have been initialized with a status callback";

    const int afterInit = calls;
    EXPECT_GT(afterInit, 0) << "the claim broadcast must reach a subscriber that "
                               "subscribed before the project was initialized";

    // A later status change from the provider's poll thread must still land.
    integration.StatusChanged();
    EXPECT_EQ(calls, afterInit + 1);
}

TEST(EditorVcsSubscriptionLifetime, ResetStopsDelivery)
{
    const std::filesystem::path root =
        MakeSubscriptionWorkspace("reset", ".subscriptionResetMarker");

    static SubscriptionFakeIntegration integration;
    integration = SubscriptionFakeIntegration{};

    GameEngine::EditorVersionControlService service;
    ed::EditorVcsProviderRegistry::Get().RegisterProvider(MakeSubscriptionDescriptor(
        "vcsSubscriptionTest.reset", integration, ".subscriptionResetMarker"));
    service.NotifyProvidersReady();

    int calls = 0;
    GameEngine::EditorVersionControlService::Subscription subscription =
        service.AddListener([&calls]() { ++calls; });

    service.InitializeForProject(root);
    ASSERT_TRUE(integration.StatusChanged);
    ASSERT_GT(calls, 0);

    const int afterInit = calls;
    subscription.Reset();
    integration.StatusChanged();
    EXPECT_EQ(calls, afterInit) << "a reset subscription must stop receiving broadcasts";
}

// Characterizes the hazard that forces subscribers to carry a liveness flag:
// Broadcast copies the listener list and invokes it outside the lock, so a
// broadcast already in flight still runs a callback that Reset() has since
// removed — unsubscribing cannot retract it. Subscribers shorter-lived than
// the service (the editor's panels) must therefore clear a flag the callback
// checks; Reset() alone leaves a window on a destroyed object.
//
// This asserts the late invocation HAPPENS. If a future change makes Broadcast
// re-check the registry per callback, this test fails — and that failure is
// the signal that those liveness flags may finally be removable.
TEST(EditorVcsSubscriptionLifetime, ResetDoesNotDisarmABroadcastAlreadyInFlight)
{
    const std::filesystem::path root =
        MakeSubscriptionWorkspace("inflight", ".subscriptionInflightMarker");

    static SubscriptionFakeIntegration integration;
    integration = SubscriptionFakeIntegration{};

    GameEngine::EditorVersionControlService service;
    ed::EditorVcsProviderRegistry::Get().RegisterProvider(MakeSubscriptionDescriptor(
        "vcsSubscriptionTest.inflight", integration, ".subscriptionInflightMarker"));
    service.NotifyProvidersReady();
    service.InitializeForProject(root);
    ASSERT_TRUE(integration.StatusChanged);

    // Registration order is invocation order, so this first listener holds the
    // broadcast open at a point where the second one has been copied out but
    // not yet invoked — the window a panel can be destroyed in.
    std::promise<void> broadcastEntered;
    std::promise<void> releaseBroadcast;
    std::future<void> released = releaseBroadcast.get_future();
    // A second broadcast would satisfy the promise twice, and std::future_error
    // on the worker thread is an uncaught exception that takes the whole binary
    // down instead of failing this test. Only the first entry signals.
    std::once_flag entered;
    GameEngine::EditorVersionControlService::Subscription blocker =
        service.AddListener([&broadcastEntered, &released, &entered]() {
            std::call_once(entered, [&broadcastEntered]() { broadcastEntered.set_value(); });
            released.wait();
        });

    // Stands in for a panel. It records whether the callback reached it after
    // teardown — the dereference a real panel would not survive — and whether
    // the liveness flag it carries short-circuits first.
    auto alive = std::make_shared<std::atomic_bool>(true);
    std::atomic<bool> subscriberDestroyed{false};
    std::atomic<int> invocationsAfterReset{0};
    std::atomic<int> reachedBodyAfterReset{0};
    GameEngine::EditorVersionControlService::Subscription guarded = service.AddListener(
        [alive, &subscriberDestroyed, &invocationsAfterReset, &reachedBodyAfterReset]() {
            const bool destroyed = subscriberDestroyed.load(std::memory_order_acquire);
            if (destroyed)
                ++invocationsAfterReset;
            if (!alive->load(std::memory_order_acquire))
                return;
            if (destroyed)
                ++reachedBodyAfterReset;
        });

    std::thread statusThread([]() { integration.StatusChanged(); });
    broadcastEntered.get_future().wait();

    // The broadcast is now past the copy and blocked before the guarded
    // listener. Tear the subscriber down exactly as the panel does.
    alive->store(false, std::memory_order_release);
    guarded.Reset();
    subscriberDestroyed.store(true, std::memory_order_release);

    releaseBroadcast.set_value();
    statusThread.join();

    EXPECT_EQ(invocationsAfterReset.load(), 1)
        << "Broadcast no longer invokes a listener Reset() removed mid-flight; if "
           "that is intentional, the subscriber liveness flags guarding this window "
           "can be revisited";
    EXPECT_EQ(reachedBodyAfterReset.load(), 0)
        << "the liveness flag must short-circuit the callback the broadcast still holds";
}
