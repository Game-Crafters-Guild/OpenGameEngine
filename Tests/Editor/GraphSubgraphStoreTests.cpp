#include <gtest/gtest.h>

#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphSubgraphStore.h"

using namespace GameEngine;

TEST(GraphSubgraphStoreTests, MissingKeyReturnsFalseAndLeavesOutUnchanged)
{
    Graph::Node host;
    Graph::Model out;
    out.KindId = "sentinel";
    Graph::Node marker;
    marker.Id = "keep";
    out.Nodes.push_back(marker);

    EXPECT_FALSE(GraphSubgraphStore::TryLoadSubgraph(host, out));
    EXPECT_EQ(out.KindId, "sentinel");
    ASSERT_EQ(out.Nodes.size(), 1u);
    EXPECT_EQ(out.Nodes[0].Id, "keep");
}

TEST(GraphSubgraphStoreTests, StoreThenTryLoadRoundTripsKindAndNode)
{
    Graph::Model nested;
    nested.KindId = "animation";
    Graph::Node state;
    state.Id = "state_0";
    state.TypeId = "State";
    nested.Nodes.push_back(state);

    Graph::Node host;
    GraphSubgraphStore::StoreSubgraph(host, nested);

    Graph::Model loaded;
    loaded.KindId = "sentinel";
    ASSERT_TRUE(GraphSubgraphStore::TryLoadSubgraph(host, loaded));
    EXPECT_EQ(loaded.KindId, "animation");
    ASSERT_EQ(loaded.Nodes.size(), 1u);
    EXPECT_EQ(loaded.Nodes[0].Id, "state_0");
    EXPECT_EQ(loaded.Nodes[0].TypeId, "State");
}

TEST(GraphSubgraphStoreTests, EmptySubgraphStringReturnsFalseAndLeavesOutUnchanged)
{
    Graph::Node host;
    host.Extensions["subgraph"] = std::string();

    Graph::Model out;
    out.KindId = "sentinel";
    EXPECT_FALSE(GraphSubgraphStore::TryLoadSubgraph(host, out));
    EXPECT_EQ(out.KindId, "sentinel");
}

TEST(GraphSubgraphStoreTests, GarbageSubgraphStringReturnsFalseAndLeavesOutUnchanged)
{
    Graph::Node host;
    host.Extensions["subgraph"] = std::string("{not-json");

    Graph::Model out;
    out.KindId = "sentinel";
    EXPECT_FALSE(GraphSubgraphStore::TryLoadSubgraph(host, out));
    EXPECT_EQ(out.KindId, "sentinel");
    EXPECT_TRUE(out.Nodes.empty());
}

TEST(GraphSubgraphStoreTests, StoreThenTryLoadRoundTripsTransitionEdge)
{
    (void)GraphNodeRegistry::Get();
    Graph::Model nested;
    nested.KindId = "animation";
    Graph::Node entry = GraphNodeRegistry::Get().CreateNode("animation", "Entry", "entry_0", 0.f, 0.f);
    Graph::Node state = GraphNodeRegistry::Get().CreateNode("animation", "State", "state_0", 0.f, 0.f);
    nested.Nodes.push_back(std::move(entry));
    nested.Nodes.push_back(std::move(state));
    Graph::Edge link;
    link.Id = "link_0";
    link.SourceNodeId = "entry_0";
    link.SourcePortId = "out";
    link.TargetNodeId = "state_0";
    link.TargetPortId = "in";
    nested.Links.push_back(link);
    ASSERT_TRUE(nested.Validate());

    Graph::Node host;
    GraphSubgraphStore::StoreSubgraph(host, nested);

    Graph::Model loaded;
    ASSERT_TRUE(GraphSubgraphStore::TryLoadSubgraph(host, loaded));
    ASSERT_EQ(loaded.Links.size(), 1u);
    EXPECT_EQ(loaded.Links[0].SourceNodeId, "entry_0");
    EXPECT_EQ(loaded.Links[0].TargetNodeId, "state_0");
    EXPECT_TRUE(loaded.Validate());
}
