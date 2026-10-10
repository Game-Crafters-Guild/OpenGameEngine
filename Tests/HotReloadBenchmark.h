#pragma once

#include "Types/Types.h"
#include <chrono>

// Forward declaration
namespace GameEngine { class HotReloadPipeline; }

namespace GameEngine::Tests {

/**
 * @brief Performance benchmark for hot-reload systems
 */
class HotReloadBenchmark {
public:
    struct BenchmarkScenario {
        String name;
        String path;
        size_t sizeOrCount;
    };

    struct BenchmarkResults {
        std::chrono::milliseconds mainThreadBlockTime{0};
        std::chrono::milliseconds totalPipelineTime{0};
        size_t memoryOverheadBytes{0};
        double cpuUtilization{0.0};
        size_t gcCollections{0};
        bool success{false};
        String errorMessage;
    };

    /**
     * @brief Run comprehensive hot-reload benchmarks
     */
    static void runAllBenchmarks();

    /**
     * @brief Benchmark current synchronous hot-reload system
     */
    static BenchmarkResults benchmarkSyncHotReload(const BenchmarkScenario& scenario);

    /**
     * @brief Benchmark proposed asynchronous hot-reload system
     */
    static BenchmarkResults benchmarkAsyncHotReload(const BenchmarkScenario& scenario, GameEngine::HotReloadPipeline& pipeline);

    /**
     * @brief Compare benchmark results and report improvements
     */
    static void compareResults(const String& scenarioName, const BenchmarkResults& sync, const BenchmarkResults& async);

    /**
     * @brief Run stress tests for reliability validation
     */
    static void runStressTests(GameEngine::HotReloadPipeline& pipeline);

private:
    // Simulation methods for benchmarking
    static void simulateSyncCompilation(const BenchmarkScenario& scenario);
    static void simulateSyncFileLoading(const BenchmarkScenario& scenario);
    static void simulateSyncAssemblySwap(const BenchmarkScenario& scenario);
    static bool simulateAsyncCompilation(const BenchmarkScenario& scenario);
    static bool simulateAsyncFileLoading(const BenchmarkScenario& scenario);
    static bool simulateAsyncAssemblyPrep(const BenchmarkScenario& scenario);
    static void simulateAsyncAssemblySwap(const BenchmarkScenario& scenario);
    static void simulateMainThreadWork(std::chrono::milliseconds duration);
};

} // namespace GameEngine::Tests
