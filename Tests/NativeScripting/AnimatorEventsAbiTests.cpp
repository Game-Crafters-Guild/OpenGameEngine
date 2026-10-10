// The scripting ABI's animation events: GE_Animator_PollEventsOnEntity hands a script the events an Animator's
// playback fired in the last animation wave, each once. These drive the export AnimatorApi.PollEvents calls, with no
// CLR, against the animation wave's own systems.

#include "Animation/AnimationEvent.h"
#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphStore.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/Animator.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/AnimationEventCollectorSystem.h"
#include "ECSModules/Rendering/Systems/AnimationGraphSystem.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "GltfTestFiles.h"
#include "Scripting/AnimatorABI.h"
#include "Scripting/ScriptManager.h"
#include "Types/StringId.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace
{
using GameEngine::ECS::World;
using GameEngine::Animation::AnimationEventCollectorStore;
using GameEngine::Animation::AnimationGraphStore;

GE_Handle HandleOf(World* world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
}

std::string_view NameOf(const GE_AnimationEventRecord& record)
{
    return {record.name, record.nameLength};
}

// A one-second clip sampled at 30 frames per second whose glTF clip schema puts "hit" on frame 19, read the way the
// importer reads a file.
std::shared_ptr<GameEngine::AnimationClip> ClipWithHitOnFrame19()
{
    constexpr size_t kKeys = 31;
    constexpr float kFramesPerSecond = 30.0f;
    const std::string file = GameEngine::TestFiles::ClipGlb(
        kKeys, kFramesPerSecond, R"({"clip": {"schemaVersion": 1, "events": [{"frame": 19, "name": "hit"}]}})");
    auto clip = std::make_shared<GameEngine::AnimationClip>(GameEngine::GUID::Generate(), "hit.glb");
    EXPECT_TRUE(clip->LoadFromData(GameEngine::Vector<GameEngine::uint8>(file.begin(), file.end())));
    return clip;
}
} // namespace

TEST(AnimatorEventsAbi, AFiredEventReachesPollEventsOnceWithItsNameAndTime)
{
    AnimationGraphStore::Instance().ClearForTest();
    AnimationEventCollectorStore::Instance().ClearForTest();
    World world(nullptr);
    GameEngine::Engine::Renderer::RegisterRenderWorldHooks(world);
    auto clipPlayer = std::make_unique<GameEngine::Animation::ClipPlayerNode>();
    clipPlayer->SetClip(ClipWithHitOnFrame19());
    clipPlayer->SetTime(0.6f);
    auto player = std::make_unique<GameEngine::Animation::AnimationGraphPlayer>();
    player->RootNode = std::move(clipPlayer);
    GameEngine::Components::Animator animator{};
    animator.source = GameEngine::Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = AnimationGraphStore::Instance().Create(std::move(player));
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, animator);

    // One animation wave from 0.60 s to 0.65 s crosses frame 19.
    GameEngine::Engine::Renderer::AnimationEventCollectorSystem collectorSystem;
    GameEngine::Engine::Renderer::AnimationGraphSystem graphSystem(nullptr);
    collectorSystem.Update(world, 0.05f);
    graphSystem.Update(world, 0.05f);

    GE_AnimationEventRecord records[4] = {};
    int32_t count = -1;
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(HandleOf(&world), entity.id, records, 4, &count), GE_Result_Ok);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(NameOf(records[0]), "hit");
    EXPECT_EQ(records[0].nameId, GameEngine::HashStringId("hit"));
    EXPECT_EQ(records[0].timeSeconds, static_cast<float>(19.0 / 30.0));

    count = -1;
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(HandleOf(&world), entity.id, records, 4, &count), GE_Result_Ok);
    EXPECT_EQ(count, 0) << "a second poll in the same frame returns none";

    world.DestroyEntityImmediate(entity);
    AnimationGraphStore::Instance().ClearForTest();
    AnimationEventCollectorStore::Instance().ClearForTest();
}

TEST(AnimatorEventsAbi, PollEventsFillsWhatFitsAndRefusesBadArguments)
{
    AnimationEventCollectorStore::Instance().ClearForTest();
    World world(nullptr);
    GameEngine::Components::Animator animator{};
    animator.eventCollectorId = AnimationEventCollectorStore::Instance().Create();
    GameEngine::Animation::AnimationEventCollector* collector =
        AnimationEventCollectorStore::Instance().Get(animator.eventCollectorId);
    ASSERT_NE(collector, nullptr);
    collector->Add({0.1f, "step", {}});
    collector->Add({0.2f, "land", {}});
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, animator);
    const GameEngine::ECS::EntityHandle bare = world.CreateEntity();
    const GE_Handle handle = HandleOf(&world);

    GE_AnimationEventRecord record{};
    int32_t count = -1;
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, 1, &count), GE_Result_Ok);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(NameOf(record), "step");
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, 1, &count), GE_Result_Ok);
    ASSERT_EQ(count, 1) << "a buffer that filled leaves the rest for the next poll";
    EXPECT_EQ(NameOf(record), "land");
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, nullptr, 0, &count), GE_Result_Ok);
    EXPECT_EQ(count, 0);

    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, 1, nullptr), GE_Result_InvalidArg);
    count = -1;
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, -1, &count), GE_Result_InvalidArg);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, nullptr, 1, &count), GE_Result_InvalidArg);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(0, entity.id, &record, 1, &count), GE_Result_InvalidArg);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, bare.id, &record, 1, &count), GE_Result_NotFound)
        << "an entity with no Animator";
    world.DestroyEntityImmediate(bare);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, bare.id, &record, 1, &count), GE_Result_NotFound)
        << "an entity that is gone";

    AnimationEventCollectorStore::Instance().ClearForTest();
}

// A poll from a thread other than the engine's main thread is refused and leaves the events for the main thread.
// Marking this test's thread as the main thread holds for the rest of the process, where every test runs on it.
TEST(AnimatorEventsAbi, PollEventsRefusesAThreadOtherThanTheMainThread)
{
    AnimationEventCollectorStore::Instance().ClearForTest();
    GameEngine::ScriptManager& scripts = GameEngine::EngineCore::GetInstance().GetScriptManager();
    scripts.MarkMainThread();
    ASSERT_TRUE(scripts.IsMainThread());
    World world(nullptr);
    GameEngine::Components::Animator animator{};
    animator.eventCollectorId = AnimationEventCollectorStore::Instance().Create();
    AnimationEventCollectorStore::Instance().Get(animator.eventCollectorId)->Add({0.1f, "step", {}});
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, animator);

    GE_AnimationEventRecord record{};
    GE_Result workerResult = GE_Result_Ok;
    int32_t workerCount = -1;
    std::thread worker([&] {
        workerResult = GE_Animator_PollEventsOnEntity(HandleOf(&world), entity.id, &record, 1, &workerCount);
    });
    worker.join();
    EXPECT_EQ(workerResult, GE_Result_Fail);
    EXPECT_EQ(workerCount, 0);

    int32_t count = -1;
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(HandleOf(&world), entity.id, &record, 1, &count), GE_Result_Ok);
    EXPECT_EQ(count, 1) << "the refused poll left the event for the main thread";
    AnimationEventCollectorStore::Instance().ClearForTest();
}
