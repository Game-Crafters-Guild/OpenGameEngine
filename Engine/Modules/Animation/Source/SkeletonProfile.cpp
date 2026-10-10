#include "Animation/SkeletonProfile.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

json Vec3ToJson(const Mathematics::Vector3& v)
{
    return json::array({v.x, v.y, v.z});
}

Mathematics::Vector3 JsonToVec3(const json& j, const Mathematics::Vector3& fallback)
{
    if (!j.is_array() || j.size() < 3)
        return fallback;
    return Mathematics::Vector3(
        j[0].get<float>(),
        j[1].get<float>(),
        j[2].get<float>());
}

json QuatToJson(const Mathematics::Quaternion& q)
{
    const glm::quat& g = q.GetGLM();
    return json::array({g.w, g.x, g.y, g.z});
}

Mathematics::Quaternion JsonToQuat(const json& j, const Mathematics::Quaternion& fallback)
{
    if (!j.is_array() || j.size() < 4)
        return fallback;
    return Mathematics::Quaternion(
        j[0].get<float>(),
        j[1].get<float>(),
        j[2].get<float>(),
        j[3].get<float>());
}

} // namespace

bool SkeletonProfile::Load()
{
    SetState(AssetState::Loading);

    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("SkeletonProfile: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool SkeletonProfile::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void SkeletonProfile::Unload()
{
    m_Name.clear();
    m_Description.clear();
    m_Version = 0;
    m_Bones.clear();
    SetState(AssetState::Unloaded);
}

bool SkeletonProfile::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool SkeletonProfile::SaveToPath(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Log::Error("SkeletonProfile: cannot open '{}' for write", path.string());
        return false;
    }
    const std::string serialized = SerializeJson();
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return file.good();
}

const ProfileBone* SkeletonProfile::FindBone(HumanBone bone) const
{
    for (const auto& pb : m_Bones)
    {
        if (pb.Bone == bone)
            return &pb;
    }
    return nullptr;
}

bool SkeletonProfile::ParseJson(const std::string& text)
{
    try
    {
        const auto doc = json::parse(text);

        // Schema version. Missing = 0; bumped (newer than supported) logs a warning
        // but proceeds with best-effort parsing for forward compatibility.
        m_Version = doc.value("version", 0);
        if (m_Version > kSchemaVersion)
        {
            Logger::Log::Warning(
                "SkeletonProfile '{}' has version {} but engine supports up to {}; reading anyway",
                GetPath().string(), m_Version, kSchemaVersion);
        }

        m_Name = doc.value("name", std::string());
        m_Description = doc.value("description", std::string());

        m_Bones.clear();
        if (doc.contains("bones") && doc["bones"].is_array())
        {
            m_Bones.reserve(doc["bones"].size());
            for (const auto& boneJson : doc["bones"])
            {
                ProfileBone pb;
                pb.Bone = HumanBoneFromString(boneJson.value("bone", std::string("None")));
                pb.Parent = HumanBoneFromString(boneJson.value("parent", std::string("None")));
                pb.RestTranslation = JsonToVec3(boneJson.value("restTranslation", json::array()),
                                                Mathematics::Vector3(0.0f, 0.0f, 0.0f));
                pb.RestRotation = JsonToQuat(boneJson.value("restRotation", json::array()),
                                             Mathematics::Quaternion::Identity());
                pb.RestScale = JsonToVec3(boneJson.value("restScale", json::array()),
                                          Mathematics::Vector3(1.0f, 1.0f, 1.0f));
                pb.LocalForward = JsonToVec3(boneJson.value("localForward", json::array()),
                                             Mathematics::Vector3(0.0f, 0.0f, 1.0f));
                pb.LocalUp = JsonToVec3(boneJson.value("localUp", json::array()),
                                        Mathematics::Vector3(0.0f, 1.0f, 0.0f));
                m_Bones.push_back(pb);
            }
        }

        SetState(AssetState::Loaded);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("SkeletonProfile: failed to parse '{}': {}", GetPath().string(), e.what());
        SetState(AssetState::Failed);
        return false;
    }
}

std::string SkeletonProfile::SerializeJson() const
{
    json doc;
    doc["version"] = kSchemaVersion;
    doc["name"] = m_Name;
    doc["description"] = m_Description;

    json bones = json::array();
    for (const auto& pb : m_Bones)
    {
        json b;
        b["bone"] = HumanBoneToString(pb.Bone);
        b["parent"] = HumanBoneToString(pb.Parent);
        b["restTranslation"] = Vec3ToJson(pb.RestTranslation);
        b["restRotation"] = QuatToJson(pb.RestRotation);
        b["restScale"] = Vec3ToJson(pb.RestScale);
        b["localForward"] = Vec3ToJson(pb.LocalForward);
        b["localUp"] = Vec3ToJson(pb.LocalUp);
        bones.push_back(std::move(b));
    }
    doc["bones"] = std::move(bones);

    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
