#include "Engine/Rendering/SceneResolveService.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace GameEngine::Engine::Renderer
{

// One model and the entities held for it. Erased once its entities are
// consumed; the registration outlives it in m_Kept.
struct SceneResolveService::ModelWork
{
    // The load in flight; null once it has landed, and for a model that was
    // resident when the first entity named it.
    std::unique_ptr<AssetFuture> Load;
    // The landed asset, held while entities wait for it: the asset stays
    // resident until the entities that waited for it are bound.
    SharedPtr<Asset> LandedAsset;
    bool Missed = false;
    // Held entities in arrival order. A record is live while m_Held still names
    // this model for its entity; a record left behind by a move to another
    // model, a drop or a sweep is skipped when reached.
    std::vector<ECS::EntityHandle> Entities;
    std::size_t Next = 0;
};

// One standalone material requested and not registered yet.
struct SceneResolveService::MaterialWork
{
    std::unique_ptr<AssetFuture> Load;
    SharedPtr<Asset> LandedAsset;
    std::vector<SceneResolveBatch> Batches;
};

// A model registration kept for later spawns. The asset is referenced weakly:
// the asset manager decides residency, and a kept registration whose asset has
// left memory is stale.
struct SceneResolveService::KeptModel
{
    std::weak_ptr<Asset> Source;
    ModelRenderResources Resources;
    // The check window (SceneResolveService::m_Steps) in which the
    // registration was last found current.
    uint64 CheckedAtStep = 0;
};

namespace
{

// Held-entity buckets the sweep for destroyed entities visits per step: a
// bounded cost on every frame that changes the world structurally while
// entities are held, whatever the size of a held scene.
constexpr std::size_t kSweepBucketsPerStep = 1024;

AssetManager* RuntimeAssets()
{
    return EngineCore::GetInstance().TryGetAssetManager();
}

bool IsMaterialRegistered(RenderServices& renderServices, const GUID& materialGuid)
{
    return renderServices.Materials().Registry().Find(materialGuid) != nullptr;
}

bool CarriesSkeletonBinding(const ECS::World& world, ECS::EntityHandle entity)
{
    return world.GetComponent<Components::SkeletonRef>(entity) != nullptr
        || world.GetComponent<Components::SkinnedMeshRenderer>(entity) != nullptr;
}

bool NeedsModel(const Components::MeshRenderer& meshRenderer)
{
    return meshRenderer.meshGpuHandleId == 0 && !meshRenderer.modelAssetGuid.IsNull();
}

bool MeshHandlesResolve(RenderServices& renderServices, const ModelRenderResources& resources)
{
    const MeshGPURegistry& meshes = renderServices.GetMeshGPURegistry();
    return std::all_of(resources.meshHandles.begin(), resources.meshHandles.end(),
                       [&](Rendering::MeshGPUHandle handle) { return meshes.Find(handle) != nullptr; });
}

// A kept registration is current while the asset manager still holds the asset
// it was made from and its mesh handles resolve. An unload is seen here before
// its invalidation reaches EvictModels (the manager drops the asset first).
bool IsKeptCurrent(RenderServices& renderServices, const GUID& modelGuid, const SharedPtr<Asset>& source,
                   const ModelRenderResources& resources)
{
    AssetManager* assets = RuntimeAssets();
    return assets && assets->GetAsset(modelGuid) == source && MeshHandlesResolve(renderServices, resources);
}

// A landed future never blocks: wait_for(0) has said it is ready.
bool HasLanded(AssetFuture& load)
{
    return !load.Valid() || load.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

SharedPtr<Asset> TakeLanded(AssetFuture& load, const GUID& assetGuid)
{
    if (load.Valid())
        return load.get();
    AssetManager* assets = RuntimeAssets();
    return assets ? assets->GetAsset(assetGuid) : nullptr;
}

// Restores the re-entrancy guard however the step ends: registration runs ECS
// hooks, and a hook that throws must not leave the service refusing every step.
class StepScope
{
  public:
    explicit StepScope(bool& inStep) : m_InStep(inStep) { m_InStep = true; }
    ~StepScope() { m_InStep = false; }
    StepScope(const StepScope&) = delete;
    StepScope& operator=(const StepScope&) = delete;

  private:
    bool& m_InStep;
};

} // namespace

SceneResolveService::SceneResolveService() = default;

// Out of line: the work records are complete types only here.
SceneResolveService::~SceneResolveService() = default;

void SceneResolveService::Reset()
{
    DropHeldWork();
    m_Materials.clear();
    m_LoadingMaterials.clear();
    m_ReadyMaterials.clear();
    m_Batches.clear();
    m_Kept.clear();
    m_HasWorld = false;
    m_ReportedMisses.clear();
    m_Counts = {};
}

void SceneResolveService::DropHeldWork()
{
    m_Models.clear();
    m_LoadingModels.clear();
    m_ReadyModels.clear();
    m_Held.clear();
    m_Sweeping = false;
    // The dropped entities are gone with their world: their batches are done.
    for (auto& [id, batch] : m_Batches)
        batch.Done = batch.Total;
    for (auto& [guid, material] : m_Materials)
        material->Batches.clear();
}

void SceneResolveService::ObserveWorld(const ECS::World& world)
{
    const uint64 worldId = world.GetWorldId();
    const uint64 generation = world.GetLifecycleResetGeneration();
    if (m_HasWorld && worldId == m_WorldId && generation == m_WorldGeneration)
        return;
    DropHeldWork();
    m_WorldId = worldId;
    m_WorldGeneration = generation;
    m_SweptStructuralVersion = world.GetStructuralChangeVersion();
    m_HasWorld = true;
}

void SceneResolveService::SweepGoneEntities(const ECS::World& world)
{
    const std::size_t version = world.GetStructuralChangeVersion();
    if (m_Held.empty())
    {
        m_Sweeping = false;
        m_SweptStructuralVersion = version;
        return;
    }
    if (!m_Sweeping)
    {
        if (version == m_SweptStructuralVersion)
            return;
        m_Sweeping = true;
        m_SweepStartVersion = version;
        m_SweepBucket = 0;
    }

    std::vector<std::pair<ECS::EntityHandle, SceneResolveBatch>> gone;
    const std::size_t lastBucket = std::min(m_Held.bucket_count(), m_SweepBucket + kSweepBucketsPerStep);
    for (; m_SweepBucket < lastBucket; ++m_SweepBucket)
        for (auto it = m_Held.cbegin(m_SweepBucket); it != m_Held.cend(m_SweepBucket); ++it)
            if (!world.IsValid(it->first) || !world.GetComponent<Components::MeshRenderer>(it->first))
                gone.emplace_back(it->first, it->second.Batch);
    for (const auto& [entity, batch] : gone)
    {
        // Erasing never rehashes, so the bucket cursor stays meaningful.
        m_Held.erase(entity);
        ReleaseEntity(world, entity, batch, false);
    }
    if (m_SweepBucket >= m_Held.bucket_count())
    {
        // A pass is complete. A change made while it ran (an insert can rehash
        // under the cursor) moved the version on, so the next step starts another.
        m_Sweeping = false;
        m_SweptStructuralVersion = m_SweepStartVersion;
    }
}

void SceneResolveService::Enqueue(ECS::World& world, RenderServices& renderServices, ECS::EntityHandle entity)
{
    ObserveWorld(world);
    const auto* meshRenderer = world.GetComponent<Components::MeshRenderer>(entity);
    if (!meshRenderer)
        return;
    if (!NeedsModel(*meshRenderer))
    {
        RequestMaterial(renderServices, meshRenderer->materialAssetGuid.ToGuid(), SceneResolveBatch::None);
        return;
    }
    const GUID modelGuid = meshRenderer->modelAssetGuid.ToGuid();
    if (!m_Held.contains(entity) && BindAtEnqueue(world, renderServices, entity, modelGuid))
        return;
    Hold(entity, modelGuid, SceneResolveBatch::None);
}

SceneResolveBatch SceneResolveService::EnqueueWorld(ECS::World& world, RenderServices& renderServices)
{
    ObserveWorld(world);
    const auto batch = static_cast<SceneResolveBatch>(m_NextBatch++);
    m_Batches.emplace(static_cast<uint32>(batch), BatchState{});
    // Asset resolution binds handles for the scene as authored: an entity that
    // is switched off still needs its mesh and material when it comes back.
    world.Query<ECS::Read<Components::MeshRenderer>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle entity, const Components::MeshRenderer& meshRenderer)
        {
            if (NeedsModel(meshRenderer))
                Hold(entity, meshRenderer.modelAssetGuid.ToGuid(), batch);
            else
                RequestMaterial(renderServices, meshRenderer.materialAssetGuid.ToGuid(), batch);
        });
    return batch;
}

void SceneResolveService::Hold(ECS::EntityHandle entity, const GUID& modelGuid, SceneResolveBatch batch)
{
    auto [held, inserted] = m_Held.try_emplace(entity, HeldEntity{modelGuid, batch});
    if (inserted)
    {
        AddBatchItem(batch);
        AddModelRecord(entity, modelGuid);
        return;
    }
    // Held already: the entity is read again when it resolves, so a later change
    // needs no new record, except a batch naming an entity handed over without
    // one, and a new model (the old one may be slower to land, or never).
    if (held->second.Batch == SceneResolveBatch::None && batch != SceneResolveBatch::None)
    {
        held->second.Batch = batch;
        AddBatchItem(batch);
    }
    if (held->second.Model != modelGuid)
    {
        held->second.Model = modelGuid;
        AddModelRecord(entity, modelGuid);
    }
}

void SceneResolveService::AddModelRecord(ECS::EntityHandle entity, const GUID& modelGuid)
{
    auto [found, created] = m_Models.try_emplace(modelGuid);
    if (created)
    {
        found->second = std::make_unique<ModelWork>();
        AssetManager* assets = RuntimeAssets();
        if (assets)
            found->second->LandedAsset = assets->GetAsset(modelGuid);
        if (found->second->LandedAsset || !assets)
        {
            // Resident (or no asset manager to load with: a miss, reported
            // nowhere, as a headless resolve has always treated it).
            found->second->Missed = !found->second->LandedAsset;
            m_ReadyModels.push_back(modelGuid);
        }
        else
        {
            found->second->Load = std::make_unique<AssetFuture>(assets->LoadAssetAsync(modelGuid));
            m_LoadingModels.push_back(modelGuid);
        }
    }
    // A model's work is either loading (in m_LoadingModels) or landed (queued
    // in m_ReadyModels until its entities are consumed and it is erased), so a
    // record added here is always reached.
    found->second->Entities.push_back(entity);
}

SceneResolveService::KeptBinding SceneResolveService::FindKept(RenderServices& renderServices,
                                                               const GUID& modelGuid)
{
    if (auto found = m_Kept.find(modelGuid); found != m_Kept.end())
    {
        KeptModel& model = *found->second;
        KeptBinding kept{model.Source.lock(), &model.Resources};
        if (kept.Pin && (model.CheckedAtStep == m_Steps
                         || IsKeptCurrent(renderServices, modelGuid, kept.Pin, model.Resources)))
        {
            model.CheckedAtStep = m_Steps;
            return kept;
        }
        m_Kept.erase(found);
    }
    // Registered by another owner (an authoring tool, the editor's build, this
    // service before an eviction that left the meshes in place): a resident
    // model whose meshes are registered is kept now. Its registration only
    // re-registers the materials, as a spawn of it always has.
    AssetManager* assets = RuntimeAssets();
    if (!assets)
        return {};
    SharedPtr<Asset> resident = assets->GetAsset(modelGuid);
    if (!resident || renderServices.GetMeshGPURegistry().GetModelHandles(modelGuid).empty())
        return {};
    return Keep(renderServices, modelGuid, resident);
}

SceneResolveService::KeptBinding SceneResolveService::Keep(RenderServices& renderServices, const GUID& modelGuid,
                                                           const SharedPtr<Asset>& asset)
{
    // Mesh upload, material conversion, registration and the compile prewarm.
    std::optional<ModelRenderResources> resources = RegisterLoadedModelRenderResources(renderServices, modelGuid, *asset);
    if (!resources)
        return {};
    ++m_Counts.RegisteredModels;
    auto kept = std::make_unique<KeptModel>();
    kept->Source = asset;
    kept->Resources = std::move(*resources);
    kept->CheckedAtStep = m_Steps;
    const ModelRenderResources* stored = &kept->Resources;
    m_Kept.insert_or_assign(modelGuid, std::move(kept));
    return {asset, stored};
}

bool SceneResolveService::BindAtEnqueue(ECS::World& world, RenderServices& renderServices, ECS::EntityHandle entity,
                                        const GUID& modelGuid)
{
    const KeptBinding kept = FindKept(renderServices, modelGuid);
    if (!kept)
        return false;
    ++m_Counts.BoundAtEnqueue;
    BindEntity(world, renderServices, entity, *kept.Resources, SceneResolveBatch::None);
    return true;
}

void SceneResolveService::BindEntity(ECS::World& world, RenderServices& renderServices, ECS::EntityHandle entity,
                                     const ModelRenderResources& resources, SceneResolveBatch batch)
{
    ApplyModelRenderResources(world, renderServices, entity, resources);
    ++m_Counts.ResolvedEntities;
    // After the model registered its own materials, so an embedded one is not
    // requested as a standalone .material.
    if (const auto* resolved = world.GetComponent<Components::MeshRenderer>(entity))
        RequestMaterial(renderServices, resolved->materialAssetGuid.ToGuid(), batch);
}

void SceneResolveService::EvictModels(const std::vector<GUID>& invalidatedModels)
{
    for (const GUID& modelGuid : invalidatedModels)
    {
        m_Kept.erase(modelGuid);
        m_ReportedMisses.erase(modelGuid);
        auto found = m_Models.find(modelGuid);
        if (found == m_Models.end() || found->second->Load)
            continue;
        // Entities still held for a landed model bind against the asset now
        // resident; one that left memory loads again.
        ModelWork& work = *found->second;
        AssetManager* assets = RuntimeAssets();
        work.LandedAsset = assets ? assets->GetAsset(modelGuid) : nullptr;
        work.Missed = !assets;
        if (assets && !work.LandedAsset)
        {
            work.Load = std::make_unique<AssetFuture>(assets->LoadAssetAsync(modelGuid));
            m_LoadingModels.push_back(modelGuid);
        }
    }
}

void SceneResolveService::RequestMaterial(RenderServices& renderServices, const GUID& materialGuid,
                                          SceneResolveBatch batch)
{
    if (materialGuid.IsNull() || IsMaterialRegistered(renderServices, materialGuid))
        return;
    auto [found, created] = m_Materials.try_emplace(materialGuid);
    if (created)
    {
        AssetManager* assets = RuntimeAssets();
        if (!assets)
        {
            m_Materials.erase(found);
            return;
        }
        found->second = std::make_unique<MaterialWork>();
        found->second->LandedAsset = assets->GetAsset(materialGuid);
        if (found->second->LandedAsset)
        {
            m_ReadyMaterials.push_back(materialGuid);
        }
        else
        {
            found->second->Load = std::make_unique<AssetFuture>(assets->LoadAssetAsync(materialGuid));
            m_LoadingMaterials.push_back(materialGuid);
        }
    }
    std::vector<SceneResolveBatch>& batches = found->second->Batches;
    if (batch != SceneResolveBatch::None && std::find(batches.begin(), batches.end(), batch) == batches.end())
    {
        batches.push_back(batch);
        AddBatchItem(batch);
    }
}

void SceneResolveService::PollModelLoads()
{
    for (std::size_t i = 0; i < m_LoadingModels.size();)
    {
        const GUID modelGuid = m_LoadingModels[i];
        auto found = m_Models.find(modelGuid);
        if (found != m_Models.end() && found->second->Load && !HasLanded(*found->second->Load))
        {
            ++i;
            continue;
        }
        m_LoadingModels[i] = m_LoadingModels.back();
        m_LoadingModels.pop_back();
        if (found == m_Models.end() || !found->second->Load)
            continue;
        ModelWork& work = *found->second;
        work.LandedAsset = TakeLanded(*work.Load, modelGuid);
        work.Load.reset();
        work.Missed = !work.LandedAsset;
        if (work.Missed)
            ReportMiss("model", modelGuid);
        m_ReadyModels.push_back(modelGuid);
    }
}

void SceneResolveService::PollMaterialLoads()
{
    for (std::size_t i = 0; i < m_LoadingMaterials.size();)
    {
        const GUID materialGuid = m_LoadingMaterials[i];
        auto found = m_Materials.find(materialGuid);
        if (found != m_Materials.end() && !HasLanded(*found->second->Load))
        {
            ++i;
            continue;
        }
        m_LoadingMaterials[i] = m_LoadingMaterials.back();
        m_LoadingMaterials.pop_back();
        if (found == m_Materials.end())
            continue;
        MaterialWork& work = *found->second;
        work.LandedAsset = TakeLanded(*work.Load, materialGuid);
        work.Load.reset();
        m_ReadyMaterials.push_back(materialGuid);
    }
}

std::size_t SceneResolveService::Step(ECS::World& world, RenderServices& renderServices,
                                      std::chrono::microseconds budget)
{
    return Run(world, renderServices, std::chrono::steady_clock::now() + budget);
}

std::size_t SceneResolveService::Run(ECS::World& world, RenderServices& renderServices,
                                     std::chrono::steady_clock::time_point deadline)
{
    if (m_InStep)
        return 0;
    StepScope scope(m_InStep);
    ++m_Steps;
    ObserveWorld(world);
    SweepGoneEntities(world);
    PollModelLoads();
    PollMaterialLoads();

    std::size_t taken = 0;
    do
    {
        if (!ResolveNextEntity(world, renderServices) && !RegisterNextMaterial(renderServices))
            break;
        ++taken;
    } while (std::chrono::steady_clock::now() < deadline);
    ++m_Steps;
    return taken;
}

bool SceneResolveService::ResolveNextEntity(ECS::World& world, RenderServices& renderServices)
{
    while (!m_ReadyModels.empty())
    {
        const GUID modelGuid = m_ReadyModels.front();
        auto found = m_Models.find(modelGuid);
        if (found == m_Models.end() || found->second->Load)
        {
            // Erased, or loading again after an eviction (queued again when it lands).
            m_ReadyModels.pop_front();
            continue;
        }
        ModelWork& work = *found->second;
        if (work.Next >= work.Entities.size())
        {
            m_ReadyModels.pop_front();
            m_Models.erase(found);
            continue;
        }
        const ECS::EntityHandle entity = work.Entities[work.Next++];
        auto held = m_Held.find(entity);
        if (held == m_Held.end() || held->second.Model != modelGuid)
            continue; // a record left behind: the entity moved, resolved or was dropped
        const SceneResolveBatch batch = held->second.Batch;
        m_Held.erase(held);
        ResolveHeldEntity(world, renderServices, modelGuid, work, entity, batch);
        return true;
    }
    return false;
}

void SceneResolveService::ResolveHeldEntity(ECS::World& world, RenderServices& renderServices,
                                            const GUID& modelGuid, ModelWork& work, ECS::EntityHandle entity,
                                            SceneResolveBatch batch)
{
    const auto* meshRenderer = world.IsValid(entity) ? world.GetComponent<Components::MeshRenderer>(entity) : nullptr;
    if (!meshRenderer)
    {
        ReleaseEntity(world, entity, batch, false);
        return;
    }
    if (!NeedsModel(*meshRenderer))
    {
        // Resolved by another owner meanwhile, or it no longer names a model.
        RequestMaterial(renderServices, meshRenderer->materialAssetGuid.ToGuid(), batch);
        ReleaseEntity(world, entity, batch, false);
        return;
    }
    const GUID currentModel = meshRenderer->modelAssetGuid.ToGuid();
    if (currentModel != modelGuid)
    {
        // Its model changed while it was held: it waits for the new one, in its batch.
        m_Held.emplace(entity, HeldEntity{currentModel, batch});
        AddModelRecord(entity, currentModel);
        return;
    }

    KeptBinding kept;
    if (!work.Missed)
    {
        kept = FindKept(renderServices, modelGuid);
        // The first entity of a model that had to land pays for its registration.
        if (!kept && work.LandedAsset)
            kept = Keep(renderServices, modelGuid, work.LandedAsset);
        work.Missed = !kept;
    }
    if (!kept)
    {
        ++m_Counts.MissedEntities;
        RequestMaterial(renderServices, meshRenderer->materialAssetGuid.ToGuid(), batch);
        ReleaseEntity(world, entity, batch, false);
        return;
    }
    BindEntity(world, renderServices, entity, *kept.Resources, batch);
    ReleaseEntity(world, entity, batch, true);
}

bool SceneResolveService::RegisterNextMaterial(RenderServices& renderServices)
{
    while (!m_ReadyMaterials.empty())
    {
        const GUID materialGuid = m_ReadyMaterials.front();
        m_ReadyMaterials.pop_front();
        auto found = m_Materials.find(materialGuid);
        if (found == m_Materials.end())
            continue;
        MaterialWork& work = *found->second;
        if (!work.LandedAsset)
        {
            ++m_Counts.MissedMaterials;
            ReportMiss("material", materialGuid);
        }
        else if (!IsMaterialRegistered(renderServices, materialGuid)
                 && RegisterLoadedStandaloneMaterial(renderServices, materialGuid, work.LandedAsset.get()))
        {
            ++m_Counts.RegisteredMaterials;
        }
        for (SceneResolveBatch batch : work.Batches)
            FinishBatchItem(batch);
        m_Materials.erase(found);
        return true;
    }
    return false;
}

void SceneResolveService::ReleaseEntity(const ECS::World& world, ECS::EntityHandle entity, SceneResolveBatch batch,
                                        bool wroteMeshRenderer)
{
    if (!wroteMeshRenderer && world.IsValid(entity) && CarriesSkeletonBinding(world, entity))
        ++m_HandBackGeneration;
    FinishBatchItem(batch);
}

void SceneResolveService::ReportMiss(const char* kind, const GUID& assetGuid)
{
    if (!m_ReportedMisses.insert(assetGuid).second)
        return;
    std::string path = "no registry record";
    AssetMetadata metadata{};
    if (AssetManager* assets = RuntimeAssets(); assets && assets->GetRegistry().TryGetAssetMetadata(assetGuid, metadata))
        path = metadata.Path.generic_string();
    Logger::Log::Warning("SceneResolveService: failed to load asset {} ({} '{}'): what references it stays unresolved",
                         assetGuid.ToString(), kind, path);
}

void SceneResolveService::AddBatchItem(SceneResolveBatch batch)
{
    if (batch == SceneResolveBatch::None)
        return;
    if (auto found = m_Batches.find(static_cast<uint32>(batch)); found != m_Batches.end())
        ++found->second.Total;
}

void SceneResolveService::FinishBatchItem(SceneResolveBatch batch)
{
    if (batch == SceneResolveBatch::None)
        return;
    if (auto found = m_Batches.find(static_cast<uint32>(batch)); found != m_Batches.end()
        && found->second.Done < found->second.Total)
        ++found->second.Done;
}

std::size_t SceneResolveService::DrainNow(ECS::World& world, RenderServices& renderServices)
{
    std::size_t taken = 0;
    do
    {
        taken += Run(world, renderServices, std::chrono::steady_clock::time_point::max());
    } while (!m_InStep && WaitForOneLoad());
    return taken;
}

bool SceneResolveService::WaitForOneLoad()
{
    // The one blocking wait in the service, reached only from DrainNow.
    for (const GUID& modelGuid : m_LoadingModels)
    {
        auto found = m_Models.find(modelGuid);
        if (found != m_Models.end() && found->second->Load && found->second->Load->Valid())
        {
            (void)found->second->Load->get();
            return true;
        }
    }
    for (const GUID& materialGuid : m_LoadingMaterials)
    {
        auto found = m_Materials.find(materialGuid);
        if (found != m_Materials.end() && found->second->Load && found->second->Load->Valid())
        {
            (void)found->second->Load->get();
            return true;
        }
    }
    return !m_LoadingModels.empty() || !m_LoadingMaterials.empty();
}

bool SceneResolveService::IsResolving(const ECS::World& world, ECS::EntityHandle entity) const
{
    return m_HasWorld && world.GetWorldId() == m_WorldId && world.GetLifecycleResetGeneration() == m_WorldGeneration
        && m_Held.contains(entity);
}

bool SceneResolveService::IsBatchComplete(SceneResolveBatch batch) const
{
    return BatchProgress(batch).Complete;
}

SceneResolveBatchProgress SceneResolveService::BatchProgress(SceneResolveBatch batch) const
{
    auto found = m_Batches.find(static_cast<uint32>(batch));
    if (found == m_Batches.end())
        return {};
    return {found->second.Total, found->second.Done, found->second.Done >= found->second.Total};
}

SceneResolveCounts SceneResolveService::Counts() const
{
    SceneResolveCounts counts = m_Counts;
    counts.HeldEntities = m_Held.size();
    counts.PendingMaterials = m_Materials.size();
    counts.KeptModels = m_Kept.size();
    return counts;
}

} // namespace GameEngine::Engine::Renderer
