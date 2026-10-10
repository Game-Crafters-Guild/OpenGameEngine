// Phase 5.3: warm-start benchmark for the AssetRegistry snapshot path.
//
// Method:
//   1. Create N synthetic asset files in a temp directory.
//   2. Initialize AssetRegistry, scan to populate the cache, Shutdown
//      (this writes watcher.snapshot.bin via AssetSourceSnapshot::Save).
//   3. Re-Initialize AssetRegistry against the same directory. The
//      reconcile path detects the snapshot, replays it, and short-circuits
//      the per-file stat+hash work for unchanged files (Block D, shipped
//      in 07e425b5).
//   4. Time the second Initialize + an idle scan (zero dirty) and assert
//      it completes under the target.
//
// The 1000-asset variant runs in CI (target derived linearly from the
// plan's 50K target of <200ms: ~10ms allowance per 1000 assets, with a
// generous CI ceiling of 100ms to absorb FS jitter on shared runners).
//
// The 50K variant is DISABLED_ by default — creating 50K files takes
// 30s+ on Windows and would dominate the regular CI run. Enable manually
// via `--gtest_also_run_disabled_tests` when checking the plan target.

#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace GameEngine;

namespace
{

void PopulateSyntheticAssetTree(const std::filesystem::path& root, size_t count)
{
    std::error_code ec;
    std::filesystem::create_directories(root, ec);

    // Spread across a small directory tree (16 subdirs) to mirror real-world
    // shape — purely linear directories would be unrealistic.
    constexpr size_t kBuckets = 16;
    for (size_t b = 0; b < kBuckets; ++b)
    {
        std::filesystem::create_directories(root / ("bucket" + std::to_string(b)), ec);
    }

    for (size_t i = 0; i < count; ++i)
    {
        const size_t bucket = i % kBuckets;
        const std::filesystem::path filePath =
            root / ("bucket" + std::to_string(bucket)) / ("asset" + std::to_string(i) + ".txt");
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        out << "synthetic asset " << i;
    }
}

struct WarmStartBreakdown
{
    double totalMs = 0.0;
    double constructMs = 0.0;
    double initializeMs = 0.0;
    double scanMs = 0.0;
    double shutdownMs = 0.0;
};

// Run Initialize + scan + Shutdown once to seed the snapshot, then run
// Initialize again and time the warm-start. Returns the per-phase
// breakdown so the loop can attack the dominant cost.
WarmStartBreakdown MeasureWarmStart(const std::filesystem::path& root, size_t /*count*/)
{
    using clock = std::chrono::high_resolution_clock;
    using ms = std::chrono::duration<double, std::milli>;

    JobSystem::WorkStealingThreadPool pool(4);

    // Cold pass: writes watcher.snapshot.bin on Shutdown. Untimed.
    {
        AssetRegistry reg;
        bool ok = reg.Initialize(root, &pool);
        if (!ok) return {-1.0, 0, 0, 0, 0};
        auto fut = reg.ScanDirectoryAsync(root, /*recursive=*/true);
        (void)fut.get();
        reg.Shutdown();
    }

    WarmStartBreakdown b{};
    const auto tStart = clock::now();
    {
        const auto t0 = clock::now();
        AssetRegistry reg;
        const auto t1 = clock::now();
        bool ok = reg.Initialize(root, &pool);
        const auto t2 = clock::now();
        if (!ok) return {-1.0, 0, 0, 0, 0};
        auto fut = reg.ScanDirectoryAsync(root, /*recursive=*/true);
        (void)fut.get();
        const auto t3 = clock::now();
        reg.Shutdown();
        const auto t4 = clock::now();

        b.constructMs  = ms(t1 - t0).count();
        b.initializeMs = ms(t2 - t1).count();
        b.scanMs       = ms(t3 - t2).count();
        b.shutdownMs   = ms(t4 - t3).count();
    }
    b.totalMs = ms(clock::now() - tStart).count();
    return b;
}

double MeasureWarmStartMs(const std::filesystem::path& root, size_t count)
{
    const auto b = MeasureWarmStart(root, count);
    if (b.totalMs < 0.0) return -1.0;
    // gtest swallows logger output; print to stderr so the breakdown
    // shows up next to the assertion line.
    std::fprintf(stderr,
                 "[Phase 5.3 bench] warm-start phases: "
                 "construct=%.2fms init=%.2fms scan=%.2fms shutdown=%.2fms total=%.2fms\n",
                 b.constructMs, b.initializeMs, b.scanMs, b.shutdownMs, b.totalMs);
    Logger::Log::Info(
        "[Phase 5.3 bench] warm-start phases: construct={:.2f}ms init={:.2f}ms scan={:.2f}ms shutdown={:.2f}ms",
        b.constructMs, b.initializeMs, b.scanMs, b.shutdownMs);
    return b.totalMs;
}

} // namespace

// Default in-CI variant. 1000 assets is large enough to surface real
// signal but small enough to run in seconds.
//
// Assertion policy:
//   - Release (NDEBUG): strict 100ms ceiling. Plan target for 50K is <200ms;
//     at 1K the per-call overhead floor (JobSystem spin-up, parser-registry
//     init) dominates so a tighter ceiling still leaves room for real work.
//     A failure here means warm-start has regressed.
//   - Debug: 30s sanity-only. Debug instrumentation dominates the budget;
//     this is just a wedge-catcher, the real signal is in Release.
TEST(SnapshotWarmStartBenchmark, OneThousandAssetsWarmStart)
{
    namespace fs = std::filesystem;
    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_warmstart_1k");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);

    constexpr size_t kCount = 1000;
    PopulateSyntheticAssetTree(tmpRoot, kCount);

    const double warmMs = MeasureWarmStartMs(tmpRoot, kCount);
    ASSERT_GE(warmMs, 0.0) << "Warm-start measurement failed";

#ifdef NDEBUG
    Logger::Log::Info("[Phase 5.3 bench] {} assets warm-start (Release): {:.1f}ms", kCount, warmMs);
    EXPECT_LT(warmMs, 100.0)
        << "Warm-start regressed past 100ms ceiling for 1000 assets (Release)";
#else
    Logger::Log::Info("[Phase 5.3 bench] {} assets warm-start (Debug): {:.1f}ms", kCount, warmMs);
    EXPECT_LT(warmMs, 30000.0)
        << "Warm-start exceeded 30s Debug sanity ceiling — something is wedged";
#endif

    fs::remove_all(tmpRoot, ec);
}

// 50K-asset variant — the plan's target case. DISABLED_ because creating
// 50K files takes 30s+ on Windows; not appropriate for default CI.
// Enable via: AssetPerformanceBenchmarks --gtest_also_run_disabled_tests
//   or:      AssetPerformanceBenchmarks --gtest_filter='*FiftyKAssets*'
//
// Assertion policy mirrors the 1K variant — strict Release plan target,
// Debug sanity-only.
TEST(SnapshotWarmStartBenchmark, DISABLED_FiftyKAssetsWarmStart)
{
    namespace fs = std::filesystem;
    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_warmstart_50k");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);

    constexpr size_t kCount = 50000;

    auto setupStart = std::chrono::high_resolution_clock::now();
    PopulateSyntheticAssetTree(tmpRoot, kCount);
    auto setupEnd = std::chrono::high_resolution_clock::now();
    const double setupMs = std::chrono::duration<double, std::milli>(setupEnd - setupStart).count();

    Logger::Log::Info("[Phase 5.3 bench] 50K asset tree creation: {:.1f}ms", setupMs);

    const double warmMs = MeasureWarmStartMs(tmpRoot, kCount);
    ASSERT_GE(warmMs, 0.0) << "Warm-start measurement failed";

#ifdef NDEBUG
    Logger::Log::Info("[Phase 5.3 bench] {} assets warm-start (Release): {:.1f}ms (plan target: <200ms)",
                      kCount, warmMs);
    EXPECT_LT(warmMs, 200.0) << "Plan target missed for 50K-asset warm-start (Release)";
#else
    Logger::Log::Info("[Phase 5.3 bench] {} assets warm-start (Debug): {:.1f}ms (plan target Release: <200ms)",
                      kCount, warmMs);
    EXPECT_LT(warmMs, 120000.0)
        << "Warm-start exceeded 2-minute Debug sanity ceiling for 50K assets — something is wedged";
#endif

    fs::remove_all(tmpRoot, ec);
}
