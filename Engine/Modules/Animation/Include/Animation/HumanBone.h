#pragma once

#include "Types/Types.h"

#include <string_view>

namespace GameEngine
{
namespace Animation
{

// Canonical humanoid bone identity. Closed enum: every supported anatomical
// slot is enumerated once. None is the sentinel and is always 0 so default-
// constructed slots resolve to "unmapped".
//
// Layout:
//   0       : None
//   1..24   : Body bones (24)
//   25..54  : Finger bones (Left/Right x Thumb/Index/Middle/Ring/Little x
//             Proximal/Intermediate/Distal = 30)
//   Count   : One past the last valid bone
//
// New entries are appended; never reorder, never reassign integer values.
enum class HumanBone : uint8
{
    None = 0,

    // Body (24)
    Hips,
    Spine,
    Chest,
    UpperChest,
    Neck,
    Head,

    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,

    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,

    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,

    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,

    LeftEye,
    RightEye,
    Jaw,

    // Fingers (30): Left/Right x Thumb/Index/Middle/Ring/Little x Proximal/Intermediate/Distal
    LeftThumbProximal,
    LeftThumbIntermediate,
    LeftThumbDistal,
    LeftIndexProximal,
    LeftIndexIntermediate,
    LeftIndexDistal,
    LeftMiddleProximal,
    LeftMiddleIntermediate,
    LeftMiddleDistal,
    LeftRingProximal,
    LeftRingIntermediate,
    LeftRingDistal,
    LeftLittleProximal,
    LeftLittleIntermediate,
    LeftLittleDistal,

    RightThumbProximal,
    RightThumbIntermediate,
    RightThumbDistal,
    RightIndexProximal,
    RightIndexIntermediate,
    RightIndexDistal,
    RightMiddleProximal,
    RightMiddleIntermediate,
    RightMiddleDistal,
    RightRingProximal,
    RightRingIntermediate,
    RightRingDistal,
    RightLittleProximal,
    RightLittleIntermediate,
    RightLittleDistal,

    Count
};

// Returns a stable C-string name for the bone. The returned pointer is valid
// for the program lifetime. Used by JSON serialization and inspectors.
const char* HumanBoneToString(HumanBone bone);

// Inverse of HumanBoneToString. Unknown names resolve to HumanBone::None.
// Comparison is case-sensitive (matches the canonical CamelCase output).
HumanBone HumanBoneFromString(std::string_view name);

} // namespace Animation
} // namespace GameEngine
