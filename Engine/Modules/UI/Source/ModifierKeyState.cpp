#include "UI/ModifierKeyState.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

namespace GameEngine
{
namespace UI
{
void ModifierKeyState::ApplyKeyEvent(int key, bool released, int mods)
{
    // A modifier key event settles that modifier's physical state, so its mask
    // bit is ignored below: some platforms report the pre-release mask on a
    // release callback, which would otherwise leave the modifier stuck held.
    const bool keyHeld = !released;
    int ignoreMask = 0;
    switch (key)
    {
        case Input::kKeyCode_LeftShift:    m_LeftShift = keyHeld;  ignoreMask = Input::kModShift; break;
        case Input::kKeyCode_RightShift:   m_RightShift = keyHeld; ignoreMask = Input::kModShift; break;
        case Input::kKeyCode_LeftAlt:      m_LeftAlt = keyHeld;    ignoreMask = Input::kModAlt; break;
        case Input::kKeyCode_RightAlt:     m_RightAlt = keyHeld;   ignoreMask = Input::kModAlt; break;
        case Input::kKeyCode_LeftControl:  m_LeftCtrl = keyHeld;   ignoreMask = Input::kModControl; break;
        case Input::kKeyCode_RightControl: m_RightCtrl = keyHeld;  ignoreMask = Input::kModControl; break;
        case Input::kKeyCode_LeftSuper:    m_LeftSuper = keyHeld;  ignoreMask = Input::kModSuper; break;
        case Input::kKeyCode_RightSuper:   m_RightSuper = keyHeld; ignoreMask = Input::kModSuper; break;
        default: break;
    }

    Derive(mods, ignoreMask);
}

void ModifierKeyState::ApplyLiveMask(int mods)
{
    // The live mask cannot tell the two sides apart: a modifier it reports
    // stays on whichever side already held it and defaults to the left side;
    // one it omits releases both sides so the tracked state matches reality.
    auto settle = [](bool& left, bool& right, bool held)
    {
        if (!held)
        {
            left = false;
            right = false;
        }
        else if (!left && !right)
        {
            left = true;
        }
    };
    settle(m_LeftShift, m_RightShift, (mods & Input::kModShift) != 0);
    settle(m_LeftAlt, m_RightAlt, (mods & Input::kModAlt) != 0);
    settle(m_LeftCtrl, m_RightCtrl, (mods & Input::kModControl) != 0);
    settle(m_LeftSuper, m_RightSuper, (mods & Input::kModSuper) != 0);
    Derive(mods, 0);
}

void ModifierKeyState::Derive(int mods, int ignoreMask)
{
    const int effective = mods & ~ignoreMask;
    m_Shift = (m_LeftShift || m_RightShift) || (effective & Input::kModShift) != 0;
    m_Alt = (m_LeftAlt || m_RightAlt) || (effective & Input::kModAlt) != 0;
    m_Ctrl = (m_LeftCtrl || m_RightCtrl) || (effective & Input::kModControl) != 0;
    m_Super = (m_LeftSuper || m_RightSuper) || (effective & Input::kModSuper) != 0;
    m_PrimaryModifier = Input::IsPrimaryShortcutModifier(Mask());
}

int ModifierKeyState::Mask() const
{
    int mods = 0;
    if (m_Shift)
        mods |= Input::kModShift;
    if (m_Alt)
        mods |= Input::kModAlt;
    if (m_Ctrl)
        mods |= Input::kModControl;
    if (m_Super)
        mods |= Input::kModSuper;
    return mods;
}
} // namespace UI
} // namespace GameEngine
