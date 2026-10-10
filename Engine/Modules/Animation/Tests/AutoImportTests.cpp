#include <gtest/gtest.h>

#include "Animation/HumanBone.h"
#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include "AssetCore/GUID.h"
#include "Mathematics/Quaternion.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

constexpr float kPi = 3.14159265358979323846f;

// Helpers ------------------------------------------------------------------

void EmplaceBone(SkeletonData& s,
                 const std::string& name,
                 int32 parent,
                 const glm::vec3& t,
                 const glm::quat& r = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
{
    s.BoneNames.push_back(name);
    s.Parent.push_back(parent);
    s.RestTranslation.push_back(t.x); s.RestTranslation.push_back(t.y); s.RestTranslation.push_back(t.z);
    s.RestRotation.push_back(r.x); s.RestRotation.push_back(r.y); s.RestRotation.push_back(r.z); s.RestRotation.push_back(r.w);
    s.RestScale.push_back(1.0f); s.RestScale.push_back(1.0f); s.RestScale.push_back(1.0f);
    s.BoneCount = static_cast<uint32>(s.BoneNames.size());
}

// Mixamo-like T-pose source skeleton (arms straight out along +X).
SkeletonData BuildMixamoTPoseSkeleton()
{
    SkeletonData s;
    // Hip at world origin (height 1.0).
    EmplaceBone(s, "mixamorig:Hips",         -1, {0.0f, 1.0f, 0.0f});
    EmplaceBone(s, "mixamorig:Spine",         0, {0.0f, 0.15f, 0.0f});
    EmplaceBone(s, "mixamorig:Spine1",        1, {0.0f, 0.15f, 0.0f});
    EmplaceBone(s, "mixamorig:Neck",          2, {0.0f, 0.20f, 0.0f});
    EmplaceBone(s, "mixamorig:Head",          3, {0.0f, 0.10f, 0.0f});
    // T-pose arms — extend straight along +X (left) / -X (right) from shoulders.
    EmplaceBone(s, "mixamorig:LeftShoulder",  2, {0.05f, 0.18f, 0.0f});
    EmplaceBone(s, "mixamorig:LeftArm",       5, {0.10f, 0.0f, 0.0f}); // upper arm out
    EmplaceBone(s, "mixamorig:LeftForeArm",   6, {0.30f, 0.0f, 0.0f});
    EmplaceBone(s, "mixamorig:LeftHand",      7, {0.25f, 0.0f, 0.0f});
    EmplaceBone(s, "mixamorig:RightShoulder", 2, {-0.05f, 0.18f, 0.0f});
    EmplaceBone(s, "mixamorig:RightArm",      9, {-0.10f, 0.0f, 0.0f});
    EmplaceBone(s, "mixamorig:RightForeArm", 10, {-0.30f, 0.0f, 0.0f});
    EmplaceBone(s, "mixamorig:RightHand",    11, {-0.25f, 0.0f, 0.0f});
    // Legs — straight down.
    EmplaceBone(s, "mixamorig:LeftUpLeg",     0, {0.10f, -0.05f, 0.0f});
    EmplaceBone(s, "mixamorig:LeftLeg",      13, {0.0f, -0.45f, 0.0f});
    EmplaceBone(s, "mixamorig:LeftFoot",     14, {0.0f, -0.45f, 0.05f});
    EmplaceBone(s, "mixamorig:RightUpLeg",    0, {-0.10f, -0.05f, 0.0f});
    EmplaceBone(s, "mixamorig:RightLeg",     16, {0.0f, -0.45f, 0.0f});
    EmplaceBone(s, "mixamorig:RightFoot",    17, {0.0f, -0.45f, 0.05f});
    s.BuildBoneNameLookup();
    return s;
}

// Synty-like A-pose source skeleton: arms hanging at ~45 degrees below horizontal.
// We model the A-pose by setting upper-arm REST TRANSLATION downward+outward.
SkeletonData BuildSyntyAPoseSkeleton()
{
    SkeletonData s;
    EmplaceBone(s, "Hips",        -1, {0.0f, 1.0f, 0.0f});
    EmplaceBone(s, "Spine_01",     0, {0.0f, 0.15f, 0.0f});
    EmplaceBone(s, "Spine_02",     1, {0.0f, 0.15f, 0.0f});
    EmplaceBone(s, "Neck_01",      2, {0.0f, 0.20f, 0.0f});
    EmplaceBone(s, "Head",         3, {0.0f, 0.10f, 0.0f});
    // Synty A-pose: shoulder is up, but upper-arm extends DOWN-and-OUT
    // ~45 degrees from horizontal. We encode that by translating the
    // upper arm with a -Y component matching its +X reach.
    EmplaceBone(s, "Shoulder_L",   2, {0.05f, 0.18f, 0.0f});
    EmplaceBone(s, "UpperArm_L",   5, {0.07f, -0.07f, 0.0f}); // ~45 deg downward
    EmplaceBone(s, "LowerArm_L",   6, {0.21f, -0.21f, 0.0f}); // continue 45 deg
    EmplaceBone(s, "Hand_L",       7, {0.18f, -0.18f, 0.0f});
    EmplaceBone(s, "Shoulder_R",   2, {-0.05f, 0.18f, 0.0f});
    EmplaceBone(s, "UpperArm_R",   9, {-0.07f, -0.07f, 0.0f});
    EmplaceBone(s, "LowerArm_R",  10, {-0.21f, -0.21f, 0.0f});
    EmplaceBone(s, "Hand_R",      11, {-0.18f, -0.18f, 0.0f});
    EmplaceBone(s, "UpperLeg_L",   0, {0.10f, -0.05f, 0.0f});
    EmplaceBone(s, "LowerLeg_L",  13, {0.0f, -0.45f, 0.0f});
    EmplaceBone(s, "Foot_L",      14, {0.0f, -0.45f, 0.05f});
    EmplaceBone(s, "UpperLeg_R",   0, {-0.10f, -0.05f, 0.0f});
    EmplaceBone(s, "LowerLeg_R",  16, {0.0f, -0.45f, 0.0f});
    EmplaceBone(s, "Foot_R",      17, {0.0f, -0.45f, 0.05f});
    s.BuildBoneNameLookup();
    return s;
}

// MetaHuman-like T-pose: arms slightly drooped (within 5 deg tolerance band).
SkeletonData BuildMetaHumanTPoseSkeleton()
{
    SkeletonData s;
    EmplaceBone(s, "pelvis",        -1, {0.0f, 1.0f, 0.0f});
    EmplaceBone(s, "spine_01",       0, {0.0f, 0.15f, 0.0f});
    EmplaceBone(s, "spine_02",       1, {0.0f, 0.15f, 0.0f});
    EmplaceBone(s, "neck_01",        2, {0.0f, 0.20f, 0.0f});
    EmplaceBone(s, "head",           3, {0.0f, 0.10f, 0.0f});
    // T-pose with tiny droop — Y component small enough that the angle
    // from horizontal stays under 30 deg threshold (~3 deg here).
    EmplaceBone(s, "clavicle_l",     2, {0.05f, 0.18f, 0.0f});
    EmplaceBone(s, "upperarm_l",     5, {0.10f, -0.005f, 0.0f}); // ~3 deg droop
    EmplaceBone(s, "lowerarm_l",     6, {0.30f, -0.015f, 0.0f});
    EmplaceBone(s, "hand_l",         7, {0.25f, -0.013f, 0.0f});
    EmplaceBone(s, "clavicle_r",     2, {-0.05f, 0.18f, 0.0f});
    EmplaceBone(s, "upperarm_r",     9, {-0.10f, -0.005f, 0.0f});
    EmplaceBone(s, "lowerarm_r",    10, {-0.30f, -0.015f, 0.0f});
    EmplaceBone(s, "hand_r",        11, {-0.25f, -0.013f, 0.0f});
    EmplaceBone(s, "thigh_l",        0, {0.10f, -0.05f, 0.0f});
    EmplaceBone(s, "calf_l",        13, {0.0f, -0.45f, 0.0f});
    EmplaceBone(s, "foot_l",        14, {0.0f, -0.45f, 0.05f});
    EmplaceBone(s, "thigh_r",        0, {-0.10f, -0.05f, 0.0f});
    EmplaceBone(s, "calf_r",        16, {0.0f, -0.45f, 0.0f});
    EmplaceBone(s, "foot_r",        17, {0.0f, -0.45f, 0.05f});
    s.BuildBoneNameLookup();
    return s;
}

// Humanoid bone names, four extremities on the ground (front legs named as arms).
SkeletonData BuildFourLeggedRestSkeleton()
{
    SkeletonData s;
    EmplaceBone(s, "pelvis",      -1, {0.0f, 0.35f, 0.0f});
    EmplaceBone(s, "spine_01",     0, {0.0f, 0.06f, 0.08f});
    EmplaceBone(s, "spine_02",     1, {0.0f, 0.04f, 0.08f});
    EmplaceBone(s, "spine_03",     2, {0.0f, 0.03f, 0.06f});
    EmplaceBone(s, "neck_01",      3, {0.0f, 0.08f, 0.10f});
    EmplaceBone(s, "head",         4, {0.0f, 0.05f, 0.08f});
    EmplaceBone(s, "clavicle_l",   2, {0.08f, -0.02f, 0.10f});
    EmplaceBone(s, "upperarm_l",   6, {0.0f, -0.14f, 0.06f});
    EmplaceBone(s, "lowerarm_l",   7, {0.0f, -0.14f, 0.02f});
    EmplaceBone(s, "hand_l",       8, {0.0f, -0.15f, 0.0f});
    EmplaceBone(s, "clavicle_r",   2, {-0.08f, -0.02f, 0.10f});
    EmplaceBone(s, "upperarm_r",  10, {0.0f, -0.14f, 0.06f});
    EmplaceBone(s, "lowerarm_r",  11, {0.0f, -0.14f, 0.02f});
    EmplaceBone(s, "hand_r",      12, {0.0f, -0.15f, 0.0f});
    EmplaceBone(s, "thigh_l",      0, {0.08f, -0.12f, -0.12f});
    EmplaceBone(s, "calf_l",      14, {0.0f, -0.12f, -0.02f});
    EmplaceBone(s, "foot_l",      15, {0.0f, -0.11f, 0.0f});
    EmplaceBone(s, "thigh_r",      0, {-0.08f, -0.12f, -0.12f});
    EmplaceBone(s, "calf_r",      17, {0.0f, -0.12f, -0.02f});
    EmplaceBone(s, "foot_r",      18, {0.0f, -0.11f, 0.0f});
    EmplaceBone(s, "tail",         0, {0.0f, 0.0f, -0.12f});
    s.BuildBoneNameLookup();
    return s;
}

// Populate a SkeletonProfile in-place with a basic T-pose layout. T-pose-
// bind source rigs against this profile produce identity retarget poses.
void PopulateTPoseReferenceProfile(SkeletonProfile& profile)
{
    std::vector<ProfileBone> bones;

    auto addBone = [&](HumanBone b, HumanBone parent,
                       const glm::vec3& t,
                       const glm::vec3& localForward,
                       const glm::quat& r = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        ProfileBone pb;
        pb.Bone = b;
        pb.Parent = parent;
        pb.RestTranslation = Mathematics::Vector3(t.x, t.y, t.z);
        pb.RestRotation = Mathematics::Quaternion(r.w, r.x, r.y, r.z);
        pb.LocalForward = Mathematics::Vector3(localForward.x, localForward.y, localForward.z);
        bones.push_back(pb);
    };

    // Mirrors HumanoidStandard.profile.json: per-bone LocalForward defines
    // the bone's primary axis direction in canonical T-pose world space.
    // Spine bones point +Y (up), arms point along their respective ±X
    // axes, legs point -Y (down), feet point +Z (forward).
    addBone(HumanBone::Hips,          HumanBone::None,           {0.0f, 1.0f, 0.0f},   {0.0f, 0.0f, 1.0f});
    addBone(HumanBone::Spine,         HumanBone::Hips,           {0.0f, 0.15f, 0.0f},  {0.0f, 1.0f, 0.0f});
    addBone(HumanBone::Chest,         HumanBone::Spine,          {0.0f, 0.15f, 0.0f},  {0.0f, 1.0f, 0.0f});
    addBone(HumanBone::Neck,          HumanBone::Chest,          {0.0f, 0.20f, 0.0f},  {0.0f, 1.0f, 0.0f});
    addBone(HumanBone::Head,          HumanBone::Neck,           {0.0f, 0.10f, 0.0f},  {0.0f, 1.0f, 0.0f});
    addBone(HumanBone::LeftShoulder,  HumanBone::Chest,          {0.05f, 0.18f, 0.0f}, {1.0f, 0.0f, 0.0f});
    addBone(HumanBone::LeftUpperArm,  HumanBone::LeftShoulder,   {0.10f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f});
    addBone(HumanBone::LeftLowerArm,  HumanBone::LeftUpperArm,   {0.30f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f});
    addBone(HumanBone::LeftHand,      HumanBone::LeftLowerArm,   {0.25f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f});
    addBone(HumanBone::RightShoulder, HumanBone::Chest,          {-0.05f, 0.18f, 0.0f},{-1.0f, 0.0f, 0.0f});
    addBone(HumanBone::RightUpperArm, HumanBone::RightShoulder,  {-0.10f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
    addBone(HumanBone::RightLowerArm, HumanBone::RightUpperArm,  {-0.30f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
    addBone(HumanBone::RightHand,     HumanBone::RightLowerArm,  {-0.25f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
    addBone(HumanBone::LeftUpperLeg,  HumanBone::Hips,           {0.10f, -0.05f, 0.0f},{0.0f, -1.0f, 0.0f});
    addBone(HumanBone::LeftLowerLeg,  HumanBone::LeftUpperLeg,   {0.0f, -0.45f, 0.0f}, {0.0f, -1.0f, 0.0f});
    addBone(HumanBone::LeftFoot,      HumanBone::LeftLowerLeg,   {0.0f, -0.45f, 0.05f},{0.0f, 0.0f, 1.0f});
    addBone(HumanBone::RightUpperLeg, HumanBone::Hips,           {-0.10f, -0.05f, 0.0f},{0.0f, -1.0f, 0.0f});
    addBone(HumanBone::RightLowerLeg, HumanBone::RightUpperLeg,  {0.0f, -0.45f, 0.0f}, {0.0f, -1.0f, 0.0f});
    addBone(HumanBone::RightFoot,     HumanBone::RightLowerLeg,  {0.0f, -0.45f, 0.05f},{0.0f, 0.0f, 1.0f});

    profile.SetBonesForTest(std::move(bones));
    profile.SetNameForTest("HumanoidStandard");
}

// Same as PopulateTPoseReferenceProfile but with the upper-arm rest rotations
// rotated +22.5 deg about world Z. An A-pose source skeleton (rest rotations
// default to identity) will then produce a non-identity quaternion at the
// upper-arm bones in BakeRetargetPoseFromAPose.
void PopulateTPoseProfileWithArmTwist(SkeletonProfile& profile)
{
    PopulateTPoseReferenceProfile(profile);
    auto bones = profile.Bones();
    for (auto& pb : bones)
    {
        if (pb.Bone == HumanBone::LeftUpperArm || pb.Bone == HumanBone::RightUpperArm)
        {
            const float angle = 22.5f * kPi / 180.0f;
            glm::quat q(std::cos(angle * 0.5f), 0.0f, 0.0f, std::sin(angle * 0.5f));
            pb.RestRotation = Mathematics::Quaternion(q.w, q.x, q.y, q.z);
        }
    }
    profile.SetBonesForTest(std::move(bones));
}

float QuatAngleDeg(const Mathematics::Quaternion& q)
{
    const glm::quat& g = q.GetGLM();
    const float w = std::max(-1.0f, std::min(1.0f, g.w));
    return 2.0f * std::acos(std::abs(w)) * 180.0f / kPi;
}

} // namespace

// ----------------------- A-pose detection -----------------------

TEST(AutoImportTest, DetectAPoseFlagsSyntyRig)
{
    auto skel = BuildSyntyAPoseSkeleton();
    HumanoidNameMatcher matcher;
    auto mapping = matcher.MatchSkeleton(skel.BoneNames);

    APoseDetectionResult ap = DetectAPose(skel, mapping);
    EXPECT_TRUE(ap.LeftSidePresent);
    EXPECT_TRUE(ap.RightSidePresent);
    EXPECT_TRUE(ap.IsAPose) << "Synty A-pose was not detected; left angle "
                            << ap.ShoulderToWristAngleDegLeft << " right angle "
                            << ap.ShoulderToWristAngleDegRight;
    // 45-deg synthetic should land very close to 45.
    EXPECT_GT(ap.ShoulderToWristAngleDegLeft, 30.0f);
    EXPECT_GT(ap.ShoulderToWristAngleDegRight, 30.0f);
    EXPECT_LT(ap.ShoulderToWristAngleDegLeft, 60.0f);
    EXPECT_LT(ap.ShoulderToWristAngleDegRight, 60.0f);
}

TEST(AutoImportTest, DetectAPoseSkipsMixamoTPose)
{
    auto skel = BuildMixamoTPoseSkeleton();
    HumanoidNameMatcher matcher;
    auto mapping = matcher.MatchSkeleton(skel.BoneNames);

    APoseDetectionResult ap = DetectAPose(skel, mapping);
    EXPECT_TRUE(ap.LeftSidePresent);
    EXPECT_FALSE(ap.IsAPose) << "Mixamo T-pose flagged as A-pose; left angle "
                             << ap.ShoulderToWristAngleDegLeft;
    EXPECT_LT(ap.ShoulderToWristAngleDegLeft, 30.0f);
    EXPECT_LT(ap.ShoulderToWristAngleDegRight, 30.0f);
}

TEST(AutoImportTest, DetectAPoseSkipsMetaHumanDroop)
{
    auto skel = BuildMetaHumanTPoseSkeleton();
    HumanoidNameMatcher matcher;
    auto mapping = matcher.MatchSkeleton(skel.BoneNames);

    APoseDetectionResult ap = DetectAPose(skel, mapping);
    EXPECT_FALSE(ap.IsAPose);
    EXPECT_LT(ap.ShoulderToWristAngleDegLeft, 30.0f);
}

// ----------------------- AutoImportHumanoidRig -----------------------

TEST(AutoImportTest, MixamoTPoseProducesIdentityRetargetPose)
{
    auto skel = BuildMixamoTPoseSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path("test://HumanoidStandard.profile.json"));
    PopulateTPoseReferenceProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path("test://mixamo.humanoidrig.json"));

    ASSERT_TRUE(AutoImportHumanoidRig(skel, profile, rig));

    // Required bones present.
    auto findMapping = [&](HumanBone b) -> const HumanoidBoneMapping*
    {
        for (const auto& m : rig.BoneMap())
            if (m.Canonical == b) return &m;
        return nullptr;
    };
    ASSERT_NE(findMapping(HumanBone::Hips), nullptr);
    ASSERT_NE(findMapping(HumanBone::LeftUpperArm), nullptr);
    ASSERT_NE(findMapping(HumanBone::LeftHand), nullptr);

    // Phase 25.1: Q reflects per-bone bind direction vs canonical
    // LocalForward. T-pose-bind Mixamo bones should produce small Q
    // values; allow up to 15° to absorb fixture bone-position jitter
    // (e.g. lower-leg-to-foot offset has slight forward bias).
    for (const auto& m : rig.BoneMap())
    {
        const float angle = QuatAngleDeg(m.RetargetPoseRotation);
        EXPECT_LT(angle, 15.0f) << "Mixamo near-T-pose bone " << HumanBoneToString(m.Canonical)
                                << " produced excessive retarget pose: " << angle << " deg";
    }
}

TEST(AutoImportTest, MetaHumanDroopProducesSmallRetargetPose)
{
    // Phase 25.1: BakeRetargetPoseFromAPose computes Q per canonical bone
    // from the rig's authored bind direction vs profile LocalForward. For
    // a near-T-pose rig (MetaHuman fixture has intentional ~3° droop on
    // arms to test the A-pose detection threshold), Q values reflect the
    // actual drift — small but non-zero. Test the magnitude stays bounded
    // (< 15°), matching the "near-canonical" intent.
    auto skel = BuildMetaHumanTPoseSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path("test://HumanoidStandard.profile.json"));
    PopulateTPoseReferenceProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path("test://metahuman.humanoidrig.json"));

    ASSERT_TRUE(AutoImportHumanoidRig(skel, profile, rig));

    for (const auto& m : rig.BoneMap())
    {
        const float angle = QuatAngleDeg(m.RetargetPoseRotation);
        EXPECT_LT(angle, 15.0f) << "MetaHuman near-T-pose bone " << HumanBoneToString(m.Canonical)
                                << " produced excessive retarget pose: " << angle << " deg";
    }
}

TEST(AutoImportTest, BodyProportionsArePopulated)
{
    auto skel = BuildMixamoTPoseSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path("test://HumanoidStandard.profile.json"));
    PopulateTPoseReferenceProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path("test://mixamo.humanoidrig.json"));

    ASSERT_TRUE(AutoImportHumanoidRig(skel, profile, rig));

    const auto& prop = rig.Proportions();
    EXPECT_NEAR(prop.HipHeight, 1.0f, 0.01f);
    EXPECT_GT(prop.ShoulderWidth, 0.05f);
    EXPECT_GT(prop.LegLength, 0.5f);
    EXPECT_GT(prop.ArmLength, 0.4f);
}

TEST(AutoImportTest, ChainsArePopulated)
{
    auto skel = BuildMixamoTPoseSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path("test://HumanoidStandard.profile.json"));
    PopulateTPoseReferenceProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path("test://mixamo.humanoidrig.json"));

    ASSERT_TRUE(AutoImportHumanoidRig(skel, profile, rig));

    auto findChain = [&](ChainKind kind) -> const HumanoidChain*
    {
        for (const auto& c : rig.Chains())
            if (c.Kind == kind) return &c;
        return nullptr;
    };

    const auto* spine = findChain(ChainKind::Spine);
    ASSERT_NE(spine, nullptr);
    EXPECT_EQ(spine->Start, HumanBone::Hips);
    EXPECT_EQ(spine->End, HumanBone::Head);

    const auto* leftArm = findChain(ChainKind::LeftArm);
    ASSERT_NE(leftArm, nullptr);
    EXPECT_EQ(leftArm->End, HumanBone::LeftHand);

    const auto* rightArm = findChain(ChainKind::RightArm);
    ASSERT_NE(rightArm, nullptr);

    const auto* leftLeg = findChain(ChainKind::LeftLeg);
    ASSERT_NE(leftLeg, nullptr);
    EXPECT_EQ(leftLeg->Start, HumanBone::LeftUpperLeg);
    // End may be LeftFoot or LeftToes depending on whether the matcher
    // resolved a toes bone in the synthetic skeleton.
    EXPECT_TRUE(leftLeg->End == HumanBone::LeftFoot || leftLeg->End == HumanBone::LeftToes);
}

TEST(AutoImportTest, FailsBelowCoverageThreshold)
{
    SkeletonData skel;
    EmplaceBone(skel, "Random_Helper", -1, {0, 0, 0});
    EmplaceBone(skel, "Other_Helper", 0, {0, 0, 0});
    skel.BuildBoneNameLookup();
    SkeletonProfile profile(GUID(), std::filesystem::path("test://HumanoidStandard.profile.json"));
    PopulateTPoseReferenceProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path("test://garbage.humanoidrig.json"));
    EXPECT_FALSE(AutoImportHumanoidRig(skel, profile, rig));
}

TEST(AutoImportTest, FourLeggedRestPoseFailsStanceGate)
{
    auto skel = BuildFourLeggedRestSkeleton();
    HumanoidNameMatcher matcher;
    const auto match = matcher.MatchSkeleton(skel.BoneNames);
    EXPECT_GE(match.Coverage, kAutoImportCoverageThreshold);

    SkeletonProfile profile(GUID(), std::filesystem::path("test://HumanoidStandard.profile.json"));
    PopulateTPoseReferenceProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path("test://fourleg.humanoidrig.json"));
    EXPECT_FALSE(AutoImportHumanoidRig(skel, profile, rig));
}

TEST(AutoImportTest, BipedTPoseWithSameNamesStillPasses)
{
    auto skel = BuildMetaHumanTPoseSkeleton();
    SkeletonProfile profile(GUID(), std::filesystem::path("test://HumanoidStandard.profile.json"));
    PopulateTPoseReferenceProfile(profile);
    HumanoidRig rig(GUID(), std::filesystem::path("test://biped.humanoidrig.json"));
    EXPECT_TRUE(AutoImportHumanoidRig(skel, profile, rig));
}
