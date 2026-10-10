#pragma once

#include "Thumbnails/IThumbnailProvider.h"

#include <unordered_map>
#include <mutex>


namespace GameEngine {

class SimpleThumbnailProvider : public IThumbnailProvider {
public:
    SimpleThumbnailProvider() = default;
    ~SimpleThumbnailProvider() override = default;

    void SetAssetsRoot(const std::filesystem::path& root) override { m_AssetsRoot = root; }

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool StaticModelListThumbnail = false) override;

private:
    static bool IsImageExtension(const std::filesystem::path& p);

    std::filesystem::path m_AssetsRoot;
    std::mutex m_Mutex;
    std::unordered_map<std::string, std::string> m_Cache; // key: absPath + "|" + size -> relPath
};

} // namespace GameEngine

