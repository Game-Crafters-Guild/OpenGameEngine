#include "Animation/AnimationEvent.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "ECSModules/Rendering/Systems/RigidAnimationSystem.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Engine/Rendering/AnimationSampling.h"
#include "GltfTestFiles.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

#include <gtest/gtest.h>

using namespace GameEngine;

class AnimatorClipTimeTests : public testing::Test
{
protected:
    void SetUp() override
    {
        Engine::Renderer::ClipStore::Instance().ClearForTest();
        const GUID guid = GUID::Generate();
        auto clip = MakeShared<AnimationClip>(guid, "Synthetic://ClipTime");
        clip->SetChannelsAndDurationForTest({}, 1.0f);
        Engine::Renderer::ClipStore::Instance().RegisterRuntimeClip(guid, clip);
        m_Animator.ClipIndex = Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(guid);
        ASSERT_NE(m_Animator.ClipIndex, 0u);
        m_Animator.Flags = Components::AnimatorRef::kFlag_Loop;
    }

    void TearDown() override
    {
        Engine::Renderer::ClipStore::Instance().ClearForTest();
    }

    void Tick(float seconds)
    {
        Engine::Renderer::TickAnimatorRef(m_Animator, Engine::Renderer::ClipStore::Instance(), seconds, nullptr);
    }

    Components::AnimatorRef m_Animator;
};

TEST_F(AnimatorClipTimeTests, OrdinaryWrapPreservesTheRemainder)
{
    m_Animator.Time = 0.875f;
    Tick(0.25f);
    EXPECT_FLOAT_EQ(m_Animator.Time, 0.125f);
}

TEST_F(AnimatorClipTimeTests, StepPastTwoToThe24LapsCompletes)
{
    m_Animator.Speed = 1.0e10f;
    Tick(1.0f);
    EXPECT_FLOAT_EQ(m_Animator.Time, 0.0f);
}

TEST_F(AnimatorClipTimeTests, SeekPastTwoToThe24LapsCompletes)
{
    m_Animator.Time = 1.0e10f;
    Tick(0.0f);
    EXPECT_FLOAT_EQ(m_Animator.Time, 0.0f);
}

TEST_F(AnimatorClipTimeTests, NonFiniteTimeRecoversToTheLapStart)
{
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()})
    {
        m_Animator.Time = invalid;
        Tick(0.0f);
        EXPECT_FLOAT_EQ(m_Animator.Time, 0.0f);
    }
}

TEST_F(AnimatorClipTimeTests, OneShotInfiniteTimeRestartsAndCollectsEventsFromZero)
{
    auto& clips = Engine::Renderer::ClipStore::Instance();
    const GUID guid = GUID::Generate();
    const auto clip = MakeShared<AnimationClip>(guid, "one-shot.glb");
    const std::string data = TestFiles::ClipGlb(5, 4.0f,
        R"({"clip":{"schemaVersion":1,"events":[{"frame":1,"name":"early"},{"frame":3,"name":"late"}]}})");
    ASSERT_TRUE(clip->LoadFromData(Vector<uint8>(data.begin(), data.end())));
    ASSERT_FLOAT_EQ(clip->GetDuration(), 1.0f);
    ASSERT_EQ(clip->GetEventTrack().GetEvents().size(), 2u);
    clips.RegisterRuntimeClip(guid, clip);
    m_Animator.ClipIndex = clips.GetIndexIfPresent(guid);
    ASSERT_NE(m_Animator.ClipIndex, 0u);
    m_Animator.Flags = 0;
    m_Animator.Time = std::numeric_limits<float>::infinity();
    Animation::AnimationEventCollector events;

    Engine::Renderer::TickAnimatorRef(m_Animator, clips, 0.5f, &events);

    EXPECT_FLOAT_EQ(m_Animator.Time, 0.5f);
    ASSERT_EQ(events.GetEvents().size(), 1u);
    EXPECT_EQ(events.GetEvents()[0].Event.Name, "early");
}

TEST_F(AnimatorClipTimeTests, NonFiniteSpeedDoesNotAdvance)
{
    m_Animator.Time = 0.375f;
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()})
    {
        m_Animator.Speed = invalid;
        Tick(0.25f);
        EXPECT_FLOAT_EQ(m_Animator.Time, 0.375f);
    }
}

TEST_F(AnimatorClipTimeTests, SectionWrapPreservesTheRemainder)
{
    m_Animator.Flags |= Components::AnimatorRef::kFlag_Section;
    m_Animator.SectionStart = 0.25f;
    m_Animator.SectionEnd = 0.75f;
    m_Animator.Time = 0.625f;
    Tick(0.25f);
    EXPECT_FLOAT_EQ(m_Animator.Time, 0.375f);
}

TEST_F(AnimatorClipTimeTests, SectionStepPastTwoToThe24LapsCompletes)
{
    m_Animator.Flags |= Components::AnimatorRef::kFlag_Section;
    m_Animator.SectionStart = 0.25f;
    m_Animator.SectionEnd = 0.75f;
    m_Animator.Time = 0.375f;
    m_Animator.Speed = 1.0e10f;
    Tick(1.0f);
    EXPECT_FLOAT_EQ(m_Animator.Time, 0.375f);
}

TEST_F(AnimatorClipTimeTests, FiniteSpeedAndDeltaDoNotOverflowTheWrap)
{
    m_Animator.Speed = std::numeric_limits<float>::max();
    Tick(2.0f);
    EXPECT_FLOAT_EQ(m_Animator.Time, 0.0f);
}

TEST_F(AnimatorClipTimeTests, OutgoingClipUsesTheSameBoundedWrap)
{
    m_Animator.PrevClipIndex = m_Animator.ClipIndex;
    m_Animator.PrevTime = 1.0e10f;
    m_Animator.BlendDuration = 1.0f;
    Tick(0.125f);
    EXPECT_FLOAT_EQ(m_Animator.Time, 0.125f);
    EXPECT_FLOAT_EQ(m_Animator.PrevTime, 0.125f);
}

namespace
{
uint32 CreateClipTimeSkeleton()
{
    auto& store = Engine::Renderer::SkeletonStore::Instance();
    const uint32 id = store.CreateSkeleton(1);
    auto* skeleton = store.Get(id);
    skeleton->RestTranslation = {1.0f, 0.0f, 0.0f};
    skeleton->RestRotation = {0.0f, 0.0f, 0.0f, 1.0f};
    skeleton->RestScale = {1.0f, 1.0f, 1.0f};
    skeleton->SkinJointCount = 1;
    skeleton->JointNodes = {0};
    for (const size_t diagonal : {0u, 5u, 10u, 15u})
    {
        skeleton->BindPose[diagonal] = 1.0f;
        skeleton->InverseBind[diagonal] = 1.0f;
    }
    return id;
}
}

TEST_F(AnimatorClipTimeTests, IndependentRetargetStepPastTwoToThe24LapsCompletes)
{
    ECS::World world(nullptr);
    auto& skeletons = Engine::Renderer::SkeletonStore::Instance();
    Components::SkeletonRef skeleton;
    skeleton.skeletonId = CreateClipTimeSkeleton();
    skeleton.runtimeId = skeletons.CreateRuntime(skeleton.skeletonId);
    Components::HumanoidRetargeterComponent retargeter;
    retargeter.Map.Set(GUID::Generate());
    retargeter.SourceClipIndex = m_Animator.ClipIndex;
    retargeter.ClipTimeSeconds = 0.375f;
    retargeter.Speed = 1.0e10f;
    retargeter.Loop = true;
    retargeter.LODOverrideEnabled = true;
    retargeter.LODOverride = Components::HumanoidRetargetLOD::PoseHold;
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, skeleton);
    world.AddComponentImmediate(entity, retargeter);

    Engine::Renderer::HumanoidRetargetSystem system(nullptr);
    system.Update(world, 1.0f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::HumanoidRetargeterComponent>(entity)->ClipTimeSeconds, 0.375f);
    skeletons.ReleaseRuntime(skeleton.runtimeId);
}

TEST_F(AnimatorClipTimeTests, PausedRigidAnimatorWithInfiniteTimeCompletes)
{
    ECS::World world(nullptr);
    Components::SkeletonRef skeleton;
    skeleton.skeletonId = CreateClipTimeSkeleton();
    m_Animator.Flags |= Components::AnimatorRef::kFlag_Paused;
    m_Animator.Time = std::numeric_limits<float>::infinity();
    Tick(0.25f);
    ASSERT_TRUE(std::isinf(m_Animator.Time)); // Paused playback leaves the incoming time untouched.
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, m_Animator);
    world.AddComponentImmediate(entity, skeleton);
    world.AddComponentImmediate(entity, Components::AnimatedNodeRef{0});
    world.AddComponentImmediate(entity, Components::Transform{});

    Engine::Renderer::RigidAnimationSystem system;
    system.Update(world, 0.25f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::AnimatorRef>(entity)->Time, 0.0f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::Transform>(entity)->matrix[12], 1.0f);
}
