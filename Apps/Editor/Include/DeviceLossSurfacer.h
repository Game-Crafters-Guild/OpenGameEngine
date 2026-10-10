#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{
enum class DeviceHealth : uint8_t;
}
namespace Platform
{
class Window;
}

namespace Editor
{

// Surfaces GPU device-loss state to the user with NATIVE OS UI, because the editor's
// own UI is GPU-rendered and cannot draw while the device is lost / recovering / failed
// (rendering is suppressed until the device is Healthy again). Polled once per
// render-loop tick on the main thread — render and UI share one thread (see
// Application::Tick). Health comes from the state machine, never from submit-time
// loss detection (the M3 lesson: a suppressed render path issues no submits, so a
// submit-driven surface would never fire).
//
// Poll() never blocks. The transient recovery toast is a title-bar write; the terminal
// save-and-restart prompt is a modal native dialog, so it is presented on a detached
// worker thread and Poll() returns immediately, applying the choice from a later Poll()
// once one is available. The main thread is the whole editor — frame loop, input,
// autosave, scripting and the debug-server IPC drain all run on it — so a modal held
// there stops everything and leaves a process that is alive but answers nothing until
// a human clicks. Unattended runs (test harnesses, CI, an unwatched machine) never
// produce that click, which makes blocking here indistinguishable from a deadlock.
class DeviceLossSurfacer
{
  public:
    // Wired by the editor to its own facilities. All run on the main thread from Poll().
    struct Actions
    {
        std::function<void()> SaveAll;        // best-effort save of the open scene
        std::function<void()> RestartAndExit; // relaunch the editor process, then quit
        std::function<void()> ExitApp;        // quit without relaunch
    };

    // Presents a modal choice and returns the chosen button index. Called on a detached
    // worker thread — never on the main thread — and may not return for as long as the
    // dialog stands. Defaults to the native OS dialog; tests substitute a fake so no
    // real dialog can wedge an unattended run.
    using ChoicePresenter =
        std::function<int(const std::string& title, const std::string& message,
                          const std::vector<std::string>& buttons, int defaultButton)>;

    explicit DeviceLossSurfacer(ChoicePresenter presenter = {});

    // Call every render-loop tick, before the per-window render (which is skipped while
    // the device is unhealthy). `window` may be null; it carries the transient recovery
    // toast in its title bar. Returns promptly for every health value.
    void Poll(Rendering::DeviceHealth health, Platform::Window* window,
              const Actions& actions);

  private:
    // A quick Hung blip should not flash the toast — matches VulkanDevice's 250 ms
    // "GPU busy" surfacing threshold.
    static constexpr std::chrono::milliseconds kToastSurfaceDelay{250};
    // A transient GPU stall the poll-resume recovers from produces a rapid
    // Hung<->Healthy flicker (some frames catch up, some don't). Treat the episode
    // as ongoing until the device has been continuously Healthy for this long, so the
    // toast is stable instead of strobing and clears once recovery has truly settled.
    static constexpr std::chrono::milliseconds kResumeDebounce{400};

    // Result slot for the detached prompt thread. Held by shared_ptr so the thread can
    // outlive this object without writing through a dangling pointer, and so destruction
    // never has to join a thread parked in a modal message loop.
    struct PendingChoice
    {
        std::atomic<bool> Ready{false};
        std::atomic<int> Choice{0};
    };

    ChoicePresenter m_Presenter;
    bool m_ToastActive = false;
    std::string m_SavedTitle; // window title captured just before the toast replaced it
    bool m_FailedHandled = false; // one prompt per Failed episode; re-armed on Healthy
    std::shared_ptr<PendingChoice> m_PendingChoice; // non-null while a prompt is open
    std::chrono::steady_clock::time_point m_RecoveringSince{}; // first unhealthy of the episode
    std::chrono::steady_clock::time_point m_LastUnhealthy{};   // most recent unhealthy poll
    const char* m_ActiveMessage = nullptr; // toast text for the current episode
};

} // namespace Editor
} // namespace GameEngine
