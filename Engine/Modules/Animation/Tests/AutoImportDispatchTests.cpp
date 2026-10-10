#include <gtest/gtest.h>

#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include "AssetCore/GUID.h"
#include "Mathematics/Quaternion.h"

#include <cgltf.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

SkeletonData BuildSyntheticGltfSkeleton()
{
    SkeletonData s;
    auto add = [&](const char* name, int32 parent, float x, float y, float z)
    {
        s.BoneNames.emplace_back(name);
        s.Parent.push_back(parent);
        s.RestTranslation.push_back(x); s.RestTranslation.push_back(y); s.RestTranslation.push_back(z);
        s.RestRotation.push_back(0.0f); s.RestRotation.push_back(0.0f);
        s.RestRotation.push_back(0.0f); s.RestRotation.push_back(1.0f);
        s.RestScale.push_back(1.0f); s.RestScale.push_back(1.0f); s.RestScale.push_back(1.0f);
        for (int i = 0; i < 16; ++i)
            s.BindPose.push_back((i % 5 == 0) ? 1.0f : 0.0f);
        s.BindPose[s.BindPose.size() - 4] = x;
        s.BindPose[s.BindPose.size() - 3] = y;
        s.BindPose[s.BindPose.size() - 2] = z;
        s.BoneCount = static_cast<uint32>(s.BoneNames.size());
    };

    // Use Mixamo-style names so the heuristic path picks them up cleanly
    // when no extension is present. The synthetic JSON below references
    // these node names; the SkeletonData maps SkinJointCount = BoneCount
    // and JointNodes = identity.
    add("mixamorig:Hips",          -1, 0.0f, 1.0f, 0.0f);
    add("mixamorig:Spine",          0, 0.0f, 1.15f, 0.0f);
    add("mixamorig:Spine1",         1, 0.0f, 1.30f, 0.0f);
    add("mixamorig:Neck",           2, 0.0f, 1.50f, 0.0f);
    add("mixamorig:Head",           3, 0.0f, 1.60f, 0.0f);
    add("mixamorig:LeftShoulder",   2, 0.05f, 1.48f, 0.0f);
    add("mixamorig:LeftArm",        5, 0.15f, 1.48f, 0.0f);
    add("mixamorig:LeftForeArm",    6, 0.45f, 1.48f, 0.0f);
    add("mixamorig:LeftHand",       7, 0.70f, 1.48f, 0.0f);
    add("mixamorig:RightShoulder",  2, -0.05f, 1.48f, 0.0f);
    add("mixamorig:RightArm",       9, -0.15f, 1.48f, 0.0f);
    add("mixamorig:RightForeArm",  10, -0.45f, 1.48f, 0.0f);
    add("mixamorig:RightHand",     11, -0.70f, 1.48f, 0.0f);
    add("mixamorig:LeftUpLeg",      0, 0.10f, 0.95f, 0.0f);
    add("mixamorig:LeftLeg",       13, 0.10f, 0.50f, 0.0f);
    add("mixamorig:LeftFoot",      14, 0.10f, 0.05f, 0.05f);
    add("mixamorig:RightUpLeg",     0, -0.10f, 0.95f, 0.0f);
    add("mixamorig:RightLeg",      16, -0.10f, 0.50f, 0.0f);
    add("mixamorig:RightFoot",     17, -0.10f, 0.05f, 0.05f);

    s.SkinJointCount = s.BoneCount;
    s.JointNodes.resize(s.BoneCount);
    for (uint32 i = 0; i < s.BoneCount; ++i)
        s.JointNodes[i] = i;

    s.BuildBoneNameLookup();
    return s;
}

void PopulateTPoseProfile(SkeletonProfile& profile)
{
    std::vector<ProfileBone> bones;
    auto add = [&](HumanBone b, HumanBone parent, float x, float y, float z)
    {
        ProfileBone pb;
        pb.Bone = b;
        pb.Parent = parent;
        pb.RestTranslation = Mathematics::Vector3(x, y, z);
        pb.RestRotation = Mathematics::Quaternion::Identity();
        bones.push_back(pb);
    };
    add(HumanBone::Hips,           HumanBone::None, 0.0f, 1.0f, 0.0f);
    add(HumanBone::Spine,          HumanBone::Hips, 0.0f, 0.15f, 0.0f);
    add(HumanBone::Chest,          HumanBone::Spine, 0.0f, 0.15f, 0.0f);
    add(HumanBone::Neck,           HumanBone::Chest, 0.0f, 0.20f, 0.0f);
    add(HumanBone::Head,           HumanBone::Neck, 0.0f, 0.10f, 0.0f);
    add(HumanBone::LeftShoulder,   HumanBone::Chest, 0.05f, 0.18f, 0.0f);
    add(HumanBone::LeftUpperArm,   HumanBone::LeftShoulder, 0.10f, 0.0f, 0.0f);
    add(HumanBone::LeftLowerArm,   HumanBone::LeftUpperArm, 0.30f, 0.0f, 0.0f);
    add(HumanBone::LeftHand,       HumanBone::LeftLowerArm, 0.25f, 0.0f, 0.0f);
    add(HumanBone::RightShoulder,  HumanBone::Chest, -0.05f, 0.18f, 0.0f);
    add(HumanBone::RightUpperArm,  HumanBone::RightShoulder, -0.10f, 0.0f, 0.0f);
    add(HumanBone::RightLowerArm,  HumanBone::RightUpperArm, -0.30f, 0.0f, 0.0f);
    add(HumanBone::RightHand,      HumanBone::RightLowerArm, -0.25f, 0.0f, 0.0f);
    add(HumanBone::LeftUpperLeg,   HumanBone::Hips, 0.10f, -0.05f, 0.0f);
    add(HumanBone::LeftLowerLeg,   HumanBone::LeftUpperLeg, 0.0f, -0.45f, 0.0f);
    add(HumanBone::LeftFoot,       HumanBone::LeftLowerLeg, 0.0f, -0.45f, 0.05f);
    add(HumanBone::RightUpperLeg,  HumanBone::Hips, -0.10f, -0.05f, 0.0f);
    add(HumanBone::RightLowerLeg,  HumanBone::RightUpperLeg, 0.0f, -0.45f, 0.0f);
    add(HumanBone::RightFoot,      HumanBone::RightLowerLeg, 0.0f, -0.45f, 0.05f);
    profile.SetBonesForTest(std::move(bones));
    profile.SetNameForTest("HumanoidStandard");
}

cgltf_data* ParseGltf(const char* json)
{
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const cgltf_result res = cgltf_parse(&options, json, std::strlen(json), &data);
    EXPECT_EQ(res, cgltf_result_success);
    return data;
}

// Bone-list shared between the synthetic JSON variants below — a flat
// 19-node mixamo-named hierarchy.
constexpr const char* kNodesJson =
"  \"nodes\": ["
"    {\"name\": \"mixamorig:Hips\"},          {\"name\": \"mixamorig:Spine\"},"
"    {\"name\": \"mixamorig:Spine1\"},        {\"name\": \"mixamorig:Neck\"},"
"    {\"name\": \"mixamorig:Head\"},          {\"name\": \"mixamorig:LeftShoulder\"},"
"    {\"name\": \"mixamorig:LeftArm\"},       {\"name\": \"mixamorig:LeftForeArm\"},"
"    {\"name\": \"mixamorig:LeftHand\"},      {\"name\": \"mixamorig:RightShoulder\"},"
"    {\"name\": \"mixamorig:RightArm\"},      {\"name\": \"mixamorig:RightForeArm\"},"
"    {\"name\": \"mixamorig:RightHand\"},     {\"name\": \"mixamorig:LeftUpLeg\"},"
"    {\"name\": \"mixamorig:LeftLeg\"},       {\"name\": \"mixamorig:LeftFoot\"},"
"    {\"name\": \"mixamorig:RightUpLeg\"},    {\"name\": \"mixamorig:RightLeg\"},"
"    {\"name\": \"mixamorig:RightFoot\"}"
"  ]";

const std::string kBothExtensionsJson = std::string(R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["VRMC_vrm", "KHR_humanoid"],
  )GLTF") + kNodesJson + R"GLTF(,
  "extensions": {
    "VRMC_vrm": {
      "specVersion": "1.0",
      "humanoid": {
        "humanBones": {
          "hips":          {"node": 0},
          "spine":         {"node": 1},
          "chest":         {"node": 2},
          "neck":          {"node": 3},
          "head":          {"node": 4},
          "leftShoulder":  {"node": 5},
          "leftUpperArm":  {"node": 6},
          "leftLowerArm":  {"node": 7},
          "leftHand":      {"node": 8},
          "rightShoulder": {"node": 9},
          "rightUpperArm": {"node": 10},
          "rightLowerArm": {"node": 11},
          "rightHand":     {"node": 12},
          "leftUpperLeg":  {"node": 13},
          "leftLowerLeg":  {"node": 14},
          "leftFoot":      {"node": 15},
          "rightUpperLeg": {"node": 16},
          "rightLowerLeg": {"node": 17},
          "rightFoot":     {"node": 18}
        }
      }
    },
    "KHR_humanoid": {
      "humanBones": {
        "hips": {"node": 0}, "spine": {"node": 1}, "chest": {"node": 2},
        "neck": {"node": 3}, "head": {"node": 4},
        "leftShoulder": {"node": 5},  "leftUpperArm": {"node": 6},
        "leftLowerArm": {"node": 7},  "leftHand": {"node": 8},
        "rightShoulder": {"node": 9}, "rightUpperArm": {"node": 10},
        "rightLowerArm": {"node": 11},"rightHand": {"node": 12},
        "leftUpperLeg": {"node": 13}, "leftLowerLeg": {"node": 14},
        "leftFoot": {"node": 15},
        "rightUpperLeg": {"node": 16},"rightLowerLeg": {"node": 17},
        "rightFoot": {"node": 18}
      }
    }
  }
}
)GLTF";

const std::string kKHROnlyJson = std::string(R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_humanoid"],
  )GLTF") + kNodesJson + R"GLTF(,
  "extensions": {
    "KHR_humanoid": {
      "humanBones": {
        "hips": {"node": 0}, "spine": {"node": 1}, "chest": {"node": 2},
        "neck": {"node": 3}, "head": {"node": 4},
        "leftShoulder": {"node": 5},  "leftUpperArm": {"node": 6},
        "leftLowerArm": {"node": 7},  "leftHand": {"node": 8},
        "rightShoulder": {"node": 9}, "rightUpperArm": {"node": 10},
        "rightLowerArm": {"node": 11},"rightHand": {"node": 12},
        "leftUpperLeg": {"node": 13}, "leftLowerLeg": {"node": 14},
        "leftFoot": {"node": 15},
        "rightUpperLeg": {"node": 16},"rightLowerLeg": {"node": 17},
        "rightFoot": {"node": 18}
      }
    }
  }
}
)GLTF";

const std::string kPlainJson = std::string(R"GLTF({
  "asset": {"version": "2.0"},
  )GLTF") + kNodesJson + R"GLTF(
}
)GLTF";

} // namespace

TEST(AutoImportDispatch, BothExtensionsTakeVRM1Path)
{
    cgltf_data* gltf = ParseGltf(kBothExtensionsJson.c_str());
    ASSERT_NE(gltf, nullptr);

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    HumanoidImportPath path = HumanoidImportPath::None;
    const bool ok = AutoImportHumanoidRigDispatch(gltf, skel, profile, rig, path);
    EXPECT_TRUE(ok);
    EXPECT_EQ(path, HumanoidImportPath::VRM1);

    cgltf_free(gltf);
}

TEST(AutoImportDispatch, KHROnlyTakesKHRPath)
{
    cgltf_data* gltf = ParseGltf(kKHROnlyJson.c_str());
    ASSERT_NE(gltf, nullptr);

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    HumanoidImportPath path = HumanoidImportPath::None;
    const bool ok = AutoImportHumanoidRigDispatch(gltf, skel, profile, rig, path);
    EXPECT_TRUE(ok);
    EXPECT_EQ(path, HumanoidImportPath::KHRHumanoid);

    cgltf_free(gltf);
}

TEST(AutoImportDispatch, PlainGltfTakesHeuristicPath)
{
    cgltf_data* gltf = ParseGltf(kPlainJson.c_str());
    ASSERT_NE(gltf, nullptr);

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    HumanoidImportPath path = HumanoidImportPath::None;
    const bool ok = AutoImportHumanoidRigDispatch(gltf, skel, profile, rig, path);
    EXPECT_TRUE(ok);
    EXPECT_EQ(path, HumanoidImportPath::Heuristic);

    cgltf_free(gltf);
}

TEST(AutoImportDispatch, NullCgltfFallsThroughToHeuristic)
{
    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    HumanoidImportPath path = HumanoidImportPath::None;
    // No gltf -> dispatch falls straight to the heuristic path. Mixamo
    // skeleton names match the regex matcher cleanly.
    const bool ok = AutoImportHumanoidRigDispatch(nullptr, skel, profile, rig, path);
    EXPECT_TRUE(ok);
    EXPECT_EQ(path, HumanoidImportPath::Heuristic);
}

TEST(AutoImportDispatch, ImportPathToStringIsStable)
{
    EXPECT_STREQ(HumanoidImportPathToString(HumanoidImportPath::VRM1), "VRM1");
    EXPECT_STREQ(HumanoidImportPathToString(HumanoidImportPath::KHRHumanoid), "KHRHumanoid");
    EXPECT_STREQ(HumanoidImportPathToString(HumanoidImportPath::Heuristic), "Heuristic");
    EXPECT_STREQ(HumanoidImportPathToString(HumanoidImportPath::None), "None");
}
