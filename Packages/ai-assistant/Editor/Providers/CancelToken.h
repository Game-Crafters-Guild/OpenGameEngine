#pragma once

#include <atomic>

namespace GameEngine
{
/// Stop request for one agent turn. The panel's thread calls Cancel(); the provider
/// running the turn polls IsCancelled() and ends the turn with TurnOutcome::Stopped.
/// One token per turn: a cancelled token stays cancelled.
class CancelToken
{
public:
    CancelToken() = default;
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    /// Requests the stop. Thread-safe; idempotent.
    void Cancel() noexcept { m_Cancelled.store(true, std::memory_order_release); }

    /// True once Cancel() has been called. Thread-safe.
    bool IsCancelled() const noexcept { return m_Cancelled.load(std::memory_order_acquire); }

    /// The flag itself, for an engine API that polls a raw flag: pass its address as
    /// HttpClient's PostOptions::Cancellation. Lives as long as the token.
    const std::atomic<bool>& Flag() const noexcept { return m_Cancelled; }

private:
    std::atomic<bool> m_Cancelled{false};
};
} // namespace GameEngine
