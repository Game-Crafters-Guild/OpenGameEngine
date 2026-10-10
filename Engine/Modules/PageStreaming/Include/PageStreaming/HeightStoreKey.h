#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <filesystem>
#include <optional>

namespace GameEngine::PageStreaming
{

/// The file a height store is cooked from.
enum class HeightSourceFormat : uint8
{
    R16 = 0,   ///< raw 16-bit unsigned samples, row-major
    R32 = 1,   ///< raw 32-bit float samples, row-major
    Png16 = 2, ///< a 16-bit grayscale PNG
};

/// The source file's content, as one full read of it gives: the identity of a store's source.
/// The asset registry's own fingerprint is not an identity for a heightmap: past 256 KB it hashes
/// a sparse sample of the file, so an edit in the middle of a DEM keeps it.
struct HeightSourceIdentity
{
    uint64 ContentHash = 0; ///< FNV-1a 64 of every byte of the file
    float32 MinHeight = 0.0f; ///< lowest sample, in the source's units (R32 sources)
    float32 MaxHeight = 0.0f;
};

/// Everything that decides a height store's bytes. Deliberately nothing else: no entity, scene,
/// runtime handle, streaming budget, material or setting that leaves the bytes unchanged, so two
/// terrains in two scenes on the same heightmap with the same import settings share one store.
struct HeightStoreCookInputs
{
    uint64 SourceContentHash = 0;
    HeightSourceFormat Format = HeightSourceFormat::R32;
    uint32 SamplesX = 0; ///< the import grid
    uint32 SamplesZ = 0;
};

/// The cook key: FNV-1a 64 seeded with kPageStoreFormatVersion over the inputs, the height filter
/// and the encoding rule (kHeightStepMax).
uint64 ComputeHeightStoreKey(const HeightStoreCookInputs& inputs);

/// The store of `asset` cooked under `key`: <directory>/<asset GUID>-<key, 16 hex digits>.gepage.
std::filesystem::path HeightStoreFile(const std::filesystem::path& directory, const GUID& asset, uint64 key);

/// The identity remembered for `source`'s file at its current modification time, size and file
/// identity, or none when there is no memo for `asset` in `directory` or the file has changed since
/// (then a full read gives the identity: ScanHeightSource).
std::optional<HeightSourceIdentity> ReadHeightSourceMemo(const std::filesystem::path& directory, const GUID& asset,
                                                         const std::filesystem::path& source);

/// Remembers `identity` for `source`'s file as it is now, in <directory>/<asset GUID>.source.
/// False on an I/O failure.
bool WriteHeightSourceMemo(const std::filesystem::path& directory, const GUID& asset,
                           const std::filesystem::path& source, const HeightSourceIdentity& identity);

} // namespace GameEngine::PageStreaming
