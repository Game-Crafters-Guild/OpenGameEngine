#pragma once

#include <filesystem>
#include <memory>

#include "AssetCore/GUID.h"
#include "MeshPicking/MeshBvh.h"
#include "Types/Types.h"

namespace GameEngine { struct Mesh; }

namespace GameEngine::Editor::Picking
{

// What a non-blocking lookup found for one mesh.
struct MeshBvhLookup
{
    // The BVH; null while it is pending, and for a mesh with no BVH (empty or invalid), which
    // the caller tests triangle by triangle.
    std::shared_ptr<const MeshPicking::MeshBvh> Bvh;
    // A job is loading or building it. The caller answers without the mesh's triangles this
    // time; a later lookup returns it.
    bool Pending = false;
};

// Process-wide BVH cache keyed by (modelGuid, submeshIndex), the one owner of picking BVH
// builds. A key is built once, by a job (TryGet) or on the calling thread (GetOrBuild), and the
// result, a mesh with no BVH included, is kept until the asset is invalidated. Thread-safe.
//
// The cache holds shared ownership of the BVH; entries survive ModelAsset
// unload (they're self-sufficient because MeshBvh deep-copies triangle data
// at build time). Callers explicitly invalidate on hot-reload.
class MeshBvhCache
{
public:
    static MeshBvhCache& Instance();

    // Never builds on the calling thread. Returns the cached BVH, or Pending after starting
    // one background job for the key (later calls find it in flight) that loads the BVH from
    // the disk cache or builds it, then publishes it. The positions and indices the job reads
    // are copied from `source` here, so a hot reload of the asset cannot change them under it.
    MeshBvhLookup TryGet(const GUID& guid, uint32 submesh, const Mesh& source);

    // Get the BVH for (guid, submesh): the cached one, the result of a build already running
    // (waited for, at most one build), or one built from `source` on the calling thread. A
    // build TryGet queued that no job has started yet is built here, so the caller never waits
    // for the jobs queued ahead of it. Returns nullptr if the source mesh is empty or invalid.
    std::shared_ptr<const MeshPicking::MeshBvh> GetOrBuild(const GUID& guid,
                                                           uint32 submesh,
                                                           const Mesh& source);

    // Drop the cached BVH for an asset. `assetPath` is optional but should
    // be supplied during AssetDestroyed handling — by that point the GUID
    // is gone from AssetRegistry::m_Assets, so the path-based cache-root
    // overload is the only way to locate (and delete) the orphaned files.
    void InvalidateAsset(const GUID& guid,
                         const std::filesystem::path& assetPath = {});
    void Clear();

    // Diagnostics: number of cache entries currently resident.
    size_t Size() const;

private:
    MeshBvhCache() = default;
    MeshBvhCache(const MeshBvhCache&) = delete;
    MeshBvhCache& operator=(const MeshBvhCache&) = delete;

    struct Impl;
    static Impl& GetImpl();
};

}
