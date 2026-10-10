#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "Rendering/Core/AccelerationStructure.h"

namespace GameEngine
{
namespace Rendering
{

class VulkanDevice;

// Vulkan implementation of the minimal AS backend, shared by every
// ray-query consumer (RT shadow-mask lane, GI probe tracing). Owned by
// VulkanDevice; constructed only when ray query was enabled at device
// creation. Storage/scratch buffers are allocated through the device's
// normal CreateBuffer path so lifetime, VMA accounting and deferred
// destruction follow the house rules.
//
// Scratch strategy: one grow-only device-local scratch buffer per frame
// batch, shared across BLAS builds and every TLAS slot's build this frame.
// ReserveBlasScratch assigns regions (host side, declaration time);
// RecordBlasBuild consumes it. Regions are disjoint, so a batch of BLAS
// builds records with no inter-build barriers; each RecordTlasBuild call
// emits its own build→build barrier before that slot's TLAS build and a
// build→compute barrier after it (see the interface doc: redundant when two
// slots build the same frame, and cheap enough not to matter at today's slot
// count). CollectGarbage resets the batch cursor each frame.
class VulkanAccelerationStructures final : public IAccelerationStructureBackend
{
  public:
    explicit VulkanAccelerationStructures(VulkanDevice& device);
    ~VulkanAccelerationStructures() override;
    VulkanAccelerationStructures(const VulkanAccelerationStructures&)            = delete;
    VulkanAccelerationStructures& operator=(const VulkanAccelerationStructures&) = delete;

    // False when any required entry point failed to load — the device then
    // reports supportsRayQuery but the backend refuses; callers see null
    // from VulkanDevice::GetAccelerationStructureBackend (warned once).
    bool IsFunctional() const { return m_Functional; }

    AccelerationStructureHandle CreateBlas(const BlasTriangleGeometry& geometry) override;
    bool ReserveBlasScratch(AccelerationStructureHandle blas) override;
    void RecordPreBuildBarrier(CommandList& cmd) override;
    void RecordBlasBuild(CommandList& cmd,
                         AccelerationStructureHandle blas,
                         const BlasTriangleGeometry& geometry) override;
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
    uint64_t GetTlasDeviceAddress(TlasSlotHandle slot) const override;

    // Backend-native TLAS object for a descriptor write
    // (VkWriteDescriptorSetAccelerationStructureKHR takes the handle, not the
    // address GetTlasDeviceAddress returns). VK_NULL_HANDLE until PrepareTlas
    // has created it; a capacity grow replaces it, so fetch per write.
    // Vulkan-only, so it is not on IAccelerationStructureBackend —
    // VulkanDevice owns this type concretely.
    VkAccelerationStructureKHR GetTlasVkHandle(TlasSlotHandle slot) const;

    void CollectGarbage() override;

    // In-place device rebuild, teardown half: destroys every acceleration
    // structure, storage buffer and the scratch against the device being torn
    // down, and forgets every BLAS (BLAS ids restart, a new generation). TLAS
    // slots stay acquired but empty, so a consumer's slot handle stays valid
    // and its next PrepareTlas creates the object on the new device. The
    // backend object itself survives, so every cached backend pointer does.
    void ReleaseForDeviceRebuild();
    // Bringup half: reloads the device-level entry points from the new
    // device; IsFunctional reports whether they resolved. A backend whose
    // rebind fails stays alive, empty and refusing every call, so the
    // pointers consumers hold stay valid; the device stops handing it out.
    void BindRebuiltDevice();

    uint64_t GetTotalBlasMemoryBytes() const override { return m_TotalBlasBytes; }
    uint32_t GetLiveBlasCount() const override { return m_LiveBlasCount; }

  private:
    struct BlasSlot
    {
        VkAccelerationStructureKHR As = VK_NULL_HANDLE;
        BufferHandle Storage{};
        VkDeviceAddress Address       = 0;
        uint64_t StorageBytes         = 0;
        // Scratch region assigned by ReserveBlasScratch for this frame.
        uint64_t ScratchOffset        = 0;
        uint64_t BuildSize            = 0;  // buildScratchSize
        bool Live                     = false;
    };

    struct PendingDestroy
    {
        VkAccelerationStructureKHR As = VK_NULL_HANDLE;
        BufferHandle Storage{};
        uint64_t FrameStamp = 0;
    };

    // Fill the (geometry, buildInfo, rangeInfo) triple for a BLAS build.
    // Shared by size query and build record so both see identical inputs.
    void FillBlasBuildInfo(const BlasTriangleGeometry& geometry,
                           VkAccelerationStructureGeometryKHR& outGeom,
                           VkAccelerationStructureBuildGeometryInfoKHR& outBuild,
                           VkAccelerationStructureBuildRangeInfoKHR& outRange) const;

    bool LoadEntryPoints();
    bool EnsureScratchCapacity(uint64_t bytes);
    void DeferDestroy(VkAccelerationStructureKHR as, BufferHandle storage);

    VulkanDevice& m_Device;
    bool m_Functional = false;

    // Entry points (loaded via vkGetDeviceProcAddr at construction).
    PFN_vkCreateAccelerationStructureKHR       m_FpCreate      = nullptr;
    PFN_vkDestroyAccelerationStructureKHR      m_FpDestroy     = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR m_FpBuildSizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR m_FpAsAddress = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR    m_FpCmdBuild    = nullptr;

    // BLAS slots — monotonic within a generation, handle id = index + 1 (see
    // AccelerationStructureHandle doc: no recycling in the prototype;
    // ReleaseAllStructures clears the vector and restarts ids).
    std::vector<BlasSlot> m_Blas;
    uint32_t m_LiveBlasCount  = 0;
    uint64_t m_TotalBlasBytes = 0;

    // Frame-batch scratch (grow-only buffer, per-frame cursor).
    BufferHandle m_Scratch{};
    uint64_t m_ScratchCapacity   = 0;
    uint64_t m_ScratchCursor     = 0;
    VkDeviceAddress m_ScratchAddress = 0;
    uint32_t m_ScratchAlignment  = 256;  // minAccelerationStructureScratchOffsetAlignment

    // Independent TLAS slots, one per consumer (shadow-mask lane, GI, ...).
    // Handle id = index + 1, matching the BLAS slot's no-recycling contract
    // (a handful of slots ever exist for the process lifetime).
    struct TlasSlot
    {
        VkAccelerationStructureKHR As = VK_NULL_HANDLE;
        VkDeviceAddress Address       = 0;
        BufferHandle Storage{};
        uint64_t StorageBytes  = 0;
        uint64_t ScratchBytes  = 0;
        uint64_t ScratchOffset = 0;  // this frame's reserved scratch region
        uint32_t Capacity      = 0;  // instances the current objects were sized for
        bool BuiltOnce         = false;
        bool Live              = false;  // false once ReleaseTlasSlot fires
    };
    std::vector<TlasSlot> m_TlasSlots;
    TlasSlot* FindTlasSlot(TlasSlotHandle slot);
    const TlasSlot* FindTlasSlot(TlasSlotHandle slot) const;

    // Deferred destruction: frame-stamped, drained by CollectGarbage after
    // the in-flight margin. The clock is CollectGarbage calls (one per
    // scheduled frame), not device frames — coarser but strictly safe.
    std::vector<PendingDestroy> m_PendingDestroy;
    uint64_t m_FrameClock = 0;
};

}  // namespace Rendering
}  // namespace GameEngine
