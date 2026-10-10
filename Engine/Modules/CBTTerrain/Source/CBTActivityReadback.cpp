#include "CBTTerrain/CBTActivityReadback.h"

#include "CBTTerrain/CBTLayout.h"

#include "Rendering/Core/CommandList.h"

#include <cstring>

namespace GameEngine::CBTTerrain
{

using namespace GameEngine::Rendering;

namespace
{
constexpr uint64_t kCounterBytes = uint64_t(kWQCounterSlots) * sizeof(int32_t);
// The stamp word follows the counters.
constexpr uint64_t kSlotBytes = kCounterBytes + sizeof(uint32_t);

// Never 0, so a zero-filled slot never matches.
uint32_t Stamp(uint64_t sequence)
{
    return static_cast<uint32_t>(sequence) + 1u;
}
} // namespace

bool CBTActivityReadback::Initialize(IDevice& device)
{
    m_Device = &device;
    BufferDesc desc{};
    desc.size = kSlotBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Readback;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "CBT.ActivityReadback";
    return m_Ring.Init(&device, desc, device.GetFramesInFlight() + 2u);
}

void CBTActivityReadback::Shutdown()
{
    m_Ring.Destroy(m_Device);
    m_Device = nullptr;
}

void CBTActivityReadback::ForgetAfterDeviceRebuild()
{
    m_Ring.Destroy(nullptr);
    m_Device = nullptr;
}

CBTActivitySlot CBTActivityReadback::Begin(const RenderGraph::RGFrame& frame, uint64_t sequence)
{
    if (!m_Ring.IsInitialized())
        return {};
    return CBTActivitySlot{m_Ring.BeginWrite(frame, sequence), sequence};
}

void CBTActivityReadback::Record(CommandList& cl, BufferHandle workQueue, const CBTActivitySlot& slot)
{
    if (!workQueue.IsValid() || !slot.Buffer.IsValid())
        return;
    cl.Barrier(ResourceBarrier::CreateBufferBarrier(workQueue, ResourceState::UnorderedAccess,
                                                    ResourceState::CopySource));
    cl.CopyBuffer(workQueue, slot.Buffer, static_cast<size_t>(kCounterBytes));
    cl.FillBuffer(slot.Buffer, static_cast<size_t>(kCounterBytes), sizeof(uint32_t),
                  Stamp(slot.Sequence));
    cl.Barrier(ResourceBarrier::CreateBufferBarrier(workQueue, ResourceState::CopySource,
                                                    ResourceState::UnorderedAccess));
}

bool CBTActivityReadback::TryRead(uint64_t& outSequence, CBTUpdateActivity& outActivity)
{
    const void* mapped = m_Ring.MapNewestReady(m_Device, &outSequence);
    if (!mapped)
        return false;
    int32_t counters[kWQCounterSlots];
    std::memcpy(counters, mapped, sizeof(counters));
    uint32_t stamp = 0;
    std::memcpy(&stamp, static_cast<const uint8_t*>(mapped) + kCounterBytes, sizeof(stamp));
    m_Ring.Unmap(m_Device);
    if (stamp != Stamp(outSequence))
        return false;
    outActivity.SplitServed = counters[kWQAllocateCounter];
    outActivity.MergeServed = counters[kWQSimplifyCounter];
    outActivity.PressureStep = counters[kWQPressureStep];
    outActivity.OffFrustumKeepStep = counters[kWQOffFrustumKeepStep];
    outActivity.VertexEvalCount = counters[kWQVertexEvalCounter];
    return true;
}

} // namespace GameEngine::CBTTerrain
