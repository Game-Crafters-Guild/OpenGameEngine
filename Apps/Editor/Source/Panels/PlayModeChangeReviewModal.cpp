#include "Panels/PlayModeChangeReviewModal.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIManager.h"

namespace GameEngine
{

PlayModeChangeReviewModal::PlayModeChangeReviewModal()
{
    // Styling is owned by editor theme CSS (Assets/UI/theme/core.css).
    // This keeps modal presentation tweakable without recompiling the editor.
    AddClass("playmode-change-review-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    // Backdrop
    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("pmcr-backdrop");
    m_Backdrop->SetFocusable(true);

    // Window
    auto window = std::make_unique<UIElement>();
    m_Window = window.get();
    m_Window->AddClass("modal-window");
    m_Window->AddClass("pmcr-window");

    // Header
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("pmcr-header");
        auto title = std::make_unique<Label>();
        title->SetText("Play Mode Changes");
        title->AddClass("pmcr-title");
        auto subtitle = std::make_unique<Label>();
        subtitle->SetText("Select the changes you want to apply back to the scene.");
        subtitle->AddClass("pmcr-subtitle");

        header->AddChild(std::move(title));
        header->AddChild(std::move(subtitle));
        m_Window->AddChild(std::move(header));
    }

    // Content: list + help
    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("pmcr-content");

        auto help = std::make_unique<Label>();
        help->SetText("Applied changes are pushed onto the Undo stack as a single compound action.");
        help->AddClass("pmcr-help");
        content->AddChild(std::move(help));

        auto scroll = std::make_unique<ScrollView>();
        scroll->AddClass("pmcr-scroll");

        auto listRoot = std::make_unique<UIElement>();
        listRoot->AddClass("pmcr-list");
        m_ListRoot = listRoot.get();
        scroll->AddContent(std::move(listRoot));

        content->AddChild(std::move(scroll));
        m_Window->AddChild(std::move(content));
    }

    // Footer
    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("pmcr-footer");

        auto discard = std::make_unique<Button>();
        discard->SetText("Discard");
        discard->AddClass("secondary");
        discard->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnDiscardClicked(); });

        auto apply = std::make_unique<Button>();
        apply->SetText("Apply Selected");
        apply->AddClass("primary");
        apply->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnApplyClicked(); });

        footer->AddChild(std::move(discard));
        footer->AddChild(std::move(apply));
        m_Window->AddChild(std::move(footer));
    }

    m_Backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
}

void PlayModeChangeReviewModal::Show(const std::vector<std::string>& changeNames)
{
    RebuildList(changeNames);
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void PlayModeChangeReviewModal::Hide()
{
    RemoveClass("visible");
}

void PlayModeChangeReviewModal::RebuildList(const std::vector<std::string>& changeNames)
{
    if (!m_ListRoot)
        return;

    // If we're in the middle of event dispatch, defer rebuilding until it's safe.
    if (UIElement::IsInEventDispatch())
    {
        std::vector<std::string> copy = changeNames;
        PostAction([this, copy]() { this->RebuildList(copy); });
        return;
    }

    // Clear existing rows (UIElement has no ClearChildren helper).
    m_ListRoot->RemoveAllChildren();
    m_Toggles.clear();
    m_Toggles.reserve(changeNames.size());

    for (const auto& name : changeNames)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("pmcr-row");

        auto toggle = std::make_unique<Toggle>();
        auto* t = toggle.get();
        t->SetChecked(true);
        t->AddClass("pmcr-toggle");
        m_Toggles.push_back(t);

        auto label = std::make_unique<Label>();
        label->SetText(name);
        label->AddClass("pmcr-row-label");

        row->AddChild(std::move(toggle));
        row->AddChild(std::move(label));
        m_ListRoot->AddChild(std::move(row));
    }
}

void PlayModeChangeReviewModal::OnApplyClicked()
{
    Hide();
    if (!m_OnApply)
        return;
    std::vector<bool> keep;
    keep.reserve(m_Toggles.size());
    for (auto* t : m_Toggles)
    {
        keep.push_back(t ? t->IsChecked() : false);
    }
    m_OnApply(keep);
}

void PlayModeChangeReviewModal::OnDiscardClicked()
{
    Hide();
    if (m_OnDiscard)
        m_OnDiscard();
}

} // namespace GameEngine
