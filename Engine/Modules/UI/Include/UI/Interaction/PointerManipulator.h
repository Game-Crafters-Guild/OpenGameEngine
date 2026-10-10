#pragma once

#include <vector>

#include "UI/Interaction/Manipulator.h"

namespace GameEngine
{

// Which press starts the gesture — UI Toolkit's ManipulatorActivationFilter, adopted as-is
// minus its clickCount field, which nothing here needs yet.
//
// Modifiers is an EXACT match against the event's bitmask, not a subset test: a filter for
// plain right-click does not fire on Ctrl+right-click, so a second filter can claim that
// combination without the two overlapping. That is Unity's rule and the reason a list of
// filters is more useful than one button index.
struct ManipulatorActivationFilter
{
    int Button = 0;    // UIEvent::Button index
    int Modifiers = 0; // Input::kMod* bitmask, matched exactly
};

// The press-arm-release gesture, once, for every manipulator that needs it.
//
// ARMING IS THE WHOLE POINT. A press that passes an activation filter arms the gesture and
// takes the pointer capture; the release fires OnActivated only if it lands inside the
// target. A release whose press began elsewhere activates nothing — which a bare
// "on pointer up" handler cannot tell apart, and which is why several hand-rolled
// right-click handlers in this editor open menus they should not.
//
// The gesture is claimed (UIEvent::Stop) only once ARMED, so an element carrying a
// manipulator that does not match the press leaves the event alone and it bubbles to an
// ancestor that wants it.
class PointerManipulator : public Manipulator
{
  protected:
    PointerManipulator() = default;

    // Adds a press that starts this gesture. A manipulator with no filter never arms.
    void AddActivationFilter(const ManipulatorActivationFilter& filter);

    // An armed press was released inside the target. x/y are the release position, in the
    // same UI space as UIElement::GetLayoutX/Y.
    virtual void OnActivated(UIElement& target, float x, float y) = 0;

  private:
    void RegisterCallbacksOnTarget(UIElement& target) final;
    void OnTargetDetached(UIElement& target) final;

    void OnPointerDown(UIEvent& e);
    void OnPointerMove(UIEvent& e);
    void OnPointerUp(UIEvent& e);
    void Disarm(UIElement& target);

    bool Activates(const UIEvent& e) const;

    // Half-open on the far edges, matching Button::IsPointInside. UIElement::ContainsPoint is
    // closed and would count the pixel one past the element's right/bottom edge as inside —
    // which for a context menu means the neighbouring control's first pixel opens this
    // element's menu.
    static bool IsInside(const UIElement& target, float x, float y);

    // One entry for nearly every manipulator; a vector because Unity's list shape earns its
    // keep the moment a gesture wants two (plain and modified) presses. Costs one allocation
    // per filter added, at attach time, never per event.
    std::vector<ManipulatorActivationFilter> m_Activators;

    // Subscribed only while armed: an unarmed manipulator costs nothing on the pointer-move
    // path, which is most of them, most of the time. It exists purely to track the pressed
    // visual as the pointer crosses the target's edge mid-press.
    UIElement::EventHandlerToken m_MoveToken;

    int m_ArmedButton = -1;
    bool m_Armed = false;
};

} // namespace GameEngine
