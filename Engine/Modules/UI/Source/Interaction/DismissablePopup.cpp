#include "UI/Interaction/DismissablePopup.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"

namespace GameEngine
{

DismissablePopup::~DismissablePopup()
{
    UpdatePopupRegistration(nullptr);
}

UIElement* DismissablePopup::GetPopupRoot()
{
    return m_Self;
}

void DismissablePopup::UpdatePopupRegistration(UIManager* owner)
{
    UIManager* registered = m_RegisteredManager.Get();
    if (registered == owner)
        return;
    if (registered)
        registered->UnregisterDismissablePopup(this);
    m_RegisteredManager = UIManagerRef(owner);
    if (owner)
        owner->RegisterDismissablePopup(this);
}

} // namespace GameEngine
