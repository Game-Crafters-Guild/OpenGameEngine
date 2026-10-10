#pragma once

// Test-only driver for UIManager's focus notification.
//
// FocusIn / FocusOut are announced by UIManager::NotifyFocusChange, which Update reaches
// only with a device; without one Update returns before it. Friended by UIManager (see the
// `friend struct UIManagerFocusAccess` declaration there) so a CPU-only suite can announce
// a focus change at the point a frame would.

#include "UI/UIManager.h"

namespace GameEngine
{

struct UIManagerFocusAccess
{
    static void AnnounceFocusChange(UIManager& ui) { ui.NotifyFocusChange(); }
};

} // namespace GameEngine
