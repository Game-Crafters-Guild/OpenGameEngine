#pragma once

#include "Rendering/Core/AccelerationStructure.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
class MeshGPURegistry;
struct GPUInstance;
struct ShaderMeta;
}  // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class SceneAccelerationStructureService;

// Per-INSTANCE BLAS cache for skinned meshes, feeding DDGI's TLAS. The shared
// mesh-keyed BLAS pool (SceneAccelerationStructureService) deliberately
// refuses skinned buckets — a bind-pose BLAS occludes where a limb was — and
// two instances of one mesh play different animations, so skinned geometry is
// per-instance by nature: each entry owns a posed OBJECT-space position
// buffer (written by ddgi_skin_positions.comp from the same bone-palette
// atlas the vertex path reads) and a BLAS rebuilt in place over it. Object
// space keeps the TLAS row's transform identical to the static path's.
//
// Rebuild, not refit: the backend has no update mode, topology never changes
// so the created storage always suffices, and a character-sized BLAS build is
// cheap at DDGI cadence. Entries are keyed on (skeleton runtimeId, meshIndex)
// — stable across GPUScene compaction — with bind-pose instances (palette 0,
// identical pose by construction) collapsing onto a shared runtimeId-0 entry
// per mesh. Entries retire after a disuse window.
class DDGISkinnedGeometry
{
  public:
    DDGISkinnedGeometry(Rendering::IDevice* device,
                        Rendering::MeshGPURegistry* meshRegistry,
                        SceneAccelerationStructureService* sceneAS);
    ~DDGISkinnedGeometry();
    DDGISkinnedGeometry(const DDGISkinnedGeometry&) = delete;
    DDGISkinnedGeometry& operator=(const DDGISkinnedGeometry&) = delete;

    // One skinned BLAS (re)build for the caller's AS pass to record, plus the
    // buffer whose write the pass must order itself after.
    struct PendingSkinnedBuild
    {
        Rendering::AccelerationStructureHandle Handle;
        Rendering::BlasTriangleGeometry Geometry;
        // Imported RG handle of the posed position buffer this build reads —
        // the caller's AS pass declares a Read on it so the graph orders the
        // skin-compute write before the BLAS build.
        Rendering::RenderGraph::RGBuffer SkinnedRG{};
    };

    // One TLAS row: the instance's index in this tick's GPUInstance list and
    // the per-instance BLAS to reference. Only entries whose BLAS content has
    // an exec-confirmed build are returned — a never-built BLAS must not
    // reach the TLAS.
    struct SkinnedTlasRow
    {
        uint32_t InstanceIndex = 0;
        uint64_t BlasAddress = 0;
    };

    // GPU geometry row for one skinned instance's posed buffer — the same
    // 40-byte layout as ddgi_hit_shade.glsl's GE_MeshGeometryDesc, published
    // per INSTANCE (not per mesh) so a hit's face normal and UVs come from
    // the posed surface the ray actually intersected.
    struct SkinnedGeomRowGPU
    {
        uint64_t VertexAddress = 0;
        uint64_t IndexAddress = 0;
        uint32_t VertexStrideBytes = 0;
        uint32_t FirstVertex = 0;
        uint32_t FirstIndex = 0;
        uint32_t IndexKind = 0;
        uint32_t UV0OffsetBytes = 0xFFFFFFFFu;
        uint32_t Pad = 0;
    };
    static_assert(sizeof(SkinnedGeomRowGPU) == 40, "must match GE_MeshGeometryDesc (std430)");

    struct TickResult
    {
        std::vector<PendingSkinnedBuild> Builds;   // record in the AS pass, before the TLAS build
        std::vector<SkinnedTlasRow> TlasRows;      // include in this tick's TLAS instance list
        // Capture in the AS pass exec and call MarkExecuted alongside the
        // static pool's token — readiness latches from it next tick. Null
        // when Builds is empty.
        std::shared_ptr<std::atomic<bool>> ConfirmToken;
        // Per-GPUScene-instance override map (u32 per instance; 0xFFFFFFFF =
        // no override) and the row array it indexes — upload-ring allocations
        // for the trace kernel to bind this tick. Valid whenever TlasRows is
        // non-empty; the caller binds a 1-element 0xFFFFFFFF map otherwise.
        Rendering::BufferHandle RowMapBuffer{};
        uint64_t RowMapOffset = 0;
        uint64_t RowMapBytes = 0;
        Rendering::BufferHandle GeomRowsBuffer{};
        uint64_t GeomRowsOffset = 0;
        uint64_t GeomRowsBytes = 0;
    };

    // Per accepted hardware-lane tick, BEFORE the TLAS instance list is
    // assembled: sweeps `instances` for skinned entries, (re)creates their
    // position buffers and BLASes, declares the skin-compute pass into
    // `frame` (its writes ordered against the caller's AS pass via the
    // returned Builds' SkinnedPositions buffers), and retires entries that
    // vanished. `paletteBuffer`/`paletteBytes` is the frame's bone-palette
    // atlas — the same one the vertex path binds.
    // `hardwareLane` false (the software lane) keeps the posed-buffer upkeep
    // and the skin-compute pass but skips everything backend-specific — BLAS
    // creation/rebuild, TLAS rows, geometry-row uploads — so a no-ray-query
    // device still gets posed vertex records for the BVH refit
    // (ddgi_bvh_refit.comp) without touching the null backend.
    TickResult Tick(Rendering::RenderGraph::RGFrame& frame,
                    const std::vector<Rendering::GPUInstance>& instances,
                    Rendering::BufferHandle paletteBuffer, uint64_t paletteBytes,
                    bool hardwareLane);

    // Software lane: the posed record buffer for one skinned instance, valid
    // after this tick's Tick() touched it. Returns false when the entry is
    // absent or its vertex count disagrees with `expectedVertexCount` (a
    // repooled mesh mid-transition — skip the refit rather than read OOB).
    struct PosedBuffer
    {
        Rendering::BufferHandle Buffer{};
        Rendering::RenderGraph::RGBuffer RG{};
        uint64_t Bytes = 0;
    };
    bool TryGetPosedBuffer(uint32_t runtimeId, uint32_t meshIndex, uint32_t expectedVertexCount,
                           PosedBuffer& out) const;

    bool HasLiveEntries() const { return !m_Entries.empty(); }

    // Device rebuilt: every handle this cache holds — posed buffers, BLASes,
    // the skin pipeline — is a corpse of the old device, and destroying a
    // stale-generation BLAS handle against the NEW backend could hit a
    // recycled slot. Drop everything without destruction and rebind to the
    // new device; the next Tick recreates from live instances.
    void AbandonDeviceObjects(Rendering::IDevice* device);

    // Retires every entry: BLAS handles through the backend's deferred
    // destroy, posed buffers through the in-flight margin. The owner calls it
    // whenever it gains or drops its claim on the shared pool: no handle may
    // outlive the claim (the pool may then be purged, restarting handle ids),
    // and a software-lane entry has no BLAS for the hardware lane to build.
    // The next Tick recreates entries for the active lane.
    void RetireAll();

  private:
    struct Entry
    {
        Rendering::BufferHandle SkinnedPositions{};
        uint64_t SkinnedPositionsAddress = 0;
        Rendering::AccelerationStructureHandle Blas{};
        uint64_t BlasAddress = 0;
        Rendering::BlasTriangleGeometry Geometry{};
        uint32_t VertexCount = 0;
        // This tick's imported RG handle of SkinnedPositions (valid only for
        // the frame Tick() ran in — RG resource ids are per-frame).
        Rendering::RenderGraph::RGBuffer PosedRG{};
        bool HasUv0 = false;
        bool Ready = false;  // at least one exec-confirmed build
        std::shared_ptr<std::atomic<bool>> PendingConfirm;
        uint64_t LastSeenTick = 0;
    };

    struct SkinnedMeshInfo
    {
        Rendering::BufferHandle CoreVB{};
        Rendering::BufferHandle JointsVB{};
        Rendering::BufferHandle WeightsVB{};
        Rendering::BufferHandle Joints1VB{};
        Rendering::BufferHandle Weights1VB{};
        Rendering::BufferHandle IndexBuffer{};
        uint32_t CoreStrideBytes = 0;
        uint32_t VertexOffset = 0;
        uint32_t Uv0OffsetFloats = 0xFFFFFFFFu;
        uint32_t NormalOffsetFloats = 0xFFFFFFFFu;
        uint32_t VertexCount = 0;
        uint32_t FirstIndex = 0;
        uint32_t IndexCount = 0;
        uint32_t IndexType = 0;
        bool HasSecondInfluence = false;
    };

    bool LoadKernelIfNeeded();
    void RetireEntry(Entry& entry);

    Rendering::IDevice* m_Device = nullptr;
    Rendering::MeshGPURegistry* m_MeshRegistry = nullptr;
    SceneAccelerationStructureService* m_SceneAS = nullptr;

    std::unordered_map<uint64_t, Entry> m_Entries;  // key: runtimeId<<32 | meshIndex
    uint64_t m_TickClock = 0;

    // Skin kernel (ddgi_skin_positions.comp).
    Rendering::ComputePipelineId m_SkinPipeline{};
    Rendering::DescriptorSetLayoutDesc m_SkinSet0Layout{};
    std::unique_ptr<Rendering::ShaderMeta> m_SkinMeta;
    bool m_SkinLoadAttempted = false;

    // Deferred buffer retirement (same frame-margin discipline as the
    // feature's instance-staging buffers — DestroyBuffer is not deferred).
    struct RetiredBuffer
    {
        Rendering::BufferHandle Buffer{};
        uint64_t TickStamp = 0;
    };
    std::vector<RetiredBuffer> m_RetiredBuffers;

    bool m_WarnedOverCap = false;
};

}  // namespace GameEngine::Engine::Renderer
