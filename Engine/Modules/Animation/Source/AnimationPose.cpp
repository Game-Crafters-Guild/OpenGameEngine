#include "Animation/AnimationPose.h"

namespace GameEngine
{
namespace Animation
{

void AnimationPose::Resize(uint32_t boneCount)
{
    BoneCount = boneCount;
    Positions.resize(boneCount, Mathematics::Vector3(0.0f, 0.0f, 0.0f));
    Rotations.resize(boneCount, Mathematics::Quaternion::Identity());
    Scales.resize(boneCount, Mathematics::Vector3(1.0f, 1.0f, 1.0f));
}

void AnimationPose::CopyFrom(const AnimationPose& other)
{
    BoneCount = other.BoneCount;
    Positions = other.Positions;
    Rotations = other.Rotations;
    Scales = other.Scales;
}

void AnimationPose::Blend(const AnimationPose& a, const AnimationPose& b, float weight, AnimationPose& result)
{
    uint32_t count = a.BoneCount < b.BoneCount ? a.BoneCount : b.BoneCount;
    result.Resize(count);

    float invWeight = 1.0f - weight;
    for (uint32_t i = 0; i < count; ++i)
    {
        result.Positions[i] = a.Positions[i] * invWeight + b.Positions[i] * weight;
        result.Scales[i] = a.Scales[i] * invWeight + b.Scales[i] * weight;
        result.Rotations[i] = Mathematics::Quaternion::Slerp(a.Rotations[i], b.Rotations[i], weight);
    }
}

void AnimationPose::BlendAdditive(const AnimationPose& base, const AnimationPose& additive, float weight, AnimationPose& result)
{
    uint32_t count = base.BoneCount < additive.BoneCount ? base.BoneCount : additive.BoneCount;
    result.Resize(count);

    Mathematics::Quaternion identity = Mathematics::Quaternion::Identity();

    for (uint32_t i = 0; i < count; ++i)
    {
        result.Positions[i] = base.Positions[i] + additive.Positions[i] * weight;

        // Additive rotation: base * Slerp(identity, additive, weight)
        Mathematics::Quaternion additiveRot = Mathematics::Quaternion::Slerp(identity, additive.Rotations[i], weight);
        result.Rotations[i] = (base.Rotations[i] * additiveRot).Normalized();

        // Additive scale is relative to identity (1,1,1), so delta = additive - 1
        Mathematics::Vector3 scaleDelta = additive.Scales[i] - Mathematics::Vector3(1.0f, 1.0f, 1.0f);
        result.Scales[i] = base.Scales[i] + scaleDelta * weight;
    }
}

} // namespace Animation
} // namespace GameEngine
