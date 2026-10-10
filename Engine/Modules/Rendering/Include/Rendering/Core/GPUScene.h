/**
 * @file GPUScene.h
 * @brief GPU Scene Management System for modern GPU-driven rendering
 *
 * This system implements the GPU scene representation from the complete render engine guide,
 * supporting 1M+ objects with GPU-driven culling and rendering.
 */

#pragma once

#include "Rendering/Common/Math.h"
#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Forward declarations
class IDevice;
class CommandList;

/**
 * @brief GPU-resident instance data
 *
 * Represents a single instance of geometry in the scene.
 * Stored in GPU buffers for efficient access during culling and rendering.
 *
 * The single GLSL mirror is Shaders/Includes/gpu_instance_fields.glsl —
 * change both in the same commit (the static_asserts below lock this side).
 */
struct GPUInstance
{
    Matrix4x4 transform;     // World transform matrix
    Matrix4x4 prevTransform; // Previous frame transform (for motion vectors)
    // Precomputed normal matrix = transpose(inverse(mat3(transform))), stored as
    // three 16-byte columns (xyz = column). The first column's spare word
    // carries the render-layer mask without changing the instance stride.
    // Computed CPU-side at
    // extraction time so the GE_INSTANCED vertex shader can dereference it
    // directly instead of paying 9 muls + 9 adds + divide per vertex.
    Vector3 normalMatrixCol0; // 12 B
    uint32_t renderLayerMask = 0xFFFFFFFFu;
    Vector4 normalMatrixCol1; // 16 B
    Vector4 normalMatrixCol2; // 16 B
    uint32_t meshIndex;      // Index into mesh buffer
    uint32_t materialIndex;  // Index into MaterialSystem's per-material tables
    uint32_t flags;          // Instance flags (visible, cast shadows, etc.)
    float lodBias;           // LOD bias for this instance
    Vector3 boundingCenter;  // Bounding sphere center (world space)
    float boundingRadius;    // Bounding sphere radius
    uint32_t skinPaletteOffset; // Offset (in bone-slot units; 1 slot = 3 vec4 rows in the
                                // mat3x4 layout — see Includes/bone_palette.glsl) into
                                // BonePaletteAtlas SSBO. 0 = unskinned, identity slot.
    uint32_t runtimeId;         // Phase 6: SkeletonStore runtime id, used by the
                                // per-runtime visibility-aggregation pass to gate
                                // skinning compute work. 0 for unskinned entities.
    uint32_t sectorPacked[2];   // Packed 21-bit-per-axis render-origin sector for camera-relative
                                // rendering (Engine/Rendering/RenderOrigin.h::PackSector). Zero =>
                                // sector (0,0,0) => transform holds full world space and the 240
                                // bytes match the pre-camera-relative layout exactly, so
                                // UpdateInstance's byte-compare still treats an untagged instance
                                // as identity (quiescence preserved). custom0 still lands on a
                                // std430 vec4 boundary. Always build instances with `GPUInstance{}`.
    Vector4 custom0 = Vector4(1.0f, 1.0f, 1.0f, 1.0f); // Per-instance shader payload.
};
static_assert(offsetof(GPUInstance, normalMatrixCol0) == 128, "GPUInstance.normalMatrixCol0 offset drift");
static_assert(offsetof(GPUInstance, renderLayerMask) == 140, "GPUInstance.renderLayerMask offset drift");
static_assert(offsetof(GPUInstance, normalMatrixCol1) == 144, "GPUInstance.normalMatrixCol1 offset drift");
static_assert(offsetof(GPUInstance, custom0) == 224,
              "GPUInstance.custom0 must match the GLSL std430 layout");
static_assert(sizeof(GPUInstance) == 240,
              "GPUInstance must be 240 bytes to match shader std430 layout");

/**
 * @brief Coalesced scatter-hot mirror of the fields draw_command_scatter.comp
 *        reads from GPUInstance.
 *
 * The scatter reads only 32 of the 240 GPUInstance bytes (meshIndex,
 * materialIndex, flags, lodBias, boundingCenter, boundingRadius) but strides
 * the full 240 B struct, so consecutive threads' reads land in disjoint cache
 * lines (uncoalesced — the 48% fetch cost in the Scatter.World attribution).
 * This tightly-packed 32 B array lets consecutive threads read consecutive
 * bytes → coalesced. It is written alongside GPUInstance in the three instance
 * mutators (Add/Update/Remove) so it stays in exact lockstep with the
 * authoritative buffer; GPUInstance remains the sole source for shading.
 *
 * The single GLSL mirror is the SCATTER_COMPACT branch of
 * draw_command_scatter.comp — field order and types are ABI, change both in
 * the same commit (the static_asserts below lock this side).
 */
struct GPUInstanceScatterHot
{
    uint32_t meshIndex;     // 0  (0xFFFFFFFF = tombstone; self-rejects in scatter)
    uint32_t materialIndex; // 4
    uint32_t flags;         // 8  (bits 16..31 = 16-bit world tag; bit 4 mirrored)
    float lodBias;          // 12
    Vector3 boundingCenter; // 16 (std430 vec3 lands on the 16 B boundary)
    float boundingRadius;   // 28
};
static_assert(sizeof(GPUInstanceScatterHot) == 32,
              "GPUInstanceScatterHot must be 32 bytes to match the shader's std430 layout");
static_assert(offsetof(GPUInstanceScatterHot, boundingCenter) == 16,
              "GPUInstanceScatterHot.boundingCenter must sit on the std430 vec3 boundary");
static_assert(offsetof(GPUInstanceScatterHot, boundingRadius) == 28,
              "GPUInstanceScatterHot.boundingRadius offset drift");

/// Extract the scatter-hot fields from a full instance. Kept inline so the
/// three mutators write the mirror without indirection.
inline GPUInstanceScatterHot MakeScatterHot(const GPUInstance& instance)
{
    return GPUInstanceScatterHot{instance.meshIndex, instance.materialIndex,
                                 instance.flags,     instance.lodBias,
                                 instance.boundingCenter, instance.boundingRadius};
}

/**
 * @brief GPU-resident mesh metadata
 *
 * Contains information about mesh geometry and clusters.
 */
// Maximum LOD levels stored per mesh (LOD0 + 3 simplified). Must match
// MeshLODConfig::kMaxLODs and the GLSL `kMaxMeshLODs` in draw_command_scatter.comp.
static constexpr uint32_t kMaxMeshLODs = 4;

// std430 layout: 128 bytes, stride matches GLSL `struct GPUMesh` in
// draw_command_scatter.comp exactly. Field order matters -- boundingCenter (vec3)
// MUST sit at a 16-byte-aligned offset (std430 vec3 alignment), so it's
// placed first. Adding fields requires keeping the trailing _pad slots so the
// struct size stays a multiple of 16.
//
// LOD: indexOffset/indexCount mirror lodIndexOffset[0]/lodIndexCount[0] (LOD0)
// so non-LOD consumers that read the legacy fields keep working unchanged.
// draw_command_scatter.comp selects a LOD per instance and emits that level's
// index range.
struct GPUMesh
{
    Vector3 boundingCenter; // 0..12  (local-space sphere center)
    float boundingRadius;   // 12..16 (local-space sphere radius)
    // Convert conservative culling-sphere coverage to the LOD0 reference metric.
    // The centers agree; instance scale cancels. Ordinary meshes use 1.
    float lodCoverageScale = 1.0f; // 16
    uint32_t clusterCount;  // 20     (reserved)
    uint32_t vertexOffset;  // 24     (in vertices, NOT bytes)
    uint32_t vertexCount;   // 28     (total vertex count for this submesh)
    uint32_t indexOffset;   // 32     (LOD0 firstIndex into bound IB pool, in indices)
    uint32_t indexCount;    // 36     (LOD0 index count)
    uint32_t lodCount;      // 40     (1..kMaxMeshLODs)
    uint32_t bucketKey;     // 44     (VertexAttributeFlags as uint32; 0 = unbucketed)
    uint32_t indexType;     // 48     (0 = uint16, 1 = uint32; matches Rendering::IndexType)
    uint32_t vertexFlags;   // 52     (source mesh's VertexAttributeFlags)
    uint32_t lodIndexOffset[kMaxMeshLODs]; // 56..72  (per-LOD firstIndex; [0]==indexOffset)
    uint32_t lodIndexCount[kMaxMeshLODs];  // 72..88  (per-LOD index count; [0]==indexCount)
    float    lodThreshold[kMaxMeshLODs];   // 88..104 (descending screen-coverage switch points)
    // Per-LOD vertex offset RELATIVE to vertexOffset (in vertices; [0]==0).
    // Non-zero only for an authored LOD whose own vertices were concatenated
    // after LOD0's. The scatter emits vertexOffset + lodVertexOffset[lod].
    // Adjacent to lodIndexOffset but a DIFFERENT space: lodIndexOffset is
    // ABSOLUTE within the IB, lodVertexOffset is RELATIVE to vertexOffset.
    uint32_t lodVertexOffset[kMaxMeshLODs]; // 104..120 (was part of _pad)
    // LOD selection flags (SSE-budget selection; see MeshLODThresholds.h).
    // Bits 0..3: lodThreshold[k] is SSE-normalized — the scatter multiplies it
    // by the per-view sseThresholdToCoverage to get that slot's coverage-space
    // switch point. Bit clear = already coverage space (authored / default /
    // sloppy slots, compared exactly as before). Bit 4: tight class
    // (skinned/character chain) — SSE slots take the tighter per-view factor.
    // 0 for every authored row, so those stay byte-identical to the pre-SSE
    // layout.
    uint32_t lodFlags;                      // 120..124
    // Ceiling on the per-view sseThresholdToCoverage factor for the SSE slots
    // flagged above, in that factor's units (MeshLODThresholds.h::
    // LodSseScaleCeil = kLodCoverageErrorScale * referenceMaxExtent / referenceRadius).
    // The scatter clamps the view factor to it, which pins a viewport shorter
    // than this mesh's break-even height to the coverage mapping's switch point
    // instead of coarsening past it. 0 = no ceiling; set only on rows carrying
    // at least one SSE slot, so authored / coverage-space / selection-off rows
    // keep a zero tail word.
    float lodSseScaleCeil;                  // 124..128
};
// GPUMesh.lodFlags bit assignments (mirrored in draw_command_scatter.comp).
inline constexpr uint32_t kGPUMeshLodSseSlotMask   = 0xFu;      // bits 0..3
inline constexpr uint32_t kGPUMeshLodTightClassBit = 1u << 4;
static_assert(sizeof(GPUMesh) == 128,
              "GPUMesh must be 128 bytes to match draw_command_scatter.comp std430 layout");
static_assert(offsetof(GPUMesh, lodCoverageScale) == 16, "GPUMesh.lodCoverageScale offset drift");
static_assert(offsetof(GPUMesh, lodIndexOffset) == 56, "GPUMesh.lodIndexOffset offset drift");
static_assert(offsetof(GPUMesh, lodThreshold) == 88, "GPUMesh.lodThreshold offset drift");
static_assert(offsetof(GPUMesh, lodVertexOffset) == 104, "GPUMesh.lodVertexOffset offset drift");
static_assert(offsetof(GPUMesh, lodFlags) == 120, "GPUMesh.lodFlags offset drift");
static_assert(offsetof(GPUMesh, lodSseScaleCeil) == 124, "GPUMesh.lodSseScaleCeil offset drift");

/**
 * @brief GPU culling data
 *
 * Data used by GPU culling shaders.
 * CRITICAL: This structure MUST match the shader layout exactly!
 *
 * `frustumPlanes` is sized for up to kMaxViewsPerCullingDispatch frusta so a
 * single dispatch can fan a candidate set out into multiple visibility slices
 * (one per shadow cascade). For single-view dispatches only slot [0] is read;
 * the rest are ignored regardless of contents.
 */
static constexpr uint32_t kMaxViewsPerCullingDispatch = 4;

struct GPUCullingData
{
    Matrix4x4 viewMatrix;     // View matrix
    Matrix4x4 projMatrix;     // Projection matrix
    Matrix4x4 viewProjMatrix; // Combined view-projection matrix
    // Per-view frustum planes: [view][L,R,B,T,N,F]. Matches the spec-const
    // `kViewCount` variant selected at PSO creation time on the GPU side.
    Vector4 frustumPlanes[kMaxViewsPerCullingDispatch][6];
    Vector3 cameraPosition;   // Camera position (world space)
    float nearPlane;          // Near plane distance
	    Vector3 cameraForward;    // Camera forward direction (MUST match shader!)
	    float farPlane;           // Far plane distance
	    // Candidate set encoded as [firstInstance, firstInstance + instanceCount)
	    // in the GPUScene instance buffer. This allows the culling shader to
	    // process arbitrary subranges of the global instance array.
	    uint32_t firstInstance;   // First instance index for this pass
	    uint32_t instanceCount;   // Number of candidate instances for this pass
    uint32_t frameIndex;      // Current frame index
    float deltaTime;          // Frame delta time (MUST match shader!)
	    // 1 = this is a shadow dispatch (cascade or area light): cull instances
	    // whose cast-shadows bit (flags & 1) is clear so non-casters stay out of
	    // the shadow map. 0 = main/reflection/thumbnail view: keep everything.
	    // (Reuses the former trailing padding slot — layout/size unchanged.)
	    uint32_t cullShadowCasters;
	    // Per-frustum radius inflation applied in the sphere/frustum test.
	    // Conservative (1.5) for the main view and cascade 0 — false culls there
	    // are the most visible; tight (1.05) for distant cascades 1..3, whose
	    // 50% inflation was passing ~50% extra casters into every shadow slice
	    // (SCALE-11). std430 float arrays have stride 4, matching C++.
	    float cullMargins[kMaxViewsPerCullingDispatch];
    // Two-phase HZB fields; frustum-only dispatches ignore them.
    // hzbMode: 1 = phase A (frustum AND prevVisible), 2 = full HZB phase B.
    // hzbMipCount: available pyramid levels for clamping the mip choice.
    uint32_t hzbMode;
    uint32_t hzbMipCount;
    // Active layers of the requesting view, shared by its color/shadow slices.
    // Uses the final std430 tail word; GPU block stride remains 656 bytes.
    uint32_t renderLayerMask = 0xFFFFFFFFu;
};
// Complete shared std430 mirror, including the former final padding word.
static_assert(sizeof(GPUCullingData) == 656,
              "GPUCullingData must match the culling shaders' std430 layout");
static_assert(offsetof(GPUCullingData, frustumPlanes) == 192, "GPUCullingData.frustumPlanes offset drift");
static_assert(offsetof(GPUCullingData, cameraPosition) == 576, "GPUCullingData.cameraPosition offset drift");
static_assert(offsetof(GPUCullingData, cameraForward) == 592, "GPUCullingData.cameraForward offset drift");
static_assert(offsetof(GPUCullingData, firstInstance) == 608, "GPUCullingData.firstInstance offset drift");
static_assert(offsetof(GPUCullingData, cullShadowCasters) == 624, "GPUCullingData.cullShadowCasters offset drift");
static_assert(offsetof(GPUCullingData, cullMargins) == 628, "GPUCullingData.cullMargins offset drift");
static_assert(offsetof(GPUCullingData, hzbMode) == 644, "GPUCullingData.hzbMode offset drift");
static_assert(offsetof(GPUCullingData, hzbMipCount) == 648, "GPUCullingData.hzbMipCount offset drift");
static_assert(offsetof(GPUCullingData, renderLayerMask) == 652, "GPUCullingData.renderLayerMask offset drift");

/// Sphere-radius inflation for the frustum test. Conservative absorbs
/// precision issues + skinned-bounds slack on the slices where a false cull
/// pops visibly (main view, cascade 0); tight is enough margin for the
/// distant cascades whose casters are small on screen.
inline constexpr float kCullMarginConservative = 1.5f;
inline constexpr float kCullMarginTightCascade = 1.05f;

/**
 * @brief Per-frame GPUScene resources
 *
 * This struct groups the subset of GPUScene buffers that may need to
 * vary per frame once true multi-frame-in-flight support is enabled.
 *
 * Initially, all frames alias a single shared physical buffer for each
 * resource type (see docs/RENDERING_GPU_BUFFERS_AND_FRAMES_IN_FLIGHT.md).
 * This keeps behavior backward compatible while providing a clear
 * extension point for future per-frame buffers.
 */
struct GPUSceneFrameResources
{
    BufferHandle instanceBuffer;
    BufferHandle scatterHotBuffer; // 32 B/instance coalesced mirror (scatter fetch)
    BufferHandle meshBuffer;
    BufferHandle visibilityBuffer;
    BufferHandle indirectArgsBuffer;
};

/**
 * @brief Visibility buffer entry
 *
 * Compact representation of visible primitives.
 */
struct VisibilityEntry
{
    uint32_t primitiveID : 24; // Primitive ID within cluster
    uint32_t clusterID : 8;    // Cluster ID (limited to 256 clusters per draw)
};

/**
 * @brief GPU Scene Management System
 *
 * Manages all GPU-resident scene data for modern GPU-driven rendering.
 * Supports large-scale scenes with 1M+ objects and GPU-driven culling.
 *
 * Lifetime and frames-in-flight assumptions:
 * - GPUScene currently owns a single set of GPU buffers that are updated from
 *   CPU memory via UpdateGPUBuffers() at the start of each frame.
 * - It assumes a single logical writer (the render thread) and that the
 *   underlying device enforces per-frame fences so the CPU does not write
 *   into these buffers while the GPU is still consuming data from a
 *   previous frame.
 * - If you introduce more frames in flight that may concurrently read from
 *   GPUScene buffers, refactor this class to use explicit per-frame
 *   resources (e.g., Rendering::RingBuffer<T> or one BufferHandle per
 *   device frame index) before widening usage.
 */
class GPUScene
{
  public:
    static constexpr uint32_t kMaxFramesInFlight = IDevice::kMaxSupportedFramesInFlight;

    // Type of a host-provided shader loader used to obtain compute shader
    // bytes for GPU culling. This indirection keeps renderer-core free of
    // file I/O and asset system dependencies.
    using CullingShaderLoaderFunc = std::vector<uint8_t> (*)(const char* name);

    // Configure the global shader loader used by GPUScene's
    // CreateCullingPipeline(). Engine/runtime code should provide an
    // implementation that pulls SPIR-V from the AssetManager. Samples and
    // tests may install a loader that reads from disk or uses hard-coded
    // byte arrays. If no loader is installed, GPUScene falls back to the
    // module-local Utils::LoadShaderFile path (the loader hook is a
    // file-static that exists once per module copy — see LoadComputeStageBytes).
    static void SetCullingShaderLoader(CullingShaderLoaderFunc loader);

    GPUScene(IDevice* device);
    ~GPUScene();

    // Scene management
    bool Initialize(uint32_t maxInstances = 1000000, uint32_t maxMeshes = 100000);
    void Shutdown();

    // Instance management
    uint32_t AddInstance(const GPUInstance& instance);
    void UpdateInstance(uint32_t instanceIndex, const GPUInstance& instance);
    void RemoveInstance(uint32_t instanceIndex);

    // One row write of UpdateInstances.
    struct InstanceWrite
    {
        uint32_t InstanceIndex = 0;
        GPUInstance Instance{};
    };
    // Runs body(begin, end) over [0, count), possibly concurrently on disjoint
    // ranges, and returns once every range has run. The caller owns the threads:
    // this module has no job system.
    using RangeRunner = std::function<void(size_t count,
                                           const std::function<void(size_t begin, size_t end)>& body)>;
    // Batched UpdateInstance for bulk motion. The scene afterwards equals calling
    // UpdateInstance for each write in span order: the same rows, scatter-hot
    // mirror rows, batch-registry membership, and the same set of dirty words
    // (their list order differs; the flush sorts it). Rows are compared and
    // copied, and their dirty bits set, through `runRanges`, concurrently, so
    // every InstanceIndex must be unique within the span; an index past the
    // scene is reported and skipped. Only the writes that changed a batch key
    // then update the registry, on the calling thread, in span order. No other
    // GPUScene mutator may run until it returns: debug builds assert that.
    void UpdateInstances(std::span<const InstanceWrite> writes, const RangeRunner& runRanges);

    // Batched removal for HLOD cluster flips. The single-index path guards
    // double-removes with an O(free-list) std::find and trims trailing free
    // slots one std::find+erase at a time — O(M·free-list) ≈ O(M²) to evict a
    // whole cluster. This tombstones + frees a whole span in one pass: sort +
    // dedup the indices, a dense free-membership check (m_InstanceSlotFree)
    // replaces every std::find, and the trailing trim runs exactly once at the
    // end. Preserves every invariant the single path maintains (mirror
    // tombstone, batch-count decrement, dirty-instance marks, free-slot reuse,
    // instance/mirror index-lock). Out-of-range or already-free indices are
    // skipped; a slot appearing twice in the span is folded to one remove.
    // RemoveInstance delegates here with a 1-span so behavior can't drift.
    void RemoveInstances(std::span<const uint32_t> instanceIndices);

    // Per-slot continuity stamp: a per-frame consumer that carries state
    // derived from an instance across frames (a previous-frame endpoint) may
    // trust that state only while the stamp is unchanged. AddInstance moves it
    // for the slot's new tenant — a fresh slot and a recycled one alike, so a
    // recycled slot never inherits the prior tenant's stamp — and extraction
    // moves it when an instance's payload changes for any reason other than
    // its transform (the transform's history is prevTransform's job). The
    // value is per slot, monotonic, never reset while the scene lives.
    void BumpInstanceContinuityStamp(uint32_t instanceIndex);
    uint32_t GetInstanceContinuityStamp(uint32_t instanceIndex) const
    {
        return instanceIndex < m_InstanceContinuityStamps.size()
                   ? m_InstanceContinuityStamps[instanceIndex]
                   : 0u;
    }

    // Move out the slots whose continuity stamp moved since the last drain. This
    // is how a device-side consumer of the stamp learns about it: the stamp is
    // processor state, GPUInstance is full at 240 B, and the scatter's compact
    // mirror carries no spare word — so the transport is an explicit reset list
    // rather than a row field. Every stamp move is queued, so the set covers new
    // tenants (a fresh slot and a recycled one) and payload changes alike, and it
    // is a superset of DrainPrevVisibleResetSlots' recycled set. Consumers refill
    // those slots' previous-frame entry with their own no-history value before
    // the frame reads it. Empty in steady state.
    std::vector<uint32_t> DrainContinuityResetSlots();

    // Move out the instance slots that were RECYCLED (reused from the free-list,
    // or re-appended into a previously-trimmed index) since the last drain, then
    // clear the internal queue. GPUCullingPipeline drains this once per frame to
    // reset the two-phase HZB prevVisible[slot] history to 0xFFFFFFFF ("unknown =
    // visible") so a recycled slot never inherits the prior tenant's occlusion
    // state — the 1-frame pop-in the design flags as C1. Reset-on-REUSE, not
    // reset-on-free: a freed-but-not-trimmed tombstone slot stays in the cull
    // candidate range, so phase B keeps rewriting its prevVisible every frame
    // until the slot is actually reused, which would clobber a reset-on-free.
    // Empty in steady state (no removes → no recycled adds), so a churn-free
    // frame schedules no reset work.
    void DrainPrevVisibleResetSlots(std::vector<uint32_t>& outSlots);

    // Mesh management
    uint32_t AddMesh(const GPUMesh& mesh);
    void UpdateMesh(uint32_t meshIndex, const GPUMesh& mesh);

    /**
     * Free a previously-allocated mesh slot. The row is zero-filled so any
     * stale GPU read of the slot before reuse sees a well-defined empty
     * record (indexCount=0, vertexCount=0). The slot is then returned to
     * the free-list and reused by the next AddMesh call.
     */
    void RemoveMesh(uint32_t meshIndex);

    // Debug / test accessor.
    const std::vector<GPUMesh>& GetMeshes() const { return m_Meshes; }


    // Frame management
    /**
     * Begin a new logical GPUScene frame.
     *
     * Call once per render frame after the device has begun its frame
     * (e.g., IDevice::BeginFrame) and before issuing GPUScene-dependent
     * culling or rendering work. Convenience for AdvanceFrameSlot() +
     * FlushGPUBuffers(); preferred for tests and any caller that owns
     * the full per-frame lifecycle. Production rendering paths drive
     * the two pieces separately via RenderGraph::BeginFrame so multi-
     * RG flows (editor windows, thumbnail RGs sharing buffers) hit
     * each step exactly once per RG per frame.
     */
    void BeginFrame();

    /**
     * Rotate to the next frame-in-flight slot. Every accessor
     * (GetInstanceBuffer / GetMeshBuffer / etc.)
     * reads m_FrameSlot, so this is what makes subsequent reads see
     * the new frame's physical buffer. Idempotent within a single
     * device frame.
     */
    void AdvanceFrameSlot();

    /**
     * End the current logical GPUScene frame.
     *
     * Resets internal dirty flags after GPU work for this frame has
     * been scheduled. Does not perform synchronization; callers are
     * responsible for respecting the device's per-frame fences.
     */
    void EndFrame();

    /**
     * True if any of m_Instances / m_Meshes has pending
     * CPU-side mutations that haven't been flushed to the current
     * frame's GPU buffer. Cheap branch + bool read; safe to call every
     * frame as a fast-skip guard before FlushGPUBuffers().
     */
    bool IsDirty() const
    {
        return m_InstancesDirtySlots[m_FrameSlot]
            || m_MeshesDirtySlots[m_FrameSlot];
    }

    /**
     * Monotonic GPU-content generation: advances whenever UpdateGPUBuffers
     * uploads to the current frame slot and whenever the GPU buffers are
     * (re)created. Dirty bits are per-slot, so one CPU mutation advances the
     * epoch on EVERY slot's next flush — the epoch goes quiet only once the
     * whole frames-in-flight ring has converged to identical content. That is
     * the property the idle-recompute elision gates key on: an epoch stable
     * across settleFrames consecutive frames proves the slot ring is
     * content-uniform, so retained scatter records indexing it stay exact
     * regardless of which slot a frame binds (per-slot HANDLES cycle every
     * frame and must never enter an elision blob directly).
     */
    uint64_t GetContentEpoch() const { return m_ContentEpoch; }

    // Validity mask over the lazily-created culling PSOs (bit 0 = the
    // single-view frustum pipeline, bit N = the N-view cascade variant).
    // ScheduleCullingPassForRange / ...ForCascadeGroup silently declare
    // NOTHING while their PSO is unavailable (async shader load, hot-reload
    // recompile), and the elision gate still counts such frames as executed —
    // so an invalid→valid flip must be an input change in the culling blob,
    // or the gate can settle over no-op frames and keep skipping after the
    // PSO arrives (RecomputeElision.h: anything that could change the GPU
    // result must be IN the blob).
    uint32_t CullingPipelineValidityMask() const
    {
        uint32_t mask = m_CullingPipeline.IsValid() ? 1u : 0u;
        for (uint32_t i = 1; i <= kMaxViewsPerCullingDispatch; ++i)
        {
            if (m_CascadeCullingPipelines[i].IsValid())
                mask |= 1u << i;
        }
        return mask;
    }

	    	    // Culling and rendering
	    	    /**
	    	     * Schedule the GPU culling compute pass to write into a subrange of the
	    	     * given visibility buffer using the provided culling parameters.
	    	     *
	    	     * visibilityOffsetElements and visibilityElementCount are expressed in
	    	     * uint32_t elements, not bytes. This is primarily intended for
	    	     * multi-view orchestration via GPUCullingPipeline, where a single large
	    	     * visibility buffer is partitioned into per-view slices and each slice
	    	     * may be driven by its own GPUCullingData.
	    	     */
	    	    // sliceStableKey is the (viewId, cascadeIndex)-derived identifier the
	    	    // scheduler uses to look up persistent per-slice state. Pass a stable
	    	    // key (e.g. `(uint64_t(viewId) << 16) | cascadeIndex`); keying on the
	    	    // *visibility offset* breaks the moment scene instance count changes
	    	    // because all cascade offsets shift, orphaning the old state and
	    	    // burning a fresh slot inside the shared culling-data buffer.
    // ── Immediate-mode culling: passes declare fresh each frame — no
    // per-slice persistent state, no culling-data slot grid (params via the
    // frame's upload ring, written at declaration), no rebind dances, no
    // tags. Ordering is the real RAW edge on the threaded visibility
    // RGBuffer value. Cascade groups fan ONE dispatch into viewCount
    // contiguous output slices via the kViewCount PSO variant;
    // sliceStableKey/groupStableKey name the dispatch for diagnostics. ──
    struct GPUSceneFrameRG
    {
        RenderGraph::RGBuffer Instances{};
        // Coalesced 32 B/instance scatter-hot mirror. Invalid when the compact
        // scatter fetch is disabled (GE_SCATTER_COMPACT=0) — the scatter then
        // binds Instances directly, exactly as before.
        RenderGraph::RGBuffer ScatterHot{};
        RenderGraph::RGBuffer Meshes{};
    };
    // One call per RGFrame per frame (dedup-by-physical makes repeats harmless).
    GPUSceneFrameRG ImportFrameResources(RenderGraph::RGFrame& frame) const;

    void ScheduleCullingPassForRange(RenderGraph::RGFrame& frame, const GPUSceneFrameRG& sceneRG,
                                     RenderGraph::RGBuffer visibility, uint32_t visibilityTotalBytes,
                                     uint32_t visibilityOffsetElements,
                                     uint32_t firstInstance, uint32_t instanceCount,
                                     const GPUCullingData& cullingData, uint64_t sliceStableKey);
    void ScheduleCullingPassForCascadeGroup(RenderGraph::RGFrame& frame, const GPUSceneFrameRG& sceneRG,
                                            RenderGraph::RGBuffer visibility, uint32_t visibilityTotalBytes,
                                            const uint32_t* sliceOffsetsElements, uint32_t viewCount,
                                            uint32_t firstInstance, uint32_t instanceCount,
                                            const GPUCullingData& cullingData, uint64_t groupStableKey);

    // Resource access (current-frame views)
    BufferHandle GetInstanceBuffer() const
    {
        const BufferHandle& handle = m_Frames[m_FrameSlot].instanceBuffer;
        return handle.IsValid() ? handle : m_InstanceBuffer;
    }
    // Coalesced scatter-hot mirror for the current frame slot. Invalid when the
    // compact scatter fetch is disabled — callers fall back to the full
    // instance buffer (GetInstanceBuffer) for the scatter's binding 1.
    BufferHandle GetScatterHotBuffer() const
    {
        const BufferHandle& handle = m_Frames[m_FrameSlot].scatterHotBuffer;
        return handle.IsValid() ? handle : m_ScatterHotBuffer;
    }
    // True when the compact scatter-hot mirror is maintained + uploaded this
    // session (GE_SCATTER_COMPACT != 0, the default). Fixed for the process.
    bool IsScatterHotEnabled() const { return m_ScatterHotEnabled; }
    BufferHandle GetMeshBuffer() const
    {
        const BufferHandle& handle = m_Frames[m_FrameSlot].meshBuffer;
        return handle.IsValid() ? handle : m_MeshBuffer;
    }
    BufferHandle GetVisibilityBuffer() const
    {
        const BufferHandle& handle = m_Frames[m_FrameSlot].visibilityBuffer;
        return handle.IsValid() ? handle : m_VisibilityBuffer;
    }
    BufferHandle GetIndirectArgsBuffer() const
    {
        const BufferHandle& handle = m_Frames[m_FrameSlot].indirectArgsBuffer;
        return handle.IsValid() ? handle : m_IndirectArgsBuffer;
    }

    // Optional explicit per-frame queries (primarily for tests /
    // advanced callers). frameIndex is interpreted as a logical frame
    // index; it is modulo-reduced into the internal frame slot.
    BufferHandle GetInstanceBufferForFrame(uint32_t frameIndex) const
    {
        uint32_t slot = frameIndex % m_FramesInFlight;
        const BufferHandle& handle = m_Frames[slot].instanceBuffer;
        return handle.IsValid() ? handle : m_InstanceBuffer;
    }
    BufferHandle GetScatterHotBufferForFrame(uint32_t frameIndex) const
    {
        uint32_t slot = frameIndex % m_FramesInFlight;
        const BufferHandle& handle = m_Frames[slot].scatterHotBuffer;
        return handle.IsValid() ? handle : m_ScatterHotBuffer;
    }
    BufferHandle GetVisibilityBufferForFrame(uint32_t frameIndex) const
    {
        uint32_t slot = frameIndex % m_FramesInFlight;
        const BufferHandle& handle = m_Frames[slot].visibilityBuffer;
        return handle.IsValid() ? handle : m_VisibilityBuffer;
    }
    BufferHandle GetIndirectArgsBufferForFrame(uint32_t frameIndex) const
    {
        uint32_t slot = frameIndex % m_FramesInFlight;
        const BufferHandle& handle = m_Frames[slot].indirectArgsBuffer;
        return handle.IsValid() ? handle : m_IndirectArgsBuffer;
    }

	    	    // Number of allocated instance slots to scan in GPU passes.
	    	    // Freed slots are tombstoned and kept in the vector so stable
	    	    // MeshGPUData::instanceIndex values remain valid.
	    	    uint32_t GetInstanceCount() const { return static_cast<uint32_t>(m_Instances.size()); }
	    	    uint32_t GetMaxInstances() const { return m_MaxInstances; }
	    	    uint32_t GetLiveInstanceCount() const { return m_InstanceCount; }
	    	    uint32_t GetMeshCount() const { return m_MeshCount; }
	    	    uint32_t GetFrameIndex() const { return m_FrameIndex; }

    // Cumulative bytes uploaded to the instance buffer FAMILY since creation:
    // the authoritative 240 B GPUInstance buffer plus, when the compact scatter
    // fetch is enabled, its 32 B/instance scatter-hot mirror (uploaded on the
    // same dirty-instance runs, so the mirror adds exactly 32/240 of the instance
    // bytes). Monotonic; diff across frames for per-frame upload bandwidth
    // (R1.1 metric). Enabling the mirror re-baselines this metric upward by
    // 32/240 (see baselines/scatter-fetch-lever1 note).
    uint64_t GetInstanceUploadBytesTotal() const
    {
        return m_InstanceUploadBytesTotal.load(std::memory_order_relaxed);
    }

    // Live-instance count per (materialIndex, meshIndex) batch, maintained by
    // the three instance mutators. Snapshot at schedule time only — mutators
    // run on the extraction path, so mid-frame reads from elsewhere race.
    const BatchRegistry& GetBatchRegistry() const { return m_BatchRegistry; }

    // Debug and profiling
    void SetDebugName(const char* name) { m_DebugName = name ? name : "GPUScene"; }
    const char* GetDebugName() const { return m_DebugName.c_str(); }

    // Debug access to instance data
    const std::vector<GPUInstance>& GetInstances() const { return m_Instances; }

    // Debug / test accessors: the free-slot reuse pool and the coalesced
    // scatter-hot mirror. Used by the batched-removal state-equivalence tests
    // to assert the free-list set and the 32 B mirror stay in lockstep with the
    // single-index path.
    const std::vector<uint32_t>& GetFreeInstanceSlots() const { return m_FreeInstanceSlots; }
    const std::vector<GPUInstanceScatterHot>& GetScatterHot() const { return m_ScatterHot; }

    // World-space AABB of all live instances. Returns false when the scene
    // has no live instances. Lazily computed and cached; recomputed only
    // when the instance set changes. Used by render features (e.g. CSM)
    // to skip per-view culling dispatches for cascades that don't intersect
    // any geometry.
    bool GetInstancesWorldBounds(Vector3& outMin, Vector3& outMax) const;

    // Re-upload any pending CPU-side instance / mesh additions to
    // the current frame's GPU buffers. BeginFrame() already calls this once at
    // the start of each frame; callers that mutate scene state AFTER the main
    // upload (e.g. editor thumbnail world extracts mid-frame) must call this
    // again before any compute / draw consumer reads the GPU buffers, or the
    // consumer will see stale data and silently produce zero output.
    void FlushGPUBuffers() { UpdateGPUBuffers(); }

    // Q6 device-lost re-provision (design §8). The in-place device rebuild freed
    // every GPU buffer this scene owned; the handle members are now dead. Recreate
    // fresh GPU buffers (the CPU mirror — m_Instances / m_Meshes — is
    // retained), mark all frame slots fully dirty, and re-upload. Does NOT destroy
    // the old handles (the rebuild teardown already freed the GPU buffers, so a
    // DestroyBuffer here would double-free a dead slot). meshIndex in the mirror
    // still references the mesh table whose GPU buffer slice 4 re-uploads — those
    // indices are NOT fixed up here (design F9 is slice 4).
    void ReprovisionAfterDeviceRebuild();

  private:
    // Device and resources
    IDevice* m_Device;
    std::string m_DebugName;

    // GPU buffers
    BufferHandle m_InstanceBuffer;     // All instance data
    BufferHandle m_ScatterHotBuffer;   // Coalesced scatter-hot mirror (fallback)
    BufferHandle m_MeshBuffer;         // All mesh metadata
    BufferHandle m_VisibilityBuffer;   // Legacy single-buffer (demo paths only)
    BufferHandle m_IndirectArgsBuffer; // Legacy single-buffer (demo paths only)

    // CPU-side data (for updates)
    std::vector<GPUInstance> m_Instances;
    // Coalesced scatter-hot mirror, maintained in exact lockstep with
    // m_Instances by the three instance mutators (index i mirrors instance i).
    // Only populated + uploaded when m_ScatterHotEnabled.
    std::vector<GPUInstanceScatterHot> m_ScatterHot;
    std::vector<GPUMesh> m_Meshes;
    BatchRegistry m_BatchRegistry;
    std::vector<uint32_t> m_FreeInstanceSlots;
    // Dense free-membership mirror: m_InstanceSlotFree[i] != 0 iff slot i is on
    // the free-list. Size-locked to m_Instances (grows on append, pops on
    // trailing-trim). Gives the double-remove guard and trailing-trim O(1)
    // membership so RemoveInstances stays linear per span instead of the
    // single-index path's O(free-list) std::find in both spots.
    std::vector<uint8_t> m_InstanceSlotFree;
    // Per-slot continuity stamp (see BumpInstanceContinuityStamp). Sized to
    // the instance high-water mark and never trimmed, so a reused slot and a
    // re-appended trimmed index both keep counting from the prior tenant's
    // value and two tenants of one slot can never share a stamp.
    std::vector<uint32_t> m_InstanceContinuityStamps;
    // Slots whose continuity stamp moved since the last DrainContinuityResetSlots
    // call. Written at the one place the stamp advances, so the queue and the
    // stamps can never disagree about which slots changed.
    std::vector<uint32_t> m_ContinuityResetSlots;
    // Advance one slot's continuity stamp and queue it for the drain above.
    void AdvanceContinuityStamp(uint32_t instanceIndex);
    std::vector<uint32_t> m_FreeMeshSlots;

    // Slots recycled since the last DrainPrevVisibleResetSlots() call (see that
    // method). Populated in AddInstance when it hands a new tenant an index the
    // scene has used before; drained + cleared once per frame by the culling
    // pipeline. m_InstanceSlotHighWater is the max instance-array size ever
    // reached: index < high-water ⇔ the slot was live at some point (so its
    // prevVisible may be dirty and needs reset on reuse); index == high-water is
    // a genuinely fresh slot still holding the buffer's 0xFFFFFFFF first-touch.
    std::vector<uint32_t> m_PrevVisibleResetSlots;
    uint32_t m_InstanceSlotHighWater = 0;

    // Cached world-space AABB over all live instances. Lazily recomputed in
    // GetInstancesWorldBounds() when m_InstancesAABBDirty is set; mutated by
    // AddInstance / UpdateInstance / RemoveInstance / Shutdown. The const
    // accessor justifies mutable because the cache is a strict function of
    // already-mutated CPU state.
    mutable Vector3 m_InstancesAABBMin = {};
    mutable Vector3 m_InstancesAABBMax = {};
    mutable bool m_InstancesAABBDirty = true;

	    	    // Scene state
	    	    uint32_t m_MaxInstances;
	    	    uint32_t m_MaxMeshes;
	    	    uint32_t m_InstanceCount;
	    	    uint32_t m_MeshCount;

    // Written on the render thread; atomic only so the debug server can read
    // it race-free from its own thread.
    std::atomic<uint64_t> m_InstanceUploadBytesTotal{0};

    // Frame state -- per-slot dirty flags so each frame-in-flight slot gets its own upload
    bool m_InstancesDirtySlots[kMaxFramesInFlight]{};
    bool m_MeshesDirtySlots[kMaxFramesInFlight]{};

    // Latched capacity diagnostic: set on the first flush that finds more CPU
    // rows than the declared capacity (which is what sizes each GPU table),
    // so the clamped upload is reported once instead of every frame.
    bool m_InstanceCapacityErrorLogged = false;
    bool m_MeshCapacityErrorLogged = false;

    // One bit per instance, plus a deduplicated list of nonzero words. Only
    // changed words are visited/cleared; sparse updates neither scan the scene
    // nor copy unchanged neighbours. Each in-flight slot converges separately.
    std::vector<uint64_t> m_InstanceDirtyBits[kMaxFramesInFlight];
    std::vector<uint32_t> m_InstanceDirtyWords[kMaxFramesInFlight];
    std::vector<BufferUpdateRange> m_InstanceUploadRanges;
    std::vector<BufferUpdateRange> m_ScatterHotUploadRanges;
    // Full-upload override per slot (set for all slots in Initialize so the
    // first upload of each slot is a full one, and by anything that
    // invalidates the whole array wholesale).
    bool m_InstanceDirtyAllSlots[kMaxFramesInFlight]{};
    void MarkInstanceDirtyAllSlots(uint32_t instanceIndex);
    // UpdateInstances' outcome per write from its concurrent row phase: the
    // row's batch key before and after the copy. The serial pass reads it only
    // for the writes the row phase listed as exceptions.
    struct RowWriteOutcome
    {
        uint32_t InstanceIndex = 0;
        uint32_t OldMaterialIndex = 0;
        uint32_t OldMeshIndex = 0;
        uint32_t NewMaterialIndex = 0;
        uint32_t NewMeshIndex = 0;
        bool OldMirrored = false;
        bool NewMirrored = false;
    };
    std::vector<RowWriteOutcome> m_RowWriteOutcomes;
    void CopyInstanceRows(std::span<const InstanceWrite> writes, size_t begin, size_t end);
    // Debug builds: asserts no instance index appears twice in one batch.
    void AssertEachRowWrittenOnce(std::span<const InstanceWrite> writes);
    // Set while UpdateInstances copies rows on other threads; every other row
    // mutator asserts it is clear, because a concurrent writer would race the copy.
    std::atomic<bool> m_RowBatchInFlight{false};
    // UpdateInstances' merge point for its concurrent ranges: each range
    // appends the dirty words it listed (staged kRowBatchWordStage at a time),
    // the writes it could not finish (an index past the scene, or a changed
    // batch key for the registry) and whether any row changed.
    static constexpr uint32_t kRowBatchWordStage = 256u;
    void AppendRowBatchWords(uint32_t slot, const uint32_t* words, uint32_t count);
    std::mutex m_RowBatchMutex;
    std::vector<size_t> m_RowBatchExceptions;
    bool m_RowBatchAnyChanged = false;
    // Debug builds' once-per-row check in UpdateInstances: the batch number
    // that last wrote each instance row (O(writes), no allocation once grown).
    // Present in every configuration so the class layout never depends on NDEBUG.
    std::vector<uint32_t> m_RowBatchStamp;
    uint32_t m_RowBatchNumber = 0;
    bool m_InstancesDirty;
    bool m_MeshesDirty;
    uint32_t m_FrameIndex;
    uint32_t m_FrameSlot;
    uint32_t m_FramesInFlight;
    // GPU-content generation (see GetContentEpoch): bumped per slot flush and
    // per buffer (re)creation, never reset.
    uint64_t m_ContentEpoch = 0;
    // Read once from GE_SCATTER_COMPACT (default on). Gates the whole scatter-
    // hot mirror machinery (m_ScatterHot, the per-frame buffers, its upload,
    // and its RG import) so GE_SCATTER_COMPACT=0 reproduces the pre-lever
    // behaviour byte-for-byte for A/B benching.
    //
    // Request vs active: this flag is GPUScene's INDEPENDENT read of the env
    // var. GPUDrawStreamBuilder reads the same var separately to pick the
    // scatter pipeline variant. Under a stable environment the two agree. They
    // diverge only on a compact-shaderpkg staging gap: the builder falls back to
    // the full-fat pipeline (binds the 240 B instance buffer) while this flag
    // stays true, so the mirror is maintained + uploaded but unused. That is
    // intentional — GetInstanceUploadBytesTotal counts the mirror's bytes
    // regardless of which pipeline consumes it; the fetch is simply not
    // coalesced that session (correct, just not the fast path).
    bool m_ScatterHotEnabled;
    GPUSceneFrameResources m_Frames[kMaxFramesInFlight];

    // Pipeline handles
    PipelineHandle m_CullingPipeline;
    bool m_CullingPipelineCreationAttempted = false;
    // Per-N cascade culling PSO variants (indices 2..kMaxViewsPerCullingDispatch).
    // Index 1 mirrors `m_CullingPipeline` for symmetry but stays
    // INVALID_PIPELINE_HANDLE so single-view callers continue using the
    // existing N=1 PSO without lazy-compiling a second copy.
    PipelineHandle m_CascadeCullingPipelines[kMaxViewsPerCullingDispatch + 1] = {};
    bool m_CascadeCullingPipelineAttempted[kMaxViewsPerCullingDispatch + 1] = {};
    // Shared SPIR-V bytes for frustum_culling.comp. Loaded once on first
    // PSO compile; reused by all per-N variants (the bytes are identical;
    // only the spec-const value differs).
    std::shared_ptr<const std::vector<uint8_t>> m_CullingShaderBytes;
    bool m_CullingShaderLoadFailureLogged = false;

    // Helper methods
    void CreateBuffers();
    void UpdateGPUBuffers();
    bool EnsureCullingShaderBytes();
    void CreateCullingPipeline();
    PipelineHandle GetOrCreateCascadeCullingPipeline(uint32_t viewCount);
    // Shared implementation for ScheduleCullingPassForRange (viewCount=1) and
    // ScheduleCullingPassForCascadeGroup (viewCount=N). `sliceOffsetsElements`
    // must point to at least `viewCount` entries; the rest are zeroed.
    void ScheduleCullingPassImpl(RenderGraph::RGFrame& frame, const GPUSceneFrameRG& sceneRG,
                                 RenderGraph::RGBuffer visibility, uint32_t visibilityTotalBytes,
                                 PipelineHandle pipeline,
                                 const uint32_t* sliceOffsetsElements, uint32_t viewCount,
                                 uint32_t firstInstance, uint32_t instanceCount,
                                 const GPUCullingData& cullingData, uint64_t sliceStableKey);
    uint32_t AllocateInstanceSlot();
    uint32_t AllocateMeshSlot();
    void FreeInstanceSlot(uint32_t slot);
    void FreeMeshSlot(uint32_t slot);
};

/**
 * @brief GPU Scene Factory
 *
 * Factory for creating GPU scene instances with different configurations.
 */
class GPUSceneFactory
{
  public:
    // Create scene for different use cases
    static std::unique_ptr<GPUScene> CreateLargeScene(IDevice* device);  // 1M+ instances
    static std::unique_ptr<GPUScene> CreateMediumScene(IDevice* device); // 100K instances
    static std::unique_ptr<GPUScene> CreateSmallScene(IDevice* device);  // 10K instances

    // Create scene with custom parameters
    static std::unique_ptr<GPUScene> CreateCustomScene(IDevice* device,
                                                       uint32_t maxInstances,
                                                       uint32_t maxMeshes);
};

} // namespace Rendering
} // namespace GameEngine
