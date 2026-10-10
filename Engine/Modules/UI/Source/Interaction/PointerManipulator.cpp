#include "UI/Interaction/PointerManipulator.h"

#include "UI/UIEvents.h"

#include <utility>

namespace GameEngine
{

namespace
{
// The class Button drives for a left press. Driving the same one keeps a press on a
// manipulated control looking exactly as it did; on an element whose stylesheet says
// nothing about it, it is inert.
constexpr const char* kPressedClass = "pressed";
} // namespace

void PointerManipulator::AddActivationFilter(const ManipulatorActivationFilter& filter)
{
    m_Activators.push_back(filter);
}

void PointerManipulator::RegisterCallbacksOnTarget(UIElement& target)
{
    auto self = std::static_pointer_cast<PointerManipulator>(shared_from_this());

    SubscribeStamped(target, kEventMouseDown, [self](UIEvent& e) { self->OnPointerDown(e); });
    SubscribeStamped(target, kEventMouseUp, [self](UIEvent& e) { self->OnPointerUp(e); });

    // The press ended without a release because the pointer stream itself did. Disarm;
    // activate nothing. Left unstopped, like every other cancel subscriber — ancestors
    // tracking the same gesture have to be cancelled too.
    SubscribeStamped(target, kEventMouseCancel,
                     [self](UIEvent& e)
                     {
                         if (e.CurrentTarget)
                             self->Disarm(*e.CurrentTarget);
                     });
}

void PointerManipulator::OnTargetDetached(UIElement& target)
{
    // A press that can no longer be released on anything. Drop it, so a later re-attach does
    // not resume a gesture the user finished with long ago.
    Disarm(target);
}

bool PointerManipulator::Activates(const UIEvent& e) const
{
    for (const ManipulatorActivationFilter& filter : m_Activators)
    {
        if (filter.Button == e.Button && filter.Modifiers == e.Mods)
            return true;
    }
    return false;
}

bool PointerManipulator::IsInside(const UIElement& target, float x, float y)
{
    const float left = target.GetLayoutX();
    const float top = target.GetLayoutY();
    return x >= left && y >= top && x < (left + target.GetLayoutWidth())
           && y < (top + target.GetLayoutHeight());
}

void PointerManipulator::OnPointerDown(UIEvent& e)
{
    if (m_Armed || !e.CurrentTarget || !Activates(e))
        return;

    UIElement& target = *e.CurrentTarget;
    m_Armed = true;
    m_ArmedButton = e.Button;
    target.AddClass(kPressedClass);

    if (!m_MoveToken)
    {
        auto self = std::static_pointer_cast<PointerManipulator>(shared_from_this());
        m_MoveToken = SubscribeStamped(target, kEventMouseMove,
                                       [self](UIEvent& move) { self->OnPointerMove(move); });
    }

    // The release must come back here even if the pointer has left, because "was it released
    // inside" is a question only this element can answer.
    e.Capture(&target);
    e.Stop();
}

void PointerManipulator::OnPointerMove(UIEvent& e)
{
    if (!m_Armed || !e.CurrentTarget)
        return;

    UIElement& target = *e.CurrentTarget;
    const bool inside = IsInside(target, e.X, e.Y);
    if (inside && !target.HasClass(kPressedClass))
        target.AddClass(kPressedClass);
    else if (!inside && target.HasClass(kPressedClass))
        target.RemoveClass(kPressedClass);

    e.Stop();
}

void PointerManipulator::OnPointerUp(UIEvent& e)
{
    if (!m_Armed || e.Button != m_ArmedButton || !e.CurrentTarget)
        return;

    UIElement& target = *e.CurrentTarget;
    // Read the position before Disarm, which unsubscribes out from under us.
    const float x = e.X;
    const float y = e.Y;
    const bool inside = IsInside(target, x, y);
    Disarm(target);

    // Claimed either way: the press was ours, so the release that ends it is too, whether or
    // not it activates anything.
    e.Stop();

    if (inside)
        OnActivated(target, x, y);
}

void PointerManipulator::Disarm(UIElement& target)
{
    if (!m_Armed)
        return;

    m_Armed = false;
    m_ArmedButton = -1;
    target.RemoveClass(kPressedClass);
    Unsubscribe(target, m_MoveToken);
}

} // namespace GameEngine
