#pragma once

#include "Types/Types.h"

namespace GameEngine::ECS
{

// What a component type says about its own on/off state, recorded by the registry at
// registration (ComponentRegistry::ComponentInfo::Flags). A type declares a flag beside its
// fields with a static constexpr bool of the flag's name, so the header that defines the
// component needs nothing from the ECS:
//
//     struct Transform { float32 matrix[16]; static constexpr bool NotToggleable = true; };
//
// A type that declares neither switches on and off through its ComponentDisabled tag.
enum class ComponentFlags : uint8
{
    None = 0,
    // No on/off state: structure (Transform, Parent, Name) or data derived from other
    // components (WorldTransform, LocalBounds). World::SetComponentEnabledImmediate refuses
    // it, Entity::SetEnabled<T> does not compile for it, and the editor shows it no dot.
    NotToggleable = 1u << 0,
    // The on/off state is the type's own bool field named Enabled, a value its system reads
    // (a post-process effect pinned off inside its volume, a terrain effect the bake skips),
    // not the ComponentDisabled tag. The registry records the field's offset
    // (ComponentInfo::EnabledFieldOffset); the editor's dot edits that field, and the scene
    // file saves it among the component's own values.
    KeepsOwnEnabledField = 1u << 1,
};

constexpr ComponentFlags operator|(ComponentFlags a, ComponentFlags b)
{
    return static_cast<ComponentFlags>(static_cast<uint8>(a) | static_cast<uint8>(b));
}

constexpr bool HasAnyFlag(ComponentFlags set, ComponentFlags mask)
{
    return (static_cast<uint8>(set) & static_cast<uint8>(mask)) != 0;
}

// The flags type T declares (see ComponentFlags above).
template <class T>
consteval ComponentFlags ComponentFlagsOf()
{
    ComponentFlags flags = ComponentFlags::None;
    if constexpr (requires { requires T::NotToggleable == true; })
        flags = flags | ComponentFlags::NotToggleable;
    if constexpr (requires { requires T::KeepsOwnEnabledField == true; })
        flags = flags | ComponentFlags::KeepsOwnEnabledField;
    return flags;
}

} // namespace GameEngine::ECS
