#include "ECS/ComponentLayoutReloadMigrator.h"

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Entity.h" // World definition
#include "Logger/Logger.h"

#include <cstdint>
#include <span>
#include <utility>

namespace GameEngine {
namespace ECS {

void ComponentLayoutReloadMigrator::SnapshotLayouts()
{
    m_PreReloadLayouts.clear();
    for (ComponentTypeId id : ComponentFactory::DefaultByteTypes())
    {
        IComponentHandler* handler = ComponentRegistry::GetHandler(id);
        if (!handler)
            continue;
        const std::span<const FieldInfo> fields = ComponentFieldRegistry::Get(id);
        LayoutSnapshot snap;
        snap.Size = handler->GetComponentSize();
        snap.Fields.assign(fields.begin(), fields.end());
        snap.NameStorage.reserve(snap.Fields.size());
        for (FieldInfo& f : snap.Fields)
        {
            snap.NameStorage.emplace_back(f.Name);
            f.Name = snap.NameStorage.back();
        }
        m_PreReloadLayouts.emplace(id, std::move(snap));
    }
}

std::vector<ComponentTypeId> ComponentLayoutReloadMigrator::MigrateChangedLayouts(World& world)
{
    std::vector<ComponentTypeId> migrated;
    for (auto it = m_PreReloadLayouts.begin(); it != m_PreReloadLayouts.end();)
    {
        const ComponentTypeId id = it->first;
        LayoutSnapshot& old = it->second;

        // Component absent from the new build (module dropped it, or the load
        // failed and purged it): nothing to migrate TO. Keep the snapshot — a
        // later load in the same bracket may bring the type back.
        std::vector<std::uint8_t> newDefaults;
        if (!ComponentFactory::GetDefaultBytes(id, newDefaults))
        {
            ++it;
            continue;
        }

        const std::span<const FieldInfo> nf = ComponentFieldRegistry::Get(id);
        std::vector<FieldInfo> newFields(nf.begin(), nf.end());
        const std::size_t newSize = newDefaults.size();
        if (!LayoutsDiffer(old.Fields, old.Size, newFields, newSize))
        {
            ++it;
            continue;
        }

        const ComponentRegistry::ComponentInfo* info = ComponentRegistry::GetComponentInfo(id);
        const std::string_view name = info ? std::string_view(info->Name) : std::string_view("<unregistered>");
        if (old.Fields.empty() && newFields.empty())
        {
            Logger::Log::Warning(
                "[NativeScripting] component '{}' has no field reflection; its size changed "
                "{} -> {} bytes across the reload — every placed instance RESET to defaults. "
                "Add GE_REFLECT/scanner coverage to carry fields by name.",
                name, old.Size, newSize);
        }

        World::MigrateComponentLayoutAcrossWorlds(
            world, ComponentLayoutChange{id, std::move(old.Fields), old.Size, std::move(newFields),
                                         newSize, std::move(newDefaults)});
        Logger::Log::Info("[NativeScripting] migrated placed component '{}' layout (id {}): {} -> {} bytes",
                          name, id, old.Size, newSize);
        migrated.push_back(id);
        // Idempotency: a re-fired after-hook (optimistic-load double fire) must
        // never remap already-migrated bytes with the stale old offsets.
        it = m_PreReloadLayouts.erase(it);
    }
    return migrated;
}

} // namespace ECS
} // namespace GameEngine
