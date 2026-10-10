#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <optional>
#include <thread>
#include "Assets/FileWatchingService.h"
#include "TestTempDir.h"

using namespace GameEngine;

namespace
{
constexpr auto kEventDeadline = std::chrono::seconds(10);

/// Rewrites @p file until @p done holds or kEventDeadline passes. One write can
/// be lost to the watcher's arming window, so a single write would decide by luck.
bool RewriteUntil(const std::filesystem::path& file, const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + kEventDeadline;
    for (int revision = 0; !done() && std::chrono::steady_clock::now() < deadline; ++revision)
    {
        {
            std::ofstream out(file, std::ios::trunc);
            out << "revision " << revision;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return done();
}

/// A wait that never ends leaves a watcher thread blocked for good, and the
/// teardown join would then hang the whole binary. Fail by name and stop here.
[[noreturn]] void AbortOnDeadlock(const char* what)
{
    ADD_FAILURE() << what;
    std::fflush(stdout);
    std::abort();
}
} // namespace

TEST(FilePattern, RecursiveAndExtensions)
{
    namespace fs = std::filesystem;
    fs::path base = fs::current_path() / "FilePatternTestRoot";

    FilePattern pat(base, ".*", {".dll", ".txt"}, true);

    EXPECT_TRUE(pat.Matches(base / "a.txt"));
    EXPECT_TRUE(pat.Matches(base / "sub" / "b.dll"));
    EXPECT_FALSE(pat.Matches(base / "sub" / "c.exe"));
    EXPECT_FALSE(pat.Matches(base.parent_path() / "outside.txt"));
}

TEST(FilePattern, NonRecursiveBlocksSubdirs)
{
    namespace fs = std::filesystem;
    fs::path base = fs::current_path() / "FilePatternTestRoot";

    FilePattern pat(base, ".*", {".dll"}, false);

    EXPECT_TRUE(pat.Matches(base / "m.dll"));
    EXPECT_FALSE(pat.Matches(base / "sub" / "n.dll")); // subdir blocked
}

TEST(FilePattern, RegexFilter)
{
    namespace fs = std::filesystem;
    fs::path base = fs::current_path() / "FilePatternTestRoot";

    FilePattern pat(base, R"([A-Za-z_]+\.dll)", {}, true);

    EXPECT_TRUE(pat.Matches(base / "Foo.dll"));
    EXPECT_FALSE(pat.Matches(base / "123.dll"));
    EXPECT_FALSE(pat.Matches(base / "Bar.pdb"));
}

// The reason an empty directory is refused at Subscribe: relative-path
// containment against an empty root can never succeed, so such a pattern is
// dead on arrival however it is watched.
TEST(FilePattern, EmptyDirectoryMatchesNothing)
{
    namespace fs = std::filesystem;

    FilePattern pat(fs::path(), ".*", {}, true);

    EXPECT_FALSE(pat.Matches(fs::current_path() / "a.txt"));
    EXPECT_FALSE(pat.Matches(fs::current_path() / "sub" / "b.navgrid"));
    EXPECT_FALSE(pat.Matches(fs::current_path().parent_path() / "c.txt"));
}

// An empty directory is a caller bug, not a watch. Registering it anyway costs
// a permanently dead subscription plus a DirectoryWatcher that fails to start
// on every attempt — and a failed watcher makes StartWatching() report failure
// for the whole service. Refuse it at the boundary instead.
TEST(FileWatchingService, EmptyDirectorySubscriptionIsRefused)
{
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const auto before = service.GetStats();

    {
        auto sub = service.Subscribe(FilePattern(fs::path(), ".*", {".navgrid"}, true),
                                     [](const FileChangeEvent&) {});

        EXPECT_FALSE(sub.IsValid()) << "a refused subscription must come back inert";
        EXPECT_EQ(sub.GetId(), FileWatchSubscription::kInvalidSubscriptionId);

        const auto mid = service.GetStats();
        EXPECT_EQ(mid.TotalSubscriptions, before.TotalSubscriptions)
            << "refused subscription was registered anyway";
        EXPECT_EQ(mid.TotalDirectories, before.TotalDirectories)
            << "empty directory created a watcher that can never start";
    }

    // Destroying an inert handle must not disturb the service's bookkeeping.
    const auto after = service.GetStats();
    EXPECT_EQ(after.TotalSubscriptions, before.TotalSubscriptions);
    EXPECT_EQ(after.TotalDirectories, before.TotalDirectories);

    // Positive control: the counters read above do move for a real directory,
    // so the equalities are a refusal rather than a dead instrument.
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_filewatcher_empty_refusal_control");
    std::error_code ec;
    fs::create_directories(root, ec);
    {
        auto control = service.Subscribe(FilePattern(root, ".*", {}, true),
                                         [](const FileChangeEvent&) {});
        EXPECT_TRUE(control.IsValid());

        const auto withControl = service.GetStats();
        EXPECT_EQ(withControl.TotalSubscriptions, before.TotalSubscriptions + 1);
        EXPECT_EQ(withControl.TotalDirectories, before.TotalDirectories + 1);
    }
    fs::remove_all(root, ec);
}

// A directory that cannot be watched is a local failure, not a global one.
// Asset source roots are created best-effort (AssetRegistry ignores
// create_directories' error code and mounts the source anyway), so a single
// unwatchable root reaching the service is realistic — and it used to leave
// m_Watching false, silently disabling hot-reload for every other subscriber.
TEST(FileWatchingService, OneUnwatchableDirectoryDoesNotDisableTheRest)
{
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const fs::path good = TestUtils::MakeUniqueTempDirectory("ge_filewatcher_partial_good");
    const fs::path missing = TestUtils::MakeUniqueTempDirectory("ge_filewatcher_partial_missing");
    std::error_code ec;
    fs::create_directories(good, ec);
    fs::remove_all(missing, ec);
    ASSERT_FALSE(fs::exists(missing));

    std::atomic<bool> goodDelivered{false};
    auto badSub = service.Subscribe(FilePattern(missing, ".*", {}, true),
                                    [](const FileChangeEvent&) {});
    auto goodSub = service.Subscribe(FilePattern(good, ".*", {".txt"}, true),
                                     [&goodDelivered](const FileChangeEvent&) {
                                         goodDelivered.store(true);
                                     });

    EXPECT_FALSE(service.StartWatching())
        << "the unwatchable directory must still be reported through the return value";
    EXPECT_TRUE(service.IsWatching())
        << "one unwatchable directory disabled watching for every other subscriber";

    // ...and the healthy directory actually delivers.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (int revision = 0; !goodDelivered.load() &&
                           std::chrono::steady_clock::now() < deadline; ++revision)
    {
        {
            std::ofstream out(good / "probe.txt", std::ios::trunc);
            out << "revision " << revision;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_TRUE(goodDelivered.load())
        << "the watchable directory delivered no events while a sibling was unwatchable";

    service.StopWatching();
    fs::remove_all(good, ec);
}

TEST(FileWatchingService, CaseVariantSubscriptionsShareSingleWatcherOnWindows)
{
#ifndef _WIN32
    GTEST_SKIP() << "Windows-only watcher key normalization behavior";
#else
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const auto before = service.GetStats();

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_filewatcher_case_dedupe");
    std::error_code ec;
    fs::create_directories(root, ec);

    std::string lower = root.string();
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    const fs::path lowerPath(lower);

    {
        auto subA = service.Subscribe(FilePattern(root, ".*", {}, true), [](const FileChangeEvent&) {});
        auto subB = service.Subscribe(FilePattern(lowerPath, ".*", {}, true), [](const FileChangeEvent&) {});

        const auto mid = service.GetStats();
        EXPECT_EQ(mid.TotalSubscriptions, before.TotalSubscriptions + 2);
        EXPECT_EQ(mid.TotalDirectories, before.TotalDirectories + 1)
            << "Same physical directory with different path case should dedupe into one watcher";
    }

    const auto after = service.GetStats();
    EXPECT_EQ(after.TotalSubscriptions, before.TotalSubscriptions);
    EXPECT_EQ(after.TotalDirectories, before.TotalDirectories);

    fs::remove_all(root, ec);
#endif
}

TEST(FileWatchingService, SubscribingUnderRecursiveAncestorReusesWatcher)
{
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const auto before = service.GetStats();

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_filewatcher_subset_ancestor");
    const fs::path child = root / "nested";
    std::error_code ec;
    fs::create_directories(child, ec);

    {
        // Recursive watcher on the parent — covers 'nested' transitively.
        auto subParent = service.Subscribe(FilePattern(root, ".*", {}, true), [](const FileChangeEvent&) {});
        // Subscribing on the nested dir should ride the parent's watcher, not create a second one.
        auto subChild = service.Subscribe(FilePattern(child, ".*", {}, true), [](const FileChangeEvent&) {});

        const auto mid = service.GetStats();
        EXPECT_EQ(mid.TotalSubscriptions, before.TotalSubscriptions + 2);
        EXPECT_EQ(mid.TotalDirectories, before.TotalDirectories + 1)
            << "Child subscription under a recursive ancestor should reuse the ancestor's watcher";
    }

    const auto after = service.GetStats();
    EXPECT_EQ(after.TotalSubscriptions, before.TotalSubscriptions);
    EXPECT_EQ(after.TotalDirectories, before.TotalDirectories);

    fs::remove_all(root, ec);
}

TEST(FileWatchingService, MixedCaseAncestorReusesWatcherOnWindows)
{
#ifndef _WIN32
    GTEST_SKIP() << "Windows-only path case insensitivity";
#else
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const auto before = service.GetStats();

    const fs::path rootUpper = TestUtils::MakeUniqueTempDirectory("GE_FileWatcher_MixedCase");
    const fs::path rootLowerChild =
        fs::path(std::string([&] {
            std::string s = rootUpper.string();
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }())) / "Nested";
    std::error_code ec;
    fs::create_directories(rootLowerChild, ec);

    {
        // Recursive parent subscription with ORIGINAL case.
        auto subParent = service.Subscribe(FilePattern(rootUpper, ".*", {}, true),
                                           [](const FileChangeEvent&) {});
        // Child subscription with LOWERCASED ancestor portion.
        // Windows treats these as the same tree; IsStrictAncestor must agree.
        auto subChild = service.Subscribe(FilePattern(rootLowerChild, ".*", {}, true),
                                          [](const FileChangeEvent&) {});

        const auto mid = service.GetStats();
        EXPECT_EQ(mid.TotalDirectories, before.TotalDirectories + 1)
            << "Case-insensitive ancestor should still be recognized";
    }

    fs::remove_all(rootUpper, ec);
#endif
}

TEST(FileWatchingService, RecursiveParentConsolidatesExistingDescendantWatchers)
{
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const auto before = service.GetStats();

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_filewatcher_subset_consolidate");
    const fs::path childA = root / "a";
    const fs::path childB = root / "b";
    std::error_code ec;
    fs::create_directories(childA, ec);
    fs::create_directories(childB, ec);

    {
        // Two narrow watchers first — two OS handles.
        auto subA = service.Subscribe(FilePattern(childA, ".*", {}, true), [](const FileChangeEvent&) {});
        auto subB = service.Subscribe(FilePattern(childB, ".*", {}, true), [](const FileChangeEvent&) {});

        const auto afterNarrow = service.GetStats();
        EXPECT_EQ(afterNarrow.TotalDirectories, before.TotalDirectories + 2);

        // Wide recursive watcher on the shared ancestor — should absorb both.
        auto subWide = service.Subscribe(FilePattern(root, ".*", {}, true), [](const FileChangeEvent&) {});

        const auto afterWide = service.GetStats();
        EXPECT_EQ(afterWide.TotalSubscriptions, before.TotalSubscriptions + 3);
        EXPECT_EQ(afterWide.TotalDirectories, before.TotalDirectories + 1)
            << "Recursive parent subscription should consolidate existing descendant watchers";
    }

    // All three subs out of scope — both the original narrow watchers
    // (consolidated away) and the new wide one should be gone.
    const auto after = service.GetStats();
    EXPECT_EQ(after.TotalSubscriptions, before.TotalSubscriptions);
    EXPECT_EQ(after.TotalDirectories, before.TotalDirectories);

    fs::remove_all(root, ec);
}

// Destroying the handle is what lets a subscriber free what its callback uses,
// so unsubscribing must not return while that callback runs on the watcher
// thread, and nothing may call it afterwards. The second subscription shares
// the watcher and keeps it alive: removing the last one joins the watcher
// thread, which would wait for the callback by accident rather than by contract.
TEST(FileWatchingService, UnsubscribeWaitsForTheRunningCallback)
{
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const fs::path root = fs::temp_directory_path() / "ge_filewatcher_unsubscribe_waits";
    std::error_code ec;
    fs::create_directories(root, ec);

    std::promise<void> entered;
    std::future<void> enteredFuture = entered.get_future();
    std::promise<void> gate;
    const std::shared_future<void> gateOpen = gate.get_future().share();
    std::atomic<int> blockedCalls{0};
    std::atomic<bool> unsubscribeReturned{false};
    std::atomic<bool> calledAfterUnsubscribe{false};
    std::atomic<int> sharedCalls{0};

    auto shared = service.Subscribe(FilePattern(root, ".*", {".txt"}, true),
                                    [&sharedCalls](const FileChangeEvent&) { sharedCalls.fetch_add(1); });
    std::optional<FileWatchSubscription> blocked;
    blocked.emplace(service.Subscribe(
        FilePattern(root, ".*", {".txt"}, true),
        [&](const FileChangeEvent&) {
            if (blockedCalls.fetch_add(1) == 0)
            {
                entered.set_value();
                gateOpen.wait();
            }
            if (unsubscribeReturned.load())
                calledAfterUnsubscribe.store(true);
        }));

    ASSERT_TRUE(service.StartWatching());
    if (!RewriteUntil(root / "probe.txt", [&enteredFuture] {
            return enteredFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }))
    {
        // A call that arrives late must not block the teardown's unsubscribe.
        gate.set_value();
        FAIL() << "no event reached the subscription, so there was no running callback to wait for";
    }

    std::promise<void> unsubscribed;
    std::future<void> unsubscribedFuture = unsubscribed.get_future();
    std::thread unsubscriber([&] {
        blocked.reset();
        unsubscribeReturned.store(true);
        unsubscribed.set_value();
    });

    EXPECT_EQ(unsubscribedFuture.wait_for(std::chrono::milliseconds(500)), std::future_status::timeout)
        << "Unsubscribe returned while the subscription's callback was still running";

    gate.set_value();
    if (unsubscribedFuture.wait_for(kEventDeadline) != std::future_status::ready)
        AbortOnDeadlock("Unsubscribe did not return after the running callback returned");
    unsubscriber.join();

    // Events keep reaching the shared watcher, and only the live subscription.
    const int blockedCallsAtUnsubscribe = blockedCalls.load();
    const int sharedCallsAtUnsubscribe = sharedCalls.load();
    EXPECT_TRUE(RewriteUntil(root / "probe.txt", [&] { return sharedCalls.load() > sharedCallsAtUnsubscribe; }))
        << "positive control: the shared watcher delivered nothing after the unsubscribe";
    EXPECT_EQ(blockedCalls.load(), blockedCallsAtUnsubscribe);
    EXPECT_FALSE(calledAfterUnsubscribe.load())
        << "the callback ran after Unsubscribe returned";

    service.StopWatching();
    fs::remove_all(root, ec);
}

// A callback may drop its own subscription. It must not wait for its own call,
// which is further up the same stack, and later events must not reach it. The
// second subscription shares the watcher, so its events show the watcher still
// delivers.
TEST(FileWatchingService, CallbackCanUnsubscribeItself)
{
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const fs::path root = fs::temp_directory_path() / "ge_filewatcher_self_unsubscribe";
    std::error_code ec;
    fs::create_directories(root, ec);

    std::promise<void> unsubscribed;
    std::future<void> unsubscribedFuture = unsubscribed.get_future();
    std::atomic<int> selfCalls{0};
    std::atomic<int> sharedCalls{0};

    auto shared = service.Subscribe(FilePattern(root, ".*", {".txt"}, true),
                                    [&sharedCalls](const FileChangeEvent&) { sharedCalls.fetch_add(1); });
    std::optional<FileWatchSubscription> self;
    self.emplace(service.Subscribe(
        FilePattern(root, ".*", {".txt"}, true),
        [&](const FileChangeEvent&) {
            if (selfCalls.fetch_add(1) != 0)
                return;
            self.reset();
            unsubscribed.set_value();
        }));

    ASSERT_TRUE(service.StartWatching());
    RewriteUntil(root / "probe.txt", [&selfCalls] { return selfCalls.load() > 0; });
    ASSERT_GT(selfCalls.load(), 0) << "no event reached the subscription";
    if (unsubscribedFuture.wait_for(kEventDeadline) != std::future_status::ready)
        AbortOnDeadlock("a callback that unsubscribed itself never returned from Unsubscribe");

    const int sharedCallsAtUnsubscribe = sharedCalls.load();
    EXPECT_TRUE(RewriteUntil(root / "probe.txt", [&] { return sharedCalls.load() > sharedCallsAtUnsubscribe; }))
        << "positive control: the shared watcher delivered nothing after the unsubscribe";
    EXPECT_EQ(selfCalls.load(), 1) << "the callback ran after it unsubscribed itself";

    service.StopWatching();
    fs::remove_all(root, ec);
}

// Removing the last subscription on a watcher stops that watcher. From inside
// the watcher's own callback its thread cannot join itself, so the stop must
// be handed to another thread instead of terminating the process.
TEST(FileWatchingService, CallbackCanRemoveTheLastSubscriptionOnItsWatcher)
{
    namespace fs = std::filesystem;

    FileWatchingService& service = FileWatchingService::GetInstance();
    service.StopWatching();

    const auto before = service.GetStats();

    const fs::path root = fs::temp_directory_path() / "ge_filewatcher_self_remove_last";
    std::error_code ec;
    fs::create_directories(root, ec);

    std::promise<void> unsubscribed;
    std::future<void> unsubscribedFuture = unsubscribed.get_future();
    std::atomic<int> calls{0};

    std::optional<FileWatchSubscription> only;
    only.emplace(service.Subscribe(
        FilePattern(root, ".*", {".txt"}, true),
        [&](const FileChangeEvent&) {
            if (calls.fetch_add(1) != 0)
                return;
            only.reset();
            unsubscribed.set_value();
        }));
    ASSERT_EQ(service.GetStats().TotalDirectories, before.TotalDirectories + 1)
        << "the subscription must be the only one on its watcher";

    ASSERT_TRUE(service.StartWatching());
    RewriteUntil(root / "probe.txt", [&calls] { return calls.load() > 0; });
    ASSERT_GT(calls.load(), 0) << "no event reached the subscription";
    if (unsubscribedFuture.wait_for(kEventDeadline) != std::future_status::ready)
        AbortOnDeadlock("a callback that removed its watcher's last subscription never returned");

    EXPECT_EQ(service.GetStats().TotalDirectories, before.TotalDirectories)
        << "the emptied watcher is still registered";

    // Joins the watcher the callback handed off.
    service.StopWatching();
    EXPECT_EQ(calls.load(), 1);

    fs::remove_all(root, ec);
}

