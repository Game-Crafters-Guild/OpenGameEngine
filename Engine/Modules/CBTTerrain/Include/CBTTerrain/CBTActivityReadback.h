#pragma once

// CBTActivityReadback — the work-queue counters of a recorded CBT update, read back without a stall.
//
// The update pass copies the counters into a ring slot right after its kernels (Record, same
// command list, so no graph edge is needed), and the slot becomes readable once that frame's
// submission has signaled. CBTUpdateRestGate consumes what this reads.

#include "CBTTerrain/CBTUpdateRestGate.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"

#include <cstdint>

namespace GameEngine::Rendering
{
class CommandList;
}

namespace GameEngine::CBTTerrain
{

// One update's readback: the ring slot it copies into, and the sequence stamped beside the copy.
struct CBTActivitySlot
{
    Rendering::BufferHandle Buffer{};
    uint64_t Sequence = 0;
};

class CBTActivityReadback
{
  public:
    // Creates frames-in-flight + 2 slots, so a reused slot's previous copy has always retired.
    bool Initialize(Rendering::IDevice& device);
    void Shutdown();
    // Drops the slot handles without destroying them: a device rebuild already did.
    void ForgetAfterDeviceRebuild();

    // At declare time, a slot for the update declared into `frame`, carrying `sequence`
    // (CBTUpdateRestGate::OnUpdateRecorded). An invalid Buffer when the ring is not ready or the
    // slot is busy; the update then simply goes unread.
    CBTActivitySlot Begin(const Rendering::RenderGraph::RGFrame& frame, uint64_t sequence);

    // Records the copy of the counters `workQueue` holds into the slot, then stamps it with the
    // slot's sequence: a slot whose copy never ran (zero-filled, or holding an older update)
    // carries no matching stamp and is never read as quiet. Expects the work queue in
    // UnorderedAccess, as the update leaves it, and restores that state.
    static void Record(Rendering::CommandList& cl, Rendering::BufferHandle workQueue,
                       const CBTActivitySlot& slot);

    // The newest reading whose frame has signaled and whose stamp matches; older ones are
    // superseded. False when none.
    bool TryRead(uint64_t& outSequence, CBTUpdateActivity& outActivity);

    void OnFrameSubmitted(const Rendering::RenderGraph::RGFrame& frame,
                          const Rendering::IDevice::GpuSyncToken& token)
    {
        m_Ring.OnFrameSubmitted(frame, token);
    }
    void OnFrameStreamRetired(const Rendering::RenderGraph::RGFrame& frame)
    {
        m_Ring.OnFrameStreamRetired(frame);
    }

  private:
    Rendering::IDevice* m_Device = nullptr;
    Rendering::RenderGraph::RGReadbackRing<uint64_t> m_Ring;
};

} // namespace GameEngine::CBTTerrain
