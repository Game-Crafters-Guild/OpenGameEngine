#include "VulkanAccelerationStructures.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

#include "Logger/Logger.h"
#include "VulkanCommandList.h"
#include "VulkanDevice.h"

namespace GameEngine
{
namespace Rendering
{

// TlasInstanceData is handed to the TLAS build as the instance array without
// translation, so its layout must equal the driver's instance descriptor. The
// public header cannot name that struct; this is where the claim is checked.
static_assert(sizeof(TlasInstanceData) == sizeof(VkAccelerationStructureInstanceKHR),
              "TlasInstanceData must match VkAccelerationStructureInstanceKHR");
static_assert(offsetof(TlasInstanceData, Transform)
                  == offsetof(VkAccelerationStructureInstanceKHR, transform),
              "TlasInstanceData::Transform must match VkAccelerationStructureInstanceKHR::transform");
static_assert(offsetof(TlasInstanceData, BlasAddress)
                  == offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference),
              "TlasInstanceData::BlasAddress must match "
              "VkAccelerationStructureInstanceKHR::accelerationStructureReference");

// The shadow lane's instance flags mirror the driver's enum so engine-side code
// needs no Vulkan header; this is where that mirror is checked.
static_assert(kTlasInstanceFlagTriangleFacingCullDisable
                  == static_cast<uint32_t>(VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR),
              "kTlasInstanceFlagTriangleFacingCullDisable must match "
              "VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR");

namespace
{

// Deferred-destruction margin in CollectGarbage ticks (one tick per
// scheduled frame). Coarser than device frame fences but strictly safe:
// an object is only reachable by frames recorded before its DeferDestroy,
// all of which have retired after kMaxSupportedFramesInFlight + 1 ticks.
constexpr uint64_t kDestroyMarginFrames = IDevice::kMaxSupportedFramesInFlight + 1;

VkIndexType ToVkIndexType(IndexType type)
{
    return type == IndexType::Uint32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

}  // namespace

VulkanAccelerationStructures::VulkanAccelerationStructures(VulkanDevice& device)
    : m_Device(device)
{
    m_Functional = LoadEntryPoints();
    if (!m_Functional)
    {
        Logger::Log::Error(
            "VulkanAccelerationStructures: entry points missing despite enabled extensions; "
            "AS backend disabled");
        return;
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(m_Device.GetVkPhysicalDevice(), &props2);
    m_ScratchAlignment = std::max(asProps.minAccelerationStructureScratchOffsetAlignment, 1u);
}

VulkanAccelerationStructures::~VulkanAccelerationStructures()
{
    // Device teardown path: the device waits idle before backend destruction,
    // so immediate destruction is safe here (mirrors other device-owned
    // Vulkan objects).
    ReleaseForDeviceRebuild();
}

bool VulkanAccelerationStructures::LoadEntryPoints()
{
    VkDevice vk = m_Device.GetVkDevice();
    m_FpCreate = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(vk, "vkCreateAccelerationStructureKHR"));
    m_FpDestroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(vk, "vkDestroyAccelerationStructureKHR"));
    m_FpBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(vk, "vkGetAccelerationStructureBuildSizesKHR"));
    m_FpAsAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(vk, "vkGetAccelerationStructureDeviceAddressKHR"));
    m_FpCmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(vk, "vkCmdBuildAccelerationStructuresKHR"));
    return m_FpCreate && m_FpDestroy && m_FpBuildSizes && m_FpAsAddress && m_FpCmdBuild;
}

void VulkanAccelerationStructures::ReleaseForDeviceRebuild()
{
    // Immediate, not deferred: both callers run with the device's queues idle
    // or lost, and the device these objects belong to is destroyed next.
    VkDevice vk = m_Device.GetVkDevice();
    for (PendingDestroy& p : m_PendingDestroy)
    {
        if (p.As != VK_NULL_HANDLE)
            m_FpDestroy(vk, p.As, nullptr);
        if (p.Storage.IsValid())
            m_Device.DestroyBuffer(p.Storage);
    }
    m_PendingDestroy.clear();
    for (BlasSlot& slot : m_Blas)
    {
        if (slot.Live && slot.As != VK_NULL_HANDLE)
            m_FpDestroy(vk, slot.As, nullptr);
        if (slot.Live && slot.Storage.IsValid())
            m_Device.DestroyBuffer(slot.Storage);
    }
    m_Blas.clear();
    m_LiveBlasCount  = 0;
    m_TotalBlasBytes = 0;
    for (TlasSlot& slot : m_TlasSlots)
    {
        if (slot.As != VK_NULL_HANDLE)
            m_FpDestroy(vk, slot.As, nullptr);
        if (slot.Storage.IsValid())
            m_Device.DestroyBuffer(slot.Storage);
        const bool live = slot.Live;
        slot      = TlasSlot{};
        slot.Live = live;
    }
    if (m_Scratch.IsValid())
        m_Device.DestroyBuffer(m_Scratch);
    m_Scratch         = {};
    m_ScratchCapacity = 0;
    m_ScratchCursor   = 0;
    m_ScratchAddress  = 0;
}

void VulkanAccelerationStructures::BindRebuiltDevice()
{
    m_Functional = LoadEntryPoints();
}

void VulkanAccelerationStructures::FillBlasBuildInfo(
    const BlasTriangleGeometry& geometry,
    VkAccelerationStructureGeometryKHR& outGeom,
    VkAccelerationStructureBuildGeometryInfoKHR& outBuild,
    VkAccelerationStructureBuildRangeInfoKHR& outRange) const
{
    outGeom = {};
    outGeom.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    outGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Opaque-only: matches today's raster shadow depth (vertex-only, no
    // alpha test). Any-hit for cutout casters is the priced G2 upgrade.
    outGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkAccelerationStructureGeometryTrianglesDataKHR& tris = outGeom.geometry.triangles;
    tris.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    // Position leads every interleaved core vertex (VertexAttributeFlags
    // binding-0 contract), so the pool reads in place at full stride.
    tris.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
    tris.vertexData.deviceAddress = geometry.VertexAddress;
    tris.vertexStride             = geometry.VertexStrideBytes;
    tris.maxVertex                = geometry.MaxVertex;
    tris.indexType                = ToVkIndexType(geometry.IndexKind);
    tris.indexData.deviceAddress  = geometry.IndexAddress;

    outBuild = {};
    outBuild.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    outBuild.type  = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    outBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    outBuild.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    outBuild.geometryCount = 1;
    outBuild.pGeometries   = &outGeom;

    outRange = {};
    outRange.primitiveCount = geometry.IndexCount / 3u;
    // primitiveOffset is BYTES into the index buffer; FirstIndex is
    // pool-space in index units (MeshGPUEntry::firstIndex semantics).
    const uint32_t indexSize = geometry.IndexKind == IndexType::Uint32 ? 4u : 2u;
    outRange.primitiveOffset = geometry.FirstIndex * indexSize;
    // Added to every index value at fetch — the vkCmdDrawIndexed
    // vertexOffset analogue (pool-space vertex base of this mesh).
    outRange.firstVertex = geometry.FirstVertex;
}

bool VulkanAccelerationStructures::EnsureScratchCapacity(uint64_t bytes)
{
    if (bytes <= m_ScratchCapacity)
        return true;

    if (m_Scratch.IsValid())
    {
        // Retire through OUR frame clock, never DestroyBuffer directly: the
        // device's deferred path keys the last SIGNALED timeline value, which
        // can predate the previous frame's still-executing builds writing
        // this scratch — freeing it under them is a device fault (observed
        // 2026-07-20: DEVICE_LOST seconds after the big BLAS burst frames,
        // which are exactly the scratch-growth frames).
        DeferDestroy(VK_NULL_HANDLE, m_Scratch);
    }

    uint64_t newCapacity = std::max<uint64_t>(m_ScratchCapacity * 2u, 1u << 20);
    newCapacity          = std::max(newCapacity, bytes);

    // The buffer's device address is NOT guaranteed to be a multiple of
    // minAccelerationStructureScratchOffsetAlignment (suballocated backings
    // observed at 64-byte bases against a 128 requirement — VUID 03710, data
    // dependent). Every scratch region is cursor-aligned RELATIVE to the
    // base, so align the base itself and over-allocate by one alignment unit
    // to keep the advertised capacity usable.
    BufferDesc desc{};
    desc.size  = newCapacity + m_ScratchAlignment;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::ShaderDeviceAddress);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.debugName   = "RTShadowMask.AS.Scratch";
    BufferHandle h = m_Device.CreateBuffer(desc);
    if (!h.IsValid())
    {
        Logger::Log::Error("VulkanAccelerationStructures: scratch alloc failed ({} bytes)",
                           newCapacity);
        return false;
    }
    m_Scratch         = h;
    m_ScratchCapacity = newCapacity;
    const VkDeviceAddress raw = m_Device.GetBufferDeviceAddress(h);
    if (raw == 0)
        return false;
    const uint64_t align = m_ScratchAlignment;
    m_ScratchAddress     = (raw + align - 1) & ~(align - 1);
    return true;
}

AccelerationStructureHandle VulkanAccelerationStructures::CreateBlas(
    const BlasTriangleGeometry& geometry)
{
    if (!m_Functional || geometry.IndexCount < 3u)
        return {};

    VkAccelerationStructureGeometryKHR geom;
    VkAccelerationStructureBuildGeometryInfoKHR build;
    VkAccelerationStructureBuildRangeInfoKHR range;
    FillBlasBuildInfo(geometry, geom, build, range);

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    const uint32_t primitiveCount = range.primitiveCount;
    m_FpBuildSizes(m_Device.GetVkDevice(),
                   VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                   &primitiveCount, &sizes);

    BufferDesc desc{};
    desc.size  = sizes.accelerationStructureSize;
    desc.usage = static_cast<uint32_t>(BufferUsage::AccelerationStructureStorage
                                       | BufferUsage::ShaderDeviceAddress);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.debugName   = "RTShadowMask.BLAS";
    BufferHandle storage = m_Device.CreateBuffer(desc);
    if (!storage.IsValid())
        return {};

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = m_Device.GetVkBuffer(storage);
    createInfo.offset = 0;
    createInfo.size   = sizes.accelerationStructureSize;
    createInfo.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    if (m_FpCreate(m_Device.GetVkDevice(), &createInfo, nullptr, &as) != VK_SUCCESS)
    {
        m_Device.DestroyBuffer(storage);
        return {};
    }

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = as;

    BlasSlot slot{};
    slot.As            = as;
    slot.Storage       = storage;
    slot.Address       = m_FpAsAddress(m_Device.GetVkDevice(), &addrInfo);
    slot.StorageBytes  = sizes.accelerationStructureSize;
    slot.BuildSize     = sizes.buildScratchSize;
    slot.Live          = true;
    m_Blas.push_back(slot);
    ++m_LiveBlasCount;
    m_TotalBlasBytes += sizes.accelerationStructureSize;

    return AccelerationStructureHandle{static_cast<uint64_t>(m_Blas.size())};
}

bool VulkanAccelerationStructures::ReserveBlasScratch(AccelerationStructureHandle blas)
{
    if (!m_Functional || !blas.IsValid() || blas.id > m_Blas.size())
        return false;
    BlasSlot& slot = m_Blas[blas.id - 1];
    if (!slot.Live)
        return false;
    const uint64_t alignedCursor =
        (m_ScratchCursor + m_ScratchAlignment - 1) & ~uint64_t(m_ScratchAlignment - 1);
    if (!EnsureScratchCapacity(alignedCursor + slot.BuildSize))
        return false;
    slot.ScratchOffset = alignedCursor;
    m_ScratchCursor    = alignedCursor + slot.BuildSize;
    return true;
}

void VulkanAccelerationStructures::RecordPreBuildBarrier(CommandList& cmd)
{
    // Orders this frame's builds after any prior in-flight build on the same
    // (graphics) queue: BLAS/TLAS reads-after-writes AND scratch reuse
    // (write-after-write at recurring offsets). COMPUTE is in the source
    // stages for the write-after-READ against the previous frame's ray-query
    // mask pass (an execution dependency is sufficient for WAR; the read bit
    // in srcAccessMask is inert but keeps the intent visible). Sync1 barrier —
    // the AS stage/access bits exist in both sync models; sync1 keeps this
    // independent of the sync2 fallback matrix.
    VkCommandBuffer vkCmd = static_cast<VulkanCommandList&>(cmd).GetVkCommandBuffer();
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
                          | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                          | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(vkCmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                             | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
}

void VulkanAccelerationStructures::RecordBlasBuild(CommandList& cmd,
                                                   AccelerationStructureHandle blas,
                                                   const BlasTriangleGeometry& geometry)
{
    if (!m_Functional || !blas.IsValid() || blas.id > m_Blas.size())
        return;
    const BlasSlot& slot = m_Blas[blas.id - 1];
    if (!slot.Live)
        return;

    VkAccelerationStructureGeometryKHR geom;
    VkAccelerationStructureBuildGeometryInfoKHR build;
    VkAccelerationStructureBuildRangeInfoKHR range;
    FillBlasBuildInfo(geometry, geom, build, range);
    build.dstAccelerationStructure  = slot.As;
    // The scratch buffer is final by record time: RG2 declaration (all
    // CreateBlas calls) strictly precedes Execute(), so a mid-batch grow
    // re-based every region into the one buffer all records now read.
    build.scratchData.deviceAddress = m_ScratchAddress + slot.ScratchOffset;

    const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &range;
    VkCommandBuffer vkCmd = static_cast<VulkanCommandList&>(cmd).GetVkCommandBuffer();
    m_FpCmdBuild(vkCmd, 1, &build, &rangePtr);
}

uint64_t VulkanAccelerationStructures::GetBlasDeviceAddress(
    AccelerationStructureHandle blas) const
{
    if (!blas.IsValid() || blas.id > m_Blas.size())
        return 0;
    const BlasSlot& slot = m_Blas[blas.id - 1];
    return slot.Live ? slot.Address : 0;
}

void VulkanAccelerationStructures::DeferDestroy(VkAccelerationStructureKHR as,
                                                BufferHandle storage)
{
    m_PendingDestroy.push_back(PendingDestroy{as, storage, m_FrameClock});
}

void VulkanAccelerationStructures::DestroyBlas(AccelerationStructureHandle blas)
{
    if (!blas.IsValid() || blas.id > m_Blas.size())
        return;
    BlasSlot& slot = m_Blas[blas.id - 1];
    if (!slot.Live)
        return;
    DeferDestroy(slot.As, slot.Storage);
    m_TotalBlasBytes -= slot.StorageBytes;
    --m_LiveBlasCount;
    slot = BlasSlot{};  // Live=false; slot index stays burned (no recycling)
}

uint64_t VulkanAccelerationStructures::ReleaseAllStructures()
{
    uint64_t releasedBytes = 0;

    for (BlasSlot& slot : m_Blas)
    {
        if (!slot.Live)
            continue;
        DeferDestroy(slot.As, slot.Storage);
        releasedBytes += slot.StorageBytes;
    }
    // New BLAS handle generation: ids restart at 1. Safe because the caller
    // runs this only while no consumer holds a handle or a TLAS slot, and RG2
    // ordering guarantees no exec lambda holding old handles runs after this
    // declaration-time call (exec(N) strictly precedes declare(N+1)).
    m_Blas.clear();
    m_LiveBlasCount  = 0;
    m_TotalBlasBytes = 0;

    // Legal only while no consumer holds a TLAS slot (see the interface doc),
    // so no slot content is left to release.
    assert(std::none_of(m_TlasSlots.begin(), m_TlasSlots.end(),
                        [](const TlasSlot& slot) { return slot.Live; })
           && "ReleaseAllStructures: a consumer still holds a TLAS slot");

    if (m_Scratch.IsValid())
    {
        // Same rule as EnsureScratchCapacity: retire through OUR frame clock,
        // never DestroyBuffer directly — prior frames' builds may still be
        // writing this scratch.
        DeferDestroy(VK_NULL_HANDLE, m_Scratch);
        releasedBytes += m_ScratchCapacity;
        m_Scratch         = {};
        m_ScratchCapacity = 0;
        m_ScratchCursor   = 0;
        m_ScratchAddress  = 0;
    }

    return releasedBytes;
}

VulkanAccelerationStructures::TlasSlot* VulkanAccelerationStructures::FindTlasSlot(
    TlasSlotHandle slot)
{
    if (!slot.IsValid() || slot.id > m_TlasSlots.size())
        return nullptr;
    TlasSlot& s = m_TlasSlots[slot.id - 1];
    return s.Live ? &s : nullptr;
}

const VulkanAccelerationStructures::TlasSlot* VulkanAccelerationStructures::FindTlasSlot(
    TlasSlotHandle slot) const
{
    if (!slot.IsValid() || slot.id > m_TlasSlots.size())
        return nullptr;
    const TlasSlot& s = m_TlasSlots[slot.id - 1];
    return s.Live ? &s : nullptr;
}

TlasSlotHandle VulkanAccelerationStructures::AcquireTlasSlot(const char* /*debugName*/)
{
    // No recycling (matches the BLAS handle contract): a handful of slots
    // ever exist for the process lifetime, one per ray-query consumer.
    m_TlasSlots.emplace_back();
    m_TlasSlots.back().Live = true;
    return TlasSlotHandle{static_cast<uint32_t>(m_TlasSlots.size())};
}

void VulkanAccelerationStructures::ReleaseTlasSlot(TlasSlotHandle slot)
{
    TlasSlot* s = FindTlasSlot(slot);
    if (!s)
        return;
    if (s->As != VK_NULL_HANDLE || s->Storage.IsValid())
        DeferDestroy(s->As, s->Storage);
    *s = TlasSlot{};  // Live=false; slot index stays burned (no recycling)
}

bool VulkanAccelerationStructures::PrepareTlas(TlasSlotHandle slotHandle, uint32_t instanceCount)
{
    TlasSlot* slot = FindTlasSlot(slotHandle);
    if (!m_Functional || !slot)
        return false;
    // Size for at least one instance so an empty scene still yields a valid
    // (empty) TLAS for future consumers to bind.
    const uint32_t sizedCount = std::max(instanceCount, 1u);

    if (sizedCount <= slot->Capacity && slot->As != VK_NULL_HANDLE)
    {
        // Reserve this frame's TLAS scratch region.
        const uint64_t alignedCursor =
            (m_ScratchCursor + m_ScratchAlignment - 1) & ~uint64_t(m_ScratchAlignment - 1);
        if (!EnsureScratchCapacity(alignedCursor + slot->ScratchBytes))
            return false;
        slot->ScratchOffset = alignedCursor;
        m_ScratchCursor     = alignedCursor + slot->ScratchBytes;
        return true;
    }

    VkAccelerationStructureGeometryKHR geom{};
    geom.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type  = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries   = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    m_FpBuildSizes(m_Device.GetVkDevice(),
                   VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &sizedCount,
                   &sizes);

    if (slot->As != VK_NULL_HANDLE)
    {
        DeferDestroy(slot->As, slot->Storage);
        slot->As      = VK_NULL_HANDLE;
        slot->Storage = {};
        slot->Address = 0;
        // The replacement object holds no built content until its
        // RecordTlasBuild runs — consumers gating on IsTlasBuilt must not
        // dereference the fresh storage in the recreate→build gap.
        slot->BuiltOnce = false;
    }

    BufferDesc desc{};
    desc.size  = sizes.accelerationStructureSize;
    desc.usage = static_cast<uint32_t>(BufferUsage::AccelerationStructureStorage
                                       | BufferUsage::ShaderDeviceAddress);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.debugName   = "SceneAS.TLAS";
    slot->Storage = m_Device.CreateBuffer(desc);
    if (!slot->Storage.IsValid())
        return false;

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = m_Device.GetVkBuffer(slot->Storage);
    createInfo.offset = 0;
    createInfo.size   = sizes.accelerationStructureSize;
    createInfo.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (m_FpCreate(m_Device.GetVkDevice(), &createInfo, nullptr, &slot->As) != VK_SUCCESS)
    {
        m_Device.DestroyBuffer(slot->Storage);
        slot->Storage = {};
        slot->As      = VK_NULL_HANDLE;
        slot->Address = 0;
        return false;
    }
    {
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
        addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addrInfo.accelerationStructure = slot->As;
        slot->Address = m_FpAsAddress(m_Device.GetVkDevice(), &addrInfo);
    }
    slot->StorageBytes = sizes.accelerationStructureSize;
    slot->ScratchBytes = sizes.buildScratchSize;
    slot->Capacity      = sizedCount;

    const uint64_t alignedCursor =
        (m_ScratchCursor + m_ScratchAlignment - 1) & ~uint64_t(m_ScratchAlignment - 1);
    if (!EnsureScratchCapacity(alignedCursor + slot->ScratchBytes))
        return false;
    slot->ScratchOffset = alignedCursor;
    m_ScratchCursor      = alignedCursor + slot->ScratchBytes;
    return true;
}

void VulkanAccelerationStructures::RecordTlasBuild(CommandList& cmd,
                                                   TlasSlotHandle slotHandle,
                                                   BufferHandle instanceBuffer,
                                                   uint64_t instanceBufferOffset,
                                                   uint32_t instanceCount)
{
    TlasSlot* slot = FindTlasSlot(slotHandle);
    if (!m_Functional || !slot || slot->As == VK_NULL_HANDLE)
        return;

    VkCommandBuffer vkCmd = static_cast<VulkanCommandList&>(cmd).GetVkCommandBuffer();

    // BLAS writes (this frame's batch) → TLAS-build reads. Host writes to the
    // instance buffer are made visible by queue submission (Vulkan host
    // write ordering guarantee) — no host barrier needed.
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(vkCmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    }

    VkAccelerationStructureGeometryKHR geom{};
    geom.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    geom.geometry.instances.data.deviceAddress =
        m_Device.GetBufferDeviceAddress(instanceBuffer) + instanceBufferOffset;

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type  = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.dstAccelerationStructure  = slot->As;
    build.geometryCount             = 1;
    build.pGeometries               = &geom;
    build.scratchData.deviceAddress = m_ScratchAddress + slot->ScratchOffset;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &range;
    m_FpCmdBuild(vkCmd, 1, &build, &rangePtr);

    // TLAS write → future ray-query reads in compute. A no-op tail until a
    // consumer's compute pass actually samples this slot, kept so the
    // visibility contract ships with the build.
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(vkCmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
    }

    slot->BuiltOnce = true;
}

bool VulkanAccelerationStructures::IsTlasBuilt(TlasSlotHandle slot) const
{
    const TlasSlot* s = FindTlasSlot(slot);
    return s && s->BuiltOnce;
}

uint64_t VulkanAccelerationStructures::GetTlasDeviceAddress(TlasSlotHandle slot) const
{
    const TlasSlot* s = FindTlasSlot(slot);
    return s ? s->Address : 0;
}

VkAccelerationStructureKHR VulkanAccelerationStructures::GetTlasVkHandle(TlasSlotHandle slot) const
{
    const TlasSlot* s = FindTlasSlot(slot);
    return s ? s->As : VK_NULL_HANDLE;
}

void VulkanAccelerationStructures::CollectGarbage()
{
    ++m_FrameClock;
    m_ScratchCursor = 0;  // regions are per-frame; reuse ordering comes from
                          // RecordPreBuildBarrier on the single queue

    std::erase_if(m_PendingDestroy,
                  [this](PendingDestroy& p)
                  {
                      if (m_FrameClock - p.FrameStamp <= kDestroyMarginFrames)
                          return false;
                      if (p.As != VK_NULL_HANDLE)
                          m_FpDestroy(m_Device.GetVkDevice(), p.As, nullptr);
                      if (p.Storage.IsValid())
                          m_Device.DestroyBuffer(p.Storage);
                      return true;
                  });
}

}  // namespace Rendering
}  // namespace GameEngine
