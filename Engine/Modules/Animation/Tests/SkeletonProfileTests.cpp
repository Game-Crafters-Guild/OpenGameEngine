#include <gtest/gtest.h>

#include "Animation/HumanBone.h"
#include "Animation/SkeletonProfile.h"

#include "AssetCore/GUID.h"

#include <filesystem>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

// Build a hand-authored 5-bone profile that exercises every field we serialize.
void FillFiveBoneProfile(SkeletonProfile& profile)
{
    profile.SetNameForTest("FiveBone");
    profile.SetDescriptionForTest("Smoke-test profile for round-trip checks");

    std::vector<ProfileBone> bones;

    {
        ProfileBone b;
        b.Bone = HumanBone::Hips;
        b.Parent = HumanBone::None;
        b.RestTranslation = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        b.RestRotation = Mathematics::Quaternion::Identity();
        b.RestScale = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
        b.LocalForward = Mathematics::Vector3(0.0f, 0.0f, 1.0f);
        b.LocalUp = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Spine;
        b.Parent = HumanBone::Hips;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.15f, 0.0f);
        b.RestRotation = Mathematics::Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
        b.RestScale = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
        b.LocalForward = Mathematics::Vector3(0.0f, 0.0f, 1.0f);
        b.LocalUp = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Chest;
        b.Parent = HumanBone::Spine;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.18f, 0.0f);
        b.RestRotation = Mathematics::Quaternion(0.9659f, 0.0f, 0.2588f, 0.0f); // tiny yaw
        b.RestScale = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
        b.LocalForward = Mathematics::Vector3(0.0f, 0.0f, 1.0f);
        b.LocalUp = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Neck;
        b.Parent = HumanBone::Chest;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.20f, 0.0f);
        b.RestRotation = Mathematics::Quaternion::Identity();
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Head;
        b.Parent = HumanBone::Neck;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.10f, 0.0f);
        b.RestRotation = Mathematics::Quaternion::Identity();
        bones.push_back(b);
    }

    profile.SetBonesForTest(std::move(bones));
}

bool BonesEqual(const ProfileBone& a, const ProfileBone& b)
{
    if (a.Bone != b.Bone) return false;
    if (a.Parent != b.Parent) return false;
    auto vec3eq = [](const Mathematics::Vector3& x, const Mathematics::Vector3& y)
    {
        return x.x == y.x && x.y == y.y && x.z == y.z;
    };
    if (!vec3eq(a.RestTranslation, b.RestTranslation)) return false;
    if (!vec3eq(a.RestScale, b.RestScale)) return false;
    if (!vec3eq(a.LocalForward, b.LocalForward)) return false;
    if (!vec3eq(a.LocalUp, b.LocalUp)) return false;
    const glm::quat& qa = a.RestRotation.GetGLM();
    const glm::quat& qb = b.RestRotation.GetGLM();
    return qa.w == qb.w && qa.x == qb.x && qa.y == qb.y && qa.z == qb.z;
}

} // namespace

TEST(SkeletonProfileTest, HumanBoneStringRoundTrip)
{
    EXPECT_STREQ(HumanBoneToString(HumanBone::None), "None");
    EXPECT_STREQ(HumanBoneToString(HumanBone::Hips), "Hips");
    EXPECT_STREQ(HumanBoneToString(HumanBone::RightLittleDistal), "RightLittleDistal");

    EXPECT_EQ(HumanBoneFromString("None"), HumanBone::None);
    EXPECT_EQ(HumanBoneFromString("Hips"), HumanBone::Hips);
    EXPECT_EQ(HumanBoneFromString("RightLittleDistal"), HumanBone::RightLittleDistal);
    EXPECT_EQ(HumanBoneFromString("not-a-real-bone"), HumanBone::None);
}

TEST(SkeletonProfileTest, HumanBoneCountIs56)
{
    // 1 sentinel + 24 body + 30 fingers = 55 named values; Count == 56
    // (one past the last enumerated bone, RightLittleDistal == 55).
    EXPECT_EQ(static_cast<int>(HumanBone::Count), 56);
    EXPECT_EQ(static_cast<int>(HumanBone::RightLittleDistal), 55);
}

TEST(SkeletonProfileTest, RoundTripByteExact)
{
    SkeletonProfile original(GUID(), std::filesystem::path("test://five-bone.profile.json"));
    FillFiveBoneProfile(original);

    std::vector<uint8> bytes;
    ASSERT_TRUE(original.SaveToData(bytes));
    ASSERT_FALSE(bytes.empty());

    SkeletonProfile loaded(GUID(), std::filesystem::path("test://five-bone.profile.json"));
    ASSERT_TRUE(loaded.LoadFromData(bytes));

    // Re-serialize and compare bytes for byte-exact round-trip.
    std::vector<uint8> bytes2;
    ASSERT_TRUE(loaded.SaveToData(bytes2));
    EXPECT_EQ(bytes.size(), bytes2.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), bytes2.begin()));

    // And per-field structural compare. Version is always set to the
    // current schema (kSchemaVersion) by the serializer regardless of the
    // in-memory version, so both the original and loaded versions must
    // match the on-disk schema version after one round-trip.
    EXPECT_EQ(original.Name(), loaded.Name());
    EXPECT_EQ(original.Description(), loaded.Description());
    EXPECT_EQ(loaded.Version(), SkeletonProfile::kSchemaVersion);
    ASSERT_EQ(original.Bones().size(), loaded.Bones().size());
    for (size_t i = 0; i < original.Bones().size(); ++i)
    {
        EXPECT_TRUE(BonesEqual(original.Bones()[i], loaded.Bones()[i]))
            << "bone index " << i;
    }
}

TEST(SkeletonProfileTest, MalformedJsonReturnsFalse)
{
    SkeletonProfile p(GUID(), std::filesystem::path("test://malformed.profile.json"));
    const std::string broken = "{ this is not valid json :: ";
    Vector<uint8> bytes(broken.begin(), broken.end());
    EXPECT_FALSE(p.LoadFromData(bytes));
}

TEST(SkeletonProfileTest, MissingVersionDefaultsToZero)
{
    SkeletonProfile p(GUID(), std::filesystem::path("test://no-version.profile.json"));
    const std::string text = R"({"name":"x","bones":[]})";
    Vector<uint8> bytes(text.begin(), text.end());
    ASSERT_TRUE(p.LoadFromData(bytes));
    EXPECT_EQ(p.Version(), 0);
}

TEST(SkeletonProfileTest, BumpedVersionLoadsAnyway)
{
    // Bumped version (newer than supported) logs a warning but parses.
    SkeletonProfile p(GUID(), std::filesystem::path("test://future.profile.json"));
    const std::string text = R"({"version":99,"name":"future","bones":[]})";
    Vector<uint8> bytes(text.begin(), text.end());
    ASSERT_TRUE(p.LoadFromData(bytes));
    EXPECT_EQ(p.Version(), 99);
    EXPECT_EQ(p.Name(), "future");
}

TEST(SkeletonProfileTest, FindBoneReturnsNullForMissing)
{
    SkeletonProfile profile(GUID(), std::filesystem::path("test://five-bone.profile.json"));
    FillFiveBoneProfile(profile);
    EXPECT_NE(profile.FindBone(HumanBone::Hips), nullptr);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftEye), nullptr);
}
