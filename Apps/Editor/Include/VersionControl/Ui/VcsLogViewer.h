#pragma once

#include <filesystem>
#include <string>

namespace GameEngine {
class IVCSIntegration;
class UIElement;
class UIManager;
}

namespace GameEngine::Editor {

// Revision-log viewer panel. Provider-agnostic: the caller hands it the
// active IVCSIntegration and a display name; entries come from GetLog.
class VcsLogViewer
{
public:
    VcsLogViewer();
    ~VcsLogViewer();

    void Show(GameEngine::UIManager* uiManager, GameEngine::IVCSIntegration* vcs,
              const std::string& vcsDisplayName, const std::filesystem::path& filePath = {});
    void Hide();
    void Refresh();

    bool IsVisible() const { return m_IsVisible; }

private:
    void BuildUI(GameEngine::UIManager* uiManager);
    void UpdateLogEntries();

    bool m_IsVisible = false;
    std::filesystem::path m_FilePath;
    std::string m_VcsDisplayName;
    // Non-owning: the UI root owns the panel element once added. Holding a
    // unique_ptr here went null at AddChild(std::move(...)) time, which made
    // UpdateLogEntries/Hide silent no-ops (empty list, unclosable panel).
    GameEngine::UIElement* m_PanelRoot = nullptr;
    GameEngine::UIManager* m_UIManager = nullptr;
    GameEngine::IVCSIntegration* m_Vcs = nullptr;
};

} // namespace GameEngine::Editor
