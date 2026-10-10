#pragma once

#include <cstdint>
#include <string>

namespace GameEngine::AssetCore
{

/// Cached fingerprint for a previously-observed file, captured during a
/// warm-start snapshot and used by the asset registry's reconcile pass to
/// skip stat'ing files whose (mtime, size, fileId) match the snapshot.
///
/// Lives in AssetCore (not AssetDatabase) so AssetRegistry.h can expose
/// the type in its public API without forcing every consumer of
/// AssetRegistry.h to link the AssetDatabase module. The struct is a
/// pure data carrier — no AssetDatabase-specific dependencies.
struct SnapshotFingerprint
{
    int64_t Mtime = 0;
    int64_t Size = 0;
    std::string FileId;
    std::string Hash; // Cached content hash; reuse if stat-confirm passes.
};

} // namespace GameEngine::AssetCore
