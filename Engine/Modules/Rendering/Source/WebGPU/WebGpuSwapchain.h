#pragma once

#include "Rendering/Core/Device.h"

#include <webgpu/webgpu.h>

#include <vector>

namespace GameEngine::Rendering
{

class WebGpuDevice;

// One presentable window target: owns the WGPUSurface plus a single stable
// texture handle. WebGPU hands out a fresh surface texture per frame and keeps
// its own buffering internally, so there is no addressable image array like
// Vulkan's — each acquire repoints the slot at the new surface texture and
// handles stay stable across frames.
//
// The acquired texture is released after the submission that consumed it
// has completed (wgpuQueueOnSubmittedWorkDone), or after kFramesInFlight
// presents if that callback never lands. Desktop wgpu-native usually
// completes promptly; the browser path can stall the callback while rAF
// keeps acquiring, and unreleased surface textures then grow Dawn's wasm
// heap until malloc fails. An earlier-than-FIF release still drops the
// in-flight submit on the browser, so the age floor matches the device
// in-flight window.
class WebGpuSwapchain
{
  public:
    WebGpuSwapchain() = default;
    ~WebGpuSwapchain() = default;

    WebGpuSwapchain(const WebGpuSwapchain&) = delete;
    WebGpuSwapchain& operator=(const WebGpuSwapchain&) = delete;

    bool Initialize(WebGpuDevice& device, void* glfwWindow, uint32_t width, uint32_t height, bool vsync);
    void Shutdown(WebGpuDevice& device);

    bool Resize(WebGpuDevice& device, uint32_t width, uint32_t height);
    void SetVsync(WebGpuDevice& device, bool vsync);

    // Acquires the frame's surface texture and repoints the slot at it.
    bool Acquire(WebGpuDevice& device);
    bool HasCurrentTexture() const { return m_CurrentTexture != nullptr; }

    // Presents the acquired texture and queues its release behind the current
    // submission.
    bool Present(WebGpuDevice& device);

    // Releases every surface texture whose submission has completed. Called
    // once per frame by the device after it drains wgpu's event loop.
    void DrainCompletedReleases();

    void* GetWindow() const { return m_GlfwWindow; }
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    TextureHandle GetSlotTexture() const { return m_SlotTexture; }
    // The format views are created with — the sRGB variant of the configured
    // (non-sRGB) surface format, so the hardware performs the encode.
    TextureFormat GetFormat() const;

  private:
    void Configure(WebGpuDevice& device);
    void ReleaseCurrent(WebGpuDevice& device);

    static void OnSubmittedWorkDone(WGPUQueueWorkDoneStatus status, WGPUStringView message,
                                    void* userdata1, void* userdata2);

    struct PendingRelease
    {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        uint32_t epoch = 0;
        bool completed = false;
    };

    WGPUSurface m_Surface = nullptr;             // owned (+1)
    WGPUTexture m_CurrentTexture = nullptr;      // owned (+1) until presented
    WGPUTextureView m_CurrentView = nullptr;     // owned (+1) until presented
    std::vector<PendingRelease> m_PendingReleases;

    void* m_GlfwWindow = nullptr;
    TextureHandle m_SlotTexture{};
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_AcquireCount = 0; // presentation forensics (throttled acquire log)
    uint32_t m_RetireEpoch = 0;
    // Work-done callbacks still in flight for textures this drain already
    // released by age. Those callbacks must not complete a younger present.
    uint32_t m_OrphanedWorkDone = 0;
    bool m_Vsync = true;
    // Configured (non-sRGB) format; views use the sRGB variant listed in
    // viewFormats — a canvas rejects an sRGB configuration format outright.
    WGPUPresentMode m_UncappedPresentMode = WGPUPresentMode_Fifo;
    WGPUTextureFormat m_Format = WGPUTextureFormat_BGRA8Unorm;
    WGPUTextureFormat m_ViewFormat = WGPUTextureFormat_BGRA8UnormSrgb;
};

} // namespace GameEngine::Rendering
