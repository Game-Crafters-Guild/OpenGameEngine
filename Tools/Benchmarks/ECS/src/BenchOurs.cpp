// GameEngine ECS lane — shipped idioms:
//  - CreateHandle(components...) for creation (direct-to-archetype)
//  - DestroyEntityImmediate / AddComponentImmediate / RemoveComponentImmediate
//    for structural changes (the immediate analogues of the other frameworks'
//    synchronous APIs; the deferred command-buffer path is a different shape)
//  - World::Query<Read<...>, Write<...>>().Each(...) for iteration
//
// OUR EXTRA rows (not part of the apples-to-apples set, framework "ours-extra"):
//  - Changed<> gated iteration on a 1%-dirty workload (+ ungated baseline)
//  - BatchEach (raw pointer + count per chunk)
//  - ParallelBatchEach on a JobSystem-backed world

#include "BenchCommon.h"

#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <memory>
#include <random>

namespace bench {
namespace {

namespace GE = GameEngine::ECS;

std::vector<GE::EntityHandle> CreateWorld(GE::World& world, std::size_t n, bool withData,
                                          bool withHealth)
{
    std::vector<GE::EntityHandle> handles;
    handles.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const Position p{static_cast<float>(i), static_cast<float>(i) * 0.5f};
        const Velocity v{0.1f, 0.1f};
        if (withData && withHealth)
            handles.push_back(world.CreateHandle(p, v, DataComp{0, 0.0f}, Health{100}));
        else if (withData)
            handles.push_back(world.CreateHandle(p, v, DataComp{0, 0.0f}));
        else
            handles.push_back(world.CreateHandle(p, v));
    }
    return handles;
}

void RegisterAll(GE::World& world)
{
    world.RegisterComponents<Position, Velocity, DataComp, Health, Damage>();
}

CaseResult CreateEntities(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "CreateEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        GE::World world(nullptr);
        RegisterAll(world);
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < n; ++i)
        {
            (void)world.CreateHandle(Position{static_cast<float>(i), static_cast<float>(i) * 0.5f},
                                     Velocity{0.1f, 0.1f});
        }
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

// OUR EXTRA: batch creation API (CreateBatchHandle) — no per-entity call.
CaseResult CreateEntitiesBatch(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours-extra", "CreateEntities_Batch", n};
    r.Note = "World::CreateBatchHandle(count, comps...) — single-call batch API";
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        GE::World world(nullptr);
        RegisterAll(world);
        const auto t0 = Clock::now();
        auto handles =
            world.CreateBatchHandle<Position, Velocity>(n, Position{0.0f, 0.0f}, Velocity{0.1f, 0.1f});
        const auto t1 = Clock::now();
        KeepAlive(static_cast<double>(handles.size()));
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult DestroyEntities(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "DestroyEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        GE::World world(nullptr);
        RegisterAll(world);
        auto handles = CreateWorld(world, n, false, false);
        const auto t0 = Clock::now();
        for (const auto h : handles)
            world.DestroyEntityImmediate(h);
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult AddRemoveComponent(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "AddRemoveComponent", n};
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, false, false);
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        const auto t0 = Clock::now();
        for (const auto h : handles)
            world.AddComponentImmediate(h, DataComp{1, 1.0f});
        for (const auto h : handles)
            world.RemoveComponentImmediate<DataComp>(h);
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult RandomAccessGet(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "RandomAccessGet", n};
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, false, false);
    std::mt19937 rng(kShuffleSeed);
    std::shuffle(handles.begin(), handles.end(), rng);
    double checksum = 0.0;
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        float sum = 0.0f;
        const auto t0 = Clock::now();
        for (const auto h : handles)
            sum += world.GetComponent<Position>(h)->x;
        const auto t1 = Clock::now();
        checksum += sum;
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    KeepAlive(checksum);
    r.Finalize();
    return r;
}

CaseResult IterateOneComponent(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "IterateOneComponent", n};
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, true, false);
    auto q = world.Query<GE::Write<Position>>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.Each([](Position& p) { p.x += 1.0f; });
    });
    KeepAlive(world.GetComponent<Position>(handles[0])->x);
    r.Finalize();
    return r;
}

CaseResult IterateTwoComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "IterateTwoComponents", n};
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, true, false);
    auto q = world.Query<GE::Write<Position>, GE::Read<Velocity>>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.Each([](Position& p, const Velocity& v) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
        });
    });
    KeepAlive(world.GetComponent<Position>(handles[0])->x);
    r.Finalize();
    return r;
}

CaseResult IterateThreeComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "IterateThreeComponents", n};
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, true, false);
    auto q = world.Query<GE::Write<Position>, GE::Read<Velocity>, GE::Write<DataComp>>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.Each([](Position& p, const Velocity& v, DataComp& d) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
            d.thingy++;
            d.dingy += 0.0001f;
        });
    });
    KeepAlive(world.GetComponent<DataComp>(handles[0])->dingy);
    r.Finalize();
    return r;
}

CaseResult ComplexSystemsUpdate(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours", "ComplexSystemsUpdate", n};
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, true, true);
    auto qMove = world.Query<GE::Write<Position>, GE::Read<Velocity>>();
    auto qData = world.Query<GE::Write<DataComp>>();
    auto qHealth = world.Query<GE::Write<Health>, GE::Read<Damage>>();
    const std::size_t churn = n / kChurnDivisor;
    std::size_t frame = 0;
    for (int run = -cfg.IterateWarmups; run < cfg.IterateRuns; ++run)
    {
        const auto t0 = Clock::now();
        // MovementSystem
        qMove.Each([](Position& p, const Velocity& v) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
        });
        // DataSystem
        qData.Each([](DataComp& d) {
            d.thingy++;
            d.dingy += 0.0001f;
        });
        // Damage churn: rotating 1/16 window adds or removes Damage
        for (std::size_t k = 0; k < churn; ++k)
        {
            const auto h = handles[(frame * churn + k) % n];
            if (world.HasComponent<Damage>(h))
                world.RemoveComponentImmediate<Damage>(h);
            else
                world.AddComponentImmediate(h, Damage{10});
        }
        // HealthSystem
        qHealth.Each([](Health& h, const Damage& d) {
            h.hp -= d.amount;
            if (h.hp <= 0)
                h.hp = 100;
        });
        const auto t1 = Clock::now();
        ++frame;
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    KeepAlive(static_cast<double>(world.GetComponent<Health>(handles[0])->hp));
    r.Finalize();
    return r;
}

// ---------------------------------------------------------------------------
// OUR EXTRA rows
// ---------------------------------------------------------------------------

// Changed<> gated iteration: each frame 1% of entities (contiguous rotating
// block, chunk-friendly) get their Position rewritten via the
// GetComponentForWrite write grant; the gated consumer then only visits
// chunks whose Position column changed since its last run.
void ChangedGated(const BenchConfig& cfg, std::size_t n, std::vector<CaseResult>& out)
{
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, false, false);

    auto q = world.Query<GE::Read<Position>, GE::Write<Velocity>>();

    // Ungated baseline: identical consumer, full scan every frame.
    CaseResult base{"ours-extra", "Changed1pct_BaselineFullScan", n};
    base.Note = "same consumer, no Changed<> gate (full scan)";
    SampleIteratePasses(base, cfg, n, [&] {
        q.Each([](const Position& p, Velocity& v) { v.x += p.x * 0.0001f; });
    });
    base.Finalize();
    out.push_back(base);

    // Gated lane. Dirty phase is NOT timed; only the gated consumer pass is.
    CaseResult gated{"ours-extra", "Changed1pct_GatedScan", n};
    gated.Note = "Changed<Position> gate, 1% contiguous rotating dirty block per frame; "
                 "single pass per sample (dirty phase untimed) — small-N values sit at "
                 "clock resolution";
    GE::ChangeGate gate;
    gate.LastRunVersion = world.GetGlobalSystemVersion(); // start clean
    const std::size_t dirtyCount = std::max<std::size_t>(1, n / kDirtyDivisor);
    std::size_t frame = 0;
    for (int run = -cfg.IterateWarmups; run < cfg.IterateRuns; ++run)
    {
        // Dirty 1% (untimed): write grant stamps the touched chunks' Position column.
        for (std::size_t k = 0; k < dirtyCount; ++k)
        {
            auto* p = world.GetComponentForWrite<Position>(handles[(frame * dirtyCount + k) % n]);
            p->x += 1.0f;
        }
        ++frame;

        const auto t0 = Clock::now();
        const auto versionBefore = world.GetGlobalSystemVersion();
        q.Changed<Position>(gate);
        q.Each([](const Position& p, Velocity& v) { v.x += p.x * 0.0001f; });
        gate.LastRunVersion = versionBefore;
        const auto t1 = Clock::now();
        if (run >= 0)
            gated.SamplesMs.push_back(MsBetween(t0, t1));
    }
    gated.Finalize();
    out.push_back(gated);
}

// BatchEach: per-chunk raw pointer + count callback (SIMD-friendly), sequential.
CaseResult BatchEachIterate(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours-extra", "IterateTwoComponents_BatchEach", n};
    r.Note = "Query::BatchEach — raw pointers + count per chunk, sequential";
    GE::World world(nullptr);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, false, false);
    auto q = world.Query<GE::Write<Position>, GE::Read<Velocity>>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.BatchEach([](Position* p, const Velocity* v, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i)
            {
                p[i].x += v[i].x * kDeltaTime;
                p[i].y += v[i].y * kDeltaTime;
            }
        });
    });
    KeepAlive(world.GetComponent<Position>(handles[0])->x);
    r.Finalize();
    return r;
}

// ParallelBatchEach: chunk batches fanned out over the engine JobSystem.
CaseResult ParallelBatchEachIterate(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"ours-extra", "IterateTwoComponents_ParallelBatchEach", n};
    r.Note = "Query::ParallelBatchEach on WorkStealingThreadPool (hw threads)";
    JobSystem::WorkStealingThreadPool js;
    GE::World world(&js);
    RegisterAll(world);
    auto handles = CreateWorld(world, n, false, false);
    auto q = world.Query<GE::Write<Position>, GE::Read<Velocity>>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.ParallelBatchEach([](Position* p, const Velocity* v, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i)
            {
                p[i].x += v[i].x * kDeltaTime;
                p[i].y += v[i].y * kDeltaTime;
            }
        });
    });
    KeepAlive(world.GetComponent<Position>(handles[0])->x);
    r.Finalize();
    return r;
}

} // namespace

std::vector<CaseResult> RunOursBenches(const BenchConfig& cfg)
{
    std::vector<CaseResult> out;
    for (const std::size_t n : cfg.Sizes)
    {
        out.push_back(CreateEntities(cfg, n));
        out.push_back(DestroyEntities(cfg, n));
        out.push_back(AddRemoveComponent(cfg, n));
        out.push_back(RandomAccessGet(cfg, n));
        out.push_back(IterateOneComponent(cfg, n));
        out.push_back(IterateTwoComponents(cfg, n));
        out.push_back(IterateThreeComponents(cfg, n));
        out.push_back(ComplexSystemsUpdate(cfg, n));

        // OUR EXTRA rows — differentiating features, not parity comparisons.
        out.push_back(CreateEntitiesBatch(cfg, n));
        ChangedGated(cfg, n, out);
        out.push_back(BatchEachIterate(cfg, n));
        out.push_back(ParallelBatchEachIterate(cfg, n));
    }
    return out;
}

} // namespace bench
