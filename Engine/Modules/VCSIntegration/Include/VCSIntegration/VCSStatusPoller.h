#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace GameEngine
{

// The background status poll every VCS provider runs: one poll as soon as it
// starts, then one per interval, and one right after each Wake. A burst of
// wakes (property-edit saves during a slider drag) collapses into at most two
// polls: the immediate one, then one more after a short cooldown. The poll
// function runs on the poll thread only; an exception it throws is logged and
// the next poll runs as scheduled.
class VCSStatusPoller
{
public:
    using PollFunction = std::function<void()>;

    static constexpr int kDefaultIntervalSeconds = 10;

    VCSStatusPoller() = default;
    ~VCSStatusPoller();
    VCSStatusPoller(const VCSStatusPoller&) = delete;
    VCSStatusPoller& operator=(const VCSStatusPoller&) = delete;

    // Starts the poll thread, stopping a running one first. threadName names
    // the thread for debuggers and profilers and prefixes poll-failure logs.
    void Start(std::string threadName, PollFunction poll);
    // Stops and joins the poll thread. Safe when it is not running.
    void Stop();
    // Requests a poll now instead of at the next interval. Safe from any
    // thread; ignored while stopped.
    void Wake();
    // True from Stop until the next Start. A poll in progress reads it to
    // abandon long work early.
    bool IsStopping() const { return m_Stopping.load(); }

    // Seconds between polls; a non-positive value restores the default. Takes
    // effect from the next wait.
    void SetIntervalSeconds(int seconds);
    int GetIntervalSeconds() const { return m_IntervalSeconds.load(); }

private:
    void Run();
    void PollOnce();
    // Blocks until the interval elapses, Wake is called or Stop begins.
    // Returns true when a Wake ended the wait.
    bool WaitForNextPoll();
    void WaitForCooldown();

    std::string m_ThreadName;
    PollFunction m_Poll;
    std::atomic<bool> m_Stopping{false};
    std::atomic<int> m_IntervalSeconds{kDefaultIntervalSeconds};
    std::mutex m_Mutex;
    std::condition_variable m_WakeCondition;
    bool m_WakeRequested = false; // guarded by m_Mutex
    std::thread m_Thread;
};

} // namespace GameEngine
