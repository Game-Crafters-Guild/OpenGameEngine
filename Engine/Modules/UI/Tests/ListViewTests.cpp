#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

#include "UI/Controls/ListView.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UIRgTestHarness.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"
#include "../Source/UIAttributeAccess.h"

using namespace GameEngine;

namespace {

// Simple test provider with uniform heights
class UniformHeightProvider : public ListChangeTrackingProvider {
public:
    explicit UniformHeightProvider(int count, float height = 24.0f)
        : m_Count(count), m_Height(height) {}

    int GetItemCount() const override { return m_Count; }
    ListId GetItemId(int index) const override { return static_cast<ListId>(index + 1); }
    float GetItemHeight(int /*index*/) const override { return m_Height; }

    void SetCount(int count) { m_Count = count; }

private:
    int m_Count;
    float m_Height;
};

// Test provider with variable heights
class VariableHeightProvider : public ListChangeTrackingProvider {
public:
    explicit VariableHeightProvider(std::vector<float> heights)
        : m_Heights(std::move(heights)) {}

    int GetItemCount() const override { return static_cast<int>(m_Heights.size()); }
    ListId GetItemId(int index) const override { return static_cast<ListId>(index + 1); }
    float GetItemHeight(int index) const override {
        if (index < 0 || index >= static_cast<int>(m_Heights.size())) return 0.0f;
        return m_Heights[static_cast<size_t>(index)];
    }

private:
    std::vector<float> m_Heights;
};

} // namespace

TEST(ListViewTests, ConstructionCreatesValidElement)
{
    ListView view;
    EXPECT_NE(nullptr, &view);
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    EXPECT_TRUE(reg.IsSameType(view, "listview"));
}

TEST(ListViewTests, SetSelectedIdUpdatesSelection)
{
    UniformHeightProvider provider(10);
    UI::Interaction::SelectionModel model;
    ListView view;
    
    view.SetDataProvider(&provider);
    view.SetSelectionModel(&model);
    
    view.SetSelectedId(5);
    EXPECT_EQ(5u, view.GetSelectedId());
    EXPECT_TRUE(model.IsSelected(5));
}

TEST(ListViewTests, SetSelectedIdSameValueNoChange)
{
    UniformHeightProvider provider(10);
    ListView view;
    
    view.SetDataProvider(&provider);
    view.SetSelectedId(5);
    view.SetSelectedId(5); // Same value
    
    EXPECT_EQ(5u, view.GetSelectedId());
}

TEST(ListViewTests, RefreshFromProviderClearsSelection)
{
    UniformHeightProvider provider(10);
    ListView view;
    
    view.SetDataProvider(&provider);
    view.SetSelectedId(5);
    EXPECT_EQ(5u, view.GetSelectedId());
    
    view.RefreshFromProvider();
    EXPECT_EQ(0u, view.GetSelectedId());
}

TEST(ListViewTests, GetSelectedIndexRecoversFromSelectionModelAfterRefresh)
{
    UniformHeightProvider provider(10);
    UI::Interaction::SelectionModel model;
    ListView view;

    view.SetDataProvider(&provider);
    view.SetSelectionModel(&model);

    view.SetSelectedId(8);
    ASSERT_EQ(7, view.GetSelectedIndex());

    view.RefreshFromProvider();

    EXPECT_EQ(0u, view.GetSelectedId());
    EXPECT_EQ(7, view.GetSelectedIndex());
}

TEST(ListViewTests, VariableHeightProviderWorks)
{
    std::vector<float> heights = {20.0f, 40.0f, 30.0f, 50.0f, 25.0f};
    VariableHeightProvider provider(heights);

    EXPECT_EQ(5, provider.GetItemCount());
    EXPECT_FLOAT_EQ(20.0f, provider.GetItemHeight(0));
    EXPECT_FLOAT_EQ(40.0f, provider.GetItemHeight(1));
    EXPECT_FLOAT_EQ(30.0f, provider.GetItemHeight(2));
    EXPECT_FLOAT_EQ(50.0f, provider.GetItemHeight(3));
    EXPECT_FLOAT_EQ(25.0f, provider.GetItemHeight(4));
}

// Test append-only pattern (simulating log additions)
TEST(ListViewTests, NotifyItemsAppendedPerformance)
{
    constexpr int kInitialCount = 10000;
    constexpr int kAppendCount = 100;

    UniformHeightProvider provider(kInitialCount);
    ListView view;
    view.SetDataProvider(&provider);
    view.RefreshFromProvider();

    // Simulate appending items one at a time
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kAppendCount; ++i)
    {
        provider.SetCount(kInitialCount + i + 1);
        view.NotifyItemsAppended(1);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto appendDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Compare with full refresh approach
    provider.SetCount(kInitialCount);
    view.RefreshFromProvider();

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kAppendCount; ++i)
    {
        provider.SetCount(kInitialCount + i + 1);
        view.RefreshFromProvider();
    }
    end = std::chrono::high_resolution_clock::now();
    auto refreshDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Appending " << kAppendCount << " items to " << kInitialCount << " items:" << std::endl;
    std::cout << "  NotifyItemsAppended: " << appendDuration.count() << " us" << std::endl;
    std::cout << "  RefreshFromProvider: " << refreshDuration.count() << " us" << std::endl;

    // NotifyItemsAppended should be faster than full refresh
    EXPECT_LT(appendDuration.count(), refreshDuration.count());
}

// Standard-UX keyboard paging (Win32/WPF/Qt/AppKit): PageUp/Down move by a
// viewport page, Home/End go to the extremes, Shift extends from the anchor.
// Headless there is no scroll viewport, so a page degenerates to one row —
// which is exactly what distinguishes paging from the old non-standard
// jump-to-extremes behavior these tests pin against.

namespace {

void SendListKey(ListView& view, int keyCode, int mods = 0)
{
    UIEvent e{};
    e.Id = kEventKeyDown;
    e.Key = keyCode;
    e.Mods = mods;
    e.Target = &view;
    e.CurrentTarget = &view;
    view.OnEvent(e);
}

} // namespace

TEST(ListViewTests, PageDownPagesInsteadOfJumpingToEnd)
{
    UniformHeightProvider provider(100);
    UI::Interaction::SelectionModel model;
    ListView view;
    view.SetDataProvider(&provider);
    view.SetSelectionModel(&model);
    view.SetSelectedIndex(0, /*scrollIntoView*/ false);

    SendListKey(view, Input::kKeyCode_PageDown);

    EXPECT_GT(view.GetSelectedIndex(), 0);
    EXPECT_LT(view.GetSelectedIndex(), 99) << "PageDown must page, not jump to the last item";

    SendListKey(view, Input::kKeyCode_PageUp);
    EXPECT_EQ(view.GetSelectedIndex(), 0);
}

TEST(ListViewTests, HomeEndGoToExtremes)
{
    UniformHeightProvider provider(100);
    UI::Interaction::SelectionModel model;
    ListView view;
    view.SetDataProvider(&provider);
    view.SetSelectionModel(&model);
    view.SetSelectedIndex(50, /*scrollIntoView*/ false);

    SendListKey(view, Input::kKeyCode_End);
    EXPECT_EQ(view.GetSelectedIndex(), 99);

    SendListKey(view, Input::kKeyCode_Home);
    EXPECT_EQ(view.GetSelectedIndex(), 0);
}

// The scroll view clamps a scroll against the content height it was last told. Rows appended
// since the last virtualization pass are not in that height yet, so a scroll toward them has to
// publish the new height first or it clamps to the old range and the new rows stay out of view.
TEST(ListViewTests, ScrollIndexIntoViewReachesRowsAppendedSinceLastPublish)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    auto list = std::make_unique<ListView>();
    ListView* listRaw = list.get();
    listRaw->Overrides()
        .Set(Style::Width, StyleLength::Px(200.0f))
        .Set(Style::Height, StyleLength::Px(100.0f));
    root->AddChild(std::move(list));

    constexpr float kRowHeight = 24.0f;
    UniformHeightProvider provider(10, kRowHeight);
    listRaw->SetDataProvider(&provider);

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    for (int i = 0; i < 5; ++i)
    {
        ui.Update(0.0f, /*interactive=*/false);
        DriveUiRender(ui, rg);
    }
    ASSERT_GT(listRaw->GetViewportHeight(), 0.0f) << "layout must have given the list a viewport";

    // Append and scroll in the same tick, before virtualization has republished the height.
    provider.SetCount(100);
    listRaw->NotifyItemsAppended(90);
    listRaw->ScrollIndexIntoView(99);

    // Row 99 spans 2376..2400 px; a 100 px viewport shows it from 2300.
    EXPECT_FLOAT_EQ(listRaw->GetScrollOffset(), 100.0f * kRowHeight - listRaw->GetViewportHeight())
        << "the scroll clamped against the content height from before the append";
}

TEST(ListViewTests, ShiftPagingExtendsSelectionFromAnchor)
{
    UniformHeightProvider provider(100);
    UI::Interaction::SelectionModel model;
    ListView view;
    view.SetDataProvider(&provider);
    view.SetSelectionModel(&model);
    view.SetSelectedIndex(10, /*scrollIntoView*/ false);

    SendListKey(view, Input::kKeyCode_PageDown, Input::kModShift);

    const int cursor = view.GetSelectedIndex();
    EXPECT_GT(cursor, 10);
    for (int i = 10; i <= cursor; ++i)
        EXPECT_TRUE(model.IsSelected(provider.GetItemId(i))) << "row " << i << " missing from range";

    // Shift+End extends the range all the way from the same fixed anchor.
    SendListKey(view, Input::kKeyCode_End, Input::kModShift);
    EXPECT_EQ(view.GetSelectedIndex(), 99);
    EXPECT_TRUE(model.IsSelected(provider.GetItemId(10)));
    EXPECT_TRUE(model.IsSelected(provider.GetItemId(99)));
    EXPECT_FALSE(model.IsSelected(provider.GetItemId(9))) << "range must start at the anchor";
}

// Drop hit-testing must only consider live bound rows: unbound pooled slots
// keep whatever layout rect they last had, and a point inside such a stale
// rect must resolve as empty space, never as a row. Regresses the
// HitTestDropTarget guard cluster shared with GetCellForIndex/GetIndexAtPoint.
TEST(ListViewTests, DropHitTestSkipsStaleUnboundSlots)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto list = std::make_unique<ListView>();
    ListView* listRaw = list.get();
    listRaw->Overrides()
        .Set(Style::Width, StyleLength::Px(200.0f))
        .Set(Style::Height, StyleLength::Px(100.0f));
    root->AddChild(std::move(list));

    constexpr float kRowHeight = 24.0f;
    UniformHeightProvider provider(20, kRowHeight);
    listRaw->SetDataProvider(&provider);

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
    ASSERT_GT(listRaw->GetVisibleCount(), 0) << "virtualization must have bound rows";

    const UI::Interaction::IDropTarget& drop = *listRaw;

    // A bound row resolves to its item id.
    UIElement* row1 = listRaw->GetCellForIndex(1);
    ASSERT_NE(row1, nullptr);
    const float rowX = row1->GetLayoutX() + 4.0f;
    UI::Interaction::DropHit hit{};
    ASSERT_TRUE(drop.HitTestDropTarget(rowX, row1->GetLayoutY() + row1->GetLayoutHeight() * 0.5f, hit));
    EXPECT_EQ(hit.TargetId, provider.GetItemId(1));
    EXPECT_EQ(hit.Location, UI::Interaction::DropLocation::OnItem);

    // Shrink to 3 rows: slots previously bound to rows 3+ unbind but stay
    // pooled with whatever rect they last had. Probe inside the old row-3 area
    // (below the last live row but still inside the list): the stale slot must
    // not produce a row hit — the empty-space fallback wins.
    provider.SetCount(3);
    listRaw->RefreshFromProvider();
    pump();
    EXPECT_EQ(listRaw->GetCellForIndex(3), nullptr) << "row 3 must be unbound after shrink";

    const float staleY = listRaw->GetLayoutY() + 3.0f * kRowHeight + kRowHeight * 0.5f;
    ASSERT_LT(staleY, listRaw->GetLayoutY() + listRaw->GetLayoutHeight())
        << "probe point must stay inside the list rect";
    UI::Interaction::DropHit staleHit{};
    ASSERT_TRUE(drop.HitTestDropTarget(rowX, staleY, staleHit))
        << "inside the ListView rect the empty-space fallback must hit";
    EXPECT_EQ(staleHit.TargetId, 0u) << "stale unbound slot must not produce a row hit";
    EXPECT_EQ(staleHit.Location, UI::Interaction::DropLocation::OnEmptySpace);
}

// A picker popup (SearchDialog's result list) exists only to choose one row, so
// the first click must commit. Browsing lists keep double-click activation.
// Regresses the asset/material picker, where a single click highlighted a row
// and never assigned it.
namespace {

struct ListClickFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    GameEngine::Rendering::IDevice* Device = nullptr;
    std::unique_ptr<UiRgHarness> Rg;
    std::unique_ptr<UIManager> Ui;
    ListView* List = nullptr;
    UniformHeightProvider Provider{20, 24.0f};

    bool Build()
    {
        Device = SharedHeadlessDevice();
        if (!Device)
            return false;
        Rg = std::make_unique<UiRgHarness>(Device);
        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto list = std::make_unique<ListView>();
        List = list.get();
        List->Overrides()
            .Set(Style::Width, StyleLength::Px(200.0f))
            .Set(Style::Height, StyleLength::Px(100.0f));
        root->AddChild(std::move(list));
        List->SetDataProvider(&Provider);

        Ui = std::make_unique<UIManager>(Device);
        Ui->SetRoot(std::move(root));
        Pump();
        return List->GetVisibleCount() > 0;
    }

    void Pump()
    {
        for (int i = 0; i < 5; ++i)
        {
            Ui->Update(0.0f, /*interactive=*/false);
            DriveUiRender(*Ui, *Rg);
        }
    }

    // A full press/release pair on the row: ListView selects and detects the
    // double-click on the press, and honours a rename request on the release.
    void ClickRow(int index, int mods = 0)
    {
        DispatchMouse(index, kEventMouseDown, mods);
        DispatchMouse(index, kEventMouseUp, mods);
    }

    void DispatchMouse(int index, EventId eventId, int mods)
    {
        UIElement* cell = List->GetCellForIndex(index);
        ASSERT_NE(cell, nullptr) << "row " << index << " must be bound";
        UIEvent e{};
        e.Id = eventId;
        e.Button = 0;
        e.Mods = mods;
        e.X = cell->GetLayoutX() + 4.0f;
        e.Y = cell->GetLayoutY() + cell->GetLayoutHeight() * 0.5f;
        e.Target = cell;
        e.CurrentTarget = cell;
        cell->DispatchEvent(e);
    }
};

// Past the platform double-click interval, so the next click on the same row is
// a slow repeat rather than a double-click.
void WaitOutDoubleClickInterval()
{
    std::this_thread::sleep_for(Platform::GetDoubleClickInterval() + std::chrono::milliseconds(50));
}

} // namespace

TEST(ListViewTests, PickerActivationCommitsOnFirstClick)
{
    ListClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int activated = 0;
    ListId activatedId = 0;
    fx.List->SetOnItemActivated([&](ListId id) { ++activated; activatedId = id; });
    fx.List->SetPickerActivation(true);

    fx.ClickRow(1);
    EXPECT_EQ(activated, 1) << "a picker row must commit on the first click";
    EXPECT_EQ(activatedId, fx.Provider.GetItemId(1));
}

// Mutation guard: without picker activation a single click must only select.
// Flipping the production condition to always-activate fails this test.
TEST(ListViewTests, BrowseListDoesNotActivateOnFirstClick)
{
    ListClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int activated = 0;
    fx.List->SetOnItemActivated([&](ListId) { ++activated; });

    fx.ClickRow(1);
    EXPECT_EQ(activated, 0) << "a browsing list must not activate on a single click";

    // The second click inside the double-click interval still activates.
    fx.ClickRow(1);
    EXPECT_EQ(activated, 1) << "a browsing list must still activate on double click";
}

TEST(ListViewTests, SlowSecondClickOnTheSoleSelectionRequestsRename)
{
    ListClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int renameRequests = 0;
    ListId renameTarget = 0;
    int activations = 0;
    fx.List->SetOnItemRenameRequested([&](ListId id) { ++renameRequests; renameTarget = id; });
    fx.List->SetOnItemActivated([&](ListId) { ++activations; });

    fx.ClickRow(1);
    EXPECT_EQ(renameRequests, 0) << "the selecting click is not a rename request";

    WaitOutDoubleClickInterval();
    fx.ClickRow(1);
    EXPECT_EQ(renameRequests, 1);
    EXPECT_EQ(renameTarget, fx.Provider.GetItemId(1));
    EXPECT_EQ(activations, 0);
}

TEST(ListViewTests, DoubleClickActivatesAndNeverRequestsRename)
{
    ListClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int renameRequests = 0;
    int activations = 0;
    fx.List->SetOnItemRenameRequested([&](ListId) { ++renameRequests; });
    fx.List->SetOnItemActivated([&](ListId) { ++activations; });

    fx.ClickRow(1);
    fx.ClickRow(1);
    EXPECT_EQ(activations, 1);
    EXPECT_EQ(renameRequests, 0);
}

TEST(ListViewTests, ModifiedRepeatClickIsASelectionEditNotARename)
{
    ListClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int renameRequests = 0;
    fx.List->SetOnItemRenameRequested([&](ListId) { ++renameRequests; });

    fx.ClickRow(1);
    WaitOutDoubleClickInterval();
    fx.ClickRow(1, Input::kModShift);
    EXPECT_EQ(renameRequests, 0);
}

TEST(ListViewTests, SlowClickOnADifferentRowSelectsWithoutRename)
{
    ListClickFixture fx;
    if (!fx.Build())
        GTEST_SKIP() << "No device";

    int renameRequests = 0;
    fx.List->SetOnItemRenameRequested([&](ListId) { ++renameRequests; });

    fx.ClickRow(0);
    WaitOutDoubleClickInterval();
    fx.ClickRow(1);
    EXPECT_EQ(renameRequests, 0);
}
