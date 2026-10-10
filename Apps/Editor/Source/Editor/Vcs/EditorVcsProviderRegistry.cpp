#include "Editor/Vcs/EditorVcsProviderRegistry.h"

#include "Logger/Logger.h"
#include "VCSIntegration/IVCSIntegration.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

EditorVcsProviderRegistry& EditorVcsProviderRegistry::Get()
{
    static EditorVcsProviderRegistry s_Instance;
    return s_Instance;
}

void EditorVcsProviderRegistry::RegisterProvider(EditorVcsProviderDescriptor descriptor)
{
    if (descriptor.TypeId.empty() || descriptor.DisplayName.empty() || !descriptor.Detect ||
        !descriptor.Integration || !descriptor.Initialize)
    {
        Logger::Log::Error(
            "EditorVcsProviderRegistry: rejecting provider registration '{}' — TypeId, "
            "DisplayName, Detect, Integration and Initialize are all required",
            descriptor.TypeId.empty() ? descriptor.DisplayName : descriptor.TypeId);
        return;
    }

    m_ModuleOwners[descriptor.TypeId] = ECS::GetActiveRegistrationModule();

    IVCSIntegration* replaced = nullptr;
    const auto sameId = [&](const EditorVcsProviderDescriptor& existing) {
        return existing.TypeId == descriptor.TypeId;
    };
    const auto existing = std::find_if(m_Providers.begin(), m_Providers.end(), sameId);
    if (existing != m_Providers.end())
    {
        // Replace-forward: the previous module DLL stays mapped for the
        // process lifetime, so swapping the descriptor is safe. The observer
        // gets the old integration to disconnect it loudly.
        Logger::Log::Info("EditorVcsProviderRegistry: provider '{}' re-registered (module "
                          "reload); replacing forward",
                          descriptor.TypeId);
        replaced = &existing->Integration();
        *existing = descriptor;
    }
    else
    {
        m_Providers.push_back(descriptor);
    }

    // Keep detection order stable and load-order independent.
    std::stable_sort(m_Providers.begin(), m_Providers.end(),
                     [](const EditorVcsProviderDescriptor& a, const EditorVcsProviderDescriptor& b) {
                         if (a.DetectionOrder != b.DetectionOrder)
                             return a.DetectionOrder < b.DetectionOrder;
                         return a.TypeId < b.TypeId;
                     });

    if (m_Observer)
        m_Observer(descriptor, replaced);
}

std::vector<EditorVcsProviderDescriptor> EditorVcsProviderRegistry::Snapshot() const
{
    return m_Providers;
}

bool EditorVcsProviderRegistry::TryGet(std::string_view typeId,
                                       EditorVcsProviderDescriptor& outDescriptor) const
{
    const auto it = std::find_if(m_Providers.begin(), m_Providers.end(),
                                 [&](const EditorVcsProviderDescriptor& d) { return d.TypeId == typeId; });
    if (it == m_Providers.end())
        return false;
    outDescriptor = *it;
    return true;
}

std::string EditorVcsProviderRegistry::DetectWorkspace(const std::filesystem::path& projectRoot) const
{
    if (projectRoot.empty())
        return {};

    for (const EditorVcsProviderDescriptor& provider : m_Providers)
    {
        if (provider.Detect(projectRoot))
            return provider.TypeId;
    }
    return {};
}

IVCSIntegration* EditorVcsProviderRegistry::ActiveIntegration() const
{
    for (const EditorVcsProviderDescriptor& provider : m_Providers)
    {
        IVCSIntegration& integration = provider.Integration();
        if (integration.IsRepository())
            return &integration;
    }
    return nullptr;
}

std::string EditorVcsProviderRegistry::ActiveTypeId() const
{
    for (const EditorVcsProviderDescriptor& provider : m_Providers)
    {
        if (provider.Integration().IsRepository())
            return provider.TypeId;
    }
    return {};
}

bool EditorVcsProviderRegistry::TryGetActiveProvider(EditorVcsProviderDescriptor& outDescriptor) const
{
    for (const EditorVcsProviderDescriptor& provider : m_Providers)
    {
        if (provider.Integration().IsRepository())
        {
            outDescriptor = provider;
            return true;
        }
    }
    return false;
}

void EditorVcsProviderRegistry::SetRegistrationObserver(RegistrationObserver observer)
{
    m_Observer = std::move(observer);

    // Replay for providers that registered before the observer existed, so
    // attach order never decides whether a provider is seen.
    if (m_Observer)
        for (const EditorVcsProviderDescriptor& provider : m_Providers)
            m_Observer(provider, nullptr);
}

void EditorVcsProviderRegistry::NotifyBadgeSettingsChanged() const
{
    if (m_BadgeSettingsChangedHandler)
        m_BadgeSettingsChangedHandler();
}

void EditorVcsProviderRegistry::SetBadgeSettingsChangedHandler(std::function<void()> handler)
{
    m_BadgeSettingsChangedHandler = std::move(handler);
}

void EditorVcsProviderRegistry::AppendModulePins(std::string_view moduleId,
                                                 std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    for (const auto& [typeId, stamp] : m_ModuleOwners)
    {
        if (stamp.ModuleId == moduleId)
            outPins.push_back("VCS provider '" + typeId + "'");
    }
}

} // namespace GameEngine::Editor
