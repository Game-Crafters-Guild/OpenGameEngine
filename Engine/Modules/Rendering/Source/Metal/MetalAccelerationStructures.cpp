#include "MetalAccelerationStructures.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "Logger/Logger.h"
#include "MetalCommandList.h"
#include "MetalDevice.h"

namespace GameEngine
{
namespace Rendering
{

namespace
{

// Deferred-destruction margin in CollectGarbage ticks (one per scheduled
// frame), matching the Vulkan backend: an object is only reachable by frames
// recorded before its DeferDestroy, all retired after this many ticks.
constexpr uint64_t kDestroyMarginFrames = IDevice::kMaxSupportedFramesInFlight + 1;

// Metal requires acceleration-structure build scratch offsets to be 32-byte
// aligned. 256 is used here for the same reason the Vulkan backend aligns
// generously: it keeps every region comfortably inside the requirement on any
// family without querying a per-device property Metal does not expose.
constexpr uint64_t kScratchAlignment = 256;

constexpr uint64_t kMinScratchBytes = 1ull << 20;

// Vulkan packs customIndex/mask and sbtOffset/flags into the low 24 / high 8
// bits of two words (VkAccelerationStructureInstanceKHR bitfields). The engine
// writes that layout on every backend, so Metal decodes it here.
constexpr uint32_t kInstanceIndexMask = 0x00FFFFFFu;
constexpr uint32_t kInstanceFlagShift = 24u;

MTL::Buffer* ResolveBuffer(MetalDevice& device, BufferHandle handle)
{
    MetalBuffer* buffer = handle.IsValid() ? device.GetMetalBuffer(handle) : nullptr;
    return buffer != nullptr ? buffer->buffer : nullptr;
}

MTL::IndexType ToMtlIndexType(IndexType type)
{
    return type == IndexType::Uint32 ? MTL::IndexTypeUInt32 : MTL::IndexTypeUInt16;
}

uint32_t IndexSizeBytes(IndexType type)
{
    return type == IndexType::Uint32 ? 4u : 2u;
}

// Vulkan's VkGeometryInstanceFlagBitsKHR subset the engine emits -> Metal's
// instance options. Only facing-cull-disable is in use today (occlusion rays);
// unknown bits are dropped rather than reinterpreted.
MTL::AccelerationStructureInstanceOptions ToMtlInstanceOptions(uint32_t vkFlags)
{
    uint32_t options = MTL::AccelerationStructureInstanceOptionNone;
    if ((vkFlags & kTlasInstanceFlagTriangleFacingCullDisable) != 0u)
    {
        options |= MTL::AccelerationStructureInstanceOptionDisableTriangleCulling;
    }
    return static_cast<MTL::AccelerationStructureInstanceOptions>(options);
}

// The engine's TlasInstanceData carries a ROW-major 3x4 (element (row, col) at
// row * 4 + col). Metal wants a column-major packed_float4x3 (column j holds
// rows 0..2). Same twelve floats, different order — a memcpy would silently
// shear every instance, so the transpose is explicit.
void WriteTransposedTransform(const float (&rowMajor3x4)[12], MTL::PackedFloat4x3& out)
{
    for (int column = 0; column < 4; ++column)
    {
        out.columns[column].x = rowMajor3x4[0 * 4 + column];
        out.columns[column].y = rowMajor3x4[1 * 4 + column];
        out.columns[column].z = rowMajor3x4[2 * 4 + column];
    }
}

}  // namespace

MetalAccelerationStructures::MetalAccelerationStructures(MetalDevice& device)
    : m_Device(device)
{
}

MetalAccelerationStructures::~MetalAccelerationStructures()
{
    // Device teardown waits for idle before destroying backends, so immediate
    // release is safe here (mirrors other device-owned Metal objects).
    for (PendingDestroy& pending : m_PendingDestroy)
    {
        if (pending.As != nullptr)
        {
            pending.As->release();
        }
        if (pending.Storage.IsValid())
        {
            m_Device.DestroyBuffer(pending.Storage);
        }
    }
    for (BlasSlot& slot : m_Blas)
    {
        if (slot.Live && slot.As != nullptr)
        {
            slot.As->release();
        }
    }
    for (TlasSlot& slot : m_TlasSlots)
    {
        if (!slot.Live)
        {
            continue;
        }
        if (slot.As != nullptr)
        {
            slot.As->release();
        }
        if (slot.InstanceStaging.IsValid())
        {
            m_Device.DestroyBuffer(slot.InstanceStaging);
        }
    }
    if (m_Scratch.IsValid())
    {
        m_Device.DestroyBuffer(m_Scratch);
    }
}

MTL::PrimitiveAccelerationStructureDescriptor*
MetalAccelerationStructures::MakeBlasDescriptor(const BlasTriangleGeometry& geometry) const
{
    // Metal's geometry descriptor binds MTLBuffer objects, not device
    // addresses. The engine hands out pool addresses, so resolve them back to
    // the owning buffer (+ byte offset) through the device's registry.
    uint64_t vertexOffset = 0;
    MTL::Buffer* vertexBuffer = m_Device.FindBufferByGpuAddress(geometry.VertexAddress, vertexOffset);
    uint64_t indexOffset = 0;
    MTL::Buffer* indexBuffer = m_Device.FindBufferByGpuAddress(geometry.IndexAddress, indexOffset);
    if (vertexBuffer == nullptr || indexBuffer == nullptr)
    {
        Logger::Log::Error("MetalAccelerationStructures: BLAS geometry address did not resolve to a "
                           "live buffer (vertex {}, index {})",
                           geometry.VertexAddress, geometry.IndexAddress);
        return nullptr;
    }

    MTL::AccelerationStructureTriangleGeometryDescriptor* tris =
        MTL::AccelerationStructureTriangleGeometryDescriptor::descriptor();
    tris->setVertexBuffer(vertexBuffer);
    // Metal has no per-build "value added to every fetched index" (Vulkan's
    // firstVertex / vkCmdDrawIndexed vertexOffset). Folding it into the vertex
    // base is exactly equivalent: the fetch is base + index * stride either way.
    tris->setVertexBufferOffset(vertexOffset
                                + static_cast<uint64_t>(geometry.FirstVertex) * geometry.VertexStrideBytes);
    // Position leads every interleaved core vertex (VertexAttributeFlags
    // binding-0 contract), so the pool is read in place at full stride.
    tris->setVertexFormat(MTL::AttributeFormatFloat3);
    tris->setVertexStride(geometry.VertexStrideBytes);
    tris->setIndexBuffer(indexBuffer);
    tris->setIndexBufferOffset(indexOffset
                               + static_cast<uint64_t>(geometry.FirstIndex)
                                     * IndexSizeBytes(geometry.IndexKind));
    tris->setIndexType(ToMtlIndexType(geometry.IndexKind));
    tris->setTriangleCount(geometry.IndexCount / 3u);
    // Opaque-only, matching the Vulkan lane: today's casters are vertex-only
    // with no alpha test, so no intersection function is needed.
    tris->setOpaque(true);

    const MTL::AccelerationStructureGeometryDescriptor* geometries[] = {tris};
    NS::Array* geometryArray = NS::Array::array(reinterpret_cast<const NS::Object* const*>(geometries), 1);

    MTL::PrimitiveAccelerationStructureDescriptor* descriptor =
        MTL::PrimitiveAccelerationStructureDescriptor::descriptor();
    descriptor->setGeometryDescriptors(geometryArray);
    // Metal's default build already prefers fast intersection; PreferFastBuild
    // would be the opposite trade, so no usage flags is the fast-trace choice.
    descriptor->setUsage(MTL::AccelerationStructureUsageNone);
    return descriptor;
}

bool MetalAccelerationStructures::EnsureScratchCapacity(uint64_t bytes)
{
    if (bytes <= m_ScratchCapacity)
    {
        return true;
    }

    if (m_Scratch.IsValid())
    {
        // Retire through OUR frame clock rather than DestroyBuffer: previous
        // frames' builds may still be writing this scratch.
        DeferDestroy(nullptr, m_Scratch);
    }

    uint64_t newCapacity = std::max<uint64_t>(m_ScratchCapacity * 2u, kMinScratchBytes);
    newCapacity          = std::max(newCapacity, bytes);

    BufferDesc desc{};
    desc.size        = newCapacity;
    desc.usage       = static_cast<uint32_t>(BufferUsage::Storage);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.debugName   = "SceneAS.Scratch";
    BufferHandle handle = m_Device.CreateBuffer(desc);
    if (!handle.IsValid())
    {
        Logger::Log::Error("MetalAccelerationStructures: scratch alloc failed ({} bytes)", newCapacity);
        return false;
    }
    m_Scratch         = handle;
    m_ScratchCapacity = newCapacity;
    return true;
}

AccelerationStructureHandle MetalAccelerationStructures::CreateBlas(const BlasTriangleGeometry& geometry)
{
    if (geometry.IndexCount < 3u)
    {
        return {};
    }

    MTL::PrimitiveAccelerationStructureDescriptor* descriptor = MakeBlasDescriptor(geometry);
    if (descriptor == nullptr)
    {
        return {};
    }

    const MTL::AccelerationStructureSizes sizes = m_Device.GetMTLDevice()->accelerationStructureSizes(descriptor);
    MTL::AccelerationStructure* as = m_Device.GetMTLDevice()->newAccelerationStructure(sizes.accelerationStructureSize);
    if (as == nullptr)
    {
        Logger::Log::Error("MetalAccelerationStructures: BLAS alloc failed ({} bytes)",
                           static_cast<uint64_t>(sizes.accelerationStructureSize));
        return {};
    }
    as->setLabel(NS::String::string("SceneAS.BLAS", NS::UTF8StringEncoding));

    BlasSlot slot{};
    slot.As           = as;
    slot.StorageBytes = sizes.accelerationStructureSize;
    slot.BuildSize    = sizes.buildScratchBufferSize;
    slot.Live         = true;
    m_Blas.push_back(slot);
    ++m_LiveBlasCount;
    m_TotalBlasBytes += slot.StorageBytes;

    return AccelerationStructureHandle{static_cast<uint64_t>(m_Blas.size())};
}

bool MetalAccelerationStructures::ReserveBlasScratch(AccelerationStructureHandle blas)
{
    if (!blas.IsValid() || blas.id > m_Blas.size())
    {
        return false;
    }
    BlasSlot& slot = m_Blas[blas.id - 1];
    if (!slot.Live)
    {
        return false;
    }
    const uint64_t alignedCursor = (m_ScratchCursor + kScratchAlignment - 1) & ~(kScratchAlignment - 1);
    if (!EnsureScratchCapacity(alignedCursor + slot.BuildSize))
    {
        return false;
    }
    slot.ScratchOffset = alignedCursor;
    m_ScratchCursor    = alignedCursor + slot.BuildSize;
    return true;
}

void MetalAccelerationStructures::RecordPreBuildBarrier(CommandList& /*cmd*/)
{
    // Metal has no equivalent command. The hazards this call exists to close on
    // Vulkan — build-after-build on the same structures, and scratch reuse at
    // recurring offsets — are covered by Metal's automatic hazard tracking:
    // every AS, and the scratch buffer, is a tracked resource, and all AS work
    // is recorded into one command buffer on one queue. Opening the
    // acceleration-structure encoder ends whatever encoder preceded it, which
    // is the ordering point. MetalCommandList::Barrier takes the same position
    // for the engine's general barriers.
}

void MetalAccelerationStructures::RecordBlasBuild(CommandList& cmd,
                                                  AccelerationStructureHandle blas,
                                                  const BlasTriangleGeometry& geometry)
{
    if (!blas.IsValid() || blas.id > m_Blas.size())
    {
        return;
    }
    const BlasSlot& slot = m_Blas[blas.id - 1];
    if (!slot.Live || slot.As == nullptr)
    {
        return;
    }
    MTL::Buffer* scratch = ResolveBuffer(m_Device, m_Scratch);
    if (scratch == nullptr)
    {
        return;
    }
    MTL::PrimitiveAccelerationStructureDescriptor* descriptor = MakeBlasDescriptor(geometry);
    if (descriptor == nullptr)
    {
        return;
    }

    MTL::AccelerationStructureCommandEncoder* encoder =
        static_cast<MetalCommandList&>(cmd).EnsureAccelerationStructureEncoder();
    if (encoder == nullptr)
    {
        return;
    }
    encoder->buildAccelerationStructure(slot.As, descriptor, scratch, slot.ScratchOffset);
}

uint64_t MetalAccelerationStructures::GetBlasDeviceAddress(AccelerationStructureHandle blas) const
{
    if (!blas.IsValid() || blas.id > m_Blas.size())
    {
        return 0;
    }
    const BlasSlot& slot = m_Blas[blas.id - 1];
    // Pool index + 1 (see the header): Metal exposes no BLAS address, and the
    // +1 keeps 0 meaning "invalid" exactly as the shared contract expects.
    return slot.Live ? blas.id : 0;
}

void MetalAccelerationStructures::DeferDestroy(MTL::AccelerationStructure* as, BufferHandle storage)
{
    m_PendingDestroy.push_back(PendingDestroy{as, storage, m_FrameClock});
}

void MetalAccelerationStructures::DestroyBlas(AccelerationStructureHandle blas)
{
    if (!blas.IsValid() || blas.id > m_Blas.size())
    {
        return;
    }
    BlasSlot& slot = m_Blas[blas.id - 1];
    if (!slot.Live)
    {
        return;
    }
    DeferDestroy(slot.As, BufferHandle{});
    m_TotalBlasBytes -= slot.StorageBytes;
    --m_LiveBlasCount;
    slot = BlasSlot{};  // Live=false; index stays burned (no recycling)
}

uint64_t MetalAccelerationStructures::ReleaseAllStructures()
{
    uint64_t releasedBytes = 0;

    for (BlasSlot& slot : m_Blas)
    {
        if (!slot.Live)
        {
            continue;
        }
        DeferDestroy(slot.As, BufferHandle{});
        releasedBytes += slot.StorageBytes;
    }
    // New BLAS handle generation: ids restart at 1 (see the interface doc).
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
        DeferDestroy(nullptr, m_Scratch);
        releasedBytes += m_ScratchCapacity;
        m_Scratch         = {};
        m_ScratchCapacity = 0;
        m_ScratchCursor   = 0;
    }

    return releasedBytes;
}

MetalAccelerationStructures::TlasSlot* MetalAccelerationStructures::FindTlasSlot(TlasSlotHandle slot)
{
    if (!slot.IsValid() || slot.id > m_TlasSlots.size())
    {
        return nullptr;
    }
    TlasSlot& found = m_TlasSlots[slot.id - 1];
    return found.Live ? &found : nullptr;
}

const MetalAccelerationStructures::TlasSlot* MetalAccelerationStructures::FindTlasSlot(
    TlasSlotHandle slot) const
{
    if (!slot.IsValid() || slot.id > m_TlasSlots.size())
    {
        return nullptr;
    }
    const TlasSlot& found = m_TlasSlots[slot.id - 1];
    return found.Live ? &found : nullptr;
}

TlasSlotHandle MetalAccelerationStructures::AcquireTlasSlot(const char* /*debugName*/)
{
    m_TlasSlots.emplace_back();
    m_TlasSlots.back().Live = true;
    return TlasSlotHandle{static_cast<uint32_t>(m_TlasSlots.size())};
}

void MetalAccelerationStructures::ReleaseTlasSlot(TlasSlotHandle slotHandle)
{
    TlasSlot* slot = FindTlasSlot(slotHandle);
    if (slot == nullptr)
    {
        return;
    }
    if (slot->As != nullptr || slot->InstanceStaging.IsValid())
    {
        DeferDestroy(slot->As, slot->InstanceStaging);
    }
    *slot = TlasSlot{};  // Live=false; index stays burned (no recycling)
}

bool MetalAccelerationStructures::PrepareTlas(TlasSlotHandle slotHandle, uint32_t instanceCount)
{
    TlasSlot* slot = FindTlasSlot(slotHandle);
    if (slot == nullptr)
    {
        return false;
    }
    // Size for at least one instance so an empty scene still yields a valid
    // (empty) TLAS for consumers to bind.
    const uint32_t sizedCount = std::max(instanceCount, 1u);

    const auto reserveScratch = [this](TlasSlot& target) {
        const uint64_t alignedCursor = (m_ScratchCursor + kScratchAlignment - 1) & ~(kScratchAlignment - 1);
        if (!EnsureScratchCapacity(alignedCursor + target.ScratchBytes))
        {
            return false;
        }
        target.ScratchOffset = alignedCursor;
        m_ScratchCursor      = alignedCursor + target.ScratchBytes;
        return true;
    };

    if (sizedCount <= slot->Capacity && slot->As != nullptr)
    {
        return reserveScratch(*slot);
    }

    // Metal sizes an instance TLAS from a descriptor carrying the instance
    // count; the instanced-structure array is only needed at build time.
    MTL::InstanceAccelerationStructureDescriptor* descriptor =
        MTL::InstanceAccelerationStructureDescriptor::descriptor();
    descriptor->setInstanceCount(sizedCount);
    descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
    descriptor->setInstanceDescriptorStride(sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor));
    descriptor->setUsage(MTL::AccelerationStructureUsageNone);

    const MTL::AccelerationStructureSizes sizes = m_Device.GetMTLDevice()->accelerationStructureSizes(descriptor);

    if (slot->As != nullptr)
    {
        DeferDestroy(slot->As, BufferHandle{});
        slot->As         = nullptr;
        slot->ResourceId = 0;
        // The replacement holds no built content until its RecordTlasBuild runs.
        slot->BuiltOnce = false;
    }

    MTL::AccelerationStructure* as =
        m_Device.GetMTLDevice()->newAccelerationStructure(sizes.accelerationStructureSize);
    if (as == nullptr)
    {
        Logger::Log::Error("MetalAccelerationStructures: TLAS alloc failed ({} bytes)",
                           static_cast<uint64_t>(sizes.accelerationStructureSize));
        return false;
    }
    as->setLabel(NS::String::string("SceneAS.TLAS", NS::UTF8StringEncoding));
    slot->As           = as;
    slot->ResourceId   = as->gpuResourceID()._impl;
    slot->StorageBytes = sizes.accelerationStructureSize;
    slot->ScratchBytes = sizes.buildScratchBufferSize;
    slot->Capacity     = sizedCount;

    // Grow this slot's translated-instance staging alongside its capacity.
    const uint64_t stagingBytes =
        static_cast<uint64_t>(sizedCount) * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor);
    if (stagingBytes > slot->InstanceStagingBytes)
    {
        if (slot->InstanceStaging.IsValid())
        {
            DeferDestroy(nullptr, slot->InstanceStaging);
        }
        BufferDesc desc{};
        desc.size        = stagingBytes;
        desc.usage       = static_cast<uint32_t>(BufferUsage::Storage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        desc.debugName   = "SceneAS.TLAS.Instances";
        slot->InstanceStaging = m_Device.CreateBuffer(desc);
        if (!slot->InstanceStaging.IsValid())
        {
            slot->InstanceStagingBytes = 0;
            return false;
        }
        slot->InstanceStagingBytes = stagingBytes;
    }

    return reserveScratch(*slot);
}

void MetalAccelerationStructures::RecordTlasBuild(CommandList& cmd,
                                                  TlasSlotHandle slotHandle,
                                                  BufferHandle instanceBuffer,
                                                  uint64_t instanceBufferOffset,
                                                  uint32_t instanceCount)
{
    TlasSlot* slot = FindTlasSlot(slotHandle);
    if (slot == nullptr || slot->As == nullptr || !slot->InstanceStaging.IsValid())
    {
        return;
    }
    MTL::Buffer* scratch = ResolveBuffer(m_Device, m_Scratch);
    if (scratch == nullptr)
    {
        return;
    }

    // Translate the engine's Vulkan-shaped instance records into Metal's.
    // Both are host-visible (the source is a mapped Upload buffer), so this is
    // a CPU pass over `instanceCount` 64-byte records. It writes only THIS
    // slot's staging buffer and each slot's RecordTlasBuild runs once per
    // frame, so parallel pass recording across slots stays safe — but unlike
    // the Vulkan backend this call is not purely command recording.
    const auto* source = static_cast<const TlasInstanceData*>(m_Device.MapBuffer(instanceBuffer));
    auto* destination =
        static_cast<MTL::AccelerationStructureUserIDInstanceDescriptor*>(m_Device.MapBuffer(slot->InstanceStaging));
    if (source == nullptr || destination == nullptr)
    {
        return;
    }
    source = reinterpret_cast<const TlasInstanceData*>(reinterpret_cast<const uint8_t*>(source)
                                                      + instanceBufferOffset);

    const uint32_t writtenCount = std::min(instanceCount, slot->Capacity);

    // Metal references a BLAS by position in the descriptor's structure array,
    // so collect exactly the BLASes this build touches and remap as we go. A
    // pool-index-parallel array would need a placeholder object for every
    // destroyed slot; compacting avoids that and keeps the array minimal.
    std::vector<const NS::Object*> referenced;
    std::unordered_map<uint64_t, uint32_t> poolIndexToArrayIndex;
    referenced.reserve(writtenCount);
    slot->ReferencedBlas.clear();
    slot->ReferencedBlas.reserve(writtenCount);

    for (uint32_t i = 0; i < writtenCount; ++i)
    {
        const TlasInstanceData& in = source[i];
        MTL::AccelerationStructureUserIDInstanceDescriptor& out = destination[i];

        WriteTransposedTransform(in.Transform, out.transformationMatrix);
        out.mask   = in.CustomIndexAndMask >> kInstanceFlagShift;
        // rayQueryGetIntersectionInstanceCustomIndexEXT maps to MSL's
        // get_committed_user_instance_id(), which reads userID — hence the
        // UserID descriptor type rather than the default one.
        out.userID = in.CustomIndexAndMask & kInstanceIndexMask;
        out.intersectionFunctionTableOffset = in.SbtOffsetAndFlags & kInstanceIndexMask;
        out.options = ToMtlInstanceOptions(in.SbtOffsetAndFlags >> kInstanceFlagShift);

        out.accelerationStructureIndex = 0;
        if (in.BlasAddress == 0)
        {
            continue;  // engine marked this instance's BLAS unavailable
        }
        const auto existing = poolIndexToArrayIndex.find(in.BlasAddress);
        if (existing != poolIndexToArrayIndex.end())
        {
            out.accelerationStructureIndex = existing->second;
            continue;
        }
        // GetBlasDeviceAddress returned poolIndex + 1.
        const uint64_t poolIndex = in.BlasAddress - 1;
        if (poolIndex >= m_Blas.size() || !m_Blas[poolIndex].Live || m_Blas[poolIndex].As == nullptr)
        {
            continue;
        }
        const auto arrayIndex = static_cast<uint32_t>(referenced.size());
        referenced.push_back(m_Blas[poolIndex].As);
        slot->ReferencedBlas.push_back(static_cast<uint32_t>(poolIndex));
        poolIndexToArrayIndex.emplace(in.BlasAddress, arrayIndex);
        out.accelerationStructureIndex = arrayIndex;
    }

    MTL::InstanceAccelerationStructureDescriptor* descriptor =
        MTL::InstanceAccelerationStructureDescriptor::descriptor();
    descriptor->setInstanceCount(writtenCount);
    descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
    descriptor->setInstanceDescriptorStride(sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor));
    descriptor->setInstanceDescriptorBuffer(ResolveBuffer(m_Device, slot->InstanceStaging));
    descriptor->setInstanceDescriptorBufferOffset(0);
    descriptor->setUsage(MTL::AccelerationStructureUsageNone);
    if (!referenced.empty())
    {
        descriptor->setInstancedAccelerationStructures(
            NS::Array::array(referenced.data(), referenced.size()));
    }

    MTL::AccelerationStructureCommandEncoder* encoder =
        static_cast<MetalCommandList&>(cmd).EnsureAccelerationStructureEncoder();
    if (encoder == nullptr)
    {
        return;
    }
    encoder->buildAccelerationStructure(slot->As, descriptor, scratch, slot->ScratchOffset);
    slot->BuiltOnce = true;
}

bool MetalAccelerationStructures::IsTlasBuilt(TlasSlotHandle slotHandle) const
{
    const TlasSlot* slot = FindTlasSlot(slotHandle);
    return slot != nullptr && slot->BuiltOnce;
}

uint64_t MetalAccelerationStructures::GetTlasDeviceAddress(TlasSlotHandle slotHandle) const
{
    const TlasSlot* slot = FindTlasSlot(slotHandle);
    return slot != nullptr ? slot->ResourceId : 0;
}

void MetalAccelerationStructures::CollectTlasResidentResources(TlasSlotHandle slotHandle,
                                                               std::vector<MTL::Resource*>& out) const
{
    const TlasSlot* slot = FindTlasSlot(slotHandle);
    if (slot == nullptr || slot->As == nullptr)
    {
        return;
    }
    out.push_back(slot->As);
    for (const uint32_t poolIndex : slot->ReferencedBlas)
    {
        if (poolIndex < m_Blas.size() && m_Blas[poolIndex].Live && m_Blas[poolIndex].As != nullptr)
        {
            out.push_back(m_Blas[poolIndex].As);
        }
    }
}

void MetalAccelerationStructures::CollectGarbage()
{
    ++m_FrameClock;
    m_ScratchCursor = 0;  // regions are per-frame; ordering comes from Metal's
                          // hazard tracking on the single queue

    std::erase_if(m_PendingDestroy,
                  [this](PendingDestroy& pending)
                  {
                      if (m_FrameClock - pending.FrameStamp <= kDestroyMarginFrames)
                      {
                          return false;
                      }
                      if (pending.As != nullptr)
                      {
                          pending.As->release();
                      }
                      if (pending.Storage.IsValid())
                      {
                          m_Device.DestroyBuffer(pending.Storage);
                      }
                      return true;
                  });
}

}  // namespace Rendering
}  // namespace GameEngine
