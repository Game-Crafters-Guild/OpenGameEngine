#include "AgentCallRow.h"

#include "AgentCallImageView.h"
#include "AgentCallPieceView.h"
#include "AgentCallResourceResolver.h"
#include "AgentCallResourceTile.h"

#include "UI/Controls/Button.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace GameEngine
{
namespace
{
using Mark = AgentCallRowModel::Mark;

// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

// A reason cut to fit ends with an ellipsis; Details holds it whole.
// The most lines a reason takes.
constexpr int kReasonLines = 2;
constexpr const char* kEllipsis = "\xE2\x80\xA6";

// The Details toggle's two labels; the toggle keeps the wider one's width.
constexpr const char* kShowDetails = "Details";
constexpr const char* kHideDetails = "Hide details";

// The glyph classes (PromptPanel.css), one per mark; None draws no glyph.
constexpr const char* kGlyphClasses[] = {"agent-call-glyph-none", "agent-call-glyph-done", "agent-call-glyph-view",
                                         "agent-call-glyph-refused", "agent-call-glyph-waiting"};

const char* GlyphClass(Mark mark)
{
    return kGlyphClasses[static_cast<size_t>(mark)];
}

void SetClassIf(UIElement& element, const char* className, bool on)
{
    if (on)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}

// Adds a new `T` with `className` to `parent` and returns it.
template <typename T>
T* AddElement(UIElement& parent, const char* className)
{
    auto element = std::make_unique<T>();
    element->AddClass(className);
    T* added = element.get();
    parent.AddChild(std::move(element));
    return added;
}

Button* AddButton(UIElement& parent, const std::string& id, const char* text, const char* variant,
                  const char* tooltip)
{
    Button* button = AddElement<Button>(parent, "small");
    button->AddClass(variant);
    button->SetId(id);
    button->SetText(text);
    button->SetTooltip(tooltip);
    return button;
}
} // namespace

AgentCallRow::AgentCallRow(std::string idSuffix, AnswerHandler onAnswer)
    : m_IdSuffix(idSuffix)
    , m_OnAnswer(std::move(onAnswer))
{
    SetFocusable(false);
    AddClass("agent-call");

    m_Line = AddElement<UIElement>(*this, "agent-call-line");
    m_Glyph = AddElement<UIElement>(*m_Line, "agent-call-glyph");
    m_Action = AddElement<Label>(*m_Line, "agent-call-action");
    m_Break = AddElement<UIElement>(*m_Line, "agent-call-break");
    m_Break->AddClass("hidden");
    m_Summary = AddElement<UIElement>(*m_Line, "agent-call-summary");
    m_Trail = AddElement<UIElement>(*m_Line, "agent-call-trail");
    m_UndoState = AddElement<Label>(*m_Trail, "agent-call-undo");
    // A text link, not a button: it takes no plate, and Enter or Space toggles it.
    m_DetailsToggle = AddElement<Label>(*m_Trail, "agent-call-details-toggle");
    m_DetailsToggle->SetId("AgentCallDetails:" + idSuffix);
    m_DetailsToggle->SetText(kShowDetails);
    m_DetailsToggle->SetTooltip("The call the assistant sent and the editor's answer");
    m_DetailsToggle->SetFocusable(true);
    m_DetailsToggle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& event) { OnDetailsMouseUp(event); });
    m_DetailsToggle->RegisterEventHandler(kEventKeyDown, [this](UIEvent& event) { OnDetailsKeyDown(event); });

    m_AnswerLine = AddElement<UIElement>(*this, "agent-call-answer");
    m_Allow = AddButton(*m_AnswerLine, "AgentCallAllow:" + idSuffix, "Allow", "primary", "Run this call once");
    m_AllowForTurn = AddButton(*m_AnswerLine, "AgentCallAllowTurn:" + idSuffix, "Allow for this turn", "secondary",
                               "Run this call and every later call of the same tool in this reply");
    m_Deny = AddButton(*m_AnswerLine, "AgentCallDeny:" + idSuffix, "Deny", "secondary",
                       "Do not run this call; the assistant is told you declined");
    m_Allow->SetOnClick([this](UIEvent&) { Answer(AssistantAnswer::Allow); });
    m_AllowForTurn->SetOnClick([this](UIEvent&) { Answer(AssistantAnswer::AllowForTurn); });
    m_Deny->SetOnClick([this](UIEvent&) { Answer(AssistantAnswer::Deny); });
    m_Waiting = AddElement<Label>(*m_AnswerLine, "agent-call-waiting");

    m_Media = AddElement<UIElement>(*this, "agent-call-media");
    m_Media->AddClass("hidden");
    auto image = std::make_unique<AgentCallImageView>(idSuffix);
    image->AddClass("hidden");
    m_Image = image.get();
    m_Media->AddChild(std::move(image));
    m_Resources = AddElement<UIElement>(*m_Media, "agent-call-resources");
    m_Resources->AddClass("hidden");

    m_Details = AddElement<UIElement>(*this, "agent-call-details");
    m_Details->AddClass("hidden");
}

void AgentCallRow::Show(const AgentCallRowModel& model)
{
    const AgentCallRowModel* shown = m_Shown ? &*m_Shown : nullptr;
    if (!shown || shown->Glyph != model.Glyph)
    {
        if (shown)
            m_Glyph->RemoveClass(GlyphClass(shown->Glyph));
        m_Glyph->AddClass(GlyphClass(model.Glyph));
    }
    if (!shown || shown->Action != model.Action)
    {
        m_Action->SetText(model.Action);
        m_AllowForTurn->SetTooltip("Run this call and every later '" + model.Action + "' call in this reply");
    }
    if (!shown || shown->Summary != model.Summary || shown->Glyph != model.Glyph)
        ShowSummary(model);
    if (!shown || shown->UndoState != model.UndoState || shown->Glyph != model.Glyph)
    {
        m_UndoState->SetText(model.UndoState);
        SetClassIf(*m_UndoState, "hidden", model.UndoState.empty());
        SetClassIf(*m_UndoState, "agent-call-undo-refused", model.Glyph == Mark::Refused);
    }
    if (!shown || shown->Asks != model.Asks || shown->Waiting != model.Waiting)
    {
        SetClassIf(*m_AnswerLine, "hidden", model.Waiting.empty());
        for (Button* button : {m_Allow, m_AllowForTurn, m_Deny})
            SetClassIf(*button, "hidden", !model.Asks);
        m_Waiting->SetText(model.Waiting);
    }
    if (!shown || shown->Image != model.Image || shown->Resources != model.Resources)
        ShowMedia(model);
    m_DetailsStale = m_DetailsStale || !shown || shown->Details != model.Details;
    m_Shown = model;
    if (m_DetailsOpen && m_DetailsStale)
        ShowDetails(m_Shown->Details);
}

void AgentCallRow::ShowMedia(const AgentCallRowModel& model)
{
    const AgentCallRowModel* shown = m_Shown ? &*m_Shown : nullptr;
    if (model.Image)
        m_Image->Show(*model.Image);
    SetClassIf(*m_Image, "hidden", !model.Image);
    if (!shown || shown->Resources != model.Resources)
        ShowResources(model.Resources);
    SetClassIf(*m_Media, "hidden", !model.Image && m_Resources->GetChildren().empty());
}

void AgentCallRow::ShowResources(const std::vector<std::string>& references)
{
    m_Resources->RemoveAllChildren();
    const std::optional<AgentCallResourceResolver> resolver = AgentCallResourceResolver::ForEngine();
    const std::vector<AgentCallResource> resources =
        resolver ? resolver->ResolveAll(references) : std::vector<AgentCallResource>();
    for (size_t index = 0; index < resources.size(); ++index)
    {
        auto tile = std::make_unique<AgentCallResourceTile>(resources[index], m_IdSuffix + ":" + std::to_string(index));
        AgentCallResourceTile* added = tile.get();
        m_Resources->AddChild(std::move(tile));
        added->RequestThumbnail();
    }
    SetClassIf(*m_Resources, "hidden", m_Resources->GetChildren().empty());
}

bool AgentCallRow::FitImage()
{
    if (m_Image->HasClass("hidden"))
        return false;
    const auto& box = m_Media->GetResolvedStyle().Layout;
    // The height cap is the image's CSS max-height.
    const StyleLength cap = m_Image->GetResolvedStyle().Layout.MaxHeight;
    if (!cap.IsPx())
        return false;
    return m_Image->Fit(m_Media->GetLayoutWidth() - box.Padding.Left - box.Padding.Right, cap.Value);
}

void AgentCallRow::SetStacked(bool stacked)
{
    m_Stacked = stacked;
    SetClassIf(*m_Line, "agent-call-line-stacked", stacked);
    SetClassIf(*m_Break, "hidden", !stacked);
    SetClassIf(*m_Summary, "agent-call-summary-stacked", stacked);
    SetClassIf(*m_Trail, "agent-call-trail-stacked", stacked);
    PlaceTrail();
}

void AgentCallRow::PlaceTrail()
{
    UIElement* parent = m_Stacked ? m_Summary : m_Line;
    if (m_Trail->GetParent() == parent && parent->GetChildren().back().get() == m_Trail)
        return;
    std::unique_ptr<UIElement> trail = m_Trail->GetParent()->TakeChild(m_Trail);
    parent->AddChild(std::move(trail));
}

void AgentCallRow::ShowSummary(const AgentCallRowModel& model)
{
    // The trail waits on the line while the summary's pieces are rebuilt.
    if (m_Trail->GetParent() == m_Summary)
        m_Line->AddChild(m_Summary->TakeChild(m_Trail));
    ShowAgentCallPieces(*m_Summary, model.Summary, AgentCallPieceSurface::Summary);
    // A call that did not run ends its summary with why (AgentCallRows: the outcome is the
    // summary's last piece); that reason is the one part of a summary that can run long.
    m_Reason = nullptr;
    m_FullReason.clear();
    if (model.Glyph == Mark::Refused && !m_Summary->GetChildren().empty())
    {
        const auto& unit = m_Summary->GetChildren().back();
        if (!unit->GetChildren().empty())
            m_Reason = dynamic_cast<Label*>(unit->GetChildren().back().get());
    }
    if (m_Reason)
        m_FullReason = m_Reason->GetText();
    // Not fitted yet: the next FitReason fits it at whatever width the row has.
    m_ReasonFitWidth = -1.0f;
    PlaceTrail();
}

bool AgentCallRow::FitReason()
{
    UIManager* ui = GetOwnerManager();
    if (!m_Reason || !ui)
        return false;
    const float width = ReasonWrapWidth();
    if (width <= 0.0f || std::fabs(width - m_ReasonFitWidth) <= 0.5f)
        return false;
    // The text's lines are measured as the label's own layout measures them, so the search
    // runs within this one call and is right before the next layout draws it.
    const int wholeLines = ui->MeasureTextLineCount(*m_Reason, m_FullReason, width);
    if (wholeLines == 0)
        return false; // not laid out as text yet
    m_ReasonFitWidth = width;
    size_t shown = m_FullReason.size();
    if (wholeLines > kReasonLines)
    {
        // The longest cut that takes at most kReasonLines lines.
        size_t fits = 0;
        size_t over = m_FullReason.size();
        while (fits + 1 < over)
        {
            const size_t probe = fits + (over - fits) / 2;
            if (ui->MeasureTextLineCount(*m_Reason, ReasonCut(probe), width) <= kReasonLines)
                fits = probe;
            else
                over = probe;
        }
        shown = fits;
    }
    std::string text = ReasonCut(shown);
    if (text == m_Reason->GetText())
        return false;
    m_Reason->SetText(std::move(text));
    return true;
}

bool AgentCallRow::FitDetailsToggle()
{
    UIManager* ui = GetOwnerManager();
    if (!ui || m_DetailsToggle->Overrides().Get(Style::MinWidth).has_value())
        return false;
    const float widest = std::max(ui->MeasureTextWidth(*m_DetailsToggle, kShowDetails),
                                  ui->MeasureTextWidth(*m_DetailsToggle, kHideDetails));
    if (widest <= 0.0f)
        return false; // not laid out as text yet
    const auto& box = m_DetailsToggle->GetResolvedStyle().Layout;
    const float chrome = box.Padding.Left + box.Padding.Right + box.BorderWidth.Left + box.BorderWidth.Right;
    m_DetailsToggle->Overrides().Set(Style::MinWidth, StyleLength::Px(std::ceil(widest) + chrome));
    m_DetailsToggle->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
    return true;
}

float AgentCallRow::ReasonWrapWidth() const
{
    // The summary's content box, less the pieces before the reason in its unit and the gaps
    // between them: the width the label wraps at once its unit has a line of its own.
    const Box4& padding = m_Summary->GetLayoutPadding();
    const auto& border = m_Summary->GetResolvedStyle().Layout.BorderWidth;
    float width = m_Summary->GetLayoutWidth() - padding.Left - padding.Right - border.Left - border.Right;
    const UIElement* unit = m_Reason->GetParent();
    const auto& unitLayout = unit->GetResolvedStyle().Layout;
    const float gap = std::max(unitLayout.ColumnGap, unitLayout.Gap);
    for (const auto& piece : unit->GetChildren())
        if (piece.get() != m_Reason)
            width -= piece->GetLayoutWidth() + gap;
    return width;
}

std::string AgentCallRow::ReasonCut(size_t bytes) const
{
    if (bytes >= m_FullReason.size())
        return m_FullReason;
    // Cut after a whole word or JSON token: before a space, or after a comma, a colon or a
    // closing quote (one not followed by the string it opens), never after an opening bracket
    // or brace. A first token longer than the cut is cut on a character, never inside one.
    size_t end = bytes;
    size_t breakAt = end > 0 ? m_FullReason.find_last_of(" ,:\"", end - 1) : std::string::npos;
    while (breakAt != std::string::npos && breakAt > 0 && m_FullReason[breakAt] == '"' &&
           breakAt + 1 < m_FullReason.size() && std::isalnum(static_cast<unsigned char>(m_FullReason[breakAt + 1])))
        breakAt = m_FullReason.find_last_of(" ,:\"", breakAt - 1);
    if (breakAt != std::string::npos && breakAt > 0)
        end = m_FullReason[breakAt] == ' ' ? breakAt : breakAt + 1;
    while (end > 0 && (static_cast<unsigned char>(m_FullReason[end]) & 0xC0) == 0x80)
        --end;
    size_t trimmed = end;
    while (trimmed > 0 && (m_FullReason[trimmed - 1] == ' ' || m_FullReason[trimmed - 1] == '[' ||
                           m_FullReason[trimmed - 1] == '{'))
        --trimmed;
    // A reason that opens with brackets keeps them rather than showing the ellipsis alone.
    if (trimmed > 0)
        end = trimmed;
    return m_FullReason.substr(0, end) + kEllipsis;
}

void AgentCallRow::OnDetailsMouseUp(UIEvent& event)
{
    if (event.Button != kLeftButton)
        return;
    event.Stop();
    ToggleDetails();
}

void AgentCallRow::OnDetailsKeyDown(UIEvent& event)
{
    if (event.Key != Input::kKeyCode_Enter && event.Key != Input::kKeyCode_Space)
        return;
    event.Stop();
    ToggleDetails();
}

UIElement* AgentCallRow::AnswerFocus() const
{
    return m_Shown && m_Shown->Asks ? m_Allow : nullptr;
}

void AgentCallRow::Answer(AssistantAnswer answer)
{
    if (m_Shown && m_Shown->Asks)
        m_OnAnswer(m_Shown->RequestId, answer);
}

void AgentCallRow::ShowDetails(const std::vector<AgentCallPieces>& lines)
{
    m_DetailsStale = false;
    m_Details->RemoveAllChildren();
    for (const AgentCallPieces& pieces : lines)
    {
        auto line = std::make_unique<UIElement>();
        line->AddClass("agent-call-details-line");
        ShowAgentCallPieces(*line, pieces, AgentCallPieceSurface::Details);
        m_Details->AddChild(std::move(line));
    }
}

void AgentCallRow::ToggleDetails()
{
    m_DetailsOpen = !m_DetailsOpen;
    if (m_DetailsOpen && m_DetailsStale && m_Shown)
        ShowDetails(m_Shown->Details);
    SetClassIf(*m_Details, "hidden", !m_DetailsOpen);
    m_DetailsToggle->SetText(m_DetailsOpen ? kHideDetails : kShowDetails);
}
} // namespace GameEngine
