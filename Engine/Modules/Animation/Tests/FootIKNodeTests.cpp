#include <gtest/gtest.h>

#include "Animation/AnimationPose.h"
#include "Animation/AnimGraphNode.h"
#include "Animation/EvaluationContext.h"
#include "Animation/Nodes/FootIKNode.h"
#include "Animation/PoseUtilities.h"

#include <Mathematics/Vector3.h>
#include <Mathematics/Quaternion.h>

#include <memory>

using namespace GameEngine::Animation;
using namespace GameEngine::Mathematics;

namespace
{

EvaluationContext g_TestCtx;
EvaluationContext& MakeCtx(float deltaTime)
{
    g_TestCtx = EvaluationContext{};
    g_TestCtx.DeltaTime = deltaTime;
    return g_TestCtx;
}

// Simple source node that outputs a fixed pose.
class FixedPoseNode : public AnimGraphNode
{
public:
    AnimationPose Pose;
    void Evaluate(EvaluationContext& /*ctx*/, AnimationPose& outPose) override { outPose = Pose; }
};

std::unique_ptr<FixedPoseNode> MakeFixed(const AnimationPose& pose)
{
    auto node = std::make_unique<FixedPoseNode>();
    node->Pose = pose;
    return node;
}

// Builds a simple 4-bone leg skeleton: root(0) -> hip(1) -> knee(2) -> foot(3)
// Local-space layout — root sits at y=2 (world); hip(1) is at y=0 relative to root,
// knee(2) at y=-1 relative to hip, foot(3) at y=-1 relative to knee.
AnimationPose MakeLegPose()
{
    AnimationPose pose;
    pose.Resize(4);
    pose.Positions[0] = Vector3(0.0f, 2.0f, 0.0f);  // root
    pose.Positions[1] = Vector3(0.0f, 0.0f, 0.0f);  // hip (relative to root)
    pose.Positions[2] = Vector3(0.0f, -1.0f, 0.0f); // knee (relative to hip)
    pose.Positions[3] = Vector3(0.0f, -1.0f, 0.0f); // foot (relative to knee)
    return pose;
}

std::vector<int32_t> MakeLegParentIndices()
{
    return {-1, 0, 1, 2};
}

} // namespace

TEST(FootIKNodeTests, NoGroundHits_PassThrough)
{
    FootIKNode footIK;
    footIK.SetSource(MakeFixed(MakeLegPose()));
    footIK.SetParentIndices(MakeLegParentIndices());
    footIK.SetHipBoneIndex(1);

    FootIKLeg leg;
    leg.HipBone = 1;
    leg.KneeBone = 2;
    leg.FootBone = 3;
    footIK.AddLeg(leg);

    // No ground results set (default: no hit)
    AnimationPose result;
    footIK.Evaluate(MakeCtx(1.0f / 60.0f), result);

    // Pose should pass through unchanged.
    ASSERT_EQ(result.BoneCount, 4u);
    EXPECT_NEAR(result.Positions[0].y, 2.0f, 1e-4f);
    EXPECT_NEAR(result.Positions[1].y, 0.0f, 1e-4f);
    EXPECT_NEAR(result.Positions[2].y, -1.0f, 1e-4f);
    EXPECT_NEAR(result.Positions[3].y, -1.0f, 1e-4f);
}

TEST(FootIKNodeTests, GroundHit_AdjustsFoot)
{
    FootIKNode footIK;
    footIK.SetSource(MakeFixed(MakeLegPose()));
    footIK.SetParentIndices(MakeLegParentIndices());
    footIK.SetHipBoneIndex(1);
    footIK.SetMaxHipOffset(1.0f);
    footIK.SetWeight(1.0f);

    FootIKLeg leg;
    leg.HipBone = 1;
    leg.KneeBone = 2;
    leg.FootBone = 3;
    footIK.AddLeg(leg);

    // Ground is 0.3 units below the current foot position (foot needs to reach down).
    FootIKGroundResult ground;
    ground.Hit = true;
    ground.Position = Vector3(0.0f, -0.3f, 0.0f);
    ground.Normal = Vector3(0.0f, 1.0f, 0.0f);
    ground.Distance = 0.3f;
    footIK.SetGroundResult(0, ground);

    AnimationPose result;
    footIK.Evaluate(MakeCtx(1.0f / 60.0f), result);

    ASSERT_EQ(result.BoneCount, 4u);

    // The hip bone's local Y should have been adjusted downward to accommodate
    // the lower foot (negative hip offset translates to negative local Y delta).
    EXPECT_LT(result.Positions[1].y, 0.0f);
}
