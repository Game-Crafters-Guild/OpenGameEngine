#pragma once

#include <string>

namespace GameEngine
{
namespace Platform
{
class Window;
}
namespace UI
{

// Minimal cross-module Platform API abstraction
// Demo implementation will be provided in the example app (GLFW-backed),
// but UI and other modules depend only on this interface.
class IPlatformApi
{
  public:
    virtual ~IPlatformApi() = default;
    // Clipboard (UTF-8)
    virtual std::string GetClipboardText() const = 0;
    virtual void SetClipboardText(const char* utf8) = 0;

    // The native window hosting this UI, as an opaque pointer — the UI module never
    // dereferences it, it only hands it to host services whose APIs take one (a context
    // menu opens ON a window). Null when the host has none (tests, headless).
    virtual Platform::Window* GetNativeWindow() const { return nullptr; }

    // DPI/content scale factor for the associated window.
    // Returns 1.0 by default; override to supply the OS-reported scale so the
    // UI system can map CSS logical-px to physical pixels.
    virtual float GetContentScale() const { return 1.0f; }

    // Per-axis content scale (physical pixels per CSS pixel). Defaults to a
    // uniform scale from GetContentScale(); overrides should match what
    // GetContentScale() returns as 0.5f * (outSx + outSy) when axes differ.
    virtual void GetContentScaleXY(float& outSx, float& outSy) const
    {
        const float s = GetContentScale();
        outSx = s;
        outSy = s;
    }

    // Whether the OS-reported content scale is applied to GetContentScale*().
    // Defaults are no-ops so callers can set these polymorphically without
    // downcasting to a concrete implementation.
    virtual void SetUseSystemContentScale(bool) {}
    virtual bool GetUseSystemContentScale() const { return true; }

    // Additional user-controlled multiplier on top of the system scale.
    virtual void SetContentScaleMultiplier(float) {}
    virtual float GetContentScaleMultiplier() const { return 1.0f; }
};

} // namespace UI
} // namespace GameEngine
