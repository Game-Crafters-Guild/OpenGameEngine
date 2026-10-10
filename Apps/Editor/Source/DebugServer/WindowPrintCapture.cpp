#include "DebugServer/WindowPrintCapture.h"

#if defined(_WIN32)

#include "Platform/Window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// Ask DWM for the composited surface. Required for swapchain-presented
// (Vulkan) windows, where plain GDI redraw sees an empty client area.
// Older SDK headers may not define it.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace GameEngine
{

bool CaptureWindowClientAreaRGBA8(Platform::Window* window,
                                  std::vector<uint8_t>& outRgba8,
                                  uint32_t& outWidth,
                                  uint32_t& outHeight)
{
    outRgba8.clear();
    outWidth = 0;
    outHeight = 0;

    if (!window)
        return false;
    GLFWwindow* glfwWin = window->GetGLFWHandle();
    if (!glfwWin)
        return false;
    HWND hwnd = glfwGetWin32Window(glfwWin);
    if (!hwnd)
        return false;

    // PW_RENDERFULLCONTENT renders the FULL window (PW_CLIENTONLY handling
    // varies across Windows versions), so capture the whole window surface and
    // copy the client sub-rect out afterwards.
    RECT windowRect{};
    RECT clientRect{};
    if (!GetWindowRect(hwnd, &windowRect) || !GetClientRect(hwnd, &clientRect))
        return false;
    const int fullW = windowRect.right - windowRect.left;
    const int fullH = windowRect.bottom - windowRect.top;
    const int clientW = clientRect.right - clientRect.left;
    const int clientH = clientRect.bottom - clientRect.top;
    if (fullW <= 0 || fullH <= 0 || clientW <= 0 || clientH <= 0)
        return false;

    POINT clientOrigin{0, 0};
    if (!ClientToScreen(hwnd, &clientOrigin))
        return false;
    const int offsetX = clientOrigin.x - windowRect.left;
    const int offsetY = clientOrigin.y - windowRect.top;
    if (offsetX < 0 || offsetY < 0
        || offsetX + clientW > fullW || offsetY + clientH > fullH)
        return false;

    HDC windowDC = GetDC(hwnd);
    if (!windowDC)
        return false;
    HDC memDC = CreateCompatibleDC(windowDC);
    if (!memDC)
    {
        ReleaseDC(hwnd, windowDC);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = fullW;
    bmi.bmiHeader.biHeight = -fullH; // negative = top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits)
    {
        if (dib)
            DeleteObject(dib);
        DeleteDC(memDC);
        ReleaseDC(hwnd, windowDC);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    const BOOL printed = PrintWindow(hwnd, memDC, PW_RENDERFULLCONTENT);

    bool ok = false;
    if (printed)
    {
        outRgba8.resize(static_cast<size_t>(clientW) * static_cast<size_t>(clientH) * 4u);
        const uint8_t* src = static_cast<const uint8_t*>(bits);
        for (int row = 0; row < clientH; ++row)
        {
            const uint8_t* srcRow =
                src + (static_cast<size_t>(row + offsetY) * fullW + offsetX) * 4u;
            uint8_t* dstRow = outRgba8.data() + static_cast<size_t>(row) * clientW * 4u;
            for (int col = 0; col < clientW; ++col)
            {
                dstRow[col * 4 + 0] = srcRow[col * 4 + 2]; // BGRA -> RGBA
                dstRow[col * 4 + 1] = srcRow[col * 4 + 1];
                dstRow[col * 4 + 2] = srcRow[col * 4 + 0];
                dstRow[col * 4 + 3] = 0xFF; // DWM alpha is unreliable; force opaque
            }
        }
        outWidth = static_cast<uint32_t>(clientW);
        outHeight = static_cast<uint32_t>(clientH);
        ok = true;
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(hwnd, windowDC);
    return ok;
}

} // namespace GameEngine

#else // !_WIN32

namespace GameEngine
{
namespace Platform
{
class Window;
}

bool CaptureWindowClientAreaRGBA8(Platform::Window* /*window*/,
                                  std::vector<uint8_t>& outRgba8,
                                  uint32_t& outWidth,
                                  uint32_t& outHeight)
{
    outRgba8.clear();
    outWidth = 0;
    outHeight = 0;
    return false;
}

} // namespace GameEngine

#endif // _WIN32
