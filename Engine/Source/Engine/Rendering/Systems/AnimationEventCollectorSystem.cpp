#include "ECSModules/Rendering/Systems/AnimationEventCollectorSystem.h"

#include "Animation/AnimationEventCollectorStore.h"
#include "Components/Animation/Animator.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine { namespace Engine::Renderer {

void AnimationEventCollectorSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto& store = Animation::AnimationEventCollectorStore::Instance();
    world.Query<ECS::Write<Components::Animator>>().Each([&](Components::Animator& animator) {
        if (Animation::AnimationEventCollector* collector = store.Get(animator.eventCollectorId))
            collector->Clear();
        else
            animator.eventCollectorId = store.Create();
    });
}

} } // namespace GameEngine::Engine::Renderer
