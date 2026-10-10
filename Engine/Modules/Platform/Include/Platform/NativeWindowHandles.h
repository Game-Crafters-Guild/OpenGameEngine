#pragma once

// Native window handles for graphics surface bridges. OS access stays here;
// the graphics backend owns its surface descriptor and lifetime.
struct GLFWwindow;

#if defined(_WIN32)
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace GameEngine::Platform
{
struct Win32WindowHandles
{
    void* Window = nullptr;
    void* Instance = nullptr;
};

// Returns empty handles on hosts without Win32, or for an invalid window.
inline Win32WindowHandles GetWin32WindowHandles(GLFWwindow* window)
{
#if defined(_WIN32)
    const HWND handle = window ? glfwGetWin32Window(window) : nullptr;
    if (handle == nullptr)
        return {};
    return {handle, reinterpret_cast<void*>(::GetWindowLongPtrW(handle, GWLP_HINSTANCE))};
#else
    (void)window;
    return {};
#endif
}
} // namespace GameEngine::Platform
