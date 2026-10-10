// Pins how a failed compile reaches the HotReloadPipeline result: the compile
// stage's task handle fails with the compiler's message, the later stages never
// run, and the run reports CompilationFailed. Without that, the pipeline went
// on to load and swap the assembly already on disk and reported success. A
// compile the pool's shutdown cancels before it runs reports Cancelled, not
// CompilationFailed.
// Every compiler (compile server, dotnet CLI, ScriptManager) reports through
// the same compile stage, so the in-process mock compiler stands for all of
// them; the suite needs no dotnet, CompileServer or CoreCLR.

#include <gtest/gtest.h>

#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/HotReloadTasks.h"
#include "Jobs/MockHotReloadDependencies.h" // Tests/Jobs
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>

using namespace GameEngine;
using namespace std::chrono_literals;

namespace
{

// Covers the later stages on a build that lets them run after a failed
// compile: assembly preparation retries for 500 ms and the swap waits up to
// 5 s for a main thread.
constexpr auto kRunDeadline = 30s;

// How long the test waits for the run's compile to reach the channel's FIFO.
constexpr auto kQueueDeadline = 10s;

// The error MockCompiler(false) reports.
constexpr const char* kMockCompilerError = "Mock error: Syntax error on line 42";

} // namespace

TEST(HotReloadPipelineCompileFailure, FailedCompileFailsThePipelineWithTheCompilerMessage)
{
    const TestUtils::ScopedTempDir workspace(TestUtils::MakeUniqueTempDirectory("GE_HotReloadPipelineCompileFailure"));
    // An assembly from an earlier compile: the later stages would load and
    // swap it if the failed compile let them run.
    const std::filesystem::path assemblyPath = workspace.Path() / "GameEngine.Scripts.dll";
    {
        std::ofstream assembly(assemblyPath, std::ios::binary);
        assembly << "MZ";
    }

    JobSystem::WorkStealingThreadPool pool(2);
    JobSystem::JobChannel compiles(pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(pool, compiles);
    pipeline.SetCompilerFactory([] { return std::make_unique<Tests::MockCompiler>(false, 1); });
    pipeline.SetAssemblyLoaderFactory([] { return std::make_unique<Tests::MockAssemblyLoader>(); });
    pipeline.SetSwapperFactory([] { return std::make_unique<Tests::MockAssemblySwapper>(true, 1); });

    auto run = pipeline.ExecuteAsync(assemblyPath.string());
    ASSERT_EQ(run.wait_for(kRunDeadline), std::future_status::ready) << "the run never finished";
    const HotReloadPipeline::PipelineStats stats = run.get();

    EXPECT_FALSE(stats.success) << "a failed compile must fail the pipeline";
    EXPECT_EQ(stats.result, HotReloadResult::CompilationFailed) << stats.errorMessage;
    EXPECT_EQ(stats.errorMessage, kMockCompilerError) << "the pipeline must report the compiler's own message";
}

TEST(HotReloadPipelineCompileFailure, CompileCancelledByThePoolShutdownReportsCancelled)
{
    const TestUtils::ScopedTempDir workspace(TestUtils::MakeUniqueTempDirectory("GE_HotReloadPipelineShutdownCancel"));
    const std::filesystem::path assemblyPath = workspace.Path() / "GameEngine.Scripts.dll";

    JobSystem::WorkStealingThreadPool pool(2);
    JobSystem::JobChannel compiles(pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(pool, compiles);
    pipeline.SetCompilerFactory([] { return std::make_unique<Tests::MockCompiler>(true, 1); });
    pipeline.SetAssemblyLoaderFactory([] { return std::make_unique<Tests::MockAssemblyLoader>(); });
    pipeline.SetSwapperFactory([] { return std::make_unique<Tests::MockAssemblySwapper>(true, 1); });

    // Another compile holds the channel's only slot, so the run's compile
    // waits in the FIFO, where the shutdown drain cancels it without the
    // run's token.
    std::promise<void> blockerStarted;
    std::promise<void> releaseBlocker;
    std::shared_future<void> released = releaseBlocker.get_future().share();
    JobSystem::TaskHandle blocker = compiles.Submit([&blockerStarted, released] {
        blockerStarted.set_value();
        released.wait();
    });
    ASSERT_TRUE(blocker.IsValid());
    blockerStarted.get_future().wait();
    const size_t registeredBeforeRun = pool.GetTaskDataRegistrySizeForTests();

    auto run = pipeline.ExecuteAsync(assemblyPath.string());
    const auto queueDeadline = std::chrono::steady_clock::now() + kQueueDeadline;
    while (pool.GetTaskDataRegistrySizeForTests() == registeredBeforeRun && std::chrono::steady_clock::now() < queueDeadline)
    {
        std::this_thread::yield();
    }
    ASSERT_GT(pool.GetTaskDataRegistrySizeForTests(), registeredBeforeRun) << "the run's compile never reached the channel";

    // Shutdown joins the blocking threads after its drain, so it returns once
    // the blocker is released.
    std::thread shutdown([&pool] { pool.Shutdown(); });
    const std::future_status runStatus = run.wait_for(kRunDeadline);
    releaseBlocker.set_value();
    shutdown.join();

    ASSERT_EQ(runStatus, std::future_status::ready) << "the run never finished";
    const HotReloadPipeline::PipelineStats stats = run.get();
    EXPECT_FALSE(stats.success);
    EXPECT_EQ(stats.result, HotReloadResult::Cancelled) << stats.errorMessage;
}
