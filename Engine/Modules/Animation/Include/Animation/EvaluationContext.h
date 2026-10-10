#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimParam.h"
#include "Animation/SkeletonData.h"
#include "Types/StringId.h"

#include <cstdint>
#include <unordered_map>

namespace GameEngine
{
namespace Animation
{

class AnimationEventCollector;
class SkeletonProfile;
class PoseStack;

// Per-Evaluate context passed through the animation graph. Replaces the v1
// `Evaluate(deltaTime, outPose)` signature with one that carries the source
// pose, source/target skeleton metadata, and a thread-local scratch stack.
//
// All pointers are non-owning. Lifetimes are guaranteed by the caller for the
// duration of the Evaluate call tree (typically the ECS animation system).
//
// Source* members are null for graphs that don't retarget. TargetSkeleton is
// set when the host found a SkeletonRef in the Animator subtree; Evaluate
// still runs when it is null (clip/SM nodes do not require it).
struct EvaluationContext
{
    const SkeletonProfile*  TargetProfile  = nullptr;
    const SkeletonProfile*  SourceProfile  = nullptr;
    const SkeletonData*     TargetSkeleton = nullptr;
    const SkeletonData*     SourceSkeleton = nullptr;
    PoseStack*              ScratchStack   = nullptr;
    float                   DeltaTime      = 0.0f;
    const std::unordered_map<::GameEngine::StringId, GraphParam>* Parameters = nullptr;
    // Where the clip players and montage slots under the node being evaluated add the events they cross; null when
    // nothing collects them (an editor preview).
    AnimationEventCollector* Events = nullptr;
    // The share of the graph's output pose that the node being evaluated supplies: 1 at the root, scaled by each
    // blend above it (EvaluateAtWeight). A clip player fires its events only while this is above 0.
    float                   Weight         = 1.0f;
};

// Evaluates `child` into `outPose` for a node whose output takes `weight` of the child's pose, so the child's Weight
// is its parent's times `weight`; restores ctx.Weight after.
inline void EvaluateAtWeight(AnimGraphNode& child, EvaluationContext& ctx, float weight, AnimationPose& outPose)
{
    const float parentWeight = ctx.Weight;
    ctx.Weight = parentWeight * weight;
    child.Evaluate(ctx, outPose);
    ctx.Weight = parentWeight;
}

} // namespace Animation
} // namespace GameEngine
