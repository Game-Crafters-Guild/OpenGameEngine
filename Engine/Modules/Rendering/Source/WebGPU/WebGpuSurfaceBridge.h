#pragma once

// Platform window -> WGPUSurface. Each desktop WSI needs a different chained
// surface source (CAMetalLayer on macOS, HWND on Windows, Xlib/Wayland on
// Linux); the browser build instead names a canvas CSS selector. Keeping the
// platform code behind this one function is what lets the rest of the backend
// stay WSI-agnostic.

#include <webgpu/webgpu.h>

#include <cstdint>

namespace GameEngine::Rendering::WebGpuSurfaceBridge
{

// Creates a surface for `glfwWindow` (a GLFWwindow*). On macOS this installs a
// CAMetalLayer on the window's content view and hands wgpu that layer, so the
// layer's lifetime is the window's. Returns nullptr when the platform is not
// supported by this slice.
WGPUSurface CreateSurface(WGPUInstance instance, void* glfwWindow);

// Framebuffer size in pixels for `glfwWindow` (0x0 on failure). The surface
// must be configured at pixel resolution, not logical points.
bool GetWindowPixelSize(void* glfwWindow, uint32_t& outWidth, uint32_t& outHeight);

} // namespace GameEngine::Rendering::WebGpuSurfaceBridge
