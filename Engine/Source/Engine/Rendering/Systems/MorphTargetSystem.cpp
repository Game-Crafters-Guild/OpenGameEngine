#include "ECSModules/Rendering/Systems/MorphTargetSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine
{
namespace Engine::Renderer
{

namespace
{

GUID RuntimeMorphGuid(const GUID& modelGuid, uint64 worldId, ECS::EntityId entityId, uint32 meshId)
{
    return GUID::Derive(
        modelGuid,
        "morph/runtime/" + std::to_string(worldId) + "/" +
            std::to_string(entityId) + "/" + std::to_string(meshId));
}

// Resident-only lookup: never reaches disk, so it is safe under the registry's
// table mutex. Returns nullptr for a model that has not streamed in, which the
// caller treats exactly as an unloaded asset and retries next frame.
ModelAsset* ResidentModelAsset(const GUID& modelGuid)
{
    auto asset = EngineCore::GetInstance().GetAssetManager().GetAsset(modelGuid);
    return asset ? dynamic_cast<ModelAsset*>(asset.get()) : nullptr;
}

// Blocking resolve for a model that is not resident yet. MUST run outside the
// registry's TableScope: it waits on asset IO, and the table mutex is shared
// with every other registrar in this ECS wave, so holding it here would pin
// them behind the load and weld an unenforced loader-acyclicity assumption into
// the lock.
void EnsureModelResident(const GUID& modelGuid)
{
    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    if (assetManager.GetAsset(modelGuid))
        return;
    assetManager.LoadAssetAsync(modelGuid).get();
}

bool AnyNonZeroWeight(const Components::MorphTargetWeights& weights, uint32 count)
{
    const uint32 clampedCount = std::min(count, Components::MorphTargetWeights::kMaxWeights);
    for (uint32 i = 0; i < clampedCount; ++i)
    {
        if (std::abs(weights.weights[i]) > 1e-6f)
            return true;
    }
    return false;
}

bool WeightsChanged(const Components::MorphTargetWeights& weights, uint32 count)
{
    if (weights.appliedVersion != weights.version)
        return true;
    const uint32 clampedCount = std::min(count, Components::MorphTargetWeights::kMaxWeights);
    for (uint32 i = 0; i < clampedCount; ++i)
    {
        if (weights.weights[i] != weights.appliedWeights[i])
            return true;
    }
    return false;
}

void MarkWeightsApplied(Components::MorphTargetWeights& weights, uint32 count)
{
    const uint32 clampedCount = std::min(count, Components::MorphTargetWeights::kMaxWeights);
    std::fill(weights.appliedWeights,
              weights.appliedWeights + Components::MorphTargetWeights::kMaxWeights,
              0.0f);
    std::memcpy(weights.appliedWeights, weights.weights, sizeof(float) * clampedCount);
    weights.appliedVersion = weights.version;
}

void ClearGuid(uint8 (&bytes)[16])
{
    std::memset(bytes, 0, 16);
}

// Value-gated writes: this system visits every morph-carrying entity every
// frame, and MeshRenderer/LocalBounds are columns Changed<>-gated consumers
// probe. Write grants stamp the whole (chunk, column), so grants are taken
// only when a value actually differs — idle frames must not stamp (same
// idiom as SkyEnvironmentSystem's per-frame sun writes).
void AssignMeshGpuHandle(ECS::World& world, ECS::EntityHandle entity,
                         const Components::MeshRenderer& meshRenderer, uint64 handleId)
{
    if (meshRenderer.meshGpuHandleId == handleId)
        return;
    if (auto* meshRendererW = world.GetComponentForWrite<Components::MeshRenderer>(entity))
        meshRendererW->meshGpuHandleId = handleId;
}

void WriteBounds(ECS::World& world, ECS::EntityHandle entity,
                 const Components::LocalBounds* localBounds,
                 const Rendering::MeshGPUEntry* entry)
{
    if (!localBounds || !entry)
        return;
    if (std::memcmp(&localBounds->Box, &entry->bounds, sizeof(localBounds->Box)) == 0)
        return;
    if (auto* localBoundsW = world.GetComponentForWrite<Components::LocalBounds>(entity))
        localBoundsW->Box = entry->bounds;
}

void UnregisterRuntimeMesh(Rendering::MeshGPURegistry& meshRegistry,
                           Components::MorphTargetWeights& morph)
{
    const GUID runtimeGuid = GUID::FromBytes(morph.runtimeModelGuid);
    if (!runtimeGuid.IsNull())
        meshRegistry.UnregisterSubmesh({runtimeGuid, 0});
    ClearGuid(morph.runtimeModelGuid);
    morph.runtimeMeshGpuHandleId = 0;
}

} // namespace

void MorphTargetSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices)
        return;

    auto& meshRegistry = m_RenderServices->GetMeshGPURegistry();
    const uint64 worldId = world.GetWorldId();

    m_SeenEntities.clear();

    // Stream in any model this pass will need BEFORE the registry scope exists.
    // The pass below holds the registry's table mutex, which every registrar in
    // this wave shares, so the one call here that can wait on disk has to happen
    // outside it. A model still not resident afterwards is skipped by the pass
    // and retried next frame — the same outcome an unloaded asset always had.
    // The test resolver supplies models directly and never touches the asset
    // manager, so it skips this pass entirely.
    if (!m_ModelResolverForTest)
    {
        world.Query<ECS::Read<Components::MeshRenderer>,
                    ECS::Read<Components::MorphTargetWeights>>()
            .Each([](const Components::MeshRenderer& meshRenderer,
                     const Components::MorphTargetWeights& /*morph*/)
        {
            const GUID modelGuid = meshRenderer.modelAssetGuid.ToGuid();
            if (!modelGuid.IsNull())
                EnsureModelResident(modelGuid);
        });
    }

    // This system runs on a job worker: its wave holds several systems, and at
    // least one of them (EZTreeExtraction) registers meshes too. Every lookup
    // below feeds a value used after the call returns — a Find() pointer handed
    // to WriteBounds, a handle written onto a component — so the exclusion has
    // to span the whole pass, not each call. Nothing inside it may block.
    Rendering::MeshGPURegistry::TableScope registryScope(meshRegistry);

    // MeshRenderer and LocalBounds are bound const and written through the
    // value-gated helpers above; MorphTargetWeights is a genuine per-frame
    // write column (applied-weight bookkeeping) and stays Write-declared.
    // A switched-off MorphTargetWeights is still visited: the entity has to go
    // back to its source mesh, which only this pass can hand it.
    auto query = world.Query<ECS::Read<Components::MeshRenderer>,
                             ECS::Write<Components::MorphTargetWeights>,
                             ECS::Optional<Components::LocalBounds>,
                             ECS::Optional<ECS::ComponentDisabled<Components::MorphTargetWeights>>>();
    query.IncludeDisabled<Components::MorphTargetWeights>();
    query.Each([&](ECS::EntityHandle entity,
                   const Components::MeshRenderer& meshRenderer,
                   Components::MorphTargetWeights& morph,
                   const Components::LocalBounds* localBounds,
                   const ECS::ComponentDisabled<Components::MorphTargetWeights>* morphOff)
    {
        const GUID modelGuid = meshRenderer.modelAssetGuid.ToGuid();
        if (modelGuid.IsNull())
            return;

        const GUID previousSourceGuid = GUID::FromBytes(morph.sourceModelGuid);
        const bool sourceChanged =
            previousSourceGuid.IsNull() || previousSourceGuid != modelGuid || morph.sourceMeshId != meshRenderer.meshId;
        if (sourceChanged)
        {
            UnregisterRuntimeMesh(meshRegistry, morph);
            modelGuid.WriteBytes(morph.sourceModelGuid);
            morph.sourceMeshId = meshRenderer.meshId;
            morph.sourceMeshGpuHandleId = 0;
            morph.appliedVersion = 0;
        }

        ModelAsset* modelAsset = m_ModelResolverForTest ? m_ModelResolverForTest(modelGuid)
                                                        : ResidentModelAsset(modelGuid);
        if (!modelAsset || !modelAsset->IsLoaded() || meshRenderer.meshId >= modelAsset->GetMeshCount())
            return;

        const Mesh& sourceMesh = modelAsset->GetMesh(meshRenderer.meshId);
        if (!sourceMesh.HasMorphTargets())
        {
            UnregisterRuntimeMesh(meshRegistry, morph);
            morph.weightCount = 0;
            return;
        }

        const uint32 targetCount = std::min<uint32>(
            static_cast<uint32>(sourceMesh.MorphTargets.size()),
            Components::MorphTargetWeights::kMaxWeights);
        morph.weightCount = targetCount;

        Rendering::MeshGPUHandle sourceHandle(morph.sourceMeshGpuHandleId);
        if (!sourceHandle.IsValid() || !meshRegistry.Find(sourceHandle))
        {
            sourceHandle = meshRegistry.FindHandle({modelGuid, meshRenderer.meshId});
            if (!sourceHandle.IsValid())
                sourceHandle = meshRegistry.RegisterSubmesh({modelGuid, meshRenderer.meshId}, sourceMesh);
            morph.sourceMeshGpuHandleId = sourceHandle.IsValid() ? static_cast<uint64>(sourceHandle) : 0;
        }

        if (morphOff || !AnyNonZeroWeight(morph, targetCount))
        {
            UnregisterRuntimeMesh(meshRegistry, morph);
            if (sourceHandle.IsValid())
            {
                AssignMeshGpuHandle(world, entity, meshRenderer, static_cast<uint64>(sourceHandle));
                WriteBounds(world, entity, localBounds, meshRegistry.Find(sourceHandle));
            }
            MarkWeightsApplied(morph, targetCount);
            return;
        }

        m_SeenEntities.push_back(entity.id);

        Rendering::MeshGPUHandle runtimeHandle(morph.runtimeMeshGpuHandleId);
        const GUID runtimeGuid = RuntimeMorphGuid(modelGuid, worldId, entity.id, meshRenderer.meshId);
        const GUID previousRuntimeGuid = GUID::FromBytes(morph.runtimeModelGuid);
        const bool runtimeMissing = !runtimeHandle.IsValid() || !meshRegistry.Find(runtimeHandle);
        const bool runtimeGuidChanged = previousRuntimeGuid != runtimeGuid;
        if (!runtimeMissing && !runtimeGuidChanged && !WeightsChanged(morph, targetCount))
        {
            AssignMeshGpuHandle(world, entity, meshRenderer, static_cast<uint64>(runtimeHandle));
            return;
        }

        if (!previousRuntimeGuid.IsNull())
            meshRegistry.UnregisterSubmesh({previousRuntimeGuid, 0});

        Mesh morphedMesh = CreateMorphedMesh(sourceMesh, morph.weights, targetCount);
        runtimeHandle = meshRegistry.RegisterSubmesh({runtimeGuid, 0}, morphedMesh);
        if (!runtimeHandle.IsValid())
        {
            Logger::Log::Warning("MorphTargetSystem: failed to upload morphed mesh for entity {}", entity.id);
            if (sourceHandle.IsValid())
                AssignMeshGpuHandle(world, entity, meshRenderer, static_cast<uint64>(sourceHandle));
            ClearGuid(morph.runtimeModelGuid);
            morph.runtimeMeshGpuHandleId = 0;
            return;
        }

        runtimeGuid.WriteBytes(morph.runtimeModelGuid);
        morph.runtimeMeshGpuHandleId = static_cast<uint64>(runtimeHandle);
        AssignMeshGpuHandle(world, entity, meshRenderer, morph.runtimeMeshGpuHandleId);
        MarkWeightsApplied(morph, targetCount);
        WriteBounds(world, entity, localBounds, meshRegistry.Find(runtimeHandle));
        m_RuntimeMeshes[entity.id] = {runtimeGuid};
    });

    if (m_RuntimeMeshes.empty())
        return;

    std::unordered_set<ECS::EntityId> seen(m_SeenEntities.begin(), m_SeenEntities.end());
    for (auto it = m_RuntimeMeshes.begin(); it != m_RuntimeMeshes.end();)
    {
        if (seen.find(it->first) == seen.end())
        {
            if (!it->second.runtimeGuid.IsNull())
                meshRegistry.UnregisterSubmesh({it->second.runtimeGuid, 0});
            it = m_RuntimeMeshes.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace Engine::Renderer
} // namespace GameEngine
