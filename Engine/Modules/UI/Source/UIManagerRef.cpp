#include "UI/UIManagerRef.h"

#include "UI/UIManager.h"

namespace GameEngine
{

UIManagerRef::UIManagerRef(UIManager* manager)
    : m_Manager(manager)
{
    if (manager)
        m_Lifetime = manager->m_Lifetime;
}

UIManager* UIManagerRef::Get() const
{
    return m_Lifetime.expired() ? nullptr : m_Manager;
}

} // namespace GameEngine
