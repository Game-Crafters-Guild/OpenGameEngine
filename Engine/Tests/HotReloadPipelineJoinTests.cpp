// Pins HotReloadPipeline's stage lifetime: Cancel and supersede resolve a run's
// future while its compile stage is still running (a Running stage is never
// interrupted), and destroying the pipeline waits for that compile. The
// ScriptManager's compiler points at the ScriptManager, so a compile that
// outlived the pipeline could call CompileScripts on a destroyed owner.
// The compile is held open by a gated fake compiler, so the suite needs no
// dotnet, CompileServer or CoreCLR.

#include <gtest/gtest.h>

#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/HotReloadTasks.h"
#include "Jobs/MockHotReloadDependencies.h" // Tests/Jobs

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

// Long enough that a destructor which does not wait finishes well inside it.
constexpr auto kBlockedWindow = 300ms;
constexpr auto kDeadline = 10s;

struct CompileGate
{
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Released = false;
    std::atomic<bool> Started{false};
    std::atomic<bool> Finished{false};

    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Released = true;
        }
        Cv.notify_all();
    }
};

// Releases the gate on every exit path, so a failed assertion cannot leave
// the pipeline's destructor waiting on a compile nobody will release.
struct GateReleaser
{
    std::shared_ptr<CompileGate> Gate;
    ~GateReleaser() { Gate->Release(); }
};

// The first compile blocks until the gate opens; later compiles return at once.
class GatedCompiler : public Jobs::ICompiler
{
  public:
    GatedCompiler(std::shared_ptr<CompileGate> gate, bool blocks) : m_Gate(std::move(gate)), m_Blocks(blocks) {}

    CompilationResult compile(const String& /*assemblyPath*/, const std::atomic<bool>* /*cancelRequested*/) override
    {
        if (m_Blocks)
        {
            m_Gate->Started.store(true);
            std::unique_lock<std::mutex> lock(m_Gate->Mutex);
            m_Gate->Cv.wait(lock, [this] { return m_Gate->Released; });
            m_Gate->Finished.store(true);
        }
        return {true, "gated", {}, {}};
    }

  private:
    std::shared_ptr<CompileGate> m_Gate;
    bool m_Blocks;
};

constexpr JobSystem::JobChannelDesc kCompilesDesc{.Name = "Script compiles", .MaxRunning = 1};

std::unique_ptr<HotReloadPipeline> MakeGatedPipeline(JobSystem::WorkStealingThreadPool& pool,
                                                     JobSystem::JobChannel& compiles,
                                                     const std::shared_ptr<CompileGate>& gate)
{
    auto pipeline = std::make_unique<HotReloadPipeline>(pool, compiles);
    auto compileCount = std::make_shared<std::atomic<int>>(0);
    pipeline->SetCompilerFactory([gate, compileCount]
                                 { return std::make_unique<GatedCompiler>(gate, compileCount->fetch_add(1) == 0); });
    pipeline->SetAssemblyLoaderFactory([] { return std::make_unique<Tests::MockAssemblyLoader>(); });
    pipeline->SetSwapperFactory([] { return std::make_unique<Tests::MockAssemblySwapper>(true, 1); });
    return pipeline;
}

std::filesystem::path WriteAssembly(const char* directoryName)
{
    const auto directory = std::filesystem::temp_directory_path() / directoryName;
    std::filesystem::create_directories(directory);
    const auto path = directory / "GameEngine.Scripts.dll";
    std::ofstream file(path, std::ios::binary);
    file << "MZ";
    return path;
}

bool WaitUntilStarted(const CompileGate& gate)
{
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (!gate.Started.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    return gate.Started.load();
}

// Destroys the pipeline on another thread, checks that the destructor is still
// blocked on the running compile, then opens the gate and checks that it
// returns once the compile has finished.
void ExpectDestructionJoinsTheRunningCompile(std::unique_ptr<HotReloadPipeline>& pipeline, CompileGate& gate)
{
    auto destroyed = std::async(std::launch::async, [&pipeline] { pipeline.reset(); });
    EXPECT_EQ(destroyed.wait_for(kBlockedWindow), std::future_status::timeout)
        << "the pipeline was destroyed while its compile stage was still running";
    EXPECT_FALSE(gate.Finished.load());

    gate.Release();
    ASSERT_EQ(destroyed.wait_for(kDeadline), std::future_status::ready);
    EXPECT_TRUE(gate.Finished.load()) << "the destructor returned before the compile stage finished";
}

} // namespace

TEST(HotReloadPipelineJoin, CancelReturnsWhileCompilingAndDestructionWaitsForTheCompile)
{
    const auto assemblyPath = WriteAssembly("GE_HotReloadPipelineJoin_Cancel");
    JobSystem::WorkStealingThreadPool pool(2);
    JobSystem::JobChannel compiles(pool, kCompilesDesc);
    auto gate = std::make_shared<CompileGate>();
    auto pipeline = MakeGatedPipeline(pool, compiles, gate);
    GateReleaser releaser{gate};

    auto run = pipeline->ExecuteAsync(assemblyPath.string());
    ASSERT_TRUE(WaitUntilStarted(*gate)) << "the compile stage never started";

    pipeline->Cancel();
    ASSERT_EQ(run.wait_for(kDeadline), std::future_status::ready) << "Cancel did not resolve the run";
    EXPECT_EQ(run.get().result, HotReloadResult::CancelledByNewOperation);

    ExpectDestructionJoinsTheRunningCompile(pipeline, *gate);

    std::error_code ec;
    std::filesystem::remove_all(assemblyPath.parent_path(), ec);
}

TEST(HotReloadPipelineJoin, DestructionWaitsForASupersededRunsCompile)
{
    const auto assemblyPath = WriteAssembly("GE_HotReloadPipelineJoin_Supersede");
    JobSystem::WorkStealingThreadPool pool(2);
    JobSystem::JobChannel compiles(pool, kCompilesDesc);
    auto gate = std::make_shared<CompileGate>();
    auto pipeline = MakeGatedPipeline(pool, compiles, gate);
    GateReleaser releaser{gate};

    auto superseded = pipeline->ExecuteAsync(assemblyPath.string());
    ASSERT_TRUE(WaitUntilStarted(*gate)) << "the first compile stage never started";

    // The superseded run resolves at once. The newer run's compile waits in the
    // "Script compiles" channel behind the compile that is still held open:
    // the channel runs one compile at a time.
    auto newest = pipeline->ExecuteAsync(assemblyPath.string());
    ASSERT_EQ(superseded.wait_for(kDeadline), std::future_status::ready);
    EXPECT_EQ(superseded.get().result, HotReloadResult::CancelledByNewOperation);
    EXPECT_EQ(newest.wait_for(kBlockedWindow), std::future_status::timeout)
        << "the superseding run's compile ran beside the superseded one";

    ExpectDestructionJoinsTheRunningCompile(pipeline, *gate);
    ASSERT_EQ(newest.wait_for(kDeadline), std::future_status::ready) << "destroying the pipeline stranded the newer run";

    std::error_code ec;
    std::filesystem::remove_all(assemblyPath.parent_path(), ec);
}
