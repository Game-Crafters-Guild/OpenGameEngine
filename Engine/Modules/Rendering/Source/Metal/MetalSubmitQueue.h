#pragma once

#include <Metal/Metal.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// FIFO deferred-commit worker for one MTLCommandQueue.
//
// Metal encodes semaphore waits at queue granularity: an encodeWait stalls
// every later command buffer on that queue until the event is signaled, and
// deadlocks outright when the matching signal is submitted later on the same
// queue (the engine's timeline-semaphore contract allows wait-before-signal
// submission order). Holding command buffers back on the CPU until their
// waits are satisfied keeps the hardware queues stall-free — the same
// strategy MoltenVK uses for VkSemaphore waits.
//
// Submissions with no waits commit inline when the FIFO is idle, so the
// common path stays synchronous.
class MetalSubmitQueue
{
  public:
    struct Submit
    {
        std::vector<std::pair<MTL::SharedEvent*, uint64_t>> Waits;   // retained
        std::vector<MTL::CommandBuffer*> Buffers;                    // retained, uncommitted
        std::vector<std::pair<MTL::SharedEvent*, uint64_t>> Signals; // retained
    };

    ~MetalSubmitQueue();

    void Start(MTL::CommandQueue* queue);
    // Drains the FIFO, then joins the worker.
    void Stop();
    // Takes ownership of the submit's retained references.
    void Enqueue(Submit&& submit);
    // Blocks until every queued submit has been committed.
    void Flush();

  private:
    void Run();
    void Execute(Submit& submit);

    MTL::CommandQueue* m_Queue = nullptr;
    std::thread m_Thread;
    std::mutex m_Mutex;
    std::condition_variable m_WorkCv;
    std::condition_variable m_IdleCv;
    std::deque<Submit> m_Pending;
    bool m_Busy = false;
    bool m_ShutdownRequested = false;
};

} // namespace Rendering
} // namespace GameEngine
