#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Platform
{
class Window;
}

// GPU-independent client-area capture through the OS compositor (Win32
// PrintWindow with PW_RENDERFULLCONTENT). This is a picture of what the
// compositor holds for the window, not a frame the render graph produced, so
// take_screenshot serves it only to a request that passed allowWindowCapture
// and labels the response `method: printwindow`. Pixels come back tightly
// packed RGBA8, top-down, at the client area's physical size — the same space
// as the render-graph readback, so crop math is shared.
// Returns false when the capture fails or on non-Windows platforms.
bool CaptureWindowClientAreaRGBA8(Platform::Window* window,
                                  std::vector<uint8_t>& outRgba8,
                                  uint32_t& outWidth,
                                  uint32_t& outHeight);

} // namespace GameEngine
