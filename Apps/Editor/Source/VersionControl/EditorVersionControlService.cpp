#include "VersionControl/EditorVersionControlService.h"

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Logger/Logger.h"
#include "VCSIntegration/IVCSIntegration.h"

#include <utility>

namespace GameEngine
{

EditorVersionControlService::EditorVersionControlService()
{
    // Provider (re)registration after startup means a package module loaded or
    // hot-reloaded. The observer fires from the module DLL's static init —
    // under the Windows loader lock — so it must only RECORD the work:
    // shutting an integration down joins its poll thread (thread exit needs
    // the loader lock → deadlock) and Initialize spawns processes. The editor
    // pumps the deferred work on the next frame.
    Editor::EditorVcsProviderRegistry::Get().SetRegistrationObserver(
        [this](const Editor::EditorVcsProviderDescriptor& descriptor,
               IVCSIntegration* replacedIntegration) {
            if (replacedIntegration)
            {
                Logger::Log::Info("VCS: provider '{}' replaced by module reload; "
                                  "previous integration disconnects next frame",
                                  descriptor.TypeId);
                m_PendingDisconnects.push_back(replacedIntegration);
            }
            m_ReinitPending = true;
        });
}

EditorVersionControlService::~EditorVersionControlService()
{
    Editor::EditorVcsProviderRegistry::Get().SetRegistrationObserver({});
    Shutdown();
}

EditorVersionControlService::Subscription
EditorVersionControlService::AddListener(Listener cb)
{
    return m_StatusChanged.Subscribe(std::move(cb));
}

void EditorVersionControlService::Broadcast()
{
    // Invoke keeps the subscriber list alive through a local shared_ptr, so a
    // status thread already inside here does not follow freed state if the
    // service is destroyed mid-broadcast.
    m_StatusChanged.Invoke();
}

void EditorVersionControlService::NotifyProvidersReady()
{
    if (m_ProvidersReady)
        return;
    m_ProvidersReady = true;

    // This reinit answers every registration that arrived while detection was
    // gated — consume the pending flag so the next pump doesn't immediately
    // disconnect-reconnect the provider initialized here.
    m_ReinitPending = false;
    if (!m_ProjectRoot.empty())
        ReinitializeProviders();
}

void EditorVersionControlService::PumpDeferredWork()
{
    if (m_PendingDisconnects.empty() && !m_ReinitPending)
        return;

    std::vector<IVCSIntegration*> disconnects;
    disconnects.swap(m_PendingDisconnects);
    // Detection stays gated until every provider had its chance to register;
    // the request stays armed meanwhile (NotifyProvidersReady consumes it).
    const bool reinit = m_ReinitPending && m_ProvidersReady;
    if (reinit)
        m_ReinitPending = false;

    for (IVCSIntegration* replaced : disconnects)
    {
        if (replaced)
            replaced->Shutdown();
    }

    // Re-detect so a provider arriving after project open (package modules
    // load then) can still claim the workspace. A loud disconnect-reconnect
    // of the active provider is acceptable on module reload.
    if (reinit && !m_ProjectRoot.empty())
        ReinitializeProviders();
}

void EditorVersionControlService::ShutdownIntegrations()
{
    // Shut down every registered integration to allow clean re-init (e.g.
    // when switching projects or VCS types).
    for (const auto& provider : Editor::EditorVcsProviderRegistry::Get().Snapshot())
        provider.Integration().Shutdown();
}

void EditorVersionControlService::Shutdown()
{
    // Subscriptions deliberately survive: this also runs on the project-switch
    // path (InitializeForProject), where the same panels go on observing the
    // next project. Each subscriber unsubscribes when it is destroyed.
    //
    // So a late broadcast raised while an integration is shutting down does
    // reach the subscribers, which clearing the list used to prevent. That is
    // safe because every subscriber carries a liveness flag it clears before it
    // dies — not because the editor happens to destroy this service before the
    // panels. Do not reintroduce the clear: on the project-switch path it is
    // what silently unsubscribed every panel for the rest of the session.
    ShutdownIntegrations();
    m_ProjectRoot.clear();
}

void EditorVersionControlService::InitializeForProject(const std::filesystem::path& projectRoot)
{
    Shutdown();

    if (projectRoot.empty())
        return;

    m_ProjectRoot = projectRoot;
    if (!m_ProvidersReady)
    {
        Logger::Log::Info("VCS: project '{}' recorded; workspace detection deferred until "
                          "package providers finish loading",
                          projectRoot.string());
        return;
    }
    ReinitializeProviders();
}

void EditorVersionControlService::ReinitializeProviders()
{
    ShutdownIntegrations();

    if (m_ProjectRoot.empty())
        return;

    auto& registry = Editor::EditorVcsProviderRegistry::Get();
    const std::string typeId = registry.DetectWorkspace(m_ProjectRoot);
    if (typeId.empty())
        return;

    Editor::EditorVcsProviderDescriptor provider;
    if (!registry.TryGet(typeId, provider))
        return;

    if (provider.Initialize(m_ProjectRoot, [this]() { Broadcast(); }))
    {
        // The one success line for the whole claim path — first detection,
        // gate-open detection, and re-detects all land here.
        Logger::Log::Info("VCS: provider '{}' claimed workspace {}", typeId, m_ProjectRoot.string());
        // The provider's background poll thread waits one full polling
        // interval before its first status query, so listeners would read a
        // stale empty ("Clean") snapshot for up to that interval right after
        // a claim. Kick an eager refresh so untracked/modified badges land
        // within one git-status duration of the claim instead. RefreshStatus
        // wakes the poll thread; its completion fires the status-changed
        // callback wired above, which re-broadcasts with real data.
        if (provider.Integration)
            provider.Integration().RefreshStatus();
        Broadcast(); // initial refresh for the newly-connected repository
    }
    else
    {
        Logger::Log::Warning("VCS: provider '{}' detected the workspace but failed to "
                             "initialize for {}",
                             typeId, m_ProjectRoot.string());
    }
}

} // namespace GameEngine
