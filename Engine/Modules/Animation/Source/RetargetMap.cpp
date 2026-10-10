#include "Animation/RetargetMap.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <fstream>
#include <iterator>
#include <string>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

constexpr const char* kFKRotationModeNames[] = {
    "OneToOne",
    "Transport",
    "SlerpAlongArc"
};

constexpr const char* kFKTranslationModeNames[] = {
    "None",
    "PerBoneScale",
    "UniformChainScale"
};

} // namespace

const char* FKRotationModeToString(FKRotationMode mode)
{
    const auto idx = static_cast<size_t>(mode);
    if (idx >= sizeof(kFKRotationModeNames) / sizeof(kFKRotationModeNames[0]))
        return "OneToOne";
    return kFKRotationModeNames[idx];
}

FKRotationMode FKRotationModeFromString(std::string_view name)
{
    for (size_t i = 0; i < sizeof(kFKRotationModeNames) / sizeof(kFKRotationModeNames[0]); ++i)
    {
        if (name == kFKRotationModeNames[i])
            return static_cast<FKRotationMode>(i);
    }
    return FKRotationMode::OneToOne;
}

const char* FKTranslationModeToString(FKTranslationMode mode)
{
    const auto idx = static_cast<size_t>(mode);
    if (idx >= sizeof(kFKTranslationModeNames) / sizeof(kFKTranslationModeNames[0]))
        return "None";
    return kFKTranslationModeNames[idx];
}

FKTranslationMode FKTranslationModeFromString(std::string_view name)
{
    for (size_t i = 0; i < sizeof(kFKTranslationModeNames) / sizeof(kFKTranslationModeNames[0]); ++i)
    {
        if (name == kFKTranslationModeNames[i])
            return static_cast<FKTranslationMode>(i);
    }
    return FKTranslationMode::None;
}

bool RetargetMap::Load()
{
    SetState(AssetState::Loading);

    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("RetargetMap: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool RetargetMap::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void RetargetMap::Unload()
{
    m_Version = 0;
    m_SourceRigRef = ::GameEngine::GUID();
    m_TargetRigRef = ::GameEngine::GUID();
    m_ChainMap.clear();
    m_OpStack.clear();
    SetState(AssetState::Unloaded);
}

bool RetargetMap::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool RetargetMap::SaveToPath(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Log::Error("RetargetMap: cannot open '{}' for write", path.string());
        return false;
    }
    const std::string serialized = SerializeJson();
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return file.good();
}

bool RetargetMap::ParseJson(const std::string& text)
{
    try
    {
        const auto doc = json::parse(text);

        m_Version = doc.value("version", 0);
        if (m_Version > kSchemaVersion)
        {
            Logger::Log::Warning(
                "RetargetMap '{}' has version {} but engine supports up to {}; reading anyway",
                GetPath().string(), m_Version, kSchemaVersion);
        }

        if (doc.contains("sourceRigRef") && doc["sourceRigRef"].is_string())
            m_SourceRigRef = ::GameEngine::GUID(doc["sourceRigRef"].get<std::string>());
        else
            m_SourceRigRef = ::GameEngine::GUID();

        if (doc.contains("targetRigRef") && doc["targetRigRef"].is_string())
            m_TargetRigRef = ::GameEngine::GUID(doc["targetRigRef"].get<std::string>());
        else
            m_TargetRigRef = ::GameEngine::GUID();

        m_ChainMap.clear();
        if (doc.contains("chainMap") && doc["chainMap"].is_array())
        {
            m_ChainMap.reserve(doc["chainMap"].size());
            for (const auto& c : doc["chainMap"])
            {
                ChainPairing cp;
                cp.Kind = ChainKindFromString(c.value("kind", std::string("Other")));

                if (c.contains("fk") && c["fk"].is_object())
                {
                    const auto& fk = c["fk"];
                    cp.FK.RotationMode = FKRotationModeFromString(
                        fk.value("rotationMode", std::string("OneToOne")));
                    cp.FK.RotationAlpha = fk.value("rotationAlpha", 1.0f);
                    cp.FK.TranslationMode = FKTranslationModeFromString(
                        fk.value("translationMode", std::string("None")));
                }

                if (c.contains("ik") && c["ik"].is_object())
                {
                    const auto& ik = c["ik"];
                    cp.IK.Enabled = ik.value("enabled", false);
                    cp.IK.BlendToSource = ik.value("blendToSource", 1.0f);
                    cp.IK.Extension = ik.value("extension", 0.0f);
                }

                if (c.contains("footLock") && c["footLock"].is_object())
                {
                    const auto& fl = c["footLock"];
                    cp.FootLock.Enabled = fl.value("enabled", false);
                    cp.FootLock.SpeedThreshold = fl.value("speedThreshold", 0.05f);
                    cp.FootLock.LockBlend = fl.value("lockBlend", 1.0f);
                }

                m_ChainMap.push_back(std::move(cp));
            }
        }

        m_OpStack.clear();
        if (doc.contains("opStack") && doc["opStack"].is_array())
        {
            m_OpStack.reserve(doc["opStack"].size());
            for (const auto& op : doc["opStack"])
            {
                OpStackEntry ose;
                ose.OpName = op.value("opName", std::string());
                if (op.contains("params"))
                    ose.Params = op["params"];
                m_OpStack.push_back(std::move(ose));
            }
        }

        SetState(AssetState::Loaded);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("RetargetMap: failed to parse '{}': {}", GetPath().string(), e.what());
        SetState(AssetState::Failed);
        return false;
    }
}

std::string RetargetMap::SerializeJson() const
{
    json doc;
    doc["version"] = kSchemaVersion;
    doc["sourceRigRef"] = m_SourceRigRef.ToString();
    doc["targetRigRef"] = m_TargetRigRef.ToString();

    json cm = json::array();
    for (const auto& cp : m_ChainMap)
    {
        json e;
        e["kind"] = ChainKindToString(cp.Kind);

        json fk;
        fk["rotationMode"] = FKRotationModeToString(cp.FK.RotationMode);
        fk["rotationAlpha"] = cp.FK.RotationAlpha;
        fk["translationMode"] = FKTranslationModeToString(cp.FK.TranslationMode);
        e["fk"] = std::move(fk);

        json ik;
        ik["enabled"] = cp.IK.Enabled;
        ik["blendToSource"] = cp.IK.BlendToSource;
        ik["extension"] = cp.IK.Extension;
        e["ik"] = std::move(ik);

        json fl;
        fl["enabled"] = cp.FootLock.Enabled;
        fl["speedThreshold"] = cp.FootLock.SpeedThreshold;
        fl["lockBlend"] = cp.FootLock.LockBlend;
        e["footLock"] = std::move(fl);

        cm.push_back(std::move(e));
    }
    doc["chainMap"] = std::move(cm);

    json os = json::array();
    for (const auto& op : m_OpStack)
    {
        json e;
        e["opName"] = op.OpName;
        e["params"] = op.Params;
        os.push_back(std::move(e));
    }
    doc["opStack"] = std::move(os);

    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
