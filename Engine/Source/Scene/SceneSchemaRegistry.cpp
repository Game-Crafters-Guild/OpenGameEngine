#include "Scene/SceneSchemaRegistry.h"

#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ModuleRegistration.h"
#include "Logger/Logger.h"
#include "Scene/ReflectionSceneSchema.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace GameEngine::Scene
{
namespace
{
// A registered schema + the module/generation stamp active when it was
// registered (C12): hand-written schema objects live in the registering
// module's image, so the loader's abort-purge must drop them before that image
// unmaps, and a stale-generation entry blocks the image's unload.
struct SchemaEntry
{
    std::unique_ptr<ISceneComponentSchema> Schema;
    ECS::ModuleRegistrationStamp Module;
};

static std::unordered_map<std::string, SchemaEntry>& Schemas()
{
    static std::unordered_map<std::string, SchemaEntry> s;
    return s;
}

// Component types owned through ISceneComponentSchema::GetOwnedComponentType, keyed by type id
// to the owning schema's registry key. Maintained alongside Schemas() under SchemasMutex.
static std::unordered_map<ECS::ComponentTypeId, std::string>& OwnedTypes()
{
    static std::unordered_map<ECS::ComponentTypeId, std::string> s;
    return s;
}

static std::mutex& SchemasMutex()
{
    static std::mutex m;
    return m;
}

static std::string ToKey(std::string_view sv)
{
    std::string s(sv);
    for (auto& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Cache of synthesized reflection schemas, keyed by component type id. Entries are stable for the
// process lifetime so GetComponentName()'s string_view (into the schema's owned m_Name) stays valid.
static std::unordered_map<ECS::ComponentTypeId, std::unique_ptr<ReflectionSceneSchema>>& ReflectionSchemas()
{
    static std::unordered_map<ECS::ComponentTypeId, std::unique_ptr<ReflectionSceneSchema>> s;
    return s;
}

static std::string SimpleName(std::string_view canonical)
{
    // Native canonical names qualify with "::"; managed (C# blob) ones with '.'. Both reduce
    // to the unqualified suffix here because the .scene component token cannot contain '.'
    // (ParseComponentLine splits "Component.prop" at the FIRST dot). Load resolves the simple
    // name back to the full canonical via ComponentFieldRegistry::FindByName suffix matching.
    auto pos = canonical.rfind("::");
    if (pos != std::string_view::npos)
        return std::string(canonical.substr(pos + 2));
    pos = canonical.rfind('.');
    if (pos != std::string_view::npos)
        return std::string(canonical.substr(pos + 1));
    return std::string(canonical);
}

// Scene token (registry key) -> registered component type, filled by ComponentTypeForSchema.
// Maintained under SchemasMutex.
static std::unordered_map<std::string, ECS::ComponentTypeId>& TokenTypes()
{
    static std::unordered_map<std::string, ECS::ComponentTypeId> s_TokenTypes;
    return s_TokenTypes;
}

// Caller must hold SchemasMutex.
static const ISceneComponentSchema* FindForComponentTypeLocked(ECS::ComponentTypeId typeId)
{
    auto& schemas = Schemas();
    if (auto owned = OwnedTypes().find(typeId); owned != OwnedTypes().end())
        if (auto it = schemas.find(owned->second); it != schemas.end())
            return it->second.Schema.get();
    // A reflected type resolves by its reflected name; an unreflected one (physics, the
    // terrain effects) by the name it registered under, whose simple form is its hand-written
    // schema's token.
    std::string_view canon = ECS::ComponentFieldRegistry::GetCanonicalName(typeId);
    if (canon.empty())
        if (const ECS::ComponentRegistry::ComponentInfo* info = ECS::ComponentRegistry::GetComponentInfo(typeId))
            canon = info->Name;
    if (canon.empty())
        return nullptr;
    if (auto it = schemas.find(ToKey(SimpleName(canon))); it != schemas.end())
        return it->second.Schema.get();
    return nullptr;
}

// Caller must hold SchemasMutex. typeId must be reflected (non-empty canonical name).
static const ISceneComponentSchema* GetOrCreateReflectionSchemaLocked(ECS::ComponentTypeId typeId)
{
    auto& cache = ReflectionSchemas();
    auto it = cache.find(typeId);
    if (it != cache.end())
        return it->second.get();
    const std::string_view canon = ECS::ComponentFieldRegistry::GetCanonicalName(typeId);
    auto schema = std::make_unique<ReflectionSceneSchema>(typeId, SimpleName(canon));
    auto* ptr = schema.get();
    cache.emplace(typeId, std::move(schema));
    return ptr;
}
} // namespace

void SceneSchemaRegistry::Register(std::unique_ptr<ISceneComponentSchema> schema)
{
    if (!schema)
        return;
    const std::string key = ToKey(schema->GetComponentName());
    if (key.empty())
        return;

    const ECS::ComponentTypeId ownedType = schema->GetOwnedComponentType();

    std::lock_guard<std::mutex> lk(SchemasMutex());
    auto& m = Schemas();
    auto it = m.find(key);
    if (it != m.end())
    {
        Logger::Log::Warning("SceneSchemaRegistry: schema '{}' already registered; overriding", key);
        if (const ECS::ComponentTypeId previous = it->second.Schema->GetOwnedComponentType())
            OwnedTypes().erase(previous);
        it->second = SchemaEntry{std::move(schema), ECS::GetActiveRegistrationModule()};
    }
    else
    {
        m.emplace(key, SchemaEntry{std::move(schema), ECS::GetActiveRegistrationModule()});
    }
    if (ownedType != 0)
        OwnedTypes()[ownedType] = key;
}

std::size_t SceneSchemaRegistry::PurgeModuleSchemas(std::string_view moduleId, std::uint64_t generation)
{
    if (moduleId.empty())
        return 0;
    std::lock_guard<std::mutex> lk(SchemasMutex());
    auto& m = Schemas();
    std::size_t purged = 0;
    for (auto it = m.begin(); it != m.end();)
    {
        if (it->second.Module.Matches(moduleId, generation))
        {
            Logger::Log::Warning("SceneSchemaRegistry: schema '{}' purged: its module '{}' (generation {}) "
                                 "failed to load and is being unmapped",
                                 it->first, moduleId, generation);
            if (const ECS::ComponentTypeId owned = it->second.Schema->GetOwnedComponentType())
                OwnedTypes().erase(owned);
            it = m.erase(it);
            ++purged;
        }
        else
        {
            ++it;
        }
    }
    return purged;
}

std::size_t SceneSchemaRegistry::CountSupersededModuleSchemas(std::string_view moduleId,
                                                              std::uint64_t currentGeneration)
{
    if (moduleId.empty())
        return 0;
    std::lock_guard<std::mutex> lk(SchemasMutex());
    std::size_t stale = 0;
    for (const auto& [key, entry] : Schemas())
    {
        if (entry.Module.ModuleId == moduleId && entry.Module.Generation < currentGeneration)
        {
            Logger::Log::Warning("SceneSchemaRegistry: schema '{}' still owned by superseded generation {} "
                                 "of module '{}' (current {})",
                                 key, entry.Module.Generation, moduleId, currentGeneration);
            ++stale;
        }
    }
    return stale;
}

const ISceneComponentSchema* SceneSchemaRegistry::Find(std::string_view componentName)
{
    std::lock_guard<std::mutex> lk(SchemasMutex());
    auto& m = Schemas();
    auto it = m.find(ToKey(componentName));
    if (it != m.end())
        return it->second.Schema.get();

    // No hand-written schema under this token: fall back to a reflection schema if the name
    // resolves to a reflected component type (engine or user). This is what makes user components
    // — which never register a hand-written schema — load with no edits to the SceneIO dispatch
    // sites. Components marked [DoNotSerialize] (runtime/derived) resolve to no schema, so
    // SceneIO's load-tolerance skips them; so does a type a hand-written schema owns under another
    // token, whose only schema is that one.
    const ECS::ComponentTypeId tid = ECS::ComponentFieldRegistry::FindByName(componentName);
    if (tid == 0 || ECS::ComponentFieldRegistry::IsComponentDoNotSerialize(tid))
        return nullptr;
    if (FindForComponentTypeLocked(tid) != nullptr)
        return nullptr;
    return GetOrCreateReflectionSchemaLocked(tid);
}

bool SceneSchemaRegistry::HasRegistered(std::string_view componentName)
{
    std::lock_guard<std::mutex> lk(SchemasMutex());
    return Schemas().find(ToKey(componentName)) != Schemas().end();
}

const ISceneComponentSchema* SceneSchemaRegistry::FindForComponentType(ECS::ComponentTypeId typeId)
{
    std::lock_guard<std::mutex> lk(SchemasMutex());
    return FindForComponentTypeLocked(typeId);
}

ECS::ComponentTypeId SceneSchemaRegistry::ComponentTypeForSchema(const ISceneComponentSchema& schema)
{
    if (const ECS::ComponentTypeId owned = schema.GetOwnedComponentType(); owned != 0)
        return owned;
    const std::string key = ToKey(schema.GetComponentName());
    std::lock_guard<std::mutex> lk(SchemasMutex());
    auto& cache = TokenTypes();
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName(schema.GetComponentName());
    if (typeId == 0)
        typeId = ECS::ComponentRegistry::FindByName(schema.GetComponentName());
    if (typeId != 0)
        cache.emplace(key, typeId);
    return typeId;
}

std::string_view SceneSchemaRegistry::SchemaTokenForComponentType(ECS::ComponentTypeId typeId)
{
    if (const ISceneComponentSchema* schema = FindForComponentType(typeId))
        return schema->GetComponentName();
    if (const ISceneComponentSchema* schema = ReflectionSchemaForUnhandledType(typeId))
        return schema->GetComponentName();
    return {};
}

const ISceneComponentSchema* SceneSchemaRegistry::ReflectionSchemaForUnhandledType(ECS::ComponentTypeId typeId)
{
    if (ECS::ComponentFieldRegistry::GetCanonicalName(typeId).empty())
        return nullptr; // not a reflected component
    if (ECS::ComponentFieldRegistry::IsComponentDoNotSerialize(typeId))
        return nullptr; // runtime/derived component — never saved

    std::lock_guard<std::mutex> lk(SchemasMutex());
    if (FindForComponentTypeLocked(typeId) != nullptr)
        return nullptr; // a hand-written schema already serialized this component
    return GetOrCreateReflectionSchemaLocked(typeId);
}

std::vector<const ISceneComponentSchema*> SceneSchemaRegistry::GetAllSorted()
{
    std::vector<const ISceneComponentSchema*> out;
    {
        std::lock_guard<std::mutex> lk(SchemasMutex());
        auto& m = Schemas();
        out.reserve(m.size());
        for (auto& kv : m)
            out.push_back(kv.second.Schema.get());
    }

    std::sort(out.begin(), out.end(), [](const ISceneComponentSchema* a, const ISceneComponentSchema* b)
              {
                  if (!a || !b)
                      return a < b;
                  return a->GetComponentName() < b->GetComponentName();
              });
    return out;
}

} // namespace GameEngine::Scene

