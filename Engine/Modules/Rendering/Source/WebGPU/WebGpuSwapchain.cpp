#include "WebGpuSwapchain.h"

#if !defined(__EMSCRIPTEN__)
// wgpu-native extras (immediates, DevicePoll); Dawn/emdawnwebgpu has no wgpu.h.
#include <webgpu/wgpu.h>
#endif

#include "WebGpuConversions.h"
#include "WebGpuDevice.h"
#include "WebGpuSurfaceBridge.h"

#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine::Rendering
{

bool WebGpuSwapchain::Initialize(WebGpuDevice& device, void* glfwWindow, uint32_t width, uint32_t height, bool vsync)
{
    m_GlfwWindow = glfwWindow;
    m_Vsync = vsync;
    m_Width = width;
    m_Height = height;

    m_Surface = WebGpuSurfaceBridge::CreateSurface(device.m_Instance, glfwWindow);
    if (m_Surface == nullptr)
    {
        Logger::Log::Error("WebGpuSwapchain: surface creation failed");
        return false;
    }

    // Prefer a non-sRGB base format that also advertises its sRGB variant; the
    // sRGB view is what the engine renders through.
    WGPUSurfaceCapabilities caps{};
    if (wgpuSurfaceGetCapabilities(m_Surface, device.m_Adapter, &caps) == WGPUStatus_Success)
    {
        for (size_t i = 0; i < caps.formatCount; ++i)
        {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm)
            {
                m_Format = WGPUTextureFormat_BGRA8Unorm;
                m_ViewFormat = WGPUTextureFormat_BGRA8UnormSrgb;
                break;
            }
            if (caps.formats[i] == WGPUTextureFormat_RGBA8Unorm)
            {
                m_Format = WGPUTextureFormat_RGBA8Unorm;
                m_ViewFormat = WGPUTextureFormat_RGBA8UnormSrgb;
            }
        }
        // Uncapped present mode: prefer Mailbox, fall back to Immediate — the
        // surface advertises a subset (macOS/Metal: [Fifo, Immediate]) and
        // wgpu-native ABORTS on a mode outside it. Fifo is always supported.
        m_UncappedPresentMode = WGPUPresentMode_Fifo;
        for (size_t i = 0; i < caps.presentModeCount; ++i)
        {
            if (caps.presentModes[i] == WGPUPresentMode_Mailbox)
            {
                m_UncappedPresentMode = WGPUPresentMode_Mailbox;
                break;
            }
            if (caps.presentModes[i] == WGPUPresentMode_Immediate)
            {
                m_UncappedPresentMode = WGPUPresentMode_Immediate;
            }
        }
        wgpuSurfaceCapabilitiesFreeMembers(caps);
    }

    // One stable handle, repointed at each frame's surface texture.
    WebGpuTexture slot{};
    slot.format = m_ViewFormat;
    slot.width = width;
    slot.height = height;
    slot.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    slot.isSurfaceSlot = true;
    slot.debugName = "WebGpuSurfaceSlot";
    {
        std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
        m_SlotTexture = FromGeneric<TextureTag>(device.m_Textures.Create(std::move(slot)));
    }

    Configure(device);
    return true;
}

void WebGpuSwapchain::Configure(WebGpuDevice& device)
{
    if (m_Surface == nullptr || m_Width == 0 || m_Height == 0)
    {
        return;
    }

    const WGPUTextureFormat viewFormats[] = {m_ViewFormat};

    WGPUSurfaceConfiguration config{};
    config.device = device.m_Device;
    config.format = m_Format;
    config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    config.width = m_Width;
    config.height = m_Height;
    config.viewFormatCount = 1;
    config.viewFormats = viewFormats;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode = m_Vsync ? WGPUPresentMode_Fifo : m_UncappedPresentMode;

    wgpuSurfaceConfigure(m_Surface, &config);
}

void WebGpuSwapchain::Shutdown(WebGpuDevice& device)
{
    ReleaseCurrent(device);
    for (PendingRelease& pending : m_PendingReleases)
    {
        if (pending.view != nullptr)    wgpuTextureViewRelease(pending.view);
        if (pending.texture != nullptr) wgpuTextureRelease(pending.texture);
    }
    m_PendingReleases.clear();

    if (m_SlotTexture.IsValid())
    {
        std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
        device.m_Textures.Destroy(ToGeneric(m_SlotTexture));
        m_SlotTexture = TextureHandle{};
    }

    if (m_Surface != nullptr)
    {
        wgpuSurfaceUnconfigure(m_Surface);
        wgpuSurfaceRelease(m_Surface);
        m_Surface = nullptr;
    }
}

bool WebGpuSwapchain::Resize(WebGpuDevice& device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return false;
    }
    ReleaseCurrent(device);
    m_Width = width;
    m_Height = height;
    Configure(device);

    std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
    if (WebGpuTexture* slot = device.m_Textures.Get(ToGeneric(m_SlotTexture)))
    {
        slot->width = width;
        slot->height = height;
    }
    return true;
}

void WebGpuSwapchain::SetVsync(WebGpuDevice& device, bool vsync)
{
    if (m_Vsync == vsync)
    {
        return;
    }
    m_Vsync = vsync;
    ReleaseCurrent(device);
    Configure(device);
}

bool WebGpuSwapchain::Acquire(WebGpuDevice& device)
{
    if (m_Surface == nullptr)
    {
        return false;
    }
    if (m_CurrentTexture != nullptr)
    {
        return true;
    }

    WGPUSurfaceTexture surfaceTexture{};
    wgpuSurfaceGetCurrentTexture(m_Surface, &surfaceTexture);
    if (surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
        surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
    {
        // Routinely hit on the very first acquire after configuration and on
        // window resizes: reconfigure at the current size and retry once
        // inline (the Vulkan swapchain's recreate-and-retry shape).
        if (surfaceTexture.texture != nullptr)
        {
            wgpuTextureRelease(surfaceTexture.texture);
        }
        Configure(device);
        surfaceTexture = WGPUSurfaceTexture{};
        wgpuSurfaceGetCurrentTexture(m_Surface, &surfaceTexture);
    }

    switch (static_cast<uint32_t>(surfaceTexture.status))
    {
    case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
    case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
        break;
#if !defined(__EMSCRIPTEN__)
    case WGPUSurfaceGetCurrentTextureStatus_Occluded:
        // wgpu-native extension status: the window is not being composited
        // (hidden, minimized, or not yet shown). A frame without a drawable
        // is normal — the caller skips rendering this frame.
        if (surfaceTexture.texture != nullptr)
        {
            wgpuTextureRelease(surfaceTexture.texture);
        }
        return false;
#endif
    case WGPUSurfaceGetCurrentTextureStatus_Outdated:
    case WGPUSurfaceGetCurrentTextureStatus_Lost:
        if (surfaceTexture.texture != nullptr)
        {
            wgpuTextureRelease(surfaceTexture.texture);
        }
        Logger::Log::Warning(
            "WebGpuSwapchain: surface still outdated/lost after reconfigure (status {}, {}x{})",
            static_cast<uint32_t>(surfaceTexture.status), m_Width, m_Height);
        return false;
    default:
        if (surfaceTexture.texture != nullptr)
        {
            wgpuTextureRelease(surfaceTexture.texture);
        }
        Logger::Log::Warning("WebGpuSwapchain: surface texture unavailable (status {})",
                             static_cast<uint32_t>(surfaceTexture.status));
        return false;
    }

    WGPUTextureViewDescriptor viewDesc{};
    viewDesc.label = WebGpu::MakeStringView("SurfaceView");
    viewDesc.format = m_ViewFormat;
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.mipLevelCount = 1;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;

    // Presentation forensics: a canvas that shows black while the engine runs
    // at frame rate means these acquires stopped reaching the compositor.
    // One line every 300 acquires keeps the trail without flooding.
    if ((++m_AcquireCount % 300u) == 1u)
    {
        Logger::Log::Info("WebGpuSwapchain: acquire #{} texture={} status={} {}x{} pendingReleases={}",
                          m_AcquireCount, static_cast<const void*>(surfaceTexture.texture),
                          static_cast<uint32_t>(surfaceTexture.status), m_Width, m_Height,
                          m_PendingReleases.size());
    }

    m_CurrentTexture = surfaceTexture.texture;
    m_CurrentView = wgpuTextureCreateView(m_CurrentTexture, &viewDesc);

    std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
    if (WebGpuTexture* slot = device.m_Textures.Get(ToGeneric(m_SlotTexture)))
    {
        slot->texture = m_CurrentTexture;
        slot->defaultView = m_CurrentView;
        slot->format = m_ViewFormat;
        slot->width = m_Width;
        slot->height = m_Height;
    }
    return true;
}

bool WebGpuSwapchain::Present(WebGpuDevice& device)
{
    if (m_Surface == nullptr || m_CurrentTexture == nullptr)
    {
        return false;
    }

#if defined(__EMSCRIPTEN__)
    // The browser presents when the rAF callback returns; an explicit
    // wgpuSurfacePresent aborts under emdawnwebgpu.
    const WGPUStatus status = WGPUStatus_Success;
#else
    const WGPUStatus status = wgpuSurfacePresent(m_Surface);
#endif

    m_PendingReleases.push_back(PendingRelease{m_CurrentTexture, m_CurrentView, m_RetireEpoch, false});
    m_CurrentTexture = nullptr;
    m_CurrentView = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
        if (WebGpuTexture* slot = device.m_Textures.Get(ToGeneric(m_SlotTexture)))
        {
            slot->texture = nullptr;
            slot->defaultView = nullptr;
        }
    }

    WGPUQueueWorkDoneCallbackInfo callbackInfo{};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = &WebGpuSwapchain::OnSubmittedWorkDone;
    callbackInfo.userdata1 = this;
    wgpuQueueOnSubmittedWorkDone(device.m_Queue, callbackInfo);

    return status == WGPUStatus_Success;
}

void WebGpuSwapchain::OnSubmittedWorkDone(WGPUQueueWorkDoneStatus /*status*/, WGPUStringView /*message*/,
                                          void* userdata1, void* /*userdata2*/)
{
    auto* swapchain = static_cast<WebGpuSwapchain*>(userdata1);
    if (swapchain == nullptr)
    {
        return;
    }
    if (swapchain->m_OrphanedWorkDone > 0)
    {
        --swapchain->m_OrphanedWorkDone;
        return;
    }
    // Submissions complete in order, so the oldest outstanding present is the
    // one this callback belongs to.
    for (PendingRelease& pending : swapchain->m_PendingReleases)
    {
        if (!pending.completed)
        {
            pending.completed = true;
            break;
        }
    }
}

void WebGpuSwapchain::DrainCompletedReleases()
{
    ++m_RetireEpoch;
    // Age floor matches WebGpuDevice::kFramesInFlight: the previous slot is
    // GPU-idle by then even when OnSubmittedWorkDone never runs.
    constexpr uint32_t kRetireAge = 3;
    auto shouldRetire = [this](const PendingRelease& p) {
        return p.completed || (m_RetireEpoch - p.epoch) >= kRetireAge;
    };
    auto firstKeep = std::find_if(m_PendingReleases.begin(), m_PendingReleases.end(),
                                  [&](const PendingRelease& p) { return !shouldRetire(p); });
    for (auto it = m_PendingReleases.begin(); it != firstKeep; ++it)
    {
        if (!it->completed)
        {
            ++m_OrphanedWorkDone;
        }
        if (it->view != nullptr)    wgpuTextureViewRelease(it->view);
        if (it->texture != nullptr) wgpuTextureRelease(it->texture);
    }
    m_PendingReleases.erase(m_PendingReleases.begin(), firstKeep);
}

void WebGpuSwapchain::ReleaseCurrent(WebGpuDevice& device)
{
    if (m_CurrentView != nullptr)
    {
        wgpuTextureViewRelease(m_CurrentView);
        m_CurrentView = nullptr;
    }
    if (m_CurrentTexture != nullptr)
    {
        wgpuTextureRelease(m_CurrentTexture);
        m_CurrentTexture = nullptr;
    }

    std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
    if (WebGpuTexture* slot = device.m_Textures.Get(ToGeneric(m_SlotTexture)))
    {
        slot->texture = nullptr;
        slot->defaultView = nullptr;
    }
}

TextureFormat WebGpuSwapchain::GetFormat() const
{
    return WebGpu::FromWgpuTextureFormat(m_ViewFormat);
}

} // namespace GameEngine::Rendering
