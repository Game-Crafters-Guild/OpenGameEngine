#pragma once

#include "Rendering/Core/Handle.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

namespace GameEngine
{
namespace Rendering
{

/// The single point through which every SUBMISSION and PRESENTATION on one PHYSICAL Vulkan
/// queue passes. Not every operation: the teardown and swapchain-recreation paths still call
/// vkQueueWaitIdle on the raw queue handles outside it.
///
/// Two Vulkan rules make this a per-physical-queue object rather than a per-QueueType one:
/// VkQueue is an externally-synchronised parameter to every call that takes it, and the values
/// one queue signals on a timeline semaphore must strictly increase. Both properties belong to
/// the queue the work actually runs on, and this device's roles do not map one-to-one onto
/// queues: presentation always resolves to the graphics queue, the compute and transfer roles
/// resolve to a single shared family whenever that family is the most dedicated candidate for
/// both, and either role falls back to graphics when no dedicated family exists. Keyed on
/// QueueType, every one of those aliases would run concurrently under separate locks against
/// separate counters — which is the same queue, unsynchronised.
///
/// The counter lives here rather than beside the queue handle because allocating a value and
/// submitting it are one indivisible step. Allocate outside the lock and two threads read the
/// same value, both signal it, and the first to complete releases the second's command buffers
/// while they are still executing — a duplicate signal is also invalid Vulkan in its own right.
class QueueSubmitContext
{
public:
    /// What a submit did. Both fields matter: a device with no timeline semaphore submits
    /// successfully while signalling nothing, which is not the same as a failed submit even
    /// though neither yields a value work can be retired against.
    struct SubmitResult
    {
        bool Submitted = false;      ///< The vkQueueSubmit itself succeeded.
        uint64_t SignalledValue = 0; ///< Timeline value this submit signalled; 0 when it signalled none.
    };

    QueueSubmitContext() = default;
    QueueSubmitContext(const QueueSubmitContext&) = delete;
    QueueSubmitContext& operator=(const QueueSubmitContext&) = delete;

    /// Binds the queue and the timeline it signals. `timelineSemaphore` may be VK_NULL_HANDLE on
    /// devices without timeline semaphores; such a queue still serialises its operations, but
    /// SubmitAndSignal allocates no values and reports none.
    void Bind(VkQueue queue, SemaphoreHandle timeline, VkSemaphore timelineSemaphore)
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        m_Queue = queue;
        m_Timeline = timeline;
        m_TimelineSemaphore = timelineSemaphore;
        m_LastSignalled.store(0, std::memory_order_release);
    }

    /// Drops the queue, the timeline and the counter. A device rebuild invalidates every handle
    /// and creates fresh semaphores starting at 0, so the counter must restart with them.
    void Unbind()
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        m_Queue = VK_NULL_HANDLE;
        m_Timeline = {};
        m_TimelineSemaphore = VK_NULL_HANDLE;
        m_LastSignalled.store(0, std::memory_order_release);
    }

    /// The binding these four read is written only by Bind/Unbind, under the submit mutex,
    /// which they do not take. Sound because binding happens only during initial bringup —
    /// before any submitting thread exists — and during device rebuild, which runs on the
    /// render-loop tick outside the frame's submit phases and holds m_DeviceRebuildMutex
    /// EXCLUSIVE, quiescing the shared-lock upload workers.
    ///
    /// The rebuild mutex alone is NOT the guarantor: it excludes only shared-lock holders,
    /// and the worker submit path (TextureService::GetOrUpload -> ExecuteCommandLists) never
    /// takes it. A NEW reader on any thread that could overlap a rebuild would be a data race.
    VkQueue Queue() const { return m_Queue; }
    SemaphoreHandle Timeline() const { return m_Timeline; }
    VkSemaphore TimelineSemaphore() const { return m_TimelineSemaphore; }
    bool HasTimeline() const { return m_TimelineSemaphore != VK_NULL_HANDLE; }

    /// How far this queue has been submitted to: the value the most recent successful submit
    /// signalled. Monotonic and published with release ordering, so a reader racing a submit
    /// sees either that submit's value or the previous one, never a torn or unpublished one.
    ///
    /// Work is retired against the value SubmitAndSignal *returned*, never against this. This
    /// answers "how far has this queue been submitted to" and nothing more: by the time a
    /// caller reads it, another thread's submit may already have advanced it past its own.
    uint64_t LastSignalled() const { return m_LastSignalled.load(std::memory_order_acquire); }

    /// Submits work that signals this queue's timeline.
    ///
    /// `submitFn(uint64_t signalValue) -> bool` must issue exactly one vkQueueSubmit and return
    /// whether it succeeded. When `signalValue` is non-zero the submit MUST signal this queue's
    /// timeline to exactly that value; when it is zero the queue has no timeline and the submit
    /// must signal none. `submitFn` runs with this queue's mutex held, so it must not submit to
    /// — or lock — any other queue.
    ///
    /// The returned value is published only on success, so a failed submit leaves the counter
    /// where it was and the next submit reuses the value: nothing may wait on, or retire
    /// against, a value the GPU will never signal.
    template <typename SubmitFn>
    SubmitResult SubmitAndSignal(SubmitFn&& submitFn)
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);

        const bool hasTimeline = m_TimelineSemaphore != VK_NULL_HANDLE;
        const uint64_t signalValue =
            hasTimeline ? m_LastSignalled.load(std::memory_order_relaxed) + 1 : 0;

        if (!submitFn(signalValue))
            return {};

        if (hasTimeline)
            m_LastSignalled.store(signalValue, std::memory_order_release);

        return {true, signalValue};
    }

    /// Runs `queueOp()` with this queue's mutex held, for the operations that consume no
    /// timeline value: fence-only frame markers, bring-up submits, and presentation. Consuming a
    /// value here would leave a hole the timeline never reaches, hanging every later wait on it.
    ///
    /// Presentation belongs here because vkQueuePresentKHR takes the same externally-synchronised
    /// VkQueue that vkQueueSubmit does.
    template <typename QueueOp>
    decltype(auto) WithQueueLocked(QueueOp&& queueOp)
    {
        std::lock_guard<std::mutex> lock(m_SubmitMutex);
        return queueOp();
    }

private:
    VkQueue m_Queue = VK_NULL_HANDLE;
    SemaphoreHandle m_Timeline;
    VkSemaphore m_TimelineSemaphore = VK_NULL_HANDLE;
    // Written only under m_SubmitMutex, which is what makes allocate-and-publish indivisible.
    // Atomic so LastSignalled() readers never serialise against a submit in flight.
    std::atomic<uint64_t> m_LastSignalled{0};
    std::mutex m_SubmitMutex;
};

} // namespace Rendering
} // namespace GameEngine
