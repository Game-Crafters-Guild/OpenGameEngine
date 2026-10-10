#pragma once

#include "UI/UIManagerRef.h"

#include <functional>
#include <memory>
#include <string>

namespace GameEngine
{
class AgentConversationController;
class AgentSessionSearchProvider;
class Button;
class SearchDialog;
struct AgentSessionChoice;

/// The AI Assistant panel's Session control: a button beside the connection dropdown,
/// shown for a local connection, that opens a searchable list of the connection's
/// sessions in the project (AgentSessionSearchProvider). Opening it asks the
/// controller to read the sessions; the list fills in when the read ends.
class AgentSessionPicker
{
public:
    /// Called with the row the user chose; the dialog is closed by then.
    using ChoiceHandler = std::function<void(const AgentSessionChoice&)>;
    /// Called with a line for the panel's status line.
    using StatusHandler = std::function<void(const std::string&)>;

    AgentSessionPicker(Button& button, AgentConversationController& controller, ChoiceHandler onChoice,
                       StatusHandler onStatus);
    /// Closes a dialog that is still open.
    ~AgentSessionPicker();

    AgentSessionPicker(const AgentSessionPicker&) = delete;
    AgentSessionPicker& operator=(const AgentSessionPicker&) = delete;

    /// Shows the button for a local connection (`available`) and hides it, closing
    /// the list, for any other.
    void SetAvailable(bool available);
    /// Once per frame, after the controller's Update(): hands a session list that
    /// arrived to the open dialog, and keeps the button's tooltip naming the session
    /// the conversation is in.
    void Update();

private:
    void Open();
    void Close();
    void RefreshTooltip();

    Button& m_Button;
    AgentConversationController& m_Controller;
    ChoiceHandler m_OnChoice;
    StatusHandler m_OnStatus;
    std::unique_ptr<AgentSessionSearchProvider> m_Search;
    /// The open dialog, a child of the UI root; nullptr when closed.
    SearchDialog* m_Dialog = nullptr;
    /// The manager whose root holds the dialog, checked before the dialog is touched
    /// after the panel may have outlived it.
    UIManagerRef m_DialogManager;
    /// The state the tooltip describes: the continued session's id, or "new" or
    /// "started" for a conversation that continues none.
    std::string m_TooltipState;
};
} // namespace GameEngine
