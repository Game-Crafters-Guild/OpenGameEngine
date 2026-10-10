#pragma once

#include "UI/UIElement.h"

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{

class TextField;
class Label;
class Button;

// Minimal “Save As” modal for scenes. Browse opens Platform::SelectFolder where an OS
// dialog can name a project location, and an in-project folder chooser where the project
// tree is sandboxed from the OS (Platform::ProjectStorageIsSandboxed).
class SaveSceneAsModal final : public UIElement
{
  public:
    SaveSceneAsModal();

    void Show(const std::filesystem::path& initialDirectory);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    void SetOnSave(std::function<void(const std::filesystem::path&)> cb) { m_OnSave = std::move(cb); }
    void SetOnCancel(std::function<void()> cb) { m_OnCancel = std::move(cb); }

    // Programmatic equivalent of the Cancel button (debug-server respond_modal).
    // Save needs the name field + overwrite flow, so only cancel is exposed.
    void ChooseCancel() { OnCancelClicked(); }

  private:
    void OnBrowseClicked();
    void OnSaveClicked();
    void OnCancelClicked();
    void CommitSave(const std::filesystem::path& outPath);
    void ClearOverwritePrompt();
    // Fill the folder list with the current directory's immediate subfolders (plus
    // an up entry, bounded at the project root). The in-project chooser, used where
    // an OS folder dialog cannot name a location inside the project.
    void PopulateFolderList();

    std::filesystem::path m_SelectedDir;
    std::filesystem::path m_ProjectRoot; // The in-project chooser is confined to this subtree.
    UIElement* m_FolderList = nullptr;    // In-project folder chooser rows; empty when hidden.
    TextField* m_NameField = nullptr;
    Label* m_DirLabel = nullptr;
    Label* m_OverwriteWarningLabel = nullptr;
    Button* m_SaveButton = nullptr;

    bool m_PendingOverwrite = false;
    std::filesystem::path m_PendingOverwritePath;

    bool m_Visible = false;

    std::function<void(const std::filesystem::path&)> m_OnSave;
    std::function<void()> m_OnCancel;
};

} // namespace GameEngine

