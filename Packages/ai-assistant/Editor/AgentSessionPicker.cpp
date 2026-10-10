#include "AgentSessionPicker.h"

#include "AgentConversationController.h"
#include "AgentSessionSearchProvider.h"
#include "AiAssistantSettings.h"

#include "Editor/EditorPaths.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/UIManager.h"

#include <any>
#include <string_view>
#include <utility>

namespace GameEngine
{
namespace
{
// Wide enough for a first prompt of about 60 characters beside nothing else.
constexpr float kDialogWidthPx = 440.0f;
constexpr const char* kTooltip =
    "Start a new session, resume the last one, or find an earlier session of this project.";
} // namespace

AgentSessionPicker::AgentSessionPicker(Button& button, AgentConversationController& controller,
                                       ChoiceHandler onChoice, StatusHandler onStatus)
    : m_Button(button)
    , m_Controller(controller)
    , m_OnChoice(std::move(onChoice))
    , m_OnStatus(std::move(onStatus))
{
    RefreshTooltip();
    m_Button.SetOnClick([this](UIEvent&) { Open(); });
}

AgentSessionPicker::~AgentSessionPicker()
{
    Close();
}

void AgentSessionPicker::SetAvailable(bool available)
{
    if (available)
    {
        m_Button.RemoveClass("hidden");
        return;
    }
    m_Button.AddClass("hidden");
    Close();
}

void AgentSessionPicker::Update()
{
    RefreshTooltip();
    if (!m_Dialog || m_Search->HasSessions() || !m_Controller.SessionList())
        return;
    const CliSessionList& list = *m_Controller.SessionList();
    if (list.SkippedFiles > 0)
        m_OnStatus(std::to_string(list.SkippedFiles) +
                   (list.SkippedFiles == 1 ? " session file could not be read" : " session files could not be read"));
    m_Search->SetSessions(list);
}

void AgentSessionPicker::RefreshTooltip()
{
    const auto& continued = m_Controller.ContinuedSession();
    const bool started = !m_Controller.GetConversation().Messages().empty();
    // Compared without a copy: this runs every frame.
    const std::string_view state = continued ? std::string_view(continued->Id) : (started ? "started" : "new");
    if (state == m_TooltipState)
        return;
    m_TooltipState = state;
    std::string now;
    if (continued)
        now = "Now continuing: " + (continued->FirstPrompt.empty() ? continued->Id : continued->FirstPrompt);
    else if (started)
        now = "Now in the session this conversation started.";
    else
        now = "The next prompt starts a new session.";
    m_Button.SetTooltip(std::string(kTooltip) + "\n" + now);
}

void AgentSessionPicker::Open()
{
    if (m_Dialog)
        return;
    UIManager* manager = m_Button.GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    if (!root)
        return;

    m_Search = std::make_unique<AgentSessionSearchProvider>(AiAssistantSettings::LastSession(
        AiAssistantSettings::Provider(), Editor::GetCurrentEditorProjectPaths().projectRoot));
    m_Controller.RequestSessionList();

    auto dialog = std::make_unique<SearchDialog>();
    // PromptPanelChrome.css styles its rows: the dialog is a child of the UI root,
    // outside the panel's own stylesheet.
    dialog->AddClass("agent-session-picker");
    dialog->SetProvider(m_Search.get());
    dialog->SetOnResult(
        [this](const SearchResultItem& item)
        {
            const auto* choice = std::any_cast<AgentSessionChoice>(&item.UserData);
            const AgentSessionChoice chosen = choice ? *choice : AgentSessionChoice{};
            Close();
            if (choice)
                m_OnChoice(chosen);
        });
    dialog->SetOnCancel([this] { Close(); });
    dialog->SetPanelWidth(kDialogWidthPx);
    m_Dialog = dialog.get();
    m_DialogManager = UIManagerRef(manager);
    root->AddChild(std::move(dialog));
    // Right edges aligned, so the list extends over the prompt field rather than past
    // the panel's right edge; it opens below the button, or above when there is no room.
    m_Dialog->SetAnchorPosition(m_Button.GetLayoutX() + m_Button.GetLayoutWidth(),
                                m_Button.GetLayoutY() + m_Button.GetLayoutHeight(),
                                SearchDialogHorizontalAnchor::TrailingRight, m_Button.GetLayoutHeight());
    m_Dialog->Show();
}

void AgentSessionPicker::Close()
{
    SearchDialog* dialog = std::exchange(m_Dialog, nullptr);
    if (dialog && m_DialogManager.Get())
        if (UIElement* parent = dialog->GetParent())
            parent->RemoveChild(dialog);
    m_DialogManager = UIManagerRef();
    m_Search.reset();
}
} // namespace GameEngine
