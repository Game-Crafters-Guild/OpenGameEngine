#include "MissingAssetTracker.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorContext.h"
#include "Logger/Logger.h"
#include "Scene/SceneSchemaRegistry.h"

#include <cstring>
#include <string_view>
#include <filesystem>
#include <system_error>
#include <unordered_map>

namespace GameEngine
{

// The virtual IsPresent's default infers presence by running the schema's full
// Serialize (string formatting of every property) — per (entity x schema) that
// made a 21k-entity rescan a ~40 s main-thread block in Debug. Schemas whose
// name maps to a registered ECS component get an O(1) signature test instead;
// unmapped names keep the serialize probe (correct, just slow).
namespace
{
struct SchemaPresence
{
    const Scene::ISceneComponentSchema* Schema = nullptr;
    ECS::ComponentTypeId TypeId = 0; // 0 = no ECS mapping; use schema->IsPresent
};

std::vector<SchemaPresence> BuildPresenceTable(
    const std::vector<const Scene::ISceneComponentSchema*>& schemas)
{
    std::vector<SchemaPresence> table;
    table.reserve(schemas.size());
    for (const auto* schema : schemas)
    {
        if (!schema)
            continue;
        table.push_back({schema, ECS::ComponentRegistry::GetComponentTypeId(
                                     std::string(schema->GetComponentName()))});
    }
    return table;
}

bool IsSchemaPresent(const SchemaPresence& sp, const ECS::World& world, ECS::EntityHandle handle)
{
    return sp.TypeId != 0 ? world.HasComponent(handle, sp.TypeId)
                          : sp.Schema->IsPresent(world, handle);
}

// The schemas holding the asset references of every entity in `arch`: the hand-written schemas
// present on the entity, then the reflection schemas of its reflected components no hand-written
// schema serializes (the ones the scene save writes through reflection).
void AppendReflectionSchemas(const ECS::Archetype& arch, std::vector<const Scene::ISceneComponentSchema*>& out)
{
    out.clear();
    for (ECS::ComponentTypeId typeId : arch.GetSignature().GetComponents())
        if (const auto* schema = Scene::SceneSchemaRegistry::ReflectionSchemaForUnhandledType(typeId))
            out.push_back(schema);
}

void CollectEntitySchemas(const std::vector<SchemaPresence>& presence,
                          const std::vector<const Scene::ISceneComponentSchema*>& reflected, const ECS::World& world,
                          ECS::EntityHandle handle, std::vector<const Scene::ISceneComponentSchema*>& out)
{
    out.clear();
    for (const auto& sp : presence)
        if (IsSchemaPresent(sp, world, handle))
            out.push_back(sp.Schema);
    out.insert(out.end(), reflected.begin(), reflected.end());
}
} // namespace

void MissingAssetTracker::Rescan(ECS::World& world, AssetRegistry& registry)
{
    Scene::EnsureBuiltInSchemasRegistered();
    const std::vector<SchemaPresence> presence =
        BuildPresenceTable(Scene::SceneSchemaRegistry::GetAllSorted());

    std::vector<MissingAssetEntry> entries;
    std::unordered_set<GUID> guids;

    // Two path sources, weakest last. The scene's authored path (recorded on the
    // world at load, since components keep only the GUID) names what the
    // reference asked for; the registry's tombstone names what this project last
    // had on disk for that GUID. The tombstones are built once per rescan —
    // GetMissingAssets enumerates the whole store, so it must not run per
    // reference.
    const std::filesystem::path assetRoot = registry.GetAssetRoot();
    std::unordered_map<GUID, std::string> lastKnownPaths;
    for (const auto& mi : registry.GetMissingAssets())
    {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(mi.lastKnownPath, assetRoot, ec);
        lastKnownPaths.emplace(mi.guid,
                               (ec || rel.empty()) ? mi.lastKnownPath.string() : rel.string());
    }
    const ECS::UnresolvedComponentStore* unresolved = world.TryGetUnresolvedComponents();

    std::vector<const Scene::ISceneComponentSchema*> reflected;
    std::vector<const Scene::ISceneComponentSchema*> schemas;
    auto archetypes = world.GetAllArchetypes();
    for (auto* arch : archetypes)
    {
        if (!arch)
            continue;
        AppendReflectionSchemas(*arch, reflected);
        for (const auto& handle : arch->CollectEntities())
        {
            if (!handle.IsValid() || !world.IsValid(handle))
                continue;

            std::string entityTag;
            if (const auto* tag = world.GetComponent<Components::SceneEntityTag>(handle))
                entityTag.assign(tag->View());

            CollectEntitySchemas(presence, reflected, world, handle, schemas);
            for (const auto* schema : schemas)
            {
                schema->EnumerateAssetReferences(world, handle,
                    [&](const GUID& guid,
                        AssetType type,
                        std::string_view authoredPath,
                        std::string_view propertyName)
                    {
                        if (guid.IsNull())
                            return;
                        const GUID canonical = registry.ResolveGuid(guid);
                        AssetMetadata metaUnused;
                        if (registry.TryGetAssetMetadata(canonical, metaUnused))
                            return; // Known to the registry — not missing.

                        if (!guids.insert(canonical).second)
                            return; // Already seen this missing guid; skip duplicate.

                        MissingAssetEntry e{};
                        e.Guid = canonical;
                        e.Type = type;
                        e.AuthoredPath = std::string(authoredPath);
                        if (const std::string* recorded =
                                unresolved ? unresolved->FindUnresolvedReference(canonical.ToString()) : nullptr;
                            recorded && !recorded->empty())
                            e.LastKnownPath = *recorded;
                        else if (const auto tomb = lastKnownPaths.find(canonical);
                                 tomb != lastKnownPaths.end())
                            e.LastKnownPath = tomb->second;
                        e.EntityTag = entityTag;
                        e.ComponentName = std::string(schema->GetComponentName());
                        e.PropertyName = std::string(propertyName);
                        entries.push_back(std::move(e));
                    });
            }
        }
    }

    {
        std::scoped_lock lock(m_Mutex);
        m_Entries = std::move(entries);
        m_MissingGuids = std::move(guids);
        ++m_Version;
    }
    Logger::Log::Info("MissingAssetTracker: rescan complete — {} missing asset reference(s).",
                      m_Entries.size());
    // Invoked outside m_Mutex: a listener is free to call back into the
    // tracker's snapshot accessors, which take the same lock.
    m_Changed.Invoke();
}

size_t MissingAssetTracker::ClearAllReferencesTo(ECS::World& world,
                                                 AssetRegistry& registry,
                                                 const GUID& guid)
{
    if (guid.IsNull())
        return 0;

    Scene::EnsureBuiltInSchemasRegistered();
    const std::vector<SchemaPresence> presence =
        BuildPresenceTable(Scene::SceneSchemaRegistry::GetAllSorted());

    size_t cleared = 0;
    std::vector<const Scene::ISceneComponentSchema*> reflected;
    std::vector<const Scene::ISceneComponentSchema*> schemas;
    auto archetypes = world.GetAllArchetypes();
    for (auto* arch : archetypes)
    {
        if (!arch)
            continue;
        AppendReflectionSchemas(*arch, reflected);
        for (const auto& handle : arch->CollectEntities())
        {
            if (!handle.IsValid() || !world.IsValid(handle))
                continue;
            CollectEntitySchemas(presence, reflected, world, handle, schemas);
            for (const auto* schema : schemas)
            {
                // Collect property names referencing this guid first; mutating
                // the entity during enumeration would invalidate the schema's
                // internal walk.
                std::vector<std::string> propsToClear;
                schema->EnumerateAssetReferences(world, handle,
                    [&](const GUID& g, AssetType, std::string_view, std::string_view propertyName)
                    {
                        if (g == guid)
                            propsToClear.emplace_back(propertyName);
                    });
                Scene::SceneLoadContext stubCtx{};
                for (const auto& prop : propsToClear)
                {
                    std::string err;
                    if (schema->ApplyProperty(world, handle, stubCtx, prop, "0", &err))
                        ++cleared;
                    else
                        Logger::Log::Warning("MissingAssetTracker: failed to clear "
                                             "{}.{} on entity {}: {}",
                                             schema->GetComponentName(), prop, handle.id, err);
                }
            }
        }
    }

    if (cleared > 0)
    {
        Logger::Log::Info("MissingAssetTracker: cleared {} reference(s) to {} from the scene.",
                          cleared, guid.ToString());
        Rescan(world, registry);
    }
    return cleared;
}

void MissingAssetTracker::Bind(EditorContext* ctx)
{
    m_BoundContext = ctx;
}

bool MissingAssetTracker::ResolveActiveTargets(ECS::World*& outWorld,
                                               AssetRegistry*& outRegistry) const
{
    outWorld = nullptr;
    outRegistry = nullptr;
    if (!m_BoundContext)
    {
        Logger::Log::Warning("MissingAssetTracker: not bound to an EditorContext.");
        return false;
    }
    if (!m_BoundContext->World)
    {
        Logger::Log::Warning("MissingAssetTracker: bound context has no active World.");
        return false;
    }
    if (!m_BoundContext->Assets)
    {
        Logger::Log::Warning("MissingAssetTracker: bound context has no AssetManager.");
        return false;
    }
    outWorld = m_BoundContext->World;
    outRegistry = &m_BoundContext->Assets->GetRegistry();
    return true;
}

void MissingAssetTracker::RescanActive()
{
    ECS::World* world = nullptr;
    AssetRegistry* reg = nullptr;
    if (!ResolveActiveTargets(world, reg))
        return;
    Rescan(*world, *reg);
}

size_t MissingAssetTracker::ClearAllReferencesToActive(const GUID& guid)
{
    ECS::World* world = nullptr;
    AssetRegistry* reg = nullptr;
    if (!ResolveActiveTargets(world, reg))
        return 0;
    return ClearAllReferencesTo(*world, *reg, guid);
}

void MissingAssetTracker::Clear()
{
    {
        std::scoped_lock lock(m_Mutex);
        m_Entries.clear();
        m_MissingGuids.clear();
        ++m_Version;
    }
    m_Changed.Invoke();
}

std::vector<MissingAssetEntry> MissingAssetTracker::GetMissing() const
{
    std::scoped_lock lock(m_Mutex);
    return m_Entries;
}

bool MissingAssetTracker::IsAssetMissing(const GUID& guid) const
{
    std::scoped_lock lock(m_Mutex);
    return m_MissingGuids.count(guid) > 0;
}

size_t MissingAssetTracker::GetMissingCount() const
{
    std::scoped_lock lock(m_Mutex);
    return m_Entries.size();
}

uint32_t MissingAssetTracker::GetVersion() const
{
    std::scoped_lock lock(m_Mutex);
    return m_Version;
}

MissingAssetTracker::Subscription MissingAssetTracker::AddListener(Listener cb)
{
    return m_Changed.Subscribe(std::move(cb));
}

} // namespace GameEngine
