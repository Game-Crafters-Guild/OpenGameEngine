#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Mathematics/Ray.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor::Picking
{

// What kind of object the picking ray hit. Tools branch on this when the
// follow-up action depends on the surface (e.g. terrain brush vs. mesh select).
enum class PickableKind : uint8
{
    None,
    Mesh,
    Terrain,
    Primitive,
    Bounds,
};

// Caller-facing options for RaycastScene. Public fields are PascalCase per
// CodingStyle.md; defaults exercise every category so callers only set
// what they want to change.
struct PickOptions
{
    bool                          IncludeMeshes        = true;
    bool                          IncludeTerrain       = true;
    bool                          IncludePrimitives    = true;
    bool                          IncludeBoundsFallback = true;  // pick generated/runtime renderers without CPU mesh data
    bool                          IncludeDisabled      = false;   // include ECS::Disabled entities
    uint32                        LayerMask            = 0xFFFFFFFFu;
    float32                       MaxDistance          = 1.0e30f;
    // Entities to skip during traversal — typically the drag-drop preview
    // model's own entities, so the placement ray doesn't self-intersect.
    std::span<const GameEngine::ECS::EntityHandle> IgnoreEntities{};

    // When set to a valid handle, RaycastScene tests only this entity's mesh
    // and ignores everything else (other meshes, terrain, primitives). Used
    // by tools like the spline brush to keep a stroke "stuck" to a single
    // surface across many ray casts.
    GameEngine::ECS::EntityHandle RestrictToEntity{};

    // A mesh large enough to pick through a BVH (kMeshAutoBvhThreshold triangles) whose BVH is
    // not built yet. False, the default for interactive queries: the pick never builds it on
    // the calling thread. A background job builds it (MeshBvhCache::TryGet), and until it is
    // published the mesh answers with its bounds (IncludeBoundsFallback), marked
    // PickHit::MeshBvhPending, or not at all. True,
    // for a caller that writes the hit into authored data and does not ask again: the BVH is
    // awaited or built on the calling thread, so the hit is always on a triangle.
    bool WaitForMeshBvh = false;

    // Whether the pick first brings the scene TLAS current (SceneTlas::WaitCurrent), which
    // blocks while it refits when something moved since the last request. True, the default,
    // for tools that need the answer inside the same call (placement, drape, snapping). False
    // for a caller that already saw RequestCurrent or CheckRequest report Ready this frame:
    // the scene view's click and hover, which defer instead of blocking.
    bool WaitForSceneTlas = true;

    // Reserved for future deformed-mesh picking (Phase A: bind-pose only).
    bool AllowSkinnedDeformed = false;
};

// Result of a successful pick. World-space position and normal are filled in
// for both mesh and terrain hits; triangle index and barycentrics are only
// meaningful for mesh hits.
struct PickHit
{
    PickableKind                  Kind          = PickableKind::None;
    GameEngine::ECS::EntityHandle Entity{};
    uint32                        SubmeshIndex  = 0u;
    uint32                        TriangleIndex = ~0u;
    Mathematics::Vector3          Barycentrics{};
    Mathematics::Vector3          WorldPosition{};
    Mathematics::Vector3          WorldNormal{};
    float32                       Distance      = 0.0f;
    // A Bounds hit standing in for a mesh whose picking BVH is still being built
    // (PickOptions::WaitForMeshBvh false). Good for a preview; a caller that would write the
    // hit into the scene refuses it and lets the user try again once the BVH is published.
    bool                          MeshBvhPending = false;
};

struct PickResult
{
    bool    Hit = false;
    PickHit Best{};
};

// Cast a world-space ray against the scene. Tests every entity matching
// (WorldTransform + MeshRenderer) plus terrain (when enabled) and returns
// the closest hit.
//
// Thread affinity: the picking *entry points* (RaycastScene/RaycastSceneAll)
// are intended for the editor / main thread because they
// query the live ECS World and dereference raw `Mesh*` from ModelAsset
// storage that AssetManager hot-reload can mutate. Calling them from a
// worker thread is unsafe today.
//
// The underlying BVH cache itself is fully thread-safe (per-key build
// coalescing via shared_future, internal mutex on the map). Phase B's
// bake-on-PostLoad path will invoke the cache from JobSystem worker
// threads — that is OK precisely because the cache is multi-thread-safe;
// only the entry points are main-thread-bound.
//
// The first picking call lazily registers a callback on AssetManager's
// event dispatcher to invalidate the BVH cache on AssetModified /
// AssetReloaded / AssetDestroyed for Model assets.
PickResult RaycastScene(const Mathematics::Ray3D& ray,
                        GameEngine::ECS::World& world,
                        const PickOptions& options = {});

// Cast a world-space ray and return ALL hits along it, sorted front to back.
// Used by cycle-pick (alt-click) tooling that needs to step through stacked
// entities, and by scene-view selection. Terrain contributes at most its
// nearest hit (it is one surface, not a stack) when IncludeTerrain is set.
void RaycastSceneAll(const Mathematics::Ray3D& ray,
                     GameEngine::ECS::World& world,
                     std::vector<PickHit>& outHits,
                     const PickOptions& options = {});

// Drop the cached BVH for a single asset (e.g. on hot-reload).
//
// `assetPath` is optional but should be passed during AssetDestroyed
// handling: by the time that event fires, the GUID has been erased from
// AssetRegistry::m_Assets, so the GUID->path lookup fails and the disk
// cache directory can't be located. Supplying the path lets the
// path-based AssetRegistry::TryGetCacheRoot overload resolve the cache
// root and clean up the orphaned files.
void InvalidateCacheForAsset(const GUID& modelGuid,
                             const std::filesystem::path& assetPath = {});

// Drop every cached BVH (e.g. on project unload).
void ClearCache();

}
