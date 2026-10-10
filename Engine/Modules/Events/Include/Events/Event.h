#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace Detail
{

/// Type-erased subscriber-list state shared between Event<Args...> and the
/// non-template EventSubscription handle.
class EventStateBase
{
  public:
    virtual ~EventStateBase() = default;
    virtual void Remove(std::uint64_t id) = 0;
};

} // namespace Detail

/**
 * @brief Registration handle: unsubscribes when destroyed, inert if the event
 *        died first.
 *
 * Non-template on purpose — a subscriber declares `EventSubscription m_Sub;`
 * without spelling the event's argument list.
 */
class EventSubscription final
{
  public:
    EventSubscription() = default;
    ~EventSubscription() { Reset(); }

    EventSubscription(EventSubscription&& other) noexcept
        : m_State(std::move(other.m_State)), m_Id(other.m_Id)
    {
        other.m_Id = 0;
    }

    EventSubscription& operator=(EventSubscription&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_State = std::move(other.m_State);
            m_Id = other.m_Id;
            other.m_Id = 0;
        }
        return *this;
    }

    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;

    /// Unsubscribe now rather than at destruction; idempotent.
    void Reset()
    {
        // An expired state means the event is already gone, which took the
        // subscriber list with it — nothing left to unsubscribe from.
        if (const std::shared_ptr<Detail::EventStateBase> state = m_State.lock();
            state && m_Id != 0)
        {
            state->Remove(m_Id);
        }
        m_State.reset();
        m_Id = 0;
    }

  private:
    template <typename... Args>
    friend class Event;

    EventSubscription(std::weak_ptr<Detail::EventStateBase> state, std::uint64_t id)
        : m_State(std::move(state)), m_Id(id)
    {
    }

    std::weak_ptr<Detail::EventStateBase> m_State;
    std::uint64_t m_Id = 0;
};

/**
 * @brief Multicast event: C#-delegate-style subscriber list with self-detaching
 *        handles.
 *
 * A plain value member of the publisher. The subscriber list lives behind a
 * `shared_ptr` and handles hold a `weak_ptr` to it, so a subscriber outliving
 * its publisher is safe by construction: unsubscribing after the event died is
 * an inert no-op, and teardown order stops being load-bearing. (Editor services
 * are destroyed during shutdown before the UI panels subscribed to them — that
 * ordering is the normal path here, not a misuse to guard against.)
 *
 * Unsubscribing does NOT join a callback already running: Invoke snapshots the
 * list under the lock and calls the copies outside it, on whichever thread
 * invoked. A subscriber that can be destroyed mid-callback needs its own
 * liveness flag as well.
 */
template <typename... Args>
class Event final
{
  public:
    using Callback = std::function<void(Args...)>;
    using Subscription = EventSubscription;

    Event() : m_State(std::make_shared<State>()) {}

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    /// Register a callback; the returned handle unsubscribes on destruction.
    /// An empty callable registers nothing and returns an inert handle.
    [[nodiscard]] EventSubscription Subscribe(Callback cb)
    {
        if (!cb)
            return {};
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        const std::uint64_t id = m_State->NextId++;
        m_State->Callbacks.emplace_back(id, std::move(cb));
        return EventSubscription(m_State, id);
    }

    /// Sugar for Subscribe: `auto sub = onChanged += [] { ... };`
    [[nodiscard]] EventSubscription operator+=(Callback cb) { return Subscribe(std::move(cb)); }

    /// Unsubscribe by resetting the handle. Handle-based, not callable-based:
    /// std::function is not equality-comparable, so C#-style `-= handler`
    /// (matching the callable) is impossible.
    void operator-=(EventSubscription& sub) { sub.Reset(); }

    /// Broadcast to all current subscribers. The list is copied under the lock
    /// and invoked outside it: a callback is free to subscribe, unsubscribe,
    /// or block.
    void Invoke(Args... args) const
    {
        // Local shared_ptr: the subscriber list stays alive for the whole
        // broadcast even if the Event (and its owner) is destroyed by a
        // callback or another thread mid-call.
        const std::shared_ptr<State> state = m_State;
        std::vector<Callback> callbacks;
        {
            std::lock_guard<std::mutex> lock(state->Mutex);
            callbacks.reserve(state->Callbacks.size());
            for (const auto& [id, callback] : state->Callbacks)
                callbacks.push_back(callback);
        }
        for (const Callback& callback : callbacks)
            callback(args...);
    }

  private:
    struct State final : Detail::EventStateBase
    {
        void Remove(std::uint64_t id) override
        {
            std::lock_guard<std::mutex> lock(Mutex);
            for (auto it = Callbacks.begin(); it != Callbacks.end(); ++it)
            {
                if (it->first == id)
                {
                    Callbacks.erase(it);
                    break;
                }
            }
        }

        mutable std::mutex Mutex;
        std::uint64_t NextId = 1;
        std::vector<std::pair<std::uint64_t, Callback>> Callbacks;
    };

    std::shared_ptr<State> m_State;
};

} // namespace GameEngine
