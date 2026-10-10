#pragma once

// SceneResolveService: the engine's persistent, non-waiting model and material
// resolve. A MeshRenderer that names a model but has no GPU mesh yet (a scene
// just loaded, an entity spawned during play) is handed to the service, which
// binds it to its model's registered meshes and materials. The standalone
// .material assets those entities reference are requested together and each is
// registered once it lands.
//
// One engine instance lives for the session in the RenderingLoop: every frame
// BindChangedMeshRenderers enqueues what changed and the loop steps the service
// for its frame slice (RenderingLoop::kResolveFrameSlice). The editor's
// scene-open build (SceneBuildPump) drives an instance of its own.
//
// The contract of an object that outlives scenes, models and entities:
//
// - Spawns of registered models bind at once. Enqueue binds an entity in the
//   call when its model is already registered (resident, its GPU meshes
//   registered): that first pass runs at enqueue, outside the frame slice, so
//   a burst of ordinary spawns (projectiles, debris, a wave of units) binds in
//   its spawn frame. Every other entity is held: its model's load starts at
//   once, and the entity binds on a later Step once the model has landed. The
//   slice bounds only models that had to land, and scene batches (EnqueueWorld).
// - Held work never outlives its world or its entity. A held entity that is
//   destroyed or loses its MeshRenderer is dropped by a bounded sweep that runs
//   while the world changes structurally, and its batch counts it done. All held
//   work and its batches are dropped when the world changes or its lifecycle
//   reset generation moves (World::Clear): a held handle could alias a new
//   entity. Loads already started are not cancelled; they land in the asset
//   manager's cache.
// - Kept registrations follow their asset. The registration of a model (its
//   mesh handles and converted materials) is kept across batches so later
//   spawns bind at enqueue. It is dropped when the model is invalidated
//   (EvictModels: reloaded in place or unloaded) and when it is found stale on
//   its first use before and in each step: the asset manager no longer holds
//   the asset it was made from, or its mesh handles stop resolving. A spawn made
//   between an unload and its invalidation therefore loads the model again
//   instead of binding to meshes about to be released. The next entity naming
//   the model registers it again from the asset then resident. A kept
//   registration never holds its model resident.
// - Misses are reported. A model or material that fails to load is named by
//   GUID and path in the log once and counted; what references it stays
//   unresolved and nothing waits for it.
// - No call waits for a load except DrainNow.
// - No global state: an instance's work never affects another instance or
//   IsSceneBuildPumpActive().
//
// Main thread only: registration runs on the material system's owner thread.

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Types/Types.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
class Asset;
namespace ECS { class World; }

namespace Engine::Renderer
{
class RenderServices;
struct ModelRenderResources;

// The work one EnqueueWorld call handed over, so its caller can ask when that
// work is done. None names no batch.
enum class SceneResolveBatch : uint32
{
    None = 0
};

// A batch's items: its entities, then the standalone materials they reference.
// Total grows while the batch's entities name materials not yet registered.
struct SceneResolveBatchProgress
{
    std::size_t Total = 0;
    std::size_t Done = 0;
    bool Complete = true;
};

struct SceneResolveCounts
{
    // Now: entities waiting for their model, materials not yet registered, and
    // model registrations kept for later spawns.
    std::size_t HeldEntities = 0;
    std::size_t PendingMaterials = 0;
    std::size_t KeptModels = 0;
    // Session totals.
    uint64 ResolvedEntities = 0;
    uint64 BoundAtEnqueue = 0;   // of ResolvedEntities: bound in their Enqueue call
    uint64 MissedEntities = 0;   // their model failed to load, or is not a model
    uint64 RegisteredModels = 0; // registrations made, a re-registration after an eviction included
    uint64 RegisteredMaterials = 0;
    uint64 MissedMaterials = 0;  // failed to load
};

class SceneResolveService
{
  public:
    SceneResolveService();
    ~SceneResolveService();
    SceneResolveService(const SceneResolveService&) = delete;
    SceneResolveService& operator=(const SceneResolveService&) = delete;

    /// Hand over one entity whose MeshRenderer changed. One that names a model
    /// but has no GPU mesh is bound now when its model is registered, and held
    /// until its model lands otherwise; its standalone material is requested when
    /// it is not registered yet. Never waits.
    void Enqueue(ECS::World& world, RenderServices& renderServices, ECS::EntityHandle entity);

    /// Every unresolved MeshRenderer of `world`, disabled ones included, as one
    /// batch: a freshly loaded scene. Every entity is held, so the batch resolves
    /// in Step's slices. A world with nothing to resolve gives a batch that is
    /// already complete.
    SceneResolveBatch EnqueueWorld(ECS::World& world, RenderServices& renderServices);

    /// Bind what has landed: held entities whose model is resident, then the
    /// materials that have loaded. The budget is checked between items and a
    /// step always takes one item that is ready, so a heavy model (its mesh
    /// upload and material registration) overshoots it by that model's cost.
    /// Never waits for a load. Returns the items it took.
    std::size_t Step(ECS::World& world, RenderServices& renderServices, std::chrono::microseconds budget);

    /// Resolve everything held now, waiting for every load still in flight. The
    /// only waiting form, for the editor's undo snapshot, which must not capture
    /// a world mid-resolve. Never on a frame path.
    std::size_t DrainNow(ECS::World& world, RenderServices& renderServices);

    /// Drop the kept registrations of models whose asset was invalidated
    /// (reloaded in place or unloaded; RenderServices::TakeModelsInvalidated).
    /// Entities still held for one of them bind against the asset then resident,
    /// or wait for it to load again.
    void EvictModels(const std::vector<GUID>& invalidatedModels);

    /// True from an entity's enqueue until the service writes its resolved
    /// MeshRenderer or records its miss. The skeleton binder skips these.
    bool IsResolving(const ECS::World& world, ECS::EntityHandle entity) const;

    bool IsBatchComplete(SceneResolveBatch batch) const;
    SceneResolveBatchProgress BatchProgress(SceneResolveBatch batch) const;
    SceneResolveCounts Counts() const;

    /// Bumped each time the service lets go of a held entity that carries a
    /// SkeletonRef or SkinnedMeshRenderer without writing its MeshRenderer (its
    /// model missed, or another owner resolved it first), so a binder that
    /// skipped the entity while it was held looks again.
    uint64 HandBackGeneration() const { return m_HandBackGeneration; }

    /// Drop all work, batches and kept registrations (a replace-open cancels a
    /// build before clearing the world the held entity handles point into).
    void Reset();

  private:
    struct HeldEntity
    {
        GUID Model;
        SceneResolveBatch Batch = SceneResolveBatch::None;
    };
    struct ModelWork;
    struct MaterialWork;
    struct KeptModel;
    // A kept registration found valid, and the asset its resources point into,
    // held for as long as the caller uses them.
    struct KeptBinding
    {
        SharedPtr<Asset> Pin;
        const ModelRenderResources* Resources = nullptr;
        explicit operator bool() const { return Resources != nullptr; }
    };
    struct BatchState
    {
        std::size_t Total = 0;
        std::size_t Done = 0;
    };

    void ObserveWorld(const ECS::World& world);
    void DropHeldWork();
    void SweepGoneEntities(const ECS::World& world);
    void Hold(ECS::EntityHandle entity, const GUID& modelGuid, SceneResolveBatch batch);
    void AddModelRecord(ECS::EntityHandle entity, const GUID& modelGuid);
    bool BindAtEnqueue(ECS::World& world, RenderServices& renderServices, ECS::EntityHandle entity,
                       const GUID& modelGuid);
    KeptBinding FindKept(RenderServices& renderServices, const GUID& modelGuid);
    KeptBinding Keep(RenderServices& renderServices, const GUID& modelGuid, const SharedPtr<Asset>& asset);
    void BindEntity(ECS::World& world, RenderServices& renderServices, ECS::EntityHandle entity,
                    const ModelRenderResources& resources, SceneResolveBatch batch);
    void RequestMaterial(RenderServices& renderServices, const GUID& materialGuid, SceneResolveBatch batch);
    void PollModelLoads();
    void PollMaterialLoads();
    std::size_t Run(ECS::World& world, RenderServices& renderServices,
                    std::chrono::steady_clock::time_point deadline);
    bool ResolveNextEntity(ECS::World& world, RenderServices& renderServices);
    void ResolveHeldEntity(ECS::World& world, RenderServices& renderServices, const GUID& modelGuid,
                           ModelWork& work, ECS::EntityHandle entity, SceneResolveBatch batch);
    bool RegisterNextMaterial(RenderServices& renderServices);
    void ReleaseEntity(const ECS::World& world, ECS::EntityHandle entity, SceneResolveBatch batch,
                       bool wroteMeshRenderer);
    void ReportMiss(const char* kind, const GUID& assetGuid);
    void AddBatchItem(SceneResolveBatch batch);
    void FinishBatchItem(SceneResolveBatch batch);
    bool WaitForOneLoad();

    std::unordered_map<GUID, std::unique_ptr<ModelWork>> m_Models;
    std::vector<GUID> m_LoadingModels;
    std::deque<GUID> m_ReadyModels;
    // Each held entity, the model it waits for and its batch.
    std::unordered_map<ECS::EntityHandle, HeldEntity, ECS::EntityHandleHash> m_Held;

    std::unordered_map<GUID, std::unique_ptr<KeptModel>> m_Kept;

    std::unordered_map<GUID, std::unique_ptr<MaterialWork>> m_Materials;
    std::vector<GUID> m_LoadingMaterials;
    std::deque<GUID> m_ReadyMaterials;

    std::unordered_map<uint32, BatchState> m_Batches;
    uint32 m_NextBatch = 1;

    // The world the held handles belong to; another world, or this one after a
    // clear, drops them (a stale handle could alias a new entity).
    uint64 m_WorldId = 0;
    uint64 m_WorldGeneration = 0;
    bool m_HasWorld = false;

    // The sweep for held entities that no longer exist: it walks m_Held's
    // buckets a bounded number per step while the world has changed
    // structurally since the last complete pass.
    std::size_t m_SweptStructuralVersion = 0;
    std::size_t m_SweepStartVersion = 0;
    std::size_t m_SweepBucket = 0;
    bool m_Sweeping = false;

    // Assets whose miss has been logged: a missing model is named once, however
    // many spawns ask for it, until it is invalidated.
    std::unordered_set<GUID> m_ReportedMisses;
    // The session totals; the held, pending and kept counts are the containers' sizes.
    SceneResolveCounts m_Counts;
    uint64 m_HandBackGeneration = 0;
    // Check windows: a kept registration is checked against the asset manager
    // and the mesh registry on its first use in each window. A window ends where
    // a step begins and where it ends, so the enqueues of a frame and its step
    // each check once.
    uint64 m_Steps = 0;
    // Re-entrancy guard: registration mutates the world and must not re-enter.
    bool m_InStep = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
