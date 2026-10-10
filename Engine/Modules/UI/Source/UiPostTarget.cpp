#include "UI/UiPostTarget.h"

#include "UI/UiDispatcher.h"

#include <thread>
#include <utility>

namespace GameEngine::UI
{

void UiPostTarget::Set(std::shared_ptr<UiDispatcher> dispatcher, UIManager* owner) noexcept
{
    Lock();
    m_Dispatcher.swap(dispatcher);
    m_Owner = owner;
    Unlock();
    // `dispatcher` now holds the previous route and releases it here, outside the lock: when it
    // is the last reference to a dead manager's dispatcher, that destructor must not run under
    // a spinlock a worker may be waiting on.
}

UiPostTarget::Route UiPostTarget::Load() const noexcept
{
    Lock();
    Route route{m_Dispatcher, m_Owner};
    Unlock();
    return route;
}

void UiPostTarget::Lock() const noexcept
{
    while (m_Locked.test_and_set(std::memory_order_acquire))
        std::this_thread::yield();
}

void UiPostTarget::Unlock() const noexcept
{
    m_Locked.clear(std::memory_order_release);
}

} // namespace GameEngine::UI
