#pragma once

#include <functional>
#include <memory>
#include <utility>

namespace GameEngine
{

// Move-only RAII handle for a registration with a broker (an event
// subscription, a draw-producer registration, ...). Destruction unregisters;
// a broker that died first makes destruction a safe no-op.
//
// The safety mechanism is the keepalive: brokers keep their subscriber state
// in a shared_ptr-owned control block and hand out a weak_ptr to it. The
// unsubscribe closure runs only while the control block can still be locked,
// with a strong reference held for the duration of the call, so the closure
// may freely touch the state the keepalive owns. A raw broker capture is unsafe:
// the broker itself may already be mid-destruction even while that state lives.
// A broker identity can instead be resolved under an independent lifetime lock
// held for the ENTIRE unsubscribe operation. Destruction must remove the broker
// from that registry under the same lock BEFORE touching its members (as ECS
// World does). The weak state alone does not provide this stronger protection.
//
// This replaces the hand-rolled trio every subscriber used to carry (raw
// broker pointer + opaque token + broker lifetime flag, checked in a manual
// destructor): a subscription cannot be leaked, double-freed, or forgotten,
// and destruction order between broker and subscriber stops mattering.
class ScopedSubscription
{
  public:
    ScopedSubscription() = default;

    ScopedSubscription(std::weak_ptr<void> keepalive, std::function<void()> unsubscribe)
        : m_Keepalive(std::move(keepalive)), m_Unsubscribe(std::move(unsubscribe))
    {
    }

    ScopedSubscription(ScopedSubscription&& other) noexcept
        : m_Keepalive(std::move(other.m_Keepalive)), m_Unsubscribe(std::move(other.m_Unsubscribe))
    {
        other.m_Keepalive.reset();
        other.m_Unsubscribe = nullptr;
    }

    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_Keepalive = std::move(other.m_Keepalive);
            m_Unsubscribe = std::move(other.m_Unsubscribe);
            other.m_Keepalive.reset();
            other.m_Unsubscribe = nullptr;
        }
        return *this;
    }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    ~ScopedSubscription() { Reset(); }

    // Unregisters now (if the broker's state still exists) and empties the
    // handle. Idempotent.
    void Reset()
    {
        if (m_Unsubscribe)
        {
            if (const std::shared_ptr<void> alive = m_Keepalive.lock())
                m_Unsubscribe();
            m_Unsubscribe = nullptr;
        }
        m_Keepalive.reset();
    }

    // True while this handle holds a registration (whether or not the broker
    // still lives — a dead broker just makes the eventual Reset a no-op).
    explicit operator bool() const { return static_cast<bool>(m_Unsubscribe); }

  private:
    std::weak_ptr<void> m_Keepalive;
    std::function<void()> m_Unsubscribe;
};

}  // namespace GameEngine
