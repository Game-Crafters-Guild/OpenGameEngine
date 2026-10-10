#include "UI/UiCoalescedPost.h"

#include <atomic>
#include <utility>

namespace GameEngine::UI
{

// Target and Action are set on construction and never change. Requested is shared with every
// thread that calls Request. Cancelled is written by Cancel and read by RunRequested, both on
// the UI thread; Request reads it from any thread only to skip a post that would do nothing.
struct UiCoalescedPost::State
{
    UiPostHandle Target;
    std::function<void()> Action;
    std::atomic<bool> Requested{false};
    std::atomic<bool> Cancelled{false};
};

UiCoalescedPost::UiCoalescedPost(UiPostHandle target, std::function<void()> action)
    : m_State(std::make_shared<State>())
{
    m_State->Target = std::move(target);
    m_State->Action = std::move(action);
}

void UiCoalescedPost::Request() const
{
    if (!m_State || m_State->Cancelled.load(std::memory_order_acquire))
        return;
    if (m_State->Requested.exchange(true, std::memory_order_acq_rel))
        return;
    std::shared_ptr<State> state = m_State;
    // A refused post (the element or its manager is gone) must not leave the latch set, or no
    // later request would ever post again.
    if (!m_State->Target.Post([state]() { RunRequested(state); }))
        m_State->Requested.store(false, std::memory_order_release);
}

void UiCoalescedPost::Cancel() const
{
    if (m_State)
        m_State->Cancelled.store(true, std::memory_order_release);
}

void UiCoalescedPost::RunRequested(const std::shared_ptr<State>& state)
{
    // Clear the latch before running, so a request that arrives while the action runs posts
    // another run rather than being absorbed into this one.
    state->Requested.store(false, std::memory_order_release);
    if (state->Cancelled.load(std::memory_order_acquire))
        return;
    state->Action();
}

} // namespace GameEngine::UI
