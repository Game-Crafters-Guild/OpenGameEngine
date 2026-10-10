#include "Thumbnails/ThumbnailService.h"

#include "AssetCore/AssetTypes.h"
#include "Types/StringUtils.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetPathKey.h"
#include "Assets/AssetRegistry.h"
#include "Editor/Assets/ShaderGlslOpen.h"
#include "Thumbnails/AssetThumbnailHandler.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "Thumbnails/ShaderGraphThumbnailMaterial.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Thumbnails/TextureThumbnailHandler.h"

#include "Logger/Logger.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/UIManager.h"

#include <cctype>
#include <unordered_set>

namespace GameEngine
{

namespace
{
constexpr uint64_t kBrowserProvisionalResult = 0xd6e8feb86659fd93ull;

// Browser requests queued since the last frame drained per frame: most resolve
// a GUID and probe the PNG cache, so a screen of cached tiles appears in a frame
// or two instead of one tile per frame. The count bounds the main-thread cost,
// the stale-request cleanup and the async decode or import work that a stopped
// scroll can release at once.
constexpr size_t kMaxBrowserRequestsInspectedPerFrame = 32;

// Local helpers mirror SimpleThumbnailProvider's extension checks so that the
// fallback path in ThumbnailService behaves the same for image/model files
// that don't yet have a dedicated AssetThumbnailHandler.
static bool IsImageExtension(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    // .ktx/.ktx2 render through the UI background path, which re-decodes
    // block-compressed containers to RGBA8 (see UIManager's
    // CreateUIBackgroundTextureFromAsset).
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
           ext == ".tga" || ext == ".gif" || ext == ".webp" || ext == ".svg" ||
           ext == ".hdr" || ext == ".ktx" || ext == ".ktx2";
}

static bool IsModelExtension(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb" ||
           ext == ".dae" || ext == ".3ds";
}

static bool IsAudioExtension(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".aac";
}

static bool IsVideoExtension(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".mp4" || ext == ".mov" || ext == ".avi" || ext == ".mkv" || ext == ".m4v" ||
           ext == ".webm" || ext == ".wmv";
}

static bool IsLensFlareDefinitionExtension(const std::filesystem::path& p)
{
    const auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".lensflare";
}

static bool IsShaderGraphSurfaceExtension(const std::filesystem::path& p)
{
    const auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".glsl";
}

// True when lexically_normal() would return the path unchanged, for the forms
// it can alter: a dot component, a doubled separator, or a trailing separator.
// Conservative on purpose — it answers false whenever a component *might* be a
// dot component (a leading dot, or a dot after a separator, which also catches
// dotfiles), so the caller normalizes in every case where it would matter.
static bool IsPathNormalized(const std::filesystem::path& p)
{
    const auto& native = p.native();
    if (native.empty())
        return false;

    const auto isSeparator = [](std::filesystem::path::value_type c)
    { return c == '/' || c == '\\'; };

    if (native.front() == '.' || isSeparator(native.back()))
        return false;

    for (size_t i = 1; i < native.size(); ++i)
    {
        if (!isSeparator(native[i - 1]))
            continue;
        if (native[i] == '.' || isSeparator(native[i]))
            return false;
    }
    return true;
}

// Reference to assetPath when it is already normal, else to `storage` filled
// with the normalized copy. Entry points normalize once through this so every
// downstream consumer — result-cache key, handlers, their disk-cache names —
// keys on the same spelling of the path.
// Requests at or below this size are grid/list/asset-field TILES and may be
// served from the disk PNG caches; anything larger is a preview surface and
// keeps the live-render (or raw-source) path.
constexpr int kDiskCacheMaxTileRequestPx = 128;

static const std::filesystem::path& NormalizedOrSelf(const std::filesystem::path& assetPath,
                                                     std::filesystem::path& storage)
{
    if (IsPathNormalized(assetPath))
        return assetPath;
    storage = assetPath.lexically_normal();
    return storage;
}

// Simple, stateless fallback that approximates SimpleThumbnailProvider's
// behavior without maintaining a cache. This keeps the initial
// ThumbnailService integration non-disruptive for existing asset types.
static std::string ComputeFallbackThumbnail(const std::filesystem::path& assetPath,
                                            const std::filesystem::path& assetsRoot,
                                            int desiredSize)
{
    (void)desiredSize;
    const bool isImage = IsImageExtension(assetPath);
    const bool isModel = IsModelExtension(assetPath);
    const bool isAudio = IsAudioExtension(assetPath);
    const bool isVideo = IsVideoExtension(assetPath);

    std::string relStr;
    std::error_code ec;

    if (isImage && !assetsRoot.empty())
    {
        auto rel = std::filesystem::relative(assetPath, assetsRoot, ec);
        if (!ec && !rel.empty())
        {
            relStr = rel.generic_string();
        }
    }

    // Fallbacks for image assets when relative() fails or assetsRoot is
    // empty. This mirrors SimpleThumbnailProvider so that images continue
    // to show thumbnails even with slightly different path forms.
    if (isImage && relStr.empty())
    {
        std::error_code ecAbs;
        auto rootAbs = std::filesystem::absolute(assetsRoot, ecAbs);
        auto assetAbs = std::filesystem::absolute(assetPath, ecAbs);
        if (!ecAbs)
        {
            auto rootStr = rootAbs.generic_string();
            auto assetStr = assetAbs.generic_string();
            if (assetStr.rfind(rootStr, 0) == 0)
            {
                auto trimmed = assetStr.substr(rootStr.size());
                if (!trimmed.empty() && (trimmed[0] == '/' || trimmed[0] == '\\'))
                {
                    trimmed.erase(trimmed.begin());
                }
                relStr = trimmed;
            }
        }

        // As a last resort, if the asset path is already relative, use it
        // as-is. This still keeps the contract of returning a path relative
        // to the Assets/ root from the editor's point of view.
        if (relStr.empty() && assetPath.is_relative())
        {
            relStr = assetPath.generic_string();
        }

        if (relStr.empty())
        {
            Logger::Log::Warning(
                "ThumbnailService: failed to compute relative path for image '{}' (assetsRoot='{}')",
                assetPath.string(), assetsRoot.string());
        }
    }

    // Non-image assets: provide a sensible default for common model types so
    // that FBX/OBJ/GLTF/etc. fall back to the static model icon in
    // Assets/Icons/ModelThumbnail@64px.png when no dedicated handler is
    // registered.
    if (!isImage && isModel && relStr.empty())
    {
        relStr = "Icons/ModelThumbnail@64px.png";
    }

    // Audio assets: use music note icon.
    if (!isImage && isAudio && relStr.empty())
    {
        relStr = "Icons/music-note-icon@64px.png";
    }

    // Video assets: use video camera icon.
    if (!isImage && isVideo && relStr.empty())
    {
        relStr = "Icons/videocam.png";
    }

    return relStr;
}

} // namespace

ThumbnailService::ThumbnailService(AssetManager* assetManager)
    : m_AssetManager(assetManager)
{
}

void ThumbnailService::SetAssetsRoot(const std::filesystem::path& root)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        changed = (m_AssetsRoot != root);
        if (changed)
            m_ProjectGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_AssetsRoot = root;

        // Propagate to all registered handlers so they can return paths relative
        // to the same Assets/ root.
        for (auto& kv : m_Handlers)
        {
            if (kv.second)
            {
                kv.second->SetAssetsRoot(root);
            }
        }

        // When switching projects, the AssetManager is reinitialized and asset GUIDs
        // change. Every handler drops the state it keyed to the old project so we
        // can't show stale thumbnails; handlers that keep none ignore the call.
        if (changed)
        {
            m_LoggedPaths.clear();

            for (auto& kv : m_Handlers)
            {
                if (kv.second)
                {
                    kv.second->ResetForProjectSwitch();
                }
            }
        }
    }

    if (changed)
        ClearResultCache();
}

void ThumbnailService::SetCacheRoot(const std::filesystem::path& root)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        changed = (m_CacheRoot != root);
        m_CacheRoot = root;

        // Every handler gets the root; the ones that keep no disk cache ignore it
        // (AssetThumbnailHandler::SetCacheRoot defaults to a no-op).
        for (auto& kv : m_Handlers)
        {
            if (kv.second)
            {
                kv.second->SetCacheRoot(root);
            }
        }
    }

    if (changed)
        ClearResultCache();
}

ModelThumbnailHandler* ThumbnailService::FindModelHandler() const
{
    const auto it = m_Handlers.find(AssetType::Model);
    return it != m_Handlers.end() ? dynamic_cast<ModelThumbnailHandler*>(it->second.get()) : nullptr;
}

void ThumbnailService::SetFolderBakeListener(std::function<void(const FolderBakeReport&)> listener)
{
    if (ModelThumbnailHandler* models = FindModelHandler())
        models->SetFolderBakeListener(std::move(listener));
}

size_t ThumbnailService::GenerateFolderThumbnails(const std::filesystem::path& folder)
{
    if (!m_AssetManager || m_CacheRoot.empty() || folder.empty())
        return 0;
    ModelThumbnailHandler* modelHandler = FindModelHandler();
    if (!modelHandler)
        return 0;

    // Registry paths arrive folded, so "under the folder" is a key question.
    const std::string folderKey = AssetPathKey(folder);
    std::vector<ModelThumbnailHandler::BakeItem> items;
    std::vector<std::filesystem::path> missingFromDisk;
    for (const AssetIndexRecord& record : m_AssetManager->GetRegistry().GetAssetIndexSnapshot())
    {
        const bool isMaterial = record.Type == AssetType::Material;
        if ((record.Type != AssetType::Model && !isMaterial) || record.Guid.IsNull())
            continue;
        if (!IsUnderAssetDirKey(folderKey, AssetPathKey(record.Path)))
            continue;
        if (isMaterial && ModelThumbnailHandler::IsEphemeralPreviewMaterialPath(record.Path))
            continue;
        // The database can still list a file deleted outside the editor: it
        // has nothing to bake, and the report tells the user to refresh.
        std::error_code error;
        if (!std::filesystem::exists(record.Path, error))
        {
            missingFromDisk.push_back(record.Path);
            continue;
        }
        items.push_back({record.Guid, isMaterial});
    }
    const size_t queued = modelHandler->QueueBakes(items, missingFromDisk);
    Logger::Log::Info("Thumbnails: queued {} of {} assets under '{}' for baking; {} listed but not on disk",
                      queued, items.size(), folder.string(), missingFromDisk.size());
    return queued;
}

void ThumbnailService::RegisterHandler(AssetType type, std::unique_ptr<AssetThumbnailHandler> handler)
{
    if (!handler)
        return;

    std::lock_guard<std::mutex> lock(m_Mutex);

    // If the assets root is already known, propagate immediately so handlers
    // stay in sync with the service.
    if (!m_AssetsRoot.empty())
    {
        handler->SetAssetsRoot(m_AssetsRoot);
    }

    // Same for the cache root: if ThumbnailService already received SetCacheRoot
    // before this handler was registered, push it through now.
    if (!m_CacheRoot.empty())
    {
        handler->SetCacheRoot(m_CacheRoot);
    }

    m_Handlers[type] = std::move(handler);
    ClearResultCache();
}

std::string ThumbnailService::GetOrRequestInlineImage(const std::filesystem::path& image,
                                                      std::function<void(const std::string& path)> onReady)
{
    TextureThumbnailHandler* textures = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto it = m_Handlers.find(AssetType::Texture);
        if (it != m_Handlers.end())
            textures = dynamic_cast<TextureThumbnailHandler*>(it->second.get());
    }
    if (!textures)
    {
        if (onReady)
            onReady({});
        return {};
    }
    return textures->GetOrRequestInlineImage(image, std::move(onReady));
}

std::string ThumbnailService::GetOrRequestBrowser(const std::filesystem::path& assetPath,
                                                  int desiredSize, const std::string& consumer,
                                                  std::function<bool()> stillVisible,
                                                  std::function<void(const std::string&)> onReady)
{
    std::filesystem::path storage;
    const auto& path = NormalizedOrSelf(assetPath, storage);
    const auto generation = m_ProjectGeneration.load(std::memory_order_acquire);
    const auto key = MakeResultCacheKey(path, desiredSize, false, generation);
    std::string cached;
    if (TryGetCachedResult(key, cached))
    {
        // A cell rebound to a warm asset must not start its previous miss.
        if (auto pending = m_BrowserRequests.find(consumer); pending != m_BrowserRequests.end())
            pending->second.StillVisible = [] { return false; };
        return cached;
    }
    // Provisional material handles and image fallbacks must be rechecked after
    // settle (a disk thumbnail may have appeared), but their previously shown
    // images are useful immediately when scrolling back to the same asset.
    TryGetCachedResult(key ^ kBrowserProvisionalResult, cached);

    // No handler, filesystem query, decode, or service-lock wait in cell bind.
    // Rebinding a consumer replaces its callback instead of duplicating work.
    constexpr size_t kMaxPending = 512;
    if (!m_BrowserRequests.contains(consumer))
    {
        if (m_BrowserOrder.size() >= kMaxPending)
        {
            m_BrowserRequests.erase(m_BrowserOrder.front());
            m_BrowserOrder.pop_front();
        }
        m_BrowserOrder.push_back(consumer);
    }
    m_BrowserRequests.insert_or_assign(consumer, BrowserRequest{
        path, desiredSize, generation, std::move(stillVisible), std::move(onReady)});
    return cached;
}

void ThumbnailService::ProcessBrowserRequests()
{
    if (ModelThumbnailHandler::IsAssetBrowserScrolling())
        return;

    for (size_t inspected = 0;
         inspected < kMaxBrowserRequestsInspectedPerFrame && !m_BrowserOrder.empty(); ++inspected)
    {
        auto it = m_BrowserRequests.find(m_BrowserOrder.front());
        if (it == m_BrowserRequests.end())
        {
            m_BrowserOrder.pop_front();
            continue;
        }
        auto& request = it->second;
        if (request.Generation != m_ProjectGeneration.load(std::memory_order_acquire) ||
            (request.StillVisible && !request.StillVisible()))
        {
            m_BrowserRequests.erase(it);
            m_BrowserOrder.pop_front();
            continue;
        }
        std::unique_lock<std::mutex> lock(m_Mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        BrowserRequest next = std::move(request);
        m_BrowserRequests.erase(it);
        m_BrowserOrder.pop_front();
        // Some handlers return an immediate path without invoking onReady
        // (notably raw-image fallbacks). Browser callers already returned, so
        // deliver that path here. Serialize with an asynchronous completion so
        // a late fallback can never overwrite the finished thumbnail.
        struct Delivery
        {
            std::mutex Mutex;
            bool HandlerCompleted = false;
            std::function<void(const std::string&)> Callback;
        };
        auto delivery = std::make_shared<Delivery>();
        delivery->Callback = std::move(next.OnReady);
        auto completed = [delivery](const std::string& value)
        {
            std::lock_guard<std::mutex> guard(delivery->Mutex);
            delivery->HandlerCompleted = true;
            if (delivery->Callback)
                delivery->Callback(value);
        };
        bool cacheable = true;
        const auto result = GetOrRequestLocked(next.Path, next.Size, std::move(completed),
                                               false, next.Generation, &cacheable);
        lock.unlock();
        if (!result.empty())
        {
            std::lock_guard<std::mutex> guard(delivery->Mutex);
            if (!delivery->HandlerCompleted && delivery->Callback)
                delivery->Callback(result);
        }
        if (cacheable && !result.empty())
            StoreCachedResult(MakeResultCacheKey(next.Path, next.Size, false, next.Generation), result);
        else if (!result.empty())
            StoreCachedResult(MakeResultCacheKey(next.Path, next.Size, false, next.Generation) ^
                                  kBrowserProvisionalResult, result);
    }
}

std::string ThumbnailService::GetOrRequest(const std::filesystem::path& assetPath,
                                           int desiredSize,
                                           std::function<void(const std::string& relPath)> onReady,
                                           bool StaticModelListThumbnail)
{
    std::filesystem::path normalizedStorage;
    const std::filesystem::path& path = NormalizedOrSelf(assetPath, normalizedStorage);

    const uint64_t generation = m_ProjectGeneration.load(std::memory_order_acquire);
    const uint64_t cacheKey = MakeResultCacheKey(path, desiredSize, StaticModelListThumbnail, generation);
    std::string cached;
    if (TryGetCachedResult(cacheKey, cached))
    {
        if (onReady)
            onReady(cached);
        return cached;
    }

    bool resultCacheable = true;
    std::string result = GetOrRequestForGeneration(
        path, desiredSize, std::move(onReady), StaticModelListThumbnail, generation,
        &resultCacheable);
    if (resultCacheable && !result.empty() &&
        m_ProjectGeneration.load(std::memory_order_acquire) == generation)
        StoreCachedResult(cacheKey, result);
    return result;
}

std::string ThumbnailService::GetOrRequestForGeneration(const std::filesystem::path& assetPath,
                                                        int desiredSize,
                                                        std::function<void(const std::string& relPath)> onReady,
                                                        bool StaticModelListThumbnail,
                                                        uint64_t expectedGeneration,
                                                        bool* outResultCacheable)
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    return GetOrRequestLocked(assetPath, desiredSize, std::move(onReady), StaticModelListThumbnail,
                              expectedGeneration, outResultCacheable);
}

uint64_t ThumbnailService::MakeResultCacheKey(const std::filesystem::path& assetPath,
                                              int desiredSize,
                                              bool staticModelListThumbnail,
                                              uint64_t generation) const
{
    constexpr uint64_t kOffsetBasis = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;

    auto fold = [](uint64_t hash, uint64_t value)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8)) & 0xFFull;
            hash *= kPrime;
        }
        return hash;
    };

    // Hash the path's own code units rather than a generic_string() copy of
    // them. Separators are folded so a native backslash keys the same as the
    // forward slash the callers mix in — the equivalence the discarded
    // generic_string() used to provide.
    //
    // Dot components and doubled separators are the entry points' problem:
    // GetOrRequest and GetOrRequestBrowser normalize through
    // NormalizedOrSelf before this key is computed.
    uint64_t hash = kOffsetBasis;
    const std::filesystem::path& normalized = assetPath;
    for (const std::filesystem::path::value_type unit : normalized.native())
    {
        const auto code = static_cast<uint64_t>(
            static_cast<std::make_unsigned_t<std::filesystem::path::value_type>>(unit));
        hash = fold(hash, code == U'\\' ? static_cast<uint64_t>(U'/') : code);
    }

    // Length guards the concatenation ambiguity the old key's size prefix did.
    hash = fold(hash, normalized.native().size());
    hash = fold(hash, static_cast<uint64_t>(static_cast<uint32_t>(desiredSize)));
    hash = fold(hash, staticModelListThumbnail ? 1ull : 0ull);
    hash = fold(hash, generation);
    hash = fold(hash, ModelThumbnailHandler::GetPreviewIblEnabled() ? 1ull : 0ull);
    return hash;
}

bool ThumbnailService::TryGetCachedResult(uint64_t key, std::string& outResult) const
{
    std::unique_lock<std::mutex> lock(m_ResultCacheMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    const auto it = m_ResultCache.find(key);
    if (it == m_ResultCache.end())
        return false;
    outResult = it->second;
    return true;
}

void ThumbnailService::StoreCachedResult(uint64_t key, const std::string& result)
{
    if (result.empty())
        return;

    constexpr size_t kMaxCachedResults = 8192;
    std::lock_guard<std::mutex> lock(m_ResultCacheMutex);
    if (m_ResultCache.size() >= kMaxCachedResults && m_ResultCache.find(key) == m_ResultCache.end())
        m_ResultCache.erase(m_ResultCache.begin());
    m_ResultCache[key] = result;
}

void ThumbnailService::ClearResultCache()
{
    std::lock_guard<std::mutex> lock(m_ResultCacheMutex);
    m_ResultCache.clear();
}

std::string ThumbnailService::GetOrRequestLocked(const std::filesystem::path& assetPath,
                                                 int desiredSize,
                                                 std::function<void(const std::string& relPath)> onReady,
                                                 bool StaticModelListThumbnail,
                                                 uint64_t expectedGeneration,
                                                 bool* outResultCacheable)
{
    if (outResultCacheable)
        *outResultCacheable = true;

    if (!m_AssetManager)
    {
        if (onReady)
            onReady(std::string());
        return std::string();
    }

    if (expectedGeneration != m_ProjectGeneration.load(std::memory_order_acquire))
    {
        if (onReady)
            onReady(std::string());
        return std::string();
    }

    // Resolve asset type using the registry when possible; fall back to
    // extension-based detection otherwise so that new or unregistered files
    // still get a reasonable handler.
    AssetType assetType = AssetType::Unknown;

    AssetRegistry& registry = m_AssetManager->GetRegistry();
    AssetMetadata meta{};
    if (registry.TryGetAssetMetadata(assetPath, meta))
    {
        assetType = meta.Type;
    }
    // If the asset is registered but its type is unknown (e.g., type registry was not yet
    // populated when the DB/scan ran), fall back to extension-based detection so we can
    // still route to the best handler (model thumbnails, etc.).
    if (assetType == AssetType::Unknown)
    {
        const std::string ext = assetPath.has_extension() ? assetPath.extension().string() : std::string();
        if (!ext.empty())
        {
            assetType = GetAssetTypeFromExtension(ext.c_str());
        }
    }

    // If the registry reports a non-model type but the path clearly looks like a
    // model (e.g. .glb/.gltf/.fbx), prefer the model handler so all 3D assets
    // share the same runtime thumbnail path (and behaviors like rotation).
    if (assetType != AssetType::Model && IsModelExtension(assetPath))
    {
        assetType = AssetType::Model;
    }

    // Lens-flare tiles render through the same runtime thumbnail path as
    // Material View previews. This keeps the same behavior across asset grid,
    // list, and focused preview use-cases.
    if (IsLensFlareDefinitionExtension(assetPath))
    {
        auto modelIt = m_Handlers.find(AssetType::Model);
        if (modelIt != m_Handlers.end() && modelIt->second)
        {
            if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(modelIt->second.get()))
                return modelHandler->GetOrRequestLensFlare(
                    assetPath, desiredSize, std::move(onReady));
        }
    }

    // Shader-graph surface .glsl files (@sg-graph tag block): materialize to a derived
    // .thumb.material and render with the same orbit preview as .material assets.
    // The extension gate comes first: FileIsShaderGraphGlsl opens the file and reads
    // 8 KB, and every panel calls GetOrRequest per visible cell per rebuild. Graph
    // surfaces are .glsl by construction (GraphAsset::LoadFromFile gates the same way).
    if (m_AssetManager &&
        IsShaderGraphSurfaceExtension(assetPath) &&
        !ShaderGraphThumbnailMaterial::IsDerivedThumbnailAssetPath(assetPath) &&
        Editor::FileIsShaderGraphGlsl(assetPath))
    {
        const std::filesystem::path thumbMaterialPath =
            ShaderGraphThumbnailMaterial::EnsureMaterial(*m_AssetManager, assetPath);
        if (!thumbMaterialPath.empty())
        {
            auto it = m_Handlers.find(AssetType::Model);
            if (it != m_Handlers.end() && it->second)
            {
                if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
                {
                    return modelHandler->GetOrRequestMaterial(thumbMaterialPath, desiredSize,
                                                                std::move(onReady));
                }
            }
        }
    }

    // Routing diagnostic, once per path: panels call GetOrRequest per visible
    // cell on every rebuild, and the interactive editor initialises the logger
    // at Debug (Apps/Editor/Source/main.cpp), so a per-call line floods the log
    // at any level a developer would actually run.
    if (m_LoggedPaths.insert(assetPath.generic_string()).second)
    {
        Logger::Log::Debug("ThumbnailService::GetOrRequest: path='{}' assetType={}",
                           assetPath.string(), static_cast<int>(assetType));
    }

    // Material thumbnails are rendered via the model handler (shared ECS world,
    // orbit, double-buffering) on a configurable primitive mesh.
    if (assetType == AssetType::Material)
    {
        // Disk-cache fast path for TILE requests (<=128 px): the model handler
        // reads back a settled grid render into a PNG; subsequent navigations
        // bypass the live-render queue entirely. Larger requests (the Asset View preview) keep the
        // live path so a cache hit doesn't replace the orbiting preview with a
        // static image. The node-graph live preview material is excluded — it
        // changes on every edit and must always render live.
        if (!m_CacheRoot.empty() && !meta.Guid.IsNull() &&
            desiredSize > 0 && desiredSize <= kDiskCacheMaxTileRequestPx &&
            !ModelThumbnailHandler::IsEphemeralPreviewMaterialPath(assetPath))
        {
            // Recheck the material's freshness on each bind, like a model's.
            if (outResultCacheable)
                *outResultCacheable = false;
            const std::filesystem::path cachePath = m_CacheRoot /
                ModelThumbnailHandler::MakeMaterialCacheFileName(
                    meta.Guid, ModelThumbnailHandler::GetPreviewIblEnabled());
            // A PNG older than its material is about to be replaced.
            std::error_code ecSrc, ecCache;
            const auto srcTime = std::filesystem::last_write_time(assetPath, ecSrc);
            const auto cacheTime = std::filesystem::last_write_time(cachePath, ecCache);
            if (!ecSrc && !ecCache && cacheTime >= srcTime)
            {
                const std::string absStr = cachePath.generic_string();
                if (onReady)
                    onReady(absStr);
                return absStr;
            }
        }

        auto it = m_Handlers.find(AssetType::Model);
        if (it != m_Handlers.end() && it->second)
        {
            if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
            {
                // The live-render handle is provisional twice over: the disk
                // cache checked above is written by a separate producer, and
                // the call itself prewarms the material's shader once per
                // frame. Pinning it would make the PNG unreachable and retire
                // the prewarm after a single call.
                if (outResultCacheable)
                    *outResultCacheable = false;
                return modelHandler->GetOrRequestMaterial(assetPath, desiredSize, std::move(onReady));
            }
        }
        // No model handler registered; fall through to icon fallback.
    }

    // Scene assets: check the thumbnail cache for a saved scene screenshot.
    // Written by Editor::SceneThumbnailCapture after each scene save.
    if (assetType == AssetType::Scene && !m_CacheRoot.empty())
    {
        const std::size_t h = std::hash<std::string>{}(assetPath.string());
        const std::filesystem::path cachePath = m_CacheRoot /
            ("scenethumb_" + std::to_string(h) + ".png");
        std::error_code ec;
        if (std::filesystem::exists(cachePath, ec))
        {
            // Return the absolute path — the UI layer accepts both abs and
            // relative paths (VideoThumbnailHandler does the same).
            const std::string absStr = cachePath.generic_string();
            if (onReady)
                onReady(absStr);
            return absStr;
        }
        // No cached thumbnail yet — fall through to the static icon fallback.
        // SceneThumbnailCapture writes this file independently, so do not pin
        // the provisional fallback in the result cache before it appears.
        if (outResultCacheable)
            *outResultCacheable = false;
    }

    // Model assets, small (grid/list/asset-field) requests: serve the PNG the
    // model thumbnail readback wrote, bypassing slot acquisition, the
    // high-priority mesh import and the live-render queue entirely. The Asset
    // View preview asks >= 256 and keeps the live orbit path. Stale PNGs
    // (source re-exported) fall through to the live path; the readback sweep
    // re-bakes them.
    if (assetType == AssetType::Model && !m_CacheRoot.empty() && !meta.Guid.IsNull() &&
        desiredSize > 0 && desiredSize <= kDiskCacheMaxTileRequestPx)
    {
        // Recheck source freshness after settle on each bind, while the
        // browser's provisional cache still supplies immediate scroll images.
        if (outResultCacheable)
            *outResultCacheable = false;
        const std::filesystem::path cachePath =
            m_CacheRoot / ModelThumbnailHandler::MakeModelCacheFileName(meta.Guid);
        std::error_code ec;
        if (std::filesystem::exists(cachePath, ec))
        {
            std::error_code ecSrc, ecCache;
            const auto srcTime = std::filesystem::last_write_time(assetPath, ecSrc);
            const auto cacheTime = std::filesystem::last_write_time(cachePath, ecCache);
            if (!ecSrc && !ecCache && cacheTime >= srcTime)
            {
                if (ModelThumbnailHandler* models = FindModelHandler())
                    models->ReleaseGridSlotServedByPng(meta.Guid);
                const std::string absStr = cachePath.generic_string();
                if (onReady)
                    onReady(absStr);
                return absStr;
            }
        }
        // Not cached yet: keep live handles provisional so the next bind can
        // discover the PNG without callbacks from detached encoder threads.
    }

    // If we have a dedicated handler for this asset type, delegate to it.
    auto it = m_Handlers.find(assetType);
    if (it != m_Handlers.end() && it->second)
    {
        // Small texture-TILE requests must never fall back to the raw source:
        // that path decodes and GPU-uploads the FULL-resolution image for a
        // <=128 px cell and retains the texture. The handler knows when the
        // raw source is genuinely small (safe) versus pending/oversized/
        // undecodable — queried at fallback time in both the sync and async
        // arms, because the verdict changes when a decode finishes. Preview
        // requests (>128) keep the raw-source fallback: full resolution is
        // the point there.
        TextureThumbnailHandler* rawFallbackGate = nullptr;
        if (assetType == AssetType::Texture && desiredSize > 0 &&
            desiredSize <= kDiskCacheMaxTileRequestPx)
            rawFallbackGate = dynamic_cast<TextureThumbnailHandler*>(it->second.get());

        std::function<void(const std::string&)> wrappedCallback;
        if (onReady)
        {
            const std::filesystem::path assetsRoot = m_AssetsRoot;
            // Ensure that handlers which cannot currently produce a
            // thumbnail (and therefore report an empty path) still result
            // in the same behavior as the legacy SimpleThumbnailProvider
            // by falling back to ComputeFallbackThumbnail.
            wrappedCallback = [assetPath, desiredSize, onReady, assetsRoot,
                               rawFallbackGate](const std::string& relFromHandler)
            {
                if (!relFromHandler.empty())
                {
                    onReady(relFromHandler);
                }
                else if (rawFallbackGate && !rawFallbackGate->AllowsRawSourceFallback(assetPath))
                {
                    onReady(std::string());
                }
                else
                {
                    std::string fallback = ComputeFallbackThumbnail(assetPath, assetsRoot, desiredSize);
                    onReady(fallback);
                }
            };
        }

        std::string relFromHandler =
            it->second->GetOrRequest(assetPath, desiredSize, std::move(wrappedCallback), StaticModelListThumbnail);
        if (!relFromHandler.empty())
        {
            return relFromHandler;
        }

        // Handler did not provide an immediate result; preserve existing
        // behavior by falling back to the static/icon-based path. The handler
        // may still complete asynchronously (notably video extraction), so the
        // fallback must not prevent later calls from discovering its real result.
        if (outResultCacheable)
            *outResultCacheable = false;
        if (rawFallbackGate && !rawFallbackGate->AllowsRawSourceFallback(assetPath))
            return {};
        return ComputeFallbackThumbnail(assetPath, m_AssetsRoot, desiredSize);
    }

    // Fallback behavior: approximate SimpleThumbnailProvider so existing
    // image/model thumbnails keep working until specialized handlers are
    // implemented for each asset type.
    std::string rel = ComputeFallbackThumbnail(assetPath, m_AssetsRoot, desiredSize);

    if (onReady)
    {
        onReady(rel);
    }

    return rel;
}

bool ThumbnailService::IsEngineThumbnailReadyForWindow(uint64_t windowId,
                                                       const std::string& engineName) const
{
    auto it = m_Handlers.find(AssetType::Model);
    if (it == m_Handlers.end() || !it->second)
        return false;
    if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
        return modelHandler->IsEngineThumbnailReadyForWindow(windowId, engineName);
    return false;
}

void ThumbnailService::BeginFrame()
{
    ProcessBrowserRequests();
    std::unique_lock<std::mutex> lock(m_Mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    auto it = m_Handlers.find(AssetType::Model);
    if (it != m_Handlers.end() && it->second)
    {
        if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
        {
            modelHandler->BeginFrame();
        }
    }
}

void ThumbnailService::TickThumbnailsForWindowRG(uint64_t windowId, Rendering::RenderGraph::RGFrame& frame)
{
    std::unique_lock<std::mutex> lock(m_Mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    auto it = m_Handlers.find(AssetType::Model);
    if (it == m_Handlers.end() || !it->second)
    {
        return;
    }

    if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
    {
        modelHandler->TickRenderRG(windowId, frame);
    }
}

void ThumbnailService::EnsureEngineThumbnailRequested(uint64_t windowId,
                                                      const std::string& engineName)
{
    if (ModelThumbnailHandler::IsAssetBrowserScrolling())
    {
        m_ExternalTextureRetryWindows.insert(windowId);
        return;
    }
    std::unique_lock<std::mutex> lock(m_Mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        m_ExternalTextureRetryWindows.insert(windowId);
        return;
    }
    auto it = m_Handlers.find(AssetType::Model);
    if (it == m_Handlers.end() || !it->second)
        return;
    if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
        modelHandler->EnsureEngineThumbnailRequested(windowId, engineName);
}

std::string ThumbnailService::RequestLiveMaterialPreview(uint64_t windowId,
                                                         const GUID& materialGuid,
                                                         uint32_t widthPx,
                                                         uint32_t heightPx,
                                                         bool previewIblEnabled)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Handlers.find(AssetType::Model);
    if (it == m_Handlers.end() || !it->second)
        return {};
    if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
        return modelHandler->RequestLiveMaterialPreview(windowId, materialGuid, widthPx,
                                                        heightPx, previewIblEnabled);
    return {};
}

bool ThumbnailService::IsLiveMaterialPreviewReady(uint64_t windowId,
                                                  const std::string& engineName) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Handlers.find(AssetType::Model);
    if (it == m_Handlers.end() || !it->second)
        return false;
    if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
        return modelHandler->IsEngineThumbnailReadyForWindow(windowId, engineName);
    return false;
}

bool ThumbnailService::RegisterReadyThumbnails(uint64_t windowId, UIManager* ui)
{
    ResumeBrowserThumbnails(windowId, ui);
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Handlers.find(AssetType::Model);
    if (it == m_Handlers.end() || !it->second)
        return false;

    if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
    {
        return modelHandler->RegisterReadyThumbnailsRG(windowId, ui, nullptr);
    }
    return false;
}

bool ThumbnailService::RegisterReadyThumbnailsRG(uint64_t windowId, UIManager* ui,
                                                 Rendering::RenderGraph::RGFrame* pureFrame)
{
    ResumeBrowserThumbnails(windowId, ui);
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Handlers.find(AssetType::Model);
    if (it == m_Handlers.end() || !it->second)
        return false;

    if (auto* modelHandler = dynamic_cast<ModelThumbnailHandler*>(it->second.get()))
    {
        return modelHandler->RegisterReadyThumbnailsRG(windowId, ui, pureFrame);
    }
    return false;
}

void ThumbnailService::ResumeBrowserThumbnails(uint64_t windowId, UIManager* ui)
{
    if (!ui)
        return;
    if (ModelThumbnailHandler::IsAssetBrowserScrolling())
        m_ExternalTextureRetryWindows.insert(windowId);
    else if (m_ExternalTextureRetryWindows.erase(windowId) != 0)
        // Misses are recorded during primitive generation, not every frame.
        // Revisit the current visible tiles once after the hold, otherwise an
        // evicted engine thumbnail encountered mid-scroll could stay blank.
        ui->RequestExternalTextureRefresh();
}

} // namespace GameEngine
