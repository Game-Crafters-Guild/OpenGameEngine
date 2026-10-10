#pragma once

#include "AssetCore/GUID.h"
#include "Thumbnails/FolderBakeReport.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <string>
#include <utility>

namespace GameEngine {

class IThumbnailProvider {
public:
    virtual ~IThumbnailProvider() = default;

    // Set the runtime Assets/ root so returned paths can be relative to it.
    virtual void SetAssetsRoot(const std::filesystem::path& root) = 0;

    // Returns a relative path (from Assets/) to a thumbnail or representative image if already cached.
    // If not cached, starts an async request and invokes onReady with the relative path when ready.
    // Implementations should call onReady on a background thread; the UI side should marshal to UI thread.
    // When \p StaticModelListThumbnail is true (models only), use a separate cached
    // engine texture that does not share the rotating Asset View preview.
    virtual std::string GetOrRequest(const std::filesystem::path& assetPath,
                                     int desiredSize,
                                     std::function<void(const std::string& relPath)> onReady,
                                     bool StaticModelListThumbnail = false) = 0;

    // Browser cells call this on the UI thread. Implementations can defer a
    // miss; stillVisible is evaluated on the UI thread before starting work.
    virtual std::string GetOrRequestBrowser(const std::filesystem::path& assetPath,
                                            int desiredSize, const std::string& /*consumer*/,
                                            std::function<bool()> /*stillVisible*/,
                                            std::function<void(const std::string&)> onReady)
    {
        return GetOrRequest(assetPath, desiredSize, std::move(onReady));
    }

    // An image file shown inline at about its display size (the AI Assistant's
    // captures): a cached copy at most 1024 px on its long edge, apart from the
    // tile images GetOrRequest serves. Returns the copy's path when it is
    // current (and calls onReady with it); otherwise answers once through
    // onReady, possibly on a background thread: the copy, the file itself when
    // it is no larger, or empty when there is none.
    virtual std::string GetOrRequestInlineImage(const std::filesystem::path& /*image*/,
                                                std::function<void(const std::string& path)> onReady)
    {
        if (onReady)
            onReady({});
        return {};
    }
    // Returns true once the live-rendered thumbnail backing `engineName`
    // (e.g. `engine:editor_material_thumb_<guid>`) has produced a frame and
    // is currently bound for UI sampling in the given window. Default
    // implementation returns false — callers can use this to defer
    // swapping a focused preview's bound resource until the new render is
    // ready, avoiding a one-frame blank gap during transitions.
    virtual bool IsEngineThumbnailReadyForWindow(uint64_t /*windowId*/,
                                                 const std::string& /*engineName*/) const
    {
        return false;
    }

    // Ask the provider to produce a live engine thumbnail for `engineName`
    // in `windowId` even before the UI resolver observes the missing texture.
    virtual void EnsureEngineThumbnailRequested(uint64_t /*windowId*/,
                                                const std::string& /*engineName*/)
    {
    }

    // Bakes the persistent thumbnail of every asset under `folder` and its
    // subfolders in the background, after the visible tiles, so later visits
    // show them at once. Returns how many assets were queued; a provider
    // without a persistent cache queues none.
    virtual size_t GenerateFolderThumbnails(const std::filesystem::path& /*folder*/) { return 0; }

    // Called on the main thread whenever the current folder bake's report
    // changes, and once more when it finishes. One listener; an empty function
    // removes it.
    virtual void SetFolderBakeListener(std::function<void(const FolderBakeReport&)> /*listener*/) {}

    // Dedicated live material preview surface. Unlike GetOrRequest(), this is
    // not an asset-browser thumbnail lookup: callers already have a runtime
    // material GUID and want a UI-sampleable engine resource of the requested
    // size for a focused preview surface.
    virtual std::string RequestLiveMaterialPreview(uint64_t /*windowId*/,
                                                   const GUID& /*materialGuid*/,
                                                   uint32_t /*widthPx*/,
                                                   uint32_t /*heightPx*/,
                                                   bool /*previewIblEnabled*/ = true)
    {
        return {};
    }

    virtual bool IsLiveMaterialPreviewReady(uint64_t windowId,
                                            const std::string& engineName) const
    {
        return IsEngineThumbnailReadyForWindow(windowId, engineName);
    }
};

} // namespace GameEngine
