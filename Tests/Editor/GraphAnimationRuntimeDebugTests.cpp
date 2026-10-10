#include <gtest/gtest.h>

#include "Animation/AnimParam.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Graph/GraphAnimationRuntimeDebug.h"
#include "Graph/GraphModel.h"

#include <limits>

using namespace GameEngine;

namespace {

Graph::Model MakeIdleWalkModel()
{
    Graph::Model model;
    Graph::Node idle;
    idle.Id = "idle";
    idle.TypeId = "State";
    idle.Parameters["title"] = "Idle";
    Graph::Node walk;
    walk.Id = "walk";
    walk.TypeId = "State";
    walk.Parameters["title"] = "Walk";
    Graph::Node entry;
    entry.Id = "entry";
    entry.TypeId = "Entry";
    model.Nodes.push_back(entry);
    model.Nodes.push_back(idle);
    model.Nodes.push_back(walk);

    Graph::Edge entryLink;
    entryLink.Id = "e0";
    entryLink.SourceNodeId = "entry";
    entryLink.SourcePortId = "out";
    entryLink.TargetNodeId = "idle";
    entryLink.TargetPortId = "in";
    Graph::Edge idleWalk;
    idleWalk.Id = "t_idle_walk";
    idleWalk.SourceNodeId = "idle";
    idleWalk.SourcePortId = "out";
    idleWalk.TargetNodeId = "walk";
    idleWalk.TargetPortId = "in";
    model.Links.push_back(entryLink);
    model.Links.push_back(idleWalk);
    return model;
}

Animation::StateMachineNode MakeIdleWalkMachine()
{
    Animation::StateMachineNode sm;
    sm.AddState("Idle", std::make_unique<Animation::ClipPlayerNode>());
    sm.AddState("Walk", std::make_unique<Animation::ClipPlayerNode>());
    sm.AddTransition(0, 1, 1.0f,
                     {Animation::TransitionCondition::Make("Speed", Animation::ParamCompare::Greater, 0.5f)});
    return sm;
}

} // namespace

TEST(GraphAnimationRuntimeDebugTests, FindStateNodeIdUsesTitleThenId)
{
    const Graph::Model model = MakeIdleWalkModel();
    EXPECT_EQ(GraphAnimationRuntimeDebug::FindStateNodeId(model, "Idle"), "idle");
    EXPECT_EQ(GraphAnimationRuntimeDebug::FindStateNodeId(model, "Walk"), "walk");
    EXPECT_TRUE(GraphAnimationRuntimeDebug::FindStateNodeId(model, "Missing").empty());
}

TEST(GraphAnimationRuntimeDebugTests, ActiveStateHighlightsIdle)
{
    const Graph::Model model = MakeIdleWalkModel();
    const Animation::StateMachineNode sm = MakeIdleWalkMachine();
    const GraphAnimationRuntimeHighlight hl = GraphAnimationRuntimeDebug::ForStateMachine(model, sm);
    ASSERT_EQ(hl.NodeIds.size(), 1u);
    EXPECT_TRUE(hl.NodeIds.count("idle") != 0);
    EXPECT_TRUE(hl.LinkIds.empty());
}

TEST(GraphAnimationRuntimeDebugTests, TransitioningPulsesIdleToWalkLink)
{
    const Graph::Model model = MakeIdleWalkModel();
    Animation::StateMachineNode sm = MakeIdleWalkMachine();
    sm.SetParam("Speed", 1.0f);
    Animation::AnimationPose dummy;
    Animation::EvaluationContext ctx;
    ctx.DeltaTime = 0.0f;
    sm.Evaluate(ctx, dummy);
    ASSERT_TRUE(sm.IsTransitioning());

    const GraphAnimationRuntimeHighlight hl = GraphAnimationRuntimeDebug::ForStateMachine(model, sm);
    EXPECT_TRUE(hl.NodeIds.count("idle") != 0);
    EXPECT_TRUE(hl.NodeIds.count("walk") != 0);
    ASSERT_EQ(hl.LinkIds.size(), 1u);
    EXPECT_EQ(hl.LinkIds[0], "t_idle_walk");
}

TEST(GraphAnimationRuntimeDebugTests, StateIsEvaluatingMatchesActiveAndTransitionTarget)
{
    const Graph::Model model = MakeIdleWalkModel();
    Animation::StateMachineNode sm = MakeIdleWalkMachine();
    EXPECT_TRUE(GraphAnimationRuntimeDebug::StateIsEvaluating(model, "idle", sm));
    EXPECT_FALSE(GraphAnimationRuntimeDebug::StateIsEvaluating(model, "walk", sm));
    EXPECT_FALSE(GraphAnimationRuntimeDebug::StateIsEvaluating(model, "entry", sm));

    sm.SetParam("Speed", 1.0f);
    Animation::AnimationPose dummy;
    Animation::EvaluationContext ctx;
    ctx.DeltaTime = 0.0f;
    sm.Evaluate(ctx, dummy);
    ASSERT_TRUE(sm.IsTransitioning());
    EXPECT_TRUE(GraphAnimationRuntimeDebug::StateIsEvaluating(model, "idle", sm));
    EXPECT_TRUE(GraphAnimationRuntimeDebug::StateIsEvaluating(model, "walk", sm));
}

TEST(GraphAnimationRuntimeDebugTests, ForPoseGraphHighlightsOutputAndItsPoseSource)
{
    Graph::Model pose;
    Graph::Node clip;
    clip.Id = "clip";
    clip.TypeId = "ClipPlayer";
    Graph::Node out;
    out.Id = "out";
    out.TypeId = "OutputPose";
    Graph::Node unused;
    unused.Id = "other";
    unused.TypeId = "ClipPlayer";
    pose.Nodes.push_back(clip);
    pose.Nodes.push_back(out);
    pose.Nodes.push_back(unused);
    Graph::Edge link;
    link.Id = "l0";
    link.SourceNodeId = "clip";
    link.SourcePortId = "poseOut";
    link.TargetNodeId = "out";
    link.TargetPortId = "pose";
    pose.Links.push_back(link);

    const GraphAnimationRuntimeHighlight hl = GraphAnimationRuntimeDebug::ForPoseGraph(pose);
    EXPECT_TRUE(hl.NodeIds.count("out") != 0);
    EXPECT_TRUE(hl.NodeIds.count("clip") != 0);
    EXPECT_TRUE(hl.NodeIds.count("other") == 0);
    EXPECT_TRUE(hl.LinkIds.empty());
}

TEST(GraphAnimationRuntimeDebugTests, TryPreviewFloatRejectsMissingEmptyAndNonFinite)
{
    Animation::AnimationGraphPlayer player;
    float value = 7.f;
    EXPECT_FALSE(GraphAnimationRuntimeDebug::TryPreviewFloat(player, "Speed", value));
    EXPECT_FALSE(GraphAnimationRuntimeDebug::TryPreviewFloat(player, "", value));

    player.SetParameter("Speed", 1.5f);
    ASSERT_TRUE(GraphAnimationRuntimeDebug::TryPreviewFloat(player, "Speed", value));
    EXPECT_FLOAT_EQ(value, 1.5f);

    player.SetParameter("Speed", std::numeric_limits<float>::infinity());
    EXPECT_FALSE(GraphAnimationRuntimeDebug::TryPreviewFloat(player, "Speed", value));
    player.SetParameter("Speed", std::numeric_limits<float>::quiet_NaN());
    EXPECT_FALSE(GraphAnimationRuntimeDebug::TryPreviewFloat(player, "Speed", value));
}

TEST(GraphAnimationRuntimeDebugTests, TryPreviewFloatRequiresBothAxesFor2D)
{
    Animation::AnimationGraphPlayer player;
    player.SetParameter("Speed", 2.f);
    float x = 0.f;
    float y = 0.f;
    const bool hasX = GraphAnimationRuntimeDebug::TryPreviewFloat(player, "Speed", x);
    const bool hasY = GraphAnimationRuntimeDebug::TryPreviewFloat(player, "Direction", y);
    EXPECT_TRUE(hasX);
    EXPECT_FALSE(hasY);
    EXPECT_FALSE(hasX && hasY);
}
