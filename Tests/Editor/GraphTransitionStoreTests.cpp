#include <gtest/gtest.h>

#include "Graph/GraphTransitionStore.h"

#include <limits>

using namespace GameEngine;

TEST(GraphTransitionStoreTests, EntryToStateIsNotATransition)
{
    Graph::Model model;
    Graph::Node entry;
    entry.Id = "entry";
    entry.TypeId = "Entry";
    Graph::Node idle;
    idle.Id = "idle";
    idle.TypeId = "State";
    model.Nodes.push_back(entry);
    model.Nodes.push_back(idle);

    Graph::Edge link;
    link.SourceNodeId = "entry";
    link.SourcePortId = "out";
    link.TargetNodeId = "idle";
    link.TargetPortId = "in";
    EXPECT_FALSE(GraphTransitionStore::IsStateTransition(model, link));
}

TEST(GraphTransitionStoreTests, StateToStateOutInIsATransition)
{
    Graph::Model model;
    Graph::Node idle;
    idle.Id = "idle";
    idle.TypeId = "State";
    Graph::Node walk;
    walk.Id = "walk";
    walk.TypeId = "State";
    model.Nodes.push_back(idle);
    model.Nodes.push_back(walk);

    Graph::Edge link;
    link.SourceNodeId = "idle";
    link.SourcePortId = "out";
    link.TargetNodeId = "walk";
    link.TargetPortId = "in";
    EXPECT_TRUE(GraphTransitionStore::IsStateTransition(model, link));
}

TEST(GraphTransitionStoreTests, WrongPortsAreNotTransitions)
{
    Graph::Model model;
    Graph::Node idle;
    idle.Id = "idle";
    idle.TypeId = "State";
    Graph::Node walk;
    walk.Id = "walk";
    walk.TypeId = "State";
    model.Nodes.push_back(idle);
    model.Nodes.push_back(walk);

    Graph::Edge link;
    link.SourceNodeId = "idle";
    link.SourcePortId = "poseOut";
    link.TargetNodeId = "walk";
    link.TargetPortId = "pose";
    EXPECT_FALSE(GraphTransitionStore::IsStateTransition(model, link));
}

TEST(GraphTransitionStoreTests, LoadMissingPassthroughUsesDefaultDuration)
{
    Graph::Edge link;
    const GraphTransitionDesc desc = GraphTransitionStore::Load(link);
    EXPECT_FLOAT_EQ(desc.Duration, GraphTransitionStore::kDefaultDuration);
    EXPECT_TRUE(desc.Conditions.empty());
}

TEST(GraphTransitionStoreTests, StoreThenLoadRoundTripsDurationAndCondition)
{
    Graph::Edge link;
    GraphTransitionDesc desc;
    desc.Duration = 0.15f;
    GraphTransitionConditionDesc cond;
    cond.Param = "Speed";
    cond.Op = "greaterThan";
    cond.Value = 0.5f;
    desc.Conditions.push_back(cond);
    GraphTransitionStore::Store(link, desc);

    const GraphTransitionDesc loaded = GraphTransitionStore::Load(link);
    EXPECT_FLOAT_EQ(loaded.Duration, 0.15f);
    ASSERT_EQ(loaded.Conditions.size(), 1u);
    EXPECT_EQ(loaded.Conditions[0].Param, "Speed");
    EXPECT_EQ(loaded.Conditions[0].Op, "greaterThan");
    ASSERT_TRUE(loaded.Conditions[0].Value.IsFloat());
    EXPECT_DOUBLE_EQ(loaded.Conditions[0].Value.AsFloat(), 0.5);
}

TEST(GraphTransitionStoreTests, StoreEmptyConditionsWritesEmptyList)
{
    Graph::Edge link;
    GraphTransitionDesc desc;
    desc.Duration = 0.2f;
    GraphTransitionStore::Store(link, desc);

    const auto it = link.Passthrough.find("conditions");
    ASSERT_NE(it, link.Passthrough.end());
    const std::vector<Graph::GraphValue>* list = it->second.TryList();
    ASSERT_NE(list, nullptr);
    EXPECT_TRUE(list->empty());
    EXPECT_TRUE(GraphTransitionStore::Load(link).Conditions.empty());
}

TEST(GraphTransitionStoreTests, StoreRejectsNonFiniteDuration)
{
    Graph::Edge link;
    GraphTransitionDesc desc;
    desc.Duration = std::numeric_limits<float>::infinity();
    GraphTransitionStore::Store(link, desc);
    EXPECT_FLOAT_EQ(GraphTransitionStore::Load(link).Duration, GraphTransitionStore::kDefaultDuration);
}

TEST(GraphTransitionStoreTests, TryParseValueTextAcceptsBoolIntFloat)
{
    Graph::GraphValue value;
    ASSERT_TRUE(GraphTransitionStore::TryParseValueText("true", value));
    ASSERT_TRUE(value.IsBool());
    EXPECT_TRUE(value.AsBool());

    ASSERT_TRUE(GraphTransitionStore::TryParseValueText("  -3  ", value));
    ASSERT_TRUE(value.IsInt());
    EXPECT_EQ(value.AsInt(), -3);

    ASSERT_TRUE(GraphTransitionStore::TryParseValueText("0.5", value));
    ASSERT_TRUE(value.IsFloat());
    EXPECT_DOUBLE_EQ(value.AsFloat(), 0.5);
    EXPECT_EQ(GraphTransitionStore::ValueToText(value), "0.5");
}

TEST(GraphTransitionStoreTests, TryParseValueTextRejectsEmptyAndNonFinite)
{
    Graph::GraphValue value{1};
    EXPECT_FALSE(GraphTransitionStore::TryParseValueText("", value));
    EXPECT_FALSE(GraphTransitionStore::TryParseValueText("   ", value));
    EXPECT_FALSE(GraphTransitionStore::TryParseValueText("inf", value));
    EXPECT_FALSE(GraphTransitionStore::TryParseValueText("Speed", value));
    EXPECT_FALSE(GraphTransitionStore::TryParseValueText("1e40", value));
}

TEST(GraphTransitionStoreTests, StoreSkipsFloatOverflowConditionValue)
{
    Graph::Edge link;
    GraphTransitionDesc desc;
    GraphTransitionConditionDesc cond;
    cond.Param = "Speed";
    cond.Op = "greaterThan";
    cond.Value = 1e40;
    desc.Conditions.push_back(cond);
    GraphTransitionStore::Store(link, desc);
    EXPECT_TRUE(GraphTransitionStore::Load(link).Conditions.empty());
}

TEST(GraphTransitionStoreTests, StoreSkipsNonFiniteConditionValue)
{
    Graph::Edge link;
    GraphTransitionDesc desc;
    GraphTransitionConditionDesc cond;
    cond.Param = "Speed";
    cond.Op = "greaterThan";
    cond.Value = std::numeric_limits<float>::infinity();
    desc.Conditions.push_back(cond);
    GraphTransitionStore::Store(link, desc);
    EXPECT_TRUE(GraphTransitionStore::Load(link).Conditions.empty());
}

TEST(GraphTransitionStoreTests, MakeDefaultConditionIsStoreable)
{
    Graph::Edge link;
    GraphTransitionDesc desc;
    desc.Conditions.push_back(GraphTransitionStore::MakeDefaultCondition());
    GraphTransitionStore::Store(link, desc);
    const GraphTransitionDesc loaded = GraphTransitionStore::Load(link);
    ASSERT_EQ(loaded.Conditions.size(), 1u);
    EXPECT_EQ(loaded.Conditions[0].Param, "Speed");
    EXPECT_EQ(loaded.Conditions[0].Op, "greaterThan");
}
