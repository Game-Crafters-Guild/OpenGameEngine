#include <gtest/gtest.h>

#include "Graph/GraphBlendSpace2DStore.h"

#include <limits>
#include <vector>

using namespace GameEngine;

TEST(GraphBlendSpace2DStoreTests, MissingKeyReturnsFalseAndLeavesOutUnchanged)
{
    Graph::Node host;
    std::vector<BlendSpace2DSampleDesc> out;
    BlendSpace2DSampleDesc sentinel;
    sentinel.X = 42.f;
    sentinel.Label = "keep";
    out.push_back(sentinel);

    EXPECT_FALSE(GraphBlendSpace2DStore::TryLoad(host, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].X, 42.f);
    EXPECT_EQ(out[0].Label, "keep");
}

TEST(GraphBlendSpace2DStoreTests, StoreThenLoadRoundTrips)
{
    Graph::Node host;
    const std::vector<BlendSpace2DSampleDesc> samples = {
        {0.f, 1.f, "idle", "01234567-89ab-cdef-0123-456789abcdef"},
        {1.5f, -0.5f, "run", ""},
    };
    GraphBlendSpace2DStore::Store(host, samples);

    std::vector<BlendSpace2DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace2DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_FLOAT_EQ(loaded[0].X, 0.f);
    EXPECT_FLOAT_EQ(loaded[0].Y, 1.f);
    EXPECT_EQ(loaded[0].Label, "idle");
    EXPECT_EQ(loaded[0].ClipGuid, "01234567-89ab-cdef-0123-456789abcdef");
    EXPECT_FLOAT_EQ(loaded[1].X, 1.5f);
    EXPECT_FLOAT_EQ(loaded[1].Y, -0.5f);
    EXPECT_TRUE(loaded[1].ClipGuid.empty());
}

TEST(GraphBlendSpace2DStoreTests, AxisClampedTPortsPadMapping)
{
    BlendSpace2DAxis axis;
    axis.Min = 0.f;
    axis.Max = 1.f;
    EXPECT_FLOAT_EQ(axis.ClampedT(0.f), 0.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(axis.ClampedT(1.f), 1.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(1e20f), 1.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(-1e20f), 0.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(std::numeric_limits<float>::infinity()), 0.5f);
    EXPECT_FLOAT_EQ(axis.ClampedT(std::numeric_limits<float>::quiet_NaN()), 0.5f);

    const BlendSpace2DAxis collapsed = BlendSpace2DAxis::FromSamples({{4.f, 4.f, "", ""}}, true);
    EXPECT_FLOAT_EQ(collapsed.Min, 3.f);
    EXPECT_FLOAT_EQ(collapsed.Max, 5.f);
    EXPECT_FLOAT_EQ(collapsed.ClampedT(4.f), 0.5f);
}

TEST(GraphBlendSpace2DStoreTests, StoreSkipsNonFinite)
{
    Graph::Node host;
    BlendSpace2DSampleDesc inf;
    inf.X = std::numeric_limits<float>::infinity();
    inf.Y = 0.f;
    inf.Label = "bad";
    GraphBlendSpace2DStore::Store(host, {{0.f, 0.f, "ok", ""}, inf});
    std::vector<BlendSpace2DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace2DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].Label, "ok");
}
