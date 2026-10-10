#include "MetalLayerBridge.h"

#include <QuartzCore/QuartzCore.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <objc/message.h>
#include <objc/runtime.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

namespace GameEngine::Rendering::MetalLayerBridge
{

namespace
{
using ObjcId = ::id;

ObjcId Send(ObjcId receiver, const char* selector)
{
    using Fn = ObjcId (*)(ObjcId, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector));
}

void SendVoidBool(ObjcId receiver, const char* selector, bool value)
{
    using Fn = void (*)(ObjcId, SEL, BOOL);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value ? YES : NO);
}

void SendVoidId(ObjcId receiver, const char* selector, ObjcId value)
{
    using Fn = void (*)(ObjcId, SEL, ObjcId);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

void SendVoidDouble(ObjcId receiver, const char* selector, double value)
{
    using Fn = void (*)(ObjcId, SEL, double);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

void SendVoidULong(ObjcId receiver, const char* selector, unsigned long value)
{
    using Fn = void (*)(ObjcId, SEL, unsigned long);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

double SendDouble(ObjcId receiver, const char* selector)
{
    using Fn = double (*)(ObjcId, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector));
}
} // namespace

bool AttachLayerToGLFWWindow(void* glfwWindow, CA::MetalLayer* layer)
{
    if (glfwWindow == nullptr || layer == nullptr)
    {
        return false;
    }

    ObjcId nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(glfwWindow));
    if (nsWindow == nullptr)
    {
        return false;
    }

    ObjcId contentView = Send(nsWindow, "contentView");
    if (contentView == nullptr)
    {
        return false;
    }

    const double scale = SendDouble(nsWindow, "backingScaleFactor");

    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    SendVoidDouble(layerId, "setContentsScale:", scale > 0.0 ? scale : 1.0);
    SendVoidBool(contentView, "setWantsLayer:", true);
    SendVoidId(contentView, "setLayer:", layerId);
    return true;
}

double GetWindowBackingScale(void* glfwWindow)
{
    if (glfwWindow == nullptr)
    {
        return 1.0;
    }
    ObjcId nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(glfwWindow));
    if (nsWindow == nullptr)
    {
        return 1.0;
    }
    const double scale = SendDouble(nsWindow, "backingScaleFactor");
    return scale > 0.0 ? scale : 1.0;
}

void SetLayerDisplaySyncEnabled(CA::MetalLayer* layer, bool enabled)
{
    if (layer != nullptr)
    {
        SendVoidBool(reinterpret_cast<ObjcId>(layer), "setDisplaySyncEnabled:", enabled);
    }
}

void SetLayerMaximumDrawableCount(CA::MetalLayer* layer, unsigned long count)
{
    if (layer != nullptr)
    {
        SendVoidULong(reinterpret_cast<ObjcId>(layer), "setMaximumDrawableCount:", count);
    }
}

bool SyncLayerContentsScale(void* glfwWindow, CA::MetalLayer* layer)
{
    if (glfwWindow == nullptr || layer == nullptr)
    {
        return false;
    }
    const double scale = GetWindowBackingScale(glfwWindow);
    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    const double currentScale = SendDouble(layerId, "contentsScale");
    if (scale <= 0.0 || currentScale == scale)
    {
        return false;
    }
    SendVoidDouble(layerId, "setContentsScale:", scale);
    return true;
}

void SetLayerExtendedDynamicRange(CA::MetalLayer* layer, bool enabled)
{
    if (layer == nullptr)
    {
        return;
    }
    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    SendVoidBool(layerId, "setWantsExtendedDynamicRangeContent:", enabled);
    CGColorSpaceRef colorSpace =
        enabled ? CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB) : nullptr;
    using SetColorSpaceFn = void (*)(ObjcId, SEL, CGColorSpaceRef);
    reinterpret_cast<SetColorSpaceFn>(objc_msgSend)(layerId, sel_registerName("setColorspace:"), colorSpace);
    if (colorSpace != nullptr)
    {
        CGColorSpaceRelease(colorSpace); // the layer retains it
    }
}

void GetWindowEdrHeadroom(void* glfwWindow, float& outCurrent, float& outPotential)
{
    outCurrent = 1.0f;
    outPotential = 1.0f;
    if (glfwWindow == nullptr)
    {
        return;
    }
    ObjcId nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(glfwWindow));
    if (nsWindow == nullptr)
    {
        return;
    }
    ObjcId screen = Send(nsWindow, "screen");
    if (screen == nullptr)
    {
        return;
    }
    const double current = SendDouble(screen, "maximumExtendedDynamicRangeColorComponentValue");
    const double potential = SendDouble(screen, "maximumPotentialExtendedDynamicRangeColorComponentValue");
    outCurrent = current > 0.0 ? static_cast<float>(current) : 1.0f;
    outPotential = potential > 0.0 ? static_cast<float>(potential) : 1.0f;
}

bool GetLayerPixelSize(CA::MetalLayer* layer, unsigned int& outWidth, unsigned int& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    if (layer == nullptr)
    {
        return false;
    }
    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    using BoundsFn = CGRect (*)(ObjcId, SEL);
#if defined(__aarch64__)
    const CGRect bounds = reinterpret_cast<BoundsFn>(objc_msgSend)(layerId, sel_registerName("bounds"));
#else
    const CGRect bounds = reinterpret_cast<BoundsFn>(objc_msgSend_stret)(layerId, sel_registerName("bounds"));
#endif
    const double scale = SendDouble(layerId, "contentsScale");
    const double effectiveScale = scale > 0.0 ? scale : 1.0;
    const double w = bounds.size.width * effectiveScale;
    const double h = bounds.size.height * effectiveScale;
    if (w < 1.0 || h < 1.0)
    {
        return false;
    }
    outWidth = static_cast<unsigned int>(w + 0.5);
    outHeight = static_cast<unsigned int>(h + 0.5);
    return true;
}

} // namespace GameEngine::Rendering::MetalLayerBridge
