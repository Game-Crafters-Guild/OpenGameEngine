// The animation event dispatch: the events a clip's track holds fire into the playing Animator's collector as
// playback crosses them, on the clip path (AnimationSystem), the graph path (AnimationGraphSystem) and a montage
// slot, and last one frame.

#include <gtest/gtest.h>

#include "Animation/AnimationEvent.h"
#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphStore.h"
#include "Animation/AnimationMontage.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/BoneMask.h"
#include "Animation/Nodes/AdditiveBlendNode.h"
#include "Animation/Nodes/Blend2Node.h"
#include "Animation/Nodes/BlendSpace1DNode.h"
#include "Animation/Nodes/BlendSpace2DNode.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Animation/Nodes/LayeredBlendNode.h"
#include "Animation/Nodes/MontageSlotNode.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Hierarchy.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/Systems/AnimationEventCollectorSystem.h"
#include "ECSModules/Rendering/Systems/AnimationGraphSystem.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "ECSModules/Rendering/Systems/RegisterRenderingSystems.h"
#include "Engine/Rendering/AnimationEventDispatch.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "GltfTestFiles.h"

#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

using Animation::AnimationEventCollector;
using Animation::AnimationGraphPlayer;
using Animation::AnimationGraphStore;
using Animation::ClipPlayerNode;
using Names = std::vector<std::string>;

// The clips here are sampled at 30 frames per second and last one second. Frames of playback last 0.05 s, so no
// frame boundary lands on an event frame the tests use, and frame 19 (0.633 s) is crossed in the 13th frame of
// playback, from 0.60 s to 0.65 s.
constexpr double kClipFramesPerSecond = 30.0;
constexpr size_t kClipKeys = 31;
constexpr float kFrameSeconds = 0.05f;
constexpr int kFramesToCrossFrame19 = 13;

// The time the glTF clip schema gives an event on `frame` of a clip sampled at kClipFramesPerSecond.
float FrameTime(int frame)
{
    return static_cast<float>(frame / kClipFramesPerSecond);
}

void SetIdentity(float* matrix)
{
    for (int i = 0; i < 16; ++i)
        matrix[i] = 0.0f;
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

// A one-second clip sampled at kClipFramesPerSecond whose glTF clip schema declares an event named `name` on each
// `frame`, read the way the importer reads a file.
SharedPtr<AnimationClip> MakeClip(std::initializer_list<std::pair<int, const char*>> events)
{
    std::string schema = R"({"clip": {"schemaVersion": 1, "events": [)";
    const char* separator = "";
    for (const auto& [frame, name] : events)
    {
        schema += std::format(R"({}{{"frame": {}, "name": "{}"}})", separator, frame, name);
        separator = ", ";
    }
    schema += "]}}";
    const std::string file = TestFiles::ClipGlb(kClipKeys, static_cast<float>(kClipFramesPerSecond), schema);
    auto clip = MakeShared<AnimationClip>(GUID::Generate(), std::filesystem::path("events.glb"));
    EXPECT_TRUE(clip->LoadFromData(Vector<uint8>(file.begin(), file.end())));
    EXPECT_EQ(clip->GetEventTrack().GetEvents().size(), events.size());
    return clip;
}

uint32 RegisterClip(const SharedPtr<AnimationClip>& clip)
{
    Engine::Renderer::TestHooks::SetClipCacheForTest(clip->GetGUID(), clip);
    return Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(clip->GetGUID());
}

uint32 MakeSingleBoneSkeleton()
{
    auto& skeletons = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skeletonId = skeletons.CreateSkeleton(1);
    auto* skeleton = skeletons.Get(skeletonId);
    skeleton->Parent = {-1};
    skeleton->InverseBind.resize(16);
    SetIdentity(skeleton->InverseBind.data());
    skeleton->BindPose.resize(16);
    SetIdentity(skeleton->BindPose.data());
    skeleton->RestTranslation = {0.0f, 0.0f, 0.0f};
    skeleton->RestRotation = {0.0f, 0.0f, 0.0f, 1.0f};
    skeleton->RestScale = {1.0f, 1.0f, 1.0f};
    skeleton->SkinJointCount = 1;
    skeleton->JointNodes = {0};
    return skeletonId;
}

Names NamesOf(std::span<const Animation::FiredEvent> events)
{
    Names names;
    for (const Animation::FiredEvent& fired : events)
        names.push_back(fired.Event.Name);
    return names;
}

// An Animator on the clip path over two skinned meshes, each with its own AnimatorRef as scene playback gives them,
// ticked through the wave's collector system and AnimationSystem.
struct ClipPathRig
{
    Engine::Renderer::RenderServices renderServices;
    Engine::Renderer::AnimationEventCollectorSystem collectorSystem;
    Engine::Renderer::AnimationSystem animationSystem{&renderServices};
    ECS::World world{nullptr};
    ECS::EntityHandle root;

    ClipPathRig(uint32 clipIndex, uint32 animatorRefFlags)
    {
        Engine::Renderer::RegisterRenderWorldHooks(world);
        root = world.CreateEntity();
        world.AddComponentImmediate(root, Components::Animator{});
        const uint32 skeletonId = MakeSingleBoneSkeleton();
        for (int mesh = 0; mesh < 2; ++mesh)
        {
            const ECS::EntityHandle child = world.CreateEntity();
            world.AddComponentImmediate(child, Components::Parent{root});
            Components::AnimatorRef animatorRef{};
            animatorRef.ClipIndex = clipIndex;
            animatorRef.Flags = animatorRefFlags;
            world.AddComponentImmediate(child, animatorRef);
            Components::SkeletonRef skeletonRef{};
            skeletonRef.skeletonId = skeletonId;
            skeletonRef.runtimeId = Engine::Renderer::SkeletonStore::Instance().CreateRuntime(skeletonId);
            world.AddComponentImmediate(child, skeletonRef);
        }
    }

    // Runs one animation wave of `seconds` and returns the names of the events it fired, in order.
    Names Frame(float seconds)
    {
        collectorSystem.Update(world, seconds);
        animationSystem.Update(world, seconds);
        return NamesOf(Engine::Renderer::GetFiredEvents(*world.GetComponent<Components::Animator>(root)));
    }

    // Sets every AnimatorRef's clip time, as a seek does.
    void SetTime(float seconds)
    {
        world.Query<ECS::Write<Components::AnimatorRef>>().Each(
            [seconds](Components::AnimatorRef& animatorRef) { animatorRef.Time = seconds; });
    }
};

// An Animator on the graph path playing `root`, ticked through the wave's collector system and AnimationGraphSystem.
struct GraphPathRig
{
    Engine::Renderer::AnimationEventCollectorSystem collectorSystem;
    Engine::Renderer::AnimationGraphSystem graphSystem{nullptr};
    ECS::World world{nullptr};
    ECS::EntityHandle entity;

    explicit GraphPathRig(std::unique_ptr<Animation::AnimGraphNode> rootNode)
    {
        Engine::Renderer::RegisterRenderWorldHooks(world);
        auto player = std::make_unique<AnimationGraphPlayer>();
        player->RootNode = std::move(rootNode);
        Components::Animator animator{};
        animator.source = Components::AnimatorPlaybackSource::Graph;
        animator.graphRuntimeId = AnimationGraphStore::Instance().Create(std::move(player));
        entity = world.CreateEntity();
        world.AddComponentImmediate(entity, animator);
    }

    Names Frame(float seconds)
    {
        collectorSystem.Update(world, seconds);
        graphSystem.Update(world, seconds);
        return NamesOf(Engine::Renderer::GetFiredEvents(*world.GetComponent<Components::Animator>(entity)));
    }
};

// Runs `frames` frames of kFrameSeconds and returns, per frame, the events it fired.
template <typename Rig>
std::vector<Names> Play(Rig& rig, int frames)
{
    std::vector<Names> fired;
    for (int frame = 0; frame < frames; ++frame)
        fired.push_back(rig.Frame(kFrameSeconds));
    return fired;
}

// Expects `fired` to hold `name` once, in the frame at `frameIndex`.
void ExpectFiredOnceIn(const std::vector<Names>& fired, const std::string& name, int frameIndex)
{
    int count = 0;
    for (size_t frame = 0; frame < fired.size(); ++frame)
    {
        for (const std::string& firedName : fired[frame])
        {
            if (firedName != name)
                continue;
            ++count;
            EXPECT_EQ(static_cast<int>(frame), frameIndex) << name << " fired in the wrong frame";
        }
    }
    EXPECT_EQ(count, 1) << name << " fired " << count << " times";
}

// How many times `name` fired across `fired`.
int CountOf(const std::vector<Names>& fired, const std::string& name)
{
    int count = 0;
    for (const Names& frame : fired)
        for (const std::string& firedName : frame)
            count += firedName == name ? 1 : 0;
    return count;
}

// Evaluates `node` for one frame of `seconds` into a collector of its own and returns what it fired, in order.
Names EvaluateOnce(Animation::AnimGraphNode& node, float seconds)
{
    AnimationEventCollector collector;
    Animation::EvaluationContext context;
    context.Events = &collector;
    context.DeltaTime = seconds;
    Animation::AnimationPose pose;
    node.Evaluate(context, pose);
    return NamesOf(collector.GetEvents());
}

// A clip player at `time` on a clip whose track holds `events`.
std::unique_ptr<ClipPlayerNode> PlayerAt(float time, std::initializer_list<std::pair<int, const char*>> events)
{
    auto player = std::make_unique<ClipPlayerNode>();
    player->SetClip(MakeClip(events));
    player->SetTime(time);
    return player;
}

// What one looping frame of `seconds` from `from` fires on a clip holding `events`: on the clip path (AnimationSystem)
// and on the graph path (a ClipPlayerNode), in that order.
std::pair<Names, Names> FiredInOneLoopingFrame(float from, float seconds,
                                               std::initializer_list<std::pair<int, const char*>> events)
{
    const uint32 clip = RegisterClip(MakeClip(events));
    EXPECT_NE(clip, 0u);
    ClipPathRig rig(clip, Components::AnimatorRef::kFlag_Loop);
    rig.SetTime(from);
    Names clipPath = rig.Frame(seconds);
    const std::unique_ptr<ClipPlayerNode> player = PlayerAt(from, events);
    player->SetLooping(true);
    Names graphPath = EvaluateOnce(*player, seconds);
    return {clipPath, graphPath};
}

// Expects each of `names` to have fired once in `fired`.
void ExpectEachFiredOnce(const Names& fired, std::initializer_list<const char*> names, const char* path)
{
    for (const char* name : names)
        EXPECT_EQ(CountOf({fired}, name), 1) << path << ": " << name;
}

// What a state machine fires over the first two frames of a transition of `transitionSeconds` out of a state whose
// clip crosses frame 19 in the second frame, from 0.60 s to 0.65 s.
Names FiredLeavingAState(float transitionSeconds)
{
    Animation::StateMachineNode machine;
    machine.AddState("Leaving", PlayerAt(0.55f, {{19, "leaving"}}));
    machine.AddState("Entering", PlayerAt(0.0f, {}));
    machine.AddTransition(0, 1, transitionSeconds, {});
    Names fired = EvaluateOnce(machine, kFrameSeconds);
    const Names second = EvaluateOnce(machine, kFrameSeconds);
    fired.insert(fired.end(), second.begin(), second.end());
    return fired;
}

// What a layered blend under `mask` fires with its overlay at full weight, both layers crossing frame 19.
Names FiredUnderALayer(const Animation::BoneMask& mask)
{
    Animation::LayeredBlendNode layered;
    layered.SetBase(PlayerAt(0.6f, {{19, "base"}}));
    layered.SetOverlay(PlayerAt(0.6f, {{19, "overlay"}}));
    layered.SetMask(mask);
    layered.SetBlendWeight(1.0f);
    return EvaluateOnce(layered, kFrameSeconds);
}

// What a montage slot fires in the first frame of a montage that blends in at once, its source crossing frame 19;
// `mask`, when given, confines the montage to part of the body.
Names FiredUnderAMontage(const Animation::BoneMask* mask)
{
    Animation::AnimationMontage montage;
    montage.SetClipDuration(1.0f);
    montage.SetBlendInDuration(0.0f);
    montage.SetBoneMask(mask);
    Animation::MontageSlotNode slot;
    slot.SetSource(PlayerAt(0.6f, {{19, "source"}}));
    slot.PlayMontage(&montage);
    return EvaluateOnce(slot, kFrameSeconds);
}

// The wave of the default schedule that runs the system named `name`; -1 when no wave runs it.
size_t WaveOf(const ECS::SystemManager& manager, std::string_view name)
{
    const auto& waves = manager.GetExecutionPlan().Waves;
    for (size_t wave = 0; wave < waves.size(); ++wave)
        for (size_t index : waves[wave].SystemIndices)
        {
            const char* systemName = manager.GetSequentialSystemName(index);
            if (systemName && name == systemName)
                return wave;
        }
    return static_cast<size_t>(-1);
}

class AnimationEventDispatch : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Engine::Renderer::TestHooks::ClearClipCacheForTest();
        AnimationGraphStore::Instance().ClearForTest();
        Animation::AnimationEventCollectorStore::Instance().ClearForTest();
    }

    void TearDown() override
    {
        Engine::Renderer::TestHooks::ClearClipCacheForTest();
        AnimationGraphStore::Instance().ClearForTest();
        Animation::AnimationEventCollectorStore::Instance().ClearForTest();
    }
};

} // namespace

TEST_F(AnimationEventDispatch, ClipPathFiresFrame19OnceAtTheClipsRate)
{
    const uint32 clip = RegisterClip(MakeClip({{19, "hit"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, 0u);

    // Past the end: the clip does not loop, so it stops there and fires nothing more.
    const std::vector<Names> fired = Play(rig, 30);
    ExpectFiredOnceIn(fired, "hit", kFramesToCrossFrame19 - 1);
}

TEST_F(AnimationEventDispatch, GraphPathFiresFrame19OnceAtTheClipsRate)
{
    auto player = std::make_unique<ClipPlayerNode>();
    player->SetClip(MakeClip({{19, "hit"}}));
    player->SetLooping(false);
    GraphPathRig rig(std::move(player));

    const std::vector<Names> fired = Play(rig, 30);
    ExpectFiredOnceIn(fired, "hit", kFramesToCrossFrame19 - 1);
}

TEST_F(AnimationEventDispatch, MontageSlotFiresTheMontagesEventOnce)
{
    Animation::AnimationMontage montage;
    montage.SetClipDuration(1.0f);
    montage.SetBlendInDuration(0.1f);
    montage.SetBlendOutDuration(0.1f);
    montage.GetEventTrack().AddEvent({FrameTime(19), "hit", {}});
    auto slot = std::make_unique<Animation::MontageSlotNode>();
    Animation::MontageSlotNode* slotNode = slot.get();
    GraphPathRig rig(std::move(slot));
    slotNode->PlayMontage(&montage);

    const std::vector<Names> fired = Play(rig, 30);
    ExpectFiredOnceIn(fired, "hit", kFramesToCrossFrame19 - 1);
    EXPECT_FALSE(slotNode->IsPlaying());
}

TEST_F(AnimationEventDispatch, AMontageAtANegativeRateFiresLatestFirst)
{
    Animation::AnimationMontage montage;
    montage.SetClipDuration(1.0f);
    montage.SetBlendInDuration(0.0f);
    montage.SetPlayRate(-1.0f);
    montage.AddSection({"End", 0.9f, 1.0f, {}});
    montage.GetEventTrack().AddEvent({FrameTime(15), "early", {}});
    montage.GetEventTrack().AddEvent({FrameTime(24), "late", {}});
    Animation::MontageSlotNode slot;
    slot.PlayMontage(&montage);
    slot.JumpToSection("End");

    AnimationEventCollector collector;
    Animation::EvaluationContext context;
    context.Events = &collector;
    context.DeltaTime = 0.5f;
    Animation::AnimationPose pose;
    slot.Evaluate(context, pose);

    EXPECT_EQ(NamesOf(collector.GetEvents()), (Names{"late", "early"}))
        << "from 0.9 s down to 0.4 s: frame 24 (0.8 s), then frame 15 (0.5 s)";
}

TEST_F(AnimationEventDispatch, ALoopWrapFiresTheTailThenTheHead)
{
    const uint32 clip = RegisterClip(MakeClip({{3, "head"}, {27, "tail"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, Components::AnimatorRef::kFlag_Loop);
    rig.SetTime(0.85f);

    EXPECT_EQ(rig.Frame(0.3f), (Names{"tail", "head"})) << "0.85 s past the end to 0.15 s";
}

TEST_F(AnimationEventDispatch, TwoEventsInOneFrameFireInTimeOrder)
{
    const uint32 clip = RegisterClip(MakeClip({{19, "second"}, {18, "first"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, 0u);
    rig.SetTime(0.55f);

    EXPECT_EQ(rig.Frame(kFrameSeconds * 2.0f), (Names{"first", "second"}));
}

TEST_F(AnimationEventDispatch, ASeekFiresNothing)
{
    const uint32 clip = RegisterClip(MakeClip({{19, "hit"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, 0u);
    rig.SetTime(0.5f);
    EXPECT_TRUE(rig.Frame(kFrameSeconds).empty());

    // Animator.Seek's command writes the time between waves (AnimatorScenePlayback); from 0.55 s to 0.8 s it jumps
    // over frame 19, and the next wave plays from 0.8 s.
    rig.SetTime(0.8f);
    EXPECT_TRUE(rig.Frame(kFrameSeconds).empty());
}

TEST_F(AnimationEventDispatch, TheEventsLastOneWave)
{
    const uint32 clip = RegisterClip(MakeClip({{19, "hit"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, 0u);
    rig.SetTime(0.6f);

    EXPECT_EQ(rig.Frame(kFrameSeconds), (Names{"hit"}));
    EXPECT_TRUE(rig.Frame(kFrameSeconds).empty()) << "the next wave empties the collector before its playback";
}

TEST_F(AnimationEventDispatch, AClipPlayerAtWeight0FiresNothingAndAbove0Fires)
{
    auto collectFrame19 = [](float weightOfTheClipWithTheEvent) {
        Animation::Blend2Node blend;
        auto silent = std::make_unique<ClipPlayerNode>();
        silent->SetClip(MakeClip({}));
        auto withEvent = std::make_unique<ClipPlayerNode>();
        withEvent->SetClip(MakeClip({{19, "hit"}}));
        withEvent->SetTime(0.6f);
        blend.SetInputA(std::move(silent));
        blend.SetInputB(std::move(withEvent));
        blend.SetWeight(weightOfTheClipWithTheEvent);

        AnimationEventCollector collector;
        Animation::EvaluationContext context;
        context.Events = &collector;
        context.DeltaTime = kFrameSeconds;
        Animation::AnimationPose pose;
        blend.Evaluate(context, pose);
        return NamesOf(collector.GetEvents());
    };

    EXPECT_TRUE(collectFrame19(0.0f).empty()) << "a clip blended out entirely fires nothing";
    EXPECT_EQ(collectFrame19(0.01f), (Names{"hit"}));
}

TEST_F(AnimationEventDispatch, TheOutgoingClipOfACrossfadeFiresUntilTheFadeEnds)
{
    const uint32 outgoing = RegisterClip(MakeClip({{19, "outgoing"}}));
    const uint32 incoming = RegisterClip(MakeClip({}));
    ASSERT_NE(outgoing, 0u);
    ASSERT_NE(incoming, 0u);

    // The outgoing clip plays on from 0.55 s and crosses frame 19 (0.633 s) in the fade's second frame: within a
    // fade of 0.2 s, and in the frame that ends a fade of 0.1 s, where its weight has reached 0.
    auto fireDuringFade = [&](float fadeSeconds) {
        ClipPathRig rig(outgoing, 0u);
        rig.SetTime(0.55f);
        rig.world.Query<ECS::Write<Components::AnimatorRef>>().Each(
            [&](Components::AnimatorRef& animatorRef) { animatorRef.SetAnimation(incoming, fadeSeconds); });
        Names fired = rig.Frame(kFrameSeconds);
        const Names second = rig.Frame(kFrameSeconds);
        fired.insert(fired.end(), second.begin(), second.end());
        return fired;
    };

    EXPECT_EQ(fireDuringFade(0.2f), (Names{"outgoing"}));
    EXPECT_TRUE(fireDuringFade(0.1f).empty()) << "the fade ended in the frame that crossed the event";
}

TEST_F(AnimationEventDispatch, ClipPathFrameLongerThanALapFiresEachEventOnce)
{
    const uint32 clip = RegisterClip(MakeClip({{3, "head"}, {9, "middle"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, Components::AnimatorRef::kFlag_Loop);
    rig.SetTime(0.2f);

    EXPECT_EQ(rig.Frame(1.2f), (Names{"middle", "head"}))
        << "0.2 s round the whole loop to 0.4 s: the lap from 0.2 s, then the head up to 0.2 s";
}

TEST_F(AnimationEventDispatch, GraphPathFrameLongerThanALapFiresEachEventOnce)
{
    const std::unique_ptr<ClipPlayerNode> player = PlayerAt(0.2f, {{3, "head"}, {9, "middle"}});
    player->SetLooping(true);

    EXPECT_EQ(EvaluateOnce(*player, 1.2f), (Names{"middle", "head"}));
}

TEST_F(AnimationEventDispatch, AFrameOfExactlyOneLapFiresEachEventOnce)
{
    const auto [clipPath, graphPath] =
        FiredInOneLoopingFrame(FrameTime(6), 1.0f, {{3, "head"}, {6, "atStart"}, {9, "middle"}, {27, "tail"}});
    ExpectEachFiredOnce(clipPath, {"head", "atStart", "middle", "tail"}, "clip path");
    ExpectEachFiredOnce(graphPath, {"head", "atStart", "middle", "tail"}, "graph path");
}

TEST_F(AnimationEventDispatch, AFrameOfTwoLapsFiresEachEventOnce)
{
    const auto [clipPath, graphPath] =
        FiredInOneLoopingFrame(FrameTime(6), 2.0f, {{3, "head"}, {6, "atStart"}, {9, "middle"}, {27, "tail"}});
    ExpectEachFiredOnce(clipPath, {"head", "atStart", "middle", "tail"}, "clip path");
    ExpectEachFiredOnce(graphPath, {"head", "atStart", "middle", "tail"}, "graph path");
}

TEST_F(AnimationEventDispatch, AFrameLongerThanALapLandingBeforeItsStartFiresEachEventOnce)
{
    // 0.5 s round the loop and on to 0.2 s: the event at 0.3 s is crossed only in the whole lap.
    const auto [clipPath, graphPath] =
        FiredInOneLoopingFrame(FrameTime(15), 1.7f, {{3, "head"}, {9, "middle"}, {27, "tail"}});
    ExpectEachFiredOnce(clipPath, {"head", "middle", "tail"}, "clip path");
    ExpectEachFiredOnce(graphPath, {"head", "middle", "tail"}, "graph path");
}

TEST_F(AnimationEventDispatch, AnEventAtTheStartOfAFrameLongerThanALapFiresOnce)
{
    const auto [clipPath, graphPath] = FiredInOneLoopingFrame(FrameTime(6), 1.2f, {{6, "atStart"}, {9, "middle"}});
    ExpectEachFiredOnce(clipPath, {"atStart", "middle"}, "clip path");
    ExpectEachFiredOnce(graphPath, {"atStart", "middle"}, "graph path");
}

TEST_F(AnimationEventDispatch, ASectionFrameLongerThanTheSectionLandingBeforeItsStartFiresEachEventOnce)
{
    const uint32 clip = RegisterClip(MakeClip({{10, "early"}, {12, "middle"}, {16, "late"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, Components::AnimatorRef::kFlag_Loop | Components::AnimatorRef::kFlag_Section);
    rig.world.Query<ECS::Write<Components::AnimatorRef>>().Each([](Components::AnimatorRef& animatorRef) {
        animatorRef.SectionStart = 0.3f;
        animatorRef.SectionEnd = 0.6f;
        animatorRef.Time = 0.5f;
    });

    // 0.5 s to the section's end at 0.6 s, round the whole section, and on to 0.35 s.
    ExpectEachFiredOnce(rig.Frame(0.45f), {"early", "middle", "late"}, "section");
}

TEST_F(AnimationEventDispatch, AWrapLandingExactlyOnAnEventFiresItOnce)
{
    // 0.75 s to the end at 1.0 s and on to exactly 0 s, then a frame from 0 s: the event at 0 s fires in the second
    // frame only.
    const uint32 clip = RegisterClip(MakeClip({{0, "zero"}, {30, "end"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, Components::AnimatorRef::kFlag_Loop);
    rig.SetTime(0.75f);
    Names clipPath = rig.Frame(0.25f);
    const Names clipPathNext = rig.Frame(0.25f);
    clipPath.insert(clipPath.end(), clipPathNext.begin(), clipPathNext.end());
    EXPECT_EQ(clipPath, (Names{"end", "zero"})) << "clip path";

    const std::unique_ptr<ClipPlayerNode> player = PlayerAt(0.75f, {{0, "zero"}, {30, "end"}});
    player->SetLooping(true);
    Names graphPath = EvaluateOnce(*player, 0.25f);
    const Names graphPathNext = EvaluateOnce(*player, 0.25f);
    graphPath.insert(graphPath.end(), graphPathNext.begin(), graphPathNext.end());
    EXPECT_EQ(graphPath, (Names{"end", "zero"})) << "graph path";
}

TEST_F(AnimationEventDispatch, ASpeedOf0AtTheExactEndOfALoopFiresNothing)
{
    // A seek to exactly the end of a looping clip, then a frame at a speed of 0: the time does not move.
    const uint32 clip = RegisterClip(MakeClip({{0, "zero"}, {30, "end"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, Components::AnimatorRef::kFlag_Loop);
    rig.world.Query<ECS::Write<Components::AnimatorRef>>().Each([](Components::AnimatorRef& animatorRef) {
        animatorRef.Time = 1.0f;
        animatorRef.Speed = 0.0f;
    });
    EXPECT_TRUE(rig.Frame(kFrameSeconds).empty()) << "clip path";

    const std::unique_ptr<ClipPlayerNode> player = PlayerAt(1.0f, {{0, "zero"}, {30, "end"}});
    player->SetLooping(true);
    player->SetSpeed(0.0f);
    EXPECT_TRUE(EvaluateOnce(*player, kFrameSeconds).empty()) << "graph path";
}

TEST_F(AnimationEventDispatch, GraphPathLoopWrapFiresTheTailThenTheHead)
{
    const std::unique_ptr<ClipPlayerNode> player = PlayerAt(0.85f, {{3, "head"}, {27, "tail"}});
    player->SetLooping(true);

    EXPECT_EQ(EvaluateOnce(*player, 0.3f), (Names{"tail", "head"})) << "0.85 s past the end to 0.15 s";
}

TEST_F(AnimationEventDispatch, AnEventOnTheLastFrameOfAClipThatDoesNotLoopFiresOnArrivalOnce)
{
    const uint32 clip = RegisterClip(MakeClip({{30, "end"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, 0u);
    rig.SetTime(0.9f);
    EXPECT_EQ(CountOf(Play(rig, 6), "end"), 1) << "the clip path";

    const std::unique_ptr<ClipPlayerNode> player = PlayerAt(0.9f, {{30, "end"}});
    player->SetLooping(false);
    std::vector<Names> fired;
    for (int frame = 0; frame < 6; ++frame)
        fired.push_back(EvaluateOnce(*player, kFrameSeconds));
    EXPECT_EQ(CountOf(fired, "end"), 1) << "the graph path";
}

TEST_F(AnimationEventDispatch, ALoopingSectionFiresOnlyItsOwnEventsOncePerLap)
{
    const uint32 clip = RegisterClip(MakeClip({{3, "before"}, {12, "inside"}, {27, "after"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, Components::AnimatorRef::kFlag_Loop | Components::AnimatorRef::kFlag_Section);
    rig.world.Query<ECS::Write<Components::AnimatorRef>>().Each([](Components::AnimatorRef& animatorRef) {
        animatorRef.SectionStart = 0.3f;
        animatorRef.SectionEnd = 0.6f;
        animatorRef.Time = 0.3f;
    });

    const std::vector<Names> fired = Play(rig, 18); // three laps of the 0.3 s section
    EXPECT_EQ(CountOf(fired, "inside"), 3);
    EXPECT_EQ(CountOf(fired, "before"), 0);
    EXPECT_EQ(CountOf(fired, "after"), 0);
}

TEST_F(AnimationEventDispatch, RetargetedClipPlaybackFiresOnce)
{
    const uint32 clip = RegisterClip(MakeClip({{19, "hit"}}));
    ASSERT_NE(clip, 0u);
    ClipPathRig rig(clip, 0u);
    std::vector<ECS::EntityHandle> retargeted;
    rig.world.Query<ECS::Read<Components::AnimatorRef>>().Each(
        [&](ECS::EntityHandle entity, const Components::AnimatorRef&) { retargeted.push_back(entity); });
    for (ECS::EntityHandle entity : retargeted)
        rig.world.AddComponentImmediate(entity, Components::HumanoidRetargeterComponent{});
    Engine::Renderer::HumanoidRetargetSystem retargetSystem(nullptr);

    std::vector<Names> fired;
    for (int frame = 0; frame < 30; ++frame)
    {
        rig.Frame(kFrameSeconds);
        retargetSystem.Update(rig.world, kFrameSeconds);
        fired.push_back(
            NamesOf(Engine::Renderer::GetFiredEvents(*rig.world.GetComponent<Components::Animator>(rig.root))));
    }
    ExpectFiredOnceIn(fired, "hit", kFramesToCrossFrame19 - 1);
}

TEST_F(AnimationEventDispatch, ABlendSpaceFiresEachSampleItBlends)
{
    Animation::BlendSpace1DNode line;
    line.AddSample(PlayerAt(0.6f, {{19, "left"}}), 0.0f);
    line.AddSample(PlayerAt(0.6f, {{19, "right"}}), 1.0f);
    line.SetParameter(0.25f);
    EXPECT_EQ(EvaluateOnce(line, kFrameSeconds), (Names{"left", "right"}));

    Animation::BlendSpace2DNode plane;
    plane.AddSample(PlayerAt(0.6f, {{19, "near"}}), 0.0f, 0.0f);
    plane.AddSample(PlayerAt(0.6f, {{19, "far"}}), 1.0f, 0.0f);
    plane.SetParameter(0.25f, 0.0f);
    EXPECT_EQ(EvaluateOnce(plane, kFrameSeconds), (Names{"near", "far"}));
}

TEST_F(AnimationEventDispatch, AStateATransitionLeavesFiresUntilTheTransitionEnds)
{
    EXPECT_EQ(FiredLeavingAState(0.2f), (Names{"leaving"})) << "at half weight in the transition's second frame";
    EXPECT_TRUE(FiredLeavingAState(0.1f).empty()) << "the transition ended in the frame that crossed the event";
}

TEST_F(AnimationEventDispatch, ALayeredBlendsBaseFiresWhereTheOverlayLeavesItShowing)
{
    EXPECT_EQ(FiredUnderALayer(Animation::BoneMask{}), (Names{"overlay"}))
        << "an overlay without a mask at full weight covers the base on every bone";
    Animation::BoneMask upperBody;
    upperBody.Weights = {1.0f, 0.0f};
    EXPECT_EQ(FiredUnderALayer(upperBody), (Names{"base", "overlay"}));
}

TEST_F(AnimationEventDispatch, AnAdditiveLayerFiresAtItsWeightAndItsBaseKeepsFiring)
{
    Animation::AdditiveBlendNode additive;
    additive.SetBase(PlayerAt(0.6f, {{19, "base"}}));
    additive.SetAdditive(PlayerAt(0.6f, {{19, "additive"}}));
    additive.SetWeight(1.0f);

    EXPECT_EQ(EvaluateOnce(additive, kFrameSeconds), (Names{"base", "additive"}));
}

TEST_F(AnimationEventDispatch, AMontageSlotsSourceStopsFiringUnderAFullBodyMontage)
{
    EXPECT_TRUE(FiredUnderAMontage(nullptr).empty()) << "a full-body montage at full weight covers its source";
    Animation::BoneMask upperBody;
    upperBody.Weights = {1.0f, 0.0f};
    EXPECT_EQ(FiredUnderAMontage(&upperBody), (Names{"source"}));
}

// AnimationEventCollectorStore takes no lock: the systems that create, read and fill collectors must each run in a
// wave of their own, in this order, so no two of them ever touch the store at once.
TEST_F(AnimationEventDispatch, TheSystemsThatUseTheCollectorsRunOneAfterAnother)
{
    ECS::SystemScheduleBuilder builder;
    Engine::Renderer::AddRenderingSystemsToSchedule(builder, nullptr);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager);

    const size_t collectors = WaveOf(manager, "AnimationEventCollector");
    ASSERT_NE(collectors, static_cast<size_t>(-1));
    EXPECT_LT(collectors, WaveOf(manager, "AnimationGraph"));
    EXPECT_LT(WaveOf(manager, "AnimationGraph"), WaveOf(manager, "Animation"));
    EXPECT_LT(WaveOf(manager, "Animation"), WaveOf(manager, "HumanoidRetarget"));
    EXPECT_NE(WaveOf(manager, "HumanoidRetarget"), static_cast<size_t>(-1));
}
