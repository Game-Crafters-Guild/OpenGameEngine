// gaia-ecs lane — documented idiomatic API: world.add()/add<T>(e, value),
// world.query().all<...>().each(...). Archetype/chunk architecture — the
// closest structural cousin to our ECS.

#include "BenchCommon.h"

#include <gaia.h>

#include <algorithm>
#include <random>

namespace bench {
namespace {

namespace gecs = gaia::ecs;

std::vector<gecs::Entity> CreateWorld(gecs::World& world, std::size_t n, bool withData,
                                      bool withHealth)
{
    std::vector<gecs::Entity> entities;
    entities.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        auto e = world.add();
        world.add<Position>(e, {static_cast<float>(i), static_cast<float>(i) * 0.5f});
        world.add<Velocity>(e, {0.1f, 0.1f});
        if (withData)
            world.add<DataComp>(e, {0, 0.0f});
        if (withHealth)
            world.add<Health>(e, {100});
        entities.push_back(e);
    }
    return entities;
}

CaseResult CreateEntities(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"gaia", "CreateEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        gecs::World world;
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < n; ++i)
        {
            auto e = world.add();
            world.add<Position>(e, {static_cast<float>(i), static_cast<float>(i) * 0.5f});
            world.add<Velocity>(e, {0.1f, 0.1f});
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
    CaseResult r{"gaia", "DestroyEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        gecs::World world;
        auto entities = CreateWorld(world, n, false, false);
        const auto t0 = Clock::now();
        for (const auto e : entities)
            world.del(e);
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult AddRemoveComponent(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"gaia", "AddRemoveComponent", n};
    gecs::World world;
    auto entities = CreateWorld(world, n, false, false);
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        const auto t0 = Clock::now();
        for (const auto e : entities)
            world.add<DataComp>(e, {1, 1.0f});
        for (const auto e : entities)
            world.del<DataComp>(e);
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult RandomAccessGet(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"gaia", "RandomAccessGet", n};
    gecs::World world;
    auto entities = CreateWorld(world, n, false, false);
    std::mt19937 rng(kShuffleSeed);
    std::shuffle(entities.begin(), entities.end(), rng);
    double checksum = 0.0;
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        float sum = 0.0f;
        const auto t0 = Clock::now();
        for (const auto e : entities)
            sum += world.get<Position>(e).x;
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
    CaseResult r{"gaia", "IterateOneComponent", n};
    gecs::World world;
    auto entities = CreateWorld(world, n, true, false);
    auto q = world.query().all<Position&>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.each([](Position& p) { p.x += 1.0f; });
    });
    KeepAlive(world.get<Position>(entities[0]).x);
    r.Finalize();
    return r;
}

CaseResult IterateTwoComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"gaia", "IterateTwoComponents", n};
    gecs::World world;
    auto entities = CreateWorld(world, n, true, false);
    auto q = world.query();
    q.all<Position&>().all<Velocity>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.each([](Position& p, const Velocity& v) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
        });
    });
    KeepAlive(world.get<Position>(entities[0]).x);
    r.Finalize();
    return r;
}

CaseResult IterateThreeComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"gaia", "IterateThreeComponents", n};
    gecs::World world;
    auto entities = CreateWorld(world, n, true, false);
    auto q = world.query();
    q.all<Position&>().all<Velocity>().all<DataComp&>();
    SampleIteratePasses(r, cfg, n, [&] {
        q.each([](Position& p, const Velocity& v, DataComp& d) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
            d.thingy++;
            d.dingy += 0.0001f;
        });
    });
    KeepAlive(world.get<DataComp>(entities[0]).dingy);
    r.Finalize();
    return r;
}

CaseResult ComplexSystemsUpdate(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"gaia", "ComplexSystemsUpdate", n};
    gecs::World world;
    auto entities = CreateWorld(world, n, true, true);
    auto qMove = world.query();
    qMove.all<Position&>().all<Velocity>();
    auto qData = world.query().all<DataComp&>();
    auto qHealth = world.query();
    qHealth.all<Health&>().all<Damage>();
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
            const auto e = entities[(frame * churn + k) % n];
            if (world.has<Damage>(e))
                world.del<Damage>(e);
            else
                world.add<Damage>(e, {10});
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
    KeepAlive(static_cast<double>(world.get<Health>(entities[0]).hp));
    r.Finalize();
    return r;
}

} // namespace

std::vector<CaseResult> RunGaiaBenches(const BenchConfig& cfg)
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
