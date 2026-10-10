#pragma once

#include <filesystem>

#include "AssetCore/GUID.h"
#include "MeshPicking/MeshBvh.h"
#include "Types/Types.h"

namespace GameEngine::Editor::Picking
{

// Disk-cache version constants. Bump kBvhFileVersion on layout changes
// (rebuild every cache); bump kBvhBuilderVersion when the BVH algorithm
// changes shape so cached trees produced by older builders are rejected
// even when the file format is identical.
constexpr uint32 kBvhFileMagic       = 0x56424547u; // 'GEBV' little-endian
constexpr uint32 kBvhFileVersion     = 2u;
constexpr uint32 kBvhBuilderVersion  = 1u;

// Triangle count below which disk persistence is skipped. Currently 0:
// every BVH-eligible mesh (>= MeshPicking::kMeshAutoBvhThreshold = 256
// triangles) gets persisted. The original audit recommended 25K to avoid
// filesystem clutter on huge projects, but at typical project scale even
// 100s of small files are negligible vs MFT capacity, and a high threshold
// makes the cache invisible during normal verification. Bump this if a
// real project shows MFT pressure or .Cache size becomes a problem.
constexpr uint32 kDiskCacheTriThreshold = 0u;

// Persist a BVH for the given asset+submesh under cacheRoot, keyed by `geometryHash`
// (ComputeGeometryHash of the mesh it was built from). Path layout:
//   <cacheRoot>/MeshBvh/<guidCompact>/sm<submesh>_v<builderVer>.meshbvh
// Atomic temp+rename. Best-effort: returns false on I/O failure but never
// throws - picking falls back to in-memory builds.
bool PersistBvh(const std::filesystem::path& cacheRoot,
                const GUID& modelGuid,
                uint32 submesh,
                uint64 geometryHash,
                const MeshPicking::MeshBvh& bvh);

// Try to load a previously persisted BVH. Validates magic, file version,
// builder version and the geometry hash before trusting the bytes, then
// rebuilds the triangles from `source`. Returns false on any mismatch or I/O
// error, and logs the reason once; outBvh is left empty on failure.
bool TryLoadBvh(const std::filesystem::path& cacheRoot,
                const GUID& modelGuid,
                uint32 submesh,
                uint64 expectedGeometryHash,
                const MeshPicking::MeshView& source,
                MeshPicking::MeshBvh& outBvh);

// Delete every cache file for the given asset (all submeshes). No-op if the
// directory doesn't exist. Used by the AssetEvent invalidation handler.
void InvalidateBvhCacheFiles(const std::filesystem::path& cacheRoot,
                             const GUID& modelGuid);

// The disk-cache key of a mesh: a hash of every position and index it holds, and of their
// counts. The BVH is a function of exactly that geometry, so the key is the same in every
// session and on every copy of the project, whatever the source file's timestamps or the
// asset database's state, and any edit to the geometry changes it.
uint64 ComputeGeometryHash(const MeshPicking::MeshView& mesh);

}
