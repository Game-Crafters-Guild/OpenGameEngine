#include "AgentConversationRow.h"

#include "AgentReplyActions.h"

#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "ai-assistant:Editor/UI/controls/AgentConversationRow.uxml";

void SetClassIf(UIElement& element, const char* className, bool on)
{
    if (on)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}

// Finds a template element by its id and gives it the row's own id.
template <typename T>
T* TakeElement(UIElement& row, const char* id, size_t index)
{
    T* element = dynamic_cast<T*>(row.FindById(id));
    if (element)
        element->SetId(std::string(id) + ":" + std::to_string(index));
    return element;
}
} // namespace

bool PinHeightPx(UIElement& element, float heightPx)
{
    const auto existing = element.Overrides().Get(Style::Height);
    if (existing.has_value() && existing->IsPx() && std::fabs(existing->Value - heightPx) <= 0.5f)
        return false;

    element.Overrides().Set(Style::Height, StyleLength::Px(heightPx));
    element.MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
    return true;
}

AgentConversationRow::AgentConversationRow(size_t index, std::function<void()> onStop, std::function<void()> onRetry,
                                           ReplyActionHandler onReplyAction, AgentCallRow::AnswerHandler onAnswer,
                                           std::function<void()> onUndoTurn)
    : m_Index(index)
    , m_OnStop(std::move(onStop))
    , m_OnRetry(std::move(onRetry))
    , m_OnReplyAction(std::move(onReplyAction))
    , m_OnAnswer(std::move(onAnswer))
    , m_OnUndoTurn(std::move(onUndoTurn))
{
    SetFocusable(false);
}

bool AgentConversationRow::Build(UIManager& ui)
{
    if (!ui.InstantiateLayoutChildrenFromAssetPath(this, kLayoutAssetPath))
    {
        Logger::Log::Error("AI Assistant: {} did not instantiate; check that the package's assets are staged.",
                           kLayoutAssetPath);
        return false;
    }

    m_Header = TakeElement<Label>(*this, "AgentRowHeader", m_Index);
    m_Time = TakeElement<Label>(*this, "AgentRowTime", m_Index);
    m_Stop = TakeElement<Button>(*this, "AgentRowStop", m_Index);
    m_Retry = TakeElement<Button>(*this, "AgentRowRetry", m_Index);
    m_UndoTurn = TakeElement<Button>(*this, "AgentRowUndoTurn", m_Index);
    m_Retried = TakeElement<Label>(*this, "AgentRowRetried", m_Index);
    m_Text = TakeElement<TextArea>(*this, "AgentRowText", m_Index);
    m_Calls = TakeElement<UIElement>(*this, "AgentRowCalls", m_Index);
    m_Status = TakeElement<UIElement>(*this, "AgentRowStatus", m_Index);
    m_Actions = TakeElement<UIElement>(*this, "AgentRowActions", m_Index);
    if (!m_Header || !m_Time || !m_Stop || !m_Retry || !m_UndoTurn || !m_Retried || !m_Text || !m_Calls || !m_Status || !m_Actions)
    {
        Logger::Log::Error("AI Assistant: {} is missing a row element.", kLayoutAssetPath);
        return false;
    }

    m_Stop->SetTooltip("Stop this reply and every queued prompt (Esc)");
    m_Stop->SetOnClick([this](UIEvent&) { m_OnStop(); });
    m_Retry->SetTooltip("Send this prompt again as a new message");
    m_Retry->SetOnClick([this](UIEvent&) { m_OnRetry(); });
    m_UndoTurn->SetOnClick([this](UIEvent&) { m_OnUndoTurn(); });
    return true;
}

void AgentConversationRow::Show(const AgentConversationRowModel& model, const std::string& time)
{
    if (!m_Text)
        return;
    const AgentConversationRowModel* shown = m_Shown ? &*m_Shown : nullptr;
    if (!shown)
    {
        m_Time->SetText(time);
        SetClassIf(*this, "agent-row-user", model.FromUser);
    }
    if (!shown || shown->Header != model.Header)
        m_Header->SetText(model.Header);
    if (!shown || shown->Text != model.Text)
        m_Text->SetValue(model.Text);
    if (!shown || shown->ShowsText != model.ShowsText)
        SetClassIf(*m_Text, "hidden", !model.ShowsText);
    if (!shown || shown->Calls != model.Calls)
        ShowCalls(model.Calls);
    if (!shown || shown->Asks != model.Asks)
        SetClassIf(*this, "agent-row-asks", model.Asks);
    if (!shown || shown->ShowsUndoTurn != model.ShowsUndoTurn || shown->CanUndoTurn != model.CanUndoTurn ||
        shown->UndoTurnTooltip != model.UndoTurnTooltip)
    {
        SetClassIf(*m_UndoTurn, "hidden", !model.ShowsUndoTurn);
        m_UndoTurn->SetEnabled(model.CanUndoTurn);
        m_UndoTurn->SetTooltip(model.UndoTurnTooltip);
    }
    if (!shown || shown->Status != model.Status || shown->Failed != model.Failed || shown->Stopped != model.Stopped)
        ShowStatus(model);
    if (!shown || shown->Failed != model.Failed || shown->Retried != model.Retried)
    {
        SetClassIf(*m_Retry, "hidden", !model.Failed || model.Retried);
        SetClassIf(*m_Retried, "hidden", !model.Retried);
    }
    if (!shown || shown->CanStop != model.CanStop)
        SetClassIf(*m_Stop, "hidden", !model.CanStop);
    if (model.ShowsReplyActions && !m_ReplyActionsBuilt)
        BuildReplyActions();
    m_Shown = model;
}

void AgentConversationRow::ShowStatus(const AgentConversationRowModel& model)
{
    constexpr std::string_view kSeparator = " · ";
    m_Status->RemoveAllChildren();
    SetClassIf(*m_Status, "hidden", model.Status.empty());
    const std::string_view status = model.Status;
    for (size_t start = 0; start < status.size();)
    {
        const size_t end = std::min(status.find(kSeparator, start), status.size());
        // The separator leads the part after it, as in a call row's summary, so a line never
        // ends with one.
        const bool last = end == status.size();
        auto part = std::make_unique<Label>();
        part->AddClass("agent-row-status-part");
        SetClassIf(*part, "agent-row-status-failed", model.Failed);
        SetClassIf(*part, "agent-row-status-stopped", model.Stopped);
        part->SetText((start == 0 ? std::string() : std::string("· ")) +
                      std::string(status.substr(start, end - start)));
        m_Status->AddChild(std::move(part));
        start = last ? end : end + kSeparator.size();
    }
}

void AgentConversationRow::ShowCalls(const std::vector<AgentCallRowModel>& calls)
{
    while (m_CallRows.size() > calls.size())
    {
        m_Calls->RemoveChild(m_CallRows.back());
        m_CallRows.pop_back();
    }
    while (m_CallRows.size() < calls.size())
    {
        auto row = std::make_unique<AgentCallRow>(std::to_string(m_Index) + ":" + std::to_string(m_CallRows.size()),
                                                  m_OnAnswer);
        row->SetStacked(m_CallsStacked);
        m_CallRows.push_back(row.get());
        m_Calls->AddChild(std::move(row));
    }
    for (size_t call = 0; call < calls.size(); ++call)
        m_CallRows[call]->Show(calls[call]);
    SetClassIf(*m_Calls, "hidden", calls.empty());
}

void AgentConversationRow::BuildReplyActions()
{
    m_ReplyActionsBuilt = true;
    const std::span<const AgentReplyAction> actions = AgentReplyActions::All();
    for (size_t actionIndex = 0; actionIndex < actions.size(); ++actionIndex)
    {
        const AgentReplyAction& action = actions[actionIndex];
        auto button = std::make_unique<Button>();
        button->SetId("AgentRowAction:" + action.Id + ":" + std::to_string(m_Index));
        button->AddClass("small");
        button->AddClass("secondary");
        button->SetFocusable(false);
        button->SetText(action.Label);
        button->SetTooltip(action.Tooltip);
        button->SetOnClick([this, actionIndex](UIEvent&) { m_OnReplyAction(AgentReplyActions::All()[actionIndex]); });
        m_Actions->AddChild(std::move(button));
    }
}

void AgentConversationRow::SetCallsStacked(bool stacked)
{
    m_CallsStacked = stacked;
    if (m_Status && m_Status->GetParent())
        SetClassIf(*m_Status->GetParent(), "agent-row-footer-stacked", stacked);
    if (m_Actions)
        SetClassIf(*m_Actions, "agent-row-actions-stacked", stacked);
    for (AgentCallRow* call : m_CallRows)
        call->SetStacked(stacked);
}

UIElement* AgentConversationRow::AnswerFocus() const
{
    for (AgentCallRow* call : m_CallRows)
        if (UIElement* allow = call->AnswerFocus())
            return allow;
    return nullptr;
}

void AgentConversationRow::RefreshTextVisuals()
{
    if (m_Text)
        m_Text->MarkDirty(VisualDirty);
}

bool AgentConversationRow::FitHeight()
{
    if (!m_Text)
        return false;
    const float advance = m_Text->GetResolvedLineAdvancePx();
    if (advance <= 0.0f)
        return false;

    const auto& textBox = m_Text->GetResolvedStyle().Layout;
    const float textHeight = static_cast<float>(m_Text->GetVisualLineCount()) * advance + textBox.Padding.Top +
                             textBox.Padding.Bottom + textBox.BorderWidth.Top + textBox.BorderWidth.Bottom;
    bool moved = PinHeightPx(*m_Text, textHeight);
    for (AgentCallRow* call : m_CallRows)
    {
        moved = call->FitImage() || moved;
        moved = call->FitDetailsToggle() || moved;
        moved = call->FitReason() || moved;
    }

    float extent = 0.0f;
    const float top = GetLayoutY();
    for (const auto& child : GetChildren())
    {
        if (!child || child->HasClass("hidden"))
            continue;
        const float childHeight = child.get() == m_Text ? textHeight : child->GetLayoutHeight();
        extent = std::max(extent, child->GetLayoutY() - top + childHeight + child->GetResolvedStyle().Layout.Margin.Bottom);
    }
    const auto& rowBox = GetResolvedStyle().Layout;
    extent += rowBox.Padding.Bottom + rowBox.BorderWidth.Bottom;
    if (extent > 0.0f && PinHeightPx(*this, extent))
        moved = true;
    return moved;
}
} // namespace GameEngine
