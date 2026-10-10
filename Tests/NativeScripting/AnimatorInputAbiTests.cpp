#include "Components/Animation/Animator.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "EngineLogCapture.h"
#include "Scripting/AnimatorABI.h"
#include "Scripting/ECSABI.h"
#include "StagedTestPaths.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
namespace
{
GE_Handle HandleOf(ECS::World* world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
}
}

TEST(AnimatorInputAbi, NonFiniteComponentWritesAreClampedAndLoggedPerWrite)
{
    ECS::World world(nullptr);
    Engine::Renderer::RegisterRenderWorldHooks(world);
    const auto first = world.CreateEntity();
    const auto second = world.CreateEntity();
    for (const auto entity : {first, second})
        world.AddComponentImmediate(entity, Components::Animator{});
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    Logger::Log::Warning("Animator input capture is live");
    size_t writes = 0;
    for (const auto entity : {first, second})
    {
        size_t entityWrites = 0;
        for (const float invalid : {std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::quiet_NaN(),
                                    -std::numeric_limits<float>::infinity()})
        {
            Components::Animator animator{};
            animator.speedScale = invalid;
            animator.seekTimeSeconds = invalid;
            animator.sectionStartSeconds = invalid;
            animator.sectionEndSeconds = invalid;
            ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(&world), entity.id,
                ECS::GetComponentTypeId<Components::Animator>(),
                reinterpret_cast<const uint8_t*>(&animator), sizeof(animator)), GE_Result_Ok);
            const auto* stored = world.GetComponent<Components::Animator>(entity);
            ASSERT_NE(stored, nullptr);
            EXPECT_FLOAT_EQ(stored->speedScale, 1.0f);
            EXPECT_FLOAT_EQ(stored->seekTimeSeconds, 0.0f);
            EXPECT_FLOAT_EQ(stored->sectionStartSeconds, 0.0f);
            EXPECT_FLOAT_EQ(stored->sectionEndSeconds, 0.0f);
            Logger::Log::Flush();
            EXPECT_EQ(TestLog::CountLinesContaining(lines, "Animator playback values must be finite"), ++writes);
            EXPECT_EQ(TestLog::CountLinesContaining(lines, "entity=" + std::to_string(entity.id) + ":"), ++entityWrites);
        }
    }
    const Components::Animator valid{};
    EXPECT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(&world), first.id,
        ECS::GetComponentTypeId<Components::Animator>(),
        reinterpret_cast<const uint8_t*>(&valid), sizeof(valid)), GE_Result_Ok);
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Animator input capture is live"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Animator playback values must be finite"), writes);
}

TEST(AnimatorInputAbi, NonFiniteSeekIsIgnoredAndLoggedPerRequest)
{
    ApplicationConfig config{};
    config.AssetDirectory = TestPaths::StagedEngineAssetsDir().string();
    ScriptsConfig scripts{};
    scripts.disableClr = true;
    scripts.enableHotReload = false;
    scripts.enableAsyncHotReload = false;
    scripts.enableAutoProjectGeneration = false;
    auto& engine = EngineCore::GetInstance();
    engine.SetScriptsConfig(scripts);
    ASSERT_TRUE(engine.Initialize(config));
    {
        ECS::World world(nullptr);
        const auto first = world.CreateEntity();
        const auto second = world.CreateEntity();
        Components::Animator animator{};
        animator.source = Components::AnimatorPlaybackSource::Graph;
        animator.seekTimeSeconds = 0.375f;
        for (const auto entity : {first, second})
            world.AddComponentImmediate(entity, animator);
        std::vector<std::string> lines;
        TestLog::ScopedEngineLogCapture capture(&lines);
        Logger::Log::Warning("Animator seek capture is live");
        size_t requests = 0;
        for (const auto entity : {first, second})
        {
            size_t entityRequests = 0;
            for (const float invalid : {std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::quiet_NaN(),
                                        -std::numeric_limits<float>::infinity()})
            {
                EXPECT_EQ(GE_Animator_SeekOnEntity(HandleOf(&world), entity.id, invalid), GE_Result_InvalidArg);
                const auto* stored = world.GetComponent<Components::Animator>(entity);
                EXPECT_FLOAT_EQ(stored->seekTimeSeconds, 0.375f);
                EXPECT_EQ(stored->pendingCommand, Components::AnimatorPlaybackCommand::None);
                Logger::Log::Flush();
                EXPECT_EQ(TestLog::CountLinesContaining(lines, "AnimatorApi.Seek ignored non-finite"), ++requests);
                EXPECT_EQ(TestLog::CountLinesContaining(lines, "entity=" + std::to_string(entity.id) + ":"), ++entityRequests);
            }
            EXPECT_EQ(GE_Animator_SeekOnEntity(HandleOf(&world), entity.id, 0.5f), GE_Result_Ok);
            EXPECT_FLOAT_EQ(world.GetComponent<Components::Animator>(entity)->seekTimeSeconds, 0.5f);
        }
        Logger::Log::Flush();
        EXPECT_EQ(TestLog::CountLinesContaining(lines, "Animator seek capture is live"), 1u);
        EXPECT_EQ(TestLog::CountLinesContaining(lines, "AnimatorApi.Seek ignored non-finite"), requests);
    }
    engine.Shutdown();
}
