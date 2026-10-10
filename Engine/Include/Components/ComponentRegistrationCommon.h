#pragma once

// ComponentRegistrationCommon.h — the part of component registration that does not
// depend on how a component self-registers.
//
// Two registration headers exist because a component's *self-registration* differs by
// consumer: Components/ComponentRegistration.h registers the engine's own components
// through the typed factory, Components/ComponentRegistrationLite.h registers a user
// DLL's components through the type-erased blob path. Each defines its own
// RegisterReflectedComponent<T> plus the four macros that call it, and nothing else:
// every other helper and macro lives here and is included by both, so the macro surface
// a generated translation unit can use is identical whichever header it includes.
//
// That identity is a hard requirement, not a convenience: ComponentScanner emits the
// same macros in both modes and only switches which registration header it includes
// (Managed/ComponentScanner/Program.cs). A macro defined in one header and not the
// other is a compile error in generated code.
//
// Place GE_REGISTER_COMPONENT_* and the metadata macros in a .cpp, never a header:
// GE_REFLECT defines an explicit specialization of Reflection<T>, which must have
// exactly one definition.

#include "ECS/ECS.h"                    // GetComponentTypeId<T>
#include "ECS/Reflection.h"             // GE_REFLECT_BEGIN/_FIELD/_END, FieldFlags, EnumNameValue, GE_PP_CONCAT
#include "ECS/ComponentFieldRegistry.h" // ComponentFieldRegistry
#include "Components/MathReflectionTraits.h"  // Vector2/3/4 + Quaternion -> Vec/Quat field types

#include <limits>  // generated code passes std::numeric_limits<float>::infinity() for an open range maximum
#include <span>
#include <string_view>

namespace GameEngine {
namespace Components {

// Apply per-field policy flags (Hidden / ReadOnly / Transient) to a reflected component.
// Must run after the component is registered (GE_REFLECT_FIELD_FLAGS is placed after the
// GE_REGISTER_COMPONENT block). Returns true so it can seed a file-scope static.
template <class T>
inline bool SetReflectedFieldFlags(std::string_view fieldName, ECS::FieldFlags flags)
{
    ECS::ComponentFieldRegistry::SetFieldFlags(ECS::GetComponentTypeId<T>(), fieldName, flags);
    return true;
}

// Bind inspector slider bounds [minValue, maxValue] to a reflected field (by name).
// Must run after the component is registered. Returns true so it can seed a
// file-scope static; also callable directly (e.g. from a module-init function).
template <class T>
inline bool SetReflectedFieldRange(std::string_view fieldName, float minValue, float maxValue)
{
    ECS::ComponentFieldRegistry::SetFieldRange(ECS::GetComponentTypeId<T>(), fieldName, minValue,
                                               maxValue);
    return true;
}

// Bind editor tooltip text to a reflected field. The tooltip string is non-owning;
// pass a string literal or other static storage. Must run after registration.
template <class T>
inline bool SetReflectedFieldTooltip(std::string_view fieldName, std::string_view tooltip)
{
    ECS::ComponentFieldRegistry::SetFieldTooltip(ECS::GetComponentTypeId<T>(), fieldName, tooltip);
    return true;
}

// Mark a reflected component as non-serializable (scene save/load skips it — it's runtime/derived
// state rebuilt at runtime). Must run after the component is registered. Returns true to seed a
// file-scope static. Emitted by the scanner for "// [DoNotSerialize]"-marked components.
template <class T>
inline bool MarkComponentDoNotSerialize()
{
    ECS::ComponentFieldRegistry::SetComponentDoNotSerialize(ECS::GetComponentTypeId<T>());
    return true;
}

// Mark a reflected component as editor-only (a game export strips its lines from staged scenes;
// the entity stays). Must run after the component is registered. Returns true to seed a
// file-scope static. Emitted by the scanner for "// @ge-editor-only"-marked components.
template <class T>
inline bool MarkComponentEditorOnly()
{
    ECS::ComponentFieldRegistry::SetComponentEditorOnly(ECS::GetComponentTypeId<T>());
    return true;
}

// Bind a reflected enum field to its value-name table so the scene serializer writes/reads the
// enumerator name (integer fallback) and the inspector can show a dropdown. Must run after the
// component is registered. Returns true to seed a file-scope static. Emitted by the scanner for
// fields whose type is an enum it discovered, paired with the constexpr table it also emits.
template <class T>
inline bool RegisterReflectedFieldEnum(std::string_view fieldName,
                                       std::span<const ECS::EnumNameValue> table)
{
    ECS::ComponentFieldRegistry::SetFieldEnum(ECS::GetComponentTypeId<T>(), fieldName, table);
    return true;
}

} // namespace Components
} // namespace GameEngine

// Cap-free registration block openers. GE_REGISTER_COMPONENT_END / _END_NO_ADD close the
// block and live in the two registration headers, because closing the block is what
// performs the self-registration:
//
//     GE_REGISTER_COMPONENT_BEGIN(MyGame::Foo)
//         GE_REGISTER_COMPONENT_FIELD(MyGame::Foo, A)
//         GE_REGISTER_COMPONENT_FIELD(MyGame::Foo, B)
//     GE_REGISTER_COMPONENT_END(MyGame::Foo)            // addable (_END_NO_ADD to hide)
//
// The cap-free form has no field-count limit (the variadic GE_REGISTER_COMPONENT is capped
// by its hand-unrolled FOR_EACH); the scanner emits this form for every reflected component.
#define GE_REGISTER_COMPONENT_BEGIN(TypeName) GE_REFLECT_BEGIN(TypeName)

#define GE_REGISTER_COMPONENT_FIELD(TypeName, FieldName) GE_REFLECT_FIELD(TypeName, FieldName)

// GE_REFLECT_FIELD_FLAGS(Type, Field, flags) — mark one reflected field Hidden
// and/or ReadOnly (e.g. ::GameEngine::ECS::FieldFlags::ReadOnly). Place AFTER the
// type's GE_REGISTER_COMPONENT line; the field is matched by name. Used to keep a
// field out of the inspector or block set_component from writing it.
#define GE_REFLECT_FIELD_FLAGS(TypeName, FieldName, Flags)                         \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_field_flags_, __LINE__) =          \
        ::GameEngine::Components::SetReflectedFieldFlags<TypeName>(#FieldName, (Flags)); \
    }

// GE_REFLECT_FIELD_TRANSIENT(Type, Field) — mark a reflected field as runtime-only so the scene
// serializer skips it (the value is rebuilt at runtime, e.g. a live physics body handle). Place
// AFTER the type's GE_REGISTER_COMPONENT line; the field is matched by name.
#define GE_REFLECT_FIELD_TRANSIENT(TypeName, FieldName)                            \
    GE_REFLECT_FIELD_FLAGS(TypeName, FieldName, ::GameEngine::ECS::FieldFlags::Transient)

// GE_REFLECT_FIELD_TOOLTIP(Type, Field, TooltipText) — attach editor tooltip text
// to one reflected field. TooltipText should be a string literal or static storage.
#define GE_REFLECT_FIELD_TOOLTIP(TypeName, FieldName, TooltipText)                 \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_field_tooltip_, __LINE__) =        \
        ::GameEngine::Components::SetReflectedFieldTooltip<TypeName>(#FieldName, (TooltipText)); \
    }

// GE_REFLECT_FIELD_RANGE(Type, Field, Min, Max) — bind inspector bounds to one
// reflected float field; the float row clamps edits to [Min, Max]. Pass
// std::numeric_limits<float>::infinity() as Max for an open maximum. The scanner
// emits this for fields carrying a range marker comment (MarkerScanner.cs).
#define GE_REFLECT_FIELD_RANGE(TypeName, FieldName, MinValue, MaxValue)            \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_field_range_, __LINE__) =          \
        ::GameEngine::Components::SetReflectedFieldRange<TypeName>(#FieldName, (MinValue), (MaxValue)); \
    }

// GE_REFLECT_COMPONENT_DONOTSERIALIZE(Type) — exclude a reflected component from scene save/load
// (runtime/derived state). Place AFTER the type's GE_REGISTER_COMPONENT line. The scanner emits
// this for components carrying a "// [DoNotSerialize]" marker comment.
#define GE_REFLECT_COMPONENT_DONOTSERIALIZE(TypeName)                              \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_donotserialize_, __LINE__) =       \
        ::GameEngine::Components::MarkComponentDoNotSerialize<TypeName>();         \
    }

// GE_REFLECT_COMPONENT_EDITORONLY(Type) — strip a reflected component from game exports: its
// lines leave every staged scene and the entity keeps its other components (SceneExportStrip.h).
// Place AFTER the type's GE_REGISTER_COMPONENT line. The scanner emits this for components
// carrying a "// @ge-editor-only" marker comment.
#define GE_REFLECT_COMPONENT_EDITORONLY(TypeName)                                  \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_editoronly_, __LINE__) =           \
        ::GameEngine::Components::MarkComponentEditorOnly<TypeName>();             \
    }

// Enum value-name tables. C++ can't reflect enumerator names before C++26, so the build-time
// ComponentScanner — which sees the enum declaration — emits one constexpr table per enum used by a
// reflected field, then binds it to that field with GE_REFLECT_ENUM_FIELD (placed after the
// component block, like GE_REFLECT_FIELD_FLAGS). The table is file-local constexpr storage (rodata,
// no allocation); the field holds a non-owning span into it.
//
//     GE_REFLECT_ENUM_TABLE_BEGIN(LightType)
//         GE_REFLECT_ENUM_TABLE_VALUE("Directional", 0)
//         GE_REFLECT_ENUM_TABLE_VALUE("Point", 1)
//     GE_REFLECT_ENUM_TABLE_END()
//     ...
//     GE_REFLECT_ENUM_FIELD(GameEngine::Components::Light, Type, LightType)
//
// TableId is a bare identifier the scanner derives from the enum's simple name.
#define GE_REFLECT_ENUM_TABLE_BEGIN(TableId)                                       \
    namespace GeEnumTables {                                                       \
    static constexpr ::GameEngine::ECS::EnumNameValue TableId[] = {

#define GE_REFLECT_ENUM_TABLE_VALUE(NameStr, Value)                                \
    ::GameEngine::ECS::EnumNameValue{ (NameStr), static_cast<::std::int64_t>(Value) },

#define GE_REFLECT_ENUM_TABLE_END() }; }

// GE_REFLECT_ENUM_FIELD(Type, Field, TableId) — bind a reflected field to an enum table emitted
// above. Place AFTER the type's GE_REGISTER_COMPONENT line; the field is matched by name.
#define GE_REFLECT_ENUM_FIELD(TypeName, FieldName, TableId)                        \
    namespace {                                                                    \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_enum_field_, __LINE__) =           \
        ::GameEngine::Components::RegisterReflectedFieldEnum<TypeName>(            \
            #FieldName, ::GeEnumTables::TableId);                                  \
    }
