// flecs lane — documented idiomatic C++ API: world.entity().set(), cached
// queries built once and .each()'d per frame (flecs's documented pattern for
// queries iterated every frame). Archetype ("table") architecture.

#include "BenchCommon.h"

#include <flecs.h>

#include <algorithm>
#include <random>

namespace bench {
namespace {

std::vector<flecs::entity> CreateWorld(flecs::world& world, std::size_t n, bool withData,
                                       bool withHealth)
{
    std::vector<flecs::entity> entities;
    entities.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        auto e = world.entity()
                     .set<Position>({static_cast<float>(i), static_cast<float>(i) * 0.5f})
                     .set<Velocity>({0.1f, 0.1f});
        if (withData)
            e.set<DataComp>({0, 0.0f});
        if (withHealth)
            e.set<Health>({100});
        entities.push_back(e);
    }
    return entities;
}

CaseResult CreateEntities(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"flecs", "CreateEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        flecs::world world;
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < n; ++i)
        {
            world.entity()
                .set<Position>({static_cast<float>(i), static_cast<float>(i) * 0.5f})
                .set<Velocity>({0.1f, 0.1f});
        }
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult DestroyEntities(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"flecs", "DestroyEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        flecs::world world;
        auto entities = CreateWorld(world, n, false, false);
        const auto t0 = Clock::now();
        for (auto e : entities)
            e.destruct();
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult AddRemoveComponent(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"flecs", "AddRemoveComponent", n};
    flecs::world world;
    auto entities = CreateWorld(world, n, false, false);
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        const auto t0 = Clock::now();
        for (auto e : entities)
            e.set<DataComp>({1, 1.0f});
        for (auto e : entities)
            e.remove<DataComp>();
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult RandomAccessGet(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"flecs", "RandomAccessGet", n};
    flecs::world world;
    auto entities = CreateWorld(world, n, false, false);
    std::mt19937 rng(kShuffleSeed);
    std::shuffle(entities.begin(), entities.end(), rng);
    double checksum = 0.0;
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        float sum = 0.0f;
        const auto t0 = Clock::now();
        for (auto e : entities)
            sum += e.get<Position>().x;
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
    CaseResult r{"flecs", "IterateOneComponent", n};
    flecs::world world;
    auto entities = CreateWorld(world, n, true, false);
    auto q = world.query_builder<Position>().cache_kind(flecs::QueryCacheAuto).build();
    SampleIteratePasses(r, cfg, n, [&] {
        q.each([](Position& p) { p.x += 1.0f; });
    });
    KeepAlive(entities[0].get<Position>().x);
    r.Finalize();
    return r;
}

CaseResult IterateTwoComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"flecs", "IterateTwoComponents", n};
    flecs::world world;
    auto entities = CreateWorld(world, n, true, false);
    auto q =
        world.query_builder<Position, const Velocity>().cache_kind(flecs::QueryCacheAuto).build();
    SampleIteratePasses(r, cfg, n, [&] {
        q.each([](Position& p, const Velocity& v) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
        });
    });
    KeepAlive(entities[0].get<Position>().x);
    r.Finalize();
    return r;
}

CaseResult IterateThreeComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"flecs", "IterateThreeComponents", n};
    flecs::world world;
    auto entities = CreateWorld(world, n, true, false);
    auto q = world.query_builder<Position, const Velocity, DataComp>()
                 .cache_kind(flecs::QueryCacheAuto)
                 .build();
    SampleIteratePasses(r, cfg, n, [&] {
        q.each([](Position& p, const Velocity& v, DataComp& d) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
            d.thingy++;
            d.dingy += 0.0001f;
        });
    });
    KeepAlive(entities[0].get<DataComp>().dingy);
    r.Finalize();
    return r;
}

CaseResult ComplexSystemsUpdate(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"flecs", "ComplexSystemsUpdate", n};
    flecs::world world;
    auto entities = CreateWorld(world, n, true, true);
    auto qMove =
        world.query_builder<Position, const Velocity>().cache_kind(flecs::QueryCacheAuto).build();
    auto qData = world.query_builder<DataComp>().cache_kind(flecs::QueryCacheAuto).build();
    auto qHealth =
        world.query_builder<Health, const Damage>().cache_kind(flecs::QueryCacheAuto).build();
    const std::size_t churn = n / kChurnDivisor;
    std::size_t frame = 0;
    for (int run = -cfg.IterateWarmups; run < cfg.IterateRuns; ++run)
    {
        const auto t0 = Clock::now();
        // MovementSystem
        qMove.each([](Position& p, const Velocity& v) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
        });
        // DataSystem
        qData.each([](DataComp& d) {
            d.thingy++;
            d.dingy += 0.0001f;
        });
        // Damage churn: rotating 1/16 window adds or removes Damage
        for (std::size_t k = 0; k < churn; ++k)
        {
            auto e = entities[(frame * churn + k) % n];
            if (e.has<Damage>())
                e.remove<Damage>();
            else
                e.set<Damage>({10});
        }
        // HealthSystem
        qHealth.each([](Health& h, const Damage& d) {
            h.hp -= d.amount;
            if (h.hp <= 0)
                h.hp = 100;
        });
        const auto t1 = Clock::now();
        ++frame;
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    KeepAlive(static_cast<double>(entities[0].get<Health>().hp));
    r.Finalize();
    return r;
}

} // namespace

std::vector<CaseResult> RunFlecsBenches(const BenchConfig& cfg)
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
    }
    return out;
}

} // namespace bench
