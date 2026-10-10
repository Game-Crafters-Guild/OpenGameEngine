#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{

// Wording for the three buttons. The ACTIONS are fixed — confirm, alternative, cancel — and only
// their labels vary, so a prompt like "Save Anyway / Save As... / Cancel" reuses this modal rather
// than growing a near-identical second one. Omitted labels keep the unsaved-changes defaults.
struct SaveSceneChangesModalLabels
{
    std::string Save = "Save";
    std::string DontSave = "Don't Save";
    std::string Cancel = "Cancel";
};

// Simple modal prompt: Save / Don't Save / Cancel.
class SaveSceneChangesModal final : public UIElement
{
  public:
    SaveSceneChangesModal();

    void Show(const std::string& title, const std::string& message,
              const SaveSceneChangesModalLabels& labels = {});
    void Hide();
    bool IsVisible() const { return m_Visible; }

    void SetOnSave(std::function<void()> cb) { m_OnSave = std::move(cb); }
    void SetOnDontSave(std::function<void()> cb) { m_OnDontSave = std::move(cb); }
    void SetOnCancel(std::function<void()> cb) { m_OnCancel = std::move(cb); }

    // Programmatic equivalents of the buttons (debug-server respond_modal).
    void ChooseSave() { OnSaveClicked(); }
    void ChooseDontSave() { OnDontSaveClicked(); }
    void ChooseCancel() { OnCancelClicked(); }

  private:
    void OnSaveClicked();
    void OnDontSaveClicked();
    void OnCancelClicked();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_Window = nullptr;
    UIElement* m_Title = nullptr;
    UIElement* m_Message = nullptr;
    UIElement* m_SaveButton = nullptr;
    UIElement* m_DontSaveButton = nullptr;
    UIElement* m_CancelButton = nullptr;

    bool m_Visible = false;

    std::function<void()> m_OnSave;
    std::function<void()> m_OnDontSave;
    std::function<void()> m_OnCancel;
};

} // namespace GameEngine

