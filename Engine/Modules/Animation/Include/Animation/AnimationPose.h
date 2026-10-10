#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Quaternion.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Animation
{

// A snapshot of all bone local transforms at a moment in time.
// This is the core data type that flows between animation graph nodes.
struct AnimationPose
{
    std::vector<Mathematics::Vector3> Positions;
    std::vector<Mathematics::Quaternion> Rotations;
    std::vector<Mathematics::Vector3> Scales;
    uint32_t BoneCount = 0;

    void Resize(uint32_t boneCount);
    void CopyFrom(const AnimationPose& other);

    // Blend two poses by weight: 0 = fully a, 1 = fully b.
    // Positions and scales are linearly interpolated; rotations use SLERP.
    static void Blend(const AnimationPose& a, const AnimationPose& b, float weight, AnimationPose& result);

    // Additive blend: applies a weighted additive layer on top of a base pose.
    // position += additive.position * weight
    // rotation *= Slerp(identity, additive.rotation, weight)
    // scale += (additive.scale - 1) * weight  (additive scale is relative to identity)
    static void BlendAdditive(const AnimationPose& base, const AnimationPose& additive, float weight, AnimationPose& result);
};

} // namespace Animation
} // namespace GameEngine
