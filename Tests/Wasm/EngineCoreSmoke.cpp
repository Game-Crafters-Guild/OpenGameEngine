// Headless engine-core smoke for the wasm target (web platform plan, Phase 4).
// Boots Foundation + Platform capabilities + JobSystem + ECS and prints one
// PASS line per subsystem; any failure prints FAIL and exits non-zero.
//
// Workers are sized by Platform::RecommendedWorkerCount(), as the engine sizes
// its own JobSystem: 0 on the single-threaded ABI (GE_WASM_SINGLE_THREAD), where
// the JobSystem runs inline; otherwise the main thread joins the workers via
// the participating Wait.

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Capabilities.h"
#include "Platform/Thread.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{

struct Position
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct Velocity
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

constexpr std::uint32_t kJobCount = 64;
constexpr std::uint32_t kEntityCount = 4096;
constexpr float kDeltaTime = 0.5f;

[[noreturn]] void Fail(const char* what)
{
    std::printf("FAIL: %s\n", what);
    std::exit(1);
}

} // namespace

int main()
{
    using namespace GameEngine;

    const std::uint32_t workerCount = Platform::RecommendedWorkerCount();
    const bool multipleWindows = Platform::SupportsMultipleWindows();
    std::printf("EngineCoreSmoke: dynamicNativeModules=%d multipleWindows=%d workers=%u\n",
                Platform::SupportsDynamicNativeModules() ? 1 : 0,
                multipleWindows ? 1 : 0, workerCount);

    // Editor popups route on this: a browser has one canvas, so Window::Create
    // refuses the second window and a native tool window would never open.
#if defined(__EMSCRIPTEN__)
    if (Platform::CanBlockCurrentThread())
        Fail("the wasm main thread must participate in job waits");
    if (Platform::SupportsAuxiliaryThreadPools())
        Fail("auxiliary worker pools would exceed the reserved wasm thread budget");
    if (multipleWindows)
        Fail("SupportsMultipleWindows() is true on wasm; popups would open windows Window::Create refuses");
#else
    if (!multipleWindows)
        Fail("SupportsMultipleWindows() is false on a desktop build");
#endif

#if defined(GE_WASM_SINGLE_THREAD)
    // A worker on this ABI is a std::thread that cannot be created: the pool
    // (and EngineCore's, sized the same way) aborts in its constructor.
    if (workerCount != 0)
        Fail("RecommendedWorkerCount() asks for workers on the single-threaded ABI");
#endif
    JobSystem::WorkStealingThreadPool pool(workerCount);
    if (pool.IsInlineMode() != (workerCount == 0))
        Fail("JobSystem inline mode does not match worker count");

    // Parallel job batch joined from the main thread (participating Wait).
    std::atomic<std::uint64_t> jobSum{0};
    JobSystem::JobCounter counter;
    for (std::uint32_t i = 0; i < kJobCount; ++i)
    {
        pool.Run([i, &jobSum] { jobSum.fetch_add(i + 1, std::memory_order_relaxed); }, counter);
    }
    pool.Wait(counter);
    if (jobSum.load() != static_cast<std::uint64_t>(kJobCount) * (kJobCount + 1) / 2)
        Fail("job batch sum mismatch");
    std::printf("PASS: JobSystem %u jobs joined (workers=%u inline=%d)\n",
                kJobCount, workerCount, pool.IsInlineMode() ? 1 : 0);

    // ECS world: create entities, run a mutating query, verify with a read.
    ECS::World world(&pool);
    for (std::uint32_t i = 0; i < kEntityCount; ++i)
    {
        const float f = static_cast<float>(i);
        world.Create(Position{f, 0.0f, 0.0f}, Velocity{1.0f, 2.0f, 0.0f});
    }
    if (world.GetEntityCount() != kEntityCount)
        Fail("entity count mismatch after Create");

    world.Query<ECS::Read<Velocity>, ECS::Write<Position>>()
        .Each([](const Velocity& vel, Position& pos) {
            pos.X += vel.X * kDeltaTime;
            pos.Y += vel.Y * kDeltaTime;
        });

    std::uint32_t visited = 0;
    bool valuesOk = true;
    world.Query<ECS::Read<Position>>().Each([&](const Position& pos) {
        // X started at the entity index, so after one step it is index + 0.5.
        const float expectedY = 2.0f * kDeltaTime;
        if (pos.Y != expectedY)
            valuesOk = false;
        ++visited;
    });
    if (visited != kEntityCount)
        Fail("query visited wrong entity count");
    if (!valuesOk)
        Fail("query results wrong");
    std::printf("PASS: ECS %u entities created, queried, verified\n", kEntityCount);

    std::printf("PASS: EngineCoreSmoke complete\n");
    return 0;
}
