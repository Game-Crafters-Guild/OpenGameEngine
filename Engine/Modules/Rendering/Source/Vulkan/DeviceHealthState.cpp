#include "DeviceHealthState.h"

namespace GameEngine
{
namespace Rendering
{

bool DeviceHealthState::TransitionToLost(Clock::time_point now, Clock::duration stickWindow) noexcept
{
    DeviceHealth prev = m_Health.load(std::memory_order_relaxed);
    for (;;)
    {
        // Already Lost -> no new edge. Failed is terminal -> a stray loss
        // observation must not revive it into another rebuild attempt.
        if (prev == DeviceHealth::Lost || prev == DeviceHealth::Failed)
        {
            return false;
        }
        if (m_Health.compare_exchange_weak(prev, DeviceHealth::Lost, std::memory_order_relaxed))
        {
            // Judge the recovery this loss just ended. Before the first recovery
            // there is nothing to judge, so a session's first loss starts at zero.
            if (m_HasRecovered)
            {
                m_RecoveryCycles = ((now - m_LastRecoveryTime) >= stickWindow) ? 0 : m_RecoveryCycles + 1;
            }
            return true;
        }
    }
}

bool DeviceHealthState::BeginRebuild() noexcept
{
    DeviceHealth expected = DeviceHealth::Lost;
    if (m_Health.compare_exchange_strong(expected, DeviceHealth::Rebuilding, std::memory_order_relaxed))
    {
        ++m_RebuildAttempts;
        return true;
    }
    return false;
}

bool DeviceHealthState::NoteRebuildSucceeded() noexcept
{
    DeviceHealth expected = DeviceHealth::Rebuilding;
    return m_Health.compare_exchange_strong(expected, DeviceHealth::AwaitingReprovision, std::memory_order_relaxed);
}

bool DeviceHealthState::NoteRebuildFailed() noexcept
{
    DeviceHealth expected = DeviceHealth::Rebuilding;
    return m_Health.compare_exchange_strong(expected, DeviceHealth::Lost, std::memory_order_relaxed);
}

bool DeviceHealthState::NoteReprovisioned(Clock::time_point now) noexcept
{
    DeviceHealth expected = DeviceHealth::AwaitingReprovision;
    if (m_Health.compare_exchange_strong(expected, DeviceHealth::Healthy, std::memory_order_relaxed))
    {
        m_RebuildAttempts = 0;
        m_LastRecoveryTime = now;
        m_HasRecovered = true;
        return true;
    }
    return false;
}

bool DeviceHealthState::TransitionToFailed() noexcept
{
    DeviceHealth prev = m_Health.load(std::memory_order_relaxed);
    for (;;)
    {
        if (prev != DeviceHealth::Lost && prev != DeviceHealth::Rebuilding)
        {
            return false;
        }
        if (m_Health.compare_exchange_weak(prev, DeviceHealth::Failed, std::memory_order_relaxed))
        {
            return true;
        }
    }
}

bool DeviceHealthState::TransitionToHung(Clock::time_point now) noexcept
{
    DeviceHealth expected = DeviceHealth::Healthy;
    if (m_Health.compare_exchange_strong(expected, DeviceHealth::Hung, std::memory_order_relaxed))
    {
        m_HungSince = now;
        return true;
    }
    return false;
}

bool DeviceHealthState::NoteFenceSignaled() noexcept
{
    DeviceHealth expected = DeviceHealth::Hung;
    return m_Health.compare_exchange_strong(expected, DeviceHealth::Healthy, std::memory_order_relaxed);
}

double DeviceHealthState::HungElapsedMs(Clock::time_point now) const noexcept
{
    if (Load() != DeviceHealth::Hung)
    {
        return 0.0;
    }
    return std::chrono::duration<double, std::milli>(now - m_HungSince).count();
}

bool DeviceHealthState::ShouldEscalate(Clock::time_point now, Clock::duration cap) const noexcept
{
    return Load() == DeviceHealth::Hung && (now - m_HungSince) >= cap;
}

} // namespace Rendering
} // namespace GameEngine
