#include "PromptPanel.h"
#include "AgentConversationController.h"
#include "AgentConversationRow.h"
#include "AgentConversationRowModel.h"
#include "AgentModelPopover.h"
#include "AgentReplyActions.h"
#include "AgentSessionPicker.h"
#include "AgentSessionSearchProvider.h"
#include "AgentSessionState.h"
#include "AgentStatusMessages.h"
#include "AiAssistantSettings.h"
#include "AssistantMode.h"
#include "PromptTextArea.h"
#include "ModeControlFit.h"
#include "Providers/IAgentProvider.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

namespace GameEngine
{
namespace
{
// Everything in the input pane that is not a line of text: the pane's own
// padding (4 top + 8 bottom), the field border (1 each side) and the text
// area's vertical padding (10 each side). Keep in sync with PromptPanel.css.
constexpr float kPromptInputPaneChromePx = 34.0f;
// Used until the font resolves and the text area can report a real advance.
constexpr float kPromptInputLineAdvanceFallbackPx = 18.0f;
// Past this the field stops growing and the text scrolls inside it.
constexpr int kPromptInputMaxAutoGrowLines = 8;
// The narrowest prompt field beside the controls: a pane that would leave the field
// less than this, after the visible controls take their width, puts the controls on a
// line of their own under the field. Wide enough to show the whole placeholder, which
// teaches Up for history: its 29 characters at the 14 px script face's 8.4 px advance
// (244 px, measured in the fold-4 h5 capture), its 12 px left inset and the 28 px it
// keeps clear of the clear button, plus 4 px.
constexpr float kMinPromptFieldPx = 288.0f;
// The input row's 1 px border on each side (.prompt-panel-input-row).
constexpr float kInputRowBorderPx = 2.0f;
// The controls' own line when they sit under the field. Keep in sync with
// .prompt-panel-input-controls-stacked in PromptPanel.css.
constexpr float kStackedControlsRowPx = 36.0f;
// The history list narrower than this stacks each call row: the action, then the summary
// with the undo state and Details beside it when they fit (AgentCallRow::SetStacked). The
// list is the panel less its scrollbar.
// A row's summary needs about 360 px beside its action (~120 px) and an undo step's name
// (~200 px at most half the line) to keep its entity links and vectors on one line.
constexpr float kNarrowHistoryPx = 520.0f;
// The mode control's states while its connection cannot act.
constexpr const char* kCheckingState = "checking";
constexpr const char* kConversationOnlyState = "conversationOnly";

// The mode control's tooltip: what the control is, then what the chosen mode lets the
// assistant do. The other modes are read in the list itself.
std::string ModeTooltip(AssistantMode mode)
{
    const AssistantModeChoice& choice = DescribeAssistantMode(mode);
    return "What the assistant may do; new conversations start in the mode last chosen.\n" +
           std::string(choice.Label) + ": " + std::string(choice.Description);
}

std::shared_ptr<IAgentProvider> LookUpProvider(std::string_view providerId)
{
    return AgentSessionState::Get().Provider(providerId);
}

bool IsFinished(const ConversationMessage& message)
{
    return message.Status != MessageStatus::Queued && message.Status != MessageStatus::InProgress;
}

std::string LocalTimeOfDay(std::chrono::system_clock::time_point time)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%H:%M");
    return out.str();
}

void AddSemanticClassToDescendants(UIElement& element, const char* sourceClass,
                                   const char* semanticClass)
{
    for (const auto& child : element.GetChildren())
    {
        if (child->HasClass(sourceClass))
            child->AddClass(semanticClass);
        AddSemanticClassToDescendants(*child, sourceClass, semanticClass);
    }
}

void ConfigurePromptScrollView(ScrollView& scroll, bool input)
{
    if (UIElement* horizontal = scroll.GetHorizontalScrollbar())
    {
        horizontal->AddClass("prompt-panel-horizontal-scrollbar");
        horizontal->AddClass("prompt-panel-scrollbar");
    }
    if (UIElement* vertical = scroll.GetVerticalScrollbar())
        vertical->AddClass("prompt-panel-scrollbar");
    if (input)
    {
        if (UIElement* viewport = scroll.GetClipViewport())
            viewport->AddClass("prompt-panel-input-scroll-viewport");
        if (UIElement* content = scroll.GetViewport())
            content->AddClass("prompt-panel-input-scroll-content");
    }
}

// The width `element`'s stylesheet lets it shrink to; 0 when it sets none in pixels.
float MinWidthPx(const UIElement& element)
{
    const StyleLength minimum = element.GetResolvedStyle().Layout.MinWidth;
    return minimum.IsPx() ? minimum.Value : 0.0f;
}

auto s_PromptTextAreaRegistration =
    UIRegistration::RegisterWithFactory<PromptTextArea>(
        "PromptTextArea", []() { return std::make_unique<PromptTextArea>(); });
} // namespace

PromptPanel::PromptPanel()
    : AssetBoundDockPanel("AI Assistant", "ai-assistant", "Editor/UI/panels/PromptPanel.uxml",
                          "Editor/UI/panels/PromptPanel.css")
    , m_Controller(std::make_unique<AgentConversationController>(LookUpProvider))
{
    AddClass("prompt-panel-root");
}

PromptPanel::~PromptPanel()
{
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
}

void PromptPanel::OnLayoutBound()
{
    auto findElement = [this](const char* id) { return FindById(id); };
    auto findButton = [this](const char* id) { return dynamic_cast<Button*>(FindById(id)); };
    auto findDropdown = [this](const char* id) { return dynamic_cast<Dropdown*>(FindById(id)); };

    UIElement* providerSettings = findElement("PromptProviderSettings");
    m_ProviderDropdown = findDropdown("PromptProviderDropdown");
    m_ModeDropdown = findDropdown("PromptModeDropdown");

    m_HistoryScroll = dynamic_cast<ScrollView*>(findElement("PromptHistoryScroll"));
    m_HistoryList = findElement("PromptHistoryList");
    m_HistoryEmpty = dynamic_cast<Label*>(findElement("PromptHistoryEmpty"));
    m_InputPane = findElement("PromptInputPane");
    m_InputRow = findElement("PromptInputRow");
    m_InputControls = findElement("PromptInputControls");
    m_PromptInputScroll = dynamic_cast<ScrollView*>(findElement("PromptInputScroll"));
    m_PromptText = dynamic_cast<PromptTextArea*>(findElement("PromptInputText"));
    m_ClearPromptButton = findButton("PromptInputClear");
    m_PromptPlaceholder = dynamic_cast<Label*>(findElement("PromptInputPlaceholder"));
    m_SendButton = findButton("PromptSendButton");
    m_StatusLabel = dynamic_cast<Label*>(findElement("PromptStatusLabel"));
    m_SessionButton = findButton("PromptSessionButton");
    m_ModelButton = findButton("PromptModelButton");
    m_ContinuedRow = dynamic_cast<Label*>(findElement("PromptContinuedSession"));

    const bool hasRequiredControls = providerSettings && m_ProviderDropdown && m_HistoryScroll &&
        m_HistoryList && m_HistoryEmpty && m_InputPane && m_InputRow && m_InputControls && m_PromptInputScroll &&
        m_PromptText &&
        m_ClearPromptButton && m_SendButton && m_StatusLabel && m_SessionButton && m_ContinuedRow &&
        m_ModeDropdown && m_ModelButton;
    if (!hasRequiredControls)
    {
        Logger::Log::Error("AI Assistant panel layout is missing one or more required controls");
        return;
    }

    auto moveIntoScrollContent = [](ScrollView& scroll, UIElement* child)
    {
        if (!child || child->GetParent() != &scroll)
            return;
        std::unique_ptr<UIElement> owned = scroll.TakeChild(child);
        if (owned)
            scroll.AddContent(std::move(owned));
    };
    moveIntoScrollContent(*m_HistoryScroll, m_HistoryList);
    moveIntoScrollContent(*m_PromptInputScroll, m_PromptText);
    ConfigurePromptScrollView(*m_HistoryScroll, false);
    ConfigurePromptScrollView(*m_PromptInputScroll, true);

    std::vector<Dropdown::Option> providers;
    for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::ProviderChoices())
        providers.push_back({std::string(choice.Id), std::string(choice.Label), ""});
    m_ProviderDropdown->SetOptions(providers, 0);
    m_ProviderGeneration = AiAssistantSettings::ProviderGeneration();
    m_ProviderDropdown->SetSelectedValue(AiAssistantSettings::Provider());
    auto modelPopover = std::make_unique<AgentModelPopover>(*m_ModelButton);
    m_ModelPopover = modelPopover.get();
    AddChild(std::move(modelPopover));
    if (UIManager* ui = GetOwnerManager())
        m_ModelPopover->Build(*ui);
    // A model label narrowed in a narrow panel stays on one line and is cut.
    AddSemanticClassToDescendants(*m_ModelButton, "button-text", "prompt-panel-input-task-label");
    m_SessionPicker = std::make_unique<AgentSessionPicker>(
        *m_SessionButton, *m_Controller, [this](const AgentSessionChoice& choice) { OnSessionChosen(choice); },
        [this](const std::string& text) { SetStatus(text); });
    FollowProvider(AiAssistantSettings::Provider());
    m_ProviderDropdown->SetOnValueChanged([](const std::string& providerId)
    {
        AiAssistantSettings::SetProvider(providerId);
    });
    m_ModeDropdown->SetOnValueChanged([this](const std::string& modeId)
    {
        if (m_SyncingModeControl)
            return;
        if (const std::optional<AssistantMode> mode = ParseAssistantMode(modeId))
            m_Controller->SetMode(*mode);
    });
    if (UIElement* header = m_ModeDropdown->GetHeaderContainer())
        header->AddClass("prompt-panel-input-task-header");
    AddSemanticClassToDescendants(*m_ModeDropdown, "dropdown-chevron", "prompt-panel-input-task-chevron");
    AddSemanticClassToDescendants(*m_ModeDropdown, "dropdown-header-label", "prompt-panel-input-task-label");
    // Below, over the panel's neighbour: to the right it would cover Session…, the
    // connection and Send; above, the open list.
    m_ModeDropdown->SetTooltipPlacement(UIElement::TooltipPlacement::Below);
    SyncModeControl();
    if (UIElement* header = m_ProviderDropdown->GetHeaderContainer())
        header->AddClass("prompt-panel-input-task-header");
    AddSemanticClassToDescendants(*m_ProviderDropdown, "dropdown-chevron", "prompt-panel-input-task-chevron");
    AddSemanticClassToDescendants(*m_ProviderDropdown, "dropdown-header-label", "prompt-panel-input-task-label");
    m_ProviderDropdown->SetTooltip("The connection that answers; changing it starts a new conversation. The "
                                   "button beside it chooses the model; executables are in Settings > AI "
                                   "Assistant. Claude (API) reads ANTHROPIC_API_KEY from the editor's "
                                   "environment: set it before starting the editor");
    m_ProviderDropdown->SetTooltipPlacement(UIElement::TooltipPlacement::Right);

    if (Button* settings = findButton("PromptSettingsToggle"))
    {
        settings->SetTooltip("Prompt settings");
        settings->SetOnClick([providerSettings](UIEvent&)
        {
            if (providerSettings->HasClass("hidden"))
                providerSettings->RemoveClass("hidden");
            else
                providerSettings->AddClass("hidden");
        });
    }
    if (Button* clearSession = findButton("PromptClearSession"))
    {
        clearSession->SetTooltip("Start a new conversation");
        clearSession->SetOnClick([this](UIEvent&) { ClearSession(); });
    }

    m_HistoryScroll->SetOnScrollChanged([this](float, float) { RefreshHistoryTextVisuals(); });
    m_HistoryScroll->RegisterEventHandler(kEventScroll, [this](UIEvent&) { RefreshHistoryTextVisuals(); });
    m_HistoryScroll->RegisterEventHandler(kEventMouseDown, [this](UIEvent& event)
    {
        if (event.Button == 0)
            if (UIManager* ui = GetOwnerManager())
                ui->FocusElement(m_HistoryList);
    });
    m_HistoryList->SetFocusable(true);
    m_HistoryList->RegisterEventHandler(kEventKeyDown, [this](UIEvent& event) { OnHistoryKeyDown(event); });

    UIElement* splitter = findElement("PromptPanelInputSplitter");
    if (splitter)
    {
        splitter->RegisterEventHandler(kEventMouseDown, [this, splitter](UIEvent& event)
        {
            m_InputResizeDragging = true;
            m_InputResizeStartMouseY = event.Y;
            m_InputResizeStartHeightPx = m_InputPaneHeightPx;
            event.Capture(splitter);
            event.Stop();
        });
        splitter->RegisterEventHandler(kEventMouseMove, [this](UIEvent& event)
        {
            if (!m_InputResizeDragging)
                return;
            ApplyPromptInputPaneHeight(m_InputResizeStartHeightPx + (m_InputResizeStartMouseY - event.Y));
            m_InputPaneManualHeightPx = m_InputPaneHeightPx;
            event.Stop();
        });
        splitter->RegisterEventHandler(kEventMouseUp, [this](UIEvent& event)
        {
            if (!m_InputResizeDragging)
                return;
            m_InputResizeDragging = false;
            event.Stop();
        });
        splitter->RegisterEventHandler(kEventMouseCancel, [this](UIEvent&)
        {
            m_InputResizeDragging = false;
        });
        splitter->RegisterEventHandler(kEventKeyDown, [this](UIEvent& event)
        {
            if (event.Mods != 0 ||
                (event.Key != Input::kKeyCode_Up && event.Key != Input::kKeyCode_Down))
                return;

            constexpr float kKeyboardResizeStepPx = 8.0f;
            const float direction = event.Key == Input::kKeyCode_Up ? 1.0f : -1.0f;
            ApplyPromptInputPaneHeight(m_InputPaneHeightPx + direction * kKeyboardResizeStepPx);
            m_InputPaneManualHeightPx = m_InputPaneHeightPx;
            event.Stop();
        });
    }

    m_PromptText->SetFocusable(true);
    auto updatePromptInputScroll = [this]()
    {
        if (!m_PromptInputScroll || !m_PromptText)
            return;
        const float textHeight = static_cast<float>(PromptInputVisualLineCount()) * PromptInputLineAdvancePx();
        const float contentHeight = std::max(m_PromptInputScroll->GetViewportHeight(), textHeight + 20.0f);
        m_PromptInputScroll->SetContentSize(std::max(0.0f, m_PromptInputScroll->GetViewportWidth()), contentHeight);
        m_PromptInputScroll->SetScrollY(m_PromptInputScroll->GetContentHeight());
        m_PromptText->MarkDirty(VisualDirty | LayoutDirty);
    };
    m_PromptText->SetOnValueChanged([this, updatePromptInputScroll](const std::string& value)
    {
        AutoSizePromptInputPane();
        updatePromptInputScroll();
        UpdatePromptPlaceholderVisibility(value);
    });
    m_PromptText->SetKeyFilter([this](UIEvent& event)
    {
        if (event.Key == Input::kKeyCode_Escape)
        {
            if (!m_Controller->IsBusy())
                return false;
            StopTurns();
            return true;
        }
        // Ctrl+Enter takes the keyboard to a call waiting for the user's answer.
        if (event.Key == Input::kKeyCode_Enter && (event.Mods & Input::kModControl) != 0)
            return FocusWaitingAnswer();
        if (event.Key == Input::kKeyCode_Up)
            return NavigatePromptRecall(-1);
        if (event.Key == Input::kKeyCode_Down)
            return NavigatePromptRecall(1);
        return false;
    });
    // Enter sends, Shift+Enter breaks the line: the text area's own submit mode.
    m_PromptText->SetOnSubmit([this]() { SubmitPrompt(); });

    m_ClearPromptButton->SetFocusable(false);
    m_ClearPromptButton->SetTooltip("Clear prompt");
    m_ClearPromptButton->SetOnClick([this, updatePromptInputScroll](UIEvent&)
    {
        if (!m_PromptText || m_PromptText->GetValue().empty())
            return;
        m_PromptText->SetValue("");
        m_PromptText->SetSelection(0, 0);
        m_PromptRecallCursor = -1;
        m_PromptRecallDraft.clear();
        AutoSizePromptInputPane();
        updatePromptInputScroll();
        UpdatePromptPlaceholderVisibility("");
        if (UIManager* ui = GetOwnerManager())
            ui->FocusElement(m_PromptText);
    });
    m_SendButton->SetTooltip("Send prompt (Enter); a prompt sent while a reply runs waits its turn");
    m_SendButton->SetOnClick([this](UIEvent&) { SubmitPrompt(); });

    ApplyPromptInputPaneHeight(PromptInputPaneHeightForLines(1));
    updatePromptInputScroll();
    UpdatePromptPlaceholderVisibility(m_PromptText->GetValue());
    SyncRows();

    if (m_RefreshToken == 0)
    {
        if (UIManager* manager = GetOwnerManager())
        {
            m_RefreshManager = UIManagerRef(manager);
            m_RefreshToken = manager->RegisterPeriodicRefresh([this]() { UpdateConversation(); });
        }
    }
}

float PromptPanel::PromptInputLineAdvancePx() const
{
    const float advance = m_PromptText ? m_PromptText->GetResolvedLineAdvancePx() : 0.0f;
    return advance > 0.0f ? advance : kPromptInputLineAdvanceFallbackPx;
}

int PromptPanel::PromptInputVisualLineCount() const
{
    // Visual, not logical: a single long prompt wraps onto several rows, and the
    // field has to grow for those too.
    return m_PromptText ? static_cast<int>(m_PromptText->GetVisualLineCount()) : 1;
}

float PromptPanel::PromptInputPaneHeightForLines(int lines) const
{
    const int clamped = std::clamp(lines, 1, kPromptInputMaxAutoGrowLines);
    const float controls = m_InputStacked ? kStackedControlsRowPx : 0.0f;
    return kPromptInputPaneChromePx + controls + static_cast<float>(clamped) * PromptInputLineAdvancePx();
}

void PromptPanel::AutoSizePromptInputPane()
{
    // Dragging the splitter is a request, not a suggestion: keep it as a floor
    // so the next keystroke does not undo the resize. Content still grows the
    // field past that floor when the prompt needs more room.
    const float contentHeight = PromptInputPaneHeightForLines(PromptInputVisualLineCount());
    ApplyPromptInputPaneHeight(std::max(contentHeight, m_InputPaneManualHeightPx));
}

void PromptPanel::UpdateInputStacking()
{
    if (!m_InputPane || !m_InputRow || !m_InputControls)
        return;
    // The controls group never shrinks beside the field, so its width is that of the
    // controls the connection shows (Claude (API) hides the mode control and Session…);
    // under the field it shrinks only once the pane is narrower than that width, which
    // leaves no room for the field anyway.
    const float rowWidth = m_InputRow->GetLayoutWidth();
    if (rowWidth <= 0.0f)
        return;
    const float fieldWidth = rowWidth - kInputRowBorderPx - m_InputControls->GetLayoutWidth();
    const bool stacked = fieldWidth < kMinPromptFieldPx;
    if (stacked == m_InputStacked)
        return;
    m_InputStacked = stacked;
    if (stacked)
    {
        m_InputRow->AddClass("prompt-panel-input-row-stacked");
        m_InputControls->AddClass("prompt-panel-input-controls-stacked");
    }
    else
    {
        m_InputRow->RemoveClass("prompt-panel-input-row-stacked");
        m_InputControls->RemoveClass("prompt-panel-input-controls-stacked");
    }
}

void PromptPanel::UpdateHistoryNarrow()
{
    if (!m_HistoryList)
        return;
    const float width = m_HistoryList->GetLayoutWidth();
    if (width <= 0.0f)
        return;
    const bool narrow = width < kNarrowHistoryPx;
    if (narrow == m_HistoryNarrow)
        return;
    m_HistoryNarrow = narrow;
    for (AgentConversationRow* row : m_Rows)
        row->SetCallsStacked(narrow);
}

void PromptPanel::ApplyPromptInputPaneHeight(float heightPx)
{
    m_InputPaneHeightPx = std::clamp(heightPx,
                                     PromptInputPaneHeightForLines(1),
                                     PromptInputPaneHeightForLines(kPromptInputMaxAutoGrowLines));
    if (!m_InputPane)
        return;

    m_InputPane->Overrides()
        .Set(Style::Height, StyleLength::Px(m_InputPaneHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(m_InputPaneHeightPx))
        .Set(Style::MaxHeight, StyleLength::Px(m_InputPaneHeightPx))
        .Set(Style::FlexBasis, StyleLength::Px(m_InputPaneHeightPx));
    m_InputPane->RequestRelayout();
}

void PromptPanel::UpdateConversation()
{
    // The settings page changed the connection while the panel was open.
    const uint64_t providerGeneration = AiAssistantSettings::ProviderGeneration();
    if (m_ProviderDropdown && providerGeneration != m_ProviderGeneration)
    {
        m_ProviderGeneration = providerGeneration;
        m_ProviderDropdown->SetSelectedValue(AiAssistantSettings::Provider());
        FollowProvider(AiAssistantSettings::Provider());
    }

    m_Controller->Update();
    SyncModeControl();
    if (m_SessionPicker)
        m_SessionPicker->Update();
    if (m_ModelPopover)
        m_ModelPopover->Update();
    RefreshContinuedRow();
    if (m_Controller->GetConversation().Revision() != m_RowsRevision ||
        m_Controller->ActionsRevision() != m_ActionsRevision)
        SyncRows();
}

void PromptPanel::FollowProvider(const std::string& providerId)
{
    m_Controller->SetProvider(providerId);
    const std::shared_ptr<IAgentProvider> provider = AgentSessionState::Get().Provider(providerId);
    m_ProviderActs = provider && provider->Capabilities().CanActWithTools;
    SyncModeControl();
    // Only a local session keeps sessions to pick from.
    if (m_SessionPicker)
        m_SessionPicker->SetAvailable(AgentSessionState::Get().SessionProvider(providerId) != nullptr);
    if (m_ModelPopover)
        m_ModelPopover->SetProvider(providerId);
}

void PromptPanel::SyncModeControl()
{
    if (!m_ModeDropdown)
        return;
    if (!m_ProviderActs)
    {
        m_ModeDropdown->AddClass("hidden");
        m_ModeControlState.clear();
        return;
    }
    m_ModeDropdown->RemoveClass("hidden");

    const std::string& refusal = m_Controller->ActingRefusal();
    std::string state;
    if (!refusal.empty())
        state = kConversationOnlyState;
    else if (m_Controller->ActingCheckRunning())
        state = kCheckingState;
    else
        state = DescribeAssistantMode(m_Controller->Mode()).Id;
    const std::string stateKey = state + "|" + refusal;
    if (stateKey == m_ModeControlState)
        return;
    m_ModeControlState = stateKey;
    // A state the user cannot change is a value to read: the disabled dropdown drops the
    // chevron and the hover plate, and opens nothing (Dropdown.css).
    m_ModeDropdown->SetEnabled(state != kConversationOnlyState && state != kCheckingState);

    m_ModeOptions.clear();
    int selected = 0;
    if (state == kConversationOnlyState)
    {
        m_ModeOptions.push_back({kConversationOnlyState, "Conversation only", "Chat"});
        m_ModeDropdown->SetTooltip("This connection answers without the editor's tools. " + refusal);
    }
    else if (state == kCheckingState)
    {
        m_ModeOptions.push_back({kCheckingState, "Checking tools…", "Checking…"});
        m_ModeDropdown->SetTooltip("Checking whether this connection can use the editor's tools.");
    }
    else
    {
        for (const AssistantModeChoice& choice : AssistantModeChoices())
        {
            if (choice.Mode == m_Controller->Mode())
                selected = static_cast<int>(m_ModeOptions.size());
            m_ModeOptions.push_back(
                {std::string(choice.Id), std::string(choice.Label), std::string(choice.CompactLabel)});
        }
        m_ModeDropdown->SetTooltip(ModeTooltip(m_Controller->Mode()));
    }
    ShowModeLabels(selected);
}

void PromptPanel::ShowModeLabels(int selected)
{
    std::vector<Dropdown::Option> options;
    options.reserve(m_ModeOptions.size());
    for (const ModeOption& option : m_ModeOptions)
        options.push_back({option.Value, m_ModeCompact ? option.CompactLabel : option.Label, ""});
    m_SyncingModeControl = true;
    m_ModeDropdown->SetOptions(options, selected);
    m_SyncingModeControl = false;
}

void PromptPanel::FitModeControl()
{
    if (!m_ModeDropdown || !m_ModelButton || m_ModeDropdown->HasClass("hidden") || m_ModeOptions.empty())
        return;
    const StyleLength width = m_ModeDropdown->GetResolvedStyle().Layout.Width;
    if (!width.IsPx())
        return;
    // The widest label and compact label the control offers now, measured, never past its
    // stylesheet width.
    float widest = 0.0f;
    float widestCompact = 0.0f;
    for (const ModeOption& option : m_ModeOptions)
    {
        widest = std::max(widest, m_ModeDropdown->WidthForLabel(option.Label));
        widestCompact = std::max(widestCompact, m_ModeDropdown->WidthForLabel(option.CompactLabel));
    }
    // The connection's first word with the ellipsis its cut label ends in; a one-word name whole.
    const std::string& name = m_ProviderDropdown->GetSelectedLabel();
    const size_t space = name.find(' ');
    const float readableConnection = m_ProviderDropdown->WidthForLabel(
        space == std::string::npos ? name : name.substr(0, space) + "\xE2\x80\xA6");
    // 0 until the controls have been laid out as text.
    if (widest <= 0.0f || readableConnection <= 0.0f)
        return;

    // What the line holds besides the three controls that shrink: Session…, Send and the
    // margins between them, measured from where the visible controls start and end.
    float left = std::numeric_limits<float>::max();
    float right = 0.0f;
    for (const auto& child : m_InputControls->GetChildren())
    {
        if (child->HasClass("hidden") || child->GetLayoutWidth() <= 0.0f)
            continue;
        left = std::min(left, child->GetLayoutX());
        right = std::max(right, child->GetLayoutX() + child->GetLayoutWidth() +
                                    child->GetResolvedStyle().Layout.Margin.Right);
    }
    const ResolvedStyle& line = m_InputControls->GetResolvedStyle();
    ModeControlFit fit;
    fit.WholeLabelPx = std::min(std::ceil(widest), width.Value);
    fit.CompactLabelPx = std::min(std::ceil(widestCompact), width.Value);
    fit.OthersPx = right - left - m_ModeDropdown->GetLayoutWidth() - m_ModelButton->GetLayoutWidth() -
                   m_ProviderDropdown->GetLayoutWidth();
    fit.AvailablePx = m_InputControls->GetLayoutWidth() - line.Layout.Padding.Left - line.Layout.Padding.Right;
    fit.ModelMinimumPx = MinWidthPx(*m_ModelButton);
    fit.ConnectionReadablePx = std::ceil(readableConnection);
    fit.Stacked = m_InputStacked;

    const ModeControlLayout layout = ModeControlLayoutFor(fit);
    if (layout.Compact != m_ModeCompact)
    {
        m_ModeCompact = layout.Compact;
        ShowModeLabels(m_ModeDropdown->GetSelectedIndex());
    }
    const std::optional<float>& minimum = layout.MinimumPx;
    const auto current = m_ModeDropdown->Overrides().Get(Style::MinWidth);
    if (minimum)
    {
        if (current && current->IsPx() && std::fabs(current->Value - *minimum) <= 0.5f)
            return;
        m_ModeDropdown->Overrides().Set(Style::MinWidth, StyleLength::Px(*minimum));
    }
    else
    {
        // Back to the stylesheet's minimum.
        if (!current)
            return;
        m_ModeDropdown->Overrides().Reset(Style::MinWidth);
    }
    m_ModeDropdown->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
}

void PromptPanel::RefreshContinuedRow()
{
    if (!m_ContinuedRow || !m_HistoryEmpty)
        return;
    // Checked every frame; the row's text is built only when the session changes.
    const auto& continued = m_Controller->ContinuedSession();
    const std::string_view continuedId = continued ? std::string_view(continued->Id) : std::string_view();
    if (continuedId == m_ContinuedId)
        return;
    m_ContinuedId = continuedId;
    m_ContinuedText = continued ? ContinuedSessionText(*continued) : std::string();
    m_ContinuedRow->SetText(m_ContinuedText);
    if (m_ContinuedText.empty())
        m_ContinuedRow->AddClass("hidden");
    else
        m_ContinuedRow->RemoveClass("hidden");
    // A continued conversation is not empty, so the empty-conversation hint goes.
    if (m_ContinuedText.empty() && m_Controller->GetConversation().Messages().empty())
        m_HistoryEmpty->RemoveClass("hidden");
    else
        m_HistoryEmpty->AddClass("hidden");
}

bool PromptPanel::NavigatePromptRecall(int direction)
{
    if (!m_PromptText || m_PromptRecall.empty() || direction == 0)
        return false;

    if (m_PromptRecallCursor < 0)
    {
        m_PromptRecallDraft = m_PromptText->GetValue();
        m_PromptRecallCursor = static_cast<int>(m_PromptRecall.size());
    }

    m_PromptRecallCursor = std::clamp(m_PromptRecallCursor + direction, 0, static_cast<int>(m_PromptRecall.size()));
    if (m_PromptRecallCursor == static_cast<int>(m_PromptRecall.size()))
        m_PromptText->SetValue(m_PromptRecallDraft);
    else
        m_PromptText->SetValue(m_PromptRecall[static_cast<std::size_t>(m_PromptRecallCursor)]);

    return true;
}

void PromptPanel::SubmitPrompt()
{
    if (!m_PromptText)
        return;

    const std::string prompt = TrimWhitespace(m_PromptText->GetValue());
    if (prompt.empty())
    {
        SetStatus("Enter a prompt first");
        return;
    }

    // A prompt over the cap is refused whole and stays in the field to be shortened.
    const std::string providerId = AiAssistantSettings::Provider();
    FollowProvider(providerId);
    if (!m_Controller->Send(prompt, AiAssistantSettings::Model(providerId), AiAssistantSettings::Effort(providerId)))
    {
        SetStatus(PromptTooLongStatus(prompt.size()));
        return;
    }

    if (m_PromptRecall.empty() || m_PromptRecall.back() != prompt)
        m_PromptRecall.push_back(prompt);
    m_PromptRecallCursor = -1;
    m_PromptRecallDraft.clear();
    m_PromptText->SetValue("");
    UpdateConversation();
}

void PromptPanel::StopTurns()
{
    m_Controller->Stop();
    UpdateConversation();
}

void PromptPanel::RetryTurn(size_t messageIndex)
{
    m_Controller->Retry(messageIndex);
    // SyncRows refreshes only rows whose message can still change; a retried reply
    // is finished but now reads "Retried below".
    const auto messages = m_Controller->GetConversation().Messages();
    if (messageIndex < m_Rows.size() && messageIndex < messages.size())
        m_Rows[messageIndex]->Show(RowModelAt(messageIndex), LocalTimeOfDay(messages[messageIndex].Time));
    UpdateConversation();
}

void PromptPanel::AnswerCall(uint64_t requestId, AssistantAnswer answer)
{
    // The call runs, or is declined, at the editor's next update; its row follows.
    m_Controller->Answer(requestId, answer);
}

bool PromptPanel::FocusWaitingAnswer()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return false;
    for (auto row = m_Rows.rbegin(); row != m_Rows.rend(); ++row)
    {
        if (UIElement* allow = (*row)->AnswerFocus())
        {
            ui->FocusElement(allow);
            return true;
        }
    }
    return false;
}

AgentConversationRowModel PromptPanel::RowModelAt(size_t messageIndex) const
{
    const Conversation& conversation = m_Controller->GetConversation();
    return AgentConversationRowModel::From(conversation.Messages()[messageIndex], m_Controller->Actions(),
                                           conversation.TurnOf(messageIndex), m_Controller->UndoHistory());
}

void PromptPanel::ClearSession()
{
    m_InputPaneManualHeightPx = 0.0f;
    m_Controller->NewConversation();
    // The next turn of each local session starts a new conversation.
    AgentSessionState::Get().ForgetSessions();
    UpdateConversation();
    SetStatus("New conversation");
}

void PromptPanel::OnSessionChosen(const AgentSessionChoice& choice)
{
    if (choice.Choice == AgentSessionChoice::Kind::NewSession)
    {
        ClearSession();
        return;
    }
    m_InputPaneManualHeightPx = 0.0f;
    m_Controller->ContinueSession(choice.Session);
    UpdateConversation();
}

void PromptPanel::RunReplyAction(const AgentReplyAction& action, size_t messageIndex)
{
    const auto messages = m_Controller->GetConversation().Messages();
    if (messageIndex >= messages.size())
        return;
    const std::string feedback = action.Run(messages[messageIndex]);
    if (!feedback.empty())
        SetStatus(feedback);
}

void PromptPanel::RemoveRows()
{
    for (AgentConversationRow* row : m_Rows)
        m_HistoryList->RemoveChild(row);
    m_Rows.clear();
    m_RowRevisions.clear();
    m_FirstLiveRow = 0;
    m_SelectedRow = SIZE_MAX;
}

// One row per message: new messages append a row, and only rows whose message can
// still change (queued or streaming) are refreshed.
void PromptPanel::SyncRows()
{
    UIManager* ui = GetOwnerManager();
    if (!m_HistoryList || !ui)
        return;
    if (UIElement::IsInEventDispatch())
    {
        PostSafeAction([this]() { SyncRows(); });
        return;
    }

    const Conversation& conversation = m_Controller->GetConversation();
    const auto messages = conversation.Messages();
    if (messages.size() < m_Rows.size())
        RemoveRows();

    const size_t previousRowCount = m_Rows.size();
    while (m_Rows.size() < messages.size())
    {
        const size_t index = m_Rows.size();
        auto row = std::make_unique<AgentConversationRow>(
            index, [this] { StopTurns(); }, [this, index] { RetryTurn(index); },
            [this, index](const AgentReplyAction& action) { RunReplyAction(action, index); },
            [this](uint64_t requestId, AssistantAnswer answer) { AnswerCall(requestId, answer); },
            [this, index] { m_Controller->UndoTurn(m_Controller->GetConversation().TurnOf(index)); });
        if (!row->Build(*ui))
            break;
        row->SetCallsStacked(m_HistoryNarrow);
        m_Rows.push_back(row.get());
        m_HistoryList->AddChild(std::move(row));
    }

    // A finished reply's calls can still change (a waiting call cancelled by Stop, a step
    // undone), so a change of the calls refreshes the rows whose turn's records or undo
    // steps moved; a live row refreshes every time.
    m_ActionsRevision = m_Controller->ActionsRevision();
    m_RowRevisions.resize(m_Rows.size());
    for (size_t index = 0; index < m_Rows.size(); ++index)
    {
        const AgentActionsRevision revision = m_Controller->TurnRevision(conversation.TurnOf(index));
        if (index < m_FirstLiveRow && revision == m_RowRevisions[index])
            continue;
        m_RowRevisions[index] = revision;
        m_Rows[index]->Show(RowModelAt(index), LocalTimeOfDay(messages[index].Time));
    }
    while (m_FirstLiveRow < m_Rows.size() && IsFinished(messages[m_FirstLiveRow]))
        ++m_FirstLiveRow;

    if (messages.empty() && m_ContinuedText.empty())
        m_HistoryEmpty->RemoveClass("hidden");
    else
        m_HistoryEmpty->AddClass("hidden");
    m_RowsRevision = conversation.Revision();

    // Follow a new message, and a streaming reply while the view is at the end;
    // a user who scrolled up to read stays where they are.
    const bool atEnd = m_HistoryScroll && m_HistoryScroll->GetScrollY() + m_HistoryScroll->GetViewportHeight() >=
                                              m_HistoryScroll->GetContentHeight() - 2.0f;
    if (m_Rows.size() != previousRowCount || atEnd)
        m_ScrollHistoryToBottomPending = true;
    UpdateStatus();
}

void PromptPanel::UpdateStatus()
{
    const auto messages = m_Controller->GetConversation().Messages();
    if (messages.empty())
    {
        SetStatus("Ready");
        return;
    }
    const auto actions = m_Controller->Actions();
    const bool asks = std::any_of(actions.begin(), actions.end(), [](const AssistantAction& action) {
        return action.State == AssistantActionState::Waiting && action.WaitingFor == AssistantWaitReason::Answer;
    });
    if (asks)
    {
        SetStatus("Waiting for your answer (Ctrl+Enter goes to it)");
        return;
    }
    const auto queued = std::count_if(messages.begin(), messages.end(),
                                      [](const ConversationMessage& m) { return m.Status == MessageStatus::Queued; });
    const ConversationMessage* running = nullptr;
    for (const ConversationMessage& message : messages)
        if (message.Status == MessageStatus::InProgress)
            running = &message;
    if (running)
    {
        SetStatus(queued > 0 ? "Generating... (" + std::to_string(queued) + " queued; Esc stops all)"
                             : "Generating... (Esc stops)");
        return;
    }
    switch (messages.back().Status)
    {
    case MessageStatus::Complete:
        SetStatus(messages.back().Truncated ? "Reply complete (truncated at 64 KB)" : "Reply complete");
        break;
    case MessageStatus::Failed:
        SetStatus(m_Controller->SessionWasLost() ? kSessionLostStatus : "Reply failed");
        break;
    case MessageStatus::Stopped:
    case MessageStatus::Cancelled:
        SetStatus("Stopped");
        break;
    case MessageStatus::Queued:
    case MessageStatus::InProgress:
        SetStatus("Generating...");
        break;
    }
}

void PromptPanel::OnPostLayout()
{
    AssetBoundDockPanel::OnPostLayout();
    UpdateConversation();
    FitModeControl();
    // Before the height reconcile below, which counts the controls' own line when stacked.
    UpdateInputStacking();
    UpdateHistoryNarrow();

    // Center the complete text block when the user has made the input taller
    // than its content. Pin its height too: TextArea otherwise retains the
    // height of an earlier, longer prompt after that prompt is cleared.
    if (m_PromptText && m_PromptInputScroll)
    {
        // The layout binds before font metrics are available. Reconcile the
        // initial pane height once layout resolves them, just as typing does.
        const float paneHeight = std::max(PromptInputPaneHeightForLines(PromptInputVisualLineCount()),
                                          m_InputPaneManualHeightPx);
        if (!m_InputResizeDragging && std::fabs(m_InputPaneHeightPx - paneHeight) > 0.5f)
            ApplyPromptInputPaneHeight(paneHeight);

        const float viewport = m_PromptInputScroll->GetViewportHeight();
        const float textHeight = static_cast<float>(PromptInputVisualLineCount()) * PromptInputLineAdvancePx();
        if (viewport > 0.0f)
        {
            const float padding = std::max(10.0f, (viewport - textHeight) * 0.5f);
            const auto oldPadding = m_PromptText->Overrides().Get(Style::PaddingTop);
            if (!oldPadding || !oldPadding->IsPx() || std::fabs(oldPadding->Value - padding) > 0.25f)
            {
                m_PromptText->Overrides()
                    .Set(Style::PaddingTop, StyleLength::Px(padding))
                    .Set(Style::PaddingBottom, StyleLength::Px(padding));
                m_PromptText->MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
            }
            const float contentHeight = textHeight + 2.0f * padding;
            PinHeightPx(*m_PromptText, contentHeight);
            m_PromptInputScroll->SetContentSize(m_PromptInputScroll->GetViewportWidth(), contentHeight);
        }
        UpdatePromptPlaceholderVisibility(m_PromptText->GetValue());
    }

    if (!m_HistoryScroll)
        return;

    // TextArea::OnPostLayout grows its own height override to fit wrapped text
    // and never shrinks, so leaving a row's text to self-size makes the row chase
    // it upward every pass. Each row pins its text to the height its wrapped line
    // count implies, which is what stops the growth; every row, because a panel
    // resize rewraps finished messages too.
    bool settling = false;
    for (AgentConversationRow* row : m_Rows)
        settling = row->FitHeight() || settling;

    // Scrolling mid-settle clamps short of the end, so wait for the heights to
    // stop moving before jumping to the newest message.
    if (!settling && m_ScrollHistoryToBottomPending)
    {
        m_ScrollHistoryToBottomPending = false;
        m_HistoryScroll->SetScrollY(m_HistoryScroll->GetContentHeight());
        RefreshHistoryTextVisuals();
    }
}

void PromptPanel::RefreshHistoryTextVisuals()
{
    for (AgentConversationRow* row : m_Rows)
        row->RefreshTextVisuals();
}

void PromptPanel::OnHistoryKeyDown(UIEvent& e)
{
    if (m_Rows.empty())
        return;
    if (e.Key == Input::kKeyCode_Up)
    {
        SelectRowByOffset(-1);
        e.Stop();
    }
    else if (e.Key == Input::kKeyCode_Down)
    {
        SelectRowByOffset(1);
        e.Stop();
    }
}

void PromptPanel::SelectRowByOffset(int offset)
{
    if (m_Rows.empty() || offset == 0)
        return;
    const int last = static_cast<int>(m_Rows.size()) - 1;
    const int target = m_SelectedRow >= m_Rows.size()
        ? (offset < 0 ? last : 0)
        : std::clamp(static_cast<int>(m_SelectedRow) + offset, 0, last);
    m_SelectedRow = static_cast<size_t>(target);
    ScrollRowIntoView(m_SelectedRow);
}

void PromptPanel::ScrollRowIntoView(size_t rowIndex)
{
    if (!m_HistoryScroll || !m_HistoryList || rowIndex >= m_Rows.size())
        return;
    const AgentConversationRow* row = m_Rows[rowIndex];
    const float rowTop = row->GetLayoutY() - m_HistoryList->GetLayoutY();
    const float rowBottom = rowTop + row->GetLayoutHeight();
    const float viewTop = m_HistoryScroll->GetScrollY();
    const float viewHeight = m_HistoryScroll->GetViewportHeight();
    if (rowTop < viewTop)
        m_HistoryScroll->SetScrollY(rowTop);
    else if (rowBottom > viewTop + viewHeight)
        m_HistoryScroll->SetScrollY(rowBottom - viewHeight);
}

void PromptPanel::UpdatePromptPlaceholderVisibility(const std::string& value)
{
    if (m_PromptPlaceholder)
    {
        if (value.empty())
            m_PromptPlaceholder->RemoveClass("prompt-panel-placeholder-hidden");
        else
            m_PromptPlaceholder->AddClass("prompt-panel-placeholder-hidden");
    }
    if (m_ClearPromptButton)
    {
        if (value.empty())
            m_ClearPromptButton->AddClass("prompt-panel-input-clear-hidden");
        else
            m_ClearPromptButton->RemoveClass("prompt-panel-input-clear-hidden");
    }
}

void PromptPanel::SetStatus(const std::string& text)
{
    if (m_StatusLabel)
        m_StatusLabel->SetText(text);
}

} // namespace GameEngine
