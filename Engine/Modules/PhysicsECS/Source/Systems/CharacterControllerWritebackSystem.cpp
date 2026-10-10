#include "PhysicsECS/Systems/CharacterControllerWritebackSystem.h"

#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/CharacterController.h"

#include "Components/Transform.h"
#include "ECS/Query.h"

namespace GameEngine::PhysicsECS
{
void CharacterControllerWritebackSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    uint32 composed = 0;
    world.Query<ECS::Write<GameEngine::Components::Transform>,
                ECS::Write<GameEngine::Components::WorldTransform>,
                ECS::Write<GameEngine::Components::CharacterController>>()
        .Each([&](ECS::EntityHandle e,
                  GameEngine::Components::Transform& t,
                  GameEngine::Components::WorldTransform& wt,
                  GameEngine::Components::CharacterController& cc)
              {
                  if (!cc.initialized || !pw->IsCharacterValid(cc.character))
                      return;

                  cc.grounded = pw->IsCharacterGrounded(cc.character);
                  const Physics::Vector3 n = pw->GetCharacterGroundNormal(cc.character);
                  cc.groundNormalX = n.x;
                  cc.groundNormalY = n.y;
                  cc.groundNormalZ = n.z;

                  if (WritePhysicsPose(world, e, pw->GetCharacterTransform(cc.character), nullptr, 1.0f, t, wt,
                                       cc.WritebackRecord))
                      ++composed;
              });
    PhysicsWorldService::SetLastCharactersComposed(composed);
}
} // namespace GameEngine::PhysicsECS
