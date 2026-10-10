#pragma once

#include "ECS/ModuleRegistration.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace GameEngine::ECS
{
class SystemScheduleBuilder;
}

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Plugins
{

struct PluginDescriptor
{
    const char* Id = "";
    const char* DisplayName = "";
    const char* Version = "";
    bool EnabledByDefault = true;
};

struct EnginePluginContext
{
    Engine::Renderer::RenderServices* RenderServices = nullptr;
};

class IEnginePlugin
{
public:
    virtual ~IEnginePlugin() = default;

    virtual const PluginDescriptor& GetDescriptor() const = 0;

    virtual void RegisterEngineComponents() {}
    virtual void RegisterSceneSchemas() {}
    virtual void RegisterRenderPipelineNodes(EnginePluginContext& /*context*/) {}
    virtual void AddSystemsToSchedule(ECS::SystemScheduleBuilder& /*builder*/,
                                      EnginePluginContext& /*context*/) {}
    virtual void OnRuntimeInitialized(EnginePluginContext& /*context*/) {}
    virtual void OnRuntimeShutdown() {}
};

class EnginePluginRegistry
{
public:
    using EnabledResolver = std::function<bool(std::string_view pluginId, bool defaultEnabled)>;

    // Invoked by RegisterPlugin for an enabled plugin that registers AFTER the
    // engine's one-shot hook fan-outs already ran — e.g. a package/user DLL
    // loaded at project open, after EnableRenderingLoop built the ECS
    // schedule and fired OnRuntimeInitialized. EngineCore installs a handler
    // that replays the missed hooks for exactly that plugin when the rendering
    // runtime goes live, and clears it again on shutdown. A replace-forward
    // re-registration fires it too — the replacement instance missed every
    // fan-out just like a late plugin. Main thread only (module loads and the
    // registry itself are main-thread).
    using LateRegistrationHandler = std::function<void(IEnginePlugin& plugin)>;

    static EnginePluginRegistry& Get();

    // Replace-forward on same-id re-registration: a rebuilt module DLL
    // re-registers its plugin at load (static init) and the NEW instance takes
    // over all dispatch — the old instance receives no further hooks
    // (including OnRuntimeShutdown). Schedule replay replaces same-owner
    // systems in place and retires names the replacement no longer contributes.
    // A superseded module stays mapped until the quiesce check confirms no
    // registrations still reference it. Re-registering the exact instance
    // already active for its id is an idempotent no-op (no late-replay re-fire).
    void RegisterPlugin(IEnginePlugin& plugin);

    // Detaches the exact instance from all future dispatch. Refused (loud,
    // returns false) when the instance is not the active registration for its
    // id — a stale, already-replaced instance must not knock out its
    // successor. Unregistering does not retire scheduled systems; callers
    // about to unmap a module must also satisfy the module quiesce check.
    bool UnregisterPlugin(IEnginePlugin& plugin);

    // C12 load-abort purge: unregister every plugin stamped with exactly
    // {moduleId, generation} (see ECS/ModuleRegistration.h — RegisterPlugin
    // stamps entries from the loader's active-module bracket). Called before a
    // module that failed its load handshake is unmapped, so the registry never
    // dispatches into a dead image. Returns the number unregistered.
    std::size_t PurgeModulePlugins(std::string_view moduleId, std::uint64_t generation);

    // C12 unload quiesce ledger: active plugins still attributed to an OLDER
    // generation of `moduleId` (the newest load stopped registering that id).
    // Non-zero blocks unmapping the superseded image.
    std::size_t CountSupersededModulePlugins(std::string_view moduleId,
                                             std::uint64_t currentGeneration) const;

    // Ids of the plugins currently attributed to `moduleId` (any generation).
    std::vector<std::string> GetModulePluginIds(std::string_view moduleId) const;

    void SetLateRegistrationHandler(LateRegistrationHandler handler);

    // C12 loader-lock contract: module registrars run during LoadLibrary
    // static init, but the late-registration replay does real engine work
    // (component/schema/system registration, schedule re-solve) that must not
    // run under the OS loader lock — and must never run at all for a DLL that
    // goes on to fail its load handshake. The loader brackets LoadLibrary +
    // handshake with a hold: registrations still mutate the registry
    // immediately, but replays queue and fire on EndRegistrationHold (after
    // the handshake passed, outside the loader lock). PurgeModulePlugins
    // scrubs a purged module's queued replays, so an aborted load replays
    // nothing. EndRegistrationHold is idempotent. Main thread only.
    void BeginRegistrationHold();
    void EndRegistrationHold();

    std::vector<IEnginePlugin*> GetPlugins() const;
    std::vector<IEnginePlugin*> GetEnabledPlugins() const;

    void SetEnabledResolver(EnabledResolver resolver);
    bool IsEnabled(const PluginDescriptor& descriptor) const;
    bool IsEnabled(std::string_view pluginId, bool defaultEnabled = true) const;

    void RegisterEngineComponents();
    void RegisterSceneSchemas();
    void RegisterRenderPipelineNodes(EnginePluginContext& context);
    void OnRuntimeInitialized(EnginePluginContext& context);
    void OnRuntimeShutdown();

private:
    EnginePluginRegistry() = default;

    // Registration + the module/generation stamp active when it was made
    // (empty stamp = statically linked / registered outside a loader bracket).
    struct Entry
    {
        IEnginePlugin* Plugin = nullptr;
        ECS::ModuleRegistrationStamp Module;
    };

    std::vector<Entry> m_Plugins;
    EnabledResolver m_EnabledResolver;
    LateRegistrationHandler m_LateRegistrationHandler;
    // Registration hold (see BeginRegistrationHold): queued replay targets.
    bool m_RegistrationHoldActive = false;
    std::vector<IEnginePlugin*> m_HeldReplays;
};

} // namespace GameEngine::Plugins
