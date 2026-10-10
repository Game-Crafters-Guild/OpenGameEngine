#pragma once

#include "Animation/HumanBone.h"
#include "AssetCore/Asset.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

// One canonical bone within a SkeletonProfile. The Bone field is the
// HumanBone identity; Parent is the canonical parent (HumanBone::None for
// the root). RestTranslation/Rotation/Scale describe the reference T-pose
// in the profile's canonical frame (LH +Y up, +Z forward, X-axis as the
// arm-extension axis). LocalForward / LocalUp encode the bone's authored
// "primary axis" — used by retarget IK and chain solvers to pick the
// correct rotation plane independently of how the source rig encodes it.
struct ProfileBone
{
    HumanBone Bone = HumanBone::None;
    HumanBone Parent = HumanBone::None;
    Mathematics::Vector3 RestTranslation;
    Mathematics::Quaternion RestRotation;
    Mathematics::Vector3 RestScale = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
    Mathematics::Vector3 LocalForward = Mathematics::Vector3(0.0f, 0.0f, 1.0f);
    Mathematics::Vector3 LocalUp = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
};

// SkeletonProfile defines a canonical humanoid skeleton: which HumanBone
// slots are populated, their hierarchy, rest pose, and reference axes. It
// is the contract that HumanoidRigs map to and that retargeting math is
// expressed in. Engine ships HumanoidStandard.profile.json; users can
// author custom profiles for stylized characters.
class SkeletonProfile : public ::GameEngine::Asset
{
  public:
    static constexpr int32 kSchemaVersion = 1;

    SkeletonProfile(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::SkeletonProfile, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;
    bool SaveToPath(const std::filesystem::path& path) const;

    const std::string& Name() const { return m_Name; }
    const std::string& Description() const { return m_Description; }
    int32 Version() const { return m_Version; }

    const std::vector<ProfileBone>& Bones() const { return m_Bones; }

    // Linear search; profile bone counts are small and bounded by HumanBone::Count.
    const ProfileBone* FindBone(HumanBone bone) const;

    // Test/editor seam: replace bones array directly.
    void SetBonesForTest(std::vector<ProfileBone> bones) { m_Bones = std::move(bones); }
    void SetNameForTest(std::string name) { m_Name = std::move(name); }
    void SetDescriptionForTest(std::string description) { m_Description = std::move(description); }

  private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    std::string m_Name;
    std::string m_Description;
    int32 m_Version = 0;
    std::vector<ProfileBone> m_Bones;
};

} // namespace Animation
} // namespace GameEngine
