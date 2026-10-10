#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"

#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include "PhysicsECS/Components/PhysicsBody.h"

#include "Components/Transform.h"
#include "ECS/Query.h"

namespace GameEngine::PhysicsECS
{
void PhysicsWritebackSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    const float32 alpha = PhysicsWorldService::GetRenderInterpolationAlpha();

    auto q = world.Query<
        ECS::Write<GameEngine::Components::Transform>,
        ECS::Write<GameEngine::Components::WorldTransform>,
        ECS::Write<GameEngine::Components::PhysicsBody>>();

    uint32 composed = 0;
    q.Each([&](ECS::EntityHandle e,
               GameEngine::Components::Transform& t,
               GameEngine::Components::WorldTransform& wt,
               GameEngine::Components::PhysicsBody& body) {
        if (!body.initialized || !pw->IsBodyValid(body.body))
            return;

        // Interpolate from the snapshot captured in PhysicsStepSystem.
        const Physics::Transform* interpolateFrom =
            (body.hasPrevPhysicsTransform && alpha < 1.0f) ? &body.prevPhysicsTransform : nullptr;

        if (WritePhysicsPose(world, e, pw->GetBodyTransform(body.body), interpolateFrom, alpha, t, wt,
                             body.WritebackRecord))
            ++composed;
    });
    PhysicsWorldService::SetLastBodiesComposed(composed);
}

} // namespace GameEngine::PhysicsECS
