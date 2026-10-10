#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

struct AnimationLibraryEntry
{
    std::string Name;
    GUID AssetGuid;
    AssetType Type = AssetType::Animation;
    float32 DefaultBlendSeconds = 0.0f;
    bool Loop = true;
};

class AnimationLibrary : public ::GameEngine::Asset
{
  public:
    static constexpr int32 kSchemaVersion = 1;

    AnimationLibrary(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::AnimationLibrary, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;
    bool SaveToPath(const std::filesystem::path& path) const;

    const std::vector<AnimationLibraryEntry>& Entries() const { return m_Entries; }
    const AnimationLibraryEntry* Find(const std::string& name) const;

    void SetEntriesForTest(std::vector<AnimationLibraryEntry> entries) { m_Entries = std::move(entries); }

  private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    std::vector<AnimationLibraryEntry> m_Entries;
};

} // namespace Animation
} // namespace GameEngine
