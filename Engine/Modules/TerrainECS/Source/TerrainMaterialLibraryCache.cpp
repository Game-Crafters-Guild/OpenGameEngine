#include "TerrainECS/TerrainMaterialLibraryCache.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"

namespace GameEngine::TerrainECS
{
namespace
{
// Long enough that a library which simply has not finished loading costs one retry rather than
// one per frame, short enough that recovery reads as instant once the scan registers the file.
constexpr std::chrono::milliseconds kLibraryLoadRetryInterval{500};
} // namespace

void TerrainMaterialLibraryCache::EnsureSubscribed()
{
    if (m_Subscribed)
        return;

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    // ReloadedOnly is load-bearing: AssetReloaded is the only event the asset manager raises on
    // the main thread (AssetManager::Update -> CheckForReloads), which is what puts this erase
    // AHEAD of the same frame's extraction reads rather than beside them — extraction itself runs
    // on a JobSystem worker. See the threading note on the class. Dropping the parse is the whole
    // job: the GPU table is rebuilt from the per-frame ring, so nothing else needs invalidating.
    m_ReloadInvalidator = AssetReloadInvalidator(
        engine.GetAssetManager().GetEventDispatcher(), AssetType::TerrainMaterialLibrary,
        [this](const GUID& guid)
        {
            m_Libraries.erase(guid);
            m_Pending.erase(guid);
        },
        AssetReloadInvalidator::EventSet::ReloadedOnly);

    m_Subscribed = true;
}

const std::vector<TerrainMaterialEntry>* TerrainMaterialLibraryCache::Get(
    const GUID& guid, const TerrainTextureDeclarer& declare)
{
    if (guid.IsNull())
        return nullptr;

    EnsureSubscribed();

    if (const auto it = m_Libraries.find(guid); it != m_Libraries.end())
        return &it->second;

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return nullptr;
    auto& assetManager = engine.GetAssetManager();

    const auto asset = assetManager.GetAsset(guid);
    // dynamic_cast, not static_cast: GetType() is metadata the object carries, not proof of its
    // class, so a wrong-class object under this GUID would have had a materials vector read off
    // it. One cast per resolution, not per frame — a resolved library is served from above.
    const auto* library = dynamic_cast<const TerrainMaterialLibraryAsset*>(asset.get());

    // LOADED is the whole gate, and it separates "no library" from "a library whose payload is
    // not there yet". A registered-but-unpopulated object — one still under construction, or torn
    // down by the Unload half of Asset::Reload while extraction samples it from a worker — is
    // non-null and reports the right type while carrying no materials. Caching THAT empty list is
    // the trap: m_Libraries is consulted ahead of the asset manager on every later call, so the
    // terrain would never ask again, never warn, and shade every role from the built-in palette
    // for the rest of the session. A file that honestly holds no materials still reaches Loaded
    // and caches normally.
    if (!library || !library->IsLoaded())
    {
        // Nothing else in the engine pulls a terrain's library in, so this is the only thing that
        // will ever ask for it — which is exactly why one attempt is not enough.
        RequestLoadThrottled(assetManager, guid, /*registeredButUnusable*/ asset != nullptr);
        return nullptr;
    }

    m_Pending.erase(guid);
    const std::vector<TerrainMaterialEntry>& adopted =
        m_Libraries.emplace(guid, library->GetMaterials()).first->second;
    // Ahead of the return, because the caller resolves these very GUIDs to bindless indices the
    // moment it has the entries — and a texture uploaded before its kind is known is uploaded
    // wrong (a normal map as sRGB decodes to garbage vectors).
    DeclareTerrainLibraryTextures(adopted, declare);
    return &adopted;
}

void TerrainMaterialLibraryCache::RequestLoadThrottled(AssetManager& assetManager, const GUID& guid,
                                                       bool registeredButUnusable)
{
    const auto now = std::chrono::steady_clock::now();
    const auto [it, inserted] = m_Pending.try_emplace(guid);
    PendingLoad& pending = it->second;

    if (!inserted)
    {
        if (now < pending.NextAttempt)
            return;

        // Still unresolved a full interval after the first request. Say so once, and say WHICH
        // way it is unusable: a missing library and a registered-but-unpopulated one both fall
        // back to the built-in materials, which look like a plausible terrain rather than like a
        // failure, so nothing else about either is observable.
        if (!pending.Warned)
        {
            pending.Warned = true;
            Logger::Log::Warning(
                "Terrain materials: library {} is bound but not usable ({}) — the terrain is "
                "shading from the built-in materials meanwhile. Check the .terrainmatlib is "
                "inside the project's asset root and parses.",
                guid.ToString(),
                registeredButUnusable ? "registered but its payload is not loaded"
                                      : "not resident at all");
        }
    }

    pending.NextAttempt = now + kLibraryLoadRetryInterval;
    // A registered-but-unpopulated asset cannot be repaired by LoadAsset: its fast path finds the
    // object already in the loaded map and hands it back without loading anything. The async
    // reload pipeline is what rebuilds a payload (decode on a worker, adopt on the main thread);
    // its queue is mutex-guarded, so reaching it from the extraction thread is safe.
    if (registeredButUnusable)
        assetManager.RequestAsyncReload(guid);
    else
        assetManager.LoadAsset(guid, [](auto&&) {}, AssetLoadPriority::High);
}

} // namespace GameEngine::TerrainECS
