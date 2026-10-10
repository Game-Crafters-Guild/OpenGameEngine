#include "DeviceLossSurfacer.h"

#include "Logger/Logger.h"
#include "Platform/Shell.h"
#include "Platform/Window.h"
#include "Rendering/Core/Device.h"

#include <thread>
#include <utility>

namespace GameEngine
{
namespace Editor
{

using Rendering::DeviceHealth;

namespace
{
const char* RecoveringMessage(DeviceHealth health)
{
    // Hung = GPU alive but behind (fence timeout); the others = a real loss mid-recovery.
    return health == DeviceHealth::Hung ? "GPU appears hung - attempting recovery..."
                                        : "GPU device lost - recovering...";
}

constexpr const char* kTerminalTitle =
    "The graphics device was reset and could not be recovered";
constexpr const char* kTerminalMessage =
    "Rendering has stopped. Your open work is still in memory but cannot be "
    "displayed. Save and restart the editor to continue.";
} // namespace

DeviceLossSurfacer::DeviceLossSurfacer(ChoicePresenter presenter)
    : m_Presenter(presenter ? std::move(presenter)
                            : ChoicePresenter(&Platform::ShowNativeChoiceDialog))
{
}

void DeviceLossSurfacer::Poll(DeviceHealth health, Platform::Window* window,
                              const Actions& actions)
{
    // Terminal: the rebuild retry gave up (real TDR = process-lifetime adapter poison
    // on this driver; see the Q6 design doc). Surface once per episode via a native
    // modal — the GPU-rendered editor UI can never draw this — but present it off the
    // main thread and apply the answer from a later poll, so the frame loop, IPC and
    // autosave keep running whether or not anyone is there to click.
    if (health == DeviceHealth::Failed)
    {
        m_RecoveringSince = {};
        m_LastUnhealthy = {};
        if (m_ToastActive && window)
        {
            window->SetTitle(m_SavedTitle);
            m_ToastActive = false;
        }

        if (!m_FailedHandled)
        {
            m_FailedHandled = true;
            Logger::Log::Warning("DeviceLossSurfacer: device health=Failed — showing native "
                                 "save-and-restart dialog (main loop keeps running)");

            m_PendingChoice = std::make_shared<PendingChoice>();
            // Detached by design: a modal returns only when a human answers, which may
            // be never. Nothing joins this thread, so neither a later Poll() nor this
            // object's destruction can inherit that wait. The slot is shared, so the
            // thread stays safe if the surfacer dies first.
            std::thread([slot = m_PendingChoice, presenter = m_Presenter] {
                const int choice =
                    presenter(kTerminalTitle, kTerminalMessage,
                              {"Save All & Restart", "Save All & Close",
                               "Continue without rendering"},
                              /*defaultButton=*/0);
                slot->Choice.store(choice, std::memory_order_relaxed);
                slot->Ready.store(true, std::memory_order_release);
            }).detach();
        }

        if (m_PendingChoice && m_PendingChoice->Ready.load(std::memory_order_acquire))
        {
            const int choice = m_PendingChoice->Choice.load(std::memory_order_relaxed);
            m_PendingChoice.reset();

            Logger::Log::Warning("DeviceLossSurfacer: save-and-restart dialog choice = {}",
                                 choice);
            switch (choice)
            {
            case 0:
                if (actions.SaveAll)
                    actions.SaveAll();
                if (actions.RestartAndExit)
                    actions.RestartAndExit();
                break;
            case 1:
                if (actions.SaveAll)
                    actions.SaveAll();
                if (actions.ExitApp)
                    actions.ExitApp();
                break;
            default:
                Logger::Log::Warning(
                    "DeviceLossSurfacer: continuing without rendering (IPC / scripting still "
                    "work; restart to render again)");
                break;
            }
        }
        return;
    }

    // Transient recovery states hold the last-good frame; show a non-blocking title-bar
    // toast so the frozen editor is not mistaken for a hard hang. A stall the poll-resume
    // recovers from flickers Hung<->Healthy frame to frame, so the episode is debounced:
    // it opens on the first unhealthy poll and closes only after the device has been
    // continuously Healthy for kResumeDebounce (keeps the toast stable, not strobing).
    const auto now = std::chrono::steady_clock::now();
    const bool unhealthy =
        health == DeviceHealth::Hung || health == DeviceHealth::Lost ||
        health == DeviceHealth::Rebuilding || health == DeviceHealth::AwaitingReprovision;
    if (unhealthy)
    {
        m_LastUnhealthy = now;
        m_ActiveMessage = RecoveringMessage(health);
        if (m_RecoveringSince == std::chrono::steady_clock::time_point{})
            m_RecoveringSince = now;
    }

    const bool episodeActive =
        m_RecoveringSince != std::chrono::steady_clock::time_point{} &&
        (now - m_LastUnhealthy) < kResumeDebounce;
    if (episodeActive)
    {
        if ((now - m_RecoveringSince) >= kToastSurfaceDelay && window)
        {
            if (!m_ToastActive)
            {
                m_SavedTitle = window->GetTitle();
                m_ToastActive = true;
                Logger::Log::Info("DeviceLossSurfacer: recovery toast shown — '{}'", m_ActiveMessage);
            }
            // Re-assert every tick: the editor's own title logic runs in Update() and
            // could otherwise clobber the toast mid-recovery.
            window->SetTitle(m_ActiveMessage);
        }
        return;
    }

    // Settled: device has been Healthy past the debounce. Clear the toast, restore the
    // editor's title, re-arm the Failed one-shot.
    m_RecoveringSince = {};
    if (m_ToastActive)
    {
        if (window)
            window->SetTitle(m_SavedTitle);
        m_ToastActive = false;
        Logger::Log::Info("DeviceLossSurfacer: recovery toast cleared — device Healthy");
    }
    m_FailedHandled = false;
}

} // namespace Editor
} // namespace GameEngine
