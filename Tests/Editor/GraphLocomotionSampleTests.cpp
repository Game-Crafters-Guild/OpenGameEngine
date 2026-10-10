#include <gtest/gtest.h>

#include "Graph/GraphBlendSpace1DStore.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphSubgraphStore.h"
#include "Graph/GraphTransitionStore.h"
#include "StagedTestPaths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace GameEngine;

namespace {

// The sample as the editor ships it: staged beside this executable by the
// EditorTests build, never climbed to from __FILE__ or the working directory
// (a relative __FILE__ — clang through a ccache base_dir — resolves against
// whatever directory the process is in, and a repo path is absent once the
// build output moves).
std::filesystem::path LocomotionSamplePath()
{
    return TestPaths::ExecutableDirectory() / "Assets" / "Graphs" / "LocomotionSample.animgraph";
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(GraphLocomotionSampleTests, FileExistsAndParsesAsAnimationModel)
{
    const std::filesystem::path path = LocomotionSamplePath();
    ASSERT_TRUE(std::filesystem::exists(path)) << path.string();

    Graph::Model model;
    ASSERT_TRUE(Graph::FromJson(ReadAll(path), model));
    EXPECT_EQ(model.KindId, "animation");

    const Graph::Node* sm = model.FindNode("sm");
    const Graph::Node* out = model.FindNode("out");
    ASSERT_NE(sm, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(sm->TypeId, "StateMachine");
    EXPECT_EQ(out->TypeId, "OutputPose");
}

TEST(GraphLocomotionSampleTests, NestedStateMachineHasIdleWalkAndConditionedWires)
{
    Graph::Model model;
    ASSERT_TRUE(Graph::FromJson(ReadAll(LocomotionSamplePath()), model));
    const Graph::Node* sm = model.FindNode("sm");
    ASSERT_NE(sm, nullptr);

    Graph::Model nested;
    ASSERT_TRUE(GraphSubgraphStore::TryLoadSubgraph(*sm, nested));
    const Graph::Node* idle = nested.FindNode("idle");
    const Graph::Node* walk = nested.FindNode("walk");
    const Graph::Node* entry = nested.FindNode("entry");
    ASSERT_NE(idle, nullptr);
    ASSERT_NE(walk, nullptr);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(idle->Parameters.GetString("title"), "Idle");
    EXPECT_EQ(walk->Parameters.GetString("title"), "Walk");

    const Graph::Edge* idleToWalk = nested.FindLink("t_idle_walk");
    const Graph::Edge* walkToIdle = nested.FindLink("t_walk_idle");
    ASSERT_NE(idleToWalk, nullptr);
    ASSERT_NE(walkToIdle, nullptr);
    EXPECT_TRUE(GraphTransitionStore::IsStateTransition(nested, *idleToWalk));
    EXPECT_TRUE(GraphTransitionStore::IsStateTransition(nested, *walkToIdle));
    const Graph::Edge* entryLink = nested.FindLink("e0");
    ASSERT_NE(entryLink, nullptr);
    EXPECT_FALSE(GraphTransitionStore::IsStateTransition(nested, *entryLink));

    const GraphTransitionDesc idleWalk = GraphTransitionStore::Load(*idleToWalk);
    ASSERT_EQ(idleWalk.Conditions.size(), 1u);
    EXPECT_EQ(idleWalk.Conditions[0].Param, "Speed");
    EXPECT_EQ(idleWalk.Conditions[0].Op, "greaterThan");

    const GraphTransitionDesc walkIdle = GraphTransitionStore::Load(*walkToIdle);
    ASSERT_EQ(walkIdle.Conditions.size(), 1u);
    EXPECT_EQ(walkIdle.Conditions[0].Op, "lessThan");
}

TEST(GraphLocomotionSampleTests, WalkStateHostsBlendSpace1D)
{
    Graph::Model model;
    ASSERT_TRUE(Graph::FromJson(ReadAll(LocomotionSamplePath()), model));
    const Graph::Node* sm = model.FindNode("sm");
    ASSERT_NE(sm, nullptr);
    Graph::Model nested;
    ASSERT_TRUE(GraphSubgraphStore::TryLoadSubgraph(*sm, nested));
    const Graph::Node* walk = nested.FindNode("walk");
    ASSERT_NE(walk, nullptr);
    Graph::Model walkPose;
    ASSERT_TRUE(GraphSubgraphStore::TryLoadSubgraph(*walk, walkPose));
    const Graph::Node* bs = walkPose.FindNode("bs");
    ASSERT_NE(bs, nullptr);
    EXPECT_EQ(bs->TypeId, "BlendSpace1D");

    std::vector<BlendSpace1DSampleDesc> samples;
    ASSERT_TRUE(GraphBlendSpace1DStore::TryLoad(*bs, samples));
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_FLOAT_EQ(samples[0].Position, 0.0f);
    EXPECT_EQ(samples[0].Label, "Walk");
    EXPECT_FLOAT_EQ(samples[1].Position, 1.5f);
    EXPECT_EQ(samples[1].Label, "Run");
}
