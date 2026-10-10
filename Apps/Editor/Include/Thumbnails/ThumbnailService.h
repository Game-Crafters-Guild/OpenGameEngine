#pragma once

// Future-facing ThumbnailService that implements IThumbnailProvider
// and dispatches to per-asset-type handlers. This is a high-level API
// sketch; implementation can be filled in incrementally.

#include "Thumbnails/IThumbnailProvider.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{

namespace Rendering
{
namespace RenderGraph
{
class RGFrame;
}
} // namespace Rendering

enum class AssetType; // defined in AssetCore/AssetTypes.h
class AssetManager;
class AssetThumbnailHandler;
class ModelThumbnailHandler;
class UIManager;

class ThumbnailService : public IThumbnailProvider
{
  public:
    explicit ThumbnailService(AssetManager* assetManager);
    ~ThumbnailService() override = default;

    void SetAssetsRoot(const std::filesystem::path& root) override;

    // Optional project-local cache root for persistent thumbnail images.
    // Design default is <ProjectRoot>/.Editor/Thumbnails, but callers are
    // free to override when embedding the editor.
    void SetCacheRoot(const std::filesystem::path& root);

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool StaticModelListThumbnail = false) override;

    std::string GetOrRequestBrowser(const std::filesystem::path& assetPath,
                                    int desiredSize, const std::string& consumer,
                                    std::function<bool()> stillVisible,
                                    std::function<void(const std::string&)> onReady) override;

    std::string GetOrRequestInlineImage(const std::filesystem::path& image,
                                        std::function<void(const std::string& path)> onReady) override;

    // --- Editor integration helpers (not part of IThumbnailProvider) ---
    //
    // True when `engineName` resolves to a ready, UI-bound texture in this
    // window. Callers use this to defer a focused-preview swap until the
    // new render is ready, so the previous content stays visible during
    // the transition instead of going blank.
    bool IsEngineThumbnailReadyForWindow(uint64_t windowId, const std::string& engineName) const override;
    /// Call once per application frame before any TickThumbnailsForWindowRG so model orbit state advances at most once per frame.
    void BeginFrame();

    // Called once per frame per window (before UI) to render any pending
    // thumbnails into their resolved textures.
    // RenderGraph arm (slice 8c-2b): declarations into the window's frame.
    void TickThumbnailsForWindowRG(uint64_t windowId, Rendering::RenderGraph::RGFrame& frame);

    // Drain a window UIManager's unresolved engine-thumbnail requests: acquire +
    // enqueue the matching (list-aware) slots so they render. Call before
    // TickThumbnailsForWindowRG so the renders are declared into the same frame.
    void EnsureEngineThumbnailRequested(uint64_t windowId, const std::string& engineName) override;

    // Models and materials have persistent thumbnails; the model handler bakes both.
    size_t GenerateFolderThumbnails(const std::filesystem::path& folder) override;
    void SetFolderBakeListener(std::function<void(const FolderBakeReport&)> listener) override;

    std::string RequestLiveMaterialPreview(uint64_t windowId,
                                           const GUID& materialGuid,
                                           uint32_t widthPx,
                                           uint32_t heightPx,
                                           bool previewIblEnabled = true) override;

    bool IsLiveMaterialPreviewReady(uint64_t windowId,
                                    const std::string& engineName) const override;

    // Register ready (non-in-flight) thumbnails with UIManager so the UI pass
    // declares proper read dependencies (barrier generation).
    // Returns true when external texture bindings changed.
    bool RegisterReadyThumbnails(uint64_t windowId, class UIManager* ui);
    // RenderGraph-mode binding: per-frame publish on pure frames (pureFrame != null),
    // stable device-handle binds on hybrid frames (pureFrame == null).
    bool RegisterReadyThumbnailsRG(uint64_t windowId, class UIManager* ui,
                                   Rendering::RenderGraph::RGFrame* pureFrame);

    // Registration API for asset-type handlers (image, model, material, etc.).
    void RegisterHandler(AssetType type, std::unique_ptr<AssetThumbnailHandler> handler);

  private:
    struct BrowserRequest
    {
        std::filesystem::path Path;
        int Size;
        uint64_t Generation;
        std::function<bool()> StillVisible;
        std::function<void(const std::string&)> OnReady;
    };
    // UI-thread owned; bounded even when scrubbing thousands of assets.
    std::deque<std::string> m_BrowserOrder;
    std::unordered_map<std::string, BrowserRequest> m_BrowserRequests;
    void ProcessBrowserRequests();
    // The handler registered for models, or null.
    ModelThumbnailHandler* FindModelHandler() const;
    void ResumeBrowserThumbnails(uint64_t windowId, UIManager* ui);
    std::unordered_set<uint64_t> m_ExternalTextureRetryWindows;

    using HandlerMap = std::unordered_map<AssetType, std::unique_ptr<AssetThumbnailHandler>>;

    std::string GetOrRequestForGeneration(const std::filesystem::path& assetPath,
                                          int desiredSize,
                                          std::function<void(const std::string& relPath)> onReady,
                                          bool StaticModelListThumbnail,
                                          uint64_t expectedGeneration,
                                          bool* outResultCacheable = nullptr);
    std::string GetOrRequestLocked(const std::filesystem::path& assetPath,
                                   int desiredSize,
                                   std::function<void(const std::string& relPath)> onReady,
                                   bool StaticModelListThumbnail,
                                   uint64_t expectedGeneration,
                                   bool* outResultCacheable = nullptr);

    // 64-bit digest of everything that decides the result, so the lookup on the
    // per-cell path costs no allocation. A collision would serve one asset's
    // thumbnail for another; at the 8192-entry bound the odds of any collision
    // are on the order of 1e-12, which is well under the rate at which the
    // sources themselves change underneath the cache.
    uint64_t MakeResultCacheKey(const std::filesystem::path& assetPath,
                                int desiredSize,
                                bool staticModelListThumbnail,
                                uint64_t generation) const;
    bool TryGetCachedResult(uint64_t key, std::string& outResult) const;
    void StoreCachedResult(uint64_t key, const std::string& result);
    void ClearResultCache();

    mutable std::mutex m_Mutex;
    mutable std::mutex m_ResultCacheMutex;
    std::atomic<uint64_t> m_ProjectGeneration{0};
    AssetManager* m_AssetManager = nullptr; // not owned
    std::filesystem::path m_AssetsRoot;
    std::filesystem::path m_CacheRoot; // e.g. <ProjectRoot>/.Editor/Thumbnails
    HandlerMap m_Handlers;
    std::unordered_map<uint64_t, std::string> m_ResultCache;
    // Paths whose routing decision has already been logged, so the per-cell
    // request path logs once instead of once per rebuild. Guarded by m_Mutex.
    std::unordered_set<std::string> m_LoggedPaths;
};

} // namespace GameEngine
