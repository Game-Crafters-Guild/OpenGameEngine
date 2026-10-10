#include "UI/Controls/InspectorActionButton.h"

#include <memory>

namespace GameEngine::EditorUI
{
namespace
{
constexpr const char* kStyleAssetPath = "UI/controls/InspectorActionButton/InspectorActionButton.css";
}

InspectorActionButton::InspectorActionButton(const std::string& text, const std::string& iconClass)
{
    AddClass("inspector-action-button");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    auto icon = std::make_unique<UIElement>();
    icon->AddClass("inspector-action-icon");
    m_Icon = icon.get();
    InsertChild(0, std::move(icon));
    SetIconClass(iconClass);
    SetText(text);
}

void InspectorActionButton::SetIconClass(const std::string& iconClass)
{
    if (m_IconClass == iconClass)
        return;
    if (!m_IconClass.empty())
        m_Icon->RemoveClass(m_IconClass);
    m_IconClass = iconClass;
    m_Icon->AddClass(m_IconClass);
}

} // namespace GameEngine::EditorUI
