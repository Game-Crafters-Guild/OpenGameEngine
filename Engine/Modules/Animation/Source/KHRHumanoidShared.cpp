#include "KHRHumanoidShared.h"

#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidNameMatcher.h"
#include "Animation/KHRHumanoidImporter.h"
#include "Logger/Logger.h"

#include <cgltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{
namespace KHRDetail
{

namespace
{

// Returns the canonical bones that participate in each ChainKind. Mirrors
// HumanoidImport.cpp's CanonicalChainBones; duplicated locally to keep the
// importer translation unit self-contained.
const std::vector<HumanBone>& CanonicalChainBones(ChainKind kind)
{
    static const std::vector<HumanBone> kEmpty;
    static const std::vector<HumanBone> kLeftArm  = {HumanBone::LeftShoulder, HumanBone::LeftUpperArm, HumanBone::LeftLowerArm, HumanBone::LeftHand};
    static const std::vector<HumanBone> kRightArm = {HumanBone::RightShoulder, HumanBone::RightUpperArm, HumanBone::RightLowerArm, HumanBone::RightHand};
    static const std::vector<HumanBone> kLeftLeg  = {HumanBone::LeftUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftFoot, HumanBone::LeftToes};
    static const std::vector<HumanBone> kRightLeg = {HumanBone::RightUpperLeg, HumanBone::RightLowerLeg, HumanBone::RightFoot, HumanBone::RightToes};
    static const std::vector<HumanBone> kSpine    = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest, HumanBone::UpperChest, HumanBone::Neck, HumanBone::Head};
    static const std::vector<HumanBone> kHead     = {HumanBone::Neck, HumanBone::Head};

    switch (kind)
    {
        case ChainKind::LeftArm:  return kLeftArm;
        case ChainKind::RightArm: return kRightArm;
        case ChainKind::LeftLeg:  return kLeftLeg;
        case ChainKind::RightLeg: return kRightLeg;
        case ChainKind::Spine:    return kSpine;
        case ChainKind::Head:     return kHead;
        default:                  return kEmpty;
    }
}

uint32 SkinJointForNode(const SkeletonData& skel, uint32 nodeIdx)
{
    for (uint32 j = 0; j < skel.SkinJointCount; ++j)
    {
        if (j < skel.JointNodes.size() && skel.JointNodes[j] == nodeIdx)
            return j;
    }
    return ~0u;
}

} // namespace

const cgltf_extension* FindRootExtension(const cgltf_data* gltf,
                                         const char* name)
{
    if (!gltf || !name) return nullptr;
    for (cgltf_size i = 0; i < gltf->data_extensions_count; ++i)
    {
        const cgltf_extension& ext = gltf->data_extensions[i];
        if (ext.name && std::strcmp(ext.name, name) == 0) return &ext;
    }
    return nullptr;
}

int32 NodeIndex(const cgltf_data* gltf, const cgltf_node* node)
{
    if (!gltf || !node) return -1;
    for (cgltf_size i = 0; i < gltf->nodes_count; ++i)
        if (&gltf->nodes[i] == node) return static_cast<int32>(i);
    return -1;
}

uint32 ResolveSkinJoint(const cgltf_data* gltf, const SkeletonData& skel,
                        uint32 nodeIdx, std::string& outSourceName)
{
    outSourceName.clear();

    if (gltf && nodeIdx < gltf->nodes_count && gltf->nodes[nodeIdx].name)
        outSourceName = gltf->nodes[nodeIdx].name;

    uint32 boneIdx = SkinJointForNode(skel, nodeIdx);
    if (boneIdx != ~0u) return boneIdx;

    // Fall back to bone-name lookup.
    if (!outSourceName.empty())
    {
        for (uint32 i = 0; i < skel.BoneCount; ++i)
        {
            if (i < skel.BoneNames.size() && skel.BoneNames[i] == outSourceName)
                return i;
        }
    }
    return ~0u;
}

bool ImportFromHumanBonesJson(const nlohmann::json& humanBones,
                              const cgltf_data* gltf,
                              const SkeletonData& skel,
                              const SkeletonProfile& profile,
                              HumanoidRig& outRig)
{
    using json = nlohmann::json;

    if (!humanBones.is_object())
    {
        Logger::Log::Warning("KHR/VRM importer: humanBones is not an object");
        return false;
    }

    auto& boneMap = outRig.BoneMapMutable();
    boneMap.clear();
    boneMap.reserve(humanBones.size());

    uint32 requiredMatched = 0;
    uint32 requiredTotal = 0;
    {
        // Iterate every HumanBone enum value once to count required slots.
        for (uint32 v = 1; v < static_cast<uint32>(HumanBone::Count); ++v)
        {
            const HumanBone b = static_cast<HumanBone>(v);
            if (HumanoidNameMatcher::IsRequiredBone(b))
                ++requiredTotal;
        }
    }

    for (auto it = humanBones.begin(); it != humanBones.end(); ++it)
    {
        const std::string& key = it.key();
        const json& entry = it.value();
        if (!entry.is_object()) continue;
        if (!entry.contains("node") || !entry["node"].is_number_integer()) continue;

        const HumanBone bone = HumanBoneFromKHRName(key);
        if (bone == HumanBone::None)
        {
            Logger::Log::Warning("KHR/VRM importer: unknown humanBones entry '{}'", key);
            continue;
        }

        const int32 nodeIdx = entry["node"].get<int32>();
        if (nodeIdx < 0 || (gltf && static_cast<cgltf_size>(nodeIdx) >= gltf->nodes_count))
        {
            Logger::Log::Warning("KHR/VRM importer: humanBones[{}].node = {} out of range",
                                 key, nodeIdx);
            continue;
        }

        std::string sourceName;
        const uint32 boneIdx = ResolveSkinJoint(gltf, skel,
                                                static_cast<uint32>(nodeIdx),
                                                sourceName);

        HumanoidBoneMapping m;
        m.Canonical = bone;
        m.SourceBoneName = sourceName;
        m.CachedSourceIndex = boneIdx;
        m.RetargetPoseRotation = Mathematics::Quaternion::Identity();
        boneMap.push_back(m);

        if (boneIdx != ~0u && HumanoidNameMatcher::IsRequiredBone(bone))
            ++requiredMatched;
    }

    const float coverage = (requiredTotal > 0)
                           ? static_cast<float>(requiredMatched) / static_cast<float>(requiredTotal)
                           : 0.0f;
    if (coverage < kAutoImportCoverageThreshold)
    {
        Logger::Log::Warning("KHR/VRM importer: required-bone coverage {:.0f}% below threshold {:.0f}%",
                             coverage * 100.0f,
                             kAutoImportCoverageThreshold * 100.0f);
        return false;
    }

    auto findMapping = [&](HumanBone canonical) -> const HumanoidBoneMapping*
    {
        for (const auto& m : boneMap)
            if (m.Canonical == canonical) return &m;
        return nullptr;
    };

    auto& chains = outRig.ChainsMutable();
    chains.clear();

    auto emitChain = [&](ChainKind kind)
    {
        const auto& bones = CanonicalChainBones(kind);
        if (bones.size() < 2) return;

        std::vector<HumanBone> mapped;
        mapped.reserve(bones.size());
        for (HumanBone b : bones)
            if (findMapping(b)) mapped.push_back(b);
        if (mapped.size() < 2) return;

        HumanoidChain c;
        c.Kind = kind;
        c.Start = mapped.front();
        c.End = mapped.back();
        c.IncludeBones = std::move(mapped);
        chains.push_back(std::move(c));
    };

    emitChain(ChainKind::Spine);
    emitChain(ChainKind::LeftArm);
    emitChain(ChainKind::RightArm);
    emitChain(ChainKind::LeftLeg);
    emitChain(ChainKind::RightLeg);
    emitChain(ChainKind::Head);

    auto& prop = outRig.ProportionsMutable();
    prop = BodyProportions{};

    auto worldOf = [&](HumanBone b) -> std::pair<bool, std::array<float, 3>>
    {
        const HumanoidBoneMapping* m = findMapping(b);
        if (!m || m->CachedSourceIndex == ~0u) return {false, {0.0f, 0.0f, 0.0f}};
        const uint32 idx = m->CachedSourceIndex;
        if (skel.BindPose.size() < (idx + 1) * 16)
            return {false, {0.0f, 0.0f, 0.0f}};
        const float* bp = &skel.BindPose[idx * 16];
        return {true, {bp[12], bp[13], bp[14]}};
    };

    auto distance = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> float
    {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    const auto hipsPos      = worldOf(HumanBone::Hips);
    const auto leftFootPos  = worldOf(HumanBone::LeftFoot);
    const auto leftShPos    = worldOf(HumanBone::LeftShoulder);
    const auto rightShPos   = worldOf(HumanBone::RightShoulder);
    const auto leftHandPos  = worldOf(HumanBone::LeftHand);
    const auto leftUpLegPos = worldOf(HumanBone::LeftUpperLeg);

    if (hipsPos.first) prop.HipHeight = std::abs(hipsPos.second[1]);
    if (leftShPos.first && rightShPos.first)
        prop.ShoulderWidth = distance(leftShPos.second, rightShPos.second);
    if (leftUpLegPos.first && leftFootPos.first)
        prop.LegLength = distance(leftUpLegPos.second, leftFootPos.second);
    else if (hipsPos.first && leftFootPos.first)
        prop.LegLength = distance(hipsPos.second, leftFootPos.second);
    if (leftShPos.first && leftHandPos.first)
        prop.ArmLength = distance(leftShPos.second, leftHandPos.second);

    auto& tb = outRig.TranslationBonesMutable();
    tb.clear();
    if (findMapping(HumanBone::Hips))
        tb.push_back(HumanBone::Hips);

    BakeRetargetPoseFromAPose(skel, profile, outRig);

    outRig.SetProfileRef(profile.GetGUID());
    return true;
}

} // namespace KHRDetail
} // namespace Animation
} // namespace GameEngine
