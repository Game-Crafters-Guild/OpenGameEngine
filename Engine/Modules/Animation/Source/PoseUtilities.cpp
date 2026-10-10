#include "Animation/PoseUtilities.h"
#include "Animation/AnimationPose.h"

namespace GameEngine::Animation::PoseUtilities
{

void ComputeWorldSpace(
    const AnimationPose& localPose,
    const std::vector<int32_t>& parentIndices,
    WorldSpacePose& outWorld)
{
    const uint32_t boneCount = localPose.BoneCount;

    if (outWorld.Positions.size() < boneCount)
        outWorld.Positions.resize(boneCount);
    if (outWorld.Rotations.size() < boneCount)
        outWorld.Rotations.resize(boneCount);

    for (uint32_t i = 0; i < boneCount; ++i)
    {
        const int32_t parent = (i < parentIndices.size()) ? parentIndices[i] : -1;
        if (parent < 0 || static_cast<uint32_t>(parent) >= boneCount)
        {
            outWorld.Positions[i] = localPose.Positions[i];
            outWorld.Rotations[i] = localPose.Rotations[i];
        }
        else
        {
            outWorld.Rotations[i] = outWorld.Rotations[parent] * localPose.Rotations[i];
            outWorld.Positions[i] = outWorld.Positions[parent] +
                                    outWorld.Rotations[parent].Rotate(localPose.Positions[i]);
        }
    }
}

WorldSpacePose ComputeWorldSpace(
    const AnimationPose& localPose,
    const std::vector<int32_t>& parentIndices)
{
    WorldSpacePose world;
    ComputeWorldSpace(localPose, parentIndices, world);
    return world;
}

Mathematics::Quaternion WorldToLocalRotation(
    const Mathematics::Quaternion& worldRotation,
    const Mathematics::Quaternion& parentWorldRotation)
{
    // localRot = inverse(parentWorldRot) * worldRot
    return (parentWorldRotation.Conjugated() * worldRotation).Normalized();
}

Mathematics::Vector3 WorldToLocalPosition(
    const Mathematics::Vector3& worldPosition,
    const Mathematics::Vector3& parentWorldPosition,
    const Mathematics::Quaternion& parentWorldRotation)
{
    // localPos = inverse(parentWorldRot) * (worldPos - parentWorldPos)
    Mathematics::Vector3 offset = worldPosition - parentWorldPosition;
    return parentWorldRotation.Conjugated().Rotate(offset);
}

} // namespace GameEngine::Animation::PoseUtilities
