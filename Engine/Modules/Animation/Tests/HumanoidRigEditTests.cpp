#include <gtest/gtest.h>

#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/HumanoidRigEdit.h"

#include "AssetCore/GUID.h"
#include "Mathematics/Quaternion.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

constexpr float kPi = 3.14159265358979323846f;

void FillRigForEditTests(HumanoidRig& rig)
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

TEST(HumanoidRigEdit, ForceRetargetPoseToIdentity_ResetsAllRotationsToIdentity)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://edit.humanoidrig.json"));
    FillRigForEditTests(rig);
    rig.BoneMapMutable()[0].RetargetPoseRotation = Mathematics::Quaternion(0.7071f, 0.7071f, 0.0f, 0.0f);
    rig.BoneMapMutable()[1].RetargetPoseRotation = Mathematics::Quaternion(0.5f, 0.5f, 0.5f, 0.5f);
    rig.BoneMapMutable()[2].RetargetPoseRotation = Mathematics::Quaternion(0.9f, 0.0f, 0.4359f, 0.0f);

    ForceRetargetPoseToIdentity(rig);

    for (const auto& m : rig.BoneMap())
    {
        EXPECT_NEAR(m.RetargetPoseRotation.GetGLM().w, 1.0f, 1e-5f);
        EXPECT_NEAR(m.RetargetPoseRotation.GetGLM().x, 0.0f, 1e-5f);
        EXPECT_NEAR(m.RetargetPoseRotation.GetGLM().y, 0.0f, 1e-5f);
        EXPECT_NEAR(m.RetargetPoseRotation.GetGLM().z, 0.0f, 1e-5f);
    }
}

TEST(HumanoidRigEdit, CapturePoseFromPreview_StampsNonIdentityRotations)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://edit.humanoidrig.json"));
    FillRigForEditTests(rig);

    std::vector<Mathematics::Quaternion> capture(rig.BoneMap().size());
    capture[0] = Mathematics::Quaternion::Identity();
    // 30 deg around X.
    const float halfRad = (30.0f * kPi / 180.0f) * 0.5f;
    capture[1] = Mathematics::Quaternion(std::cos(halfRad), std::sin(halfRad), 0.0f, 0.0f);
    capture[2] = Mathematics::Quaternion::Identity();

    CaptureRetargetPoseFromPreview(rig, capture, 5.0f);

    EXPECT_FLOAT_EQ(rig.BoneMap()[0].RetargetPoseRotation.GetGLM().w, 1.0f);
    // Bone 1 should retain a non-identity rotation (>5 deg from identity).
    EXPECT_LT(rig.BoneMap()[1].RetargetPoseRotation.GetGLM().w, 0.99f);
    EXPECT_FLOAT_EQ(rig.BoneMap()[2].RetargetPoseRotation.GetGLM().w, 1.0f);
}

TEST(HumanoidRigEdit, CapturePoseFromPreview_SnapsSmallRotationsToIdentity)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://edit.humanoidrig.json"));
    FillRigForEditTests(rig);

    std::vector<Mathematics::Quaternion> capture(rig.BoneMap().size());
    // 1 deg around X — within the 5 deg snap threshold.
    const float halfRad = (1.0f * kPi / 180.0f) * 0.5f;
    capture[0] = Mathematics::Quaternion(std::cos(halfRad), std::sin(halfRad), 0.0f, 0.0f);
    capture[1] = Mathematics::Quaternion::Identity();
    capture[2] = Mathematics::Quaternion::Identity();

    CaptureRetargetPoseFromPreview(rig, capture, 5.0f);

    // All snapped to identity.
    for (const auto& m : rig.BoneMap())
    {
        EXPECT_NEAR(m.RetargetPoseRotation.GetGLM().w, 1.0f, 1e-5f);
    }
}

TEST(HumanoidRigEdit, CapturePoseFromPreview_RejectsMismatchedSize)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://edit.humanoidrig.json"));
    FillRigForEditTests(rig);
    // Stash original rotations.
    auto originals = rig.BoneMap();

    std::vector<Mathematics::Quaternion> tooShort(2);
    tooShort[0] = Mathematics::Quaternion(0.7071f, 0.0f, 0.7071f, 0.0f);
    tooShort[1] = Mathematics::Quaternion(0.7071f, 0.7071f, 0.0f, 0.0f);

    CaptureRetargetPoseFromPreview(rig, tooShort, 5.0f);

    for (size_t i = 0; i < rig.BoneMap().size(); ++i)
    {
        EXPECT_EQ(rig.BoneMap()[i].RetargetPoseRotation.GetGLM().w, originals[i].RetargetPoseRotation.GetGLM().w);
    }
}

// BakeRetargetPoseFromClipFrame depends on AnimationClip::Load + the Engine
// library's Asset I/O surface. Coverage for the bake helper lives in
// Engine/Tests/HumanoidRigEditClipBakeTests.cpp where the Engine library is
// already linked. Keep the Animation-module test executable a pure
// data-and-math suite.
