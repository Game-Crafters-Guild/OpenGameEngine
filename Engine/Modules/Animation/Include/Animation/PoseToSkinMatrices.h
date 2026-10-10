#pragma once

#include "Animation/AnimationPose.h"
#include "Animation/SkeletonData.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace GameEngine
{
namespace Animation
{

// Workspace buffers reused by BuildPoseToSkinMatrices to avoid per-frame
// heap allocations. Layout matches the Engine::Renderer::PoseSampleWorkspace
// shape used by SampleAnimationPose so the same instance can be threaded
// through both calls.
struct PoseSampleWorkspace
{
    struct TRS
    {
        glm::vec3 t{0.0f};
        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 s{1.0f};
    };

    std::vector<TRS> Pose;
    std::vector<uint8_t> ChangedTranslation;
    std::vector<uint8_t> ChangedRotation;
    std::vector<uint8_t> ChangedScale;
    std::vector<glm::mat4> Local;
    std::vector<glm::mat4> World;
    std::vector<uint8_t> Built;
};

// Build target-skeleton local matrices, walk the hierarchy, and emit the
// compact skin palette. Both the regular and retargeted sample paths call
// this after their respective ws.Pose population. Operates entirely on the
// SkeletonData asset + the workspace; no rendering deps.
void BuildPoseToSkinMatrices(const SkeletonData& skeleton,
                             std::vector<float>* outNodeWorldMatrices,
                             std::vector<float>* outCompactSkinMatrices,
                             PoseSampleWorkspace& ws);

// Copy local TRS from an AnimationPose into the workspace and emit the
// compact skin palette. Bones past pose.BoneCount keep workspace defaults
// (identity TRS) and are marked dirty so the world walk still runs.
void EmitSkinMatricesFromAnimationPose(const SkeletonData& skeleton,
                                       const AnimationPose& localPose,
                                       std::vector<float>& outCompactSkinMatrices,
                                       PoseSampleWorkspace& ws);

} // namespace Animation
} // namespace GameEngine
