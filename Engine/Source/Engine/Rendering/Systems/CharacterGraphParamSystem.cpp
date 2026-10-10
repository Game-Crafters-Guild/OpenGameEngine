#include "ECSModules/Rendering/Systems/CharacterGraphParamSystem.h"

#include "Components/Animation/Animator.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "Types/StringId.h"

#include <cmath>

namespace GameEngine { namespace Engine::Renderer {

namespace
{
const StringId kSpeedId = HashStringId("Speed");
const StringId kGroundedId = HashStringId("Grounded");
} // namespace

void CharacterGraphParamSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    world.Query<ECS::Read<Components::CharacterController>, ECS::Write<Components::Animator>>()
        .Each([](const Components::CharacterController& character, Components::Animator& animator) {
            if (!animator.active || animator.source != Components::AnimatorPlaybackSource::Graph)
                return;
            const float32 speed = std::hypot(character.desiredVelocityX, character.desiredVelocityZ);
            animator.SetFloat(kSpeedId, std::isfinite(speed) ? speed : 0.0f);
            animator.SetBool(kGroundedId, character.grounded);
        });
}

} } // namespace GameEngine::Engine::Renderer
