#pragma once

#include "Animation/AnimationEvent.h"

#include <span>

namespace GameEngine::Components
{
struct Animator;
}

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
}

namespace GameEngine::Engine::Renderer
{

/// The events `animator`'s playback fired in the last animation wave, in the order they fired: a graph's in the
/// order it evaluates its clip players and montage slots, the clip path's outgoing clip before its incoming clip.
/// They stay until the next animation wave begins; empty for an animator no wave has reached. Scripts read the same
/// events through AnimatorApi.PollEvents. A system that calls this declares a dependency on "HumanoidRetarget", the last
/// system in the wave that fires events: it then runs after all of them, and never beside a system that creates or
/// fills a collector (AnimationEventCollectorStore takes no lock).
std::span<const Animation::FiredEvent> GetFiredEvents(const Components::Animator& animator);

/// The collector the clip playback of `entity`'s AnimatorRef fires into this wave: that of the nearest Animator at or
/// above `entity`, when no other AnimatorRef under that Animator has claimed it this wave
/// (AnimationEventCollector::TryClaimClipPlayback); null otherwise, and when that Animator has no collector yet.
Animation::AnimationEventCollector* ClaimClipPlaybackEvents(const ECS::World& world, ECS::EntityHandle entity);

} // namespace GameEngine::Engine::Renderer
