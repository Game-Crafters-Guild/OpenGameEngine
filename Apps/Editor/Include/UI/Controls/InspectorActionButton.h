#pragma once

#include "UI/Controls/Button.h"

#include <string>

namespace GameEngine::EditorUI
{

/// An outlined inspector action, as wide as its content, with an icon before its text (Add Phase,
/// Restart, Pause). `iconClass` is a CSS class that sets the icon's background-image: the control's
/// own sheet defines inspector-action-add, inspector-action-restart, inspector-action-play,
/// inspector-action-pause and inspector-action-step, and a consumer's sheet may define another.
class InspectorActionButton final : public Button
{
  public:
    InspectorActionButton(const std::string& text, const std::string& iconClass);

    /// Swaps the icon, for an action that changes what it does (Pause becoming Play).
    void SetIconClass(const std::string& iconClass);

  private:
    UIElement* m_Icon = nullptr;
    std::string m_IconClass;
};

} // namespace GameEngine::EditorUI
