#include "Animation/AnimationLibrary.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

AssetType AnimationAssetTypeFromString(const std::string& value)
{
    if (value == "Timeline") return AssetType::Timeline;
    if (value == "ClipSet") return AssetType::ClipSet;
    if (value == "AnimationLibrary") return AssetType::AnimationLibrary;
    if (value == "AnimationController") return AssetType::AnimationController;
    if (value == "SpriteFrames") return AssetType::SpriteFrames;
    return AssetType::Animation;
}

} // namespace

bool AnimationLibrary::Load()
{
    SetState(AssetState::Loading);

    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("AnimationLibrary: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool AnimationLibrary::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void AnimationLibrary::Unload()
{
    m_Entries.clear();
    SetState(AssetState::Unloaded);
}

bool AnimationLibrary::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool AnimationLibrary::SaveToPath(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Log::Error("AnimationLibrary: cannot open '{}' for write", path.string());
        return false;
    }
    const std::string serialized = SerializeJson();
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return file.good();
}

const AnimationLibraryEntry* AnimationLibrary::Find(const std::string& name) const
{
    for (const auto& entry : m_Entries)
    {
        if (entry.Name == name)
            return &entry;
    }
    return nullptr;
}

bool AnimationLibrary::ParseJson(const std::string& text)
{
    try
    {
        const auto doc = json::parse(text);
        if (!doc.is_object())
        {
            SetState(AssetState::Failed);
            return false;
        }

        m_Entries.clear();
        if (doc.contains("animations") && doc["animations"].is_array())
        {
            for (const auto& entryJson : doc["animations"])
            {
                AnimationLibraryEntry entry;
                entry.Name = entryJson.value("name", std::string());
                if (entry.Name.empty())
                    continue;

                if (entryJson.contains("assetGuid") && entryJson["assetGuid"].is_string())
                    entry.AssetGuid = GUID(entryJson["assetGuid"].get<std::string>());

                entry.Type = AnimationAssetTypeFromString(entryJson.value("type", std::string("Animation")));
                entry.DefaultBlendSeconds = std::max(0.0f, entryJson.value("defaultBlendSeconds", 0.0f));
                entry.Loop = entryJson.value("loop", true);
                m_Entries.push_back(std::move(entry));
            }
        }

        SetState(AssetState::Loaded);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AnimationLibrary: failed to parse '{}': {}", GetPath().string(), e.what());
        m_Entries.clear();
        SetState(AssetState::Failed);
        return false;
    }
}

std::string AnimationLibrary::SerializeJson() const
{
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "AnimationLibrary";

    json animations = json::array();
    for (const auto& entry : m_Entries)
    {
        json item;
        item["name"] = entry.Name;
        item["assetGuid"] = entry.AssetGuid.ToString();
        item["type"] = AssetTypeToString(entry.Type);
        item["defaultBlendSeconds"] = entry.DefaultBlendSeconds;
        item["loop"] = entry.Loop;
        animations.push_back(std::move(item));
    }
    doc["animations"] = std::move(animations);
    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
