#include "EditorApplication.h"
#include "EditorPanelIds.h"
#include "UI/Layout/Docking.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/UIManager.h"
#include "PlayMode/PlayModeManager.h"
#include "Platform/Window.h"

namespace GameEngine
{

namespace
{

void ScheduleGameViewViewportFocus(UIManager* ui, Platform::Window* window, int remainingAttempts)
{
    if (!ui || remainingAttempts <= 0)
        return;

    if (window)
        window->Focus();

    if (UIElement* rootEl = ui->GetRootElement())
    {
        if (rootEl->FindById(EditorPanelIds::GameViewViewport))
            ui->SetFocusById(EditorPanelIds::GameViewViewport);

        rootEl->PostAction([ui, window, remainingAttempts]()
        {
            ScheduleGameViewViewportFocus(ui, window, remainingAttempts - 1);
        });
    }
}

} // namespace

bool EditorApplication::CanEnterPlayFullscreen() const
{
    return m_PlayFullscreenOnEnter && !m_PlayFullscreenActive && !m_Windows.empty();
}

void EditorApplication::EnterPlayFullscreen()
{
    if (m_Windows.empty())
        return;
    auto* main = m_Windows[0].get();
    if (!main || !main->window)
        return;

    if (m_Docking && m_Docking->GetRoot() && !m_PlayPrevDockLayout)
        m_PlayPrevDockLayout = CloneCurrentDockLayout();
    if (m_Docking)
    {
        auto root = DockNode::MakeLeaf();
        root->AddTab(EditorPanelIds::GameView);
        m_Docking->SetRoot(std::move(root));
        (void)m_Docking->ActivateTab(EditorPanelIds::GameView);
        if (main->ui)
        {
            if (UIElement* rootEl = main->ui->GetRootElement())
            {
                if (auto* ds = dynamic_cast<DockspaceElement*>(rootEl->FindById("dock")))
                    ds->RequestRebuildFromModel();
                rootEl->AddClass("play-fullscreen-gameview");
            }
        }
    }
    main->window->SetFullscreen(true);
    main->window->Focus();
    if (main->ui)
        ScheduleGameViewViewportFocus(main->ui.get(), main->window.get(), 8);
    m_PlayFullscreenActive = true;
    m_PlayFullscreenFocusFrames = 12;
}

void EditorApplication::ExitPlayFullscreen()
{
    if (!m_PlayFullscreenActive || m_Windows.empty())
        return;
    auto* main = m_Windows[0].get();
    if (!main || !main->window)
        return;

    main->window->SetFullscreen(false);
    if (m_Docking && m_PlayPrevDockLayout)
    {
        SetDockLayoutFromClone(m_PlayPrevDockLayout.get());
        if (main->ui)
        {
            if (UIElement* rootEl = main->ui->GetRootElement())
            {
                if (auto* ds = dynamic_cast<DockspaceElement*>(rootEl->FindById("dock")))
                    ds->RequestRebuildFromModel();
                rootEl->RemoveClass("play-fullscreen-gameview");
            }
        }
        m_PlayPrevDockLayout.reset();
    }
    m_PlayFullscreenActive = false;
    m_PlayFullscreenFocusFrames = 0;
}

void EditorApplication::RequestEnterPlayFullscreenAndPlay()
{
    if (!m_PlayMode || m_PlayMode->GetState() != Editor::PlayModeState::Edit || !CanEnterPlayFullscreen())
        return;
    m_DeferredPreUiActions.EnqueueUnique("editor.enter-play-fullscreen", [this]() {
        if (m_PlayMode->GetState() != Editor::PlayModeState::Edit || m_Windows.empty())
            return;
        EnterPlayFullscreen();
        m_PlayMode->EnterPlayMode();
    });
}

void EditorApplication::DeferToggleWindowFullscreen()
{
    if (m_Windows.empty())
        return;
    if (!m_Windows[0]->window)
        return;
    m_DeferredPreUiActions.EnqueueUnique("editor.toggle-window-fullscreen", [this]() {
        if (m_Windows.empty() || !m_Windows[0]->window)
            return;
        auto* win = m_Windows[0]->window.get();
        win->SetFullscreen(!win->IsFullscreen());
    });
}

void EditorApplication::TickPlayFullscreenFocus()
{
    if (m_PlayFullscreenFocusFrames <= 0 || !m_PlayFullscreenActive || m_Windows.empty())
        return;
    if (auto* main = m_Windows[0].get(); main && main->window && main->ui)
    {
        main->window->Focus();
        main->ui->SetFocusById(EditorPanelIds::GameViewViewport);
    }
    --m_PlayFullscreenFocusFrames;
}

void EditorApplication::BindPlayFullscreenChangedHandler(Platform::Window& window)
{
    // Browser Esc exits fullscreen without consulting the engine, so play-fullscreen
    // chrome unwinds on that transition, not only on the editor's own toggle.
    // Deferred: the callback arrives from the DOM event loop, potentially mid-frame.
    window.SetFullscreenChangedHandler([this](bool isFullscreen)
    {
        if (isFullscreen || !m_PlayFullscreenActive)
            return;
        m_DeferredPreUiActions.EnqueueUnique("editor.exit-play-fullscreen", [this]() {
            if (m_PlayFullscreenActive)
                ExitPlayFullscreen();
        });
    });
}

} // namespace GameEngine
