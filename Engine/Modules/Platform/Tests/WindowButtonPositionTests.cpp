// A Win32 press must reach the mouse-move handler at the button message's own
// point. The desktop pointer is never moved: the hidden window is handed a press
// away from wherever the pointer really is, so a press that re-queried the OS
// cursor would report the pointer's position instead of the press's.
#include <gtest/gtest.h>

#include "Platform/Window.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <utility>
#include <vector>

namespace
{
using GameEngine::Platform::Window;
using GameEngine::Platform::WindowDesc;
using Point = std::pair<float, float>;

class PlatformSession
{
  public:
    PlatformSession() : m_Initialized(Window::Initialize()) {}
    ~PlatformSession()
    {
        if (m_Initialized)
            Window::Terminate();
    }
    bool Initialized() const { return m_Initialized; }

  private:
    bool m_Initialized;
};

void Deliver(HWND handle, UINT message, WPARAM wParam, int x, int y)
{
    const auto procedure = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(handle, GWLP_WNDPROC));
    ::CallWindowProcW(procedure, handle, message, wParam,
                      MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
}
} // namespace

TEST(WindowButtonPosition, PressAndReleaseReportTheirOwnPointNotTheCurrentCursor)
{
    PlatformSession session;
    ASSERT_TRUE(session.Initialized());
    Window window;
    WindowDesc desc;
    desc.Title = "Window button position regression (hidden)";
    desc.Width = 80;
    desc.Height = 80;
    desc.StartHidden = true;
    ASSERT_TRUE(window.Create(desc));
    const HWND handle = glfwGetWin32Window(window.GetGLFWHandle());
    ASSERT_NE(handle, nullptr);

    // Place the press well away from the real pointer, in client coordinates.
    POINT cursor{};
    ASSERT_TRUE(::GetCursorPos(&cursor));
    ASSERT_TRUE(::ScreenToClient(handle, &cursor));
    const int pressX = static_cast<int>(cursor.x) + 137;
    const int pressY = static_cast<int>(cursor.y) - 91;
    const int releaseX = pressX + 24;
    const int releaseY = pressY + 30;

    Point pointer{-9999.0f, -9999.0f};
    std::vector<Point> buttonPoints;
    window.SetMouseMoveHandler([&](float x, float y) { pointer = {x, y}; });
    window.SetMouseButtonHandler([&](int, bool, int) { buttonPoints.push_back(pointer); });

    Deliver(handle, WM_LBUTTONDOWN, MK_LBUTTON, pressX, pressY);
    Deliver(handle, WM_LBUTTONUP, 0, releaseX, releaseY);

    ASSERT_EQ(buttonPoints.size(), 2u);
    EXPECT_EQ(buttonPoints[0], Point(static_cast<float>(pressX), static_cast<float>(pressY)))
        << "the press was reported at the current OS cursor instead of its own point";
    EXPECT_EQ(buttonPoints[1], Point(static_cast<float>(releaseX), static_cast<float>(releaseY)))
        << "the release was reported away from its own point";
}
