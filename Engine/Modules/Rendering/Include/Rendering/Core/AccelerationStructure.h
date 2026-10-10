#pragma once

#include <cstdint>

#include "Rendering/Core/CommandList.h"  // IndexType
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"

namespace GameEngine
{
namespace Rendering
{

// Opaque backend-owned acceleration structure. 0 = invalid. Slots are
// monotonic within a build generation — the RT shadow-mask prototype destroys
// BLASes only in drop-all sweeps (mesh hot-reload, the inactivity purge) and
// at shutdown, so generational recycling would be dead machinery today.
// ReleaseAllStructures starts a new generation: every outstanding handle is
// invalidated and ids restart, so it is legal only while no consumer holds a
// handle or a TLAS slot (SceneAccelerationStructureService::BeginFrame).
// Encoding: raw id = slot index + 1, 0 = invalid (StrongHandle's own invalid).
struct AccelerationStructureTag{};
using AccelerationStructureHandle = StrongHandle<AccelerationStructureTag>;

// TlasSlotHandle lives in Rendering/Core/Handle.h — DescriptorSetUpdate names
// it, and that header sits below this one in the include order.

// One triangle-geometry range reading pool-resident vertex/index data in
// place via device addresses. Buffers must carry
// BufferUsage::AccelerationStructureBuildInput | ShaderDeviceAddress.
// Offsets are pool-space, matching MeshGPUEntry semantics: FirstVertex is
// added to every fetched index (the vkCmdDrawIndexed vertexOffset analogue),
// FirstIndex is in index units from the pool base. Position must lead each
// interleaved vertex (VertexAttributeFlags binding-0 contract).
struct BlasTriangleGeometry
{
    uint64_t VertexAddress     = 0;
    uint32_t VertexStrideBytes = 0;
    uint32_t FirstVertex       = 0;
    uint32_t MaxVertex         = 0;  // highest pool-space vertex index reachable
    uint64_t IndexAddress      = 0;
    uint32_t FirstIndex        = 0;
    uint32_t IndexCount        = 0;
    IndexType IndexKind        = IndexType::Uint16;
};

// CPU-written TLAS instance record, 64 bytes, laid out so a backend can hand it
// to the driver as the instance array without translating it; plain words
// instead of bitfields so the layout is compiler-independent. The backend that
// consumes it directly asserts its size and the offsets of the transform and
// address words against its own header; the bit packing inside the two middle
// words is convention (see the per-field comments) — the driver declares them as
// bitfields, whose offsets no assert can take.
// Transform is ROW-major 3x4 — transpose the engine's column-major mat4.
struct TlasInstanceData
{
    float    Transform[12]       = {};  // rows 0..2 of the world matrix
    uint32_t CustomIndexAndMask  = 0;   // customIndex bits 0..23, visibility mask bits 24..31
    uint32_t SbtOffsetAndFlags   = 0;   // sbtOffset bits 0..23, geometry instance flags bits 24..31
    uint64_t BlasAddress         = 0;   // from GetBlasDeviceAddress
};
static_assert(sizeof(TlasInstanceData) == 64,
              "TlasInstanceData must stay 64 bytes to match the native instance descriptor");

// Geometry instance flag values used by the shadow lane, mirrored so engine-side
// code does not include a backend header; the backend that consumes them asserts
// each value against its own enum. Facing cull is disabled for shadow occlusion
// rays — a caster occludes regardless of winding.
constexpr uint32_t kTlasInstanceFlagTriangleFacingCullDisable = 0x1;

// Minimal acceleration-structure backend shared by every ray-query consumer
// (the DirectionalShadowMode::RayTraced shadow-mask lane, GI probe tracing).
// One shared BLAS pool; each consumer acquires its own TlasSlotHandle for its
// own filtered instance set. Deliberately narrow beyond that: triangle-only
// BLAS, no compaction yet (queued follow-up), no host builds. Obtain via
// IDevice::GetAccelerationStructureBackend() — null when
// RenderingDeviceCapabilities::supportsRayQuery is false.
//
// Threading: host-side calls (CreateBlas/PrepareTlas/Destroy*/CollectGarbage)
// are render-thread-only, made during RG pass DECLARATION. Record* calls run
// inside RG pass exec lambdas and only record commands — they mutate no
// backend state, so they are safe under parallel pass recording.
class IAccelerationStructureBackend
{
  public:
    virtual ~IAccelerationStructureBackend() = default;

    // Host side: compute sizes, allocate storage and create the BLAS object.
    // Does NOT reserve scratch — call ReserveBlasScratch each frame the build
    // will be recorded (builds may be re-recorded across frames until the
    // caller confirms an exec actually ran; see RTShadowMaskService).
    // Returns invalid on allocation failure (caller warns and skips the mesh).
    virtual AccelerationStructureHandle CreateBlas(const BlasTriangleGeometry& geometry) = 0;

    // Host side, once per frame per to-be-recorded build: assign this BLAS a
    // disjoint region of the frame's scratch batch (grows the scratch buffer
    // as needed). Regions reset each CollectGarbage tick.
    virtual bool ReserveBlasScratch(AccelerationStructureHandle blas) = 0;

    // Emit the frame's build-ordering barrier: orders this frame's builds
    // (and their scratch reuse) after any prior in-flight build on the same
    // queue. Call once per frame before the first RecordBlasBuild /
    // RecordTlasBuild. All AS passes run on the graphics queue — cross-queue
    // AS hazards are out of scope for the prototype by construction.
    virtual void RecordPreBuildBarrier(CommandList& cmd) = 0;

    // Record the build into the command list. `geometry` must be the same
    // data CreateBlas saw this frame. Scratch regions are disjoint per BLAS,
    // so batched builds need no barriers between them.
    virtual void RecordBlasBuild(CommandList& cmd,
                                 AccelerationStructureHandle blas,
                                 const BlasTriangleGeometry& geometry) = 0;

    virtual uint64_t GetBlasDeviceAddress(AccelerationStructureHandle blas) const = 0;

    // Deferred destruction: the object dies after the in-flight frame margin.
    virtual void DestroyBlas(AccelerationStructureHandle blas) = 0;

    // Deferred release of every BLAS and the frame-batch scratch buffer
    // through the same frame-stamped queue DestroyBlas uses, so in-flight
    // frames stay safe. Legal only while no TLAS slot is held: a TLAS
    // references BLASes, so its owner must release the slot first. Invalidates
    // every outstanding AccelerationStructureHandle (see the handle doc above)
    // and returns the device bytes queued for release. The queue drains
    // through CollectGarbage, which the owner calls every frame.
    virtual uint64_t ReleaseAllStructures() = 0;

    // Host side: reserve an independent TLAS slot — its own storage, scratch
    // region and address, over the SAME shared BLAS pool. One slot per
    // consumer with a distinct filtered instance set (e.g. the shadow-mask
    // lane's cast-shadow-only TLAS vs. a GI lane's unfiltered TLAS); slots
    // are cheap and never recycled (a handful ever exist for the process
    // lifetime), matching the BLAS handle's no-recycling contract.
    virtual TlasSlotHandle AcquireTlasSlot(const char* debugName) = 0;

    // Deferred release of one slot's storage/scratch through the same
    // frame-stamped queue DestroyBlas uses. The handle is invalid after.
    virtual void ReleaseTlasSlot(TlasSlotHandle slot) = 0;

    // Host side: create or grow `slot`'s TLAS for `instanceCount` instances.
    // Safe to call every frame; no-ops when capacity suffices.
    virtual bool PrepareTlas(TlasSlotHandle slot, uint32_t instanceCount) = 0;

    // Record the TLAS build reading `instanceCount` TlasInstanceData records
    // at `instanceBufferOffset` in `instanceBuffer` (usage:
    // AccelerationStructureBuildInput | ShaderDeviceAddress; host writes are
    // made visible by queue submission). Emits its own pre-barrier against
    // prior BLAS builds and a post-barrier making the TLAS visible to
    // ray-query consumers. Multiple slots built in the same frame each pay
    // this barrier independently — a harmless redundancy while there are
    // only ever a couple of slots; worth batching only if a third consumer
    // makes the extra syncs measurable.
    virtual void RecordTlasBuild(CommandList& cmd,
                                 TlasSlotHandle slot,
                                 BufferHandle instanceBuffer,
                                 uint64_t instanceBufferOffset,
                                 uint32_t instanceCount) = 0;

    virtual bool IsTlasBuilt(TlasSlotHandle slot) const = 0;

    // TLAS handle for `slot` in the backend's native form: on Vulkan the device
    // address (vkGetAccelerationStructureDeviceAddressKHR), on Metal the TLAS
    // object's MTLResourceID (see MetalAccelerationStructures.h). 0 until
    // PrepareTlas has created the object; changes when a capacity grow recreates
    // it (fetch per declare, never cache across frames).
    //
    // Every ray-query consumer binds the TLAS as a
    // DescriptorType::AccelerationStructure descriptor rather than converting an
    // address in-shader: OpConvertUToAccelerationStructureKHR has no MSL
    // equivalent (Metal's raytracing::acceleration_structure is always
    // resource-bound). This value feeds the Metal descriptor write (which
    // resolves the slot to its MTLResourceID) and serves as a non-zero
    // "TLAS is built" token; it is never handed to a shader as a raw address.
    virtual uint64_t GetTlasDeviceAddress(TlasSlotHandle slot) const = 0;

    // Advance the deferred-destruction clock and rewind the per-frame scratch
    // cursor. Exactly once per app frame, before any scratch reservation that
    // frame: without it every reservation stacks on the last one, the scratch
    // buffer grows without bound and no deferred destroy is ever freed.
    virtual void CollectGarbage() = 0;

    // Diagnostics (log-once summaries, tests).
    virtual uint64_t GetTotalBlasMemoryBytes() const = 0;
    virtual uint32_t GetLiveBlasCount() const = 0;
};

}  // namespace Rendering
}  // namespace GameEngine
