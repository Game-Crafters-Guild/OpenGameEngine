#include <gtest/gtest.h>

#include "Animation/AnimationPose.h"
#include "Animation/AnimGraphNode.h"
#include "Animation/Nodes/Blend2Node.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Animation/Nodes/BlendSpace1DNode.h"
#include "Animation/Nodes/BlendSpace2DNode.h"
#include "Animation/BoneMask.h"
#include "Animation/AnimationEvent.h"
#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/EvaluationContext.h"
#include "Animation/RootMotionExtractor.h"
#include "Animation/AnimParam.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphSerializer.h"
#include "Animation/AnimationGraphStore.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "AssetCore/GUID.h"
#include "Types/StringId.h"

#include <cmath>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

using namespace GameEngine::Animation;
using namespace GameEngine::Mathematics;
using GameEngine::HashStringId;

namespace
{
// Returns a reference to a thread-local context so call-sites can pass
// the result directly to `Evaluate(EvaluationContext&, ...)` without
// binding an rvalue temporary.
EvaluationContext& MakeCtx(float deltaTime)
{
    thread_local EvaluationContext ctx;
    ctx = EvaluationContext{};
    ctx.DeltaTime = deltaTime;
    return ctx;
}
} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static AnimationPose MakeTestPose(uint32_t boneCount, float posValue, float scaleValue)
{
    AnimationPose pose;
    pose.Resize(boneCount);
    for (uint32_t i = 0; i < boneCount; i++)
    {
        pose.Positions[i] = Vector3(posValue, posValue, posValue);
        pose.Rotations[i] = Quaternion::Identity();
        pose.Scales[i] = Vector3(scaleValue, scaleValue, scaleValue);
    }
    return pose;
}

// A stub node that always returns a fixed pose.
class ConstantPoseNode : public AnimGraphNode
{
public:
    AnimationPose FixedPose;

    void Evaluate(EvaluationContext& /*ctx*/, AnimationPose& outPose) override
    {
        outPose = FixedPose;
    }
};

constexpr float kEpsilon = 0.001f;

static std::unique_ptr<ConstantPoseNode> MakeOwnedPose(uint32_t boneCount, float posValue, float scaleValue)
{
    auto node = std::make_unique<ConstantPoseNode>();
    node->FixedPose = MakeTestPose(boneCount, posValue, scaleValue);
    return node;
}

// ---------------------------------------------------------------------------
// AnimationPose tests
// ---------------------------------------------------------------------------

TEST(AnimationPose, Blend_HalfWeight_InterpolatesCorrectly)
{
    auto poseA = MakeTestPose(3, 0.0f, 1.0f);
    auto poseB = MakeTestPose(3, 10.0f, 3.0f);

    AnimationPose result;
    AnimationPose::Blend(poseA, poseB, 0.5f, result);

    EXPECT_EQ(result.BoneCount, 3u);
    for (uint32_t i = 0; i < 3; ++i)
    {
        EXPECT_NEAR(result.Positions[i].x, 5.0f, kEpsilon);
        EXPECT_NEAR(result.Positions[i].y, 5.0f, kEpsilon);
        EXPECT_NEAR(result.Positions[i].z, 5.0f, kEpsilon);
        EXPECT_NEAR(result.Scales[i].x, 2.0f, kEpsilon);
        EXPECT_NEAR(result.Scales[i].y, 2.0f, kEpsilon);
        EXPECT_NEAR(result.Scales[i].z, 2.0f, kEpsilon);
    }
}

TEST(AnimationPose, Blend_ZeroWeight_ReturnsFirst)
{
    auto poseA = MakeTestPose(2, 1.0f, 2.0f);
    auto poseB = MakeTestPose(2, 9.0f, 8.0f);

    AnimationPose result;
    AnimationPose::Blend(poseA, poseB, 0.0f, result);

    for (uint32_t i = 0; i < 2; ++i)
    {
        EXPECT_NEAR(result.Positions[i].x, 1.0f, kEpsilon);
        EXPECT_NEAR(result.Scales[i].x, 2.0f, kEpsilon);
    }
}

TEST(AnimationPose, Blend_OneWeight_ReturnsSecond)
{
    auto poseA = MakeTestPose(2, 1.0f, 2.0f);
    auto poseB = MakeTestPose(2, 9.0f, 8.0f);

    AnimationPose result;
    AnimationPose::Blend(poseA, poseB, 1.0f, result);

    for (uint32_t i = 0; i < 2; ++i)
    {
        EXPECT_NEAR(result.Positions[i].x, 9.0f, kEpsilon);
        EXPECT_NEAR(result.Scales[i].x, 8.0f, kEpsilon);
    }
}

TEST(AnimationPose, BlendAdditive_AppliesDelta)
{
    auto base = MakeTestPose(2, 5.0f, 1.0f);

    AnimationPose additive;
    additive.Resize(2);
    for (uint32_t i = 0; i < 2; ++i)
    {
        additive.Positions[i] = Vector3(1.0f, 2.0f, 3.0f);
        additive.Rotations[i] = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.1f);
        additive.Scales[i] = Vector3(2.0f, 2.0f, 2.0f);
    }

    AnimationPose result;
    AnimationPose::BlendAdditive(base, additive, 1.0f, result);

    EXPECT_NEAR(result.Positions[0].x, 6.0f, kEpsilon);
    EXPECT_NEAR(result.Positions[0].y, 7.0f, kEpsilon);
    EXPECT_NEAR(result.Positions[0].z, 8.0f, kEpsilon);

    EXPECT_NEAR(result.Scales[0].x, 2.0f, kEpsilon);
    EXPECT_NEAR(result.Scales[0].y, 2.0f, kEpsilon);

    // Rotation should differ from identity after composing a Y-axis rotation
    const auto& q = result.Rotations[0].GetGLM();
    const float rotMagnitude = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    EXPECT_GT(rotMagnitude, 0.01f);
}

TEST(AnimationPose, Resize_SetsCorrectBoneCount)
{
    AnimationPose pose;
    pose.Resize(7);

    EXPECT_EQ(pose.BoneCount, 7u);
    EXPECT_EQ(pose.Positions.size(), 7u);
    EXPECT_EQ(pose.Rotations.size(), 7u);
    EXPECT_EQ(pose.Scales.size(), 7u);
}

// ---------------------------------------------------------------------------
// Blend2Node tests
// ---------------------------------------------------------------------------

TEST(Blend2Node, NoInputs_EmptyPose)
{
    Blend2Node node;
    AnimationPose result;
    node.Evaluate(MakeCtx(0.016f), result);

    EXPECT_EQ(result.BoneCount, 0u);
}

TEST(Blend2Node, OneInput_PassThrough)
{
    auto nodeA = std::make_unique<ConstantPoseNode>();
    nodeA->FixedPose = MakeTestPose(3, 4.0f, 1.0f);

    Blend2Node blend;
    blend.SetInputA(std::move(nodeA));
    blend.SetWeight(0.7f);

    AnimationPose result;
    blend.Evaluate(MakeCtx(0.016f), result);

    EXPECT_EQ(result.BoneCount, 3u);
    EXPECT_NEAR(result.Positions[0].x, 4.0f, kEpsilon);
}

TEST(Blend2Node, HalfBlend_Interpolates)
{
    auto nodeA = std::make_unique<ConstantPoseNode>();
    nodeA->FixedPose = MakeTestPose(2, 0.0f, 1.0f);

    auto nodeB = std::make_unique<ConstantPoseNode>();
    nodeB->FixedPose = MakeTestPose(2, 10.0f, 3.0f);

    Blend2Node blend;
    blend.SetInputA(std::move(nodeA));
    blend.SetInputB(std::move(nodeB));
    blend.SetWeight(0.5f);

    AnimationPose result;
    blend.Evaluate(MakeCtx(0.016f), result);

    EXPECT_EQ(result.BoneCount, 2u);
    EXPECT_NEAR(result.Positions[0].x, 5.0f, kEpsilon);
    EXPECT_NEAR(result.Scales[0].x, 2.0f, kEpsilon);
}

// ---------------------------------------------------------------------------
// StateMachineNode tests
// ---------------------------------------------------------------------------

TEST(StateMachineNode, SingleState_EvaluatesIt)
{
    auto poseNode = std::make_unique<ConstantPoseNode>();
    poseNode->FixedPose = MakeTestPose(2, 3.0f, 1.0f);

    StateMachineNode sm;
    sm.AddState("Idle", std::move(poseNode));

    AnimationPose result;
    sm.Evaluate(MakeCtx(0.016f), result);

    EXPECT_EQ(result.BoneCount, 2u);
    EXPECT_NEAR(result.Positions[0].x, 3.0f, kEpsilon);
}

TEST(StateMachineNode, Transition_Crossfades)
{
    auto idleNode = std::make_unique<ConstantPoseNode>();
    idleNode->FixedPose = MakeTestPose(2, 0.0f, 1.0f);
    auto runNode = std::make_unique<ConstantPoseNode>();
    runNode->FixedPose = MakeTestPose(2, 10.0f, 1.0f);

    StateMachineNode sm;
    uint32_t idleIdx = sm.AddState("Idle", std::move(idleNode));
    uint32_t runIdx = sm.AddState("Run", std::move(runNode));

    // Transition when "shouldRun" param is true
    sm.AddTransition(idleIdx, runIdx, 1.0f, {
        TransitionCondition::Make("shouldRun", ParamCompare::Equals, ParamValue{true})
    });

    // Trigger transition
    sm.SetParam("shouldRun", ParamValue{true});
    AnimationPose dummy;
    sm.Evaluate(MakeCtx(0.0f), dummy); // detects transition
    EXPECT_TRUE(sm.IsTransitioning());
    EXPECT_EQ(sm.GetTransitionTargetIndex(), runIdx);
    EXPECT_EQ(sm.GetActiveStateIndex(), idleIdx);

    sm.SetParam("shouldRun", ParamValue{false});
    AnimationPose result;
    sm.Evaluate(MakeCtx(0.5f), result); // halfway through 1s crossfade

    EXPECT_EQ(result.BoneCount, 2u);
    EXPECT_NEAR(result.Positions[0].x, 5.0f, kEpsilon);
}

TEST(StateMachineNode, TransitionCompletes_FullyInNewState)
{
    auto idleNode = std::make_unique<ConstantPoseNode>();
    idleNode->FixedPose = MakeTestPose(2, 0.0f, 1.0f);
    auto runNode = std::make_unique<ConstantPoseNode>();
    runNode->FixedPose = MakeTestPose(2, 10.0f, 1.0f);

    StateMachineNode sm;
    uint32_t idleIdx = sm.AddState("Idle", std::move(idleNode));
    uint32_t runIdx = sm.AddState("Run", std::move(runNode));

    sm.AddTransition(idleIdx, runIdx, 0.5f, {
        TransitionCondition::Make("shouldRun", ParamCompare::Equals, ParamValue{true})
    });

    // Trigger and complete the transition in one large step
    sm.SetParam("shouldRun", ParamValue{true});
    AnimationPose dummy;
    sm.Evaluate(MakeCtx(0.0f), dummy);

    sm.SetParam("shouldRun", ParamValue{false});
    AnimationPose result;
    sm.Evaluate(MakeCtx(1.0f), result);

    EXPECT_NEAR(result.Positions[0].x, 10.0f, kEpsilon);
}

TEST(StateMachineNode, NoTransitionConditionMet_StaysInState)
{
    auto idleNode = std::make_unique<ConstantPoseNode>();
    idleNode->FixedPose = MakeTestPose(2, 3.0f, 1.0f);
    auto runNode = std::make_unique<ConstantPoseNode>();
    runNode->FixedPose = MakeTestPose(2, 10.0f, 1.0f);

    StateMachineNode sm;
    uint32_t idleIdx = sm.AddState("Idle", std::move(idleNode));
    uint32_t runIdx = sm.AddState("Run", std::move(runNode));

    // Condition always false
    sm.AddTransition(idleIdx, runIdx, 0.5f, {
        TransitionCondition::Make("shouldRun", ParamCompare::Equals, ParamValue{true})
    });
    sm.SetParam("shouldRun", ParamValue{false});

    AnimationPose result;
    sm.Evaluate(MakeCtx(0.5f), result);

    EXPECT_NEAR(result.Positions[0].x, 3.0f, kEpsilon);
}

// ---------------------------------------------------------------------------
// BlendSpace1DNode tests
// ---------------------------------------------------------------------------

TEST(BlendSpace1DNode, SingleSample_ReturnsIt)
{
    BlendSpace1DNode bs;
    bs.AddSample(MakeOwnedPose(2, 7.0f, 1.0f), 0.0f);
    bs.Sort();
    bs.SetParameter(0.5f);

    AnimationPose result;
    bs.Evaluate(MakeCtx(0.016f), result);

    EXPECT_NEAR(result.Positions[0].x, 7.0f, kEpsilon);
}

TEST(BlendSpace1DNode, TwoSamples_Interpolates)
{
    BlendSpace1DNode bs;
    bs.AddSample(MakeOwnedPose(2, 0.0f, 1.0f), 0.0f);
    bs.AddSample(MakeOwnedPose(2, 10.0f, 1.0f), 1.0f);
    bs.Sort();
    bs.SetParameter(0.5f);

    AnimationPose result;
    bs.Evaluate(MakeCtx(0.016f), result);

    EXPECT_NEAR(result.Positions[0].x, 5.0f, kEpsilon);
}

TEST(BlendSpace1DNode, ClampsBeyondRange)
{
    BlendSpace1DNode bs;
    bs.AddSample(MakeOwnedPose(2, 0.0f, 1.0f), 0.0f);
    bs.AddSample(MakeOwnedPose(2, 10.0f, 1.0f), 1.0f);
    bs.Sort();
    bs.SetParameter(5.0f);

    AnimationPose result;
    bs.Evaluate(MakeCtx(0.016f), result);

    EXPECT_NEAR(result.Positions[0].x, 10.0f, kEpsilon);
}

TEST(BlendSpace1DNode, ExactMatch_NoBlend)
{
    BlendSpace1DNode bs;
    bs.AddSample(MakeOwnedPose(2, 0.0f, 1.0f), 0.0f);
    bs.AddSample(MakeOwnedPose(2, 10.0f, 1.0f), 0.5f);
    bs.AddSample(MakeOwnedPose(2, 20.0f, 1.0f), 1.0f);
    bs.Sort();
    bs.SetParameter(0.5f);

    AnimationPose result;
    bs.Evaluate(MakeCtx(0.016f), result);

    // At parameter 0.5, we land exactly on the second sample, so we get nodeB.
    EXPECT_NEAR(result.Positions[0].x, 10.0f, kEpsilon);
}

// ---------------------------------------------------------------------------
// BlendSpace2DNode tests
// ---------------------------------------------------------------------------

TEST(BlendSpace2DNode, SingleSample_ReturnsIt)
{
    BlendSpace2DNode bs;
    bs.AddSample(MakeOwnedPose(2, 4.0f, 1.0f), 0.0f, 0.0f);
    bs.SetParameter(0.5f, 0.5f);

    AnimationPose result;
    bs.Evaluate(MakeCtx(0.016f), result);

    EXPECT_NEAR(result.Positions[0].x, 4.0f, kEpsilon);
}

TEST(BlendSpace2DNode, ExactMatch_ReturnsIt)
{
    BlendSpace2DNode bs;
    bs.AddSample(MakeOwnedPose(2, 1.0f, 1.0f), 0.0f, 0.0f);
    bs.AddSample(MakeOwnedPose(2, 9.0f, 1.0f), 1.0f, 1.0f);
    bs.SetParameter(1.0f, 1.0f);

    AnimationPose result;
    bs.Evaluate(MakeCtx(0.016f), result);

    EXPECT_NEAR(result.Positions[0].x, 9.0f, kEpsilon);
}

TEST(BlendSpace2DNode, Equidistant_EqualWeights)
{
    BlendSpace2DNode bs;
    bs.AddSample(MakeOwnedPose(2, 0.0f, 1.0f), -1.0f, 0.0f);
    bs.AddSample(MakeOwnedPose(2, 10.0f, 1.0f), 1.0f, 0.0f);
    bs.SetParameter(0.0f, 0.0f);

    AnimationPose result;
    bs.Evaluate(MakeCtx(0.016f), result);

    // Both samples equidistant (distance 1.0 each), so equal blend -> midpoint
    EXPECT_NEAR(result.Positions[0].x, 5.0f, kEpsilon);
}

TEST(BlendSpace2DNode, NamedParametersReadFromContext)
{
    BlendSpace2DNode bs;
    bs.AddSample(MakeOwnedPose(2, 0.0f, 1.0f), 0.0f, 0.0f);
    bs.AddSample(MakeOwnedPose(2, 10.0f, 1.0f), 1.0f, 0.0f);
    bs.SetParameterNameX("Speed");
    bs.SetParameter(0.0f, 0.0f);

    std::unordered_map<GameEngine::StringId, GraphParam> params;
    GraphParam speed;
    speed.Name = "Speed";
    speed.Value = 1.0f;
    params[HashStringId("Speed")] = speed;
    EvaluationContext& ctx = MakeCtx(0.016f);
    ctx.Parameters = &params;

    AnimationPose result;
    bs.Evaluate(ctx, result);
    EXPECT_NEAR(result.Positions[0].x, 10.0f, kEpsilon);
}

// ---------------------------------------------------------------------------
// BoneMask tests
// ---------------------------------------------------------------------------

TEST(BoneMask, Resize_DefaultWeight)
{
    BoneMask mask;
    mask.Resize(5, 0.75f);

    EXPECT_EQ(mask.Weights.size(), 5u);
    for (uint32_t i = 0; i < 5; ++i)
    {
        EXPECT_FLOAT_EQ(mask.Weights[i], 0.75f);
    }
}

TEST(BoneMask, CreateFromBoneAndDescendants_SimpleHierarchy)
{
    // 5-bone chain: 0 -> 1 -> 2 -> 3 -> 4
    std::vector<int32_t> parents = {-1, 0, 1, 2, 3};

    BoneMask mask = BoneMask::CreateFromBoneAndDescendants(2, parents, 5);

    EXPECT_EQ(mask.Weights.size(), 5u);
    EXPECT_FLOAT_EQ(mask.Weights[0], 0.0f);
    EXPECT_FLOAT_EQ(mask.Weights[1], 0.0f);
    EXPECT_FLOAT_EQ(mask.Weights[2], 1.0f);
    EXPECT_FLOAT_EQ(mask.Weights[3], 1.0f);
    EXPECT_FLOAT_EQ(mask.Weights[4], 1.0f);
}

// ---------------------------------------------------------------------------
// AnimationEvent tests
// ---------------------------------------------------------------------------

TEST(AnimationEventTrack, AddEvent_KeepsTimeOrderAndTheOrderAddedAtOneTime)
{
    AnimationEventTrack track;
    track.AddEvent({0.5f, "Hit", {}});
    track.AddEvent({0.1f, "FootDown", {}});
    track.AddEvent({0.8f, "FootUp", {}});
    track.AddEvent({0.5f, "Shout", {}});

    const auto& events = track.GetEvents();
    ASSERT_EQ(events.size(), 4u);
    EXPECT_FLOAT_EQ(events[0].Time, 0.1f);
    EXPECT_EQ(events[0].Name, "FootDown");
    EXPECT_FLOAT_EQ(events[1].Time, 0.5f);
    EXPECT_EQ(events[1].Name, "Hit");
    EXPECT_EQ(events[2].Name, "Shout");
    EXPECT_FLOAT_EQ(events[3].Time, 0.8f);
    EXPECT_EQ(events[3].Name, "FootUp");
}

TEST(AnimationEventCollector, AddAndRetrieve)
{
    AnimationEventCollector collector;
    AnimationEvent stepEvent{0.2f, "Step", {}};
    AnimationEvent jumpEvent{0.7f, "Jump", {}};
    collector.Add(stepEvent);
    collector.Add(jumpEvent);

    const auto& events = collector.GetEvents();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].Event.Name, "Step");
    EXPECT_EQ(events[1].Event.Name, "Jump");
}

TEST(AnimationEventCollector, Clear_EmptiesEvents)
{
    AnimationEventCollector collector;
    AnimationEvent eventA{0.1f, "A", {}};
    AnimationEvent eventB{0.2f, "B", {}};
    collector.Add(eventA);
    collector.Add(eventB);
    EXPECT_EQ(collector.GetEvents().size(), 2u);

    collector.Clear();
    EXPECT_TRUE(collector.GetEvents().empty());
}

// ---------------------------------------------------------------------------
// RootMotionExtractor tests
// ---------------------------------------------------------------------------

TEST(RootMotionExtractor, Extract_FirstFrame_NoDelta)
{
    AnimationPose pose = MakeTestPose(3, 5.0f, 1.0f);

    RootMotionExtractor extractor;
    auto delta = extractor.Extract(pose);

    EXPECT_NEAR(delta.Translation.x, 0.0f, kEpsilon);
    EXPECT_NEAR(delta.Translation.y, 0.0f, kEpsilon);
    EXPECT_NEAR(delta.Translation.z, 0.0f, kEpsilon);
}

TEST(RootMotionExtractor, Extract_SecondFrame_ComputesDelta)
{
    RootMotionExtractor extractor;

    AnimationPose frame1 = MakeTestPose(3, 0.0f, 1.0f);
    extractor.Extract(frame1);

    AnimationPose frame2 = MakeTestPose(3, 0.0f, 1.0f);
    frame2.Positions[0] = Vector3(3.0f, 1.0f, 2.0f);

    auto delta = extractor.Extract(frame2);

    EXPECT_NEAR(delta.Translation.x, 3.0f, kEpsilon);
    EXPECT_NEAR(delta.Translation.y, 1.0f, kEpsilon);
    EXPECT_NEAR(delta.Translation.z, 2.0f, kEpsilon);
}

TEST(RootMotionExtractor, Extract_ZerosBoneZero)
{
    RootMotionExtractor extractor;

    AnimationPose pose = MakeTestPose(3, 5.0f, 1.0f);
    extractor.Extract(pose);

    EXPECT_NEAR(pose.Positions[0].x, 0.0f, kEpsilon);
    EXPECT_NEAR(pose.Positions[0].y, 0.0f, kEpsilon);
    EXPECT_NEAR(pose.Positions[0].z, 0.0f, kEpsilon);

    // Other bones should be unaffected
    EXPECT_NEAR(pose.Positions[1].x, 5.0f, kEpsilon);
}

TEST(AnimationGraphPlayer, TryGetFloat_DoesNotInventZero)
{
    AnimationGraphPlayer player;
    float value = 99.0f;
    const GameEngine::StringId speedId = HashStringId("Speed");
    EXPECT_FALSE(player.TryGetFloat(speedId, value));
    EXPECT_FLOAT_EQ(value, 99.0f);

    player.SetFloat(speedId, 3.5f);
    ASSERT_TRUE(player.TryGetFloat(speedId, value));
    EXPECT_NEAR(value, 3.5f, kEpsilon);
}

TEST(AnimationGraphStore, Create_ReturnsIndependentPlayers)
{
    auto& store = AnimationGraphStore::Instance();
    store.ClearForTest();

    const GameEngine::StringId speedId = HashStringId("Speed");
    auto a = std::make_unique<AnimationGraphPlayer>();
    a->SetFloat(speedId, 1.0f);
    auto b = std::make_unique<AnimationGraphPlayer>();
    b->SetFloat(speedId, 2.0f);

    const uint64_t idA = store.Create(std::move(a));
    const uint64_t idB = store.Create(std::move(b));
    EXPECT_NE(idA, 0u);
    EXPECT_NE(idB, 0u);
    EXPECT_NE(idA, idB);

    float value = 0.0f;
    ASSERT_TRUE(store.Get(idA)->TryGetFloat(speedId, value));
    EXPECT_NEAR(value, 1.0f, kEpsilon);
    ASSERT_TRUE(store.Get(idB)->TryGetFloat(speedId, value));
    EXPECT_NEAR(value, 2.0f, kEpsilon);

    store.Destroy(idA);
    EXPECT_EQ(store.Get(idA), nullptr);
    ASSERT_NE(store.Get(idB), nullptr);

    store.ClearForTest();
    EXPECT_EQ(store.Get(idB), nullptr);
}

TEST(StateMachineNode, Transition_ViaContextParameters)
{
    auto idleNode = std::make_unique<ConstantPoseNode>();
    idleNode->FixedPose = MakeTestPose(2, 0.0f, 1.0f);
    auto runNode = std::make_unique<ConstantPoseNode>();
    runNode->FixedPose = MakeTestPose(2, 10.0f, 1.0f);

    StateMachineNode sm;
    const uint32_t idleIdx = sm.AddState("Idle", std::move(idleNode));
    const uint32_t runIdx = sm.AddState("Run", std::move(runNode));
    sm.AddTransition(idleIdx, runIdx, 1.0f, {
        TransitionCondition::Make("shouldRun", ParamCompare::Equals, ParamValue{true})
    });

    std::unordered_map<GameEngine::StringId, GraphParam> params;
    GraphParam flag;
    flag.Name = "shouldRun";
    flag.Value = true;
    params[HashStringId("shouldRun")] = flag;

    EvaluationContext ctx;
    ctx.DeltaTime = 0.0f;
    ctx.Parameters = &params;
    AnimationPose dummy;
    sm.Evaluate(ctx, dummy);

    params[HashStringId("shouldRun")].Value = false;
    ctx.DeltaTime = 0.5f;
    AnimationPose result;
    sm.Evaluate(ctx, result);
    EXPECT_NEAR(result.Positions[0].x, 5.0f, kEpsilon);
}

TEST(BlendSpace1DNode, NamedParameter_ReadsContext)
{
    BlendSpace1DNode bs;
    bs.AddSample(MakeOwnedPose(2, 0.0f, 1.0f), 0.0f);
    bs.AddSample(MakeOwnedPose(2, 10.0f, 1.0f), 1.0f);
    bs.Sort();
    bs.SetParameterName("Speed");

    std::unordered_map<GameEngine::StringId, GraphParam> params;
    GraphParam speed;
    speed.Name = "Speed";
    speed.Value = 0.5f;
    params[HashStringId("Speed")] = speed;

    EvaluationContext ctx;
    ctx.DeltaTime = 0.016f;
    ctx.Parameters = &params;
    AnimationPose result;
    bs.Evaluate(ctx, result);
    EXPECT_NEAR(result.Positions[0].x, 5.0f, kEpsilon);
}

TEST(AnimationGraphSerializer, ClipGuidAndConditionRoundTrip)
{
    auto clipNode = std::make_unique<ClipPlayerNode>();
    const GameEngine::GUID clipGuid = GameEngine::GUID::Generate();
    clipNode->SetClipGuid(clipGuid);

    auto idle = std::make_unique<ConstantPoseNode>();
    idle->FixedPose = MakeTestPose(1, 0.0f, 1.0f);
    auto run = std::make_unique<ConstantPoseNode>();
    run->FixedPose = MakeTestPose(1, 1.0f, 1.0f);

    auto sm = std::make_unique<StateMachineNode>();
    const uint32_t idleIdx = sm->AddState("Idle", std::move(idle));
    const uint32_t runIdx = sm->AddState("Run", std::move(run));
    sm->AddTransition(idleIdx, runIdx, 0.25f, {
        TransitionCondition::Make("Speed", ParamCompare::Greater, ParamValue{1.5f})
    });

    AnimationGraphPlayer player;
    player.SetParameter("Speed", ParamValue{0.0f});
    player.RootNode = std::move(sm);

    const nlohmann::json json = AnimationGraphSerializer::Serialize(player);
    auto loaded = AnimationGraphSerializer::Deserialize(json);
    ASSERT_NE(loaded, nullptr);

    float speed = -1.0f;
    ASSERT_TRUE(loaded->TryGetFloat(HashStringId("Speed"), speed));
    EXPECT_NEAR(speed, 0.0f, kEpsilon);

    auto* loadedSm = dynamic_cast<StateMachineNode*>(loaded->RootNode.get());
    ASSERT_NE(loadedSm, nullptr);
    ASSERT_EQ(loadedSm->GetStates().size(), 2u);
    ASSERT_EQ(loadedSm->GetStates()[0].Transitions.size(), 1u);
    const TransitionCondition& cond = loadedSm->GetStates()[0].Transitions[0].Conditions[0];
    EXPECT_EQ(cond.ParamName, "Speed");
    EXPECT_EQ(cond.ParamId, HashStringId("Speed"));
    EXPECT_EQ(cond.Op, ParamCompare::Greater);
    EXPECT_NEAR(ParamAsFloat(cond.Expected), 1.5f, kEpsilon);

    AnimationGraphPlayer clipPlayer;
    clipPlayer.RootNode = std::move(clipNode);
    const nlohmann::json clipJson = AnimationGraphSerializer::Serialize(clipPlayer);
    auto loadedClipPlayer = AnimationGraphSerializer::Deserialize(clipJson);
    ASSERT_NE(loadedClipPlayer, nullptr);
    auto* loadedClip = dynamic_cast<ClipPlayerNode*>(loadedClipPlayer->RootNode.get());
    ASSERT_NE(loadedClip, nullptr);
    EXPECT_EQ(loadedClip->GetClipGuid(), clipGuid);
}

TEST(AnimationGraphSerializer, HostileTypeIsNotString_ReturnsNull)
{
    nlohmann::json j = {{"rootNode", {{"type", 123}}}};
    EXPECT_NO_THROW({
        auto player = AnimationGraphSerializer::Deserialize(j);
        EXPECT_EQ(player, nullptr);
    });
}

TEST(AnimationGraphSerializer, MissingParamDefault_DoesNotThrow)
{
    nlohmann::json j = {
        {"parameters", {{"Speed", {{"type", "float"}}}}},
        {"rootNode", nullptr}
    };
    EXPECT_NO_THROW({
        auto player = AnimationGraphSerializer::Deserialize(j);
        ASSERT_NE(player, nullptr);
        float speed = 99.0f;
        EXPECT_FALSE(player->TryGetFloat(HashStringId("Speed"), speed));
        EXPECT_FLOAT_EQ(speed, 99.0f);
    });
}

TEST(AnimationGraphSerializer, UnknownNestedType_EvaluateDoesNotCrash)
{
    nlohmann::json j = {
        {"rootNode", {
            {"type", "StateMachine"},
            {"states", nlohmann::json::array({
                {{"name", "Idle"}, {"node", {{"type", "NotANode"}}}}
            })}
        }}
    };
    auto player = AnimationGraphSerializer::Deserialize(j);
    ASSERT_NE(player, nullptr);
    AnimationPose pose;
    EXPECT_NO_THROW(player->Evaluate(MakeCtx(0.016f), pose));
}

TEST(AnimParam, GreaterAliasAcceptsShortName)
{
    ParamCompare op = ParamCompare::Equals;
    ASSERT_TRUE(TryParamCompareFromName("greater", op));
    EXPECT_EQ(op, ParamCompare::Greater);
    ASSERT_TRUE(TryParamCompareFromName("less", op));
    EXPECT_EQ(op, ParamCompare::Less);
    ASSERT_TRUE(TryParamCompareFromName("greaterEqual", op));
    EXPECT_EQ(op, ParamCompare::GreaterEqual);
}

TEST(AnimationGraphSerializer, UnknownConditionOp_DoesNotFire)
{
    auto idle = std::make_unique<ConstantPoseNode>();
    idle->FixedPose = MakeTestPose(1, 0.0f, 1.0f);
    auto run = std::make_unique<ConstantPoseNode>();
    run->FixedPose = MakeTestPose(1, 10.0f, 1.0f);
    auto sm = std::make_unique<StateMachineNode>();
    const uint32_t idleIdx = sm->AddState("Idle", std::move(idle));
    const uint32_t runIdx = sm->AddState("Run", std::move(run));
    sm->AddTransition(idleIdx, runIdx, 0.0f, {
        TransitionCondition::Make("go", ParamCompare::Equals, ParamValue{true})
    });
    AnimationGraphPlayer player;
    player.SetParameter("go", ParamValue{true});
    player.RootNode = std::move(sm);
    nlohmann::json j = AnimationGraphSerializer::Serialize(player);
    j["rootNode"]["transitions"][0]["conditions"][0]["op"] = "bogus";
    auto loaded = AnimationGraphSerializer::Deserialize(j);
    ASSERT_NE(loaded, nullptr);
    AnimationPose pose;
    loaded->Evaluate(MakeCtx(0.0f), pose);
    auto* loadedSm = dynamic_cast<StateMachineNode*>(loaded->RootNode.get());
    ASSERT_NE(loadedSm, nullptr);
    EXPECT_EQ(loadedSm->GetActiveStateIndex(), 0u);
}

TEST(AnimationGraphStore, Destroy_StaleHandleMissesReusedSlot)
{
    auto& store = AnimationGraphStore::Instance();
    store.ClearForTest();
    const uint64_t first = store.Create(std::make_unique<AnimationGraphPlayer>());
    store.Destroy(first);
    EXPECT_EQ(store.Get(first), nullptr);
    const uint64_t second = store.Create(std::make_unique<AnimationGraphPlayer>());
    EXPECT_NE(second, 0u);
    EXPECT_NE(second, first);
    EXPECT_EQ(store.Get(first), nullptr);
    EXPECT_NE(store.Get(second), nullptr);
    store.ClearForTest();
}

// One more live entry than a 16-bit slot index can name: each store must keep every id reaching its own entry.
constexpr size_t kMoreThanA16BitIndex = 65537;

TEST(AnimationGraphStore, EveryLivePlayerKeepsAnIdOfItsOwnPastA16BitIndex)
{
    auto& store = AnimationGraphStore::Instance();
    store.ClearForTest();
    std::unordered_set<const AnimationGraphPlayer*> players;
    size_t unreachable = 0;
    for (size_t i = 0; i < kMoreThanA16BitIndex; ++i)
    {
        const auto id = store.Create(std::make_unique<AnimationGraphPlayer>());
        const AnimationGraphPlayer* player = store.Get(id);
        if (player)
            players.insert(player);
        else
            ++unreachable;
    }
    EXPECT_EQ(unreachable, 0u) << "an id the store handed out reaches no player";
    EXPECT_EQ(players.size() + unreachable, kMoreThanA16BitIndex) << "two ids reach one player";
    store.ClearForTest();
}

TEST(AnimationEventCollectorStore, EveryLiveCollectorKeepsAnIdOfItsOwnPastA16BitIndex)
{
    auto& store = AnimationEventCollectorStore::Instance();
    store.ClearForTest();
    std::unordered_set<const AnimationEventCollector*> collectors;
    size_t unreachable = 0;
    for (size_t i = 0; i < kMoreThanA16BitIndex; ++i)
    {
        const auto id = store.Create();
        const AnimationEventCollector* collector = store.Get(id);
        if (collector)
            collectors.insert(collector);
        else
            ++unreachable;
    }
    EXPECT_EQ(unreachable, 0u) << "an id the store handed out reaches no collector";
    EXPECT_EQ(collectors.size() + unreachable, kMoreThanA16BitIndex) << "two ids reach one collector";
    store.ClearForTest();
}

TEST(AnimationGraphSerializer, BlendSpaceChildIsOwned)
{
    nlohmann::json j = {
        {"rootNode", {
            {"type", "BlendSpace1D"},
            {"samples", nlohmann::json::array({
                {{"position", 0.0}, {"node", {{"type", "ClipPlayer"}, {"loop", true}}}}
            })}
        }}
    };
    auto player = AnimationGraphSerializer::Deserialize(j);
    ASSERT_NE(player, nullptr);
    auto* bs = dynamic_cast<BlendSpace1DNode*>(player->RootNode.get());
    ASSERT_NE(bs, nullptr);
    ASSERT_EQ(bs->GetSamples().size(), 1u);
    ASSERT_NE(bs->GetSamples()[0].Node, nullptr);
    player.reset();
}


