#pragma once

#include "Animation/PoseToSkinMatrices.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/AnimatorRef.h"
#include "ECSModules/Rendering/SkeletonStore.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace GameEngine::Animation
{
class AnimationEventCollector;
}

namespace GameEngine
{
namespace Engine::Renderer
{

// Retained scratch buffers for SampleAnimationPose. The struct itself lives
// in the Animation module; this alias keeps existing callsites working.
using PoseSampleWorkspace = ::GameEngine::Animation::PoseSampleWorkspace;

// Samples node-local animation channels onto a skeleton/node hierarchy and optionally
// produces node world matrices and/or a compact skinning palette.
//
// Same-rig only: clip channels are interpreted as the rig's authored local
// rotations / translations and applied directly. Cross-rig retargeting
// lives in HumanoidRetargetSystem + RetargetNode and routes around this
// path entirely (different entity wiring at AnimationSystem dispatch time).
void SampleAnimationPose(const SkeletonData& skeleton,
                         const AnimationClip* clip,
                         float time,
                         std::vector<float>* outNodeWorldMatrices,
                         std::vector<float>* outCompactSkinMatrices,
                         PoseSampleWorkspace& workspace);

/// Wrap a time at or beyond lapEnd into the lap in bounded work. Finite times
/// before lapEnd stay unchanged; non-finite times or invalid laps return lapStart.
/// The double input preserves the fraction when a finite step crosses many laps.
double WrapClipTime(double time, float32 lapStart, float32 lapEnd);

class ClipStore;

// Advance Time / PrevTime / BlendTime. Blend duration is wall-clock seconds
// (not scaled by Speed) so SetAnimation(..., 0.2f) lasts 0.2 s. Clip times
// still scale with Speed. Retained-buffer; no heap. With `events`, adds the
// clip events the advance crossed (Animation::CollectCrossedEvents): the
// outgoing clip's while a crossfade runs, then the incoming clip's. A paused
// ref, a speed of 0 and a time set before the call (a seek) cross nothing.
void TickAnimatorRef(Components::AnimatorRef& anim, ClipStore& clips, float32 deltaTime,
                     Animation::AnimationEventCollector* events);

// Lerp locals in `current` toward `previous` at alpha 0 (previous) .. 1 (current).
// Marks every bone dirty so the skin walk rebuilds. Capacities unchanged.
void BlendLocalPoses(PoseSampleWorkspace& current,
                     const PoseSampleWorkspace& previous,
                     float32 alpha);

// These entry points do not retarget across rigs: they sample a clip onto its
// own skeleton. Cross-rig humanoid retargeting is the GPU-resident pipeline in
// HumanoidRetargetSystem.

} // namespace Engine::Renderer
} // namespace GameEngine
