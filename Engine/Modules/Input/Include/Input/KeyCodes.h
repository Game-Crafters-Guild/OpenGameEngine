#pragma once

// Shared key-code and modifier helpers for the input system and UI.
//
// These helpers operate on raw platform modifier bits and key codes. For now we
// assume GLFW-compatible values (e.g. Ctrl = 0x0002, Super/Command = 0x0008,
// ASCII letter key codes for A/C/V/X), but the rest of the engine should treat
// them as opaque input-layer details.
//
// NOTE: Modifier mask constants (kModShift, kModControl, kModAlt, kModSuper,
// kModCapsLock, kModNumLock) are defined in InputSystem.h as the ModifierMask
// enum. Include that header if you need modifier masks.

namespace GameEngine {
namespace Input {

// ---------------------------------------------------------------------------
// Modifier masks - defined in InputSystem.h as ModifierMask enum
// Use: #include "Input/InputSystem.h" for kModShift, kModControl, etc.
// ---------------------------------------------------------------------------

// Returns true if the given modifier mask has the "primary" shortcut modifier
// pressed. On Windows/Linux this is Ctrl; on macOS this is Command (Super).
inline bool IsPrimaryShortcutModifier(int mods)
{
    constexpr int kModControl = 0x0002;
    constexpr int kModSuper   = 0x0008;
    return (mods & (kModControl | kModSuper)) != 0;
}

// ---------------------------------------------------------------------------
// Modifier keys (physical key codes)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_LeftShift    = 340; // GLFW_KEY_LEFT_SHIFT
inline constexpr int kKeyCode_LeftControl  = 341; // GLFW_KEY_LEFT_CONTROL
inline constexpr int kKeyCode_LeftAlt      = 342; // GLFW_KEY_LEFT_ALT
inline constexpr int kKeyCode_LeftSuper    = 343; // GLFW_KEY_LEFT_SUPER
inline constexpr int kKeyCode_RightShift   = 344; // GLFW_KEY_RIGHT_SHIFT
inline constexpr int kKeyCode_RightControl = 345; // GLFW_KEY_RIGHT_CONTROL
inline constexpr int kKeyCode_RightAlt     = 346; // GLFW_KEY_RIGHT_ALT
inline constexpr int kKeyCode_RightSuper   = 347; // GLFW_KEY_RIGHT_SUPER

// ---------------------------------------------------------------------------
// Letter keys (A-Z)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_A = 65;
inline constexpr int kKeyCode_B = 66;
inline constexpr int kKeyCode_C = 67;
inline constexpr int kKeyCode_D = 68;
inline constexpr int kKeyCode_E = 69;
inline constexpr int kKeyCode_F = 70;
inline constexpr int kKeyCode_G = 71;
inline constexpr int kKeyCode_H = 72;
inline constexpr int kKeyCode_I = 73;
inline constexpr int kKeyCode_J = 74;
inline constexpr int kKeyCode_K = 75;
inline constexpr int kKeyCode_L = 76;
inline constexpr int kKeyCode_M = 77;
inline constexpr int kKeyCode_N = 78;
inline constexpr int kKeyCode_O = 79;
inline constexpr int kKeyCode_P = 80;
inline constexpr int kKeyCode_Q = 81;
inline constexpr int kKeyCode_R = 82;
inline constexpr int kKeyCode_S = 83;
inline constexpr int kKeyCode_T = 84;
inline constexpr int kKeyCode_U = 85;
inline constexpr int kKeyCode_V = 86;
inline constexpr int kKeyCode_W = 87;
inline constexpr int kKeyCode_X = 88;
inline constexpr int kKeyCode_Y = 89;
inline constexpr int kKeyCode_Z = 90;

// ---------------------------------------------------------------------------
// Digit keys (0-9, top row)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_0 = 48;
inline constexpr int kKeyCode_1 = 49;
inline constexpr int kKeyCode_2 = 50;
inline constexpr int kKeyCode_3 = 51;
inline constexpr int kKeyCode_4 = 52;
inline constexpr int kKeyCode_5 = 53;
inline constexpr int kKeyCode_6 = 54;
inline constexpr int kKeyCode_7 = 55;
inline constexpr int kKeyCode_8 = 56;
inline constexpr int kKeyCode_9 = 57;

// ---------------------------------------------------------------------------
// Function keys (F1-F12)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_F1  = 290;
inline constexpr int kKeyCode_F2  = 291;
inline constexpr int kKeyCode_F3  = 292;
inline constexpr int kKeyCode_F4  = 293;
inline constexpr int kKeyCode_F5  = 294;
inline constexpr int kKeyCode_F6  = 295;
inline constexpr int kKeyCode_F7  = 296;
inline constexpr int kKeyCode_F8  = 297;
inline constexpr int kKeyCode_F9  = 298;
inline constexpr int kKeyCode_F10 = 299;
inline constexpr int kKeyCode_F11 = 300;
inline constexpr int kKeyCode_F12 = 301;

// ---------------------------------------------------------------------------
// Navigation keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_Up       = 265; // GLFW_KEY_UP
inline constexpr int kKeyCode_Down     = 264; // GLFW_KEY_DOWN
inline constexpr int kKeyCode_Left     = 263; // GLFW_KEY_LEFT
inline constexpr int kKeyCode_Right    = 262; // GLFW_KEY_RIGHT
inline constexpr int kKeyCode_Home     = 268; // GLFW_KEY_HOME
inline constexpr int kKeyCode_End      = 269; // GLFW_KEY_END
inline constexpr int kKeyCode_PageUp   = 266; // GLFW_KEY_PAGE_UP
inline constexpr int kKeyCode_PageDown = 267; // GLFW_KEY_PAGE_DOWN
inline constexpr int kKeyCode_Insert   = 260; // GLFW_KEY_INSERT

// ---------------------------------------------------------------------------
// Editing / control keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_Space       = 32;  // GLFW_KEY_SPACE
inline constexpr int kKeyCode_Escape      = 256; // GLFW_KEY_ESCAPE
inline constexpr int kKeyCode_Enter       = 257; // GLFW_KEY_ENTER
inline constexpr int kKeyCode_Tab         = 258; // GLFW_KEY_TAB
inline constexpr int kKeyCode_Backspace   = 259; // GLFW_KEY_BACKSPACE
inline constexpr int kKeyCode_Delete      = 261; // GLFW_KEY_DELETE
inline constexpr int kKeyCode_CapsLock    = 280; // GLFW_KEY_CAPS_LOCK
inline constexpr int kKeyCode_ScrollLock  = 281; // GLFW_KEY_SCROLL_LOCK
inline constexpr int kKeyCode_NumLock     = 282; // GLFW_KEY_NUM_LOCK
inline constexpr int kKeyCode_PrintScreen = 283; // GLFW_KEY_PRINT_SCREEN
inline constexpr int kKeyCode_Pause       = 284; // GLFW_KEY_PAUSE

// ---------------------------------------------------------------------------
// Punctuation / symbol keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_Apostrophe   = 39;  // GLFW_KEY_APOSTROPHE (')
inline constexpr int kKeyCode_Comma        = 44;  // GLFW_KEY_COMMA (,)
inline constexpr int kKeyCode_Minus        = 45;  // GLFW_KEY_MINUS (-)
inline constexpr int kKeyCode_Period       = 46;  // GLFW_KEY_PERIOD (.)
inline constexpr int kKeyCode_Slash        = 47;  // GLFW_KEY_SLASH (/)
inline constexpr int kKeyCode_Semicolon    = 59;  // GLFW_KEY_SEMICOLON (;)
inline constexpr int kKeyCode_Equal        = 61;  // GLFW_KEY_EQUAL (=)
inline constexpr int kKeyCode_LeftBracket  = 91;  // GLFW_KEY_LEFT_BRACKET ([)
inline constexpr int kKeyCode_Backslash    = 92;  // GLFW_KEY_BACKSLASH (\)
inline constexpr int kKeyCode_RightBracket = 93;  // GLFW_KEY_RIGHT_BRACKET (])
inline constexpr int kKeyCode_GraveAccent  = 96;  // GLFW_KEY_GRAVE_ACCENT (`)

// The two extra printing keys non-US layouts carry beyond the ASCII block
// above (GLFW names them for the position, not the character, because which
// character they produce is the layout's business). They sit outside the
// contiguous ASCII range, so every classifier has to name them.
inline constexpr int kKeyCode_World1 = 161; // GLFW_KEY_WORLD_1
inline constexpr int kKeyCode_World2 = 162; // GLFW_KEY_WORLD_2

// ---------------------------------------------------------------------------
// Numpad keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_NumPad0        = 320; // GLFW_KEY_KP_0
inline constexpr int kKeyCode_NumPad1        = 321; // GLFW_KEY_KP_1
inline constexpr int kKeyCode_NumPad2        = 322; // GLFW_KEY_KP_2
inline constexpr int kKeyCode_NumPad3        = 323; // GLFW_KEY_KP_3
inline constexpr int kKeyCode_NumPad4        = 324; // GLFW_KEY_KP_4
inline constexpr int kKeyCode_NumPad5        = 325; // GLFW_KEY_KP_5
inline constexpr int kKeyCode_NumPad6        = 326; // GLFW_KEY_KP_6
inline constexpr int kKeyCode_NumPad7        = 327; // GLFW_KEY_KP_7
inline constexpr int kKeyCode_NumPad8        = 328; // GLFW_KEY_KP_8
inline constexpr int kKeyCode_NumPad9        = 329; // GLFW_KEY_KP_9
inline constexpr int kKeyCode_NumPadDecimal  = 330; // GLFW_KEY_KP_DECIMAL
inline constexpr int kKeyCode_NumPadDivide   = 331; // GLFW_KEY_KP_DIVIDE
inline constexpr int kKeyCode_NumPadMultiply = 332; // GLFW_KEY_KP_MULTIPLY
inline constexpr int kKeyCode_NumPadSubtract = 333; // GLFW_KEY_KP_SUBTRACT
inline constexpr int kKeyCode_NumPadAdd      = 334; // GLFW_KEY_KP_ADD
inline constexpr int kKeyCode_NumPadEnter    = 335; // GLFW_KEY_KP_ENTER
inline constexpr int kKeyCode_NumPadEqual    = 336; // GLFW_KEY_KP_EQUAL

// ---------------------------------------------------------------------------
// Key classification
// ---------------------------------------------------------------------------

// A modifier's own key-down, as opposed to the modifier bits carried by some
// other key's event.
inline constexpr bool IsModifierKey(int key)
{
    return key == kKeyCode_LeftShift || key == kKeyCode_RightShift ||
           key == kKeyCode_LeftControl || key == kKeyCode_RightControl ||
           key == kKeyCode_LeftAlt || key == kKeyCode_RightAlt ||
           key == kKeyCode_LeftSuper || key == kKeyCode_RightSuper;
}

inline constexpr bool IsFunctionKey(int key)
{
    return key >= kKeyCode_F1 && key <= kKeyCode_F12;
}

// Keys the platform also delivers as a character event. A text control claims
// such a keystroke because the character it is about to receive is the thing it
// acts on — the key event and its character are one keystroke, and consuming
// only the character would leave the key itself free to reach gameplay. The
// ASCII-valued GLFW codes from space to backtick cover letters, digits and
// punctuation; the keypad block adds its digits and operators; World1/World2
// are the same thing for the keys only non-US layouts have, and missing them
// would leak that keystroke to gameplay while the field still edited on it.
inline constexpr bool ProducesTextInput(int key)
{
    if (key >= kKeyCode_Space && key <= kKeyCode_GraveAccent)
        return true;
    if (key == kKeyCode_World1 || key == kKeyCode_World2)
        return true;
    return key >= kKeyCode_NumPad0 && key <= kKeyCode_NumPadEqual && key != kKeyCode_NumPadEnter;
}

// The keystroke-level form of the above: true when this key event, under these
// modifiers, is about to arrive as a character event — the condition under
// which a focused text control claims the key. A primary-modifier chord never
// composes. Alt is the platform split: Alt composes characters on macOS
// (Option+F types 'ƒ'), so the keystroke is text and stays with the control;
// on Windows and Linux Alt+letter is a chord that composes nothing, so it must
// bubble to the binding that implements it.
inline bool ComposesTextInput(int key, int mods)
{
    if (!ProducesTextInput(key) || IsPrimaryShortcutModifier(mods))
        return false;
#if defined(__APPLE__)
    return true;
#else
    constexpr int kModAlt = 0x0004;
    return (mods & kModAlt) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Mouse buttons
// ---------------------------------------------------------------------------
inline constexpr int kMouseButton_Left   = 0; // GLFW_MOUSE_BUTTON_LEFT
inline constexpr int kMouseButton_Right  = 1; // GLFW_MOUSE_BUTTON_RIGHT
inline constexpr int kMouseButton_Middle = 2; // GLFW_MOUSE_BUTTON_MIDDLE

} // namespace Input
} // namespace GameEngine

