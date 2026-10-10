#include <gtest/gtest.h>

#include "ECS/ModuleRegistration.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "NativeScripting/UserModuleHandle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "ScopedPipelineFrame.h"
#include "TestDeviceHelper.h"
#include "Core/Application.h"

#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{
struct CapturedLifetime
{
    int* Destroyed;
    ~CapturedLifetime() { ++*Destroyed; }
};

struct ObservedFeature final : IRenderFeature
{
    bool& Alive;
    explicit ObservedFeature(bool& alive) : Alive(alive) { Alive = true; }
    ~ObservedFeature() override { Alive = false; }
};

struct CapturedFeatureUse
{
    const bool* Alive;
    bool* ReleasedWhileAlive;
    ~CapturedFeatureUse() { *ReleasedWhileAlive = *Alive; }
};

class CallbackProbeNode final : public IRenderPipelineNode
{
  public:
    CallbackProbeNode(int& destroyed, int& executed) : m_Destroyed(destroyed), m_Executed(executed) {}
    const char* GetTypeName() const override { return "CallbackProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare& d) override
    {
        auto lifetime = std::make_shared<CapturedLifetime>(&m_Destroyed);
        d.Frame.AddPass("ModuleCallback", PassPhase::kDefault, [](RenderGraph::RGPassBuilder& pass)
                        { pass.PreventCulling(); }, [lifetime, executed = &m_Executed](RenderGraph::RGContext&)
                        { ++*executed; });
    }

  private:
    int& m_Destroyed;
    int& m_Executed;
};
} // namespace

void CheckCallbackRetirement(bool dynamicModule, bool shutdown = false)
{
    NativeScripting::UserModuleHandle module;
    using RegisterCallback = bool (*)(RenderServices*, int*, int*);
    RegisterCallback registerCallback = nullptr;
    if (dynamicModule)
    {
        // The probe is staged beside this executable, like every runtime dependency.
        module = NativeScripting::UserModuleHandle(PathUtils::GetExecutableDirectory() / CALLBACK_PROBE_MODULE_NAME);
        ASSERT_TRUE(module.IsValid()) << module.LastError();
        registerCallback = module.GetFunction<RegisterCallback>("RegisterCallbackProbe");
        ASSERT_NE(registerCallback, nullptr);
    }
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        int destroyed = 0, executed = 0;
        bool featureAlive = false, captureReleasedWhileFeatureAlive = false;
        RenderServices services;
        ASSERT_TRUE(services.Initialize(device.get()));
        if (shutdown)
            services.EnsureFeature<ObservedFeature>(featureAlive);
        const auto camera = services.Views().AllocateCamera("CallbackCamera");
        const auto view = services.Views().AllocateView("CallbackView", camera);
        services.Views().SetViewRenderLayerMask(view, 1);
        services.Views().SetViewTargets(view, 0, 0, 0, ViewClearConfig{});
        const auto registerGeneration = [&](uint64_t generation)
        {
            ECS::SetActiveRegistrationModule("CallbackModule", generation);
            const bool registered = generation == 1 && registerCallback
                                        ? registerCallback(&services, &destroyed, &executed)
                                        : services.Spine().RegisterPipelineNodeType("CallbackProbe", [&]
                                                                                    { return std::make_unique<CallbackProbeNode>(destroyed, executed); }, true);
            ECS::ClearActiveRegistrationModule();
            return registered;
        };
        ASSERT_TRUE(registerGeneration(1));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "CallbackLifetime";
        blueprint.contentHash = 0xCAC11;
        RenderPipelineBlueprint::Pass pass;
        pass.id = "Callback";
        pass.type = "CallbackProbe";
        pass.enabled = true;
        pass.perView = true;
        pass.passJson = R"({"type":"CallbackProbe"})";
        blueprint.passes.push_back(pass);
        services.Spine().SetActiveRenderPipelineBlueprint(blueprint);

        RenderGraph::RGResourcePool persistent(device.get());
        RenderGraph::RGTransientPool transient(device.get());
        RenderGraph::RGUploadRing ring(device.get(), 2, 262144);
        RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
        Testing::ScopedPipelineFrame frameRegistration(services.Spine(), frame);
        frame.BeginFrame(0);
        TextureDesc desc{};
        desc.width = desc.height = 16;
        desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        const auto color = frame.ImportPersistentTexture("Callback.Color", desc);
        services.BeginWorldDrawFrame();
        services.BuildWorldBatchKeys();
        const ViewTargetsRG targets{view, color, {}, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&targets, 1);
        services.Spine().BuildFrameGraph(frame, params);
        if (shutdown)
        {
            frame.AddPass("FeatureCapture", PassPhase::kDefault,
                [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                [capture = std::make_shared<CapturedFeatureUse>(&featureAlive, &captureReleasedWhileFeatureAlive)]
                (RenderGraph::RGContext&) {});
        }
        frame.Execute();
        EXPECT_EQ(executed, 1);
        EXPECT_EQ(destroyed, 0);
        if (shutdown)
        {
            services.Shutdown();
            EXPECT_TRUE(captureReleasedWhileFeatureAlive)
                << "frame callbacks must retire before their feature/resource owners";
            EXPECT_FALSE(featureAlive);
        }
        else
        {
            ASSERT_TRUE(registerGeneration(2));
            services.Spine().ReconcileModuleNodeRegistrations("CallbackModule", 2);
        }
        EXPECT_EQ(services.Spine().PipelineInstanceForFrame(frame), nullptr);
        EXPECT_EQ(services.Spine().CountSupersededModulePipelineNodes("CallbackModule", 2), 0);
        // The real loader is allowed to unmap generation one at this point.
        // Its node's captured state AND arena destructor thunk must already be gone.
        EXPECT_EQ(destroyed, 1) << "the unload boundary still owns the previous module's frame closure";
        // A failing baseline is cleaned up while its image is still mapped. On
        // success, really unmap before the next frame resets the remaining arena.
        if (dynamicModule && destroyed == 1)
            module.Reset();
        frame.BeginFrame(1);
        EXPECT_EQ(destroyed, 1) << "the following frame must not call a retired destructor again";
        module.Reset();
        frame.Execute();
        EXPECT_EQ(executed, 1);
        services.Spine().RemovePipelineInstanceForFrame(&frame);
        if (!shutdown)
            services.Shutdown();
    }
    device->Shutdown();
}

TEST(RenderPipelineCallbackLifetime, ReconcileRetiresExecutedFrameCapturesBeforeUnload)
{
    CheckCallbackRetirement(false);
}

TEST(RenderPipelineCallbackLifetime, DynamicModuleCaptureIsDestroyedBeforeImageUnmaps)
{
    CheckCallbackRetirement(true);
}

TEST(RenderPipelineCallbackLifetime, ShutdownRetiresCallbacksWhileTheirFrameIsAlive)
{
    CheckCallbackRetirement(false, true);
}

namespace
{
// Mirrors the production window owner: remove the borrowed stream while frame,
// pools, counters and RenderServices are still alive, including early test exits.
struct FrameOwner
{
    RenderServices& Services;
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    RenderGraph::RGFrame Frame;
    explicit FrameOwner(RenderServices& services, IDevice* device)
        : Services(services), Persistent(device), Transient(device), Ring(device, 2, 262144),
          Frame(device, &Persistent, &Transient, &Ring) {}
    ~FrameOwner() { Services.Spine().RemovePipelineInstanceForFrame(&Frame); }
};

void CheckStreamRetirement(bool abandonedSameFrame)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices services;
        ASSERT_TRUE(services.Initialize(device.get()));
        int destroyed = 0, executed = 0;
        const auto camera = services.Views().AllocateCamera("StreamCamera");
        const auto view = services.Views().AllocateView("StreamView", camera);
        services.Views().SetViewRenderLayerMask(view, 1);
        services.Views().SetViewTargets(view, 0, 0, 0, ViewClearConfig{});
        ECS::SetActiveRegistrationModule("StreamModule", 1);
        const bool registered = services.Spine().RegisterPipelineNodeType("CallbackProbe", [&]
                                                                          { return std::make_unique<CallbackProbeNode>(destroyed, executed); }, true);
        ECS::ClearActiveRegistrationModule();
        ASSERT_TRUE(registered);
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "StreamLifetime";
        blueprint.contentHash = 0xCAC12;
        RenderPipelineBlueprint::Pass pass;
        pass.id = "Callback";
        pass.type = "CallbackProbe";
        pass.enabled = pass.perView = true;
        pass.passJson = R"({"type":"CallbackProbe"})";
        blueprint.passes.push_back(pass);
        services.Spine().SetActiveRenderPipelineBlueprint(blueprint);

        std::vector<std::unique_ptr<FrameOwner>> frames;
        services.BeginWorldDrawFrame();
        for (unsigned i = 0; i < 17; ++i) // crosses the existing 16-stream cache cap
        {
            if (!abandonedSameFrame)
                services.BeginWorldDrawFrame();
            auto frame = std::make_unique<FrameOwner>(services, device.get());
            frame->Frame.BeginFrame(i);
            TextureDesc desc{};
            desc.width = desc.height = 16;
            desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
            desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
            const auto color = frame->Frame.ImportPersistentTexture("Stream.Color", desc);
            services.BuildWorldBatchKeys();
            const ViewTargetsRG targets{view, color, {}, {}};
            RenderServices::FrameGraphBuildParamsRG params{};
            params.ViewTargets = std::span<const ViewTargetsRG>(&targets, 1);
            services.Spine().BuildFrameGraph(frame->Frame, params);
            if (!abandonedSameFrame)
                frame->Frame.Execute();
            frames.push_back(std::move(frame));
        }
        EXPECT_EQ(destroyed, 1) << "eviction must retire the lost stream's closure";
        EXPECT_EQ(executed, abandonedSameFrame ? 0 : 17);
        EXPECT_EQ(frames.front()->Frame.Graph().PassCount(), 0);
        frames.front()->Frame.Execute();
        EXPECT_EQ(executed, abandonedSameFrame ? 0 : 17);
        // An unrelated module with no node registrations must leave the streams alone.
        services.Spine().ReconcileModuleNodeRegistrations("UnrelatedModule", 2);
        EXPECT_EQ(destroyed, 1);
        // This generation drops its type entirely; remaining stream nodes and
        // callbacks must both retire before its old image could be unmapped.
        services.Spine().ReconcileModuleNodeRegistrations("StreamModule", 2);
        EXPECT_EQ(destroyed, 17);
        for (auto& frame : frames)
        {
            const auto index = frame->Frame.FrameIndex();
            services.Spine().RemovePipelineInstanceForFrame(&frame->Frame);
            services.Spine().RemovePipelineInstanceForFrame(&frame->Frame);
            frame->Frame.Execute();
            EXPECT_EQ(frame->Frame.FrameIndex(), index);
            frame->Frame.BeginFrame(index + 100);
        }
        EXPECT_EQ(destroyed, 17);
        EXPECT_EQ(executed, abandonedSameFrame ? 0 : 17);
        frames.clear();
        // No dead stack/heap frame is retained when the documented owner pair is used.
        services.Spine().ReconcileModuleNodeRegistrations("StreamModule", 3);
        services.Shutdown();
    }
    device->Shutdown();
}
} // namespace

TEST(RenderPipelineCallbackLifetime, SubmittedInactiveStreamsRetireBeforeEvictionAndReload)
{
    CheckStreamRetirement(false);
}

TEST(RenderPipelineCallbackLifetime, AbandonedSameFrameStreamsCannotExecuteAfterEviction)
{
    CheckStreamRetirement(true);
}

TEST(RenderPipelineCallbackLifetime, DiscardPreservesPoolResourcesAndDoesNotAdvanceFrame)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderGraph::RGResourcePool persistent(device.get());
        RenderGraph::RGTransientPool transient(device.get());
        RenderGraph::RGUploadRing ring(device.get(), 2, 4096);
        RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
        frame.BeginFrame(5);
        TextureDesc desc{};
        desc.width = desc.height = 16;
        desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        const auto color = frame.ImportPersistentTexture("Retained.Color", desc);
        const auto physical = frame.PhysicalTexture(color);
        int destroyed = 0;
        auto state = std::make_shared<CapturedLifetime>(&destroyed);
        frame.AddPass("Abandoned", 0, [&](RenderGraph::RGPassBuilder& pass)
                      { pass.AttachColor(0, color); }, [state](RenderGraph::RGContext&)
                      { ADD_FAILURE() << "discarded callback executed"; });
        state.reset();
        frame.DiscardRecordedPasses();
        frame.DiscardRecordedPasses();
        EXPECT_EQ(destroyed, 1);
        EXPECT_EQ(frame.FrameIndex(), 5);
        frame.Execute();
        frame.BeginFrame(6);
        const auto reused = frame.ImportPersistentTexture("Retained.Color", desc);
        EXPECT_EQ(frame.PhysicalTexture(reused), physical);
        EXPECT_EQ(destroyed, 1);
    }
    device->Shutdown();
}

TEST(RenderPipelineCallbackLifetime, RemovingAnUnregisteredFrameDoesNotDiscardItsPasses)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices services;
        ASSERT_TRUE(services.Initialize(device.get()));
        int destroyed = 0, executed = 0;
        FrameOwner owner(services, device.get());
        owner.Frame.BeginFrame(0);
        owner.Frame.AddPass("Unregistered", PassPhase::kDefault, [](RenderGraph::RGPassBuilder& pass)
                            { pass.PreventCulling(); }, [lifetime = std::make_shared<CapturedLifetime>(&destroyed), &executed](RenderGraph::RGContext&)
                            { ++executed; });
        services.Spine().RemovePipelineInstanceForFrame(&owner.Frame);
        services.Spine().RemovePipelineInstanceForFrame(&owner.Frame);
        EXPECT_EQ(destroyed, 0);
        owner.Frame.Execute();
        EXPECT_EQ(executed, 1);
        owner.Frame.DiscardRecordedPasses();
        EXPECT_EQ(destroyed, 1);
        services.Shutdown();
    }
    device->Shutdown();
}
