#include <gtest/gtest.h>

#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"

#include "AssetCore/GUID.h"

#include <filesystem>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

void FillRig(HumanoidRig& rig)
{
    rig.SetProfileRef(GUID("01234567-89ab-cdef-0123-456789abcdef"));

    auto& bm = rig.BoneMapMutable();
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Hips;
        m.SourceBoneName = "mixamorig:Hips";
        m.CachedSourceIndex = 7; // intentionally non-default; should NOT round-trip
        m.RetargetPoseRotation = Mathematics::Quaternion::Identity();
        bm.push_back(m);
    }
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Spine;
        m.SourceBoneName = "mixamorig:Spine";
        m.RetargetPoseRotation = Mathematics::Quaternion(0.9f, 0.1f, 0.2f, 0.3f);
        bm.push_back(m);
    }
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Head;
        m.SourceBoneName = "mixamorig:Head";
        bm.push_back(m);
    }
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::LeftUpperArm;
        m.SourceBoneName = "mixamorig:LeftArm";
        bm.push_back(m);
    }
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::LeftLowerArm;
        m.SourceBoneName = "mixamorig:LeftForeArm";
        bm.push_back(m);
    }

    auto& chains = rig.ChainsMutable();
    {
        HumanoidChain c;
        c.Kind = ChainKind::LeftArm;
        c.Start = HumanBone::LeftUpperArm;
        c.End = HumanBone::LeftHand;
        c.IncludeBones = {HumanBone::LeftUpperArm, HumanBone::LeftLowerArm, HumanBone::LeftHand};
        chains.push_back(c);
    }
    {
        HumanoidChain c;
        c.Kind = ChainKind::Spine;
        c.Start = HumanBone::Hips;
        c.End = HumanBone::Head;
        c.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest, HumanBone::Neck, HumanBone::Head};
        chains.push_back(c);
    }

    auto& tb = rig.TranslationBonesMutable();
    tb.clear();
    tb.push_back(HumanBone::Hips);

    auto& prop = rig.ProportionsMutable();
    prop.HipHeight = 1.0f;
    prop.ShoulderWidth = 0.42f;
    prop.LegLength = 0.85f;
    prop.ArmLength = 0.62f;
    for (size_t i = 0; i < prop.PerChainLengths.size(); ++i)
        prop.PerChainLengths[i] = 0.1f * static_cast<float>(i);

    auto& atts = rig.AttachmentsMutable();
    {
        AttachmentBone a;
        a.Name = "CloakAnchor";
        a.ParentBone = HumanBone::Chest;
        a.LocalOffset = Mathematics::Vector3(0.0f, -0.1f, -0.05f);
        a.Mode = AttachmentPassthroughMode::Procedural;
        atts.push_back(a);
    }
}

} // namespace

TEST(HumanoidRigTest, ChainKindStringRoundTrip)
{
    EXPECT_EQ(ChainKindFromString("LeftArm"), ChainKind::LeftArm);
    EXPECT_EQ(ChainKindFromString("Spine"), ChainKind::Spine);
    EXPECT_EQ(ChainKindFromString("garbage"), ChainKind::Other);
    EXPECT_STREQ(ChainKindToString(ChainKind::LeftFingers), "LeftFingers");
}

TEST(HumanoidRigTest, AttachmentModeStringRoundTrip)
{
    EXPECT_EQ(AttachmentPassthroughModeFromString("Procedural"), AttachmentPassthroughMode::Procedural);
    EXPECT_EQ(AttachmentPassthroughModeFromString("Static"), AttachmentPassthroughMode::Static);
    EXPECT_EQ(AttachmentPassthroughModeFromString("garbage"), AttachmentPassthroughMode::CopyLocal);
    EXPECT_STREQ(AttachmentPassthroughModeToString(AttachmentPassthroughMode::Static), "Static");
}

TEST(HumanoidRigTest, RoundTripPreservesAllFields)
{
    HumanoidRig original(GUID(), std::filesystem::path("test://example.humanoidrig.json"));
    FillRig(original);

    std::vector<uint8> bytes;
    ASSERT_TRUE(original.SaveToData(bytes));

    HumanoidRig loaded(GUID(), std::filesystem::path("test://example.humanoidrig.json"));
    ASSERT_TRUE(loaded.LoadFromData(bytes));

    EXPECT_EQ(original.ProfileRef().ToString(), loaded.ProfileRef().ToString());
    ASSERT_EQ(original.BoneMap().size(), loaded.BoneMap().size());
    for (size_t i = 0; i < original.BoneMap().size(); ++i)
    {
        EXPECT_EQ(original.BoneMap()[i].Canonical, loaded.BoneMap()[i].Canonical);
        EXPECT_EQ(original.BoneMap()[i].SourceBoneName, loaded.BoneMap()[i].SourceBoneName);
        const glm::quat& qa = original.BoneMap()[i].RetargetPoseRotation.GetGLM();
        const glm::quat& qb = loaded.BoneMap()[i].RetargetPoseRotation.GetGLM();
        EXPECT_FLOAT_EQ(qa.w, qb.w);
        EXPECT_FLOAT_EQ(qa.x, qb.x);
        EXPECT_FLOAT_EQ(qa.y, qb.y);
        EXPECT_FLOAT_EQ(qa.z, qb.z);
    }

    ASSERT_EQ(original.Chains().size(), loaded.Chains().size());
    for (size_t i = 0; i < original.Chains().size(); ++i)
    {
        EXPECT_EQ(original.Chains()[i].Kind, loaded.Chains()[i].Kind);
        EXPECT_EQ(original.Chains()[i].Start, loaded.Chains()[i].Start);
        EXPECT_EQ(original.Chains()[i].End, loaded.Chains()[i].End);
        ASSERT_EQ(original.Chains()[i].IncludeBones.size(), loaded.Chains()[i].IncludeBones.size());
        for (size_t k = 0; k < original.Chains()[i].IncludeBones.size(); ++k)
            EXPECT_EQ(original.Chains()[i].IncludeBones[k], loaded.Chains()[i].IncludeBones[k]);
    }

    ASSERT_EQ(original.TranslationBones().size(), loaded.TranslationBones().size());
    for (size_t i = 0; i < original.TranslationBones().size(); ++i)
        EXPECT_EQ(original.TranslationBones()[i], loaded.TranslationBones()[i]);

    EXPECT_FLOAT_EQ(original.Proportions().HipHeight, loaded.Proportions().HipHeight);
    EXPECT_FLOAT_EQ(original.Proportions().ShoulderWidth, loaded.Proportions().ShoulderWidth);
    EXPECT_FLOAT_EQ(original.Proportions().LegLength, loaded.Proportions().LegLength);
    EXPECT_FLOAT_EQ(original.Proportions().ArmLength, loaded.Proportions().ArmLength);
    for (size_t i = 0; i < original.Proportions().PerChainLengths.size(); ++i)
        EXPECT_FLOAT_EQ(original.Proportions().PerChainLengths[i], loaded.Proportions().PerChainLengths[i]);

    ASSERT_EQ(original.Attachments().size(), loaded.Attachments().size());
    for (size_t i = 0; i < original.Attachments().size(); ++i)
    {
        EXPECT_EQ(original.Attachments()[i].Name, loaded.Attachments()[i].Name);
        EXPECT_EQ(original.Attachments()[i].ParentBone, loaded.Attachments()[i].ParentBone);
        EXPECT_EQ(original.Attachments()[i].Mode, loaded.Attachments()[i].Mode);
    }
}

TEST(HumanoidRigTest, CachedSourceIndexResetsOnLoad)
{
    HumanoidRig original(GUID(), std::filesystem::path("test://example.humanoidrig.json"));
    FillRig(original);
    // The first mapping was set with CachedSourceIndex = 7; it must NOT round-trip.
    ASSERT_EQ(original.BoneMap()[0].CachedSourceIndex, 7u);

    std::vector<uint8> bytes;
    ASSERT_TRUE(original.SaveToData(bytes));

    HumanoidRig loaded(GUID(), std::filesystem::path("test://example.humanoidrig.json"));
    ASSERT_TRUE(loaded.LoadFromData(bytes));

    EXPECT_EQ(loaded.BoneMap()[0].CachedSourceIndex, ~0u);
}

TEST(HumanoidRigTest, MissingFieldsHaveSensibleDefaults)
{
    HumanoidRig rig(GUID(), std::filesystem::path("test://minimal.humanoidrig.json"));
    const std::string minimal = R"({"version":1})";
    Vector<uint8> bytes(minimal.begin(), minimal.end());
    ASSERT_TRUE(rig.LoadFromData(bytes));
    EXPECT_TRUE(rig.ProfileRef().IsNull());
    EXPECT_TRUE(rig.BoneMap().empty());
    EXPECT_TRUE(rig.Chains().empty());
    // TranslationBones defaults to {Hips} when omitted.
    ASSERT_EQ(rig.TranslationBones().size(), 1u);
    EXPECT_EQ(rig.TranslationBones()[0], HumanBone::Hips);
}
