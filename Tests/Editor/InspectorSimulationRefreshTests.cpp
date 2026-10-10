#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"

#include "InspectorRegistry.h"
#include "EditorChangeNotifications.h"
#include "PlayMode/PlayModeManager.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "UI/UIElement.h"

#include "Components/Transform.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::InspectorContext;
using GameEngine::UIElement;
using GameEngine::Editor::EditorChangeNotifications;
using GameEngine::Editor::PlayModeManager;
using GameEngine::Editor::PlayModeState;

namespace
{

class InspectorSimulationRefreshTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    GameEngine::ECS::World* GetWorld() const
    {
        return EngineCore::GetInstance().EnsurePrimaryWorld();
    }

    void SetUp() override
    {
        auto* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
    }
};

// ------------------------------------------------------------------
// InspectorContext.SimulationRefreshCallbacks wiring
// ------------------------------------------------------------------

TEST_F(InspectorSimulationRefreshTests, SimulationRefreshCallbacks_DefaultsToNull)
{
    InspectorContext ctx{};
    EXPECT_EQ(ctx.SimulationRefreshCallbacks, nullptr);
}

TEST_F(InspectorSimulationRefreshTests, SimulationRefreshCallbacks_AcceptsVector)
{
    std::vector<std::function<void()>> callbacks;
    InspectorContext ctx{};
    ctx.SimulationRefreshCallbacks = &callbacks;

    EXPECT_EQ(ctx.SimulationRefreshCallbacks, &callbacks);
    EXPECT_TRUE(callbacks.empty());
}

TEST_F(InspectorSimulationRefreshTests, SimulationRefreshCallbacks_InspectorCanRegister)
{
    std::vector<std::function<void()>> callbacks;
    InspectorContext ctx{};
    ctx.SimulationRefreshCallbacks = &callbacks;

    int callCount = 0;
    // Simulate what an inspector does during construction
    if (ctx.SimulationRefreshCallbacks)
        ctx.SimulationRefreshCallbacks->push_back([&callCount]() { ++callCount; });

    ASSERT_EQ(callbacks.size(), 1u);

    // Simulate what TickSimulationRefresh does
    for (auto& cb : callbacks)
        cb();

    EXPECT_EQ(callCount, 1);
}

TEST_F(InspectorSimulationRefreshTests, SimulationRefreshCallbacks_MultipleInspectorsRegister)
{
    std::vector<std::function<void()>> callbacks;
    InspectorContext ctx{};
    ctx.SimulationRefreshCallbacks = &callbacks;

    int transformCount = 0;
    int physicsCount = 0;

    ctx.SimulationRefreshCallbacks->push_back([&transformCount]() { ++transformCount; });
    ctx.SimulationRefreshCallbacks->push_back([&physicsCount]() { ++physicsCount; });

    ASSERT_EQ(callbacks.size(), 2u);

    for (auto& cb : callbacks)
        cb();

    EXPECT_EQ(transformCount, 1);
    EXPECT_EQ(physicsCount, 1);
}

TEST_F(InspectorSimulationRefreshTests, SimulationRefreshCallbacks_ClearRemovesAll)
{
    std::vector<std::function<void()>> callbacks;
    InspectorContext ctx{};
    ctx.SimulationRefreshCallbacks = &callbacks;

    int callCount = 0;
    ctx.SimulationRefreshCallbacks->push_back([&callCount]() { ++callCount; });
    ASSERT_EQ(callbacks.size(), 1u);

    // Simulate ClearContent
    callbacks.clear();
    EXPECT_TRUE(callbacks.empty());

    // After clear, iterating should do nothing
    for (auto& cb : callbacks)
        cb();
    EXPECT_EQ(callCount, 0);
}

// ------------------------------------------------------------------
// PlayModeManager state queries used by TickSimulationRefresh
// ------------------------------------------------------------------

TEST_F(InspectorSimulationRefreshTests, PlayModeManager_EditModeIsNotPlayingOrPaused)
{
    PlayModeManager pm;
    pm.SetWorld(GetWorld());

    EXPECT_FALSE(pm.IsPlayingOrPaused());
    EXPECT_FALSE(pm.IsPlaying());
    EXPECT_FALSE(pm.IsPaused());
    EXPECT_EQ(pm.GetState(), PlayModeState::Edit);
}

// ------------------------------------------------------------------
// ContainsFocusedElement utility
// ------------------------------------------------------------------

TEST_F(InspectorSimulationRefreshTests, ContainsFocusedElement_NullRootReturnsFalse)
{
    EXPECT_FALSE(GameEngine::InspectorUI::ContainsFocusedElement(nullptr, "someId"));
}

TEST_F(InspectorSimulationRefreshTests, ContainsFocusedElement_EmptyFocusIdReturnsFalse)
{
    auto element = std::make_unique<UIElement>();
    EXPECT_FALSE(GameEngine::InspectorUI::ContainsFocusedElement(element.get(), ""));
}

TEST_F(InspectorSimulationRefreshTests, ContainsFocusedElement_NoMatchReturnsFalse)
{
    auto element = std::make_unique<UIElement>();
    element->SetId("otherElement");
    EXPECT_FALSE(GameEngine::InspectorUI::ContainsFocusedElement(element.get(), "focusTarget"));
}

// ------------------------------------------------------------------
// Integration: Simulation refresh reads live component data
// ------------------------------------------------------------------

TEST_F(InspectorSimulationRefreshTests, RefreshCallback_ReadsLiveTransformData)
{
    auto* world = GetWorld();

    // Create an entity with a Transform
    GameEngine::ECS::Entity entity = world->Create<GameEngine::Components::Transform>(
        GameEngine::Components::Transform::FromTRS(
            GameEngine::Mathematics::Vector3(1.0f, 2.0f, 3.0f),
            GameEngine::Mathematics::Quaternion(),
            GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f)));
    GameEngine::ECS::EntityHandle handle = entity.GetHandle();

    // Set up the callback mechanism (simulating what InspectorPanel + TransformInspector do)
    std::vector<std::function<void()>> callbacks;
    GameEngine::Mathematics::Vector3 lastReadPosition{};

    callbacks.push_back([world, handle, &lastReadPosition]()
    {
        auto* t = world->GetComponent<GameEngine::Components::Transform>(handle);
        if (t)
            lastReadPosition = t->GetPosition();
    });

    // First refresh: reads initial position
    for (auto& cb : callbacks)
        cb();
    EXPECT_FLOAT_EQ(lastReadPosition.x, 1.0f);
    EXPECT_FLOAT_EQ(lastReadPosition.y, 2.0f);
    EXPECT_FLOAT_EQ(lastReadPosition.z, 3.0f);

    // Simulate physics moving the entity (direct ECS write, no notifications)
    auto* t = world->GetComponentForWrite<GameEngine::Components::Transform>(handle);
    ASSERT_NE(t, nullptr);
    *t = GameEngine::Components::Transform::FromTRS(
        GameEngine::Mathematics::Vector3(10.0f, 20.0f, 30.0f),
        GameEngine::Mathematics::Quaternion(),
        GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f));

    // Second refresh: reads updated position without any notification
    for (auto& cb : callbacks)
        cb();
    EXPECT_FLOAT_EQ(lastReadPosition.x, 10.0f);
    EXPECT_FLOAT_EQ(lastReadPosition.y, 20.0f);
    EXPECT_FLOAT_EQ(lastReadPosition.z, 30.0f);
}

TEST_F(InspectorSimulationRefreshTests, RefreshCallback_SkippedAfterEntityDestroyed)
{
    auto* world = GetWorld();

    GameEngine::ECS::Entity entity = world->Create<GameEngine::Components::Transform>(
        GameEngine::Components::Transform::FromTRS(
            GameEngine::Mathematics::Vector3(1.0f, 2.0f, 3.0f),
            GameEngine::Mathematics::Quaternion(),
            GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f)));
    GameEngine::ECS::EntityHandle handle = entity.GetHandle();

    std::vector<std::function<void()>> callbacks;
    int refreshCount = 0;

    callbacks.push_back([world, handle, &refreshCount]()
    {
        if (!world->IsValid(handle))
            return;
        ++refreshCount;
    });

    // Refresh while alive
    for (auto& cb : callbacks)
        cb();
    EXPECT_EQ(refreshCount, 1);

    // Destroy entity
    world->DestroyEntity(handle);
    world->ProcessCommands();

    // Refresh after destruction — should be skipped
    for (auto& cb : callbacks)
        cb();
    EXPECT_EQ(refreshCount, 1); // unchanged
}

} // namespace
