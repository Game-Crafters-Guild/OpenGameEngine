#include <gtest/gtest.h>

#include "Graph/GraphNest.h"

using namespace GameEngine;

TEST(GraphNestTests, NestKindForDoubleClickTable)
{
    struct Case
    {
        GraphNestKind Current;
        const char* TypeId;
        GraphNestKind Expected;
    };
    const Case cases[] = {
        {GraphNestKind::Root, "StateMachine", GraphNestKind::StateMachine},
        {GraphNestKind::Root, "BlendSpace1D", GraphNestKind::BlendSpace1D},
        {GraphNestKind::Root, "BlendSpace2D", GraphNestKind::BlendSpace2D},
        {GraphNestKind::Root, "State", GraphNestKind::Root},
        {GraphNestKind::Root, "ClipPlayer", GraphNestKind::Root},
        {GraphNestKind::Root, "", GraphNestKind::Root},
        {GraphNestKind::Root, "Unknown", GraphNestKind::Root},
        {GraphNestKind::StateMachine, "State", GraphNestKind::PoseGraph},
        {GraphNestKind::StateMachine, "BlendSpace1D", GraphNestKind::StateMachine},
        {GraphNestKind::StateMachine, "StateMachine", GraphNestKind::StateMachine},
        {GraphNestKind::PoseGraph, "BlendSpace1D", GraphNestKind::BlendSpace1D},
        {GraphNestKind::PoseGraph, "BlendSpace2D", GraphNestKind::BlendSpace2D},
        {GraphNestKind::PoseGraph, "StateMachine", GraphNestKind::PoseGraph},
        {GraphNestKind::PoseGraph, "State", GraphNestKind::PoseGraph},
        {GraphNestKind::BlendSpace1D, "StateMachine", GraphNestKind::BlendSpace1D},
        {GraphNestKind::BlendSpace1D, "BlendSpace1D", GraphNestKind::BlendSpace1D},
        {GraphNestKind::BlendSpace1D, "State", GraphNestKind::BlendSpace1D},
        {GraphNestKind::BlendSpace1D, "ClipPlayer", GraphNestKind::BlendSpace1D},
        {GraphNestKind::BlendSpace1D, "", GraphNestKind::BlendSpace1D},
        {GraphNestKind::BlendSpace2D, "StateMachine", GraphNestKind::BlendSpace2D},
        {GraphNestKind::BlendSpace2D, "BlendSpace1D", GraphNestKind::BlendSpace2D},
        {GraphNestKind::BlendSpace2D, "BlendSpace2D", GraphNestKind::BlendSpace2D},
    };
    for (const Case& c : cases)
    {
        EXPECT_EQ(NestKindForDoubleClick(c.Current, c.TypeId), c.Expected)
            << "current=" << static_cast<int>(c.Current) << " type=" << c.TypeId;
    }
}

TEST(GraphNestTests, NestKindTitleNonEmptyForNonRoot)
{
    const GraphNestKind kinds[] = {
        GraphNestKind::StateMachine,
        GraphNestKind::PoseGraph,
        GraphNestKind::BlendSpace1D,
        GraphNestKind::BlendSpace2D,
    };
    for (GraphNestKind kind : kinds)
    {
        const char* title = NestKindTitle(kind);
        ASSERT_NE(title, nullptr);
        EXPECT_NE(title[0], '\0');
    }
}

TEST(GraphNestTests, NestKindUsesSubgraphModelAndHidesCanvas)
{
    EXPECT_FALSE(NestKindUsesSubgraphModel(GraphNestKind::Root));
    EXPECT_TRUE(NestKindUsesSubgraphModel(GraphNestKind::StateMachine));
    EXPECT_TRUE(NestKindUsesSubgraphModel(GraphNestKind::PoseGraph));
    EXPECT_FALSE(NestKindUsesSubgraphModel(GraphNestKind::BlendSpace1D));
    EXPECT_FALSE(NestKindUsesSubgraphModel(GraphNestKind::BlendSpace2D));

    EXPECT_FALSE(NestKindHidesCanvas(GraphNestKind::Root));
    EXPECT_FALSE(NestKindHidesCanvas(GraphNestKind::StateMachine));
    EXPECT_FALSE(NestKindHidesCanvas(GraphNestKind::PoseGraph));
    EXPECT_TRUE(NestKindHidesCanvas(GraphNestKind::BlendSpace1D));
    EXPECT_TRUE(NestKindHidesCanvas(GraphNestKind::BlendSpace2D));
}
