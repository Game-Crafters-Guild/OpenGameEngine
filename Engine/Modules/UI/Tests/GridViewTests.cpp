#include <gtest/gtest.h>

#include "UI/Controls/GridView.h"
#include "UI/Controls/Label.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include "Input/InputSystem.h"
#include "Platform/SystemMetrics.h"

#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>

using namespace GameEngine;

namespace {

// Minimal grid provider: ids are index+1, no icons, no grouping.
class UniformGridProvider : public GridChangeTrackingProvider {
public:
    explicit UniformGridProvider(int count) : m_Count(count) {}

    int GetItemCount() const override { return m_Count; }
    GridId GetItemId(int index) const override { return static_cast<GridId>(index + 1); }
    const char* GetLabel(GridId /*id*/) const override { return "item"; }
    uint64_t GetIcon(GridId /*id*/) const override { return 0; }
    const char* GetTypeKey(GridId /*id*/) const override { return ""; }
    void ApplySort(const SortDescriptor& /*desc*/) override {}
    void ApplyGrouping(const GroupDescriptor& /*desc*/) override {}

    void SetCount(int count) { m_Count = count; }

private:
    int m_Count;
};

} // namespace

// Drop hit-testing must only consider live bound cells: unbound pooled slots
// keep whatever layout rect they last had, and a point inside such a stale
// rect must resolve as empty space, never as a cell. Mirrors
// ListViewTests.DropHitTestSkipsStaleUnboundSlots for the grid pool.
TEST(GridViewTests, DropHitTestSkipsStaleUnboundSlots)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto grid = std::make_unique<GridView>();
    GridView* gridRaw = grid.get();
    // At least two columns at the default cell metrics (icon 80 -> cell
    // 96x120), with headroom for scrollbar gutter/chrome.
    gridRaw->Overrides()
        .Set(Style::Width, StyleLength::Px(300.0f))
        .Set(Style::Height, StyleLength::Px(160.0f));
    root->AddChild(std::move(grid));

    UniformGridProvider provider(20);
    gridRaw->SetDataProvider(&provider);

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    auto pump = [&]
    {
        for (int i = 0; i < 5; ++i)
        {
            ui.Update(0.0f, /*interactive=*/false);
            DriveUiRender(ui, rg);
        }
    };
    pump();

    const UI::Interaction::IDropTarget& drop = *gridRaw;
    const float gx = gridRaw->GetLayoutX();
    const float gy = gridRaw->GetLayoutY();
    const float gw = gridRaw->GetLayoutWidth();
    const float gh = gridRaw->GetLayoutHeight();
    ASSERT_GT(gw, 0.0f) << "grid must have layout";
    ASSERT_GE(gridRaw->GetColumnCount(), 2) << "test needs two columns for a stale second cell";

    // Locate points inside item 0's and item 1's cells by scanning with the
    // public hit-test (geometry-independent: no cell accessor on GridView).
    float item0X = -1.0f;
    float item1X = -1.0f;
    float probeY = -1.0f;
    for (float py = gy + 8.0f; py < gy + gh && item1X < 0.0f; py += 16.0f)
    {
        for (float px = gx + 4.0f; px < gx + gw; px += 4.0f)
        {
            UI::Interaction::DropHit h{};
            if (!drop.HitTestDropTarget(px, py, h) ||
                h.Location != UI::Interaction::DropLocation::OnItem)
                continue;
            if (h.TargetId == provider.GetItemId(0) && item0X < 0.0f)
            {
                item0X = px;
                probeY = py;
            }
            else if (h.TargetId == provider.GetItemId(1) && item0X >= 0.0f)
            {
                item1X = px;
                break;
            }
        }
    }
    ASSERT_GE(item0X, 0.0f) << "grid must bind item 0";
    ASSERT_GE(item1X, 0.0f) << "grid must bind item 1 on the same row as item 0";

    // Shrink to a single item: the slot that held item 1 unbinds but stays
    // pooled with whatever rect it last had. The same point must now fall
    // through to the empty-space hit, never a cell.
    provider.SetCount(1);
    gridRaw->RefreshFromProvider();
    pump();

    UI::Interaction::DropHit staleHit{};
    ASSERT_TRUE(drop.HitTestDropTarget(item1X, probeY, staleHit))
        << "inside the GridView rect the empty-space fallback must hit";
    EXPECT_EQ(staleHit.TargetId, 0u) << "stale unbound slot must not produce a cell hit";
    EXPECT_EQ(staleHit.Location, UI::Interaction::DropLocation::OnEmptySpace);

    // The surviving item still hits.
    UI::Interaction::DropHit hitAlive{};
    ASSERT_TRUE(drop.HitTestDropTarget(item0X, probeY, hitAlive));
    EXPECT_EQ(hitAlive.TargetId, provider.GetItemId(0));
    EXPECT_EQ(hitAlive.Location, UI::Interaction::DropLocation::OnItem);
}

// Grid mode of a picker popup must commit on the first click too, so the two
// SearchDialog result views cannot disagree about what a click means.
namespace {

struct GridClickFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    GameEngine::Rendering::IDevice* Device = nullptr;
    std::unique_ptr<UiRgHarness> Rg;
    std::unique_ptr<UIManager> Ui;
    GridView* Grid = nullptr;
    UniformGridProvider Provider{20};
    std::unordered_map<GridId, UIElement*> Cells;

    bool Build()
    {
        Device = SharedHeadlessDevice();
        if (!Device)
            return false;
        Rg = std::make_unique<UiRgHarness>(Device);
        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto grid = std::make_unique<GridView>();
        Grid = grid.get();
        Grid->Overrides()
            .Set(Style::Width, StyleLength::Px(300.0f))
            .Set(Style::Height, StyleLength::Px(160.0f));
        root->AddChild(std::move(grid));
        Grid->SetDataProvider(&Provider);
        // The binder receives the pooled cell that carries the click handler —
        // GridView exposes no cell accessor, so record it here.
        Grid->SetItemBinder([this](UIElement* cell, GridId id, IGridDataProvider*) { Cells[id] = cell; });

        Ui = std::make_unique<UIManager>(Device);
        Ui->SetRoot(std::move(root));
        Pump();
        return !Cells.empty();
    }

    void Pump()
    {
        for (int i = 0; i < 5; ++i)
        {
            Ui->Update(0.0f, /*interactive=*/false);
            DriveUiRender(*Ui, *Rg);
        }
    }

    // A full press/release pair on the cell: GridView establishes selection on the
    // press and decides click/double-click/rename on the release.
    void ClickCell(GridId id, int mods = 0)
    {
        DispatchMouse(id, kEventMouseDown, mods);
        DispatchMouse(id, kEventMouseUp, mods);
    }

    void DispatchMouse(GridId id, EventId eventId, int mods)
    {
        auto it = Cells.find(id);
        ASSERT_NE(it, Cells.end()) << "cell " << id << " must be bound";
        UIElement* cell = it->second;
        UIEvent e{};
        e.Id = eventId;
        e.Button = 0;
        e.Mods = mods;
        e.X = cell->GetLayoutX() + 4.0f;
        e.Y = cell->GetLayoutY() + 4.0f;
        e.Target = cell;
        e.CurrentTarget = cell;
        cell->DispatchEvent(e);
    }
};

// Past the platform double-click interval, so the next click on the same item is
// a slow repeat rather than a double-click.
void WaitOutDoubleClickInterval()
{
    std::this_thread::sleep_for(Platform::GetDoubleClickInterval() + std::chrono::milliseconds(50));
}

} // namespace

TEST(GridViewTests, TitleGeometryRemainsBoundedAfterRebindAndIconResize)
{
    GridClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";
    auto* title = fx.Grid->GetTitleLabelForIndex(0);
    ASSERT_NE(title, nullptr);
    const float width = title->GetLayoutWidth();
    const float height = title->GetLayoutHeight();
    EXPECT_GT(width, 0.0f);
    EXPECT_GE(height, 20.0f);
    title->SetText("A much longer asset filename that must stay within the pooled cell");
    fx.Pump();
    EXPECT_FLOAT_EQ(title->GetLayoutWidth(), width);
    EXPECT_FLOAT_EQ(title->GetLayoutHeight(), height);
    fx.Grid->SetIconSize(96.0f);
    fx.Pump();
    title = fx.Grid->GetTitleLabelForIndex(0);
    ASSERT_NE(title, nullptr);
    EXPECT_GT(title->GetLayoutWidth(), 0.0f);
    EXPECT_GE(title->GetLayoutHeight(), 20.0f);
    EXPECT_LT(title->GetLayoutWidth(), fx.Grid->GetLayoutWidth());
}

TEST(GridViewTests, PickerActivationCommitsOnFirstClick)
{
    GridClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int activated = 0;
    GridId activatedId = 0;
    fx.Grid->SetOnItemActivated([&](GridId id) { ++activated; activatedId = id; });
    fx.Grid->SetPickerActivation(true);

    const GridId target = fx.Provider.GetItemId(1);
    fx.ClickCell(target);
    EXPECT_EQ(activated, 1) << "a picker cell must commit on the first click";
    EXPECT_EQ(activatedId, target);
}

// Mutation guard: browsing grids keep double-click activation.
TEST(GridViewTests, BrowseGridDoesNotActivateOnFirstClick)
{
    GridClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int activated = 0;
    fx.Grid->SetOnItemActivated([&](GridId) { ++activated; });

    const GridId target = fx.Provider.GetItemId(1);
    fx.ClickCell(target);
    EXPECT_EQ(activated, 0) << "a browsing grid must not activate on a single click";

    fx.ClickCell(target);
    EXPECT_EQ(activated, 1) << "a browsing grid must still activate on double click";
}

// The file-manager rename gesture: a slow second click on the item that is
// already the sole selection asks the host to rename it, and does not activate.
TEST(GridViewTests, SlowSecondClickOnTheSoleSelectionRequestsRename)
{
    GridClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int renameRequests = 0;
    GridId renameTarget = 0;
    int activations = 0;
    fx.Grid->SetOnItemRenameRequested([&](GridId id) { ++renameRequests; renameTarget = id; });
    fx.Grid->SetOnItemActivated([&](GridId) { ++activations; });

    const GridId target = fx.Cells.begin()->first;
    fx.ClickCell(target);
    EXPECT_EQ(renameRequests, 0) << "the selecting click is not a rename request";

    WaitOutDoubleClickInterval();
    fx.ClickCell(target);
    EXPECT_EQ(renameRequests, 1);
    EXPECT_EQ(renameTarget, target);
    EXPECT_EQ(activations, 0);
}

TEST(GridViewTests, DoubleClickActivatesAndNeverRequestsRename)
{
    GridClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int renameRequests = 0;
    int activations = 0;
    fx.Grid->SetOnItemRenameRequested([&](GridId) { ++renameRequests; });
    fx.Grid->SetOnItemActivated([&](GridId) { ++activations; });

    const GridId target = fx.Cells.begin()->first;
    fx.ClickCell(target);
    fx.ClickCell(target);
    EXPECT_EQ(activations, 1);
    EXPECT_EQ(renameRequests, 0);
}

TEST(GridViewTests, ModifiedRepeatClickIsASelectionEditNotARename)
{
    GridClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int renameRequests = 0;
    fx.Grid->SetOnItemRenameRequested([&](GridId) { ++renameRequests; });

    const GridId target = fx.Cells.begin()->first;
    fx.ClickCell(target);
    WaitOutDoubleClickInterval();
    fx.ClickCell(target, Input::kModShift);
    EXPECT_EQ(renameRequests, 0);
}

TEST(GridViewTests, SlowClickOnADifferentItemSelectsWithoutRename)
{
    GridClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";
    ASSERT_GE(fx.Cells.size(), 2u);

    int renameRequests = 0;
    fx.Grid->SetOnItemRenameRequested([&](GridId) { ++renameRequests; });

    auto it = fx.Cells.begin();
    const GridId first = it->first;
    const GridId second = (++it)->first;
    fx.ClickCell(first);
    WaitOutDoubleClickInterval();
    fx.ClickCell(second);
    EXPECT_EQ(renameRequests, 0);
}

// The item resize gesture grows items on wheel up in every view, the grid included, so one
// gesture means one thing across the Assets panel's views.
TEST(GridViewTests, ItemResizeGestureWheelUpGrowsAndWheelDownShrinksIcons)
{
    GridView grid;
    grid.SetIconSize(64.0f);
    ASSERT_FALSE(grid.GetChildren().empty());
    UIElement* scrollView = grid.GetChildren()[0].get();

    auto wheel = [scrollView](float scrollY)
    {
        UIEvent e;
        e.Id = kEventScroll;
        e.Mods = Input::kModControl;
        e.ScrollY = scrollY;
        scrollView->DispatchEvent(e);
    };

    constexpr float kWheelUpOneDetent = -30.0f;
    wheel(kWheelUpOneDetent);
    const float afterUp = grid.GetIconSize();
    EXPECT_GT(afterUp, 64.0f) << "wheel up must make grid icons bigger";

    wheel(-kWheelUpOneDetent);
    EXPECT_LT(grid.GetIconSize(), afterUp) << "wheel down must make grid icons smaller";
}

// With a size step, every icon size lands on the step grid, whether it is set directly or by the
// item resize gesture, so a host's slider drag and the wheel reach the same sizes.
TEST(GridViewTests, AnIconSizeStepSnapsSetAndWheeledSizesAlike)
{
    constexpr float kStepPx = 16.0f;
    constexpr float kWheelUpOneDetent = -30.0f;
    GridView grid;
    grid.SetIconSizeStep(kStepPx);
    ASSERT_FALSE(grid.GetChildren().empty());
    UIElement* scrollView = grid.GetChildren()[0].get();
    auto wheel = [scrollView](float scrollY)
    {
        UIEvent e;
        e.Id = kEventScroll;
        e.Mods = Input::kModControl;
        e.ScrollY = scrollY;
        scrollView->DispatchEvent(e);
    };

    grid.SetIconSize(100.0f);
    EXPECT_FLOAT_EQ(grid.GetIconSize(), 96.0f) << "a set size is not snapped to 32 + n * 16";

    grid.SetIconSize(64.0f);
    wheel(kWheelUpOneDetent);
    EXPECT_FLOAT_EQ(grid.GetIconSize(), 80.0f) << "the wheel left the step grid";
    wheel(-kWheelUpOneDetent);
    EXPECT_FLOAT_EQ(grid.GetIconSize(), 64.0f);
}

// A wheel step smaller than half the size grid would snap back to where it started; the gesture
// moves one whole step instead, so a wheel detent always changes the size.
TEST(GridViewTests, AWheelDetentAlwaysMovesOneStepOnACoarseSizeGrid)
{
    constexpr float kCoarseStepPx = 64.0f;
    GridView grid;
    grid.SetIconSizeStep(kCoarseStepPx);
    grid.SetIconSize(96.0f);
    ASSERT_FLOAT_EQ(grid.GetIconSize(), 96.0f);
    ASSERT_FALSE(grid.GetChildren().empty());

    UIEvent e;
    e.Id = kEventScroll;
    e.Mods = Input::kModControl;
    e.ScrollY = -30.0f;
    grid.GetChildren()[0]->DispatchEvent(e);
    EXPECT_FLOAT_EQ(grid.GetIconSize(), 160.0f);
}

// On a size grid the wheel is reversible: as many detents down as up return the icons to the size
// they started at, at small sizes and at large ones, where one detent spans several grid steps.
TEST(GridViewTests, WheelDownUndoesWheelUpOnTheSizeGrid)
{
    constexpr float kStepPx = 16.0f;
    constexpr int kDetents = 4;
    GridView grid;
    grid.SetIconSizeStep(kStepPx);
    ASSERT_FALSE(grid.GetChildren().empty());
    UIElement* scrollView = grid.GetChildren()[0].get();
    auto wheel = [scrollView](float scrollY)
    {
        UIEvent e;
        e.Id = kEventScroll;
        e.Mods = Input::kModControl;
        e.ScrollY = scrollY;
        scrollView->DispatchEvent(e);
    };

    for (const float start : {64.0f, 128.0f, 400.0f})
    {
        grid.SetIconSize(start);
        const float snappedStart = grid.GetIconSize();
        for (int i = 0; i < kDetents; ++i)
            wheel(-30.0f);
        ASSERT_GT(grid.GetIconSize(), snappedStart) << "wheel up did not grow the icons from " << start;
        for (int i = 0; i < kDetents; ++i)
            wheel(30.0f);
        EXPECT_FLOAT_EQ(grid.GetIconSize(), snappedStart) << "four detents up then down from " << start;
    }
}
