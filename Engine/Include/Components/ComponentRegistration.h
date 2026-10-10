#pragma once

// GE_REGISTER_COMPONENT — declarative, self-registering component reflection.
//
// One macro per component (in a .cpp — the engine's ComponentReflection.cpp, a
// generated TU, or a user component's .cpp) declares the field table AND wires
// it into the runtime registries at static-init time:
//   * ComponentFieldRegistry  — field table + canonical type name (drives the
//     default inspector drawer and the editor's derived display name)
//   * ComponentFactory         — default-construct/add by type id (scene load + Add menu)
//
// This replaces a hand-maintained central registration list: adding a component
// is one macro line next to its fields. A build-time header scanner emits these
// same calls for zero boilerplate.
//
// This header is the ENGINE side of registration. A user-script DLL includes
// Components/ComponentRegistrationLite.h instead, which registers through the
// type-erased blob path. Both include ComponentRegistrationCommon.h, which owns
// every helper and macro that does not depend on the registration strategy — this
// file adds only RegisterReflectedComponent<T> and the four macros that call it.
// The two headers are never co-included in one translation unit: they define the
// same names for their two registration strategies.
//
// Place in a .cpp, never a header: GE_REFLECT defines an explicit specialization
// of Reflection<T>, which must have exactly one definition, and keeping it out of
// headers avoids an engine-wide recompile.

#include "Components/ComponentRegistrationCommon.h"  // metadata helpers + strategy-free macros
#include "ECS/ECS.h"                    // GetComponentTypeId<T>, Component concept, ComponentTypeId
#include "ECS/ComponentTypeName.h"      // ComponentTypeName<T>
#include "ECS/ComponentFactory.h"       // ComponentFactory::RegisterDefaultBytes

#include <type_traits>

namespace GameEngine {
namespace Components {

// Register a GE_REFLECT'd component's runtime reflection: field table + canonical
// (normalized, cross-compiler-stable) type name, and — when T is a valid ECS component —
// default bytes the type-erased factory can use for scene load. addable only controls
// whether the type appears in the Add Component menu. Safe at static-init: the registries are lazy
// function-local statics, ComponentTypeName/GetReflectedFields are compile-time,
// and the factory lambda is stored (not invoked) here.
//
// The factory is gated on the Component concept so a POD helper struct that isn't a
// real component (and thus can't satisfy AddComponentImmediate<T>) registers a field
// table for the inspector without breaking the build or becoming addable.
template <class T>
inline bool RegisterReflectedComponent(bool addable)
{
    const ECS::ComponentTypeId id = ECS::GetComponentTypeId<T>();
    ECS::ComponentFieldRegistry::Register(id, ECS::GetReflectedFields<T>(), ECS::ComponentTypeName<T>());
    if constexpr (ECS::Component<T> && std::is_default_constructible_v<T>)
    {
        const T defaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(id, &defaults, sizeof(T), addable);
    }
    return true;
}

} // namespace Components
} // namespace GameEngine

// GE_REGISTER_COMPONENT(Type, field, field, ...) — field table + self-registration,
// addable in the Add Component menu by default. Fully-qualified type name;
// 1-24 fields; caller supplies the trailing ';'.
#define GE_REGISTER_COMPONENT(TypeName, ...)                                       \
    GE_REFLECT(TypeName, __VA_ARGS__);                                             \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_auto_register_, __LINE__) =        \
        ::GameEngine::Components::RegisterReflectedComponent<TypeName>(true);      \
    }

// GE_REGISTER_COMPONENT_NO_ADD(Type, ...) — same, but NOT offered in the Add menu.
// For data helpers and components that are added by other means (the scanner emits
// this for structs marked with the opt-out comment).
#define GE_REGISTER_COMPONENT_NO_ADD(TypeName, ...)                                \
    GE_REFLECT(TypeName, __VA_ARGS__);                                             \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_auto_register_, __LINE__) =        \
        ::GameEngine::Components::RegisterReflectedComponent<TypeName>(false);     \
    }

// Close a cap-free registration block opened with GE_REGISTER_COMPONENT_BEGIN (the block
// form is documented in ComponentRegistrationCommon.h). _END self-terminates (no trailing
// ';' needed). Use _END_NO_ADD to reflect the component without offering it in the
// editor's Add Component menu.
#define GE_REGISTER_COMPONENT_END(TypeName)                                        \
    GE_REFLECT_END(TypeName);                                                      \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_auto_register_, __LINE__) =        \
        ::GameEngine::Components::RegisterReflectedComponent<TypeName>(true);      \
    }

#define GE_REGISTER_COMPONENT_END_NO_ADD(TypeName)                                 \
    GE_REFLECT_END(TypeName);                                                      \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_auto_register_, __LINE__) =        \
        ::GameEngine::Components::RegisterReflectedComponent<TypeName>(false);     \
    }
