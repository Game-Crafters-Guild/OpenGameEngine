// Cell bookkeeping for the material-graph node preview atlas: stable assignment
// across graph edits, page growth, recycling, and the CSS sprite addressing a
// node thumb uses to show its cell.

#include "ShaderGraph/GraphPreviewAtlasLayout.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using GameEngine::Editor::GraphPreviewAtlasLayout;

namespace
{

std::vector<std::string> Keys(size_t count, const char* prefix = "n")
{
    std::vector<std::string> keys;
    keys.reserve(count);
    for (size_t i = 0; i < count; ++i)
        keys.push_back(std::string(prefix) + std::to_string(i));
    return keys;
}

uint32_t CellOf(const GraphPreviewAtlasLayout& layout, const std::string& key)
{
    uint32_t cell = 0;
    EXPECT_TRUE(layout.TryGetCell(key, cell)) << key;
    return cell;
}

} // namespace

TEST(GraphPreviewAtlasLayoutTests, AssignsDistinctCellsAndReportsChange)
{
    GraphPreviewAtlasLayout layout;
    EXPECT_TRUE(layout.Reconcile(Keys(3)));

    const uint32_t a = CellOf(layout, "n0");
    const uint32_t b = CellOf(layout, "n1");
    const uint32_t c = CellOf(layout, "n2");
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);

    uint32_t missing = 0;
    EXPECT_FALSE(layout.TryGetCell("absent", missing));
}

TEST(GraphPreviewAtlasLayoutTests, ReconcilingTheSameSetIsANoOp)
{
    GraphPreviewAtlasLayout layout;
    const std::vector<std::string> keys = Keys(5);
    EXPECT_TRUE(layout.Reconcile(keys));
    const uint32_t before = CellOf(layout, "n3");

    EXPECT_FALSE(layout.Reconcile(keys));
    EXPECT_EQ(CellOf(layout, "n3"), before);
}

TEST(GraphPreviewAtlasLayoutTests, SurvivingKeysKeepTheirCellWhenOthersLeave)
{
    GraphPreviewAtlasLayout layout;
    layout.Reconcile(Keys(4));
    const uint32_t keep = CellOf(layout, "n2");

    // n1 leaves; n2 must not be renumbered under it.
    EXPECT_TRUE(layout.Reconcile({"n0", "n2", "n3"}));
    EXPECT_EQ(CellOf(layout, "n2"), keep);
}

TEST(GraphPreviewAtlasLayoutTests, ReleasedCellsAreRecycledBeforeNewOnesAreMinted)
{
    GraphPreviewAtlasLayout layout;
    layout.Reconcile(Keys(4));
    const uint32_t freed = CellOf(layout, "n1");

    layout.Reconcile({"n0", "n2", "n3"});
    layout.Reconcile({"n0", "n2", "n3", "fresh"});
    EXPECT_EQ(CellOf(layout, "fresh"), freed);
}

TEST(GraphPreviewAtlasLayoutTests, GrowsByWholePages)
{
    GraphPreviewAtlasLayout layout;
    const uint32_t perPage = GraphPreviewAtlasLayout::kColumns * GraphPreviewAtlasLayout::kRowsPerPage;

    layout.Reconcile(Keys(1));
    EXPECT_EQ(layout.RowCount(), GraphPreviewAtlasLayout::kRowsPerPage);
    EXPECT_EQ(layout.Capacity(), perPage);

    layout.Reconcile(Keys(perPage));
    EXPECT_EQ(layout.RowCount(), GraphPreviewAtlasLayout::kRowsPerPage);

    layout.Reconcile(Keys(perPage + 1));
    EXPECT_EQ(layout.RowCount(), 2u * GraphPreviewAtlasLayout::kRowsPerPage);
    EXPECT_GE(layout.Capacity(), perPage + 1);
}

TEST(GraphPreviewAtlasLayoutTests, EmptyLayoutHasNoRows)
{
    GraphPreviewAtlasLayout layout;
    EXPECT_EQ(layout.RowCount(), 0u);
    EXPECT_EQ(layout.WidthPx(), GraphPreviewAtlasLayout::kColumns * GraphPreviewAtlasLayout::kCellPx);
    EXPECT_EQ(layout.HeightPx(), 0u);
}

TEST(GraphPreviewAtlasLayoutTests, CellOriginWalksTheGridRowMajor)
{
    GraphPreviewAtlasLayout layout;
    layout.Reconcile(Keys(GraphPreviewAtlasLayout::kColumns + 1));

    uint32_t x = 0;
    uint32_t y = 0;
    layout.CellOrigin(0, x, y);
    EXPECT_EQ(x, 0u);
    EXPECT_EQ(y, 0u);

    layout.CellOrigin(1, x, y);
    EXPECT_EQ(x, GraphPreviewAtlasLayout::kCellPx);
    EXPECT_EQ(y, 0u);

    layout.CellOrigin(GraphPreviewAtlasLayout::kColumns, x, y);
    EXPECT_EQ(x, 0u);
    EXPECT_EQ(y, GraphPreviewAtlasLayout::kCellPx);
}

TEST(GraphPreviewAtlasLayoutTests, BackgroundRectScalesByGridAndWalksCorners)
{
    // 4x4 grid: one cell fills the element at 400% and the position sweeps
    // 0..100% across the three gaps between the four cell origins.
    const auto topLeft = GraphPreviewAtlasLayout::BackgroundRect(0, 4, 4);
    EXPECT_FLOAT_EQ(topLeft.SizeXPercent, 400.f);
    EXPECT_FLOAT_EQ(topLeft.SizeYPercent, 400.f);
    EXPECT_FLOAT_EQ(topLeft.PosXPercent, 0.f);
    EXPECT_FLOAT_EQ(topLeft.PosYPercent, 0.f);

    const auto bottomRight = GraphPreviewAtlasLayout::BackgroundRect(15, 4, 4);
    EXPECT_FLOAT_EQ(bottomRight.PosXPercent, 100.f);
    EXPECT_FLOAT_EQ(bottomRight.PosYPercent, 100.f);

    const auto secondColumn = GraphPreviewAtlasLayout::BackgroundRect(1, 4, 4);
    EXPECT_FLOAT_EQ(secondColumn.PosXPercent, 100.f / 3.f);
    EXPECT_FLOAT_EQ(secondColumn.PosYPercent, 0.f);

    const auto secondRow = GraphPreviewAtlasLayout::BackgroundRect(4, 4, 4);
    EXPECT_FLOAT_EQ(secondRow.PosXPercent, 0.f);
    EXPECT_FLOAT_EQ(secondRow.PosYPercent, 100.f / 3.f);
}

TEST(GraphPreviewAtlasLayoutTests, BackgroundRectHandlesSingleCellAxes)
{
    // A one-row grid has no vertical travel: the position must collapse to 0
    // rather than divide by (rows - 1).
    const auto oneRow = GraphPreviewAtlasLayout::BackgroundRect(2, 4, 1);
    EXPECT_FLOAT_EQ(oneRow.SizeYPercent, 100.f);
    EXPECT_FLOAT_EQ(oneRow.PosYPercent, 0.f);
    EXPECT_FLOAT_EQ(oneRow.PosXPercent, (2.f / 3.f) * 100.f);

    const auto degenerate = GraphPreviewAtlasLayout::BackgroundRect(0, 0, 0);
    EXPECT_FLOAT_EQ(degenerate.SizeXPercent, 100.f);
    EXPECT_FLOAT_EQ(degenerate.PosXPercent, 0.f);
}

TEST(GraphPreviewAtlasLayoutTests, LiveLayoutRectMatchesTheStaticRuleForItsGrid)
{
    GraphPreviewAtlasLayout layout;
    layout.Reconcile(Keys(9));
    const uint32_t cell = CellOf(layout, "n5");
    const auto expected =
        GraphPreviewAtlasLayout::BackgroundRect(cell, GraphPreviewAtlasLayout::kColumns,
                                                layout.RowCount());
    EXPECT_EQ(layout.BackgroundRectForCell(cell), expected);
}
