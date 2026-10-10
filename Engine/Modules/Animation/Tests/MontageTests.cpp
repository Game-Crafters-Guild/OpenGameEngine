#include "Animation/Nodes/MontageSlotNode.h"

#include "Animation/AnimationMontage.h"
#include "Animation/AnimationPose.h"
#include "Animation/BoneMask.h"
#include "Animation/EvaluationContext.h"
#include "Animation/MontageInstance.h"
#include "Animation/Nodes/ClipPlayerNode.h"

#include <Mathematics/Quaternion.h>
#include <Mathematics/Vector3.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace GameEngine::Animation;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{

EvaluationContext g_TestCtx;
EvaluationContext& MakeCtx(float deltaTime)
{
    g_TestCtx = EvaluationContext{};
    g_TestCtx.DeltaTime = deltaTime;
    return g_TestCtx;
}

constexpr uint32_t kBoneCount = 4;
constexpr float kEpsilon = 1e-4f;

// A trivial source node that always returns a known pose.
class ConstantPoseNode : public AnimGraphNode
{
public:
    void SetPose(const AnimationPose& pose) { m_Pose = pose; }

    void Evaluate(EvaluationContext& /*ctx*/, AnimationPose& outPose) override
    {
        outPose = m_Pose;
    }

private:
    AnimationPose m_Pose;
};

std::unique_ptr<ConstantPoseNode> MakeOwnedPose(const AnimationPose& pose)
{
    auto node = std::make_unique<ConstantPoseNode>();
    node->SetPose(pose);
    return node;
}

AnimationPose MakeIdentityPose(uint32_t boneCount)
{
    AnimationPose pose;
    pose.Resize(boneCount);
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        pose.Positions[i] = Vector3(0.0f, 0.0f, 0.0f);
        pose.Rotations[i] = Quaternion::Identity();
        pose.Scales[i] = Vector3(1.0f, 1.0f, 1.0f);
    }
    return pose;
}

AnimationPose MakeOffsetPose(uint32_t boneCount, float offset)
{
    AnimationPose pose;
    pose.Resize(boneCount);
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        pose.Positions[i] = Vector3(offset, offset, offset);
        pose.Rotations[i] = Quaternion::Identity();
        pose.Scales[i] = Vector3(1.0f, 1.0f, 1.0f);
    }
    return pose;
}

bool PosesEqual(const AnimationPose& a, const AnimationPose& b, float epsilon = kEpsilon)
{
    if (a.BoneCount != b.BoneCount)
        return false;
    for (uint32_t i = 0; i < a.BoneCount; ++i)
    {
        if (std::abs(a.Positions[i].x - b.Positions[i].x) > epsilon) return false;
        if (std::abs(a.Positions[i].y - b.Positions[i].y) > epsilon) return false;
        if (std::abs(a.Positions[i].z - b.Positions[i].z) > epsilon) return false;
    }
    return true;
}

// Create a simple montage for testing.
AnimationMontage MakeTestMontage(float clipDuration,
                                 float blendIn,
                                 float blendOut,
                                 float playRate = 1.0f)
{
    AnimationMontage montage;
    montage.SetClipGuid("test-clip-guid");
    montage.SetClipDuration(clipDuration);
    montage.SetBlendInDuration(blendIn);
    montage.SetBlendOutDuration(blendOut);
    montage.SetPlayRate(playRate);
    return montage;
}

} // namespace

TEST(MontageSlotTests, PassThrough)
{
    AnimationPose sourcePose = MakeOffsetPose(kBoneCount, 5.0f);

    MontageSlotNode slot;
    slot.SetSource(MakeOwnedPose(sourcePose));

    AnimationPose result;
    result.Resize(kBoneCount);
    slot.Evaluate(MakeCtx(0.016f), result);

    // No montage playing: output should match source exactly.
    EXPECT_TRUE(PosesEqual(result, sourcePose));
}

TEST(MontageSlotTests, BlendIn)
{
    AnimationPose sourcePose = MakeIdentityPose(kBoneCount);

    constexpr float kBlendInDuration = 0.4f;
    constexpr float kClipDuration = 2.0f;
    AnimationMontage montage = MakeTestMontage(kClipDuration, kBlendInDuration, 0.2f);

    MontageSlotNode slot;
    slot.SetSource(MakeOwnedPose(sourcePose));
    slot.PlayMontage(&montage);

    AnimationPose result;
    result.Resize(kBoneCount);
    slot.Evaluate(MakeCtx(0.1f), result);
    EXPECT_TRUE(slot.IsPlaying());

    slot.Evaluate(MakeCtx(0.15f), result);
    slot.Evaluate(MakeCtx(0.15f), result);
    EXPECT_TRUE(slot.IsPlaying());
}

TEST(MontageSlotTests, BlendOut)
{
    AnimationPose sourcePose = MakeIdentityPose(kBoneCount);

    constexpr float kClipDuration = 1.0f;
    constexpr float kBlendOut = 0.2f;
    AnimationMontage montage = MakeTestMontage(kClipDuration, 0.0f, kBlendOut);

    MontageSlotNode slot;
    slot.SetSource(MakeOwnedPose(sourcePose));
    slot.PlayMontage(&montage);

    AnimationPose result;
    result.Resize(kBoneCount);

    for (int i = 0; i < 8; ++i)
        slot.Evaluate(MakeCtx(0.1f), result);

    EXPECT_TRUE(slot.IsPlaying());

    slot.Evaluate(MakeCtx(0.1f), result);
    EXPECT_TRUE(slot.IsPlaying());

    slot.Evaluate(MakeCtx(0.15f), result);
    EXPECT_FALSE(slot.IsPlaying());
}

TEST(MontageSlotTests, FullPlayback)
{
    AnimationPose sourcePose = MakeOffsetPose(kBoneCount, 3.0f);

    constexpr float kClipDuration = 0.5f;
    constexpr float kBlendIn = 0.1f;
    constexpr float kBlendOut = 0.1f;
    AnimationMontage montage = MakeTestMontage(kClipDuration, kBlendIn, kBlendOut);

    MontageSlotNode slot;
    slot.SetSource(MakeOwnedPose(sourcePose));
    slot.PlayMontage(&montage);

    EXPECT_TRUE(slot.IsPlaying());

    AnimationPose result;
    result.Resize(kBoneCount);

    float totalTime = 0.0f;
    constexpr float kStep = 0.05f;
    while (slot.IsPlaying() && totalTime < 2.0f)
    {
        slot.Evaluate(MakeCtx(kStep), result);
        totalTime += kStep;
    }

    EXPECT_FALSE(slot.IsPlaying());
    // After montage finishes, output returns to source pose pass-through.
    slot.Evaluate(MakeCtx(kStep), result);
    EXPECT_TRUE(PosesEqual(result, sourcePose));
}

TEST(MontageSlotTests, Interrupt)
{
    AnimationPose sourcePose = MakeIdentityPose(kBoneCount);

    constexpr float kClipDuration = 2.0f;
    AnimationMontage montageA = MakeTestMontage(kClipDuration, 0.1f, 0.2f);
    AnimationMontage montageB = MakeTestMontage(kClipDuration, 0.1f, 0.2f);

    MontageSlotNode slot;
    slot.SetSource(MakeOwnedPose(sourcePose));
    slot.PlayMontage(&montageA);

    AnimationPose result;
    result.Resize(kBoneCount);

    slot.Evaluate(MakeCtx(0.05f), result);
    slot.Evaluate(MakeCtx(0.05f), result);
    EXPECT_TRUE(slot.IsPlaying());

    slot.PlayMontage(&montageB);
    EXPECT_TRUE(slot.IsPlaying());

    slot.Evaluate(MakeCtx(0.05f), result);
    EXPECT_TRUE(slot.IsPlaying());
}

TEST(MontageSlotTests, BoneMask)
{
    AnimationPose sourcePose = MakeIdentityPose(kBoneCount);

    // Mask: bones 0 and 1 affected (weight 1.0), bones 2 and 3 not affected (weight 0.0)
    BoneMask mask;
    mask.Resize(kBoneCount, 0.0f);
    mask.Weights[0] = 1.0f;
    mask.Weights[1] = 1.0f;

    constexpr float kClipDuration = 2.0f;
    AnimationMontage montage = MakeTestMontage(kClipDuration, 0.0f, 0.0f);
    montage.SetBoneMask(&mask);

    MontageSlotNode slot;
    slot.SetSource(MakeOwnedPose(sourcePose));
    slot.PlayMontage(&montage);

    AnimationPose result;
    result.Resize(kBoneCount);

    slot.Evaluate(MakeCtx(0.016f), result);

    // Bones 2 and 3 should be unchanged (mask weight 0).
    EXPECT_NEAR(result.Positions[2].x, sourcePose.Positions[2].x, kEpsilon);
    EXPECT_NEAR(result.Positions[3].x, sourcePose.Positions[3].x, kEpsilon);
}

TEST(MontageInstanceTests, SectionJump)
{
    constexpr float kClipDuration = 3.0f;
    AnimationMontage montage = MakeTestMontage(kClipDuration, 0.1f, 0.1f);

    MontageSection windUp;
    windUp.Name = "WindUp";
    windUp.StartTime = 0.0f;
    windUp.EndTime = 1.0f;

    MontageSection strike;
    strike.Name = "Strike";
    strike.StartTime = 1.0f;
    strike.EndTime = 2.0f;

    MontageSection recovery;
    recovery.Name = "Recovery";
    recovery.StartTime = 2.0f;
    recovery.EndTime = 3.0f;

    montage.AddSection(windUp);
    montage.AddSection(strike);
    montage.AddSection(recovery);

    MontageInstance instance(&montage);

    EXPECT_NEAR(instance.GetCurrentTime(), 0.0f, kEpsilon);

    instance.JumpToSection("Strike");
    EXPECT_NEAR(instance.GetCurrentTime(), 1.0f, kEpsilon);

    instance.JumpToSection("Recovery");
    EXPECT_NEAR(instance.GetCurrentTime(), 2.0f, kEpsilon);

    instance.JumpToSection("NonExistent");
    EXPECT_NEAR(instance.GetCurrentTime(), 2.0f, kEpsilon);
}

TEST(MontageInstanceTests, BlendWeightRamp)
{
    constexpr float kClipDuration = 2.0f;
    constexpr float kBlendIn = 0.4f;
    constexpr float kBlendOut = 0.4f;
    AnimationMontage montage = MakeTestMontage(kClipDuration, kBlendIn, kBlendOut);

    MontageInstance instance(&montage);

    EXPECT_EQ(instance.GetState(), MontageInstance::State::BlendingIn);
    EXPECT_NEAR(instance.GetBlendWeight(), 0.0f, kEpsilon);

    instance.Update(0.2f);
    EXPECT_EQ(instance.GetState(), MontageInstance::State::BlendingIn);
    EXPECT_NEAR(instance.GetBlendWeight(), 0.5f, kEpsilon);

    instance.Update(0.2f);
    EXPECT_EQ(instance.GetState(), MontageInstance::State::Playing);
    EXPECT_NEAR(instance.GetBlendWeight(), 1.0f, kEpsilon);

    instance.Update(1.2f);
    EXPECT_EQ(instance.GetState(), MontageInstance::State::BlendingOut);

    instance.Update(0.2f);
    EXPECT_NEAR(instance.GetBlendWeight(), 0.5f, kEpsilon);

    instance.Update(0.2f);
    EXPECT_TRUE(instance.IsFinished());
    EXPECT_NEAR(instance.GetBlendWeight(), 0.0f, kEpsilon);
}

TEST(MontageInstanceTests, RequestBlendOut)
{
    constexpr float kClipDuration = 5.0f;
    constexpr float kBlendIn = 0.0f;
    constexpr float kBlendOut = 0.2f;
    AnimationMontage montage = MakeTestMontage(kClipDuration, kBlendIn, kBlendOut);

    MontageInstance instance(&montage);

    instance.Update(0.1f);
    EXPECT_EQ(instance.GetState(), MontageInstance::State::Playing);
    EXPECT_NEAR(instance.GetBlendWeight(), 1.0f, kEpsilon);

    instance.RequestBlendOut();
    EXPECT_EQ(instance.GetState(), MontageInstance::State::BlendingOut);

    instance.Update(0.1f);
    EXPECT_NEAR(instance.GetBlendWeight(), 0.5f, kEpsilon);

    instance.Update(0.1f);
    EXPECT_TRUE(instance.IsFinished());
}
