#include "AgentModelPopover.h"

#include "AiAssistantSettings.h"
#include "ModelDisplayName.h"

#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Layout/PopupPlacement.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "ai-assistant:Editor/UI/controls/AgentModelPopover.uxml";
// Until the panel has a layout of its own: .agent-model-popover-panel's width, and the
// header, two rows and the caption.
constexpr float kPanelWidthEstimatePx = 300.0f;
constexpr float kPanelHeightEstimatePx = 160.0f;
// Between the panel's bottom edge and the button's top edge.
constexpr float kAnchorGapPx = 6.0f;
// The panel keeps this far from the AI Assistant panel's edges.
constexpr float kEdgePaddingPx = 8.0f;
// A placement that moves less than this is left alone, so an open popover costs no
// layout while nothing moves.
constexpr float kPlacementEpsilonPx = 0.25f;
constexpr const char* kButtonTooltip =
    "The model and effort the next prompt asks for: click to change them. They apply from the next prompt, "
    "in the same conversation.";
constexpr const char* kEffortTooltip = "How much the model reasons before it answers: higher levels answer more "
                                       "carefully, take longer and cost more.";

// " · High" for the effort `effortId` of `providerId`; empty for a connection without
// effort levels.
std::string EffortSuffix(std::string_view providerId, std::string_view effortId)
{
    for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::EffortChoices(providerId))
        if (choice.Id == effortId)
            return " · " + std::string(choice.Label);
    return {};
}

const AiAssistantSettings::Choice* FindChoice(std::span<const AiAssistantSettings::Choice> choices,
                                              std::string_view id)
{
    for (const AiAssistantSettings::Choice& choice : choices)
        if (choice.Id == id)
            return &choice;
    return nullptr;
}
} // namespace

AgentModelPopover::AgentModelPopover(Button& button)
    : DismissablePopup(this)
    , m_Button(button)
{
    // Above the panel's neighbours too, as a dropdown's list is. No z-index of its own:
    // hit testing compares z-indices across stacking contexts (issue #3541), so with
    // one its model list, drawn on top, would lose the pointer to the popover.
    SetOverlayLayer(OverlayLayer::Dropdown);
    m_Button.SetTooltip(kButtonTooltip);
    m_Button.SetOnClick([this](UIEvent&) { Open(); });
}

bool AgentModelPopover::Build(UIManager& ui)
{
    if (!ui.InstantiateLayoutChildrenFromAssetPath(this, kLayoutAssetPath))
    {
        Logger::Log::Error("AI Assistant: {} did not instantiate; check that the package's assets are staged.",
                           kLayoutAssetPath);
        return false;
    }
    m_Panel = FindById("AgentModelPopoverPanel");
    m_ModelDropdown = dynamic_cast<Dropdown*>(FindById("AgentModelDropdown"));
    m_EffortRow = FindById("AgentEffortRow");
    m_EffortSlider = dynamic_cast<Slider*>(FindById("AgentEffortSlider"));
    m_EffortValue = dynamic_cast<Label*>(FindById("AgentEffortValue"));
    m_EffortEnds = FindById("AgentEffortEnds");
    m_EffortLowest = dynamic_cast<Label*>(FindById("AgentEffortLowest"));
    m_EffortHighest = dynamic_cast<Label*>(FindById("AgentEffortHighest"));
    if (!m_Panel || !m_ModelDropdown || !m_EffortRow || !m_EffortSlider || !m_EffortValue || !m_EffortEnds ||
        !m_EffortLowest || !m_EffortHighest)
    {
        Logger::Log::Error("AI Assistant: {} is missing one of its controls.", kLayoutAssetPath);
        return false;
    }
    m_EffortRow->SetTooltip(kEffortTooltip);
    m_ModelDropdown->SetOnValueChanged([this](const std::string& modelId) { OnModelChosen(modelId); });
    // The row's gap already spaces the track from the label.
    m_EffortSlider->SetTrackPaddingPx(0.0f);
    m_EffortSlider->SetOnValueChanging([this](const float& stop) { OnEffortMoved(stop, false); });
    m_EffortSlider->SetOnValueChanged([this](const float& stop) { OnEffortMoved(stop, true); });
    m_Generation = UINT64_MAX;
    m_ModelLabels.clear();
    if (!m_ProviderId.empty())
        Refresh();
    return true;
}


void AgentModelPopover::SetProvider(std::string providerId)
{
    if (providerId == m_ProviderId)
        return;
    m_ProviderId = std::move(providerId);
    Close();
    m_ModelLabels.clear();
    if (AiAssistantSettings::ModelChoices(m_ProviderId).empty())
    {
        m_Button.AddClass("hidden");
        return;
    }
    m_Button.RemoveClass("hidden");
    Refresh();
}

void AgentModelPopover::Update()
{
    if (AiAssistantSettings::ModelGeneration() != m_Generation)
        Refresh();
    // The button's text is measured once it has been laid out as text.
    else if (!m_Button.Overrides().Get(Style::Width) && !m_Button.HasClass("hidden"))
        SizeButton();
    else if (std::fabs(m_Button.GetLayoutWidth() - m_FittedWidthPx) > kPlacementEpsilonPx)
        FitButtonText();
    if (m_Open)
        Place();
}

void AgentModelPopover::Open()
{
    if (m_Open || !m_Panel || AiAssistantSettings::ModelChoices(m_ProviderId).empty())
        return;
    Refresh();
    m_Open = true;
    RemoveClass("hidden");
    Place();
}

void AgentModelPopover::Close()
{
    if (!m_Open)
        return;
    m_Open = false;
    AddClass("hidden");
}

void AgentModelPopover::Refresh()
{
    m_Generation = AiAssistantSettings::ModelGeneration();
    const std::string model = AiAssistantSettings::Model(m_ProviderId);
    const std::string answered = AiAssistantSettings::AnsweredModel(m_ProviderId, model);
    const std::string effortSuffix = EffortSuffix(m_ProviderId, AiAssistantSettings::Effort(m_ProviderId));
    m_FullText = AiAssistantSettings::ModelLabel(m_ProviderId, model) + effortSuffix;
    m_ModelText = ModelDisplayName(answered.empty() ? model : answered);
    m_CompactText = m_ModelText + effortSuffix;
    m_Button.SetTooltip(m_FullText + "\n" + kButtonTooltip);
    SizeButton();
    FitButtonText();
    if (!m_Panel)
        return;
    m_Refreshing = true;
    RefreshModelOptions();

    const std::span<const AiAssistantSettings::Choice> efforts = AiAssistantSettings::EffortChoices(m_ProviderId);
    if (efforts.empty())
    {
        m_EffortRow->AddClass("hidden");
        m_EffortEnds->AddClass("hidden");
    }
    else
    {
        m_EffortRow->RemoveClass("hidden");
        m_EffortEnds->RemoveClass("hidden");
        m_EffortLowest->SetText(std::string(efforts.front().Label));
        m_EffortHighest->SetText(std::string(efforts.back().Label));
        std::vector<float> stops;
        for (size_t stop = 0; stop < efforts.size(); ++stop)
            stops.push_back(static_cast<float>(stop));
        m_EffortSlider->SetMax(stops.back());
        m_EffortSlider->SetTickMarks(std::move(stops));
        const AiAssistantSettings::Choice* effort = FindChoice(efforts, AiAssistantSettings::Effort(m_ProviderId));
        const size_t stop = effort ? static_cast<size_t>(effort - efforts.data()) : 0;
        m_EffortSlider->SetValueWithoutNotify(static_cast<float>(stop));
        m_EffortValue->SetText(std::string(efforts[stop].Label));
    }
    m_Refreshing = false;
}

void AgentModelPopover::SizeButton()
{
    // Every text the button can show for the connection, the "(latest)" forms
    // included after a turn has named a version, so the width stays put.
    float widest = 0.0f;
    const std::span<const AiAssistantSettings::Choice> efforts = AiAssistantSettings::EffortChoices(m_ProviderId);
    for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::ModelChoices(m_ProviderId))
        for (const std::string& name : {std::string(choice.Label), AiAssistantSettings::ModelLabel(m_ProviderId, choice.Id)})
        {
            if (efforts.empty())
                widest = std::max(widest, m_Button.WidthForText(name));
            for (const AiAssistantSettings::Choice& effort : efforts)
                widest = std::max(widest, m_Button.WidthForText(name + EffortSuffix(m_ProviderId, effort.Id)));
        }
    // 0 before the button has been laid out as text.
    if (widest <= 0.0f)
        return;
    const float width = std::ceil(widest);
    const auto current = m_Button.Overrides().Get(Style::Width);
    if (current && current->IsPx() && std::fabs(current->Value - width) <= kPlacementEpsilonPx)
        return;
    m_Button.Overrides().Set(Style::Width, StyleLength::Px(width));
    m_Button.MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
}

void AgentModelPopover::FitButtonText()
{
    m_FittedWidthPx = m_Button.GetLayoutWidth();
    // The whole text, then without "(latest)", then the model alone: the label is cut
    // only when even the model does not fit. Before its first layout the button shows
    // the whole text.
    std::string text = m_FullText;
    if (m_FittedWidthPx > 0.0f)
        for (const std::string* candidate : {&m_FullText, &m_CompactText, &m_ModelText})
        {
            text = *candidate;
            const float needed = m_Button.WidthForText(text);
            if (needed > 0.0f && needed <= m_FittedWidthPx + kPlacementEpsilonPx)
                break;
        }
    if (m_Button.GetText() != text)
        m_Button.SetText(text);
}

void AgentModelPopover::RefreshModelOptions()
{
    const std::span<const AiAssistantSettings::Choice> models = AiAssistantSettings::ModelChoices(m_ProviderId);
    std::vector<std::string> labels;
    for (const AiAssistantSettings::Choice& choice : models)
        labels.push_back(AiAssistantSettings::ModelLabel(m_ProviderId, choice.Id));
    const AiAssistantSettings::Choice* model = FindChoice(models, AiAssistantSettings::Model(m_ProviderId));
    const int selected = model ? static_cast<int>(model - models.data()) : 0;
    // The rows change only with the connection or a model that answered; a choice
    // only moves the selection.
    if (labels == m_ModelLabels)
    {
        m_ModelDropdown->SetSelectedIndexWithoutNotify(selected);
        return;
    }
    std::vector<Dropdown::Option> options;
    for (size_t index = 0; index < models.size(); ++index)
        options.push_back({std::string(models[index].Id), labels[index], ""});
    m_ModelDropdown->SetOptions(options, selected);
    m_ModelLabels = std::move(labels);
}

void AgentModelPopover::Place()
{
    const UIElement* parent = GetParent();
    if (!parent)
        return;
    const float parentWidth = parent->GetLayoutWidth();
    const float parentHeight = parent->GetLayoutHeight();
    if (parentWidth <= 0.0f || parentHeight <= 0.0f)
        return;
    // Layout rectangles are window-absolute; the panel is placed in the parent's space.
    const float buttonCenterX = m_Button.GetLayoutX() + m_Button.GetLayoutWidth() * 0.5f - parent->GetLayoutX();
    const float buttonTop = m_Button.GetLayoutY() - parent->GetLayoutY();
    const float width = m_Panel->GetLayoutWidth() > 0.0f ? m_Panel->GetLayoutWidth() : kPanelWidthEstimatePx;
    const float height = m_Panel->GetLayoutHeight() > 0.0f ? m_Panel->GetLayoutHeight() : kPanelHeightEstimatePx;
    // Above the button, which sits on the panel's bottom line.
    const UI::Layout::PopupPosition placed =
        UI::Layout::ClampPopupToViewport({0.0f, 0.0f, parentWidth, parentHeight}, buttonCenterX - width * 0.5f,
                                         buttonTop - kAnchorGapPx - height, width, height, kEdgePaddingPx);

    const auto moved = [](const std::optional<StyleLength>& current, float value)
    { return !current || !current->IsPx() || std::fabs(current->Value - value) > kPlacementEpsilonPx; };
    if (!moved(m_Panel->Overrides().Get(Style::PositionLeft), placed.X) &&
        !moved(m_Panel->Overrides().Get(Style::PositionTop), placed.Y))
        return;
    m_Panel->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(placed.X))
        .Set(Style::PositionTop, StyleLength::Px(placed.Y));
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void AgentModelPopover::OnModelChosen(const std::string& modelId)
{
    if (m_Refreshing)
        return;
    AiAssistantSettings::SetModel(m_ProviderId, modelId);
    Refresh();
}

void AgentModelPopover::OnEffortMoved(float stop, bool commit)
{
    if (m_Refreshing)
        return;
    const std::span<const AiAssistantSettings::Choice> efforts = AiAssistantSettings::EffortChoices(m_ProviderId);
    if (efforts.empty())
        return;
    const size_t index = std::min(static_cast<size_t>(std::max(0l, std::lround(stop))), efforts.size() - 1);
    m_EffortValue->SetText(std::string(efforts[index].Label));
    // A drag shows each stop it passes and stores the one it ends on.
    if (!commit)
        return;
    AiAssistantSettings::SetEffort(m_ProviderId, efforts[index].Id);
    Refresh();
}
} // namespace GameEngine
