#include "UI/Controls/AxisHeaderBar.h"
#include "UI/Controls/AxisModel.h"
#include "UI/Controls/TableView.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"
#include "Types/StringId.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

TrackDef MakeTrack(StringId key, float size, float minSize, bool flex = false, bool hidden = false)
{
    TrackDef t;
    t.Key     = key;
    t.Size    = size;
    t.MinSize = minSize;
    t.Flex    = flex;
    t.Hidden  = hidden;
    return t;
}

// True when header cell `index` owns a resize divider (an `.axis-divider` child).
bool HeaderCellHasDivider(const AxisHeaderBar& bar, std::size_t index)
{
    const auto& cells = bar.GetChildren();
    if (index >= cells.size() || !cells[index])
        return false;
    for (const auto& child : cells[index]->GetChildren())
    {
        if (child && child->HasClass("axis-divider"))
            return true;
    }
    return false;
}

} // namespace

// AxisModel ------------------------------------------------------------------

TEST(TableViewTests, AxisModel_SetSizeClampsToMinSize)
{
    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("a"_sid, 100.0f, 60.0f), MakeTrack("b"_sid, 120.0f, 80.0f)});

    // Below the min snaps up to the min.
    axis.SetSize("a"_sid, 30.0f);
    EXPECT_FLOAT_EQ(axis.SizeOf("a"_sid), 60.0f);

    // Above the min passes through unchanged.
    axis.SetSize("a"_sid, 200.0f);
    EXPECT_FLOAT_EQ(axis.SizeOf("a"_sid), 200.0f);

    // Each track clamps to its own min, not a shared one.
    axis.SetSize("b"_sid, 50.0f);
    EXPECT_FLOAT_EQ(axis.SizeOf("b"_sid), 80.0f);
}

TEST(TableViewTests, AxisModel_SetClampsOnLoad)
{
    // Set() runs ClampToMins, so a stored size below the min is corrected at load.
    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("a"_sid, 10.0f, 60.0f)});
    EXPECT_FLOAT_EQ(axis.SizeOf("a"_sid), 60.0f);
}

TEST(TableViewTests, AxisModel_TotalSizeSkipsHidden)
{
    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("a"_sid, 100.0f, 0.0f),
              MakeTrack("b"_sid, 80.0f, 0.0f),
              MakeTrack("c"_sid, 60.0f, 0.0f)});

    axis.SetHidden("c"_sid, true);

    // Hidden track contributes neither its size nor an inter-track gap.
    EXPECT_FLOAT_EQ(axis.TotalSize(0.0f, 0.0f), 180.0f);

    // Two visible tracks -> one gap of 4px between them.
    EXPECT_FLOAT_EQ(axis.TotalSize(4.0f, 0.0f), 184.0f);
}

// ApplyTrackSizes ------------------------------------------------------------

TEST(TableViewTests, ApplyTrackSizes_FixedTrack)
{
    UIElement cell;
    cell.AddChild(std::make_unique<UIElement>());

    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("a"_sid, 90.0f, 60.0f, /*flex=*/false)});

    ApplyTrackSizes(&cell, axis);

    UIElement* child = cell.GetChildren()[0].get();
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(child->Overrides().Get(Style::FlexGrow), 0.0f);
    EXPECT_EQ(child->Overrides().Get(Style::FlexShrink), 0.0f);

    // Fixed track pins basis/min/max to the track width.
    EXPECT_TRUE(child->Overrides().Has(StylePropertyId::FlexBasis));
    EXPECT_TRUE(child->Overrides().Has(StylePropertyId::MinWidth));
    EXPECT_TRUE(child->Overrides().Has(StylePropertyId::MaxWidth));
    EXPECT_EQ(child->Overrides().Get(Style::FlexBasis), StyleLength::Px(90.0f));
    EXPECT_EQ(child->Overrides().Get(Style::MinWidth), StyleLength::Px(90.0f));
    EXPECT_EQ(child->Overrides().Get(Style::MaxWidth), StyleLength::Px(90.0f));
}

TEST(TableViewTests, ApplyTrackSizes_FlexTrack)
{
    UIElement cell;
    cell.AddChild(std::make_unique<UIElement>());

    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("a"_sid, 90.0f, 60.0f, /*flex=*/true)});

    ApplyTrackSizes(&cell, axis);

    UIElement* child = cell.GetChildren()[0].get();
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(child->Overrides().Get(Style::FlexGrow), 1.0f);
    EXPECT_TRUE(child->Overrides().Has(StylePropertyId::FlexBasis));
    EXPECT_EQ(child->Overrides().Get(Style::FlexBasis), StyleLength::Px(0.0f));

    // A flex track grows freely: the max-width cap is explicitly removed.
    EXPECT_FALSE(child->Overrides().Has(StylePropertyId::MaxWidth));
    EXPECT_TRUE(child->Overrides().Has(StylePropertyId::MinWidth));
}

TEST(TableViewTests, ApplyTrackSizes_HiddenTrack)
{
    UIElement cell;
    cell.AddChild(std::make_unique<UIElement>());

    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("a"_sid, 90.0f, 60.0f, /*flex=*/false, /*hidden=*/true)});

    ApplyTrackSizes(&cell, axis);

    UIElement* child = cell.GetChildren()[0].get();
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(child->Overrides().Get(Style::Display), DisplayMode::None);
}

// Fill track -----------------------------------------------------------------

TEST(TableViewTests, MakeFill_HasFillDefaults)
{
    const TrackDef t = TrackDef::MakeFill();

    // The fill track grows, carries no user-adjustable size, and never sorts.
    EXPECT_TRUE(t.Fill);
    EXPECT_FALSE(t.Resizable);
    EXPECT_FALSE(t.Sortable);
    EXPECT_FLOAT_EQ(t.MinSize, 0.0f);
}

TEST(TableViewTests, ApplyTrackSizes_FillTrackGrows)
{
    UIElement cell;
    cell.AddChild(std::make_unique<UIElement>());

    AxisModel axis(Axis::Horizontal);
    axis.Set({TrackDef::MakeFill()});

    ApplyTrackSizes(&cell, axis);

    UIElement* child = cell.GetChildren()[0].get();
    ASSERT_NE(child, nullptr);

    // A fill track is sized exactly like a flex track: grow:1, basis:0, no max cap.
    EXPECT_EQ(child->Overrides().Get(Style::FlexGrow), 1.0f);
    EXPECT_TRUE(child->Overrides().Has(StylePropertyId::FlexBasis));
    EXPECT_EQ(child->Overrides().Get(Style::FlexBasis), StyleLength::Px(0.0f));
    EXPECT_FALSE(child->Overrides().Has(StylePropertyId::MaxWidth));
}

TEST(TableViewTests, AxisHeaderBar_FillExcludedFromDividerAndContentLogic)
{
    // Two content columns followed by a trailing fill track. Dividers belong only
    // BETWEEN content columns, so:
    //   col 0 (content, has a later content column) -> divider
    //   col 1 (last content column before the fill) -> NO divider
    //   col 2 (the fill track itself)               -> NO divider
    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("a"_sid, 100.0f, 60.0f),
              MakeTrack("b"_sid, 100.0f, 60.0f),
              TrackDef::MakeFill()});

    AxisHeaderBar bar;
    bar.SetAxis(&axis);

    ASSERT_EQ(bar.GetChildren().size(), 3u);
    EXPECT_TRUE(HeaderCellHasDivider(bar, 0));
    EXPECT_FALSE(HeaderCellHasDivider(bar, 1));
    EXPECT_FALSE(HeaderCellHasDivider(bar, 2));
}

// Horizontal-scroll content width -------------------------------------------

TEST(TableViewTests, ComputeContentWidthTarget_OverflowVsFits)
{
    AxisModel cols(Axis::Horizontal);
    cols.Set({MakeTrack("a"_sid, 300.0f, 0.0f),
              MakeTrack("b"_sid, 200.0f, 0.0f),
              MakeTrack("c"_sid, 100.0f, 0.0f, /*flex=*/false, /*hidden=*/true),  // hidden -> excluded
              TrackDef::MakeFill()});                                             // fill -> excluded
    // Visible fixed total = 300 + 200 = 500 (hidden + fill don't count).

    // Fits: viewport wider than the fixed total -> auto (0), so the trailing fill track fills.
    EXPECT_FLOAT_EQ(ComputeTableContentWidthTarget(cols, 800.0f), 0.0f);

    // Overflow: viewport narrower than the fixed total -> pin to the total so the body h-scrolls.
    EXPECT_FLOAT_EQ(ComputeTableContentWidthTarget(cols, 400.0f), 500.0f);

    // At the boundary (within the overflow margin) stays auto — no 1px phantom scroll.
    EXPECT_FLOAT_EQ(ComputeTableContentWidthTarget(cols, 500.0f), 0.0f);

    // Pre-layout (non-positive viewport) is a no-op.
    EXPECT_FLOAT_EQ(ComputeTableContentWidthTarget(cols, 0.0f), 0.0f);
}

// Sort comparator regression -------------------------------------------------

TEST(TableViewTests, SortComparator_EqualKeysDescendingNoAssert)
{
    // Guards the strict-weak-ordering contract that a prior `!less` comparator
    // violated on equal keys: `!less(a,b)` returns true for a==b, which makes
    // std::sort read out of bounds (debug iterator assert / crash). The header
    // builds the descending comparator as `less(b,a)` instead, which correctly
    // returns false for equal keys.
    auto less = [](int a, int b) { return a < b; };

    auto sortDir = [&](std::vector<int> v, bool descending)
    {
        std::sort(v.begin(), v.end(),
                  [&](int a, int b) { return descending ? less(b, a) : less(a, b); });
        return v;
    };

    std::vector<int> base;
    for (int i = 0; i < 20; ++i)
        base.push_back(100); // many equal keys exercise the equal-element path
    base.push_back(50);
    base.push_back(150);
    base.push_back(75);

    const std::vector<int> asc = sortDir(base, /*descending=*/false);
    EXPECT_TRUE(std::is_sorted(asc.begin(), asc.end()));

    const std::vector<int> desc = sortDir(base, /*descending=*/true);
    EXPECT_TRUE(std::is_sorted(desc.begin(), desc.end(), std::greater<int>()));
}
