#pragma once

#include "EditorApplication.h" // EditorApplication::EditorWindowContext

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace GameEngine
{
class UIManager;
namespace Rendering
{
struct DeviceDesc;
}

namespace Editor
{

/// Owns HDR/display-output policy for every editor window: the effective HDR
/// request, per-monitor metadata tuning, the deferred (settle-gated) swapchain
/// re-spec on monitor changes, the post-switch render-suppression windows, and
/// the OS SDR-white live poll that re-derives the UI HDR mapping. Also refreshes
/// swapchains on monitor moves even when HDR is Off (forceSwapchainRefresh path).
///
/// The effective request combines the persisted rendering.hdr settings with the
/// GE_FORCE_HDR_OUTPUT launch override. This class owns the I/O — reading the
/// settings file and the environment — while the precedence between them is
/// Editor::ResolveHdrOutputRequest in HdrOutputMatch, where it is unit-tested.
///
/// Lifetime: constructed and initialized before the first window handler
/// registration, and NEVER reset during shutdown — monitor-changed callbacks
/// have no shutdown guard and may fire while windows tear down, so the
/// controller must outlive the window list.
class HdrOutputController
{
  public:
    using WindowContext = EditorApplication::EditorWindowContext;

    struct Dependencies
    {
        // App-lifetime window list; the app owns/mutates the vector, the
        // controller iterates it and mutates per-window HDR fields.
        std::vector<std::unique_ptr<WindowContext>>* Windows = nullptr;
        // EditorApplication.cpp's ApplyUiRuntimeConfig (JobSystem wiring +
        // HDR UI paper-white/black-lift re-derive). Shared with non-HDR
        // window-bootstrap paths, so it stays app-side.
        std::function<void(UIManager*)> ApplyUiRuntimeConfig;
        // EditorApplication::RefreshWorldRenderForPassiveFrame — synchronous
        // world re-record after a swapchain discard (ghost-geometry recovery).
        std::function<void(WindowContext*)> RefreshWorldRenderForPassiveFrame;
    };

    void Initialize(Dependencies deps);

    /// Startup: fill the DeviceDesc's hdr* fields from the effective request
    /// (saved settings resolved against GE_FORCE_HDR_OUTPUT) tuned for the
    /// startup monitor, and emit the startup-HDR log line. Static because it
    /// runs before the device (and the controller's windows) exist.
    static void ConfigureStartupDeviceDesc(Rendering::DeviceDesc& desc,
                                           const std::filesystem::path& settingsRoot,
                                           int activeMonitorIndex);

    /// Queue a deferred HDR/output re-apply for one window (settle-gated
    /// unless waitForMonitorSettle is false).
    void QueueUpdate(WindowContext* ctx, int monitorIndex,
                     bool waitForMonitorSettle = true,
                     bool forceSwapchainRefresh = false);

    /// A window was just created: evaluate the saved HDR request against that
    /// window's own monitor. A new window target inherits the HDR request of
    /// whichever target was active when it was created — target creation clears
    /// only activeMode — so without this a floating window wears the tuning
    /// (paper white, mastering luminance, targetDisplay gate) of another
    /// window's monitor until it crosses a monitor boundary.
    void OnWindowCreated(WindowContext* ctx);

    /// A window's active monitor changed: queue a forced re-apply after the
    /// display settles and arm the render-suppression window.
    void OnWindowMonitorChanged(WindowContext* ctx, int monitorIndex);

    /// Re-apply the saved HDR settings on every window (settings-page edits).
    void RequestRefreshForAllWindows();

    /// Per-frame pump: polls the OS SDR white level and drains settled
    /// pending updates. Call once per Update().
    void Tick();

    /// Render() gate for the post-switch suppression window. Mutating query —
    /// clears the window on expiry — so call exactly once per Render().
    bool ShouldSuppressRender();

    /// Arm the monitor-settle suppression window and the per-window skip
    /// frames (macOS deferred monitor/content-scale transitions).
    void BeginMonitorSettleSuppression(WindowContext* ctx);

  private:
    bool ApplyForMonitor(WindowContext* ctx, int monitorIndex, bool forceSwapchainRefresh);

    Dependencies m_Deps;
    std::chrono::steady_clock::time_point m_SuppressRenderUntil{};
    std::chrono::steady_clock::time_point m_LastSdrWhitePollAt{};
    float m_LastSdrWhiteLevelNits = 0.0f;
};

} // namespace Editor
} // namespace GameEngine
