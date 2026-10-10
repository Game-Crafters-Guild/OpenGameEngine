#include "Animation/KHRHumanoidImporter.h"

#include "KHRHumanoidShared.h"
#include "Logger/Logger.h"

#include <cgltf.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <exception>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace Animation
{

namespace
{

// VRM 1.0 / KHR_humanoid bone-name table. Names are camelCase per VRM 1.0
// spec; the table is intentionally exhaustive (24 body + 30 fingers + eye/jaw
// = 55 entries) so an unknown name in the JSON gets logged as a parse error.
struct KHRBoneRow
{
    const char* name;
    HumanBone bone;
};

constexpr KHRBoneRow kBoneTable[] = {
    {"hips",                   HumanBone::Hips},
    {"spine",                  HumanBone::Spine},
    {"chest",                  HumanBone::Chest},
    {"upperChest",             HumanBone::UpperChest},
    {"neck",                   HumanBone::Neck},
    {"head",                   HumanBone::Head},

    {"leftShoulder",           HumanBone::LeftShoulder},
    {"leftUpperArm",           HumanBone::LeftUpperArm},
    {"leftLowerArm",           HumanBone::LeftLowerArm},
    {"leftHand",               HumanBone::LeftHand},

    {"rightShoulder",          HumanBone::RightShoulder},
    {"rightUpperArm",          HumanBone::RightUpperArm},
    {"rightLowerArm",          HumanBone::RightLowerArm},
    {"rightHand",              HumanBone::RightHand},

    {"leftUpperLeg",           HumanBone::LeftUpperLeg},
    {"leftLowerLeg",           HumanBone::LeftLowerLeg},
    {"leftFoot",               HumanBone::LeftFoot},
    {"leftToes",               HumanBone::LeftToes},

    {"rightUpperLeg",          HumanBone::RightUpperLeg},
    {"rightLowerLeg",          HumanBone::RightLowerLeg},
    {"rightFoot",              HumanBone::RightFoot},
    {"rightToes",              HumanBone::RightToes},

    {"leftEye",                HumanBone::LeftEye},
    {"rightEye",               HumanBone::RightEye},
    {"jaw",                    HumanBone::Jaw},

    {"leftThumbProximal",      HumanBone::LeftThumbProximal},
    {"leftThumbIntermediate",  HumanBone::LeftThumbIntermediate},
    {"leftThumbDistal",        HumanBone::LeftThumbDistal},
    {"leftIndexProximal",      HumanBone::LeftIndexProximal},
    {"leftIndexIntermediate",  HumanBone::LeftIndexIntermediate},
    {"leftIndexDistal",        HumanBone::LeftIndexDistal},
    {"leftMiddleProximal",     HumanBone::LeftMiddleProximal},
    {"leftMiddleIntermediate", HumanBone::LeftMiddleIntermediate},
    {"leftMiddleDistal",       HumanBone::LeftMiddleDistal},
    {"leftRingProximal",       HumanBone::LeftRingProximal},
    {"leftRingIntermediate",   HumanBone::LeftRingIntermediate},
    {"leftRingDistal",         HumanBone::LeftRingDistal},
    {"leftLittleProximal",     HumanBone::LeftLittleProximal},
    {"leftLittleIntermediate", HumanBone::LeftLittleIntermediate},
    {"leftLittleDistal",       HumanBone::LeftLittleDistal},

    {"rightThumbProximal",     HumanBone::RightThumbProximal},
    {"rightThumbIntermediate", HumanBone::RightThumbIntermediate},
    {"rightThumbDistal",       HumanBone::RightThumbDistal},
    {"rightIndexProximal",     HumanBone::RightIndexProximal},
    {"rightIndexIntermediate", HumanBone::RightIndexIntermediate},
    {"rightIndexDistal",       HumanBone::RightIndexDistal},
    {"rightMiddleProximal",    HumanBone::RightMiddleProximal},
    {"rightMiddleIntermediate",HumanBone::RightMiddleIntermediate},
    {"rightMiddleDistal",      HumanBone::RightMiddleDistal},
    {"rightRingProximal",      HumanBone::RightRingProximal},
    {"rightRingIntermediate",  HumanBone::RightRingIntermediate},
    {"rightRingDistal",        HumanBone::RightRingDistal},
    {"rightLittleProximal",    HumanBone::RightLittleProximal},
    {"rightLittleIntermediate",HumanBone::RightLittleIntermediate},
    {"rightLittleDistal",      HumanBone::RightLittleDistal},
};

} // namespace

HumanBone HumanBoneFromKHRName(std::string_view khrName)
{
    for (const auto& row : kBoneTable)
    {
        if (khrName == row.name) return row.bone;
    }
    return HumanBone::None;
}

bool HasKHRHumanoidExtension(const cgltf_data* gltf)
{
    return KHRDetail::FindRootExtension(gltf, "KHR_humanoid") != nullptr;
}

bool ImportFromKHRHumanoid(const cgltf_data* gltf,
                           const SkeletonData& skel,
                           const SkeletonProfile& profile,
                           HumanoidRig& outRig)
{
    if (!gltf)
    {
        Logger::Log::Warning("ImportFromKHRHumanoid: null cgltf_data");
        return false;
    }

    const cgltf_extension* ext = KHRDetail::FindRootExtension(gltf, "KHR_humanoid");
    if (!ext)
        return false; // caller falls back to other paths

    if (!ext->data || std::strlen(ext->data) == 0)
    {
        Logger::Log::Warning("ImportFromKHRHumanoid: extension present but data empty");
        return false;
    }

    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(ext->data);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("ImportFromKHRHumanoid: JSON parse failed: {}", e.what());
        return false;
    }

    if (!root.contains("humanBones"))
    {
        Logger::Log::Warning("ImportFromKHRHumanoid: extension missing 'humanBones'");
        return false;
    }

    return KHRDetail::ImportFromHumanBonesJson(root["humanBones"], gltf, skel, profile, outRig);
}

} // namespace Animation
} // namespace GameEngine
