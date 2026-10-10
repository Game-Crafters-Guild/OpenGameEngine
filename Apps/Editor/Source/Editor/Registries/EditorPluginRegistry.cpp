#include "Editor/Registries/EditorPluginRegistry.h"

#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

EditorPluginRegistry& EditorPluginRegistry::Get()
{
    static EditorPluginRegistry s_Instance;
    return s_Instance;
}

void EditorPluginRegistry::RegisterPlugin(IEditorPlugin& plugin)
{
    const Plugins::PluginDescriptor& descriptor = plugin.GetDescriptor();
    if (!descriptor.Id || descriptor.Id[0] == '\0')
        return;

    // Replace-forward: a module rebuild re-registers under the same id and the
    // NEW plugin instance wins. The previous instance's DLL stays mapped for
    // the process lifetime (editor-kind modules refuse the C12 unload), so
    // swapping the pointer is safe and stale code stops being dispatched.
    const auto sameId = [&](const Entry& existing) {
        return existing.Plugin && std::string_view(existing.Plugin->GetDescriptor().Id) == descriptor.Id;
    };
    const auto existing = std::find_if(m_Plugins.begin(), m_Plugins.end(), sameId);
    if (existing != m_Plugins.end())
    {
        if (existing->Plugin != &plugin)
            Logger::Log::Info("[Plugins] Editor plugin '{}' replaced by a new instance (module "
                              "re-registration): the previous instance is no longer dispatched",
                              descriptor.Id);
        existing->Plugin = &plugin;
        existing->Module = ECS::GetActiveRegistrationModule();
    }
    else
    {
        m_Plugins.push_back(Entry{&plugin, ECS::GetActiveRegistrationModule()});
    }

    // A (re-)registration may carry a different EnabledByDefault for this id,
    // and it changes the contributor set either way.
    if (const auto cached = m_EnabledCache.find(std::string_view(descriptor.Id));
        cached != m_EnabledCache.end())
        m_EnabledCache.erase(cached);
    m_MaskContributorsDirty = true;

    // Editor-kind package modules register at project open — after the
    // editor's startup inspector pass. Replay it so their inspectors exist
    // without waiting for a pass that already ran.
    if (m_InspectorsRegistered && IsEnabled(descriptor))
        plugin.RegisterInspectors();
}

std::vector<IEditorPlugin*> EditorPluginRegistry::GetPlugins() const
{
    std::vector<IEditorPlugin*> out;
    out.reserve(m_Plugins.size());
    for (const Entry& entry : m_Plugins)
        out.push_back(entry.Plugin);
    return out;
}

std::vector<IEditorPlugin*> EditorPluginRegistry::GetEnabledPlugins() const
{
    std::vector<IEditorPlugin*> out;
    out.reserve(m_Plugins.size());
    for (const Entry& entry : m_Plugins)
    {
        if (entry.Plugin && IsEnabled(entry.Plugin->GetDescriptor()))
            out.push_back(entry.Plugin);
    }
    return out;
}

void EditorPluginRegistry::AppendModulePins(std::string_view moduleId,
                                            std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    for (const Entry& entry : m_Plugins)
    {
        if (entry.Plugin && entry.Module.ModuleId == moduleId)
            outPins.push_back(std::string("editor plugin '") + entry.Plugin->GetDescriptor().Id + "'");
    }
}

void EditorPluginRegistry::SetEnabledResolver(EnabledResolver resolver)
{
    m_EnabledResolver = std::move(resolver);
    InvalidateEnabledCache();
}

void EditorPluginRegistry::InvalidateEnabledCache()
{
    m_EnabledCache.clear();
    m_MaskContributorsDirty = true;
}

bool EditorPluginRegistry::IsEnabled(const Plugins::PluginDescriptor& descriptor) const
{
    return IsEnabled(descriptor.Id ? std::string_view(descriptor.Id) : std::string_view{},
                     descriptor.EnabledByDefault);
}

bool EditorPluginRegistry::IsEnabled(std::string_view pluginId, bool defaultEnabled) const
{
    if (pluginId.empty())
        return false;
    if (!m_EnabledResolver)
        return defaultEnabled;
    // Memoized: the resolver reads the project settings file from disk, and
    // callers include per-entity editor paths. InvalidateEnabledCache is the
    // re-resolution point.
    if (const auto it = m_EnabledCache.find(pluginId); it != m_EnabledCache.end())
        return it->second;
    const bool enabled = m_EnabledResolver(pluginId, defaultEnabled);
    m_EnabledCache.emplace(std::string(pluginId), enabled);
    return enabled;
}

void EditorPluginRegistry::RegisterInspectors()
{
    m_InspectorsRegistered = true;
    for (IEditorPlugin* plugin : GetEnabledPlugins())
        plugin->RegisterInspectors();
}

void EditorPluginRegistry::BuildHierarchyContextMenu(ContextMenuBuilder& builder,
                                                     const HierarchyContextMenuContext& context)
{
    for (IEditorPlugin* plugin : GetEnabledPlugins())
        plugin->BuildHierarchyContextMenu(builder, context);
}

bool EditorPluginRegistry::HandleHierarchyCommand(uint32_t commandId,
                                                  const HierarchyCommandContext& context)
{
    for (IEditorPlugin* plugin : GetEnabledPlugins())
    {
        if (plugin->HandleHierarchyCommand(commandId, context))
            return true;
    }
    return false;
}

const std::vector<IEditorPlugin*>& EditorPluginRegistry::MaskContributors() const
{
    if (m_MaskContributorsDirty)
    {
        m_MaskContributorsDirty = false;
        m_MaskContributors.clear();
        for (const Entry& entry : m_Plugins)
        {
            if (entry.Plugin && entry.Plugin->ContributesSelectionMask() &&
                IsEnabled(entry.Plugin->GetDescriptor()))
                m_MaskContributors.push_back(entry.Plugin);
        }
    }
    return m_MaskContributors;
}

void EditorPluginRegistry::CollectSelectionMaskParts(ECS::World& world,
                                                     ECS::EntityHandle entity,
                                                     std::vector<SelectionMaskPart>& outParts)
{
    // Runs per hovered/selected entity: consult only the cached
    // enabled-and-contributing plugins, and bail before any component lookup
    // when there are none (the common all-static case).
    const std::vector<IEditorPlugin*>& contributors = MaskContributors();
    if (contributors.empty())
        return;

    // An entity that is not active in the hierarchy (switched off, or under a
    // switched-off parent) renders nothing, so it contributes no outline mask
    // parts. Enforced at the dispatch so every plugin inherits the contract
    // instead of re-implementing entity-enabled semantics.
    if (world.IsValid(entity) && !ECS::Entity(&world, entity).IsEnabledInHierarchy())
        return;

    for (IEditorPlugin* plugin : contributors)
        plugin->CollectSelectionMaskParts(world, entity, outParts);
}

} // namespace GameEngine::Editor
