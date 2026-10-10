#pragma once

// Pure-C++ bridge between a GLFW window's NSView and a CAMetalLayer.
// Uses the Objective-C runtime C API directly (the same mechanism metal-cpp
// is built on) so the Rendering module stays free of Objective-C++ TUs.

namespace CA
{
class MetalLayer;
}

namespace GameEngine::Rendering::MetalLayerBridge
{

// Installs `layer` as the layer of the GLFW window's content view and applies
// the view's backing scale factor as the layer contentsScale.
// `glfwWindow` is a GLFWwindow*. Returns false if the native window could not
// be resolved.
bool AttachLayerToGLFWWindow(void* glfwWindow, CA::MetalLayer* layer);

// Backing scale factor of the window's screen (1.0 on failure).
double GetWindowBackingScale(void* glfwWindow);

// Re-applies the window's current backingScaleFactor to the layer. AppKit
// does not update contentsScale on layers it didn't create, so a window
// dragged between displays with different scales keeps the stale value
// otherwise. Returns true when the scale changed.
bool SyncLayerContentsScale(void* glfwWindow, CA::MetalLayer* layer);

// Properties metal-cpp's CAMetalLayer header does not expose.
void SetLayerDisplaySyncEnabled(CA::MetalLayer* layer, bool enabled);
void SetLayerMaximumDrawableCount(CA::MetalLayer* layer, unsigned long count);

// EDR output config: extended-dynamic-range compositing plus an extended
// linear sRGB colorspace (scRGB: 1.0 = SDR reference white, values above
// reach into the display's EDR headroom). `enabled=false` restores the
// default sRGB-managed output.
void SetLayerExtendedDynamicRange(CA::MetalLayer* layer, bool enabled);

// NSScreen EDR headroom for the window's current display. `outCurrent` is
// the live headroom (1.0 until EDR engages), `outPotential` the stable
// capability. Both default to 1.0 on failure.
void GetWindowEdrHeadroom(void* glfwWindow, float& outCurrent, float& outPotential);

// Layer bounds in pixels (bounds.size * contentsScale) — the Metal analogue of
// Vulkan's surface currentExtent. Returns false until the layer is attached to
// a view (bounds still zero).
bool GetLayerPixelSize(CA::MetalLayer* layer, unsigned int& outWidth, unsigned int& outHeight);

} // namespace GameEngine::Rendering::MetalLayerBridge
