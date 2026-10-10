#pragma once

namespace GameEngine
{
namespace Animation
{

struct AnimationPose;
struct EvaluationContext;

// Base class for all animation graph nodes. Each node produces a pose
// when evaluated. Nodes form a tree: leaf nodes sample clips, interior
// nodes blend or select among their children.
//
// EvaluationContext carries the source pose, source/target skeleton
// metadata, the per-thread scratch PoseStack, and DeltaTime. Subclasses
// should read DeltaTime from ctx instead of taking a separate float param;
// retarget-aware nodes read SourceProfile/SourceSkeleton/Source*.
class AnimGraphNode
{
public:
    virtual ~AnimGraphNode() = default;

    virtual void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) = 0;
};

} // namespace Animation
} // namespace GameEngine
