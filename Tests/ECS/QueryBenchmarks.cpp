#include <gtest/gtest.h>
#include <cstdio>
#include <numeric>
#include <atomic>
#include <vector>
#include <chrono>
#include <algorithm>
#include <functional>

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ChunkCallAdapter.h" // for optional diagnostics counters

#include "Logger/Logger.h"
#include "ArchetypeSpread.h"
#include "TestComponents.h"

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace {
    static uint64_t MeasureMedianMicros(int warmups, int runs, const std::function<void()>& fn) {
        for (int i = 0; i < warmups; ++i) fn();
        std::vector<uint64_t> samples; samples.reserve(runs);
        for (int i = 0; i < runs; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            fn();
            auto t1 = std::chrono::high_resolution_clock::now();
            samples.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[samples.size()/2];
    }
}

namespace {
struct BenchResult { std::size_t count; uint64_t micros; };

BenchResult RunEach(World& w) {
    auto start = std::chrono::high_resolution_clock::now();
    std::size_t cnt = 0;
    w.Query<Write<Position>, Read<Velocity>>().Each([&](EntityHandle, Position& p, const Velocity& v){
        p.x += v.x * 0.016f; ++cnt;
    });
    auto end = std::chrono::high_resolution_clock::now();
    return {cnt, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(end-start).count()};
}

BenchResult RunParallel(World& w, std::size_t minBatch) {
    auto start = std::chrono::high_resolution_clock::now();
    std::atomic<std::size_t> cnt{0};
    w.Query<Write<Position>, Read<Velocity>>().Parallel([&](EntityHandle, Position& p, const Velocity& v){
        p.x += v.x * 0.016f; cnt.fetch_add(1, std::memory_order_relaxed);
    }, minBatch); // synchronous fork-join (slice 5)
    auto end = std::chrono::high_resolution_clock::now();
    return {cnt.load(), (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(end-start).count()};
}

BenchResult RunParallelChunks(World& w) {
    auto start = std::chrono::high_resolution_clock::now();
    std::atomic<std::size_t> cnt{0};
    w.Query<Write<Position>, Read<Velocity>>().ParallelChunks([&](Position* pos, const Velocity* vel, std::size_t count){
        for (std::size_t i = 0; i < count; ++i) {
            pos[i].x += vel[i].x * 0.016f;
            cnt.fetch_add(1, std::memory_order_relaxed);
        }
    }); // synchronous fork-join (slice 5)
    auto end = std::chrono::high_resolution_clock::now();
    return {cnt.load(), (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(end-start).count()};
}
}

// Simple micro-benchmarks; assertions keep execution bounded and validate work
TEST(QueryBenchmarks, ParallelVsSequential_VaryingBatchSizes) {
    JobSystem::WorkStealingThreadPool js; // defaults to hw_concurrency
    World w(&js);

    // Populate data
    const int N = 8000;
    {
        // Ensure all entity creation is completed in this thread to avoid TLS command buffers lingering
        for (int i = 0; i < N; ++i) {
            auto e = w.Create();
            e.Set(Position{float(i), float(i), float(i)});
            e.Set(Velocity{1,2,3});
        }
        w.ProcessCommands();
    // Use median timing to reduce noise
    auto seqMedian = MeasureMedianMicros(1, 3, [&]{ (void)RunEach(w); });
    Logger::Log::Info("[Bench] Each median={}us", seqMedian);

    }

    // Baseline sequential
    auto seq = RunEach(w);
    Logger::Log::Info("[Bench] Each: count={}, time={}us", seq.count, seq.micros);
    EXPECT_EQ(seq.count, static_cast<std::size_t>(N));

    // Median timings for parallel variants (light sampling)
    for (std::size_t batch : {128ull, 512ull}) {
        auto medianPar = MeasureMedianMicros(0, 3, [&]{ w.Query<Write<Position>, Read<Velocity>>().Parallel([](EntityHandle, Position& p, const Velocity& v){ p.x += v.x * 0.016f; }); });
        Logger::Log::Info("[Bench] Parallel(minBatch={}): median={}us", batch, medianPar);
    }

    // Parallel with varying minBatchSize
    for (std::size_t batch : {128ull, 512ull}) {
        auto par = RunParallel(w, batch);
        Logger::Log::Info("[Bench] Parallel(minBatch={}): count={}, time={}us", batch, par.count, par.micros);
        EXPECT_EQ(par.count, static_cast<std::size_t>(N));
    }
    // Optional diagnostics if enabled
    #ifdef ECS_ADAPTER_DIAGNOSTICS
    Detail::ChunkAdapterDiag::Reset();
    #endif


    // Chunk-parallel benchmark
    auto chunk = RunParallelChunks(w);
    Logger::Log::Info("[Bench] ParallelChunks: count={}, time={}us", chunk.count, chunk.micros);
    EXPECT_EQ(chunk.count, static_cast<std::size_t>(N));

}

TEST(QueryBenchmarks, ForEachChunk_AdapterOverhead) {
    JobSystem::WorkStealingThreadPool js; // hw threads
    World w(&js);

    // Populate a larger set to get meaningful chunk loops
    const int N = 64000;
    for (int i = 0; i < N; ++i) {
        auto e = w.Create();
        e.Set(Position{float(i), float(i), float(i)});
        e.Set(Velocity{1,2,3});
    }
    w.ProcessCommands();

    // Both legs run the IDENTICAL fixed-kWork body so the ratio isolates pure
    // adapter dispatch cost. The old shape compared full-count work against
    // fixed-256 work, so the ratio conflated a genuine work difference with
    // load noise: under machine load only the longer leg inflated (measured
    // in-suite ratios up to 11.9x with zero regression), while a REAL adapter
    // regression had to reach ~15x before the ratio crossed the old 5x line
    // from the no-count side. Identical bodies center the quiet ratio at ~1.
    //
    // Samples are interleaved A,B pairs (median-of-5 per leg, 1 warmup each)
    // so a load burst lands on both legs instead of skewing one.
    constexpr std::size_t kWork = 256; // < min chunk fill; avoids last-chunk overrun
    const auto runWithCount = [&]{
        w.Query<Write<Position>, Read<Velocity>>().ForEachChunk([](Position* pos, const Velocity* vel, std::size_t count){
            (void)count; // deliberately unused: identical body to the adapter leg
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        });
    };
    const auto runNoCount = [&]{
        w.Query<Write<Position>, Read<Velocity>>().ForEachChunk([](Position* pos, const Velocity* vel){
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        });
    };

    runWithCount();
    runNoCount();
    std::vector<uint64_t> samplesWith, samplesNo;
    for (int k = 0; k < 5; ++k) {
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runWithCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesWith.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runNoCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesNo.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
    }
    std::sort(samplesWith.begin(), samplesWith.end());
    std::sort(samplesNo.begin(), samplesNo.end());
    const uint64_t tCount = samplesWith[samplesWith.size() / 2];
    const uint64_t tNoCount = samplesNo[samplesNo.size() / 2];

    Logger::Log::Info("[Bench] ForEachChunk explicit-count={}us, no-count={}us (medians of 5)", tCount, tNoCount);
    std::printf("[Bench] ForEachChunk explicit-count=%lluus, no-count=%lluus (medians of 5)\n",
                (unsigned long long)tCount, (unsigned long long)tNoCount);

    // Threshold 3x, calibrated 2026-07-19 (#634 method). Measured median-ratio
    // distributions of this identical-body shape: quiet n=25 p50=1.03
    // max=1.24; under full-core spin load (NUMBER_OF_PROCESSORS PowerShell
    // sqrt-spinners) n=30 p50=1.07 p90=1.32 max=1.58. 3x = 1.9x headroom over
    // the loaded max, and trips on any adapter dispatch path >=3x slower than
    // the explicit-count call — the regression class this gate exists for
    // (verified by pessimizing the adapter locally: ratio >>3, gate fails).
    EXPECT_LE(std::max(tCount, tNoCount), std::max<uint64_t>(1, std::min(tCount, tNoCount)) * 3)
        << "explicit-count=" << tCount << "us no-count=" << tNoCount << "us";
}

TEST(QueryBenchmarks, ForEachChunk_AdapterOverhead_WithOptional) {
    JobSystem::WorkStealingThreadPool js; // hw threads
    World w(&js);

    const int N = 64000;
    for (int i = 0; i < N; ++i) {
        auto e = w.Create();
        e.Set(Position{float(i), float(i), float(i)});
        if ((i & 1) == 0) e.Set(Velocity{1,2,3}); // half have Velocity
    }
    w.ProcessCommands();

        constexpr std::size_t kWork = 256;
    const auto runWithCount = [&]{
        w.Query<Write<Position>, Optional<Velocity>>().ForEachChunk([](Position* pos, const Velocity* vel, std::size_t count){
            (void)count; // deliberately unused: identical body to the adapter leg
            if (!vel) return;
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        });
    };
    const auto runNoCount = [&]{
        w.Query<Write<Position>, Optional<Velocity>>().ForEachChunk([](Position* pos, const Velocity* vel){
            if (!vel) return;
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        });
    };

    runWithCount();
    runNoCount();
    std::vector<uint64_t> samplesWith, samplesNo;
    for (int k = 0; k < 5; ++k) {
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runWithCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesWith.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runNoCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesNo.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
    }
    std::sort(samplesWith.begin(), samplesWith.end());
    std::sort(samplesNo.begin(), samplesNo.end());
    const uint64_t tCount = samplesWith[samplesWith.size() / 2];
    const uint64_t tNoCount = samplesNo[samplesNo.size() / 2];

    Logger::Log::Info("[Bench] ForEachChunk(Optional) explicit-count={}us, no-count={}us (medians of 5)", tCount, tNoCount);
    std::printf("[Bench] ForEachChunk(Optional) explicit-count=%lluus, no-count=%lluus (medians of 5)\n",
                (unsigned long long)tCount, (unsigned long long)tNoCount);

    // Identical-body + interleaved-median shape shared with
    // ForEachChunk_AdapterOverhead; threshold 3x per the same 2026-07-19
    // calibration method (the old single-sample 8x/10x lines were quiet-box
    // calibrations and still flaked under full-core load).
    EXPECT_LE(std::max(tCount, tNoCount), std::max<uint64_t>(1, std::min(tCount, tNoCount)) * 3)
        << "explicit-count=" << tCount << "us no-count=" << tNoCount << "us";
}






TEST(QueryBenchmarks, ParallelChunks_AdapterOverhead_WithOptional) {
    JobSystem::WorkStealingThreadPool js; // hw threads
    World w(&js);

    const int N = 64000;
    for (int i = 0; i < N; ++i) {
        auto e = w.Create();
        e.Set(Position{float(i), float(i), float(i)});
        if ((i & 1) == 0) e.Set(Velocity{1,2,3});
    }
    w.ProcessCommands();

    uint64_t tCount = 0, tNoCount = 0;

    {
        auto t0 = std::chrono::high_resolution_clock::now();
        w.Query<Write<Position>, Optional<Velocity>>().ParallelChunks([](Position* pos, const Velocity* vel, std::size_t count){
            if (!vel) return;
            for (std::size_t i = 0; i < count; ++i) pos[i].x += vel[i].x * 0.0001f;
        }); // synchronous fork-join (slice 5)
        auto t1 = std::chrono::high_resolution_clock::now();
        tCount = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    }

    {
        auto t0 = std::chrono::high_resolution_clock::now();
        w.Query<Write<Position>, Optional<Velocity>>().ParallelChunks([](Position* pos, const Velocity* vel){
            if (!vel) return;
            constexpr std::size_t kWork = 256;
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        }); // synchronous fork-join (slice 5)
        auto t1 = std::chrono::high_resolution_clock::now();
        tNoCount = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    }

    Logger::Log::Info("[Bench] ParallelChunks(Optional) explicit-count={}us, no-count={}us", tCount, tNoCount);
    EXPECT_LE(std::max(tCount, tNoCount), std::max<uint64_t>(1, std::min(tCount, tNoCount)) * 5);
}

TEST(QueryBenchmarks, ParallelChunks_AdapterOverhead) {
    JobSystem::WorkStealingThreadPool js; // hw threads
    World w(&js);

    // Populate data
    const int N = 64000;
    for (int i = 0; i < N; ++i) {
        auto e = w.Create();
        e.Set(Position{float(i), float(i), float(i)});
        e.Set(Velocity{1,2,3});
    }
    w.ProcessCommands();

    // Identical fixed-kWork bodies + interleaved median-of-5 pairs, same shape
    // as ForEachChunk_AdapterOverhead above and for the same reason: the old
    // single-sample full-count-vs-kWork legs conflated a genuine 2x work
    // difference with load noise (its own history notes the old line sat on
    // the measured ratio), and it flaked in loaded full-suite runs. Identical
    // bodies center the ratio at ~1 and make it a pure adapter-dispatch gate;
    // fork-join dispatch jitter is absorbed by the per-leg medians.
    constexpr std::size_t kWork = 256;
    const auto runWithCount = [&]{
        w.Query<Write<Position>, Read<Velocity>>().ParallelChunks([](Position* pos, const Velocity* vel, std::size_t count){
            (void)count; // deliberately unused: identical body to the adapter leg
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        }); // synchronous fork-join (slice 5)
    };
    const auto runNoCount = [&]{
        w.Query<Write<Position>, Read<Velocity>>().ParallelChunks([](Position* pos, const Velocity* vel){
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        }); // synchronous fork-join (slice 5)
    };

    runWithCount();
    runNoCount();
    std::vector<uint64_t> samplesWith, samplesNo;
    for (int k = 0; k < 5; ++k) {
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runWithCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesWith.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runNoCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesNo.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
    }
    std::sort(samplesWith.begin(), samplesWith.end());
    std::sort(samplesNo.begin(), samplesNo.end());
    const uint64_t tCount = samplesWith[samplesWith.size() / 2];
    const uint64_t tNoCount = samplesNo[samplesNo.size() / 2];

    Logger::Log::Info("[Bench] ParallelChunks explicit-count={}us, no-count={}us (medians of 5)", tCount, tNoCount);
    std::printf("[Bench] ParallelChunks explicit-count=%lluus, no-count=%lluus (medians of 5)\n",
                (unsigned long long)tCount, (unsigned long long)tNoCount);

    // Threshold 3x, calibrated 2026-07-19 (#634 method) on the identical-body
    // shape: quiet/loaded distributions in the calibration notes (loaded
    // full-core-spin max well under 2x; parallel fork-join adds jitter but the
    // medians absorb it). Trips on an adapter dispatch path >=3x slower.
    EXPECT_LE(std::max(tCount, tNoCount), std::max<uint64_t>(1, std::min(tCount, tNoCount)) * 3)
        << "explicit-count=" << tCount << "us no-count=" << tNoCount << "us";
}

// Fragmentation scenario: multiple archetypes, random distribution, churn
TEST(QueryBenchmarks, ForEachChunk_AdapterOverhead_FragmentedOptional) {
    JobSystem::WorkStealingThreadPool js; // hw threads
    World w(&js);

    const int N = 120000;
    // Create entities across three archetypes: {P}, {P,V}, {P,R}
    for (int i = 0; i < N; ++i) {
        auto e = w.Create();
        e.Set(Position{float(i), float(i), float(i)});
        if (i % 3 == 0) { e.Set(Velocity{1,2,3}); }
        else if (i % 3 == 1) { e.Set(Rotation{0,0,0,1}); }
    }
    w.ProcessCommands();

    // Churn: move some entities between archetypes to fragment chunks
    for (int i = 0; i < N; i += 10) {
        auto e = w.Create(); // temp helper for Set on existing handle is not available; simulate churn by adding/removing on new entities
        e.Set(Position{float(i), float(i), float(i)});
        if (i % 2 == 0) e.Set(Velocity{1,2,3});
    }
    w.ProcessCommands();

        constexpr std::size_t kWork = 256;
    const auto runWithCount = [&]{
        w.Query<Write<Position>, Optional<Velocity>>().ForEachChunk([](Position* pos, const Velocity* vel, std::size_t count){
            (void)count; // deliberately unused: identical body to the adapter leg
            if (!vel) return;
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        });
    };
    const auto runNoCount = [&]{
        w.Query<Write<Position>, Optional<Velocity>>().ForEachChunk([](Position* pos, const Velocity* vel){
            if (!vel) return;
            for (std::size_t i = 0; i < kWork; ++i) pos[i].x += vel[i].x * 0.0001f;
        });
    };

    runWithCount();
    runNoCount();
    std::vector<uint64_t> samplesWith, samplesNo;
    for (int k = 0; k < 5; ++k) {
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runWithCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesWith.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            runNoCount();
            auto t1 = std::chrono::high_resolution_clock::now();
            samplesNo.push_back((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
    }
    std::sort(samplesWith.begin(), samplesWith.end());
    std::sort(samplesNo.begin(), samplesNo.end());
    const uint64_t tCount = samplesWith[samplesWith.size() / 2];
    const uint64_t tNoCount = samplesNo[samplesNo.size() / 2];

    Logger::Log::Info("[Bench] Fragmented ForEachChunk(Optional) explicit-count={}us, no-count={}us (medians of 5)", tCount, tNoCount);
    std::printf("[Bench] Fragmented ForEachChunk(Optional) explicit-count=%lluus, no-count=%lluus (medians of 5)\n",
                (unsigned long long)tCount, (unsigned long long)tNoCount);

    // Identical-body + interleaved-median shape shared with
    // ForEachChunk_AdapterOverhead; threshold 3x per the same 2026-07-19
    // calibration method (the old single-sample 8x/10x lines were quiet-box
    // calibrations and still flaked under full-core load).
    EXPECT_LE(std::max(tCount, tNoCount), std::max<uint64_t>(1, std::min(tCount, tNoCount)) * 3)
        << "explicit-count=" << tCount << "us no-count=" << tNoCount << "us";
}

// The per-frame query shape — construct, match, snapshot, iterate — on a world whose
// archetype spread makes the match and the snapshot the cost, not the entity loop. The
// retained shape beside it shows what the snapshot costs. Reported, not gated: the pass
// rule (a change may not make the fresh shape slower) compares two builds.
TEST(QueryBenchmarks, FreshConstructionOnArchetypeSpread) {
    constexpr unsigned kArchetypeCount = 200;
    constexpr int kConstructionsPerSample = 200;

    World w(nullptr);
    PopulateArchetypeSpread(w, kArchetypeCount);

    std::size_t visited = 0;
    const auto visit = [&visited](EntityHandle, const Position&, Velocity&) { ++visited; };

    visited = 0;
    w.Query<Read<Position>, Write<Velocity>>().Each(visit);
    ASSERT_EQ(visited, static_cast<std::size_t>(kArchetypeCount));

    // A single construction is below the clock's microsecond resolution, so each sample
    // times a batch.
    const uint64_t freshBatch = MeasureMedianMicros(3, 15, [&]{
        for (int i = 0; i < kConstructionsPerSample; ++i)
            w.Query<Read<Position>, Write<Velocity>>().Each(visit);
    });

    Query<Read<Position>, Write<Velocity>> retained(&w);
    const uint64_t retainedBatch = MeasureMedianMicros(3, 15, [&]{
        for (int i = 0; i < kConstructionsPerSample; ++i)
            retained.Each(visit);
    });

    std::printf("[Bench] query over %u archetypes: fresh construction + Each %.3f us, retained Each %.3f us"
                " (medians of 15 samples of %d)\n",
                kArchetypeCount,
                static_cast<double>(freshBatch) / kConstructionsPerSample,
                static_cast<double>(retainedBatch) / kConstructionsPerSample,
                kConstructionsPerSample);
}
