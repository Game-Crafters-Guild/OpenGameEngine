#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine
{
namespace ECS { class World; }
namespace Engine::Renderer
{
class RenderServices;

// Counts published by RefreshLocalBoundsAfterMeshReload so the caller can log a
// single observable line proving the pass ran and how much it changed.
struct MeshReloadBoundsRefreshReport
{
    uint32 EntitiesVisited   = 0; // entities of a reloaded model carrying LocalBounds
    uint32 BoundsRefreshed   = 0; // entities whose LocalBounds.Box actually changed
    uint32 HandlesUnresolved = 0; // entities whose mesh handle resolved to no entry

    bool ChangedAnything() const { return BoundsRefreshed != 0; }
};

// Re-derive Components::LocalBounds for the entities of a model whose GPU
// geometry was just refreshed IN PLACE.
//
// LocalBounds.Box is derived state, never authored. ResolveOneModelEntity seeds
// it from the entity's MeshGPUEntry, but only when the component is ABSENT — so
// an in-place hot-reload, which deliberately keeps the mesh handles valid,
// leaves every entity describing its pre-reload geometry with nothing to
// correct it. The GPUMesh row's own bounds DO refresh (BuildGpuMeshRow
// re-derives from the entry), so GPU LOD selection stays right; what goes stale
// is everything derived from the ECS column, which is at least: the per-instance
// frustum cull radius (RenderExtractionSystem publishes boundingRadius =
// Box.Radius() * maxScale, so geometry that GREW is culled early at frustum
// edges), and the Scene TLAS leaf AABB behind editor picking, marquee select
// and framing.
//
// `reloadedModelGuids` bounds the pass to the models whose geometry actually
// changed. That scoping is load-bearing, not an optimisation: LocalBounds has
// writers whose value deliberately differs from the entity's own submesh entry
// — PrimitiveGenerator seeds a default-parameter box for a GUID the registry
// does not hold, and a package extraction system (EZTree) publishes its own
// generated bounds — and refreshing world-wide would silently rewrite those
// entities on an unrelated model's reload.
//
// Bounds come per-entity from the MeshGPUEntry its own handle resolves to,
// which is the derivation ResolveOneModelEntity uses. Only Box is written:
// DynamicObject and CastShadows are authored and are preserved. Writes are
// value-gated against the entry, because extraction runs a Changed<> probe over
// this column and a write grant stamps the whole (chunk, column) — the same
// idiom MorphTargetSystem::WriteBounds uses for this same column.
//
// Runs on the thread that owns the World, ahead of the ECS schedule. That
// position is load-bearing: waves are solved purely from declared dependency
// edges, so a system writing this column would race every wave-mate that reads
// it — OceanExtraction and EZTreeExtraction both read it and declare only
// {TransformHierarchy, Camera} — and a package can add another reader at any
// time. Ahead of the whole schedule there is nothing to race and no reader has
// to declare an edge it cannot know about.
MeshReloadBoundsRefreshReport RefreshLocalBoundsAfterMeshReload(
    ECS::World& world, RenderServices& renderServices,
    const std::vector<GUID>& reloadedModelGuids);

} // namespace Engine::Renderer
} // namespace GameEngine
