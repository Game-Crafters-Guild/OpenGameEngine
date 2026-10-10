#pragma once

#include "Events/Event.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace GameEngine
{

class IVCSIntegration;

// Owns Editor-side initialization/shutdown of the VCS integrations registered
// with EditorVcsProviderRegistry, and broadcasts status-change notifications
// to any number of UI listeners. On provider (re)registration — a package
// module loading or hot-reloading — the active connection is torn down and
// re-detected (a loud disconnect-reconnect; listeners survive).
class EditorVersionControlService final
{
  public:
    using Listener = std::function<void()>;
    // Registration handle: unsubscribes when destroyed, and does nothing if the
    // service died first. Subscribers are UI panels, which the editor destroys
    // *after* the service, so outliving the service is the normal case here
    // rather than a misuse to guard against.
    using Subscription = EventSubscription;

    EditorVersionControlService();
    ~EditorVersionControlService();

    EditorVersionControlService(const EditorVersionControlService&) = delete;
    EditorVersionControlService& operator=(const EditorVersionControlService&) = delete;

    void InitializeForProject(const std::filesystem::path& projectRoot);
    void Shutdown();

    // One-shot: every VCS provider had its chance to register (the package
    // module load pass finished, or can never produce providers). Until this
    // fires, InitializeForProject only records the project root — the FIRST
    // workspace detection is deferred so a later-ordered provider can never
    // transiently claim a workspace whose real (earlier-ordered) provider is
    // still loading, e.g. SVN claiming a mixed .git+.svn workspace before the
    // git-vcs package module registered. Runs the deferred detection itself.
    void NotifyProvidersReady();

    // Executes work deferred from provider (re)registration: disconnecting a
    // replaced integration and re-running detection. Module registrations run
    // inside LoadLibrary static init (loader lock held) — integration
    // shutdown joins poll threads and Initialize spawns processes, both of
    // which would deadlock there. The editor calls this once per frame.
    void PumpDeferredWork();

    // Subscribe to "VCS status changed" notifications (may come from background threads).
    // Caller is responsible for marshaling to UI thread (e.g. via PostAction).
    // Unsubscribing does not join a callback already running on a status
    // thread, so a subscriber that can be destroyed mid-callback needs its own
    // guard as well.
    [[nodiscard]] Subscription AddListener(Listener cb);

  private:
    void Broadcast();
    // Shuts down every registered integration without touching listeners.
    void ShutdownIntegrations();
    // Detect + initialize for the current project root (listeners survive).
    void ReinitializeProviders();

    Event<> m_StatusChanged;
    std::filesystem::path m_ProjectRoot;

    // Registration-observer deferrals (main-thread only: registrations and
    // the pump both run there).
    std::vector<IVCSIntegration*> m_PendingDisconnects;
    bool m_ReinitPending = false;
    // Detection gate (see NotifyProvidersReady). Main thread only.
    bool m_ProvidersReady = false;
};

} // namespace GameEngine

