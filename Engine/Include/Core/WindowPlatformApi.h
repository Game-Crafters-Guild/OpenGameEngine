#pragma once

#include "Platform/Clipboard.h"
#include "Platform/Window.h"
#include "UI/UIPlatform.h"

#include <algorithm>

namespace GameEngine
{
// Concrete `UI::IPlatformApi` implementation backed by the Platform module.
// Lives in the Engine layer because it bridges UI (interface) with Platform
// (Window + clipboard). The UI module sees Platform only through opaque
// interfaces — `UI::IPlatformApi` here, and `INativeContextMenu` behind the
// menu factory ContextMenuManipulator uses; it never touches a concrete
// Platform::Window.
//
// Accepts an optional Platform::Window* so GetContentScale() can report the
// OS DPI scale for that window (used by UIManager for HiDPI layout).
class WindowPlatformApi final : public UI::IPlatformApi
{
  public:
    explicit WindowPlatformApi(Platform::Window* window = nullptr) : m_Window(window) {}

    std::string GetClipboardText() const override { return Platform::GetClipboardText(); }
    void SetClipboardText(const char* utf8) override { Platform::SetClipboardText(utf8 ? utf8 : ""); }

    Platform::Window* GetNativeWindow() const override { return m_Window; }

    void SetUseSystemContentScale(bool use) override { m_UseSystemContentScale = use; }
    bool GetUseSystemContentScale() const override { return m_UseSystemContentScale; }

    void SetContentScaleMultiplier(float mult) override
    {
        m_ContentScaleMultiplier = std::clamp(mult, 0.25f, 4.0f);
    }
    float GetContentScaleMultiplier() const override { return m_ContentScaleMultiplier; }

    void GetContentScaleXY(float& outSx, float& outSy) const override
    {
        float sx = 1.0f;
        float sy = 1.0f;
        if (m_Window && m_UseSystemContentScale)
            m_Window->GetContentScale(sx, sy);
        sx *= m_ContentScaleMultiplier;
        sy *= m_ContentScaleMultiplier;
        outSx = std::max(0.01f, sx);
        outSy = std::max(0.01f, sy);
        if (m_Window)
            m_Window->SetUiContentScale(0.5f * (outSx + outSy));
    }

    float GetContentScale() const override
    {
        float sx = 1.0f;
        float sy = 1.0f;
        GetContentScaleXY(sx, sy);
        return 0.5f * (sx + sy);
    }

  private:
    Platform::Window* m_Window = nullptr;
    bool m_UseSystemContentScale = true;
    float m_ContentScaleMultiplier = 1.0f;
};
} // namespace GameEngine
