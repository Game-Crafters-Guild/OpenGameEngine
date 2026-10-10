#include <gtest/gtest.h>
#include "Core/Engine.h"
#include "Core/Application.h"
#include "Scripting/ScriptManager.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/AnimationGraphSystem.h"
#include "ECSModules/Rendering/Systems/AnimationGraphCompileFromModel.h"
#include "ECSModules/Rendering/Systems/CharacterGraphParamSystem.h"
#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphSerializer.h"
#include "Animation/AnimationGraphStore.h"
#include "Animation/Nodes/Blend2Node.h"
#include "Animation/Nodes/BlendSpace1DNode.h"
#include "Animation/Nodes/BlendSpace2DNode.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "Components/Transform.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Assets/AnimationClip.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Scripting/ScriptingABI.h"
#include "Engine/Rendering/AnimationSampling.h"
#include "Engine/Rendering/AnimatorScenePlayback.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/TimelinePlaybackSystem.h"
#include "Types/StringId.h"
#include "EngineLogCapture.h"
#include "StagedTestPaths.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// GE_* no-op stubs come from Engine/Source/HotReloadNativeStubs.cpp, added to
// this target via CMake to satisfy IncrementalCompilationTask's references
// without requiring a live CLR.

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <variant>
using namespace GameEngine;

namespace {
    static void SetIdentity(float* m) { for (int i=0;i<16;++i) m[i]=0.0f; m[0]=m[5]=m[10]=m[15]=1.0f; }
}


TEST(AnimationSystemTests, SamplesTranslationAndBuildsPalette) {
    // Initialize engine minimally (creates AssetManager used by system paths)
    ApplicationConfig config{};
    config.AssetDirectory = TestPaths::StagedEngineAssetsDir().string();

    ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    EXPECT_TRUE(EngineCore::GetInstance().Initialize(config));

    // Skeleton: 1 bone, identity inverse bind, via SkeletonStore
    auto& skelStore = Engine::Renderer::SkeletonStore::Instance();
    uint32 skeletonId = skelStore.CreateSkeleton(1);
    auto* skel = skelStore.Get(skeletonId);
    ASSERT_NE(skel, nullptr);
    skel->Parent = { -1 };
    skel->InverseBind.resize(16); SetIdentity(skel->InverseBind.data());
    skel->BindPose.resize(16);    SetIdentity(skel->BindPose.data());
    skel->RestTranslation = { 0.0f, 0.0f, 0.0f };
    skel->RestRotation    = { 0.0f, 0.0f, 0.0f, 1.0f };
    skel->RestScale       = { 1.0f, 1.0f, 1.0f };
    skel->SkinJointCount = 1;
    skel->JointNodes = { 0 };

    // Build a synthetic clip: translation channel from (0,0,0) at t=0 to (1,0,0) at t=1
    GUID clipGuid = GUID::Generate();
    AnimKeyframe k0{}; k0.time=0.0f; k0.translation[0]=0.0f; k0.translation[1]=0.0f; k0.translation[2]=0.0f; k0.rotation[3]=1.0f; k0.scale[0]=k0.scale[1]=k0.scale[2]=1.0f;
    AnimKeyframe k1{}; k1.time=1.0f; k1.translation[0]=1.0f; k1.translation[1]=0.0f; k1.translation[2]=0.0f; k1.rotation[3]=1.0f; k1.scale[0]=k1.scale[1]=k1.scale[2]=1.0f;

    AnimChannel ch{}; ch.boneIndex = 0; ch.path = AnimPath::Translation; ch.keys = {k0, k1};
    auto clip = MakeShared<AnimationClip>(clipGuid, std::filesystem::path("Synthetic://Clip"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);

    // Inject into clip cache
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    ASSERT_TRUE(Engine::Renderer::TestHooks::SetClipCacheForTest(clipGuid, clip));

    // AnimatorRef with fixed time (paused), pointing to the registered clip
    uint32 clipIndex = Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(clipGuid);
    ASSERT_NE(clipIndex, 0u);

    Components::AnimatorRef animRef{};
    animRef.ClipIndex = clipIndex;
    animRef.Time = 0.5f; // halfway between keyframes
    animRef.Flags = Components::AnimatorRef::kFlag_Paused;

    // Run test hook via the modern AnimatorRef path
    Engine::Renderer::TestHooks::SampleByRefsForTest(animRef, skeletonId, 1.0f/60.0f);

    // Verify skin palette in the per-entity runtime state that the hook created.
    const uint32 runtimeId = Engine::Renderer::TestHooks::GetLastTestRuntimeId();
    ASSERT_NE(runtimeId, 0u);
    auto* runtime = Engine::Renderer::SkeletonStore::Instance().GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 16u);
    EXPECT_NEAR(runtime->CompactSkinMatrices[12], 0.5f, 1e-4f);
    EXPECT_NEAR(runtime->CompactSkinMatrices[13], 0.0f, 1e-4f);
    EXPECT_NEAR(runtime->CompactSkinMatrices[14], 0.0f, 1e-4f);

    // Shutdown engine after test
    EngineCore::GetInstance().Shutdown();
}

TEST(AnimationSystemTests, SavesAndLoadsNativeAnimationClip)
{
    const std::filesystem::path tempPath =
        std::filesystem::temp_directory_path() / "gameengine-animation-save-load.anim";
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);

    AnimationClip clip(GUID::Generate(), std::filesystem::path());
    clip.SetSourceInfo("Source/Fox.glb", 2u);
    const size_t channelIndex = clip.FindOrAddChannel(7u, "Spine", AnimPath::Translation);
    ASSERT_TRUE(clip.AddKeyframe(channelIndex, 0.0f));
    ASSERT_TRUE(clip.AddKeyframe(channelIndex, 1.0f));
    ASSERT_TRUE(clip.SetKeyframeComponentValue(channelIndex, 1.0f, 0u, 2.5f));
    ASSERT_TRUE(clip.SaveToPath(tempPath));

    AnimationClip loaded(GUID::Generate(), tempPath);
    ASSERT_TRUE(loaded.Load());
    ASSERT_EQ(loaded.GetChannels().size(), 1u);
    EXPECT_EQ(loaded.GetChannels()[0].boneIndex, 7u);
    EXPECT_EQ(loaded.GetChannels()[0].targetName, "Spine");
    EXPECT_NEAR(loaded.GetDuration(), 1.0f, 1e-4f);
    EXPECT_EQ(loaded.GetSourcePath(), std::filesystem::path("Source/Fox.glb"));
    EXPECT_EQ(loaded.GetSourceAnimationIndex(), 2u);
    ASSERT_EQ(loaded.GetChannels()[0].keys.size(), 2u);
    EXPECT_NEAR(loaded.GetChannels()[0].keys[1].translation[0], 2.5f, 1e-4f);

    std::filesystem::remove(tempPath, ec);
}

TEST(AnimationSystemTests, SupportsAuthoringOperationsOnAnimationClip)
{
    AnimationClip clip(GUID::Generate(), std::filesystem::path());
    const size_t channelIndex = clip.FindOrAddChannel(0u, "Root", AnimPath::Scale);

    ASSERT_TRUE(clip.AddKeyframe(channelIndex, 0.0f));
    ASSERT_TRUE(clip.SetKeyframeComponentValue(channelIndex, 0.0f, 0u, 1.25f));
    ASSERT_TRUE(clip.DuplicateKeyframe(channelIndex, 0.0f, 0.5f));
    ASSERT_TRUE(clip.SetKeyframeTime(channelIndex, 0.5f, 1.0f));
    ASSERT_TRUE(clip.SetChannelInterpolation(channelIndex, AnimInterp::CubicSpline));
    ASSERT_TRUE(clip.RemoveKeyframe(channelIndex, 0.0f));

    ASSERT_EQ(clip.GetChannels().size(), 1u);
    EXPECT_EQ(clip.GetChannels()[0].interp, AnimInterp::CubicSpline);
    ASSERT_EQ(clip.GetChannels()[0].keys.size(), 1u);
    EXPECT_NEAR(clip.GetChannels()[0].keys[0].time, 1.0f, 1e-4f);
    EXPECT_NEAR(clip.GetChannels()[0].keys[0].scale[0], 1.25f, 1e-4f);
}

TEST(AnimationSystemTests, SetKeyframeTimeReSortsAndUpdatesDuration)
{
    // Build a synthetic clip with 3 translation keyframes at t=0, t=0.5, t=1.0
    AnimKeyframe k0{}; k0.time = 0.0f; k0.translation[0] = 0.0f; k0.rotation[3] = 1.0f; k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimKeyframe k1{}; k1.time = 0.5f; k1.translation[0] = 1.0f; k1.rotation[3] = 1.0f; k1.scale[0] = k1.scale[1] = k1.scale[2] = 1.0f;
    AnimKeyframe k2{}; k2.time = 1.0f; k2.translation[0] = 2.0f; k2.rotation[3] = 1.0f; k2.scale[0] = k2.scale[1] = k2.scale[2] = 1.0f;

    AnimChannel ch{}; ch.boneIndex = 0; ch.path = AnimPath::Translation; ch.keys = {k0, k1, k2};
    auto clip = MakeShared<AnimationClip>(GUID::Generate(), std::filesystem::path("Synthetic://SetKeyTest"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);

    // Move the middle keyframe (t=0.5) to t=1.5 — should re-sort and extend duration
    EXPECT_TRUE(clip->SetKeyframeTime(0, 0.5f, 1.5f));
    EXPECT_FLOAT_EQ(clip->GetDuration(), 1.5f);

    // Verify keys are re-sorted: t=0.0, t=1.0, t=1.5
    const auto& keys = clip->GetChannels()[0].keys;
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_FLOAT_EQ(keys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(keys[1].time, 1.0f);
    EXPECT_FLOAT_EQ(keys[2].time, 1.5f);

    // Verify the moved keyframe retains its data
    EXPECT_FLOAT_EQ(keys[2].translation[0], 1.0f);

    // Invalid channel index returns false
    EXPECT_FALSE(clip->SetKeyframeTime(99, 0.0f, 0.5f));

    // Non-existent time returns false
    EXPECT_FALSE(clip->SetKeyframeTime(0, 999.0f, 0.0f));

    // Negative time gets clamped to 0
    EXPECT_TRUE(clip->SetKeyframeTime(0, 1.0f, -1.0f));
    EXPECT_FLOAT_EQ(clip->GetChannels()[0].keys[0].time, 0.0f);
}

// =============================================================================
// IsClipSkeletonGPUSafe gate tests.
//
// Pre-condition for all tests: a 4-bone skeleton with the names {"Hips",
// "Spine", "End", "End"} so we exercise both unique-name and duplicate-name
// behavior. The duplicate-name pair models the real-world FBX `_End` leaf
// markers that broke the prior gate (resolved 1st instance, but channel
// authored against the 2nd).
//
// The new gate checks `BoneNameIds[channel.boneIndex] == channel.targetNameId`
// directly — duplicate names are NOT a problem as long as the stored index
// points at a bone whose name hashes to the channel's target. Cross-rig
// authoring (different bone ordering, but same names) fails because stored
// points to a bone with a DIFFERENT name in this skeleton.
// =============================================================================

namespace {

uint32 MakeDuplicateNameSkeleton()
{
    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    uint32 skeletonId = skStore.CreateSkeleton(4);
    auto* skel = skStore.Get(skeletonId);
    skel->BoneCount = 4;
    skel->Parent = { -1, 0, 1, 2 };
    skel->BoneNames = { "Hips", "Spine", "End", "End" };
    skel->RestTranslation.assign(12, 0.0f);
    skel->RestRotation = { 0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1 };
    skel->RestScale.assign(12, 1.0f);
    skel->BuildBoneNameLookup();
    return skeletonId;
}

uint32 RegisterClipForGateTest(std::vector<AnimChannel>&& channels)
{
    GUID g = GUID::Generate();
    auto clip = MakeShared<AnimationClip>(g, std::filesystem::path("Synthetic://GateTest"));
    clip->SetChannelsAndDurationForTest(std::move(channels), 1.0f);
    Engine::Renderer::TestHooks::SetClipCacheForTest(g, clip);
    return Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(g);
}

AnimChannel MakeRotationChannelByName(uint32 boneIndex, const char* name)
{
    AnimChannel ch{};
    ch.boneIndex = boneIndex;
    ch.targetName = name;
    ch.targetNameId = name && name[0] ? HashStringId(name) : 0u;
    ch.path = AnimPath::Rotation;
    ch.interp = AnimInterp::Linear;
    AnimKeyframe k{};
    k.rotation[3] = 1.0f;
    k.scale[0] = k.scale[1] = k.scale[2] = 1.0f;
    ch.keys = {k};
    return ch;
}

} // namespace

TEST(AnimationSystemGateTests, DuplicateNameSameFBX_PassesGate)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    uint32 skeletonId = MakeDuplicateNameSkeleton();

    // Same-FBX: each channel's stored boneIndex points to a bone whose name
    // matches its targetNameId. Two "End" channels target indices 2 and 3
    // — duplicate names but self-consistent storage.
    std::vector<AnimChannel> channels = {
        MakeRotationChannelByName(0, "Hips"),
        MakeRotationChannelByName(1, "Spine"),
        MakeRotationChannelByName(2, "End"), // first "End"
        MakeRotationChannelByName(3, "End"), // second "End" — would collapse via ResolveBoneIndex
    };
    uint32 clipIndex = RegisterClipForGateTest(std::move(channels));
    ASSERT_NE(clipIndex, 0u);

    EXPECT_TRUE(Engine::Renderer::TestHooks::IsClipSkeletonGPUSafeForTest(skeletonId, clipIndex))
        << "Self-consistent duplicate-name FBX clip should pass the new gate";
}

TEST(AnimationSystemGateTests, CrossRigPositionalMismatch_FailsGate)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    uint32 skeletonId = MakeDuplicateNameSkeleton();

    // Cross-rig: channel says "Spine" with stored index 0 (which is "Hips"
    // in THIS skeleton). BoneNameIds[0] = hash("Hips") != hash("Spine") →
    // should fail the gate and route to CPU.
    std::vector<AnimChannel> channels = {
        MakeRotationChannelByName(0, "Spine"), // wrong: index 0 is "Hips" here
    };
    uint32 clipIndex = RegisterClipForGateTest(std::move(channels));
    ASSERT_NE(clipIndex, 0u);

    EXPECT_FALSE(Engine::Renderer::TestHooks::IsClipSkeletonGPUSafeForTest(skeletonId, clipIndex))
        << "Cross-rig positional mismatch should fail the gate";
}

TEST(AnimationSystemGateTests, LegacyClipNullNameId_PassesGate)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    uint32 skeletonId = MakeDuplicateNameSkeleton();

    // Legacy clip: targetNameId=0 means trust stored boneIndex without
    // name-resolution check. Should pass even when bone has a different
    // (or empty) name — there's no name to verify against.
    std::vector<AnimChannel> channels = {
        MakeRotationChannelByName(0, ""), // empty name → targetNameId=0
        MakeRotationChannelByName(1, ""),
    };
    uint32 clipIndex = RegisterClipForGateTest(std::move(channels));
    ASSERT_NE(clipIndex, 0u);

    EXPECT_TRUE(Engine::Renderer::TestHooks::IsClipSkeletonGPUSafeForTest(skeletonId, clipIndex))
        << "Legacy clip (targetNameId=0) should pass the gate";
}

TEST(AnimationSystemGateTests, OutOfRangeBoneIndex_FailsGate)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    uint32 skeletonId = MakeDuplicateNameSkeleton();

    // Channel claims bone index 99 in a 4-bone skeleton — out of range.
    // Even if the name "Hips" is in the lookup at index 0, the stored
    // index doesn't point to a valid bone in THIS skeleton, so GPU
    // upload would write past the bone palette. Must fail.
    AnimChannel oob = MakeRotationChannelByName(0, "Hips");
    oob.boneIndex = 99;
    std::vector<AnimChannel> channels = { std::move(oob) };
    uint32 clipIndex = RegisterClipForGateTest(std::move(channels));
    ASSERT_NE(clipIndex, 0u);

    EXPECT_FALSE(Engine::Renderer::TestHooks::IsClipSkeletonGPUSafeForTest(skeletonId, clipIndex))
        << "Out-of-range stored boneIndex must fail the gate";
}

// The GPU animation kernel holds 256 bones of the joint closure (the skin's joints and their
// ancestors) and walks 64 hierarchy levels; a skeleton over either limit animates on the CPU
// path and says so once, naming its model.
namespace {

// A skeleton of `parent.size()` nodes whose skin names `jointNodes`, finished the way the model
// loaders finish one, from the model file `modelName`.
uint32 MakeSkinnedSkeleton(std::vector<int32> parent, std::vector<uint32> jointNodes, const char* modelName)
{
    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 nodeCount = static_cast<uint32>(parent.size());
    const uint32 skeletonId = skStore.CreateSkeleton(nodeCount);
    auto* skel = skStore.Get(skeletonId);
    skel->BoneCount = nodeCount;
    skel->Parent = std::move(parent);
    skel->SkinJointCount = static_cast<uint32>(jointNodes.size());
    skel->JointNodes = std::move(jointNodes);
    skel->SourceModelPath = std::filesystem::path("Synthetic") / modelName;
    skel->ComputeTopologicalSort();
    return skeletonId;
}

// `nodeCount` nodes, node 0 the root of all others; the skin names nodes 0 .. jointCount - 1.
uint32 MakeFlatSkeleton(uint32 nodeCount, uint32 jointCount, const char* modelName)
{
    std::vector<int32> parent(nodeCount, 0);
    parent[0] = -1;
    std::vector<uint32> jointNodes(jointCount);
    for (uint32 joint = 0; joint < jointCount; ++joint)
        jointNodes[joint] = joint;
    return MakeSkinnedSkeleton(std::move(parent), std::move(jointNodes), modelName);
}

// A chain of `levels` joints, one per hierarchy level.
uint32 MakeChainSkeleton(uint32 levels, const char* modelName)
{
    std::vector<int32> parent(levels);
    std::vector<uint32> jointNodes(levels);
    for (uint32 node = 0; node < levels; ++node)
    {
        parent[node] = static_cast<int32>(node) - 1;
        jointNodes[node] = node;
    }
    return MakeSkinnedSkeleton(std::move(parent), std::move(jointNodes), modelName);
}

struct GpuRoute
{
    bool GpuSafe = false;
    std::vector<std::string> Warnings;
};

// Routes a one-channel clip on `skeletonId`, capturing the engine's warnings.
GpuRoute RouteOneChannelClip(uint32 skeletonId)
{
    const uint32 clipIndex = RegisterClipForGateTest({MakeRotationChannelByName(0, "")});
    GpuRoute route;
    {
        TestLog::ScopedEngineLogCapture capture(&route.Warnings, Logger::LogLevel::Warning);
        Logger::Log::Warning("AnimationSystemGateTests sink is live");
        route.GpuSafe = Engine::Renderer::TestHooks::IsClipSkeletonGPUSafeForTest(skeletonId, clipIndex);
        Logger::Log::Flush();
    }
    return route;
}

} // namespace

TEST(AnimationSystemGateTests, JointClosureOverTheBoneLimitRoutesToCpuWithAWarning)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    const GpuRoute atLimit = RouteOneChannelClip(MakeFlatSkeleton(256, 256, "AtBoneLimit.glb"));
    const uint32 overLimitSkeleton = MakeFlatSkeleton(257, 257, "OverBoneLimit.glb");
    const GpuRoute overLimit = RouteOneChannelClip(overLimitSkeleton);
    // A second clip, through a second animation system, on the same skeleton.
    const GpuRoute overLimitAgain = RouteOneChannelClip(overLimitSkeleton);

    ASSERT_EQ(TestLog::CountLinesContaining(atLimit.Warnings, "sink is live"), 1u);
    EXPECT_TRUE(atLimit.GpuSafe);
    EXPECT_EQ(TestLog::CountLinesContaining(atLimit.Warnings, "AtBoneLimit.glb"), 0u);

    ASSERT_EQ(TestLog::CountLinesContaining(overLimit.Warnings, "sink is live"), 1u);
    EXPECT_FALSE(overLimit.GpuSafe);
    ASSERT_EQ(TestLog::CountLinesContaining(overLimit.Warnings, "OverBoneLimit.glb"), 1u);
    const std::string warning = TestLog::FirstLineContaining(overLimit.Warnings, "OverBoneLimit.glb");
    EXPECT_NE(warning.find("257"), std::string::npos) << warning;
    EXPECT_NE(warning.find("256"), std::string::npos) << warning;
    EXPECT_NE(warning.find("CPU"), std::string::npos) << warning;

    ASSERT_EQ(TestLog::CountLinesContaining(overLimitAgain.Warnings, "sink is live"), 1u);
    EXPECT_FALSE(overLimitAgain.GpuSafe);
    EXPECT_EQ(TestLog::CountLinesContaining(overLimitAgain.Warnings, "OverBoneLimit.glb"), 0u)
        << "the warning is logged once per skeleton, not once per animation system";
}

TEST(AnimationSystemGateTests, NodesOutsideTheJointClosureDoNotCountTowardTheBoneLimit)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    // 300 nodes under one root; the skin names the root and 99 of its children, 100 closure bones.
    const GpuRoute route = RouteOneChannelClip(MakeFlatSkeleton(300, 100, "DecoratedRig.glb"));

    ASSERT_EQ(TestLog::CountLinesContaining(route.Warnings, "sink is live"), 1u);
    EXPECT_TRUE(route.GpuSafe);
    EXPECT_EQ(TestLog::CountLinesContaining(route.Warnings, "DecoratedRig.glb"), 0u);
}

TEST(AnimationSystemGateTests, HierarchyDeeperThanTheLevelLimitRoutesToCpuWithAWarning)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    const GpuRoute atLimit = RouteOneChannelClip(MakeChainSkeleton(64, "AtLevelLimit.glb"));
    const GpuRoute overLimit = RouteOneChannelClip(MakeChainSkeleton(65, "OverLevelLimit.glb"));

    ASSERT_EQ(TestLog::CountLinesContaining(atLimit.Warnings, "sink is live"), 1u);
    EXPECT_TRUE(atLimit.GpuSafe);
    EXPECT_EQ(TestLog::CountLinesContaining(atLimit.Warnings, "AtLevelLimit.glb"), 0u);

    ASSERT_EQ(TestLog::CountLinesContaining(overLimit.Warnings, "sink is live"), 1u);
    EXPECT_FALSE(overLimit.GpuSafe);
    ASSERT_EQ(TestLog::CountLinesContaining(overLimit.Warnings, "OverLevelLimit.glb"), 1u);
    const std::string warning = TestLog::FirstLineContaining(overLimit.Warnings, "OverLevelLimit.glb");
    EXPECT_NE(warning.find("65"), std::string::npos) << warning;
    EXPECT_NE(warning.find("64"), std::string::npos) << warning;
    EXPECT_NE(warning.find("CPU"), std::string::npos) << warning;
}

TEST(AnimatorRefSetAnimation, SameClipIsNoOp)
{
    Components::AnimatorRef anim{};
    anim.ClipIndex = 7;
    anim.Time = 0.4f;
    anim.SetAnimation(7, 0.2f);
    EXPECT_EQ(anim.ClipIndex, 7u);
    EXPECT_FLOAT_EQ(anim.Time, 0.4f);
    EXPECT_FALSE(anim.IsBlending());
}

TEST(AnimatorRefSetAnimation, ZeroBlendHardCuts)
{
    Components::AnimatorRef anim{};
    anim.ClipIndex = 3;
    anim.Time = 0.8f;
    anim.SetAnimation(9, 0.0f);
    EXPECT_EQ(anim.ClipIndex, 9u);
    EXPECT_FLOAT_EQ(anim.Time, 0.0f);
    EXPECT_EQ(anim.PrevClipIndex, 0u);
    EXPECT_FALSE(anim.IsBlending());
}

TEST(AnimatorRefSetAnimation, NonFiniteBlendHardCuts)
{
    Components::AnimatorRef anim{};
    anim.ClipIndex = 3;
    anim.Time = 0.8f;
    anim.SetAnimation(9, std::numeric_limits<float>::quiet_NaN());
    EXPECT_FALSE(anim.IsBlending());
    anim.ClipIndex = 3;
    anim.Time = 0.8f;
    anim.SetAnimation(9, std::numeric_limits<float>::infinity());
    EXPECT_FALSE(anim.IsBlending());
}

TEST(AnimatorRefSetAnimation, CrossfadeStashesPrevious)
{
    Components::AnimatorRef anim{};
    anim.ClipIndex = 3;
    anim.Time = 0.8f;
    anim.SetAnimation(9, 0.25f);
    EXPECT_EQ(anim.ClipIndex, 9u);
    EXPECT_FLOAT_EQ(anim.Time, 0.0f);
    EXPECT_EQ(anim.PrevClipIndex, 3u);
    EXPECT_FLOAT_EQ(anim.PrevTime, 0.8f);
    EXPECT_FLOAT_EQ(anim.BlendDuration, 0.25f);
    EXPECT_TRUE(anim.IsBlending());
    EXPECT_NEAR(anim.BlendAlpha(), 0.0f, 1e-6f);
}

TEST(AnimatorRefSetAnimation, ReverseKeepsRemainingFade)
{
    Components::AnimatorRef anim{};
    anim.ClipIndex = 3;
    anim.Time = 0.5f;
    anim.SetAnimation(9, 0.20f);
    anim.BlendTime = 0.05f;
    anim.SetAnimation(3, 0.20f);
    EXPECT_EQ(anim.ClipIndex, 3u);
    EXPECT_FLOAT_EQ(anim.Time, 0.5f);
    EXPECT_EQ(anim.PrevClipIndex, 9u);
    EXPECT_NEAR(anim.BlendTime, 0.15f, 1e-6f);
    EXPECT_TRUE(anim.IsBlending());
}

TEST(AnimatorRefSetAnimation, ClearStopsPlayback)
{
    Components::AnimatorRef anim{};
    anim.ClipIndex = 3;
    anim.SetAnimation(9, 0.2f);
    anim.SetAnimation(0);
    EXPECT_EQ(anim.ClipIndex, 0u);
    EXPECT_FALSE(anim.IsBlending());
}

TEST(AnimationSystemTests, SetAnimationCrossfadeMidpointLerpsTranslation)
{
    ApplicationConfig config{};
    config.AssetDirectory = TestPaths::StagedEngineAssetsDir().string();
    ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
    ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));

    auto& skelStore = Engine::Renderer::SkeletonStore::Instance();
    uint32 skeletonId = skelStore.CreateSkeleton(1);
    auto* skel = skelStore.Get(skeletonId);
    ASSERT_NE(skel, nullptr);
    skel->Parent = { -1 };
    skel->InverseBind.resize(16); SetIdentity(skel->InverseBind.data());
    skel->BindPose.resize(16);    SetIdentity(skel->BindPose.data());
    skel->RestTranslation = { 0.0f, 0.0f, 0.0f };
    skel->RestRotation    = { 0.0f, 0.0f, 0.0f, 1.0f };
    skel->RestScale       = { 1.0f, 1.0f, 1.0f };
    skel->SkinJointCount = 1;
    skel->JointNodes = { 0 };

    auto makeConstX = [](float x) {
        AnimKeyframe k0{};
        k0.time = 0.0f;
        k0.translation[0] = x;
        k0.rotation[3] = 1.0f;
        k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
        AnimKeyframe k1 = k0;
        k1.time = 1.0f;
        AnimChannel ch{};
        ch.boneIndex = 0;
        ch.path = AnimPath::Translation;
        ch.keys = {k0, k1};
        GUID guid = GUID::Generate();
        auto clip = MakeShared<AnimationClip>(guid, std::filesystem::path("Synthetic://Blend"));
        clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);
        Engine::Renderer::TestHooks::SetClipCacheForTest(guid, clip);
        return Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(guid);
    };

    Engine::Renderer::TestHooks::ClearClipCacheForTest();
    const uint32 clipA = makeConstX(0.0f);
    const uint32 clipB = makeConstX(2.0f);
    ASSERT_NE(clipA, 0u);
    ASSERT_NE(clipB, 0u);

    Components::AnimatorRef anim{};
    anim.ClipIndex = clipA;
    anim.Time = 0.0f;
    anim.Flags = Components::AnimatorRef::kFlag_Paused;
    anim.SetAnimation(clipB, 0.20f);
    anim.BlendTime = 0.10f;
    anim.Flags = Components::AnimatorRef::kFlag_Paused;

    Engine::Renderer::TestHooks::SampleByRefsForTest(anim, skeletonId, 0.0f);
    const uint32 runtimeId = Engine::Renderer::TestHooks::GetLastTestRuntimeId();
    ASSERT_NE(runtimeId, 0u);
    auto* runtime = Engine::Renderer::SkeletonStore::Instance().GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 16u);
    EXPECT_NEAR(runtime->CompactSkinMatrices[12], 1.0f, 1e-4f);

    EngineCore::GetInstance().Shutdown();
}

TEST(AnimatorRefSetAnimation, PlayStateAndCrossFadeSecondsMatchSetAnimation)
{
    Components::AnimatorRef anim{};
    anim.ClipIndex = 3;
    anim.Time = 0.8f;
    anim.PlayState(9);
    EXPECT_EQ(anim.ClipIndex, 9u);
    EXPECT_FLOAT_EQ(anim.Time, 0.0f);
    EXPECT_FALSE(anim.IsBlending());

    anim.ClipIndex = 3;
    anim.Time = 0.8f;
    anim.CrossFadeSeconds(9, 0.25f);
    EXPECT_EQ(anim.ClipIndex, 9u);
    EXPECT_EQ(anim.PrevClipIndex, 3u);
    EXPECT_FLOAT_EQ(anim.BlendDuration, 0.25f);
    EXPECT_TRUE(anim.IsBlending());
}

namespace {

struct ClipCommandFixture
{
    ApplicationConfig config{};
    GUID clipAGuid{};
    GUID clipBGuid{};
    uint32 clipA = 0;
    uint32 clipB = 0;

    bool Init()
    {
        config.AssetDirectory = TestPaths::StagedEngineAssetsDir().string();
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        if (!EngineCore::GetInstance().Initialize(config))
            return false;

        auto makeClip = [](float x) {
            AnimKeyframe k0{};
            k0.time = 0.0f;
            k0.translation[0] = x;
            k0.rotation[3] = 1.0f;
            k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
            AnimKeyframe k1 = k0;
            k1.time = 1.0f;
            AnimChannel ch{};
            ch.boneIndex = 0;
            ch.path = AnimPath::Translation;
            ch.keys = {k0, k1};
            GUID guid = GUID::Generate();
            auto clip = MakeShared<AnimationClip>(guid, std::filesystem::path("Synthetic://ClipCmd"));
            clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);
            Engine::Renderer::TestHooks::SetClipCacheForTest(guid, clip);
            return guid;
        };

        Engine::Renderer::TestHooks::ClearClipCacheForTest();
        clipAGuid = makeClip(0.0f);
        clipBGuid = makeClip(2.0f);
        clipA = Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(clipAGuid);
        clipB = Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(clipBGuid);
        return clipA != 0 && clipB != 0;
    }

    void Shutdown()
    {
        Engine::Renderer::TestHooks::ClearClipCacheForTest();
        EngineCore::GetInstance().Shutdown();
    }
};

ECS::EntityHandle MakeClipEntity(ECS::World& world, Components::Animator animator)
{
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    Components::AnimatorRef animRef{};
    animRef.Flags = Components::AnimatorRef::kFlag_Loop;
    world.AddComponentImmediate(e, animRef);
    world.AddComponentImmediate(e, Components::SkeletonRef{});
    return e;
}

} // namespace

TEST(ClipAnimatorCommands, PlayStateAppliesClipToAnimatorRef)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayState(fx.clipAGuid);
    EXPECT_EQ(animator.pendingCommand, Components::AnimatorPlaybackCommand::Play);
    EXPECT_EQ(animator.source, Components::AnimatorPlaybackSource::Clip);

    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);

    const auto* cmd = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->pendingCommand, Components::AnimatorPlaybackCommand::None);
    const auto* ref = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->ClipIndex, fx.clipA);
    EXPECT_FLOAT_EQ(ref->Time, 0.0f);
    EXPECT_FALSE(ref->IsPaused());
    EXPECT_FALSE(ref->IsBlending());

    fx.Shutdown();
}

TEST(ClipAnimatorCommands, CrossFadeSecondsStashesPreviousClip)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayState(fx.clipAGuid);
    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);

    auto* playing = world.GetComponentForWrite<Components::Animator>(e);
    ASSERT_NE(playing, nullptr);
    playing->CrossFadeSeconds(fx.clipBGuid, 0.20f);
    auto* refBefore = world.GetComponentForWrite<Components::AnimatorRef>(e);
    ASSERT_NE(refBefore, nullptr);
    refBefore->Time = 0.40f;
    world.AddComponentImmediate(e, *refBefore);

    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* ref = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->ClipIndex, fx.clipB);
    EXPECT_EQ(ref->PrevClipIndex, fx.clipA);
    EXPECT_FLOAT_EQ(ref->PrevTime, 0.40f);
    EXPECT_FLOAT_EQ(ref->BlendDuration, 0.20f);
    EXPECT_TRUE(ref->IsBlending());

    fx.Shutdown();
}

TEST(ClipAnimatorCommands, TimelineSourceLeavesPendingCommand)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayTimeline(GUID::Generate());
    EXPECT_EQ(animator.pendingCommand, Components::AnimatorPlaybackCommand::Play);
    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* cmd = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->pendingCommand, Components::AnimatorPlaybackCommand::Play);
    EXPECT_EQ(cmd->source, Components::AnimatorPlaybackSource::Timeline);

    fx.Shutdown();
}

TEST(ClipAnimatorCommands, LibrarySourceDropsPendingCommand)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayState(fx.clipAGuid);
    animator.source = Components::AnimatorPlaybackSource::Library;
    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* cmd = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->pendingCommand, Components::AnimatorPlaybackCommand::None);
    const auto* ref = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->ClipIndex, 0u);

    fx.Shutdown();
}

TEST(ClipAnimatorCommands, PauseStopSeek)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayState(fx.clipAGuid);
    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);

    auto* cmd = world.GetComponentForWrite<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    cmd->Pause();
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* paused = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(paused, nullptr);
    EXPECT_TRUE(paused->IsPaused());
    EXPECT_EQ(paused->ClipIndex, fx.clipA);

    cmd = world.GetComponentForWrite<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    cmd->Seek(0.35f);
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* sought = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(sought, nullptr);
    EXPECT_FALSE(sought->IsPaused());
    EXPECT_FLOAT_EQ(sought->Time, 0.35f);

    cmd = world.GetComponentForWrite<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    cmd->Stop();
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* stopped = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(stopped, nullptr);
    EXPECT_EQ(stopped->ClipIndex, 0u);
    EXPECT_TRUE(stopped->IsPaused());

    fx.Shutdown();
}

TEST(ClipAnimatorCommands, SameClipPlayStateDoesNotRestartTime)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayState(fx.clipAGuid);
    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);

    auto* ref = world.GetComponentForWrite<Components::AnimatorRef>(e);
    ASSERT_NE(ref, nullptr);
    ref->Time = 0.55f;
    world.AddComponentImmediate(e, *ref);

    auto* cmd = world.GetComponentForWrite<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    cmd->PlayState(fx.clipAGuid);
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* after = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->ClipIndex, fx.clipA);
    EXPECT_FLOAT_EQ(after->Time, 0.55f);

    fx.Shutdown();
}

TEST(ClipAnimatorCommands, TimelinePlaybackSystemConsumesClipCommands)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayState(fx.clipBGuid);
    const ECS::EntityHandle e = MakeClipEntity(world, animator);

    Engine::Renderer::TimelinePlaybackSystem system(nullptr);
    system.Update(world, 0.016f);

    const auto* cmd = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->pendingCommand, Components::AnimatorPlaybackCommand::None);
    const auto* ref = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->ClipIndex, fx.clipB);

    fx.Shutdown();
}

TEST(ClipAnimatorCommands, ZeroCrossFadeIsHardCut)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayState(fx.clipAGuid);
    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);

    auto* ref = world.GetComponentForWrite<Components::AnimatorRef>(e);
    ASSERT_NE(ref, nullptr);
    ref->Time = 0.30f;
    world.AddComponentImmediate(e, *ref);

    auto* cmd = world.GetComponentForWrite<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    cmd->CrossFadeSeconds(fx.clipBGuid, 0.0f);
    EXPECT_EQ(cmd->pendingCommand, Components::AnimatorPlaybackCommand::Play);
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* after = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->ClipIndex, fx.clipB);
    EXPECT_FALSE(after->IsBlending());
    EXPECT_FLOAT_EQ(after->Time, 0.0f);

    fx.Shutdown();
}

TEST(AnimatorGraphParams, SetFloatOnClipSourceIsNoOp)
{
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Clip;
    animator.SetFloat("Speed", 4.0f);
    EXPECT_EQ(animator.source, Components::AnimatorPlaybackSource::Clip);
    EXPECT_EQ(animator.pendingGraphParamCount, 0u);
}

TEST(ClipAnimatorCommands, GraphSourceLeavesPendingCommand)
{
    ClipCommandFixture fx;
    ASSERT_TRUE(fx.Init());

    ECS::World world;
    Components::Animator animator{};
    animator.PlayGraph(GUID::Generate());
    animator.Pause();
    EXPECT_EQ(animator.pendingCommand, Components::AnimatorPlaybackCommand::Pause);
    EXPECT_EQ(animator.source, Components::AnimatorPlaybackSource::Graph);
    const ECS::EntityHandle e = MakeClipEntity(world, animator);
    Engine::Renderer::ProcessClipAnimatorCommands(world);
    const auto* cmd = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->pendingCommand, Components::AnimatorPlaybackCommand::Pause);
    EXPECT_EQ(cmd->source, Components::AnimatorPlaybackSource::Graph);

    fx.Shutdown();
}

namespace {

void FillIdentityInverseBind(GameEngine::Animation::SkeletonData* skel)
{
    skel->SkinJointCount = skel->BoneCount;
    skel->JointNodes.resize(skel->BoneCount);
    for (uint32 i = 0; i < skel->BoneCount; ++i)
    {
        skel->JointNodes[i] = i;
        float* m = &skel->InverseBind[static_cast<size_t>(i) * 16u];
        for (uint32 e = 0; e < 16; ++e)
            m[e] = 0.0f;
        m[0] = 1.0f;
        m[5] = 1.0f;
        m[10] = 1.0f;
        m[15] = 1.0f;
    }
}

SharedPtr<AnimationClip> MakeTranslationClip(float x)
{
    AnimKeyframe k0{};
    k0.time = 0.0f;
    k0.translation[0] = x;
    k0.rotation[3] = 1.0f;
    k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimKeyframe k1 = k0;
    k1.time = 1.0f;
    AnimChannel ch{};
    ch.boneIndex = 0;
    ch.path = AnimPath::Translation;
    ch.keys = {k0, k1};
    auto clip = MakeShared<AnimationClip>(GUID::Generate(), std::filesystem::path("Synthetic://GraphClip"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);
    return clip;
}

SharedPtr<AnimationClip> MakeRootTravelClip()
{
    AnimKeyframe k0{};
    k0.time = 0.0f;
    k0.rotation[3] = 1.0f;
    k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimKeyframe k1 = k0;
    k1.time = 1.0f;
    k1.translation[0] = 4.0f;
    AnimChannel ch{};
    ch.boneIndex = 0;
    ch.path = AnimPath::Translation;
    ch.keys = {k0, k1};
    auto clip = MakeShared<AnimationClip>(GUID::Generate(), std::filesystem::path("Synthetic://RootTravel"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);
    return clip;
}

} // namespace

TEST(AnimationGraphHost, EvaluateWritesCompactSkinMatricesAndClearsClipIndex)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;
    using GameEngine::Animation::ClipPlayerNode;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();

    auto node = std::make_unique<ClipPlayerNode>();
    node->SetClip(MakeTranslationClip(2.0f));
    auto player = std::make_unique<AnimationGraphPlayer>();
    player->RootNode = std::move(node);
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skelId = skStore.CreateSkeleton(1);
    auto* skel = skStore.Get(skelId);
    ASSERT_NE(skel, nullptr);
    FillIdentityInverseBind(skel);
    const uint32 runtimeId = skStore.CreateRuntime(skelId);
    ASSERT_NE(runtimeId, 0u);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    Components::AnimatorRef animRef{};
    animRef.ClipIndex = 7;
    world.AddComponentImmediate(e, animRef);
    Components::SkeletonRef sref{};
    sref.skeletonId = skelId;
    sref.runtimeId = runtimeId;
    world.AddComponentImmediate(e, sref);

    Engine::Renderer::AnimationGraphSystem system(nullptr);
    system.Update(world, 0.0f);

    const auto* afterRef = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(afterRef, nullptr);
    EXPECT_EQ(afterRef->ClipIndex, 0u);

    auto* runtime = skStore.GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 16u);
    EXPECT_NEAR(runtime->CompactSkinMatrices[12], 2.0f, 1e-4f);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, SetFloatPendingAppliesToPlayer)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    auto player = std::make_unique<AnimationGraphPlayer>();
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    animator.SetFloat("Speed", 4.0f);
    EXPECT_EQ(animator.pendingGraphParamCount, 1u);
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);

    Engine::Renderer::AnimationGraphSystem system(nullptr);
    system.Update(world, 0.016f);

    auto* live = graphStore.Get(graphId);
    ASSERT_NE(live, nullptr);
    float speed = 0.0f;
    ASSERT_TRUE(live->TryGetFloat(GameEngine::HashStringId("Speed"), speed));
    EXPECT_NEAR(speed, 4.0f, 1e-4f);

    const auto* after = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->pendingGraphParamCount, 0u);

    graphStore.ClearForTest();
}

namespace {

struct GraphCharacter
{
    ECS::EntityHandle Entity;
    uint32 RuntimeId = 0;
};

// A one-bone skinned character driven by a paused graph rooted at `root`.
GraphCharacter AddPausedGraphCharacter(ECS::World& world, std::unique_ptr<GameEngine::Animation::AnimGraphNode> root)
{
    auto player = std::make_unique<GameEngine::Animation::AnimationGraphPlayer>();
    player->RootNode = std::move(root);
    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skelId = skStore.CreateSkeleton(1);
    FillIdentityInverseBind(skStore.Get(skelId));

    GraphCharacter character;
    character.RuntimeId = skStore.CreateRuntime(skelId);
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = GameEngine::Animation::AnimationGraphStore::Instance().Create(std::move(player));
    animator.graphPaused = true;
    character.Entity = world.CreateEntity();
    world.AddComponentImmediate(character.Entity, animator);
    world.AddComponentImmediate(character.Entity, Components::AnimatorRef{});
    Components::SkeletonRef sref{};
    sref.skeletonId = skelId;
    sref.runtimeId = character.RuntimeId;
    world.AddComponentImmediate(character.Entity, sref);
    return character;
}

std::unique_ptr<GameEngine::Animation::ClipPlayerNode> TranslationClipNode(float x)
{
    auto clip = std::make_unique<GameEngine::Animation::ClipPlayerNode>();
    clip->SetClip(MakeTranslationClip(x));
    return clip;
}

} // namespace

// Every palette notification advances the world's caster version, which re-arms the
// depth-derived passes, so the graph system notifies only when a palette can differ.
TEST(AnimationGraphHost, GraphAtRestLetsAStillSceneSettle)
{
    // A graph character whose inputs do not change must stop notifying, and a parameter
    // that moves its pose must not.
    auto& graphStore = GameEngine::Animation::AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    auto blend = std::make_unique<GameEngine::Animation::BlendSpace1DNode>();
    for (const float x : {0.0f, 2.0f})
        blend->AddSample(TranslationClipNode(x), x * 0.5f);
    blend->SetParameterName("Speed");
    ECS::World world;
    const GraphCharacter character = AddPausedGraphCharacter(world, std::move(blend));

    Engine::Renderer::RenderServices services;
    Engine::Renderer::AnimationGraphSystem system(&services);
    const auto casterVersion = [&] { return services.ShadowCasterContentVersion(world.GetWorldId()); };
    system.Update(world, 0.1f);
    const auto before = casterVersion();
    for (int frame = 0; frame < 3; ++frame)
        system.Update(world, 0.1f);
    EXPECT_EQ(casterVersion(), before) << "a graph whose inputs are unchanged re-poses nothing";

    world.GetComponentForWrite<Components::Animator>(character.Entity)->SetFloat("Speed", 1.0f);
    system.Update(world, 0.1f);
    EXPECT_GT(casterVersion(), before) << "a parameter that moves the pose re-poses the character";
    auto* runtime = Engine::Renderer::SkeletonStore::Instance().GetRuntime(character.RuntimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 16u);
    EXPECT_NEAR(runtime->CompactSkinMatrices[12], 2.0f, 1e-4f);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, GraphCharacterJoiningOrLeavingIsReported)
{
    // A palette that appears or disappears is a change even when every remaining pose
    // is unchanged.
    auto& graphStore = GameEngine::Animation::AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    ECS::World world;
    AddPausedGraphCharacter(world, TranslationClipNode(1.0f));

    Engine::Renderer::RenderServices services;
    Engine::Renderer::AnimationGraphSystem system(&services);
    const auto casterVersion = [&] { return services.ShadowCasterContentVersion(world.GetWorldId()); };
    system.Update(world, 0.1f);
    system.Update(world, 0.1f);
    auto before = casterVersion();

    const GraphCharacter joined = AddPausedGraphCharacter(world, TranslationClipNode(1.0f));
    system.Update(world, 0.1f);
    EXPECT_GT(casterVersion(), before) << "a character joining adds a palette";
    before = casterVersion();
    system.Update(world, 0.1f);
    EXPECT_EQ(casterVersion(), before) << "both characters are at rest";

    world.GetComponentForWrite<Components::Animator>(joined.Entity)->active = false;
    system.Update(world, 0.1f);
    EXPECT_GT(casterVersion(), before) << "a character leaving removes a palette";
    before = casterVersion();
    system.Update(world, 0.1f);
    EXPECT_EQ(casterVersion(), before) << "the remaining character is at rest";

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, PausedGraphThatStillChangesPoseIsReported)
{
    // Graph nodes hold state no input sees: with zero delta time and unchanged parameters,
    // a chain of zero-duration transitions moves the pose one state per update.
    using GameEngine::Animation::StateMachineNode;
    auto& graphStore = GameEngine::Animation::AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    auto machine = std::make_unique<StateMachineNode>();
    for (const float x : {0.0f, 1.0f, 2.0f})
        machine->AddState("At" + std::to_string(static_cast<int>(x)), TranslationClipNode(x));
    machine->AddTransition(0, 1, 0.0f, {});
    machine->AddTransition(1, 2, 0.0f, {});
    ECS::World world;
    const GraphCharacter character = AddPausedGraphCharacter(world, std::move(machine));

    Engine::Renderer::RenderServices services;
    Engine::Renderer::AnimationGraphSystem system(&services);
    const auto casterVersion = [&] { return services.ShadowCasterContentVersion(world.GetWorldId()); };
    system.Update(world, 0.1f);
    auto before = casterVersion();
    system.Update(world, 0.1f);
    EXPECT_GT(casterVersion(), before) << "the second transition moved the pose";
    auto* runtime = Engine::Renderer::SkeletonStore::Instance().GetRuntime(character.RuntimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 16u);
    EXPECT_NEAR(runtime->CompactSkinMatrices[12], 2.0f, 1e-4f);

    before = casterVersion();
    system.Update(world, 0.1f);
    system.Update(world, 0.1f);
    EXPECT_EQ(casterVersion(), before) << "the last state has no transition, so the pose holds";

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, ClipPlayerHugeSpeedDoesNotHang)
{
    using GameEngine::Animation::ClipPlayerNode;
    using GameEngine::Animation::EvaluationContext;
    using GameEngine::Animation::AnimationPose;

    ClipPlayerNode node;
    node.SetClip(MakeTranslationClip(1.0f));
    node.SetLooping(true);
    node.SetSpeed(std::numeric_limits<float>::infinity());
    EvaluationContext ctx;
    ctx.DeltaTime = 0.016f;
    AnimationPose pose;
    node.Evaluate(ctx, pose);
    node.SetSpeed(1.0e20f);
    node.Evaluate(ctx, pose);
    EXPECT_TRUE(std::isfinite(node.GetTime()));
    EXPECT_GE(node.GetTime(), 0.0f);
    EXPECT_LE(node.GetTime(), 1.0f);
}

TEST(AnimationGraphHost, MissingGraphGuidLatchesAndClearsClipIndex)
{
    using GameEngine::Animation::AnimationGraphStore;

    ApplicationConfig config{};
    config.AssetDirectory = TestPaths::StagedEngineAssetsDir().string();
    ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
    ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();

    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skelId = skStore.CreateSkeleton(1);
    auto* skel = skStore.Get(skelId);
    ASSERT_NE(skel, nullptr);
    FillIdentityInverseBind(skel);
    const uint32 runtimeId = skStore.CreateRuntime(skelId);
    ASSERT_NE(runtimeId, 0u);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphGuid.Set(GUID::Generate());
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    Components::AnimatorRef animRef{};
    animRef.ClipIndex = 7;
    world.AddComponentImmediate(e, animRef);
    Components::SkeletonRef sref{};
    sref.skeletonId = skelId;
    sref.runtimeId = runtimeId;
    world.AddComponentImmediate(e, sref);

    Engine::Renderer::AnimationGraphSystem system(nullptr);
    system.Update(world, 0.016f);
    system.Update(world, 0.016f);

    const auto* after = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->graphRuntimeId, 0u);
    EXPECT_EQ(after->graphInstanceGuid, animator.graphGuid.ToGuid());
    const auto* afterRef = world.GetComponent<Components::AnimatorRef>(e);
    ASSERT_NE(afterRef, nullptr);
    EXPECT_EQ(afterRef->ClipIndex, 0u);

    graphStore.ClearForTest();
    EngineCore::GetInstance().Shutdown();
}

TEST(AnimationGraphHost, ZeroingCopiedGraphHandleSparesOriginalOnDestroy)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    const uint64 graphId = graphStore.Create(std::make_unique<AnimationGraphPlayer>());
    ASSERT_NE(graphId, 0u);

    ECS::World world;
    Engine::Renderer::RegisterRenderWorldHooks(world);
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    const ECS::EntityHandle original = world.CreateEntity();
    world.AddComponentImmediate(original, animator);

    const ECS::EntityHandle clone = world.CloneEntity(original);
    ASSERT_TRUE(clone.IsValid());
    auto* cloned = world.GetComponent<Components::Animator>(clone);
    ASSERT_NE(cloned, nullptr);
    Components::Animator repaired = *cloned;
    repaired.graphRuntimeId = 0;
    repaired.graphInstanceGuid = GUID{};
    repaired.pendingGraphParamCount = 0;
    world.AddComponentImmediate(clone, repaired);

    world.DestroyEntityImmediate(clone);
    EXPECT_NE(graphStore.Get(graphId), nullptr);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, DestroyEntityReleasesStoreSlot)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    const uint64 graphId = graphStore.Create(std::make_unique<AnimationGraphPlayer>());
    ASSERT_NE(graphId, 0u);
    auto& collectorStore = GameEngine::Animation::AnimationEventCollectorStore::Instance();
    const uint64 collectorId = collectorStore.Create();
    ASSERT_NE(collectorId, 0u);

    ECS::World world;
    Engine::Renderer::RegisterRenderWorldHooks(world);
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    animator.eventCollectorId = collectorId;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.DestroyEntityImmediate(e);

    EXPECT_EQ(graphStore.Get(graphId), nullptr);
    EXPECT_EQ(collectorStore.Get(collectorId), nullptr);
    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, CharacterGraphParamsStampSpeedAndGrounded)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    auto player = std::make_unique<AnimationGraphPlayer>();
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    Components::CharacterController character{};
    character.desiredVelocityX = 3.0f;
    character.desiredVelocityY = 100.0f;
    character.desiredVelocityZ = -4.0f;
    character.grounded = true;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.AddComponentImmediate(e, character);

    Engine::Renderer::CharacterGraphParamSystem params;
    params.Update(world, 0.016f);

    const auto* stamped = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(stamped, nullptr);
    EXPECT_EQ(stamped->pendingGraphParamCount, 2u);

    Engine::Renderer::AnimationGraphSystem graph(nullptr);
    graph.Update(world, 0.016f);

    auto* live = graphStore.Get(graphId);
    ASSERT_NE(live, nullptr);
    float speed = 0.0f;
    ASSERT_TRUE(live->TryGetFloat(GameEngine::HashStringId("Speed"), speed));
    EXPECT_NEAR(speed, 5.0f, 1e-4f);
    bool grounded = false;
    ASSERT_TRUE(live->TryGetBool(GameEngine::HashStringId("Grounded"), grounded));
    EXPECT_TRUE(grounded);

    const auto* after = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->pendingGraphParamCount, 0u);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, CharacterGraphParamsClipSourceIsNoOp)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    auto player = std::make_unique<AnimationGraphPlayer>();
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Clip;
    animator.graphRuntimeId = graphId;
    Components::CharacterController character{};
    character.desiredVelocityX = 3.0f;
    character.desiredVelocityZ = 4.0f;
    character.grounded = true;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.AddComponentImmediate(e, character);

    Engine::Renderer::CharacterGraphParamSystem params;
    params.Update(world, 0.016f);

    const auto* after = world.GetComponent<Components::Animator>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->pendingGraphParamCount, 0u);

    auto* live = graphStore.Get(graphId);
    ASSERT_NE(live, nullptr);
    float speed = 99.0f;
    EXPECT_FALSE(live->TryGetFloat(GameEngine::HashStringId("Speed"), speed));
    EXPECT_FLOAT_EQ(speed, 99.0f);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, CharacterGraphParamsNonFiniteWishIsZeroSpeed)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();
    auto player = std::make_unique<AnimationGraphPlayer>();
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    Components::CharacterController character{};
    character.desiredVelocityX = std::numeric_limits<float>::infinity();
    character.desiredVelocityZ = 4.0f;
    character.grounded = false;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.AddComponentImmediate(e, character);

    Engine::Renderer::CharacterGraphParamSystem params;
    params.Update(world, 0.016f);
    Engine::Renderer::AnimationGraphSystem graph(nullptr);
    graph.Update(world, 0.016f);

    auto* live = graphStore.Get(graphId);
    ASSERT_NE(live, nullptr);
    float speed = 99.0f;
    ASSERT_TRUE(live->TryGetFloat(GameEngine::HashStringId("Speed"), speed));
    EXPECT_FLOAT_EQ(speed, 0.0f);
    bool grounded = true;
    ASSERT_TRUE(live->TryGetBool(GameEngine::HashStringId("Grounded"), grounded));
    EXPECT_FALSE(grounded);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, RootMotionStampsCharacterDisplacement)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;
    using GameEngine::Animation::ClipPlayerNode;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();

    auto node = std::make_unique<ClipPlayerNode>();
    node->SetClip(MakeRootTravelClip());
    auto player = std::make_unique<AnimationGraphPlayer>();
    player->RootNode = std::move(node);
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skelId = skStore.CreateSkeleton(1);
    auto* skel = skStore.Get(skelId);
    ASSERT_NE(skel, nullptr);
    FillIdentityInverseBind(skel);
    const uint32 runtimeId = skStore.CreateRuntime(skelId);
    ASSERT_NE(runtimeId, 0u);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    animator.rootMotionLocal = true;
    Components::CharacterController character{};
    character.desiredVelocityX = 99.0f;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.AddComponentImmediate(e, character);
    world.AddComponentImmediate(e, Components::Transform{});
    Components::AnimatorRef animRef{};
    world.AddComponentImmediate(e, animRef);
    Components::SkeletonRef sref{};
    sref.skeletonId = skelId;
    sref.runtimeId = runtimeId;
    world.AddComponentImmediate(e, sref);

    Engine::Renderer::AnimationGraphSystem system(nullptr);
    system.Update(world, 0.25f);
    const auto* first = world.GetComponent<Components::CharacterController>(e);
    ASSERT_NE(first, nullptr);
    EXPECT_FALSE(first->hasAnimationDisplacement);

    system.Update(world, 0.25f);
    const auto* stamped = world.GetComponent<Components::CharacterController>(e);
    ASSERT_NE(stamped, nullptr);
    ASSERT_TRUE(stamped->hasAnimationDisplacement);
    EXPECT_NEAR(stamped->animationDisplacementX, 1.0f, 1e-3f);
    EXPECT_NEAR(stamped->animationDisplacementZ, 0.0f, 1e-3f);
    EXPECT_FLOAT_EQ(stamped->desiredVelocityX, 99.0f);

    auto* runtime = skStore.GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 16u);
    EXPECT_NEAR(runtime->CompactSkinMatrices[12], 0.0f, 1e-3f);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, RootMotionOffDoesNotStampDisplacement)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;
    using GameEngine::Animation::ClipPlayerNode;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();

    auto node = std::make_unique<ClipPlayerNode>();
    node->SetClip(MakeRootTravelClip());
    auto player = std::make_unique<AnimationGraphPlayer>();
    player->RootNode = std::move(node);
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skelId = skStore.CreateSkeleton(1);
    auto* skel = skStore.Get(skelId);
    ASSERT_NE(skel, nullptr);
    FillIdentityInverseBind(skel);
    const uint32 runtimeId = skStore.CreateRuntime(skelId);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    animator.rootMotionLocal = false;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.AddComponentImmediate(e, Components::CharacterController{});
    Components::AnimatorRef animRef{};
    world.AddComponentImmediate(e, animRef);
    Components::SkeletonRef sref{};
    sref.skeletonId = skelId;
    sref.runtimeId = runtimeId;
    world.AddComponentImmediate(e, sref);

    Engine::Renderer::AnimationGraphSystem system(nullptr);
    system.Update(world, 0.25f);
    system.Update(world, 0.25f);

    const auto* cc = world.GetComponent<Components::CharacterController>(e);
    ASSERT_NE(cc, nullptr);
    EXPECT_FALSE(cc->hasAnimationDisplacement);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, RootMotionAppliesToTransformWithoutCharacter)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;
    using GameEngine::Animation::ClipPlayerNode;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();

    auto node = std::make_unique<ClipPlayerNode>();
    node->SetClip(MakeRootTravelClip());
    auto player = std::make_unique<AnimationGraphPlayer>();
    player->RootNode = std::move(node);
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skelId = skStore.CreateSkeleton(1);
    auto* skel = skStore.Get(skelId);
    ASSERT_NE(skel, nullptr);
    FillIdentityInverseBind(skel);
    const uint32 runtimeId = skStore.CreateRuntime(skelId);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    animator.rootMotionLocal = true;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.AddComponentImmediate(e, Components::Transform{});
    Components::AnimatorRef animRef{};
    world.AddComponentImmediate(e, animRef);
    Components::SkeletonRef sref{};
    sref.skeletonId = skelId;
    sref.runtimeId = runtimeId;
    world.AddComponentImmediate(e, sref);

    Engine::Renderer::AnimationGraphSystem system(nullptr);
    system.Update(world, 0.25f);
    system.Update(world, 0.25f);

    const auto* transform = world.GetComponent<Components::Transform>(e);
    ASSERT_NE(transform, nullptr);
    EXPECT_NEAR(transform->GetPosition().x, 1.0f, 1e-3f);

    graphStore.ClearForTest();
}

TEST(AnimationGraphHost, RootMotionPausedZerosPoseWithoutStamping)
{
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;
    using GameEngine::Animation::ClipPlayerNode;

    auto& graphStore = AnimationGraphStore::Instance();
    graphStore.ClearForTest();

    auto node = std::make_unique<ClipPlayerNode>();
    node->SetClip(MakeRootTravelClip());
    auto player = std::make_unique<AnimationGraphPlayer>();
    player->RootNode = std::move(node);
    const uint64 graphId = graphStore.Create(std::move(player));
    ASSERT_NE(graphId, 0u);

    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skelId = skStore.CreateSkeleton(1);
    auto* skel = skStore.Get(skelId);
    ASSERT_NE(skel, nullptr);
    FillIdentityInverseBind(skel);
    const uint32 runtimeId = skStore.CreateRuntime(skelId);

    ECS::World world;
    Components::Animator animator{};
    animator.source = Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    animator.rootMotionLocal = true;
    animator.graphPaused = true;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, animator);
    world.AddComponentImmediate(e, Components::CharacterController{});
    world.AddComponentImmediate(e, Components::Transform{});
    Components::AnimatorRef animRef{};
    world.AddComponentImmediate(e, animRef);
    Components::SkeletonRef sref{};
    sref.skeletonId = skelId;
    sref.runtimeId = runtimeId;
    world.AddComponentImmediate(e, sref);

    Engine::Renderer::AnimationGraphSystem system(nullptr);
    system.Update(world, 0.25f);
    system.Update(world, 0.25f);

    const auto* cc = world.GetComponent<Components::CharacterController>(e);
    ASSERT_NE(cc, nullptr);
    EXPECT_FALSE(cc->hasAnimationDisplacement);
    auto* runtime = skStore.GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 16u);
    EXPECT_NEAR(runtime->CompactSkinMatrices[12], 0.0f, 1e-3f);

    graphStore.ClearForTest();
}

TEST(AnimationGraphCompileFromModel, LooksLikeAuthoringModelDetectsKindAndNodes)
{
    using Engine::Renderer::LooksLikeAuthoringModel;
    EXPECT_TRUE(LooksLikeAuthoringModel(nlohmann::json::parse(R"({"kind":"animation","nodes":[]})")));
    EXPECT_FALSE(LooksLikeAuthoringModel(nlohmann::json{{"rootNode", {{"type", "ClipPlayer"}}}}));
    const nlohmann::json both = {
        {"rootNode", {{"type", "ClipPlayer"}}},
        {"nodes", nlohmann::json::array()}
    };
    EXPECT_FALSE(LooksLikeAuthoringModel(both));
}

TEST(AnimationGraphCompileFromModel, ClipPlayerToOutputPose)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "parameters": {"speed": 2.0, "looping": false}
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* clip = dynamic_cast<Animation::ClipPlayerNode*>(player->RootNode.get());
    ASSERT_NE(clip, nullptr);
    EXPECT_FLOAT_EQ(clip->GetSpeed(), 2.0f);
    EXPECT_FALSE(clip->GetLooping());
    EXPECT_TRUE(clip->GetClipGuid().IsNull());
}

TEST(AnimationGraphCompileFromModel, ClipPlayerBindsClipGuid)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "parameters": {"clipGuid": "01234567-89ab-cdef-0123-456789abcdef"}
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* clip = dynamic_cast<Animation::ClipPlayerNode*>(player->RootNode.get());
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->GetClipGuid(), GUID("01234567-89ab-cdef-0123-456789abcdef"));
}

TEST(AnimationGraphCompileFromModel, ClipPlayerInvalidClipGuidIsNull)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "parameters": {"clipGuid": "not-a-guid"}
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* clip = dynamic_cast<Animation::ClipPlayerNode*>(player->RootNode.get());
    ASSERT_NE(clip, nullptr);
    EXPECT_TRUE(clip->GetClipGuid().IsNull());
}

TEST(AnimationGraphCompileFromModel, Blend2OfTwoClipPlayers)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clipA",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "clipB",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "blend",
          "typeId": "Blend2",
          "pins": [
            {"id": "a", "direction": "in", "dataType": "pose"},
            {"id": "b", "direction": "in", "dataType": "pose"},
            {"id": "poseOut", "direction": "out", "dataType": "pose"}
          ],
          "parameters": {"weight": 0.25}
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "la",
          "sourceNodeId": "clipA",
          "sourcePinId": "poseOut",
          "targetNodeId": "blend",
          "targetPinId": "a"
        },
        {
          "id": "lb",
          "sourceNodeId": "clipB",
          "sourcePinId": "poseOut",
          "targetNodeId": "blend",
          "targetPinId": "b"
        },
        {
          "id": "lo",
          "sourceNodeId": "blend",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* blend = dynamic_cast<Animation::Blend2Node*>(player->RootNode.get());
    ASSERT_NE(blend, nullptr);
    EXPECT_FLOAT_EQ(blend->GetWeight(), 0.25f);
    EXPECT_NE(dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetInputA()), nullptr);
    EXPECT_NE(dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetInputB()), nullptr);
}

TEST(AnimationGraphCompileFromModel, OutputPoseCountMustBeOne)
{
    const nlohmann::json zero = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        }
      ],
      "links": []
    })JSON");
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(zero), nullptr);

    const nlohmann::json two = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "outA",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        },
        {
          "id": "outB",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": []
    })JSON");
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(two), nullptr);
}

TEST(AnimationGraphCompileFromModel, BlendSpace1DTwoSamplesToOutputPose)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "bs",
          "typeId": "BlendSpace1D",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "parameters": {"parameter": "Speed"},
          "extensions": {
            "blendSpace1D": {
              "samples": [
                {"position": 0.0, "label": "idle"},
                {"position": 1.5, "label": "run"}
              ]
            }
          }
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "bs",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* blend = dynamic_cast<Animation::BlendSpace1DNode*>(player->RootNode.get());
    ASSERT_NE(blend, nullptr);
    EXPECT_EQ(blend->GetParameterName(), "Speed");
    ASSERT_EQ(blend->GetSamples().size(), 2u);
    EXPECT_FLOAT_EQ(blend->GetSamples()[0].Position, 0.0f);
    EXPECT_FLOAT_EQ(blend->GetSamples()[1].Position, 1.5f);
    EXPECT_NE(dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[0].Node.get()),
              nullptr);
    EXPECT_NE(dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[1].Node.get()),
              nullptr);
    auto* idle = dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[0].Node.get());
    auto* run = dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[1].Node.get());
    ASSERT_NE(idle, nullptr);
    ASSERT_NE(run, nullptr);
    EXPECT_TRUE(idle->GetClipGuid().IsNull());
    EXPECT_TRUE(run->GetClipGuid().IsNull());
}

TEST(AnimationGraphCompileFromModel, BlendSpace1DSamplesBindClipGuid)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "bs",
          "typeId": "BlendSpace1D",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "parameters": {"parameter": "Speed"},
          "extensions": {
            "blendSpace1D": {
              "samples": [
                {"position": 0.0, "label": "idle", "clipGuid": "01234567-89ab-cdef-0123-456789abcdef"},
                {"position": 1.5, "label": "run", "clipGuid": "fedcba98-7654-3210-fedc-ba9876543210"}
              ]
            }
          }
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "bs",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* blend = dynamic_cast<Animation::BlendSpace1DNode*>(player->RootNode.get());
    ASSERT_NE(blend, nullptr);
    ASSERT_EQ(blend->GetSamples().size(), 2u);
    auto* idle = dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[0].Node.get());
    auto* run = dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[1].Node.get());
    ASSERT_NE(idle, nullptr);
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(idle->GetClipGuid(), GUID("01234567-89ab-cdef-0123-456789abcdef"));
    EXPECT_EQ(run->GetClipGuid(), GUID("fedcba98-7654-3210-fedc-ba9876543210"));
}

TEST(AnimationGraphCompileFromModel, BlendSpace1DInvalidSampleClipGuidIsNull)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "bs",
          "typeId": "BlendSpace1D",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "extensions": {
            "blendSpace1D": {
              "samples": [
                {"position": 0.0, "clipGuid": "not-a-guid"}
              ]
            }
          }
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "bs",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* blend = dynamic_cast<Animation::BlendSpace1DNode*>(player->RootNode.get());
    ASSERT_NE(blend, nullptr);
    ASSERT_EQ(blend->GetSamples().size(), 1u);
    auto* clip = dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[0].Node.get());
    ASSERT_NE(clip, nullptr);
    EXPECT_TRUE(clip->GetClipGuid().IsNull());
}

TEST(AnimationGraphCompileFromModel, StateMachineEntryToIdleState)
{
    const nlohmann::json idlePose = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "entry"},
                 {"typeId", "Entry"},
                 {"pins", nlohmann::json::array({
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })}
             },
             {
                 {"id", "idle"},
                 {"typeId", "State"},
                 {"parameters", {{"title", "Idle"}}},
                 {"pins", nlohmann::json::array({
                      {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })},
                 {"extensions", {{"subgraph", idlePose.dump()}}}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "e0"},
                 {"sourceNodeId", "entry"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "idle"},
                 {"targetPinId", "in"}
             }
         })}
    };

    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* sm = dynamic_cast<Animation::StateMachineNode*>(player->RootNode.get());
    ASSERT_NE(sm, nullptr);
    ASSERT_EQ(sm->GetStates().size(), 1u);
    EXPECT_EQ(sm->GetStates()[0].Name, "Idle");
    EXPECT_NE(dynamic_cast<const Animation::ClipPlayerNode*>(sm->GetStates()[0].Node.get()),
              nullptr);
    EXPECT_EQ(sm->GetActiveStateIndex(), 0u);
    EXPECT_TRUE(sm->GetStates()[0].Transitions.empty());
}

TEST(AnimationGraphCompileFromModel, StateMachineTransitionCompilesConditions)
{
    const nlohmann::json pose = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "entry"},
                 {"typeId", "Entry"},
                 {"pins", nlohmann::json::array({
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })}
             },
             {
                 {"id", "idle"},
                 {"typeId", "State"},
                 {"parameters", {{"title", "Idle"}}},
                 {"pins", nlohmann::json::array({
                      {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })},
                 {"extensions", {{"subgraph", pose.dump()}}}
             },
             {
                 {"id", "walk"},
                 {"typeId", "State"},
                 {"parameters", {{"title", "Walk"}}},
                 {"pins", nlohmann::json::array({
                      {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })},
                 {"extensions", {{"subgraph", pose.dump()}}}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "e0"},
                 {"sourceNodeId", "entry"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "idle"},
                 {"targetPinId", "in"}
             },
             {
                 {"id", "t0"},
                 {"sourceNodeId", "idle"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "walk"},
                 {"targetPinId", "in"},
                 {"duration", 0.15},
                 {"conditions", nlohmann::json::array({
                      {{"param", "Speed"}, {"op", "greaterThan"}, {"value", 0.5}}
                  })}
             }
         })}
    };

    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* sm = dynamic_cast<Animation::StateMachineNode*>(player->RootNode.get());
    ASSERT_NE(sm, nullptr);
    ASSERT_EQ(sm->GetStates().size(), 2u);
    ASSERT_EQ(sm->GetStates()[0].Transitions.size(), 1u);
    EXPECT_TRUE(sm->GetStates()[1].Transitions.empty());
    const auto& tr = sm->GetStates()[0].Transitions[0];
    EXPECT_EQ(tr.TargetStateIndex, 1u);
    EXPECT_FLOAT_EQ(tr.Duration, 0.15f);
    ASSERT_EQ(tr.Conditions.size(), 1u);
    EXPECT_EQ(tr.Conditions[0].ParamName, "Speed");
    EXPECT_EQ(tr.Conditions[0].Op, Animation::ParamCompare::Greater);
    ASSERT_TRUE(std::holds_alternative<float>(tr.Conditions[0].Expected));
    EXPECT_FLOAT_EQ(std::get<float>(tr.Conditions[0].Expected), 0.5f);
}

TEST(AnimationGraphCompileFromModel, StateMachineUnconditionedLinkIsSkipped)
{
    const nlohmann::json pose = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto makeState = [&](const char* id, const char* title)
    {
        return nlohmann::json{
            {"id", id},
            {"typeId", "State"},
            {"parameters", {{"title", title}}},
            {"pins", nlohmann::json::array({
                 {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                 {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
             })},
            {"extensions", {{"subgraph", pose.dump()}}}};
    };

    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({makeState("idle", "Idle"), makeState("walk", "Walk")})},
        {"links", nlohmann::json::array({
             {
                 {"id", "t0"},
                 {"sourceNodeId", "idle"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "walk"},
                 {"targetPinId", "in"}
             }
         })}
    };

    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* sm = dynamic_cast<Animation::StateMachineNode*>(player->RootNode.get());
    ASSERT_NE(sm, nullptr);
    ASSERT_EQ(sm->GetStates().size(), 2u);
    EXPECT_TRUE(sm->GetStates()[0].Transitions.empty());
}

TEST(AnimationGraphCompileFromModel, StateMachineEmptyConditionsListIsSkipped)
{
    const nlohmann::json pose = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto makeState = [&](const char* id, const char* title)
    {
        return nlohmann::json{
            {"id", id},
            {"typeId", "State"},
            {"parameters", {{"title", title}}},
            {"pins", nlohmann::json::array({
                 {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                 {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
             })},
            {"extensions", {{"subgraph", pose.dump()}}}};
    };

    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({makeState("idle", "Idle"), makeState("walk", "Walk")})},
        {"links", nlohmann::json::array({
             {
                 {"id", "t0"},
                 {"sourceNodeId", "idle"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "walk"},
                 {"targetPinId", "in"},
                 {"conditions", nlohmann::json::array()}
             }
         })}
    };

    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* sm = dynamic_cast<Animation::StateMachineNode*>(player->RootNode.get());
    ASSERT_NE(sm, nullptr);
    ASSERT_EQ(sm->GetStates().size(), 2u);
    EXPECT_TRUE(sm->GetStates()[0].Transitions.empty());
}

TEST(AnimationGraphCompileFromModel, StateMachineHugeDurationFailsClosed)
{
    const nlohmann::json pose = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto makeState = [&](const char* id, const char* title)
    {
        return nlohmann::json{
            {"id", id},
            {"typeId", "State"},
            {"parameters", {{"title", title}}},
            {"pins", nlohmann::json::array({
                 {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                 {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
             })},
            {"extensions", {{"subgraph", pose.dump()}}}};
    };

    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({makeState("idle", "Idle"), makeState("walk", "Walk")})},
        {"links", nlohmann::json::array({
             {
                 {"id", "t0"},
                 {"sourceNodeId", "idle"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "walk"},
                 {"targetPinId", "in"},
                 {"duration", 1e40},
                 {"conditions", nlohmann::json::array({
                      {{"param", "Speed"}, {"op", "greaterThan"}, {"value", 0.5}}
                  })}
             }
         })}
    };

    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };

    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, StateMachineHugeConditionValueFailsClosed)
{
    const nlohmann::json pose = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto makeState = [&](const char* id, const char* title)
    {
        return nlohmann::json{
            {"id", id},
            {"typeId", "State"},
            {"parameters", {{"title", title}}},
            {"pins", nlohmann::json::array({
                 {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                 {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
             })},
            {"extensions", {{"subgraph", pose.dump()}}}};
    };

    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({makeState("idle", "Idle"), makeState("walk", "Walk")})},
        {"links", nlohmann::json::array({
             {
                 {"id", "t0"},
                 {"sourceNodeId", "idle"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "walk"},
                 {"targetPinId", "in"},
                 {"conditions", nlohmann::json::array({
                      {{"param", "Speed"}, {"op", "greaterThan"}, {"value", 1e40}}
                  })}
             }
         })}
    };

    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };

    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, StateMachineInvalidConditionsFailClosed)
{
    const nlohmann::json pose = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "clip",
          "typeId": "ClipPlayer",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "clip",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto makeState = [&](const char* id, const char* title)
    {
        return nlohmann::json{
            {"id", id},
            {"typeId", "State"},
            {"parameters", {{"title", title}}},
            {"pins", nlohmann::json::array({
                 {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                 {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
             })},
            {"extensions", {{"subgraph", pose.dump()}}}};
    };

    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({makeState("idle", "Idle"), makeState("walk", "Walk")})},
        {"links", nlohmann::json::array({
             {
                 {"id", "t0"},
                 {"sourceNodeId", "idle"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "walk"},
                 {"targetPinId", "in"},
                 {"conditions", "not-a-list"}
             }
         })}
    };

    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };

    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, MissingStateMachineSubgraphFailsClosed)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "sm",
          "typeId": "StateMachine",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "sm",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, InvalidStateMachineSubgraphFailsClosed)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "sm",
          "typeId": "StateMachine",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "extensions": {"subgraph": "{not-json"}
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "sm",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, InvalidStatePoseSubgraphFailsClosed)
{
    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "entry"},
                 {"typeId", "Entry"},
                 {"pins", nlohmann::json::array({
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })}
             },
             {
                 {"id", "idle"},
                 {"typeId", "State"},
                 {"parameters", {{"title", "Idle"}}},
                 {"pins", nlohmann::json::array({
                      {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })},
                 {"extensions", {{"subgraph", "{not-json"}}}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "e0"},
                 {"sourceNodeId", "entry"},
                 {"sourcePinId", "out"},
                 {"targetNodeId", "idle"},
                 {"targetPinId", "in"}
             }
         })}
    };
    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, StatePoseTwoOutputPoseFailsClosed)
{
    const nlohmann::json idlePose = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "clip"},
                 {"typeId", "ClipPlayer"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })}
             },
             {
                 {"id", "outA"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             },
             {
                 {"id", "outB"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "c0"},
                 {"sourceNodeId", "clip"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "outA"},
                 {"targetPinId", "pose"}
             }
         })}
    };
    const nlohmann::json smSubgraph = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "idle"},
                 {"typeId", "State"},
                 {"parameters", {{"title", "Idle"}}},
                 {"pins", nlohmann::json::array({
                      {{"id", "in"}, {"direction", "in"}, {"dataType", "transition"}},
                      {{"id", "out"}, {"direction", "out"}, {"dataType", "transition"}}
                  })},
                 {"extensions", {{"subgraph", idlePose.dump()}}}
             }
         })}
    };
    const nlohmann::json doc = {
        {"kind", "animation"},
        {"nodes", nlohmann::json::array({
             {
                 {"id", "sm"},
                 {"typeId", "StateMachine"},
                 {"pins", nlohmann::json::array({
                      {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
                  })},
                 {"extensions", {{"subgraph", smSubgraph.dump()}}}
             },
             {
                 {"id", "out"},
                 {"typeId", "OutputPose"},
                 {"pins", nlohmann::json::array({
                      {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
                  })}
             }
         })},
        {"links", nlohmann::json::array({
             {
                 {"id", "l0"},
                 {"sourceNodeId", "sm"},
                 {"sourcePinId", "poseOut"},
                 {"targetNodeId", "out"},
                 {"targetPinId", "pose"}
             }
         })}
    };
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, BlendSpace2DTwoSamplesToOutputPose)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "bs",
          "typeId": "BlendSpace2D",
          "pins": [{"id": "poseOut", "direction": "out", "dataType": "pose"}],
          "parameters": {"parameterX": "Speed", "parameterY": "Direction"},
          "extensions": {
            "blendSpace2D": {
              "samples": [
                {"x": 0.0, "y": 0.0, "label": "idle", "clipGuid": "01234567-89ab-cdef-0123-456789abcdef"},
                {"x": 1.5, "y": 1.0, "label": "run"}
              ]
            }
          }
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "l0",
          "sourceNodeId": "bs",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");

    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* blend = dynamic_cast<Animation::BlendSpace2DNode*>(player->RootNode.get());
    ASSERT_NE(blend, nullptr);
    EXPECT_EQ(blend->GetParameterNameX(), "Speed");
    EXPECT_EQ(blend->GetParameterNameY(), "Direction");
    ASSERT_EQ(blend->GetSamples().size(), 2u);
    EXPECT_FLOAT_EQ(blend->GetSamples()[0].X, 0.0f);
    EXPECT_FLOAT_EQ(blend->GetSamples()[0].Y, 0.0f);
    EXPECT_FLOAT_EQ(blend->GetSamples()[1].X, 1.5f);
    EXPECT_FLOAT_EQ(blend->GetSamples()[1].Y, 1.0f);
    auto* idle = dynamic_cast<const Animation::ClipPlayerNode*>(blend->GetSamples()[0].Node.get());
    ASSERT_NE(idle, nullptr);
    EXPECT_EQ(idle->GetClipGuid(), GUID("01234567-89ab-cdef-0123-456789abcdef"));
}

TEST(AnimationGraphCompileFromModel, CycleFailsClosed)
{
    const nlohmann::json doc = nlohmann::json::parse(R"JSON({
      "kind": "animation",
      "nodes": [
        {
          "id": "a",
          "typeId": "Blend2",
          "pins": [
            {"id": "a", "direction": "in", "dataType": "pose"},
            {"id": "b", "direction": "in", "dataType": "pose"},
            {"id": "poseOut", "direction": "out", "dataType": "pose"}
          ]
        },
        {
          "id": "b",
          "typeId": "Blend2",
          "pins": [
            {"id": "a", "direction": "in", "dataType": "pose"},
            {"id": "b", "direction": "in", "dataType": "pose"},
            {"id": "poseOut", "direction": "out", "dataType": "pose"}
          ]
        },
        {
          "id": "out",
          "typeId": "OutputPose",
          "pins": [{"id": "pose", "direction": "in", "dataType": "pose"}]
        }
      ],
      "links": [
        {
          "id": "ab",
          "sourceNodeId": "a",
          "sourcePinId": "poseOut",
          "targetNodeId": "b",
          "targetPinId": "a"
        },
        {
          "id": "ba",
          "sourceNodeId": "b",
          "sourcePinId": "poseOut",
          "targetNodeId": "a",
          "targetPinId": "a"
        },
        {
          "id": "lo",
          "sourceNodeId": "a",
          "sourcePinId": "poseOut",
          "targetNodeId": "out",
          "targetPinId": "pose"
        }
      ]
    })JSON");
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, DeepPoseChainFailsClosed)
{
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json links = nlohmann::json::array();

    nodes.push_back({
        {"id", "clip"},
        {"typeId", "ClipPlayer"},
        {"pins", nlohmann::json::array({
            {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
        })}
    });

    constexpr int kChain = 80;
    std::string prev = "clip";
    for (int i = 0; i < kChain; ++i)
    {
        const std::string id = "b" + std::to_string(i);
        nodes.push_back({
            {"id", id},
            {"typeId", "Blend2"},
            {"pins", nlohmann::json::array({
                {{"id", "a"}, {"direction", "in"}, {"dataType", "pose"}},
                {{"id", "b"}, {"direction", "in"}, {"dataType", "pose"}},
                {{"id", "poseOut"}, {"direction", "out"}, {"dataType", "pose"}}
            })}
        });
        links.push_back({
            {"id", "l" + std::to_string(i)},
            {"sourceNodeId", prev},
            {"sourcePinId", "poseOut"},
            {"targetNodeId", id},
            {"targetPinId", "a"}
        });
        prev = id;
    }

    nodes.push_back({
        {"id", "out"},
        {"typeId", "OutputPose"},
        {"pins", nlohmann::json::array({
            {{"id", "pose"}, {"direction", "in"}, {"dataType", "pose"}}
        })}
    });
    links.push_back({
        {"id", "lo"},
        {"sourceNodeId", prev},
        {"sourcePinId", "poseOut"},
        {"targetNodeId", "out"},
        {"targetPinId", "pose"}
    });

    const nlohmann::json doc = {{"kind", "animation"}, {"nodes", nodes}, {"links", links}};
    EXPECT_EQ(Engine::Renderer::CompileAuthoringModel(doc), nullptr);
}

TEST(AnimationGraphCompileFromModel, SerializerNestedRootNodeUnchanged)
{
    const nlohmann::json doc = {{"rootNode", {{"type", "ClipPlayer"}}}};
    EXPECT_FALSE(Engine::Renderer::LooksLikeAuthoringModel(doc));
    auto player = Animation::AnimationGraphSerializer::Deserialize(doc);
    ASSERT_NE(player, nullptr);
    ASSERT_NE(player->RootNode, nullptr);
    EXPECT_NE(dynamic_cast<Animation::ClipPlayerNode*>(player->RootNode.get()), nullptr);
}

// The sample as the editor ships it, staged beside this executable by the build and read
// through the install assets root the engine itself resolves. Never climbed to from
// __FILE__: under Ninja that macro is relative to the build directory, so the result
// followed the working directory, and a repo path is absent once the build output moves.
TEST(AnimationGraphCompileFromModel, LocomotionSampleAnimGraphCompiles)
{
    const auto path = GameEngine::PathUtils::GetInstallAssetsRoot() / "Graphs" / "LocomotionSample.animgraph";
    ASSERT_TRUE(std::filesystem::exists(path)) << path.string();
    std::ifstream in(path);
    ASSERT_TRUE(in) << path.string();
    nlohmann::json doc;
    in >> doc;
    ASSERT_TRUE(Engine::Renderer::LooksLikeAuthoringModel(doc));
    auto player = Engine::Renderer::CompileAuthoringModel(doc);
    ASSERT_NE(player, nullptr);
    auto* sm = dynamic_cast<Animation::StateMachineNode*>(player->RootNode.get());
    ASSERT_NE(sm, nullptr);
    ASSERT_EQ(sm->GetStates().size(), 2u);
    EXPECT_EQ(sm->GetStates()[0].Name, "Idle");
    EXPECT_EQ(sm->GetStates()[1].Name, "Walk");
    ASSERT_EQ(sm->GetStates()[0].Transitions.size(), 1u);
    ASSERT_EQ(sm->GetStates()[1].Transitions.size(), 1u);
    EXPECT_NE(dynamic_cast<const Animation::ClipPlayerNode*>(sm->GetStates()[0].Node.get()), nullptr);
    EXPECT_NE(dynamic_cast<const Animation::BlendSpace1DNode*>(sm->GetStates()[1].Node.get()), nullptr);
}

