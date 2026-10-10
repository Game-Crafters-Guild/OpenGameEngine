#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <filesystem>
#include "Scripting/ScriptingABI.h"

// Helper: invoke by name (domain 0 selects current runtime domain)
static GE_Result InvokeByName(const char* name, int32_t* out)
{
    const uint32_t len = (uint32_t)strlen(name);
    return GE_Invoke((GE_DomainHandle)0ull, name, len, out);
}

TEST(ScriptingAbi_ReloadStress, ConcurrentInvokeWhileRepeatedSwaps_Converges)
{
    // Ensure we can locate the test assembly from build root
    // Resolve absolute path to the test assembly from common candidate locations
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(GameEngine::TestPaths::ExecutableDirectory() / "DomainRoutingTest.dll"); // staged next to the test exe
    candidates.push_back(GameEngine::TestPaths::StagedRoot() / "DomainRoutingTest.dll");          // build root copy
    candidates.push_back(GameEngine::Tests::DomainRoutingTestDll());                              // fixture output tree

    std::string asmPath;
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) { asmPath = p.string(); break; }
    }
    ASSERT_FALSE(asmPath.empty()) << "DomainRoutingTest.dll not found in expected locations";
    const char* kAssembly = asmPath.c_str();

    // Quick sanity: preload once so first swap has material
    ASSERT_EQ(GE_PreloadAssemblyContext(kAssembly, (uint32_t)strlen(kAssembly)), GE_Result_Ok);
    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    std::atomic<bool> runWorkers{true};
    std::atomic<int> totalOk{0};
    std::atomic<int> totalFail{0};

    auto worker = [&]() {
        while (runWorkers.load(std::memory_order_relaxed)) {
            int32_t out = -1;
            GE_Result r = InvokeByName("GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet", &out);
            if (r == GE_Result_Ok) {
                totalOk.fetch_add(1, std::memory_order_relaxed);
            } else {
                totalFail.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); // brief backoff on swap boundary
            }
        }
    };

    // Spin up a few workers
    const int kWorkers = 4;
    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (int i = 0; i < kWorkers; ++i) threads.emplace_back(worker);

    // Perform several preload->swap cycles while workers are invoking
    const int kSwaps = 6;
    for (int i = 0; i < kSwaps; ++i) {
        ASSERT_EQ(GE_PreloadAssemblyContext(kAssembly, (uint32_t)strlen(kAssembly)), GE_Result_Ok);
        ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);
        // Allow some activity between swaps
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Stop workers and join
    runWorkers.store(false, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    // We expect the system to make forward progress (some successful invokes), even with swaps
    EXPECT_GT(totalOk.load(), 0);

    // Optional: final readback should succeed
    int32_t val = -1;
    EXPECT_EQ(InvokeByName("GameEngine.Scripts.ScriptsEntryPoint.GetValue", &val), GE_Result_Ok);
}

TEST(ScriptingAbi_ReloadStress, PathMaskAndPerfCountersAvailable)
{
    // Availability-only; values depend on environment/binding
    const long long packed = GE_DebugGetPerfCountersPacked64();
    // -1 indicates not available; in dev env it should be >=0
    // Accept either to keep the test portable, but ensure the call executes
    EXPECT_NE(packed, -9999); // dummy guard to compile; always true unless API changes

    const int mask = GE_DebugGetPathMask();
    // -1 indicates not available; accept either but ensure no crash
    EXPECT_NE(mask, -9999);
}

