// Regression: a job-pool worker must never park on a future whose producer
// (the asset decode task) runs on the same pool.
//
// This reproduces the CreateLoadHandle futureWaitTask starvation that wedged
// ElvenRealm/Demo_unity.scene on a cold-cache open (fresh isolated appdata ->
// full-project FBX/thumbnail import storm -> flood of callback-loads). Each
// LoadAsset(guid, callback) submits a "futureWaitTask" that blocks on the decode
// future (AssetManager::CreateLoadHandle, the inner lambda's fut.get()); the
// decode itself is submitted to the SAME pool by an IO reader thread
// (AssetIOService::SubmitRead -> m_JobSystem->Submit). When more callback-loads
// than pool workers are in flight, the futureWaitTasks saturate every worker and
// the decode producers starve in the queue -> the loads never complete.
//
// Real-scene deadlock signature (cdb ~*kn), for greppability:
//   main   : WorkStealingThreadPool::Wait  <- ECS::SystemManager::UpdateWaveBased
//   workers: SubmitTask<AssetManager::CreateLoadHandle::<lambda>>::Execute
//            -> shared_future<shared_ptr<Asset>>::get -> condition_variable::wait
//
// Field trigger = cold-cache concurrent load flood (isolated/cold launches; a
// transient dangling AssetDatabase was one such instance, not a one-off). Recovery
// on a pre-fix binary: clear+rebuild <project>/.Cache/AssetDatabase.
//
// Pre-fix this test DEADLOCKS (times out -> RED). The continuation fix fires the
// callback on decode completion instead of parking a worker, so all loads
// complete regardless of pool size or load count (GREEN).

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h" // AssetSourceDesc
#include "AssetCore/GUID.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <atomic>
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
    auto dir = std::filesystem::temp_directory_path() / ("ge_asset_starve_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

// BinaryAsset (.bin) is the simplest device-free, registered asset type: the
// decode just holds the file bytes, so this exercises the real LoadAsset ->
// futureWaitTask -> IO-read -> pool-decode path without any renderer/EngineCore.
std::vector<std::filesystem::path> WriteBinaryAssets(const std::filesystem::path& dir, int count)
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        auto p = dir / ("starve_" + std::to_string(i) + ".bin");
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        const std::string payload = "ge-asset-load-starvation-payload-" + std::to_string(i);
        f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        paths.push_back(std::move(p));
    }
    return paths;
}

// Scaffolding for the cache-hit re-entrancy tests below: a pool-backed
// AssetManager over `count` throwaway .bin assets, each already resident in
// m_LoadedAssets with its in-flight entry retired.
struct WarmAssetFixture
{
    static constexpr size_t kPoolWorkers = 4;

    std::filesystem::path Dir;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> Pool;
    std::unique_ptr<AssetManager> Assets;
    std::vector<GUID> Guids;

    // Deadlock containment: a wedged exerciser holds m_LoadedAssetsMutex
    // forever, so destroying the manager (it waits on in-flight loads) or the
    // pool (it joins workers) would hang the binary instead of reporting.
    void Leak()
    {
        (void)Assets.release();
        (void)Pool.release();
    }

    void Teardown()
    {
        Assets.reset();
        Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(Dir, ec);
    }
};

// Warm-up deliberately loads each GUID exactly once, while it is still absent
// from m_LoadedAssets: that misses the already-loaded fast path entirely, so the
// warm-up itself cannot trip the defect under test on a pre-fix binary.
// Callbacks fire from the completion path AFTER EraseInFlightLoadGeneration, so
// a fired callback also proves the in-flight entry is gone -- which is what
// sends the NEXT LoadAsset down CreateLoadHandle's synchronous branch.
bool BuildWarmFixture(WarmAssetFixture& fx, int count, std::chrono::seconds timeout)
{
    fx.Dir = MakeUniqueTempDir();
    const std::vector<std::filesystem::path> paths = WriteBinaryAssets(fx.Dir, count);

    fx.Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(WarmAssetFixture::kPoolWorkers);
    fx.Assets = std::make_unique<AssetManager>();
    if (!fx.Assets->Initialize(fx.Pool.get()))
        return false;

    AssetSourceDesc source;
    source.Alias = "reentr";
    source.Root = fx.Dir;
    source.DerivedIdentity = true; // GUID = hash(alias + rel path); no DB required
    source.RequiresScan = true;
    if (!fx.Assets->RegisterSource(source))
        return false;
    fx.Assets->WaitForStartupScan("reentr");

    for (const auto& p : paths)
    {
        const GUID g = fx.Assets->ResolveAssetGuid(p, "reentr");
        if (g.IsNull())
            return false;
        fx.Guids.push_back(g);
    }

    // Handles are held for the whole wait, mirroring the real callers whose
    // requests stay withdrawable while the load runs.
    std::atomic<int> warmed{0};
    std::vector<AssetLoadHandle> handles;
    handles.reserve(fx.Guids.size());
    for (const GUID& g : fx.Guids)
    {
        handles.push_back(fx.Assets->LoadAsset(
            g,
            [&warmed](Result<SharedPtr<Asset>, AssetError>) { warmed.fetch_add(1, std::memory_order_acq_rel); },
            AssetLoadPriority::High));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (warmed.load(std::memory_order_acquire) < count && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return warmed.load(std::memory_order_acquire) == count;
}

} // namespace

TEST(AssetLoadPoolStarvation, ConcurrentCallbackLoadsDoNotStarveTinyPool)
{
    // Pool of 4: comfortably runs the bounded startup scan (the test thread waits
    // for it as an external, non-pool waiter), while 4 is far smaller than the
    // load count so the callback-loads saturate every worker.
    constexpr size_t kPoolWorkers = 4;
    constexpr int kLoadCount = 24; // >> pool workers -> guaranteed saturation pre-fix
    constexpr auto kTimeout = std::chrono::seconds(10);

    const std::filesystem::path assetDir = MakeUniqueTempDir();
    const std::vector<std::filesystem::path> assetPaths = WriteBinaryAssets(assetDir, kLoadCount);

    auto pool = std::make_unique<JobSystem::WorkStealingThreadPool>(kPoolWorkers);
    auto assets = std::make_unique<AssetManager>();
    ASSERT_TRUE(assets->Initialize(pool.get()));

    AssetSourceDesc source;
    source.Alias = "starve";
    source.Root = assetDir;
    source.DerivedIdentity = true; // GUID = hash(alias + rel path); no DB required
    source.RequiresScan = true;
    ASSERT_TRUE(assets->RegisterSource(source));

    // The startup scan runs on `pool`; block here (as an external waiter, not a
    // pool worker) until the .bin files are registered and resolvable.
    assets->WaitForStartupScan("starve");

    std::vector<GUID> guids;
    guids.reserve(assetPaths.size());
    for (const auto& p : assetPaths)
    {
        const GUID g = assets->ResolveAssetGuid(p, "starve");
        ASSERT_FALSE(g.IsNull()) << "could not resolve GUID for " << p.string();
        guids.push_back(g);
    }

    std::atomic<int> completed{0};
    // Hold the handles the way the real callers do (e.g. the editor
    // ThumbnailService that triggered the ElvenRealm wedge), so the futureWaitTasks
    // stay live and park -> starvation.
    std::vector<AssetLoadHandle> handles;
    handles.reserve(guids.size());
    for (const GUID& g : guids)
    {
        handles.push_back(assets->LoadAsset(
            g,
            [&completed](Result<SharedPtr<Asset>, AssetError>) {
                completed.fetch_add(1, std::memory_order_acq_rel);
            },
            AssetLoadPriority::High));
    }

    // Pre-fix: the futureWaitTasks (submitted synchronously here) grab all pool
    // workers and block on decode futures; the decodes (submitted afterward by IO
    // reader threads) starve -> `completed` stalls and this loop exhausts the
    // timeout. Post-fix: callbacks fire on decode completion -> completed reaches
    // kLoadCount well before the deadline.
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (completed.load(std::memory_order_acquire) < kLoadCount &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const int done = completed.load(std::memory_order_acquire);
    EXPECT_EQ(done, kLoadCount)
        << "asset-load pool starvation: only " << done << "/" << kLoadCount
        << " callback-loads completed within " << kTimeout.count()
        << "s. A pool worker is parked on a decode future whose producer is "
           "starved on the same pool (AssetManager::CreateLoadHandle futureWaitTask "
           "-> shared_future::get).";

    if (done == kLoadCount)
    {
        // Clean shutdown only when every load completed: destroying the manager
        // waits on in-flight loads, and joining the pool joins idle workers.
        handles.clear();
        assets.reset();
        pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(assetDir, ec);
    }
    else
    {
        // Deadlock repro: worker threads are permanently parked in
        // futureWaitTask's fut.get(); joining them (via either dtor) would hang
        // the whole test binary. Intentionally leak the manager + pool so the
        // failure is reported and the process moves on; the OS reaps the parked
        // threads at exit. (Temp dir is left behind; it is a unique scratch path.)
        (void)assets.release();
        (void)pool.release();
    }
}

// Regression: LoadAsset's already-loaded fast path must not hold
// m_LoadedAssetsMutex across CreateLoadHandle.
//
// Field signature (cdb ~*k on a cold-cache editor start; main wedged 440s+):
//   main    : RtlpAcquireSRWLockSharedContended
//             AssetManager::GetLoadedAsset       <- shared, NESTED
//             AssetManager::CreateLoadHandle
//             AssetManager::LoadAsset            <- shared, OUTER (fast path)
//             SceneViewPanel::LoadBindAttachLayoutAndStyle
//   workers : RtlpAcquireSRWLockExclusiveContended
//             AssetManager::RegisterLoadedAsset  <- decode threads publishing
//
// std::shared_mutex is not recursive (nested acquisition is UB) and MSVC builds
// it on SRWLOCK, which is writer-priority: the nested shared acquire parks
// behind the queued decode writers, and those writers wait on the outer shared
// that this same thread still holds. Nothing breaks the cycle.
//
// Rather than race for that window, this test pins the invariant
// deterministically: it lets the callback -- user code, which the fast path
// invokes from inside the lock scope -- take the writer side itself. Pre-fix
// that is a same-thread shared->exclusive acquire, an unconditional hang on the
// same lock and the same frames as the field stack.
TEST(AssetLoadCacheHitReentrancy, CacheHitCallbackMayUseTheAssetManager)
{
    constexpr auto kTimeout = std::chrono::seconds(10);

    WarmAssetFixture fx;
    ASSERT_TRUE(BuildWarmFixture(fx, 2, std::chrono::seconds(60))) << "fixture warm-up did not complete";

    const GUID hitGuid = fx.Guids[0];
    const GUID victimGuid = fx.Guids[1];

    std::atomic<bool> callbackRan{false};
    std::promise<void> finished;
    std::future<void> finishedFuture = finished.get_future();

    // The exercise runs on its own thread: pre-fix it wedges permanently, and
    // the test has to outlive it to report the failure.
    std::thread exerciser(
        [&]
        {
            AssetLoadHandle handle = fx.Assets->LoadAsset(
                hitGuid,
                [&](Result<SharedPtr<Asset>, AssetError>)
                {
                    // A load callback is ordinary user code and may call back
                    // into the manager -- the completion path already fires
                    // callbacks with no manager lock held. Pre-fix, this
                    // exclusive acquire blocks forever behind the shared lock
                    // the fast path still holds on this very thread.
                    fx.Assets->UnregisterLoadedAsset(victimGuid);
                    callbackRan.store(true, std::memory_order_release);
                },
                AssetLoadPriority::High);
            (void)handle;
            finished.set_value();
        });

    const bool completed = finishedFuture.wait_for(kTimeout) == std::future_status::ready;

    EXPECT_TRUE(completed)
        << "cache-hit LoadAsset deadlocked: the callback's exclusive acquire of "
           "m_LoadedAssetsMutex is blocked by the shared lock the same thread still "
           "holds in LoadAsset's already-loaded fast path (LoadAsset -> CreateLoadHandle).";

    if (completed)
    {
        exerciser.join();
        EXPECT_TRUE(callbackRan.load(std::memory_order_acquire)) << "cache-hit load never invoked its callback";
        fx.Teardown();
    }
    else
    {
        exerciser.detach();
        fx.Leak();
    }
}

// The field shape of the same defect, without the re-entrant callback: a decode
// thread queued for exclusive while the fast path holds shared. This one races
// for the window the editor hit, so it is a reproduction rather than the gate --
// CacheHitCallbackMayUseTheAssetManager above is the deterministic gate.
TEST(AssetLoadCacheHitReentrancy, CacheHitLoadsSurvivePublishingWriters)
{
    constexpr auto kTimeout = std::chrono::seconds(30);
    constexpr int kIterations = 5000;

    WarmAssetFixture fx;
    ASSERT_TRUE(BuildWarmFixture(fx, 1, std::chrono::seconds(60))) << "fixture warm-up did not complete";

    const GUID hitGuid = fx.Guids[0];
    SharedPtr<Asset> resident = fx.Assets->GetAsset(hitGuid);
    ASSERT_TRUE(resident) << "warm-up asset is not resident";

    std::atomic<bool> stopWriter{false};
    std::atomic<int> iterationsDone{0};
    std::promise<void> finished;
    std::future<void> finishedFuture = finished.get_future();

    // Stands in for the decode threads: a fresh GUID each time, so every
    // iteration is a real exclusive acquire on m_LoadedAssetsMutex. The yield
    // keeps this from simply starving the reader, which would time out for a
    // reason other than the deadlock under test.
    std::thread writer(
        [&]
        {
            while (!stopWriter.load(std::memory_order_acquire))
            {
                fx.Assets->RegisterLoadedAsset(GUID::Generate(), resident);
                std::this_thread::yield();
            }
        });

    // A non-null callback is required: CreateLoadHandle only re-reads the map
    // (GetLoadedAsset -- the nested shared acquire) when there is a callback to
    // resolve, which is why the field repro needs LoadAsset(guid, callback).
    std::thread exerciser(
        [&]
        {
            for (int i = 0; i < kIterations; ++i)
            {
                AssetLoadHandle handle =
                    fx.Assets->LoadAsset(hitGuid, [](Result<SharedPtr<Asset>, AssetError>) {}, AssetLoadPriority::High);
                (void)handle;
                iterationsDone.store(i + 1, std::memory_order_release);
            }
            finished.set_value();
        });

    const bool completed = finishedFuture.wait_for(kTimeout) == std::future_status::ready;
    stopWriter.store(true, std::memory_order_release);

    EXPECT_TRUE(completed)
        << "cache-hit LoadAsset wedged after " << iterationsDone.load(std::memory_order_acquire) << "/" << kIterations
        << " iterations: the fast path's nested shared acquire of m_LoadedAssetsMutex "
           "(CreateLoadHandle -> GetLoadedAsset) parked behind a writer queued in "
           "RegisterLoadedAsset.";

    if (completed)
    {
        exerciser.join();
        writer.join();
        fx.Teardown();
    }
    else
    {
        // Both threads are permanently blocked on m_LoadedAssetsMutex.
        exerciser.detach();
        writer.detach();
        fx.Leak();
    }
}
