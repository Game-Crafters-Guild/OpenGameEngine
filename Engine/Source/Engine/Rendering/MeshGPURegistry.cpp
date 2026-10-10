// MeshGPURegistry implementation: upload, dedup, and lifetime management
// for GPU-resident mesh buffers keyed by (asset GUID, submesh index).

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Assets/AssetDbProfiler.h"
#include "Assets/ModelAsset.h"
#include "Assets/MeshLODGeometry.h"
#include "Assets/MeshLODGenerator.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h" // IndexType
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/HashUtils.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>

namespace
{

// Initial commit per bucket stream pool. Streams grow by adding sibling pools
// so existing entries keep stable BufferHandles/offsets while later large
// meshes can still upload.
constexpr uint64_t kBucketCoreInitialBytes  = 16ull << 20; // 16 MB
constexpr uint64_t kBucketOtherInitialBytes = 8ull  << 20; // 8 MB
constexpr uint64_t kBucketIndexInitialBytes = 4ull  << 20; // 4 MB

// Per-stream alignment MUST equal that stream's stride. The draw call
// binds a single vertexOffset (resp. firstIndex) that is applied across
// every bound VB (resp. the IB). For mesh B's offset to be consistent
// across all streams -- e.g. coreOffset/32 == jointsOffset/8 ==
// firstIndex/2 -- each allocator must produce offsets that are exact
// multiples of *its* stride. Using a uniform 16-byte alignment for an
// 8-byte-stride stream injects 8-byte padding between meshes, which
// reads as a phantom vertex slot on the draw and corrupts skinning
// (and similar for IBs).

// kMaxFramesInFlight is mirrored from IDevice::kMaxSupportedFramesInFlight.
// MeshGPURegistry does not depend on the device's exact value at compile
// time; using a small constant here is safe because the value is used
// only as a lower bound on the deferred-free retire-after delta. Higher
// is always safe.
constexpr uint64_t kRetireDelay = 4ull;

// WebGPU Queue.WriteBuffer requires the destination offset AND the byte size to
// be multiples of 4 (Vulkan is looser). Index pools stride at 2 bytes (Uint16),
// which yields 2-aligned offsets and, for an odd index count, a 2-mod-4 size.
// 4 is a multiple of both index strides (2 and 4), so firstIndex = offset /
// indexStride stays an exact integer; the alignment only pads by <= 2 bytes.
constexpr uint32_t kIndexUploadAlignmentBytes = 4u;

constexpr std::array<GameEngine::Rendering::VertexAttributeFlags, 6> kExtraUvFlags = {
    GameEngine::Rendering::VertexAttributeFlags::HasUV2,
    GameEngine::Rendering::VertexAttributeFlags::HasUV3,
    GameEngine::Rendering::VertexAttributeFlags::HasUV4,
    GameEngine::Rendering::VertexAttributeFlags::HasUV5,
    GameEngine::Rendering::VertexAttributeFlags::HasUV6,
    GameEngine::Rendering::VertexAttributeFlags::HasUV7,
};

constexpr std::array<const char*, 6> kExtraUvDebugNames = {
    "MeshGPU.Bucket.UV2VB",
    "MeshGPU.Bucket.UV3VB",
    "MeshGPU.Bucket.UV4VB",
    "MeshGPU.Bucket.UV5VB",
    "MeshGPU.Bucket.UV6VB",
    "MeshGPU.Bucket.UV7VB",
};

GameEngine::Rendering::PrimitiveTopology ToRenderingTopology(GameEngine::MeshPrimitiveTopology topology)
{
    using GameEngine::MeshPrimitiveTopology;
    switch (topology)
    {
    case MeshPrimitiveTopology::Points: return GameEngine::Rendering::PrimitiveTopology::PointList;
    case MeshPrimitiveTopology::Lines:  return GameEngine::Rendering::PrimitiveTopology::LineList;
    case MeshPrimitiveTopology::Triangles:
    default:                            return GameEngine::Rendering::PrimitiveTopology::TriangleList;
    }
}

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    if (alignment <= 1)
        return value;
    return ((value + alignment - 1u) / alignment) * alignment;
}

uint64_t NextPoolCapacity(const std::vector<GameEngine::Rendering::MeshGPUStreamPool>& pools,
                          uint64_t initialCapacity,
                          uint64_t requestedBytes,
                          uint64_t alignmentBytes)
{
    const uint64_t requestedAligned = AlignUp(requestedBytes, alignmentBytes);
    uint64_t capacity = std::max(initialCapacity, requestedAligned);
    if (!pools.empty())
    {
        const uint64_t lastCapacity = pools.back().capacityBytes;
        const uint64_t doubled = lastCapacity > (std::numeric_limits<uint64_t>::max() / 2u)
            ? std::numeric_limits<uint64_t>::max()
            : lastCapacity * 2u;
        capacity = std::max(capacity, doubled);
    }
    return capacity;
}

void ReclaimPools(std::vector<GameEngine::Rendering::MeshGPUStreamPool>& pools, uint64_t frameIndex)
{
    for (auto& pool : pools)
        pool.allocator.ReclaimUpToFrame(frameIndex);
}

void DestroyPools(GameEngine::Rendering::IDevice* device,
                  std::vector<GameEngine::Rendering::MeshGPUStreamPool>& pools)
{
    if (!device)
        return;
    for (auto& pool : pools)
    {
        if (pool.buffer.IsValid())
        {
            device->DestroyBuffer(pool.buffer);
            pool.buffer = {};
        }
    }
    pools.clear();
}

GameEngine::Rendering::MeshGPUStreamPool* GetPool(
    std::vector<GameEngine::Rendering::MeshGPUStreamPool>& pools,
    uint32_t index)
{
    if (index == GameEngine::Rendering::kInvalidMeshGPUPoolIndex ||
        index >= static_cast<uint32_t>(pools.size()))
    {
        return nullptr;
    }
    return &pools[index];
}

const GameEngine::Rendering::MeshGPUStreamPool* GetPool(
    const std::vector<GameEngine::Rendering::MeshGPUStreamPool>& pools,
    uint32_t index)
{
    if (index == GameEngine::Rendering::kInvalidMeshGPUPoolIndex ||
        index >= static_cast<uint32_t>(pools.size()))
    {
        return nullptr;
    }
    return &pools[index];
}

GameEngine::Rendering::BufferHandle GetPoolBuffer(
    const std::vector<GameEngine::Rendering::MeshGPUStreamPool>& pools,
    uint32_t index)
{
    if (const auto* pool = GetPool(pools, index))
        return pool->buffer;
    return {};
}

} // namespace

namespace GameEngine
{
namespace Rendering
{

#if GE_DEBUG_INSTRUMENTATION
thread_local uint32_t MeshGPURegistry::s_LocalScopeDepth = 0;
#endif

MeshGPURegistry::TableScope::TableScope(const MeshGPURegistry& registry)
    : m_Registry(registry)
{
    m_Registry.m_TableMutex.lock();
#if GE_DEBUG_INSTRUMENTATION
    m_Registry.m_ScopesHeld.fetch_add(1u, std::memory_order_release);
    ++s_LocalScopeDepth;
#endif
}

MeshGPURegistry::TableScope::~TableScope()
{
#if GE_DEBUG_INSTRUMENTATION
    --s_LocalScopeDepth;
    m_Registry.m_ScopesHeld.fetch_sub(1u, std::memory_order_release);
#endif
    m_Registry.m_TableMutex.unlock();
}

#if GE_DEBUG_INSTRUMENTATION
void MeshGPURegistry::AssertNotRacingMutation() const
{
    assert((s_LocalScopeDepth > 0 || m_ScopesHeld.load(std::memory_order_acquire) == 0u) &&
           "MeshGPURegistry: lock-free lookup ran while another thread held the tables. "
           "A caller that reads the registry from inside an ECS wave must hold a "
           "MeshGPURegistry::TableScope across the lookup AND the use of what it returns.");
}
#endif

void MeshGPURegistry::Initialize(IDevice* device)
{
    assert(device && "MeshGPURegistry requires a valid IDevice");
    TableScope scope(*this);
    m_Device = device;
}

void MeshGPURegistry::Shutdown()
{
    TableScope scope(*this);

    // All entry-level GPU storage lives in the bucket pools (destroyed
    // below). Entries themselves carry only suballocation offsets +
    // metadata, no GPU handles of their own.
    m_KeyToHandle.clear();
    m_ModelHandles.clear();
    m_Entries.Clear();
    ++m_ContentRevision;

    // Destroy all bucket pool VkBuffers.
    if (m_Device)
    {
        for (auto& [bkey, bucket] : m_Buckets)
        {
            DestroyPools(m_Device, bucket.corePools);
            DestroyPools(m_Device, bucket.tangentPools);
            DestroyPools(m_Device, bucket.colorPools);
            DestroyPools(m_Device, bucket.uv1Pools);
            DestroyPools(m_Device, bucket.jointsPools);
            DestroyPools(m_Device, bucket.weightsPools);
            DestroyPools(m_Device, bucket.joints1Pools);
            DestroyPools(m_Device, bucket.weights1Pools);
            for (auto& pools : bucket.extraUvPools)
                DestroyPools(m_Device, pools);
            DestroyPools(m_Device, bucket.ib16Pools);
            DestroyPools(m_Device, bucket.ib32Pools);
        }
    }
    m_Buckets.clear();

    m_CurrentFrameIndex = 0;
    m_Device   = nullptr;
    m_GPUScene = nullptr;
}

void MeshGPURegistry::BeginFrame(uint64_t frameIndex)
{
    TableScope scope(*this);

    // Monotonic guard: tolerate same-frame repeated calls but flag a
    // regression if a caller tries to rewind.
    assert(frameIndex >= m_CurrentFrameIndex && "MeshGPURegistry::BeginFrame went backwards");
    m_CurrentFrameIndex = frameIndex;

    // Owner of the per-frame residency counters' reset. The process totals and
    // the window-exhaustion count are deliberately not cleared — they are what
    // a single poll can be read against.
    m_RouteBRefusedNonResident.store(0u, std::memory_order_relaxed);
    m_RouteBRefusedBucketMissing.store(0u, std::memory_order_relaxed);

    for (auto& [bkey, bucket] : m_Buckets)
    {
        ReclaimPools(bucket.corePools, frameIndex);
        ReclaimPools(bucket.tangentPools, frameIndex);
        ReclaimPools(bucket.colorPools, frameIndex);
        ReclaimPools(bucket.uv1Pools, frameIndex);
        ReclaimPools(bucket.jointsPools, frameIndex);
        ReclaimPools(bucket.weightsPools, frameIndex);
        ReclaimPools(bucket.joints1Pools, frameIndex);
        ReclaimPools(bucket.weights1Pools, frameIndex);
        for (auto& pools : bucket.extraUvPools)
            ReclaimPools(pools, frameIndex);
        ReclaimPools(bucket.ib16Pools, frameIndex);
        ReclaimPools(bucket.ib32Pools, frameIndex);
    }
}

std::vector<MeshGPUHandle> MeshGPURegistry::RegisterModelMeshes(
    const GUID& assetGuid,
    const ModelAsset& asset)
{
    TableScope scope(*this);

    // Fast path: already registered?
    auto itModel = m_ModelHandles.find(assetGuid);
    if (itModel != m_ModelHandles.end())
    {
        return itModel->second;
    }

    using Clock = std::chrono::high_resolution_clock;
    const auto tStart = Clock::now();

    const uint32_t meshCount = asset.GetMeshCount();
    std::vector<MeshGPUHandle> handles;
    handles.reserve(meshCount);

    for (uint32_t i = 0; i < meshCount; ++i)
    {
        MeshGPUKey key{assetGuid, i};
        MeshGPUHandle h = RegisterSubmesh(key, asset.GetMesh(i));
        handles.push_back(h);
    }

    m_ModelHandles[assetGuid] = handles;

    const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    uint32_t totalVerts = 0;
    for (uint32_t i = 0; i < meshCount; ++i)
        totalVerts += static_cast<uint32_t>(asset.GetMesh(i).Vertices.size());
    Logger::Log::Trace(
        "[ModelLoad]   MeshUpload: {} submeshes, {} verts: {:.1f}ms",
        meshCount, totalVerts, totalMs);

    return handles;
}

// FNV-1a hash over everything UploadMesh consumes, so a dedup hit can detect an
// in-place content change (LOD generation, geometry edit) the {assetGuid,
// submeshIndex} key cannot see. False positives only cost a redundant re-upload;
// there are no false negatives for the streams covered here.
static uint64_t ComputeMeshContentHash(const Mesh& mesh)
{
    using namespace HashUtils;
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    h = HashValue(h, static_cast<uint32_t>(mesh.PrimitiveTopology));
    h = HashValue(h, mesh.MinBounds);
    h = HashValue(h, mesh.MaxBounds);
    h = HashValue(h, static_cast<uint64_t>(mesh.Vertices.size()));
    if (!mesh.Vertices.empty())
        h = Fnv1a64(mesh.Vertices.data(), mesh.Vertices.size() * sizeof(Vertex), h);
    h = HashVecU32(h, mesh.Indices);
    h = HashValue(h, static_cast<uint64_t>(mesh.ExtraLODs.size()));
    for (const auto& lod : mesh.ExtraLODs)
        h = HashVecU32(h, lod);
    // Cover the error carrier so an errors-only retune (same indices, different
    // achieved error after a generator change) re-seeds the derived thresholds
    // through the content-hash re-upload path.
    h = HashValue(h, static_cast<uint64_t>(mesh.ExtraLODErrors.size()));
    if (!mesh.ExtraLODErrors.empty())
        h = Fnv1a64(mesh.ExtraLODErrors.data(),
                    mesh.ExtraLODErrors.size() * sizeof(float), h);
    if (!mesh.ExtraLODSloppy.empty())
        h = Fnv1a64(mesh.ExtraLODSloppy.data(),
                    mesh.ExtraLODSloppy.size() * sizeof(uint8), h);
    // Per-LOD vertex blocks and switch coverages (Phase C1; blocks also carry
    // generated attribute-honest shells now). Without these, a vertex-block or
    // coverage edit would re-upload stale geometry / stale thresholds under the
    // same handle on a dedup hit — the exact stale-under-same-handle class the
    // error/index hashing above guards. Both hash as "size 0" when empty (every
    // index-only mesh), a constant term that keeps that path's dedup behavior
    // unperturbed. The authored provenance flag folds too: it selects the
    // threshold table (authored default vs sloppy-capped derived), so identical
    // bytes with different provenance must not dedup into one row.
    h = HashValue(h, mesh.AuthoredLODs);
    h = HashValue(h, static_cast<uint64_t>(mesh.ExtraLODVertices.size()));
    for (const auto& block : mesh.ExtraLODVertices)
    {
        h = HashValue(h, static_cast<uint64_t>(block.size()));
        if (!block.empty())
            h = Fnv1a64(block.data(), block.size() * sizeof(Vertex), h);
    }
    h = HashValue(h, static_cast<uint64_t>(mesh.ExtraLODCoverage.size()));
    if (!mesh.ExtraLODCoverage.empty())
        h = Fnv1a64(mesh.ExtraLODCoverage.data(),
                    mesh.ExtraLODCoverage.size() * sizeof(float), h);
    auto hashFloats = [&h](const Vector<float>& v) {
        h = HashValue(h, static_cast<uint64_t>(v.size()));
        if (!v.empty()) h = Fnv1a64(v.data(), v.size() * sizeof(float), h);
    };
    auto hashU16 = [&h](const Vector<uint16>& v) {
        h = HashValue(h, static_cast<uint64_t>(v.size()));
        if (!v.empty()) h = Fnv1a64(v.data(), v.size() * sizeof(uint16), h);
    };
    hashFloats(mesh.Color0);
    if (!mesh.ExtraLODColor0.empty()) {
        h = HashValue(h, static_cast<uint64_t>(mesh.ExtraLODColor0.size()));
        for (const auto& colours : mesh.ExtraLODColor0) hashFloats(colours);
    }
    hashFloats(mesh.TexCoords1);
    h = HashValue(h, static_cast<uint64_t>(mesh.ExtraTexCoords.size()));
    for (const auto& uv : mesh.ExtraTexCoords) hashFloats(uv);
    hashU16(mesh.Joints0);  hashFloats(mesh.Weights0);
    hashU16(mesh.Joints1);  hashFloats(mesh.Weights1);
    h = HashValue(h, mesh.Skinned);
    h = HashValue(h, static_cast<uint64_t>(mesh.MorphTargets.size()));
    return h;
}

// Build the std430 GPUMesh SSBO row from an uploaded entry. lodIndexOffset[0]/
// lodIndexCount[0] mirror the legacy indexOffset/indexCount so non-LOD consumers
// keep working; draw_command_scatter.comp selects a LOD and emits that range.
static GPUMesh BuildGpuMeshRow(const MeshGPUEntry& entry, LodSelectionMode lodMode)
{
    static_assert(MeshGPUEntry::kMaxEntryLODs == kMaxMeshLODs,
                  "MeshGPUEntry LOD capacity must match GPUMesh LOD capacity");
    GPUMesh row{};
    row.vertexOffset   = entry.vertexOffset;
    // vertexCount isn't tracked on the entry today; consumers infer via indices.
    row.vertexCount    = 0;
    row.indexOffset    = entry.firstIndex;
    row.indexCount     = entry.indexCount;
    row.boundingCenter = Vector3(entry.bounds.center.x,
                                 entry.bounds.center.y,
                                 entry.bounds.center.z);
    row.boundingRadius = entry.bounds.Radius();
    row.lodCoverageScale = row.boundingRadius > 0.0f
        ? entry.lodReferenceRadius / row.boundingRadius : 1.0f;
    row.bucketKey      = static_cast<uint32_t>(entry.bucketKey);
    row.indexType      = entry.indexType;
    row.vertexFlags    = static_cast<uint32_t>(entry.vertexFlags);
    row.lodCount       = entry.lodCount;
    static_assert(sizeof(MeshGPURegistry::kDefaultLODThresholds) / sizeof(float) == MeshGPUEntry::kMaxEntryLODs,
                  "LOD threshold table must cover every GPUMesh LOD slot");
    for (uint32_t k = 0; k < MeshGPUEntry::kMaxEntryLODs; ++k)
    {
        const bool present = k < entry.lodCount;
        row.lodIndexOffset[k]  = present ? entry.lodFirstIndex[k]   : 0u;
        row.lodIndexCount[k]   = present ? entry.lodIndexCount[k]   : 0u;
        // Zero for every index-only chain (entry.lodVertexOffset stays 0), so
        // those rows are byte-identical to pre-C1 in the reserved-tail bytes
        // 104..120 (the zero-diff guarantee / ElvenRealm oracle). Non-zero for
        // own-vertex levels — authored or generated attribute-honest shells.
        row.lodVertexOffset[k] = present ? entry.lodVertexOffset[k] : 0u;
    }
    // Off outranks every mapping, artist-authored coverages included: selection
    // is off for the whole scene or it is not off at all. Otherwise authored
    // chains carry no meshopt error, so an artist-supplied coverage seeds the
    // switch points directly (Phase C2 direct-coverage path) and a no-coverage
    // authored chain reproduces the default table (explicit lodAuthored
    // provenance); every other mesh derives its switch points from the achieved
    // simplify errors under `lodMode` (MeshLODThresholds.h), falling back to the
    // global defaults for no-data / sloppy / coarsest / absent slots. Authored
    // rows keep lodFlags == 0 in every mode — byte-identical selection to the
    // pre-SSE path.
    if (lodMode == LodSelectionMode::Off)
    {
        DeriveLODThresholdsOff(MeshGPUEntry::kMaxEntryLODs, row.lodThreshold);
    }
    else if (entry.lodCoverageCount > 0)
    {
        ApplyAuthoredLODCoverage(entry.lodCoverage, entry.lodCoverageCount,
                                 entry.lodCount, MeshGPUEntry::kMaxEntryLODs,
                                 row.lodThreshold);
    }
    else if (lodMode == LodSelectionMode::Coverage)
    {
        // Reference arm: the pre-SSE mapping, every slot in coverage space, so
        // lodFlags stays 0 and the scatter's convert-and-clamp step is inert.
        DeriveLODThresholdsCoverage(
            entry.lodError, entry.lodSloppy, entry.lodCount, entry.lodAuthored,
            MeshGPURegistry::kDefaultLODThresholds, MeshGPUEntry::kMaxEntryLODs,
            row.lodThreshold);
    }
    else
    {
        // Meshopt errors are normalized to the max AABB axis; the SSE mapping
        // uses the original reference metric, independently of culling growth.
        const float maxExtent = entry.lodReferenceMaxExtent;
        row.lodFlags = DeriveLODThresholds(
            entry.lodError, entry.lodSloppy, entry.lodCount, entry.lodAuthored,
            entry.lodReferenceRadius, maxExtent, MeshGPURegistry::kDefaultLODThresholds,
            MeshGPUEntry::kMaxEntryLODs, row.lodThreshold);
        // Only a row carrying an SSE slot has anything for the ceiling to
        // clamp; leaving it 0 elsewhere keeps the tail word zero for authored
        // and coverage-space rows.
        if (row.lodFlags != 0u)
            row.lodSseScaleCeil = LodSseScaleCeil(entry.lodReferenceRadius, maxExtent);
        // Skinned/character chains take the tighter per-view budget — their
        // erosion is salient well below the prop budget (the per-class ask).
        const bool skinned =
            (entry.vertexFlags & (VertexAttributeFlags::HasJoints |
                                  VertexAttributeFlags::HasWeights)) !=
            VertexAttributeFlags::None;
        if (row.lodFlags != 0u && skinned)
            row.lodFlags |= kGPUMeshLodTightClassBit;
    }
    return row;
}

// The content hash to store for an entry UploadMesh just produced.
//
// An upload that did not complete (pool allocation failed, or the residency
// window was exhausted) leaves an entry that can never draw. Storing the real
// hash on it would make the next byte-identical RegisterSubmesh hit the dedup
// fast path and return that entry unchanged — a permanently invisible mesh, the
// same trap the device-rebuild tombstone defends against. 0 is this registry's
// existing "no content hash" sentinel, shared with that tombstone path: it
// forces the content-changed re-upload path, so the next registration heals the
// key. The price of overloading 0: a real mesh whose FNV-1a hash IS 0 matches
// the sentinel, so its registration dedups into the entry that can never draw
// and the healing does not fire for that one mesh — the trap this sentinel
// exists to close, at 2^-64. An explicit "upload completed" bit would remove
// even that; it costs a byte on every entry for a case no engine will meet.
static uint64_t ContentHashForUploadedEntry(const MeshGPUEntry& entry, uint64_t contentHash)
{
    const bool uploadCompleted =
        entry.uploadSeq != 0u && entry.bucketKey != VertexAttributeFlags::None;
    return uploadCompleted ? contentHash : 0u;
}

MeshGPUHandle MeshGPURegistry::RegisterSubmesh(const MeshGPUKey& key,
                                                const Mesh& mesh,
                                                bool retainCpuMesh)
{
    TableScope scope(*this);

    const uint64_t contentHash = ComputeMeshContentHash(mesh);

    // Dedup: already registered?
    auto it = m_KeyToHandle.find(key);
    if (it != m_KeyToHandle.end())
    {
        Handle raw(static_cast<Detail::HandleType>(it->second));
        MeshGPUEntry* existing = m_Entries.Get(raw);
        if (existing && existing->contentHash == contentHash)
        {
            // Unchanged content: reuse the cached GPU upload.
            if (retainCpuMesh && !existing->cpuMesh)
            {
                existing->cpuMesh = std::make_shared<Mesh>(mesh);
                ++m_ContentRevision;
            }
            return it->second;
        }
        if (existing)
        {
            // Content changed in place (e.g. LOD generation). Re-upload into the
            // SAME handle slot and update the GPUScene row at its existing index
            // so entity references (handle + GPUInstance.meshIndex) stay valid.
            ReleaseEntryToBucket(*existing);
            const uint32_t oldGpuMeshIndex = existing->gpuMeshIndex;

            MeshGPUEntry updated = UploadMesh(mesh);
            updated.contentHash = ContentHashForUploadedEntry(updated, contentHash);
            updated.cpuMesh = retainCpuMesh ? std::make_shared<Mesh>(mesh)
                                                      : existing->cpuMesh;

            const bool needRow = m_GPUScene && updated.indexCount > 0;
            bool freedGpuRow = false;
            if (needRow && oldGpuMeshIndex != ~0u)
            {
                m_GPUScene->UpdateMesh(oldGpuMeshIndex, BuildGpuMeshRow(updated, m_LodSelectionMode));
                updated.gpuMeshIndex = oldGpuMeshIndex;
            }
            else if (needRow)
            {
                updated.gpuMeshIndex = m_GPUScene->AddMesh(BuildGpuMeshRow(updated, m_LodSelectionMode));
            }
            else if (oldGpuMeshIndex != ~0u)
            {
                m_GPUScene->RemoveMesh(oldGpuMeshIndex);
                updated.gpuMeshIndex = ~0u;
                freedGpuRow = true;
            }

            *existing = std::move(updated);
            ++m_ContentRevision;
            const MeshGPUHandle handle = it->second;

            // Valid -> sentinel: the handle stays live but its GPU mesh row
            // was just freed, and a later AddMesh may hand that row to a
            // different mesh. Consumers caching row indices only hear about
            // freed rows through the unregister notification (UnregisterModel
            // fires it) — emit the same event here so the empty-content
            // re-register shape can't leave them referencing a recycled row.
            // Notified after the entry update so subscribers observe the
            // sentinel state, and after copying the handle out of the map:
            // a re-entrant subscriber (register of a new key, unregister of
            // this one) can mutate m_KeyToHandle and invalidate the iterator.
            if (freedGpuRow)
                NotifyUnregister(key.assetGuid);
            return handle;
        }
    }

    // Upload to GPU.
    MeshGPUEntry entry = UploadMesh(mesh);
    entry.contentHash = ContentHashForUploadedEntry(entry, contentHash);
    if (retainCpuMesh)
        entry.cpuMesh = std::make_shared<Mesh>(mesh);

    // Write a GPUMesh SSBO row if a GPUScene is wired. The bucketer +
    // draw_command_scatter.comp read this table via GPUInstance.meshIndex.
    if (m_GPUScene && entry.indexCount > 0)
        entry.gpuMeshIndex = m_GPUScene->AddMesh(BuildGpuMeshRow(entry, m_LodSelectionMode));

    // Store in generational vector.
    Handle raw = m_Entries.Create(std::move(entry));
    MeshGPUHandle handle(static_cast<Detail::HandleType>(raw));

    m_KeyToHandle[key] = handle;
    ++m_ContentRevision;
    m_ModelHandles[key.assetGuid].push_back(handle);
    return handle;
}

bool MeshGPURegistry::UnregisterSubmesh(const MeshGPUKey& key)
{
    TableScope scope(*this);

    auto it = m_KeyToHandle.find(key);
    if (it == m_KeyToHandle.end())
        return false;

    const MeshGPUHandle handle = it->second;
    Handle raw(static_cast<Detail::HandleType>(handle));
    bool freedGpuRow = false;
    if (MeshGPUEntry* entry = m_Entries.Get(raw))
    {
        if (m_GPUScene && entry->gpuMeshIndex != ~0u)
        {
            m_GPUScene->RemoveMesh(entry->gpuMeshIndex);
            entry->gpuMeshIndex = ~0u;
            freedGpuRow = true;
        }
        ReleaseEntryToBucket(*entry);
        m_Entries.Destroy(raw);
    }

    m_KeyToHandle.erase(it);
    ++m_ContentRevision;

    auto itModel = m_ModelHandles.find(key.assetGuid);
    if (itModel != m_ModelHandles.end())
    {
        auto& handles = itModel->second;
        handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
        if (handles.empty())
            m_ModelHandles.erase(itModel);
    }

    // Consumers caching GPU row indices (GPUInstance.meshIndex, extraction's
    // cached submissions, the TLAS) only hear about freed rows through the
    // unregister notification, and a later AddMesh may hand the freed row to
    // a different mesh — every row-freeing path must announce it. Emitted
    // last, with no registry state touched afterward: a re-entrant
    // subscriber may mutate the containers.
    if (freedGpuRow)
        NotifyUnregister(key.assetGuid);

    return true;
}

const MeshGPUEntry* MeshGPURegistry::Find(MeshGPUHandle handle) const
{
#if GE_DEBUG_INSTRUMENTATION
    AssertNotRacingMutation();
#endif
    Handle raw(static_cast<Detail::HandleType>(handle));
    return m_Entries.Get(raw);
}

const MeshGPUEntry* MeshGPURegistry::FindByKey(const MeshGPUKey& key) const
{
    TableScope scope(*this);

    auto it = m_KeyToHandle.find(key);
    if (it == m_KeyToHandle.end())
        return nullptr;
    return Find(it->second);
}

MeshGPUHandle MeshGPURegistry::FindHandle(const MeshGPUKey& key) const
{
    TableScope scope(*this);

    auto it = m_KeyToHandle.find(key);
    return (it != m_KeyToHandle.end()) ? it->second : MeshGPUHandle{};
}

size_t MeshGPURegistry::SetLodSelectionMode(LodSelectionMode mode)
{
    TableScope scope(*this);

    if (mode == m_LodSelectionMode)
        return 0;
    m_LodSelectionMode = mode;
    if (!m_GPUScene)
        return 0;

    // Rewrite every row through the new mapping. Only lodThreshold/lodFlags can
    // differ, and the row is rebuilt from the entry rather than patched, so the
    // rest is reproduced identically and the mesh table keeps its indices.
    size_t rewritten = 0;
    m_Entries.ForEach([&](auto /*handle*/, const MeshGPUEntry& entry) {
        if (entry.gpuMeshIndex == ~0u)
            return;
        m_GPUScene->UpdateMesh(entry.gpuMeshIndex, BuildGpuMeshRow(entry, mode));
        ++rewritten;
    });
    return rewritten;
}

const MeshGPUBucket* MeshGPURegistry::GetBucket(MeshGPUBucketKey key) const
{
#if GE_DEBUG_INSTRUMENTATION
    AssertNotRacingMutation();
#endif
    auto it = m_Buckets.find(static_cast<uint32_t>(key));
    return (it == m_Buckets.end()) ? nullptr : &it->second;
}

uint32_t MeshGPURegistry::GetEntryCoreStrideBytes(const MeshGPUEntry& entry) const
{
    const MeshGPUBucket* bucket = GetBucket(entry.bucketKey);
    return bucket ? bucket->coreStrideBytes : 0u;
}

size_t MeshGPURegistry::GetBucketIndexPoolCount(MeshGPUBucketKey key, uint32_t indexType) const
{
    TableScope scope(*this);

    const MeshGPUBucket* bucket = GetBucket(key);
    if (!bucket)
        return 0u;
    const bool use16 = (indexType == static_cast<uint32_t>(IndexType::Uint16));
    return use16 ? bucket->ib16Pools.size() : bucket->ib32Pools.size();
}

bool MeshGPURegistry::IsFlushResident(const MeshGPUEntry& entry) const
{
    return m_Residency.IsResident(entry.uploadSeq);
}

MeshGPUResidencyStats MeshGPURegistry::GetResidencyStats() const
{
    MeshGPUResidencyStats stats{};
    stats.RouteBRefusedNonResident   = m_RouteBRefusedNonResident.load(std::memory_order_relaxed);
    stats.RouteBRefusedBucketMissing = m_RouteBRefusedBucketMissing.load(std::memory_order_relaxed);
    stats.RouteBRefusedNonResidentTotal =
        m_RouteBRefusedNonResidentTotal.load(std::memory_order_relaxed);
    stats.RouteBRefusedBucketMissingTotal =
        m_RouteBRefusedBucketMissingTotal.load(std::memory_order_relaxed);
    stats.WindowExhausted = m_Residency.WindowExhaustedCount();
    return stats;
}

// Extra UV sets share one pool-stream shape, so PoolIndex alone would not
// distinguish set 0's pool 0 from set 1's pool 0. The set index is carried in
// the stream name instead.
static constexpr const char* kExtraUvStreamNames[] = {"UV2", "UV3", "UV4", "UV5", "UV6", "UV7"};
static_assert(std::size(kExtraUvStreamNames) ==
                  std::tuple_size_v<decltype(MeshGPUBucket::extraUvPools)>,
              "extra UV stream names must cover every extra UV pool slot");

std::vector<MeshGPUPoolStats> MeshGPURegistry::GetPoolStats() const
{
    TableScope scope(*this);

    std::vector<MeshGPUPoolStats> stats;
    for (const auto& bucketEntry : m_Buckets)
    {
        const MeshGPUBucket& bucket = bucketEntry.second;

        auto appendStream = [&stats, &bucket](const char* stream,
                                              const std::vector<MeshGPUStreamPool>& pools)
        {
            for (size_t i = 0; i < pools.size(); ++i)
            {
                MeshGPUPoolStats entry{};
                entry.BucketKey = bucket.key;
                entry.Stream    = stream;
                entry.PoolIndex = static_cast<uint32_t>(i);
                entry.Allocator = pools[i].allocator.GetStats();
                stats.push_back(entry);
            }
        };

        appendStream("Core", bucket.corePools);
        appendStream("Tangent", bucket.tangentPools);
        appendStream("Color", bucket.colorPools);
        appendStream("UV1", bucket.uv1Pools);
        appendStream("Joints", bucket.jointsPools);
        appendStream("Weights", bucket.weightsPools);
        appendStream("Joints1", bucket.joints1Pools);
        appendStream("Weights1", bucket.weights1Pools);
        for (size_t set = 0; set < bucket.extraUvPools.size(); ++set)
            appendStream(kExtraUvStreamNames[set], bucket.extraUvPools[set]);
        appendStream("IB16", bucket.ib16Pools);
        appendStream("IB32", bucket.ib32Pools);
    }
    return stats;
}

MeshGPURegistry::Drawability MeshGPURegistry::ClassifyDrawability(const MeshGPUEntry& entry,
                                                                  MeshGPUEntryBindings& out) const
{
    // Postcondition on refusal: `out` is value-initialised and no pool handle
    // is written to it. Cleared FIRST so every refusal path below shares it —
    // the bytes escape through this parameter, and [[nodiscard]] does not
    // cover them.
    out = MeshGPUEntryBindings{};

    // Bucket-missing is tested BEFORE residency, and the order is a
    // classification rule, not a safety one — both arms refuse, and both leave
    // `out` value-initialised. An entry with no bucket owns no storage at all,
    // so its refusal is a designed answer (tombstone, failed allocation, a mesh
    // whose geometry has not been built) that steady-state operation reaches on
    // purpose. Counting those as non-resident would put routine states into the
    // one counter that must read zero, and the gate would then report a defect
    // every time it behaved correctly.
    if (entry.bucketKey == VertexAttributeFlags::None)
        return Drawability::BucketMissing;
    const MeshGPUBucket* bucket = GetBucket(entry.bucketKey);
    if (!bucket)
        return Drawability::BucketMissing;
    // An entry that HAS storage but whose bytes are not readable yet is the
    // gate proper: nothing in steady state should reach this.
    if (!m_Residency.IsResident(entry.uploadSeq))
        return Drawability::NonResident;

    out.coreVB = GetPoolBuffer(bucket->corePools, entry.corePoolIndex);
    // Optional streams: only return a handle if the entry's vertexFlags
    // owns the stream. The bucket may have OTHER meshes that own a
    // stream this mesh doesn't -- exposing those would tempt callers to
    // bind streams the pipeline didn't declare.
    if (HasFlag(entry.vertexFlags, VertexAttributeFlags::HasTangent))
        out.tangentVB = GetPoolBuffer(bucket->tangentPools, entry.tangentPoolIndex);
    if (HasFlag(entry.vertexFlags, VertexAttributeFlags::HasColor))
        out.colorVB = GetPoolBuffer(bucket->colorPools, entry.colorPoolIndex);
    if (HasFlag(entry.vertexFlags, VertexAttributeFlags::HasUV1))
        out.uv1VB = GetPoolBuffer(bucket->uv1Pools, entry.uv1PoolIndex);
    if (HasFlag(entry.vertexFlags, VertexAttributeFlags::HasJoints))
        out.jointsVB = GetPoolBuffer(bucket->jointsPools, entry.jointsPoolIndex);
    if (HasFlag(entry.vertexFlags, VertexAttributeFlags::HasWeights))
        out.weightsVB = GetPoolBuffer(bucket->weightsPools, entry.weightsPoolIndex);
    if (HasFlag(entry.vertexFlags, VertexAttributeFlags::HasJoints1))
        out.joints1VB = GetPoolBuffer(bucket->joints1Pools, entry.joints1PoolIndex);
    if (HasFlag(entry.vertexFlags, VertexAttributeFlags::HasWeights1))
        out.weights1VB = GetPoolBuffer(bucket->weights1Pools, entry.weights1PoolIndex);
    for (size_t i = 0; i < kExtraUvFlags.size(); ++i)
    {
        if (HasFlag(entry.vertexFlags, kExtraUvFlags[i]))
            out.extraUvVB[i] = GetPoolBuffer(bucket->extraUvPools[i], entry.extraUvPoolIndex[i]);
    }

    const bool use16 = (entry.indexType == static_cast<uint32_t>(IndexType::Uint16));
    out.indexBuffer = use16
        ? GetPoolBuffer(bucket->ib16Pools, entry.indexPoolIndex)
        : GetPoolBuffer(bucket->ib32Pools, entry.indexPoolIndex);
    out.indexType   = entry.indexType;

    // A bucket that exists but does not own the entry's pool indices resolves
    // to invalid handles. Refuse rather than hand those back: the promise is
    // that a true return means every handle in `out` is bindable.
    if (!out.coreVB.IsValid() || !out.indexBuffer.IsValid())
    {
        out = MeshGPUEntryBindings{};
        return Drawability::BucketMissing;
    }
    return Drawability::Drawable;
}

bool MeshGPURegistry::TryGetDrawableBindings(const MeshGPUEntry& entry,
                                             MeshGPUEntryBindings& out) const
{
    switch (ClassifyDrawability(entry, out))
    {
    case Drawability::Drawable:
        return true;
    case Drawability::NonResident:
        CountRefusedNonResident();
        return false;
    case Drawability::BucketMissing:
        CountRefusedBucketMissing();
        return false;
    }
    return false;
}

bool MeshGPURegistry::IsEntryDrawable(const MeshGPUEntry& entry) const
{
    MeshGPUEntryBindings unused{};
    return ClassifyDrawability(entry, unused) == Drawability::Drawable;
}

std::vector<MeshGPUHandle> MeshGPURegistry::GetModelHandles(const GUID& assetGuid) const
{
    TableScope scope(*this);

    auto it = m_ModelHandles.find(assetGuid);
    if (it != m_ModelHandles.end())
        return it->second;
    return {};
}

void MeshGPURegistry::UnregisterModel(const GUID& assetGuid)
{
    TableScope scope(*this);

    auto itModel = m_ModelHandles.find(assetGuid);
    if (itModel == m_ModelHandles.end())
        return;

    for (MeshGPUHandle handle : itModel->second)
    {
        Handle raw(static_cast<Detail::HandleType>(handle));
        if (MeshGPUEntry* entry = m_Entries.Get(raw))
        {
            // Clear the GPUMesh row (if one was written) before bucket
            // suballocations are deferred-freed -- prevents the SSBO from
            // pointing at a soon-to-be-reused range.
            if (m_GPUScene && entry->gpuMeshIndex != ~0u)
            {
                m_GPUScene->RemoveMesh(entry->gpuMeshIndex);
                entry->gpuMeshIndex = ~0u;
            }
            ReleaseEntryToBucket(*entry);
            m_Entries.Destroy(raw);
        }

        // Remove from dedup map (find the key that maps to this handle).
        for (auto it = m_KeyToHandle.begin(); it != m_KeyToHandle.end();)
        {
            if (it->second == handle)
                it = m_KeyToHandle.erase(it);
            else
                ++it;
        }
    }

    m_ModelHandles.erase(itModel);

    ++m_ContentRevision;

    // Notify subscribers AFTER the buffers are gone, with no registry state
    // touched afterward: a re-entrant subscriber may mutate the containers.
    NotifyUnregister(assetGuid);
}

MeshGPUReloadReport MeshGPURegistry::ReloadModelMeshes(const GUID& assetGuid, const ModelAsset& asset)
{
    TableScope scope(*this);

    MeshGPUReloadReport report{};

    if (m_ModelHandles.find(assetGuid) == m_ModelHandles.end())
        return report; // never registered: nothing resident to refresh

    const uint32_t newCount = asset.GetMeshCount();
    report.SubmeshesTotal = newCount;

    std::vector<MeshGPUHandle> handles;
    handles.reserve(newCount);
    for (uint32_t i = 0; i < newCount; ++i)
    {
        const MeshGPUKey key{assetGuid, i};

        // Snapshot the resident content hash first: RegisterSubmesh re-uploads
        // in place when the geometry differs and takes the dedup fast path when
        // it does not, and the hash is the only way to tell those apart after
        // the fact.
        //
        // A prior hash of 0 is excluded because 0 is not a hash: it is the
        // sentinel an INCOMPLETE upload stores (see ContentHashForUploadedEntry),
        // and it is the one value RegisterSubmesh's dedup can never match, so it
        // always re-uploads. Without the exclusion a submesh that failed to
        // upload twice compares 0 == 0 and would be reported as an upload this
        // pass skipped — the opposite of what it did.
        const MeshGPUEntry* before = FindByKey(key);
        const uint64_t priorHash = before ? before->contentHash : 0;

        const MeshGPUHandle handle = RegisterSubmesh(key, asset.GetMesh(i));
        handles.push_back(handle);

        const MeshGPUEntry* after = Find(handle);
        if (priorHash != 0 && after && after->contentHash == priorHash)
            ++report.SubmeshesUnchanged;
        else
            ++report.SubmeshesReuploaded;
    }

    // A re-import that dropped submeshes leaves the stale keys resident and
    // still drawing pre-reload geometry. The stale set is derived from the
    // resident KEYS, never from the handle list's length: RegisterSubmesh
    // accepts any submesh index, so a model's indices need not be dense, and
    // the list is append-ordered rather than index-addressed. A key past a gap
    // would otherwise survive here AND be dropped from the list below, leaving
    // it uploaded and drawing where UnregisterModel — which walks that same
    // list — can never reach it again. Collected before unregistering, since
    // UnregisterSubmesh mutates m_KeyToHandle; sorted so GPU rows are freed in
    // submesh order rather than hash order.
    std::vector<MeshGPUKey> stale;
    for (const auto& [key, handle] : m_KeyToHandle)
    {
        if (key.assetGuid == assetGuid && key.submeshIndex >= newCount)
            stale.push_back(key);
    }
    std::sort(stale.begin(), stale.end(),
              [](const MeshGPUKey& a, const MeshGPUKey& b) { return a.submeshIndex < b.submeshIndex; });
    for (const MeshGPUKey& key : stale)
    {
        if (UnregisterSubmesh(key))
            ++report.SubmeshesRemoved;
    }

    // Re-key the model's handle list from the reloaded submesh order. Both loops
    // above already maintain it incrementally; assigning makes the post-state
    // independent of which mix of paths ran.
    if (handles.empty())
        m_ModelHandles.erase(assetGuid);
    else
        m_ModelHandles[assetGuid] = std::move(handles);

    // Same contract as UnregisterModel: announce last, with no registry state
    // touched afterward. A model whose every submesh was byte-identical changed
    // nothing on the GPU, so it owes no invalidation.
    if (report.ChangedAnything())
        NotifyUnregister(assetGuid);

    return report;
}

MeshGPUReprovisionReport MeshGPURegistry::ReprovisionAfterDeviceRebuild(
    const std::function<MeshGPUCpuSource(const MeshGPUKey&)>& sourceLookup)
{
    TableScope scope(*this);

    MeshGPUReprovisionReport report{};
    if (!m_Device)
        return report;

    // The rebuild teardown already freed every bucket-pool VkBuffer. Drop the dead
    // pool tracking (the VkBuffers are gone, so DestroyBuffer here would just be a
    // wasted generational no-op) — but the real reason to forget them is that the
    // pools are persistently MAPPED: keeping them would leave UploadMesh writing
    // through dangling mapped pointers into freed VMA memory. The BucketPoolAllocator
    // bookkeeping (offsets) goes with it: every entry is re-uploaded below into a
    // fresh allocation, so no recorded offset is trusted across the rebuild.
    m_Buckets.clear();

    // Immediately after the pools are gone and BEFORE anything is re-uploaded.
    // Every outstanding stamp targeted storage that no longer exists, so the
    // table is discarded wholesale rather than drained — and the stamps the
    // entries still carry are cleared in the same step, because a stamp left
    // below the new base would read RESIDENT against freed memory. The
    // re-upload loop then mints fresh stamps that mean what they say.
    m_Residency.ResetAfterDeviceRebuild();
    ++m_ContentRevision;
    m_Entries.ForEach([](auto /*handle*/, MeshGPUEntry& entry) { entry.uploadSeq = 0u; });

    // The residency log latches on the first pool and then stays quiet while
    // later pools match it. Every pool that set the latch has just been freed,
    // so keeping it would suppress the one line proving where the REBUILT
    // device's pools landed — exactly the claim the log exists to support, on
    // exactly the path least likely to be checked by hand.
    m_FirstPoolMemoryResidency.reset();

    // Neutralize an entry that cannot be restored in place: zero the draw-relevant
    // fields so TryGetDrawableBindings refuses, and empty its GPUScene row so
    // the scatter emits no draw. The generational handle stays live (so a stale
    // component reference resolves to a valid-but-empty entry) until the owning
    // system re-registers or unregisters it.
    auto tombstone = [&](MeshGPUEntry& entry)
    {
        if (m_GPUScene && entry.gpuMeshIndex != ~0u)
            m_GPUScene->UpdateMesh(entry.gpuMeshIndex, GPUMesh{});
        // Reset the content hash to the never-a-real-mesh sentinel 0 (FNV-1a of any
        // non-empty mesh is never 0). Otherwise a later byte-identical RegisterSubmesh
        // would hit the dedup fast-path (contentHash match) and return THIS empty
        // tombstone without re-uploading — a permanently invisible mesh. With the
        // sentinel, re-registration takes the content-changed re-upload path and heals.
        entry.contentHash     = 0;
        entry.uploadSeq       = 0;
        entry.bucketKey       = VertexAttributeFlags::None;
        entry.indexCount      = 0;
        entry.corePoolIndex   = kInvalidMeshGPUPoolIndex;
        entry.indexPoolIndex  = kInvalidMeshGPUPoolIndex;
        entry.tangentPoolIndex = kInvalidMeshGPUPoolIndex;
        entry.colorPoolIndex  = kInvalidMeshGPUPoolIndex;
        entry.uv1PoolIndex    = kInvalidMeshGPUPoolIndex;
        entry.jointsPoolIndex = kInvalidMeshGPUPoolIndex;
        entry.weightsPoolIndex = kInvalidMeshGPUPoolIndex;
        entry.joints1PoolIndex = kInvalidMeshGPUPoolIndex;
        entry.weights1PoolIndex = kInvalidMeshGPUPoolIndex;
        entry.extraUvPoolIndex.fill(kInvalidMeshGPUPoolIndex);
        ++report.Tombstoned;
    };

    // Iterate the dedup map: it holds every registered (key -> handle), and the key
    // carries the (assetGuid, submeshIndex) an asset re-upload needs. UploadMesh
    // touches only m_Buckets + m_Device, so mutating entries in place here is safe.
    for (const auto& [key, handle] : m_KeyToHandle)
    {
        Handle raw(static_cast<Detail::HandleType>(handle));
        MeshGPUEntry* entry = m_Entries.Get(raw);
        if (!entry)
            continue;
        ++report.EntriesTotal;

        // Resolve the CPU source. The caller's lookup is authoritative (asset-backed
        // geometry per M1, or an identity-regenerated built-in); a retained picking
        // mirror (procedural-with-picking) is the fallback.
        const MeshGPUCpuSource looked = sourceLookup ? sourceLookup(key) : MeshGPUCpuSource{};
        const Mesh* source = looked.Geometry;
        const MeshGPUSourceOrigin origin = source ? looked.Origin : MeshGPUSourceOrigin::None;
        // Tracked separately from `origin`: the mirror fallback is the registry's own
        // decision, not something the lookup reported. Inferring it from Origin::None
        // would let a lookup that returned geometry without setting Origin be counted
        // as a mirror restore it never was.
        bool fromCpuMirror = false;
        if (!source && entry->cpuMesh)
        {
            source = entry->cpuMesh.get();
            fromCpuMirror = true;
        }

        if (!source)
        {
            tombstone(*entry);
            continue;
        }

        // Preserve the surviving entry's identity across the re-upload: the
        // GPUScene mesh-row index (F9) and the picking mirror. Keep a strong ref to
        // the mirror so it outlives the UploadMesh read when it IS the source.
        //
        // The content hash may only be carried over when the source IS the original
        // geometry. A regenerated source is the identity's DEFAULT geometry, which is
        // not necessarily what was registered — `RegisterSubmesh` supports re-uploading
        // different content under an existing key. Carrying the old hash onto default
        // geometry would make the entry unhealable: a later re-registration of the real
        // geometry hashes to the retained value and takes the dedup fast path, so it
        // never re-uploads. Hash what was actually uploaded instead.
        const uint32_t oldGpuMeshIndex = entry->gpuMeshIndex;
        std::shared_ptr<const Mesh> keepCpuMesh = entry->cpuMesh;
        const bool sourceIsOriginal = (origin != MeshGPUSourceOrigin::Generated);
        const uint64_t keepContentHash =
            sourceIsOriginal ? entry->contentHash : ComputeMeshContentHash(*source);

        MeshGPUEntry fresh = UploadMesh(*source);
        if (fresh.bucketKey == VertexAttributeFlags::None || fresh.indexCount == 0)
        {
            // Upload failed (pool creation OOM) — degrade to a tombstone rather than
            // commit a half-built entry that would draw a garbage range.
            tombstone(*entry);
            continue;
        }
        fresh.cpuMesh     = std::move(keepCpuMesh);
        fresh.contentHash = keepContentHash;

        // Restore the GPUScene row IN PLACE at its original index so GPUInstance
        // .meshIndex references stay valid (F9). AddMesh only when the entry had no
        // row before (indexCount was 0 at first upload but is non-zero now).
        if (m_GPUScene && oldGpuMeshIndex != ~0u)
        {
            m_GPUScene->UpdateMesh(oldGpuMeshIndex, BuildGpuMeshRow(fresh, m_LodSelectionMode));
            fresh.gpuMeshIndex = oldGpuMeshIndex;
            ++report.GpuRowsPreserved;
        }
        else if (m_GPUScene)
        {
            fresh.gpuMeshIndex = m_GPUScene->AddMesh(BuildGpuMeshRow(fresh, m_LodSelectionMode));
        }
        else
        {
            fresh.gpuMeshIndex = oldGpuMeshIndex; // no GPUScene wired (~0u)
        }

        assert((oldGpuMeshIndex == ~0u || fresh.gpuMeshIndex == oldGpuMeshIndex) &&
               "MeshGPURegistry re-provision must preserve the GPUScene mesh-row index (F9)");

        *entry = std::move(fresh);
        if (fromCpuMirror)
        {
            ++report.RestoredFromCpuMesh;
        }
        else
        {
            switch (origin)
            {
            case MeshGPUSourceOrigin::Asset:     ++report.RestoredFromAsset; break;
            case MeshGPUSourceOrigin::Generated: ++report.RestoredFromGenerated; break;
            case MeshGPUSourceOrigin::None:
                assert(false && "a restored entry must report where its geometry came from");
                break;
            }
        }
    }

    return report;
}

ScopedSubscription MeshGPURegistry::SubscribeReload(ReloadCallback callback)
{
    if (!callback)
        return {};
    auto subscriber = std::make_shared<ReloadSubscriber>();
    subscriber->Callback = std::make_shared<ReloadCallback>(std::move(callback));
    ReloadSubscriberBlock* block = m_ReloadSubscribers.get();
    // Construct the (potentially allocating) subscription before publishing.
    ScopedSubscription subscription(m_ReloadSubscribers,
                                    [block, weak = std::weak_ptr<ReloadSubscriber>(subscriber)]
                                    {
                                        std::shared_ptr<ReloadCallback> retired;
                                        if (auto entry = weak.lock())
                                        {
                                            std::lock_guard<std::mutex> lock(block->Mutex);
                                            retired = std::move(entry->Callback);
                                        }
                                        // Destroy user captures outside the block mutex, including Reset
                                        // of another subscription. An executing call owns a separate ref.
                                    });
    {
        std::lock_guard<std::mutex> lock(block->Mutex);
        auto next = std::make_shared<ReloadSubscriberBlock::Snapshot>();
        if (block->Subscribers)
        {
            next->reserve(block->Subscribers->size() + 1);
            for (const auto& entry : *block->Subscribers)
                if (entry->Callback)
                    next->push_back(entry);
        }
        next->push_back(std::move(subscriber));
        block->Subscribers = std::move(next);
    }
    return subscription;
}

void MeshGPURegistry::NotifyUnregister(const GUID& assetGuid)
{
    const auto block = m_ReloadSubscribers;
    std::shared_ptr<const ReloadSubscriberBlock::Snapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(block->Mutex);
        snapshot = block->Subscribers;
    }
    if (!snapshot)
        return;
    for (const auto& entry : *snapshot)
    {
        std::shared_ptr<ReloadCallback> callback;
        {
            std::lock_guard<std::mutex> lock(block->Mutex);
            callback = entry->Callback;
        }
        if (!callback)
            continue;
        try
        {
            (*callback)(assetGuid);
        }
        catch (...)
        {
            // This is post-commit invalidation, not a veto. Even diagnostics
            // must not prevent the remaining cache owners from observing it.
            try
            {
                Logger::Log::Error("MeshGPURegistry: mesh-change subscriber threw");
            }
            catch (...)
            {
            }
        }
    }
}

// ---- Private helpers ----

MeshGPUEntry MeshGPURegistry::UploadMesh(const Mesh& mesh)
{
    AssetDbProfiler::StageScope profileScope(AssetDbProfiler::IsOnMainThread()
        ? AssetDbProfiler::Bucket::UploadMeshMain
        : AssetDbProfiler::Bucket::UploadMeshWorker);

    MeshGPUEntry entry{};

    if (!m_Device || mesh.Vertices.empty())
        return entry;

    static_assert(MeshGPUEntry::kMaxEntryLODs == MeshLODConfig::kMaxLODs);
    const MeshLODGeometry geometry = ResolveMeshLODGeometry(mesh);
    if (geometry.Issue != MeshLODGeometryIssue::None) {
        const char* reason = "";
        switch (geometry.Issue) {
        case MeshLODGeometryIssue::InvalidBaseBounds: reason = "invalid LOD0 bounds"; break;
        case MeshLODGeometryIssue::UnsupportedStreams: reason = "unsupported per-LOD attribute streams"; break;
        case MeshLODGeometryIssue::InvalidLowerGeometry: reason = "invalid lower-level positions or indices"; break;
        case MeshLODGeometryIssue::EmptyLevel: reason = "empty level terminates the LOD chain"; break;
        case MeshLODGeometryIssue::None: break;
        }
        Logger::Log::Warning("MeshGPURegistry: mesh '{}': {}; admitted {} levels.",
            mesh.Name, reason, geometry.LevelCount);
    }
    if (geometry.LevelCount == 0)
        return entry;

    // Determine vertex attribute flags from the mesh data.
    VertexAttributeFlags flags = VertexAttributeFlags::HasPosition;

    // Check tangents.
    bool hasTangent = false;
    for (const auto& v : mesh.Vertices)
    {
        if (v.Tangent[0] != 0.0f || v.Tangent[1] != 0.0f || v.Tangent[2] != 0.0f)
        {
            hasTangent = true;
            break;
        }
    }
    if (hasTangent)
        flags |= VertexAttributeFlags::HasTangent;
    // A refused own-vertex chain still draws LOD0. Do not publish a non-finite
    // base stream into that fallback's colour/alpha calculation; HasColor0()
    // already rejects a stream whose length does not match the vertex count.
    if (mesh.HasColor0())
    {
        if (std::all_of(mesh.Color0.begin(), mesh.Color0.end(),
                        [](float value) { return std::isfinite(value); }))
        {
            flags |= VertexAttributeFlags::HasColor;
        }
        else
        {
            Logger::Log::Warning(
                "MeshGPURegistry: mesh '{}' has a non-finite vertex color stream; uploading it "
                "without vertex colors, so shading reads the default white.", mesh.Name);
        }
    }
    if (mesh.HasTexCoords1())
        flags |= VertexAttributeFlags::HasUV1;
    for (size_t i = 0; i < kExtraUvFlags.size(); ++i)
    {
        if (i < mesh.ExtraTexCoords.size()
            && mesh.ExtraTexCoords[i].size() == mesh.Vertices.size() * 2)
        {
            flags |= kExtraUvFlags[i];
        }
    }

    // Check skinning data.
    if (mesh.IsSkinned())
        flags |= VertexAttributeFlags::Skinned;
    if (mesh.HasSkinning1())
        flags |= VertexAttributeFlags::HasJoints1 | VertexAttributeFlags::HasWeights1;

    // Always include Normal and UV0 in the core VB so the interleaved stride
    // matches the StandardMesh shader variant (all composed shaders are compiled
    // with Position + Normal + UV0). Meshes that lack normals or UVs get
    // zero-filled data, which is visually correct (flat-shaded, no UVs).
    flags |= VertexAttributeFlags::HasNormal;
    flags |= VertexAttributeFlags::HasUV0;

    entry.vertexFlags = flags;
    entry.topology = ToRenderingTopology(mesh.PrimitiveTopology);

    const uint32_t baseVertCount = static_cast<uint32_t>(mesh.Vertices.size());
    const uint32_t coreStride    = CoreVertexStride(flags);

    // --- Per-LOD own-vertex plan (Phase C1; generated shells ride it too) ---
    // A non-empty ExtraLODVertices[k-1] marks LOD k as own-vertex: it carries
    // its OWN vertices and ExtraLODs[k-1] indexes them LOD-locally. Own blocks
    // are concatenated after LOD0's block in the core VB and each records a
    // RELATIVE vertex offset; index-only levels leave the offset 0 and share
    // LOD0's block, exactly as before. Producers: artist-authored chains
    // (AuthoredLodImport) and generated attribute-honest sloppy shells — the
    // upload mechanics are identical; only threshold PROVENANCE differs
    // (mesh.HasAuthoredLODs(), the explicit flag).
    const bool authoredChain = mesh.HasAuthoredLODs();
    const uint32_t lodCount = geometry.LevelCount;
    bool honorOwnVertex = false;
    for (uint32_t level = 1; level < lodCount && level <= mesh.ExtraLODVertices.size(); ++level)
        honorOwnVertex |= !mesh.ExtraLODVertices[level - 1].empty();

    uint32_t lodVertexOffset[MeshGPUEntry::kMaxEntryLODs] = {}; // relative, in vertices
    uint32_t maxLevelVertCount = baseVertCount; // for the per-LOD u16/u32 decision
    std::vector<Vertex> concatVertices;   // only populated for own-vertex chains
    std::vector<float> concatColours;
    const float* colours = mesh.Color0.data();
    const Vertex* verts     = mesh.Vertices.data();
    uint32_t      vertCount  = baseVertCount;
    if (honorOwnVertex)
    {
        concatVertices = mesh.Vertices;
        if (HasFlag(flags, VertexAttributeFlags::HasColor)) concatColours = mesh.Color0;
        for (uint32_t k = 1; k < lodCount; ++k)
        {
            const size_t blockIdx = k - 1u;
            if (blockIdx < mesh.ExtraLODVertices.size() &&
                !mesh.ExtraLODVertices[blockIdx].empty())
            {
                const auto& block  = mesh.ExtraLODVertices[blockIdx];
                lodVertexOffset[k] = static_cast<uint32_t>(concatVertices.size());
                maxLevelVertCount =
                    std::max(maxLevelVertCount, static_cast<uint32_t>(block.size()));
                concatVertices.insert(concatVertices.end(), block.begin(), block.end());
                if (HasFlag(flags, VertexAttributeFlags::HasColor)) {
                    const auto& colourBlock = mesh.ExtraLODColor0[blockIdx];
                    concatColours.insert(concatColours.end(), colourBlock.begin(), colourBlock.end());
                }
            }
            // else index-only level: lodVertexOffset[k] stays 0 (shares LOD0).
        }
        verts     = concatVertices.data();
        vertCount = static_cast<uint32_t>(concatVertices.size());
        if (HasFlag(flags, VertexAttributeFlags::HasColor)) colours = concatColours.data();
    }

    const size_t coreBytes = static_cast<size_t>(vertCount) * coreStride;
    // Index width keys on the MAX PER-LEVEL vertex count, not the concatenated
    // total: own-vertex indices are LOD-local (bounded by their own block) and
    // index-only levels address LOD0's block, so no index value can exceed the
    // largest single level. Without own blocks this reduces to
    // (baseVertCount <= 65535), byte-identical to before. The base vertexOffset
    // is a separate int32 in the draw command and never narrows with the index
    // width.
    const bool     use16Indices = (maxLevelVertCount <= 65535u);
    const uint32_t indexStride  = use16Indices ? 2u : 4u;

    // --- Build core interleaved data (pos + normal + uv0) ---
    std::vector<uint8_t> coreData(coreBytes);
    {
        uint8_t* dst = coreData.data();
        for (uint32_t vi = 0; vi < vertCount; ++vi)
        {
            const Vertex& v = verts[vi];
            std::memcpy(dst, v.Position, 3 * sizeof(float));
            dst += 3 * sizeof(float);
            std::memcpy(dst, v.Normal, 3 * sizeof(float));
            dst += 3 * sizeof(float);
            std::memcpy(dst, v.TexCoords, 2 * sizeof(float));
            dst += 2 * sizeof(float);
        }
    }

    // --- Build index data: all LODs concatenated into one allocation ---
    // Generated LODs share the vertex buffer (meshopt reuses indices); authored
    // LODs index their own concatenated block via lodVertexOffset above. Either
    // way we pack LOD0..N contiguously and record each level's local offset/count.
    uint32_t lodLocalOffset[MeshGPUEntry::kMaxEntryLODs] = {}; // in indices
    uint32_t lodCounts[MeshGPUEntry::kMaxEntryLODs]      = {};
    size_t   totalIndices = 0;
    for (uint32_t k = 0; k < lodCount; ++k)
    {
        lodLocalOffset[k] = static_cast<uint32_t>(totalIndices);
        lodCounts[k]      = static_cast<uint32_t>(mesh.LODIndices(k).size());
        totalIndices += lodCounts[k];
    }

    std::vector<uint16_t> indices16;
    std::vector<uint32_t> indices32;
    const void*           ibSrc;
    size_t                ibBytes;
    if (use16Indices)
    {
        indices16.resize(totalIndices);
        size_t w = 0;
        for (uint32_t k = 0; k < lodCount; ++k)
            for (uint32 idx : mesh.LODIndices(k))
                indices16[w++] = static_cast<uint16_t>(idx);
        // Pad to an even index count so the upload size is a multiple of 4 (see
        // kIndexUploadAlignmentBytes). The pad index is outside every LOD's count
        // and is never drawn.
        if (indices16.size() & 1u)
            indices16.push_back(0u);
        ibSrc   = indices16.data();
        ibBytes = indices16.size() * sizeof(uint16_t);
    }
    else
    {
        indices32.resize(totalIndices);
        size_t w = 0;
        for (uint32_t k = 0; k < lodCount; ++k)
        {
            const auto& src = mesh.LODIndices(k);
            std::memcpy(indices32.data() + w, src.data(), src.size() * sizeof(uint32_t));
            w += src.size();
        }
        ibSrc   = indices32.data();
        ibBytes = indices32.size() * sizeof(uint32_t);
    }

    // --- Build optional stream data ---
    std::vector<float> tangentData;
    size_t             tBytes = 0;
    if (HasFlag(flags, VertexAttributeFlags::HasTangent))
    {
        tangentData.resize(static_cast<size_t>(vertCount) * 4);
        for (uint32_t i = 0; i < vertCount; ++i)
        {
            tangentData[i * 4 + 0] = verts[i].Tangent[0];
            tangentData[i * 4 + 1] = verts[i].Tangent[1];
            tangentData[i * 4 + 2] = verts[i].Tangent[2];
            tangentData[i * 4 + 3] = verts[i].Tangent[3];
        }
        tBytes = tangentData.size() * sizeof(float);
    }
    const size_t cBytes = HasFlag(flags, VertexAttributeFlags::HasColor)
                              ? static_cast<size_t>(vertCount) * 4u * sizeof(float) : 0;
    const size_t uv1Bytes = HasFlag(flags, VertexAttributeFlags::HasUV1)
                              ? mesh.TexCoords1.size() * sizeof(float) : 0;
    const size_t jBytes = HasFlag(flags, VertexAttributeFlags::HasJoints)
                              ? mesh.Joints0.size() * sizeof(uint16_t) : 0;
    const size_t wBytes = HasFlag(flags, VertexAttributeFlags::HasWeights)
                              ? mesh.Weights0.size() * sizeof(float)    : 0;
    const size_t j1Bytes = HasFlag(flags, VertexAttributeFlags::HasJoints1)
                              ? mesh.Joints1.size() * sizeof(uint16_t) : 0;
    const size_t w1Bytes = HasFlag(flags, VertexAttributeFlags::HasWeights1)
                              ? mesh.Weights1.size() * sizeof(float)    : 0;
    std::array<size_t, 6> extraUvBytes{};
    for (size_t i = 0; i < extraUvBytes.size(); ++i)
    {
        if (HasFlag(flags, kExtraUvFlags[i]) && i < mesh.ExtraTexCoords.size())
            extraUvBytes[i] = mesh.ExtraTexCoords[i].size() * sizeof(float);
    }

    // --- Allocate bucket ranges (append sibling pools on overflow) ---
    MeshGPUBucket& bucket = GetOrCreateBucket(flags);

    auto allocateStream = [&](std::vector<MeshGPUStreamPool>& pools,
                              uint64_t initialCapacity,
                              uint32_t stride,
                              size_t srcBytes,
                              const char* debugName,
                              BucketPoolAllocator::Allocation& outAlloc,
                              uint32_t& outPoolIndex) -> bool
    {
        outAlloc = {};
        outPoolIndex = kInvalidMeshGPUPoolIndex;
        if (srcBytes == 0)
            return true;

        for (uint32_t i = 0; i < static_cast<uint32_t>(pools.size()); ++i)
        {
            BucketPoolAllocator::Allocation alloc = pools[i].allocator.Allocate(srcBytes);
            if (alloc.IsValid())
            {
                outAlloc = alloc;
                outPoolIndex = i;
                return true;
            }
        }

        const uint64_t capacity = NextPoolCapacity(pools, initialCapacity, srcBytes, stride);
        if (!CreateBucketStreamPool(pools, capacity, stride, debugName))
        {
            Logger::Log::Warning(
                "MeshGPURegistry: failed to create sibling pool '{}' (capacity={} bytes) for mesh flags={:x}.",
                debugName ? debugName : "<unnamed>",
                static_cast<unsigned long long>(capacity),
                static_cast<uint32_t>(flags));
            return false;
        }

        MeshGPUStreamPool& pool = pools.back();
        BucketPoolAllocator::Allocation alloc = pool.allocator.Allocate(srcBytes);
        if (!alloc.IsValid())
        {
            Logger::Log::Warning(
                "MeshGPURegistry: newly-created sibling pool '{}' could not satisfy {} bytes (capacity={} bytes).",
                debugName ? debugName : "<unnamed>",
                static_cast<unsigned long long>(srcBytes),
                static_cast<unsigned long long>(pool.capacityBytes));
            return false;
        }

        outAlloc = alloc;
        outPoolIndex = static_cast<uint32_t>(pools.size() - 1u);
        return true;
    };

    auto freeStream = [](std::vector<MeshGPUStreamPool>& pools,
                         uint32_t poolIndex,
                         BucketPoolAllocator::Allocation alloc)
    {
        if (!alloc.IsValid())
            return;
        if (MeshGPUStreamPool* pool = GetPool(pools, poolIndex))
            pool->allocator.Free(alloc);
    };

    BucketPoolAllocator::Allocation coreSub{}, indexSub{}, tangentSub{}, colorSub{}, uv1Sub{}, jointsSub{}, weightsSub{}, joints1Sub{}, weights1Sub{};
    std::array<BucketPoolAllocator::Allocation, 6> extraUvSub{};
    uint32_t corePoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t indexPoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t tangentPoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t colorPoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t uv1PoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t jointsPoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t weightsPoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t joints1PoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t weights1PoolIndex = kInvalidMeshGPUPoolIndex;
    std::array<uint32_t, 6> extraUvPoolIndex{
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex};

    struct VertexStreamRequest
    {
        std::vector<MeshGPUStreamPool>* pools = nullptr;
        uint64_t initialCapacity = 0;
        uint32_t stride = 1;
        size_t bytes = 0;
        const char* debugName = nullptr;
        BucketPoolAllocator::Allocation* alloc = nullptr;
        uint32_t* poolIndex = nullptr;
    };

    std::vector<VertexStreamRequest> vertexStreams;
    auto addVertexStream = [&](std::vector<MeshGPUStreamPool>& pools,
                               uint64_t initialCapacity,
                               uint32_t stride,
                               size_t bytes,
                               const char* debugName,
                               BucketPoolAllocator::Allocation& alloc,
                               uint32_t& poolIndex)
    {
        if (bytes == 0)
            return;
        vertexStreams.push_back(VertexStreamRequest{
            &pools, initialCapacity, stride, bytes, debugName, &alloc, &poolIndex});
    };

    addVertexStream(bucket.corePools, kBucketCoreInitialBytes, coreStride,
                    coreBytes, "MeshGPU.Bucket.CoreVB",
                    coreSub, corePoolIndex);
    addVertexStream(bucket.tangentPools, kBucketOtherInitialBytes, 16u,
                    tBytes, "MeshGPU.Bucket.TangentVB",
                    tangentSub, tangentPoolIndex);
    addVertexStream(bucket.colorPools, kBucketOtherInitialBytes, 16u,
                    cBytes, "MeshGPU.Bucket.ColorVB",
                    colorSub, colorPoolIndex);
    addVertexStream(bucket.uv1Pools, kBucketOtherInitialBytes, 8u,
                    uv1Bytes, "MeshGPU.Bucket.UV1VB",
                    uv1Sub, uv1PoolIndex);
    addVertexStream(bucket.jointsPools, kBucketOtherInitialBytes, 8u,
                    jBytes, "MeshGPU.Bucket.JointsVB",
                    jointsSub, jointsPoolIndex);
    addVertexStream(bucket.weightsPools, kBucketOtherInitialBytes, 16u,
                    wBytes, "MeshGPU.Bucket.WeightsVB",
                    weightsSub, weightsPoolIndex);
    addVertexStream(bucket.joints1Pools, kBucketOtherInitialBytes, 8u,
                    j1Bytes, "MeshGPU.Bucket.Joints1VB",
                    joints1Sub, joints1PoolIndex);
    addVertexStream(bucket.weights1Pools, kBucketOtherInitialBytes, 16u,
                    w1Bytes, "MeshGPU.Bucket.Weights1VB",
                    weights1Sub, weights1PoolIndex);
    for (size_t i = 0; i < extraUvBytes.size(); ++i)
        addVertexStream(bucket.extraUvPools[i], kBucketOtherInitialBytes, 8u,
                        extraUvBytes[i], kExtraUvDebugNames[i],
                        extraUvSub[i], extraUvPoolIndex[i]);

    auto clearVertexAllocations = [&]()
    {
        for (auto& stream : vertexStreams)
        {
            freeStream(*stream.pools, *stream.poolIndex, *stream.alloc);
            *stream.alloc = {};
            *stream.poolIndex = kInvalidMeshGPUPoolIndex;
        }
    };

    auto tryAllocateVertexGroup = [&](uint32_t groupIndex) -> bool
    {
        uint64_t vertexOffset = std::numeric_limits<uint64_t>::max();
        for (auto& stream : vertexStreams)
        {
            if (!stream.pools || groupIndex >= static_cast<uint32_t>(stream.pools->size()))
            {
                clearVertexAllocations();
                return false;
            }

            MeshGPUStreamPool& pool = (*stream.pools)[groupIndex];
            BucketPoolAllocator::Allocation alloc = pool.allocator.Allocate(stream.bytes);
            if (!alloc.IsValid())
            {
                clearVertexAllocations();
                return false;
            }

            *stream.alloc = alloc;
            *stream.poolIndex = groupIndex;

            const uint64_t streamVertexOffset = alloc.offset / stream.stride;
            if (vertexOffset == std::numeric_limits<uint64_t>::max())
                vertexOffset = streamVertexOffset;
            else if (vertexOffset != streamVertexOffset)
            {
                clearVertexAllocations();
                return false;
            }
        }
        return true;
    };

    bool vertexAllocated = false;
    const uint32_t existingVertexGroupCount = static_cast<uint32_t>(bucket.corePools.size());
    for (uint32_t groupIndex = 0; groupIndex < existingVertexGroupCount; ++groupIndex)
    {
        if (tryAllocateVertexGroup(groupIndex))
        {
            vertexAllocated = true;
            break;
        }
    }

    if (!vertexAllocated)
    {
        const uint32_t newGroupIndex = static_cast<uint32_t>(bucket.corePools.size());
        bool createdGroup = true;
        std::vector<VertexStreamRequest*> createdStreams;
        for (auto& stream : vertexStreams)
        {
            if (!stream.pools || stream.pools->size() != newGroupIndex)
            {
                Logger::Log::Warning(
                    "MeshGPURegistry: vertex stream pool group mismatch for '{}' (expected group {}).",
                    stream.debugName ? stream.debugName : "<unnamed>",
                    newGroupIndex);
                createdGroup = false;
                break;
            }

            const uint64_t capacity = NextPoolCapacity(*stream.pools,
                                                       stream.initialCapacity,
                                                       stream.bytes,
                                                       stream.stride);
            if (!CreateBucketStreamPool(*stream.pools, capacity, stream.stride, stream.debugName))
            {
                Logger::Log::Warning(
                    "MeshGPURegistry: failed to create vertex sibling pool '{}' (capacity={} bytes) for mesh flags={:x}.",
                    stream.debugName ? stream.debugName : "<unnamed>",
                    static_cast<unsigned long long>(capacity),
                    static_cast<uint32_t>(flags));
                createdGroup = false;
                break;
            }
            createdStreams.push_back(&stream);
        }

        if (!createdGroup)
        {
            for (VertexStreamRequest* stream : createdStreams)
            {
                if (!stream || !stream->pools || stream->pools->size() <= newGroupIndex)
                    continue;
                MeshGPUStreamPool& pool = (*stream->pools)[newGroupIndex];
                if (pool.buffer.IsValid())
                    m_Device->DestroyBuffer(pool.buffer);
                stream->pools->pop_back();
            }
        }

        vertexAllocated = createdGroup && tryAllocateVertexGroup(newGroupIndex);
    }

    auto& indexPools = use16Indices ? bucket.ib16Pools : bucket.ib32Pools;
    bool allocated = vertexAllocated;
    // The index pool aligns to the WebGPU upload alignment, not the raw index
    // stride: firstIndex still divides by indexStride below, and 4 is a multiple
    // of both stride widths, so the derived firstIndex is unchanged in value.
    const uint32_t indexPoolAlignment = std::max(indexStride, kIndexUploadAlignmentBytes);
    allocated = allocated && allocateStream(indexPools, kBucketIndexInitialBytes, indexPoolAlignment,
                                            ibBytes, use16Indices ? "MeshGPU.Bucket.IB16" : "MeshGPU.Bucket.IB32",
                                            indexSub, indexPoolIndex);

    // Give back every range allocated before the upload was abandoned. These
    // ranges were never written to GPU, so immediate Free is safe.
    auto releaseAllocations = [&]
    {
        freeStream(bucket.corePools, corePoolIndex, coreSub);
        freeStream(indexPools, indexPoolIndex, indexSub);
        freeStream(bucket.tangentPools, tangentPoolIndex, tangentSub);
        freeStream(bucket.colorPools, colorPoolIndex, colorSub);
        freeStream(bucket.uv1Pools, uv1PoolIndex, uv1Sub);
        freeStream(bucket.jointsPools, jointsPoolIndex, jointsSub);
        freeStream(bucket.weightsPools, weightsPoolIndex, weightsSub);
        freeStream(bucket.joints1Pools, joints1PoolIndex, joints1Sub);
        freeStream(bucket.weights1Pools, weights1PoolIndex, weights1Sub);
        for (size_t i = 0; i < extraUvSub.size(); ++i)
            freeStream(bucket.extraUvPools[i], extraUvPoolIndex[i], extraUvSub[i]);
    };

    if (!allocated)
    {
        releaseAllocations();
        Logger::Log::Warning(
            "MeshGPURegistry: failed to allocate bucket storage for mesh flags={:x}; mesh will not draw.",
            static_cast<uint32_t>(flags));
        return entry;
    }

    // Open the upload AFTER the allocations are secured and BEFORE the first
    // byte is written, so neither early return above can leave a slot open and
    // every stream write below is covered by a stamp that exists. A refused
    // mint degrades to exactly the failed-allocation state: no bytes written,
    // no bucket, uploadSeq 0, entry not drawable.
    const uint64_t uploadSeq = m_Residency.BeginUpload();
    if (uploadSeq == 0u)
    {
        releaseAllocations();
        Logger::Log::Warning(
            "MeshGPURegistry: upload window exhausted for mesh flags={:x}; mesh will not draw.",
            static_cast<uint32_t>(flags));
        return entry;
    }
    entry.uploadSeq = uploadSeq;

    // --- Commit ---
    entry.bucketKey = flags;
    entry.corePoolIndex = corePoolIndex;
    entry.indexPoolIndex = indexPoolIndex;
    entry.tangentPoolIndex = tangentPoolIndex;
    entry.colorPoolIndex = colorPoolIndex;
    entry.uv1PoolIndex = uv1PoolIndex;
    entry.jointsPoolIndex = jointsPoolIndex;
    entry.weightsPoolIndex = weightsPoolIndex;
    entry.joints1PoolIndex = joints1PoolIndex;
    entry.weights1PoolIndex = weights1PoolIndex;
    entry.extraUvPoolIndex = extraUvPoolIndex;

    auto updateStream = [&](std::vector<MeshGPUStreamPool>& pools,
                            uint32_t poolIndex,
                            BucketPoolAllocator::Allocation alloc,
                            size_t size,
                            const void* data)
    {
        if (size == 0)
            return;
        MeshGPUStreamPool* pool = GetPool(pools, poolIndex);
        if (!pool || !pool->buffer.IsValid())
            return;
        m_Device->UpdateBuffer(pool->buffer, alloc.offset, size, data);
    };

    updateStream(bucket.corePools, corePoolIndex, coreSub, coreBytes, coreData.data());
    updateStream(indexPools, indexPoolIndex, indexSub, ibBytes, ibSrc);
    updateStream(bucket.tangentPools, tangentPoolIndex, tangentSub, tBytes, tangentData.data());
    updateStream(bucket.colorPools, colorPoolIndex, colorSub, cBytes, colours);
    updateStream(bucket.uv1Pools, uv1PoolIndex, uv1Sub, uv1Bytes, mesh.TexCoords1.data());
    updateStream(bucket.jointsPools, jointsPoolIndex, jointsSub, jBytes, mesh.Joints0.data());
    updateStream(bucket.weightsPools, weightsPoolIndex, weightsSub, wBytes, mesh.Weights0.data());
    updateStream(bucket.joints1Pools, joints1PoolIndex, joints1Sub, j1Bytes, mesh.Joints1.data());
    updateStream(bucket.weights1Pools, weights1PoolIndex, weights1Sub, w1Bytes, mesh.Weights1.data());
    for (size_t i = 0; i < extraUvBytes.size(); ++i)
        updateStream(bucket.extraUvPools[i], extraUvPoolIndex[i], extraUvSub[i],
                     extraUvBytes[i], i < mesh.ExtraTexCoords.size() ? mesh.ExtraTexCoords[i].data() : nullptr);

    entry.coreSubAlloc    = coreSub;
    entry.indexSubAlloc   = indexSub;
    entry.tangentSubAlloc = tangentSub;
    entry.colorSubAlloc   = colorSub;
    entry.uv1SubAlloc     = uv1Sub;
    entry.jointsSubAlloc  = jointsSub;
    entry.weightsSubAlloc = weightsSub;
    entry.joints1SubAlloc  = joints1Sub;
    entry.weights1SubAlloc = weights1Sub;
    entry.extraUvSubAlloc  = extraUvSub;

    entry.vertexOffset = static_cast<uint32_t>(coreSub.offset / coreStride);
    entry.firstIndex   = static_cast<uint32_t>(indexSub.offset / indexStride);

    entry.indexType  = use16Indices ? static_cast<uint32_t>(IndexType::Uint16)
                                    : static_cast<uint32_t>(IndexType::Uint32);
    entry.indexCount = lodCounts[0];

    // Record per-LOD ranges relative to the index allocation base, plus the
    // achieved-error carrier (absolute-LOD indexed; LOD0 has no error). ExtraLOD
    // error/sloppy arrays are parallel to ExtraLODs (LOD1..N), so LOD k>=1 reads
    // slot k-1.
    entry.lodCount = lodCount;
    // Threshold provenance is the EXPLICIT authored flag, never the presence of
    // vertex blocks: generated attribute-honest shells own vertices too but must
    // take the sloppy-capped derived table, not the authored default table.
    entry.lodAuthored = authoredChain && lodCount > 1;
    for (uint32_t k = 0; k < lodCount; ++k)
    {
        entry.lodFirstIndex[k] = entry.firstIndex + lodLocalOffset[k];
        entry.lodIndexCount[k] = lodCounts[k];
        // Invariant (amendment #4): a level with no indices has no vertex block,
        // so its relative vertex offset must be 0 — the scatter's lodIndexCount==0
        // fallback then composes a correct LOD0 vertexOffset. Enforced here rather
        // than only asserted so it holds in every build config.
        entry.lodVertexOffset[k] = (lodCounts[k] != 0u) ? lodVertexOffset[k] : 0u;
        if (k >= 1u && (k - 1u) < mesh.ExtraLODErrors.size())
        {
            entry.lodError[k]  = mesh.ExtraLODErrors[k - 1u];
            entry.lodSloppy[k] = (k - 1u) < mesh.ExtraLODSloppy.size()
                                     ? mesh.ExtraLODSloppy[k - 1u] : 0u;
        }
    }

    // Authored switch coverages seed the direct-coverage threshold path. Copied
    // only for an honored authored chain: a chain dropped to LOD0-only over
    // optional streams keeps lodCount==1, so this stays 0 and never seeds
    // thresholds for a LOD the GPU did not upload (C2 precondition above).
    // ExtraLODCoverage is parallel to ExtraLODs (leaving-LOD-k), matching
    // entry.lodCoverage; the coarsest present slot has no coverage.
    if (authoredChain && lodCount > 1 && !mesh.ExtraLODCoverage.empty())
    {
        const uint32_t covCount = std::min<uint32_t>(
            {static_cast<uint32_t>(mesh.ExtraLODCoverage.size()),
             lodCount > 0u ? lodCount - 1u : 0u, MeshGPUEntry::kMaxEntryLODs});
        for (uint32_t k = 0; k < covCount; ++k)
            entry.lodCoverage[k] = mesh.ExtraLODCoverage[k];
        entry.lodCoverageCount = covCount;
    }

    entry.bounds = geometry.Bounds;
    entry.lodReferenceRadius = geometry.ReferenceBounds.Radius();
    const auto& referenceExtent = geometry.ReferenceBounds.halfExtents;
    entry.lodReferenceMaxExtent = 2.0f * std::max({referenceExtent.x, referenceExtent.y, referenceExtent.z});

    // Every write this upload owns has been ISSUED — which is residency for
    // host-visible pools, where the stream write IS the copy. Closing the
    // bracket is what makes the stamp read resident, and it is the last
    // statement on the only path that reaches here from the mint above.
    //
    // What the bracket does not promise: a stream whose pool is missing or
    // whose buffer is invalid is skipped by the writer above rather than
    // written, and the bracket still closes. For the core and index streams
    // that is caught downstream — the same missing pool makes their handles
    // unresolvable and the chokepoint refuses. An optional stream in that state
    // is a pre-existing pool bug this gate does not claim to cover.
    //
    // Deliberately an explicit call and NOT a scope guard: a guard would close
    // the stamp on an exit path that had written only some of the streams, and
    // a stamp that reads resident over partly-written storage is the exact
    // failure this gate exists to prevent. An abandoned path that leaks an open
    // slot instead keeps the entry undrawable — worse for the ring (a leaked
    // slot pins the base) but never wrong at the chokepoint.
    m_Residency.EndUpload(uploadSeq);
    return entry;
}

MeshGPUBucket& MeshGPURegistry::GetOrCreateBucket(MeshGPUBucketKey key)
{
    const uint32_t kkey = static_cast<uint32_t>(key);
    auto [it, inserted] = m_Buckets.try_emplace(kkey);
    if (inserted)
    {
        it->second.key             = key;
        it->second.coreStrideBytes = CoreVertexStride(key);
    }
    return it->second;
}

bool MeshGPURegistry::CreateBucketStreamPool(std::vector<MeshGPUStreamPool>& pools,
                                             uint64_t capacityBytes,
                                             uint64_t alignmentBytes,
                                             const char* debugName) const
{
    if (!m_Device || capacityBytes == 0)
        return false;

    BufferDesc desc{};
    desc.size        = capacityBytes;
    // Stream classification by debug-name prefix: VBs vs IB pools differ
    // only by usage flag. Cheap and avoids threading an extra parameter
    // through every call site.
    const bool isIndex = (debugName && std::strstr(debugName, "IB") != nullptr);
    desc.usage       = static_cast<uint32_t>(isIndex ? BufferUsage::Index
                                                     : BufferUsage::Vertex);
    // When ray query is available, pool VB/IB double as BLAS geometry input,
    // read in place via device address (position leads the binding-0
    // interleaved stream, so triangles read at suballocation offset + full
    // stride — no copies). Gated: the AS usage bit is invalid on devices
    // without VK_KHR_acceleration_structure enabled.
    if (m_Device->GetCapabilities().supportsRayQuery)
    {
        desc.usage |= static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress
                                            | BufferUsage::AccelerationStructureBuildInput);
    }
    // Pool bytes are written once by the CPU and then re-read by the GPU for
    // every frame the mesh is visible — depth prepass, four shadow cascades and
    // the forward pass all re-fetch them — so the re-read bandwidth dominates
    // the write by orders of magnitude. That is the shape
    // UploadDeviceLocalPreferred names. The device decides whether it can honour
    // it (VulkanBufferResidency.h, DeviceLocalMemoryIsFullyHostWritable); where
    // it cannot, this resolves exactly as Upload did and the pool stays mapped
    // either way, so PersistentlyMapped below means the same thing in both arms.
    desc.memoryUsage = BufferMemoryUsage::UploadDeviceLocalPreferred;
    desc.flags       = BufferCreateFlags::PersistentlyMapped;
    desc.debugName   = debugName;

    BufferHandle h = m_Device->CreateBuffer(desc);
    if (!h.IsValid())
        return false;

    LogPoolMemoryResidency(h, debugName);

    MeshGPUStreamPool pool{};
    pool.buffer = h;
    pool.capacityBytes = capacityBytes;
    pool.strideBytes = static_cast<uint32_t>(std::max<uint64_t>(1u, alignmentBytes));
    pool.allocator.Initialize(capacityBytes, alignmentBytes);
    pools.push_back(std::move(pool));
    return true;
}

void MeshGPURegistry::LogPoolMemoryResidency(BufferHandle buffer, const char* debugName) const
{
    const IDevice::BufferMemoryResidency residency = m_Device->GetBufferMemoryResidency(buffer);
    if (!residency.reported)
        return;

    const bool isFirst = !m_FirstPoolMemoryResidency.has_value();
    if (!isFirst)
    {
        const IDevice::BufferMemoryResidency& first = *m_FirstPoolMemoryResidency;
        const bool matchesFirst = residency.deviceLocal == first.deviceLocal &&
                                  residency.hostVisible == first.hostVisible &&
                                  residency.hostCoherent == first.hostCoherent &&
                                  residency.heapIndex == first.heapIndex;
        if (matchesFirst)
            return;
    }

    constexpr double kBytesPerMiB = 1024.0 * 1024.0;
    Logger::Log::Info(
        "MeshGPURegistry pool '{}' memory: deviceLocal={} hostVisible={} hostCoherent={} "
        "heapIndex={} heapSize={:.0f} MiB{}",
        debugName ? debugName : "<unnamed>",
        residency.deviceLocal,
        residency.hostVisible,
        residency.hostCoherent,
        residency.heapIndex,
        static_cast<double>(residency.heapSizeBytes) / kBytesPerMiB,
        isFirst ? "" : " (differs from first pool)");

    if (isFirst)
        m_FirstPoolMemoryResidency = residency;
}

void MeshGPURegistry::ReleaseEntryToBucket(MeshGPUEntry& entry)
{
    // FIRST statement, above the bucket lookup, because that lookup returns
    // early for every bucketKey == None entry and for anything called after
    // m_Buckets.clear() — a cancel placed with the frees would silently skip
    // exactly the entries whose bytes are least trustworthy.
    //
    // Both halves are needed. CancelRecordsFor lets the ring base move past the
    // stamp; clearing the field is what stops the entry reading resident, since
    // a retired stamp has no outstanding obligation and so reads resident by
    // design. Between here and the next successful upload the entry is
    // findable, and it must not draw.
    m_Residency.CancelRecordsFor(entry.uploadSeq);
    entry.uploadSeq = 0u;

    auto it = m_Buckets.find(static_cast<uint32_t>(entry.bucketKey));
    if (it == m_Buckets.end())
        return;
    MeshGPUBucket& bucket = it->second;

    const uint64_t retire = m_CurrentFrameIndex + kRetireDelay;
    auto freeDeferred = [retire](std::vector<MeshGPUStreamPool>& pools,
                                 uint32_t poolIndex,
                                 BucketPoolAllocator::Allocation& alloc)
    {
        if (!alloc.IsValid())
            return;
        if (MeshGPUStreamPool* pool = GetPool(pools, poolIndex))
        {
            pool->allocator.FreeDeferred(alloc, retire);
            alloc = {};
        }
    };

    freeDeferred(bucket.corePools, entry.corePoolIndex, entry.coreSubAlloc);
    freeDeferred(bucket.tangentPools, entry.tangentPoolIndex, entry.tangentSubAlloc);
    freeDeferred(bucket.colorPools, entry.colorPoolIndex, entry.colorSubAlloc);
    freeDeferred(bucket.uv1Pools, entry.uv1PoolIndex, entry.uv1SubAlloc);
    freeDeferred(bucket.jointsPools, entry.jointsPoolIndex, entry.jointsSubAlloc);
    freeDeferred(bucket.weightsPools, entry.weightsPoolIndex, entry.weightsSubAlloc);
    freeDeferred(bucket.joints1Pools, entry.joints1PoolIndex, entry.joints1SubAlloc);
    freeDeferred(bucket.weights1Pools, entry.weights1PoolIndex, entry.weights1SubAlloc);
    for (size_t i = 0; i < entry.extraUvSubAlloc.size(); ++i)
        freeDeferred(bucket.extraUvPools[i], entry.extraUvPoolIndex[i], entry.extraUvSubAlloc[i]);
    if (entry.indexSubAlloc.IsValid())
    {
        const bool use16 = (entry.indexType == static_cast<uint32_t>(IndexType::Uint16));
        freeDeferred(use16 ? bucket.ib16Pools : bucket.ib32Pools,
                     entry.indexPoolIndex,
                     entry.indexSubAlloc);
    }
}

VertexAttributeFlags BoundStreamVertexFlags(const MeshGPUEntry& entry, const MeshGPUEntryBindings& bindings,
                                            bool ignoreVertexColor)
{
    VertexAttributeFlags flags = entry.vertexFlags;
    if (!bindings.tangentVB.IsValid())
        flags &= ~VertexAttributeFlags::HasTangent;
    if (ignoreVertexColor || !bindings.colorVB.IsValid())
        flags &= ~VertexAttributeFlags::HasColor;
    if (!bindings.uv1VB.IsValid())
        flags &= ~VertexAttributeFlags::HasUV1;
    for (size_t i = 0; i < bindings.extraUvVB.size(); ++i)
    {
        const auto uvFlag = static_cast<VertexAttributeFlags>(static_cast<uint32_t>(VertexAttributeFlags::HasUV2) << i);
        if (!bindings.extraUvVB[i].IsValid())
            flags &= ~uvFlag;
    }
    if (!bindings.jointsVB.IsValid())
        flags &= ~VertexAttributeFlags::HasJoints;
    if (!bindings.weightsVB.IsValid())
        flags &= ~VertexAttributeFlags::HasWeights;
    if (!bindings.joints1VB.IsValid())
        flags &= ~VertexAttributeFlags::HasJoints1;
    if (!bindings.weights1VB.IsValid())
        flags &= ~VertexAttributeFlags::HasWeights1;
    return flags;
}

} // namespace Rendering
} // namespace GameEngine
