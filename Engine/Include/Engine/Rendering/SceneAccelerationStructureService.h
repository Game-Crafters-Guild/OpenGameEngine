#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Types/ScopedSubscription.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/AccelerationStructure.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Rendering
{
class GPUScene;
class IDevice;
namespace RenderGraph
{
class RGFrame;
}
}  // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

// Shared BLAS pool + multi-slot TLAS plumbing for every ray-query consumer
// (the DirectionalShadowMode::RayTraced shadow-mask lane, GI probe tracing).
// One BLAS per pooled static mesh, keyed by GPUScene's mesh SSBO slot —
// built once and reused by every consumer's TLAS, since BLAS content
// (triangle geometry) has no per-consumer filter. Each consumer acquires its
// own Rendering::TlasSlotHandle (SceneAS backend) for its own filtered
// instance set and still owns: its epoch/content-hash policy, its filtered
// TlasInstanceData construction, its instance staging buffer lifetime, and
// its own RG pass declaration/exec — this service does not know what a
// "shadow caster" or a "GI-relevant instance" is.
//
// Deliberately narrow, mirroring RTShadowMaskService's original prototype
// scope (this is its BLAS-pool half, extracted so a second consumer stops
// duplicating BLAS builds against the same meshes): skinned meshes excluded
// (no post-skin vertex buffer), no BLAS compaction yet. See MakeSourceKey for
// why the sweep revalidates every registry entry instead of trusting the
// reload notification alone.
class SceneAccelerationStructureService
{
  public:
    SceneAccelerationStructureService(Rendering::IDevice* device,
                                      Rendering::MeshGPURegistry* meshRegistry,
                                      Rendering::GPUScene* gpuScene);
    ~SceneAccelerationStructureService();
    SceneAccelerationStructureService(const SceneAccelerationStructureService&) = delete;
    SceneAccelerationStructureService& operator=(const SceneAccelerationStructureService&) = delete;

    // The pool's per-frame step. Owned by the frame spine (FrameOrchestrator):
    // exactly one call per app frame, ahead of every consumer's entry point,
    // whether or not any consumer is active. In order it
    //  - advances the backend's deferred-destruction clock and rewinds its
    //    per-frame scratch cursor;
    //  - drains the previous frame's build confirmations (IsBlasReady reflects
    //    work that provably ran) and drops every BLAS on a mesh-registry
    //    reload notification;
    //  - sweeps the registry for new/changed BLAS candidates, but only while a
    //    consumer holds a channel — an unclaimed pool creates no storage;
    //  - purges the pool once it has gone unclaimed for
    //    kUnclaimedFramesBeforePurge frames (the grace window lets a consumer
    //    that toggles off and back on keep its BLASes).
    void BeginFrame();

    // True when a BLAS became Ready in this frame's BeginFrame. A consumer
    // whose TLAS refresh is gated on content changes ORs this in so a
    // just-built mesh joins its TLAS without waiting for an epoch bump.
    bool BlasBecameReadyThisFrame() const { return m_BlasBecameReadyThisFrame; }

    bool IsBlasReady(uint32_t gpuMeshIndex) const;
    // 0 if the mesh has no BLAS yet or it is not yet Ready.
    uint64_t GetBlasAddress(uint32_t gpuMeshIndex) const;

    // One pending BLAS build, scratch-reserved and ready to record.
    struct PendingBlasBuild
    {
        uint32_t MeshIndex = 0;
        Rendering::AccelerationStructureHandle Handle;
        Rendering::BlasTriangleGeometry Geometry;
    };

    // Confirmation token for one CollectPendingBuilds() call. The caller's
    // exec lambda captures the returned shared_ptr by value and calls
    // MarkExecuted() once its recording of every returned build provably
    // ran. MarkExecuted touches only an atomic — safe to call from a record
    // worker thread (Rendering::IAccelerationStructureBackend's Record* calls
    // may run there). The actual BLAS Ready mutation stays render-thread-only,
    // applied by the NEXT frame's BeginFrame — mirrors
    // RTShadowMaskService's original ConfirmBatch discipline: a BLAS becomes
    // instance-visible only after its build is confirmed to have run,
    // because dereferencing an unbuilt BLAS in a TLAS is a device fault, not
    // a wrong pixel.
    class BuildConfirmToken
    {
      public:
        void MarkExecuted() { m_Executed.store(true, std::memory_order_release); }

      private:
        friend class SceneAccelerationStructureService;
        std::atomic<bool> m_Executed{false};
        std::vector<uint32_t> m_MeshIndexes;
    };

    // Claims every BLAS build that is pending (created by BeginFrame's sweep,
    // not yet Ready) and not already claimed this frame, reserving its
    // backend scratch region. CLAIMED ONCE PER FRAME: if two consumers both
    // call this the same frame, the second gets whatever the first left
    // unclaimed (normally nothing), so each build is recorded once. A claimed
    // build whose pass did not run is offered again next frame. `outToken` is set to nullptr
    // when the returned list is empty (nothing to confirm); otherwise the
    // caller must record every returned build via the backend
    // (GetBackend()->RecordBlasBuild) inside its own RG pass exec and call
    // MarkExecuted() on the token there.
    std::vector<PendingBlasBuild> CollectPendingBuilds(std::shared_ptr<BuildConfirmToken>& outToken);

    // A channel is a consumer's claim on the pool: acquire it on the first
    // frame the consumer traces, release it (which frees the consumer's own
    // TLAS) on the first frame it stops, together with every backend handle
    // it created itself. BeginFrame sweeps only while a claim is held and
    // purges the pool after it goes unclaimed; no consumer purges the pool,
    // so no consumer can release another's BLASes.
    Rendering::TlasSlotHandle AcquireTlasChannel(const char* debugName);
    void ReleaseTlasChannel(Rendering::TlasSlotHandle channel);
    Rendering::IAccelerationStructureBackend* GetBackend() const { return m_Backend; }
    // After an in-place device rebuild: the backend destroyed every BLAS with
    // the old device and restarted BLAS ids, so every handle, address and
    // buffer this service holds is dead, and a stale id could name a fresh
    // BLAS. Forgets them without destroying; the next claimed BeginFrame
    // cold-rebuilds. Held TLAS channels stay valid: the backend keeps its slots.
    void OnDeviceRebuilt();
    // The device this service (and every consumer sharing it) was
    // constructed with — consumers use it for their OWN buffers/shaders/
    // pipelines unrelated to the shared BLAS pool, so they need not also
    // carry their own IDevice* from the caller.
    Rendering::IDevice* GetDevice() const { return m_Device; }

    uint32_t GetBlasCount() const;
    uint64_t GetLiveBlasMemoryBytes() const;

    // GPU-visible per-mesh geometry descriptor, one row per Ready BLAS,
    // indexed by gpuMeshIndex (same indexing as GPUScene's own mesh buffer —
    // sized to GPUScene::GetMeshCount()). Lets a ray-hit shading kernel that
    // has no vertex-input-stage binding (a compute shader, not a vertex
    // shader) fetch the hit triangle's vertex positions via GL_EXT_buffer_reference,
    // exactly the same {VertexAddress, IndexAddress, stride, first*, indexKind}
    // this service already computes host-side to build the BLAS — this only
    // republishes it as an SSBO. GLSL mirror: GE_MeshGeometryDesc in
    // Includes/ddgi_hit_shade.glsl (std430, 32 B, locked by the static_assert below).
    // Sentinel UV0 byte offset: the mesh carries no UV0 stream.
    static constexpr uint32_t kNoUV0Offset = 0xFFFFFFFFu;

    struct MeshGeometryDescGPU
    {
        uint64_t VertexAddress     = 0;
        uint64_t IndexAddress      = 0;
        uint32_t VertexStrideBytes = 0;
        uint32_t FirstVertex       = 0;
        uint32_t FirstIndex        = 0;
        uint32_t IndexKind         = 0;  // Rendering::IndexType
        // Byte offset of UV0 within the interleaved core vertex, derived from
        // the mesh's VertexAttributeFlags (position always leads, so position
        // needs no equivalent). kNoUV0Offset for a mesh with no UV0 stream.
        uint32_t UV0OffsetBytes    = kNoUV0Offset;
        uint32_t Pad               = 0;
    };
    static_assert(sizeof(MeshGeometryDescGPU) == 40,
                 "MeshGeometryDescGPU must match Includes/ddgi_hit_shade.glsl's GE_MeshGeometryDesc (std430, 40 B)");

    // Uploads a row for every BLAS that became Ready since the last call and
    // was not yet published (host-visible persistently-mapped buffer; growth
    // recreates and copies forward). Call once per frame AFTER BeginFrame
    // (a build confirmed this frame is not Ready until next frame's sweep, so
    // there is nothing to publish before that). Returns the buffer to bind —
    // invalid until the first mesh publishes.
    Rendering::BufferHandle PublishMeshGeometry();

    // Bind-pose ATTRIBUTE rows for skinned meshes: the pool builds no BLAS
    // for a skinned bucket, but a per-instance skinned BLAS (DDGI's
    // DDGISkinnedGeometry) still reports the mesh's meshIndex at hit time,
    // and the hit-shade kernel dereferences this table's addresses raw — a
    // zero row is a device-loss, not a soft miss. The row points at the
    // bind-pose pool streams: UVs and shared-index topology are exact; a
    // posed limb's face normal is bind-pose (acceptable for diffuse bounce).
    uint32_t GetMeshGeometryCapacity() const { return m_MeshGeometryCapacity; }
    // Byte size of the buffer PublishMeshGeometry returns — a consumer's
    // NamedDescriptorWriter::AddStorageBuffer needs an explicit size, and
    // MeshGeometryDescGPU is private (this avoids exposing the type just for
    // a sizeof).
    uint64_t GetMeshGeometryCapacityBytes() const
    {
        return static_cast<uint64_t>(m_MeshGeometryCapacity) * sizeof(MeshGeometryDescGPU);
    }

  private:
    // Identity of the registry entry a BLAS was derived from — see
    // RTShadowMaskService's original comment (unchanged reasoning, just no
    // longer shadow-specific): the registry's unregister notification fires
    // on every row-freeing path but not on a direct RegisterSubmesh that
    // swaps content into the same handle/row, so the sweep revalidates every
    // visited entry against this key and rebuilds on any divergence.
    struct BlasSourceKey
    {
        uint64_t ContentHash      = 0;
        uint64_t CoreOffsetBytes  = 0;
        uint64_t IndexOffsetBytes = 0;
        uint32_t BucketKey        = 0;
        uint32_t CorePoolIndex    = 0;
        uint32_t IndexPoolIndex   = 0;
        uint32_t VertexOffset     = 0;
        uint32_t FirstIndex       = 0;
        uint32_t IndexCount       = 0;
        uint32_t IndexKind        = 0;
        bool operator==(const BlasSourceKey&) const = default;
    };
    static BlasSourceKey MakeSourceKey(const Rendering::MeshGPUEntry& entry);

    struct MeshBlas
    {
        Rendering::AccelerationStructureHandle Handle;
        uint64_t Address = 0;
        bool Ready       = false;
        // The m_ClaimFrame value at which CollectPendingBuilds last claimed
        // this build (0 = never). Compared against the CURRENT m_ClaimFrame
        // rather than reset with an O(BLAS count) sweep every frame: once
        // BeginFrame bumps m_ClaimFrame, every entry's claim goes stale
        // automatically, so an unconfirmed build (its consumer's pass culled,
        // errored, or simply didn't run) is offered again next frame instead
        // of being silently stuck un-Ready forever.
        uint64_t ClaimedFrame = 0;
        Rendering::BlasTriangleGeometry Geometry;  // kept while pending, for re-record
        // Not part of BlasTriangleGeometry (the AS builder only ever reads
        // positions); resolved once at creation so PublishMeshGeometry does
        // not have to go back to the registry entry.
        uint32_t UV0OffsetBytes = kNoUV0Offset;
        BlasSourceKey Source;
    };

    bool ConfirmExecutedBuilds();
    void SweepRegistryForNewBlas();
    void RetireMeshGeometryBuffers();
    void PurgePool();

    Rendering::IDevice* m_Device                       = nullptr;
    Rendering::MeshGPURegistry* m_MeshRegistry          = nullptr;
    Rendering::GPUScene* m_GpuScene                     = nullptr;
    Rendering::IAccelerationStructureBackend* m_Backend = nullptr;

    // gpuMeshIndex (GPUScene mesh SSBO slot == GPUInstance.meshIndex) → BLAS.
    std::unordered_map<uint32_t, MeshBlas> m_BlasByMeshIndex;
    uint64_t m_ClaimFrame = 0;  // bumped by BeginFrame; see MeshBlas::ClaimedFrame

    // Outstanding confirmations from CollectPendingBuilds, drained at the
    // START of each BeginFrame (render-thread-only mutation of Ready).
    std::vector<std::shared_ptr<BuildConfirmToken>> m_PendingConfirmTokens;

    // Unsubscribes on destruction; safe even if the registry died first.
    ScopedSubscription m_ReloadSubscription;
    std::atomic<bool> m_ReloadPending{false};

    // Skip tripwires for the summary log (fail-visible, never silent) —
    // degenerate-BLAS-geometry only; per-consumer tripwires (sector,
    // shadow-caster filtering, ...) stay with the consumer.
    uint64_t m_SkippedDegenerate  = 0;
    uint64_t m_RebuiltStaleBlas   = 0;
    bool m_WarnedDegenerate       = false;

    // Mesh geometry descriptor publication (host-visible, persistently
    // mapped; grown by recreate-and-copy like MeshGPURegistry's pools). A
    // growth event retires the OLD buffer rather than destroying it
    // immediately — a still-in-flight frame's compute dispatch may hold a
    // descriptor bound to it (same hazard, same fix as
    // RTShadowMaskService::m_RetiredInstanceBuffers).
    Rendering::BufferHandle m_MeshGeometryBuffer{};
    void* m_MeshGeometryMapped        = nullptr;
    uint32_t m_MeshGeometryCapacity   = 0;  // rows the current buffer can hold
    uint32_t m_HeldChannelCount = 0;  // claims on the pool; see AcquireTlasChannel
    uint64_t m_UnclaimedFrames  = 0;  // consecutive BeginFrames with no claim
    bool m_BlasBecameReadyThisFrame = false;
    // gpuMeshIndex values already written into m_MeshGeometryBuffer — avoids
    // re-uploading every Ready mesh's row every frame.
    std::unordered_map<uint32_t, bool> m_MeshGeometryPublished;
    // See PublishMeshGeometry's doc: bind-pose attribute rows for skinned
    // meshes (no pool BLAS), keyed by gpuMeshIndex, refreshed by the sweep.
    std::unordered_map<uint32_t, MeshGeometryDescGPU> m_SkinnedAttributeRows;
    struct RetiredBuffer
    {
        Rendering::BufferHandle Buffer{};
        uint64_t FrameStamp = 0;
    };
    std::vector<RetiredBuffer> m_RetiredMeshGeometryBuffers;
    uint64_t m_FrameClock = 0;  // advanced by BeginFrame
};

}  // namespace GameEngine::Engine::Renderer
