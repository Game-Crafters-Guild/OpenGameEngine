// Rebinding an asset source must never block the thread that asks for it.
//
// The eject a rebind starts can withdraw a queued read and a decode job that has
// not started; what it cannot withdraw is a decode already executing, which may
// still register the asset the eject is about to drop. That one state is what
// the eject has to outlast, and it used to do so with an unbounded
// `std::future::wait()` on the calling thread — in the editor the thread that
// runs the frame, in the browser build the requestAnimationFrame callback, where
// one load that never finishes kills the tab. The wait is a per-frame readiness
// test now.
//
// So the held-decode cases here hold the decode inside the asset's own parse,
// not by occupying the pool: a pool hold leaves the job Pending, and a Pending
// job is cancelled inline, which would make the test pass for the wrong reason
// on a host whose file read is faster than this one's.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h" // AssetSourceDesc
#include "JobSystem/WorkStealingThreadPool.h"

#include "Assets/ParserRegistry.h"
#include "Assets/BinaryAsset.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace GameEngine;

std::filesystem::path MakeUniqueTempDir()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("ge_source_rebind_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

// A project shaped the way the editor expects. Identity is derived from the
// alias plus the relative path (BinaryAsset, no database needed), which is what
// the sibling starvation suite uses to exercise the real load pipeline without
// a renderer or an EngineCore.
struct ProjectDirs
{
    std::filesystem::path Root;
    std::filesystem::path Assets;
};

void WriteAssetFile(const std::filesystem::path& dir, const std::string& name, const std::string& payload)
{
    std::ofstream file(dir / name, std::ios::binary | std::ios::trunc);
    file << payload;
}

ProjectDirs MakeProject(const std::filesystem::path& parent, const std::string& name, int assetCount)
{
    ProjectDirs dirs;
    dirs.Root = parent / name;
    dirs.Assets = dirs.Root / "Assets";
    std::filesystem::create_directories(dirs.Assets);
    for (int i = 0; i < assetCount; ++i)
    {
        std::ofstream file(dirs.Assets / (name + "_" + std::to_string(i) + ".bin"), std::ios::binary | std::ios::trunc);
        file << "rebind-payload-" << name << '-' << i;
    }
    return dirs;
}

AssetSourceDesc MakeSource(const ProjectDirs& dirs, const std::string& alias)
{
    AssetSourceDesc source;
    source.Alias = alias;
    source.Root = dirs.Assets;
    source.DerivedIdentity = true; // GUID = hash(alias + rel path); no DB required
    source.RequiresScan = true;
    return source;
}

AssetManager::SourceRebindDesc MakeRebind(const ProjectDirs& dirs)
{
    AssetManager::SourceRebindDesc desc;
    desc.NewRoot = dirs.Assets;
    return desc;
}

// Holds every decode of a .bin asset until the test releases it, from inside
// the decode job, so the load sits in the one state an eject cannot withdraw.
// Registered at a higher priority than the engine's own binary parser, which it
// otherwise defers to for the actual bytes.
class HeldBinaryParser : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Unknown; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".bin"}; }
    int GetPriority() const override { return 1000; }
    std::string GetName() const override { return "HeldBinaryParser"; }

    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override
    {
        // Only the files named for it are held; everything else decodes
        // normally, so one test can have a load stuck and another free.
        if (metadata.Path.filename().string().rfind("hold", 0) == 0)
        {
            Entered.fetch_add(1, std::memory_order_release);
            Gate.wait();
        }
        return m_Inner.Parse(metadata, assetManager);
    }

    /// Incremented as each decode enters, before it blocks.
    std::atomic<int> Entered{0};
    /// Released by the test (or by its watchdog) to let every decode through.
    std::shared_future<void> Gate;

  private:
    BinaryAssetParser m_Inner;
};

// The registry stores paths normalised (lower case, forward slashes), so a
// resident asset's path is compared in that form rather than verbatim.
std::string LowerGeneric(const std::filesystem::path& path)
{
    std::string s = path.generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Wait until at least `count` decodes are inside the parser, i.e. running.
bool WaitForDecodesToEnter(const HeldBinaryParser& parser, int count,
                           std::chrono::seconds budget = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (parser.Entered.load(std::memory_order_acquire) < count)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// Joins its thread however the test leaves the scope. A gtest ASSERT_ returns
// from the test body, and a joinable std::thread destroyed there terminates the
// process instead of reporting the failure.
struct JoinOnScopeExit
{
    std::thread Thread;
    void Join()
    {
        if (Thread.joinable())
            Thread.join();
    }
    ~JoinOnScopeExit() { Join(); }
};

// Drive Update() until `done` or the deadline. Each iteration is real work, not
// a sleep: Update is where a rebind whose loads were still in flight lands.
bool ConvergeOnUpdate(AssetManager& assets, const std::atomic<bool>& done,
                      std::chrono::seconds budget = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        assets.Update();
        std::this_thread::yield();
    }
    return true;
}

} // namespace

TEST(SourceRebindAsync, RebindWithNothingInFlightCompletesInsideTheCall)
{
    const auto root = MakeUniqueTempDir();
    const ProjectDirs projectA = MakeProject(root, "ProjectA", 2);
    const ProjectDirs projectB = MakeProject(root, "ProjectB", 2);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&pool));
    ASSERT_TRUE(assets.RegisterSource(MakeSource(projectA, "project")));
    assets.WaitForStartupScan("project");

    bool rebound = false;
    bool reboundBeforeReturn = false;
    const bool started = assets.BeginRebindSource("project", MakeRebind(projectB),
                                                  [&](bool ok)
                                                  {
                                                      rebound = ok;
                                                      reboundBeforeReturn = true;
                                                  });
    ASSERT_TRUE(started);
    // Nothing of ProjectA is in flight, so the eject drains inside its own call
    // and the rebind lands before this line. Desktop behaviour is unchanged.
    EXPECT_TRUE(reboundBeforeReturn);
    EXPECT_TRUE(rebound);
    EXPECT_EQ(assets.GetSourceRoot("project"), projectB.Assets);

    assets.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(SourceRebindAsync, RejectedRebindChangesNothing)
{
    const auto root = MakeUniqueTempDir();
    const ProjectDirs projectA = MakeProject(root, "ProjectA", 1);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&pool));
    ASSERT_TRUE(assets.RegisterSource(MakeSource(projectA, "project")));
    assets.WaitForStartupScan("project");

    bool called = false;
    AssetManager::SourceRebindDesc missing;
    missing.NewRoot = root / "NoSuchProject" / "Assets";
    EXPECT_FALSE(assets.BeginRebindSource("project", missing, [&](bool) { called = true; }));
    EXPECT_FALSE(called);
    EXPECT_EQ(assets.GetSourceRoot("project"), projectA.Assets);

    EXPECT_FALSE(assets.BeginRebindSource("no-such-alias", MakeRebind(projectA), [&](bool) { called = true; }));
    EXPECT_FALSE(called);

    assets.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(SourceRebindAsync, RebindWithARunningDecodeReturnsBeforeTheDecodeDoes)
{
    const auto root = MakeUniqueTempDir();
    const ProjectDirs projectA = MakeProject(root, "ProjectA", 0);
    const ProjectDirs projectB = MakeProject(root, "ProjectB", 1);
    WriteAssetFile(projectA.Assets, "hold_0.bin", "held inside its decode");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&pool));

    auto held = std::make_shared<HeldBinaryParser>();
    std::promise<void> release;
    held->Gate = release.get_future().share();
    ASSERT_TRUE(assets.GetParserRegistry().RegisterParser(held, held->GetPriority()));

    ASSERT_TRUE(assets.RegisterSource(MakeSource(projectA, "project")));
    assets.WaitForStartupScan("project");

    const GUID heldGuid = assets.ResolveAssetGuid(projectA.Assets / "hold_0.bin", "project");
    ASSERT_FALSE(heldGuid.IsNull());
    AssetLoadHandle heldLoad = assets.LoadAsset(heldGuid, [](Result<SharedPtr<Asset>, AssetError>) {});

    // Running, not queued: the eject cannot take this one back.
    ASSERT_TRUE(WaitForDecodesToEnter(*held, 1));

    // A blocking implementation would wait here forever, so the watchdog turns
    // the hang into a late return that the assertion below can fail on. Joined
    // by the guard on every exit path: a failing ASSERT_ returns from here, and
    // a joinable thread at that point aborts the process instead of reporting.
    std::atomic<bool> releasedByWatchdog{false};
    JoinOnScopeExit watchdog{std::thread(
        [&release, &releasedByWatchdog]
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            releasedByWatchdog.store(true, std::memory_order_release);
            release.set_value();
        })};

    std::atomic<bool> rebound{false};
    const auto beganAt = std::chrono::steady_clock::now();
    const bool started = assets.BeginRebindSource("project", MakeRebind(projectB),
                                                  [&rebound](bool) { rebound.store(true, std::memory_order_release); });
    const auto returnedAfter = std::chrono::steady_clock::now() - beganAt;
    ASSERT_TRUE(started);

    // The call must not have waited for the running decode. The watchdog holds
    // it for three seconds, so the bound only has to separate "returned" from
    // "blocked"; it is deliberately far above any scheduling hiccup on a loaded
    // machine.
    EXPECT_LT(returnedAfter, std::chrono::seconds(2));
    EXPECT_FALSE(releasedByWatchdog.load(std::memory_order_acquire));
    EXPECT_FALSE(rebound.load(std::memory_order_acquire));
    EXPECT_EQ(assets.GetSourceRoot("project"), projectA.Assets);

    watchdog.Join();
    EXPECT_TRUE(ConvergeOnUpdate(assets, rebound));
    EXPECT_EQ(assets.GetSourceRoot("project"), projectB.Assets);

    assets.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(SourceRebindAsync, ASecondBeginForAnAliasWithAPendingEjectIsRefused)
{
    // Two ejects of one alias would share a suppression window that the first
    // to drain lifts, and their continuations would run against the same source
    // in an order neither caller chose. The API refuses the second instead.
    const auto root = MakeUniqueTempDir();
    const ProjectDirs projectA = MakeProject(root, "ProjectA", 0);
    const ProjectDirs projectB = MakeProject(root, "ProjectB", 1);
    const ProjectDirs projectC = MakeProject(root, "ProjectC", 1);
    WriteAssetFile(projectA.Assets, "hold_0.bin", "held inside its decode");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&pool));

    auto held = std::make_shared<HeldBinaryParser>();
    std::promise<void> release;
    held->Gate = release.get_future().share();
    ASSERT_TRUE(assets.GetParserRegistry().RegisterParser(held, held->GetPriority()));

    ASSERT_TRUE(assets.RegisterSource(MakeSource(projectA, "project")));
    assets.WaitForStartupScan("project");

    const GUID heldGuid = assets.ResolveAssetGuid(projectA.Assets / "hold_0.bin", "project");
    ASSERT_FALSE(heldGuid.IsNull());
    AssetLoadHandle heldLoad = assets.LoadAsset(heldGuid, [](Result<SharedPtr<Asset>, AssetError>) {});
    ASSERT_TRUE(WaitForDecodesToEnter(*held, 1));

    JoinOnScopeExit watchdog{std::thread(
        [&release]
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            release.set_value();
        })};

    std::atomic<bool> rebound{false};
    ASSERT_TRUE(assets.BeginRebindSource("project", MakeRebind(projectB),
                                         [&rebound](bool) { rebound.store(true, std::memory_order_release); }));

    bool secondRebound = false;
    EXPECT_FALSE(assets.BeginRebindSource("project", MakeRebind(projectC),
                                          [&secondRebound](bool) { secondRebound = true; }));
    bool unregistered = false;
    EXPECT_FALSE(assets.BeginUnregisterSource("project", [&unregistered](bool) { unregistered = true; }));
    EXPECT_FALSE(secondRebound);
    EXPECT_FALSE(unregistered);

    watchdog.Join();
    EXPECT_TRUE(ConvergeOnUpdate(assets, rebound));

    // The refused calls left the first one to finish on its own terms.
    EXPECT_EQ(assets.GetSourceRoot("project"), projectB.Assets);
    EXPECT_FALSE(secondRebound);
    EXPECT_FALSE(unregistered);

    // Refused while pending, accepted once drained.
    std::atomic<bool> later{false};
    EXPECT_TRUE(assets.BeginRebindSource("project", MakeRebind(projectC),
                                         [&later](bool) { later.store(true, std::memory_order_release); }));
    EXPECT_TRUE(ConvergeOnUpdate(assets, later));
    EXPECT_EQ(assets.GetSourceRoot("project"), projectC.Assets);

    assets.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(SourceRebindAsync, ALoadRequestedWhileTheEjectIsPendingIsRefusedAndRetriesAgainstTheNewRoot)
{
    // An eject that no longer blocks its caller leaves a window in which the
    // source is still mounted and a request for one of its assets can arrive:
    // the eject's own cancellations resolve waiters, and a waiter's usual
    // reaction is to ask again. If such a request were served, it would resolve
    // against the outgoing root, and the eject's finish would then erase its
    // in-flight entry — dropping the callbacks joined to it — while its decode
    // registered the outgoing root's bytes under a GUID the incoming root owns.
    const auto root = MakeUniqueTempDir();
    const ProjectDirs projectA = MakeProject(root, "ProjectA", 0);
    const ProjectDirs projectB = MakeProject(root, "ProjectB", 0);
    WriteAssetFile(projectA.Assets, "hold_0.bin", "held while the eject is pending");
    WriteAssetFile(projectA.Assets, "shared_0.bin", "the outgoing root's bytes");
    // Same relative name under the same alias is the same derived GUID, which is
    // what makes a stale resident from the outgoing root observable.
    WriteAssetFile(projectB.Assets, "shared_0.bin", "the incoming root's bytes");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&pool));

    auto held = std::make_shared<HeldBinaryParser>();
    std::promise<void> release;
    held->Gate = release.get_future().share();
    ASSERT_TRUE(assets.GetParserRegistry().RegisterParser(held, held->GetPriority()));

    ASSERT_TRUE(assets.RegisterSource(MakeSource(projectA, "project")));
    assets.WaitForStartupScan("project");

    const GUID sharedGuid = assets.ResolveAssetGuid(projectA.Assets / "shared_0.bin", "project");
    ASSERT_FALSE(sharedGuid.IsNull());

    // One load stuck in its decode is what keeps the eject pending for the rest
    // of this test; it is not the asset under test.
    const GUID holdGuid = assets.ResolveAssetGuid(projectA.Assets / "hold_0.bin", "project");
    ASSERT_FALSE(holdGuid.IsNull());
    AssetLoadHandle holdLoad = assets.LoadAsset(holdGuid, [](Result<SharedPtr<Asset>, AssetError>) {});
    ASSERT_TRUE(WaitForDecodesToEnter(*held, 1));

    std::atomic<bool> rebound{false};
    ASSERT_TRUE(assets.BeginRebindSource("project", MakeRebind(projectB),
                                         [&rebound](bool) { rebound.store(true, std::memory_order_release); }));
    ASSERT_FALSE(rebound.load(std::memory_order_acquire));

    // The request that arrives inside the window.
    std::atomic<int> inWindowCallbacks{0};
    AssetLoadHandle inWindow = assets.LoadAsset(
        sharedGuid, [&inWindowCallbacks](Result<SharedPtr<Asset>, AssetError>)
        { inWindowCallbacks.fetch_add(1, std::memory_order_release); });

    release.set_value();
    EXPECT_TRUE(ConvergeOnUpdate(assets, rebound));
    EXPECT_EQ(assets.GetSourceRoot("project"), projectB.Assets);

    // Answered rather than dropped by the eject's finish.
    EXPECT_EQ(inWindowCallbacks.load(std::memory_order_acquire), 1);
    // And nothing of the outgoing root left resident under the shared GUID.
    EXPECT_FALSE(assets.IsAssetLoaded(sharedGuid));

    // Once the eject is done, the same GUID loads from the root that is live.
    const GUID afterGuid = assets.ResolveAssetGuid(projectB.Assets / "shared_0.bin", "project");
    EXPECT_EQ(afterGuid, sharedGuid);
    std::atomic<int> afterCallbacks{0};
    AssetLoadHandle afterLoad = assets.LoadAsset(
        afterGuid, [&afterCallbacks](Result<SharedPtr<Asset>, AssetError>)
        { afterCallbacks.fetch_add(1, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (afterCallbacks.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
    {
        assets.Update();
        std::this_thread::yield();
    }
    EXPECT_EQ(afterCallbacks.load(std::memory_order_acquire), 1);
    ASSERT_TRUE(assets.IsAssetLoaded(afterGuid));
    SharedPtr<Asset> loaded = assets.GetAsset(afterGuid);
    ASSERT_TRUE(loaded != nullptr);
    EXPECT_EQ(LowerGeneric(loaded->GetPath()), LowerGeneric(projectB.Assets / "shared_0.bin"));

    assets.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(SourceRebindAsync, RebindWaitsForAStoreSaveWithoutBlockingTheCaller)
{
    // Unregistering or rebinding a source destroys objects an in-flight store
    // save holds raw pointers to, so neither may run while one is writing. The
    // registry enforces that by waiting; the thread that runs the frame must
    // not, so the eject holds the rebind until the save releases the store.
    const auto root = MakeUniqueTempDir();
    ProjectDirs projectA = MakeProject(root, "ProjectA", 1);
    const ProjectDirs projectB = MakeProject(root, "ProjectB", 1);

    // One worker, so the save the registry submits cannot run until released.
    JobSystem::WorkStealingThreadPool pool(1);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&pool));

    AssetSourceDesc source = MakeSource(projectA, "project");
    source.AuthoritativeDbFile = projectA.Root / "AssetDatabase.assetdb";
    source.CacheRoot = projectA.Root / ".Cache" / "AssetDatabase";
    ASSERT_TRUE(assets.RegisterSource(source));
    assets.WaitForStartupScan("project");

    std::promise<void> releaseSave;
    std::shared_future<void> saveReleased = releaseSave.get_future().share();
    pool.EnqueueWork([saveReleased]() { saveReleased.wait(); }, JobSystem::JobPriority::Normal);

    // Drive Update until the registry has claimed the store for a save. It
    // claims on this thread and submits the write to the pool, which is held,
    // so the claim stays. Asserting it is what keeps this test from passing
    // vacuously on a build where no flush is ever attempted.
    const auto claimDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!assets.GetRegistry().IsSourceFlushInProgress("project") &&
           std::chrono::steady_clock::now() < claimDeadline)
    {
        assets.Update();
        std::this_thread::yield();
    }
    ASSERT_TRUE(assets.GetRegistry().IsSourceFlushInProgress("project"))
        << "no store save was ever claimed, so this test would prove nothing";

    std::atomic<bool> rebound{false};
    const auto beganAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(assets.BeginRebindSource("project", MakeRebind(projectB),
                                         [&rebound](bool) { rebound.store(true, std::memory_order_release); }));
    const auto returnedAfter = std::chrono::steady_clock::now() - beganAt;

    EXPECT_LT(returnedAfter, std::chrono::seconds(2));
    EXPECT_FALSE(rebound.load(std::memory_order_acquire));
    EXPECT_EQ(assets.GetSourceRoot("project"), projectA.Assets);

    releaseSave.set_value();
    EXPECT_TRUE(ConvergeOnUpdate(assets, rebound));
    EXPECT_EQ(assets.GetSourceRoot("project"), projectB.Assets);

    assets.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(SourceRebindAsync, UnregisterSourceCompletesThroughUpdate)
{
    const auto root = MakeUniqueTempDir();
    const ProjectDirs projectA = MakeProject(root, "ProjectA", 1);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&pool));
    ASSERT_TRUE(assets.RegisterSource(MakeSource(projectA, "package-under-test")));
    assets.WaitForStartupScan("package-under-test");

    std::atomic<bool> unregistered{false};
    ASSERT_TRUE(assets.BeginUnregisterSource("package-under-test",
                                             [&unregistered](bool ok)
                                             { unregistered.store(ok, std::memory_order_release); }));
    EXPECT_TRUE(ConvergeOnUpdate(assets, unregistered));
    EXPECT_TRUE(assets.GetSourceRoot("package-under-test").empty());

    EXPECT_FALSE(assets.BeginUnregisterSource("package-under-test"));

    assets.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
