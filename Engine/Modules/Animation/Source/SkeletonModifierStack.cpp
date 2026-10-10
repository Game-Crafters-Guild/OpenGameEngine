#include "Animation/SkeletonModifierStack.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace Animation
{

namespace
{

float32 ClampModifierWeight01(float32 value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

bool IsValidBone(const AnimationPose& pose, uint32 bone)
{
    return bone < pose.BoneCount &&
           bone < pose.Positions.size() &&
           bone < pose.Rotations.size() &&
           bone < pose.Scales.size();
}

void ApplyCopyTransform(const SkeletonModifier& modifier, AnimationPose& pose)
{
    if (!IsValidBone(pose, modifier.SourceBone) || !IsValidBone(pose, modifier.TargetBone))
        return;

    const float32 weight = ClampModifierWeight01(modifier.Weight);
    pose.Positions[modifier.TargetBone] =
        pose.Positions[modifier.TargetBone] * (1.0f - weight) +
        pose.Positions[modifier.SourceBone] * weight;
    pose.Rotations[modifier.TargetBone] =
        Mathematics::Quaternion::Slerp(pose.Rotations[modifier.TargetBone], pose.Rotations[modifier.SourceBone], weight);
    pose.Scales[modifier.TargetBone] =
        pose.Scales[modifier.TargetBone] * (1.0f - weight) +
        pose.Scales[modifier.SourceBone] * weight;
}

} // namespace

void SkeletonModifierStack::Add(SkeletonModifier modifier)
{
    m_Modifiers.push_back(std::move(modifier));
}

void SkeletonModifierStack::Clear()
{
    m_Modifiers.clear();
}

void SkeletonModifierStack::Apply(AnimationPose& pose) const
{
    for (const auto& modifier : m_Modifiers)
    {
        if (!modifier.Enabled || modifier.Weight <= 0.0f)
            continue;

        switch (modifier.Kind)
        {
            case SkeletonModifierKind::CopyTransform:
                ApplyCopyTransform(modifier, pose);
                break;
            case SkeletonModifierKind::LookAt:
            case SkeletonModifierKind::Aim:
            case SkeletonModifierKind::TwoBoneIK:
            case SkeletonModifierKind::FABRIK:
            case SkeletonModifierKind::CCDIK:
            case SkeletonModifierKind::SpringBone:
            case SkeletonModifierKind::TwistDispersion:
            case SkeletonModifierKind::PhysicalBone:
                break;
        }
    }
}

const char* SkeletonModifierKindToString(SkeletonModifierKind kind)
{
    switch (kind)
    {
        case SkeletonModifierKind::LookAt: return "LookAt";
        case SkeletonModifierKind::Aim: return "Aim";
        case SkeletonModifierKind::CopyTransform: return "CopyTransform";
        case SkeletonModifierKind::TwoBoneIK: return "TwoBoneIK";
        case SkeletonModifierKind::FABRIK: return "FABRIK";
        case SkeletonModifierKind::CCDIK: return "CCDIK";
        case SkeletonModifierKind::SpringBone: return "SpringBone";
        case SkeletonModifierKind::TwistDispersion: return "TwistDispersion";
        case SkeletonModifierKind::PhysicalBone: return "PhysicalBone";
    }
    return "CopyTransform";
}

} // namespace Animation
} // namespace GameEngine
