#pragma once

#include "ECS/ECS.h"
#include "ECS/ComponentFlags.h"
#include "ECS/Components.h"
#include "ECS/ComponentHandler.h"
#include "ECS/Reflection.h"
#include "ECS/ComponentTypeName.h"
#include "ECS/ModuleRegistration.h"
#include "Logger/Logger.h"
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>

// Forward declaration to avoid circular dependency
namespace GameEngine {
namespace ECS {
    class World;
    class Archetype;
}
}

namespace GameEngine {
namespace ECS {

// Component registration system for automatic component management with handler support
class ComponentRegistry {
public:
    struct ComponentInfo {
        std::string Name;
        std::size_t Size;
        ComponentTypeId TypeId;
        // Owning module + load generation (C12): empty for engine/static
        // registrations. A re-registration from a newer generation of the same
        // module replaces the handler (its reflection metadata must come from
        // the newest mapped image); the loader's abort-purge and unload quiesce ledger
        // select entries by this stamp.
        ModuleRegistrationStamp Module;
        // What the type declares about its on/off state (ComponentFlags.h).
        ComponentFlags Flags = ComponentFlags::None;
        // Byte offset of the bool Enabled field a KeepsOwnEnabledField type switches
        // through; 0 for every other type.
        uint32 EnabledFieldOffset = 0;
    };

private:
    template<Component T>
    static std::unique_ptr<IComponentHandler> CreateHandler() {
        std::optional<std::span<const FieldInfo>> fields;
        if constexpr (HasReflection<T>)
            fields = GetReflectedFields<T>();
        return std::make_unique<ComponentHandler>(ECS::GetComponentTypeId<T>(),
            ComponentTypeNameCStr<T>(), sizeof(T), typeid(T).name(), fields);
    }

    // Copies T's declared ComponentFlags into `info`, with the offset of the field a
    // KeepsOwnEnabledField type switches through.
    template<Component T>
    static void RecordDeclaredFlags(ComponentInfo& info) {
        constexpr ComponentFlags flags = ComponentFlagsOf<T>();
        static_assert(!(HasAnyFlag(flags, ComponentFlags::NotToggleable) &&
                        HasAnyFlag(flags, ComponentFlags::KeepsOwnEnabledField)),
                      "A component with no on/off state cannot keep its own Enabled field");
        info.Flags = flags;
        info.EnabledFieldOffset = 0;
        if constexpr (HasAnyFlag(flags, ComponentFlags::KeepsOwnEnabledField)) {
            static_assert(std::is_same_v<decltype(T::Enabled), bool>,
                          "A KeepsOwnEnabledField component switches through a bool field named Enabled");
            static_assert(std::is_standard_layout_v<T>,
                          "The Enabled field's offset is only defined for a standard-layout component");
            info.EnabledFieldOffset = static_cast<uint32>(offsetof(T, Enabled));
        }
    }

    // Map accessors — defined in ComponentRegistry.cpp with cross-module redirect
    // support. When Engine.lib is linked into both an EXE and a DLL, the DLL's
    // accessors can be redirected to the host's maps via SetExternalRegistryState().
    static std::unordered_map<ComponentTypeId, ComponentInfo>& GetComponents();
    static std::unordered_map<std::string, ComponentTypeId>& GetNameToTypeId();
    static std::unordered_map<ComponentTypeId, std::unique_ptr<IComponentHandler>>& GetHandlers();

public:
    // Cross-module state sharing: returns opaque pointers to the module-local maps.
    static void* GetComponentsPtr();
    static void* GetNameToTypeIdPtr();
    static void* GetHandlersPtr();
    // Redirect all map accesses to an external module's maps (pass results of the
    // Get*Ptr() functions from the host). Pass all nullptr to revert to local maps.
    static void SetExternalRegistryState(void* components, void* nameToTypeId, void* handlers);

    // Register a component type with the registry with validation
    template<Component T>
    static void RegisterComponent(const std::string& name) {
        ComponentTypeId typeId = ECS::GetComponentTypeId<T>();

        // Validation checks
        if (name.empty()) {
            throw std::invalid_argument("[ECS] Component name cannot be empty");
        }

        // Validate component size (empty types are valid but unusual, except the
        // enable-state tags, which are empty by design)
        if constexpr (std::is_empty_v<T> && !kIsEnableStateTag<T>) {
            Logger::Log::Warning("[ECS] Component '{}' is an empty type - this is unusual but valid", name);
        }

        if constexpr (sizeof(T) > 1024) {
            Logger::Log::Warning("[ECS] Component '{}' is very large ({} bytes). Consider using references or handles.",
                           name, sizeof(T));
        }

        // Duplicate registration of the same type id is expected and benign:
        // AutoComponentRegistrar<T>::registered is a template static, and template
        // instantiations are not exported from Engine.dll, so every other image that
        // instantiates it — the host exe, GameEngine.Native, a test binary — runs its
        // own static registration into this one registry and re-sees the entries
        // Engine.dll registered first.
        // typeId is a stable compile-time hash, so a match is genuinely the same type;
        // skip quietly (a real name/id collision is still reported below). Logged at
        // Debug to match the successful-registration log level.
        //
        // Exception (C12): a re-registration from a NEWER load generation of the
        // owning module replaces the handler. Its virtuals live in ECS, but its
        // reflection table views belong to the registering image and must be
        // replaced before the superseded module may be unmapped.
        const ModuleRegistrationStamp& active = GetActiveRegistrationModule();
        if (auto existing = GetComponents().find(typeId); existing != GetComponents().end()) {
            if (!active.Supersedes(existing->second.Module)) {
                Logger::Log::Debug("[ECS] Component '{}' already registered, skipping", name);
                return;
            }
            existing->second.Module = active;
            existing->second.Size = sizeof(T);
            RecordDeclaredFlags<T>(existing->second);
            GetHandlers()[typeId] = CreateHandler<T>();
            Logger::Log::Info("[ECS] Component '{}' handler re-owned by module '{}' generation {} (reload)",
                              name, active.ModuleId, active.Generation);
            RecordModuleTypedDefaults<T>(typeId, name);
            return;
        }

        if (GetNameToTypeId().find(name) != GetNameToTypeId().end()) {
            throw std::invalid_argument("[ECS] Component name '" + name + "' already in use");
        }

        ComponentInfo info;
        info.Name = name;
        info.Size = sizeof(T);
        info.TypeId = typeId;
        info.Module = active;
        RecordDeclaredFlags<T>(info);

        GetComponents()[typeId] = info;
        GetNameToTypeId()[name] = typeId;

        // Create and register the handler for automatic component operations
        GetHandlers()[typeId] = CreateHandler<T>();

        Logger::Log::Debug("[ECS] Registered component: {} (ID: {}, Size: {} bytes) with handler",
                     name, typeId, sizeof(T));
        RecordModuleTypedDefaults<T>(typeId, name);
    }

    // Get component info by type ID
    static const ComponentInfo* GetComponentInfo(ComponentTypeId typeId) {
        auto it = GetComponents().find(typeId);
        return (it != GetComponents().end()) ? &it->second : nullptr;
    }


    // Get component info by name
    static const ComponentInfo* GetComponentInfo(const std::string& name) {
        auto it = GetNameToTypeId().find(name);
        if (it != GetNameToTypeId().end()) {
            return GetComponentInfo(it->second);
        }
        return nullptr;
    }

    // Get component type ID by name
    static ComponentTypeId GetComponentTypeId(const std::string& name) {
        auto it = GetNameToTypeId().find(name);
        return (it != GetNameToTypeId().end()) ? it->second : 0;
    }

    // Get all registered component names
    static std::vector<std::string> GetAllComponentNames() {
        std::vector<std::string> names;
        names.reserve(GetComponents().size());

        for (const auto& [typeId, info] : GetComponents()) {
            names.push_back(info.Name);
        }

        return names;
    }

    // Get total number of registered components
    static std::size_t GetComponentCount() {
        return GetComponents().size();
    }

    // Register a runtime-defined fixed-size blob component.
    // Returns assigned type id on success; 0 on failure.
    static ComponentTypeId RegisterBlobComponent(const std::string& name, std::size_t sizeBytes);

    // The ComponentDisabled tag of a registered component, registered on first
    // use when no typed ComponentDisabled<T> has been (a blob with the tag's
    // name, so its id is the typed tag's id). Returns 0 for an unknown
    // component, for a NotToggleable one and for the enable-state tags
    // themselves.
    static ComponentTypeId RegisterComponentDisabledTag(ComponentTypeId componentType);

    // True for the enable-state tags — Disabled, DisabledInHierarchy and every
    // ComponentDisabled tag, typed or registered by name. They are a state of
    // an entity or a component, not components a user adds or reads.
    static bool IsEnableStateTag(ComponentTypeId typeId);

    // Whether the component's enable state is its ComponentDisabled tag: the type
    // is registered, is not an enable-state tag itself, and declared neither
    // NotToggleable nor KeepsOwnEnabledField (ComponentFlags.h). The scene file's
    // "Token.enabled" line, the editor's section toggle and the debug server's
    // component writes all follow this one answer.
    static bool SwitchesThroughDisabledTag(ComponentTypeId typeId);

    // The registered type whose canonical name is `name`, or ends in "::name"
    // (native) or ".name" (a managed blob), case-insensitively: how a scene token
    // names a hand-written schema's unreflected type. 0 when none matches.
    static ComponentTypeId FindByName(std::string_view name);

    // Test seam: forget one component registration (info + name mapping + handler),
    // simulating a package/module whose types are absent in a fresh session. Live
    // instances in worlds are NOT touched — mirror of the real removal timeline,
    // where unregistration only ever "happens" across a process restart. Returns
    // false if the id isn't registered.
    static bool UnregisterComponentForTests(ComponentTypeId typeId);

    // C12 load-abort purge: drop every registration stamped with exactly
    // {moduleId, generation}. Called (via the loader's purge fan-out) when a
    // module DLL is about to be unmapped after a failed load handshake — its
    // registrars already ran, and their reflection tables belong to the image.
    // Returns the number of registrations dropped.
    static std::size_t PurgeModuleComponents(std::string_view moduleId, std::uint64_t generation);

    // C12 unload quiesce ledger: registrations still attributed to an OLDER
    // generation of `moduleId` (a type the newest load stopped registering).
    // Non-zero blocks unmapping the superseded image: a stale handler's
    // reflection table can still refer to that image.
    static std::size_t CountSupersededModuleComponents(std::string_view moduleId,
                                                       std::uint64_t currentGeneration);

    // Update a registered (blob) component's recorded size after a hot-reload changed its
    // layout. Updates ComponentInfo.Size and the BlobComponentHandler's size. Returns false
    // if the id isn't registered. The caller (World::MigrateComponentLayout) must also resize
    // the archetype columns that already hold the component so size and stride agree.
    static bool UpdateBlobComponentSize(ComponentTypeId typeId, std::size_t newSize);



    // Get component handler by type ID
    static IComponentHandler* GetHandler(ComponentTypeId typeId) {
        auto it = GetHandlers().find(typeId);
        return (it != GetHandlers().end()) ? it->second.get() : nullptr;
    }

    // Get all registered handlers
    static const std::unordered_map<ComponentTypeId, std::unique_ptr<IComponentHandler>>& GetAllHandlers() {
        return GetHandlers();
    }

    // Clear all registered components (for testing)
    static void Clear() {
        GetComponents().clear();
        GetNameToTypeId().clear();
        GetHandlers().clear();
        Logger::Log::Debug("[ECS] Component registry cleared");
    }

private:
    // Module typed components join the hot-reload migration set STRUCTURALLY:
    // recording default-constructed bytes puts the type in ComponentFactory::
    // DefaultByteTypes(), so a reload that changes sizeof(T) is detected and
    // migrated (reset loudly, or carried by name when the module also
    // registers field tables) instead of leaving the archetype stride and the
    // recorded size silently disagreeing — a corruption on the next typed
    // access. Engine (unstamped) registrations stay out: they cannot reload.
    // addable=false — migration coverage, not an Add Component menu entry
    // (modules opt into the menu with an explicit RegisterDefaultBytes).
    template<Component T>
    static void RecordModuleTypedDefaults(ComponentTypeId typeId, const std::string& name) {
        if (!GetActiveRegistrationModule().IsSet())
            return;
        if constexpr (std::is_default_constructible_v<T>) {
            const T defaults{};
            RecordModuleComponentDefaults(typeId, &defaults, sizeof(T));
        } else {
            Logger::Log::Warning(
                "[ECS] Module component '{}' is not default-constructible — it stays OUTSIDE the "
                "hot-reload migration set; a reload that changes its layout will corrupt placed "
                "instances. Make it default-constructible.",
                name);
        }
    }

    // Out-of-line bridge to ComponentFactory::RegisterDefaultBytes — this
    // widely-included header must not pull ComponentFactory.h.
    static void RecordModuleComponentDefaults(ComponentTypeId typeId, const void* defaultBytes,
                                              std::size_t size);
};

} // namespace ECS
} // namespace GameEngine

