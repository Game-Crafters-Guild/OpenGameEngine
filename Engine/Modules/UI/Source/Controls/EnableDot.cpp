#include "UI/Controls/EnableDot.h"

#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <memory>
#include <string>

namespace GameEngine {

namespace
{
void SetClassPresent(UIElement& element, const std::string& className, bool present)
{
    if (present)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}
} // namespace

EnableDot::EnableDot()
{
    AddClass("enable-dot");
    // Control styling is an Editor-shipped UIStyle asset, as Toggle's is.
    RequestSubtreeStyleAssetPath("UI/controls/EnableDot/EnableDot.css", "editor");

    auto mark = std::make_unique<UIElement>();
    m_Mark = mark.get();
    m_Mark->AddClass("enable-dot-mark");
    AddChild(std::move(mark));
    OnToggleStateChanged();
}

void EnableDot::SetInactive(bool inactive)
{
    if (m_Inactive == inactive)
        return;
    m_Inactive = inactive;
    SetClassPresent(*this, "enable-dot-inactive", inactive);
}

void EnableDot::OnEvent(UIEvent& e)
{
    // Only the primary button switches the dot. Any other press is left to the parent (a section
    // header's context menu), as a press anywhere else in the header would be.
    if ((e.Id == kEventMouseDown || e.Id == kEventMouseUp) && e.Button != 0)
        return;
    ToggleBase::OnEvent(e);
}

void EnableDot::OnToggleStateChanged()
{
    if (!m_Mark)
        return;
    SetClassPresent(*m_Mark, "enable-dot-mark-mixed", IsMixed());
    SetClassPresent(*m_Mark, "enable-dot-mark-on", !IsMixed() && IsChecked());
    SetClassPresent(*m_Mark, "enable-dot-mark-off", !IsMixed() && !IsChecked());
}

} // namespace GameEngine
