#include <gtest/gtest.h>

#include "Animation/HumanBone.h"
#include "Animation/SkeletonProfile.h"

#include "AssetCore/GUID.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
// windows.h defines GUID; we use ::GameEngine::GUID below.
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#endif

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

// Locate the executable's own directory so the test can resolve assets
// staged alongside it via POST_BUILD, regardless of cwd.
std::filesystem::path ExeDir()
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, len)).parent_path();
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    return std::filesystem::canonical(buf).parent_path();
#else
    char buf[1024];
    const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf));
    if (len <= 0)
        return {};
    return std::filesystem::path(std::string(buf, len)).parent_path();
#endif
}

} // namespace

TEST(HumanoidStandardProfileTest, FileExistsAndParses)
{
    // Resolve relative to the test exe's directory. CMake POST_BUILD on this
    // target stages HumanoidStandard.profile.json to TestAssets/ next to the
    // exe, so this path is hermetic — no cwd dependence, no candidate paths.
    const std::filesystem::path path =
        ExeDir() / "TestAssets" / "SkeletonProfiles" / "HumanoidStandard.profile.json";
    ASSERT_TRUE(std::filesystem::exists(path))
        << "HumanoidStandard.profile.json missing at " << path.string();

    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path.string();

    std::vector<uint8> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());

    SkeletonProfile profile(::GameEngine::GUID(), path);
    ASSERT_TRUE(profile.LoadFromData(bytes));

    EXPECT_EQ(profile.Name(), "HumanoidStandard");
    EXPECT_EQ(profile.Version(), 1);

    // 25 anatomical body slots in v1 (no fingers): 6 spine + 4 left arm +
    // 4 right arm + 4 left leg + 4 right leg + 3 face (LeftEye, RightEye, Jaw).
    EXPECT_EQ(profile.Bones().size(), 25u);

    // Spot-check hierarchy + identity bones.
    EXPECT_NE(profile.FindBone(HumanBone::Hips), nullptr);
    EXPECT_NE(profile.FindBone(HumanBone::Head), nullptr);
    EXPECT_NE(profile.FindBone(HumanBone::LeftEye), nullptr);
    EXPECT_NE(profile.FindBone(HumanBone::RightEye), nullptr);
    EXPECT_NE(profile.FindBone(HumanBone::Jaw), nullptr);

    // Hips is the root.
    EXPECT_EQ(profile.FindBone(HumanBone::Hips)->Parent, HumanBone::None);

    // Spine -> Hips, Chest -> Spine, UpperChest -> Chest, Neck -> UpperChest,
    // Head -> Neck. Walking the canonical spine chain should reach the root.
    EXPECT_EQ(profile.FindBone(HumanBone::Spine)->Parent, HumanBone::Hips);
    EXPECT_EQ(profile.FindBone(HumanBone::Chest)->Parent, HumanBone::Spine);
    EXPECT_EQ(profile.FindBone(HumanBone::UpperChest)->Parent, HumanBone::Chest);
    EXPECT_EQ(profile.FindBone(HumanBone::Neck)->Parent, HumanBone::UpperChest);
    EXPECT_EQ(profile.FindBone(HumanBone::Head)->Parent, HumanBone::Neck);

    // Arm chain.
    EXPECT_EQ(profile.FindBone(HumanBone::LeftShoulder)->Parent, HumanBone::UpperChest);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftUpperArm)->Parent, HumanBone::LeftShoulder);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftLowerArm)->Parent, HumanBone::LeftUpperArm);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftHand)->Parent, HumanBone::LeftLowerArm);

    // Leg chain.
    EXPECT_EQ(profile.FindBone(HumanBone::LeftUpperLeg)->Parent, HumanBone::Hips);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftLowerLeg)->Parent, HumanBone::LeftUpperLeg);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftFoot)->Parent, HumanBone::LeftLowerLeg);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftToes)->Parent, HumanBone::LeftFoot);
}
