#include "Animation/HumanBone.h"

#include <array>

namespace GameEngine
{
namespace Animation
{

namespace
{

constexpr std::array<const char*, static_cast<size_t>(HumanBone::Count)> kHumanBoneNames = {
    "None",

    // Body
    "Hips",
    "Spine",
    "Chest",
    "UpperChest",
    "Neck",
    "Head",

    "LeftShoulder",
    "LeftUpperArm",
    "LeftLowerArm",
    "LeftHand",

    "RightShoulder",
    "RightUpperArm",
    "RightLowerArm",
    "RightHand",

    "LeftUpperLeg",
    "LeftLowerLeg",
    "LeftFoot",
    "LeftToes",

    "RightUpperLeg",
    "RightLowerLeg",
    "RightFoot",
    "RightToes",

    "LeftEye",
    "RightEye",
    "Jaw",

    // Fingers
    "LeftThumbProximal",
    "LeftThumbIntermediate",
    "LeftThumbDistal",
    "LeftIndexProximal",
    "LeftIndexIntermediate",
    "LeftIndexDistal",
    "LeftMiddleProximal",
    "LeftMiddleIntermediate",
    "LeftMiddleDistal",
    "LeftRingProximal",
    "LeftRingIntermediate",
    "LeftRingDistal",
    "LeftLittleProximal",
    "LeftLittleIntermediate",
    "LeftLittleDistal",

    "RightThumbProximal",
    "RightThumbIntermediate",
    "RightThumbDistal",
    "RightIndexProximal",
    "RightIndexIntermediate",
    "RightIndexDistal",
    "RightMiddleProximal",
    "RightMiddleIntermediate",
    "RightMiddleDistal",
    "RightRingProximal",
    "RightRingIntermediate",
    "RightRingDistal",
    "RightLittleProximal",
    "RightLittleIntermediate",
    "RightLittleDistal",
};

} // namespace

const char* HumanBoneToString(HumanBone bone)
{
    const auto idx = static_cast<size_t>(bone);
    if (idx >= kHumanBoneNames.size())
    {
        return "None";
    }
    return kHumanBoneNames[idx];
}

HumanBone HumanBoneFromString(std::string_view name)
{
    for (size_t i = 0; i < kHumanBoneNames.size(); ++i)
    {
        if (name == kHumanBoneNames[i])
        {
            return static_cast<HumanBone>(i);
        }
    }
    return HumanBone::None;
}

} // namespace Animation
} // namespace GameEngine
