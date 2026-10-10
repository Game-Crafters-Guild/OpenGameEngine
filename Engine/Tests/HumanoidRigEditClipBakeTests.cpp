#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/HumanoidRigEdit.h"
#include "Animation/SkeletonData.h"

#include "AssetCore/GUID.h"
#include "Mathematics/Quaternion.h"

#include <cmath>
#include <filesystem>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

constexpr float kPi = 3.14159265358979323846f;

void FillRig(HumanoidRig& rig)
{
    auto& bm = rig.BoneMapMutable();
    bm.clear();
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Hips;
        m.SourceBoneName = "src:Hips";
        bm.push_back(m);
    }
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Spine;
        m.SourceBoneName = "src:Spine";
        bm.push_back(m);
    }
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Head;
        m.SourceBoneName = "src:Head";
        bm.push_back(m);
    }
}

} // namespace

TEST(HumanoidRigEditClipBake, AppliesPerBoneRotationsAtRequestedTime)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://edit.humanoidrig.json"));
    FillRig(rig);

    AnimationClip clip(GUID(), std::filesystem::path("test://edit.clip"));
    {
        std::vector<AnimChannel> channels;

        AnimChannel spine;
        spine.targetName = "src:Spine";
        spine.path = AnimPath::Rotation;
        spine.interp = AnimInterp::Linear;

        const float halfRad = (30.0f * kPi / 180.0f) * 0.5f;
        AnimKeyframe k0{};
        k0.time = 0.0f;
        k0.rotation[0] = 0.0f;
        k0.rotation[1] = 0.0f;
        k0.rotation[2] = 0.0f;
        k0.rotation[3] = 1.0f;
        AnimKeyframe k1{};
        k1.time = 1.0f;
        k1.rotation[0] = std::sin(halfRad);
        k1.rotation[1] = 0.0f;
        k1.rotation[2] = 0.0f;
        k1.rotation[3] = std::cos(halfRad);
        spine.keys.push_back(k0);
        spine.keys.push_back(k1);
        channels.push_back(spine);

        clip.SetChannelsAndDurationForTest(channels, 1.0f);
    }

    SkeletonData skel;
    const int updated = BakeRetargetPoseFromClipFrame(rig, clip, skel, 1.0f, 5.0f);
    EXPECT_EQ(updated, 1);

    EXPECT_NEAR(rig.BoneMap()[0].RetargetPoseRotation.GetGLM().w, 1.0f, 1e-5f);
    const float halfRadCheck = (30.0f * kPi / 180.0f) * 0.5f;
    EXPECT_NEAR(rig.BoneMap()[1].RetargetPoseRotation.GetGLM().w, std::cos(halfRadCheck), 1e-3f);
    EXPECT_NEAR(rig.BoneMap()[1].RetargetPoseRotation.GetGLM().x, std::sin(halfRadCheck), 1e-3f);
    EXPECT_NEAR(rig.BoneMap()[2].RetargetPoseRotation.GetGLM().w, 1.0f, 1e-5f);
}

TEST(HumanoidRigEditClipBake, SnapsToIdentityWhenWithinEpsilon)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://edit.humanoidrig.json"));
    FillRig(rig);

    AnimationClip clip(GUID(), std::filesystem::path("test://edit.clip"));
    {
        std::vector<AnimChannel> channels;
        AnimChannel hips;
        hips.targetName = "src:Hips";
        hips.path = AnimPath::Rotation;
        hips.interp = AnimInterp::Linear;
        const float halfRad = (1.0f * kPi / 180.0f) * 0.5f;
        AnimKeyframe k{};
        k.time = 0.0f;
        k.rotation[0] = std::sin(halfRad);
        k.rotation[1] = 0.0f;
        k.rotation[2] = 0.0f;
        k.rotation[3] = std::cos(halfRad);
        hips.keys.push_back(k);
        channels.push_back(hips);
        clip.SetChannelsAndDurationForTest(channels, 0.0f);
    }

    SkeletonData skel;
    BakeRetargetPoseFromClipFrame(rig, clip, skel, 0.0f, 5.0f);

    EXPECT_NEAR(rig.BoneMap()[0].RetargetPoseRotation.GetGLM().w, 1.0f, 1e-5f);
}

TEST(HumanoidRigEditClipBake, EmptyClipLeavesRigUnchanged)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://edit.humanoidrig.json"));
    FillRig(rig);
    rig.BoneMapMutable()[1].RetargetPoseRotation = Mathematics::Quaternion(0.7071f, 0.7071f, 0.0f, 0.0f);

    AnimationClip clip(GUID(), std::filesystem::path("test://edit.clip"));
    SkeletonData skel;
    const int updated = BakeRetargetPoseFromClipFrame(rig, clip, skel, 0.5f, 5.0f);
    EXPECT_EQ(updated, 0);

    // Bone 1's manually-set rotation must persist (no clip channels means
    // the helper never wrote to it).
    EXPECT_NEAR(rig.BoneMap()[1].RetargetPoseRotation.GetGLM().w, 0.7071f, 1e-3f);
    EXPECT_NEAR(rig.BoneMap()[1].RetargetPoseRotation.GetGLM().x, 0.7071f, 1e-3f);
}
