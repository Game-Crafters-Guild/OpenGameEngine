#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

class PolyhavenDownloadManager;

// Modal dialog shown when the user triggers play mode, build, or save while
// Polyhaven downloads are still in progress. Buttons: Wait / Proceed Anyway / Cancel.
// The modal auto-polls the download manager each frame and dismisses itself
// when all downloads complete, then executes the deferred action.
class DownloadInProgressModal final : public UIElement
{
public:
    DownloadInProgressModal();

    void Show(const std::string& actionName,
              PolyhavenDownloadManager* downloadManager,
              std::function<void()> onProceed);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    // Call each frame from Update() to auto-dismiss when downloads finish.
    void Poll();

private:
    void OnWaitClicked();
    void OnProceedClicked();
    void OnCancelClicked();
    void UpdateMessage();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_Window = nullptr;
    UIElement* m_Title = nullptr;
    UIElement* m_Message = nullptr;

    bool m_Visible = false;
    PolyhavenDownloadManager* m_DownloadManager = nullptr;
    std::function<void()> m_OnProceed;
};

} // namespace GameEngine
