#include <gtest/gtest.h>
#include <numeric>
#include <atomic>
#include <thread>
#include <vector>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"
#include "TestComponents.h"

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

static void Populate(World& w, int N){
    for(int i=0;i<N;++i){
        auto e=w.Create();
        e.Set(Position{float(i),float(i),float(i)});
        e.Set(Velocity{1,2,3});
        if((i&1)==0) e.Set(Health{100,100});
    }
    w.ProcessCommands();
}

TEST(ParallelPerfTest, DISABLED_ParallelWithinReasonableFactorVsSequential) {
    JobSystem::WorkStealingThreadPool js; // defaults to hardware_concurrency
    World w(&js);

    const int N=2000; // tiny for debug/CI stability
    Populate(w,N);

    uint64_t accSeq=0;
    std::atomic<uint64_t> accPar{0};

    w.Query<Read<Position>,Read<Velocity>,Optional<Health>>().Each([&](EntityHandle,const Position&p,const Velocity&v,const Health*h){
        accSeq += uint64_t(p.x)+uint64_t(v.x)+(h?1u:0u);
    });

    w.Query<Read<Position>,Read<Velocity>,Optional<Health>>().Parallel([&](EntityHandle,const Position&p,const Velocity&v,const Health*h){
        accPar.fetch_add(uint64_t(p.x)+uint64_t(v.x)+(h?1u:0u), std::memory_order_relaxed);
    }, /*minBatchSize*/ 256); // synchronous fork-join (slice 5)

    auto accParVal = accPar.load(std::memory_order_relaxed);
    EXPECT_GT(accSeq,0u);
    EXPECT_GT(accParVal,0u);
    EXPECT_LE(std::llabs((long long)accParVal-(long long)accSeq), 100000LL);
}

TEST(ParallelPerfTest, DISABLED_ParallelChunksWithinReasonableFactor) {
    JobSystem::WorkStealingThreadPool js;
    World w(&js);

    const int N=2000;
    Populate(w,N);

    std::atomic<uint64_t> acc{0};
    w.Query<Read<Position>,Read<Velocity>>().ParallelChunks([&](const Position* p, const Velocity* v, size_t count){
        uint64_t local=0;
        for(size_t i=0;i<count;++i){ local += uint64_t(p[i].x)+uint64_t(v[i].x); }
        acc.fetch_add(local, std::memory_order_relaxed);
    }); // synchronous fork-join (slice 5)

    EXPECT_GT(acc.load(std::memory_order_relaxed),0u);
}

// Slice-5 storm: the migrated wave joins (Query::Parallel now dispatches
// through a JobCounter and joins synchronously) hammered from several
// external threads at once. Each fork must observe exactly its entity count —
// a lost decrement hangs, an arbitrary-drain bug corrupts counts.
TEST(ParallelPerfTest, JobCounterWaveJoinStorm) {
    JobSystem::WorkStealingThreadPool js(4);
    World w(&js);

    const int N = 4000;
    Populate(w, N);

    constexpr int kThreads = 4;
    constexpr int kItersPerThread = 50;
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int th = 0; th < kThreads; ++th) {
        threads.emplace_back([&] {
            for (int i = 0; i < kItersPerThread; ++i) {
                std::atomic<size_t> cnt{0};
                w.Query<Read<Position>, Read<Velocity>>().Parallel(
                    [&cnt](EntityHandle, const Position&, const Velocity&) {
                        cnt.fetch_add(1, std::memory_order_relaxed);
                    },
                    /*minBatchSize*/ 256);
                if (cnt.load(std::memory_order_relaxed) != static_cast<size_t>(N))
                    failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failures.load(), 0);
}

// Slice-5 nested join: Query::Parallel called from INSIDE a pool task (the
// Scheduler::MakeJobParallel shape — a wave job running on a worker joins its
// own query fork). Deadlock-free because the worker-thread Wait participates
// (F17); pre-slice-5 this was the worker-wait deadlock class.
TEST(ParallelPerfTest, NestedParallelFromWorkerParticipates) {
    JobSystem::WorkStealingThreadPool js(2);
    World w(&js);

    const int N = 4000;
    Populate(w, N);

    std::atomic<size_t> cnt{0};
    JobSystem::JobCounter outer;
    js.Run([&] {
        w.Query<Read<Position>, Read<Velocity>>().Parallel(
            [&cnt](EntityHandle, const Position&, const Velocity&) {
                cnt.fetch_add(1, std::memory_order_relaxed);
            },
            /*minBatchSize*/ 256);
    }, outer);
    js.Wait(outer);

    EXPECT_EQ(cnt.load(std::memory_order_relaxed), static_cast<size_t>(N));
}

