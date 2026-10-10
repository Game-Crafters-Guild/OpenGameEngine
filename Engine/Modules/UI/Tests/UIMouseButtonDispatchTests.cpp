// Synchronous mouse-button dispatch: OnMouseButton delivers the transition at
// the platform callback and reports whether a control acted on it.
//
// These are the successors to the transition-queue tests. The queue existed to
// stop a press+release pair that completed inside one frame interval from being
// cancelled out by a per-frame latch; dispatching at the callback removes the
// sampling boundary the pair was lost at, so the guarantee is now structural.
// Each arm below names the guarantee it protects rather than the mechanism that
// used to provide it, because the mechanism is gone and the guarantees are not.
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

#include "UIRgTestHarness.h"
#include "UI/Controls/Button.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

using namespace GameEngine;

namespace
{

// Button under a 200x40 root at a known rect, with MouseDown/MouseUp/Click
// observation. The CSS file gives the elements real layout so hit-testing works.
struct DispatchFixture
{
    std::unique_ptr<Rendering::IDevice> Dev;
    std::unique_ptr<UiRgHarness> Rg;
    std::unique_ptr<UIManager> Ui;
    Button* Btn = nullptr;

    std::vector<EventId> Events; // kEventMouseDown / kEventMouseUp in arrival order
    int Clicks = 0;
    int Cancels = 0;

    bool Init()
    {
        Dev = MakeHeadlessDevice();
        if (!Dev)
            return false;
        Rg = std::make_unique<UiRgHarness>(Dev.get());
        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto btnOwned = std::make_unique<Button>();
        Btn = btnOwned.get();
        Btn->SetId("btn");
        Btn->RegisterEventHandler(kEventMouseDown, [this](UIEvent&) { Events.push_back(kEventMouseDown); });
        Btn->RegisterEventHandler(kEventMouseUp, [this](UIEvent&) { Events.push_back(kEventMouseUp); });
        Btn->RegisterEventHandler(kEventMouseCancel, [this](UIEvent&) { ++Cancels; });
        Btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ++Clicks; });
        root->AddChild(std::move(btnOwned));

        Ui = std::make_unique<UIManager>(Dev.get());
        Ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_mouse_button_dispatch.css";
        {
            std::ofstream f(css);
            f << R"(
#root { display: flex; width: 200px; height: 40px; }
#btn { width: 100px; height: 20px; }
)";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        // First layout, then park the cursor over the button. Dispatch hit-tests
        // against the last completed layout, so a layout has to exist before any
        // button event means anything.
        Ui->Update(0.0f, /*interactive=*/false);
        DriveUiRender(*Ui, *Rg);
        Ui->OnMouseMove(Btn->GetLayoutX() + 5.0f, Btn->GetLayoutY() + 5.0f);
        Frame();
        return true;
    }

    void Frame(bool interactive = true)
    {
        Ui->Update(0.0f, interactive);
        DriveUiRender(*Ui, *Rg);
    }
};

} // namespace

// THE HEADLINE GUARANTEE, inherited verbatim from the queue suite: a press and
// release that both arrive before the next frame still produce a click. The
// queue drained the pair over two frames to achieve this; dispatch at the
// callback achieves it by leaving no sampling boundary for the pair to be lost
// at, so both edges land before Update is ever called.
TEST(UIMouseButtonDispatchTests, SameFrameClickPairStillClicks)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Ui->OnMouseButton(0, true);
    f.Ui->OnMouseButton(0, false); // same frame interval — no Update between

    // The whole point: no frame has run, and the click has already happened.
    ASSERT_EQ(f.Events.size(), 2u);
    EXPECT_EQ(f.Events[0], kEventMouseDown);
    EXPECT_EQ(f.Events[1], kEventMouseUp);
    EXPECT_EQ(f.Clicks, 1);
}

// The order half of "no transition lost". Two full clicks inside one frame
// interval deliver four edges, in platform order, with no frame in between.
// The queue's drain-order test measured the same property of a mechanism that
// no longer exists; the property still has to hold.
TEST(UIMouseButtonDispatchTests, RapidTransitionsDispatchInPlatformOrder)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Ui->OnMouseButton(0, true);
    f.Ui->OnMouseButton(0, false);
    f.Ui->OnMouseButton(0, true);
    f.Ui->OnMouseButton(0, false);

    ASSERT_EQ(f.Events.size(), 4u);
    EXPECT_EQ(f.Events[0], kEventMouseDown);
    EXPECT_EQ(f.Events[1], kEventMouseUp);
    EXPECT_EQ(f.Events[2], kEventMouseDown);
    EXPECT_EQ(f.Events[3], kEventMouseUp);
    EXPECT_EQ(f.Clicks, 2);
}

// The replacement for the passive-frame pin. The queue had to wait for an
// interactive frame because a passive pass committed the latch without
// dispatching, which would have eaten the transition. Dispatch no longer
// depends on the frame at all, so the guarantee inverts into a stronger one:
// a host that is rendering passively still delivers every event, immediately.
TEST(UIMouseButtonDispatchTests, PassiveHostStillDispatchesEveryTransition)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Frame(/*interactive=*/false);
    f.Ui->OnMouseButton(0, true);
    f.Ui->OnMouseButton(0, false);
    f.Frame(/*interactive=*/false);

    ASSERT_EQ(f.Events.size(), 2u);
    EXPECT_EQ(f.Events[0], kEventMouseDown);
    EXPECT_EQ(f.Events[1], kEventMouseUp);
    EXPECT_EQ(f.Clicks, 1) << "a passively-rendering host must not swallow input";
}

// CancelPress with no queue behind it. The queue-clear is gone because nothing
// is queued; what has to survive is the semantic: the gesture is terminated,
// the armed control is disarmed and told so, capture is dropped, and nothing
// activates.
TEST(UIMouseButtonDispatchTests, CancelPressTerminatesTheGestureWithoutActivating)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Ui->OnMouseButton(0, true);
    ASSERT_EQ(f.Events.size(), 1u) << "the press dispatches at the callback";
    ASSERT_EQ(f.Events[0], kEventMouseDown);
    ASSERT_TRUE(f.Ui->IsMouseDown());

    f.Ui->CancelPress();

    EXPECT_EQ(f.Cancels, 1) << "the armed control must be told the gesture died";
    EXPECT_FALSE(f.Ui->IsMouseDown()) << "cancel disarms the press";
    EXPECT_EQ(f.Clicks, 0) << "a cancel activates nothing";

    // And it stays dead: frames after the cancel must not resurrect the press.
    f.Frame();
    f.Frame();
    EXPECT_EQ(f.Clicks, 0);
}

// A release the manager never saw a press for must be a no-op. With no queue
// there is no stored state for it to disturb, and the router forwards releases
// unconditionally precisely because they are safe — this pins that they are.
TEST(UIMouseButtonDispatchTests, UnmatchedReleaseIsANoOp)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    EXPECT_FALSE(f.Ui->OnMouseButton(0, false)) << "nothing acted on a release with no press";
    EXPECT_TRUE(f.Events.empty());
    EXPECT_EQ(f.Clicks, 0);
    EXPECT_FALSE(f.Ui->IsMouseDown());

    // A real click still works afterwards: the stray release left nothing behind.
    f.Ui->OnMouseButton(0, true);
    f.Ui->OnMouseButton(0, false);
    EXPECT_EQ(f.Clicks, 1);
}

// A second release while already released is the same no-op, from the other
// direction: repeated edges in the same direction carry no transition.
TEST(UIMouseButtonDispatchTests, RepeatedEdgeInTheSameDirectionCarriesNoTransition)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    f.Ui->OnMouseButton(0, true);
    f.Ui->OnMouseButton(0, true); // duplicate press
    f.Ui->OnMouseButton(0, false);
    f.Ui->OnMouseButton(0, false); // duplicate release

    ASSERT_EQ(f.Events.size(), 2u) << "a duplicate edge must not arm or activate a second time";
    EXPECT_EQ(f.Events[0], kEventMouseDown);
    EXPECT_EQ(f.Events[1], kEventMouseUp);
    EXPECT_EQ(f.Clicks, 1);
}

// The answer the router routes on. A press over a control that acts is
// consumed; the same press over bare background is not, which is what lets a
// click fall through the editor chrome to the game behind it.
TEST(UIMouseButtonDispatchTests, ConsumptionReportsWhetherAControlActed)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    EXPECT_TRUE(f.Ui->OnMouseButton(0, true)) << "a press on a Button is that Button's press";
    f.Ui->OnMouseButton(0, false);

    // Move onto the root, outside the button's 100x20 rect, and re-hover.
    f.Ui->OnMouseMove(f.Btn->GetLayoutX() + 150.0f, f.Btn->GetLayoutY() + 5.0f);
    f.Frame();

    EXPECT_FALSE(f.Ui->OnMouseButton(0, true))
        << "a press on nothing that acts must stay unconsumed, or it can never reach the game";
    f.Ui->OnMouseButton(0, false);
}

// Dispatch takes the freshest pointer position available, not the hover state
// the last rendered frame happened to leave behind. A press that arrives after
// the cursor moved — with no frame in between to re-run the hover pass — must
// land on the element the cursor is actually over.
TEST(UIMouseButtonDispatchTests, PressHitTestsTheCurrentPositionNotLastFrameHover)
{
    DispatchFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    // Cursor is parked on the button from Init, and a frame has resolved hover
    // there. Now move off it and press without letting a frame run.
    f.Ui->OnMouseMove(f.Btn->GetLayoutX() + 150.0f, f.Btn->GetLayoutY() + 5.0f);
    f.Ui->OnMouseButton(0, true);
    f.Ui->OnMouseButton(0, false);

    EXPECT_TRUE(f.Events.empty())
        << "the press was dispatched to the stale hover instead of the current position";
    EXPECT_EQ(f.Clicks, 0);
}
