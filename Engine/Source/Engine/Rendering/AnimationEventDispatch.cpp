#include "Engine/Rendering/AnimationEventDispatch.h"

#include "Animation/AnimationEventCollectorStore.h"
#include "Components/Animation/Animator.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <cstddef>

namespace GameEngine::Engine::Renderer
{

namespace
{

// The Animator whose playback drives `entity`'s AnimatorRef: the nearest one at or above `entity`. The walk is
// bounded like every parent walk, so a broken chain cannot loop.
const Components::Animator* FindOwningAnimator(const ECS::World& world, ECS::EntityHandle entity)
{
    ECS::EntityHandle current = entity;
    for (std::size_t step = 0; step < Components::kMaxHierarchyDepth && current.IsValid(); ++step)
    {
        if (const auto* animator = world.GetComponent<Components::Animator>(current))
            return animator;
        const auto* parent = world.GetComponent<Components::Parent>(current);
        if (!parent)
            return nullptr;
        current = parent->parent;
    }
    return nullptr;
}

} // namespace

std::span<const Animation::FiredEvent> GetFiredEvents(const Components::Animator& animator)
{
    const Animation::AnimationEventCollector* collector =
        Animation::AnimationEventCollectorStore::Instance().Get(animator.eventCollectorId);
    if (!collector)
        return {};
    return collector->GetEvents();
}

Animation::AnimationEventCollector* ClaimClipPlaybackEvents(const ECS::World& world, ECS::EntityHandle entity)
{
    const Components::Animator* animator = FindOwningAnimator(world, entity);
    if (!animator)
        return nullptr;
    Animation::AnimationEventCollector* collector =
        Animation::AnimationEventCollectorStore::Instance().Get(animator->eventCollectorId);
    if (!collector || !collector->TryClaimClipPlayback())
        return nullptr;
    return collector;
}

} // namespace GameEngine::Engine::Renderer
