#pragma once

#include "Rendering/Core/Device.h"
#include "Types/Types.h"

#include <memory>
#include "Rendering/CameraDerivation.h"

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::DeviceDesc;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::WindowTargetHandle;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer
{
class RenderServices;

/**
 * @brief Owns a single rendering device and its associated RenderServices.
 *
 * Centralizes shutdown ordering so higher layers do not need ad-hoc
 * WaitForIdle/cleanup calls to avoid validation errors.
 *
 * Policy:
 * - Higher layers should destroy device-using objects (UI, scene controllers, etc.)
 *   before calling Shutdown() on this context.
 * - Shutdown() is idempotent.
 */
class RenderDeviceContext final
{
  public:
    struct InitParams
    {
        Rendering::DeviceDesc deviceDesc{};
        // Optional shared device attachment mode:
        // - nullptr: this context owns its own device (legacy mode).
        // - non-null: this context attaches to an existing device and only owns
        //   per-window swapchain target + optional RenderServices.
        Rendering::IDevice* sharedDevice = nullptr;
        void* windowHandle = nullptr;
        uint32 width = 0;
        uint32 height = 0;

        // Create and own RenderServices for this device.
        bool createRenderServices = false;
    };

    RenderDeviceContext();
    ~RenderDeviceContext();

    RenderDeviceContext(const RenderDeviceContext&) = delete;
    RenderDeviceContext& operator=(const RenderDeviceContext&) = delete;

    bool Initialize(const InitParams& params);
    void Shutdown();

    bool IsInitialized() const { return m_Initialized; }

    Rendering::IDevice* GetDevice() const { return m_OwnsDevice ? m_Device.get() : m_AttachedDevice; }
    RenderServices* GetRenderServices() const { return m_RenderServices.get(); }
    Rendering::WindowTargetHandle GetWindowTarget() const { return m_WindowTarget; }
    bool HasWindowTarget() const { return m_WindowTarget.IsValid(); }
    bool ActivateWindowTarget() const;
    bool RecreateWindowTargetSwapchain(uint32 width, uint32 height) const;

    // Compile and execute whatever rendering infrastructure this context owns.
    // Dispatches to RenderServices when available.
    struct CompileAndExecuteResult
    {
        double compileMs = 0.0;
        double executeMs = 0.0;
        double variantCompileMs = 0.0;
        double prewarmMs = 0.0;
    };
    CompileAndExecuteResult CompileAndExecuteRenderGraph();

    // Advanced: release ownership (callers take over lifetime).
    std::unique_ptr<Rendering::IDevice> ReleaseDevice();
    std::unique_ptr<RenderServices> ReleaseRenderServices();

  private:
    bool m_Initialized = false;
    bool m_OwnsDevice = true;
    std::unique_ptr<Rendering::IDevice> m_Device;
    Rendering::IDevice* m_AttachedDevice = nullptr; // not owned
    Rendering::WindowTargetHandle m_WindowTarget{};
    std::unique_ptr<RenderServices> m_RenderServices;
};

} // namespace GameEngine::Engine::Renderer


