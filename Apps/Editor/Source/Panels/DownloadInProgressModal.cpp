#include "Panels/DownloadInProgressModal.h"
#include "Assets/PolyhavenDownloadManager.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"

namespace GameEngine
{

DownloadInProgressModal::DownloadInProgressModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("modal-backdrop");
    m_Backdrop->SetFocusable(true);

    auto window = std::make_unique<UIElement>();
    m_Window = window.get();
    m_Window->AddClass("modal-window");
    m_Window->AddClass("modal-window-shadow");
    m_Window->AddClass("modal-window-520");

    // Header
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("modal-header");
        auto title = std::make_unique<Label>();
        m_Title = title.get();
        title->SetText("Downloads In Progress");
        title->AddClass("modal-title");
        header->AddChild(std::move(title));
        m_Window->AddChild(std::move(header));
    }

    // Content
    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("modal-content");
        auto msg = std::make_unique<Label>();
        m_Message = msg.get();
        msg->SetText("Polyhaven assets are still downloading...");
        msg->AddClass("modal-message");
        content->AddChild(std::move(msg));
        m_Window->AddChild(std::move(content));
    }

    // Footer buttons
    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("modal-footer");

        auto cancel = std::make_unique<Button>();
        cancel->SetText("Cancel");
        cancel->AddClass("secondary");
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });

        auto proceed = std::make_unique<Button>();
        proceed->SetText("Proceed Anyway");
        proceed->AddClass("secondary");
        proceed->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnProceedClicked(); });

        auto wait = std::make_unique<Button>();
        wait->SetText("Wait");
        wait->AddClass("primary");
        wait->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnWaitClicked(); });

        footer->AddChild(std::move(cancel));
        footer->AddChild(std::move(proceed));
        footer->AddChild(std::move(wait));
        m_Window->AddChild(std::move(footer));
    }

    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (m_Visible && e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    m_Backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
}

void DownloadInProgressModal::Show(const std::string& /*actionName*/,
                                    PolyhavenDownloadManager* downloadManager,
                                    std::function<void()> onProceed)
{
    m_Visible = true;
    m_DownloadManager = downloadManager;
    m_OnProceed = std::move(onProceed);

    if (auto* t = dynamic_cast<Label*>(m_Title))
        t->SetText("Downloads In Progress");

    UpdateMessage();

    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void DownloadInProgressModal::Hide()
{
    m_Visible = false;
    m_DownloadManager = nullptr;
    m_OnProceed = nullptr;
    RemoveClass("visible");
}

void DownloadInProgressModal::Poll()
{
    if (!m_Visible || !m_DownloadManager)
        return;

    UpdateMessage();

    // Auto-dismiss when all downloads complete.
    if (!m_DownloadManager->HasActiveDownloads())
    {
        auto proceed = std::move(m_OnProceed);
        Hide();
        if (proceed)
            proceed();
    }
}

void DownloadInProgressModal::OnWaitClicked()
{
    // Keep modal visible — Poll() will auto-dismiss when done.
}

void DownloadInProgressModal::OnProceedClicked()
{
    auto proceed = std::move(m_OnProceed);
    Hide();
    if (proceed)
        proceed();
}

void DownloadInProgressModal::OnCancelClicked()
{
    Hide();
}

void DownloadInProgressModal::UpdateMessage()
{
    if (!m_DownloadManager || !m_Message)
        return;

    uint32_t count = m_DownloadManager->ActiveDownloadCount();
    std::vector<std::string> slugs;
    m_DownloadManager->GetActiveDownloadSlugs(slugs);

    std::string text = std::to_string(count) + " download" + (count != 1 ? "s" : "") + " in progress";
    if (!slugs.empty())
    {
        text += ": ";
        for (size_t i = 0; i < slugs.size() && i < 5; ++i)
        {
            if (i > 0) text += ", ";
            text += slugs[i];
        }
        if (slugs.size() > 5)
            text += "...";
    }
    text += "\n\nWait for downloads to finish, or proceed anyway with placeholders in the scene.";

    if (auto* lbl = dynamic_cast<Label*>(m_Message))
        lbl->SetText(text);
}

} // namespace GameEngine
