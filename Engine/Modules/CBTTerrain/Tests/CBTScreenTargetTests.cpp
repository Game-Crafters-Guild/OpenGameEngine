#include "CBTTerrain/CBTScreenTarget.h"

#include <gtest/gtest.h>

using GameEngine::CBTTerrain::SplitThresholdPixels;

// The target is authored against 1080 rows: the split threshold scales with the render height, so a
// view draws the same triangle count at any resolution, with triangles proportionally larger in
// pixels on a taller one.
TEST(CBTScreenTarget, SplitThresholdIsTheTargetAt1080Rows)
{
    EXPECT_FLOAT_EQ(SplitThresholdPixels(11.0f, 1080u), 11.0f);
    EXPECT_FLOAT_EQ(SplitThresholdPixels(8.0f, 1080u), 8.0f);
}

TEST(CBTScreenTarget, SplitThresholdScalesWithRenderHeight)
{
    EXPECT_FLOAT_EQ(SplitThresholdPixels(11.0f, 2160u), 22.0f); // 4K: twice the pixels per edge
    EXPECT_FLOAT_EQ(SplitThresholdPixels(11.0f, 720u), 7.333333f);
    EXPECT_FLOAT_EQ(SplitThresholdPixels(11.0f, 900u), 9.166667f); // the 3067 x 900 editor viewport
}

TEST(CBTScreenTarget, ZeroHeightKeepsTheTarget)
{
    // A view with no render size yet (first frame of a new viewport) must not collapse the
    // threshold to 0, which would split every bisector.
    EXPECT_FLOAT_EQ(SplitThresholdPixels(11.0f, 0u), 11.0f);
}
