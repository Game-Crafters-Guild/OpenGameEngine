#include <gtest/gtest.h>

#include "Graph/GraphBlendSpace1DStore.h"

#include <limits>
#include <string>
#include <vector>

using namespace GameEngine;

TEST(GraphBlendSpace1DStoreTests, MissingKeyReturnsFalseAndLeavesOutUnchanged)
{
    Graph::Node host;
    std::vector<BlendSpace1DSampleDesc> out;
    BlendSpace1DSampleDesc sentinel;
    sentinel.Position = 42.f;
    sentinel.Label = "keep";
    out.push_back(sentinel);

    EXPECT_FALSE(GraphBlendSpace1DStore::TryLoad(host, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].Position, 42.f);
    EXPECT_EQ(out[0].Label, "keep");
}

TEST(GraphBlendSpace1DStoreTests, StoreThenTryLoadRoundTripsPositionsAndLabels)
{
    Graph::Node host;
    const std::vector<BlendSpace1DSampleDesc> samples = {
        {0.25f, "idle"},
        {1.5f, "walk"},
        {5.0f, ""},
    };
    GraphBlendSpace1DStore::Store(host, samples);

    std::vector<BlendSpace1DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_FLOAT_EQ(loaded[0].Position, 0.25f);
    EXPECT_EQ(loaded[0].Label, "idle");
    EXPECT_TRUE(loaded[0].ClipGuid.empty());
    EXPECT_FLOAT_EQ(loaded[1].Position, 1.5f);
    EXPECT_EQ(loaded[1].Label, "walk");
    EXPECT_FLOAT_EQ(loaded[2].Position, 5.0f);
    EXPECT_TRUE(loaded[2].Label.empty());
}

TEST(GraphBlendSpace1DStoreTests, StoreThenTryLoadRoundTripsClipGuid)
{
    Graph::Node host;
    const std::vector<BlendSpace1DSampleDesc> samples = {
        {0.0f, "idle", "01234567-89ab-cdef-0123-456789abcdef"},
        {1.5f, "run", ""},
    };
    GraphBlendSpace1DStore::Store(host, samples);

    std::vector<BlendSpace1DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].ClipGuid, "01234567-89ab-cdef-0123-456789abcdef");
    EXPECT_TRUE(loaded[1].ClipGuid.empty());

    const auto it = host.Extensions.find(GraphBlendSpace1DStore::kExtensionKey);
    ASSERT_NE(it, host.Extensions.end());
    const Graph::GraphObject* bag = it->second.TryObject();
    ASSERT_NE(bag, nullptr);
    const auto samplesIt = bag->find("samples");
    ASSERT_NE(samplesIt, bag->end());
    const std::vector<Graph::GraphValue>* list = samplesIt->second.TryList();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->size(), 2u);
    const Graph::GraphObject* first = (*list)[0].TryObject();
    const Graph::GraphObject* second = (*list)[1].TryObject();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(first->contains("clipGuid"));
    EXPECT_FALSE(second->contains("clipGuid"));
}

TEST(GraphBlendSpace1DStoreTests, StoreSortsByPosition)
{
    Graph::Node host;
    const std::vector<BlendSpace1DSampleDesc> samples = {
        {2.f, "b"},
        {0.f, "a"},
        {1.f, "c"},
    };
    GraphBlendSpace1DStore::Store(host, samples);

    std::vector<BlendSpace1DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_FLOAT_EQ(loaded[0].Position, 0.f);
    EXPECT_EQ(loaded[0].Label, "a");
    EXPECT_FLOAT_EQ(loaded[1].Position, 1.f);
    EXPECT_EQ(loaded[1].Label, "c");
    EXPECT_FLOAT_EQ(loaded[2].Position, 2.f);
    EXPECT_EQ(loaded[2].Label, "b");
}

TEST(GraphBlendSpace1DStoreTests, EmptySamplesListRoundTrips)
{
    Graph::Node host;
    GraphBlendSpace1DStore::Store(host, {});
    std::vector<BlendSpace1DSampleDesc> loaded;
    BlendSpace1DSampleDesc sentinel;
    sentinel.Position = 9.f;
    loaded.push_back(sentinel);
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(host, loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST(GraphBlendSpace1DStoreTests, GarbageKeyTypesLeaveOutUnchanged)
{
    const auto expectUnchanged = [](Graph::Node& host)
    {
        std::vector<BlendSpace1DSampleDesc> out;
        BlendSpace1DSampleDesc sentinel;
        sentinel.Position = 7.f;
        sentinel.Label = "sent";
        out.push_back(sentinel);
        EXPECT_FALSE(GraphBlendSpace1DStore::TryLoad(host, out));
        ASSERT_EQ(out.size(), 1u);
        EXPECT_FLOAT_EQ(out[0].Position, 7.f);
        EXPECT_EQ(out[0].Label, "sent");
    };

    Graph::Node asString;
    asString.Extensions[GraphBlendSpace1DStore::kExtensionKey] = std::string("nope");
    expectUnchanged(asString);

    Graph::Node asInt;
    asInt.Extensions[GraphBlendSpace1DStore::kExtensionKey] = 3;
    expectUnchanged(asInt);

    Graph::Node asList;
    asList.Extensions[GraphBlendSpace1DStore::kExtensionKey] =
        Graph::GraphValue(std::vector<Graph::GraphValue>{});
    expectUnchanged(asList);
}

TEST(GraphBlendSpace1DStoreTests, ObjectMissingOrWrongSamplesFailsClosed)
{
    Graph::Node missing;
    Graph::GraphObject bag;
    bag["other"] = 1;
    missing.Extensions[GraphBlendSpace1DStore::kExtensionKey] = Graph::GraphValue(std::move(bag));
    std::vector<BlendSpace1DSampleDesc> out;
    BlendSpace1DSampleDesc sentinel;
    sentinel.Position = 1.f;
    out.push_back(sentinel);
    EXPECT_FALSE(GraphBlendSpace1DStore::TryLoad(missing, out));
    ASSERT_EQ(out.size(), 1u);

    Graph::Node wrongType;
    Graph::GraphObject bag2;
    bag2["samples"] = std::string("nope");
    wrongType.Extensions[GraphBlendSpace1DStore::kExtensionKey] = Graph::GraphValue(std::move(bag2));
    EXPECT_FALSE(GraphBlendSpace1DStore::TryLoad(wrongType, out));
    ASSERT_EQ(out.size(), 1u);
}

TEST(GraphBlendSpace1DStoreTests, MixedListSkipsInvalidEntries)
{
    Graph::GraphObject good;
    good["position"] = 1.5f;
    good["label"] = "walk";

    Graph::GraphObject stringPos;
    stringPos["position"] = "fast";

    Graph::GraphObject nanPos;
    nanPos["position"] = std::numeric_limits<float>::quiet_NaN();

    std::vector<Graph::GraphValue> list;
    list.emplace_back(Graph::GraphValue(std::move(good)));
    list.emplace_back(std::string("nope"));
    list.emplace_back(Graph::GraphValue(std::move(stringPos)));
    list.emplace_back(Graph::GraphValue(std::move(nanPos)));

    Graph::GraphObject bag;
    bag["samples"] = Graph::GraphValue(std::move(list));
    Graph::Node host;
    host.Extensions[GraphBlendSpace1DStore::kExtensionKey] = Graph::GraphValue(std::move(bag));

    std::vector<BlendSpace1DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_FLOAT_EQ(loaded[0].Position, 1.5f);
    EXPECT_EQ(loaded[0].Label, "walk");
}

TEST(GraphBlendSpace1DStoreTests, StoreSkipsInfAndNegInf)
{
    Graph::Node host;
    const std::vector<BlendSpace1DSampleDesc> samples = {
        {std::numeric_limits<float>::infinity(), "pos"},
        {1.5f, "walk"},
        {-std::numeric_limits<float>::infinity(), "neg"},
    };
    GraphBlendSpace1DStore::Store(host, samples);

    std::vector<BlendSpace1DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_FLOAT_EQ(loaded[0].Position, 1.5f);
    EXPECT_EQ(loaded[0].Label, "walk");
}

TEST(GraphBlendSpace1DStoreTests, ExtraKeysIgnored)
{
    Graph::GraphObject sample;
    sample["position"] = 0.5f;
    sample["label"] = "jog";
    sample["clip"] = "ignored";

    std::vector<Graph::GraphValue> list;
    list.emplace_back(Graph::GraphValue(std::move(sample)));

    Graph::GraphObject bag;
    bag["samples"] = Graph::GraphValue(std::move(list));
    bag["axis"] = "also-ignored";
    Graph::Node host;
    host.Extensions[GraphBlendSpace1DStore::kExtensionKey] = Graph::GraphValue(std::move(bag));

    std::vector<BlendSpace1DSampleDesc> loaded;
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(host, loaded));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_FLOAT_EQ(loaded[0].Position, 0.5f);
    EXPECT_EQ(loaded[0].Label, "jog");
}

TEST(GraphBlendSpace1DStoreTests, AxisFromSamplesRanges)
{
    const BlendSpace1DAxis empty = BlendSpace1DAxis::FromSamples({});
    EXPECT_FLOAT_EQ(empty.Min, 0.f);
    EXPECT_FLOAT_EQ(empty.Max, 1.f);

    const BlendSpace1DAxis one = BlendSpace1DAxis::FromSamples({{3.f, ""}});
    EXPECT_FLOAT_EQ(one.Min, 2.f);
    EXPECT_FLOAT_EQ(one.Max, 4.f);

    const BlendSpace1DAxis span = BlendSpace1DAxis::FromSamples({{0.f, ""}, {5.f, ""}});
    EXPECT_FLOAT_EQ(span.Min, 0.f);
    EXPECT_FLOAT_EQ(span.Max, 5.f);

    const BlendSpace1DAxis equal = BlendSpace1DAxis::FromSamples({{4.f, "a"}, {4.f, "b"}});
    EXPECT_FLOAT_EQ(equal.Min, 3.f);
    EXPECT_FLOAT_EQ(equal.Max, 5.f);
}

TEST(GraphBlendSpace1DStoreTests, AxisPositionXRoundTripAndExtrapolate)
{
    BlendSpace1DAxis axis;
    axis.Min = 0.f;
    axis.Max = 5.f;
    const float left = 40.f;
    const float width = 400.f;

    const float x0 = left;
    const float xMid = left + 0.5f * width;
    const float x1 = left + width;
    EXPECT_FLOAT_EQ(axis.PositionFromX(x0, left, width), 0.f);
    EXPECT_FLOAT_EQ(axis.PositionFromX(xMid, left, width), 2.5f);
    EXPECT_FLOAT_EQ(axis.PositionFromX(x1, left, width), 5.f);
    EXPECT_FLOAT_EQ(axis.XFromPosition(0.f, left, width), x0);
    EXPECT_FLOAT_EQ(axis.XFromPosition(2.5f, left, width), xMid);
    EXPECT_FLOAT_EQ(axis.XFromPosition(5.f, left, width), x1);

    // Past the track extrapolates so a drag can extend min/max; axis recomputes on mouse-up.
    EXPECT_FLOAT_EQ(axis.PositionFromX(left - 80.f, left, width), -1.f);
    EXPECT_FLOAT_EQ(axis.PositionFromX(left + width + 80.f, left, width), 6.f);
    EXPECT_FLOAT_EQ(axis.XFromPosition(-1.f, left, width), left - 80.f);
    EXPECT_FLOAT_EQ(axis.XFromPosition(6.f, left, width), left + width + 80.f);
    EXPECT_FLOAT_EQ(axis.PositionFromX(100.f, left, 0.f), axis.Min);
}

TEST(GraphBlendSpace1DStoreTests, AxisClampedTPortsPlayheadMapping)
{
    BlendSpace1DAxis axis;
    axis.Min = 0.f;
    axis.Max = 5.f;
    EXPECT_FLOAT_EQ(axis.ClampedT(0.f), 0.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(2.5f), 0.5f);
    EXPECT_FLOAT_EQ(axis.ClampedT(5.f), 1.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(1e20f), 1.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(-1e20f), 0.f);
    EXPECT_FLOAT_EQ(axis.ClampedT(std::numeric_limits<float>::infinity()), 0.5f);
    EXPECT_FLOAT_EQ(axis.ClampedT(std::numeric_limits<float>::quiet_NaN()), 0.5f);

    const BlendSpace1DAxis collapsed = BlendSpace1DAxis::FromSamples({{4.f, "", ""}});
    EXPECT_FLOAT_EQ(collapsed.Min, 3.f);
    EXPECT_FLOAT_EQ(collapsed.Max, 5.f);
    EXPECT_FLOAT_EQ(collapsed.ClampedT(4.f), 0.5f);

    BlendSpace1DAxis zeroSpan;
    zeroSpan.Min = 4.f;
    zeroSpan.Max = 4.f;
    EXPECT_FLOAT_EQ(zeroSpan.ClampedT(4.f), 0.5f);
    EXPECT_FLOAT_EQ(zeroSpan.ClampedT(1e20f), 0.5f);
}
