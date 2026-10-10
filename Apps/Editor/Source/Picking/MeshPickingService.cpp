#include "Picking/MeshPickingService.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Core/CpuProfiler.h"
#include "MeshPicking/MeshBvh.h"
#include "MeshPicking/PickTypes.h"
#include "Scene/SceneTlas.h"

#include "Picking/EditorPickProviders.h"
#include "Picking/MeshBvhCache.h"
#include "Picking/PrimitiveMeshes.h"
#include "Picking/TerrainPicking.h"

namespace GameEngine::Editor::Picking
{

namespace
{

using Mathematics::Vector3;

// Produce the renderer's model GUID and a validity flag (some renderers
// haven't been assigned a model yet).
bool ExtractGuid(const Components::MeshRenderer& mr, GUID& outGuid)
{
    if (mr.modelAssetGuid.IsNull())
        return false;
    outGuid = mr.modelAssetGuid.ToGuid();
    return true;
}

// Resolve the CPU mesh data for a renderer. For built-in primitives the
// geometry is generated on demand by PrimitiveMeshes; for asset-backed
// renderers we fetch the ModelAsset out of AssetManager (no force-load:
// silently skip if not yet resident). Returns nullptr on miss.
const Mesh* ResolveCpuMesh(const GUID& guid,
                           uint32 submesh,
                           PickableKind& outKind)
{
    if (const Mesh* prim = ResolvePrimitiveMesh(guid))
    {
        outKind = PickableKind::Primitive;
        return prim;
    }

    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    auto asset = assetManager.GetAsset(guid);
    if (!asset)
        return nullptr;
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return nullptr;
    if (submesh >= modelAsset->GetMeshCount())
        return nullptr;

    outKind = PickableKind::Mesh;
    return &modelAsset->GetMesh(submesh);
}

const Mesh* ResolveRegisteredCpuMesh(const Components::MeshRenderer& mr,
                                     const GUID* guid,
                                     PickableKind& outKind)
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return nullptr;

    const Rendering::MeshGPURegistry& registry = renderServices->GetMeshGPURegistry();
    const Rendering::MeshGPUEntry* entry = nullptr;

    if (mr.meshGpuHandleId != 0u)
    {
        entry = registry.Find(Rendering::MeshGPUHandle(mr.meshGpuHandleId));
    }
    if (!entry && guid && !guid->IsNull())
    {
        entry = registry.FindByKey(Rendering::MeshGPUKey{*guid, mr.meshId});
    }

    if (!entry || !entry->cpuMesh)
        return nullptr;

    outKind = PickableKind::Mesh;
    return entry->cpuMesh.get();
}

// World-space triangle normal at the hit. Phase A: applies the world matrix
// to the local face normal and renormalises. Correct for uniform scale and
// rotation; under non-uniform scale this is approximate (proper handling
// requires inverse-transpose of the upper 3x3 and is deferred until a tool
// surfaces the need).
Vector3 ComputeWorldNormal(const Mesh& mesh,
                           uint32 triangleIndex,
                           const Mathematics::Matrix4x4& worldMatrix)
{
    if (triangleIndex == ~0u)
        return Vector3(0.0f, 1.0f, 0.0f);
    const uint32 base = triangleIndex * 3u;
    if (base + 2u >= mesh.Indices.size())
        return Vector3(0.0f, 1.0f, 0.0f);
    const uint32 i0 = mesh.Indices[base + 0];
    const uint32 i1 = mesh.Indices[base + 1];
    const uint32 i2 = mesh.Indices[base + 2];
    if (i0 >= mesh.Vertices.size() || i1 >= mesh.Vertices.size() || i2 >= mesh.Vertices.size())
        return Vector3(0.0f, 1.0f, 0.0f);

    const auto& a = mesh.Vertices[i0].Position;
    const auto& b = mesh.Vertices[i1].Position;
    const auto& c = mesh.Vertices[i2].Position;
    const Vector3 v0(a[0], a[1], a[2]);
    const Vector3 v1(b[0], b[1], b[2]);
    const Vector3 v2(c[0], c[1], c[2]);
    const Vector3 nLocal = Vector3::Cross(v1 - v0, v2 - v0).Normalize();

    Mathematics::Matrix4x4 worldCopy = worldMatrix;
    const Vector3 worldZero = worldCopy.TransformPoint(Vector3(0, 0, 0));
    const Vector3 worldHead = worldCopy.TransformPoint(nLocal);
    Vector3 worldNormal = worldHead - worldZero;
    const float32 len = worldNormal.Length();
    if (len > 1e-6f)
        return worldNormal * (1.0f / len);
    return Vector3(0.0f, 1.0f, 0.0f);
}

Vector3 ComputeAabbHitNormal(const Mathematics::AABB& box, const Vector3& point)
{
    struct Candidate
    {
        float32 Distance;
        Vector3 Normal;
    };

    const Candidate candidates[] = {
        {std::fabs(point.x - box.min.x), Vector3(-1.0f, 0.0f, 0.0f)},
        {std::fabs(point.x - box.max.x), Vector3( 1.0f, 0.0f, 0.0f)},
        {std::fabs(point.y - box.min.y), Vector3(0.0f, -1.0f, 0.0f)},
        {std::fabs(point.y - box.max.y), Vector3(0.0f,  1.0f, 0.0f)},
        {std::fabs(point.z - box.min.z), Vector3(0.0f, 0.0f, -1.0f)},
        {std::fabs(point.z - box.max.z), Vector3(0.0f, 0.0f,  1.0f)},
    };

    const Candidate* best = &candidates[0];
    for (const Candidate& candidate : candidates)
    {
        if (candidate.Distance < best->Distance)
            best = &candidate;
    }
    return best->Normal;
}

bool FillBoundsFallbackHit(const Mathematics::Ray3D& ray,
                           GameEngine::ECS::EntityHandle entity,
                           const Mathematics::AABB& box,
                           float32 boxEntry,
                           PickHit& outHit)
{
    if (boxEntry < 0.0f)
        boxEntry = 0.0f;

    const Vector3 worldHit(ray.origin.x + ray.direction.x * boxEntry,
                           ray.origin.y + ray.direction.y * boxEntry,
                           ray.origin.z + ray.direction.z * boxEntry);

    outHit.Kind          = PickableKind::Bounds;
    outHit.Entity        = entity;
    outHit.SubmeshIndex  = 0u;
    outHit.TriangleIndex = ~0u;
    outHit.Barycentrics  = Vector3{};
    outHit.WorldPosition = worldHit;
    outHit.WorldNormal   = ComputeAabbHitNormal(box, worldHit);
    outHit.Distance      = boxEntry;
    return true;
}

// Inner narrow-phase test for a single entity. AABB cull, fetch CPU mesh,
// transform the ray to local space, run BVH or brute-force per-triangle.
// Returns true and fills outHit on hit. maxT bounds the ray length.
bool TestEntityHit(const Mathematics::Ray3D& ray,
                   GameEngine::ECS::EntityHandle entity,
                   const Components::WorldTransform& worldXf,
                   const Components::MeshRenderer& mr,
                   const Components::LocalBounds* bounds,
                   const PickOptions& options,
                   float32 maxT,
                   PickHit& outHit)
{
    if ((mr.renderLayerMask & options.LayerMask) == 0u)
        return false;

    const Mathematics::BoundingBox kDefault{};
    const auto box = (bounds ? bounds->Box : kDefault).TransformToAABB(worldXf.matrix);
    float32 boxTMin = 0.0f, boxTMax = 0.0f;
    if (!Mathematics::IntersectRayAABB(ray, box, boxTMin, boxTMax))
        return false;
    const float32 boxEntry = (boxTMin >= 0.0f) ? boxTMin : 0.0f;
    if (boxEntry >= maxT)
        return false;

    PickableKind kind = PickableKind::Mesh;
    const Mesh* meshPtr = nullptr;
    GUID guid{};
    if (ExtractGuid(mr, guid))
        meshPtr = ResolveCpuMesh(guid, mr.meshId, kind);
    if (!meshPtr)
        meshPtr = ResolveRegisteredCpuMesh(mr, guid.IsNull() ? nullptr : &guid, kind);
    if (!meshPtr || meshPtr->Indices.empty() || meshPtr->Vertices.empty())
    {
        return options.IncludeBoundsFallback
            ? FillBoundsFallbackHit(ray, entity, box, boxEntry, outHit)
            : false;
    }
    if (kind == PickableKind::Primitive && !options.IncludePrimitives)
        return false;
    if (kind == PickableKind::Mesh && !options.IncludeMeshes)
        return false;

    const Mathematics::Matrix4x4 worldM = Mathematics::Matrix4x4::FromColumnMajor(worldXf.matrix);
    const Mathematics::Matrix4x4 invM = Mathematics::Inverse(worldM);

    const Vector3 worldOrigin(ray.origin.x, ray.origin.y, ray.origin.z);
    const Vector3 worldDir   (ray.direction.x, ray.direction.y, ray.direction.z);
    const Vector3 localOrigin = invM.TransformPoint(worldOrigin);
    const Vector3 localDirEnd = invM.TransformPoint(worldOrigin + worldDir);
    const Vector3 localDir    = localDirEnd - localOrigin;

    MeshPicking::PickHit raw;
    bool hit = false;
    bool testedByBvh = false;
    const uint32 triCount = static_cast<uint32>(meshPtr->Indices.size() / 3u);
    if (triCount >= MeshPicking::kMeshAutoBvhThreshold)
    {
        MeshBvhLookup lookup;
        if (options.WaitForMeshBvh)
            lookup.Bvh = MeshBvhCache::Instance().GetOrBuild(guid, mr.meshId, *meshPtr);
        else
            lookup = MeshBvhCache::Instance().TryGet(guid, mr.meshId, *meshPtr);
        // Testing every triangle instead would cost the calling thread what the job saves it.
        if (lookup.Pending)
        {
            if (!options.IncludeBoundsFallback)
                return false;
            FillBoundsFallbackHit(ray, entity, box, boxEntry, outHit);
            outHit.MeshBvhPending = true;
            return true;
        }
        if (lookup.Bvh)
        {
            hit = lookup.Bvh->Raycast(localOrigin, localDir, raw);
            testedByBvh = true;
        }
    }
    // Every triangle is tested only for a mesh without a BVH: one below kMeshAutoBvhThreshold,
    // or one whose BVH came out empty or failed to build. A BVH's miss is final.
    if (!testedByBvh)
    {
        MeshPicking::MeshView view;
        view.Positions    = reinterpret_cast<const Vector3*>(&meshPtr->Vertices[0].Position[0]);
        view.VertexStride = sizeof(Vertex);
        view.VertexCount  = static_cast<uint32>(meshPtr->Vertices.size());
        view.IndexCount   = static_cast<uint32>(meshPtr->Indices.size());
        view.Indices      = meshPtr->Indices.data();
        hit = MeshPicking::RayMesh(view, localOrigin, localDir, raw);
    }
    if (!hit || raw.Distance >= maxT)
        return false;

    const Vector3 worldHit(ray.origin.x + ray.direction.x * raw.Distance,
                           ray.origin.y + ray.direction.y * raw.Distance,
                           ray.origin.z + ray.direction.z * raw.Distance);

    outHit.Kind          = kind;
    outHit.Entity        = entity;
    outHit.SubmeshIndex  = mr.meshId;
    outHit.TriangleIndex = raw.TriangleIndex;
    outHit.Barycentrics  = raw.Barycentrics;
    outHit.WorldPosition = worldHit;
    outHit.WorldNormal   = ComputeWorldNormal(*meshPtr, raw.TriangleIndex, worldM);
    outHit.Distance      = raw.Distance;
    return true;
}

bool RaycastSingleEntity(const Mathematics::Ray3D& ray,
                         GameEngine::ECS::EntityHandle entity,
                         GameEngine::ECS::World& world,
                         float32 maxT,
                         const PickOptions& options,
                         PickHit& outHit)
{
    if (!world.IsValid(entity))
        return false;
    const auto* worldXf = world.GetComponent<Components::WorldTransform>(entity);
    const auto* mr      = world.GetComponent<Components::MeshRenderer>(entity);
    const auto* bounds  = world.GetComponent<Components::LocalBounds>(entity);
    const auto* sector  = world.GetComponent<Components::WorldSectorCoord>(entity);
    if (!worldXf || !mr)
        return false;
    const float32 sectorSize = Scene::GetSceneTlas(world).GetSectorSize();
    const Components::WorldTransform effectiveXf =
        Components::ComposeEffectiveWorldTransform(*worldXf, sector, sectorSize);
    return TestEntityHit(ray, entity, effectiveXf, *mr, bounds, options, maxT, outHit);
}

bool IsIgnored(ECS::EntityHandle entity, const PickOptions& options)
{
    for (ECS::EntityHandle ignored : options.IgnoreEntities)
    {
        if (ignored == entity)
            return true;
    }
    return false;
}

// Raycast one entity through a provider-supplied synthetic mesh (plugin-owned
// runtime meshes with no MeshRenderer — registered EditorPickProviders). The
// provider hands over pure data; the whole narrow-phase (BVH cache, sector
// compose, layer/ignore rules) stays in this service.
bool RaycastSyntheticPick(const Mathematics::Ray3D& ray,
                          ECS::EntityHandle entity,
                          ECS::World& world,
                          float32 maxT,
                          const PickOptions& options,
                          const SyntheticMeshPick& pick,
                          PickHit& outHit)
{
    if ((options.LayerMask & pick.RenderLayerMask) == 0u || pick.MeshGpuHandleId == 0u ||
        IsIgnored(entity, options))
        return false;
    const auto* worldXf = world.GetComponent<Components::WorldTransform>(entity);
    if (!worldXf)
        return false;
    const auto* bounds = world.GetComponent<Components::LocalBounds>(entity);
    const auto* sector = world.GetComponent<Components::WorldSectorCoord>(entity);

    Components::MeshRenderer synthetic{};
    synthetic.meshGpuHandleId = pick.MeshGpuHandleId;
    synthetic.renderLayerMask = pick.RenderLayerMask;
    synthetic.modelAssetGuid.Set(pick.ModelAssetGuid);

    const float32 sectorSize = Scene::GetSceneTlas(world).GetSectorSize();
    const Components::WorldTransform effectiveXf =
        Components::ComposeEffectiveWorldTransform(*worldXf, sector, sectorSize);
    return TestEntityHit(ray, entity, effectiveXf, synthetic, bounds, options, maxT, outHit);
}

// Restricted-entity variant: resolve the entity against every registered
// provider (first pickable resolve wins).
bool RaycastProviderEntity(const Mathematics::Ray3D& ray,
                           ECS::EntityHandle entity,
                           ECS::World& world,
                           float32 maxT,
                           const PickOptions& options,
                           PickHit& outHit)
{
    for (const EditorPickProvider& provider : EditorPickProviderRegistry::Get().Snapshot())
    {
        SyntheticMeshPick pick;
        if (provider.Resolve && provider.Resolve(world, entity, pick) &&
            RaycastSyntheticPick(ray, entity, world, maxT, options, pick, outHit))
            return true;
    }
    return false;
}

// Lazy: register an AssetEvent callback the first time picking runs, so
// hot-reloading or destroying a model invalidates its cached BVH. Lazy
// because the cache is empty until picking happens, and EngineCore is
// guaranteed alive once the editor is dispatching pointer events.
//
// Uses dispatcher-pointer comparison (not std::call_once) so a recreated
// AssetManager (test harness, project reload) gets the callback re-attached
// on its new dispatcher. The previous registration is implicitly cleaned up
// by the old dispatcher's destruction.
//
// Note on event timing: AssetModified fires on the file-watcher thread BEFORE
// AssetManager::CheckForReloads mutates the asset on the main thread. We
// invalidate eagerly; if a pick races between the invalidate and the actual
// reload, it rebuilds from old vertex data. AssetReloaded then fires after
// the mutation and we invalidate again. Worst case: one wasted build per
// reload. Acceptable.
void EnsureAssetReloadHookRegistered()
{
    static std::atomic<const AssetEventDispatcher*> s_LastDispatcher{nullptr};
    static std::mutex s_RegMutex;

    auto& assetMgr   = EngineCore::GetInstance().GetAssetManager();
    auto& dispatcher = assetMgr.GetEventDispatcher();
    if (s_LastDispatcher.load(std::memory_order_acquire) == &dispatcher)
        return;

    std::lock_guard lock(s_RegMutex);
    if (s_LastDispatcher.load(std::memory_order_relaxed) == &dispatcher)
        return;

    dispatcher.AddCallback([](const AssetEvent& ev)
    {
        if (ev.Type != AssetType::Model || ev.AssetGuid.IsNull())
            return;
        switch (ev.EventType)
        {
            case AssetEventType::AssetModified:
            case AssetEventType::AssetReloaded:
            case AssetEventType::AssetDestroyed:
                // Forward AssetPath so the destroy handler can resolve the
                // disk cache directory after the GUID is erased from m_Assets.
                InvalidateCacheForAsset(ev.AssetGuid, ev.AssetPath);
                break;
            default:
                break;
        }
    });
    s_LastDispatcher.store(&dispatcher, std::memory_order_release);
}

// A terrain surface hit as a scene pick. Terrain carries no triangle identity, so the
// mesh-only fields stay at their "not applicable" defaults.
PickHit MakeTerrainPick(const TerrainPickHit& terrainHit)
{
    PickHit hit{};
    hit.Kind          = PickableKind::Terrain;
    hit.Entity        = terrainHit.Entity;
    hit.SubmeshIndex  = 0u;
    hit.TriangleIndex = ~0u;
    hit.Barycentrics  = Vector3{};
    hit.WorldPosition = terrainHit.WorldPosition;
    hit.WorldNormal   = terrainHit.WorldNormal;
    hit.Distance      = terrainHit.Distance;
    return hit;
}

}

PickResult RaycastScene(const Mathematics::Ray3D& ray,
                        GameEngine::ECS::World& world,
                        const PickOptions& options)
{
    GE_CPU_PROFILE_SCOPE("MeshPicking.RaycastScene");
    EnsureAssetReloadHookRegistered();

    PickResult result{};
    float32 bestT = std::min(options.MaxDistance, std::numeric_limits<float32>::max());

    // Restricted-entity path: single mesh, no terrain, no broad scan.
    if (world.IsValid(options.RestrictToEntity))
    {
        PickHit hit{};
        if (RaycastSingleEntity(ray, options.RestrictToEntity, world, bestT, options, hit)
            || RaycastProviderEntity(ray, options.RestrictToEntity, world, bestT, options, hit))
        {
            result.Hit  = true;
            result.Best = hit;
        }
        return result;
    }

    // Mesh broad-phase: O(log N) TLAS traversal. Per-triangle narrow-phase
    // is unchanged — TestEntityHit runs once per leaf hit.
    if (options.IncludeMeshes || options.IncludePrimitives)
    {
        auto& tlas = Scene::GetSceneTlas(world);
        if (options.WaitForSceneTlas)
            tlas.WaitCurrent(world); // blocks only when something changed since the last request

        Scene::TraverseRayOptions topts;
        topts.MaxDistance     = bestT;
        topts.LayerMask       = options.LayerMask;
        topts.IncludeDisabled = options.IncludeDisabled;
        topts.IgnoreEntities  = options.IgnoreEntities;

        const float32 sectorSize = tlas.GetSectorSize();
        auto cb = [&](const Scene::TlasInstance& inst, float32 tEnter, float32 /*tExit*/) -> bool {
            // Leaves are dispatched in entry-t order. Once the next leaf's
            // entry t exceeds our best confirmed hit distance, no further
            // leaf can yield a closer triangle hit — bail out.
            if (tEnter >= bestT)
                return false;

            const auto* worldXf = world.GetComponent<Components::WorldTransform>(inst.Entity);
            const auto* mr      = world.GetComponent<Components::MeshRenderer>(inst.Entity);
            if (!worldXf || !mr)
                return true;
            const auto* bounds  = world.GetComponent<Components::LocalBounds>(inst.Entity);
            const auto* sector  = world.GetComponent<Components::WorldSectorCoord>(inst.Entity);
            const Components::WorldTransform effectiveXf =
                Components::ComposeEffectiveWorldTransform(*worldXf, sector, sectorSize);
            PickHit hit;
            if (TestEntityHit(ray, inst.Entity, effectiveXf, *mr, bounds, options, bestT, hit))
            {
                bestT       = hit.Distance;
                result.Hit  = true;
                result.Best = hit;
            }
            return true;
        };
        tlas.TraverseRay(ray, topts, cb);
    }

    // Plugin-owned runtime meshes (registered pick providers): each provider
    // enumerates its pickable entities; the narrow-phase runs here.
    if (options.IncludeMeshes)
    {
        for (const EditorPickProvider& provider : EditorPickProviderRegistry::Get().Snapshot())
        {
            if (!provider.Enumerate)
                continue;
            provider.Enumerate(world, [&](ECS::EntityHandle entity, const SyntheticMeshPick& pick)
            {
                if (!world.IsValid(entity))
                    return;
                PickHit hit{};
                if (RaycastSyntheticPick(ray, entity, world, bestT, options, pick, hit))
                {
                    bestT = hit.Distance;
                    result.Hit = true;
                    result.Best = hit;
                }
            });
        }
    }

    if (options.IncludeTerrain)
    {
        TerrainPickHit terrainHit;
        if (RaycastTerrain(ray, world, bestT, terrainHit) && terrainHit.Distance < bestT)
        {
            bestT = terrainHit.Distance;
            result.Hit  = true;
            result.Best = MakeTerrainPick(terrainHit);
        }
    }

    return result;
}

void RaycastSceneAll(const Mathematics::Ray3D& ray,
                     GameEngine::ECS::World& world,
                     std::vector<PickHit>& outHits,
                     const PickOptions& options)
{
    GE_CPU_PROFILE_SCOPE("MeshPicking.RaycastSceneAll");
    EnsureAssetReloadHookRegistered();

    outHits.clear();

    const float32 maxT = std::min(options.MaxDistance, std::numeric_limits<float32>::max());

    if (options.IncludeMeshes || options.IncludePrimitives)
    {
        auto& tlas = Scene::GetSceneTlas(world);
        if (options.WaitForSceneTlas)
            tlas.WaitCurrent(world);

        Scene::TraverseRayOptions topts;
        topts.MaxDistance     = maxT;
        topts.LayerMask       = options.LayerMask;
        topts.IncludeDisabled = options.IncludeDisabled;
        topts.IgnoreEntities  = options.IgnoreEntities;

        const float32 sectorSize = tlas.GetSectorSize();
        auto cb = [&](const Scene::TlasInstance& inst, float32 /*tEnter*/, float32 /*tExit*/) -> bool {
            const auto* worldXf = world.GetComponent<Components::WorldTransform>(inst.Entity);
            const auto* mr      = world.GetComponent<Components::MeshRenderer>(inst.Entity);
            if (!worldXf || !mr)
                return true;
            const auto* bounds  = world.GetComponent<Components::LocalBounds>(inst.Entity);
            const auto* sector  = world.GetComponent<Components::WorldSectorCoord>(inst.Entity);
            const Components::WorldTransform effectiveXf =
                Components::ComposeEffectiveWorldTransform(*worldXf, sector, sectorSize);
            PickHit hit;
            if (TestEntityHit(ray, inst.Entity, effectiveXf, *mr, bounds, options, maxT, hit))
                outHits.push_back(hit);
            return true;  // collect ALL hits along the ray
        };
        tlas.TraverseRay(ray, topts, cb);
    }

    // Plugin-owned runtime meshes (registered pick providers).
    if (options.IncludeMeshes)
    {
        for (const EditorPickProvider& provider : EditorPickProviderRegistry::Get().Snapshot())
        {
            if (!provider.Enumerate)
                continue;
            provider.Enumerate(world, [&](ECS::EntityHandle entity, const SyntheticMeshPick& pick)
            {
                if (!world.IsValid(entity))
                    return;
                PickHit hit{};
                if (RaycastSyntheticPick(ray, entity, world, maxT, options, pick, hit))
                    outHits.push_back(hit);
            });
        }
    }

    // Terrain is one surface, so it contributes at most its nearest hit — enough for the
    // click-through stack to step past it onto whatever the ray reaches behind.
    if (options.IncludeTerrain)
    {
        TerrainPickHit terrainHit;
        if (RaycastTerrain(ray, world, maxT, terrainHit))
            outHits.push_back(MakeTerrainPick(terrainHit));
    }

    std::sort(outHits.begin(), outHits.end(), [](const PickHit& a, const PickHit& b) {
        return a.Distance < b.Distance;
    });
}

void InvalidateCacheForAsset(const GUID& modelGuid,
                             const std::filesystem::path& assetPath)
{
    MeshBvhCache::Instance().InvalidateAsset(modelGuid, assetPath);
}

void ClearCache()
{
    MeshBvhCache::Instance().Clear();
}

}
