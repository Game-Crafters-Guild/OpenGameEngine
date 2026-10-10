#include "VCSIntegration/VCSStatusPoller.h"

#include "Logger/Logger.h"
#include "Platform/Thread.h"

#include <chrono>
#include <exception>
#include <utility>

namespace GameEngine
{
namespace
{
// Cooldown after a wake-driven poll. Wakes that arrive during it are absorbed
// into one poll right after it instead of each running its own VCS status.
// Leading edge: the first wake in a burst still polls immediately.
constexpr std::chrono::milliseconds kWakeCooldown{500};
} // namespace

VCSStatusPoller::~VCSStatusPoller()
{
    Stop();
}

void VCSStatusPoller::Start(std::string threadName, PollFunction poll)
{
    Stop();
    m_ThreadName = std::move(threadName);
    m_Poll = std::move(poll);
    m_Stopping = false;
    m_Thread = std::thread(&VCSStatusPoller::Run, this);
}

void VCSStatusPoller::Stop()
{
    {
        // Set under the lock so the poll thread cannot miss the notify between
        // testing its wait predicate and blocking.
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stopping = true;
    }
    m_WakeCondition.notify_all();
    if (m_Thread.joinable())
        m_Thread.join();
}

void VCSStatusPoller::Wake()
{
    {
        // Tested under the lock so a Stop racing this call cannot leave the
        // request set for the next Start to act on.
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopping)
            return;
        m_WakeRequested = true;
    }
    m_WakeCondition.notify_one();
}

void VCSStatusPoller::SetIntervalSeconds(int seconds)
{
    m_IntervalSeconds = seconds > 0 ? seconds : kDefaultIntervalSeconds;
}

void VCSStatusPoller::Run()
{
    Platform::SetCurrentThreadName(m_ThreadName.c_str());

    if (!m_Stopping)
        PollOnce();
    while (!m_Stopping)
    {
        const bool woken = WaitForNextPoll();
        if (m_Stopping)
            break;

        PollOnce();
        if (woken)
            WaitForCooldown();
    }
}

void VCSStatusPoller::PollOnce()
{
    try
    {
        m_Poll();
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("{}: status poll failed: {}", m_ThreadName, e.what());
    }
    catch (...)
    {
        Logger::Log::Warning("{}: status poll failed with an unknown error", m_ThreadName);
    }
}

bool VCSStatusPoller::WaitForNextPoll()
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_WakeCondition.wait_for(lock, std::chrono::seconds(m_IntervalSeconds.load()),
                             [this] { return m_Stopping.load() || m_WakeRequested; });
    const bool woken = m_WakeRequested;
    m_WakeRequested = false;
    return woken;
}

void VCSStatusPoller::WaitForCooldown()
{
    // Leaves m_WakeRequested alone: a wake during the cooldown makes the next
    // WaitForNextPoll return at once.
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_WakeCondition.wait_for(lock, kWakeCooldown, [this] { return m_Stopping.load(); });
}

} // namespace GameEngine
