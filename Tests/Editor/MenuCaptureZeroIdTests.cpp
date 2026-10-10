// Command id 0 addresses no row: separators and submenu rows carry it, a caller may
// register a real label row with it (the version control panel's disabled
// "Locked by <user>" entry), and it is what AddSubMenu returns when it fails —
// ContextMenuBuilder feeds that straight back into SetSubMenuIcon. Stamping id 0
// would therefore smear one icon, colour or state across unrelated rows, so the
// decorator refuses it before the backend sees it. Pinned on both sides: refused for
// zero, still delivered for every real id.

#include <gtest/gtest.h>

#include "EditorContextMenu/InterceptableContextMenu.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

// Mints real submenu ids (the noop backend returns 0, which the decorator declines to
// record) and records every setter that reaches it.
class RecordingBackendMenu final : public INativeContextMenu
{
  public:
    struct Call
    {
        std::string Setter;
        uint32_t Id = 0;
    };

    void Clear() override {}
    uint32_t AddSubMenu(uint32_t, const std::string&) override { return m_NextSubMenuId++; }
    void AddItem(uint32_t, const std::string&, uint32_t, uint32_t) override {}
    void AddSeparator(uint32_t) override {}
    void SetCommandHandler(CommandCallback) override {}
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t id, bool) override { Calls.push_back({"SetItemEnabled", id}); }
    void SetItemChecked(uint32_t id, bool) override { Calls.push_back({"SetItemChecked", id}); }
    void SetItemColor(uint32_t id, const std::string&) override
    {
        Calls.push_back({"SetItemColor", id});
    }
    void SetItemIcon(uint32_t id, const std::string&) override
    {
        Calls.push_back({"SetItemIcon", id});
    }
    void SetSubMenuIcon(uint32_t id, const std::string&) override
    {
        Calls.push_back({"SetSubMenuIcon", id});
    }
    void Show(Platform::Window*, int, int) override {}

    std::vector<Call> Calls;

  private:
    uint32_t m_NextSubMenuId = 1000;
};

class InterceptorGuard
{
  public:
    explicit InterceptorGuard(InterceptableContextMenu::Interceptor interceptor)
    {
        InterceptableContextMenu::SetInterceptor(std::move(interceptor));
    }
    InterceptorGuard(const InterceptorGuard&) = delete;
    InterceptorGuard& operator=(const InterceptorGuard&) = delete;
    ~InterceptorGuard() { InterceptableContextMenu::SetInterceptor(nullptr); }
};

const InterceptableContextMenu::CapturedItem* FindByPath(
    const InterceptableContextMenu::Capture& capture, const std::string& path)
{
    for (const auto& item : capture.Items)
        if (item.Path == path)
            return &item;
    return nullptr;
}

// The interceptor consumes the show, so no window is dereferenced or parked.
InterceptableContextMenu::Capture CaptureOf(InterceptableContextMenu& menu)
{
    std::optional<InterceptableContextMenu::Capture> captured;
    InterceptorGuard guard(
        [&captured](InterceptableContextMenu::Capture&& capture)
        {
            captured = std::move(capture);
            return true;
        });
    menu.Show(reinterpret_cast<Platform::Window*>(std::uintptr_t{1}), 0, 0);
    return captured.value_or(InterceptableContextMenu::Capture{});
}

// A menu holding one of every row the zero-id smear could reach: a real command, a
// non-clickable label registered with id 0, a separator, and a submenu.
uint32_t BuildMixedMenu(InterceptableContextMenu& menu)
{
    menu.AddItem(0, "Command", 7, MenuItemFlag_None);
    menu.AddItem(0, "Locked by another user", 0, MenuItemFlag_Disabled);
    menu.AddSeparator(0);
    return menu.AddSubMenu(0, "More");
}

} // namespace

TEST(MenuCaptureZeroIdTests, ZeroIdStampsAreRefusedAndReachNoRow)
{
    auto backend = std::make_unique<RecordingBackendMenu>();
    RecordingBackendMenu& recorder = *backend;
    InterceptableContextMenu menu(std::move(backend));
    const uint32_t submenuId = BuildMixedMenu(menu);
    ASSERT_NE(submenuId, 0u);

    menu.SetItemEnabled(0, true);
    menu.SetItemChecked(0, true);
    menu.SetItemColor(0, "#FF0000");
    menu.SetItemIcon(0, "editor:Icons/leak.png");
    menu.SetSubMenuIcon(0, "editor:Icons/leak.png");

    EXPECT_TRUE(recorder.Calls.empty())
        << "a zero-id stamp reached the backend, so the live menu and the capture disagree";

    const InterceptableContextMenu::Capture capture = CaptureOf(menu);
    ASSERT_EQ(capture.Items.size(), 4u);
    for (const auto& item : capture.Items)
    {
        EXPECT_TRUE(item.Icon.empty()) << "a zero-id icon smeared onto '" << item.Path << "'";
        EXPECT_TRUE(item.Color.empty()) << "a zero-id colour smeared onto '" << item.Path << "'";
    }

    const auto* command = FindByPath(capture, "Command");
    ASSERT_NE(command, nullptr);
    EXPECT_TRUE(command->Enabled);
    EXPECT_FALSE(command->Checked) << "a zero-id check smeared onto a real command row";

    // The label row carries command id 0 and is registered disabled. Refusing later
    // stamps is what keeps it disabled: it is a label, not a command, and nothing in
    // the editor re-enables it by id.
    const auto* label = FindByPath(capture, "Locked by another user");
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->Enabled) << "a zero-id enable revived a non-clickable label row";
    EXPECT_FALSE(label->Checked);
}

TEST(MenuCaptureZeroIdTests, RealIdsStillStampTheirOwnRow)
{
    auto backend = std::make_unique<RecordingBackendMenu>();
    RecordingBackendMenu& recorder = *backend;
    InterceptableContextMenu menu(std::move(backend));
    const uint32_t submenuId = BuildMixedMenu(menu);
    ASSERT_NE(submenuId, 0u);

    menu.SetItemColor(7, "#3FA7FF");
    menu.SetItemIcon(7, "editor:Icons/command.png");
    menu.SetItemChecked(7, true);
    menu.SetSubMenuIcon(submenuId, "editor:Icons/more.png");

    ASSERT_EQ(recorder.Calls.size(), 4u) << "the guard swallowed a real id";

    const InterceptableContextMenu::Capture capture = CaptureOf(menu);
    const auto* command = FindByPath(capture, "Command");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->Color, "#3FA7FF");
    EXPECT_EQ(command->Icon, "editor:Icons/command.png");
    EXPECT_TRUE(command->Checked);

    const auto* submenu = FindByPath(capture, "More");
    ASSERT_NE(submenu, nullptr);
    EXPECT_TRUE(submenu->IsSubMenu);
    EXPECT_EQ(submenu->Icon, "editor:Icons/more.png");

    const auto* label = FindByPath(capture, "Locked by another user");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->Icon.empty()) << "a real id's icon reached the zero-id label row";
}
