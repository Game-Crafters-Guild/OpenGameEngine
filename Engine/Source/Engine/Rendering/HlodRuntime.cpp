#include "Engine/Rendering/HlodRuntime.h"

#include "Assets/HlodBakeDriver.h" // ResolveHlodVolumeConfig
#include "Assets/HlodCache.h"
#include "Assets/HlodReconcile.h"
#include "Assets/ModelAsset.h" // Mesh / Vertex
#include "Components/Rendering/HLODProxy.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "Logger/Logger.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <limits>
#include <string>

namespace GameEngine {
namespace Hlod {

namespace {

// Synthetic mesh identity for a cluster's proxy submeshes (C7): namespaced under
// engine/hlod/<scene>/<cluster> so it can never collide with a real asset GUID (a
// model-level reload or unregister would otherwise be able to kill a proxy mesh).
GUID ProxyMeshAssetGuid(const std::string& sceneKey, uint32 runtimeClusterId) {
    return GUID::Derive(GUID::Null(),
                        "engine/hlod/" + sceneKey + "/" + std::to_string(runtimeClusterId));
}

// Rebuild a renderable Mesh from a baked proxy submesh. The vertices are already
// cluster-relative (baked around the cluster sphere center), so the local bounds
// come straight from them.
Mesh MeshFromBakedSubmesh(const BakedSubmesh& sub) {
    Mesh mesh;
    mesh.Name = "HlodProxy";
    mesh.Vertices = sub.Vertices;
    mesh.Indices = sub.Indices;
    if (!sub.Color0.empty())
        mesh.Color0 = sub.Color0;
    if (!sub.TexCoords1.empty())
        mesh.TexCoords1 = sub.TexCoords1;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;

    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    for (const Vertex& v : sub.Vertices) {
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], v.Position[c]);
            hi[c] = std::max(hi[c], v.Position[c]);
        }
    }
    if (sub.Vertices.empty()) {
        for (int c = 0; c < 3; ++c)
            lo[c] = hi[c] = 0.0f;
    }
    for (int c = 0; c < 3; ++c) {
        mesh.MinBounds[c] = lo[c];
        mesh.MaxBounds[c] = hi[c];
    }
    return mesh;
}

// Build the live-scene reconcile index from the same gather the offline bake used,
// so StableIds and the C3 key inputs line up field-for-field.
LiveMemberIndex BuildLiveIndex(GameEngine::ECS::World& world, const ModelResolver& resolveModel) {
    LiveMemberIndex live;
    GatheredMembers gathered = GatherHlodMembers(world, resolveModel);
    live.reserve(gathered.Members.size());
    for (size_t i = 0; i < gathered.Members.size(); ++i) {
        const MemberInput& m = gathered.Members[i];
        LiveMemberState state;
        state.Entity = gathered.Entities[i];
        state.Resolved.StableId = m.StableId;
        state.Resolved.MeshContentHash = m.MeshContentHash;
        state.Resolved.MaterialGuid = m.MaterialGuid;
        state.Resolved.MeshHandleKey = m.MeshHandleKey;
        for (int k = 0; k < 16; ++k)
            state.Resolved.Transform[k] = m.WorldMatrix[k];
        state.Resolved.ChosenLod = m.ChosenLod;
        state.Resolved.CastsShadow = m.CastsShadow ? 1u : 0u;
        live.emplace(m.StableId, state);
    }
    return live;
}

} // namespace

void HlodRuntime::Retire(GameEngine::ECS::World& world,
                         GameEngine::Rendering::MeshGPURegistry& meshReg) {
    if (m_Clusters.empty())
        return;

    for (RuntimeCluster& cluster : m_Clusters) {
        for (GameEngine::ECS::EntityHandle proxy : cluster.Proxies) {
            if (world.IsValid(proxy))
                world.DestroyEntity(proxy);
        }
        // C7: no refcount on RegisterSubmesh, so the proxy meshes leak until
        // Shutdown unless explicitly unregistered here.
        for (const GameEngine::Rendering::MeshGPUKey& key : cluster.ProxyMeshKeys)
            meshReg.UnregisterSubmesh(key);
    }
    world.ProcessCommands();
    m_Clusters.clear();
    m_BoundWorldId = 0;
}

uint32 HlodRuntime::ReconcileScene(GameEngine::ECS::World& world,
                                   GameEngine::Rendering::MeshGPURegistry& meshReg,
                                   const std::filesystem::path& cacheFile,
                                   const std::string& sceneKey,
                                   const ModelResolver& resolveModel) {
    Retire(world, meshReg);

    // The bake's ConfigHash folds the cook-semantics versions (LOD generator,
    // baker, meshopt, FBX loader) plus the HLODVolume grid config. Recompute the
    // expectation from the live scene so a .gehlod baked under older semantics
    // is REJECTED here instead of silently drawing stale proxy geometry beside
    // freshly-cooked members. Member drift is not gated here — the per-cluster
    // reconcile below handles it one cluster at a time (C3).
    GridConfig volumeConfig;
    if (!ResolveHlodVolumeConfig(world, volumeConfig)) {
        Logger::Log::Info(
            "Hlod: '{}' exists but the scene has no enabled HLODVolume — bake ignored, "
            "members render directly (re-run Bake HLOD after re-adding a volume)",
            cacheFile.string());
        return 0u;
    }
    const uint64 expectConfigHash = ComputeHlodConfigHash(volumeConfig);

    HlodBakedScene baked;
    const HlodCacheStatus status = ReadHlodCache(cacheFile, &expectConfigHash, baked);
    if (status == HlodCacheStatus::KeyMismatch) {
        Logger::Log::Warning(
            "Hlod: STALE bake '{}' rejected (config/version mismatch: the LOD cook "
            "semantics or the HLODVolume config changed since it was baked) — members "
            "render directly. Re-run Bake HLOD to restore proxies.",
            cacheFile.string());
        return 0u;
    }
    if (status != HlodCacheStatus::Hit || baked.Clusters.empty())
        return 0u;

    const LiveMemberIndex live = BuildLiveIndex(world, resolveModel);
    const ReconcileResult reconciled = ReconcileBakedScene(baked, live);

    m_BoundWorldId = world.GetWorldId();

    for (const ReconciledCluster& rc : reconciled.Clusters) {
        if (rc.Members.empty())
            continue; // no live member resolved -> nothing to render or toggle

        const uint32 runtimeId = static_cast<uint32>(m_Clusters.size());
        RuntimeCluster cluster;
        for (int c = 0; c < 3; ++c)
            cluster.SphereCenter[c] = rc.SphereCenter[c];
        cluster.SphereRadius = rc.SphereRadius;
        cluster.Members = rc.Members;
        cluster.Stale = rc.Stale;

        // Capture the C3 staleness baseline + a member render-layer mask to give
        // the proxy the same layer the members drew on.
        uint32 memberLayerMask = 1u;
        cluster.MemberTransformVersion.reserve(rc.Members.size());
        cluster.MemberMeshHandleId.reserve(rc.Members.size());
        cluster.MemberMaterialGuid.reserve(rc.Members.size());
        for (GameEngine::ECS::EntityHandle member : rc.Members) {
            const auto* wt = world.GetComponent<Components::WorldTransform>(member);
            const auto* mr = world.GetComponent<Components::MeshRenderer>(member);
            cluster.MemberTransformVersion.push_back(wt ? wt->Version : 0u);
            cluster.MemberMeshHandleId.push_back(mr ? mr->meshGpuHandleId : 0u);
            cluster.MemberMaterialGuid.push_back(mr ? mr->materialAssetGuid.ToGuid() : GUID::Null());
            if (mr)
                memberLayerMask = mr->renderLayerMask;
        }

        // A stale cluster stays members-only (no proxy spawned); the members drive
        // the scene, the cluster never goes proxy-active until a rebake + reload.
        if (!cluster.Stale) {
            const BakedCluster& bc = baked.Clusters[rc.ClusterId];
            const GUID proxyAsset = ProxyMeshAssetGuid(sceneKey, runtimeId);
            for (size_t s = 0; s < bc.Submeshes.size(); ++s) {
                const BakedSubmesh& sub = bc.Submeshes[s];
                if (sub.Vertices.empty() || sub.Indices.empty())
                    continue;

                const GameEngine::Rendering::MeshGPUKey key{proxyAsset,
                                                            static_cast<uint32>(s)};
                const Mesh proxyMesh = MeshFromBakedSubmesh(sub);
                const GameEngine::Rendering::MeshGPUHandle handle =
                    meshReg.RegisterSubmesh(key, proxyMesh);
                if (!handle.IsValid())
                    continue;
                cluster.ProxyMeshKeys.push_back(key);

                GameEngine::ECS::Entity proxy = world.Create();
                proxy.Set(Components::Transform::FromTRS(
                    Mathematics::Vector3{cluster.SphereCenter[0], cluster.SphereCenter[1],
                                         cluster.SphereCenter[2]},
                    Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));

                Components::MeshRenderer mr{};
                mr.meshGpuHandleId = static_cast<uint64>(handle);
                mr.materialAssetGuid.Set(sub.MaterialGuid);
                mr.renderLayerMask = memberLayerMask;
                mr.castShadows = true;
                mr.receiveShadows = true;
                mr.motionVectors = false; // immobile proxy: no motion vectors
                proxy.Set(mr);

                Components::LocalBounds bounds{};
                bounds.Box = Mathematics::BoundingBox::FromMinMax(
                    Mathematics::Vector3{proxyMesh.MinBounds[0], proxyMesh.MinBounds[1],
                                         proxyMesh.MinBounds[2]},
                    Mathematics::Vector3{proxyMesh.MaxBounds[0], proxyMesh.MaxBounds[1],
                                         proxyMesh.MaxBounds[2]});
                bounds.DynamicObject = false;
                bounds.CastShadows = true;
                proxy.Set(bounds);

                Components::MeshGPUData gpu{};
                gpu.hlodEvicted = true; // proxies start evicted; members are resident
                proxy.Set(gpu);

                proxy.Set(Components::HLODProxy{runtimeId});
                proxy.Set(Components::RuntimeOnlyEntity{});

                cluster.Proxies.push_back(proxy.GetHandle());
            }
        }

        m_Clusters.push_back(std::move(cluster));
    }

    world.ProcessCommands();

    Logger::Log::Info(
        "Hlod: reconciled {} runtime clusters ({} stale, {} missing members) from '{}'",
        m_Clusters.size(), reconciled.StaleClusters, reconciled.TotalMissingMembers,
        cacheFile.string());

    return static_cast<uint32>(m_Clusters.size());
}

} // namespace Hlod
} // namespace GameEngine
