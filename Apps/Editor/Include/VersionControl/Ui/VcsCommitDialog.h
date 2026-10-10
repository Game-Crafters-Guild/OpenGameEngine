#pragma once

#include <functional>
#include <string>

namespace GameEngine {
class UIElement;
class UIManager;
}

namespace GameEngine::Editor {

// Provider-agnostic commit dialog: collects a commit message and hands it to
// the caller, which routes it to the active IVCSIntegration.
class VcsCommitDialog
{
public:
    VcsCommitDialog();
    ~VcsCommitDialog();

    // Show the dialog and return the commit message (empty if cancelled)
    // onCommit: callback with commit message
    // onCancel: callback when cancelled
    void Show(GameEngine::UIManager* uiManager,
              const std::string& defaultMessage,
              std::function<void(const std::string&)> onCommit,
              std::function<void()> onCancel);

    void Hide();

    bool IsVisible() const { return m_IsVisible; }

private:
    bool m_IsVisible = false;
    // Non-owning: the UI root owns the dialog element once added. Holding a
    // unique_ptr here went null at AddChild(std::move(...)) time, so Hide
    // could never remove the dialog from the tree.
    GameEngine::UIElement* m_DialogRoot = nullptr;
    GameEngine::UIManager* m_UIManager = nullptr;
    std::function<void(const std::string&)> m_OnCommit;
    std::function<void()> m_OnCancel;
};

} // namespace GameEngine::Editor
