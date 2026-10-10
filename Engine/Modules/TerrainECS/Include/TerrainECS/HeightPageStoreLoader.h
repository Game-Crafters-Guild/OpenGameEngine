#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace GameEngine::PageStreaming
{
class PageStoreReader;
struct HeightCookProgress;
}

namespace GameEngine::TerrainECS
{

/// Where a process finds the cooked height stores of its heightmaps.
struct HeightPageStoreLocation
{
    /// The editor's cache (<project>/.Cache/TerrainPages): a missing store is cooked there, in the
    /// background. Empty when the process does not cook (the Player, or no project workspace).
    std::filesystem::path CacheDirectory;
    /// The packaged terrain containers (<content>/Cooked/Terrain/<guid>.geterrain) the Player
    /// reads. Empty in the editor.
    std::filesystem::path PackagedDirectory;
};

/// The open height store of each heightmap a paged terrain asks for. In the editor a missing store
/// is cooked on a background thread (logged at its start and its end with the size and the time);
/// until it is open, Store answers null and the terrain keeps its texture. In the Player the
/// packaged container is opened once; a heightmap without one keeps its texture for the session.
/// Main thread, except the cook threads, which hand their store over under a lock.
class HeightPageStoreLoader
{
public:
    HeightPageStoreLoader();
    ~HeightPageStoreLoader();

    HeightPageStoreLoader(const HeightPageStoreLoader&) = delete;
    HeightPageStoreLoader& operator=(const HeightPageStoreLoader&) = delete;

    /// Sets where stores are found or cooked. Cancels the cooks in progress and forgets every store.
    void SetLocation(HeightPageStoreLocation location);

    /// The open store of heightmap `asset` at content version `contentVersion`, or null while it
    /// cooks or when it has none. Starts the cook on the first ask in the editor, named for
    /// `terrain` (the asking terrain entity's name) in CookStatuses.
    std::shared_ptr<const PageStreaming::PageStoreReader> Store(const GUID& asset, uint64 contentVersion,
                                                                std::string_view terrain);

    /// A cook in progress: the terrain that asked for it and the share of its source rows read
    /// (0 to 1; each row is read twice, HeightCookProgress).
    struct CookStatus
    {
        std::string Terrain;
        float32 Fraction = 0.0f;
    };

    /// The cooks in progress into `out` (cleared first), by terrain name. Empty in the Player.
    void CookStatuses(std::vector<CookStatus>& out) const;

    /// A store a cook thread opened, handed to the main thread.
    struct Cooked
    {
        GUID Asset;
        uint64 ContentVersion = 0;
        std::shared_ptr<const PageStreaming::PageStoreReader> Reader;
    };

private:
    struct Entry
    {
        uint64 ContentVersion = 0;
        std::string Terrain; ///< the terrain that asked first, for CookStatuses
        // What the cook thread reports: its rows, and that it has ended (whatever the outcome).
        std::shared_ptr<PageStreaming::HeightCookProgress> Progress;
        std::shared_ptr<std::atomic<bool>> CookFinished;
        std::shared_ptr<const PageStreaming::PageStoreReader> Reader;
        std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);
        // A thread of its own, not a job: HeightStoreCache::Ensure blocks until the cook is done and
        // fans its work onto the engine's job pool, so on a pool worker it would wait on its own pool.
        std::thread Cook;
        bool Failed = false;
    };

    void StartCook(const GUID& asset, Entry& entry);
    void OpenPackaged(const GUID& asset, Entry& entry) const;
    void StopCooks();
    void AdoptCooked();

    HeightPageStoreLocation m_Location;
    std::unordered_map<GUID, Entry> m_Entries;

    // Handed over by the cook threads.
    std::shared_ptr<std::mutex> m_CookedMutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<Cooked>> m_Cooked = std::make_shared<std::vector<Cooked>>();
};

} // namespace GameEngine::TerrainECS
