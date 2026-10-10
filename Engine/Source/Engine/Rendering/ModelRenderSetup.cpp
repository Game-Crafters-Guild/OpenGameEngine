#include "Engine/Rendering/ModelRenderSetup.h"

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Name.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MeshNameRegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "Engine/Rendering/SceneResolveService.h"
#include "Engine/Rendering/SkeletonResolveState.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{

namespace
{

// Sanity-checks a loaded model and fills `out` with modelGuid, modelAsset,
// meshHandles and the converted-material list (no GPU registration of
// materials yet). Returns the ModelAsset* (or nullptr after a warning).
ModelAsset* ConvertLoadedModel(RenderServices& rs, const GUID& modelGuid, Asset* asset,
                               ModelRenderResources& out)
{
    if (!asset)
    {
        Logger::Log::Warning("ModelRenderSetup: failed to load asset {}", modelGuid.ToString());
        return nullptr;
    }

    auto* modelAsset = dynamic_cast<ModelAsset*>(asset);
    if (!modelAsset)
    {
        Logger::Log::Warning("ModelRenderSetup: asset {} is not a ModelAsset", modelGuid.ToString());
        return nullptr;
    }
    if (!modelAsset->IsLoaded())
    {
        Logger::Log::Warning("ModelRenderSetup: model asset {} is not loaded", modelGuid.ToString());
        return nullptr;
    }

    out.modelGuid = modelGuid;
    out.modelAsset = modelAsset;
    out.meshHandles = rs.GetMeshGPURegistry().RegisterModelMeshes(modelGuid, *modelAsset);
    out.materials = ModelMaterialBridge::ConvertAll(modelGuid, modelAsset->GetMaterials());
    return modelAsset;
}

// The waiting load: a model that is not resident is loaded and waited for.
// Authoring tools and the editor's synchronous additive resolve only; the
// runtime resolve (SceneResolveService) hands in the landed asset instead.
ModelAsset* LoadAndConvertResources(RenderServices& rs, const GUID& modelGuid,
                                    ModelRenderResources& out)
{
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return nullptr; // pre-init / headless: nothing to load
    auto asset = assetManager->GetAsset(modelGuid);
    if (!asset)
        asset = assetManager->LoadAssetAsync(modelGuid).get();
    return ConvertLoadedModel(rs, modelGuid, asset.get(), out);
}

void RegisterModelMaterials(RenderServices& rs, const ModelAsset& modelAsset, const ModelRenderResources& resources)
{
    const auto& embeddedImages = modelAsset.GetEmbeddedImages();
    for (const auto& cm : resources.materials)
    {
        Material* mat = rs.RegisterAndPrewarmMaterial(cm.derivedGuid, cm.document);
        rs.Textures().ResolveEmbeddedTextures(mat, cm.document, resources.modelGuid, embeddedImages);
    }
}

// The skeleton binder's model lookup never waits. A model that is not resident
// is requested (the request joins a load already in flight) and reads as an
// unavailable source this time; the binder's residency probe runs it again once
// it lands.
ModelAsset* ResidentModelAsset(AssetManager& assetManager, const GUID& modelGuid)
{
    if (modelGuid.IsNull())
        return nullptr;

    SharedPtr<Asset> asset = assetManager.GetAsset(modelGuid);
    if (!asset)
    {
        (void)assetManager.LoadAssetAsync(modelGuid);
        return nullptr;
    }
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return nullptr;
    return modelAsset;
}

// A MeshRenderer the resolve service has work for: a model with no GPU mesh
// yet, or a standalone material not registered yet.
bool NeedsResolve(RenderServices& rs, const Components::MeshRenderer& mr)
{
    if (mr.meshGpuHandleId == 0 && !mr.modelAssetGuid.IsNull())
        return true;
    const GUID materialGuid = mr.materialAssetGuid.ToGuid();
    return !materialGuid.IsNull() && !rs.Materials().Registry().Find(materialGuid);
}

} // anonymous namespace

// Resolve which submesh a MeshRenderer targets within its model. Explicit
// positional `meshId` wins; otherwise a non-zero `MeshNameId` selects the
// submesh whose (case-folded) name matches — this is how a scene importer that
// cannot know the positional index (its source format addresses a mesh by file id, not by index) still
// addresses one submesh of a multi-submesh model. First name match wins if a
// model has duplicate submesh names. A match interns the matched name, so the
// scene serializer can write the name even for an id that was loaded as a bare
// hash. A miss falls back to submesh 0 and logs a single diagnostic per
// (model, name), naming the wanted mesh and listing what the model offers.
uint32 ResolveSubmeshIndex(const GUID& modelGuid, const ModelAsset& model,
                           const Components::MeshRenderer& mr)
{
    const uint32 meshCount = model.GetMeshCount();

    // Explicit positional selector wins — but bounds-check it. A stale scene can
    // reference a submesh index that a re-import shrank away; returning it
    // unguarded feeds an out-of-range index straight to GetMesh. Fall back to
    // submesh 0 (still binds and renders) rather than reading past the array.
    if (mr.meshId != 0)
        return mr.meshId < meshCount ? mr.meshId : 0u;

    if (mr.MeshNameId == 0 || meshCount <= 1)
        return 0;

    // Pass 1 — exact (raw, unfolded) name-hash match. Byte-identical to pre-C1,
    // so persisted raw hashes still resolve, and two distinct submeshes that
    // differ only by a trailing _LOD<N> (e.g. "Torso" and "Torso_LOD1") resolve
    // to the RIGHT index instead of colliding on a folded hash.
    for (uint32 i = 0; i < meshCount; ++i)
    {
        if (Components::HashMeshName(model.GetMesh(i).Name) == mr.MeshNameId)
        {
            // Interning the matched name teaches the registry the string behind
            // an id that arrived as a bare `meshNameHash`, so saving the scene
            // migrates it to the `meshName` form. A no-op for the common case
            // where the id already came from a name.
            InternMeshName(model.GetMesh(i).Name);
            return i;
        }
    }

    // Pass 2 — fallback: fold a trailing _LOD<N> off each submesh name and retry,
    // so a bare base-name query ("Torso") still binds a suffixed submesh
    // ("Torso_LOD0") when no exact match caught it. A distinct base sibling would
    // have won pass 1, so a suffixed submesh can never shadow it here. (The
    // reverse — a suffixed query against a stripped base — is a C2 importer/
    // converter concern: it re-hashes names from source, so it never reaches a
    // stale hash here.)
    for (uint32 i = 0; i < meshCount; ++i)
    {
        const std::string_view folded = Components::FoldMeshLodSuffix(model.GetMesh(i).Name);
        if (Components::HashMeshName(folded) == mr.MeshNameId)
        {
            // The FOLDED spelling is the one that hashes to this id, so it is the
            // one the registry must hold for a save to round-trip.
            InternMeshName(folded);
            return i;
        }
    }

    static std::unordered_set<uint64_t> warned;
    constexpr uint64_t kMixPrime = 1099511628211ull;
    const uint64_t key = std::hash<GUID>{}(modelGuid) ^ (mr.MeshNameId * kMixPrime);
    if (warned.insert(key).second)
    {
        std::string names;
        for (uint32 i = 0; i < meshCount; ++i)
        {
            if (i != 0)
                names += ", ";
            names += model.GetMesh(i).Name;
        }
        const std::string wanted = FindMeshName(mr.MeshNameId);
        Logger::Log::Warning(
            "ModelRenderSetup: submesh '{}' (id {}) not found in model {} (submeshes: {}); using submesh 0",
            wanted.empty() ? "<name unknown: scene stored only the hash>" : wanted,
            mr.MeshNameId, modelGuid.ToString(), names);
    }
    return 0;
}

std::optional<ModelRenderResources> RegisterModelRenderResources(
    RenderServices& rs, const GUID& modelGuid)
{
    ModelRenderResources result{};
    ModelAsset* modelAsset = LoadAndConvertResources(rs, modelGuid, result);
    if (!modelAsset)
        return std::nullopt;
    RegisterModelMaterials(rs, *modelAsset, result);
    return result;
}

std::optional<ModelRenderResources> RegisterLoadedModelRenderResources(
    RenderServices& rs, const GUID& modelGuid, Asset& loadedAsset)
{
    ModelRenderResources result{};
    ModelAsset* modelAsset = ConvertLoadedModel(rs, modelGuid, &loadedAsset, result);
    if (!modelAsset)
        return std::nullopt;
    RegisterModelMaterials(rs, *modelAsset, result);
    return result;
}

std::optional<ModelRenderResources> RegisterModelRenderResourcesForSubmesh(
    RenderServices& rs, const GUID& modelGuid, uint32 submeshIndex)
{
    ModelRenderResources result{};
    ModelAsset* modelAsset = LoadAndConvertResources(rs, modelGuid, result);
    if (!modelAsset)
        return std::nullopt;

    if (submeshIndex >= modelAsset->GetMeshCount())
        return result;

    const uint32 matIdx = modelAsset->GetMesh(submeshIndex).MaterialIndex;
    if (matIdx >= result.materials.size())
        return result;

    const auto& cm = result.materials[matIdx];
    const auto& embeddedImages = modelAsset->GetEmbeddedImages();
    Material* mat = rs.RegisterAndPrewarmMaterial(cm.derivedGuid, cm.document);
    rs.Textures().ResolveEmbeddedTextures(mat, cm.document, modelGuid, embeddedImages);

    return result;
}

void PopulateMeshRenderer(Components::MeshRenderer& mr,
                          const ModelRenderResources& resources,
                          uint32 submeshIndex,
                          MeshMaterialFill materialFill)
{
    // Model asset reference (authoring-only).
    mr.modelAssetGuid.Set(resources.modelGuid);

    // Mesh GPU handle.
    if (submeshIndex < resources.meshHandles.size() && resources.meshHandles[submeshIndex].IsValid())
        mr.meshGpuHandleId = static_cast<uint64>(resources.meshHandles[submeshIndex]);
    else
        mr.meshGpuHandleId = 0;

    // A scene-authored MeshRenderer.material must survive resolution: scene load
    // deserializes the explicit override before this pass, so overwriting it with
    // the model's embedded per-submesh material silently drops per-part recolors
    // (the [mat k] siblings a multi-material FBX splits into). Keep it as-is.
    if (materialFill == MeshMaterialFill::PreserveExplicit && !mr.materialAssetGuid.IsNull())
        return;

    // Clear material GUID first to avoid stale data from a prior assignment.
    mr.materialAssetGuid.Clear();

    // Material GUID (resolved from the submesh's material index in the model).
    if (resources.modelAsset)
    {
        uint32 materialIndex = 0;
        if (submeshIndex < resources.modelAsset->GetMeshCount())
            materialIndex = resources.modelAsset->GetMesh(submeshIndex).MaterialIndex;

        if (materialIndex < resources.materials.size())
        {
            const GUID& matGuid = resources.materials[materialIndex].derivedGuid;
            mr.materialAssetGuid.Set(matGuid);
        }
    }
}

Components::LocalBounds ComputeModelBounds(const ModelAsset& modelAsset)
{
    Components::LocalBounds lb{};

    if (modelAsset.GetMeshCount() == 0)
        return lb;

    float bbMin[3], bbMax[3];
    modelAsset.GetBoundingBox(bbMin, bbMax);

    lb.Box = Mathematics::BoundingBox::FromMinMax(
        {bbMin[0], bbMin[1], bbMin[2]},
        {bbMax[0], bbMax[1], bbMax[2]});

    return lb;
}

std::vector<GUID> CollectStandaloneMaterialGuids(ECS::World& world, RenderServices& rs)
{
    auto& matReg = rs.Materials().Registry();

    // Dedupe: many entities can share the same material; only register each once.
    std::unordered_set<GUID> seen;
    std::vector<GUID> needed;
    // Asset resolution binds handles for the scene as authored: an entity that
    // is switched off still needs its material and mesh when it comes back.
    world.Query<ECS::Read<Components::MeshRenderer>>()
        .IncludeDisabled()
        .Each([&](const Components::MeshRenderer& mr)
        {
            const GUID matGuid = mr.materialAssetGuid.ToGuid();
            if (matGuid.IsNull())
                return;
            // Already in the runtime registry — model resolution or a prior
            // editor interaction handled it. Skip.
            if (matReg.Find(matGuid))
                return;
            if (seen.insert(matGuid).second)
                needed.push_back(matGuid);
        });

    return needed;
}

bool RegisterLoadedStandaloneMaterial(RenderServices& rs, const GUID& materialGuid, Asset* asset)
{
    if (!asset)
    {
        Logger::Log::Warning("ModelRenderSetup: failed to load material asset {}", materialGuid.ToString());
        return false;
    }
    auto* matAsset = dynamic_cast<MaterialAsset*>(asset);
    if (!matAsset)
    {
        // Not a MaterialAsset — likely a model-derived material whose GUID is
        // registered in the asset registry but produced by ModelMaterialBridge,
        // not loaded as a .material file. The model resolve registers those.
        return false;
    }
    rs.RegisterAndPrewarmMaterial(materialGuid, matAsset->GetDocument());
    return true;
}

void RegisterOneStandaloneMaterial(RenderServices& rs, const GUID& materialGuid)
{
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return; // pre-init / headless: nothing to load
    SharedPtr<Asset> asset = assetManager->GetAsset(materialGuid);
    if (!asset)
        asset = assetManager->LoadAssetAsync(materialGuid).get();
    RegisterLoadedStandaloneMaterial(rs, materialGuid, asset.get());
}

void ResolveStandaloneMaterials(ECS::World& world, RenderServices& rs)
{
    std::vector<GUID> needed = CollectStandaloneMaterialGuids(world, rs);
    if (needed.empty())
        return;

    Logger::Log::Info("ModelRenderSetup: registering {} standalone material asset(s)",
                      needed.size());

    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;

    // Issue all loads in parallel. The JobSystem fans them out across workers so
    // the load phase is max(load_time) not sum — RegisterAndPrewarmMaterial itself
    // touches Vulkan/MaterialRegistry and must stay serial on this thread, so we
    // drain the futures in order and register each as it lands. (The runtime
    // resolve, SceneResolveService, issues them the same way but registers each
    // only once it has landed, never waiting.)
    struct PendingLoad
    {
        GUID guid;
        SharedPtr<Asset> cached;
        std::optional<AssetFuture> future;
    };
    std::vector<PendingLoad> pending;
    pending.reserve(needed.size());
    for (const GUID& matGuid : needed)
    {
        PendingLoad p{matGuid, assetManager->GetAsset(matGuid), std::nullopt};
        if (!p.cached)
            p.future.emplace(assetManager->LoadAssetAsync(matGuid));
        pending.push_back(std::move(p));
    }

    for (auto& p : pending)
    {
        SharedPtr<Asset> asset = p.cached ? p.cached : (p.future ? p.future->get() : nullptr);
        RegisterLoadedStandaloneMaterial(rs, p.guid, asset.get());
    }
}

std::vector<UnresolvedModelEntity> CollectUnresolvedModelEntities(ECS::World& world)
{
    std::vector<UnresolvedModelEntity> pending;
    world.Query<ECS::Read<Components::MeshRenderer>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle e, const Components::MeshRenderer& mr)
        {
            if (mr.meshGpuHandleId != 0)
                return;
            if (mr.modelAssetGuid.IsNull())
                return;
            pending.push_back({e, mr.modelAssetGuid.ToGuid()});
        });
    return pending;
}

void ResolveOneModelEntity(ECS::World& world, RenderServices& rs, ECS::EntityHandle entity,
                           const GUID& modelGuid, ModelResolveCache& cache)
{
    auto it = cache.find(modelGuid);
    if (it == cache.end())
        it = cache.emplace(modelGuid, RegisterModelRenderResources(rs, modelGuid)).first;

    if (it->second)
        ApplyModelRenderResources(world, rs, entity, *it->second);
}

void ApplyModelRenderResources(ECS::World& world, RenderServices& rs, ECS::EntityHandle entity,
                               const ModelRenderResources& resources)
{
    const GUID& modelGuid = resources.modelGuid;

    auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
    if (!mr)
        return;

    Components::MeshRenderer updated = *mr;
    uint32 submeshIndex = ResolveSubmeshIndex(modelGuid, *resources.modelAsset, updated);
    PopulateMeshRenderer(updated, resources, submeshIndex, MeshMaterialFill::PreserveExplicit);
    world.AddComponentImmediate(entity, updated);

    // A scene-authored SKINNED model must also gain its SkinnedMeshRenderer
    // — the editor's ModelEntityFactory adds it at instantiation, but scene
    // files author only the MeshRenderer. Without it the model renders
    // statically in bind pose: the skeleton binder finds nothing to create
    // SkeletonRef/AnimatorRef on, and BootstrapSceneAnimators silently
    // no-ops.
    if (submeshIndex < resources.modelAsset->GetMeshCount()
        && resources.modelAsset->GetMesh(submeshIndex).IsSkinned()
        && !world.GetComponent<Components::SkinnedMeshRenderer>(entity))
    {
        Components::SkinnedMeshRenderer smr{};
        smr.meshId = 0;
        smr.skeletonId = resources.modelAsset->GetSkeletonId();
        smr.renderLayerMask = updated.renderLayerMask;
        smr.castShadows = updated.castShadows;
        smr.receiveShadows = updated.receiveShadows;
        world.AddComponentImmediate(entity, smr);
    }

    if (submeshIndex < resources.modelAsset->GetMeshCount()
        && resources.modelAsset->GetMesh(submeshIndex).HasMorphTargets()
        && !world.GetComponent<Components::MorphTargetWeights>(entity))
    {
        const Mesh& mesh = resources.modelAsset->GetMesh(submeshIndex);
        Components::MorphTargetWeights morph{};
        morph.weightCount = std::min<uint32>(
            static_cast<uint32>(mesh.MorphTargets.size()),
            Components::MorphTargetWeights::kMaxWeights);
        for (uint32 wi = 0; wi < morph.weightCount && wi < mesh.MorphTargetDefaultWeights.size(); ++wi)
            morph.weights[wi] = mesh.MorphTargetDefaultWeights[wi];
        morph.version = 2;
        morph.sourceMeshId = submeshIndex;
        morph.sourceMeshGpuHandleId = updated.meshGpuHandleId;
        resources.modelGuid.WriteBytes(morph.sourceModelGuid);
        world.AddComponentImmediate(entity, morph);
    }

    // Ensure LocalBounds is present for culling.
    if (!world.GetComponent<Components::LocalBounds>(entity))
    {
        // Use per-submesh bounds from the GPU registry when available.
        const auto* gpuEntry = (submeshIndex < resources.meshHandles.size())
            ? rs.GetMeshGPURegistry().Find(resources.meshHandles[submeshIndex])
            : nullptr;
        if (gpuEntry)
        {
            Components::LocalBounds lb{};
            lb.Box = gpuEntry->bounds;
            world.AddComponentImmediate(entity, lb);
        }
        else
        {
            world.AddComponentImmediate(entity, ComputeModelBounds(*resources.modelAsset));
        }
    }

    // Ensure WorldTransform exists (required by RenderExtractionSystem).
    if (!world.GetComponent<Components::WorldTransform>(entity))
    {
        Components::WorldTransform wt{};
        world.AddComponentImmediate(entity, wt);
    }
}

void ResolveModelMeshRenderers(ECS::World& world, RenderServices& rs)
{
    std::vector<UnresolvedModelEntity> pending = CollectUnresolvedModelEntities(world);
    if (pending.empty())
        return;

    Logger::Log::Info("ModelRenderSetup: resolving {} model mesh renderer(s)", pending.size());

    ModelResolveCache cache;
    for (const auto& u : pending)
        ResolveOneModelEntity(world, rs, u.Entity, u.ModelGuid, cache);
}

void BindChangedMeshRenderers(ECS::World& world, RenderServices& rs, ECS::ChangeGate& gate,
                              SceneResolveService& resolves)
{
    // The service's loads run on workers; what it binds now (a registered
    // model) and its later steps register on this thread.
    assert(rs.Materials().IsOwnerThread()
           && "BindChangedMeshRenderers is owner-thread only");

    // The editor's scene-open build owns first bind while it runs. Leave the
    // gate where it is: the first call after the build walks the changes made
    // meanwhile and hands over whatever the build did not resolve, so a spawn
    // made during the build is never dropped.
    if (IsSceneBuildPumpActive())
        return;

    const uint64 entryVersion = world.GetGlobalSystemVersion();
    // First call after process start: the gate is at version 0, so this walks
    // every MeshRenderer and a spawn made before the first tick is found too.
    // Collect first: the service binds a registered model's entity in Enqueue,
    // and that write would invalidate the walk.
    std::vector<ECS::EntityHandle> unresolved;
    auto q = world.Query<ECS::Read<Components::MeshRenderer>>();
    q.IncludeDisabled();
    if (ECS::ChangeFilter::Enabled())
        q.Changed<Components::MeshRenderer>(gate);
    q.Each([&](ECS::EntityHandle entity, const Components::MeshRenderer& mr)
    {
        if (NeedsResolve(rs, mr))
            unresolved.push_back(entity);
    });
    for (ECS::EntityHandle entity : unresolved)
        resolves.Enqueue(world, rs, entity);
    gate.LastRunVersion = entryVersion;
}

namespace
{
// A temporary construction reference. Each published SkeletonRef acquires its
// own reference before installation; failed ECS/map allocations release here.
struct RuntimeReference
{
    uint32 Id = 0;
    explicit RuntimeReference(uint32 id) : Id(id) {}
    ~RuntimeReference() { if (Id) SkeletonStore::Instance().ReleaseRuntime(Id); }
    RuntimeReference(const RuntimeReference&) = delete;
    RuntimeReference& operator=(const RuntimeReference&) = delete;
    RuntimeReference(RuntimeReference&& other) noexcept : Id(std::exchange(other.Id, 0)) {}
};

struct ModelInstanceKey
{
    ECS::EntityHandle Owner;
    GUID Model;
    bool operator==(const ModelInstanceKey&) const = default;
};
struct ModelInstanceHash
{
    size_t operator()(const ModelInstanceKey& key) const
    {
        return std::hash<GUID>{}(key.Model) ^ (std::hash<uint32>{}(key.Owner.id) << 1);
    }
};

uint32 LoadedSkeletonId(const Asset* asset)
{
    const auto* model = dynamic_cast<const ModelAsset*>(asset);
    return model && model->IsLoaded() && SkeletonStore::Instance().Get(model->GetSkeletonId())
        ? model->GetSkeletonId() : 0;
}

// A SkeletonRef that names neither a model source nor a skeleton has nothing
// to rebuild a rig from; say so once per entity rather than animating nothing.
void ReportSourcelessSkeletonRef(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* name = world.GetComponent<Components::Name>(entity);
    Logger::Log::Error(
        "SkeletonRef on '{}' (entity {}) names no model source and no skeleton, so no rig can be "
        "rebuilt for it: re-spawn the model from its asset, or set SkeletonRef.sourceModelGuid and "
        "instanceOwner on the node (procedural rigs assign skeletonId in memory).",
        name ? name->View() : std::string_view(), entity.id);
}

// Whether `resolves` still holds `entity`, or the instance owner `ref` names
// for a helper; `ref` is null for a SkinnedMeshRenderer with no SkeletonRef.
// No service holds nothing.
bool IsHeldByResolveService(const SceneResolveService* resolves, const ECS::World& world,
                            ECS::EntityHandle entity, const Components::SkeletonRef* ref)
{
    if (!resolves)
        return false;
    if (resolves->IsResolving(world, entity))
        return true;
    return ref && ref->ownerMode == Components::SkeletonInstanceOwner::Entity
        && resolves->IsResolving(world, ref->instanceOwner);
}

// `reported` is the owner's once-per-entity record for sourceless refs; null
// reports every call (one-shot hosts). `resolves`, when given, is the resolve
// service whose held entities are not bound yet: an entity the service still
// holds, and a helper whose instance owner it holds, is skipped (not bound to a
// placeholder); the service's MeshRenderer write when it completes the entity
// is the change that brings it back. Model lookups never wait.
void ResolveSkeletons(ECS::World& world, AssetManager* assetManager,
                      std::vector<SkeletonResolveState::ModelState>* observations = nullptr,
                      std::vector<ECS::EntityHandle>* reported = nullptr,
                      const SceneResolveService* resolves = nullptr)
{
    using Components::AnimatorRef;
    using Components::MeshRenderer;
    using Components::SkeletonRef;
    using Components::SkinnedMeshRenderer;
    using Components::SkeletonInstanceOwner;

    auto& skStore = SkeletonStore::Instance();
    struct Pending
    {
        ECS::EntityHandle Entity;
        SkeletonRef Previous, Updated;
        bool HadRef = false;
        uint32 SkeletonId = 0;
        SkeletonRuntimeSource Source;
    };
    std::vector<ECS::EntityHandle> entities;
    world.Query<ECS::Read<SkeletonRef>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle entity, const SkeletonRef& ref)
        {
            if (!IsHeldByResolveService(resolves, world, entity, &ref)) entities.push_back(entity);
        });
    world.Query<ECS::Read<SkinnedMeshRenderer>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle entity, const SkinnedMeshRenderer&)
        {
            if (!world.GetComponent<SkeletonRef>(entity) && !IsHeldByResolveService(resolves, world, entity, nullptr))
                entities.push_back(entity);
        });

    std::vector<Pending> pending;
    pending.reserve(entities.size());
    std::unordered_map<GUID, uint32> models;
    std::unordered_map<ModelInstanceKey, RuntimeReference, ModelInstanceHash> instances;

    for (auto entity : entities)
    {
        Pending item{};
        item.Entity = entity;
        if (const auto* ref = world.GetComponent<SkeletonRef>(entity))
        {
            item.HadRef = true;
            item.Previous = item.Updated = *ref;
        }
        auto& ref = item.Updated;
        // A model assignment selects that mesh's rig as well. Helpers have no
        // MeshRenderer and therefore require their own durable model source.
        if (const auto* mesh = world.GetComponent<MeshRenderer>(entity))
            if (!mesh->modelAssetGuid.IsNull())
                ref.sourceModelGuid = mesh->modelAssetGuid;

        if (!ref.sourceModelGuid.IsNull())
        {
            const auto owner = ref.ownerMode == SkeletonInstanceOwner::Self ? entity : ref.instanceOwner;
            item.Source = {ref.sourceModelGuid.ToGuid(), owner, world.GetWorldId(), world.GetLifecycleResetGeneration()};
            if ((ref.ownerMode == SkeletonInstanceOwner::Self || ref.ownerMode == SkeletonInstanceOwner::Entity)
                && world.IsValid(owner))
            {
                auto [model, inserted] = models.try_emplace(item.Source.ModelGuid, 0);
                if (inserted && assetManager)
                    model->second = LoadedSkeletonId(ResidentModelAsset(*assetManager, item.Source.ModelGuid));
                item.SkeletonId = model->second;
            }
            // Prefer an already owned runtime in the same exact instance. A
            // stale cache cannot join another world's or reset's allocation.
            const auto* runtime = skStore.GetRuntime(ref.runtimeId);
            if (item.SkeletonId && ref.runtimeGeneration
                && ref.runtimeGeneration == skStore.GetRuntimeGeneration(ref.runtimeId)
                && runtime && runtime->Source == item.Source
                && skStore.GetRuntimeSkeletonId(ref.runtimeId) == item.SkeletonId)
            {
                const ModelInstanceKey key{owner, item.Source.ModelGuid};
                if (!instances.contains(key) && skStore.RetainRuntime(ref.runtimeId))
                    instances.emplace(key, RuntimeReference(ref.runtimeId));
            }
        }
        else
        {
            // Explicit procedural rigs have no durable model source. Preserve
            // their existing in-memory assignment rather than inventing one.
            const auto* mesh = world.GetComponent<SkinnedMeshRenderer>(entity);
            const uint32 id = mesh ? mesh->skeletonId : ref.skeletonId;
            if (skStore.Get(id)) item.SkeletonId = id;
        }
        if (!item.HadRef && ref.sourceModelGuid.IsNull() && !item.SkeletonId)
            continue;
        pending.push_back(item);
    }

    // Allocate observation storage before component publication. Commit it only
    // after the resolver succeeds, so exceptions leave the owner's gates armed.
    std::vector<SkeletonResolveState::ModelState> observedModels;
    if (observations)
    {
        observedModels.reserve(models.size());
        for (const auto& [guid, skeleton] : models) observedModels.push_back({guid, skeleton});
    }

    std::vector<ECS::EntityHandle> sourceless;
    for (auto& item : pending)
    {
        auto& ref = item.Updated;
        RuntimeReference procedural(0);
        uint32 runtimeId = 0;
        if (item.SkeletonId)
        {
            if (!ref.sourceModelGuid.IsNull())
            {
                const ModelInstanceKey key{item.Source.InstanceOwner, item.Source.ModelGuid};
                auto found = instances.find(key);
                if (found == instances.end())
                {
                    RuntimeReference created(skStore.CreateRuntime(item.SkeletonId));
                    if (auto* runtime = skStore.GetRuntime(created.Id)) runtime->Source = item.Source;
                    found = instances.emplace(key, std::move(created)).first;
                }
                runtimeId = found->second.Id;
            }
            else
            {
                const bool generationMatches = ref.runtimeGeneration == 0
                    || ref.runtimeGeneration == skStore.GetRuntimeGeneration(ref.runtimeId);
                const auto* current = skStore.GetRuntime(ref.runtimeId);
                if (generationMatches && current && current->Source.ModelGuid.IsNull()
                    && skStore.GetRuntimeSkeletonId(ref.runtimeId) == item.SkeletonId)
                    runtimeId = ref.runtimeId;
                else
                    runtimeId = procedural.Id = skStore.CreateRuntime(item.SkeletonId);
            }
        }
        ref.skeletonId = item.SkeletonId;
        ref.runtimeId = runtimeId;
        ref.runtimeGeneration = skStore.GetRuntimeGeneration(runtimeId);

        if (ref.sourceModelGuid.IsNull() && !item.SkeletonId)
        {
            sourceless.push_back(item.Entity);
            if (!reported || std::find(reported->begin(), reported->end(), item.Entity) == reported->end())
                ReportSourcelessSkeletonRef(world, item.Entity);
        }

        const auto& old = item.Previous;
        const bool sameReference = runtimeId != 0 && runtimeId == old.runtimeId
            && (old.runtimeGeneration == ref.runtimeGeneration
                || (old.runtimeGeneration == 0 && old.sourceModelGuid.IsNull()));
        const bool changed = !item.HadRef || old.skeletonId != ref.skeletonId
            || old.runtimeId != ref.runtimeId || old.runtimeGeneration != ref.runtimeGeneration
            || old.sourceModelGuid != ref.sourceModelGuid;
        if (changed)
        {
            if (!item.HadRef)
            {
                // Structural hooks run before this component owns a runtime.
                // If an OnAdd hook throws, the remaining source-only component
                // is safe to remove or resolve again; construction refs unwind.
                auto unresolved = ref;
                unresolved.skeletonId = 0;
                unresolved.runtimeId = 0;
                unresolved.runtimeGeneration = 0;
                world.AddComponentImmediate(item.Entity, unresolved);
            }
            auto* writable = world.GetComponentForWrite<SkeletonRef>(item.Entity);
            if (!writable) continue;
            // Derived caches use the existing direct-write grant (column/dirty
            // stamps), not value hooks. No fallible work follows acquisition.
            RuntimeReference acquired(0);
            if (runtimeId && !sameReference)
            {
                if (!skStore.RetainRuntime(runtimeId)) continue;
                acquired.Id = runtimeId;
            }
            *writable = ref;
            acquired.Id = 0; // published component owns this reference
            if (!sameReference)
            {
                auto replaced = old;
                ReleaseSkeletonRuntime(replaced);
            }
        }

        if (const auto* mesh = world.GetComponent<SkinnedMeshRenderer>(item.Entity))
        {
            if (mesh->skeletonId != item.SkeletonId)
            {
                auto updated = *mesh;
                updated.skeletonId = item.SkeletonId;
                world.AddComponentImmediate(item.Entity, updated);
            }
        }
        if (runtimeId && !world.GetComponent<AnimatorRef>(item.Entity))
        {
            AnimatorRef animator{};
            animator.Flags = AnimatorRef::kFlag_Loop;
            world.AddComponentImmediate(item.Entity, animator);
        }
    }
    if (observations) observations->swap(observedModels);
    if (reported) reported->swap(sourceless);
}

void BindChangedSkeletons(ECS::World& world, SkeletonResolveState& state, const SceneResolveService& resolves,
                          AssetManager* assets)
{
    // Do not consume these changes while the editor's scene-open build is
    // still adding model prerequisites: the first tick after it must see the
    // complete instance. (The engine's resolve service never sets this flag;
    // its held entities are skipped one by one in ResolveSkeletons.)
    if (IsSceneBuildPumpActive()) return;
    if (state.WorldId != world.GetWorldId() || state.WorldGeneration != world.GetLifecycleResetGeneration())
    {
        state = {};
        state.WorldId = world.GetWorldId();
        state.WorldGeneration = world.GetLifecycleResetGeneration();
    }
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const auto structuralVersion = world.GetStructuralChangeVersion();
    bool changed = !ECS::ChangeFilter::Enabled();
    // The service let go of a skinned entity without writing its MeshRenderer
    // (its model missed, or another owner resolved it): an entity skipped
    // while held gets no other change to bring it back.
    if (state.ResolveHandBacks != resolves.HandBackGeneration())
        changed = true;
    if (assets)
        for (const auto& model : state.Models)
            if (LoadedSkeletonId(assets->GetAsset(model.ModelGuid).get()) != model.SkeletonId)
            {
                changed = true;
                break;
            }
    if (state.StructuralVersion != structuralVersion)
    {
        // Removing a container need not write any surviving child's columns.
        // Probe liveness on structural changes without loading any model assets.
        world.Query<ECS::Read<Components::SkeletonRef>>().IncludeDisabled().Each(
            [&](ECS::EntityHandle, const Components::SkeletonRef& ref)
            {
                if (ref.runtimeId && ref.ownerMode == Components::SkeletonInstanceOwner::Entity
                    && !world.IsValid(ref.instanceOwner)) changed = true;
            });
    }
    auto skeletons = world.Query<ECS::Read<Components::SkeletonRef>>();
    skeletons.IncludeDisabled();
    skeletons.Changed<Components::SkeletonRef>(state.Skeletons);
    skeletons.Each([&](ECS::EntityHandle, const Components::SkeletonRef&) { changed = true; });
    auto skinned = world.Query<ECS::Read<Components::SkinnedMeshRenderer>>();
    skinned.IncludeDisabled();
    skinned.Changed<Components::SkinnedMeshRenderer>(state.SkinnedMeshes);
    skinned.Each([&](ECS::EntityHandle, const Components::SkinnedMeshRenderer&) { changed = true; });
    auto meshes = world.Query<ECS::Read<Components::MeshRenderer>>();
    meshes.IncludeDisabled();
    meshes.Changed<Components::MeshRenderer>(state.ModelMeshes);
    meshes.Each([&](ECS::EntityHandle entity, const Components::MeshRenderer&)
    {
        if (world.GetComponent<Components::SkeletonRef>(entity) || world.GetComponent<Components::SkinnedMeshRenderer>(entity))
            changed = true;
    });
    if (changed) ResolveSkeletons(world, assets, &state.Models, &state.ReportedSourceless, &resolves);
    state.ResolveHandBacks = resolves.HandBackGeneration();
    state.Skeletons.LastRunVersion = entryVersion;
    state.SkinnedMeshes.LastRunVersion = entryVersion;
    state.ModelMeshes.LastRunVersion = entryVersion;
    state.StructuralVersion = structuralVersion;
}
} // namespace

void ResolveSkinnedMeshAnimation(ECS::World& world, AssetManager& assetManager)
{
    ResolveSkeletons(world, &assetManager);
}

void BindChangedSkinnedMeshAnimation(ECS::World& world, SkeletonResolveState& state,
                                     const SceneResolveService& resolves)
{
    BindChangedSkeletons(world, state, resolves, EngineCore::GetInstance().TryGetAssetManager());
}

void BindChangedSkinnedMeshAnimation(ECS::World& world, SkeletonResolveState& state,
                                     const SceneResolveService& resolves, AssetManager& assets)
{
    BindChangedSkeletons(world, state, resolves, &assets);
}

} // namespace Engine::Renderer
} // namespace GameEngine
