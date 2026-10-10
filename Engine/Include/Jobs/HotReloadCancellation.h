#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace GameEngine
{

/**
 * @brief One hot-reload run's cancellation: the flag its stages poll, and a
 * wake-up for a stage that blocks on another thread.
 *
 * Stages receive the flag as their cancel token (Token) and poll it. A stage
 * that waits for work on another thread, such as the swap waiting for the main
 * thread, waits through WaitForOrCancel, which Cancel wakes at once instead of
 * letting the wait run to its timeout.
 *
 * Thread safety: every member may be called from any thread. The waiter
 * evaluates its predicate under m_Mutex, and Cancel and NotifyWaiters take
 * m_Mutex before they notify, so a state change made before either call is
 * never missed by a waiter.
 */
class HotReloadCancellation
{
  public:
    /**
     * @brief Set the flag, then wake every waiter.
     */
    void Cancel()
    {
        m_Cancelled.store(true, std::memory_order_relaxed);
        NotifyWaiters();
    }

    bool IsCancelled() const { return m_Cancelled.load(std::memory_order_relaxed); }

    /**
     * @brief The cancel token stages poll: @p cancellation's flag, sharing its
     * ownership. Null when @p cancellation is null.
     */
    static std::shared_ptr<std::atomic<bool>> Token(const std::shared_ptr<HotReloadCancellation>& cancellation)
    {
        if (!cancellation)
        {
            return nullptr;
        }
        return std::shared_ptr<std::atomic<bool>>(cancellation, &cancellation->m_Cancelled);
    }

    /**
     * @brief Wake every waiter so it re-evaluates its predicate. Call after
     * changing the state a predicate reads.
     */
    void NotifyWaiters()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
        }
        m_Cv.notify_all();
    }

    /**
     * @brief Block until @p ready returns true, the run is cancelled, or
     * @p timeout passes.
     * @return The value of @p ready when the wait ended.
     */
    template <typename Predicate>
    bool WaitForOrCancel(std::chrono::milliseconds timeout, Predicate ready)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Cv.wait_for(lock, timeout, [&] { return ready() || IsCancelled(); });
        return ready();
    }

    /**
     * @brief Block until @p ready returns true, whether or not the run is cancelled.
     */
    template <typename Predicate>
    void Wait(Predicate ready)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Cv.wait(lock, ready);
    }

  private:
    std::atomic<bool> m_Cancelled{false};
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
};

} // namespace GameEngine
