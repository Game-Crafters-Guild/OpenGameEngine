#include "Animation/VRM1Importer.h"

#include "Animation/KHRHumanoidImporter.h"
#include "KHRHumanoidShared.h"
#include "Logger/Logger.h"

#include <cgltf.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <exception>
#include <string>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

// Compose two unit quaternions q' = a * b in (x,y,z,w) layout. Used by the
// VRM 0.x axis-flip path (rotates every rest rotation by 180 degrees on Y).
void QuatMul(const float* a, const float* b, float* out)
{
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
}

// 180-degree rotation about the Y axis (xyzw layout): (0, 1, 0, 0).
constexpr float kYFlipQuat[4] = {0.0f, 1.0f, 0.0f, 0.0f};

} // namespace

SkeletonData ApplyVRM0AxisFlip(const SkeletonData& src)
{
    SkeletonData out = src;

    // Apply the Y-axis flip to every bone's rest rotation. The rest
    // translation and bind matrices are left untouched — the engine's
    // retargeting math already operates in source-rest-relative space, so
    // a per-bone rotation flip is sufficient to converge VRM 0.x rest
    // poses onto the +Z-forward convention.
    if (out.RestRotation.size() >= static_cast<size_t>(out.BoneCount) * 4)
    {
        for (uint32 i = 0; i < out.BoneCount; ++i)
        {
            float* r = &out.RestRotation[i * 4];
            float result[4];
            QuatMul(kYFlipQuat, r, result);
            r[0] = result[0];
            r[1] = result[1];
            r[2] = result[2];
            r[3] = result[3];
        }
    }
    return out;
}

bool HasVRMExtension(const cgltf_data* gltf)
{
    if (!gltf) return false;
    return KHRDetail::FindRootExtension(gltf, "VRMC_vrm") != nullptr ||
           KHRDetail::FindRootExtension(gltf, "VRM") != nullptr;
}

namespace
{

// VRM 1.0 path. Parses VRMC_vrm.humanoid.humanBones (camelCase keys —
// same shape as KHR_humanoid). Captures metadata in outMeta.
bool ImportFromVRM1Impl(const cgltf_data* gltf,
                        const cgltf_extension* ext,
                        const SkeletonData& skel,
                        const SkeletonProfile& profile,
                        HumanoidRig& outRig,
                        VRM1Metadata& outMeta)
{
    json root;
    try
    {
        root = json::parse(ext->data);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("ImportFromVRM1: VRMC_vrm JSON parse failed: {}", e.what());
        return false;
    }

    if (root.contains("specVersion") && root["specVersion"].is_string())
        outMeta.SpecVersion = root["specVersion"].get<std::string>();
    else
        outMeta.SpecVersion = "1.0";

    if (!root.contains("humanoid") || !root["humanoid"].is_object())
    {
        Logger::Log::Warning("ImportFromVRM1: VRMC_vrm missing 'humanoid' object");
        return false;
    }
    const json& humanoid = root["humanoid"];
    if (!humanoid.contains("humanBones") || !humanoid["humanBones"].is_object())
    {
        Logger::Log::Warning("ImportFromVRM1: VRMC_vrm.humanoid missing 'humanBones'");
        return false;
    }

    if (root.contains("firstPerson") && root["firstPerson"].is_object())
    {
        const json& fp = root["firstPerson"];
        if (fp.contains("meshAnnotations") && fp["meshAnnotations"].is_array())
        {
            for (const auto& a : fp["meshAnnotations"])
            {
                if (!a.is_object()) continue;
                VRM1Metadata::FirstPersonAnnotation entry;
                if (a.contains("node") && a["node"].is_number_integer())
                    entry.MeshIndex = a["node"].get<int32>();
                if (a.contains("type") && a["type"].is_string())
                    entry.Type = a["type"].get<std::string>();
                outMeta.FirstPersonMeshAnnotations.push_back(std::move(entry));
            }
        }
    }

    if (root.contains("lookAt") && root["lookAt"].is_object())
    {
        const json& la = root["lookAt"];
        outMeta.HasLookAt = true;
        if (la.contains("type") && la["type"].is_string())
            outMeta.LookAtSettings.Type = la["type"].get<std::string>();
        if (la.contains("offsetFromHeadBone") && la["offsetFromHeadBone"].is_array() &&
            la["offsetFromHeadBone"].size() == 3)
        {
            outMeta.LookAtSettings.OffsetFromHeadX = la["offsetFromHeadBone"][0].get<float>();
            outMeta.LookAtSettings.OffsetFromHeadY = la["offsetFromHeadBone"][1].get<float>();
            outMeta.LookAtSettings.OffsetFromHeadZ = la["offsetFromHeadBone"][2].get<float>();
        }
    }

    if (root.contains("expressions") && root["expressions"].is_object())
    {
        // VRM 1.0 expressions are organized into "preset" + "custom" maps.
        // We capture clip names only (ImportableButIgnored — face system
        // owns the blendshape resolve).
        for (const auto& topKey : {"preset", "custom"})
        {
            if (!root["expressions"].contains(topKey)) continue;
            const json& bag = root["expressions"][topKey];
            if (!bag.is_object()) continue;
            for (auto it = bag.begin(); it != bag.end(); ++it)
                outMeta.Expressions.push_back(it.key());
        }
        if (!outMeta.Expressions.empty())
            Logger::Log::Info("ImportFromVRM1: captured {} expression clip names (ImportableButIgnored)",
                               outMeta.Expressions.size());
    }

    return KHRDetail::ImportFromHumanBonesJson(humanoid["humanBones"], gltf, skel, profile, outRig);
}

// VRM 0.x path. Detected via the legacy `VRM` extension name. The bone-
// list is an array of `{ bone: "hips", node: <int> }` objects. We
// translate it to the VRM 1.0 humanBones object shape and reuse the
// shared importer with a Y-flipped working copy of the SkeletonData.
bool ImportFromVRM0Impl(const cgltf_data* gltf,
                        const cgltf_extension* ext,
                        const SkeletonData& skel,
                        const SkeletonProfile& profile,
                        HumanoidRig& outRig,
                        VRM1Metadata& outMeta)
{
    Logger::Log::Warning("ImportFromVRM1: source is VRM 0.x; converting -Z forward to +Z forward via Y-axis flip on rest rotations");

    json root;
    try
    {
        root = json::parse(ext->data);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("ImportFromVRM1: VRM 0.x JSON parse failed: {}", e.what());
        return false;
    }

    outMeta.IsLegacyVRM0 = true;
    outMeta.SpecVersion = "0.x";

    if (!root.contains("humanoid") || !root["humanoid"].is_object())
    {
        Logger::Log::Warning("ImportFromVRM1: VRM 0.x missing 'humanoid'");
        return false;
    }
    const json& vrm0Humanoid = root["humanoid"];
    if (!vrm0Humanoid.contains("humanBones") || !vrm0Humanoid["humanBones"].is_array())
    {
        Logger::Log::Warning("ImportFromVRM1: VRM 0.x missing humanoid.humanBones array");
        return false;
    }

    // Translate VRM 0.x array shape to VRM 1.0 object shape. The bone
    // identifiers are the same camelCase strings; the wire format differs.
    json humanBonesObj = json::object();
    for (const auto& entry : vrm0Humanoid["humanBones"])
    {
        if (!entry.is_object()) continue;
        if (!entry.contains("bone") || !entry["bone"].is_string()) continue;
        if (!entry.contains("node") || !entry["node"].is_number_integer()) continue;
        const std::string boneName = entry["bone"].get<std::string>();
        humanBonesObj[boneName] = json::object();
        humanBonesObj[boneName]["node"] = entry["node"].get<int32>();
    }

    const SkeletonData flipped = ApplyVRM0AxisFlip(skel);
    return KHRDetail::ImportFromHumanBonesJson(humanBonesObj, gltf, flipped, profile, outRig);
}

} // namespace

bool ImportFromVRM1(const cgltf_data* gltf,
                    const SkeletonData& skel,
                    const SkeletonProfile& profile,
                    HumanoidRig& outRig,
                    VRM1Metadata& outMeta)
{
    outMeta = VRM1Metadata{};

    if (!gltf)
    {
        Logger::Log::Warning("ImportFromVRM1: null cgltf_data");
        return false;
    }

    // Try VRM 1.0 first.
    if (const cgltf_extension* ext1 = KHRDetail::FindRootExtension(gltf, "VRMC_vrm"))
    {
        if (!ext1->data || std::strlen(ext1->data) == 0)
        {
            Logger::Log::Warning("ImportFromVRM1: VRMC_vrm present but data empty");
            return false;
        }
        return ImportFromVRM1Impl(gltf, ext1, skel, profile, outRig, outMeta);
    }

    // VRM 0.x fallback path.
    if (const cgltf_extension* ext0 = KHRDetail::FindRootExtension(gltf, "VRM"))
    {
        if (!ext0->data || std::strlen(ext0->data) == 0)
        {
            Logger::Log::Warning("ImportFromVRM1: legacy VRM extension present but data empty");
            return false;
        }
        return ImportFromVRM0Impl(gltf, ext0, skel, profile, outRig, outMeta);
    }

    return false;
}

} // namespace Animation
} // namespace GameEngine
