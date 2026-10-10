#include "Engine/Rendering/DDGISceneService.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "Core/DebugMetrics.h"
#include "SceneBvh/ThreadedBvhPool.h"
#include "Engine/Rendering/DDGIEmissive.h"
#include "Engine/Rendering/DDGIUberMaterialBake.h"
#include "Engine/Rendering/DDGIMaterialMapAtlas.h"
#include "Engine/Rendering/DDGIMaterialUpdates.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "SceneBvh/MeshGeometryView.h"
#include "SceneBvh/ThreadedBvh.h"
#include "SceneBvh/ThreadedBvhBuilder.h"
#include "SceneBvh/TlasPacker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace GameEngine::Engine::Renderer
{

namespace
{

#if GE_ENABLE_METRICS
// Process-wide counters remain monotonic when a scene replaces its service.
std::atomic<uint32_t> s_SoftwareGeometryRebuilds{0};
std::atomic<uint32_t> s_SoftwareMaterialPatches{0};
#endif

// Same sanity bound the hardware lane's instance filter uses
// (DDGIProbeFeature.cpp) — a transform or radius past this is corrupt data,
// not a big scene.
constexpr float kMaxSaneWorldUnits = 1.0e6f;

// A world matrix this close to singular has no usable inverse, and the packed
// instance record IS the inverse (TlasPacker.h) — admitting one would write
// inf/NaN rows that poison every ray transformed through them. The hardware
// lane uploads the forward transform and never inverts, so this guard is
// specific to the software lane rather than a divergence from its filter.
constexpr float kMinInvertibleDeterminant = 1.0e-12f;

// Sentinel for Material::GetGpuSceneMaterialIndex on a material that has not
// been assigned an SSBO row. Mirrors Material::kInvalidSSBOIndex, which is
// private.
constexpr uint32_t kUnassignedMaterialSlot = 0xFFFFFFFFu;

// A plan-mesh slot recorded for a gpuMeshIndex that failed to resolve this
// sweep, so the next instance referencing it skips the lookup instead of
// retrying it per instance.
constexpr uint32_t kUnresolvedMeshSlot = 0xFFFFFFFFu;

// Rebuild debounce. Same value and same rationale as
// DDGIProbeFeature::kStructuralIdleGateMs: a change that is still in motion
// (an inspector drag, a streaming burst) should cost one rebuild when it
// settles, not one per frame.
constexpr float kStructuralIdleGateMs = 200.0f;

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime       = 1099511628211ull;

void HashBytes(uint64_t& hash, const void* data, size_t bytes)
{
    const auto* cursor = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= cursor[i];
        hash *= kFnvPrime;
    }
}

template <typename T>
void HashValue(uint64_t& hash, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>, "hashed value must be trivially copyable");
    HashBytes(hash, &value, sizeof(T));
}

bool IsSaneTransform(const Mathematics::Matrix4x4& transform, float boundingRadius)
{
    if (!std::isfinite(boundingRadius) || boundingRadius >= kMaxSaneWorldUnits)
        return false;
    const float* m = transform.Data();
    for (int i = 0; i < 16; ++i)
    {
        if (!std::isfinite(m[i]) || std::fabs(m[i]) >= kMaxSaneWorldUnits)
            return false;
    }
    return std::fabs(glm::determinant(transform.GetGLM())) >= kMinInvertibleDeterminant;
}



}  // namespace

DDGISceneService::DDGISceneService(Rendering::IDevice* device,
                                   Rendering::MeshGPURegistry* meshRegistry,
                                   Rendering::GPUScene* gpuScene, MaterialSystem* materials,
                                   const DDGIMaterialMapAtlas* mapAtlas)
    : m_Device(device), m_MeshRegistry(meshRegistry), m_GpuScene(gpuScene), m_Materials(materials),
      m_MapAtlas(mapAtlas)
{
#if GE_ENABLE_METRICS
    auto& metrics = Debug::DebugMetrics::Get();
    metrics.PushSample("DDGI/Software/GeometryRebuilds", static_cast<float>(s_SoftwareGeometryRebuilds.load()));
    metrics.PushSample("DDGI/Software/MaterialPatches", static_cast<float>(s_SoftwareMaterialPatches.load()));
#endif
    if (m_MeshRegistry)
    {
        m_ReloadSubscription = m_MeshRegistry->SubscribeReload(
            [this](const GUID&) { m_ReloadCounter.fetch_add(1, std::memory_order_relaxed); });
    }
}

DDGISceneService::~DDGISceneService()
{
    if (!m_Device)
        return;
    ReleaseBuffers();
    for (const RetiredBuffer& retired : m_RetiredBuffers)
        m_Device->DestroyBuffer(retired.Buffer);
    m_RetiredBuffers.clear();
}

bool DDGISceneService::Tick(float deltaTimeSeconds)
{
    if (!m_Device || !m_GpuScene || !m_MeshRegistry)
        return false;

    ++m_FrameClock;
    constexpr uint64_t kRetireMargin = Rendering::IDevice::kMaxSupportedFramesInFlight + 1;
    std::erase_if(m_RetiredBuffers,
                  [&](const RetiredBuffer& retired)
                  {
                      if (m_FrameClock - retired.FrameStamp <= kRetireMargin)
                          return false;
                      m_Device->DestroyBuffer(retired.Buffer);
                      return true;
                  });

    // Cheap gate first: no instance content change, no material edit and no
    // mesh reload means the plan from the last sweep is still exact.
    const uint64_t sceneEpoch    = m_GpuScene->GetContentEpoch();
    const uint64_t materialEpoch = Material::GetGlobalContentEpoch();
    const uint64_t reloadCounter = m_ReloadCounter.load(std::memory_order_relaxed);
    // The atlas epoch is a signal in its own right: a base-colour or emissive
    // texture finishing its stream-in gives a material a layer it did not have,
    // and that changes the baked records without touching any other epoch.
    const uint64_t atlasEpoch = m_MapAtlas ? m_MapAtlas->GetLayoutEpoch() : 0;
    const bool signalsMoved = !m_HaveEpochs || sceneEpoch != m_LastSceneEpoch ||
                              materialEpoch != m_LastMaterialEpoch ||
                              reloadCounter != m_LastReloadCounter ||
                              atlasEpoch != m_LastAtlasLayoutEpoch;
    m_HaveEpochs            = true;
    m_LastSceneEpoch        = sceneEpoch;
    m_LastMaterialEpoch     = materialEpoch;
    m_LastReloadCounter     = reloadCounter;
    m_LastAtlasLayoutEpoch  = atlasEpoch;

    if (signalsMoved)
    {
        // The epochs over-invalidate (any content change bumps them, not just
        // the ones this lane's filter cares about); the hash is what keeps
        // those spurious bumps from reaching the GPU.
        const PlanHashes swept = SweepScene();
        if (swept.Structure != m_Observed.Structure)
            m_IdleTimerMs = 0.0f;
        m_Observed = swept;
    }

    // Advanced on every tick, including the ones that short-circuit above:
    // the gate fires precisely when the structure has stopped moving.
    m_IdleTimerMs += std::max(deltaTimeSeconds, 0.0f) * 1000.0f;

    if (m_Observed.Full == m_BuiltHash)
        return false;

    // Scalar/color changes retain the material table and instance assignments.
    // They bypass the structural idle gate and patch only changed records.
    if (m_Observed.Structure == m_BuiltStructure &&
        m_Observed.Transforms == m_BuiltTransforms && QueueMaterialUpdatesFromPlan())
    {
        m_BuiltHash = m_Observed.Full;
        return true;
    }

    // Transform-only: the trees are local-space, so nothing that was built
    // needs rebuilding. Re-pack the TLAS against the cached pool and skip the
    // idle gate — a moving occluder's GI shadow must follow it, not wait for
    // it to stop.
    if (m_Observed.Structure == m_BuiltStructure && RepackInstancesFromPlan())
    {
        m_BuiltHash = m_Observed.Full;
        m_BuiltTransforms = m_Observed.Transforms;
        return true;
    }

    if (m_IdleTimerMs < kStructuralIdleGateMs)
        return false;

    RebuildFromPlan();
    m_BuiltHash      = m_Observed.Full;
    m_BuiltStructure = m_Observed.Structure;
    m_BuiltTransforms = m_Observed.Transforms;
    return true;
}

DDGISceneService::PlanHashes DDGISceneService::SweepScene()
{
    m_PlanMeshes.clear();
    m_PlanParticipants.clear();
    m_PlanMaterials.clear();
    m_PlanMeshSlotByGpuIndex.clear();
    m_PlanMaterialSlotByHash.clear();

    // gpuMeshIndex -> the registry identity behind it. Re-derived every sweep
    // rather than cached against the reload notification, for the reason
    // SceneAccelerationStructureService::MakeSourceKey documents: a direct
    // RegisterSubmesh can swap new content into an existing row without the
    // notification firing.
    struct RegistryIdentity
    {
        GUID AssetGuid{};
        uint32_t SubmeshIndex = 0;
        uint64_t ContentHash  = 0;
        std::shared_ptr<const GameEngine::Mesh> CpuMesh;
    };
    std::unordered_map<uint32_t, RegistryIdentity> identityByGpuIndex;
    m_MeshRegistry->ForEachKeyedEntry(
        [&](const Rendering::MeshGPUKey& key, const Rendering::MeshGPUEntry& entry)
        {
            if (entry.gpuMeshIndex == ~0u)
                return;
            identityByGpuIndex[entry.gpuMeshIndex] =
                RegistryIdentity{key.assetGuid, key.submeshIndex, entry.contentHash, entry.cpuMesh};
        });

    std::unordered_map<uint32_t, const Material*> materialBySlot;
    if (m_Materials)
    {
        m_Materials->Registry().ForEach(
            [&](const GUID&, const Material& material)
            {
                const uint32_t slot = material.GetGpuSceneMaterialIndex();
                if (slot != kUnassignedMaterialSlot)
                    materialBySlot[slot] = &material;
            });
    }

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();

    // Bakes a GPU-scene material slot into an uber material and returns its row
    // in the shared table, reusing an existing row whenever the bytes match.
    // Content, not material index, is the identity: instances of two distinct
    // materials that bake identically shade identically, so they may as well
    // cost one row — and the common case (one material, many instances) folds
    // to a single row without a per-instance bake.
    auto materialSlotFor = [&](uint32_t gpuSceneMaterialIndex) -> uint32_t
    {
        const auto found = materialBySlot.find(gpuSceneMaterialIndex);
        const SceneBvh::UberMaterial baked = BakeUberMaterial(
            found != materialBySlot.end() ? found->second : nullptr, m_MapAtlas);

        uint64_t materialHash = kFnvOffsetBasis;
        HashBytes(materialHash, &baked, sizeof(baked));
        const auto range = m_PlanMaterialSlotByHash.equal_range(materialHash);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (std::memcmp(&m_PlanMaterials[it->second], &baked, sizeof(baked)) == 0)
                return it->second;
        }

        const auto slot = static_cast<uint32_t>(m_PlanMaterials.size());
        m_PlanMaterials.push_back(baked);
        m_PlanMaterialSlotByHash.emplace(materialHash, slot);
        return slot;
    };

    const std::vector<Rendering::GPUInstance>& instances = m_GpuScene->GetInstances();
    m_PlanParticipants.reserve(instances.size());
    for (const Rendering::GPUInstance& instance : instances)
    {
        // Sector-tagged (camera-relative) instances are unhandled by either
        // lane yet. Skinned instances participate: each gets its OWN plan
        // slot below (a shared slot cannot hold two different poses), whose
        // pooled BVH range the per-tick GPU refit re-poses
        // (ddgi_bvh_refit.comp) — the software twin of the hardware lane's
        // per-instance posed BLASes.
        if ((instance.sectorPacked[0] | instance.sectorPacked[1]) != 0u)
            continue;
        const bool isSkinned = instance.skinPaletteOffset != 0u && instance.runtimeId != 0u;
        if (!IsSaneTransform(instance.transform, instance.boundingRadius))
            continue;

        // The material rides on the instance record, so a plan slot is per MESH
        // — every instance of it shares one BLAS however many materials the
        // scene draws it with.
        const uint32_t materialSlot = materialSlotFor(instance.materialIndex);

        uint32_t planSlot = kUnresolvedMeshSlot;
        const auto cached =
            isSkinned ? m_PlanMeshSlotByGpuIndex.end() : m_PlanMeshSlotByGpuIndex.find(instance.meshIndex);
        if (cached != m_PlanMeshSlotByGpuIndex.end())
        {
            planSlot = cached->second;
        }
        else
        {
            ResolvedMesh resolved;
            resolved.GpuMeshIndex = instance.meshIndex;

            const auto identity = identityByGpuIndex.find(instance.meshIndex);
            if (identity != identityByGpuIndex.end())
            {
                resolved.ContentHash = identity->second.ContentHash;
                // Two-tier CPU geometry resolve, matching the editor picking
                // path (MeshPickingService::ResolveCpuMesh): the registry's own
                // copy covers generated/procedural meshes that no ModelAsset
                // backs, otherwise go through the asset. Never force-load — a
                // mesh that is not resident yet simply sits this rebuild out,
                // the same tolerance the hardware lane has for a BLAS that is
                // not Ready.
                if (identity->second.CpuMesh)
                {
                    resolved.CpuMeshOwner = identity->second.CpuMesh;
                    resolved.Geometry     = resolved.CpuMeshOwner.get();
                }
                else if (SharedPtr<Asset> asset = assetManager.GetAsset(identity->second.AssetGuid))
                {
                    auto* model = dynamic_cast<ModelAsset*>(asset.get());
                    if (model && model->IsLoaded() &&
                        identity->second.SubmeshIndex < model->GetMeshCount())
                    {
                        resolved.AssetOwner = asset;
                        resolved.Geometry   = &model->GetMesh(identity->second.SubmeshIndex);
                    }
                }
            }

            if (resolved.Geometry &&
                resolved.Geometry->PrimitiveTopology != MeshPrimitiveTopology::Triangles)
            {
                resolved.Geometry = nullptr;  // the BVH format is triangles only
            }

            if (resolved.Geometry)
            {
                resolved.DefaultMaterialSlot = materialSlot;
                resolved.SkinnedRuntimeId = isSkinned ? instance.runtimeId : 0u;
                planSlot = static_cast<uint32_t>(m_PlanMeshes.size());
                m_PlanMeshes.push_back(std::move(resolved));
            }
            // A skinned slot is per instance — never cached for sharing (and a
            // skinned resolve failure must not poison the static cache).
            if (!isSkinned)
                m_PlanMeshSlotByGpuIndex[instance.meshIndex] = planSlot;
        }

        if (planSlot == kUnresolvedMeshSlot)
            continue;
        m_PlanParticipants.push_back(
            Participant{instance.transform, instance.meshIndex, planSlot, materialSlot,
                        (instance.flags & Rendering::kInstanceFlagGIEmitter) != 0u});
    }

    // Values do not affect BVH topology. Keep table layout, map bindings and
    // instance slot assignments structural so deduplication changes cannot
    // patch a record still shared by a different material.
    uint64_t structureHash = kFnvOffsetBasis;
    HashValue(structureHash, m_PlanMeshes.size());
    HashValue(structureHash, m_PlanParticipants.size());
    HashValue(structureHash, m_PlanMaterials.size());
    for (const ResolvedMesh& mesh : m_PlanMeshes)
    {
        HashValue(structureHash, mesh.GpuMeshIndex);
        HashValue(structureHash, mesh.DefaultMaterialSlot);
        HashValue(structureHash, mesh.ContentHash);
        HashValue(structureHash, mesh.SkinnedRuntimeId);
    }
    for (const SceneBvh::UberMaterial& material : m_PlanMaterials)
    {
        const auto bindings = DDGIMaterialBindings(material);
        HashBytes(structureHash, bindings.data(), sizeof(bindings));
    }
    for (const Participant& participant : m_PlanParticipants)
    {
        HashValue(structureHash, participant.PlanMeshSlot);
        HashValue(structureHash, participant.MaterialSlot);
        // In the structure hash or toggling the flag never rebuilds the packed
        // scene, and the software lane keeps shading with the stale answer.
        HashValue(structureHash, participant.GIEmitter ? 1u : 0u);
    }

    // Full: the structure plus the world transforms, which is what the packed
    // instance records actually hold.
    uint64_t transformHash = kFnvOffsetBasis;
    for (const Participant& participant : m_PlanParticipants)
        HashBytes(transformHash, participant.WorldFromLocal.Data(), sizeof(float) * 16);
    uint64_t fullHash = structureHash;
    HashValue(fullHash, transformHash);
    for (const SceneBvh::UberMaterial& material : m_PlanMaterials)
        HashBytes(fullHash, &material, sizeof(material));

    // 0 is the "nothing built yet" sentinel for both built-state fields.
    return PlanHashes{structureHash == 0 ? 1ull : structureHash,
                      transformHash == 0 ? 1ull : transformHash,
                      fullHash == 0 ? 1ull : fullHash};
}

void DDGISceneService::RebuildFromPlan()
{
    m_ParticipatingInstanceCount = 0;
    if (m_PlanMeshes.empty() || m_PlanParticipants.empty())
    {
        ReleaseBuffers();
        return;
    }

    const size_t meshCount = m_PlanMeshes.size();

    // De-interleaved build inputs. ModelAsset::Mesh stores an interleaved
    // Vertex array; MeshGeometryView wants parallel flat spans. These live
    // only until BuildMany returns — the ThreadedBvh it produces owns its own
    // interleaved GPU vertex record.
    std::vector<std::vector<float>> positions(meshCount);
    std::vector<std::vector<float>> normals(meshCount);
    std::vector<std::vector<float>> texCoords(meshCount);
    std::vector<SceneBvh::MeshGeometryView> soups(meshCount);
    for (size_t m = 0; m < meshCount; ++m)
    {
        const GameEngine::Mesh& source = *m_PlanMeshes[m].Geometry;
        const size_t vertexCount       = source.Vertices.size();
        positions[m].resize(vertexCount * 3);
        normals[m].resize(vertexCount * 3);
        texCoords[m].resize(vertexCount * 2);
        for (size_t v = 0; v < vertexCount; ++v)
        {
            const Vertex& vertex = source.Vertices[v];
            positions[m][v * 3 + 0] = vertex.Position[0];
            positions[m][v * 3 + 1] = vertex.Position[1];
            positions[m][v * 3 + 2] = vertex.Position[2];
            normals[m][v * 3 + 0]   = vertex.Normal[0];
            normals[m][v * 3 + 1]   = vertex.Normal[1];
            normals[m][v * 3 + 2]   = vertex.Normal[2];
            texCoords[m][v * 2 + 0] = vertex.TexCoords[0];
            texCoords[m][v * 2 + 1] = vertex.TexCoords[1];
        }
        // TriangleMaterials is deliberately left empty: the builder defaults
        // every triangle to local index 0 and the BVH pool stamps the mesh's
        // global uber-material slot over it (one material per submesh).
        soups[m] = SceneBvh::MeshGeometryView{positions[m], normals[m], texCoords[m],
                                              source.Indices, {}};
    }

    std::vector<SceneBvh::ThreadedBvh> bvhs(meshCount);
    SceneBvh::ThreadedBvhBuilder::BuildMany(&EngineCore::GetInstance().GetJobSystem(), soups, bvhs);

    // The shared BLAS stamps its triangles with the mesh's default material;
    // each instance then overrides it with its own slot below, so two instances
    // of one mesh shade with two materials off ONE tree.
    std::vector<uint32_t> meshDefaultMaterialSlots(meshCount);
    for (size_t m = 0; m < meshCount; ++m)
        meshDefaultMaterialSlots[m] = m_PlanMeshes[m].DefaultMaterialSlot;

    const SceneBvh::PooledThreadedBvh pooled = PoolThreadedBvhs(bvhs, meshDefaultMaterialSlots);

    std::vector<Mathematics::AABB> localBounds(meshCount);
    for (size_t m = 0; m < meshCount; ++m)
        localBounds[m] = bvhs[m].LocalBounds;

    // Skinned ranges for the per-tick GPU refit, with vertex bases re-derived
    // by the pool's own running-sum rule (VertexData appended verbatim, in
    // input order). A skinned instance's TLAS entry gets INFLATED local
    // bounds: the CPU-packed TLAS holds build-time (bind pose) boxes and is
    // not repacked per pose, so the instance box must conservatively contain
    // every reachable pose — a miss there is a wrong trace, not a slow one.
    m_SkinnedRefitRanges.clear();
    {
        uint32_t vertexBase = 0;
        for (size_t m = 0; m < meshCount; ++m)
        {
            const uint32_t vertexCount = bvhs[m].VertexCount();
            if (m_PlanMeshes[m].SkinnedRuntimeId != 0u && !pooled.Ranges[m].IsEmpty())
            {
                SkinnedRefitRange range;
                range.SkinnedRuntimeId = m_PlanMeshes[m].SkinnedRuntimeId;
                range.GpuMeshIndex = m_PlanMeshes[m].GpuMeshIndex;
                range.NodeBegin = pooled.Ranges[m].NodeBegin;
                range.NodeCount = pooled.Ranges[m].NodeEnd - pooled.Ranges[m].NodeBegin;
                range.VertexBegin = vertexBase;
                range.VertexCount = vertexCount;
                m_SkinnedRefitRanges.push_back(range);

                constexpr float kSkinnedBoundsScale = 1.5f;
                const Mathematics::Vector3 centre =
                    (localBounds[m].min + localBounds[m].max) * 0.5f;
                const Mathematics::Vector3 half =
                    (localBounds[m].max - localBounds[m].min) * (0.5f * kSkinnedBoundsScale);
                localBounds[m] = Mathematics::AABB{centre - half, centre + half};
            }
            vertexBase += vertexCount;
        }
    }

    const SceneBvh::PackedTlas packed =
        SceneBvh::TlasPacker::Pack(m_PlanMaterials, BuildTlasInstances(pooled.Ranges, localBounds));
    if (packed.IsEmpty() || pooled.Nodes.empty())
    {
        m_BuiltBlasRanges.clear();
        m_BuiltLocalBounds.clear();
        ReleaseBuffers();
        return;
    }

    ReleaseBuffers();
    const bool uploaded =
        UploadBuffer(m_Buffers.PackedScene, packed.Buffer.data(),
                     packed.Buffer.size() * sizeof(float), "DDGI.Sw.PackedScene") &&
        UploadBuffer(m_Buffers.Nodes, pooled.Nodes.data(),
                     pooled.Nodes.size() * sizeof(uint32_t), "DDGI.Sw.Nodes") &&
        UploadBuffer(m_Buffers.TriangleIndices, pooled.TriangleIndices.data(),
                     pooled.TriangleIndices.size() * sizeof(uint32_t), "DDGI.Sw.TriangleIndices") &&
        UploadBuffer(m_Buffers.TriangleMaterials, pooled.TriangleMaterials.data(),
                     pooled.TriangleMaterials.size() * sizeof(uint32_t),
                     "DDGI.Sw.TriangleMaterials") &&
        UploadBuffer(m_Buffers.VertexData, pooled.VertexData.data(),
                     pooled.VertexData.size() * sizeof(float), "DDGI.Sw.VertexData");
    if (!uploaded)
    {
        Logger::Log::Error("DDGISceneService: software scene upload failed; lane stays inactive");
        m_BuiltBlasRanges.clear();
        m_BuiltLocalBounds.clear();
        ReleaseBuffers();
        return;
    }

    m_Buffers.PackedSceneBytes       = packed.Buffer.size() * sizeof(float);
    m_Buffers.NodesBytes             = pooled.Nodes.size() * sizeof(uint32_t);
    m_Buffers.TriangleIndicesBytes   = pooled.TriangleIndices.size() * sizeof(uint32_t);
    m_Buffers.TriangleMaterialsBytes = pooled.TriangleMaterials.size() * sizeof(uint32_t);
    m_Buffers.VertexDataBytes        = pooled.VertexData.size() * sizeof(float);
    m_Buffers.TlasNodeCount          = packed.TlasNodeCount;
    m_Buffers.InstanceBase           = packed.InstanceBase;
    m_Buffers.TlasBase               = packed.TlasBase;
    m_ParticipatingInstanceCount     = packed.InstanceCount;

    m_BuiltBlasRanges = pooled.Ranges;
    m_BuiltLocalBounds = std::move(localBounds);
    m_BuiltMaterials = m_PlanMaterials;
    m_PendingMaterials.clear();
#if GE_ENABLE_METRICS
    Debug::DebugMetrics::Get().PushSample("DDGI/Software/GeometryRebuilds",
        static_cast<float>(s_SoftwareGeometryRebuilds.fetch_add(1) + 1));
#endif
    Logger::Log::Debug("DDGI: rebuilt software scene geometry ({} meshes, {} instances)",
                       meshCount, m_ParticipatingInstanceCount);
}

std::vector<SceneBvh::TlasInstance> DDGISceneService::BuildTlasInstances(
    const std::vector<SceneBvh::PooledBvhRange>& ranges, const std::vector<Mathematics::AABB>& localBounds) const
{
    std::vector<SceneBvh::TlasInstance> tlasInstances;
    tlasInstances.reserve(m_PlanParticipants.size());
    for (const Participant& participant : m_PlanParticipants)
    {
        const SceneBvh::PooledBvhRange& range = ranges[participant.PlanMeshSlot];
        if (range.IsEmpty())
            continue;  // mesh built empty, or the pool refused it
        SceneBvh::TlasInstance instance;
        instance.WorldFromLocal = participant.WorldFromLocal;
        instance.LocalBounds    = localBounds[participant.PlanMeshSlot];
        instance.BlasRoot       = range.NodeBegin;
        instance.BlasEnd        = range.NodeEnd;
        instance.MaterialSlot   = participant.MaterialSlot;
        instance.GIEmitter      = participant.GIEmitter;
        tlasInstances.push_back(instance);
    }
    return tlasInstances;
}

bool DDGISceneService::RepackInstancesFromPlan()
{
    // The cached pool describes the plan mesh set the last full rebuild saw.
    // A matching structure hash is what says the current plan is that same
    // set, so a size mismatch here means the two got out of step and the
    // cheap path must not be trusted with it.
    if (m_BuiltBlasRanges.size() != m_PlanMeshes.size() || m_BuiltBlasRanges.empty())
        return false;
    if (!m_Buffers.IsValid())
        return false;

    const SceneBvh::PackedTlas packed = SceneBvh::TlasPacker::Pack(
        m_PlanMaterials, BuildTlasInstances(m_BuiltBlasRanges, m_BuiltLocalBounds));
    if (packed.IsEmpty())
        return false;

    // Only the packed buffer is rewritten. Nodes, triangle indices, triangle
    // materials and vertex data are all local-space BLAS content that no
    // instance transform can touch, so their handles — and any descriptor
    // already written against them — stay valid.
    if (!UploadBuffer(m_Buffers.PackedScene, packed.Buffer.data(),
                      packed.Buffer.size() * sizeof(float), "DDGI.Sw.PackedScene"))
    {
        // Dropping the cache sends the caller down the full-rebuild path,
        // which re-creates every buffer — the only recovery available when the
        // packed buffer is the one that failed to allocate.
        m_BuiltBlasRanges.clear();
        m_BuiltLocalBounds.clear();
        ReleaseBuffers();
        return false;
    }

    m_Buffers.PackedSceneBytes   = packed.Buffer.size() * sizeof(float);
    m_Buffers.TlasNodeCount      = packed.TlasNodeCount;
    m_Buffers.InstanceBase       = packed.InstanceBase;
    m_Buffers.TlasBase           = packed.TlasBase;
    m_ParticipatingInstanceCount = packed.InstanceCount;
    m_BuiltMaterials = m_PlanMaterials;
    m_PendingMaterials.clear();
    return true;
}

bool DDGISceneService::QueueMaterialUpdatesFromPlan()
{
    if (!m_Buffers.IsValid() || m_BuiltMaterials.empty())
        return false;
    std::vector<DDGIMaterialUpdateRange> ranges;
    if (!PlanDDGIMaterialUpdates(m_BuiltMaterials, m_PlanMaterials, ranges))
        return false;
    // Compare against the last submitted table, not a pending edit: if an
    // upload allocation fails, a newer edit must retain all unsent changes.
    m_PendingMaterials = m_PlanMaterials;
    return true;
}

bool DDGISceneService::DeclareMaterialUploads(Rendering::RenderGraph::RGFrame& frame,
                                              Rendering::RenderGraph::RGBuffer packedScene)
{
    namespace RG = Rendering::RenderGraph;
    if (m_PendingMaterials.empty())
        return true;
    std::vector<DDGIMaterialUpdateRange> ranges;
    if (!packedScene.IsValid() ||
        !PlanDDGIMaterialUpdates(m_BuiltMaterials, m_PendingMaterials, ranges))
        return false;

    struct Copy
    {
        Rendering::BufferHandle Source;
        uint64_t SourceOffset;
        uint64_t DestinationOffset;
        uint64_t Bytes;
    };
    std::vector<Copy> copies;
    uint64_t recordCount = 0;
    for (const auto& range : ranges)
        recordCount += range.Count;
    if (recordCount > 0)
    {
        const auto upload = frame.AllocUpload(recordCount * sizeof(SceneBvh::UberMaterial));
        if (!upload.Ptr)
            return false;
        const auto source = frame.ImportExternalBuffer(
            "DDGI.Sw.MaterialUpload", upload.Buffer,
            upload.Offset + recordCount * sizeof(SceneBvh::UberMaterial));
        uint64_t offset = 0;
        for (const auto& range : ranges)
        {
            const uint64_t bytes = range.Count * sizeof(SceneBvh::UberMaterial);
            std::memcpy(static_cast<char*>(upload.Ptr) + offset,
                        m_PendingMaterials.data() + range.First, bytes);
            copies.push_back({upload.Buffer, upload.Offset + offset,
                              range.First * sizeof(SceneBvh::UberMaterial), bytes});
            offset += bytes;
        }
        frame.AddPass("DDGI.Sw.UpdateMaterials", Rendering::PassPhase::kEarlySetup,
            [source, packedScene](RG::RGPassBuilder& pass)
            {
                pass.PreventCulling();
                pass.Read(source, RG::RGBufferRead::CopySrc);
                pass.Write(packedScene, RG::RGBufferWrite::CopyDst);
            },
            [copies = std::move(copies), packedScene](RG::RGContext& ctx)
            {
                for (const auto& copy : copies)
                    ctx.Cmd->CopyBuffer(copy.Source, ctx.GetBuffer(packedScene), copy.Bytes,
                                        copy.SourceOffset, copy.DestinationOffset);
            });
#if GE_ENABLE_METRICS
        Debug::DebugMetrics::Get().PushSample("DDGI/Software/MaterialPatches",
            static_cast<float>(s_SoftwareMaterialPatches.fetch_add(1) + 1));
#endif
        Logger::Log::Debug("DDGI: patched {} software material records in {} ranges",
                           recordCount, ranges.size());
    }
    m_BuiltMaterials = std::move(m_PendingMaterials);
    m_PendingMaterials.clear();
    return true;
}

void DDGISceneService::ReleaseBuffers()
{
    for (Rendering::BufferHandle* buffer :
         {&m_Buffers.PackedScene, &m_Buffers.Nodes, &m_Buffers.TriangleIndices,
          &m_Buffers.TriangleMaterials, &m_Buffers.VertexData})
    {
        if (buffer->IsValid())
            m_RetiredBuffers.push_back(RetiredBuffer{*buffer, m_FrameClock});
    }
    m_Buffers = SceneBuffers{};
    m_BuiltMaterials.clear();
    m_PendingMaterials.clear();
}

bool DDGISceneService::UploadBuffer(Rendering::BufferHandle& buffer, const void* data,
                                    uint64_t bytes, const char* debugName)
{
    if (buffer.IsValid())
    {
        m_RetiredBuffers.push_back(RetiredBuffer{buffer, m_FrameClock});
        buffer = {};
    }
    if (bytes == 0 || !data)
        return false;

    Rendering::BufferDesc desc{};
    desc.size        = static_cast<size_t>(bytes);
    desc.usage       = static_cast<uint32_t>(Rendering::BufferUsage::Storage) |
                       static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    desc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    desc.persistent  = true;
    desc.debugName   = debugName;
    buffer           = m_Device->CreateBuffer(desc);
    if (!buffer.IsValid())
        return false;

    m_Device->UpdateBuffer(buffer, 0, static_cast<size_t>(bytes), data);
    return true;
}

}  // namespace GameEngine::Engine::Renderer
