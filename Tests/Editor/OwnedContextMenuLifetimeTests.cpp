// Ownership contract for a context menu that is on screen.
//
// Only Win32 blocks in Show(): TrackPopupMenuEx runs a nested loop and
// dispatches the command before returning. The editor-drawn backend returns as
// soon as its elements exist, and the macOS backend defers the popup to the next
// main-queue turn behind a liveness flag its destructor clears — so on both, a
// caller-local unique_ptr destructs before the menu is ever on screen and the
// menu silently never appears. Show() parks the backend so it cannot: these
// tests show every menu through a caller-local wrapper that destructs on
// return, which is exactly what a call site does.
//
// Hazards the slot introduces, and that these tests pin:
// - A command handler running *inside* Show() (Win32) that opens another menu
//   must not free the menu owning its own stack frame.
// - A command handler running *after* Show() returned (drawn row handler, macOS
//   deferred popup) is the same free-under-the-frame; destruction waits for
//   DrainShowingContextMenus().
// - The slot must be released while the UIManager is still alive, not from
//   Unregister after ui.reset(), and not only on platforms that Register.
// - Closing one window must not destroy a menu that is open on another.

#include <gtest/gtest.h>

#include "EditorContextMenu/ContextMenuBackendPolicy.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

using namespace GameEngine;

namespace
{

// Records its own destruction into a caller-owned flag and can run an arbitrary
// action from inside Show(), which is where Win32 dispatches menu commands.
class LifetimeProbeMenu final : public INativeContextMenu
{
  public:
    LifetimeProbeMenu(bool* destroyedFlag, int* showCount)
        : m_Destroyed(destroyedFlag), m_ShowCount(showCount)
    {
    }

    // Reported outward rather than read back from the object: once the outermost
    // Show() returns, the retire list drains and this menu is gone, so a probe
    // that only recorded into itself could not be asked afterwards.
    void SetCompletedShowFlag(bool* flag) { m_CompletedShow = flag; }

    ~LifetimeProbeMenu() override
    {
        if (m_Destroyed)
            *m_Destroyed = true;
    }

    void SetInsideShow(std::function<void()> action) { m_InsideShow = std::move(action); }

    void Clear() override {}
    uint32_t AddSubMenu(uint32_t, const std::string&) override { return 0; }
    void AddItem(uint32_t, const std::string&, uint32_t, uint32_t) override {}
    void AddSeparator(uint32_t) override {}
    void SetCommandHandler(CommandCallback) override {}
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t, bool) override {}
    void SetItemChecked(uint32_t, bool) override {}

    void Show(Platform::Window*, int, int) override
    {
        if (m_ShowCount)
            ++*m_ShowCount;
        if (m_InsideShow)
            m_InsideShow();
        // Writing a member after the reentrant action is the access a
        // free-under-the-frame would corrupt.
        m_ShownAtLeastOnce = true;
        if (m_CompletedShow)
            *m_CompletedShow = m_ShownAtLeastOnce;
    }

  private:
    bool* m_Destroyed = nullptr;
    int* m_ShowCount = nullptr;
    bool* m_CompletedShow = nullptr;
    std::function<void()> m_InsideShow;
    bool m_ShownAtLeastOnce = false;
};

std::unique_ptr<LifetimeProbeMenu> MakeProbe(bool* destroyed, int* showCount = nullptr)
{
    return std::make_unique<LifetimeProbeMenu>(destroyed, showCount);
}

Platform::Window* FakeWindow(std::uintptr_t id)
{
    return reinterpret_cast<Platform::Window*>(id);
}

// Mirrors a real call site: build the menu, show it, let the local destruct on
// return. Nothing here keeps the wrapper alive, so anything that survives the
// call survives because Show() parked it.
void ShowThroughCallerLocal(std::unique_ptr<INativeContextMenu> backend, Platform::Window* window,
                            int x, int y)
{
    auto menu = std::make_unique<InterceptableContextMenu>(std::move(backend));
    menu->Show(window, x, y);
}

} // namespace

// The defect this helper exists for: a menu shown through it is still alive
// after the call that showed it returns, so a backend that puts the menu on
// screen later still has one.
TEST(OwnedContextMenuLifetime, TheMenuOutlivesTheCallThatShowedIt)
{
    ReleaseShowingContextMenu();

    bool destroyed = false;
    int shows = 0;
    ShowThroughCallerLocal(MakeProbe(&destroyed, &shows), nullptr, 10, 20);

    EXPECT_EQ(shows, 1);
    EXPECT_FALSE(destroyed) << "the menu died in the call that showed it, which is the defect "
                               "ShowOwnedContextMenu exists to prevent";

    ReleaseShowingContextMenu();
    EXPECT_TRUE(destroyed);
}

// One open menu at a time: taking the slot closes whoever held it. Show() has
// already returned, so destruction waits for the drain — the same stack a drawn
// row handler or macOS deferred popup would still be on.
TEST(OwnedContextMenuLifetime, ShowingASecondMenuClosesTheFirst)
{
    ReleaseShowingContextMenu();

    bool firstDestroyed = false;
    bool secondDestroyed = false;
    ShowThroughCallerLocal(MakeProbe(&firstDestroyed), nullptr, 0, 0);
    ASSERT_FALSE(firstDestroyed);

    ShowThroughCallerLocal(MakeProbe(&secondDestroyed), nullptr, 0, 0);
    EXPECT_FALSE(firstDestroyed)
        << "displaced after Show returned; destroying now would free a handler still on the stack";
    EXPECT_FALSE(secondDestroyed);

    DrainShowingContextMenus();
    EXPECT_TRUE(firstDestroyed) << "the displaced menu was never released";
    EXPECT_FALSE(secondDestroyed);

    ReleaseShowingContextMenu();
    EXPECT_TRUE(secondDestroyed);
}

// DrainShowingContextMenus must not take the currently showing menu — only ones
// already displaced.
TEST(OwnedContextMenuLifetime, DrainDoesNotReleaseTheShowingMenu)
{
    ReleaseShowingContextMenu();

    bool destroyed = false;
    ShowThroughCallerLocal(MakeProbe(&destroyed), nullptr, 0, 0);
    DrainShowingContextMenus();
    EXPECT_FALSE(destroyed);

    ReleaseShowingContextMenu();
    EXPECT_TRUE(destroyed);
}

// Win32 invokes the command handler inside TrackPopupMenuEx. A command that
// opens another menu is therefore running inside the menu it displaces, and
// freeing it there frees the object owning the live frame.
TEST(OwnedContextMenuLifetime, AMenuOpenedFromInsideShowDoesNotFreeItsOwnFrame)
{
    ReleaseShowingContextMenu();

    bool outerDestroyed = false;
    bool innerDestroyed = false;
    bool outerWasAliveDuringInnerShow = false;
    bool outerFinishedShow = false;

    auto outer = MakeProbe(&outerDestroyed);
    outer->SetCompletedShowFlag(&outerFinishedShow);
    outer->SetInsideShow([&]() {
        ShowThroughCallerLocal(MakeProbe(&innerDestroyed), nullptr, 1, 1);
        outerWasAliveDuringInnerShow = !outerDestroyed;
    });

    ShowThroughCallerLocal(std::move(outer), nullptr, 0, 0);

    EXPECT_TRUE(outerWasAliveDuringInnerShow)
        << "the reentrant open destroyed the menu whose Show() was on the stack";
    EXPECT_TRUE(outerFinishedShow)
        << "Show() could not finish writing to its own object after the reentrant open";
    EXPECT_TRUE(outerDestroyed) << "the displaced menu is retired but never released";
    EXPECT_FALSE(innerDestroyed);

    ReleaseShowingContextMenu();
    EXPECT_TRUE(innerDestroyed);
}

// Drawn backends and macOS run the command after Show() has returned. Opening
// another menu from that handler must not destroy the outer object until drain.
TEST(OwnedContextMenuLifetime, AMenuOpenedAfterShowReturnsIsNotFreedUntilDrain)
{
    ReleaseShowingContextMenu();

    bool outerDestroyed = false;
    bool innerDestroyed = false;
    bool outerFinishedShow = false;

    auto outer = MakeProbe(&outerDestroyed);
    outer->SetCompletedShowFlag(&outerFinishedShow);
    ShowThroughCallerLocal(std::move(outer), nullptr, 0, 0);
    ASSERT_TRUE(outerFinishedShow);
    ASSERT_FALSE(outerDestroyed);

    ShowThroughCallerLocal(MakeProbe(&innerDestroyed), nullptr, 1, 1);
    EXPECT_FALSE(outerDestroyed)
        << "post-Show replace destroyed the menu a row handler would still be running on";
    EXPECT_FALSE(innerDestroyed);

    DrainShowingContextMenus();
    EXPECT_TRUE(outerDestroyed);
    EXPECT_FALSE(innerDestroyed);

    ReleaseShowingContextMenu();
    EXPECT_TRUE(innerDestroyed);
}

// The slot must be empty before the editor destroys UIManagers: OnShutdown
// calls this, and a drawn menu's destructor removes its overlay through a
// UIManager that ui.reset() is about to destroy.
TEST(OwnedContextMenuLifetime, ReleasingTheSlotIsIdempotent)
{
    ReleaseShowingContextMenu();

    bool destroyed = false;
    ShowThroughCallerLocal(MakeProbe(&destroyed), nullptr, 0, 0);

    ReleaseShowingContextMenu();
    EXPECT_TRUE(destroyed);
    ReleaseShowingContextMenu(); // no double free, no crash
}

// A null menu is a no-op rather than a way to silently close the open one.
TEST(OwnedContextMenuLifetime, ANullMenuLeavesTheOpenOneAlone)
{
    ReleaseShowingContextMenu();

    bool destroyed = false;
    ShowThroughCallerLocal(MakeProbe(&destroyed), nullptr, 0, 0);
    ShowThroughCallerLocal(nullptr, nullptr, 0, 0);
    EXPECT_FALSE(destroyed);

    ReleaseShowingContextMenu();
    EXPECT_TRUE(destroyed);
}

// Closing a floating window must not take a menu that is open on another window.
TEST(OwnedContextMenuLifetime, ReleasingForADifferentWindowLeavesTheMenu)
{
    ReleaseShowingContextMenu();

    bool destroyed = false;
    Platform::Window* windowA = FakeWindow(1);
    Platform::Window* windowB = FakeWindow(2);
    ShowThroughCallerLocal(MakeProbe(&destroyed), windowA, 0, 0);

    ReleaseShowingContextMenuForWindow(windowB);
    EXPECT_FALSE(destroyed);

    ReleaseShowingContextMenuForWindow(windowA);
    EXPECT_TRUE(destroyed);
}

// A menu outlives the UIManager it last opened in when its owner (a panel, the
// application) is destroyed after the window's UI. Its teardown closes the menu
// and must reach neither the destroyed manager nor the overlay that died under
// its root. Without the liveness check this is a use-after-free, which only an
// address-sanitized run reports.
TEST(OwnedContextMenuLifetime, AMenuDestroyedAfterItsUIManagerDoesNotReachIt)
{
    ReleaseShowingContextMenu();
    const ContextMenuBackend restoreBackend = GetContextMenuBackend();
    SetContextMenuBackend(ContextMenuBackend::BuiltIn);

    Platform::Window* window = FakeWindow(3);
    auto manager = std::make_unique<UIManager>(nullptr);
    manager->SetRoot(std::make_unique<UIElement>());
    UIContextMenu::Register(window, manager.get());

    auto menu = CreateContextMenu();
    ASSERT_NE(menu, nullptr);
    bool closed = false;
    menu->SetCloseHandler([&closed]() { closed = true; });
    menu->AddItem(0, "Item", 1);
    menu->Show(window, 10, 10);
    ReleaseShowingContextMenu();

    UIContextMenu::Unregister(window);
    manager.reset();
    menu.reset();

    EXPECT_TRUE(closed) << "destroying an open menu must still report it closed";
    SetContextMenuBackend(restoreBackend);
}
