#include <gtest/gtest.h>

#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/KHRHumanoidImporter.h"
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

// Build a SkeletonData with a 19-bone humanoid hierarchy that matches the
// node ordering used by the synthetic glTF JSON below. The skeleton's bind
// pose stores rest world translations so the importer can compute body
// proportions.
SkeletonData BuildSyntheticGltfSkeleton()
{
    SkeletonData s;
    // Each bone is added with its LOCAL (parent-relative) translation. The
    // BindPose matrix stores the cumulative WORLD translation, computed
    // alongside as we walk in parent-before-child order.
    std::vector<Mathematics::Vector3> worldByBone;
    auto add = [&](const char* name, int32 parent, float lx, float ly, float lz)
    {
        s.BoneNames.emplace_back(name);
        s.Parent.push_back(parent);
        s.RestTranslation.push_back(lx);
        s.RestTranslation.push_back(ly);
        s.RestTranslation.push_back(lz);
        s.RestRotation.push_back(0.0f); s.RestRotation.push_back(0.0f);
        s.RestRotation.push_back(0.0f); s.RestRotation.push_back(1.0f);
        s.RestScale.push_back(1.0f); s.RestScale.push_back(1.0f); s.RestScale.push_back(1.0f);
        // World position = parent world + local. Identity rotations
        // throughout, so no rotation compose needed.
        const Mathematics::Vector3 parentWorld = (parent < 0)
            ? Mathematics::Vector3(0.0f, 0.0f, 0.0f)
            : worldByBone[static_cast<size_t>(parent)];
        const Mathematics::Vector3 ownWorld(parentWorld.x + lx,
                                              parentWorld.y + ly,
                                              parentWorld.z + lz);
        worldByBone.push_back(ownWorld);
        // BindPose is column-major identity with the bone-world translation
        // in column 3 (entries [12..14]).
        for (int i = 0; i < 16; ++i)
            s.BindPose.push_back((i % 5 == 0) ? 1.0f : 0.0f);
        s.BindPose[s.BindPose.size() - 4] = ownWorld.x;
        s.BindPose[s.BindPose.size() - 3] = ownWorld.y;
        s.BindPose[s.BindPose.size() - 2] = ownWorld.z;
        s.BoneCount = static_cast<uint32>(s.BoneNames.size());
    };

    // Canonical T-pose layout. Spine chain along +Y; arms along ±X (level
    // with the shoulders); legs along -Y. Local translations only — the
    // FK walk composes them into the world positions stored in BindPose.
    add("Hips",           -1, 0.00f,  1.00f, 0.00f);  // 0  world (0, 1, 0)
    add("Spine",           0, 0.00f,  0.15f, 0.00f);  // 1  world (0, 1.15, 0)
    add("Chest",           1, 0.00f,  0.15f, 0.00f);  // 2  world (0, 1.30, 0)
    add("Neck",            2, 0.00f,  0.20f, 0.00f);  // 3  world (0, 1.50, 0)
    add("Head",            3, 0.00f,  0.10f, 0.00f);  // 4  world (0, 1.60, 0)
    add("LeftShoulder",    2, 0.05f,  0.18f, 0.00f);  // 5  world (0.05, 1.48, 0)
    add("LeftUpperArm",    5, 0.10f,  0.00f, 0.00f);  // 6
    add("LeftLowerArm",    6, 0.30f,  0.00f, 0.00f);  // 7
    add("LeftHand",        7, 0.25f,  0.00f, 0.00f);  // 8
    add("RightShoulder",   2,-0.05f,  0.18f, 0.00f);  // 9
    add("RightUpperArm",   9,-0.10f,  0.00f, 0.00f);  // 10
    add("RightLowerArm",  10,-0.30f,  0.00f, 0.00f);  // 11
    add("RightHand",      11,-0.25f,  0.00f, 0.00f);  // 12
    add("LeftUpperLeg",    0, 0.10f, -0.05f, 0.00f);  // 13
    add("LeftLowerLeg",   13, 0.00f, -0.45f, 0.00f);  // 14
    add("LeftFoot",       14, 0.00f, -0.45f, 0.00f);  // 15  pure -Y (no toe-forward offset)
    add("RightUpperLeg",   0,-0.10f, -0.05f, 0.00f);  // 16
    add("RightLowerLeg",  16, 0.00f, -0.45f, 0.00f);  // 17
    add("RightFoot",      17, 0.00f, -0.45f, 0.00f);  // 18  pure -Y

    // Skin joint identity mapping.
    s.SkinJointCount = s.BoneCount;
    s.JointNodes.resize(s.BoneCount);
    for (uint32 i = 0; i < s.BoneCount; ++i)
        s.JointNodes[i] = i;

    s.BuildBoneNameLookup();
    return s;
}

// Synthetic 19-node glTF JSON with KHR_humanoid extension. The node names
// match the SkeletonData order so the synthetic skeleton's joint mapping
// resolves cleanly. ~100 lines.
const char* kKHRSampleJson = R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_humanoid"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Hips",          "children": [1, 13, 16]},
    {"name": "Spine",         "children": [2]},
    {"name": "Chest",         "children": [3, 5, 9]},
    {"name": "Neck",          "children": [4]},
    {"name": "Head"},
    {"name": "LeftShoulder",  "children": [6]},
    {"name": "LeftUpperArm",  "children": [7]},
    {"name": "LeftLowerArm",  "children": [8]},
    {"name": "LeftHand"},
    {"name": "RightShoulder", "children": [10]},
    {"name": "RightUpperArm", "children": [11]},
    {"name": "RightLowerArm", "children": [12]},
    {"name": "RightHand"},
    {"name": "LeftUpperLeg",  "children": [14]},
    {"name": "LeftLowerLeg",  "children": [15]},
    {"name": "LeftFoot"},
    {"name": "RightUpperLeg", "children": [17]},
    {"name": "RightLowerLeg", "children": [18]},
    {"name": "RightFoot"}
  ],
  "extensions": {
    "KHR_humanoid": {
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
  }
}
)GLTF";

// glTF JSON with NO extensions (the importer must return false).
const char* kPlainGltfJson = R"GLTF({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Hips"}]
}
)GLTF";

// glTF JSON with KHR_humanoid but bone names that don't match the schema
// (the importer skips unknown keys with a warning, ends up under coverage).
const char* kKHRBadNamesJson = R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_humanoid"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Hips"},
    {"name": "Spine"}
  ],
  "extensions": {
    "KHR_humanoid": {
      "humanBones": {
        "weirdBoneName1": {"node": 0},
        "weirdBoneName2": {"node": 1}
      }
    }
  }
}
)GLTF";

cgltf_data* ParseGltf(const char* json)
{
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const cgltf_result res = cgltf_parse(&options, json, std::strlen(json), &data);
    EXPECT_EQ(res, cgltf_result_success);
    return data;
}

void PopulateTPoseProfile(SkeletonProfile& profile)
{
    std::vector<ProfileBone> bones;
    // LocalForward matches the canonical T-pose direction the bone's primary
    // axis points in. For the synthetic skeleton (BuildSyntheticGltfSkeleton)
    // these match the world-space segment directions so the chain-aware Q
    // bake collapses to identity for the trivial "rig already at canonical
    // T-pose" case. Mirrors the production HumanoidStandard.profile.json.
    auto add = [&](HumanBone b, HumanBone parent, float x, float y, float z,
                   float fx, float fy, float fz)
    {
        ProfileBone pb;
        pb.Bone = b;
        pb.Parent = parent;
        pb.RestTranslation = Mathematics::Vector3(x, y, z);
        pb.RestRotation = Mathematics::Quaternion::Identity();
        pb.LocalForward = Mathematics::Vector3(fx, fy, fz);
        bones.push_back(pb);
    };
    // Spine chain points +Y (head-up).
    add(HumanBone::Hips,           HumanBone::None, 0.0f, 1.0f, 0.0f,           0, 1, 0);
    add(HumanBone::Spine,          HumanBone::Hips, 0.0f, 0.15f, 0.0f,          0, 1, 0);
    add(HumanBone::Chest,          HumanBone::Spine, 0.0f, 0.15f, 0.0f,         0, 1, 0);
    add(HumanBone::Neck,           HumanBone::Chest, 0.0f, 0.20f, 0.0f,         0, 1, 0);
    add(HumanBone::Head,           HumanBone::Neck, 0.0f, 0.10f, 0.0f,          0, 1, 0);
    // Left arm points +X (T-pose, synthetic skeleton's arm bones lie along +X).
    add(HumanBone::LeftShoulder,   HumanBone::Chest, 0.05f, 0.18f, 0.0f,        1, 0, 0);
    add(HumanBone::LeftUpperArm,   HumanBone::LeftShoulder, 0.10f, 0.0f, 0.0f,  1, 0, 0);
    add(HumanBone::LeftLowerArm,   HumanBone::LeftUpperArm, 0.30f, 0.0f, 0.0f,  1, 0, 0);
    add(HumanBone::LeftHand,       HumanBone::LeftLowerArm, 0.25f, 0.0f, 0.0f,  1, 0, 0);
    // Right arm points -X.
    add(HumanBone::RightShoulder,  HumanBone::Chest, -0.05f, 0.18f, 0.0f,       -1, 0, 0);
    add(HumanBone::RightUpperArm,  HumanBone::RightShoulder, -0.10f, 0.0f, 0.0f, -1, 0, 0);
    add(HumanBone::RightLowerArm,  HumanBone::RightUpperArm, -0.30f, 0.0f, 0.0f, -1, 0, 0);
    add(HumanBone::RightHand,      HumanBone::RightLowerArm, -0.25f, 0.0f, 0.0f, -1, 0, 0);
    // Legs point -Y.
    add(HumanBone::LeftUpperLeg,   HumanBone::Hips, 0.10f, -0.05f, 0.0f,        0, -1, 0);
    add(HumanBone::LeftLowerLeg,   HumanBone::LeftUpperLeg, 0.0f, -0.45f, 0.0f, 0, -1, 0);
    add(HumanBone::LeftFoot,       HumanBone::LeftLowerLeg, 0.0f, -0.45f, 0.05f, 0, -1, 0);
    add(HumanBone::RightUpperLeg,  HumanBone::Hips, -0.10f, -0.05f, 0.0f,       0, -1, 0);
    add(HumanBone::RightLowerLeg,  HumanBone::RightUpperLeg, 0.0f, -0.45f, 0.0f, 0, -1, 0);
    add(HumanBone::RightFoot,      HumanBone::RightLowerLeg, 0.0f, -0.45f, 0.05f, 0, -1, 0);
    profile.SetBonesForTest(std::move(bones));
    profile.SetNameForTest("HumanoidStandard");
}

} // namespace

TEST(KHRHumanoidImporter, BoneNameTable_AllCanonicalBonesMapped)
{
    // Round-trip every camelCase name we declare back to its canonical
    // HumanBone enum. Catches table edits that drop a row.
    EXPECT_EQ(HumanBoneFromKHRName("hips"), HumanBone::Hips);
    EXPECT_EQ(HumanBoneFromKHRName("spine"), HumanBone::Spine);
    EXPECT_EQ(HumanBoneFromKHRName("leftUpperArm"), HumanBone::LeftUpperArm);
    EXPECT_EQ(HumanBoneFromKHRName("rightLittleDistal"), HumanBone::RightLittleDistal);
    EXPECT_EQ(HumanBoneFromKHRName("jaw"), HumanBone::Jaw);
    EXPECT_EQ(HumanBoneFromKHRName("leftEye"), HumanBone::LeftEye);
    // Unknown name falls through to None.
    EXPECT_EQ(HumanBoneFromKHRName("totallyMadeUpBone"), HumanBone::None);
    // Empty string also resolves to None.
    EXPECT_EQ(HumanBoneFromKHRName(""), HumanBone::None);
}

TEST(KHRHumanoidImporter, ImportsKHRExtensionAndPopulatesRig)
{
    cgltf_data* gltf = ParseGltf(kKHRSampleJson);
    ASSERT_NE(gltf, nullptr);

    EXPECT_TRUE(HasKHRHumanoidExtension(gltf));

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    const bool ok = ImportFromKHRHumanoid(gltf, skel, profile, rig);
    EXPECT_TRUE(ok);

    // Every body bone we declared in the JSON should be in the bone map
    // and resolve to a valid SkeletonData index.
    auto findBone = [&](HumanBone b) -> const HumanoidBoneMapping*
    {
        for (const auto& m : rig.BoneMap())
            if (m.Canonical == b) return &m;
        return nullptr;
    };

    const HumanoidBoneMapping* hipsMap = findBone(HumanBone::Hips);
    ASSERT_NE(hipsMap, nullptr);
    EXPECT_EQ(hipsMap->CachedSourceIndex, 0u);
    EXPECT_EQ(hipsMap->SourceBoneName, "Hips");

    const HumanoidBoneMapping* leftHandMap = findBone(HumanBone::LeftHand);
    ASSERT_NE(leftHandMap, nullptr);
    EXPECT_EQ(leftHandMap->CachedSourceIndex, 8u);

    // Chains were emitted (at least Spine + LeftArm + RightArm + LeftLeg + RightLeg).
    EXPECT_GE(rig.Chains().size(), 5u);

    // Translation bones default to {Hips}.
    ASSERT_EQ(rig.TranslationBones().size(), 1u);
    EXPECT_EQ(rig.TranslationBones()[0], HumanBone::Hips);

    cgltf_free(gltf);
}

TEST(KHRHumanoidImporter, MissingExtensionReturnsFalse)
{
    cgltf_data* gltf = ParseGltf(kPlainGltfJson);
    ASSERT_NE(gltf, nullptr);

    EXPECT_FALSE(HasKHRHumanoidExtension(gltf));

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    EXPECT_FALSE(ImportFromKHRHumanoid(gltf, skel, profile, rig));

    cgltf_free(gltf);
}

TEST(KHRHumanoidImporter, UnknownBoneNamesGracefullySkipped)
{
    cgltf_data* gltf = ParseGltf(kKHRBadNamesJson);
    ASSERT_NE(gltf, nullptr);

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    // No required canonical bones get bound -> coverage gate fails -> false.
    EXPECT_FALSE(ImportFromKHRHumanoid(gltf, skel, profile, rig));

    cgltf_free(gltf);
}

TEST(KHRHumanoidImporter, NullCgltfDataReturnsFalse)
{
    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path{});
    EXPECT_FALSE(ImportFromKHRHumanoid(nullptr, skel, profile, rig));
    EXPECT_FALSE(HasKHRHumanoidExtension(nullptr));
}

TEST(KHRHumanoidImporter, ImportFromKHR_RetargetPoseIsIdentityForTPose)
{
    // T-pose source skeleton (rest rotations all identity) against a
    // T-pose profile -> per-bone retarget delta should be identity.
    cgltf_data* gltf = ParseGltf(kKHRSampleJson);
    ASSERT_NE(gltf, nullptr);

    SkeletonData skel = BuildSyntheticGltfSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path{});
    PopulateTPoseProfile(profile);

    HumanoidRig rig(GUID(), std::filesystem::path{});
    ASSERT_TRUE(ImportFromKHRHumanoid(gltf, skel, profile, rig));

    for (const auto& m : rig.BoneMap())
    {
        if (m.CachedSourceIndex == ~0u) continue;
        const glm::quat& q = m.RetargetPoseRotation.GetGLM();
        // Identity quaternion: w=1, x=y=z=0 (within float epsilon).
        EXPECT_NEAR(q.w, 1.0f, 1e-3f);
        EXPECT_NEAR(q.x, 0.0f, 1e-3f);
        EXPECT_NEAR(q.y, 0.0f, 1e-3f);
        EXPECT_NEAR(q.z, 0.0f, 1e-3f);
    }

    cgltf_free(gltf);
}
