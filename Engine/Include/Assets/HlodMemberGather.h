#pragma once

// HLOD member gather (design v0.2 §3.2, runtime-half member-gather slice). The
// bridge from the live ECS scene to the pure offline baker (HlodBaker.h): it
// mirrors the RenderExtractionSystem eligibility query, resolves each eligible
// static MeshRenderer's identity + spatial + C3 bake-key inputs (stable GUID,
// resolved material GUID, model+submesh selector, content hash, world AABB,
// coarsest LOD) and its CPU source geometry, and emits the parallel MemberInput
// + Mesh* arrays BakeScene consumes.
//
// Model geometry is resolved through a caller-supplied ModelResolver rather than
// reaching into the AssetManager directly, so the gather is unit-testable with a
// World(nullptr) + SetMeshesForTest-backed model map and no live asset system.
// The resolved ModelAsset (and the Mesh pointers into it) must outlive the
// subsequent BakeScene call.

#include "Assets/HlodClustering.h" // MemberInput
#include "AssetCore/GUID.h"
#include "ECS/ECS.h" // EntityHandle
#include "Types/Types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine {

class ModelAsset;
struct Mesh;

namespace ECS { class World; }

namespace Hlod {

// Resolve a model GUID to its loaded ModelAsset, or nullptr when unavailable.
using ModelResolver = std::function<const ModelAsset*(const GUID&)>;

// Parallel arrays feeding HlodBaker::BakeScene, plus the owning member entity
// (for the editor bake-time proxy setup) and a breakdown of what was excluded.
struct GatheredMembers {
    std::vector<MemberInput> Members;                    // BakeScene member span
    std::vector<const Mesh*>  Geometry;                  // parallel: source submesh, may be null
    std::vector<GameEngine::ECS::EntityHandle> Entities; // parallel: owning member entity

    uint32 SkippedSkinned = 0;     // SkinnedMeshRenderer / skinned mesh
    uint32 SkippedMorph = 0;       // morph-target carriers
    uint32 SkippedNoModel = 0;     // no resolvable ModelAsset
    uint32 SkippedNoMaterial = 0;  // null material GUID
    uint32 SkippedNonTriangle = 0; // non-triangle topology submesh
    uint32 SkippedDisabled = 0;    // inactive entity or switched-off renderer
};

// Gather eligible static members from `world`. Excludes skinned/morph members,
// members with no resolvable model or material, non-triangle topology, and
// disabled entities or renderers. Members are emitted in world iteration order; a fixed world
// yields a deterministic gather. The non-default-custom0 exclusion (design §3.2)
// is satisfied by construction — no MeshRenderer static writes GPUInstance.custom0
// today (only the particle path does, and it is not gathered here).
GatheredMembers GatherHlodMembers(GameEngine::ECS::World& world, const ModelResolver& resolveModel);

} // namespace Hlod
} // namespace GameEngine
