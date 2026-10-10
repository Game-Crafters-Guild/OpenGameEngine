#include "Assets/AssetRenameController.h"

#include "Assets/AssetRename.h"
#include "UndoRedo/RenameAssetFileCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

namespace GameEngine {

namespace {

constexpr const char* kFieldClass = "asset-rename-field";
constexpr const char* kInvalidClass = "invalid";

// Enter commits and Escape cancels the whole session whether or not the text
// changed: the base field only reports edits, and an unchanged Enter must
// still close the editor.
class RenameTextField final : public TextField
{
public:
    std::function<void()> OnEnter;
    std::function<void()> OnEscape;

    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override
    {
        if (key == Input::kKeyCode_Enter || key == Input::kKeyCode_NumPadEnter)
        {
            if (OnEnter) OnEnter();
            return true;
        }
        if (key == Input::kKeyCode_Escape)
        {
            if (OnEscape) OnEscape();
            return true;
        }
        return TextField::OnKey(key, mods, platform);
    }
};

// The field sits inside the item cell, whose press/drag/rename handlers must
// not see clicks meant for the caret.
void IsolatePointerEventsFromHost(UIElement& field)
{
    for (const EventId id : {kEventMouseDown, kEventMouseUp, kEventMouseMove})
        field.RegisterEventHandler(id, [](UIEvent& e) { e.Stop(); });
}

} // namespace

AssetRenameController::~AssetRenameController()
{
    End();
}

void AssetRenameController::Begin(Label* title,
                                  UIElement* focusOnEnd,
                                  const std::filesystem::path& itemPath,
                                  Editor::UndoRedoService* undo,
                                  AssetManager* assets,
                                  OnRenamed onRenamed)
{
    if (m_Field && m_ItemPath == itemPath)
        return;
    End();
    if (!title || !title->GetParent() || itemPath.empty())
        return;

    m_Title = title;
    m_FocusOnEnd = focusOnEnd;
    m_ItemPath = itemPath;
    m_Undo = undo;
    m_Assets = assets;
    m_OnRenamed = std::move(onRenamed);

    auto field = std::make_unique<RenameTextField>();
    field->AddClass(kFieldClass);
    field->SetValue(itemPath.stem().string());
    field->SetFocusable(true);
    field->OnEnter = [this]() { Commit(); };
    field->OnEscape = [this]() { End(); };
    field->SetOnValueChanged([this](const std::string& stem) { ApplyValidation(stem); });
    // Clicking away is a commit, the same as every file manager.
    field->RegisterEventHandler(kEventFocusOut, [this](UIEvent&) { Commit(); });
    IsolatePointerEventsFromHost(*field);
    m_Field = field.get();

    UI::Layout::SetElementHidden(*m_Title, true);
    UIElement* host = m_Title->GetParent();
    const auto& siblings = host->GetChildren();
    std::size_t insertAt = siblings.size();
    for (std::size_t i = 0; i < siblings.size(); ++i)
    {
        if (siblings[i].get() == m_Title)
        {
            insertAt = i + 1;
            break;
        }
    }
    host->InsertChild(insertAt, std::move(field));

    if (UIManager* ui = host->GetOwnerManager())
    {
        ui->FocusElement(m_Field);
        m_Field->SelectAll();
    }
}

void AssetRenameController::ApplyValidation(const std::string& stem)
{
    if (!m_Field)
        return;
    const Editor::AssetRenameValidation validation = Editor::ValidateAssetRenameStem(stem, m_ItemPath);
    if (validation.Ok)
    {
        m_Field->RemoveClass(kInvalidClass);
        m_Field->SetTooltip({});
        return;
    }
    m_Field->AddClass(kInvalidClass);
    m_Field->SetTooltip(validation.Reason);
}

void AssetRenameController::Commit()
{
    if (!m_Field)
        return;

    const std::string newStem = m_Field->GetValue();
    if (newStem == m_ItemPath.stem().string())
    {
        End();
        return;
    }

    const Editor::AssetRenameValidation validation = Editor::ValidateAssetRenameStem(newStem, m_ItemPath);
    if (!validation.Ok)
    {
        // Keep editing: the field stays marked and its tooltip carries the reason.
        ApplyValidation(newStem);
        Logger::Log::Warning("Assets: rename of '{}' rejected: {}", m_ItemPath.filename().string(),
                             validation.Reason);
        return;
    }

    auto command = std::make_unique<Editor::RenameAssetFileCommand>(
        Editor::PlanAssetRename(m_ItemPath, newStem), m_Assets, m_OnRenamed);
    Editor::UndoRedoService* undo = m_Undo;
    End();

    if (undo)
        undo->Execute(std::move(command));
    else
        command->Do();
}

void AssetRenameController::End()
{
    if (!m_Field)
        return;

    TextField* field = m_Field;
    Label* title = m_Title;
    UIElement* focusOnEnd = m_FocusOnEnd;
    m_Field = nullptr;
    m_Title = nullptr;
    m_FocusOnEnd = nullptr;
    m_ItemPath.clear();
    m_Undo = nullptr;
    m_Assets = nullptr;
    m_OnRenamed = nullptr;

    UI::Layout::SetElementHidden(*title, false);
    // Detach after the current event finishes: End() runs from the field's own
    // key and focus handlers, and destroying the element mid-dispatch would pull
    // the handler out from under itself.
    UIElement* host = field->GetParent();
    if (host)
        host->PostAction([host, field]() { host->RemoveChild(field); });
    if (focusOnEnd)
    {
        if (UIManager* ui = focusOnEnd->GetOwnerManager())
            ui->FocusElement(focusOnEnd);
    }
}

} // namespace GameEngine
