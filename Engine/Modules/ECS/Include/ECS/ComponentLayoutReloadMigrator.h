#pragma once

// Native hot-reload component-layout migration (C12 module reload, S8 carry).
//
// A native module reload can change a placed component's byte layout (field
// added / removed / moved / retyped, size change). The migrator brackets the
// load: SnapshotLayouts runs BEFORE the new DLL overwrites the reflection
// registries and captures every reflected user component's field table +
// size — deep-copying field names, because the registry's default tables are
// non-owning views into the registrant module's rdata and the C12 unload may
// UNMAP that image inside the load this snapshot brackets. MigrateChangedLayouts
// runs after the load and re-packs placed instances of every component whose
// layout changed: same-named fields carry over (name+type+size match), new
// fields take their defaults, removed fields drop — the same by-name carry the
// C# blob-schema path applies (ECSABI's RegisterBlobComponentWithSchema).
//
// Components with default bytes but NO field table (typed plugin components
// without GE_REFLECT / scanner coverage) cannot carry fields: a size change
// resets every placed instance to defaults, loudly. A SAME-SIZE relayout of
// such a component is undetectable and reinterprets in place — field
// reflection is what buys detection + carry.

#include "ECS/ComponentMigration.h"
#include "ECS/Reflection.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace ECS {

class World;

class ComponentLayoutReloadMigrator
{
public:
    // Before-load hook: snapshot each reflected user component's field table +
    // current byte size. The set is ComponentFactory::DefaultByteTypes() —
    // exactly the components the migration can re-seed (typed plugin
    // components opt in via RegisterDefaultBytes; engine components are out).
    void SnapshotLayouts();

    // After-load hook: migrate placed instances of every snapshotted component
    // whose layout changed — in `world` AND every other live world holding the
    // component (World::MigrateComponentLayoutAcrossWorlds; one snapshot
    // serves all worlds since it captures the registries, not a world).
    // Idempotent — a migrated component leaves the snapshot, so a re-fired
    // after-hook (the optimistic-load double fire) can never re-remap
    // already-migrated bytes. Returns the migrated component type ids (the
    // editor rebuilds inspectors watching them).
    std::vector<ComponentTypeId> MigrateChangedLayouts(World& world);

private:
    struct LayoutSnapshot
    {
        // Owns the field-name bytes; Fields' Name views point into it.
        // Reserved exactly once — a reallocation would move SSO strings and
        // dangle the views.
        std::vector<std::string> NameStorage;
        std::vector<FieldInfo> Fields;
        std::size_t Size = 0;
    };
    std::unordered_map<ComponentTypeId, LayoutSnapshot> m_PreReloadLayouts;
};

} // namespace ECS
} // namespace GameEngine
