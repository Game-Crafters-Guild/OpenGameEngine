#pragma once

// ComponentFieldRegistry — runtime ComponentTypeId -> reflected field table.
//
// GE_REFLECT produces a per-type Reflection<T>::Fields table at compile time, but
// the editor (and other runtime consumers) only know a component by its runtime
// ComponentTypeId. This registry bridges the two: reflected components register
// their field span here once at startup, and consumers look it up by type id.
//
// The registry holds a NON-OWNING span over each component's constexpr Reflection<T>::Fields by
// default (zero per-component heap). The first per-field mutation (SetFieldFlags / SetFieldEnum)
// copies the table into an owned vector (copy-on-write) and re-points the span at it, so the
// constexpr source is never modified and unflagged components stay copy-free.
//
// Kept deliberately standalone (NOT folded into the widely-included
// ComponentRegistry.h) so adding it doesn't recompile the whole engine.

#include "ECS/Reflection.h"  // FieldInfo

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace GameEngine {
namespace ECS {

// Mirror of ECS.h's alias (consteval Hash64 of the type name). Declared here to
// avoid pulling the heavy ECS.h into this small header; identical re-declaration
// is well-formed when both are visible.
using ComponentTypeId = std::uint64_t;

class ComponentFieldRegistry
{
public:
    // Register a reflected component's field table and its canonical (normalized,
    // cross-compiler-stable) type name. Idempotent (last write wins). The name is
    // a view over storage that must outlive the registry (e.g. ComponentTypeName<T>()
    // returns a view into a string literal / static signature). Editor code derives
    // a human title from this name instead of a hardcoded map.
    static void Register(ComponentTypeId typeId, std::span<const FieldInfo> fields,
                         std::string_view canonicalName = {});

    // Register a field table the registry must deep-copy: the FieldInfo array, every
    // field's Name, and the canonical name are copied into registry-owned storage, so
    // the caller's buffers may be temporaries (the scripting ABI marshals them from
    // managed memory that dies when the call returns). Only Name is deep-copied —
    // Tooltip/EnumNames views must be empty or point at static storage. Idempotent
    // (last write wins): re-registration on a hot-reload replaces the owned table.
    static void RegisterOwned(ComponentTypeId typeId, std::span<const FieldInfo> fields,
                              std::string_view canonicalName);

    // Field table for a component type, or an empty span if it has none registered.
    static std::span<const FieldInfo> Get(ComponentTypeId typeId);

    // Apply policy flags (Hidden / ReadOnly) to a single reflected field, found by
    // name. No-op if the type or field isn't registered. Must run AFTER the type's
    // Register (GE_REFLECT_FIELD_FLAGS is emitted/written after GE_REGISTER_COMPONENT).
    static void SetFieldFlags(ComponentTypeId typeId, std::string_view fieldName, FieldFlags flags);

    // Bind inspector slider bounds [minValue, maxValue] to a single reflected
    // field, found by name. No-op if the type or field isn't registered. Must run
    // AFTER the type's Register. The inspector renders a clamped slider when set.
    static void SetFieldRange(ComponentTypeId typeId, std::string_view fieldName, float minValue,
                              float maxValue);

    // Bind editor tooltip text to a single reflected field, found by name. The
    // string is non-owning and must outlive the registry entry, so use string
    // literals or static storage. No-op if the type or field isn't registered.
    static void SetFieldTooltip(ComponentTypeId typeId, std::string_view fieldName,
                                std::string_view tooltip);

    // Bind a reflected enum field's value<->name table (found by name). The table is a non-owning
    // view over scanner-emitted constexpr storage that must outlive the registry. No-op if the type
    // or field isn't registered. Must run AFTER Register (GE_REFLECT_ENUM_FIELD is emitted after the
    // component block). Lets the scene serializer write/read the enumerator name and the inspector
    // show a dropdown, without C++ enum-name reflection.
    static void SetFieldEnum(ComponentTypeId typeId, std::string_view fieldName,
                             std::span<const EnumNameValue> enumNames);

    // Canonical type name (e.g. "GameEngine::Components::RenderLayer"), or empty if
    // unregistered / registered without a name.
    static std::string_view GetCanonicalName(ComponentTypeId typeId);

    // Resolve a reflected component by name: matches a registered canonical name
    // exactly, or by its simple (unqualified) suffix (e.g. "Camera" matches
    // "GameEngine::Components::Camera"). Returns 0 if no match. Lets type-erased
    // callers (editor/scripting/MCP) find a component type from a plain name.
    static ComponentTypeId FindByName(std::string_view name);

    // The reflected field of `typeId` named `fieldName`, matched case-insensitively (the
    // scene loader lowercases keys; editor and MCP callers type them). Null when the
    // component has no field table or no such field.
    static const FieldInfo* FindField(ComponentTypeId typeId, std::string_view fieldName);

    // True if the component type has a registered field table.
    static bool Has(ComponentTypeId typeId);

    // Test seam: drop the type's field table (pairs with
    // ComponentRegistry::UnregisterComponentForTests to simulate a removed
    // package's types being absent in a fresh session).
    static void UnregisterForTests(ComponentTypeId typeId);

    // C12 load-abort purge / unload quiesce ledger (see ECS/ModuleRegistration.h).
    // The default Register path keeps NON-OWNING views into the registrant's
    // constexpr field tables (module rdata), so a module's entries must be
    // purged before its image unmaps, and an entry still stamped with an older
    // generation blocks that image's unload.
    static std::size_t PurgeModuleFieldTables(std::string_view moduleId, std::uint64_t generation);
    static std::size_t CountSupersededModuleFieldTables(std::string_view moduleId,
                                                        std::uint64_t currentGeneration);

    // Mark a reflected component as non-serializable: scene save/load skips it entirely (it is
    // runtime/derived state — a live GPU handle, a computed world matrix — rebuilt at runtime, not
    // authored). Emitted by the build-time scanner for components carrying a "// [DoNotSerialize]"
    // marker. Must run AFTER the type's Register. No-op if the type isn't registered.
    static void SetComponentDoNotSerialize(ComponentTypeId typeId);

    // True if the component was marked non-serializable (default false).
    static bool IsComponentDoNotSerialize(ComponentTypeId typeId);

    // Mark a reflected component as editor-only: it saves and loads in the editor like any
    // component, and a game export strips its lines from every staged scene (the entity stays;
    // SceneExportStrip.h). Emitted by the build-time scanner for components carrying a
    // "// @ge-editor-only" marker. Must run AFTER the type's Register. No-op if the type isn't
    // registered.
    static void SetComponentEditorOnly(ComponentTypeId typeId);

    // True if the component was marked editor-only (default false).
    static bool IsComponentEditorOnly(ComponentTypeId typeId);
};

} // namespace ECS
} // namespace GameEngine
