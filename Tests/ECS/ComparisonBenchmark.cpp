#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/CachedQuery.h"
#include "ECS/ECSTemplates.h" // CreateBatchWithInit definition (C5046 with lambda init otherwise)
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestComponents.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

// ============================================================================
// ECS Comparison Benchmarks
//
// Patterns match https://github.com/abeimler/ecs_benchmark
// Reference numbers are from abeimler on Linux/GCC, ~1M entities:
//   - "2 systems update": EnTT 12ms, Flecs 12ms, pico_ecs 12ms, mustache 8ms
//   - "7 systems mixed":  Flecs 19ms, pico_ecs 25ms, EnTT 40ms
//   - "262K create+2comp": pico_ecs 6ms, EnTT 10ms, Flecs 109ms
//   - "262K destroy":      pico_ecs 2ms, EnTT 10ms, Flecs 17ms
//
// Hardware differences exist; what matters is relative standing.
// ============================================================================

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;
using Clock = std::chrono::high_resolution_clock;

static double ToMs(std::chrono::nanoseconds ns) {
    return static_cast<double>(ns.count()) / 1'000'000.0;
}

// ============================================================================
// Test components matching abeimler's benchmark suite
// ============================================================================

// abeimler uses a DataComponent with RNG state — we replicate that
struct DataComponent {
    uint32_t seed = 42;
    int32_t thingy = 0;
    float truthy = 1.0f;
    bool toggle = false;

    // Cheap PRNG matching the benchmark pattern
    uint32_t rng() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    }
};

struct SpriteComponent {
    char character = '.';
    uint32_t layer = 0;
};

struct HealthComponent {
    float hp = 100.0f;
    float maxHp = 100.0f;
    bool alive = true;
};

struct DamageComponent {
    float amount = 10.0f;
    bool applied = false;
};

// ============================================================================
// Benchmark 1: Entity Creation
// ============================================================================

TEST(ECSBenchmark, EntityCreation_262K) {
    constexpr size_t N = 262'144;

    // Individual creation (matches typical benchmark pattern)
    {
        WorldConfig cfg;
        cfg.ExpectedEntityCount = N; // Pre-allocate metadata (same as other benchmarks do)
        World w(cfg, nullptr);
        w.RegisterComponents<Position, Velocity>();

        auto start = Clock::now();
        for (size_t i = 0; i < N; ++i) {
            w.CreateHandle(
                Position{static_cast<float>(i), 0.0f, 0.0f},
                Velocity{1.0f, 1.0f, 1.0f}
            );
        }
        auto end = Clock::now();
        printf("\n  [BENCH] Create Individual 262K (2 comp): %7.2f ms\n", ToMs(end - start));
        EXPECT_EQ(w.GetEntityCount(), N);
    }

    // Batch creation (our optimized path)
    {
        WorldConfig cfg;
        cfg.ExpectedEntityCount = N;
        World w(cfg, nullptr);
        w.RegisterComponents<Position, Velocity>();

        auto start = Clock::now();
        w.CreateBatchHandle<Position, Velocity>(N, Position{0.f, 0.f, 0.f}, Velocity{1.f, 1.f, 1.f});
        auto end = Clock::now();
        printf("  [BENCH] Create Batch 262K (2 comp):      %7.2f ms\n", ToMs(end - start));
        printf("  [REF]   pico_ecs 6ms, EnTT 10ms, Flecs 109ms\n");
        EXPECT_EQ(w.GetEntityCount(), N);
    }
}

// ============================================================================
// Benchmark 2: Entity Destruction
// ============================================================================

TEST(ECSBenchmark, EntityDestruction_262K) {
    constexpr size_t N = 262'144;

    World w(nullptr);
    w.RegisterComponents<Position, Velocity>();
    auto handles = w.CreateBatchHandle<Position, Velocity>(N, Position{}, Velocity{});

    auto start = Clock::now();
    for (auto h : handles) {
        w.DestroyEntityImmediate(h);
    }
    auto end = Clock::now();

    printf("\n  [BENCH] Destroy 262K:                    %7.2f ms\n", ToMs(end - start));
    printf("  [REF]   pico_ecs 2ms, EnTT 10ms, Flecs 17ms\n");
    EXPECT_EQ(w.GetEntityCount(), 0u);
}

// ============================================================================
// Benchmark 3: Simple Movement System (1 system, 2 components)
// The baseline iteration test: p += v * dt
// ============================================================================

TEST(ECSBenchmark, MovementSystem_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;

    World w(nullptr);
    w.RegisterComponents<Position, Velocity>();
    w.CreateBatchHandle<Position, Velocity>(N, Position{0.f, 0.f, 0.f}, Velocity{1.f, 2.f, 3.f});

    auto q = w.Query<Position, Velocity>();

    // Warm up
    q.Each([dt](EntityHandle, Position& p, Velocity& v) {
        p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
    });

    constexpr int RUNS = 10;
    double totalMs = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        auto start = Clock::now();
        q.Each([dt](EntityHandle, Position& p, Velocity& v) {
            p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
        });
        totalMs += ToMs(Clock::now() - start);
    }

    // Verify work was done
    auto firstEntity = w.GetAllArchetypes()[0]->CollectEntities()[0];
    auto* pos = w.GetComponent<Position>(firstEntity);
    ASSERT_NE(pos, nullptr);
    EXPECT_GT(pos->x, 0.0f) << "Iteration work was optimized away";

    printf("\n  [BENCH] Movement System 1M (p += v*dt):  %7.2f ms avg (%d runs)\n", totalMs / RUNS, RUNS);
}

// ============================================================================
// Benchmark 3b: Batch Movement via BatchEach
// Compares: Each (per-entity callback) vs BatchEach (raw pointer batch loop).
// Position(12B) and Velocity(12B) are flat float arrays in the colocated
// chunk, so the batch path is a straight-line loop over contiguous memory.
// ============================================================================

// Verify test components have no padding (batch loops assume contiguous floats).
static_assert(sizeof(Position) == 3 * sizeof(float), "Position must be 3 contiguous floats");
static_assert(sizeof(Velocity) == 3 * sizeof(float), "Velocity must be 3 contiguous floats");

TEST(ECSBenchmark, MovementSystem_Batch_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;

    World w(nullptr);
    w.RegisterComponents<Position, Velocity>();
    w.CreateBatchHandle<Position, Velocity>(N, Position{0.f, 0.f, 0.f}, Velocity{1.f, 2.f, 3.f});

    auto q = w.Query<Position, Velocity>();

    auto timeEach = [&]() {
        auto start = Clock::now();
        q.Each([dt](EntityHandle, Position& p, Velocity& v) {
            p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
        });
        return ToMs(Clock::now() - start);
    };
    auto timeEachNoEH = [&]() {
        auto start = Clock::now();
        q.Each([dt](Position& p, Velocity& v) {
            p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
        });
        return ToMs(Clock::now() - start);
    };
    auto timeBatchScalar = [&]() {
        auto start = Clock::now();
        q.BatchEach([dt](Position* pos, Velocity* vel, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                pos[i].x += vel[i].x * dt;
                pos[i].y += vel[i].y * dt;
                pos[i].z += vel[i].z * dt;
            }
        });
        return ToMs(Clock::now() - start);
    };
    // Warm all paths before sampling so caches/TLB state are settled
    // uniformly. Warming only one biases the others relative to it.
    for (int i = 0; i < 3; ++i) {
        (void)timeEach(); (void)timeEachNoEH();
        (void)timeBatchScalar();
    }

    constexpr int RUNS = 15;
    std::vector<double> eachS, eachNoEHS, batchScalarS;
    eachS.reserve(RUNS); eachNoEHS.reserve(RUNS);
    batchScalarS.reserve(RUNS);

    // Interleave the three paths; rotate the starting path so no single path
    // consistently runs first/last on a given iteration's cache state.
    for (int r = 0; r < RUNS; ++r) {
        switch (r % 3) {
            case 0:
                eachS.push_back(timeEach());
                eachNoEHS.push_back(timeEachNoEH());
                batchScalarS.push_back(timeBatchScalar());
                break;
            case 1:
                eachNoEHS.push_back(timeEachNoEH());
                batchScalarS.push_back(timeBatchScalar());
                eachS.push_back(timeEach());
                break;
            default:
                batchScalarS.push_back(timeBatchScalar());
                eachS.push_back(timeEach());
                eachNoEHS.push_back(timeEachNoEH());
                break;
        }
    }

    auto median = [](std::vector<double>& s) {
        std::sort(s.begin(), s.end());
        return s[s.size() / 2];
    };
    auto minv = [](const std::vector<double>& s) { return *std::min_element(s.begin(), s.end()); };

    const double eachMed = median(eachS),       eachMin = minv(eachS);
    const double noEHMed = median(eachNoEHS),   noEHMin = minv(eachNoEHS);
    const double scMed   = median(batchScalarS), scMin  = minv(batchScalarS);

    // Verify work wasn't optimized away
    auto firstEntity = w.GetAllArchetypes()[0]->CollectEntities()[0];
    auto* pos = w.GetComponent<Position>(firstEntity);
    ASSERT_NE(pos, nullptr);
    EXPECT_GT(pos->x, 0.0f) << "Iteration work was optimized away";

    printf("\n  [BENCH] Movement 1M Each (w/ EntityHandle): median=%5.3f ms  min=%5.3f ms\n", eachMed, eachMin);
    printf("  [BENCH] Movement 1M Each (no EntityHandle): median=%5.3f ms  min=%5.3f ms\n", noEHMed, noEHMin);
    printf("  [BENCH] Movement 1M BatchEach:              median=%5.3f ms  min=%5.3f ms\n", scMed, scMin);
    printf("  [BENCH] Speedup (Each median / BatchEach median): %.2fx\n", eachMed / scMed);
}

// ============================================================================
// Benchmark 4: Two Systems (Movement + Data) — matches abeimler "2 systems"
// This is the fair comparison to the ~12ms reference numbers.
// ============================================================================

TEST(ECSBenchmark, TwoSystems_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;

    World w(nullptr);
    w.RegisterComponents<Position, Velocity, DataComponent>();

    // Create entities with 3 components
    {
        auto handles = w.CreateBatchHandle<Position, Velocity, DataComponent>(
            N, Position{0.f, 0.f, 0.f}, Velocity{1.f, 2.f, 3.f}, DataComponent{42, 0, 1.0f, false});
    }

    auto movementQuery = w.Query<Position, Velocity>();
    auto dataQuery = w.Query<DataComponent>();

    // Movement system
    auto movementSystem = [dt](EntityHandle, Position& p, Velocity& v) {
        p.x += v.x * dt;
        p.y += v.y * dt;
        p.z += v.z * dt;
    };

    // Data system (matches abeimler pattern: RNG, modulus, toggle)
    auto dataSystem = [](EntityHandle, DataComponent& d) {
        d.thingy = static_cast<int32_t>(d.rng() % 100);
        d.truthy = static_cast<float>(d.thingy) + 0.5f;
        d.toggle = !d.toggle;
    };

    // Warm up
    movementQuery.Each(movementSystem);
    dataQuery.Each(dataSystem);

    constexpr int RUNS = 10;
    double totalMs = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        auto start = Clock::now();
        movementQuery.Each(movementSystem);
        dataQuery.Each(dataSystem);
        totalMs += ToMs(Clock::now() - start);
    }

    printf("\n  [BENCH] Two Systems 1M (move + data):    %7.2f ms avg (%d runs)\n", totalMs / RUNS, RUNS);
    printf("  [REF]   mustache 8ms, EnTT/Flecs/pico 12ms (abeimler, different hw)\n");
}

// ============================================================================
// Benchmark 5: Fragmented Iteration (multiple archetypes)
// Creates entities with different component combos to fragment archetypes.
// Tests the query matching + multi-archetype iteration overhead.
// ============================================================================

TEST(ECSBenchmark, FragmentedIteration_100K) {
    constexpr size_t N = 100'000;

    World w(nullptr);
    w.RegisterComponents<Position, Velocity, Health, DataComponent, SpriteComponent>();

    // Create 5 different archetypes with overlapping components
    size_t perArchetype = N / 5;

    // Archetype 1: Position + Velocity (moving entities)
    w.CreateBatchHandle<Position, Velocity>(
        perArchetype, Position{0.f, 0.f, 0.f}, Velocity{1.f, 0.f, 0.f});

    // Archetype 2: Position + Velocity + Health (characters)
    w.CreateBatchHandle<Position, Velocity, Health>(
        perArchetype, Position{0.f, 0.f, 0.f}, Velocity{0.f, 1.f, 0.f}, Health{100});

    // Archetype 3: Position + Health (static damageable)
    w.CreateBatchHandle<Position, Health>(
        perArchetype, Position{0.f, 0.f, 0.f}, Health{50});

    // Archetype 4: Position + Velocity + DataComponent (data-driven)
    w.CreateBatchHandle<Position, Velocity, DataComponent>(
        perArchetype, Position{0.f, 0.f, 0.f}, Velocity{0.f, 0.f, 1.f}, DataComponent{});

    // Archetype 5: Position + SpriteComponent (renderable)
    w.CreateBatchHandle<Position, SpriteComponent>(
        perArchetype, Position{0.f, 0.f, 0.f}, SpriteComponent{'@', 0});

    EXPECT_EQ(w.GetEntityCount(), N);
    EXPECT_GE(w.GetArchetypeCount(), 5u);

    // Query Position (matches all 5 archetypes)
    auto posQuery = w.Query<Position>();

    // Query Position + Velocity (matches archetypes 1, 2, 4)
    auto moveQuery = w.Query<Position, Velocity>();

    constexpr int RUNS = 10;
    double posMs = 0.0, moveMs = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        {
            auto start = Clock::now();
            posQuery.Each([](EntityHandle, Position& p) { p.x += 1.0f; });
            posMs += ToMs(Clock::now() - start);
        }
        {
            auto start = Clock::now();
            moveQuery.Each([](EntityHandle, Position& p, Velocity& v) {
                p.x += v.x; p.y += v.y; p.z += v.z;
            });
            moveMs += ToMs(Clock::now() - start);
        }
    }

    printf("\n  [BENCH] Fragmented Pos query 100K (5 arch): %7.2f ms avg\n", posMs / RUNS);
    printf("  [BENCH] Fragmented Move query 60K (3 arch): %7.2f ms avg\n", moveMs / RUNS);
}

// ============================================================================
// Benchmark 6: Component Add/Remove (structural changes)
// ============================================================================

TEST(ECSBenchmark, StructuralChanges_10K) {
    constexpr size_t N = 10'000;

    World w(nullptr);
    w.RegisterComponents<Position, Velocity, Health>();
    auto handles = w.CreateBatchHandle<Position>(N, Position{0.f, 0.f, 0.f});

    // Add component (archetype migration)
    auto startAdd = Clock::now();
    for (auto h : handles) w.AddComponentImmediate(h, Velocity{1.f, 0.f, 0.f});
    auto addMs = ToMs(Clock::now() - startAdd);

    // Remove component (archetype migration)
    auto startRem = Clock::now();
    for (auto h : handles) w.RemoveComponentImmediate<Velocity>(h);
    auto remMs = ToMs(Clock::now() - startRem);

    printf("\n  [BENCH] Add component 10K:               %7.2f ms (%.1f us/entity)\n", addMs, addMs * 1000.0 / N);
    printf("  [BENCH] Remove component 10K:            %7.2f ms (%.1f us/entity)\n", remMs, remMs * 1000.0 / N);
}

// ============================================================================
// Benchmark 7: Parallel Iteration
// ============================================================================

TEST(ECSBenchmark, ParallelMovement_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;

    JobSystem::WorkStealingThreadPool jobSystem;
    World w(&jobSystem);
    w.RegisterComponents<Position, Velocity>();
    w.CreateBatchHandle<Position, Velocity>(N, Position{0.f, 0.f, 0.f}, Velocity{1.f, 2.f, 3.f});

    auto q = w.Query<Position, Velocity>();

    // Warm up (synchronous fork-join since slice 5)
    q.Parallel([dt](EntityHandle, Position& p, Velocity& v) {
        p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
    }, 10000);

    constexpr int RUNS = 10;
    double totalMs = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        auto start = Clock::now();
        q.Parallel([dt](EntityHandle, Position& p, Velocity& v) {
            p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
        }, 10000);
        totalMs += ToMs(Clock::now() - start);
    }

    unsigned int cores = std::thread::hardware_concurrency();
    printf("\n  [BENCH] Parallel Movement 1M (%u cores):  %7.2f ms avg\n", cores, totalMs / RUNS);
}

// ============================================================================
// Benchmark 7b: ParallelBatchEach — the fastest iteration path. Combines
// Parallel's chunk batching with BatchEach's raw-pointer callback. This
// workload is memory-bound (see bandwidth print), so a straight scalar loop
// saturates it.
// ============================================================================

TEST(ECSBenchmark, ParallelBatchEach_Movement_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;

    JobSystem::WorkStealingThreadPool jobSystem;
    World w(&jobSystem);
    w.RegisterComponents<Position, Velocity>();
    w.CreateBatchHandle<Position, Velocity>(N, Position{0.f, 0.f, 0.f}, Velocity{1.f, 2.f, 3.f});

    auto q = w.Query<Position, Velocity>();

    auto timeScalar = [&]() {
        auto start = Clock::now();
        q.ParallelBatchEach([dt](Position* pos, Velocity* vel, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                pos[i].x += vel[i].x * dt;
                pos[i].y += vel[i].y * dt;
                pos[i].z += vel[i].z * dt;
            }
        }, 10000);
        return ToMs(Clock::now() - start);
    };

    // Warm up before timing — ensures cache, TLB, and page faults are settled.
    for (int i = 0; i < 3; ++i) {
        (void)timeScalar();
    }

    constexpr int RUNS = 15;
    std::vector<double> scalarSamples;
    scalarSamples.reserve(RUNS);

    for (int r = 0; r < RUNS; ++r) {
        scalarSamples.push_back(timeScalar());
    }

    std::sort(scalarSamples.begin(), scalarSamples.end());
    const double scalarMedian = scalarSamples[RUNS / 2];
    const double scalarMin = scalarSamples.front();

    // Verify work happened
    auto firstEntity = w.GetAllArchetypes()[0]->CollectEntities()[0];
    auto* pos = w.GetComponent<Position>(firstEntity);
    ASSERT_NE(pos, nullptr);
    EXPECT_GT(pos->x, 0.0f) << "Iteration work was optimized away";

    unsigned int cores = std::thread::hardware_concurrency();
    printf("\n  [BENCH] ParallelBatchEach 1M (%u cores): median=%5.3f ms  min=%5.3f ms\n", cores, scalarMedian, scalarMin);
    // Memory bandwidth floor check: 1M entities × 36B (read P+V, write P) ≈ 36MB per pass.
    // At 0.4ms that's ~90 GB/s — L3 bandwidth territory: memory-bound.
    const double bytesPerPass = static_cast<double>(N) * 36.0;
    printf("  [BENCH] Effective bandwidth: %5.1f GB/s\n", bytesPerPass / (scalarMin * 1e6));
}

// ============================================================================
// Batch-size sweep for ParallelBatchEach. Informs whether the default
// minBatchSize=1000 is appropriate for SIMD/memory-bound workloads, or
// whether a separate `MinBatchSizeDefaultBatch` knob is justified.
// ============================================================================

TEST(ECSBenchmark, ParallelBatchEach_BatchSizeSweep_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;
    JobSystem::WorkStealingThreadPool js;
    World w(&js);
    w.RegisterComponents<Position, Velocity>();
    w.CreateBatchHandle<Position, Velocity>(N, Position{0.f, 0.f, 0.f}, Velocity{1.f, 2.f, 3.f});

    auto q = w.Query<Position, Velocity>();

    auto timeAt = [&](size_t minBatch) {
        auto start = Clock::now();
        q.ParallelBatchEach(
            [dt](Position* p, Velocity* v, size_t count) {
                for (size_t i = 0; i < count; ++i) {
                    p[i].x += v[i].x * dt;
                    p[i].y += v[i].y * dt;
                    p[i].z += v[i].z * dt;
                }
            },
            minBatch);
        return ToMs(Clock::now() - start);
    };

    const size_t sizes[] = {500, 1000, 2000, 5000, 10000, 25000, 50000, 100000};
    constexpr int kRuns = 11;
    constexpr int kWarmups = 3;

    // Warm every size to equalize initial thermal/cache state across the sweep.
    for (int i = 0; i < kWarmups; ++i)
        for (size_t s : sizes) (void)timeAt(s);

    std::vector<std::vector<double>> samples(std::size(sizes));
    for (auto& v : samples) v.reserve(kRuns);

    // Interleave across sizes — rotate start each iteration so no size is always first.
    for (int r = 0; r < kRuns; ++r) {
        const size_t n = std::size(sizes);
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = (r + i) % n;
            samples[idx].push_back(timeAt(sizes[idx]));
        }
    }

    printf("\n  [BENCH] ParallelBatchEach batch-size sweep (1M entities, SIMD, 32 cores)\n");
    double bestMed = 1e9;
    size_t bestSize = 0;
    for (size_t i = 0; i < std::size(sizes); ++i) {
        auto& v = samples[i];
        std::sort(v.begin(), v.end());
        double med = v[kRuns / 2];
        double minv = v.front();
        printf("  [BENCH]   minBatch=%6zu  median=%5.3f ms  min=%5.3f ms\n", sizes[i], med, minv);
        if (med < bestMed) { bestMed = med; bestSize = sizes[i]; }
    }
    printf("  [BENCH] Best median at minBatch=%zu (%.3f ms)\n", bestSize, bestMed);
}

// ============================================================================
// Benchmark 8: Singleton access
// ============================================================================

struct GameConfig {
    float Gravity = 9.81f;
    float TimeScale = 1.0f;
    uint32_t MaxEntities = 100000;
};

TEST(ECSBenchmark, SingletonAccess) {
    World w(nullptr);
    w.SetSingleton(GameConfig{9.81f, 1.0f, 100000});

    constexpr int RUNS = 1'000'000;
    auto start = Clock::now();
    float sum = 0.0f;
    for (int i = 0; i < RUNS; ++i) {
        auto* cfg = w.GetSingleton<GameConfig>();
        sum += cfg->Gravity;
    }
    auto ms = ToMs(Clock::now() - start);

    EXPECT_GT(sum, 0.0f); // Prevent optimization
    printf("\n  [BENCH] Singleton Get (1M calls):        %7.2f ms (%.0f ns/call)\n",
           ms, ms * 1'000'000.0 / RUNS);
}

// ============================================================================
// abeimler parity benchmarks — fill gaps vs https://github.com/abeimler/ecs_benchmark
//   #2  mixed-composition 2 systems (fragmented across archetypes)
//   #3  7 systems update
//   #4  mixed-composition 7 systems
//   #7  unpack 1 component (pure read)
//   #8  unpack 2 components
//   #9  unpack 3 components
//   #10 remove+add component
// Also adds create/destroy at 1M to align the scale with abeimler reports.
// ============================================================================

// Helper: build a scene of N entities split across 5 archetype shapes that
// overlap on Position+Velocity (so iteration crosses multiple archetypes).
static void BuildMixedScene(World& w, size_t N) {
    const size_t per = N / 5;
    w.CreateBatchHandle<Position, Velocity>(per, Position{0,0,0}, Velocity{1,2,3});
    w.CreateBatchHandle<Position, Velocity, HealthComponent>(per, Position{0,0,0}, Velocity{1,2,3}, HealthComponent{100.f, 100.f, true});
    w.CreateBatchHandle<Position, Velocity, DataComponent>(per, Position{0,0,0}, Velocity{1,2,3}, DataComponent{});
    w.CreateBatchHandle<Position, Velocity, SpriteComponent>(per, Position{0,0,0}, Velocity{1,2,3}, SpriteComponent{'x', 0});
    w.CreateBatchHandle<Position, Velocity, DamageComponent>(per, Position{0,0,0}, Velocity{1,2,3}, DamageComponent{5.f, false});
}

// ---- #2 mixed 2 systems ----
TEST(ECSBenchmark, FragmentedTwoSystems_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;
    World w(nullptr);
    w.RegisterComponents<Position, Velocity, DataComponent, HealthComponent, SpriteComponent, DamageComponent>();
    BuildMixedScene(w, N);

    auto moveQ = w.Query<Position, Velocity>();
    auto dataQ = w.Query<DataComponent>();

    auto run = [&]() {
        moveQ.Each([dt](EntityHandle, Position& p, Velocity& v) {
            p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
        });
        dataQ.Each([](EntityHandle, DataComponent& d) {
            d.thingy = static_cast<int32_t>(d.rng() % 100);
            d.truthy = static_cast<float>(d.thingy) + 0.5f;
            d.toggle = !d.toggle;
        });
    };

    for (int i = 0; i < 3; ++i) run();
    constexpr int RUNS = 10;
    std::vector<double> samples; samples.reserve(RUNS);
    for (int r = 0; r < RUNS; ++r) {
        auto t0 = Clock::now();
        run();
        samples.push_back(ToMs(Clock::now() - t0));
    }
    std::sort(samples.begin(), samples.end());
    printf("\n  [BENCH] Fragmented 2-systems 1M (5 archetypes): median=%5.2f ms  min=%5.2f ms\n",
           samples[RUNS/2], samples.front());
    printf("  [REF]   abeimler mixed-2: EnTT ~13ms, Flecs ~12ms, pico_ecs ~12ms @ 1M\n");
}

// ---- #3 seven systems (uniform archetype) ----
TEST(ECSBenchmark, SevenSystems_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;
    World w(nullptr);
    w.RegisterComponents<Position, Velocity, DataComponent, HealthComponent, DamageComponent, SpriteComponent>();
    w.CreateBatchHandle<Position, Velocity, DataComponent, HealthComponent, DamageComponent, SpriteComponent>(
        N, Position{0,0,0}, Velocity{1,2,3}, DataComponent{}, HealthComponent{100.f, 100.f, true},
        DamageComponent{5.f, false}, SpriteComponent{'z', 0});

    auto movementQ = w.Query<Position, Velocity>();
    auto dataQ     = w.Query<DataComponent>();
    auto complexQ  = w.Query<Position, Velocity, DataComponent>();
    auto healthQ   = w.Query<HealthComponent, DamageComponent>();
    auto damageQ   = w.Query<DamageComponent>();
    auto spriteQ   = w.Query<SpriteComponent, Position>();
    auto renderQ   = w.Query<SpriteComponent>();

    auto run = [&]() {
        movementQ.Each([dt](EntityHandle, Position& p, Velocity& v) { p.x += v.x*dt; p.y += v.y*dt; p.z += v.z*dt; });
        dataQ.Each([](EntityHandle, DataComponent& d) { d.thingy = static_cast<int32_t>(d.rng() % 100); d.toggle = !d.toggle; });
        complexQ.Each([dt](EntityHandle, Position& p, Velocity& v, DataComponent& d) {
            if (d.toggle) { p.x -= v.x * dt * 0.5f; }
        });
        healthQ.Each([](EntityHandle, HealthComponent& h, DamageComponent& dmg) {
            if (!dmg.applied) { h.hp -= dmg.amount; dmg.applied = true; }
            h.alive = h.hp > 0.f;
        });
        damageQ.Each([](EntityHandle, DamageComponent& d) { d.applied = false; });
        spriteQ.Each([](EntityHandle, SpriteComponent& s, Position& p) { s.layer = static_cast<uint32_t>(p.z); });
        uint64_t sink = 0;
        renderQ.Each([&sink](EntityHandle, SpriteComponent& s) { sink += s.layer; });
        (void)sink;
    };

    for (int i = 0; i < 3; ++i) run();
    constexpr int RUNS = 10;
    std::vector<double> samples; samples.reserve(RUNS);
    for (int r = 0; r < RUNS; ++r) {
        auto t0 = Clock::now();
        run();
        samples.push_back(ToMs(Clock::now() - t0));
    }
    std::sort(samples.begin(), samples.end());
    printf("\n  [BENCH] 7-systems 1M:                      median=%5.2f ms  min=%5.2f ms\n",
           samples[RUNS/2], samples.front());
    printf("  [REF]   abeimler 7-sys: EnTT ~60ms, Flecs ~25ms, pico_ecs ~30ms, mustache ~20ms @ 1M\n");
}

// ---- #4 mixed 7 systems (fragmented across archetypes) ----
TEST(ECSBenchmark, FragmentedSevenSystems_1M) {
    constexpr size_t N = 1'000'000;
    constexpr float dt = 0.016f;
    World w(nullptr);
    w.RegisterComponents<Position, Velocity, DataComponent, HealthComponent, DamageComponent, SpriteComponent>();
    BuildMixedScene(w, N);

    auto movementQ = w.Query<Position, Velocity>();
    auto dataQ     = w.Query<DataComponent>();
    auto complexQ  = w.Query<Position, Velocity, DataComponent>();
    auto healthQ   = w.Query<HealthComponent>();
    auto damageQ   = w.Query<DamageComponent>();
    auto spriteQ   = w.Query<SpriteComponent, Position>();
    auto renderQ   = w.Query<SpriteComponent>();

    auto run = [&]() {
        movementQ.Each([dt](EntityHandle, Position& p, Velocity& v) { p.x += v.x*dt; p.y += v.y*dt; p.z += v.z*dt; });
        dataQ.Each([](EntityHandle, DataComponent& d) { d.thingy = static_cast<int32_t>(d.rng() % 100); d.toggle = !d.toggle; });
        complexQ.Each([dt](EntityHandle, Position& p, Velocity& v, DataComponent& d) {
            if (d.toggle) { p.x -= v.x * dt * 0.5f; }
        });
        healthQ.Each([](EntityHandle, HealthComponent& h) { h.alive = h.hp > 0.f; });
        damageQ.Each([](EntityHandle, DamageComponent& d) { d.applied = !d.applied; });
        spriteQ.Each([](EntityHandle, SpriteComponent& s, Position& p) { s.layer = static_cast<uint32_t>(p.z); });
        uint64_t sink = 0;
        renderQ.Each([&sink](EntityHandle, SpriteComponent& s) { sink += s.layer; });
        (void)sink;
    };

    for (int i = 0; i < 3; ++i) run();
    constexpr int RUNS = 10;
    std::vector<double> samples; samples.reserve(RUNS);
    for (int r = 0; r < RUNS; ++r) {
        auto t0 = Clock::now();
        run();
        samples.push_back(ToMs(Clock::now() - t0));
    }
    std::sort(samples.begin(), samples.end());
    printf("\n  [BENCH] Fragmented 7-systems 1M:           median=%5.2f ms  min=%5.2f ms\n",
           samples[RUNS/2], samples.front());
    printf("  [REF]   abeimler mixed-7: EnTT ~90ms, Flecs ~20ms, pico_ecs ~25ms @ 1M\n");
}

// ---- #7/#8/#9 unpack N components (pure read) ----
TEST(ECSBenchmark, UnpackComponents_1M) {
    constexpr size_t N = 1'000'000;
    World w(nullptr);
    w.RegisterComponents<Position, Velocity, HealthComponent>();
    w.CreateBatchHandle<Position, Velocity, HealthComponent>(
        N, Position{1.f, 2.f, 3.f}, Velocity{1.f, 1.f, 1.f}, HealthComponent{100.f, 100.f, true});

    auto q1 = w.Query<Position>();
    auto q2 = w.Query<Position, Velocity>();
    auto q3 = w.Query<Position, Velocity, HealthComponent>();

    auto time1 = [&]() {
        auto t0 = Clock::now();
        volatile float sum = 0;
        q1.Each([&sum](EntityHandle, Position& p) { sum = sum + p.x; });
        return ToMs(Clock::now() - t0);
    };
    auto time2 = [&]() {
        auto t0 = Clock::now();
        volatile float sum = 0;
        q2.Each([&sum](EntityHandle, Position& p, Velocity& v) { sum = sum + p.x + v.x; });
        return ToMs(Clock::now() - t0);
    };
    auto time3 = [&]() {
        auto t0 = Clock::now();
        volatile float sum = 0;
        q3.Each([&sum](EntityHandle, Position& p, Velocity& v, HealthComponent& h) { sum = sum + p.x + v.x + h.hp; });
        return ToMs(Clock::now() - t0);
    };

    // Warm all three.
    for (int i = 0; i < 3; ++i) { (void)time1(); (void)time2(); (void)time3(); }

    constexpr int RUNS = 11;
    std::vector<double> s1, s2, s3; s1.reserve(RUNS); s2.reserve(RUNS); s3.reserve(RUNS);
    for (int r = 0; r < RUNS; ++r) {
        switch (r % 3) {
            case 0: s1.push_back(time1()); s2.push_back(time2()); s3.push_back(time3()); break;
            case 1: s2.push_back(time2()); s3.push_back(time3()); s1.push_back(time1()); break;
            default: s3.push_back(time3()); s1.push_back(time1()); s2.push_back(time2()); break;
        }
    }
    auto med = [](std::vector<double>& v) { std::sort(v.begin(), v.end()); return v[v.size()/2]; };
    const double m1 = med(s1), m2 = med(s2), m3 = med(s3);
    const double mn1 = s1.front(), mn2 = s2.front(), mn3 = s3.front();

    printf("\n  [BENCH] Unpack 1 comp 1M (read Pos):       median=%5.3f ms  min=%5.3f ms  (%.1f ns/entity)\n",
           m1, mn1, mn1 * 1e6 / N);
    printf("  [BENCH] Unpack 2 comp 1M (read Pos+Vel):   median=%5.3f ms  min=%5.3f ms  (%.1f ns/entity)\n",
           m2, mn2, mn2 * 1e6 / N);
    printf("  [BENCH] Unpack 3 comp 1M (read Pos+Vel+H): median=%5.3f ms  min=%5.3f ms  (%.1f ns/entity)\n",
           m3, mn3, mn3 * 1e6 / N);
    printf("  [REF]   abeimler @ 1M: EnTT 3-12ms, pico_ecs 1-3ms, Flecs/mustache 13-59ms\n");
}

// ---- #10 remove + add component ----
TEST(ECSBenchmark, RemoveAddComponent_100K) {
    // 100K chosen (not 1M) because every op is a full archetype move; 1M would
    // dominate the suite runtime.
    constexpr size_t N = 100'000;
    World w(nullptr);
    w.RegisterComponents<Position, Velocity, HealthComponent>();
    auto handles = w.CreateBatchHandle<Position, Velocity, HealthComponent>(
        N, Position{}, Velocity{}, HealthComponent{100.f, 100.f, true});

    // Remove + Add round-trip — measures structural churn cost.
    auto t0 = Clock::now();
    for (auto h : handles) {
        w.RemoveComponentImmediate<HealthComponent>(h);
    }
    auto tMid = Clock::now();
    for (auto h : handles) {
        w.AddComponentImmediate(h, HealthComponent{50.f, 100.f, true});
    }
    auto t1 = Clock::now();

    const double rmMs = ToMs(tMid - t0);
    const double addMs = ToMs(t1 - tMid);
    printf("\n  [BENCH] Remove comp 100K: %5.2f ms  (%.0f ns/entity)\n", rmMs, rmMs * 1e6 / N);
    printf("  [BENCH] Add    comp 100K: %5.2f ms  (%.0f ns/entity)\n", addMs, addMs * 1e6 / N);
    printf("  [BENCH] Round-trip total: %5.2f ms\n", rmMs + addMs);
    printf("  [REF]   abeimler remove+add @ 1M: EnTT ~27ms, Flecs ~260ms, pico_ecs ~8ms\n");
}

// ---- Create / Destroy at 1M (align scale with abeimler) ----
TEST(ECSBenchmark, EntityCreation_1M) {
    constexpr size_t N = 1'000'000;
    {
        WorldConfig cfg; cfg.ExpectedEntityCount = N;
        World w(cfg, nullptr);
        w.RegisterComponents<Position, Velocity>();
        auto t0 = Clock::now();
        for (size_t i = 0; i < N; ++i) {
            w.CreateHandle(Position{static_cast<float>(i), 0.f, 0.f}, Velocity{1.f, 1.f, 1.f});
        }
        printf("\n  [BENCH] Create Individual 1M (2 comp): %6.1f ms\n", ToMs(Clock::now() - t0));
    }
    {
        WorldConfig cfg; cfg.ExpectedEntityCount = N;
        World w(cfg, nullptr);
        w.RegisterComponents<Position, Velocity>();
        auto t0 = Clock::now();
        w.CreateBatchHandle<Position, Velocity>(N, Position{}, Velocity{});
        printf("  [BENCH] Create Batch 1M (2 comp):      %6.1f ms\n", ToMs(Clock::now() - t0));
        printf("  [REF]   abeimler @ 1M: EnTT ~66ms, pico_ecs ~38ms, Flecs ~370ms\n");
    }
}

TEST(ECSBenchmark, EntityDestruction_1M) {
    constexpr size_t N = 1'000'000;
    World w(nullptr);
    w.RegisterComponents<Position, Velocity>();
    auto handles = w.CreateBatchHandle<Position, Velocity>(N, Position{}, Velocity{});
    auto t0 = Clock::now();
    for (auto h : handles) w.DestroyEntityImmediate(h);
    printf("\n  [BENCH] Destroy 1M:                    %6.1f ms\n", ToMs(Clock::now() - t0));
    printf("  [REF]   abeimler @ 1M: EnTT ~43ms, pico_ecs ~9ms, Flecs ~59ms\n");
    EXPECT_EQ(w.GetEntityCount(), 0u);
}

// ---- CreateBatchWithInit: per-entity init via callback ----
// Compares three patterns for creating N entities with per-entity distinct
// values. The loop-of-CreateHandle pattern is the worst case (lock per
// entity); CreateBatchHandle + after-loop Set is middle; CreateBatchWithInit
// is the target (single lock, no intermediate copies).
TEST(ECSBenchmark, CreateBatchWithInit_1M) {
    constexpr size_t N = 1'000'000;

    // Pattern A: individual CreateHandle loop — lock per entity.
    {
        WorldConfig cfg; cfg.ExpectedEntityCount = N;
        World w(cfg, nullptr);
        w.RegisterComponents<Position, Velocity>();
        auto t0 = Clock::now();
        for (size_t i = 0; i < N; ++i) {
            w.CreateHandle(Position{float(i), 0, 0}, Velocity{float(i), 1, 0});
        }
        printf("\n  [BENCH] 1M per-entity init, loop CreateHandle:    %6.1f ms\n",
               ToMs(Clock::now() - t0));
    }

    // Pattern B: uniform CreateBatchHandle, then loop to set per-entity values.
    {
        WorldConfig cfg; cfg.ExpectedEntityCount = N;
        World w(cfg, nullptr);
        w.RegisterComponents<Position, Velocity>();
        auto t0 = Clock::now();
        auto handles = w.CreateBatchHandle<Position, Velocity>(N, Position{}, Velocity{});
        for (size_t i = 0; i < N; ++i) {
            auto* p = w.GetComponentForWrite<Position>(handles[i]);
            auto* v = w.GetComponentForWrite<Velocity>(handles[i]);
            *p = {float(i), 0, 0};
            *v = {float(i), 1, 0};
        }
        printf("  [BENCH] 1M per-entity init, batch + SetAfter:     %6.1f ms\n",
               ToMs(Clock::now() - t0));
    }

    // Pattern C: CreateBatchWithInit — single lock, direct column writes.
    {
        WorldConfig cfg; cfg.ExpectedEntityCount = N;
        World w(cfg, nullptr);
        w.RegisterComponents<Position, Velocity>();
        auto t0 = Clock::now();
        auto handles = w.CreateBatchWithInit<Position, Velocity>(N,
            [](size_t i, Position& p, Velocity& v) {
                p = {float(i), 0, 0};
                v = {float(i), 1, 0};
            });
        printf("  [BENCH] 1M per-entity init, CreateBatchWithInit:  %6.1f ms\n",
               ToMs(Clock::now() - t0));
        EXPECT_EQ(handles.size(), N);
    }
}

// ---- Archetype-transition cost — Remove+Add round-trip (MoveEntity hot path).
// Rerun to measure the archetype-lock removal in MoveEntity.
TEST(ECSBenchmark, ArchetypeTransition_100K_Detailed) {
    constexpr size_t N = 100'000;
    World w(nullptr);
    w.RegisterComponents<Position, Velocity, HealthComponent>();
    auto handles = w.CreateBatchHandle<Position, Velocity, HealthComponent>(
        N, Position{}, Velocity{}, HealthComponent{100.f, 100.f, true});

    // Warm one pass
    for (size_t i = 0; i < 1000; ++i) {
        w.RemoveComponentImmediate<HealthComponent>(handles[i]);
        w.AddComponentImmediate(handles[i], HealthComponent{50.f, 100.f, true});
    }

    auto t0 = Clock::now();
    for (auto h : handles) w.RemoveComponentImmediate<HealthComponent>(h);
    auto tMid = Clock::now();
    for (auto h : handles) w.AddComponentImmediate(h, HealthComponent{50.f, 100.f, true});
    auto t1 = Clock::now();

    const double rmMs = ToMs(tMid - t0);
    const double addMs = ToMs(t1 - tMid);
    printf("\n  [BENCH] Remove comp 100K (MoveEntity unsafe path): %5.2f ms (%.0f ns/entity)\n",
           rmMs, rmMs * 1e6 / N);
    printf("  [BENCH] Add    comp 100K (MoveEntity unsafe path): %5.2f ms (%.0f ns/entity)\n",
           addMs, addMs * 1e6 / N);
    printf("  [BENCH] Round-trip total:                          %5.2f ms\n", rmMs + addMs);
}

// ============================================================================
// Deferred-pipeline + CachedQuery coverage (perf audit 2026-07, quick-wins batch)
// ============================================================================

// Raw SPSC command-ring push/pop throughput. Bursts of half the ring force a
// wrap every other cycle, so the index arithmetic (modulo vs pow2 mask) is on
// the measured path.
TEST(ECSBenchmark, CommandRing_PushPop_1M) {
    constexpr size_t kTotal = 1'048'576;
    constexpr size_t kBurst = 16'384;
    CommandBuffer ring(32768);

    auto start = Clock::now();
    size_t done = 0;
    Command out;
    while (done < kTotal) {
        for (size_t i = 0; i < kBurst; ++i) {
            Command cmd;
            cmd.type = Command::DESTROY_ENTITY;
            cmd.entity = EntityHandle(static_cast<uint32_t>(i), 1);
            ASSERT_TRUE(ring.TryPush(std::move(cmd)));
        }
        for (size_t i = 0; i < kBurst; ++i) {
            ASSERT_TRUE(ring.TryPop(out));
        }
        done += kBurst;
    }
    auto end = Clock::now();

    const double ms = ToMs(end - start);
    printf("\n  [BENCH] CommandRing push+pop 1M:         %7.2f ms (%.1f ns/pair)\n",
           ms, ms * 1e6 / kTotal);
}

// Deferred destroy burst: enqueue cost (GetCommandBuffer + ring push per op)
// and drain cost (ProcessCommands). Ring is sized to hold the whole burst so
// nothing falls back to immediate execution.
TEST(ECSBenchmark, DeferredDestroyBurst_64K) {
    constexpr size_t N = 65'536;
    WorldConfig cfg;
    cfg.CommandBufferSize = 131'072;
    World w(cfg, nullptr);
    w.RegisterComponents<Position, Velocity>();
    auto handles = w.CreateBatchHandle<Position, Velocity>(N, Position{}, Velocity{});

    auto t0 = Clock::now();
    for (auto h : handles) {
        w.DestroyEntity(h);
    }
    auto t1 = Clock::now();
    w.ProcessCommands();
    auto t2 = Clock::now();

    const double enqueueMs = ToMs(t1 - t0);
    const double drainMs = ToMs(t2 - t1);
    printf("\n  [BENCH] Deferred destroy enqueue 64K:    %7.2f ms (%.0f ns/op)\n",
           enqueueMs, enqueueMs * 1e6 / N);
    printf("  [BENCH] Deferred destroy drain 64K:      %7.2f ms (%.0f ns/op)\n",
           drainMs, drainMs * 1e6 / N);
    EXPECT_EQ(w.GetEntityCount(), 0u);
}

// CachedQuery sliced entity-id iteration — the managed IEntitySystem tick
// path (GE_ECSABI_CachedQueryGetEntityIdSlice). A full pass reads every
// entity id once in slice-sized windows.
TEST(ECSBenchmark, CachedQuerySlice_64K) {
    constexpr size_t N = 65'536;
    constexpr size_t kSlice = 4096;
    World w(nullptr);
    w.RegisterComponents<Position>();
    w.CreateBatchHandle<Position>(N, Position{0.f, 0.f, 0.f});

    CachedQuery q(&w, {GetComponentTypeId<Position>()}, {});
    q.Refresh();
    ASSERT_EQ(q.GetArchetypeCount(), 1u);

    (void)q.GetEntityIdSlice(0, 0, kSlice); // warm

    constexpr int RUNS = 20;
    size_t totalIds = 0;
    auto start = Clock::now();
    for (int r = 0; r < RUNS; ++r) {
        size_t offset = 0;
        for (;;) {
            auto [ids, count] = q.GetEntityIdSlice(0, offset, kSlice);
            if (count == 0) {
                break;
            }
            totalIds += count;
            offset += count;
        }
    }
    auto end = Clock::now();

    const double ms = ToMs(end - start);
    EXPECT_EQ(totalIds, N * static_cast<size_t>(RUNS));
    printf("\n  [BENCH] CachedQuery sliced pass 64K (4096/slice): %7.3f ms avg over %d runs\n",
           ms / RUNS, RUNS);
}

// World-construction memory cost of per-thread command buffers. Buffers are
// value-initialized at allocation, so their pages land in the working set.
// Windows-only measurement (working-set delta).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi")

namespace {
size_t CurrentWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
}
} // namespace

TEST(ECSBenchmark, WorldConstruction_CommandBufferMemory) {
    constexpr int kWorlds = 4;
    const size_t before = CurrentWorkingSetBytes();

    std::vector<std::unique_ptr<World>> worlds;
    worlds.reserve(kWorlds);
    for (int i = 0; i < kWorlds; ++i) {
        worlds.push_back(std::make_unique<World>(nullptr));
    }

    const size_t after = CurrentWorkingSetBytes();
    const double totalMb = static_cast<double>(after - before) / (1024.0 * 1024.0);
    printf("\n  [BENCH] World construction RSS delta:    %6.1f MB total (%d worlds, %.1f MB/world, no deferred ops issued)\n",
           totalMb, kWorlds, totalMb / kWorlds);
}
#endif // _WIN32
