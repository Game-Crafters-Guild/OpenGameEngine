// Pins the job-system route ScriptManager takes for hot reload when async hot
// reload is disabled (SubmitSyncHotReload):
//  - A C# change event returns while its reload is still compiling. The
//    FileWatchingService calls every subscriber of a directory from one watcher
//    thread, asset hot reload included, so a compile there stalls them all.
//  - The first reload runs although nothing ran before it.
//  - Shutdown joins a reload that is still compiling.
//  - A reload requested after Shutdown has started is refused, not queued
//    behind Shutdown's join, and in async mode not started on the pipeline
//    Shutdown is about to destroy.
// The same watcher route serves package C# module roots: an edit under an
// Embedded package's root starts a reload, a Git cache entry's root is not
// watched, a watched root costs one subscription and no events at rest, and
// the package subscriptions follow hot reload being disabled and Shutdown.
// The compile is held open at the CompileServer transport seam. The transport
// then refuses to connect, so the compile fails fast and never starts a server.
// Every compile is a job of the manager's "Script compiles" channel (cap 1): a
// reload parked in its compile holds no compute worker, and two compiles of the
// project never meet at the compile server's pipe.

#include <gtest/gtest.h>

#include "Assets/FileWatchingService.h"
#include "Assets/Packages/PackageCodeModules.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/CompileServerTestDoubles.h" // Tests/Jobs
#include "Jobs/CompileServerVersion.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/IHotReloadTransport.h"
#include "Jobs/ScopedHotReloadTestHooks.h" // Tests/Jobs
#include "Scripting/PathResolver.h"
#include "Scripting/ScriptManager.h"
#include "WatchedFileEvents.h"             // Tests/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace GameEngine;
using namespace std::chrono_literals;

namespace
{
constexpr size_t kPoolWorkerCount = 4;
constexpr auto kCompileStartTimeout = 30s;
constexpr auto kEventDispatchTimeout = 10s;
constexpr auto kReloadFinishTimeout = 30s;
// Long enough for an unjoined Shutdown to return; a joined one stays blocked
// until the gate opens, so the probe cannot pass by luck.
constexpr auto kShutdownBlockedProbe = 300ms;
constexpr auto kPollInterval = 10ms;
// A watched root at rest: time for in-flight arming events to land, then a
// window that must see none.
constexpr auto kRestSettle = 500ms;
constexpr auto kRestProbe = 1000ms;
// ScriptManager subscribes at the default priority 0; the service calls higher
// priorities first, so this subscriber runs after OnFileChanged returns.
constexpr int kAfterScriptManagerPriority = -1;

// Holds the first compile open inside the transport factory until Release.
class CompileGate
{
  public:
    void Enter()
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        if (m_Entered)
            return;
        m_Entered = true;
        m_Changed.notify_all();
        m_Changed.wait(lock, [this] { return m_Released; });
    }

    bool WaitUntilEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Changed.wait_for(lock, timeout, [this] { return m_Entered; });
    }

    void Release()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Released = true;
        m_Changed.notify_all();
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    bool m_Entered = false;
    bool m_Released = false;
};

// Opens the gate on every exit path, so a failed assertion cannot leave
// Shutdown or StopWatching waiting on a compile that never ends. Declare it
// after everything that waits on the compile, so it is destroyed first.
struct CompileGateReleaser
{
    std::shared_ptr<CompileGate> Gate;
    ~CompileGateReleaser() { Gate->Release(); }
};

struct ServiceWatchingScope
{
    ServiceWatchingScope() { FileWatchingService::GetInstance().StartWatching(); }
    ~ServiceWatchingScope() { FileWatchingService::GetInstance().StopWatching(); }
};

void InstallGatedTransport(const std::shared_ptr<CompileGate>& gate)
{
    SetCompileServerTransportFactoryForTests([gate](const std::string&) -> std::unique_ptr<IHotReloadTransport> {
        gate->Enter();
        return std::make_unique<Tests::RefusingTransport>();
    });
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::filesystem::path PrepareWorkspace(const std::string& name)
{
    const std::filesystem::path workspace = std::filesystem::temp_directory_path() / ("GE_SM_SyncHotReload_" + name);
    std::error_code ec;
    std::filesystem::remove_all(workspace, ec);
    std::filesystem::create_directories(workspace / "Scripts", ec);
    // An explicit project, so the compile reaches the transport even when the
    // editor scripts project cannot be generated.
    WriteText(workspace / "Scripts" / "GameEngine.Scripts.csproj",
              "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>");
    return workspace;
}

ScriptsConfig MakeSyncHotReloadConfig(const std::filesystem::path& workspace, bool enableHotReload)
{
    ScriptsConfig config{};
    config.workspaceRoot = workspace;
    config.scriptsRoot = workspace / "Scripts";
    config.assembliesRoot = workspace / "ScriptAssemblies";
    config.generatedProjectRoot = workspace / "Generated";
    config.disableClr = true;
    config.enableHotReload = enableHotReload;
    config.enableAsyncHotReload = false;
    config.enableAutoProjectGeneration = false;
    return config;
}

// Shutdown drops the watch subscriptions right after it sets its shutting-down
// flag. The count is read under the service mutex that the unsubscribe takes,
// so a request made after this returns true is ordered after the flag.
bool WaitUntilShutdownReleasedSubscriptions(uint32 subscriptionsBeforeShutdown)
{
    FileWatchingService& service = FileWatchingService::GetInstance();
    const auto deadline = std::chrono::steady_clock::now() + kEventDispatchTimeout;
    while (service.GetStats().TotalSubscriptions >= subscriptionsBeforeShutdown)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

bool WaitForFlag(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}
} // namespace

TEST(ScriptManagerSyncHotReload, ChangeEventReturnsWhileTheReloadCompiles)
{
    const std::filesystem::path workspace = PrepareWorkspace("ChangeEvent");
    const std::filesystem::path scriptsRoot = workspace / "Scripts";
    const std::filesystem::path changedScript = scriptsRoot / "Changed.cs";

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/true), pool));
    ASSERT_FALSE(manager.IsAsyncHotReloadEnabled());

    auto laterSubscriberSawChange = std::make_shared<std::atomic<bool>>(false);
    FileWatchSubscription laterSubscriber = FileWatchingService::GetInstance().Subscribe(
        FilePattern(scriptsRoot, ".*", {".cs"}, /*recursive=*/true),
        [laterSubscriberSawChange, changedScript](const FileChangeEvent& event) {
            if (event.Path == changedScript)
                laterSubscriberSawChange->store(true, std::memory_order_release);
        },
        kAfterScriptManagerPriority);
    ServiceWatchingScope watching;
    CompileGateReleaser releaser{gate};
    ASSERT_TRUE(TestUtils::WaitUntilWatchingIsArmed(scriptsRoot)) << "The watcher never delivered an event";

    WriteText(changedScript, "public static class Changed { }\n");

    ASSERT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout))
        << "The change never reached a compile (GE_DISABLE_COMPILE_SERVER=1 bypasses the transport seam)";
    EXPECT_TRUE(WaitForFlag(*laterSubscriberSawChange, kEventDispatchTimeout))
        << "The watcher thread is still inside the compile: OnFileChanged compiled inline instead of on the job system";

    gate->Release();
    // Let that dispatch finish before its subscriptions go away. Teardown then
    // runs in the editor's order: StopWatching joins the watcher threads before
    // the manager's destructor shuts it down.
    EXPECT_TRUE(WaitForFlag(*laterSubscriberSawChange, kReloadFinishTimeout));
}

TEST(ScriptManagerPackageModuleWatch, EditInAnEmbeddedModuleRootStartsAReload)
{
    const std::filesystem::path workspace = PrepareWorkspace("PackageModuleEdit");
    const std::filesystem::path moduleRoot = workspace / "Packages" / "watched-pack" / "Runtime";
    std::filesystem::create_directories(moduleRoot);
    WriteText(moduleRoot / "Existing.cs", "public static class Existing { }\n");

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/true), pool));

    FileWatchingService& service = FileWatchingService::GetInstance();
    const uint32 subscriptionsBefore = service.GetStats().TotalSubscriptions;
    PackageCodeModule module;
    module.PackageName = "watched-pack";
    module.AssemblyName = "WatchedPack";
    module.RootDir = moduleRoot;
    module.PackageRootDir = moduleRoot.parent_path();
    manager.SetPackageCodeModules({module}, {});
    EXPECT_EQ(service.GetStats().TotalSubscriptions, subscriptionsBefore + 1)
        << "An embedded package's C# module root takes one subscription";

    ServiceWatchingScope watching;
    CompileGateReleaser releaser{gate};
    ASSERT_TRUE(TestUtils::WaitUntilWatchingIsArmed(moduleRoot)) << "The watcher never delivered an event";

    // At rest the root costs no events: the arming sentinel's removal settles,
    // then nothing arrives until the edit.
    std::this_thread::sleep_for(kRestSettle);
    const uint32 eventsAtRest = service.GetStats().EventsProcessed;
    std::this_thread::sleep_for(kRestProbe);
    EXPECT_EQ(service.GetStats().EventsProcessed, eventsAtRest) << "The watched root produced events with no edit";
    EXPECT_FALSE(gate->WaitUntilEntered(0ms)) << "A reload started with no edit";

    WriteText(moduleRoot / "Existing.cs", "public static class Existing { public static int Value = 1; }\n");

    EXPECT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout))
        << "A saved .cs under an embedded package's module root started no reload";
}

TEST(ScriptManagerPackageModuleWatch, GitPackageModuleRootIsNotWatched)
{
    const std::filesystem::path workspace = PrepareWorkspace("PackageModuleGit");
    const std::filesystem::path moduleRoot = workspace / "GitCache" / "cached-pack@0123abc" / "Runtime";
    std::filesystem::create_directories(moduleRoot);
    WriteText(moduleRoot / "Cached.cs", "public static class Cached { }\n");

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/true), pool));

    FileWatchingService& service = FileWatchingService::GetInstance();
    const uint32 subscriptionsBefore = service.GetStats().TotalSubscriptions;
    PackageCodeModule module;
    module.PackageName = "cached-pack";
    module.AssemblyName = "CachedPack";
    module.RootDir = moduleRoot;
    module.PackageRootDir = moduleRoot.parent_path();
    module.SourceKind = PackageSourceKind::Git;
    manager.SetPackageCodeModules({module}, {});
    EXPECT_EQ(service.GetStats().TotalSubscriptions, subscriptionsBefore)
        << "A git cache entry never changes in place, so its module root takes no subscription";

    // Called after any ScriptManager subscription on the same event, so once it
    // has seen the edit, a subscribed manager would already have started its reload.
    const std::filesystem::path cachedScript = moduleRoot / "Cached.cs";
    auto edited = std::make_shared<std::atomic<bool>>(false);
    FileWatchSubscription laterSubscriber = service.Subscribe(
        FilePattern(moduleRoot, ".*", {".cs"}, /*recursive=*/true),
        [edited, cachedScript](const FileChangeEvent& event) {
            if (event.Path == cachedScript)
                edited->store(true, std::memory_order_release);
        },
        kAfterScriptManagerPriority);

    ServiceWatchingScope watching;
    CompileGateReleaser releaser{gate};
    ASSERT_TRUE(TestUtils::WaitUntilWatchingIsArmed(moduleRoot)) << "The watcher never delivered an event";
    WriteText(cachedScript, "public static class Cached { public static int Value = 1; }\n");

    ASSERT_TRUE(WaitForFlag(*edited, kEventDispatchTimeout)) << "The edit was never delivered, so it proves nothing";
    EXPECT_FALSE(gate->WaitUntilEntered(kRestProbe)) << "An edit in a git package's module root started a reload";
}

TEST(ScriptManagerPackageModuleWatch, DisableAndShutdownDropPackageSubscriptionsForGood)
{
    const std::filesystem::path workspace = PrepareWorkspace("PackageModuleLifecycle");
    const std::filesystem::path moduleRoot = workspace / "Packages" / "lifecycle-pack";
    std::filesystem::create_directories(moduleRoot);
    WriteText(moduleRoot / "Existing.cs", "public static class Existing { }\n");

    Tests::ScopedHotReloadTestHooks hookGuard;
    SetCompileServerTransportFactoryForTests(
        [](const std::string&) -> std::unique_ptr<IHotReloadTransport> { return std::make_unique<Tests::RefusingTransport>(); });
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    PackageCodeModule module;
    module.PackageName = "lifecycle-pack";
    module.AssemblyName = "LifecyclePack";
    module.RootDir = moduleRoot;
    module.PackageRootDir = moduleRoot;

    FileWatchingService& service = FileWatchingService::GetInstance();
    const uint32 baseline = service.GetStats().TotalSubscriptions;
    ScriptManager manager;
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/true), pool));
    const uint32 afterInitialize = service.GetStats().TotalSubscriptions;
    manager.SetPackageCodeModules({module}, {});
    EXPECT_EQ(service.GetStats().TotalSubscriptions, afterInitialize + 1);

    manager.SetHotReloadEnabled(false);
    EXPECT_EQ(service.GetStats().TotalSubscriptions, baseline)
        << "Disabling hot reload drops the project, editor and package subscriptions";
    manager.SetPackageCodeModules({module}, {});
    EXPECT_EQ(service.GetStats().TotalSubscriptions, baseline)
        << "A module set handed over while hot reload is disabled subscribes nothing";
    manager.SetHotReloadEnabled(true);
    EXPECT_EQ(service.GetStats().TotalSubscriptions, afterInitialize + 1)
        << "Enabling hot reload again restores the package subscription";

    manager.Shutdown();
    EXPECT_EQ(service.GetStats().TotalSubscriptions, baseline) << "Shutdown drops every subscription";
    manager.SetPackageCodeModules({module}, {});
    EXPECT_EQ(service.GetStats().TotalSubscriptions, baseline)
        << "A module set handed over after Shutdown must not subscribe again";
}

TEST(ScriptManagerSyncHotReload, FirstReloadRunsAndShutdownJoinsIt)
{
    const std::filesystem::path workspace = PrepareWorkspace("ShutdownJoin");

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    CompileGateReleaser releaser{gate};
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/false), pool));

    std::future<HotReloadPipeline::PipelineStats> reload =
        manager.RecompileAndReloadAsync((workspace / "ScriptAssemblies" / ScriptingPaths::kScriptsAssemblyFileName).string());
    ASSERT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout)) << "The first sync reload never ran";

    // This test holds the reload's future, so only Shutdown's task-handle wait can join it.
    std::future<void> shutdown = std::async(std::launch::async, [&manager] { manager.Shutdown(); });
    EXPECT_EQ(shutdown.wait_for(kShutdownBlockedProbe), std::future_status::timeout)
        << "Shutdown returned while a queued reload was still compiling";

    gate->Release();
    ASSERT_EQ(shutdown.wait_for(kReloadFinishTimeout), std::future_status::ready);
    EXPECT_EQ(reload.wait_for(0s), std::future_status::ready) << "Shutdown returned before the reload finished";

    // Keep the manager alive until the reload ends, even when Shutdown did not join it.
    ASSERT_EQ(reload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    EXPECT_FALSE(reload.get().success) << "The refusing transport fails every compile";
}

TEST(ScriptManagerSyncHotReload, ReloadRequestedDuringShutdownIsRefused)
{
    const std::filesystem::path workspace = PrepareWorkspace("DuringShutdown");
    const std::string assemblyPath = (workspace / "ScriptAssemblies" / ScriptingPaths::kScriptsAssemblyFileName).string();

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    CompileGateReleaser releaser{gate};
    // Hot reload on, so Shutdown has watch subscriptions to drop.
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/true), pool));

    std::future<HotReloadPipeline::PipelineStats> heldReload = manager.RecompileAndReloadAsync(assemblyPath);
    ASSERT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout)) << "The first sync reload never ran";

    const uint32 subscriptionsBeforeShutdown = FileWatchingService::GetInstance().GetStats().TotalSubscriptions;
    std::future<void> shutdown = std::async(std::launch::async, [&manager] { manager.Shutdown(); });

    // Shutdown is still waiting on the held reload.
    const bool shutdownStarted = WaitUntilShutdownReleasedSubscriptions(subscriptionsBeforeShutdown);
    if (!shutdownStarted)
        gate->Release(); // The shutdown future's destructor waits on Shutdown, which waits on the held reload.
    ASSERT_TRUE(shutdownStarted) << "Shutdown never released its watch subscriptions";

    std::future<HotReloadPipeline::PipelineStats> lateReload = manager.RecompileAndReloadAsync(assemblyPath);

    gate->Release();
    ASSERT_EQ(shutdown.wait_for(kReloadFinishTimeout), std::future_status::ready);
    // Keep the manager alive until both reloads end, even when the late one was queued.
    ASSERT_EQ(heldReload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    ASSERT_EQ(lateReload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    EXPECT_EQ(lateReload.get().result, HotReloadResult::Cancelled)
        << "A reload requested after Shutdown began was queued instead of refused";
}

TEST(ScriptManagerAsyncHotReload, ReloadRequestedDuringShutdownIsRefused)
{
    const std::filesystem::path workspace = PrepareWorkspace("AsyncDuringShutdown");
    const std::string assemblyPath = (workspace / "ScriptAssemblies" / ScriptingPaths::kScriptsAssemblyFileName).string();

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    CompileGateReleaser releaser{gate};
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/true), pool));

    // An async pipeline run cannot hold Shutdown: Shutdown cancels it and does
    // not wait for its stages. A held sync reload keeps Shutdown between setting
    // its flag and destroying the pipeline while async mode is on.
    std::future<HotReloadPipeline::PipelineStats> heldReload = manager.RecompileAndReloadAsync(assemblyPath);
    ASSERT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout)) << "The first sync reload never ran";
    manager.SetAsyncHotReloadEnabled(true);

    const uint32 subscriptionsBeforeShutdown = FileWatchingService::GetInstance().GetStats().TotalSubscriptions;
    std::future<void> shutdown = std::async(std::launch::async, [&manager] { manager.Shutdown(); });
    const bool shutdownStarted = WaitUntilShutdownReleasedSubscriptions(subscriptionsBeforeShutdown);
    if (!shutdownStarted)
        gate->Release(); // The shutdown future's destructor waits on Shutdown, which waits on the held reload.
    ASSERT_TRUE(shutdownStarted) << "Shutdown never released its watch subscriptions";

    // Shutdown is still waiting on the held reload, so a pipeline run started
    // here ends before Shutdown destroys the pipeline.
    std::future<HotReloadPipeline::PipelineStats> lateReload = manager.RecompileAndReloadAsync(assemblyPath);
    const bool lateReloadEnded = lateReload.wait_for(kReloadFinishTimeout) == std::future_status::ready;

    gate->Release();
    ASSERT_EQ(shutdown.wait_for(kReloadFinishTimeout), std::future_status::ready);
    ASSERT_EQ(heldReload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    ASSERT_TRUE(lateReloadEnded);
    EXPECT_EQ(lateReload.get().result, HotReloadResult::Cancelled)
        << "An async reload requested after Shutdown began was started instead of refused";
}

namespace
{
// Longer than the client's whole connect budget (three attempts, 0 + 150 + 300
// ms of backoff), so a second compile that reaches the pipe while the first is
// held can only succeed by waiting for it outside the pipe.
constexpr auto kFirstCompileHold = 2s;
constexpr auto kComputeJobDeadline = 5s;

// A compile server that serves one connection at a time, as the host does
// (one pipe instance, or one accepted socket, at a time). A connect while a
// connection is open is refused, the way a busy Windows pipe instance refuses
// it; on Unix the real host would leave that client in its listen backlog. The
// first compile request is held for kFirstCompileHold.
struct OneConnectionHost
{
    std::mutex Mutex;
    int Open = 0;
    int MostOpenAtOnce = 0;
    int RefusedConnects = 0;
    int CompileRequests = 0;
    std::atomic<bool> FirstCompileEntered{false};
};

class OneConnectionTransport : public IHotReloadTransport
{
  public:
    explicit OneConnectionTransport(std::shared_ptr<OneConnectionHost> host) : m_Host(std::move(host)) {}
    ~OneConnectionTransport() override { Close(); }

    bool Connect() override
    {
        std::lock_guard<std::mutex> lock(m_Host->Mutex);
        if (m_Connected)
            return true;
        if (m_Host->Open > 0)
        {
            ++m_Host->RefusedConnects;
            return false;
        }
        ++m_Host->Open;
        m_Host->MostOpenAtOnce = std::max(m_Host->MostOpenAtOnce, m_Host->Open);
        m_Connected = true;
        return true;
    }

    bool SendRequest(const std::string& requestJson, std::string& outResponse) override
    {
        if (!m_Connected)
            return false;
        if (requestJson == "__version__")
        {
            outResponse = std::string("{\"Version\":\"") + kExpectedCompileServerVersion + "\"}";
            return true;
        }
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(m_Host->Mutex);
            first = m_Host->CompileRequests++ == 0;
        }
        if (first)
        {
            m_Host->FirstCompileEntered.store(true);
            std::this_thread::sleep_for(kFirstCompileHold);
        }
        // "MZ", base64.
        outResponse = "{\"Success\":true,\"AssemblyBytes\":\"TVo=\"}";
        return true;
    }

    void Close() override
    {
        std::lock_guard<std::mutex> lock(m_Host->Mutex);
        if (!m_Connected)
            return;
        m_Connected = false;
        --m_Host->Open;
    }

  private:
    std::shared_ptr<OneConnectionHost> m_Host;
    bool m_Connected = false;
};

bool ComputeJobCompletes(JobSystem::WorkStealingThreadPool& pool)
{
    JobSystem::TaskHandle job = pool.Submit([] {});
    const auto deadline = std::chrono::steady_clock::now() + kComputeJobDeadline;
    while (!job.IsDone() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(kPollInterval);
    return job.IsCompleted();
}
} // namespace

// A pool with one compute worker: the reload parked in its compile runs on the
// "Script compiles" channel's blocking thread, so a compute job still runs.
TEST(ScriptCompilesChannel, SyncReloadParkedInItsCompileHoldsNoComputeWorker)
{
    const std::filesystem::path workspace = PrepareWorkspace("ParkedReloadHoldsNoWorker");

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(1);
    ScriptManager manager;
    CompileGateReleaser releaser{gate};
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/false), pool));

    std::future<HotReloadPipeline::PipelineStats> reload =
        manager.RecompileAndReloadAsync((workspace / "ScriptAssemblies" / ScriptingPaths::kScriptsAssemblyFileName).string());
    ASSERT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout)) << "The sync reload never reached its compile";
    EXPECT_TRUE(ComputeJobCompletes(pool)) << "The reload parked in its compile holds the only compute worker";

    gate->Release();
    ASSERT_EQ(reload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    manager.Shutdown();
}

// A compile from outside the manager (the game export's csproj refresh and Debug
// build) run through RunCompile waits in the "Script compiles" FIFO behind a live
// compile, so it never reads the csproj or the package assemblies while that
// compile rewrites them, and it holds no compute worker while it waits. After
// Shutdown, with no live manager and nothing else compiling, it runs on its caller.
TEST(ScriptCompilesChannel, ACompileRunFromOutsideWaitsBehindALiveCompile)
{
    const std::filesystem::path workspace = PrepareWorkspace("OutsideCompileWaits");

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(1);
    ScriptManager manager;
    CompileGateReleaser releaser{gate};
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/false), pool));

    std::future<HotReloadPipeline::PipelineStats> reload =
        manager.RecompileAndReloadAsync((workspace / "ScriptAssemblies" / ScriptingPaths::kScriptsAssemblyFileName).string());
    ASSERT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout)) << "The sync reload never reached its compile";

    std::atomic<bool> outsideRan{false};
    std::future<bool> outside = std::async(std::launch::async, [&manager, &outsideRan] {
        return manager.RunCompile([&outsideRan] { outsideRan.store(true); });
    });
    EXPECT_EQ(outside.wait_for(kShutdownBlockedProbe), std::future_status::timeout)
        << "RunCompile returned while a live compile held the channel";
    EXPECT_FALSE(outsideRan.load()) << "The outside compile ran beside the live compile";
    EXPECT_TRUE(ComputeJobCompletes(pool)) << "The queued outside compile holds the only compute worker";

    gate->Release();
    ASSERT_EQ(reload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    ASSERT_EQ(outside.wait_for(kReloadFinishTimeout), std::future_status::ready);
    EXPECT_TRUE(outside.get());
    EXPECT_TRUE(outsideRan.load());

    manager.Shutdown();
    std::atomic<bool> afterShutdownRan{false};
    const std::thread::id caller = std::this_thread::get_id();
    std::thread::id ranOn;
    EXPECT_TRUE(manager.RunCompile([&] {
        ranOn = std::this_thread::get_id();
        afterShutdownRan.store(true);
    }));
    EXPECT_TRUE(afterShutdownRan.load()) << "With no live manager the compile did not run";
    EXPECT_EQ(ranOn, caller) << "With no live manager the compile did not run on its caller";
}

// The export's compile job calls back into the manager (RefreshGeneratedProject,
// GeneratedProjectPath), so Shutdown must not tear the manager down under one that
// is running: it waits for every compile RunCompile queued before it began.
TEST(ScriptCompilesChannel, ShutdownWaitsForACompileRunFromOutside)
{
    const std::filesystem::path workspace = PrepareWorkspace("ShutdownWaitsOutsideCompile");

    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    auto outsideGate = std::make_shared<CompileGate>();
    CompileGateReleaser releaser{outsideGate};
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/false), pool));

    std::future<bool> outside = std::async(std::launch::async, [&manager, outsideGate] {
        return manager.RunCompile([outsideGate] { outsideGate->Enter(); });
    });
    ASSERT_TRUE(outsideGate->WaitUntilEntered(kCompileStartTimeout)) << "The outside compile never ran";

    std::future<void> shutdown = std::async(std::launch::async, [&manager] { manager.Shutdown(); });
    EXPECT_EQ(shutdown.wait_for(kShutdownBlockedProbe), std::future_status::timeout)
        << "Shutdown returned while a compile queued through RunCompile was still running";

    outsideGate->Release();
    ASSERT_EQ(shutdown.wait_for(kReloadFinishTimeout), std::future_status::ready);
    ASSERT_EQ(outside.wait_for(kReloadFinishTimeout), std::future_status::ready);
    EXPECT_TRUE(outside.get());
}

// Once Shutdown has started, a compile from outside is refused, never run on its
// caller: Shutdown is still waiting on a live compile, and running the outside one
// beside it is the overlap the channel removes.
TEST(ScriptCompilesChannel, ACompileRunFromOutsideDuringShutdownIsRefused)
{
    const std::filesystem::path workspace = PrepareWorkspace("OutsideCompileDuringShutdown");

    auto gate = std::make_shared<CompileGate>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    InstallGatedTransport(gate);
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    CompileGateReleaser releaser{gate};
    // Hot reload on, so Shutdown has watch subscriptions to drop.
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/true), pool));

    std::future<HotReloadPipeline::PipelineStats> heldReload =
        manager.RecompileAndReloadAsync((workspace / "ScriptAssemblies" / ScriptingPaths::kScriptsAssemblyFileName).string());
    ASSERT_TRUE(gate->WaitUntilEntered(kCompileStartTimeout)) << "The sync reload never reached its compile";

    const uint32 subscriptionsBeforeShutdown = FileWatchingService::GetInstance().GetStats().TotalSubscriptions;
    std::future<void> shutdown = std::async(std::launch::async, [&manager] { manager.Shutdown(); });

    // Shutdown is still waiting on the held reload.
    const bool shutdownStarted = WaitUntilShutdownReleasedSubscriptions(subscriptionsBeforeShutdown);
    if (!shutdownStarted)
        gate->Release(); // The shutdown future's destructor waits on Shutdown, which waits on the held reload.
    ASSERT_TRUE(shutdownStarted) << "Shutdown never released its watch subscriptions";

    std::atomic<bool> lateRan{false};
    const bool lateResult = manager.RunCompile([&lateRan] { lateRan.store(true); });

    gate->Release();
    ASSERT_EQ(shutdown.wait_for(kReloadFinishTimeout), std::future_status::ready);
    ASSERT_EQ(heldReload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    EXPECT_FALSE(lateResult) << "RunCompile reported a compile it should have refused";
    EXPECT_FALSE(lateRan.load()) << "A compile requested after Shutdown began ran beside the live compile";
}

// A sync reload's compile is held at the host; a pipeline compile of the same
// project is requested meanwhile. Through the channel the second compile waits
// for the first and both are served on one connection at a time. Without it the
// second client reaches the pipe while the first connection is open, is
// refused for the whole of its connect budget and fails ("CompileServer attempt
// failed"); on Windows a client past that budget can launch a second host. The
// injected transport never launches a host, so the test asserts the clients'
// outcome at the pipe. (The design names the deferred initial build as the
// first compile; that build runs only with a live CLR, ScriptManager::Initialize,
// and is a Submit to the same channel as the sync reload.)
TEST(ScriptCompilesChannel, TwoCompilesOfOneProjectNeverMeetAtThePipe)
{
    const std::filesystem::path workspace = PrepareWorkspace("TwoCompilesOneProject");
    const std::string assemblyPath = (workspace / "ScriptAssemblies" / ScriptingPaths::kScriptsAssemblyFileName).string();

    auto host = std::make_shared<OneConnectionHost>();
    Tests::ScopedHotReloadTestHooks hookGuard;
    SetCompileServerTransportFactoryForTests([host](const std::string&) -> std::unique_ptr<IHotReloadTransport> {
        return std::make_unique<OneConnectionTransport>(host);
    });
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount);
    ScriptManager manager;
    ASSERT_TRUE(manager.Initialize(MakeSyncHotReloadConfig(workspace, /*enableHotReload=*/false), pool));

    std::future<HotReloadPipeline::PipelineStats> syncReload = manager.RecompileAndReloadAsync(assemblyPath);
    ASSERT_TRUE(WaitForFlag(host->FirstCompileEntered, kCompileStartTimeout)) << "The sync reload never reached the host";

    manager.SetAsyncHotReloadEnabled(true);
    std::future<HotReloadPipeline::PipelineStats> pipelineReload = manager.RecompileAndReloadAsync(assemblyPath);

    ASSERT_EQ(syncReload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    ASSERT_EQ(pipelineReload.wait_for(kReloadFinishTimeout), std::future_status::ready);
    const HotReloadPipeline::PipelineStats pipelineStats = pipelineReload.get();
    manager.Shutdown();

    std::lock_guard<std::mutex> lock(host->Mutex);
    EXPECT_EQ(host->RefusedConnects, 0) << "A second compile reached the pipe while the first connection was open";
    EXPECT_EQ(host->MostOpenAtOnce, 1);
    EXPECT_GE(host->CompileRequests, 2) << "Both compiles are served";
    EXPECT_NE(pipelineStats.result, HotReloadResult::CompilationFailed) << pipelineStats.errorMessage;
}
