#pragma once

// HLOD runtime state owner (design v0.2 §5.1, runtime integration layer). Bridges
// a loaded scene to its baked .gehlod: on scene load it reads + validates the
// cache, reconciles the content-keyed baked clusters against the entity-keyed live
// scene (HlodReconcile / F6), registers each admitted cluster's merged proxy
// meshes directly (RegisterSubmesh, C7), spawns one runtime-only proxy entity per
// submesh — started EVICTED so members are the resident set — and tags the live
// members. The resulting runtime cluster table is the authority the
// HLODSelectSystem toggles each frame. On unload/reconcile it retires: unregisters
// the proxy meshes (C7, no refcount -> explicit) and destroys the proxy entities.
//
// One HlodRuntime is owned by RenderServices and bound to one World at a time
// (v1: the editor primary world / the packaged Player world).

#include "Assets/HlodMemberGather.h"           // ModelResolver
#include "Engine/Rendering/MeshGPURegistry.h"  // MeshGPUKey
#include "AssetCore/GUID.h"
#include "ECS/ECS.h"                            // EntityHandle
#include "Types/Types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine {

namespace ECS { class World; }
namespace Rendering { class MeshGPURegistry; }

namespace Hlod {

// One runtime cluster: the live member + proxy entities, the switch inputs, and
// the per-member cached identity for live staleness detection (design §3.4 / C3).
// Members resident when !ProxyActive, proxies resident when ProxyActive — never
// both, never neither, driven by the single ProxyActive boolean (§5.4).
struct RuntimeCluster {
    float SphereCenter[3] = {0.0f, 0.0f, 0.0f};
    float SphereRadius = 0.0f;
    std::vector<GameEngine::ECS::EntityHandle> Members;
    std::vector<GameEngine::ECS::EntityHandle> Proxies;
    // Synthetic mesh keys registered for this cluster's proxy submeshes; retire
    // unregisters each (C7 — no refcount, so this is the only release path).
    std::vector<GameEngine::Rendering::MeshGPUKey> ProxyMeshKeys;
    // Captured at reconcile so a member move (WorldTransform.Version) or a
    // material/mesh reassignment (MeshRenderer) can be detected per-frame and
    // fall the cluster back to members-only with a persistent stale flag (C3).
    std::vector<uint32> MemberTransformVersion;
    std::vector<uint64> MemberMeshHandleId;
    std::vector<GUID>   MemberMaterialGuid;
    bool ProxyActive = false; // hysteresis residency state (members resident when false)
    bool Stale = false;       // members-only fallback: never goes proxy-active
};

class HlodRuntime {
public:
    // Retire any prior cluster set, then reconcile `world` against the sidecar at
    // `cacheFile`. The read validates format + bounds AND the bake's ConfigHash
    // against one recomputed from the live HLODVolume + current cook-semantics
    // versions — a bake from older semantics (e.g. a pre-kLodGeneratorVersion-
    // bump .gehlod) is rejected with a warning and members render directly,
    // never a silent stale proxy. Member drift stays per-cluster (live source
    // hash drives staleness). Registers proxy meshes + spawns proxy entities +
    // tags members. `sceneKey` namespaces the synthetic proxy mesh GUIDs (C7).
    // A missing/invalid/stale cache (or a scene with no enabled HLODVolume)
    // retires and returns 0. Returns the number of runtime clusters built
    // (admitted, with >= 1 live member).
    uint32 ReconcileScene(GameEngine::ECS::World& world,
                          GameEngine::Rendering::MeshGPURegistry& meshReg,
                          const std::filesystem::path& cacheFile,
                          const std::string& sceneKey,
                          const ModelResolver& resolveModel);

    // Destroy proxy entities + unregister proxy meshes + drop member tags. Safe to
    // call with no active clusters.
    void Retire(GameEngine::ECS::World& world, GameEngine::Rendering::MeshGPURegistry& meshReg);

    std::vector<RuntimeCluster>& GetClusters() { return m_Clusters; }
    const std::vector<RuntimeCluster>& GetClusters() const { return m_Clusters; }
    bool HasClusters() const { return !m_Clusters.empty(); }
    uint64 GetBoundWorldId() const { return m_BoundWorldId; }

private:
    std::vector<RuntimeCluster> m_Clusters;
    uint64 m_BoundWorldId = 0;
};

} // namespace Hlod
} // namespace GameEngine
