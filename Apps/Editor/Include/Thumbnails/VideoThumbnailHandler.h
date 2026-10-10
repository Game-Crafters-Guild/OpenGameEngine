#pragma once

#include "Thumbnails/AssetThumbnailHandler.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace GameEngine
{

// Generates thumbnails for video files by extracting a representative frame.
// Seeks past black/dark intro frames to find a visually meaningful thumbnail.
// Results are cached as PNGs in the thumbnail cache directory.
class VideoThumbnailHandler : public AssetThumbnailHandler
{
public:
    VideoThumbnailHandler();
    ~VideoThumbnailHandler() override = default;

    void SetCacheRoot(const std::filesystem::path& root) override { m_CacheRoot = root; }

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool StaticModelListThumbnail = false) override;

private:
    std::filesystem::path GetCachePath(const std::filesystem::path& assetPath) const;

    std::filesystem::path m_CacheRoot;

    // Track in-flight generation to avoid duplicate work. Held on the heap behind
    // a shared_ptr so the background job captures a copy and clears its entry
    // through it without dereferencing `this` — the handler can be destroyed
    // while a generation is still running.
    struct InFlightState
    {
        std::mutex Mutex;
        std::unordered_set<std::string> InFlight;
    };

    std::shared_ptr<InFlightState> m_State = std::make_shared<InFlightState>();
};

} // namespace GameEngine
