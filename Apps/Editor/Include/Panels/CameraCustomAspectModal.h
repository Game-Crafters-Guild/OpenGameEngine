#pragma once

#include "UI/UIElement.h"

#include <functional>

namespace GameEngine
{

class FloatField;
class Label;
class Button;

class CameraCustomAspectModal final : public UIElement
{
  public:
    CameraCustomAspectModal();

    void Show(float initialWidth, float initialHeight);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    void SetOnCommit(std::function<void(float width, float height)> cb) { m_OnCommit = std::move(cb); }
    void SetOnCancel(std::function<void()> cb) { m_OnCancel = std::move(cb); }

  private:
    void OnCommitClicked();
    void OnCancelClicked();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_Window = nullptr;
    FloatField* m_WidthField = nullptr;
    FloatField* m_HeightField = nullptr;

    bool m_Visible = false;

    std::function<void(float width, float height)> m_OnCommit;
    std::function<void()> m_OnCancel;
};

} // namespace GameEngine
