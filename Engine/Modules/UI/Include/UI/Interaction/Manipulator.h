#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "UI/UIElement.h"

namespace GameEngine
{

// A reusable behavior attached to an element — UI Toolkit's Manipulator, and the analogy
// is close enough to keep the name: one way to reuse a behavior across elements.
//
// A manipulator subscribes to its target's events, keeps whatever state its gesture needs,
// and calls back when the gesture completes. The element knows nothing about it.
//
// WHAT THE BASE OWNS, and the reason it exists rather than each manipulator repeating it:
//
//   * LIFETIME. The target's handler table owns the manipulator: its own subscriptions hold
//     the only STRONG references, so it is destroyed exactly when they are — when the element
//     dies, when it is removed, or when a user module that supplied the callback is unloaded
//     and its handlers are revoked. The element's own manipulator list is weak and owns
//     nothing, which is what keeps that last case total. Half a deviation from UI Toolkit:
//     Unity's element keeps a manipulator list (this one does too, weakly, so
//     Add/RemoveManipulator can exist) but its manipulator also keeps a `target`
//     back-pointer, which is not here — see the ownership section of
//     context-menu-manipulator-design.html for the trade.
//
//   * NO BACK-POINTER. Every handler is handed its element as UIEvent::CurrentTarget, so a
//     manipulator never stores one and cannot dangle. This is what makes destruction during
//     revocation and destruction during ~UIElement equally safe.
//
//   * STAMPING. Handlers registered here are Engine.dll lambdas wrapping a caller's
//     callable, so the ownership stamp read off the outer lambda would say Engine.dll and
//     revoke nothing. Every subscription goes through SubscribeStamped, which attributes the
//     entry to the image owning the CALLER's callable (UIElement::AdoptHandlerOwnerFrom).
//     A manipulator that registers a handler any other way leaks its module's code past the
//     unmap; there is deliberately no second path.
//
//   * DETACH. The base subscribes kEventDetachedFromPanel itself, so every manipulator gets
//     OnTargetDetached without asking. That event also fires synchronously on the
//     pre-destruction detach, where the element is still whole — which is what makes it safe
//     to touch the target's handler table from there.
//
//   * ONE ELEMENT AT A TIME. An instance serves one element until it is removed from it, and
//     AddManipulator refuses a second attach rather than accepting it. The subscription
//     records below are the reason — see m_Tokens. Build one instance per element; the reuse
//     the name promises is of the TYPE, not of a single object.
class Manipulator : public std::enable_shared_from_this<Manipulator>
{
  public:
    virtual ~Manipulator() = default;

    Manipulator(const Manipulator&) = delete;
    Manipulator& operator=(const Manipulator&) = delete;

  protected:
    Manipulator() = default;

    // Registers, attributes and records in one step. `ownerProbe` is the caller-supplied
    // callable the manipulator was built with — the thing whose module must not outlive the
    // subscription.
    //
    // The recording is what makes removal possible: a manipulator that hands out
    // subscriptions and forgets them has no way to take them back.
    template <class R, class... A>
    UIElement::EventHandlerToken Subscribe(UIElement& target, EventId id,
                                           UIElement::EventHandler handler,
                                           const std::function<R(A...)>& ownerProbe)
    {
        const UIElement::EventHandlerToken token =
            target.RegisterEventHandler(id, std::move(handler));
        target.AdoptHandlerOwnerFrom(token, ownerProbe);
        m_Tokens.push_back(token);
        return token;
    }

    // Drops one subscription and the record of it, for the handlers a gesture keeps only
    // while it is armed. Clears `token`.
    void Unsubscribe(UIElement& target, UIElement::EventHandlerToken& token);

    // The one subscription path, so an intermediate class (PointerManipulator) can register
    // handlers without knowing the concrete manipulator's callback type. A leaf implements it
    // by forwarding to Subscribe with its own callable as the probe.
    virtual UIElement::EventHandlerToken SubscribeStamped(UIElement& target, EventId id,
                                                          UIElement::EventHandler handler) = 0;

    // Subscribe the events this manipulator needs. Called once, from Attach.
    virtual void RegisterCallbacksOnTarget(UIElement& target) = 0;

    // The target left its panel — including the synchronous announcement made just before it
    // is destroyed. A manipulator holding in-flight gesture state drops it here.
    //
    // Disarm only, never unsubscribe: the subscriptions live in the element's own table and
    // travel with it, so an element that leaves a panel and comes back has a working
    // manipulator with nothing to re-register.
    virtual void OnTargetDetached(UIElement& target) = 0;

  private:
    // Attachment and removal are UIElement's to drive — AddManipulator is the only way in, so
    // there is deliberately no second path that could skip the double-attach check or the
    // weak entry.
    friend class UIElement;

    // Subscribes `self` to its target's lifecycle and then lets it register its own events.
    // Called with the only shared_ptr in existence; from here on the handler table holds it.
    static void Attach(const std::shared_ptr<Manipulator>& self, UIElement& element);

    // The full teardown: disarm, then unregister every subscription this manipulator holds on
    // `target`. Not the same thing as OnTargetDetached, which is the element leaving a panel
    // and keeps the subscriptions.
    void DetachFromTarget(UIElement& target);

    // Every subscription made on the target, in registration order. On the manipulator rather
    // than the element because they are the manipulator's to release — and because an element
    // carrying two manipulators must be able to remove one without touching the other's.
    //
    // THE ONE-ELEMENT RULE LIVES HERE: a record names no element, and EventHandlerToken::Key
    // is a per-element counter, so tokens minted on two elements are both indistinguishable
    // and liable to collide. Detaching from the second would unregister the first's handlers —
    // or, on a key collision, a stranger's.
    std::vector<UIElement::EventHandlerToken> m_Tokens;

    // Attached to an element right now: set by Attach, cleared by DetachFromTarget, and read
    // by AddManipulator to refuse the second attach. Deliberately untouched by
    // OnTargetDetached, which is the element leaving a panel and keeps every subscription.
    // Only RemoveManipulator clears it, so an instance whose element was DESTROYED stays
    // marked attached and cannot be reused — build a new one for the next element.
    bool m_Attached = false;
};

} // namespace GameEngine
