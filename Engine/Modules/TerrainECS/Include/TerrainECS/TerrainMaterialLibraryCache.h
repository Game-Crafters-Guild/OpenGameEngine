#pragma once

#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
// TerrainMaterialEntry by value: the cached vector needs it complete. Engine's include
// directories are already a precondition of this module's public headers (IRenderFeature.h).
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "TerrainECS/TerrainMaterialAuthoring.h" // TerrainTextureDeclarer

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::TerrainECS
{

// Parsed terrain material libraries, keyed by library asset GUID.
//
// Extraction resolves every terrain's library once per frame, so the parse is cached here
// instead of being re-read from the asset manager each time. The cache holds only the AUTHORED
// entries: the GPU records extraction derives from them also depend on the render origin, so the
// material table is rebuilt from the per-frame ring regardless and a reload has nothing to
// invalidate beyond this parse.
//
// THREADING: unsynchronized, and safe because its two writers cannot overlap — NOT because they
// share a thread. Get() runs inside TerrainExtractionSystem::Update, which SystemManager dispatches
// to a JobSystem worker whenever its wave holds more than one enabled system (ECS/Systems.h,
// UpdateWaveBased). The reload callback runs on the main thread, inside AssetManager::Update. Both
// are driven from one main-thread EngineCore::Update, asset manager first and the ECS waves second,
// and the main thread blocks in jobSystem->Wait until the wave joins — so the erase and the reads
// are strictly ordered by the frame loop. Extraction also iterates terrains serially (.Each, not
// ParallelEach), so Get() is never concurrent with itself.
//
// Moving either writer out of that frame-loop ordering — a reload raised off AssetManager::Update,
// or a parallel terrain iteration — makes this map a data race and needs a lock.
//
// ReloadedOnly is what keeps the callback on the main thread at all: AssetReloaded is the one
// dispatcher event raised there, while Unloaded/Destroyed arrive on the file-watcher thread.
class TerrainMaterialLibraryCache
{
public:
    // The library's authored entries, or nullptr until the asset is resident AND LOADED. A miss
    // requests the repair and returns nullptr, so the caller shades from the built-in materials
    // for the frames before it arrives — and KEEPS ASKING, throttled, until it does.
    //
    // Only a loaded document is ever cached. An object that exists without its payload is a miss,
    // not an empty library: caching its emptiness would answer every later call from the cache
    // and strand the terrain on the built-in palette permanently.
    //
    // `declare` reports each entry's texture kinds (see TerrainTextureDeclarer) and runs exactly
    // where a parse is ADOPTED — both the earliest point those kinds are known and the last point
    // before the caller resolves the same GUIDs to bindless indices. A parameter rather than cache
    // state, so the one call that adopts a parse cannot forget it and a reload re-declares against
    // whatever the edited document now references.
    const std::vector<TerrainMaterialEntry>* Get(const GUID& guid,
                                                 const TerrainTextureDeclarer& declare);

private:
    // Subscribes on first use: the asset manager's event dispatcher only exists once the engine
    // is initialized, which a render feature's construction does not guarantee.
    void EnsureSubscribed();

    // Ask the asset manager for a library that is not usable yet, at most once per retry
    // interval. `registeredButUnusable` picks the repair: a missing asset needs a load, while an
    // asset registered without a payload needs a RELOAD — a load would find it in the loaded map
    // and hand the empty object straight back.
    void RequestLoadThrottled(AssetManager& assetManager, const GUID& guid,
                              bool registeredButUnusable);

    // A terrain binds its library the moment its scene text parses, which can be BEFORE the
    // project scan has registered the file. A request made in that window resolves to nothing,
    // so the request has to be repeatable: latching it permanently is what left a correctly
    // bound terrain shading from its legacy per-layer fields for the rest of the session.
    // Throttled rather than per-frame because the retry is only worth its cost at human
    // timescales, and AssetManager::LoadAsset already joins a genuinely in-flight request.
    struct PendingLoad
    {
        std::chrono::steady_clock::time_point NextAttempt{};
        bool Warned = false;
    };

    std::unordered_map<GUID, std::vector<TerrainMaterialEntry>> m_Libraries;
    std::unordered_map<GUID, PendingLoad> m_Pending;
    AssetReloadInvalidator m_ReloadInvalidator;
    bool m_Subscribed = false;
};

} // namespace GameEngine::TerrainECS
