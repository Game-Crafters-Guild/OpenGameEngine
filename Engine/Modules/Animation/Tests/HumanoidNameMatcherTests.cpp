#include <gtest/gtest.h>

#include "Animation/HumanBone.h"
#include "Animation/HumanoidNameMatcher.h"

#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

// Mixamo bone-name fixture (the prefix style).
const std::vector<std::string>& MixamoBones()
{
    static const std::vector<std::string> bones = {
        "mixamorig:Hips",
        "mixamorig:Spine",
        "mixamorig:Spine1",
        "mixamorig:Spine2",
        "mixamorig:Neck",
        "mixamorig:Head",
        "mixamorig:LeftShoulder",
        "mixamorig:LeftArm",
        "mixamorig:LeftForeArm",
        "mixamorig:LeftHand",
        "mixamorig:RightShoulder",
        "mixamorig:RightArm",
        "mixamorig:RightForeArm",
        "mixamorig:RightHand",
        "mixamorig:LeftUpLeg",
        "mixamorig:LeftLeg",
        "mixamorig:LeftFoot",
        "mixamorig:LeftToeBase",
        "mixamorig:RightUpLeg",
        "mixamorig:RightLeg",
        "mixamorig:RightFoot",
        "mixamorig:RightToeBase",
    };
    return bones;
}

// Synty bone-name fixture (no prefix; numeric suffixes).
const std::vector<std::string>& SyntyBones()
{
    static const std::vector<std::string> bones = {
        "Hips",
        "Spine_01",
        "Spine_02",
        "Spine_03",
        "Neck_01",
        "Head",
        "Shoulder_L",
        "UpperArm_L",
        "LowerArm_L",
        "Hand_L",
        "Shoulder_R",
        "UpperArm_R",
        "LowerArm_R",
        "Hand_R",
        "UpperLeg_L",
        "LowerLeg_L",
        "Foot_L",
        "Toes_L",
        "UpperLeg_R",
        "LowerLeg_R",
        "Foot_R",
        "Toes_R",
    };
    return bones;
}

// BlenRig 6 / TLOU / Ellie-style bone-name fixture. Auto-Rig Pro and
// BlenRig prefix every bone with a control-rig namespace (FK-/CTR-/IK-/
// MSTR-/DEF-/STR-/...). The deform-affecting FK chain is what artists
// animate on. The matcher must recognise these via separator-class `-`
// support; Phase B4 added BlenRig coverage to support .blend files
// authored against Auto-Rig Pro / BlenRig presets (the Ellie pose-library
// bundle from plan-blender §6).
const std::vector<std::string>& BlenRigBones()
{
    static const std::vector<std::string> bones = {
        // Centerline (BlenRig style, FK-prefixed + a hip-centroid bone).
        "Hip_Center",
        "MSTR-Spine_Hips",
        "MSTR-Spine_Torso",
        "FK-Spine",
        "FK-Chest",
        "FK-Neck",
        "FK-Head",
        // Left arm.
        "FK-Shoulder.L",
        "FK-UpperArm.L",
        "FK-Forearm.L",
        "FK-Wrist.L",
        // Right arm.
        "FK-Shoulder.R",
        "FK-UpperArm.R",
        "FK-Forearm.R",
        "FK-Wrist.R",
        // Left leg. BlenRig wraps the foot bone in a `W-` (world-space)
        // helper namespace; the matcher has to peel it off.
        "FK-Thigh.L",
        "FK-Knee.L",
        "FK-W-Foot.L",
        "FK-Toes.L",
        // Right leg.
        "FK-Thigh.R",
        "FK-Knee.R",
        "FK-W-Foot.R",
        "FK-Toes.R",
        // Some rig-control bones that shouldn't pollute body matches but
        // *might* appear earlier in the skeleton's bone order than the FK
        // chain. The matcher's first-match-wins iteration claims sources
        // in skeleton order, so we list these last so the FK names win;
        // a real Ellie skeleton interleaves them but BlenRig 6 places
        // FK chain ahead in the bone hierarchy.
        "DEF-Wrist.L",
        "DEF-Wrist.R",
        "IK-MSTR-Wrist.L",
        "IK-MSTR-Wrist.R",
        "STR-Wrist.L",
        "STR-Wrist.R",
        "STR-Head",
    };
    return bones;
}

// MetaHuman bone-name fixture (lowercase, _l/_r suffix).
const std::vector<std::string>& MetaHumanBones()
{
    static const std::vector<std::string> bones = {
        "pelvis",
        "spine_01",
        "spine_02",
        "spine_03",
        "neck_01",
        "head",
        "clavicle_l",
        "upperarm_l",
        "lowerarm_l",
        "hand_l",
        "clavicle_r",
        "upperarm_r",
        "lowerarm_r",
        "hand_r",
        "thigh_l",
        "calf_l",
        "foot_l",
        "ball_l",
        "thigh_r",
        "calf_r",
        "foot_r",
        "ball_r",
    };
    return bones;
}

} // namespace

TEST(HumanoidNameMatcherTest, MixamoMapsAllRequired)
{
    HumanoidNameMatcher matcher;
    auto result = matcher.MatchSkeleton(MixamoBones());

    EXPECT_EQ(result.RequiredMatched, result.RequiredTotal)
        << "Mixamo coverage was " << result.RequiredMatched << "/" << result.RequiredTotal;

    auto check = [&](HumanBone bone, const char* expectedSource)
    {
        const auto* m = result.Find(bone);
        ASSERT_NE(m, nullptr) << "Mixamo missing canonical bone " << HumanBoneToString(bone);
        EXPECT_EQ(m->SourceBoneName, expectedSource);
    };

    check(HumanBone::Hips, "mixamorig:Hips");
    check(HumanBone::Spine, "mixamorig:Spine");
    check(HumanBone::Head, "mixamorig:Head");
    check(HumanBone::LeftUpperArm, "mixamorig:LeftArm");
    check(HumanBone::LeftLowerArm, "mixamorig:LeftForeArm");
    check(HumanBone::LeftHand, "mixamorig:LeftHand");
    check(HumanBone::RightUpperArm, "mixamorig:RightArm");
    check(HumanBone::LeftUpperLeg, "mixamorig:LeftUpLeg");
    check(HumanBone::LeftFoot, "mixamorig:LeftFoot");
}

TEST(HumanoidNameMatcherTest, SyntyMapsAllRequired)
{
    HumanoidNameMatcher matcher;
    auto result = matcher.MatchSkeleton(SyntyBones());

    EXPECT_EQ(result.RequiredMatched, result.RequiredTotal)
        << "Synty coverage was " << result.RequiredMatched << "/" << result.RequiredTotal;

    auto check = [&](HumanBone bone, const char* expectedSource)
    {
        const auto* m = result.Find(bone);
        ASSERT_NE(m, nullptr) << "Synty missing canonical bone " << HumanBoneToString(bone);
        EXPECT_EQ(m->SourceBoneName, expectedSource);
    };

    check(HumanBone::Hips, "Hips");
    check(HumanBone::Spine, "Spine_01");
    check(HumanBone::Head, "Head");
    // Critical: Shoulder_L canonical = LeftShoulder; UpperArm_L canonical = LeftUpperArm.
    check(HumanBone::LeftShoulder, "Shoulder_L");
    check(HumanBone::LeftUpperArm, "UpperArm_L");
    check(HumanBone::LeftLowerArm, "LowerArm_L");
    check(HumanBone::LeftHand, "Hand_L");
    check(HumanBone::LeftUpperLeg, "UpperLeg_L");
    check(HumanBone::LeftFoot, "Foot_L");
}

TEST(HumanoidNameMatcherTest, MetaHumanMapsAllRequired)
{
    HumanoidNameMatcher matcher;
    auto result = matcher.MatchSkeleton(MetaHumanBones());

    EXPECT_EQ(result.RequiredMatched, result.RequiredTotal)
        << "MetaHuman coverage was " << result.RequiredMatched << "/" << result.RequiredTotal;

    auto check = [&](HumanBone bone, const char* expectedSource)
    {
        const auto* m = result.Find(bone);
        ASSERT_NE(m, nullptr) << "MetaHuman missing canonical bone " << HumanBoneToString(bone);
        EXPECT_EQ(m->SourceBoneName, expectedSource);
    };

    check(HumanBone::Hips, "pelvis");
    check(HumanBone::Spine, "spine_01");
    check(HumanBone::Head, "head");
    check(HumanBone::LeftShoulder, "clavicle_l");
    check(HumanBone::LeftUpperArm, "upperarm_l");
    check(HumanBone::LeftLowerArm, "lowerarm_l");
    check(HumanBone::LeftHand, "hand_l");
    check(HumanBone::LeftUpperLeg, "thigh_l");
    check(HumanBone::LeftLowerLeg, "calf_l");
    check(HumanBone::LeftFoot, "foot_l");
}

TEST(HumanoidNameMatcherTest, BlenRigMapsAllRequired)
{
    HumanoidNameMatcher matcher;
    auto result = matcher.MatchSkeleton(BlenRigBones());

    EXPECT_EQ(result.RequiredMatched, result.RequiredTotal)
        << "BlenRig coverage was " << result.RequiredMatched << "/" << result.RequiredTotal;

    auto check = [&](HumanBone bone, const char* expectedSource)
    {
        const auto* m = result.Find(bone);
        ASSERT_NE(m, nullptr) << "BlenRig missing canonical bone " << HumanBoneToString(bone);
        EXPECT_EQ(m->SourceBoneName, expectedSource);
    };

    // Hips should be claimed by the dedicated centroid (Hip_Center) which
    // appears first in iteration order before MSTR-Spine_Hips.
    check(HumanBone::Hips, "Hip_Center");
    // Spine: MSTR-Spine_Hips is claimed by Hips first; MSTR-Spine_Torso
    // doesn't match (ends with Torso, not spine); FK-Spine wins as the
    // first remaining bone whose tail matches the Spine pattern.
    check(HumanBone::Spine, "FK-Spine");
    check(HumanBone::Head, "FK-Head");
    check(HumanBone::Neck, "FK-Neck");
    check(HumanBone::Chest, "FK-Chest");
    // Arms — FK chain (the user-authored animation channels target these).
    check(HumanBone::LeftShoulder,  "FK-Shoulder.L");
    check(HumanBone::LeftUpperArm,  "FK-UpperArm.L");
    check(HumanBone::LeftLowerArm,  "FK-Forearm.L");
    check(HumanBone::LeftHand,      "FK-Wrist.L");
    check(HumanBone::RightShoulder, "FK-Shoulder.R");
    check(HumanBone::RightUpperArm, "FK-UpperArm.R");
    check(HumanBone::RightLowerArm, "FK-Forearm.R");
    check(HumanBone::RightHand,     "FK-Wrist.R");
    // Legs — including the BlenRig "W-Foot" wrapper.
    check(HumanBone::LeftUpperLeg,  "FK-Thigh.L");
    check(HumanBone::LeftLowerLeg,  "FK-Knee.L");
    check(HumanBone::LeftFoot,      "FK-W-Foot.L");
    check(HumanBone::LeftToes,      "FK-Toes.L");
    check(HumanBone::RightUpperLeg, "FK-Thigh.R");
    check(HumanBone::RightLowerLeg, "FK-Knee.R");
    check(HumanBone::RightFoot,     "FK-W-Foot.R");
    check(HumanBone::RightToes,     "FK-Toes.R");
}

TEST(HumanoidNameMatcherTest, BlenRigSinglePatternResolution)
{
    // Sanity: every BlenRig bone naming convention should single-pattern-
    // match the right canonical when handed in isolation. This covers
    // the separator-`-` support added in B4.
    HumanoidNameMatcher matcher;
    EXPECT_EQ(matcher.Match("Hip_Center"),       HumanBone::Hips);
    EXPECT_EQ(matcher.Match("FK-UpperArm.L"),    HumanBone::LeftUpperArm);
    EXPECT_EQ(matcher.Match("FK-UpperArm.R"),    HumanBone::RightUpperArm);
    EXPECT_EQ(matcher.Match("FK-Forearm.L"),     HumanBone::LeftLowerArm);
    EXPECT_EQ(matcher.Match("FK-Wrist.L"),       HumanBone::LeftHand);
    EXPECT_EQ(matcher.Match("FK-Shoulder.L"),    HumanBone::LeftShoulder);
    EXPECT_EQ(matcher.Match("DEF-Wrist.L"),      HumanBone::LeftHand);
    EXPECT_EQ(matcher.Match("IK-MSTR-Wrist.L"),  HumanBone::LeftHand);
    EXPECT_EQ(matcher.Match("FK-Thigh.L"),       HumanBone::LeftUpperLeg);
    EXPECT_EQ(matcher.Match("FK-Knee.R"),        HumanBone::RightLowerLeg);
    EXPECT_EQ(matcher.Match("FK-W-Foot.L"),      HumanBone::LeftFoot);
    EXPECT_EQ(matcher.Match("FK-Toes.L"),        HumanBone::LeftToes);
    EXPECT_EQ(matcher.Match("FK-Spine"),         HumanBone::Spine);
    EXPECT_EQ(matcher.Match("FK-Chest"),         HumanBone::Chest);
    EXPECT_EQ(matcher.Match("FK-Head"),          HumanBone::Head);
    EXPECT_EQ(matcher.Match("FK-Neck"),          HumanBone::Neck);
}

TEST(HumanoidNameMatcherTest, SideSuffixVariantsAllResolve)
{
    HumanoidNameMatcher matcher;
    // L/R, Left/Right, prefix and suffix forms all work.
    EXPECT_EQ(matcher.Match("UpperArm_L"), HumanBone::LeftUpperArm);
    EXPECT_EQ(matcher.Match("UpperArm_R"), HumanBone::RightUpperArm);
    EXPECT_EQ(matcher.Match("L_UpperArm"), HumanBone::LeftUpperArm);
    EXPECT_EQ(matcher.Match("R_UpperArm"), HumanBone::RightUpperArm);
    EXPECT_EQ(matcher.Match("LeftUpperArm"), HumanBone::LeftUpperArm);
    EXPECT_EQ(matcher.Match("RightUpperArm"), HumanBone::RightUpperArm);
    EXPECT_EQ(matcher.Match("upperarm_l"), HumanBone::LeftUpperArm);
    EXPECT_EQ(matcher.Match("UPPERARM_R"), HumanBone::RightUpperArm);
    // "Bicep" and "Forearm" alternates (Maya/Adobe naming).
    EXPECT_EQ(matcher.Match("L_Bicep"), HumanBone::LeftUpperArm);
    EXPECT_EQ(matcher.Match("R_Forearm"), HumanBone::RightLowerArm);
}

TEST(HumanoidNameMatcherTest, NoMatchReturnsNone)
{
    HumanoidNameMatcher matcher;
    EXPECT_EQ(matcher.Match(""), HumanBone::None);
    EXPECT_EQ(matcher.Match("RandomNoiseBone"), HumanBone::None);
    EXPECT_EQ(matcher.Match("Cape_Anchor"), HumanBone::None);
    EXPECT_EQ(matcher.Match("Sword_Sheath_01"), HumanBone::None);
    EXPECT_EQ(matcher.Match("Bow_Hook"), HumanBone::None);
}

TEST(HumanoidNameMatcherTest, RootSpineRootHipPattern)
{
    HumanoidNameMatcher matcher;
    EXPECT_EQ(matcher.Match("Hips"), HumanBone::Hips);
    EXPECT_EQ(matcher.Match("hip"), HumanBone::Hips);
    EXPECT_EQ(matcher.Match("Pelvis"), HumanBone::Hips);
    EXPECT_EQ(matcher.Match("pelvis"), HumanBone::Hips);
}

TEST(HumanoidNameMatcherTest, SpineLevelDisambiguation)
{
    HumanoidNameMatcher matcher;
    // Spine vs Chest vs UpperChest by numeric level.
    EXPECT_EQ(matcher.Match("Spine"),     HumanBone::Spine);
    EXPECT_EQ(matcher.Match("Spine_01"),  HumanBone::Spine);
    EXPECT_EQ(matcher.Match("Spine1"),    HumanBone::Spine);
    EXPECT_EQ(matcher.Match("Spine_02"),  HumanBone::Chest);
    EXPECT_EQ(matcher.Match("Spine_03"),  HumanBone::UpperChest);
    EXPECT_EQ(matcher.Match("Chest"),     HumanBone::Chest);
    EXPECT_EQ(matcher.Match("UpperChest"),HumanBone::UpperChest);
}
