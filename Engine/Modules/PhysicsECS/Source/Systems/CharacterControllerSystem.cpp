#include "PhysicsECS/Systems/CharacterControllerSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"

#include <cmath>

namespace GameEngine::PhysicsECS
{
void CharacterControllerSystem::ReleaseDisabledCharacters(ECS::World& world)
{
    // A missed window (the system was paused while the engine kept swapping)
    // lost its transitions: sweep every character that is off and still built.
    if (m_SwapGuard.ConsumeAndCheckMissed(world.GetWorldId(), world.GetLifecycleSwapGeneration()))
    {
        auto q = world.Query<ECS::Write<GameEngine::Components::CharacterController>,
                             ECS::Optional<ECS::Disabled>,
                             ECS::Optional<ECS::DisabledInHierarchy>,
                             ECS::Optional<ECS::ComponentDisabled<GameEngine::Components::CharacterController>>>();
        q.IncludeDisabled();
        q.Each([](GameEngine::Components::CharacterController& cc, const ECS::Disabled* disabled,
                  const ECS::DisabledInHierarchy* inactive,
                  const ECS::ComponentDisabled<GameEngine::Components::CharacterController>* controllerOff) {
            if (cc.character.IsValid() && (disabled || inactive || controllerOff))
                ReleaseCharacterController(cc);
        });
        return;
    }

    for (ECS::EntityHandle e : world.GetDisabled<GameEngine::Components::CharacterController>())
    {
        if (auto* cc = world.GetComponentForWrite<GameEngine::Components::CharacterController>(e);
            cc && cc->character.IsValid())
            ReleaseCharacterController(*cc);
    }
}

void CharacterControllerSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    ReleaseDisabledCharacters(world);

    world.Query<ECS::Write<GameEngine::Components::CharacterController>,
                ECS::Read<GameEngine::Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e,
                  GameEngine::Components::CharacterController& cc,
                  const GameEngine::Components::WorldTransform& worldTransform)
              {
                  if (world.GetComponent<GameEngine::Components::PhysicsBody>(e) != nullptr)
                  {
                      if (cc.character.IsValid())
                      {
                          pw->DestroyCharacter(cc.character);
                          GameEngine::Components::ClearCharacterControllerRuntimeState(cc);
                      }
                      if (!cc.refusedBecausePhysicsBody)
                      {
                          Logger::Log::Error(
                              "[Physics] CharacterController refused: entity also has PhysicsBody (one motor per entity)");
                          cc.refusedBecausePhysicsBody = true;
                      }
                      return;
                  }
                  cc.refusedBecausePhysicsBody = false;

                  const bool needsCreate =
                      !cc.initialized || !pw->IsCharacterValid(cc.character) || cc.motor != cc.createdMotor;
                  if (needsCreate)
                  {
                      if (cc.character.IsValid())
                      {
                          pw->DestroyCharacter(cc.character);
                          cc.character = {};
                      }

                      GameEngine::Components::Transform pose{};
                      for (int i = 0; i < 16; ++i)
                          pose.matrix[i] = worldTransform.matrix[i];

                      Physics::CharacterSettings desc{};
                      desc.motor = cc.motor;
                      desc.position = Physics::Vector3(
                          pose.GetPosition().x, pose.GetPosition().y, pose.GetPosition().z);
                      desc.rotation = pose.GetRotation();
                      desc.radius = cc.radius;
                      desc.height = cc.height;
                      desc.maxSlopeAngleDegrees = cc.maxSlopeAngleDegrees;
                      desc.maxStepHeight = cc.maxStepHeight;
                      desc.skinWidth = cc.skinWidth;
                      desc.mass = cc.mass;
                      desc.gravityScale = cc.gravityScale;
                      desc.layer = (cc.motor == Physics::CharacterMotor::Dynamic)
                                       ? Physics::Layers::Dynamic
                                       : Physics::Layers::Kinematic;
                      desc.userData = static_cast<uint64>(e.id);

                      cc.character = pw->CreateCharacter(desc);
                      cc.initialized = cc.character.IsValid();
                      cc.createdMotor = cc.motor;
                      if (!cc.initialized)
                          return;
                  }

                  Physics::Vector3 vel = pw->GetCharacterLinearVelocity(cc.character);
                  if (cc.hasAnimationDisplacement)
                  {
                      const float32 dt =
                          (deltaTime > 0.0f && std::isfinite(deltaTime)) ? deltaTime : 0.0f;
                      if (dt > 0.0f && std::isfinite(cc.animationDisplacementX)
                          && std::isfinite(cc.animationDisplacementZ))
                      {
                          vel.x = cc.animationDisplacementX / dt;
                          vel.z = cc.animationDisplacementZ / dt;
                      }
                      else
                      {
                          vel.x = 0.0f;
                          vel.z = 0.0f;
                      }
                      cc.hasAnimationDisplacement = false;
                      cc.animationDisplacementX = 0.0f;
                      cc.animationDisplacementY = 0.0f;
                      cc.animationDisplacementZ = 0.0f;
                  }
                  else
                  {
                      vel.x = cc.desiredVelocityX;
                      vel.z = cc.desiredVelocityZ;
                  }
                  if (cc.desiredVelocityY > 0.0f)
                  {
                      vel.y = cc.desiredVelocityY;
                      cc.desiredVelocityY = 0.0f;
                  }
                  pw->SetCharacterLinearVelocity(cc.character, vel);
              });
}
} // namespace GameEngine::PhysicsECS
