#include "Platform/ContextMenu.h"
#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <filesystem>

using namespace GameEngine;

namespace {

class FakeContextMenu final : public INativeContextMenu {
public:
    struct SubMenuRecord {
        uint32_t    parentId;
        std::string title;
        uint32_t    id;
    };

    struct ItemRecord {
        uint32_t    parentId;
        std::string title;
        uint32_t    commandId;
        uint32_t    flags;
    };

    struct SeparatorRecord {
        uint32_t parentId;
    };

    void Clear() override
    {
        m_SubMenus.clear();
        m_Items.clear();
        m_Separators.clear();
        m_NextId = 1;
    }

    uint32_t AddSubMenu(uint32_t parentId, const std::string& title) override
    {
        const uint32_t id = m_NextId++;
        m_SubMenus.push_back({ parentId, title, id });
        return id;
    }

    void AddItem(uint32_t parentId, const std::string& title, uint32_t commandId, uint32_t ItemFlags) override
    {
        m_Items.push_back({ parentId, title, commandId, ItemFlags });
    }

    void AddSeparator(uint32_t parentId) override
    {
        m_Separators.push_back({ parentId });
    }

    void SetCommandHandler(CommandCallback) override {}
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t, bool) override {}
    void SetItemChecked(uint32_t, bool) override {}
    void SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath) override
    {
        m_SubMenuIcons.emplace_back(submenuId, imagePath);
    }
    void Show(Platform::Window*, int, int) override {}

    std::vector<SubMenuRecord>    m_SubMenus;
    std::vector<ItemRecord>       m_Items;
    std::vector<SeparatorRecord>  m_Separators;
    std::vector<std::pair<uint32_t, std::string>> m_SubMenuIcons;

private:
    uint32_t m_NextId = 1;
};

} // namespace


TEST(ContextMenuBuilderTests, BuildsHierarchicalMenuFromPaths)
{
    FakeContextMenu menu;

    std::vector<ContextMenuItemDesc> items;
    items.push_back({ "File/Open", 1u, MenuItemFlag_None, 10 });
    items.push_back({ "File/Save", 2u, MenuItemFlag_None, 5 });
    items.push_back({ "Edit/Undo", 3u, MenuItemFlag_Disabled, 0 });

    BuildContextMenuFromPaths(&menu, items, /*SortByPriorityThenPath=*/true);

    ASSERT_EQ(menu.m_SubMenus.size(), 2u);
    EXPECT_EQ(menu.m_SubMenus[0].title, "Edit");
    EXPECT_EQ(menu.m_SubMenus[0].parentId, 0u);
    EXPECT_EQ(menu.m_SubMenus[1].title, "File");
    EXPECT_EQ(menu.m_SubMenus[1].parentId, 0u);

    const uint32_t editId = menu.m_SubMenus[0].id;
    const uint32_t fileId = menu.m_SubMenus[1].id;

    ASSERT_EQ(menu.m_Items.size(), 3u);

    EXPECT_EQ(menu.m_Items[0].title, "Undo");
    EXPECT_EQ(menu.m_Items[0].parentId, editId);
    EXPECT_EQ(menu.m_Items[0].commandId, 3u);
    EXPECT_EQ(menu.m_Items[0].flags, MenuItemFlag_Disabled);

    EXPECT_EQ(menu.m_Items[1].title, "Save");
    EXPECT_EQ(menu.m_Items[1].parentId, fileId);
    EXPECT_EQ(menu.m_Items[1].commandId, 2u);

    EXPECT_EQ(menu.m_Items[2].title, "Open");
    EXPECT_EQ(menu.m_Items[2].parentId, fileId);
    EXPECT_EQ(menu.m_Items[2].commandId, 1u);
}


TEST(ContextMenuBuilderTests, ReusesSubmenusForSharedPrefixes)
{
    FakeContextMenu menu;

    std::vector<ContextMenuItemDesc> items;
    items.push_back({ "File/Open", 1u, MenuItemFlag_None, 0 });
    items.push_back({ "File/Save", 2u, MenuItemFlag_None, 0 });
    items.push_back({ "File/Recent/Project1", 3u, MenuItemFlag_None, 0 });
    items.push_back({ "File/Recent/Project2", 4u, MenuItemFlag_None, 0 });

    BuildContextMenuFromPaths(&menu, items, /*SortByPriorityThenPath=*/false);

    // Expect one "File" submenu and one "Recent" submenu beneath it.
    ASSERT_EQ(menu.m_SubMenus.size(), 2u);
    EXPECT_EQ(menu.m_SubMenus[0].title, "File");
    const uint32_t fileId = menu.m_SubMenus[0].id;
    EXPECT_EQ(menu.m_SubMenus[0].parentId, 0u);

    EXPECT_EQ(menu.m_SubMenus[1].title, "Recent");
    EXPECT_EQ(menu.m_SubMenus[1].parentId, fileId);

    // All items should either be under File or Recent.
    for (const auto& item : menu.m_Items) {
        EXPECT_TRUE(item.parentId == fileId || item.parentId == menu.m_SubMenus[1].id);
    }
}


TEST(ContextMenuBuilderTests, CreatesSubmenusForCommandLessLeaf)
{
    FakeContextMenu menu;

    std::vector<ContextMenuItemDesc> items;
    items.push_back({ "View/Advanced/Options", 0u, MenuItemFlag_None, 0 });

    BuildContextMenuFromPaths(&menu, items, /*SortByPriorityThenPath=*/false);

    // Expect a chain of three submenus and no items.
    ASSERT_EQ(menu.m_SubMenus.size(), 3u);
    ASSERT_TRUE(menu.m_Items.empty());

    const auto& view    = menu.m_SubMenus[0];
    const auto& advanced = menu.m_SubMenus[1];
    const auto& options  = menu.m_SubMenus[2];

    EXPECT_EQ(view.parentId, 0u);
    EXPECT_EQ(view.title, "View");

    EXPECT_EQ(advanced.parentId, view.id);
    EXPECT_EQ(advanced.title, "Advanced");

    EXPECT_EQ(options.parentId, advanced.id);
    EXPECT_EQ(options.title, "Options");
}

TEST(ContextMenuBuilderTests, AppliesIconToCommandlessSubmenu)
{
    FakeContextMenu menu;

    BuildContextMenuFromPaths(&menu,
                              {{ "Create/Terrain", 0u, MenuItemFlag_None, 0, "editor:Icons/terrain.png" },
                               { "Create/Terrain/Planet", 1u, MenuItemFlag_None, 0 }},
                              /*SortByPriorityThenPath=*/true);

    ASSERT_EQ(menu.m_SubMenuIcons.size(), 1u);
    ASSERT_EQ(menu.m_SubMenus.size(), 2u);
    EXPECT_EQ(menu.m_SubMenuIcons[0].first, menu.m_SubMenus[1].id);
    EXPECT_EQ(menu.m_SubMenuIcons[0].second, "editor:Icons/terrain.png");
}


TEST(PlatformShellTests, OpenPath_ReturnsFalseForEmptyAndNonExisting)
{
    using namespace GameEngine::Platform;

    EXPECT_FALSE(OpenPath(std::filesystem::path()));

    const std::filesystem::path bogus = std::filesystem::path("this/path/should/not/exist/hopefully_123456789");
    EXPECT_FALSE(OpenPath(bogus));
}


TEST(PlatformShellTests, ShowInFileManager_ReturnsFalseForEmptyAndNonExisting)
{
    using namespace GameEngine::Platform;

    EXPECT_FALSE(ShowInFileManager(std::filesystem::path()));

    const std::filesystem::path bogus = std::filesystem::path("this/path/should/not/exist/hopefully_987654321");
    EXPECT_FALSE(ShowInFileManager(bogus));
}
