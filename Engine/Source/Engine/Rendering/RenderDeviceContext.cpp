#include "Engine/Rendering/RenderDeviceContext.h"

#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"

#include <chrono>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;

RenderDeviceContext::RenderDeviceContext() = default;

RenderDeviceContext::~RenderDeviceContext()
{
    Shutdown();
}

bool RenderDeviceContext::Initialize(const InitParams& params)
{
    if (m_Initialized)
    {
        return true;
    }

    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };
    const auto tCtxStart = std::chrono::high_resolution_clock::now();

    m_OwnsDevice = (params.sharedDevice == nullptr);
    if (m_OwnsDevice)
    {
        // Create and own device (legacy mode).
        auto tDev = std::chrono::high_resolution_clock::now();
        m_Device = GameEngine::Rendering::DeviceFactory::CreateDevice(params.deviceDesc);
        if (!m_Device)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: DeviceFactory::CreateDevice failed");
            return false;
        }
        if (!m_Device->Initialize(params.deviceDesc))
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: device Initialize failed");
            m_Device.reset();
            return false;
        }
        Logger::Log::Info("[Startup]   Device create+init: {:.1f}ms", MsSince(tDev));
    }
    else
    {
        // Attach mode: device is owned by another context.
        m_AttachedDevice = params.sharedDevice;
        if (!m_AttachedDevice)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: sharedDevice is null");
            return false;
        }
    }

    Rendering::IDevice* device = GetDevice();
    if (!device)
    {
        Logger::Log::Error("RenderDeviceContext::Initialize: no device available");
        return false;
    }

    // Swapchain/WSI setup (if requested)
    if (params.deviceDesc.enableSwapchain)
    {
        auto tSwap = std::chrono::high_resolution_clock::now();
        if (!params.windowHandle)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: enableSwapchain=true but windowHandle is null");
            Shutdown();
            return false;
        }
        if (params.width == 0 || params.height == 0)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: invalid swapchain size {}x{}", params.width, params.height);
            Shutdown();
            return false;
        }
        m_WindowTarget = device->CreateWindowTarget(params.windowHandle, params.width, params.height);
        if (!m_WindowTarget.IsValid())
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: CreateWindowTarget failed");
            Shutdown();
            return false;
        }
        if (!device->SetActiveWindowTarget(m_WindowTarget))
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: SetActiveWindowTarget failed");
            Shutdown();
            return false;
        }
        Logger::Log::Info("[Startup]   Swapchain: {:.1f}ms", MsSince(tSwap));
    }

    if (params.createRenderServices)
    {
        auto tRS = std::chrono::high_resolution_clock::now();
        m_RenderServices = std::make_unique<GameEngine::Engine::Renderer::RenderServices>();
        if (!m_RenderServices)
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: failed to allocate RenderServices");
            Shutdown();
            return false;
        }

        if (!m_RenderServices->Initialize(device))
        {
            Logger::Log::Error("RenderDeviceContext::Initialize: RenderServices::Initialize failed");
            Shutdown();
            return false;
        }

        Logger::Log::Info("[Startup]   RenderServices: {:.1f}ms", MsSince(tRS));
    }

    Logger::Log::Info("[Startup] RenderDeviceContext TOTAL: {:.1f}ms", MsSince(tCtxStart));
    m_Initialized = true;
    return true;
}

void RenderDeviceContext::Shutdown()
{
    if (!GetDevice() && !m_RenderServices)
    {
        m_Initialized = false;
        return;
    }

    if (m_OwnsDevice)
    {
        // Owning contexts drain the entire device before teardown.
        if (Rendering::IDevice* device = GetDevice())
            device->WaitForIdle();
    }

    // Tear down RenderServices first so it can release device-backed resources
    // while the device is still valid.
    if (m_RenderServices)
    {
        m_RenderServices->Shutdown();
        m_RenderServices.reset();
    }

    // Detach/destroy this context's window target before device shutdown.
    if (Rendering::IDevice* device = GetDevice(); device && m_WindowTarget.IsValid())
    {
        device->DestroyWindowTarget(m_WindowTarget);
        m_WindowTarget = {};
    }

    // Shut down owned device last. In attach mode, caller owns device lifetime.
    if (m_OwnsDevice && m_Device)
    {
        m_Device->Shutdown();
        m_Device.reset();
    }
    m_AttachedDevice = nullptr;

    m_Initialized = false;
}

std::unique_ptr<GameEngine::Rendering::IDevice> RenderDeviceContext::ReleaseDevice()
{
    m_Initialized = false;
    return std::move(m_Device);
}

std::unique_ptr<GameEngine::Engine::Renderer::RenderServices> RenderDeviceContext::ReleaseRenderServices()
{
    m_Initialized = false;
    return std::move(m_RenderServices);
}

bool RenderDeviceContext::ActivateWindowTarget() const
{
    Rendering::IDevice* device = GetDevice();
    if (!device)
    {
        return false;
    }
    if (!m_WindowTarget.IsValid())
    {
        return true;
    }
    return device->SetActiveWindowTarget(m_WindowTarget);
}

bool RenderDeviceContext::RecreateWindowTargetSwapchain(uint32 width, uint32 height) const
{
    Rendering::IDevice* device = GetDevice();
    if (!device)
    {
        return false;
    }
    if (!m_WindowTarget.IsValid())
    {
        Logger::Log::Warning("RenderDeviceContext::RecreateWindowTargetSwapchain called without a valid window target");
        return false;
    }
    return device->RecreateWindowTargetSwapchain(m_WindowTarget, width, height);
}

RenderDeviceContext::CompileAndExecuteResult RenderDeviceContext::CompileAndExecuteRenderGraph()
{
    // Old-arm graph compile/execute is gone under RenderGraph — the host drives
    // BuildFrameGraph(RGFrame&) directly and timings come from caller chrono.
    // Empty shim until the Stage-6 editor cutover removes the last caller
    // (EditorApplication RGCsvSidecar emit).
    return CompileAndExecuteResult{};
}

} // namespace GameEngine::Engine::Renderer


