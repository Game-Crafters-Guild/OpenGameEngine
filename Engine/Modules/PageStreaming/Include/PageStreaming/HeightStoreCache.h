#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetIOService;
}

namespace GameEngine::PageStreaming
{

struct HeightCookProgress;

/// The folder of a project's cooked height stores (<project>/.Cache/TerrainPages) and its one
/// owner: finds the store of a heightmap's current content and import grid, cooks it when missing,
/// and keeps the folder to one store per heightmap.
///
/// A store is named for its asset and its cook key (HeightStoreFile), so two terrains on one
/// heightmap with one grid share it, and an edit of the heightmap re-keys it. A cook first removes
/// the temporary stores of cooks that died (their writer is no longer running), then patches the
/// asset's previous store when the edit kept its grid and encoding (HeightPageCooker), then removes
/// the asset's stores under any other key. Blocking: call from a thread that is not a job-pool
/// worker. One cook per asset at a time.
class HeightStoreCache
{
public:
    explicit HeightStoreCache(std::filesystem::path directory);

    /// What Ensure found or made.
    struct Result
    {
        std::string Error;           ///< empty on success, else the reason worded as the fix
        std::filesystem::path Store; ///< the store of the heightmap's current content
        bool Cooked = false;         ///< false when the store was already there
    };

    /// The store of `asset`, whose heightmap file is `source` with the import grid `samplesX` x
    /// `samplesZ` (0 x 0 for a PNG, whose grid is its own). Cooks it when missing; `progress`, when
    /// set, follows the rows read (HeightCookProgress).
    Result Ensure(const GUID& asset, const std::filesystem::path& source, uint32 samplesX, uint32 samplesZ,
                  JobSystem::WorkStealingThreadPool* pool, AssetIOService* io,
                  const std::atomic<bool>* cancel = nullptr, HeightCookProgress* progress = nullptr) const;

private:
    std::filesystem::path m_Directory;
};

} // namespace GameEngine::PageStreaming
