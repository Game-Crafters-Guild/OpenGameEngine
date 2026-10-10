#include "PhysicsECS/Systems/PhysicsStepSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include "PhysicsECS/Components/PhysicsBody.h"

#include "ECS/Query.h"

#include <chrono>

namespace GameEngine::PhysicsECS
{
void PhysicsStepSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* w = PhysicsWorldService::TryGet();
    if (!w)
        return;

    // Time the whole step (all fixed sub-steps + interpolation snapshot) so the
    // editor Monitors panel can chart per-frame physics cost. This is the clean
    // simulation boundary; measuring here keeps the timing decoupled from the
    // scheduler and from Core diagnostics.
    const auto stepStart = std::chrono::steady_clock::now();

    const auto& s = PhysicsWorldService::GetSettings();
    const int32 collisionSteps = (s.collisionSteps > 0) ? s.collisionSteps : 1;

    // Fixed-timestep stepping (recommended for stability and determinism).
    // Note: PhysicsWorld is currently a global singleton, so this accumulator is effectively global too.
    static float32 accumulator = 0.0f;
    static std::uint64_t lastGen = 0;
    const std::uint64_t gen = PhysicsWorldService::GetWorldGeneration();
    if (gen != lastGen)
    {
        accumulator = 0.0f;
        lastGen = gen;
    }

    const float32 fixed = s.fixedTimeStep;
    const int32 maxSub = (s.maxSubSteps > 0) ? s.maxSubSteps : 1;

    if (fixed > 0.0f)
    {
        // Clamp to avoid "spiral of death" on long frames (e.g., debugger break).
        const float32 clampedDt = (deltaTime > 0.25f) ? 0.25f : ((deltaTime < 0.0f) ? 0.0f : deltaTime);
        accumulator += clampedDt;

        int32 steps = 0;
        while (accumulator >= fixed && steps < maxSub)
        {
            // Snapshot previous transforms for interpolation (prev <- current).
            world.Query<ECS::Write<GameEngine::Components::PhysicsBody>>()
                .Each(
                [&](ECS::EntityHandle /*e*/, GameEngine::Components::PhysicsBody& body)
                {
                    if (!body.initialized || !w->IsBodyValid(body.body))
                        return;
                    body.prevPhysicsTransform = w->GetBodyTransform(body.body);
                    body.hasPrevPhysicsTransform = true;
                });

            w->Step(fixed, collisionSteps);
            accumulator -= fixed;
            ++steps;
        }

        const float32 alpha = (fixed > 0.0f) ? (accumulator / fixed) : 1.0f;
        PhysicsWorldService::SetRenderInterpolationAlpha(alpha);
    }
    else
    {
        // Variable timestep fallback.
        w->Step(deltaTime, collisionSteps);
        PhysicsWorldService::SetRenderInterpolationAlpha(1.0f);
    }

    const auto stepEnd = std::chrono::steady_clock::now();
    PhysicsWorldService::SetLastStepMs(
        static_cast<float32>(std::chrono::duration<double, std::milli>(stepEnd - stepStart).count()));
}
} // namespace GameEngine::PhysicsECS

