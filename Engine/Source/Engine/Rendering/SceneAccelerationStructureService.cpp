#include "Engine/Rendering/SceneAccelerationStructureService.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"

namespace GameEngine::Engine::Renderer
{

namespace
{

// Sweep budget: BLAS creations per frame. Small enough to bound the per-frame
// scratch batch (and its growth churn) during streaming scene loads; a
// Synty-scale scene (~900 uniques) completes in a handful of frames. Carried
// verbatim from RTShadowMaskService's original constant.
constexpr size_t kMaxBlasBuildsPerFrame = 128;

// Byte offset of UV0 inside the interleaved core vertex (binding 0), per
// VertexLayoutBuilder's layout: position (vec3) then, if present, normal
// (vec3), then UV0. Position always leads, which is why it needs no
// equivalent; UV0's offset is the one that moves with the flags. Consumed by
// DDGI's hardware trace kernel, which fetches the hit triangle's UVs by device
// address and has no vertex-input stage to lay them out for it.
uint32_t CoreVertexUV0Offset(Rendering::VertexAttributeFlags flags)
{
    using Rendering::VertexAttributeFlags;
    if ((flags & VertexAttributeFlags::HasUV0) == VertexAttributeFlags::None)
        return SceneAccelerationStructureService::kNoUV0Offset;
    constexpr uint32_t kVec3Bytes = 3u * sizeof(float);
    uint32_t offset = kVec3Bytes;  // past position
    if ((flags & VertexAttributeFlags::HasNormal) != VertexAttributeFlags::None)
        offset += kVec3Bytes;
    return offset;
}

// Pool purge: consecutive BeginFrames with no consumer holding a channel
// before every BLAS and the scratch are released. Frame-based so it shares the
// clock every other deferred margin here uses. 900 frames is 15 s at 60 fps
// and ~4 s at a 240 fps editor — far past any A/B toggle of a consumer, so
// toggling never thrashes a purge + cold-rebuild cycle, while an abandoned
// pool still returns its VRAM within seconds.
constexpr uint64_t kUnclaimedFramesBeforePurge = 900;

// Debug override for exercising the purge without waiting out the threshold
// (runtime smoke, purge-path bisection). Never ship a tiny value: the constant
// above is the shipped behavior.
uint64_t UnclaimedFramesBeforePurge()
{
    static const uint64_t s_Threshold = []
    {
        if (const char* v = std::getenv("GE_SCENE_AS_PURGE_FRAMES"))
        {
            const unsigned long long parsed = std::strtoull(v, nullptr, 10);
            if (parsed > 0)
                return static_cast<uint64_t>(parsed);
        }
        return kUnclaimedFramesBeforePurge;
    }();
    return s_Threshold;
}

}  // namespace

SceneAccelerationStructureService::SceneAccelerationStructureService(
    Rendering::IDevice* device, Rendering::MeshGPURegistry* meshRegistry,
    Rendering::GPUScene* gpuScene)
    : m_Device(device), m_MeshRegistry(meshRegistry), m_GpuScene(gpuScene)
{
    m_Backend = m_Device ? m_Device->GetAccelerationStructureBackend() : nullptr;
    if (m_MeshRegistry)
    {
        // See BlasSourceKey's doc: this is the coarse half of the safety net
        // (drop-all on any row-freeing notification); the sweep's per-entry
        // BlasSourceKey revalidation covers the remaining un-notified case
        // (in-place content swap into the same handle/row).
        m_ReloadSubscription = m_MeshRegistry->SubscribeReload(
            [this](const GameEngine::GUID& /*assetGuid*/)
            { m_ReloadPending.store(true, std::memory_order_relaxed); });
    }
}

SceneAccelerationStructureService::~SceneAccelerationStructureService()
{
    if (m_MeshGeometryBuffer.IsValid() && m_Device)
        m_Device->DestroyBuffer(m_MeshGeometryBuffer);
    if (m_Device)
        for (const RetiredBuffer& r : m_RetiredMeshGeometryBuffers)
            m_Device->DestroyBuffer(r.Buffer);
    // BLAS/TLAS objects are backend-owned; the device tears the backend down.
}

uint32_t SceneAccelerationStructureService::GetBlasCount() const
{
    return static_cast<uint32_t>(m_BlasByMeshIndex.size());
}

uint64_t SceneAccelerationStructureService::GetLiveBlasMemoryBytes() const
{
    return m_Backend ? m_Backend->GetTotalBlasMemoryBytes() : 0u;
}

SceneAccelerationStructureService::BlasSourceKey SceneAccelerationStructureService::MakeSourceKey(
    const Rendering::MeshGPUEntry& entry)
{
    BlasSourceKey k{};
    k.ContentHash      = entry.contentHash;
    k.CoreOffsetBytes  = entry.coreSubAlloc.offset;
    k.IndexOffsetBytes = entry.indexSubAlloc.offset;
    k.BucketKey        = static_cast<uint32_t>(entry.bucketKey);
    k.CorePoolIndex    = entry.corePoolIndex;
    k.IndexPoolIndex   = entry.indexPoolIndex;
    k.VertexOffset     = entry.vertexOffset;
    k.FirstIndex       = entry.firstIndex;
    k.IndexCount       = entry.indexCount;
    k.IndexKind        = entry.indexType;
    return k;
}

void SceneAccelerationStructureService::SweepRegistryForNewBlas()
{
    size_t created = 0;
    m_MeshRegistry->ForEachEntry(
        [&](const Rendering::MeshGPUEntry& entry)
        {
            if (entry.gpuMeshIndex == ~0u)
                return;
            const BlasSourceKey source = MakeSourceKey(entry);
            const auto existing        = m_BlasByMeshIndex.find(entry.gpuMeshIndex);
            if (existing != m_BlasByMeshIndex.end())
            {
                if (existing->second.Source == source)
                    return;
                // In-place content swap under the same handle/row (no
                // row-freeing notification fires for this — see the header
                // doc): drop the stale BLAS; the fall-through below rebuilds
                // from the current entry. Every consumer's TLAS gate must
                // close until the rebuilt content confirms, never reference
                // stale triangles — consumers see this via IsBlasReady
                // flipping back to false for this meshIndex.
                m_Backend->DestroyBlas(existing->second.Handle);
                m_BlasByMeshIndex.erase(existing);
                ++m_RebuiltStaleBlas;
            }
            if (created >= kMaxBlasBuildsPerFrame)
                return;
            // Prototype exclusion (carried from the original shadow-mask
            // lane): skinned buckets carry bind-pose geometry only. A
            // bind-pose BLAS is worse than the enumerated no-BLAS diff.
            // The mesh still gets an ATTRIBUTE row (m_SkinnedAttributeRows):
            // DDGI's per-instance skinned BLASes hit-shade through the
            // geometry table by meshIndex, and a zero row is a raw
            // device-address dereference of 0.
            if ((entry.vertexFlags & Rendering::VertexAttributeFlags::Skinned)
                != Rendering::VertexAttributeFlags::None)
            {
                const uint32_t skinnedStride = m_MeshRegistry->GetEntryCoreStrideBytes(entry);
                Rendering::MeshGPUEntryBindings skinnedBindings{};
                if (skinnedStride < 12u ||
                    !m_MeshRegistry->TryGetDrawableBindings(entry, skinnedBindings))
                    return;
                MeshGeometryDescGPU row{};
                row.VertexAddress     = m_Device->GetBufferDeviceAddress(skinnedBindings.coreVB);
                row.IndexAddress      = m_Device->GetBufferDeviceAddress(skinnedBindings.indexBuffer);
                row.VertexStrideBytes = skinnedStride;
                row.FirstVertex       = entry.vertexOffset;
                row.FirstIndex        = entry.firstIndex;
                row.IndexKind         = entry.indexType;
                row.UV0OffsetBytes    = CoreVertexUV0Offset(entry.vertexFlags);
                if (row.VertexAddress == 0 || row.IndexAddress == 0)
                    return;
                auto [it, inserted] = m_SkinnedAttributeRows.try_emplace(entry.gpuMeshIndex, row);
                if (!inserted && std::memcmp(&it->second, &row, sizeof(row)) != 0)
                {
                    it->second = row;
                    m_MeshGeometryPublished.erase(entry.gpuMeshIndex);
                }
                return;
            }
            if (entry.indexCount < 3u
                || entry.topology != Rendering::PrimitiveTopology::TriangleList)
                return;

            const uint32_t coreStrideBytes = m_MeshRegistry->GetEntryCoreStrideBytes(entry);
            if (coreStrideBytes == 0)
                return;
            Rendering::MeshGPUEntryBindings bindings{};
            if (!m_MeshRegistry->TryGetDrawableBindings(entry, bindings))
                return;  // Not drawable yet; the next sweep re-tries it.

            Rendering::BlasTriangleGeometry g{};
            g.VertexAddress     = m_Device->GetBufferDeviceAddress(bindings.coreVB);
            g.VertexStrideBytes = coreStrideBytes;
            g.FirstVertex       = entry.vertexOffset;
            const uint64_t localVertexCount = entry.coreSubAlloc.size / coreStrideBytes;
            if (localVertexCount == 0)
                return;
            g.MaxVertex    = entry.vertexOffset + static_cast<uint32_t>(localVertexCount) - 1u;
            g.IndexAddress = m_Device->GetBufferDeviceAddress(bindings.indexBuffer);
            g.FirstIndex   = entry.firstIndex;  // LOD0 only (documented residual)
            g.IndexCount   = entry.indexCount;
            g.IndexKind    = static_cast<Rendering::IndexType>(entry.indexType);
            if (g.VertexAddress == 0 || g.IndexAddress == 0)
                return;
            // Geometry sanity tripwire (fail-visible): a torn or corrupt
            // entry must never reach the driver's BVH builder.
            if (g.IndexCount > 50000000u || g.VertexStrideBytes < 12u
                || g.VertexStrideBytes > 512u || localVertexCount > (1u << 24))
            {
                if (!m_WarnedDegenerate)
                {
                    m_WarnedDegenerate = true;
                    Logger::Log::Warning(
                        "SceneAS: skipped implausible BLAS geometry (gpuMeshIndex {}: {} "
                        "indices, stride {}, {} vertices) — first occurrence, further skips "
                        "counted silently",
                        entry.gpuMeshIndex, g.IndexCount, g.VertexStrideBytes, localVertexCount);
                }
                ++m_SkippedDegenerate;
                return;
            }

            const Rendering::AccelerationStructureHandle handle = m_Backend->CreateBlas(g);
            if (!handle.IsValid())
            {
                Logger::Log::Warning("SceneAS: BLAS creation failed for gpuMeshIndex {} ({} indices)",
                                     entry.gpuMeshIndex, entry.indexCount);
                return;
            }
            MeshBlas blas;
            blas.UV0OffsetBytes = CoreVertexUV0Offset(entry.vertexFlags);
            blas.Handle   = handle;
            blas.Address  = m_Backend->GetBlasDeviceAddress(handle);
            blas.Ready    = false;
            blas.Geometry = g;
            blas.Source   = source;
            m_BlasByMeshIndex.emplace(entry.gpuMeshIndex, std::move(blas));
            ++created;
        });
}

bool SceneAccelerationStructureService::ConfirmExecutedBuilds()
{
    // Drain confirmations from builds recorded (and provably executed) by a
    // PREVIOUS frame's consumer exec lambda. Render-thread-only mutation of
    // Ready — see BuildConfirmToken's doc for why this cannot happen inside
    // the exec lambda itself.
    bool anyBecameReady = false;
    std::erase_if(m_PendingConfirmTokens,
                  [&](const std::shared_ptr<BuildConfirmToken>& token)
                  {
                      if (!token->m_Executed.load(std::memory_order_acquire))
                          return false;
                      for (uint32_t meshIndex : token->m_MeshIndexes)
                      {
                          auto it = m_BlasByMeshIndex.find(meshIndex);
                          if (it != m_BlasByMeshIndex.end())
                          {
                              it->second.Ready = true;
                              anyBecameReady   = true;
                          }
                      }
                      return true;
                  });

    if (m_ReloadPending.exchange(false, std::memory_order_relaxed))
    {
        for (auto& [meshIndex, blas] : m_BlasByMeshIndex)
            m_Backend->DestroyBlas(blas.Handle);
        m_BlasByMeshIndex.clear();
        m_PendingConfirmTokens.clear();  // confirmations for dropped BLASes are moot
        m_MeshGeometryPublished.clear(); // republish once the rebuilt BLASes go Ready
        m_SkinnedAttributeRows.clear();  // stale pool addresses — the next sweep re-derives
    }
    return anyBecameReady;
}

void SceneAccelerationStructureService::BeginFrame()
{
    m_BlasBecameReadyThisFrame = false;
    if (!m_Backend || !m_MeshRegistry)
        return;

    m_Backend->CollectGarbage();
    ++m_FrameClock;
    RetireMeshGeometryBuffers();

    ++m_ClaimFrame;  // new frame: every MeshBlas::ClaimedFrame is now stale automatically
    m_BlasBecameReadyThisFrame = ConfirmExecutedBuilds();

    if (m_HeldChannelCount > 0)
    {
        m_UnclaimedFrames = 0;
        SweepRegistryForNewBlas();
    }
    else if (++m_UnclaimedFrames == UnclaimedFramesBeforePurge())
    {
        PurgePool();
    }
}

bool SceneAccelerationStructureService::IsBlasReady(uint32_t gpuMeshIndex) const
{
    const auto it = m_BlasByMeshIndex.find(gpuMeshIndex);
    return it != m_BlasByMeshIndex.end() && it->second.Ready;
}

uint64_t SceneAccelerationStructureService::GetBlasAddress(uint32_t gpuMeshIndex) const
{
    const auto it = m_BlasByMeshIndex.find(gpuMeshIndex);
    return (it != m_BlasByMeshIndex.end() && it->second.Ready) ? it->second.Address : 0;
}

std::vector<SceneAccelerationStructureService::PendingBlasBuild>
SceneAccelerationStructureService::CollectPendingBuilds(std::shared_ptr<BuildConfirmToken>& outToken)
{
    outToken.reset();
    std::vector<PendingBlasBuild> out;
    for (auto& [meshIndex, blas] : m_BlasByMeshIndex)
    {
        if (blas.Ready || blas.ClaimedFrame == m_ClaimFrame)
            continue;
        if (!m_Backend->ReserveBlasScratch(blas.Handle))
        {
            Logger::Log::Warning(
                "SceneAS: scratch reservation failed for gpuMeshIndex {}; build deferred a frame",
                meshIndex);
            continue;
        }
        blas.ClaimedFrame = m_ClaimFrame;
        out.push_back(PendingBlasBuild{meshIndex, blas.Handle, blas.Geometry});
    }
    if (out.empty())
        return out;

    auto token = std::make_shared<BuildConfirmToken>();
    token->m_MeshIndexes.reserve(out.size());
    for (const PendingBlasBuild& b : out)
        token->m_MeshIndexes.push_back(b.MeshIndex);
    m_PendingConfirmTokens.push_back(token);
    outToken = std::move(token);
    return out;
}

void SceneAccelerationStructureService::RetireMeshGeometryBuffers()
{
    // Retire mesh-geometry buffers older than the frames-in-flight margin
    // (+1 for the same conservative slack the AS backend uses) — mirrors
    // RTShadowMaskService's instance-buffer retirement exactly.
    constexpr uint64_t kRetireMargin = Rendering::IDevice::kMaxSupportedFramesInFlight + 1;
    std::erase_if(m_RetiredMeshGeometryBuffers,
                  [&](const RetiredBuffer& r)
                  {
                      if (m_FrameClock - r.FrameStamp <= kRetireMargin)
                          return false;
                      if (m_Device)
                          m_Device->DestroyBuffer(r.Buffer);
                      return true;
                  });
}

void SceneAccelerationStructureService::OnDeviceRebuilt()
{
    m_BlasByMeshIndex.clear();
    m_PendingConfirmTokens.clear();
    m_SkippedDegenerate        = 0;
    m_RebuiltStaleBlas         = 0;
    m_WarnedDegenerate         = false;
    m_BlasBecameReadyThisFrame = false;
    m_MeshGeometryBuffer       = {};
    m_MeshGeometryMapped       = nullptr;
    m_MeshGeometryCapacity     = 0;
    m_MeshGeometryPublished.clear();
    m_SkinnedAttributeRows.clear();
    m_RetiredMeshGeometryBuffers.clear();
}

void SceneAccelerationStructureService::PurgePool()
{
    const uint32_t blasCount     = m_Backend->GetLiveBlasCount();
    const uint64_t releasedBytes = m_Backend->ReleaseAllStructures();

    m_BlasByMeshIndex.clear();
    m_PendingConfirmTokens.clear();
    m_SkippedDegenerate = 0;
    m_RebuiltStaleBlas  = 0;
    m_WarnedDegenerate  = false;
    m_MeshGeometryPublished.clear();  // republish once the cold-rebuilt BLASes go Ready

    if (releasedBytes > 0)
        Logger::Log::Info("SceneAS: no consumer claimed the pool for {} frames — {} BLAS + scratch, "
                          "{:.1f} MiB queued for deferred release; the next claim cold-rebuilds",
                          UnclaimedFramesBeforePurge(), blasCount,
                          static_cast<double>(releasedBytes) / (1024.0 * 1024.0));
}

namespace
{
SceneAccelerationStructureService::MeshGeometryDescGPU MakeGeometryDescGPU(
    const Rendering::BlasTriangleGeometry& g, uint32_t uv0OffsetBytes)
{
    SceneAccelerationStructureService::MeshGeometryDescGPU desc{};
    desc.VertexAddress     = g.VertexAddress;
    desc.IndexAddress      = g.IndexAddress;
    desc.VertexStrideBytes = g.VertexStrideBytes;
    desc.FirstVertex       = g.FirstVertex;
    desc.FirstIndex        = g.FirstIndex;
    desc.IndexKind         = static_cast<uint32_t>(g.IndexKind);
    desc.UV0OffsetBytes    = uv0OffsetBytes;
    return desc;
}
}  // namespace

Rendering::BufferHandle SceneAccelerationStructureService::PublishMeshGeometry()
{
    if (!m_Device || !m_GpuScene)
        return m_MeshGeometryBuffer;

    // Which meshes still need a row this call: every Ready BLAS not yet
    // published, PLUS (only when a grow is about to happen) every Ready BLAS
    // period — the grow path re-derives the whole buffer from scratch (see
    // below), so it must size for every live index, not just the unpublished
    // subset. Computed as an index set first so sizing and writing agree.
    bool anyUnpublished = false;
    uint32_t highestReadyIndex = 0;
    bool anyReady = false;
    for (const auto& [meshIndex, blas] : m_BlasByMeshIndex)
    {
        if (!blas.Ready)
            continue;
        anyReady = true;
        highestReadyIndex = std::max(highestReadyIndex, meshIndex);
        if (!m_MeshGeometryPublished.contains(meshIndex))
            anyUnpublished = true;
    }
    for (const auto& [meshIndex, row] : m_SkinnedAttributeRows)
    {
        anyReady = true;
        highestReadyIndex = std::max(highestReadyIndex, meshIndex);
        if (!m_MeshGeometryPublished.contains(meshIndex))
            anyUnpublished = true;
    }
    // Even an empty TLAS needs a valid geometry descriptor. Allocate the
    // zeroed minimum once, then keep the usual no-change fast path.
    if (!anyUnpublished && m_MeshGeometryBuffer.IsValid())
        return m_MeshGeometryBuffer;  // nothing new to publish

    // GetMeshCount() is a soft hint (usually the true capacity), but this
    // buffer must never index past what it actually allocated regardless of
    // that count's exact semantics (live-count vs. capacity is not
    // documented at this call site) — an undersized buffer here is an
    // out-of-bounds WRITE, not a soft miss. Take the max over the hint and
    // the highest index this call could possibly write.
    const uint32_t requiredCapacity =
        std::max(m_GpuScene->GetMeshCount(), anyReady ? highestReadyIndex + 1u : 0u);

    if (requiredCapacity > m_MeshGeometryCapacity || !m_MeshGeometryBuffer.IsValid())
    {
        // Grow-by-doubling avoids a resize per newly-registered mesh during a
        // streaming load (mirrors MeshGPURegistry's pool growth policy).
        uint32_t newCapacity = std::max<uint32_t>(64, m_MeshGeometryCapacity);
        while (newCapacity < requiredCapacity)
            newCapacity *= 2;

        Rendering::BufferDesc desc{};
        desc.size        = static_cast<uint64_t>(newCapacity) * sizeof(MeshGeometryDescGPU);
        desc.usage       = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
        desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        desc.flags       = Rendering::BufferCreateFlags::PersistentlyMapped;
        desc.debugName   = "SceneAS.MeshGeometry";
        const Rendering::BufferHandle newBuffer = m_Device->CreateBuffer(desc);
        void* newMapped = newBuffer.IsValid() ? m_Device->MapBuffer(newBuffer) : nullptr;
        if (!newMapped)
        {
            Logger::Log::Warning("SceneAS: mesh-geometry buffer growth to {} rows failed", newCapacity);
            if (newBuffer.IsValid())
                m_Device->DestroyBuffer(newBuffer);
            return m_MeshGeometryBuffer;
        }
        std::memset(newMapped, 0, desc.size);
        if (m_MeshGeometryBuffer.IsValid())
            m_RetiredMeshGeometryBuffers.push_back(RetiredBuffer{m_MeshGeometryBuffer, m_FrameClock});
        m_MeshGeometryBuffer   = newBuffer;
        m_MeshGeometryMapped   = newMapped;
        m_MeshGeometryCapacity = newCapacity;
        m_MeshGeometryPublished.clear();
        // Re-derive every row from scratch — simpler and equally cheap to a
        // partial copy-forward (growth is rare, and this loop is bounded by
        // the BLAS count already swept this frame), and guarantees no stale
        // bytes ever reach a shader.
        for (const auto& [meshIndex, blas] : m_BlasByMeshIndex)
        {
            if (!blas.Ready || meshIndex >= m_MeshGeometryCapacity)
                continue;  // meshIndex >= capacity: unreachable given the sizing above, but never trust it blind
            const MeshGeometryDescGPU desc2 = MakeGeometryDescGPU(blas.Geometry, blas.UV0OffsetBytes);
            std::memcpy(static_cast<MeshGeometryDescGPU*>(m_MeshGeometryMapped) + meshIndex, &desc2,
                        sizeof(MeshGeometryDescGPU));
            m_MeshGeometryPublished[meshIndex] = true;
        }
        for (const auto& [meshIndex, row] : m_SkinnedAttributeRows)
        {
            if (meshIndex >= m_MeshGeometryCapacity)
                continue;
            std::memcpy(static_cast<MeshGeometryDescGPU*>(m_MeshGeometryMapped) + meshIndex, &row,
                        sizeof(MeshGeometryDescGPU));
            m_MeshGeometryPublished[meshIndex] = true;
        }
        return m_MeshGeometryBuffer;
    }

    for (const auto& [meshIndex, blas] : m_BlasByMeshIndex)
    {
        if (!blas.Ready || m_MeshGeometryPublished.contains(meshIndex))
            continue;
        if (meshIndex >= m_MeshGeometryCapacity)
            continue;  // unreachable given the sizing above; never trust it blind
        const MeshGeometryDescGPU desc = MakeGeometryDescGPU(blas.Geometry, blas.UV0OffsetBytes);
        std::memcpy(static_cast<MeshGeometryDescGPU*>(m_MeshGeometryMapped) + meshIndex, &desc,
                    sizeof(MeshGeometryDescGPU));
        m_MeshGeometryPublished[meshIndex] = true;
    }
    for (const auto& [meshIndex, row] : m_SkinnedAttributeRows)
    {
        if (m_MeshGeometryPublished.contains(meshIndex) || meshIndex >= m_MeshGeometryCapacity)
            continue;
        std::memcpy(static_cast<MeshGeometryDescGPU*>(m_MeshGeometryMapped) + meshIndex, &row,
                    sizeof(MeshGeometryDescGPU));
        m_MeshGeometryPublished[meshIndex] = true;
    }
    return m_MeshGeometryBuffer;
}

Rendering::TlasSlotHandle SceneAccelerationStructureService::AcquireTlasChannel(
    const char* debugName)
{
    if (!m_Backend)
        return {};
    const Rendering::TlasSlotHandle channel = m_Backend->AcquireTlasSlot(debugName);
    if (channel.IsValid())
        ++m_HeldChannelCount;
    return channel;
}

void SceneAccelerationStructureService::ReleaseTlasChannel(Rendering::TlasSlotHandle channel)
{
    if (!m_Backend || !channel.IsValid())
        return;
    assert(m_HeldChannelCount > 0 && "ReleaseTlasChannel: channel released twice or never acquired");
    m_Backend->ReleaseTlasSlot(channel);
    --m_HeldChannelCount;
}

}  // namespace GameEngine::Engine::Renderer
