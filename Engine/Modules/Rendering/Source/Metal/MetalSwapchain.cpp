#include "MetalSwapchain.h"

#include "MetalDevice.h"
#include "MetalLayerBridge.h"
#include "MetalMappings.h"

#include "Logger/Logger.h"

#include <cstdlib>

namespace GameEngine
{
namespace Rendering
{

bool MetalSwapchain::Initialize(MetalDevice& device, void* glfwWindow, uint32_t width, uint32_t height, bool vsync)
{
    m_GlfwWindow = glfwWindow;
    m_Width = width;
    m_Height = height;

    CA::MetalLayer* layer = CA::MetalLayer::layer(); // autoreleased
    if (layer == nullptr)
    {
        Logger::Log::Error("MetalSwapchain: failed to create CAMetalLayer");
        return false;
    }
    layer->retain();
    m_Layer = layer;

    m_Layer->setDevice(device.GetMTLDevice());
    m_Layer->setPixelFormat(MetalMappings::ToMTLPixelFormat(m_Format));
    // framebufferOnly textures reject blit reads; the engine relies on
    // swapchain readback (thumbnails, tests), so keep it off.
    // GE_METAL_FBO_ONLY=1 flips it on for fill-rate A/B experiments (drawable
    // compression) — readback paths will fail while set.
    static const bool kFramebufferOnly = []() {
        const char* env = std::getenv("GE_METAL_FBO_ONLY");
        return env != nullptr && env[0] != '0';
    }();
    m_Layer->setFramebufferOnly(kFramebufferOnly);
    m_Layer->setDrawableSize(CGSize{static_cast<double>(width), static_cast<double>(height)});
    MetalLayerBridge::SetLayerDisplaySyncEnabled(m_Layer, vsync);
    MetalLayerBridge::SetLayerMaximumDrawableCount(m_Layer, kImageCount);

    if (!MetalLayerBridge::AttachLayerToGLFWWindow(glfwWindow, m_Layer))
    {
        Logger::Log::Error("MetalSwapchain: failed to attach CAMetalLayer to window");
        m_Layer->release();
        m_Layer = nullptr;
        return false;
    }

    // No frames are in flight during initialization, so no drain is needed.
    SyncDrawableSizeToLayerBounds(nullptr);

    // Stable slot handles; the underlying texture is repointed per acquire.
    std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
    for (uint32_t i = 0; i < kImageCount; ++i)
    {
        MetalTexture slot{};
        slot.format = m_Format;
        slot.width = width;
        slot.height = height;
        slot.usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc;
        slot.isSwapchainSlot = true;
        slot.debugName = "SwapchainSlot" + std::to_string(i);
        m_SlotTextures[i] = FromGeneric<TextureTag>(device.m_Textures.Create(std::move(slot)));
    }
    return true;
}

void MetalSwapchain::Shutdown(MetalDevice& device)
{
    if (m_CurrentDrawable != nullptr)
    {
        m_CurrentDrawable->release();
        m_CurrentDrawable = nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
    for (auto& handle : m_SlotTextures)
    {
        if (handle.IsValid() && device.m_Textures.IsValid(ToGeneric(handle)))
        {
            device.m_Textures.Destroy(ToGeneric(handle));
        }
        handle = TextureHandle{};
    }
    if (m_Layer != nullptr)
    {
        m_Layer->release();
        m_Layer = nullptr;
    }
}

void MetalSwapchain::SetDisplaySync(bool enabled)
{
    if (m_Layer != nullptr)
    {
        MetalLayerBridge::SetLayerDisplaySyncEnabled(m_Layer, enabled);
    }
}

void MetalSwapchain::SetScRGBOutput(MetalDevice& device, bool enabled)
{
    if (m_Layer == nullptr)
    {
        return;
    }
    const TextureFormat format = enabled ? TextureFormat::R16G16B16A16_FLOAT : TextureFormat::BGRA8_UNORM;
    if (format == m_Format)
    {
        return;
    }
    m_Format = format;
    m_Layer->setPixelFormat(MetalMappings::ToMTLPixelFormat(m_Format));
    MetalLayerBridge::SetLayerExtendedDynamicRange(m_Layer, enabled);

    std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
    for (const TextureHandle& handle : m_SlotTextures)
    {
        if (MetalTexture* slot = device.m_Textures.Get(ToGeneric(handle)))
        {
            slot->format = m_Format;
        }
    }
}

bool MetalSwapchain::Resize(MetalDevice& device, uint32_t width, uint32_t height)
{
    if (m_Layer == nullptr)
    {
        return false;
    }
    if (width == m_Width && height == m_Height)
    {
        return true;
    }
    // In-flight command buffers reference the current-size drawable textures;
    // reallocating them under those CBs faults the GPU. Drain first.
    device.WaitForIdle();
    m_Width = width;
    m_Height = height;
    m_Layer->setDrawableSize(CGSize{static_cast<double>(width), static_cast<double>(height)});
    return true;
}

void MetalSwapchain::SyncDrawableSizeToLayerBounds(MetalDevice* device)
{
    // CAMetalLayer has no out-of-date signal: if the OS resizes the window
    // without the engine recreating the swapchain (e.g. clamping the frame to
    // the screen during startup), the stale drawable is silently stretched to
    // the layer bounds. Track the bounds the way Vulkan tracks currentExtent.
    // The contentsScale must be refreshed first — it goes stale when the
    // window moves between displays with different backing scales, and a
    // stale scale would clamp the drawable to the wrong pixel size.
    MetalLayerBridge::SyncLayerContentsScale(m_GlfwWindow, m_Layer);
    uint32_t boundsWidth = 0;
    uint32_t boundsHeight = 0;
    if (!MetalLayerBridge::GetLayerPixelSize(m_Layer, boundsWidth, boundsHeight))
    {
        return;
    }
    if (boundsWidth == m_Width && boundsHeight == m_Height)
    {
        return;
    }
    Logger::Log::Info("MetalSwapchain: drawable {}x{} out of date vs layer bounds {}x{}; resizing",
                      m_Width, m_Height, boundsWidth, boundsHeight);
    // In-flight command buffers from the other frames still bind the old-size
    // drawable textures; reallocating the slots under them faults the GPU
    // (seen as a page-fault storm when uncapped). Drain before resizing.
    if (device != nullptr)
    {
        device->WaitForIdle();
    }
    m_Width = boundsWidth;
    m_Height = boundsHeight;
    m_Layer->setDrawableSize(CGSize{static_cast<double>(boundsWidth), static_cast<double>(boundsHeight)});
}

bool MetalSwapchain::Acquire(MetalDevice& device, uint32_t& outImageIndex)
{
    if (m_Layer == nullptr)
    {
        return false;
    }
    if (m_CurrentDrawable != nullptr)
    {
        // Acquire without an intervening present: reuse the current drawable.
        outImageIndex = m_CurrentSlot;
        return true;
    }

    SyncDrawableSizeToLayerBounds(&device);

    CA::MetalDrawable* drawable = m_Layer->nextDrawable(); // autoreleased
    if (drawable == nullptr)
    {
        Logger::Log::Warning("MetalSwapchain: nextDrawable returned null (window occluded or layer detached)");
        return false;
    }
    drawable->retain();
    m_CurrentDrawable = drawable;

    m_CurrentSlot = (m_CurrentSlot + 1) % kImageCount;

    std::lock_guard<std::recursive_mutex> lock(device.m_ResourceMutex);
    MetalTexture* slot = device.m_Textures.Get(ToGeneric(m_SlotTextures[m_CurrentSlot]));
    if (slot == nullptr)
    {
        return false;
    }
    MTL::Texture* texture = drawable->texture();
    slot->texture = texture; // borrowed from the drawable; never released by the slot
    slot->width = static_cast<uint32_t>(texture->width());
    slot->height = static_cast<uint32_t>(texture->height());

    outImageIndex = m_CurrentSlot;
    return true;
}

bool MetalSwapchain::Present(MTL::CommandBuffer* commandBuffer)
{
    if (m_CurrentDrawable == nullptr || commandBuffer == nullptr)
    {
        return false;
    }
    commandBuffer->presentDrawable(m_CurrentDrawable);
    m_CurrentDrawable->release();
    m_CurrentDrawable = nullptr;
    return true;
}

} // namespace Rendering
} // namespace GameEngine
