// EnTT lane — documented idiomatic API: registry.create / emplace / view().each.
// Sparse-set architecture reference point.

#include "BenchCommon.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <random>

namespace bench {
namespace {

std::vector<entt::entity> CreateWorld(entt::registry& reg, std::size_t n, bool withData,
                                      bool withHealth)
{
    std::vector<entt::entity> entities;
    entities.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto e = reg.create();
        reg.emplace<Position>(e, static_cast<float>(i), static_cast<float>(i) * 0.5f);
        reg.emplace<Velocity>(e, 0.1f, 0.1f);
        if (withData)
            reg.emplace<DataComp>(e, 0, 0.0f);
        if (withHealth)
            reg.emplace<Health>(e, 100);
        entities.push_back(e);
    }
    return entities;
}

CaseResult CreateEntities(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"entt", "CreateEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        entt::registry reg;
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto e = reg.create();
            reg.emplace<Position>(e, static_cast<float>(i), static_cast<float>(i) * 0.5f);
            reg.emplace<Velocity>(e, 0.1f, 0.1f);
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
    CaseResult r{"entt", "DestroyEntities", n};
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        entt::registry reg;
        auto entities = CreateWorld(reg, n, false, false);
        const auto t0 = Clock::now();
        for (const auto e : entities)
            reg.destroy(e);
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult AddRemoveComponent(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"entt", "AddRemoveComponent", n};
    entt::registry reg;
    auto entities = CreateWorld(reg, n, false, false);
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        const auto t0 = Clock::now();
        for (const auto e : entities)
            reg.emplace<DataComp>(e, 1, 1.0f);
        for (const auto e : entities)
            reg.remove<DataComp>(e);
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    r.Finalize();
    return r;
}

CaseResult RandomAccessGet(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"entt", "RandomAccessGet", n};
    entt::registry reg;
    auto entities = CreateWorld(reg, n, false, false);
    std::mt19937 rng(kShuffleSeed);
    std::shuffle(entities.begin(), entities.end(), rng);
    double checksum = 0.0;
    for (int run = -cfg.StructuralWarmups; run < cfg.StructuralRuns; ++run)
    {
        float sum = 0.0f;
        const auto t0 = Clock::now();
        for (const auto e : entities)
            sum += reg.get<Position>(e).x;
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
    CaseResult r{"entt", "IterateOneComponent", n};
    entt::registry reg;
    auto entities = CreateWorld(reg, n, true, false);
    SampleIteratePasses(r, cfg, n, [&] {
        reg.view<Position>().each([](Position& p) { p.x += 1.0f; });
    });
    KeepAlive(reg.get<Position>(entities[0]).x);
    r.Finalize();
    return r;
}

CaseResult IterateTwoComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"entt", "IterateTwoComponents", n};
    entt::registry reg;
    auto entities = CreateWorld(reg, n, true, false);
    SampleIteratePasses(r, cfg, n, [&] {
        reg.view<Position, const Velocity>().each([](Position& p, const Velocity& v) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
        });
    });
    KeepAlive(reg.get<Position>(entities[0]).x);
    r.Finalize();
    return r;
}

CaseResult IterateThreeComponents(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"entt", "IterateThreeComponents", n};
    entt::registry reg;
    auto entities = CreateWorld(reg, n, true, false);
    SampleIteratePasses(r, cfg, n, [&] {
        reg.view<Position, const Velocity, DataComp>().each(
            [](Position& p, const Velocity& v, DataComp& d) {
                p.x += v.x * kDeltaTime;
                p.y += v.y * kDeltaTime;
                d.thingy++;
                d.dingy += 0.0001f;
            });
    });
    KeepAlive(reg.get<DataComp>(entities[0]).dingy);
    r.Finalize();
    return r;
}

CaseResult ComplexSystemsUpdate(const BenchConfig& cfg, std::size_t n)
{
    CaseResult r{"entt", "ComplexSystemsUpdate", n};
    entt::registry reg;
    auto entities = CreateWorld(reg, n, true, true);
    const std::size_t churn = n / kChurnDivisor;
    std::size_t frame = 0;
    for (int run = -cfg.IterateWarmups; run < cfg.IterateRuns; ++run)
    {
        const auto t0 = Clock::now();
        // MovementSystem
        reg.view<Position, const Velocity>().each([](Position& p, const Velocity& v) {
            p.x += v.x * kDeltaTime;
            p.y += v.y * kDeltaTime;
        });
        // DataSystem
        reg.view<DataComp>().each([](DataComp& d) {
            d.thingy++;
            d.dingy += 0.0001f;
        });
        // Damage churn: rotating 1/16 window adds or removes Damage
        for (std::size_t k = 0; k < churn; ++k)
        {
            const auto e = entities[(frame * churn + k) % n];
            if (reg.all_of<Damage>(e))
                reg.remove<Damage>(e);
            else
                reg.emplace<Damage>(e, 10);
        }
        // HealthSystem
        reg.view<Health, const Damage>().each([](Health& h, const Damage& d) {
            h.hp -= d.amount;
            if (h.hp <= 0)
                h.hp = 100;
        });
        const auto t1 = Clock::now();
        ++frame;
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1));
    }
    KeepAlive(static_cast<double>(reg.get<Health>(entities[0]).hp));
    r.Finalize();
    return r;
}

} // namespace

std::vector<CaseResult> RunEnttBenches(const BenchConfig& cfg)
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
