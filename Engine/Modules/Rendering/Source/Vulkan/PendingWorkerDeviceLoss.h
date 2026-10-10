#pragma once

#include <atomic>
#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

// Cross-thread hand-off of a device loss seen on a job thread.
//
// The device-loss observation itself must run on the render thread: it drives
// DeviceHealthState's recover-relose bookkeeping, whose counters are plain
// (non-atomic) members, and it retrieves per-queue checkpoints from queues the
// render thread submits to. A worker that sees VK_ERROR_DEVICE_LOST on its own
// submit therefore records it here and lets the next BeginFrame perform it.
//
// Deliberately free of any Vulkan handle or logging dependency so the claim rules
// are unit-testable without a GPU, in the same spirit as DeviceHealthState.
class PendingWorkerDeviceLoss
{
  public:
    // Job thread: record a loss observed while the device was at `generation`.
    // The first record wins — concurrent workers are all reporting the same dead
    // device, and one observation is what the render thread needs. Release-ordered
    // so the claiming thread sees what this one wrote before it gave up.
    void Note(uint32_t generation) noexcept
    {
        uint32_t expected = kNone;
        m_Generation.compare_exchange_strong(expected, generation, std::memory_order_release,
                                             std::memory_order_relaxed);
    }

    // Render thread: take any pending loss. Returns true only when one was recorded
    // and the device has not been rebuilt since — a record from an older generation
    // describes a device that no longer exists, and performing it would latch Lost
    // onto the healthy replacement. Either way the slot is cleared, so a stale
    // record cannot block the next real one.
    bool Claim(uint32_t currentGeneration) noexcept
    {
        const uint32_t recorded = m_Generation.exchange(kNone, std::memory_order_acquire);
        return recorded != kNone && recorded == currentGeneration;
    }

    // True when a record is waiting, whatever its generation. Diagnostics only.
    bool HasPending() const noexcept { return m_Generation.load(std::memory_order_relaxed) != kNone; }

  private:
    // No generation ever takes this value: the counter starts at 0 and increments
    // once per device teardown.
    static constexpr uint32_t kNone = UINT32_MAX;
    std::atomic<uint32_t> m_Generation{kNone};
};

} // namespace Rendering
} // namespace GameEngine
