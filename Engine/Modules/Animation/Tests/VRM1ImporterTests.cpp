#include <gtest/gtest.h>

#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"
#include "Animation/VRM1Importer.h"

#include "AssetCore/GUID.h"
#include "Mathematics/Quaternion.h"

#include <cgltf.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
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
        s.RestTranslation.push_back(x);
        s.RestTranslation.push_back(y);
        s.RestTranslation.push_back(z);
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

    add("Hips",           -1, 0.0f, 1.0f,  0.0f);
    add("Spine",           0, 0.0f, 1.15f, 0.0f);
    add("Chest",           1, 0.0f, 1.30f, 0.0f);
    add("Neck",            2, 0.0f, 1.50f, 0.0f);
    add("Head",            3, 0.0f, 1.60f, 0.0f);
    add("LeftShoulder",    2, 0.05f, 1.48f, 0.0f);
    add("LeftUpperArm",    5, 0.15f, 1.48f, 0.0f);
    add("LeftLowerArm",    6, 0.45f, 1.48f, 0.0f);
    add("LeftHand",        7, 0.70f, 1.48f, 0.0f);
    add("RightShoulder",   2, -0.05f, 1.48f, 0.0f);
    add("RightUpperArm",   9, -0.15f, 1.48f, 0.0f);
    add("RightLowerArm",  10, -0.45f, 1.48f, 0.0f);
    add("RightHand",      11, -0.70f, 1.48f, 0.0f);
    add("LeftUpperLeg",    0, 0.10f, 0.95f, 0.0f);
    add("LeftLowerLeg",   13, 0.10f, 0.50f, 0.0f);
    add("LeftFoot",       14, 0.10f, 0.05f, 0.05f);
    add("RightUpperLeg",   0, -0.10f, 0.95f, 0.0f);
    add("RightLowerLeg",  16, -0.10f, 0.50f, 0.0f);
    add("RightFoot",      17, -0.10f, 0.05f, 0.05f);

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

// VRM 1.0 (`VRMC_vrm`) sample: 19-bone humanoid with humanoid + lookAt +
// firstPerson + expressions metadata.
const char* kVRM1Json = R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["VRMC_vrm"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Hips"},          {"name": "Spine"},
    {"name": "Chest"},         {"name": "Neck"},
    {"name": "Head"},          {"name": "LeftShoulder"},
    {"name": "LeftUpperArm"},  {"name": "LeftLowerArm"},
    {"name": "LeftHand"},      {"name": "RightShoulder"},
    {"name": "RightUpperArm"}, {"name": "RightLowerArm"},
    {"name": "RightHand"},     {"name": "LeftUpperLeg"},
    {"name": "LeftLowerLeg"},  {"name": "LeftFoot"},
    {"name": "RightUpperLeg"}, {"name": "RightLowerLeg"},
    {"name": "RightFoot"}
  ],
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
      },
      "firstPerson": {
        "meshAnnotations": [
          {"node": 0, "type": "auto"}
        ]
      },
      "lookAt": {
        "type": "bone",
        "offsetFromHeadBone": [0.0, 0.06, 0.0]
      },
      "expressions": {
        "preset": {
          "happy":   {},
          "angry":   {},
          "sad":     {}
        }
      }
    }
  }
}
)GLTF";

// VRM 0.x sample. Bone array shape (NOT object) + `VRM` extension name.
const char* kVRM0Json = R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["VRM"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Hips"},          {"name": "Spine"},
    {"name": "Chest"},         {"name": "Neck"},
    {"name": "Head"},          {"name": "LeftShoulder"},
    {"name": "LeftUpperArm"},  {"name": "LeftLowerArm"},
    {"name": "LeftHand"},      {"name": "RightShoulder"},
    {"name": "RightUpperArm"}, {"name": "RightLowerArm"},
    {"name": "RightHand"},     {"name": "LeftUpperLeg"},
    {"name": "LeftLowerLeg"},  {"name": "LeftFoot"},
    {"name": "RightUpperLeg"}, {"name": "RightLowerLeg"},
    {"name": "RightFoot"}
  ],
  "extensions": {
    "VRM": {
      "specVersion": "0.0",
      "humanoid": {
        "humanBones": [
          {"bone": "hips",          "node": 0},
          {"bone": "spine",         "node": 1},
          {"bone": "chest",         "node": 2},
          {"bone": "neck",          "node": 3},
          {"bone": "head",          "node": 4},
          {"bone": "leftShoulder",  "node": 5},
          {"bone": "leftUpperArm",  "node": 6},
          {"bone": "leftLowerArm",  "node": 7},
          {"bone": "leftHand",      "node": 8},
          {"bone": "rightShoulder", "node": 9},
          {"bone": "rightUpperArm", "node": 10},
          {"bone": "rightLowerArm", "node": 11},
          {"bone": "rightHand",     "node": 12},
          {"bone": "leftUpperLeg",  "node": 13},
          {"bone": "leftLowerLeg",  "node": 14},
          {"bone": "leftFoot",      "node": 15},
          {"bone": "rightUpperLeg", "node": 16},
          {"bone": "rightLowerLeg", "node": 17},
          {"bone": "rightFoot",     "node": 18}
        ]
      }
    }
  }
}
)GLTF";

// VRM 1.0 with NO humanoid block (importer must reject).
const char* kVRM1MissingHumanoidJson = R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["VRMC_vrm"],
  "nodes": [{"name": "Hips"}],
  "extensions": {"VRMC_vrm": {"specVersion": "1.0"}}
}
)GLTF";

} // namespace

TEST(VRM1Importer, ImportsVRM1AndCapturesMetadata)
{
    cgltf_data* gltf = ParseGltf(kVRM1Json);
    ASSERT_NE(gltf, nullptr);

    EXPECT_TRUE(HasVRMExtension(gltf));

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    VRM1Metadata meta;
    const bool ok = ImportFromVRM1(gltf, skel, profile, rig, meta);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(meta.IsLegacyVRM0);
    EXPECT_EQ(meta.SpecVersion, "1.0");

    // Metadata captured (data flow only — semantics live in face/lookAt
    // runtimes that aren't part of Phase 10).
    EXPECT_TRUE(meta.HasLookAt);
    EXPECT_NEAR(meta.LookAtSettings.OffsetFromHeadY, 0.06f, 1e-4f);
    EXPECT_FALSE(meta.FirstPersonMeshAnnotations.empty());
    EXPECT_EQ(meta.FirstPersonMeshAnnotations[0].Type, "auto");
    EXPECT_EQ(meta.Expressions.size(), 3u);

    // Rig populated through the shared parse path.
    auto findBone = [&](HumanBone b) -> const HumanoidBoneMapping*
    {
        for (const auto& m : rig.BoneMap())
            if (m.Canonical == b) return &m;
        return nullptr;
    };
    EXPECT_NE(findBone(HumanBone::Hips), nullptr);
    EXPECT_NE(findBone(HumanBone::LeftHand), nullptr);
    EXPECT_NE(findBone(HumanBone::RightFoot), nullptr);

    cgltf_free(gltf);
}

TEST(VRM1Importer, DetectsVRM0AndAxisFlips)
{
    cgltf_data* gltf = ParseGltf(kVRM0Json);
    ASSERT_NE(gltf, nullptr);

    EXPECT_TRUE(HasVRMExtension(gltf));

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    VRM1Metadata meta;
    const bool ok = ImportFromVRM1(gltf, skel, profile, rig, meta);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(meta.IsLegacyVRM0);
    EXPECT_EQ(meta.SpecVersion, "0.x");

    // The shared importer was invoked with a Y-flipped skeleton; verify
    // the conversion via ApplyVRM0AxisFlip is bit-stable + idempotent.
    SkeletonData flipped = ApplyVRM0AxisFlip(skel);
    SkeletonData flippedTwice = ApplyVRM0AxisFlip(flipped);
    // Y-axis flip applied twice equals identity ROTATION (q and -q encode
    // the same rotation; the double-flip lands on the antipode of the
    // original quaternion). Accept either sign.
    ASSERT_EQ(flippedTwice.RestRotation.size(), skel.RestRotation.size());
    ASSERT_EQ(skel.RestRotation.size() % 4, 0u);
    for (size_t b = 0; b * 4 < skel.RestRotation.size(); ++b)
    {
        const float* a = &flippedTwice.RestRotation[b * 4];
        const float* o = &skel.RestRotation[b * 4];
        // Per-component absolute equality OR per-component equality after
        // negating the whole quaternion.
        const bool sameSign     = std::abs(a[0] - o[0]) < 1e-4f && std::abs(a[1] - o[1]) < 1e-4f
                               && std::abs(a[2] - o[2]) < 1e-4f && std::abs(a[3] - o[3]) < 1e-4f;
        const bool oppositeSign = std::abs(a[0] + o[0]) < 1e-4f && std::abs(a[1] + o[1]) < 1e-4f
                               && std::abs(a[2] + o[2]) < 1e-4f && std::abs(a[3] + o[3]) < 1e-4f;
        EXPECT_TRUE(sameSign || oppositeSign) << "bone " << b
            << " components: (" << a[0] << "," << a[1] << "," << a[2] << "," << a[3]
            << ") vs (" << o[0] << "," << o[1] << "," << o[2] << "," << o[3] << ")";
    }

    // The flipped (single-flip) rotation should differ from the source
    // unless the source is already at identity (the test rig has identity
    // rest rotations, so the flipped result is the +Y 180-deg quat).
    // Spot-check: bone 0's rest rotation flipped once should be (0,1,0,0).
    ASSERT_GE(flipped.RestRotation.size(), 4u);
    EXPECT_NEAR(flipped.RestRotation[0], 0.0f, 1e-4f);
    EXPECT_NEAR(flipped.RestRotation[1], 1.0f, 1e-4f);
    EXPECT_NEAR(flipped.RestRotation[2], 0.0f, 1e-4f);
    EXPECT_NEAR(flipped.RestRotation[3], 0.0f, 1e-4f);

    cgltf_free(gltf);
}

TEST(VRM1Importer, MissingHumanoidReturnsFalse)
{
    cgltf_data* gltf = ParseGltf(kVRM1MissingHumanoidJson);
    ASSERT_NE(gltf, nullptr);

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    VRM1Metadata meta;
    EXPECT_FALSE(ImportFromVRM1(gltf, skel, profile, rig, meta));

    cgltf_free(gltf);
}

TEST(VRM1Importer, NullCgltfDataReturnsFalse)
{
    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path{});
    VRM1Metadata meta;
    EXPECT_FALSE(ImportFromVRM1(nullptr, skel, profile, rig, meta));
    EXPECT_FALSE(HasVRMExtension(nullptr));
}

TEST(VRM1Importer, AxisFlipPreservesBoneCount)
{
    // The flip helper must not alter array sizes, parent links, bone
    // names, or any non-RestRotation field.
    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonData flipped = ApplyVRM0AxisFlip(skel);

    EXPECT_EQ(flipped.BoneCount, skel.BoneCount);
    EXPECT_EQ(flipped.BoneNames.size(), skel.BoneNames.size());
    EXPECT_EQ(flipped.Parent.size(), skel.Parent.size());
    EXPECT_EQ(flipped.RestTranslation.size(), skel.RestTranslation.size());
    EXPECT_EQ(flipped.RestScale.size(), skel.RestScale.size());

    for (size_t i = 0; i < skel.RestTranslation.size(); ++i)
        EXPECT_EQ(flipped.RestTranslation[i], skel.RestTranslation[i]);
    for (size_t i = 0; i < skel.Parent.size(); ++i)
        EXPECT_EQ(flipped.Parent[i], skel.Parent[i]);
    for (size_t i = 0; i < skel.BoneNames.size(); ++i)
        EXPECT_EQ(flipped.BoneNames[i], skel.BoneNames[i]);
}
