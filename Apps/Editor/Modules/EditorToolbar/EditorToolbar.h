#pragma once

#include "Platform/Toolbar.h" // INativeToolbar
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace GameEngine
{

class EditorApplication; // fwd
class EditorPanelManager; // fwd
namespace Platform
{
class Window;
}                     // namespace Platform
class INativeToolbar; // fwd (Platform/Toolbar)

// EditorToolbar (Editor-level): manages menu structure and actions; uses platform-native toolbar under the hood.
class EditorToolbar
{
  public:
    EditorToolbar() = default;
    ~EditorToolbar();

    // Install the Editor toolbar on the given Platform::Window. Returns true on success.
    bool Install(Platform::Window* window, EditorApplication* app);
    void Uninstall();

    // Rebuild the menu structure (used to refresh Undo/Redo labels).
    void Refresh();
    void SetPanelManager(EditorPanelManager* panelManager) { m_PanelManager = panelManager; }

    // Efficiently refresh just the Undo/Redo menu item titles without
    // tearing down and rebuilding the entire toolbar.
    void UpdateUndoRedoTitles();

    // Editor-level wrappers to add menus/items without exposing platform types
    void Clear();
    uint32_t AddMenu(const std::string& title);
    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title);
    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId);
    void SetItemIcon(uint32_t commandId, const std::string& imagePath);

  private:
    void Build(EditorApplication* app);

    std::unique_ptr<INativeToolbar> m_Toolbar; // platform-native toolbar instance
    EditorApplication* m_App = nullptr;        // non-owning, cleared on Uninstall
    EditorPanelManager* m_PanelManager = nullptr; // non-owning, owned by EditorApplication
    std::unordered_map<uint32_t, std::string> m_WindowPanelCommands;
};

} // namespace GameEngine
