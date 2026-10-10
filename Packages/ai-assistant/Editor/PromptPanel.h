#pragma once

#include "AgentConversationController.h"
#include "AssistantActionLedger.h"

#include "UI/AssetBoundDockPanel.h"
#include "UI/UIManagerRef.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{

class AgentConversationController;
class AgentConversationRow;
class AgentModelPopover;
class AgentSessionPicker;
class Button;
class Dropdown;
class Label;
class ScrollView;
class PromptTextArea;
struct AgentConversationRowModel;
struct AgentReplyAction;
struct AgentSessionChoice;
struct ConversationMessage;
struct UIEvent;
class UIElement;

/// The AI Assistant panel: the view of one conversation. The conversation and its
/// turns belong to an AgentConversationController; the panel sends what the user
/// types, shows one AgentConversationRow per message and follows the connection
/// chosen in settings.
class PromptPanel final : public Editor::AssetBoundDockPanel
{
public:
    PromptPanel();
    ~PromptPanel() override;

    void OnPostLayout() override;

private:
    void OnLayoutBound() override;

    void SubmitPrompt();
    void StopTurns();
    void RetryTurn(size_t messageIndex);
    void ClearSession();
    void OnSessionChosen(const AgentSessionChoice& choice);
    void FollowProvider(const std::string& providerId);
    void SyncModeControl();
    void FitModeControl();
    /// Gives m_ModeDropdown m_ModeOptions with their whole or compact labels (m_ModeCompact),
    /// `selected` chosen, without reporting a change.
    void ShowModeLabels(int selected);
    void RefreshContinuedRow();
    void RunReplyAction(const AgentReplyAction& action, size_t messageIndex);
    void AnswerCall(uint64_t requestId, AssistantAnswer answer);
    bool FocusWaitingAnswer();
    AgentConversationRowModel RowModelAt(size_t messageIndex) const;
    void UpdateConversation();
    void SyncRows();
    void RemoveRows();
    void UpdateStatus();
    void RefreshHistoryTextVisuals();
    void OnHistoryKeyDown(UIEvent& e);
    void SelectRowByOffset(int offset);
    void ScrollRowIntoView(size_t rowIndex);
    bool NavigatePromptRecall(int direction);
    void UpdatePromptPlaceholderVisibility(const std::string& value);
    void SetStatus(const std::string& text);
    float PromptInputLineAdvancePx() const;
    int PromptInputVisualLineCount() const;
    float PromptInputPaneHeightForLines(int lines) const;
    void AutoSizePromptInputPane();
    void UpdateInputStacking();
    void UpdateHistoryNarrow();
    void ApplyPromptInputPaneHeight(float heightPx);

    std::unique_ptr<AgentConversationController> m_Controller;

    Dropdown* m_ProviderDropdown = nullptr;
    // What the conversation's assistant may do; hidden for a connection without tools.
    Dropdown* m_ModeDropdown = nullptr;
    // The connection can call the editor's tools (ProviderCapabilities::CanActWithTools).
    bool m_ProviderActs = false;
    // What m_ModeDropdown shows: a mode id, or the reason the connection cannot act;
    // the control is rebuilt only when it changes.
    std::string m_ModeControlState;
    // One choice m_ModeDropdown offers now.
    struct ModeOption
    {
        std::string Value;
        std::string Label;
        std::string CompactLabel;
    };
    // The choices m_ModeDropdown offers now; FitModeControl() keeps the widest label whole, or
    // the widest compact label on a line too narrow for it.
    std::vector<ModeOption> m_ModeOptions;
    // m_ModeDropdown shows the compact labels.
    bool m_ModeCompact = false;
    // Set while ShowModeLabels() rebuilds the control, whose selection then reports a
    // change the user did not make.
    bool m_SyncingModeControl = false;
    Button* m_SessionButton = nullptr;
    Button* m_ModelButton = nullptr;
    // The model control's popover, a child of the panel; it owns the button's behavior.
    AgentModelPopover* m_ModelPopover = nullptr;
    std::unique_ptr<AgentSessionPicker> m_SessionPicker;
    // The first row of a conversation that continues a chosen session; hidden otherwise.
    Label* m_ContinuedRow = nullptr;
    // The session m_ContinuedRow describes and the text it shows; both empty while
    // it is hidden.
    std::string m_ContinuedId;
    std::string m_ContinuedText;
    // AiAssistantSettings::ProviderGeneration() the panel last followed.
    uint64_t m_ProviderGeneration = 0;
    PromptTextArea* m_PromptText = nullptr;
    ScrollView* m_HistoryScroll = nullptr;
    ScrollView* m_PromptInputScroll = nullptr;
    UIElement* m_HistoryList = nullptr;
    Label* m_HistoryEmpty = nullptr;
    UIElement* m_InputPane = nullptr;
    UIElement* m_InputRow = nullptr;
    UIElement* m_InputControls = nullptr;
    // The pane is too narrow for a kMinPromptFieldPx field beside the visible controls,
    // so the controls sit on a line of their own under the field.
    bool m_InputStacked = false;
    // The history list is narrower than kNarrowHistoryPx: call rows stack their parts.
    bool m_HistoryNarrow = false;
    Label* m_PromptPlaceholder = nullptr;
    Label* m_StatusLabel = nullptr;
    Button* m_SendButton = nullptr;
    Button* m_ClearPromptButton = nullptr;

    // One row per conversation message, same index.
    std::vector<AgentConversationRow*> m_Rows;
    // Rows before this index show a finished message and never change again.
    size_t m_FirstLiveRow = 0;
    // The conversation revision the rows show.
    uint64_t m_RowsRevision = 0;
    // The calls' revision (AgentConversationController::ActionsRevision) the rows show.
    AgentActionsRevision m_ActionsRevision;
    // What each row's calls showed when it was last refreshed (TurnRevision), same index.
    std::vector<AgentActionsRevision> m_RowRevisions;
    // The row Up/Down selected in the history; SIZE_MAX for none.
    size_t m_SelectedRow = SIZE_MAX;

    std::vector<std::string> m_PromptRecall;
    int m_PromptRecallCursor = -1;
    std::string m_PromptRecallDraft;

    bool m_ScrollHistoryToBottomPending = false;
    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
    float m_InputPaneHeightPx = 64.0f;
    float m_InputResizeStartMouseY = 0.0f;
    float m_InputResizeStartHeightPx = 64.0f;
    bool m_InputResizeDragging = false;
    // Height the user dragged the splitter to; 0 until they do. Auto-grow treats
    // it as a floor so typing never undoes a manual resize.
    float m_InputPaneManualHeightPx = 0.0f;
};

} // namespace GameEngine
