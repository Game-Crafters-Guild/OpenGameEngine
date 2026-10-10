#include "Panels/AddTagModal.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/EditorTags.h"
#include "UI/StyleProperties.h"
#include "UI/Registration/ElementRegistration.h"
#include "Input/KeyCodes.h"

#include <memory>

namespace GameEngine {

namespace {

const char* kTagRowClass = "add-tag-modal-tag-row";

}

AddTagModal::AddTagModal()
{
    AddClass("add-tag-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    backdrop->AddClass("add-tag-modal-backdrop");
    backdrop->SetFocusable(true);
    backdrop->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if (e.Button == 0 && e.Target == m_Backdrop && !IsWindowDragging())
            OnBackdropClicked();
    });
    backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            Hide();
        }
    });

    auto window = std::make_unique<UIElement>();
    m_Window = window.get();
    m_Window->AddClass("modal-window");
    m_Window->AddClass("add-tag-modal-window");

    // Header: the drag handle.
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("add-tag-modal-header");
        BindDragHandle(header.get());
        auto title = std::make_unique<Label>();
        title->SetText("Tags");
        title->AddClass("add-tag-modal-title");
        header->AddChild(std::move(title));
        window->AddChild(std::move(header));
    }

    // Scrollable tag list
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->AddClass("add-tag-modal-scroll");
    auto contentContainer = std::make_unique<UIElement>();
    contentContainer->AddClass("add-tag-modal-list");
    m_TagListContainer = contentContainer.get();
    scrollView->AddContent(std::move(contentContainer));
    window->AddChild(std::move(scrollView));

    // "All Tags…" row (added to container in BuildTagList)
    auto allTagsRow = std::make_unique<UIElement>();
    m_AllTagsRow = allTagsRow.get();
    m_AllTagsRow->AddClass("add-tag-modal-all-row");
    m_AllTagsRow->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if (e.Button == 0)
            OnAllTagsClicked();
    });
    auto allTagsCircle = std::make_unique<UIElement>();
    allTagsCircle->AddClass("add-tag-modal-circle");
    auto allTagsLabel = std::make_unique<Label>();
    allTagsLabel->SetText("All Tags…");
    allTagsLabel->AddClass("add-tag-modal-all-label");
    m_AllTagsRow->AddChild(std::move(allTagsCircle));
    m_AllTagsRow->AddChild(std::move(allTagsLabel));
    m_TagListContainer->AddChild(std::move(allTagsRow));

    // Inline "Add new tag" section (hidden by default)
    auto addNewSection = std::make_unique<UIElement>();
    m_AddNewSection = addNewSection.get();
    m_AddNewSection->AddClass("add-tag-modal-new-section");
    auto nameLabel = std::make_unique<Label>();
    nameLabel->SetText("New tag name");
    nameLabel->AddClass("add-tag-modal-new-label");
    m_AddNewSection->AddChild(std::move(nameLabel));
    auto nameField = std::make_unique<TextField>();
    m_NewTagNameField = nameField.get();
    nameField->SetValue("");
    nameField->AddClass("add-tag-modal-new-field");
    auto addBtn = std::make_unique<Button>();
    m_AddTagButton = addBtn.get();
    addBtn->SetText("Add");
    addBtn->AddClass("secondary");
    addBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnAddNewTagClicked(); });
    m_AddNewSection->AddChild(std::move(nameField));
    m_AddNewSection->AddChild(std::move(addBtn));
    m_TagListContainer->AddChild(std::move(addNewSection));

    backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
    PlaceWindow(0.0f, 0.0f);
}

void AddTagModal::GetWindowOrigin(float& x, float& y) const
{
    x = m_WindowOffsetX;
    y = m_WindowOffsetY;
}

void AddTagModal::PlaceWindow(float x, float y)
{
    m_WindowOffsetX = x;
    m_WindowOffsetY = y;
    if (!m_Window)
        return;
    // Flex centering from backdrop handles the 50% base; margins apply the offset.
    m_Window->Overrides()
        .Set(Style::MarginLeft, StyleLength::Px(m_WindowOffsetX))
        .Set(Style::MarginTop, StyleLength::Px(m_WindowOffsetY));
}

void AddTagModal::Show(const std::vector<std::filesystem::path>& paths)
{
    m_Paths = paths;
    CancelWindowDrag();
    PlaceWindow(0.0f, 0.0f);
    if (m_NewTagNameField)
        m_NewTagNameField->SetValue("");
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
    // Defer building the tag list so the modal appears immediately and the UI stays responsive.
    PostAction([this]() { BuildTagList(); });
}

void AddTagModal::Hide()
{
    RemoveClass("visible");
}

void AddTagModal::BuildTagList()
{
    if (!m_TagListContainer)
        return;

    // Take out "All Tags…" and "Add new" section so we can re-add after tag rows
    std::unique_ptr<UIElement> allTagsRowPtr = m_TagListContainer->TakeChild(m_AllTagsRow);
    std::unique_ptr<UIElement> addNewPtr = m_TagListContainer->TakeChild(m_AddNewSection);

    // Remove any existing tag rows from a previous BuildTagList() so we don't duplicate.
    // This whole function must run outside event dispatch, which is why both
    // callers post it: TakeChild returns null during dispatch, so the two rows
    // held above would be dropped and this loop would never end.
    while (!m_TagListContainer->GetChildren().empty())
        (void)m_TagListContainer->TakeChild(m_TagListContainer->GetChildren()[0].get());

    const std::vector<EditorTagDefinition> tags = EditorTags::Load();
    for (const auto& tag : tags)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass(kTagRowClass);
        const std::string tagName = tag.Name;
        row->RegisterEventHandler(kEventMouseDown, [this, tagName](UIEvent& e) {
            if (e.Button == 0)
                OnTagRowClicked(tagName);
        });

        auto circle = std::make_unique<UIElement>();
        circle->AddClass("add-tag-modal-circle");
        circle->Overrides()
            .Set(Style::BorderColor, BorderColorsTRBL{tag.Color.empty() ? (uint32_t)0xFF666666 : (uint32_t)0x4DFFFFFF, tag.Color.empty() ? (uint32_t)0xFF666666 : (uint32_t)0x4DFFFFFF, tag.Color.empty() ? (uint32_t)0xFF666666 : (uint32_t)0x4DFFFFFF, tag.Color.empty() ? (uint32_t)0xFF666666 : (uint32_t)0x4DFFFFFF})
            .Set(Style::BackgroundColor, tag.Color.empty() ? (uint32_t)0x00000000
                : UIRegistration::Parser<uint32_t>::Parse(tag.Color));

        auto label = std::make_unique<Label>();
        label->SetText(tag.Name);
        label->AddClass("add-tag-modal-label");

        row->AddChild(std::move(circle));
        row->AddChild(std::move(label));
        m_TagListContainer->AddChild(std::move(row));
    }

    m_TagListContainer->AddChild(std::move(allTagsRowPtr));
    m_TagListContainer->AddChild(std::move(addNewPtr));
}

void AddTagModal::OnTagRowClicked(const std::string& tagName)
{
    if (m_OnAssignTag && !m_Paths.empty())
        m_OnAssignTag(m_Paths, tagName);
    Hide();
}

void AddTagModal::OnAllTagsClicked()
{
    Hide();
    if (m_OnOpenSettings)
        m_OnOpenSettings();
}

void AddTagModal::OnAddNewTagClicked()
{
    if (!m_NewTagNameField)
        return;
    std::string name = m_NewTagNameField->GetValue();
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.pop_back();
    for (size_t i = 0; i < name.size(); )
        if (name[i] == ' ' || name[i] == '\t')
            name.erase(i, 1);
        else
            ++i;
    if (name.empty())
        return;
    m_NewTagNameField->SetValue("");
    // Defer Load/Save/BuildTagList so the click handler returns and the UI stays responsive.
    PostAction([this, name]() {
        std::vector<EditorTagDefinition> tags = EditorTags::Load();
        tags.push_back({name, "#95a5a6"});
        if (EditorTags::Save(tags))
            BuildTagList();
    });
}

void AddTagModal::OnBackdropClicked()
{
    Hide();
}

} // namespace GameEngine
