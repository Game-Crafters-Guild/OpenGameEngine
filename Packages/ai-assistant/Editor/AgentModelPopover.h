#pragma once

#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class Button;
class Dropdown;
class Label;
class Slider;

/// The AI Assistant panel's model control: a button beside the connection dropdown
/// that reads the model and effort the next prompt asks for ("Opus (latest) · High",
/// then "Opus 5.5 · High" once a turn has named the version), and the popover it opens
/// above itself (AgentModelPopover.uxml), with the connection's models in a dropdown
/// and its effort levels as the stops of a slider. Every change is stored at once in
/// the user's settings (AiAssistantSettings::SetModel, SetEffort) and applies from the
/// next prompt, in the same conversation; the popover closes on an outside press or
/// Escape. Add it to the panel, which is the space it places itself in, then Build().
class AgentModelPopover final : public UIElement, public DismissablePopup
{
public:
    explicit AgentModelPopover(Button& button);

    /// Instantiates the popover's layout; false, with the reason in the log, when the
    /// package's layout is missing.
    bool Build(UIManager& ui);

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }
    bool IsPopupOpen() const override { return m_Open; }
    void DismissPopup() override { Close(); }

    /// Follows the connection: the button shows for one with a model list and hides,
    /// closing the popover, for any other.
    void SetProvider(std::string providerId);
    /// Once per frame: follows a choice made on the settings page and a model that
    /// answered, and keeps an open popover above the button when the panel's layout
    /// moved it.
    void Update();

    void Open();

private:
    void Close();
    void Refresh();
    /// Sizes the button to the longest text it can show for the connection, so its
    /// width follows the panel's and never its text: the row does not move when the
    /// choice changes.
    void SizeButton();
    /// Shows the whole text when it fits the button's width, else drops "(latest)",
    /// then the effort, before the label is cut; the tooltip always holds the whole text.
    void FitButtonText();
    void RefreshModelOptions();
    void Place();
    void OnModelChosen(const std::string& modelId);
    void OnEffortMoved(float stop, bool commit);

    Button& m_Button;
    std::string m_ProviderId;
    bool m_Open = false;
    // AiAssistantSettings::ModelGeneration() the button and the controls last showed.
    uint64_t m_Generation = UINT64_MAX;
    // Set while Refresh() writes the controls, whose callbacks then report a change
    // the user did not make.
    bool m_Refreshing = false;
    // The labels m_ModelDropdown's options show; its rows are rebuilt only when they change.
    std::vector<std::string> m_ModelLabels;
    // What the button reads: whole ("Opus (latest) · High"), without "(latest)"
    // ("Opus · High") and the model alone ("Opus"); FitButtonText() picks one.
    std::string m_FullText;
    std::string m_CompactText;
    std::string m_ModelText;
    // The button width FitButtonText() last fitted the text to.
    float m_FittedWidthPx = -1.0f;
    UIElement* m_Panel = nullptr;
    Dropdown* m_ModelDropdown = nullptr;
    UIElement* m_EffortRow = nullptr;
    Slider* m_EffortSlider = nullptr;
    Label* m_EffortValue = nullptr;
    // The row naming the slider's two ends, under its track.
    UIElement* m_EffortEnds = nullptr;
    Label* m_EffortLowest = nullptr;
    Label* m_EffortHighest = nullptr;
};
} // namespace GameEngine
