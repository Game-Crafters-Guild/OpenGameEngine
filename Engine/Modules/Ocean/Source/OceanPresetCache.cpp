#include "Ocean/OceanPresetCache.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Ocean/OceanPresetAsset.h"

namespace GameEngine::Ocean
{
OceanPresetCache::OceanPresetCache() = default;
OceanPresetCache::~OceanPresetCache() = default;

std::shared_ptr<const OceanPresetAsset> OceanPresetCache::Find(const GUID &guid)
{
    if (guid.IsNull())
        return nullptr;
    Entry &entry = m_Entries[guid];
    const auto now = std::chrono::steady_clock::now();
    if (entry.Pending)
    {
        if (entry.Pending->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return entry.Preset;
        const SharedPtr<Asset> asset = entry.Pending->get();
        entry.Pending.reset();
        entry.CheckedAt = now;
        if (!asset || !asset->IsLoaded() ||
            (asset->GetType() != AssetType::OceanPreset && asset->GetType() != AssetType::Unknown))
        {
            if (!entry.Failed)
                Logger::Log::Warning("Ocean preset {} did not load", guid.ToString());
            entry.Failed = true;
            return entry.Preset;
        }
        entry.Path = asset->GetPath();
        Read(entry);
        return entry.Preset;
    }
    if (now - entry.CheckedAt < kRecheckInterval && (entry.Preset || entry.Failed))
        return entry.Preset;
    entry.CheckedAt = now;
    if (entry.Path.empty())
    {
        // Not requested yet, or its asset was not found: ask the asset manager.
        AssetManager &manager = EngineCore::GetInstance().GetAssetManager();
        entry.Pending = std::make_unique<AssetFuture>(manager.LoadAssetAsync(guid));
        return entry.Preset;
    }
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(entry.Path, error);
    if (!error && writeTime != entry.WriteTime)
        Read(entry);
    return entry.Preset;
}

void OceanPresetCache::Read(Entry &entry)
{
    std::error_code error;
    entry.WriteTime = std::filesystem::last_write_time(entry.Path, error);
    auto preset = std::make_shared<OceanPresetAsset>();
    std::string loadError;
    entry.Failed = !preset->LoadJson(entry.Path, &loadError);
    if (entry.Failed)
    {
        Logger::Log::Warning("Ocean preset {} could not be read: {}", entry.Path.string(), loadError);
        return;
    }
    entry.Preset = std::move(preset);
}
} // namespace GameEngine::Ocean
