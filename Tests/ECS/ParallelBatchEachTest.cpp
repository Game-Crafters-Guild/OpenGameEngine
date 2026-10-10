#include <gtest/gtest.h>

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestComponents.h"

#include <atomic>
#include <unordered_set>

using namespace GameEngine::ECS;
using GameEngine::ECS::test::Position;
using GameEngine::ECS::test::Velocity;
using GameEngine::ECS::test::Health;

TEST(ParallelBatchEachTest, VisitsEveryEntityAcrossChunks) {
    JobSystem::WorkStealingThreadPool js;
    World w(&js);
    w.RegisterComponents<Position, Velocity>();

    constexpr int N = 40'000;
    w.CreateBatchHandle<Position, Velocity>(N, Position{0, 0, 0}, Velocity{1, 1, 1});

    std::atomic<std::size_t> totalEntities{0};
    std::atomic<std::size_t> chunkCalls{0};
    w.Query<Write<Position>, Read<Velocity>>().ParallelBatchEach(
        [&](Position* pos, const Velocity* vel, std::size_t count) {
            chunkCalls.fetch_add(1, std::memory_order_relaxed);
            totalEntities.fetch_add(count, std::memory_order_relaxed);
            for (std::size_t i = 0; i < count; ++i) {
                pos[i].x += vel[i].x;
            }
        }); // synchronous fork-join (slice 5)

    EXPECT_EQ(totalEntities.load(), static_cast<std::size_t>(N));
    EXPECT_GT(chunkCalls.load(), 0u);
}

TEST(ParallelBatchEachTest, ProducesSameMutationAsBatchEach) {
    JobSystem::WorkStealingThreadPool js;
    World wSeq;
    World wPar(&js);
    wSeq.RegisterComponents<Position, Velocity>();
    wPar.RegisterComponents<Position, Velocity>();

    constexpr int N = 20'000;
    constexpr float dt = 0.016f;
    wSeq.CreateBatchHandle<Position, Velocity>(N, Position{1, 2, 3}, Velocity{4, 5, 6});
    wPar.CreateBatchHandle<Position, Velocity>(N, Position{1, 2, 3}, Velocity{4, 5, 6});

    wSeq.Query<Write<Position>, Read<Velocity>>().BatchEach(
        [dt](Position* pos, const Velocity* vel, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                pos[i].x += vel[i].x * dt;
                pos[i].y += vel[i].y * dt;
                pos[i].z += vel[i].z * dt;
            }
        });

    wPar.Query<Write<Position>, Read<Velocity>>().ParallelBatchEach(
        [dt](Position* pos, const Velocity* vel, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                pos[i].x += vel[i].x * dt;
                pos[i].y += vel[i].y * dt;
                pos[i].z += vel[i].z * dt;
            }
        }); // synchronous fork-join (slice 5)

    double sumSeq = 0, sumPar = 0;
    wSeq.Query<Read<Position>>().Each([&](const Position& p) { sumSeq += p.x + p.y + p.z; });
    wPar.Query<Read<Position>>().Each([&](const Position& p) { sumPar += p.x + p.y + p.z; });
    EXPECT_NEAR(sumSeq, sumPar, sumSeq * 1e-5);
}

TEST(ParallelBatchEachTest, FallsBackToSequentialWithoutJobSystem) {
    World w; // no job system
    w.RegisterComponents<Position, Velocity>();

    constexpr int N = 5'000;
    w.CreateBatchHandle<Position, Velocity>(N, Position{0, 0, 0}, Velocity{1, 0, 0});

    std::size_t totalEntities = 0;
    w.Query<Read<Position>, Read<Velocity>>().ParallelBatchEach(
        [&](const Position*, const Velocity*, std::size_t count) {
            totalEntities += count;
        }); // no-job-system path runs sequentially on the caller

    EXPECT_EQ(totalEntities, static_cast<std::size_t>(N));
}

// --- CreateBatchWithInit ---
TEST(CreateBatchWithInit, PerEntityInitValues) {
    World w;
    w.RegisterComponents<Position, Velocity>();

    constexpr size_t N = 1000;
    auto handles = w.CreateBatchWithInit<Position, Velocity>(N,
        [](size_t i, Position& p, Velocity& v) {
            p = {float(i), float(i * 2), float(i * 3)};
            v = {float(i) * 0.5f, 1.0f, 2.0f};
        });

    ASSERT_EQ(handles.size(), N);
    for (size_t i = 0; i < N; ++i) {
        auto* p = w.GetComponent<Position>(handles[i]);
        auto* v = w.GetComponent<Velocity>(handles[i]);
        ASSERT_NE(p, nullptr);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(p->x, float(i));
        EXPECT_EQ(p->y, float(i * 2));
        EXPECT_EQ(p->z, float(i * 3));
        EXPECT_EQ(v->x, float(i) * 0.5f);
        EXPECT_EQ(v->y, 1.0f);
        EXPECT_EQ(v->z, 2.0f);
    }
}

TEST(CreateBatchWithInit, ZeroCountReturnsEmpty) {
    World w;
    w.RegisterComponents<Position>();
    auto handles = w.CreateBatchWithInit<Position>(0,
        [](size_t, Position&) { FAIL() << "InitFn should not be called for zero-count batch"; });
    EXPECT_TRUE(handles.empty());
}

TEST(CreateBatchWithInit, RollsBackOnInitThrow) {
    World w;
    w.RegisterComponents<Position, Velocity>();

    // Baseline: one entity exists before the batch. World state after a
    // throwing CreateBatchWithInit must match this baseline exactly.
    auto preExisting = w.CreateHandle(Position{99, 0, 0}, Velocity{});
    const size_t baselineCount = w.GetEntityCount();

    auto throwing = [&]() {
        w.CreateBatchWithInit<Position, Velocity>(100,
            [](size_t i, Position& p, Velocity&) {
                if (i == 50) throw std::runtime_error{"boom"};
                p.x = float(i);
            });
    };
    EXPECT_THROW(throwing(), std::runtime_error);

    // Entity count is back to baseline — all partial creations rolled back.
    EXPECT_EQ(w.GetEntityCount(), baselineCount);

    // Pre-existing entity still valid with original data.
    auto* preExistingPos = w.GetComponent<Position>(preExisting);
    ASSERT_NE(preExistingPos, nullptr);
    EXPECT_EQ(preExistingPos->x, 99.0f);

    // World is usable — fresh creates work and fit in the space we just vacated.
    auto after = w.CreateHandle(Position{7, 7, 7}, Velocity{});
    EXPECT_TRUE(after.IsValid());
    auto* afterPos = w.GetComponent<Position>(after);
    ASSERT_NE(afterPos, nullptr);
    EXPECT_EQ(afterPos->x, 7.0f);
}

TEST(CreateBatchWithInit, InitFnCanCaptureExternalState) {
    World w;
    w.RegisterComponents<Position>();

    std::vector<Position> presets = {{1, 0, 0}, {0, 2, 0}, {0, 0, 3}};
    auto handles = w.CreateBatchWithInit<Position>(presets.size(),
        [&presets](size_t i, Position& p) { p = presets[i]; });

    ASSERT_EQ(handles.size(), presets.size());
    for (size_t i = 0; i < presets.size(); ++i) {
        auto* p = w.GetComponent<Position>(handles[i]);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->x, presets[i].x);
        EXPECT_EQ(p->y, presets[i].y);
        EXPECT_EQ(p->z, presets[i].z);
    }
}

TEST(ParallelBatchEachTest, FragmentedArchetypesAllCovered) {
    JobSystem::WorkStealingThreadPool js;
    World w(&js);
    w.RegisterComponents<Position, Velocity, Health>();

    constexpr int perArchetype = 10'000;
    w.CreateBatchHandle<Position, Velocity>(perArchetype, Position{}, Velocity{1, 0, 0});
    w.CreateBatchHandle<Position, Velocity, Health>(perArchetype, Position{}, Velocity{2, 0, 0}, Health{100, 100});

    std::atomic<std::size_t> total{0};
    w.Query<Read<Position>, Read<Velocity>>().ParallelBatchEach(
        [&](const Position*, const Velocity*, std::size_t count) {
            total.fetch_add(count, std::memory_order_relaxed);
        }); // synchronous fork-join (slice 5)

    EXPECT_EQ(total.load(), static_cast<std::size_t>(2 * perArchetype));
}
