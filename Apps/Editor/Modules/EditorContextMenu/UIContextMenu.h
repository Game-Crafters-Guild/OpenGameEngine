#pragma once

#include "Platform/ContextMenu.h"
#include "EditorContextMenu/ContextMenuBackendPolicy.h"
#include "EditorContextMenu/InterceptableContextMenu.h"
#include "UI/UIManagerRef.h"
#include <string>
#include <vector>
#include <memory>

namespace GameEngine
{
    class UIManager;
    class UIElement;
    class SearchFieldWithFilter;
    namespace Platform { class Window; }

    class UIContextMenu : public INativeContextMenu
    {
    public:
        // Register/unregister a window<->UIManager association so that UIContextMenu
        // can find the correct UIManager at Show() time without needing it at
        // construction. Call Register once after both are initialised; call
        // Unregister before the window is destroyed.
        static void Register(Platform::Window* window, UIManager* uiManager);
        static void Unregister(Platform::Window* window);

        UIContextMenu();
        ~UIContextMenu() override;

        void Clear() override;

        uint32_t AddSubMenu(uint32_t parentId, const std::string& title) override;
        void AddItem(uint32_t parentId, const std::string& title,
                     uint32_t commandId, uint32_t itemFlags = MenuItemFlag_None) override;
        void AddSeparator(uint32_t parentId) override;
        bool AddSearchField(uint32_t parentId, const std::string& placeholder,
                            const std::string& initialText, SearchCallback onSearch) override;

        void SetCommandHandler(CommandCallback cb) override;
        void SetStateProvider(StateProviderCallback cb) override;

        void SetItemEnabled(uint32_t commandId, bool enabled) override;
        void SetItemChecked(uint32_t commandId, bool checked) override;
        void SetItemIcon(uint32_t commandId, const std::string& imagePath) override;
        void SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath) override;

        void SetItemColor(uint32_t commandId, const std::string& hexColor) override;

        void Show(Platform::Window* window, int x, int y) override;

    private:
        static constexpr uint32_t kFirstSubMenuId = 100000;

        struct MenuItem
        {
            std::string title;
            uint32_t id = 0;
            bool isSubMenu = false;
            bool isSeparator = false;
            uint32_t flags = MenuItemFlag_None;
            bool staticEnabled = true;
            bool staticChecked = false;
            std::string iconPath;
            std::string colorHex; // "#RRGGBB": trailing swatch (tag dots, current color of a picker row)
            std::vector<std::unique_ptr<MenuItem>> children;
        };

        // A leaf command surfaced by an active filter, with the submenu path it
        // was found under (empty for a root-level command).
        struct LeafMatch
        {
            const MenuItem* item = nullptr;
            std::string path;
        };

        void CloseAll();
        void SetCloseHandler(std::function<void()> cb) override { m_CloseHandler = std::move(cb); }
        void OpenSubMenu(uint32_t parentId, UIElement* anchorElement, int level);
        void BuildMenuLevel(UIElement* container, const std::vector<std::unique_ptr<MenuItem>>& items, int level);
        void AppendItemRow(UIElement* container, const MenuItem& item, int level, const std::string& path,
                           bool inSelectionGroup = false);
        // In-place mark/dim update for a sticky selection click (the menu
        // stays open); radio groups move the check, toggle groups flip the
        // clicked row.
        void UpdateSelectionVisuals(UIElement* clickedRow, bool radio);

        void AddSearchRow(UIElement* container);
        // Fills the root panel with either the item tree (empty query) or the
        // flat list of commands the query matches.
        void BuildRootRows(const std::string& query);
        void BuildFilteredRows(UIElement* container, const std::string& query);
        void CollectLeafMatches(const std::vector<std::unique_ptr<MenuItem>>& items,
                                const std::string& lowerQuery, const std::string& pathPrefix,
                                std::vector<LeafMatch>& matches) const;
        void ApplySearchQuery(const std::string& query);

        std::vector<std::unique_ptr<MenuItem>>* GetListForParent(uint32_t parentId);
        MenuItem* FindItem(uint32_t id, std::vector<std::unique_ptr<MenuItem>>& list);

        UIManager* m_UIManager = nullptr; // resolved at Show() time via the registry
        // Tells CloseAll whether m_UIManager is still alive: the menu can outlive it.
        UIManagerRef m_UIManagerLifetime;
        CommandCallback m_CommandHandler;
        StateProviderCallback m_StateProvider;

        std::vector<std::unique_ptr<MenuItem>> m_RootItems;
        uint32_t m_NextSubMenuId = kFirstSubMenuId;

        bool m_SearchEnabled = false;
        std::string m_SearchPlaceholder;
        std::string m_SearchInitialText;
        SearchCallback m_SearchCallback;
        SearchFieldWithFilter* m_SearchField = nullptr;
        UIElement* m_RootPanel = nullptr;
        UIElement* m_RowsHost = nullptr;
        // Rebuilding the filtered rows is deferred past the keystroke that asked
        // for it, so the deferred work needs to know whether this menu outlived
        // the wait.
        std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);

        // Latest submenu-open request wins; stale deferred requests no-op.
        uint64_t m_SubMenuRequestSerial = 0;

        UIElement* m_OverlayRoot = nullptr;
        std::vector<UIElement*> m_OpenMenus;
        // Submenu id each open panel was built from (0 for the root panel);
        // parallel to m_OpenMenus. Lets a re-hovered parent row keep its
        // already-open submenu instead of rebuilding it.
        std::vector<uint32_t> m_OpenMenuParentIds;
        // Anchor row of each open panel (nullptr for the root panel). Carries
        // the submenu-open class so a parent stays highlighted while the
        // cursor is inside its submenu, the way a native menu keeps it lit.
        std::vector<UIElement*> m_OpenMenuAnchors;
        std::string m_SavedFocusId;
        std::function<void()> m_CloseHandler;
    };

    // Creates a context menu with the backend the settings choose (see
    // ContextMenuBackendPolicy.h): the editor-drawn UIContextMenu, or the
    // native OS menu where one exists. Prefer this over
    // CreateNativeContextMenu() so the backend stays selectable — and so every
    // editor menu rides the InterceptableContextMenu decorator, which lets the
    // debug server drive menus without the native popup's nested message loop.
    inline std::unique_ptr<INativeContextMenu> CreateContextMenu()
    {
        return std::make_unique<InterceptableContextMenu>(CreateSelectedContextMenuBackend());
    }


} // namespace GameEngine
