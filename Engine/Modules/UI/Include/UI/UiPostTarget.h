#pragma once

#include <atomic>
#include <memory>

namespace GameEngine
{
class UIManager;

namespace UI
{
class UiDispatcher;

/// Where an element's posted actions go: the owning manager's dispatcher, and that manager.
///
/// Threading: Set runs on the UI thread only (the element's owner change); Load runs on any
/// thread (UIElement::PostAction from a file-watch, job or download callback). The pair is
/// guarded by a one-byte spinlock because std::atomic<std::shared_ptr> is missing from libc++.
/// Each critical section is one shared_ptr copy or swap, so neither side waits longer than that.
///
/// Lifetime: the dispatcher is shared with its manager, so a route loaded just before the
/// manager is destroyed still names live memory. The manager closes its dispatcher as it dies
/// and a closed dispatcher refuses posts, so work routed to a dead manager is dropped, never
/// queued. Owner may dangle once the manager is gone; it is only dereferenced by work the
/// dispatcher accepted, which runs from that manager's own Drain while the manager is alive.
class UiPostTarget
{
  public:
    struct Route
    {
        std::shared_ptr<UiDispatcher> Dispatcher;
        UIManager* Owner = nullptr;
    };

    UiPostTarget() = default;
    UiPostTarget(const UiPostTarget&) = delete;
    UiPostTarget& operator=(const UiPostTarget&) = delete;

    /// UI thread only.
    void Set(std::shared_ptr<UiDispatcher> dispatcher, UIManager* owner) noexcept;

    /// Any thread. The returned route is one consistent (dispatcher, owner) pair.
    Route Load() const noexcept;

  private:
    void Lock() const noexcept;
    void Unlock() const noexcept;

    mutable std::atomic_flag m_Locked;
    std::shared_ptr<UiDispatcher> m_Dispatcher;
    UIManager* m_Owner = nullptr;
};

} // namespace UI
} // namespace GameEngine
