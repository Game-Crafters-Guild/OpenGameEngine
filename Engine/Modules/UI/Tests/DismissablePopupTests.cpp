#include <gtest/gtest.h>

#include <memory>

#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIElement.h"
#include "UI/Internal/LayoutAccess.h"

#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;

namespace
{

// The dismissal contract in isolation: an element that reports open/closed and
// counts how often the manager asked it to close.
class FakePopup : public UIElement, public DismissablePopup
{
  public:
    FakePopup() : DismissablePopup(this) {}

    bool IsPopupOpen() const override { return Open; }
    void DismissPopup() override
    {
        Open = false;
        ++DismissCount;
    }

    bool Open = false;
    int DismissCount = 0;
};

struct Fixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    Rendering::IDevice* Device = nullptr;
    std::unique_ptr<UIManager> Ui;
    FakePopup* Popup = nullptr;
    UIElement* Inside = nullptr;   // descendant of the popup
    UIElement* Outside = nullptr;  // sibling of the popup, elsewhere in the tree

    bool Init()
    {
        Device = SharedHeadlessDevice();
        if (!Device)
            return false;

        auto root = std::make_unique<UIElement>();

        auto popup = std::make_unique<FakePopup>();
        Popup = popup.get();
        auto inside = std::make_unique<UIElement>();
        Inside = inside.get();
        popup->AddChild(std::move(inside));

        auto outside = std::make_unique<UIElement>();
        Outside = outside.get();

        root->AddChild(std::move(popup));
        root->AddChild(std::move(outside));

        Ui = std::make_unique<UIManager>(Device);
        Ui->SetRoot(std::move(root));
        UILayoutAccess::SetLastLayoutRect(*Ui->GetRootElement(), 0.0f, 0.0f, 800.0f, 600.0f);
        // Registration is the control's own job in production (via
        // OnOwnerManagerChanged); FakePopup is a bare element, so do it here.
        Ui->RegisterDismissablePopup(Popup);
        return true;
    }
};

} // namespace

TEST(DismissablePopupTests, OutsidePressDismissesOpenPopup)
{
    Fixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Popup->Open = true;
    EXPECT_TRUE(f.Ui->DismissPopupsForOutsidePress(f.Outside));
    EXPECT_FALSE(f.Popup->IsPopupOpen());
    EXPECT_EQ(f.Popup->DismissCount, 1);
}

TEST(DismissablePopupTests, PressInsidePopupDismissesNothing)
{
    Fixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Popup->Open = true;
    EXPECT_FALSE(f.Ui->DismissPopupsForOutsidePress(f.Inside));
    EXPECT_TRUE(f.Popup->IsPopupOpen());
    EXPECT_EQ(f.Popup->DismissCount, 0);
}

TEST(DismissablePopupTests, ClosedPopupIsNotDismissedAndDoesNotSwallowThePress)
{
    Fixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Popup->Open = false;
    // Returning false is what lets the press through to the control underneath.
    EXPECT_FALSE(f.Ui->DismissPopupsForOutsidePress(f.Outside));
    EXPECT_EQ(f.Popup->DismissCount, 0);
}

TEST(DismissablePopupTests, PressOnNothingStillDismisses)
{
    Fixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    // A press that hit no element at all (gated away, or empty chrome) is still
    // outside the popup.
    f.Popup->Open = true;
    EXPECT_TRUE(f.Ui->DismissPopupsForOutsidePress(nullptr));
    EXPECT_FALSE(f.Popup->IsPopupOpen());
}

TEST(DismissablePopupTests, PressInHostClosesNestedPopupButKeepsTheHost)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    // A dropdown opened inside a search dialog: pressing the dialog's body is
    // outside the dropdown but inside the dialog.
    auto root = std::make_unique<UIElement>();
    auto hostOwned = std::make_unique<FakePopup>();
    FakePopup* host = hostOwned.get();

    auto hostBody = std::make_unique<UIElement>();
    UIElement* body = hostBody.get();
    host->AddChild(std::move(hostBody));

    auto nestedOwned = std::make_unique<FakePopup>();
    FakePopup* nested = nestedOwned.get();
    host->AddChild(std::move(nestedOwned));

    root->AddChild(std::move(hostOwned));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    UILayoutAccess::SetLastLayoutRect(*ui.GetRootElement(), 0.0f, 0.0f, 800.0f, 600.0f);
    ui.RegisterDismissablePopup(host);
    ui.RegisterDismissablePopup(nested);

    host->Open = true;
    nested->Open = true;

    // Return value is "consume the press": the nested popup closes, but the
    // press landed inside the surviving host, so it keeps flowing to its
    // target (clicking a menu's search field with a submenu open must close
    // the submenu AND reach the field in one click).
    EXPECT_FALSE(ui.DismissPopupsForOutsidePress(body));
    EXPECT_FALSE(nested->IsPopupOpen());
    EXPECT_TRUE(host->IsPopupOpen());
}

TEST(DismissablePopupTests, EscapeClosesOnlyTheTopmostPopup)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    auto lowerOwned = std::make_unique<FakePopup>();
    auto upperOwned = std::make_unique<FakePopup>();
    FakePopup* lower = lowerOwned.get();
    FakePopup* upper = upperOwned.get();
    root->AddChild(std::move(lowerOwned));
    root->AddChild(std::move(upperOwned));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    UILayoutAccess::SetLastLayoutRect(*ui.GetRootElement(), 0.0f, 0.0f, 800.0f, 600.0f);
    // Registration order is attach order, so `upper` is topmost.
    ui.RegisterDismissablePopup(lower);
    ui.RegisterDismissablePopup(upper);

    lower->Open = true;
    upper->Open = true;

    EXPECT_TRUE(ui.DismissTopmostPopup());
    EXPECT_FALSE(upper->IsPopupOpen());
    EXPECT_TRUE(lower->IsPopupOpen());

    EXPECT_TRUE(ui.DismissTopmostPopup());
    EXPECT_FALSE(lower->IsPopupOpen());

    EXPECT_FALSE(ui.DismissTopmostPopup());
}

TEST(DismissablePopupTests, UnregisteredPopupNoLongerAnswersToDismissal)
{
    Fixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Popup->Open = true;
    f.Ui->UnregisterDismissablePopup(f.Popup);

    EXPECT_FALSE(f.Ui->DismissPopupsForOutsidePress(f.Outside));
    EXPECT_FALSE(f.Ui->DismissTopmostPopup());
    EXPECT_TRUE(f.Popup->IsPopupOpen());
}

TEST(DismissablePopupTests, DetachedPopupDoesNotGateOrDismiss)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    UILayoutAccess::SetLastLayoutRect(*ui.GetRootElement(), 0.0f, 0.0f, 800.0f, 600.0f);

    // Owned by the manager but never attached to the root — the state an
    // inactive dock tab's popup is in. It must not swallow input for the
    // panels that are actually on screen.
    FakePopup orphan;
    orphan.Open = true;
    ui.RegisterDismissablePopup(&orphan);

    EXPECT_FALSE(ui.DismissPopupsForOutsidePress(nullptr));
    EXPECT_FALSE(ui.DismissTopmostPopup());
    EXPECT_TRUE(orphan.IsPopupOpen());

    ui.UnregisterDismissablePopup(&orphan);
}
