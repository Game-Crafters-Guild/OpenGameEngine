// The interceptor's capture is the automation channel for every editor menu
// (open_context_menu reads it), so a field only the backend could see is a
// field automation cannot verify. SetItemColor rides the shadow into the
// capture exactly like SetItemIcon's path — pinned here because colour was
// the one setter the shadow did not record.

#include <gtest/gtest.h>

#include "EditorContextMenu/InterceptableContextMenu.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace GameEngine;

namespace
{

class NoopBackendMenu final : public INativeContextMenu
{
  public:
    void Clear() override {}
    uint32_t AddSubMenu(uint32_t, const std::string&) override { return 0; }
    void AddItem(uint32_t, const std::string&, uint32_t, uint32_t) override {}
    void AddSeparator(uint32_t) override {}
    void SetCommandHandler(CommandCallback) override {}
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t, bool) override {}
    void SetItemChecked(uint32_t, bool) override {}
    void Show(Platform::Window*, int, int) override {}
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

} // namespace

TEST(MenuCaptureColorTests, SetItemColorRidesTheCaptureAndOnlyOnItsRow)
{
    InterceptableContextMenu menu(std::make_unique<NoopBackendMenu>());
    menu.AddItem(0, "Swatch", 1, MenuItemFlag_None);
    menu.AddItem(0, "Plain", 2, MenuItemFlag_None);
    menu.SetItemColor(1, "#3FA7FF");

    std::optional<InterceptableContextMenu::Capture> captured;
    InterceptorGuard guard(
        [&captured](InterceptableContextMenu::Capture&& capture)
        {
            captured = std::move(capture);
            return true;
        });

    // The interceptor consumes the show, so no window is dereferenced or parked.
    menu.Show(reinterpret_cast<Platform::Window*>(std::uintptr_t{1}), 0, 0);

    ASSERT_TRUE(captured.has_value());
    const auto* swatch = FindByPath(*captured, "Swatch");
    ASSERT_NE(swatch, nullptr);
    EXPECT_EQ(swatch->Color, "#3FA7FF") << "the capture lost the row's colour";
    const auto* plain = FindByPath(*captured, "Plain");
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(plain->Color.empty()) << "the colour leaked onto a row it was never set on";
}

TEST(MenuCaptureColorTests, ClearDropsARowsColourWithTheRow)
{
    InterceptableContextMenu menu(std::make_unique<NoopBackendMenu>());
    menu.AddItem(0, "Swatch", 1, MenuItemFlag_None);
    menu.SetItemColor(1, "#3FA7FF");
    menu.Clear();
    menu.AddItem(0, "Swatch", 1, MenuItemFlag_None);

    std::optional<InterceptableContextMenu::Capture> captured;
    InterceptorGuard guard(
        [&captured](InterceptableContextMenu::Capture&& capture)
        {
            captured = std::move(capture);
            return true;
        });

    menu.Show(reinterpret_cast<Platform::Window*>(std::uintptr_t{1}), 0, 0);

    ASSERT_TRUE(captured.has_value());
    const auto* swatch = FindByPath(*captured, "Swatch");
    ASSERT_NE(swatch, nullptr);
    EXPECT_TRUE(swatch->Color.empty())
        << "a rebuilt row inherited the colour of the row it replaced";
}
