#pragma once

// A platform stub whose only job is to report a fixed content scale.
// UIManager reads IPlatformApi::GetContentScale() once per frame and drives the
// whole logical-px -> physical-px mapping from it, so this is the single hook a
// test needs to pin a DPI scale.
//
// UIManager::SetPlatform stores a non-owning pointer: the stub must outlive the
// manager, so declare it before the manager in any fixture that owns both.

#include "UI/UIPlatform.h"

#include <string>

namespace GameEngine::UITesting
{

class FixedScalePlatform final : public UI::IPlatformApi
{
  public:
    explicit FixedScalePlatform(float scale) : m_Scale(scale) {}

    std::string GetClipboardText() const override { return m_Clipboard; }
    void SetClipboardText(const char* utf8) override { m_Clipboard = utf8 ? utf8 : ""; }
    float GetContentScale() const override { return m_Scale; }

  private:
    float m_Scale = 1.0f;
    std::string m_Clipboard;
};

} // namespace GameEngine::UITesting
