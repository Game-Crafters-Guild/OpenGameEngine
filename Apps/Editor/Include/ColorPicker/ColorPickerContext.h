#pragma once

#include <cstdint>
#include <functional>

namespace GameEngine
{
class UIManager;
namespace Platform { class Window; }

// Color picker-specific state for an editor window.
// Separated from EditorWindowContext to avoid bloating that struct with color picker details.
// All color picker visuals (SV gradient, hue bar, and eventually the color wheel) are now
// rendered as shader gradients -- no external textures are needed.
struct ColorPickerContext
{
    // Callbacks for the color picker dialog.
    // argb is the clamped 0-255 color; intensity is the HDR multiplier (>= 1).
    std::function<void(uint32_t argb, float intensity)> onApply;
    std::function<void()> onCancel;
    std::function<void(uint32_t argb, float intensity)> onChange;

    // State flags
    bool eyedropperActive = false; // true when user is picking a color from screen
    bool eyedropperMonitorsArmed = false; // true while global move/click monitors are installed
    // Color to restore when canceling eyedropper (0xAARRGGBB).
    // Defaults to white as a safe fallback if we fail to capture the starting value.
    uint32_t eyedropperStartArgb = 0xFFFFFFFF;

    // Non-owning pointer to the owning window's UI manager.
    // Used by global eyedropper callbacks so they don't capture window-context pointers.
    UIManager* ownerUi = nullptr;

    // Non-owning pointer to the picker's own native window, for ColorPickerWindow::Close.
    Platform::Window* window = nullptr;
};

} // namespace GameEngine
