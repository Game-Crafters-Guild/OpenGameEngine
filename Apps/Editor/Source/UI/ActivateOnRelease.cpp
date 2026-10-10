#include "UI/ActivateOnRelease.h"

#include "Input/KeyCodes.h"
#include "UI/Interaction/PointerManipulator.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <memory>
#include <utility>

namespace GameEngine {
namespace EditorUI {

namespace
{
// The left-press-to-activate half of the gesture. Everything that makes it a
// gesture rather than an up-handler — the arming, the capture, the pressed
// visual, the cancel — is PointerManipulator's.
class ActivateOnReleaseManipulator final : public PointerManipulator
{
public:
    static std::shared_ptr<ActivateOnReleaseManipulator> Create(std::function<void()> activate)
    {
        if (!activate)
            return nullptr;
        auto manipulator =
            std::shared_ptr<ActivateOnReleaseManipulator>(new ActivateOnReleaseManipulator(std::move(activate)));
        manipulator->AddActivationFilter({/*Button=*/0, /*Modifiers=*/0});
        return manipulator;
    }

private:
    explicit ActivateOnReleaseManipulator(std::function<void()> activate)
        : m_Activate(std::move(activate))
    {
    }

    /* The activate callable is what must not outlive the subscription, so it is
       what every entry is attributed to for hot-reload revocation. */
    UIElement::EventHandlerToken SubscribeStamped(UIElement& target, EventId id,
                                                  UIElement::EventHandler handler) override
    {
        return Subscribe(target, id, std::move(handler), m_Activate);
    }

    void OnActivated(UIElement&, float, float) override { m_Activate(); }

    std::function<void()> m_Activate;
};
} // namespace

void ActivateOnRelease(UIElement& element, std::function<void()> activate)
{
    if (!activate)
        return;

    element.AddManipulator(ActivateOnReleaseManipulator::Create(activate));

    // A plain element is not focusable, so without this the line would be
    // mouse-only — the keyboard path a Button gets for free.
    element.SetFocusable(true);
    const UIElement::EventHandlerToken keyToken =
        element.RegisterEventHandler(kEventKeyDown, [activate](UIEvent& ev)
        {
            if (ev.Key == Input::kKeyCode_Space || ev.Key == Input::kKeyCode_Enter)
            {
                activate();
                ev.Stop();
            }
        });
    // The keyboard half is registered here rather than through the manipulator,
    // so it needs the same attribution the manipulator gives its own handlers:
    // the entry holds a copy of `activate`, and an unload that revoked only the
    // pointer subscriptions would leave this one calling into an unmapped image.
    element.AdoptHandlerOwnerFrom(keyToken, activate);
}

} // namespace EditorUI
} // namespace GameEngine
