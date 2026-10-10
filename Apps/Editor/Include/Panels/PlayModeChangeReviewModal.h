#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

// Modal dialog shown after exiting Play Mode to review and optionally apply
// user-intentional editor changes (captured via UndoRedoService during play).
class PlayModeChangeReviewModal final : public UIElement
{
  public:
    PlayModeChangeReviewModal();

    void Show(const std::vector<std::string>& changeNames);
    void Hide();

    void SetOnApply(std::function<void(const std::vector<bool>& keep)> cb) { m_OnApply = std::move(cb); }
    void SetOnDiscard(std::function<void()> cb) { m_OnDiscard = std::move(cb); }

  private:
    void RebuildList(const std::vector<std::string>& changeNames);
    void OnApplyClicked();
    void OnDiscardClicked();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_Window = nullptr;
    UIElement* m_ListRoot = nullptr;

    std::vector<class Toggle*> m_Toggles;

    std::function<void(const std::vector<bool>& keep)> m_OnApply;
    std::function<void()> m_OnDiscard;
};

} // namespace GameEngine

