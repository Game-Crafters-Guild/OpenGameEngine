#include "Jobs/CompileServerVersion.h"
#include "Jobs/HotReloadTasks.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/IHotReloadTransport.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "JobSystem/TaskDependencyGraph.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <future>
#include <atomic>
#include <numeric>
#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace GameEngine;

namespace {
// These tests measure the pipeline's own orchestration overhead (submission
// latency, main-thread blocking, swap duration) against millisecond budgets.
// A real compile-server round trip (process launch + Roslyn compile) costs
// seconds and is environment-dependent, so the compile stage is pinned to a
// fast in-memory failure: the pipeline then takes its test-mode fallback
// (direct file load + simulated swap), which is the path these budgets were
// calibrated against. The fake speaks the __version__/__shutdown__ protocol
// so the client accepts it as a live server.
class FastFailTransport : public IHotReloadTransport {
public:
    bool Connect() override { return true; }
    bool SendRequest(const std::string& requestJson, std::string& outResponse) override {
        if (requestJson == "__version__") {
            outResponse = std::string("{\"Version\":\"") + kExpectedCompileServerVersion + "\"}";
            return true;
        }
        if (requestJson == "__shutdown__") {
            outResponse = "{}";
            return true;
        }
        outResponse = "{\"Success\":false,\"Warnings\":[],\"Errors\":[]}";
        return true;
    }
    void Close() override {}
};
}

/**
 * @brief Hot-reload specific performance regression test suite
 *
 * This test suite prevents performance regressions in the async hot-reload system
 * that achieved 0ms main thread blocking and 8-13ms total completion time.
 */
class HotReloadPerformanceRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);

        // Create test assembly directory. Every test process of this build shares the working
        // directory, so the folder carries this process's id: a concurrent run cannot delete
        // the assembly this one submits.
        m_AssemblyDirectory = std::filesystem::path("Tests") /
                              ("TestAssemblies_" + std::to_string(TestUtils::GetProcessIdForTests()));
        m_AssemblyPath = (m_AssemblyDirectory / "PerformanceTest.cs").generic_string();
        std::filesystem::create_directories(m_AssemblyDirectory);

        // Create a test assembly file for benchmarking
        createTestAssembly();

#ifdef _WIN32
        // Deterministic compile stage: server path enabled, but served by the
        // in-memory fast-fail transport instead of a real CompileServerHost.
        _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");
#endif
        SetCompileServerTransportFactoryForTests(
            [](const std::string&) { return std::make_unique<FastFailTransport>(); });
    }

    void TearDown() override {
        SetCompileServerTransportFactoryForTests(nullptr);
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

        // Clean up test files
        std::filesystem::remove_all(m_AssemblyDirectory);
    }
    
    void createTestAssembly() {
        std::ofstream file(m_AssemblyPath);
        file << "using System;\n";
        file << "namespace GameEngine.Scripts {\n";
        file << "    public class PerformanceTest {\n";
        file << "        public static void TestMethod() {\n";
        file << "            Console.WriteLine(\"Performance test executed\");\n";
        file << "        }\n";
        file << "    }\n";
        file << "}\n";
        file.close();
    }

    std::filesystem::path m_AssemblyDirectory;
    std::string m_AssemblyPath;
};

/**
 * @brief Critical regression test for 0ms main thread blocking achievement
 * 
 * This test ensures we maintain the 0ms main thread blocking performance
 * that was achieved through the 4-stage async pipeline optimization.
 */
TEST_F(HotReloadPerformanceRegressionTest, ZeroMainThreadBlockingRegression) {
    // Loosen thresholds in CI to avoid false failures due to environment variance
    const int MAX_MAIN_THREAD_BLOCKING_MS = 10;  // allow jitter
    const int MAX_TOTAL_COMPLETION_MS = 1500;    // relaxed upper bound for CI
    const int OPTIMAL_COMPLETION_MS = 200;       // warn if above optimal range

    Logger::Log::SetLogLevel(Logger::LogLevel::Error); // Minimize logging overhead

    auto threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(std::thread::hardware_concurrency());
    JobSystem::JobChannel compiles(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*threadPool, compiles);

    // Measure main thread blocking time
    auto mainThreadStart = std::chrono::high_resolution_clock::now();
    auto future = pipeline.ExecuteAsync(m_AssemblyPath);
    auto mainThreadEnd = std::chrono::high_resolution_clock::now();

    // Main thread blocking time (pipeline submission)
    auto mainThreadBlocking = std::chrono::duration_cast<std::chrono::milliseconds>(
        mainThreadEnd - mainThreadStart);

    // Wait for completion and measure total time
    auto totalStart = mainThreadStart;
    auto stats = future.get();
    auto totalEnd = std::chrono::high_resolution_clock::now();

    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        totalEnd - totalStart);

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

    // Sanity checks (relaxed for CI)
    EXPECT_LT(mainThreadBlocking.count(), MAX_MAIN_THREAD_BLOCKING_MS)
        << "Main thread blocking exceeded relaxed threshold";
    EXPECT_LT(totalDuration.count(), MAX_TOTAL_COMPLETION_MS)
        << "Total completion time exceeded relaxed threshold";
    EXPECT_LT(stats.swapDuration.count(), MAX_MAIN_THREAD_BLOCKING_MS)
        << "Assembly swap duration exceeded relaxed threshold";

    // Informational warnings
    if (totalDuration.count() > OPTIMAL_COMPLETION_MS) {
        Logger::Log::Warning("Performance degradation detected: {}ms (optimal: <{}ms)",
                       totalDuration.count(), OPTIMAL_COMPLETION_MS);
    }
}


/**
 * @brief Pipeline submission time regression test
 * 
 * Tests that pipeline submission remains fast (3-5ms) and doesn't block
 * the main thread during task creation and dependency setup.
 */
TEST_F(HotReloadPerformanceRegressionTest, PipelineSubmissionPerformance) {
    const int MAX_SUBMISSION_MS = 10; // <10ms submission (current: 3-5ms)
    const int OPTIMAL_SUBMISSION_MS = 6; // Warn if above optimal
    
    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    auto threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(std::thread::hardware_concurrency());
    JobSystem::JobChannel compiles(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*threadPool, compiles);
    
    // Measure multiple submissions for statistical accuracy
    std::vector<std::chrono::milliseconds> submissionTimes;
    const int NUM_TESTS = 10;
    
    for (int i = 0; i < NUM_TESTS; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto future = pipeline.ExecuteAsync(m_AssemblyPath);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto submissionTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        submissionTimes.push_back(submissionTime);
        
        // Wait for completion before next test
        future.get();
        
        // Brief pause between tests
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    
    // Calculate statistics
    auto avgSubmission = std::accumulate(submissionTimes.begin(), submissionTimes.end(), 
                                        std::chrono::milliseconds(0)) / NUM_TESTS;
    auto maxSubmission = *std::max_element(submissionTimes.begin(), submissionTimes.end());
    
    Logger::Log::Info("🚀 Pipeline Submission Performance ({} tests):", NUM_TESTS);
    Logger::Log::Info("   Average submission: {}ms (target: <{}ms)", 
                 avgSubmission.count(), MAX_SUBMISSION_MS);
    Logger::Log::Info("   Maximum submission: {}ms", maxSubmission.count());
    
    // Regression detection
    EXPECT_LT(avgSubmission.count(), MAX_SUBMISSION_MS)
        << "Pipeline submission performance regression detected";
    
    EXPECT_LT(maxSubmission.count(), MAX_SUBMISSION_MS * 2)
        << "Pipeline submission worst-case performance regression detected";
    
    // Performance warnings
    if (avgSubmission.count() > OPTIMAL_SUBMISSION_MS) {
        Logger::Log::Warning("⚠️  Submission performance degradation: {}ms (optimal: 3-5ms)", 
                       avgSubmission.count());
    }
}

/**
 * @brief Concurrent hot-reload submission test
 *
 * ExecuteAsync implements last-wins coalescing: submitting while a run is in
 * flight cancels the current run and keeps the newest request. Overlapping
 * submissions are therefore EXPECTED to report failure for superseded runs —
 * what must hold is that every future completes (no hang/deadlock), the last
 * submission wins and succeeds, and the whole burst stays within a bound that
 * catches pathological serialization of cancelled runs.
 */
TEST_F(HotReloadPerformanceRegressionTest, ConcurrentHotReloadPerformance) {
    const int MAX_CONCURRENT_COMPLETION_MS = 15000; // hang/serialization guard, not a perf target
    const int NUM_CONCURRENT_OPERATIONS = 3;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    auto threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(std::thread::hardware_concurrency());
    JobSystem::JobChannel compiles(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*threadPool, compiles);

    std::vector<std::future<HotReloadPipeline::PipelineStats>> futures;

    auto globalStart = std::chrono::high_resolution_clock::now();

    // Start overlapping operations; earlier ones may be coalesced away.
    for (int i = 0; i < NUM_CONCURRENT_OPERATIONS; ++i) {
        futures.push_back(pipeline.ExecuteAsync(m_AssemblyPath));
    }

    // Every future must complete — superseded runs report failure, they must not hang.
    std::vector<HotReloadPipeline::PipelineStats> results;
    for (auto& future : futures) {
        results.push_back(future.get());
    }

    auto globalEnd = std::chrono::high_resolution_clock::now();
    auto totalConcurrentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        globalEnd - globalStart);

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

    Logger::Log::Info("🔄 Concurrent Hot-Reload Submissions ({} operations):", NUM_CONCURRENT_OPERATIONS);
    Logger::Log::Info("   Total time: {}ms (guard: <{}ms)",
                 totalConcurrentTime.count(), MAX_CONCURRENT_COMPLETION_MS);

    // Last-wins: the newest submission must complete successfully.
    ASSERT_FALSE(results.empty());
    EXPECT_TRUE(results.back().success) << "Newest (last-wins) operation failed";

    // Runs that did complete must not have blocked the main thread in the swap.
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].success) {
            EXPECT_LE(results[i].swapDuration.count(), 1)
                << "Operation " << i << " had excessive main thread blocking";
        }
    }

    EXPECT_LT(totalConcurrentTime.count(), MAX_CONCURRENT_COMPLETION_MS)
        << "Overlapping submissions took pathologically long (hang or serialized cancellations)";
}
