#pragma once

#include "Rendering/Core/Device.h"

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

namespace GameEngine
{
namespace Rendering
{

class MetalDevice;

// One presentable window target: owns the CAMetalLayer and a fixed set of
// "slot" texture handles. Metal drawables are transient, so each acquire
// repoints the current slot's MetalTexture at the new drawable's texture —
// handles stay stable across frames the way Vulkan swapchain images do.
class MetalSwapchain
{
  public:
    MetalSwapchain() = default;
    ~MetalSwapchain() = default;

    MetalSwapchain(const MetalSwapchain&) = delete;
    MetalSwapchain& operator=(const MetalSwapchain&) = delete;

    bool Initialize(MetalDevice& device, void* glfwWindow, uint32_t width, uint32_t height, bool vsync);
    void Shutdown(MetalDevice& device);

    bool Resize(MetalDevice& device, uint32_t width, uint32_t height);
    // CAMetalLayer.displaySyncEnabled is live-settable; no swapchain rebuild.
    void SetDisplaySync(bool enabled);
    // scRGB EDR output: fp16 drawables in extended linear sRGB. SDR restores
    // BGRA8. Caller drains GPU work first (the slot format changes).
    void SetScRGBOutput(MetalDevice& device, bool enabled);
    void* GetWindow() const { return m_GlfwWindow; }

    // Acquires the next drawable and repoints the current slot's texture.
    bool Acquire(MetalDevice& device, uint32_t& outImageIndex);
    bool HasCurrentDrawable() const { return m_CurrentDrawable != nullptr; }

    // Presents the current drawable on `commandBuffer` and drops the reference.
    bool Present(MTL::CommandBuffer* commandBuffer);

    static constexpr uint32_t kImageCount = 3;

    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    uint32_t GetCurrentSlot() const { return m_CurrentSlot; }
    TextureHandle GetSlotTexture(uint32_t index) const
    {
        return index < kImageCount ? m_SlotTextures[index] : TextureHandle{};
    }
    TextureFormat GetFormat() const { return m_Format; }

  private:
    // Resizes the drawable to match the layer bounds when they drift (display
    // move / HDR settle). Drains in-flight GPU work first so command buffers
    // referencing the old-size drawable textures can't fault when the slots
    // are reallocated. `device` may be null only before any frame is in flight.
    void SyncDrawableSizeToLayerBounds(MetalDevice* device);

    CA::MetalLayer* m_Layer = nullptr;          // owned (+1)
    CA::MetalDrawable* m_CurrentDrawable = nullptr; // owned (+1) until present
    void* m_GlfwWindow = nullptr;
    TextureHandle m_SlotTextures[kImageCount]{};
    uint32_t m_CurrentSlot = 0;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    TextureFormat m_Format = TextureFormat::BGRA8_UNORM;
};

} // namespace Rendering
} // namespace GameEngine
