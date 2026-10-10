#pragma once

// Cooked HLOD cluster cache (.gehlod). Persists a scene's baked proxy clusters —
// per-cluster bounds, the FULL bake key inputs for every member (stable GUID,
// content hash, resolved material GUID, mesh handle selector, cluster-relative
// transform, chosen LOD), and the merged proxy geometry per material submesh —
// so repeat editor loads and the meshopt-free packaged Player skip the merge
// entirely (design v0.2 §4.3, §10). Mirrors the .gelod discipline
// (MeshLODCache.h): little-endian, magic + format version, atomic temp+rename
// write, fully bounds-checked read that never applies an out-of-range index.
//
// Two keys: the whole-file header carries {SourceHash, ConfigHash}. ConfigHash
// (grid config + cook-semantics versions) gates load validity — the reconcile
// recomputes it from the live scene and rejects a bake whose cook semantics no
// longer match, so stale proxies never draw silently. SourceHash records the
// baked member fold for diagnostics; member drift is deliberately NOT a load
// gate — the per-cluster source hash (over that cluster's members, C3) drives
// per-cluster staleness so one edited member fails one cluster, not the whole
// scene.

#include "Assets/FbxLoaderOptions.h"
#include "Assets/HlodClustering.h"
#include "Assets/MeshLODCache.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace GameEngine {
namespace Hlod {

// Bump when the .gehlod byte layout changes. Gates format compatibility only,
// never folded into the key (that is the config hash's job).
inline constexpr uint32 kHlodCacheFormatVersion = 1u;

// Bump when the baker changes produced proxy geometry for an unchanged input +
// config. Folded into ConfigHash so a baker change makes every prior cook a miss.
// v2 preserves the selected authored LOD's RGBA instead of baking white.
inline constexpr uint32 kHlodBakerVersion = 2u;

// Whole-file cache identity, persisted in the header.
struct HlodCacheKey {
    uint64 SourceHash = 0;   // fold of every cluster's source hash (member identity)
    uint64 ConfigHash = 0;   // grid config + baker/meshopt/loader/LOD-gen versions
};

enum class HlodCacheStatus : uint8 {
    Hit,            // valid: format + (optional) config hash agreed, all indices in bounds
    Missing,        // file absent or unreadable
    FormatMismatch, // wrong magic or unsupported format version
    KeyMismatch,    // ConfigHash disagrees with expectConfigHash (stale bake)
    Corrupt,        // truncated, oversized, or an out-of-range index
};

// One member's full bake-key inputs, persisted so a load can reconcile against
// the live scene (F6) and recompute the cluster key for staleness (C3).
struct BakedMemberRef {
    GUID   StableId;
    uint64 MeshContentHash = 0;
    GUID   MaterialGuid;
    uint64 MeshHandleKey = 0;
    float  Transform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    uint32 ChosenLod = 0u;
    uint8  CastsShadow = 1u;
};

// One proxy submesh: merged cluster-relative geometry for a single material.
struct BakedSubmesh {
    GUID           MaterialGuid;
    Vector<Vertex> Vertices;   // core interleaved stream
    Vector<uint32> Indices;    // every index < Vertices.size() (validated on read)
    Vector<float>  Color0;     // optional: size == Vertices*4, else empty
    Vector<float>  TexCoords1; // optional: size == Vertices*2, else empty
};

struct BakedCluster {
    CellCoord Cell;
    float  SphereCenter[3] = {0.0f, 0.0f, 0.0f};
    float  SphereRadius = 0.0f;
    float  BoundsMin[3] = {0.0f, 0.0f, 0.0f};
    float  BoundsMax[3] = {0.0f, 0.0f, 0.0f};
    uint64 ClusterSourceHash = 0; // over Members (ComputeClusterSourceHash)
    Vector<BakedMemberRef> Members;
    Vector<BakedSubmesh>   Submeshes;
};

struct HlodBakedScene {
    HlodCacheKey Key;
    float CellSize = 0.0f;
    float GridOrigin[3] = {0.0f, 0.0f, 0.0f};
    Vector<BakedCluster> Clusters;
};

// FNV-1a over a cluster's members (stable GUID order assumed already applied by
// the caller): each member's GUID, content hash, material GUID, mesh handle
// selector, quantized transform, and chosen LOD (the C3 fold). A material
// reassignment or a member move perturbs this.
uint64 ComputeClusterSourceHash(std::span<const BakedMemberRef> members);

// FNV-1a over the grid config + baker + meshopt versions, plus the FBX import
// geometry version — the proxy is merged from FBX-imported member meshes, so a
// loader-geometry change (kFbxImportGeometryVersion bump) must invalidate stale
// .gehlod proxies exactly like the .gelod fold does for cooked LODs — plus the
// LOD generator version: the proxy bakes each member's COARSEST cooked LOD
// (HlodMemberGather), so a cook-semantics change (kLodGeneratorVersion bump)
// changes the geometry a rebake would merge and must miss the cache. The
// versions are defaulted parameters so the baker binds the current salts while
// tests can perturb them — including the baker's own revision, which decides
// whether a proxy cooked by an older baker still validates.
uint64 ComputeHlodConfigHash(const GridConfig& config,
                             uint8 fbxGeometryVersion = kFbxImportGeometryVersion,
                             uint32 lodGeneratorVersion = kLodGeneratorVersion,
                             uint32 bakerVersion = kHlodBakerVersion);

// Serialize `scene` atomically (temp + rename). Returns false on I/O failure
// (target left as-is). Empty path returns false.
bool WriteHlodCache(const std::filesystem::path& file, const HlodBakedScene& scene);

// Read + validate a .gehlod into `out` (cleared first). On any non-Hit status
// `out` is left empty. When `expectConfigHash` is non-null the header's
// ConfigHash must match — the reconcile computes it from the live HLODVolume
// via ComputeHlodConfigHash (binding the CURRENT cook-semantics versions), so
// a bake produced by an older generator/baker/config is rejected as
// KeyMismatch instead of silently drawing stale proxy geometry. SourceHash is
// deliberately not an expectation: member drift is handled per-cluster by the
// reconcile (C3). Callers wanting exact-freshness can compare out.Key after a
// Hit. Never throws, never yields an out-of-range index.
HlodCacheStatus ReadHlodCache(const std::filesystem::path& file,
                              const uint64* expectConfigHash,
                              HlodBakedScene& out);

} // namespace Hlod
} // namespace GameEngine
