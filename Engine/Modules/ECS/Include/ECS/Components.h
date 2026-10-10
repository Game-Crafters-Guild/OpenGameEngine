#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <string_view>

// Forward declaration
namespace GameEngine {
namespace ECS {
    struct EntityHandle;
}
}

namespace GameEngine {
namespace ECS {

// Enable state, as tags the query engine honours by construction. A query
// excludes every archetype carrying one of these for a component it requires,
// so a disabled row is never visited rather than visited and rejected.
// Entity::SetEnabled / Entity::SetEnabled<T> are the API; these are what they
// write. Direct access (Get, GetForWrite, Has) is unaffected — scene IO, undo
// and the inspector read disabled state that way.

// The authored "this entity is off" marker, serialised as `Disabled.present`.
struct Disabled {};

// Derived state: set on an entity that is itself disabled or has a disabled
// ancestor. Never authored and never serialised — it is re-derived from the
// Parent chain whenever the hierarchy or an enable state changes (the engine's
// DisabledInHierarchySystem owns it), so a world without that system still
// excludes self-disabled entities through Disabled above.
struct DisabledInHierarchy {};

// Component T is switched off on this entity: every system acts as if T were
// absent. A query that requires T does not match the row at all; an
// Optional<T> reads as a null pointer.
//
// Its canonical name ends inside the template argument
// ("GameEngine::ECS::ComponentDisabled<ns::X>"), so the scene writer's simple
// name, cut at the last "::", is "X>", and FindByName's suffix match resolves
// that fragment to any template over ns::X. Serialising this component by name
// needs an explicitly registered name.
template <Component T>
struct ComponentDisabled {};

// The three tags above: an entity's or a component's enable state, never
// something that is itself switched on or off.
template <class T>
inline constexpr bool kIsEnableStateTag = false;
template <>
inline constexpr bool kIsEnableStateTag<Disabled> = true;
template <>
inline constexpr bool kIsEnableStateTag<DisabledInHierarchy> = true;
template <Component T>
inline constexpr bool kIsEnableStateTag<ComponentDisabled<T>> = true;

// ComponentDisabled<T>'s id for a component known only by its registered name
// (the scripting ABI, the inspector, blob components that have no C++ type):
// the hash of the name ComponentTypeName<ComponentDisabled<T>>() spells, which
// is the same on every compiler.
inline constexpr std::string_view kComponentDisabledNamePrefix = "GameEngine::ECS::ComponentDisabled<";
inline constexpr std::string_view kComponentDisabledNameSuffix = ">";

constexpr ComponentTypeId ComponentDisabledTypeId(std::string_view componentName)
{
    return Hash64(kComponentDisabledNameSuffix,
                  Hash64(componentName, Hash64(kComponentDisabledNamePrefix)));
}

} // namespace ECS
} // namespace GameEngine
