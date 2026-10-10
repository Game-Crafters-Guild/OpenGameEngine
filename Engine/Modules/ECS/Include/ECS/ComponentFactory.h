#pragma once

// ComponentFactory — runtime "add a default-constructed component by type id".
//
// Runtime scene loading and the editor's discoverable Add Component menu know a
// component only by its runtime ComponentTypeId, but adding it correctly requires
// default member values like RenderLayer::mask = 0xFFFFFFFF or AnimatorRef::Speed = 1.0
// to be honoured. This registry stores default bytes for all reflected components and
// menu creators for the subset that should be user-addable.
//
// Registered alongside the reflection field tables (see ComponentReflection.cpp).
// Kept standalone (NOT folded into the widely-included ComponentRegistry.h) so
// it adds no recompile pressure, and editor-facing UX (display name, category,
// hidden) lives editor-side, not here.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace GameEngine {
namespace ECS {

class World;
struct EntityHandle;

using ComponentTypeId = std::uint64_t;

class ComponentFactory
{
public:
    // Default-construct the component and add it to the entity.
    using CreateFn = std::function<void(World&, EntityHandle)>;
    // Adjust a component's default bytes, in place, to the entity it is added
    // to: a default that depends on what else the entity carries.
    using DeriveDefaultsFn =
        std::function<void(const World&, EntityHandle, std::span<std::uint8_t> bytes)>;

    static void Register(ComponentTypeId typeId, CreateFn create);

    // Register a creator from a snapshot of the component's default bytes (sizeof(T)
    // bytes of a default-constructed, trivially-copyable T) instead of a typed
    // closure. The stored creator adds the component via the type-erased
    // World::SetComponentBytesImmediate path, so the registering translation unit
    // never instantiates World::AddComponentImmediate<T> (and thus skips the World
    // template surface). For user-script DLLs whose registration is compiled against
    // a minimal SDK header; requires a handler registered for typeId (e.g.
    // ComponentRegistry::RegisterBlobComponent). The bytes are copied into the registry.
    // Record a component's default-value byte snapshot (sizeof(T) bytes of a default-constructed,
    // trivially-copyable T). The bytes are always stored (so hot-reload migration works on hidden
    // components too); `addable` controls whether an Add-Component-menu creator is registered.
    // The creator adds via the type-erased World::SetComponentBytesImmediate path, so the
    // registering TU never instantiates World::AddComponentImmediate<T>. Requires a handler for
    // typeId (e.g. ComponentRegistry::RegisterBlobComponent).
    static void RegisterDefaultBytes(ComponentTypeId typeId, const void* defaultBytes, std::size_t size,
                                     bool addable);

    // Copy out the registered default bytes for a type (sized to the current sizeof(T)). Returns
    // false if none recorded. Used by hot-reload migration to seed the new layout + new fields.
    static bool GetDefaultBytes(ComponentTypeId typeId, std::vector<std::uint8_t>& out);

    // Register how a type's defaults follow the entity it is added to. Create applies it to
    // the default bytes whenever it adds that type; a load then applies the saved fields over
    // them, so a saved value always wins.
    static void RegisterDerivedDefaults(ComponentTypeId typeId, DeriveDefaultsFn derive);

    // All type ids with recorded default bytes (every reflected user component, addable or not).
    // The hot-reload migration snapshots/migrates exactly this set — distinct from RegisteredTypes()
    // (Add-menu creators only).
    static std::vector<ComponentTypeId> DefaultByteTypes();

    // Add a default-constructed component of typeId to the entity, with its defaults derived
    // for the entity when the type registered a derivation. Returns false if no creator or
    // default-byte snapshot is registered for the type.
    static bool Create(World& world, EntityHandle entity, ComponentTypeId typeId);

    static bool Has(ComponentTypeId typeId);

    // All component type ids with a registered Add Component menu creator (unordered).
    static std::vector<ComponentTypeId> RegisteredTypes();
};

} // namespace ECS
} // namespace GameEngine
