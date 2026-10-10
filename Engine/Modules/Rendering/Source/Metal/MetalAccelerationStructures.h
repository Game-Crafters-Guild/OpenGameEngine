#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <Metal/Metal.hpp>

#include "Rendering/Core/AccelerationStructure.h"

namespace GameEngine
{
namespace Rendering
{

class MetalDevice;

// Metal implementation of the minimal AS backend. Owned by MetalDevice;
// constructed only when the GPU family supports MTLAccelerationStructure and
// MSL intersection_query.
//
// Two places where Metal's model genuinely differs from Vulkan's, both
// visible in this class's contract:
//
// 1. A BLAS is a first-class MTLAccelerationStructure object, not a view over
//    an engine buffer, so BLAS storage never goes through CreateBuffer. Only
//    the shared build scratch is an engine buffer.
//
// 2. Metal has no acceleration-structure device address. A TLAS instance
//    record references its BLAS by INDEX into the TLAS descriptor's
//    instancedAccelerationStructures array, and a shader references a TLAS by
//    MTLResourceID. GetBlasDeviceAddress / GetTlasDeviceAddress therefore
//    return index-space and resource-id values respectively — see their
//    declarations below. The names come from the shared interface; on this
//    backend they are not addresses and must never be arithmetic'd like one.
//
// Scratch strategy mirrors the Vulkan backend (one grow-only device-local
// buffer per frame batch, disjoint regions handed out by ReserveBlasScratch,
// cursor reset in CollectGarbage). That is not Vulkan-shaped thinking leaking
// across: the IAccelerationStructureBackend contract is written around a
// reserve-then-record scratch batch, and Metal's build takes the same
// (buffer, offset) pair, so the same strategy is also the idiomatic Metal one.
class MetalAccelerationStructures final : public IAccelerationStructureBackend
{
  public:
    explicit MetalAccelerationStructures(MetalDevice& device);
    ~MetalAccelerationStructures() override;
    MetalAccelerationStructures(const MetalAccelerationStructures&)            = delete;
    MetalAccelerationStructures& operator=(const MetalAccelerationStructures&) = delete;

    AccelerationStructureHandle CreateBlas(const BlasTriangleGeometry& geometry) override;
    bool ReserveBlasScratch(AccelerationStructureHandle blas) override;
    void RecordPreBuildBarrier(CommandList& cmd) override;
    void RecordBlasBuild(CommandList& cmd,
                         AccelerationStructureHandle blas,
                         const BlasTriangleGeometry& geometry) override;

    // NOT an address: the BLAS pool index + 1 (0 stays the invalid sentinel).
    // The engine writes this into TlasInstanceData::BlasAddress; RecordTlasBuild
    // decodes it back to a pool index and remaps it to a position in the TLAS
    // descriptor's instancedAccelerationStructures array.
    uint64_t GetBlasDeviceAddress(AccelerationStructureHandle blas) const override;

    void DestroyBlas(AccelerationStructureHandle blas) override;
    uint64_t ReleaseAllStructures() override;

    TlasSlotHandle AcquireTlasSlot(const char* debugName) override;
    void ReleaseTlasSlot(TlasSlotHandle slot) override;
    bool PrepareTlas(TlasSlotHandle slot, uint32_t instanceCount) override;
    void RecordTlasBuild(CommandList& cmd,
                         TlasSlotHandle slot,
                         BufferHandle instanceBuffer,
                         uint64_t instanceBufferOffset,
                         uint32_t instanceCount) override;
    bool IsTlasBuilt(TlasSlotHandle slot) const override;

    // NOT an address: the TLAS object's MTLResourceID, the value a shader uses
    // to reference the structure through an argument buffer. 0 until the slot
    // has been created by PrepareTlas.
    uint64_t GetTlasDeviceAddress(TlasSlotHandle slot) const override;

    // Appends the TLAS object and every BLAS its last build referenced.
    // Binding a TLAS through an argument buffer does NOT make the primitive
    // structures it instances resident — an intersection query against a
    // non-resident BLAS faults — so the descriptor write declares them all.
    // Reads state RecordTlasBuild wrote earlier in the frame; callers must be
    // ordered after that pass (they are: nothing traces a TLAS it has not
    // built).
    void CollectTlasResidentResources(TlasSlotHandle slot, std::vector<MTL::Resource*>& out) const;

    void CollectGarbage() override;

    uint64_t GetTotalBlasMemoryBytes() const override { return m_TotalBlasBytes; }
    uint32_t GetLiveBlasCount() const override { return m_LiveBlasCount; }

  private:
    struct BlasSlot
    {
        MTL::AccelerationStructure* As = nullptr;  // owned (+1)
        uint64_t StorageBytes          = 0;
        uint64_t ScratchOffset         = 0;  // region assigned for this frame
        uint64_t BuildSize             = 0;  // buildScratchBufferSize
        bool Live                      = false;
    };

    struct TlasSlot
    {
        MTL::AccelerationStructure* As = nullptr;  // owned (+1)
        uint64_t ResourceId            = 0;
        uint64_t StorageBytes          = 0;
        uint64_t ScratchBytes          = 0;
        uint64_t ScratchOffset         = 0;
        uint32_t Capacity              = 0;  // instances the object was sized for
        // Metal-shaped instance records translated from the engine's
        // Vulkan-shaped TlasInstanceData. One buffer per slot, written in that
        // slot's RecordTlasBuild only.
        BufferHandle InstanceStaging{};
        uint64_t InstanceStagingBytes = 0;
        // BLAS pool indices the last RecordTlasBuild referenced, for residency.
        // Indices rather than object pointers: a BLAS destroyed between the
        // build and the next descriptor write would leave a dangling pointer,
        // whereas a stale index is rejected by the Live check on read.
        std::vector<uint32_t> ReferencedBlas;
        bool BuiltOnce                = false;
        bool Live                     = false;
    };

    struct PendingDestroy
    {
        MTL::AccelerationStructure* As = nullptr;
        BufferHandle Storage{};
        uint64_t FrameStamp = 0;
    };

    // Builds the triangle descriptor for `geometry`, resolving the engine's
    // pool device addresses back to (MTLBuffer, offset) pairs. Returns an
    // autoreleased descriptor, or nullptr when an address does not resolve.
    MTL::PrimitiveAccelerationStructureDescriptor* MakeBlasDescriptor(
        const BlasTriangleGeometry& geometry) const;

    bool EnsureScratchCapacity(uint64_t bytes);
    void DeferDestroy(MTL::AccelerationStructure* as, BufferHandle storage);
    TlasSlot* FindTlasSlot(TlasSlotHandle slot);
    const TlasSlot* FindTlasSlot(TlasSlotHandle slot) const;

    MetalDevice& m_Device;

    std::vector<BlasSlot> m_Blas;
    uint32_t m_LiveBlasCount  = 0;
    uint64_t m_TotalBlasBytes = 0;

    BufferHandle m_Scratch{};
    uint64_t m_ScratchCapacity = 0;
    uint64_t m_ScratchCursor   = 0;

    std::vector<TlasSlot> m_TlasSlots;

    std::vector<PendingDestroy> m_PendingDestroy;
    uint64_t m_FrameClock = 0;
};

}  // namespace Rendering
}  // namespace GameEngine
