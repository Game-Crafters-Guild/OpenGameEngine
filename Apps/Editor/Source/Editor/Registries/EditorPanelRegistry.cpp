#include "Editor/Registries/EditorPanelRegistry.h"

#include "Logger/Logger.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

EditorPanelRegistry& EditorPanelRegistry::Get()
{
    static EditorPanelRegistry s_Instance;
    return s_Instance;
}

void EditorPanelRegistry::RegisterPanel(EditorPanelDescriptor descriptor)
{
    if (descriptor.PanelId.empty() || !descriptor.Factory)
    {
        Logger::Log::Error("EditorPanelRegistry: rejecting panel registration without {} "
                           "(title '{}')",
                           descriptor.PanelId.empty() ? "a PanelId" : "a Factory",
                           descriptor.Title);
        return;
    }

    const auto sameId = [&](const PanelEntry& existing) {
        return existing.Descriptor.PanelId == descriptor.PanelId;
    };
    const auto existing = std::find_if(m_Panels.begin(), m_Panels.end(), sameId);
    if (existing != m_Panels.end())
    {
        // Replace-forward: the previous descriptor's module DLL stays mapped
        // for the process lifetime, so swapping the descriptor (factory
        // included) is safe and stale factories stop being consumed.
        Logger::Log::Info("EditorPanelRegistry: panel '{}' re-registered (module reload); "
                          "replacing forward",
                          descriptor.PanelId);
        existing->Descriptor = descriptor;
        existing->Module = ECS::GetActiveRegistrationModule();
    }
    else
    {
        m_Panels.push_back(PanelEntry{descriptor, ECS::GetActiveRegistrationModule()});
    }

    if (m_Consumers.Panel)
        m_Consumers.Panel(descriptor);
}

void EditorPanelRegistry::RegisterEditorStyleSheet(EditorStyleSheetContribution contribution)
{
    if (contribution.AssetSourceAlias.empty() || contribution.StyleAssetPath.empty())
    {
        Logger::Log::Error(
            "EditorPanelRegistry: rejecting editor stylesheet contribution with empty "
            "alias/path ('{}':'{}')",
            contribution.AssetSourceAlias, contribution.StyleAssetPath);
        return;
    }

    const auto sameSheet = [&](const StyleSheetEntry& existing) {
        return existing.Contribution.AssetSourceAlias == contribution.AssetSourceAlias &&
               existing.Contribution.StyleAssetPath == contribution.StyleAssetPath;
    };
    if (std::none_of(m_StyleSheets.begin(), m_StyleSheets.end(), sameSheet))
        m_StyleSheets.push_back(StyleSheetEntry{contribution, ECS::GetActiveRegistrationModule()});
    // Re-registration (module reload) still notifies the consumer: the global
    // stylesheet attach downstream is idempotent, and a reloaded asset needs
    // the re-poke.

    if (m_Consumers.StyleSheet)
        m_Consumers.StyleSheet(contribution);
}

void EditorPanelRegistry::RegisterOverlay(EditorOverlayDescriptor descriptor)
{
    if (descriptor.OverlayId.empty() || !descriptor.Factory)
    {
        Logger::Log::Error("EditorPanelRegistry: rejecting overlay registration without {}",
                           descriptor.OverlayId.empty()
                               ? "an OverlayId"
                               : ("a Factory ('" + descriptor.OverlayId + "')"));
        return;
    }

    const auto sameId = [&](const OverlayEntry& existing) {
        return existing.Descriptor.OverlayId == descriptor.OverlayId;
    };
    const auto existing = std::find_if(m_Overlays.begin(), m_Overlays.end(), sameId);
    if (existing != m_Overlays.end())
    {
        // Replace-forward like panels: the fresh descriptor wins for future
        // consumption; whether an already-attached element is swapped is the
        // consumer's policy (the editor keeps the existing attach — overlays
        // hold live import state a hot reload must not tear down).
        Logger::Log::Info("EditorPanelRegistry: overlay '{}' re-registered (module reload); "
                          "replacing forward",
                          descriptor.OverlayId);
        existing->Descriptor = descriptor;
        existing->Module = ECS::GetActiveRegistrationModule();
    }
    else
    {
        m_Overlays.push_back(OverlayEntry{descriptor, ECS::GetActiveRegistrationModule()});
    }

    if (m_Consumers.Overlay)
        m_Consumers.Overlay(descriptor);
}

void EditorPanelRegistry::RegisterPanelType(EditorPanelTypeDescriptor descriptor)
{
    if (descriptor.TypeKey.empty() || !descriptor.Factory)
    {
        Logger::Log::Error("EditorPanelRegistry: rejecting panel type registration without {}",
                           descriptor.TypeKey.empty()
                               ? "a TypeKey"
                               : ("a Factory ('" + descriptor.TypeKey + "')"));
        return;
    }

    const auto existing = std::find_if(m_PanelTypes.begin(), m_PanelTypes.end(), [&](const PanelTypeEntry& e) {
        return e.Descriptor.TypeKey == descriptor.TypeKey;
    });
    if (existing != m_PanelTypes.end())
    {
        existing->Descriptor = std::move(descriptor);
        existing->Module = ECS::GetActiveRegistrationModule();
        return;
    }
    m_PanelTypes.push_back(PanelTypeEntry{std::move(descriptor), ECS::GetActiveRegistrationModule()});
}

std::unique_ptr<UIElement> EditorPanelRegistry::CreatePanelOfType(std::string_view typeKey) const
{
    const auto it = std::find_if(m_PanelTypes.begin(), m_PanelTypes.end(),
                                 [&](const PanelTypeEntry& e) { return e.Descriptor.TypeKey == typeKey; });
    if (it == m_PanelTypes.end())
        return nullptr;
    return it->Descriptor.Factory();
}

bool EditorPanelRegistry::TryGetPanel(std::string_view panelId,
                                      EditorPanelDescriptor& outDescriptor) const
{
    const auto it = std::find_if(m_Panels.begin(), m_Panels.end(),
                                 [&](const PanelEntry& e) { return e.Descriptor.PanelId == panelId; });
    if (it == m_Panels.end())
        return false;
    outDescriptor = it->Descriptor;
    return true;
}

std::vector<EditorPanelDescriptor> EditorPanelRegistry::PanelSnapshot() const
{
    std::vector<EditorPanelDescriptor> out;
    out.reserve(m_Panels.size());
    for (const PanelEntry& entry : m_Panels)
        out.push_back(entry.Descriptor);
    return out;
}

std::vector<EditorStyleSheetContribution> EditorPanelRegistry::StyleSheetSnapshot() const
{
    std::vector<EditorStyleSheetContribution> out;
    out.reserve(m_StyleSheets.size());
    for (const StyleSheetEntry& entry : m_StyleSheets)
        out.push_back(entry.Contribution);
    return out;
}

std::vector<EditorOverlayDescriptor> EditorPanelRegistry::OverlaySnapshot() const
{
    std::vector<EditorOverlayDescriptor> out;
    out.reserve(m_Overlays.size());
    for (const OverlayEntry& entry : m_Overlays)
        out.push_back(entry.Descriptor);
    return out;
}

void EditorPanelRegistry::AppendModulePins(std::string_view moduleId,
                                           std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    for (const PanelEntry& entry : m_Panels)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("dockable panel '" + entry.Descriptor.PanelId + "'");
    }
    for (const StyleSheetEntry& entry : m_StyleSheets)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("editor stylesheet '" + entry.Contribution.AssetSourceAlias + ":" +
                              entry.Contribution.StyleAssetPath + "'");
    }
    for (const OverlayEntry& entry : m_Overlays)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("ui overlay '" + entry.Descriptor.OverlayId + "'");
    }
    for (const PanelTypeEntry& entry : m_PanelTypes)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("panel type '" + entry.Descriptor.TypeKey + "'");
    }
}

void EditorPanelRegistry::SetPanelOpener(PanelOpener opener)
{
    m_PanelOpener = std::move(opener);
}

void EditorPanelRegistry::OpenPanel(std::string_view panelId) const
{
    if (!m_PanelOpener)
    {
        Logger::Log::Error("EditorPanelRegistry: OpenPanel('{}') before the editor installed an "
                           "opener; ignored",
                           panelId);
        return;
    }
    m_PanelOpener(std::string(panelId));
}

void EditorPanelRegistry::SetConsumers(Consumers consumers)
{
    m_Consumers = std::move(consumers);

    // Replay for entries that registered before the consumer existed, so
    // attach order never decides whether a panel appears.
    if (m_Consumers.StyleSheet)
        for (const StyleSheetEntry& entry : m_StyleSheets)
            m_Consumers.StyleSheet(entry.Contribution);
    if (m_Consumers.Panel)
        for (const PanelEntry& entry : m_Panels)
            m_Consumers.Panel(entry.Descriptor);
    if (m_Consumers.Overlay)
        for (const OverlayEntry& entry : m_Overlays)
            m_Consumers.Overlay(entry.Descriptor);
}

} // namespace GameEngine::Editor
