#include <gtest/gtest.h>

#include "Core/Engine.h"
#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"
#include "ECS/Entity.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Engine/GameUI/GameplayUI.h"
#include "GameViewController.h"
#include "Input/InputSystem.h"
#include "UI/Controls/Button.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIManager.h"

#include <memory>
#include <utility>

namespace GameEngine
{
struct GameViewControllerTestAccess
{
    static GameUIHost& InstallHost(GameViewController& controller)
    {
        controller.m_GameUI = std::make_unique<GameUIHost>(nullptr, nullptr, nullptr);
        return *controller.m_GameUI;
    }

    static void BindServices(GameViewController& controller, Engine::Renderer::RenderServices& services)
    {
        controller.m_RenderServices = &services;
    }

    static void SetUiFrame(GameViewController& controller, const ECS::World& world,
                           uint32_t width = 800, uint32_t height = 400)
    {
        controller.m_GameUiWidth = width;
        controller.m_GameUiHeight = height;
        controller.m_GameUiWorldId = world.GetWorldId();
        controller.m_GameUiWorldGeneration = world.GetLifecycleResetGeneration();
    }
};
} // namespace GameEngine

using namespace GameEngine;

namespace
{
class GameViewControllerInputTests : public testing::Test
{
  protected:
    GameUIHost OtherHost{nullptr, nullptr, nullptr};
    GameViewController Controller{nullptr, {}};
    GameUIHost* Host = nullptr;
    ECS::World* World = nullptr;
    Button* Target = nullptr;
    int Clicks = 0;
    int Presses = 0;
    int Cancels = 0;

    void SetUp() override
    {
        World = EngineCore::GetInstance().EnsurePrimaryWorld();
        ASSERT_NE(World, nullptr);
        World->Clear();
        Host = &GameViewControllerTestAccess::InstallHost(Controller);
        auto root = std::make_unique<UIElement>();
        auto button = std::make_unique<Button>();
        Target = button.get();
        Target->SetOnClick([this](auto&)
                           {
            ++Clicks;
            EXPECT_EQ(GameUI::GetHost(), Host);
            EXPECT_EQ(GameUI::GetUIManager(), Host->GetUIManager()); });
        Target->RegisterEventHandler(kEventMouseDown, [this](auto&)
                                     { ++Presses; });
        Target->RegisterEventHandler(kEventMouseCancel, [this](auto&)
                                     {
            ++Cancels;
            EXPECT_EQ(GameUI::GetHost(), Host);
            EXPECT_EQ(GameUI::GetUIManager(), Host->GetUIManager()); });
        root->AddChild(std::move(button));
        Host->GetUIManager()->SetRoot(std::move(root));
        UILayoutAccess::SetLastLayoutRect(*Host->GetUIManager()->GetRootElement(), 0, 0, 800, 400);
        UILayoutAccess::SetLastLayoutRect(*Target, 200, 100, 100, 80);
        GameViewControllerTestAccess::SetUiFrame(Controller, *World);
    }

    void TearDown() override
    {
        Controller.HandleGameUiPointer(false, 0, 0, false, 0);
        GameUI::SetHost(nullptr);
        World->Clear();
    }

    void Press() { Controller.HandleGameUiPointer(true, .3125f, .35f, true, 0); }
    void Release() { Controller.HandleGameUiPointer(true, .3125f, .35f, false, 0); }
};

TEST_F(GameViewControllerInputTests, MapsAgainstLastUiExtentWithoutRendering)
{
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
    // A different recorded UI extent uses different normalized coordinates for
    // the same gameplay button. Neither input sequence invokes a render path.
    GameViewControllerTestAccess::SetUiFrame(Controller, *World, 1600, 800);
    Controller.HandleGameUiPointer(true, .15625f, .175f, true, 0);
    Controller.HandleGameUiPointer(true, .15625f, .175f, false, 0);
    EXPECT_EQ(Clicks, 2);
    EXPECT_EQ(Presses, 2);
}

TEST_F(GameViewControllerInputTests, ActivatesReceivingHostBeforeButtonCallbacks)
{
    GameUI::SetHost(&OtherHost);
    Press();
    GameUI::SetHost(&OtherHost);
    Release();
    EXPECT_EQ(Clicks, 1);
    EXPECT_EQ(GameUI::GetHost(), Host);
}

TEST_F(GameViewControllerInputTests, CarriesReleaseModifiersIntoTheReceivingHost)
{
    int modifiers = -1;
    Target->SetOnClick([&](UIEvent& event) { modifiers = event.Mods; });
    Controller.HandleGameUiPointer(true, .3125f, .35f, true, Input::kModControl);
    Controller.HandleGameUiPointer(true, .3125f, .35f, false, Input::kModShift);
    EXPECT_EQ(modifiers, Input::kModShift);
    Press();
    Release();
    EXPECT_EQ(modifiers, 0);
}

TEST_F(GameViewControllerInputTests, UnsetExtentDoesNotDeliverPointerEdges)
{
    for (const auto extent : {std::pair{0u, 400u}, std::pair{800u, 0u}, std::pair{0u, 0u}})
    {
        GameViewControllerTestAccess::SetUiFrame(Controller, *World, extent.first, extent.second);
        Press();
        Release();
    }
    EXPECT_EQ(Presses, 0);
    EXPECT_EQ(Clicks, 0);
    GameViewControllerTestAccess::SetUiFrame(Controller, *World);
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewControllerInputTests, ForeignWorldUiFrameCancelsWithoutAnOrphanClick)
{
    Press();
    ECS::World foreign(nullptr);
    ASSERT_NE(foreign.GetWorldId(), World->GetWorldId());
    GameViewControllerTestAccess::SetUiFrame(Controller, foreign);
    GameUI::SetHost(&OtherHost);
    Release();
    EXPECT_EQ(Cancels, 1);
    EXPECT_FALSE(Target->HasClass("pressed"));
    EXPECT_EQ(Clicks, 0);
    GameViewControllerTestAccess::SetUiFrame(Controller, *World);
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewControllerInputTests, WorldResetInvalidatesPreviousUiFrameInput)
{
    Press();
    const auto generation = World->GetLifecycleResetGeneration();
    World->Clear();
    ASSERT_NE(World->GetLifecycleResetGeneration(), generation);
    GameUI::SetHost(&OtherHost);
    Release();
    EXPECT_EQ(Cancels, 1);
    EXPECT_EQ(Clicks, 0);
    EXPECT_FALSE(Target->HasClass("pressed"));
    GameViewControllerTestAccess::SetUiFrame(Controller, *World);
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

TEST_F(GameViewControllerInputTests, ExplicitCancellationUsesReceivingHost)
{
    Press();
    GameUI::SetHost(&OtherHost);
    Controller.HandleGameUiPointer(false, 0, 0, false, 0);
    EXPECT_EQ(Cancels, 1);
    EXPECT_FALSE(Target->HasClass("pressed"));
    Release();
    EXPECT_EQ(Clicks, 0);
}
TEST_F(GameViewControllerInputTests, PressBeforeValidExtentCannotArmOnLaterHeldMovement)
{
    GameViewControllerTestAccess::SetUiFrame(Controller, *World, 0, 0);
    Press();
    GameViewControllerTestAccess::SetUiFrame(Controller, *World);
    Controller.HandleGameUiPointer(true, .3125f, .35f, true, 0); // held movement, not another physical press
    Release();
    EXPECT_EQ(Presses, 0);
    EXPECT_EQ(Clicks, 0);
    Press();
    Release();
    EXPECT_EQ(Clicks, 1);
}

class GameViewControllerReadbackTests : public testing::Test
{
  protected:
    std::unique_ptr<Rendering::IDevice> Device;
    Engine::Renderer::RenderServices Services;
    ECS::World World;
    GameViewController Controller{nullptr, {}};
    ECS::EntityHandle CameraEntity;
    std::unique_ptr<Rendering::RenderGraph::RGResourcePool> Persistent;
    std::unique_ptr<Rendering::RenderGraph::RGTransientPool> Transient;
    std::unique_ptr<Rendering::RenderGraph::RGUploadRing> Upload;

    void SetUp() override
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(Services.Initialize(Device.get()));
        GameViewControllerTestAccess::InstallHost(Controller);
        GameViewControllerTestAccess::BindServices(Controller, Services);
        AddCamera();
        Persistent = std::make_unique<Rendering::RenderGraph::RGResourcePool>(Device.get());
        Transient = std::make_unique<Rendering::RenderGraph::RGTransientPool>(Device.get());
        Upload = std::make_unique<Rendering::RenderGraph::RGUploadRing>(Device.get(), 2, 262144);
    }

    void TearDown() override
    {
        Controller.DeactivateRenderView();
        GameUI::SetHost(nullptr);
        World.Clear();
        Upload.reset();
        Transient.reset();
        Persistent.reset();
        Services.Shutdown();
        if (Device)
            Device->Shutdown();
    }

    void AddCamera()
    {
        CameraEntity = World.CreateEntity();
        Components::Camera camera;
        camera.FovY = 48.0f;
        camera.ExposureControl = Components::ExposureMode::Auto;
        World.AddComponentImmediate(CameraEntity, camera);
        Components::Transform transform;
        transform.matrix[12] = 12.0f;
        transform.matrix[13] = 3.0f;
        World.AddComponentImmediate(CameraEntity, transform);
    }

    void Declare(uint64_t frameIndex)
    {
        Rendering::RenderGraph::RGFrame frame(Device.get(), Persistent.get(), Transient.get(), Upload.get());
        frame.BeginFrame(frameIndex);
        Engine::Renderer::Pipeline::ViewTargetsRG targets;
        ASSERT_TRUE(Controller.DeclareTargetsRG(frame, 160, 90, 1, &World, targets));
        Device->WaitForIdle();
    }
};

TEST_F(GameViewControllerReadbackTests, SnapshotTracksDeclaredCameraAndSurvivesAnExtractionWait)
{
    EXPECT_FALSE(Controller.GetDeclaredCamera().has_value());
    Controller.PrepareForActivation(&World, 160, 90);
    EXPECT_FALSE(Controller.GetDeclaredCamera().has_value()) << "activation is not a declared frame";
    const auto view = Controller.GetViewId();
    Services.Views().MarkWorldExtracted(World.GetWorldId(), 7);
    Services.Views().MarkViewExtracted(view, 7);
    Declare(7);
    ASSERT_TRUE(Controller.GetDeclaredCamera().has_value());
    EXPECT_EQ(Controller.GetDeclaredCamera()->FrameIndex, 7u);
    EXPECT_FLOAT_EQ(Controller.GetDeclaredCamera()->Data.cameraPos[0], 12.0f);
    EXPECT_FLOAT_EQ(Controller.GetDeclaredCamera()->Data.cameraPos[1], 3.0f);
    EXPECT_EQ(Controller.GetDeclaredCamera()->Width, 160u);
    EXPECT_EQ(Controller.GetDeclaredCamera()->Height, 90u);
    EXPECT_EQ(Controller.GetDeclaredCamera()->ExposureMode, Components::ExposureMode::Auto);

    World.GetComponentForWrite<Components::Transform>(CameraEntity)->matrix[12] = 17.0f;
    Services.Views().MarkWorldExtracted(World.GetWorldId(), 8);
    Declare(8);
    EXPECT_TRUE(Controller.IsWaitingForExtraction());
    ASSERT_TRUE(Controller.GetDeclaredCamera().has_value());
    EXPECT_EQ(Controller.GetDeclaredCamera()->FrameIndex, 7u);
    EXPECT_FLOAT_EQ(Controller.GetDeclaredCamera()->Data.cameraPos[0], 12.0f);
    Services.Views().MarkViewExtracted(view, 8);
    Declare(9);
    EXPECT_FALSE(Controller.IsWaitingForExtraction());
    ASSERT_TRUE(Controller.GetDeclaredCamera().has_value());
    EXPECT_EQ(Controller.GetDeclaredCamera()->FrameIndex, 9u);
    EXPECT_FLOAT_EQ(Controller.GetDeclaredCamera()->Data.cameraPos[0], 17.0f);

    Controller.PrepareForActivation(nullptr, 160, 90);
    EXPECT_FALSE(Controller.GetDeclaredCamera().has_value());
    Services.Views().MarkViewExtracted(view, 8);
    Declare(10);
    ASSERT_TRUE(Controller.GetDeclaredCamera().has_value());
    Controller.DeactivateRenderView();
    EXPECT_FALSE(Controller.GetDeclaredCamera().has_value());
}

TEST_F(GameViewControllerReadbackTests, MeteringFollowsAutoCameraAndReleasesOnlyItsOwnView)
{
    Controller.PrepareForActivation(&World, 160, 90);
    const auto view = Controller.GetViewId();
    auto* readback = Services.GetFeature<Engine::Renderer::ExposureReadbackFeature>();
    ASSERT_NE(readback, nullptr);
    EXPECT_TRUE(readback->IsReadbackEnabled(view));
    const Rendering::ViewId otherView = view + 100;
    readback->SetReadbackEnabled(otherView, true);
    float scale = 123.0f;
    EXPECT_FALSE(readback->TryResolveAdaptedExposure(view, scale));
    EXPECT_FLOAT_EQ(scale, 123.0f) << "no invented metered exposure before a completed GPU sample";

    World.GetComponentForWrite<Components::Camera>(CameraEntity)->ExposureControl = Components::ExposureMode::Fixed;
    Controller.PrepareForActivation(&World, 160, 90);
    EXPECT_FALSE(readback->IsReadbackEnabled(view));
    World.GetComponentForWrite<Components::Camera>(CameraEntity)->ExposureControl = Components::ExposureMode::Auto;
    Controller.PrepareForActivation(&World, 160, 90);
    EXPECT_TRUE(readback->IsReadbackEnabled(view));
    Controller.PrepareForActivation(nullptr, 160, 90);
    EXPECT_FALSE(readback->IsReadbackEnabled(view));
    Controller.PrepareForActivation(&World, 160, 90);
    EXPECT_TRUE(readback->IsReadbackEnabled(view));
    Controller.DeactivateRenderView();
    EXPECT_FALSE(readback->IsReadbackEnabled(view));
    EXPECT_TRUE(readback->IsReadbackEnabled(otherView));
    readback->SetReadbackEnabled(otherView, false);
}

} // namespace
