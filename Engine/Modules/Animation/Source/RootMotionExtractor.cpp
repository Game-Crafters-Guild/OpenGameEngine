#include "Animation/RootMotionExtractor.h"

#include "Animation/AnimationPose.h"

namespace GameEngine
{
namespace Animation
{

RootMotionDelta RootMotionExtractor::Extract(AnimationPose& pose)
{
    RootMotionDelta delta;

    if (pose.BoneCount == 0)
        return delta;

    Mathematics::Vector3 currentPos = pose.Positions[0];
    Mathematics::Quaternion currentRot = pose.Rotations[0];

    if (m_HasPrevious)
    {
        delta.Translation = currentPos - m_PrevPosition;
        delta.Rotation = m_PrevRotation.Conjugated() * currentRot;
    }

    m_PrevPosition = currentPos;
    m_PrevRotation = currentRot;
    m_HasPrevious = true;

    // Zero out the root bone so the skeleton stays in place
    pose.Positions[0] = Mathematics::Vector3();
    pose.Rotations[0] = Mathematics::Quaternion::Identity();

    return delta;
}

void RootMotionExtractor::Reset()
{
    m_HasPrevious = false;
    m_PrevPosition = Mathematics::Vector3();
    m_PrevRotation = Mathematics::Quaternion::Identity();
}

} // namespace Animation
} // namespace GameEngine
