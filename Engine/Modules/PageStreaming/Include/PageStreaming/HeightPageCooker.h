#pragma once

#include "PageStreaming/HeightStoreKey.h"

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

/// How far a cook has read its source, in source rows: each row is read twice, once to identify the
/// source (ScanHeightSource) and once to build the pyramid (CookHeightPageStore). Written by the
/// cook thread, read by any thread.
struct HeightCookProgress
{
    std::atomic<uint64> RowsDone{0};
    std::atomic<uint64> RowsTotal{0};
};

/// A heightmap file to cook, on its resolved grid.
struct HeightCookSource
{
    std::filesystem::path File;
    HeightSourceFormat Format = HeightSourceFormat::R32;
    uint32 SamplesX = 0; ///< samples per row (along X)
    uint32 SamplesZ = 0; ///< rows (along Z)
};

/// Resolves the heightmap at `file` for cooking: its format from the extension and its grid. A raw
/// file (.r16, .r32) records no grid, so `samplesX` and `samplesZ` are its import settings as
/// TerrainECS ResolveRawHeightmapLayout resolves them, and must account for every byte. A 16-bit
/// PNG's grid is its own and must fit the decoder (Terrain/Heightfield.h, ResolvePngHeightmapSize). GeoTIFF and anything
/// else is refused with the export that works. Returns an empty string and fills `out`, else the
/// reason worded as the fix.
std::string ResolveHeightCookSource(const std::filesystem::path& file, uint32 samplesX, uint32 samplesZ,
                                    HeightCookSource& out);

/// Reads the whole source once: the FNV-1a 64 of its bytes and, for an .r32 source, its range. An
/// .r32 sample that is not a finite number is refused (nodata must be filled before export). The
/// file is read through `io`'s reader threads a chunk ahead of the hashing, or on the calling
/// thread when `io` is null. Blocks: call from a thread that is not a job-pool worker. Returns an
/// empty string and fills `out`, else the reason worded as the fix.
std::string ScanHeightSource(const HeightCookSource& source, AssetIOService* io, HeightSourceIdentity& out,
                             const std::atomic<bool>* cancel = nullptr, HeightCookProgress* progress = nullptr);

/// One cook of a height store.
struct HeightCookRequest
{
    HeightCookSource Source;
    HeightSourceIdentity Identity; ///< ScanHeightSource's result for Source
    uint64 Key = 0;                ///< ComputeHeightStoreKey of the source and its settings
    std::filesystem::path Output;  ///< HeightStoreFile(directory, asset, Key)
    /// The asset's store under an earlier key, or empty. When its grid and encoding match, the cook
    /// patches it in place (moved to Output) and writes only the pages whose bytes changed;
    /// otherwise it is left as it is and a new store is written.
    std::filesystem::path Previous;
    /// Workers for the per-page and per-row work; null runs it on the calling thread.
    JobSystem::WorkStealingThreadPool* Pool = nullptr;
    /// Reader threads for the source's bands, read a band ahead of the pyramid; null reads each band
    /// on the calling thread.
    AssetIOService* Io = nullptr;
    const std::atomic<bool>* Cancel = nullptr;
    HeightCookProgress* Progress = nullptr; ///< RowsDone advanced past the identifying read as rows are built
};

/// What a cook did.
struct HeightCookResult
{
    std::string Error; ///< empty on success, else the reason worded as the fix
    uint64 PagesWritten = 0;
    uint64 PagesUnchanged = 0; ///< pages of a patched store whose bytes were already right
    uint64 StoreBytes = 0;
    bool Patched = false;
};

/// Cooks a height store: level 0 from the source a band of rows at a time, each coarser level
/// from the one below with the [1 2 1] / 4 tent as soon as its rows are complete, every page
/// encoded in the store-wide encoding (NormalizedHeightEncoding for 16-bit sources,
/// ChooseAbsoluteHeightEncoding of the source's range for .r32). Memory is bounded by a few page
/// rows per level, never by the source. The editor and the build pipeline cook; the Player never
/// does. Blocks the calling thread until the store is published or the cook fails: call it from a
/// thread that is not a job-pool worker.
HeightCookResult CookHeightPageStore(const HeightCookRequest& request);

} // namespace GameEngine::PageStreaming
