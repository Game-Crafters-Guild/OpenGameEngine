// RuntimeHost's frame contract, on a real window and device: one rendered frame calls the
// composite hook once, and a readback the hook declares on the composited image resolves
// after the frame, at the window's framebuffer size. The host stamps the frame's readbacks
// after Execute: a stamp taken before it carries the first frame's empty token, which never
// reports completion, so the readback would not resolve. A framebuffer resize recreates the
// swapchain at the new size as the resize arrives, not at the next out-of-date acquire.
//
// The host opens a visible 64 x 64 window (RuntimeHostDesc has no hidden mode). It stays open
// for the fixture's whole run, a frame or two plus the engine bring-up and shutdown: about 2 s
// with warm caches, about 30 s cold.
// Skips when no window or Vulkan device can be created.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Hosting/RuntimeFrameReadback.h"
#include "Engine/Hosting/RuntimeHost.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Platform/Window.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Scripting/ScriptsConfig.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <thread>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kWindowSize = 64;

fs::path ResolveBuildShader(const fs::path& path)
{
    const fs::path candidate = fs::path(ENGINE_TEST_BUILD_DIR) / path;
    std::error_code ec;
    return fs::is_regular_file(candidate, ec) ? candidate : fs::path{};
}

void WriteFile(const fs::path& path, const char* text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

class HostTestApplication final : public Application
{
public:
    using Application::Application;
};

// What the composite hook saw during the frame.
struct CompositeProbe
{
    int Calls = 0;
    std::shared_ptr<Rendering::RGReadbackTicket> Ticket;

    void OnFrameComposited(RuntimeFrameReadback& readback)
    {
        ++Calls;
        Ticket = Rendering::RequestTextureReadbackRG(&readback.GetDevice(), readback.GetFrame(),
                                                     readback.GetComposite().PresentSrc,
                                                     "RuntimeHostTests.Composite");
    }
};

// Brings up the engine with a project holding a pass-less pipeline and an empty scene, and a
// host over it. TearDown releases everything whether or not the test body returned early.
class RuntimeHostFrame : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_PreviousCwd = fs::current_path();
        m_Root = fs::temp_directory_path() / ("runtime-host-" + GUID::Generate().ToString());
        // A pipeline with no passes presents the host's own colour target; the empty scene gets
        // the host's default camera, so the frame has a view.
        WriteFile(m_Root / "Assets/RenderPipelines/HostTest.rendergraph",
                  R"({"schemaVersion":2,"pipelineName":"HostTest","passes":[],"outputs":{"FinalColor":"View.Color"}})");
        WriteFile(m_Root / "Assets/Scenes/Empty.scene", "[scene name=\"Empty\" version=1]\n");

        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "RuntimeHostTest";
        config.WindowWidth = kWindowSize;
        config.WindowHeight = kWindowSize;
        config.WorkspaceDirectory = m_Root.string();
        config.AssetDirectory = "Assets";
        m_Application = std::make_unique<HostTestApplication>(config);
        ASSERT_TRUE(m_Application->Initialize());
        Rendering::Utils::SetShaderFileLoader(nullptr);
        Rendering::Utils::SetShaderPathResolver(&ResolveBuildShader);
        EngineCore::GetInstance().GetAssetManager().WaitForStartupScan(kAssetSourceAliasProject);

        RuntimeHostDesc desc;
        desc.Title = "RuntimeHostTest";
        desc.WindowWidth = kWindowSize;
        desc.WindowHeight = kWindowSize;
        desc.VSync = false;
        desc.RenderPipeline = "RenderPipelines/HostTest.rendergraph";
        RuntimeHostHooks hooks;
        hooks.OnDeviceFailed = [this] { ++m_DeviceFailures; };
        hooks.OnFrameComposited = [this](RuntimeFrameReadback& readback) { m_Probe.OnFrameComposited(readback); };
        m_Host = std::make_unique<RuntimeHost>(*m_Application, desc, std::move(hooks));
        if (!m_Host->InitWindow() || !m_Host->InitRendering())
            GTEST_SKIP() << "no window or Vulkan device could be created";
    }

    void TearDown() override
    {
        m_Probe.Ticket.reset();
        if (m_Host)
            m_Host->Shutdown();
        m_Host.reset();
        if (m_Application)
            m_Application->Shutdown();
        m_Application.reset();
        std::error_code ec;
        fs::current_path(m_PreviousCwd, ec);
        fs::remove_all(m_Root, ec);
    }

    fs::path m_PreviousCwd;
    fs::path m_Root;
    std::unique_ptr<HostTestApplication> m_Application;
    std::unique_ptr<RuntimeHost> m_Host;
    CompositeProbe m_Probe;
    int m_DeviceFailures = 0;
};
} // namespace

TEST_F(RuntimeHostFrame, OneFrameCallsTheCompositeHookOnceAndItsReadbackResolves)
{
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    ASSERT_TRUE(m_Host->OpenScene("Scenes/Empty.scene"));

    m_Host->Render(1.0f / 60.0f);
    m_Host->GetRenderDeviceContext()->GetDevice()->WaitForIdle();

    EXPECT_EQ(m_Probe.Calls, 1) << "one rendered frame calls the composite hook once";
    EXPECT_EQ(m_DeviceFailures, 0);
    EXPECT_FALSE(m_Host->GetRenderDeviceContext()->GetDevice()->IsVsyncEnabled())
        << "the device takes the descriptor's vsync";
    ASSERT_TRUE(m_Probe.Ticket) << "the hook's readback of the composited image was not declared";
    Rendering::ViewReadbackResult result{};
    ASSERT_TRUE(m_Probe.Ticket->TryGet(result))
        << "a readback declared in the hook resolves once the frame finished: the host stamps "
           "the frame's readbacks after Execute";
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    m_Host->GetWindow()->GetFramebufferSize(framebufferWidth, framebufferHeight);
    EXPECT_EQ(result.width, static_cast<uint32_t>(framebufferWidth));
    EXPECT_EQ(result.height, static_cast<uint32_t>(framebufferHeight));
}

TEST_F(RuntimeHostFrame, AFramebufferResizeRecreatesTheSwapchainAtTheNewSize)
{
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    ASSERT_TRUE(m_Host->OpenScene("Scenes/Empty.scene"));
    m_Host->Render(1.0f / 60.0f);
    Rendering::IDevice& device = *m_Host->GetRenderDeviceContext()->GetDevice();
    uint32_t initialWidth = 0;
    uint32_t initialHeight = 0;
    ASSERT_TRUE(device.GetSwapchainSize(initialWidth, initialHeight));

    // Larger than any minimum window size the platform imposes on the 64 x 64 request.
    constexpr int kResizedWidth = 320;
    constexpr int kResizedHeight = 240;
    m_Host->GetWindow()->SetWindowSize(kResizedWidth, kResizedHeight);
    // Win32 resizes synchronously; X11 round-trips through the window manager, so the events
    // are pumped until the framebuffer changes, for at most kResizeWaitPolls x 10 ms (2 s).
    constexpr int kResizeWaitPolls = 200;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    for (int poll = 0; poll < kResizeWaitPolls; ++poll)
    {
        Platform::Window::PollEvents();
        m_Host->GetWindow()->GetFramebufferSize(framebufferWidth, framebufferHeight);
        if (static_cast<uint32_t>(framebufferWidth) != initialWidth &&
            static_cast<uint32_t>(framebufferHeight) != initialHeight)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(static_cast<uint32_t>(framebufferWidth), initialWidth) << "the window did not resize";
    ASSERT_NE(static_cast<uint32_t>(framebufferHeight), initialHeight) << "the window did not resize";

    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
    ASSERT_TRUE(device.GetSwapchainSize(swapchainWidth, swapchainHeight));
    EXPECT_EQ(swapchainWidth, static_cast<uint32_t>(framebufferWidth))
        << "the swapchain follows the framebuffer when it changes, before any acquire: a browser "
           "canvas never reports the resize as out of date";
    EXPECT_EQ(swapchainHeight, static_cast<uint32_t>(framebufferHeight));

    m_Probe.Calls = 0;
    m_Host->Render(1.0f / 60.0f);
    device.WaitForIdle();
    EXPECT_EQ(m_Probe.Calls, 1) << "the first frame after the resize renders";
    EXPECT_EQ(m_DeviceFailures, 0);
}
