#include "WebGpuSurfaceBridge.h"

#include "WebGpuConversions.h"

#include "Logger/Logger.h"
#include "Platform/NativeWindowHandles.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace GameEngine::Rendering::WebGpuSurfaceBridge
{

#if defined(__EMSCRIPTEN__)
namespace
{
// The engine's canvas id; the shell page (Tools/Web/shell.html) provides it.
constexpr const char* kCanvasSelector = "#canvas";
} // namespace
#endif

#if defined(__APPLE__)
namespace
{
using ObjcId = ::id;

ObjcId Send(ObjcId receiver, const char* selector)
{
    using Fn = ObjcId (*)(ObjcId, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector));
}

ObjcId SendClass(const char* className, const char* selector)
{
    Class cls = objc_getClass(className);
    if (cls == nullptr)
    {
        return nullptr;
    }
    using Fn = ObjcId (*)(Class, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(cls, sel_registerName(selector));
}

void SendVoidId(ObjcId receiver, const char* selector, ObjcId value)
{
    using Fn = void (*)(ObjcId, SEL, ObjcId);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

void SendVoidBool(ObjcId receiver, const char* selector, bool value)
{
    using Fn = void (*)(ObjcId, SEL, BOOL);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value ? YES : NO);
}

void SendVoidDouble(ObjcId receiver, const char* selector, double value)
{
    using Fn = void (*)(ObjcId, SEL, double);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

double SendDouble(ObjcId receiver, const char* selector)
{
    using Fn = double (*)(ObjcId, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector));
}

// Creates a CAMetalLayer and installs it as the window content view's layer.
// The view retains it, so the caller takes no ownership.
ObjcId AttachMetalLayer(void* glfwWindow)
{
    ObjcId nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(glfwWindow));
    if (nsWindow == nullptr)
    {
        return nullptr;
    }

    ObjcId contentView = Send(nsWindow, "contentView");
    if (contentView == nullptr)
    {
        return nullptr;
    }

    ObjcId layer = SendClass("CAMetalLayer", "layer");
    if (layer == nullptr)
    {
        return nullptr;
    }

    SendVoidDouble(layer, "setContentsScale:", SendDouble(nsWindow, "backingScaleFactor"));

    // A standalone CAMetalLayer keeps a zero drawableSize until told
    // otherwise, and wgpu treats a drawable/config size mismatch as a
    // permanently Outdated surface. Size it from the framebuffer here.
    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(glfwWindow), &fbWidth, &fbHeight);
    if (fbWidth > 0 && fbHeight > 0)
    {
        struct CGSizeMirror
        {
            double width;
            double height;
        };
        using SetSizeFn = void (*)(ObjcId, SEL, CGSizeMirror);
        reinterpret_cast<SetSizeFn>(objc_msgSend)(
            layer, sel_registerName("setDrawableSize:"),
            CGSizeMirror{static_cast<double>(fbWidth), static_cast<double>(fbHeight)});
    }

    SendVoidId(contentView, "setLayer:", layer);
    SendVoidBool(contentView, "setWantsLayer:", true);
    return layer;
}
} // namespace
#endif

WGPUSurface CreateSurface(WGPUInstance instance, void* glfwWindow)
{
    if (instance == nullptr || glfwWindow == nullptr)
    {
        return nullptr;
    }

#if defined(__EMSCRIPTEN__)
    // The web window IS the page canvas: emdawnwebgpu binds surfaces to a CSS
    // selector, not a native handle. The GLFW window (emscripten's JS GLFW)
    // is attached to the same canvas, so the handle is only a liveness token.
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source{};
    source.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    source.selector = WebGpu::MakeStringView(kCanvasSelector);

    WGPUSurfaceDescriptor desc{};
    desc.nextInChain = &source.chain;
    desc.label = WebGpu::MakeStringView("GameEngine Canvas Surface");
    return wgpuInstanceCreateSurface(instance, &desc);
#elif defined(__APPLE__)
    ObjcId layer = AttachMetalLayer(glfwWindow);
    if (layer == nullptr)
    {
        Logger::Log::Error("WebGpuSurfaceBridge: could not attach a CAMetalLayer to the window");
        return nullptr;
    }

    WGPUSurfaceSourceMetalLayer source{};
    source.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    source.layer = layer;

    WGPUSurfaceDescriptor desc{};
    desc.nextInChain = &source.chain;
    desc.label = WebGpu::MakeStringView("GameEngine Window Surface");
    return wgpuInstanceCreateSurface(instance, &desc);
#else
    const auto handles = Platform::GetWin32WindowHandles(static_cast<GLFWwindow*>(glfwWindow));
    if (handles.Window != nullptr && handles.Instance != nullptr)
    {
        WGPUSurfaceSourceWindowsHWND source{};
        source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        source.hwnd = handles.Window;
        source.hinstance = handles.Instance;

        WGPUSurfaceDescriptor desc{};
        desc.nextInChain = &source.chain;
        desc.label = WebGpu::MakeStringView("GameEngine Window Surface");
        return wgpuInstanceCreateSurface(instance, &desc);
    }
    Logger::Log::Error("WebGpuSurfaceBridge: no surface source implemented for this platform");
    return nullptr;
#endif
}

bool GetWindowPixelSize(void* glfwWindow, uint32_t& outWidth, uint32_t& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    if (glfwWindow == nullptr)
    {
        return false;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(glfwWindow), &width, &height);
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    outWidth = static_cast<uint32_t>(width);
    outHeight = static_cast<uint32_t>(height);
    return true;
}

} // namespace GameEngine::Rendering::WebGpuSurfaceBridge
