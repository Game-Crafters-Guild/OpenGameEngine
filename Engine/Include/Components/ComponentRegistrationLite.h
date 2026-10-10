#pragma once

// ComponentRegistrationLite.h — component registration for user-script DLLs.
//
// Same macro surface as Components/ComponentRegistration.h — both include
// ComponentRegistrationCommon.h, which owns every helper and macro that does not
// depend on the registration strategy, so a generated translation unit compiles
// against either header. This file adds only its own RegisterReflectedComponent<T>
// and the four macros that call it.
//
// What differs is the self-registration. A user DLL has no engine system using
// View<T> for its components, so no C++ component handler is registered automatically;
// registration here also installs a blob handler keyed on the component's type id
// so the ECS can move and store the component by id + size, and the Add Component
// menu adds it through World::SetComponentBytesImmediate:
//
//   * ComponentFieldRegistry — field table + canonical type name (same as the engine)
//   * ComponentRegistry      — blob handler for type-erased storage and movement
//   * ComponentFactory       — creator built from the component's default bytes
//
// The ComponentScanner emits an include of THIS header (not ComponentRegistration.h)
// when run in --detect-base (user) mode. The two headers are never co-included in one
// translation unit: they define the same names for their two registration strategies.
//
// Place GE_REGISTER_COMPONENT_* in a .cpp, never a header: GE_REFLECT defines an
// explicit specialization of Reflection<T>, which must have exactly one definition.

#include "Components/ComponentRegistrationCommon.h"  // metadata helpers + strategy-free macros
#include "ECS/ECS.h"                    // GetComponentTypeId<T>, Component concept, ComponentTypeId
#include "ECS/ComponentTypeName.h"      // ComponentTypeName<T>
#include "ECS/ComponentFactory.h"       // ComponentFactory::RegisterDefaultBytes
#include "ECS/ComponentRegistry.h"      // RegisterBlobComponent / GetComponentInfo

#include <string>
#include <type_traits>

namespace GameEngine {
namespace Components {

// Register a GE_REFLECT'd component the type-erased way: field table + canonical
// name, a blob handler so the ECS can manage it by id+size, and — when addable and T
// is a valid ECS component — a default-bytes factory creator (which makes it appear in
// the Add Component menu). Safe at static-init: the registries are lazy function-local
// statics and ComponentTypeName/GetReflectedFields are compile-time. Idempotent across
// reloads (engine-owned blob handlers are reused and their ownership is refreshed).
template <class T>
inline bool RegisterReflectedComponent(bool addable)
{
    const ECS::ComponentTypeId id = ECS::GetComponentTypeId<T>();
    ECS::ComponentFieldRegistry::Register(id, ECS::GetReflectedFields<T>(), ECS::ComponentTypeName<T>());
    if constexpr (ECS::Component<T> && std::is_default_constructible_v<T>)
    {
        // GetComponentTypeId<T>() == Hash64(ComponentTypeName<T>()), the same id
        // RegisterBlobComponent derives, so the handler, field table, and factory all
        // key on one id.
        // Always visit the registry: a reflected-only component has no typed
        // registrar later in the reload to refresh its module generation.
        // The registry re-owns blob handlers only; typed handlers keep their
        // old image pinned until a typed registrar replaces their vtable.
        // Keep the registered size until the reload migrator repacks live
        // columns from the new default bytes below. Registration alone must
        // neither reject a legitimate layout change nor change its stride.
        const auto* registered = ECS::ComponentRegistry::GetComponentInfo(id);
        ECS::ComponentRegistry::RegisterBlobComponent(std::string(ECS::ComponentTypeName<T>()),
                                                      registered ? registered->Size : sizeof(T));
        // Always record the default bytes so hot-reload migration runs even on hidden (NO_ADD)
        // components; `addable` only gates whether it appears in the Add Component menu.
        const T defaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(id, &defaults, sizeof(T), addable);
    }
    return true;
}

} // namespace Components
} // namespace GameEngine

// GE_REGISTER_COMPONENT(Type, field, ...) — field table + self-registration, addable
// in the Add Component menu by default. Fully-qualified type name; 1-24 fields; caller
// supplies the trailing ';'.
#define GE_REGISTER_COMPONENT(TypeName, ...)                                       \
    GE_REFLECT(TypeName, __VA_ARGS__);                                             \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_auto_register_, __LINE__) =        \
        ::GameEngine::Components::RegisterReflectedComponent<TypeName>(true);      \
    }

// GE_REGISTER_COMPONENT_NO_ADD(Type, ...) — same, but NOT offered in the Add menu.
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
