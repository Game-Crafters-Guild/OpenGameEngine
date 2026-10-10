#pragma once

#include "Platform/ContextMenu.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{

// Decorator around any INativeContextMenu that shadow-records the menu model
// as it is built. Interactive use is pure pass-through. When the debug-server
// interceptor is armed, Show() publishes the state-resolved item tree plus a
// COPY of the command callback instead of opening the popup — native popups
// (Win32 TrackPopupMenuEx / NSMenu) run a nested OS message loop that freezes
// the main loop and the debug server, so automation drives menus through this
// capture (open_context_menu / invoke_menu_item) rather than the OS.
//
// Editor-owned on purpose: the engine Platform module stays untouched, and
// because the recording happens above the platform backend it works
// identically for Win32, macOS, and the editor-drawn UIContextMenu.
// CreateContextMenu() wraps every editor menu in this decorator.
class InterceptableContextMenu final : public INativeContextMenu
{
  public:
    struct CapturedItem
    {
        std::string Path;       // "Sub/Item"; separators carry the parent path
        uint32_t CommandId = 0; // 0 for submenus and separators
        bool Enabled = true;
        bool Checked = false;
        bool IsSubMenu = false;
        bool IsSeparator = false;
        std::string Icon;  // resolved image path, empty when the row carries none
        std::string Color; // "#RRGGBB" from SetItemColor, empty when the row carries none
    };

    struct Capture
    {
        // Build order preserved; nesting is encoded in Path.
        std::vector<CapturedItem> Items;
        // Copy of the menu's command handler — invocable after Show()
        // returned, independent of the menu object's lifetime. The captured
        // callables keep whatever the menu owner captured (typically
        // long-lived panels).
        std::function<void(uint32_t)> Invoke;
        int X = 0; // logical client coords the caller passed to Show()
        int Y = 0;
    };

    // Debug hook: return true to consume the show (no popup opens). Inert
    // when unset or when it returns false. Menus only open from main-thread
    // UI callbacks, so no synchronization is needed.
    using Interceptor = std::function<bool(Capture&&)>;
    static void SetInterceptor(Interceptor interceptor);

    explicit InterceptableContextMenu(std::unique_ptr<INativeContextMenu> inner);

    void Clear() override;
    uint32_t AddSubMenu(uint32_t parentId, const std::string& title) override;
    void AddItem(uint32_t parentId, const std::string& title, uint32_t commandId,
                 uint32_t itemFlags) override;
    void AddSeparator(uint32_t parentId) override;
    bool AddSearchField(uint32_t parentId, const std::string& placeholder,
                        const std::string& initialText, SearchCallback onSearch) override;
    void SetCommandHandler(CommandCallback cb) override;
    void SetCloseHandler(std::function<void()> cb) override;
    void SetStateProvider(StateProviderCallback cb) override;
    void SetItemEnabled(uint32_t commandId, bool enabled) override;
    void SetItemChecked(uint32_t commandId, bool checked) override;
    void SetItemColor(uint32_t commandId, const std::string& hexColor) override;
    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override;
    void SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath) override;
    void Show(Platform::Window* window, int x, int y) override;

  private:
    struct ShadowItem
    {
        uint32_t ParentId = 0;  // 0 = root, else a SubMenuId
        std::string Title;
        uint32_t CommandId = 0;
        uint32_t SubMenuId = 0; // non-zero marks a submenu row
        bool IsSeparator = false;
        bool StaticEnabled = true;
        bool StaticChecked = false;
        std::string IconPath;
        std::string ColorHex;
    };

    std::string PathFor(const ShadowItem& item) const;

    // Shared, not unique: Show() parks a reference for as long as the menu is
    // on screen, so this object may be destroyed while the backend it
    // configured is still showing.
    std::shared_ptr<INativeContextMenu> m_Inner;
    unsigned m_BackendGeneration = 0;
    std::vector<ShadowItem> m_Items;
    CommandCallback m_CommandHandler;
    StateProviderCallback m_StateProvider;
    std::function<void()> m_CloseHandler;
};

/**
 * @brief Releases the showing menu and any awaiting destruction.
 *
 * Must run while the UIManager and native window are still alive: a drawn
 * menu's destructor removes its overlay through the UIManager, and a native
 * one tears down an OS menu. Call before any ui.reset() on shutdown.
 *
 * If a Show() is on the stack, destruction of that in-flight menu waits until
 * it returns; menus displaced after their Show() returned wait for the next
 * DrainShowingContextMenus().
 */
void ReleaseShowingContextMenu();

/**
 * @brief Releases the showing menu only if it was shown on `window`.
 *
 * Closing a floating window must not destroy a menu open on another window.
 * No-op when nothing is showing or it belongs to a different window.
 */
void ReleaseShowingContextMenuForWindow(Platform::Window* window);

/**
 * @brief Destroys menus displaced after their Show() returned.
 *
 * Safe to call every frame; the editor runs it at the start of Update, after
 * PollEvents has finished the handler that displaced them. Does not touch the
 * menu that is currently showing. No-op while a Show() is on the stack.
 */
void DrainShowingContextMenus();

} // namespace GameEngine
