#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace GameEngine
{
class AssetFuture;
}

namespace GameEngine::Ocean
{
class OceanPresetAsset;

// The .oceanpreset files the ocean extraction applies, loaded without blocking
// it: the first request for a preset starts an asynchronous asset load and
// later frames pick up the result. A failed load is retried, and a loaded file
// checked for edits (and re-read when edited), at most once per kRecheckInterval.
class OceanPresetCache
{
  public:
    OceanPresetCache();
    ~OceanPresetCache();
    OceanPresetCache(const OceanPresetCache &) = delete;
    OceanPresetCache &operator=(const OceanPresetCache &) = delete;

    // The preset for `guid`, or null while it loads or when it failed to load.
    std::shared_ptr<const OceanPresetAsset> Find(const GUID &guid);

  private:
    static constexpr std::chrono::seconds kRecheckInterval{1};
    struct Entry
    {
        std::unique_ptr<AssetFuture> Pending;
        std::shared_ptr<const OceanPresetAsset> Preset;
        std::filesystem::path Path;
        std::filesystem::file_time_type WriteTime{};
        std::chrono::steady_clock::time_point CheckedAt{};
        bool Failed = false;
    };
    void Read(Entry &entry);
    std::unordered_map<GUID, Entry> m_Entries;
};
} // namespace GameEngine::Ocean
