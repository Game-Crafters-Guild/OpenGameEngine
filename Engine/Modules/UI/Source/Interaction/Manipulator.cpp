#include "UI/Interaction/Manipulator.h"

#include "UI/UIEvents.h"

#include <algorithm>

namespace GameEngine
{

void Manipulator::Attach(const std::shared_ptr<Manipulator>& self, UIElement& element)
{
    if (!self)
        return;

    // Marked before the first subscription: registering runs caller code, which is free to
    // reach back into AddManipulator, and it has to see this one as taken by then.
    self->m_Attached = true;

    // Subscribed before the manipulator's own events so a detach can never find a
    // half-registered manipulator.
    self->SubscribeStamped(element, kEventDetachedFromPanel,
                           [self](UIEvent& e)
                           {
                               if (e.CurrentTarget)
                                   self->OnTargetDetached(*e.CurrentTarget);
                           });

    // Deliberately no kEventAttachedToPanel subscription: these handlers live in the
    // element's own table, so they follow it out of a panel and back with nothing to
    // re-arm, and a handler that could only ever no-op is not worth its dispatch.
    self->RegisterCallbacksOnTarget(element);
}

void Manipulator::Unsubscribe(UIElement& target, UIElement::EventHandlerToken& token)
{
    if (!token)
        return;

    target.UnregisterEventHandler(token);
    m_Tokens.erase(std::remove_if(m_Tokens.begin(), m_Tokens.end(),
                                  [&token](const UIElement::EventHandlerToken& t)
                                  { return t.Id == token.Id && t.Key == token.Key; }),
                   m_Tokens.end());
    token = {};
}

void Manipulator::DetachFromTarget(UIElement& target)
{
    // THE SELF-REFERENCE. The subscriptions below hold the only strong references to this
    // manipulator, so unregistering the last one destroys it — mid-loop, with `this` and
    // m_Tokens still being read. Holding one reference across the whole teardown is what makes
    // the last unregister safe; the manipulator dies when this function returns instead.
    const std::shared_ptr<Manipulator> keepAlive = shared_from_this();

    // Disarm BEFORE unsubscribing: an armed gesture holds the pressed visual on the element
    // and an extra subscription, and the element outlives this call. Removing a manipulator
    // mid-press would otherwise leave the control looking held down forever.
    OnTargetDetached(target);

    // Copied, because unregistering runs the handlers' captures' destructors, and those are
    // caller code that is free to touch this element — including adding a manipulator, which
    // would reallocate the vector being walked.
    //
    // Both records are surrendered here rather than after the loop, so a capture destructor
    // that re-attaches this manipulator is allowed to: handler keys are never reused, so the
    // fresh tokens it records cannot collide with the ones still being unregistered.
    const std::vector<UIElement::EventHandlerToken> tokens = m_Tokens;
    m_Tokens.clear();
    m_Attached = false;
    for (const UIElement::EventHandlerToken& token : tokens)
        target.UnregisterEventHandler(token);
}

} // namespace GameEngine
