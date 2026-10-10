// S9: AssetDatabase/Registry at package scale — the before/after oracle for
// the E2/E3/E4 work (parallel cold import, non-blocking mounts, path-lookup
// hot paths).
//
// Method:
//   1. Generate N small synthetic files under a temp source root.
//   2. Cold pass: mount the root as a project-shaped source (derived identity,
//      authoritative JSONL + SQLite cache — the editor's exact mount shape)
//      and measure mount (RegisterSource) wall time + cold-scan-to-registered
//      wall time, then per-lookup costs, then Shutdown (writes the snapshot).
//   3. Warm pass: re-mount the same root. RegisterSource now loads a 5k-record
//      JSONL + populates hot caches — this is the E3 "registry freeze" number —
//      then the warm scan replays the snapshot.
//
// Stage timings print in the existing [F.1/F.3a init-prof] style with an
// [S9 bench] prefix so before/after runs diff cleanly.
//
// DISABLED_ by default (creates 5k files; seconds of wall time). Run with:
//   AssetDbBenchmarks --gtest_also_run_disabled_tests --gtest_filter='*AssetDbScale*'

#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

namespace fs = std::filesystem;
using bclock = std::chrono::high_resolution_clock;
using msd = std::chrono::duration<double, std::milli>;
using usd = std::chrono::duration<double, std::micro>;

constexpr size_t kAssetCount = 5000;
constexpr size_t kAssetCountLarge = 50000;
constexpr size_t kDirBuckets = 64;
constexpr size_t kLookupIterations = 20000;
constexpr size_t kIndexSnapshotIterations = 10;

void GenerateSyntheticSourceTree(const fs::path& root, size_t count)
{
    std::error_code ec;
    fs::create_directories(root, ec);
    for (size_t b = 0; b < kDirBuckets; ++b)
    {
        fs::create_directories(root / ("pkg" + std::to_string(b)), ec);
    }
    for (size_t i = 0; i < count; ++i)
    {
        const fs::path filePath =
            root / ("pkg" + std::to_string(i % kDirBuckets)) / ("asset" + std::to_string(i) + ".txt");
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        out << "synthetic package asset " << i << "\n";
    }
}

AssetSourceDesc MakeBenchProjectDesc(const fs::path& root, const fs::path& stateDir)
{
    // Mirrors AssetManager::Initialize's editor/dev project mount: derived
    // identity with the JSONL store as a derived cache + SQLite fingerprints.
    AssetSourceDesc desc{};
    desc.Alias = "project";
    desc.Root = root;
    desc.DerivedIdentity = true;
    desc.AuthoritativeDbFile = stateDir / "AssetDatabase.assetdb";
    desc.CacheRoot = stateDir / "Cache";
    desc.Priority = 100;
    return desc;
}

struct ScaleBenchNumbers
{
    double coldMountMs = 0.0;
    double coldScanMs = 0.0;
    double coldShutdownMs = 0.0;
    double warmMountMs = 0.0;
    double warmMountMaxReaderStallMs = 0.0;
    double warmScanMs = 0.0;
    double lookupGuidByPathUs = 0.0;
    double lookupMetaByGuidUs = 0.0;
    double repeatedMetadataIndexMs = 0.0;
    double bulkIndexSnapshotMs = 0.0;
    size_t coldRegistered = 0;
    size_t warmRegistered = 0;
};

// Out-parameter (not a return) because ASSERT_* macros require a void
// enclosing function.
void RunScaleBench(const fs::path& root, const fs::path& stateDir, size_t count, ScaleBenchNumbers& n)
{
    JobSystem::WorkStealingThreadPool pool(8);

    std::vector<fs::path> samplePaths;
    std::vector<GUID> sampleGuids;

    // ---- Cold pass: no JSONL, no cache, no snapshot. ----
    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(&pool)) << "infra init failed";

        const auto t0 = bclock::now();
        ASSERT_TRUE(reg.RegisterSource(MakeBenchProjectDesc(root, stateDir)));
        const auto t1 = bclock::now();
        reg.WaitForStartupScan();
        const auto t2 = bclock::now();

        n.coldMountMs = msd(t1 - t0).count();
        n.coldScanMs = msd(t2 - t1).count();
        n.coldRegistered = reg.GetAssetCount();

        // Lookup micro-bench: path->GUID and GUID->metadata over a rotating
        // sample of registered paths (m_PathToGuid + m_Assets hot paths).
        samplePaths.reserve(256);
        for (size_t i = 0; i < 256; ++i)
        {
            const size_t idx = (i * 97) % count;
            samplePaths.push_back(root / ("pkg" + std::to_string(idx % kDirBuckets)) /
                                  ("asset" + std::to_string(idx) + ".txt"));
        }
        sampleGuids.reserve(samplePaths.size());
        for (const auto& p : samplePaths)
            sampleGuids.push_back(reg.GetAssetGUID(p));

        {
            const auto lt0 = bclock::now();
            size_t hits = 0;
            for (size_t i = 0; i < kLookupIterations; ++i)
            {
                if (!reg.GetAssetGUID(samplePaths[i % samplePaths.size()]).IsNull())
                    ++hits;
            }
            const auto lt1 = bclock::now();
            EXPECT_EQ(hits, kLookupIterations);
            n.lookupGuidByPathUs = usd(lt1 - lt0).count() / static_cast<double>(kLookupIterations);
        }
        {
            const auto lt0 = bclock::now();
            size_t hits = 0;
            AssetMetadata md;
            for (size_t i = 0; i < kLookupIterations; ++i)
            {
                if (reg.TryGetAssetMetadata(sampleGuids[i % sampleGuids.size()], md))
                    ++hits;
            }
            const auto lt1 = bclock::now();
            EXPECT_EQ(hits, kLookupIterations);
            n.lookupMetaByGuidUs = usd(lt1 - lt0).count() / static_cast<double>(kLookupIterations);
        }

        const auto st0 = bclock::now();
        reg.Shutdown();
        n.coldShutdownMs = msd(bclock::now() - st0).count();
    }

    // ---- Warm pass: JSONL + cache + snapshot all present. ----
    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(&pool));

        // E3 freeze probe: a reader thread hammers a shared-lock query while
        // the mount runs. The longest single call is the worst reader stall
        // the mount caused — the actual "registry freeze", as opposed to the
        // mount's wall time (which stays the same; the work just moved off
        // the lock).
        std::atomic<bool> probeStop{false};
        std::atomic<int64_t> probeMaxUs{0};
        std::thread probe([&reg, &probeStop, &probeMaxUs]
        {
            while (!probeStop.load(std::memory_order_relaxed))
            {
                const auto q0 = bclock::now();
                (void)reg.GetAssetCount();
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(bclock::now() - q0).count();
                int64_t prev = probeMaxUs.load(std::memory_order_relaxed);
                while (us > prev && !probeMaxUs.compare_exchange_weak(prev, us)) {}
                std::this_thread::yield();
            }
        });

        const auto t0 = bclock::now();
        ASSERT_TRUE(reg.RegisterSource(MakeBenchProjectDesc(root, stateDir)));
        const auto t1 = bclock::now();
        probeStop.store(true, std::memory_order_relaxed);
        probe.join();
        reg.WaitForStartupScan();
        const auto t2 = bclock::now();

        n.warmMountMs = msd(t1 - t0).count();
        n.warmMountMaxReaderStallMs = static_cast<double>(probeMaxUs.load()) / 1000.0;
        n.warmScanMs = msd(t2 - t1).count();
        n.warmRegistered = reg.GetAssetCount();

        // Search-index rebuild probe. The legacy shape enumerates paths and
        // reacquires the registry lock for every metadata row. The bulk path
        // takes one lock-consistent lightweight copy for the whole project.
        size_t indexChecksum = 0;
        {
            const auto it0 = bclock::now();
            for (size_t iteration = 0; iteration < kIndexSnapshotIterations; ++iteration)
            {
                for (const fs::path& path : reg.GetRegisteredAssetPaths())
                {
                    AssetMetadata metadata;
                    if (reg.TryGetAssetMetadata(path, metadata))
                        indexChecksum += metadata.Path.native().size();
                }
            }
            const auto it1 = bclock::now();
            n.repeatedMetadataIndexMs =
                msd(it1 - it0).count() / static_cast<double>(kIndexSnapshotIterations);
        }
        {
            const auto it0 = bclock::now();
            for (size_t iteration = 0; iteration < kIndexSnapshotIterations; ++iteration)
            {
                const Vector<AssetIndexRecord> snapshot = reg.GetAssetIndexSnapshot();
                for (const AssetIndexRecord& record : snapshot)
                    indexChecksum += record.Path.native().size();
            }
            const auto it1 = bclock::now();
            n.bulkIndexSnapshotMs =
                msd(it1 - it0).count() / static_cast<double>(kIndexSnapshotIterations);
        }
        EXPECT_GT(indexChecksum, 0u);

        reg.Shutdown();
    }
}



void RunAndReport(size_t count)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_s9_scale");
    const fs::path root = tmp / "src";
    const fs::path stateDir = tmp / "state";
    std::error_code ec;

    GenerateSyntheticSourceTree(root, count);

    ScaleBenchNumbers n{};
    RunScaleBench(root, stateDir, count, n);
    if (::testing::Test::HasFatalFailure())
    {
        fs::remove_all(tmp, ec);
        return;
    }

    EXPECT_GE(n.coldRegistered, count) << "cold scan failed to register the synthetic tree";
    EXPECT_GE(n.warmRegistered, count) << "warm re-open lost records";

    std::fprintf(stderr,
        "[S9 bench] assets=%zu cold-mount=%.1fms cold-scan-to-registered=%.1fms cold-shutdown=%.1fms\n"
        "[S9 bench] assets=%zu warm-mount=%.1fms max-reader-stall=%.1fms warm-scan=%.1fms\n"
        "[S9 bench] lookup guid-by-path=%.2fus metadata-by-guid=%.2fus (avg over %zu iters)\n"
        "[S9 bench] search-index repeated-metadata=%.2fms bulk-snapshot=%.2fms (avg over %zu iters)\n",
        count, n.coldMountMs, n.coldScanMs, n.coldShutdownMs,
        count, n.warmMountMs, n.warmMountMaxReaderStallMs, n.warmScanMs,
        n.lookupGuidByPathUs, n.lookupMetaByGuidUs, kLookupIterations,
        n.repeatedMetadataIndexMs, n.bulkIndexSnapshotMs, kIndexSnapshotIterations);

    fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// Mounted-topology matrix (asset-splice-scale arc): {1×100k, 10×10k, 50×2k}.
//
// Measures the mount path in isolation from the scan pipeline:
//   1. Generate per-source synthetic trees + hand-written JSONL stores, so
//      RegisterSource is a warm-shaped mount (load JSONL → populate → splice)
//      with RequiresScan=false. This is the exact code path whose writer-lock
//      section stalls readers.
//   2. Two probe threads run across the whole mount sequence:
//        - GetAssetCount (registry-lock stall, matches the S9 probe)
//        - GetAssetGUID on a source-0 path (the cross-source reader: an
//          asset lookup for an ALREADY-mounted source while another source
//          mounts — the "streaming package mounts during play" victim)
//      The longest single call each observes is the max reader stall.
//   3. After all mounts: path→GUID and GUID→metadata lookup costs, single-
//      thread and 4-thread (hot-path regression gate for the overlay branch).
//
// Reported as [SM matrix] lines that diff cleanly before/after.
// ---------------------------------------------------------------------------

void WriteSyntheticJsonlStore(const fs::path& dbFile, size_t count)
{
    std::error_code ec;
    fs::create_directories(dbFile.parent_path(), ec);
    std::ofstream out(dbFile, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << dbFile.string();
    for (size_t i = 0; i < count; ++i)
    {
        // Stored GUIDs are placeholders: the matrix mounts are DerivedIdentity
        // sources, so PopulateHotCachesFromSource re-derives identity from the
        // canonical path (the editor/dev mount shape).
        out << "{\"guid\":\"" << GUID::Generate().ToString() << "\","
            << "\"path\":\"pkg" << (i % kDirBuckets) << "/asset" << i << ".txt\","
            << "\"type\":\"Texture\",\"missing\":false,\"kv\":{}}\n";
    }
}

struct MountMatrixNumbers
{
    double mountWallTotalMs = 0.0;
    double mountWallMaxMs = 0.0;
    double maxStallCountProbeMs = 0.0;
    double maxStallPathProbeMs = 0.0;
    double lookup1TGuidByPathUs = 0.0;
    double lookup1TMetaByGuidUs = 0.0;
    double lookup4TGuidByPathUs = 0.0;   // per-op latency observed per thread
    double lookup4TAggOpsPerSec = 0.0;
    size_t registeredTotal = 0;
    // Rebind phase: source 0 rebound to an alternate root of equal size.
    double rebindWallMs = 0.0;
    double rebindMaxStallCountMs = 0.0;
    double rebindMaxStallPathMs = 0.0;
    // Unmount phase: every source unregistered (probe target last).
    double unmountWallTotalMs = 0.0;
    double unmountWallMaxMs = 0.0;
    double unmountMaxStallCountMs = 0.0;
    double unmountMaxStallPathMs = 0.0;
};

// Reader-stall probe pair used by every phase: GetAssetCount (writer-block)
// and a path lookup (the cross-source reader a streaming world would run).
// The longest single call each observes across the phase is the max stall.
struct StallProbePair
{
    std::atomic<bool> Stop{false};
    std::atomic<int64_t> MaxCountUs{0};
    std::atomic<int64_t> MaxPathUs{0};
    std::thread CountThread;
    std::thread PathThread;

    void Start(AssetRegistry& reg, const fs::path& probePath)
    {
        const auto trackMax = [](std::atomic<int64_t>& slot, int64_t us)
        {
            int64_t prev = slot.load(std::memory_order_relaxed);
            while (us > prev && !slot.compare_exchange_weak(prev, us)) {}
        };
        CountThread = std::thread([this, &reg, trackMax]
        {
            while (!Stop.load(std::memory_order_relaxed))
            {
                const auto q0 = bclock::now();
                (void)reg.GetAssetCount();
                trackMax(MaxCountUs,
                         std::chrono::duration_cast<std::chrono::microseconds>(bclock::now() - q0).count());
                std::this_thread::yield();
            }
        });
        PathThread = std::thread([this, &reg, probePath, trackMax]
        {
            while (!Stop.load(std::memory_order_relaxed))
            {
                const auto q0 = bclock::now();
                (void)reg.GetAssetGUID(probePath);
                trackMax(MaxPathUs,
                         std::chrono::duration_cast<std::chrono::microseconds>(bclock::now() - q0).count());
                std::this_thread::yield();
            }
        });
    }

    void Join(double& outMaxCountMs, double& outMaxPathMs)
    {
        Stop.store(true, std::memory_order_relaxed);
        CountThread.join();
        PathThread.join();
        outMaxCountMs = static_cast<double>(MaxCountUs.load()) / 1000.0;
        outMaxPathMs = static_cast<double>(MaxPathUs.load()) / 1000.0;
    }
};

void RunMountMatrix(size_t sourceCount, size_t recordsPerSource, MountMatrixNumbers& n)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_sm_matrix");
    std::error_code ec;

    std::vector<fs::path> roots(sourceCount);
    for (size_t s = 0; s < sourceCount; ++s)
    {
        roots[s] = tmp / ("src" + std::to_string(s));
        GenerateSyntheticSourceTree(roots[s], recordsPerSource);
        WriteSyntheticJsonlStore(tmp / ("state" + std::to_string(s)) / "AssetDatabase.assetdb",
                                 recordsPerSource);
    }

    // Alternate root for the rebind phase (source 0 moves here).
    const fs::path rebindRoot = tmp / "srcR";
    GenerateSyntheticSourceTree(rebindRoot, recordsPerSource);
    WriteSyntheticJsonlStore(tmp / "stateR" / "AssetDatabase.assetdb", recordsPerSource);

    JobSystem::WorkStealingThreadPool pool(8);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));

    const auto makeDesc = [&](size_t s, const fs::path& root, const char* stateName)
    {
        AssetSourceDesc desc{};
        desc.Alias = (s == 0) ? std::string("project") : ("pkg" + std::to_string(s));
        desc.Root = root;
        desc.DerivedIdentity = true;
        desc.AuthoritativeDbFile = tmp / stateName / "AssetDatabase.assetdb";
        desc.CacheRoot = tmp / stateName / "Cache";
        desc.Priority = 100 - static_cast<int32_t>(s);
        desc.RequiresScan = false;  // isolate mount paths from the scan pipeline
        return desc;
    };

    // ---- Mount phase. Cross-source probe: a source-0 asset path (misses
    // until source 0 mounts, then hits — either way the full lock+normalize+
    // probe read path a streaming-load worker would run mid-play). ----
    {
        StallProbePair probes;
        probes.Start(reg, roots[0] / "pkg0" / "asset0.txt");

        for (size_t s = 0; s < sourceCount; ++s)
        {
            const std::string stateName = "state" + std::to_string(s);
            const auto t0 = bclock::now();
            ASSERT_TRUE(reg.RegisterSource(makeDesc(s, roots[s], stateName.c_str())))
                << "mount " << s << " failed";
            const double wallMs = msd(bclock::now() - t0).count();
            n.mountWallTotalMs += wallMs;
            n.mountWallMaxMs = std::max(n.mountWallMaxMs, wallMs);

            // Mounts are atomic to readers AND complete by return: the source's
            // full record set must be resolvable now.
            EXPECT_EQ(reg.GetAssetCount(), (s + 1) * recordsPerSource)
                << "mount " << s << " did not publish its full record set by return";
        }

        probes.Join(n.maxStallCountProbeMs, n.maxStallPathProbeMs);
    }
    n.registeredTotal = reg.GetAssetCount();

    // Sample paths spanning ALL sources for the lookup benches.
    std::vector<fs::path> samplePaths;
    samplePaths.reserve(256);
    for (size_t i = 0; i < 256; ++i)
    {
        const size_t src = i % sourceCount;
        const size_t idx = (i * 97) % recordsPerSource;
        samplePaths.push_back(roots[src] / ("pkg" + std::to_string(idx % kDirBuckets)) /
                              ("asset" + std::to_string(idx) + ".txt"));
    }
    std::vector<GUID> sampleGuids;
    sampleGuids.reserve(samplePaths.size());
    for (const auto& p : samplePaths)
    {
        const GUID g = reg.GetAssetGUID(p);
        ASSERT_FALSE(g.IsNull()) << p.string();
        sampleGuids.push_back(g);
    }

    // Single-thread lookups.
    {
        const auto lt0 = bclock::now();
        size_t hits = 0;
        for (size_t i = 0; i < kLookupIterations; ++i)
        {
            if (!reg.GetAssetGUID(samplePaths[i % samplePaths.size()]).IsNull())
                ++hits;
        }
        const auto lt1 = bclock::now();
        EXPECT_EQ(hits, kLookupIterations);
        n.lookup1TGuidByPathUs = usd(lt1 - lt0).count() / static_cast<double>(kLookupIterations);
    }
    {
        const auto lt0 = bclock::now();
        size_t hits = 0;
        AssetMetadata md;
        for (size_t i = 0; i < kLookupIterations; ++i)
        {
            if (reg.TryGetAssetMetadata(sampleGuids[i % sampleGuids.size()], md))
                ++hits;
        }
        const auto lt1 = bclock::now();
        EXPECT_EQ(hits, kLookupIterations);
        n.lookup1TMetaByGuidUs = usd(lt1 - lt0).count() / static_cast<double>(kLookupIterations);
    }

    // 4-thread path→GUID lookups (shared-lock scaling gate).
    {
        constexpr size_t kThreads = 4;
        constexpr size_t kItersPerThread = 20000;
        std::atomic<size_t> totalHits{0};
        std::vector<std::thread> readers;
        const auto lt0 = bclock::now();
        for (size_t t = 0; t < kThreads; ++t)
        {
            readers.emplace_back([&, t]
            {
                size_t hits = 0;
                for (size_t i = 0; i < kItersPerThread; ++i)
                {
                    if (!reg.GetAssetGUID(samplePaths[(i * kThreads + t) % samplePaths.size()]).IsNull())
                        ++hits;
                }
                totalHits.fetch_add(hits, std::memory_order_relaxed);
            });
        }
        for (auto& r : readers)
            r.join();
        const double totalUs = usd(bclock::now() - lt0).count();
        EXPECT_EQ(totalHits.load(), kThreads * kItersPerThread);
        n.lookup4TGuidByPathUs = totalUs / static_cast<double>(kItersPerThread); // wall per-op per thread
        n.lookup4TAggOpsPerSec = static_cast<double>(kThreads * kItersPerThread) / (totalUs / 1e6);
    }

    // ---- Rebind phase: move source 0 to the alternate root while readers
    // run. The path probe targets a NON-rebound source when one exists (the
    // cross-source victim); with a single source it probes the rebound one
    // (latency is the metric either way). ----
    {
        const fs::path rebindProbePath = (sourceCount > 1)
            ? roots[1] / "pkg0" / "asset0.txt"
            : roots[0] / "pkg0" / "asset0.txt";
        StallProbePair probes;
        probes.Start(reg, rebindProbePath);

        const auto t0 = bclock::now();
        ASSERT_TRUE(reg.RebindSource("project", makeDesc(0, rebindRoot, "stateR")));
        n.rebindWallMs = msd(bclock::now() - t0).count();

        probes.Join(n.rebindMaxStallCountMs, n.rebindMaxStallPathMs);

        // Determinism at return: same total count (equal-size replacement),
        // new-root paths resolve, old-root paths are gone.
        EXPECT_EQ(reg.GetAssetCount(), sourceCount * recordsPerSource)
            << "rebind did not atomically replace source 0's record set";
        EXPECT_FALSE(reg.GetAssetGUID(rebindRoot / "pkg0" / "asset0.txt").IsNull());
        EXPECT_TRUE(reg.GetAssetGUID(roots[0] / "pkg0" / "asset0.txt").IsNull());
    }

    // ---- Unmount phase: unregister every source, probe target last so the
    // path probe stays a live cross-source reader for the whole phase. ----
    {
        const fs::path unmountProbePath = (sourceCount > 1)
            ? roots[1] / "pkg0" / "asset0.txt"
            : rebindRoot / "pkg0" / "asset0.txt";
        StallProbePair probes;
        probes.Start(reg, unmountProbePath);

        // Source 1 (the probe target) last; source 0 now lives at rebindRoot.
        std::vector<std::string> unmountOrder;
        for (size_t s = 0; s < sourceCount; ++s)
        {
            if (sourceCount > 1 && s == 1)
                continue;
            unmountOrder.push_back((s == 0) ? std::string("project") : ("pkg" + std::to_string(s)));
        }
        if (sourceCount > 1)
            unmountOrder.push_back("pkg1");

        for (const std::string& alias : unmountOrder)
        {
            const auto t0 = bclock::now();
            ASSERT_TRUE(reg.UnregisterSource(alias)) << alias;
            const double wallMs = msd(bclock::now() - t0).count();
            n.unmountWallTotalMs += wallMs;
            n.unmountWallMaxMs = std::max(n.unmountWallMaxMs, wallMs);
        }

        probes.Join(n.unmountMaxStallCountMs, n.unmountMaxStallPathMs);
        EXPECT_EQ(reg.GetAssetCount(), 0u) << "unmount left records behind";
    }

    reg.Shutdown();
    fs::remove_all(tmp, ec);
}

void RunMountMatrixAndReport(size_t sourceCount, size_t recordsPerSource)
{
    MountMatrixNumbers n{};
    RunMountMatrix(sourceCount, recordsPerSource, n);
    if (::testing::Test::HasFatalFailure())
        return;

    std::fprintf(stderr,
        "[SM matrix] sources=%zu recs=%zu total=%zu mount-wall-total=%.1fms mount-wall-max=%.1fms\n"
        "[SM matrix] max-reader-stall count-probe=%.2fms path-probe=%.2fms\n"
        "[SM matrix] lookup-1t guid-by-path=%.2fus meta-by-guid=%.2fus | lookup-4t guid-by-path=%.2fus agg=%.0f ops/s\n"
        "[SM matrix] rebind wall=%.1fms max-stall count=%.2fms path=%.2fms\n"
        "[SM matrix] unmount wall-total=%.1fms wall-max=%.1fms max-stall count=%.2fms path=%.2fms\n",
        sourceCount, recordsPerSource, n.registeredTotal, n.mountWallTotalMs, n.mountWallMaxMs,
        n.maxStallCountProbeMs, n.maxStallPathProbeMs,
        n.lookup1TGuidByPathUs, n.lookup1TMetaByGuidUs,
        n.lookup4TGuidByPathUs, n.lookup4TAggOpsPerSec,
        n.rebindWallMs, n.rebindMaxStallCountMs, n.rebindMaxStallPathMs,
        n.unmountWallTotalMs, n.unmountWallMaxMs, n.unmountMaxStallCountMs, n.unmountMaxStallPathMs);
}

} // namespace

// DISABLED_: creates 5k files and runs two full mount+scan cycles. Run
// manually for before/after comparisons (see file header).
TEST(AssetDbScaleBenchmark, DISABLED_FiveThousandAssetImport)
{
    RunAndReport(kAssetCount);
}

// Package-scale point: 10x the file count. The warm pass exercises the
// writer-section splice (see the [S9 splice-prof] line RegisterSource prints)
// at a size where O(N) work under the registry lock would be an editor-visible
// freeze. Creates 50k files; tens of seconds of wall time.
TEST(AssetDbScaleBenchmark, DISABLED_FiftyThousandAssetImport)
{
    RunAndReport(kAssetCountLarge);
}

// Mounted-topology matrix (see the block comment above RunMountMatrix).
// DISABLED_: each creates up to 100k files; minutes of wall time. Run with:
//   AssetDbBenchmarks --gtest_also_run_disabled_tests --gtest_filter='*MountMatrix*'
TEST(AssetDbScaleBenchmark, DISABLED_MountMatrix_OneSource100k)
{
    RunMountMatrixAndReport(1, 100000);
}

TEST(AssetDbScaleBenchmark, DISABLED_MountMatrix_TenSources10k)
{
    RunMountMatrixAndReport(10, 10000);
}

TEST(AssetDbScaleBenchmark, DISABLED_MountMatrix_FiftySources2k)
{
    RunMountMatrixAndReport(50, 2000);
}
