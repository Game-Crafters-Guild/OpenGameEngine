#include "ECS/ComponentRegistry.h"
#include "ECS/BlobComponent.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ECS.h"
#include "ECS/Entity.h"  // For World class definition
#include "ECS/World.h"   // For Archetype class definition
#include "Logger/Logger.h"

#include <atomic>
#include <cctype>

namespace GameEngine {
namespace ECS {

// ---------------------------------------------------------------------------
// Cross-module redirect pointers.
// When non-null, map accessors return the external (host) module's maps
// instead of the module-local statics, ensuring a single shared registry
// across EXE and DLL that both statically link Engine.lib.
// ---------------------------------------------------------------------------
namespace {

using ComponentsMap   = std::unordered_map<ComponentTypeId, ComponentRegistry::ComponentInfo>;
using NamesMap        = std::unordered_map<std::string, ComponentTypeId>;
using HandlersMap     = std::unordered_map<ComponentTypeId, std::unique_ptr<IComponentHandler>>;

static std::atomic<ComponentsMap*> g_externalComponents{nullptr};
static std::atomic<NamesMap*>      g_externalNames{nullptr};
static std::atomic<HandlersMap*>   g_externalHandlers{nullptr};

// Only this handler's dispatch belongs to the engine rather than a native
// module. GetHandler handles absent registrations; dynamic_cast also accepts
// null, so both ownership refresh and resize use the same nullable lookup.
BlobComponentHandler* FindEngineOwnedBlobHandler(ComponentTypeId typeId)
{
    return dynamic_cast<BlobComponentHandler*>(ComponentRegistry::GetHandler(typeId));
}


} // namespace

// ---------------------------------------------------------------------------
// Map accessors with redirect support
//
// AutoComponentRegistrar<T> runs during static initialization (its `registered`
// initializer fires from initterm in every module that instantiates it), so the
// registry storage MUST be construct-on-first-use: keep the maps as function-
// local statics behind these accessors — never file-scope or class-scope
// statics, whose unordered dynamic init races the registrars.
// ---------------------------------------------------------------------------
std::unordered_map<ComponentTypeId, ComponentRegistry::ComponentInfo>& ComponentRegistry::GetComponents()
{
    auto* ext = g_externalComponents.load(std::memory_order_acquire);
    if (ext) return *ext;
    static ComponentsMap s_Components;
    return s_Components;
}

std::unordered_map<std::string, ComponentTypeId>& ComponentRegistry::GetNameToTypeId()
{
    auto* ext = g_externalNames.load(std::memory_order_acquire);
    if (ext) return *ext;
    static NamesMap s_NameToTypeId;
    return s_NameToTypeId;
}

std::unordered_map<ComponentTypeId, std::unique_ptr<IComponentHandler>>& ComponentRegistry::GetHandlers()
{
    auto* ext = g_externalHandlers.load(std::memory_order_acquire);
    if (ext) return *ext;
    static HandlersMap s_Handlers;
    return s_Handlers;
}

// ---------------------------------------------------------------------------
// Public cross-module state sharing API
// ---------------------------------------------------------------------------
void* ComponentRegistry::GetComponentsPtr()   { return &GetComponents(); }
void* ComponentRegistry::GetNameToTypeIdPtr()  { return &GetNameToTypeId(); }
void* ComponentRegistry::GetHandlersPtr()      { return &GetHandlers(); }

void ComponentRegistry::SetExternalRegistryState(void* components, void* nameToTypeId, void* handlers)
{
    g_externalComponents.store(static_cast<ComponentsMap*>(components), std::memory_order_release);
    g_externalNames.store(static_cast<NamesMap*>(nameToTypeId), std::memory_order_release);
    g_externalHandlers.store(static_cast<HandlersMap*>(handlers), std::memory_order_release);
}

ComponentTypeId ComponentRegistry::RegisterComponentDisabledTag(ComponentTypeId componentType)
{
    const ComponentInfo* info = GetComponentInfo(componentType);
    if (!info || HasAnyFlag(info->Flags, ComponentFlags::NotToggleable) ||
        componentType == ECS::GetComponentTypeId<Disabled>() ||
        componentType == ECS::GetComponentTypeId<DisabledInHierarchy>() ||
        std::string_view(info->Name).starts_with(kComponentDisabledNamePrefix))
        return 0;

    const ComponentTypeId disabledTag = ComponentDisabledTypeId(info->Name);
    if (GetHandler(disabledTag) != nullptr)
        return disabledTag;

    std::string tagName;
    tagName.reserve(kComponentDisabledNamePrefix.size() + info->Name.size() + kComponentDisabledNameSuffix.size());
    tagName.append(kComponentDisabledNamePrefix).append(info->Name).append(kComponentDisabledNameSuffix);
    // The size of every empty tag struct, so this blob and a later typed
    // ComponentDisabled<T> agree on the column.
    return RegisterBlobComponent(tagName, sizeof(Disabled));
}

bool ComponentRegistry::IsEnableStateTag(ComponentTypeId typeId)
{
    if (typeId == ECS::GetComponentTypeId<Disabled>() || typeId == ECS::GetComponentTypeId<DisabledInHierarchy>())
        return true;
    const ComponentInfo* info = GetComponentInfo(typeId);
    return info && std::string_view(info->Name).starts_with(kComponentDisabledNamePrefix);
}

bool ComponentRegistry::SwitchesThroughDisabledTag(ComponentTypeId typeId)
{
    const ComponentInfo* info = GetComponentInfo(typeId);
    if (!info || IsEnableStateTag(typeId))
        return false;
    return !HasAnyFlag(info->Flags, ComponentFlags::NotToggleable | ComponentFlags::KeepsOwnEnabledField);
}

namespace
{
bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
} // namespace

ComponentTypeId ComponentRegistry::FindByName(std::string_view name)
{
    if (name.empty())
        return 0;
    for (const auto& [typeId, info] : GetComponents())
    {
        const std::string_view canonical = info.Name;
        if (EqualsIgnoreCase(canonical, name))
            return typeId;
        if (canonical.size() <= name.size() ||
            !EqualsIgnoreCase(canonical.substr(canonical.size() - name.size()), name))
            continue;
        const std::string_view scope = canonical.substr(0, canonical.size() - name.size());
        if (scope.ends_with("::") || scope.ends_with('.'))
            return typeId;
    }
    return 0;
}

ComponentTypeId ComponentRegistry::RegisterBlobComponent(const std::string& name, std::size_t sizeBytes)
{
    if (name.empty() || sizeBytes == 0)
        return 0;

    // Check if already registered by name.
    if (auto it = GetNameToTypeId().find(name); it != GetNameToTypeId().end())
    {
        auto existingId = it->second;
        auto infoIt = GetComponents().find(existingId);
        if (infoIt != GetComponents().end())
        {
            if (infoIt->second.Size != sizeBytes)
            {
                // Two definitions of the same component name with different layouts:
                // the caller's reads/writes would corrupt chunk memory. Refuse the
                // registration and make the mismatch impossible to miss.
                Logger::Log::Error("[ECS] Blob component '{}' registration REJECTED: name already "
                                   "registered with size {} but caller declared size {}. Two components "
                                   "share a fully-qualified name with different layouts — rename one "
                                   "(namespaces disambiguate) or align the struct layouts.",
                                   name, infoIt->second.Size, sizeBytes);
                return 0;
            }
            // Blob handlers hold no reflection table from the registering module.
            // A C++ handler may still view the old module's table; its typed
            // registrar must replace that view before clearing the pin.
            const bool hasEngineOwnedDispatch = FindEngineOwnedBlobHandler(existingId) != nullptr;
            if (GetActiveRegistrationModule().Supersedes(infoIt->second.Module) && hasEngineOwnedDispatch)
                infoIt->second.Module = GetActiveRegistrationModule();
        }
        return existingId;
    }

    // Derive a stable type id from the runtime name via the same Hash64 used
    // for compile-time component identity. Identical on every DLL / platform.
    ComponentTypeId typeId = Hash64(name);
    if (typeId == 0)
        return 0;

    // Prevent collisions with already-registered ids (a Hash64 collision with a
    // different name, or a blob name colliding with a native component's hash).
    if (auto existing = GetComponents().find(typeId); existing != GetComponents().end())
    {
        Logger::Log::Error("[ECS] Blob component '{}' registration REJECTED: type id {} is already "
                           "registered as '{}' — Hash64 identity collision; rename the component.",
                           name, typeId, existing->second.Name);
        return 0;
    }

    ComponentInfo info;
    info.Name = name;
    info.Size = sizeBytes;
    info.TypeId = typeId;
    info.Module = GetActiveRegistrationModule();

    GetComponents()[typeId] = info;
    GetNameToTypeId()[name] = typeId;
    GetHandlers()[typeId] = std::make_unique<BlobComponentHandler>(typeId, name, sizeBytes);

    Logger::Log::Info("[ECS] Registered blob component '{}' (ID: {}, Size: {} bytes)", name, typeId, sizeBytes);
    return typeId;
}

bool ComponentRegistry::UnregisterComponentForTests(ComponentTypeId typeId)
{
    auto& comps = GetComponents();
    auto infoIt = comps.find(typeId);
    if (infoIt == comps.end())
        return false;
    GetNameToTypeId().erase(infoIt->second.Name);
    GetHandlers().erase(typeId);
    comps.erase(infoIt);
    return true;
}

std::size_t ComponentRegistry::PurgeModuleComponents(std::string_view moduleId, std::uint64_t generation)
{
    if (moduleId.empty())
        return 0;
    auto& comps = GetComponents();
    std::size_t purged = 0;
    for (auto it = comps.begin(); it != comps.end();)
    {
        if (it->second.Module.Matches(moduleId, generation))
        {
            Logger::Log::Warning("[ECS] Component '{}' purged: its module '{}' (generation {}) failed to "
                                 "load and is being unmapped",
                                 it->second.Name, it->second.Module.ModuleId, generation);
            GetNameToTypeId().erase(it->second.Name);
            GetHandlers().erase(it->first);
            it = comps.erase(it);
            ++purged;
        }
        else
        {
            ++it;
        }
    }
    return purged;
}

std::size_t ComponentRegistry::CountSupersededModuleComponents(std::string_view moduleId,
                                                               std::uint64_t currentGeneration)
{
    if (moduleId.empty())
        return 0;
    std::size_t stale = 0;
    for (const auto& [typeId, info] : GetComponents())
    {
        if (info.Module.ModuleId == moduleId && info.Module.Generation < currentGeneration)
        {
            Logger::Log::Warning("[ECS] Component '{}' still owned by superseded generation {} of module "
                                 "'{}' (current {}): the newest load no longer registers it",
                                 info.Name, info.Module.Generation, moduleId, currentGeneration);
            ++stale;
        }
    }
    return stale;
}

void ComponentRegistry::RecordModuleComponentDefaults(ComponentTypeId typeId, const void* defaultBytes,
                                                      std::size_t size)
{
    ComponentFactory::RegisterDefaultBytes(typeId, defaultBytes, size, /*addable=*/false);
}

bool ComponentRegistry::UpdateBlobComponentSize(ComponentTypeId typeId, std::size_t newSize)
{
    auto& comps = GetComponents();
    auto infoIt = comps.find(typeId);
    if (infoIt == comps.end())
        return false;

    infoIt->second.Size = newSize;

    if (auto* blob = FindEngineOwnedBlobHandler(typeId))
        blob->SetSize(newSize);
    return true;
}

} // namespace ECS
} // namespace GameEngine
