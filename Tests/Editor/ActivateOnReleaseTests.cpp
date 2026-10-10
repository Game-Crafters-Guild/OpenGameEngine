// The activation gesture the diagnostics strip's plain lines borrow from
// Button: press arms, release inside activates, a cancelled or outside-ended
// press activates nothing, and Enter/Space activate a focused line.
// Mirrors Engine/Modules/UI/Tests/ButtonTests.cpp's event-driving pattern.

#include <gtest/gtest.h>

#include "Input/KeyCodes.h"
#include "UI/ActivateOnRelease.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/ModuleOwnedHandlers.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <cstdint>
#include <functional>

using namespace GameEngine;

namespace
{
void Send(UIElement& el, EventId id, float x, float y)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

void SendKey(UIElement& el, int key)
{
    UIEvent e{};
    e.Id = kEventKeyDown;
    e.Key = key;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}
} // namespace

TEST(ActivateOnReleaseTests, ActivatesOnReleaseInsideAndNotOnPressAlone)
{
    UIElement line;
    UILayoutAccess::SetLastLayoutRect(line, 10, 10, 100, 22);

    int activations = 0;
    EditorUI::ActivateOnRelease(line, [&]() { ++activations; });

    /* A press arms and shows it, but must not activate: activating on the
       press is what this replaced, and it is what every other control in the
       engine does not do. */
    Send(line, kEventMouseDown, 20, 20);
    EXPECT_TRUE(line.HasClass("pressed"));
    EXPECT_EQ(activations, 0);

    Send(line, kEventMouseUp, 20, 20);
    EXPECT_EQ(activations, 1);
    EXPECT_FALSE(line.HasClass("pressed"));
}

TEST(ActivateOnReleaseTests, APressThatEndsOutsideOrIsCancelledActivatesNothing)
{
    UIElement line;
    UILayoutAccess::SetLastLayoutRect(line, 10, 10, 100, 22);

    int activations = 0;
    EditorUI::ActivateOnRelease(line, [&]() { ++activations; });

    // Press, drag off, release outside.
    Send(line, kEventMouseDown, 20, 20);
    Send(line, kEventMouseMove, 1000, 1000);
    EXPECT_FALSE(line.HasClass("pressed")) << "the visual tracks the pointer while armed";
    Send(line, kEventMouseUp, 1000, 1000);
    EXPECT_EQ(activations, 0);

    // Press, then the gesture is cancelled outright.
    Send(line, kEventMouseDown, 20, 20);
    EXPECT_TRUE(line.HasClass("pressed"));
    Send(line, kEventMouseCancel, 20, 20);
    EXPECT_FALSE(line.HasClass("pressed"));
    EXPECT_EQ(activations, 0);

    /* Disarmed: a release arriving after the cancel is not the tail of a live
       gesture and must not activate. */
    Send(line, kEventMouseUp, 20, 20);
    EXPECT_EQ(activations, 0);
}

TEST(ActivateOnReleaseTests, LineIsFocusableAndActivatesFromTheKeyboard)
{
    UIElement line;
    UILayoutAccess::SetLastLayoutRect(line, 0, 0, 100, 22);

    int activations = 0;
    EditorUI::ActivateOnRelease(line, [&]() { ++activations; });

    /* A plain UIElement is not focusable by default, so keyboard activation is
       only reachable because the gesture opts in. */
    EXPECT_TRUE(line.IsFocusable());

    SendKey(line, Input::kKeyCode_Enter);
    EXPECT_EQ(activations, 1);
    SendKey(line, Input::kKeyCode_Space);
    EXPECT_EQ(activations, 2);
    SendKey(line, Input::kKeyCode_A);
    EXPECT_EQ(activations, 2) << "only Enter and Space activate";
}

// Revocation reaches BOTH halves of the gesture.
//
// The pointer half goes through the manipulator, which attributes every
// subscription to the image owning the caller's callable. The keyboard half is
// registered directly, so it has to be attributed by hand — and an entry that
// is not attributed is not revoked, which for a hot-swappable package caller
// means an unloaded module's callable still sitting in the handler table.
//
// The image is synthetic: publishing the one-byte range around the callable's
// own type_info makes exactly this callable "module-owned" and leaves every
// engine handler unattributed, which is the same split a real module load
// produces without needing one. Portable, unlike reading a real image range.
TEST(ActivateOnReleaseTests, UnloadRevokesTheKeyboardHandlerTooNotJustThePointerOnes)
{
    UIElement line;
    UILayoutAccess::SetLastLayoutRect(line, 0, 0, 100, 22);

    int activations = 0;
    const std::function<void()> activate = [&]() { ++activations; };
    const auto owner = reinterpret_cast<std::uint64_t>(&activate.target_type());

    UI::OpenImageAttribution();
    UI::CloseImageAttribution(owner, 1);

    EditorUI::ActivateOnRelease(line, activate);

    // Both halves work while the image is mapped.
    Send(line, kEventMouseDown, 20, 10);
    Send(line, kEventMouseUp, 20, 10);
    SendKey(line, Input::kKeyCode_Enter);
    ASSERT_EQ(activations, 2);

    EXPECT_GT(UI::RevokeHandlersOwnedByImage(owner, 1), 0u);
    UI::RetractHotSwappableImage(owner);

    Send(line, kEventMouseDown, 20, 10);
    Send(line, kEventMouseUp, 20, 10);
    EXPECT_EQ(activations, 2) << "the pointer half was already revoked before this change";
    SendKey(line, Input::kKeyCode_Enter);
    EXPECT_EQ(activations, 2) << "the keyboard half must be revoked with it";
}
