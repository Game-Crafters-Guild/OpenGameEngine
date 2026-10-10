#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"  // For unified auto-registration template implementations
#include "Logger/Logger.h"

namespace GameEngine {
namespace ECS {

// Entity implementation
void Entity::Destroy() {
    m_World->DestroyEntity(m_Handle);
    m_Handle = EntityHandle(); // Invalidate
}

Entity Entity::Clone() const {
    EntityHandle newHandle = m_World->CloneEntity(m_Handle);
    return Entity(m_World, newHandle);
}

bool Entity::IsValid() const {
    return m_World->IsValid(m_Handle);
}

Entity& Entity::SetEnabled(bool enabled) {
    m_World->SetEntityEnabledImmediate(m_Handle, enabled);
    return *this;
}

bool Entity::IsEnabled() const {
    return !Has<Disabled>();
}

bool Entity::IsEnabledInHierarchy() const {
    return !Has<Disabled>() && !Has<DisabledInHierarchy>();
}

// Template method implementations moved to Entity.h for automatic instantiation
// No explicit instantiations needed - templates will be instantiated automatically
// when used with any type that satisfies the Component concept

} // namespace ECS
} // namespace GameEngine
