#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{

class TextField;
class Label;
class Button;

// Minimal single-field modal used to rename a layout preset.
class RenameLayoutModal final : public UIElement
{
  public:
    RenameLayoutModal();

    void Show(const std::string& title, const std::string& initialValue);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    void SetOnCommit(std::function<void(const std::string&)> cb) { m_OnCommit = std::move(cb); }
    void SetOnCancel(std::function<void()> cb) { m_OnCancel = std::move(cb); }

  private:
    void OnCommitClicked();
    void OnCancelClicked();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_Window = nullptr;
    Label* m_Title = nullptr;
    TextField* m_NameField = nullptr;

    bool m_Visible = false;

    std::function<void(const std::string&)> m_OnCommit;
    std::function<void()> m_OnCancel;
};

} // namespace GameEngine
