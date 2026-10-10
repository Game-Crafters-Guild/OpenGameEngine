#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Quaternion.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Animation
{

struct AnimationPose;

// World-space bone positions and rotations computed from a local-space pose.
struct WorldSpacePose
{
    std::vector<Mathematics::Vector3> Positions;
    std::vector<Mathematics::Quaternion> Rotations;
};

namespace PoseUtilities
{

// Compute world-space transforms for all bones in the pose.
// parentIndices[i] = parent bone index, or -1 for root bones.
//
// Callers should reuse a stack-resident WorldSpacePose to avoid heap
// allocations on the hot path. This overload writes into outWorld and
// resizes its vectors only if they are smaller than required.
void ComputeWorldSpace(
    const AnimationPose& localPose,
    const std::vector<int32_t>& parentIndices,
    WorldSpacePose& outWorld);

// Convenience overload that returns a fresh WorldSpacePose. Allocates;
// prefer the out-param form on hot paths.
WorldSpacePose ComputeWorldSpace(
    const AnimationPose& localPose,
    const std::vector<int32_t>& parentIndices);

// Convert a world-space rotation to local space given the parent's world rotation.
Mathematics::Quaternion WorldToLocalRotation(
    const Mathematics::Quaternion& worldRotation,
    const Mathematics::Quaternion& parentWorldRotation);

// Convert a world-space position to local space given the parent's world transform.
Mathematics::Vector3 WorldToLocalPosition(
    const Mathematics::Vector3& worldPosition,
    const Mathematics::Vector3& parentWorldPosition,
    const Mathematics::Quaternion& parentWorldRotation);

} // namespace PoseUtilities

} // namespace GameEngine::Animation
