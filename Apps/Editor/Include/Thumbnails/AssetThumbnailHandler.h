#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine {

// Base interface for per-asset-type thumbnail generators used by
// ThumbnailService. Each handler is responsible for a single asset
// type and may call into engine subsystems as needed.
class AssetThumbnailHandler {
public:
    virtual ~AssetThumbnailHandler() = default;

    // Thumbnail handlers need to know the Assets/ root so they can
    // return relative paths that the UI layer can use directly in
    // CSS-style background-image URLs.
    void SetAssetsRoot(const std::filesystem::path& root) { m_AssetsRoot = root; }

    // Persistent thumbnail cache root (<ProjectRoot>/.Editor/Thumbnails).
    // ThumbnailService pushes it to every handler on registration and again on
    // each project switch. Handlers that keep generated images on disk override
    // this; the default ignores it. An empty root means no project is open —
    // handlers must not write anywhere else in that case.
    virtual void SetCacheRoot(const std::filesystem::path& root) { (void)root; }

    // Drop transient state keyed to the project being closed. Asset GUIDs and
    // paths do not survive a project switch, so anything a handler remembers
    // about them — queued work, cached decisions, spawned preview state — has
    // to go before the next project's requests arrive. Handlers that hold
    // nothing across a switch ignore it.
    virtual void ResetForProjectSwitch() {}

    // Schedules thumbnail generation or retrieval for a specific asset.
    // Returns a relative path (from Assets/) if a thumbnail is already
    // available; otherwise may return empty and invoke onReady later.
    virtual std::string GetOrRequest(const std::filesystem::path& assetPath,
                                     int desiredSize,
                                     std::function<void(const std::string& relPath)> onReady,
                                     bool StaticModelListThumbnail = false) = 0;

protected:
    std::filesystem::path m_AssetsRoot;
};

} // namespace GameEngine

