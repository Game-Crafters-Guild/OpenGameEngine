#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine {

namespace Platform { class Window; }

enum MenuItemFlags : uint32_t {
    MenuItemFlag_None     = 0,
    MenuItemFlag_Disabled = 1u << 0,
    MenuItemFlag_Checked  = 1u << 1,
    // One of a set where exactly one choice holds at a time (a sort order, a
    // view mode). Declared, never inferred: a group where one item happens to
    // be checked is indistinguishable from independent toggles that happen to
    // have one on, and guessing turns the second checkbox someone adds into a
    // radio alternative of the first.
    MenuItemFlag_Radio    = 1u << 2,
};

struct MenuItemState {
    bool Enabled = true;
    bool Checked = false;
};

class INativeContextMenu {
public:
    using CommandCallback       = std::function<void(uint32_t)>;
    using StateProviderCallback = std::function<MenuItemState(uint32_t)>;
    using SearchCallback        = std::function<void(const std::string&)>;

    virtual ~INativeContextMenu() = default;

    virtual void Clear() = 0;

    // Menu building API (UTF-8 titles)
    // Root menu is represented by parentId == 0.
    virtual uint32_t AddSubMenu(uint32_t parentId, const std::string& title) = 0;
    virtual void     AddItem(uint32_t parentId, const std::string& title,
                             uint32_t commandId,
                             uint32_t ItemFlags = MenuItemFlag_None) = 0;
    virtual void     AddSeparator(uint32_t parentId) = 0;
    /// Optional inline native search field. Returns false when the platform menu
    /// implementation cannot host editable controls inside a context menu.
    virtual bool     AddSearchField(uint32_t parentId,
                                    const std::string& placeholder,
                                    const std::string& initialText,
                                    SearchCallback onSearch)
    {
        (void)parentId;
        (void)placeholder;
        (void)initialText;
        (void)onSearch;
        return false;
    }

    // Callbacks and state providers
    virtual void SetCommandHandler(CommandCallback cb) = 0;
    /** Called once after the menu closes, chosen item or not. Default no-op so
        back-ends that cannot report it stay valid. */
    virtual void SetCloseHandler(std::function<void()> cb) { (void)cb; }
    virtual void SetStateProvider(StateProviderCallback cb) = 0;

    // Static per-item state; dynamic state is applied via StateProviderCallback at show-time.
    virtual void SetItemEnabled(uint32_t commandId, bool enabled) = 0;
    virtual void SetItemChecked(uint32_t commandId, bool checked) = 0;

    /// Optional: set a small colored dot/icon for a menu item (e.g. for tag colors). hexColor e.g. "#3498db" or "#rgb".
    virtual void SetItemColor(uint32_t commandId, const std::string& hexColor) { (void)commandId; (void)hexColor; }

    /// Optional: set a small image/icon for a menu item. UI menus resolve this
    /// like CSS background-image paths, so aliases such as "editor:Icons/foo.png"
    /// are valid. Native menus may ignore this when not supported.
    virtual void SetItemIcon(uint32_t commandId, const std::string& imagePath) { (void)commandId; (void)imagePath; }

    /// Optional: set a small image/icon for a submenu row. Native menus may
    /// ignore this when not supported.
    virtual void SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath) { (void)submenuId; (void)imagePath; }

    // Show menu at (x, y) relative to the client area of `window`.
    virtual void Show(Platform::Window* window, int x, int y) = 0;
};

struct ContextMenuItemDesc {
    std::string Path;      // e.g. "File/New/Scene"
    uint32_t    CommandId; // 0 => non-clickable node (submenu-only descriptor)
    uint32_t    Flags    = MenuItemFlag_None;
    int         Priority = 0;
    std::string IconPath;
};

// Build a menu from path-based descriptors.
// Optionally sorts by (priority, path) before constructing the tree.
void BuildContextMenuFromPaths(INativeContextMenu* menu,
                               const std::vector<ContextMenuItemDesc>& items,
                               bool SortByPriorityThenPath = true);

class ContextMenuBuilder {
public:
    ContextMenuBuilder& AddItem(const std::string& path,
                                uint32_t commandId,
                                uint32_t Flags    = MenuItemFlag_None,
                                int      Priority = 0,
                                const std::string& IconPath = {})
    {
        m_Items.push_back({ path, commandId, Flags, Priority, IconPath });
        return *this;
    }

    void Build(INativeContextMenu* menu,
               bool SortByPriorityThenPath = true) const
    {
        BuildContextMenuFromPaths(menu, m_Items, SortByPriorityThenPath);
    }

private:
    std::vector<ContextMenuItemDesc> m_Items;
};

// Factory: returns a platform-specific implementation (Win32, Cocoa, stub, ...)
std::unique_ptr<INativeContextMenu> CreateNativeContextMenu();

} // namespace GameEngine
