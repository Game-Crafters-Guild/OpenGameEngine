#include "PluginAPI/EnginePlugin.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Plugins
{

EnginePluginRegistry& EnginePluginRegistry::Get()
{
    static EnginePluginRegistry s_Instance;
    return s_Instance;
}

void EnginePluginRegistry::RegisterPlugin(IEnginePlugin& plugin)
{
    const PluginDescriptor& descriptor = plugin.GetDescriptor();
    if (!descriptor.Id || descriptor.Id[0] == '\0')
    {
        Logger::Log::Warning("[Plugins] RegisterPlugin refused: descriptor has an empty id");
        return;
    }

    const auto sameId = [&](const Entry& existing) {
        return existing.Plugin && std::string_view(existing.Plugin->GetDescriptor().Id) == descriptor.Id;
    };
    const auto existing = std::find_if(m_Plugins.begin(), m_Plugins.end(), sameId);
    if (existing != m_Plugins.end())
    {
        if (existing->Plugin == &plugin)
            return; // idempotent re-registration of the active instance — nothing to swap or replay

        Logger::Log::Info(
            "[Plugins] Engine plugin '{}' replaced by a new instance (module re-registration): "
            "the previous instance receives no further hooks (including OnRuntimeShutdown); "
            "the late-registration replay reconciles its scheduled systems onto the new instance",
            descriptor.Id);
        existing->Plugin = &plugin;
        existing->Module = ECS::GetActiveRegistrationModule();
    }
    else
    {
        m_Plugins.push_back(Entry{&plugin, ECS::GetActiveRegistrationModule()});
    }

    if (m_LateRegistrationHandler && IsEnabled(descriptor))
    {
        if (m_RegistrationHoldActive)
        {
            if (std::find(m_HeldReplays.begin(), m_HeldReplays.end(), &plugin) == m_HeldReplays.end())
                m_HeldReplays.push_back(&plugin);
        }
        else
        {
            m_LateRegistrationHandler(plugin);
        }
    }
}

void EnginePluginRegistry::BeginRegistrationHold()
{
    m_RegistrationHoldActive = true;
}

void EnginePluginRegistry::EndRegistrationHold()
{
    if (!m_RegistrationHoldActive)
        return;
    m_RegistrationHoldActive = false;
    std::vector<IEnginePlugin*> held = std::move(m_HeldReplays);
    m_HeldReplays.clear();
    for (IEnginePlugin* plugin : held)
    {
        // Fire only for instances that are still the ACTIVE registration for
        // their id (a purge or a second same-id replacement during the hold
        // superseded the rest) and are still enabled.
        const bool active = std::any_of(m_Plugins.begin(), m_Plugins.end(),
                                        [plugin](const Entry& e) { return e.Plugin == plugin; });
        if (active && m_LateRegistrationHandler && IsEnabled(plugin->GetDescriptor()))
            m_LateRegistrationHandler(*plugin);
    }
}

bool EnginePluginRegistry::UnregisterPlugin(IEnginePlugin& plugin)
{
    const char* id = plugin.GetDescriptor().Id;
    const auto slot = std::find_if(m_Plugins.begin(), m_Plugins.end(),
                                   [&plugin](const Entry& e) { return e.Plugin == &plugin; });
    if (slot == m_Plugins.end())
    {
        Logger::Log::Warning(
            "[Plugins] UnregisterPlugin refused for '{}': instance is not the active registration "
            "(never registered, already replaced, or already unregistered); "
            "active registrations are unaffected",
            id && id[0] != '\0' ? id : "<empty>");
        return false;
    }

    m_Plugins.erase(slot);
    m_HeldReplays.erase(std::remove(m_HeldReplays.begin(), m_HeldReplays.end(), &plugin),
                        m_HeldReplays.end());
    Logger::Log::Warning(
        "[Plugins] Engine plugin '{}' unregistered: it receives no further hooks (including "
        "OnRuntimeShutdown); ECS systems it already scheduled keep ticking until its module's "
        "registrations are reconciled or purged",
        id);
    return true;
}

std::size_t EnginePluginRegistry::PurgeModulePlugins(std::string_view moduleId, std::uint64_t generation)
{
    if (moduleId.empty())
        return 0;
    std::size_t purged = 0;
    for (auto it = m_Plugins.begin(); it != m_Plugins.end();)
    {
        if (it->Module.Matches(moduleId, generation))
        {
            Logger::Log::Warning("[Plugins] Engine plugin '{}' purged: its module '{}' (generation {}) "
                                 "failed to load and is being unmapped",
                                 it->Plugin ? it->Plugin->GetDescriptor().Id : "<null>", it->Module.ModuleId,
                                 generation);
            // A queued replay for the purged instance must never fire — the
            // handler would dispatch into the image being unmapped.
            m_HeldReplays.erase(std::remove(m_HeldReplays.begin(), m_HeldReplays.end(), it->Plugin),
                                m_HeldReplays.end());
            it = m_Plugins.erase(it);
            ++purged;
        }
        else
        {
            ++it;
        }
    }
    return purged;
}

std::size_t EnginePluginRegistry::CountSupersededModulePlugins(std::string_view moduleId,
                                                               std::uint64_t currentGeneration) const
{
    if (moduleId.empty())
        return 0;
    std::size_t stale = 0;
    for (const Entry& entry : m_Plugins)
    {
        if (entry.Module.ModuleId == moduleId && entry.Module.Generation < currentGeneration)
        {
            Logger::Log::Warning("[Plugins] Engine plugin '{}' still registered by superseded generation {} "
                                 "of module '{}' (current {}): the newest load no longer registers it",
                                 entry.Plugin ? entry.Plugin->GetDescriptor().Id : "<null>",
                                 entry.Module.Generation, moduleId, currentGeneration);
            ++stale;
        }
    }
    return stale;
}

std::vector<std::string> EnginePluginRegistry::GetModulePluginIds(std::string_view moduleId) const
{
    std::vector<std::string> ids;
    for (const Entry& entry : m_Plugins)
    {
        if (entry.Plugin && entry.Module.ModuleId == moduleId)
            ids.emplace_back(entry.Plugin->GetDescriptor().Id);
    }
    return ids;
}

void EnginePluginRegistry::SetLateRegistrationHandler(LateRegistrationHandler handler)
{
    m_LateRegistrationHandler = std::move(handler);
}

std::vector<IEnginePlugin*> EnginePluginRegistry::GetPlugins() const
{
    std::vector<IEnginePlugin*> out;
    out.reserve(m_Plugins.size());
    for (const Entry& entry : m_Plugins)
        out.push_back(entry.Plugin);
    return out;
}

std::vector<IEnginePlugin*> EnginePluginRegistry::GetEnabledPlugins() const
{
    std::vector<IEnginePlugin*> out;
    out.reserve(m_Plugins.size());
    for (const Entry& entry : m_Plugins)
    {
        if (entry.Plugin && IsEnabled(entry.Plugin->GetDescriptor()))
            out.push_back(entry.Plugin);
    }
    return out;
}

void EnginePluginRegistry::SetEnabledResolver(EnabledResolver resolver)
{
    m_EnabledResolver = std::move(resolver);
}

bool EnginePluginRegistry::IsEnabled(const PluginDescriptor& descriptor) const
{
    return IsEnabled(descriptor.Id ? std::string_view(descriptor.Id) : std::string_view{},
                     descriptor.EnabledByDefault);
}

bool EnginePluginRegistry::IsEnabled(std::string_view pluginId, bool defaultEnabled) const
{
    if (pluginId.empty())
        return false;
    if (!m_EnabledResolver)
        return defaultEnabled;
    return m_EnabledResolver(pluginId, defaultEnabled);
}

void EnginePluginRegistry::RegisterEngineComponents()
{
    for (IEnginePlugin* plugin : GetEnabledPlugins())
        plugin->RegisterEngineComponents();
}

void EnginePluginRegistry::RegisterSceneSchemas()
{
    for (IEnginePlugin* plugin : GetEnabledPlugins())
        plugin->RegisterSceneSchemas();
}

void EnginePluginRegistry::RegisterRenderPipelineNodes(EnginePluginContext& context)
{
    for (IEnginePlugin* plugin : GetEnabledPlugins())
        plugin->RegisterRenderPipelineNodes(context);
}

void EnginePluginRegistry::OnRuntimeInitialized(EnginePluginContext& context)
{
    for (IEnginePlugin* plugin : GetEnabledPlugins())
        plugin->OnRuntimeInitialized(context);
}

void EnginePluginRegistry::OnRuntimeShutdown()
{
    for (IEnginePlugin* plugin : GetEnabledPlugins())
        plugin->OnRuntimeShutdown();
}

} // namespace GameEngine::Plugins
