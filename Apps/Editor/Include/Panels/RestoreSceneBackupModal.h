#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{

// Crash-recovery prompt shown when opening a scene that has autosave staging
// left over from an unclean shutdown. Two explicit, honestly-labeled buttons:
// Restore (promote the autosaved work) / Discard (delete it and open the last
// saved state). No Escape shortcut — Discard is destructive, so dismissing the
// prompt must be a deliberate click.
class RestoreSceneBackupModal final : public UIElement
{
  public:
    RestoreSceneBackupModal();

    void Show(const std::string& title, const std::string& message);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    void SetOnRestore(std::function<void()> cb) { m_OnRestore = std::move(cb); }
    void SetOnDiscard(std::function<void()> cb) { m_OnDiscard = std::move(cb); }

    // Programmatic equivalents of the buttons (debug-server respond_modal).
    void ChooseRestore() { OnRestoreClicked(); }
    void ChooseDiscard() { OnDiscardClicked(); }

  private:
    void OnRestoreClicked();
    void OnDiscardClicked();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_Window = nullptr;
    UIElement* m_Title = nullptr;
    UIElement* m_Message = nullptr;

    bool m_Visible = false;

    std::function<void()> m_OnRestore;
    std::function<void()> m_OnDiscard;
};

} // namespace GameEngine
