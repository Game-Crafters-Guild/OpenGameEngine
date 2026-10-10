// Regression for the Win32 GLFW patch in cmake/ports/glfw3: a button message's
// own client coordinates reach the cursor callback before the button callback,
// so a consumer that hit-tests at its last cursor position clicks where the
// button went down, not where the pointer was last reported or is now.
//
// Links only GLFW and Win32: no Engine.dll, hidden windows only, messages are
// handed straight to this process's own window procedure, and the desktop
// pointer is never moved.
//
// Cases (--case <name>):
//   buttons            left, right, middle, X1 and X2 presses and releases each
//                      report their own point (negative ones included) before
//                      the button callback
//   release_point      a release far from its press reports the release point
//   repeated_position  a press at the last reported point still reports it, so
//                      a consumer that cleared its position (cursor leave,
//                      focus loss) is resynchronised
//   relative_modes     a disabled cursor, with and without raw motion, gets no
//                      absolute point injected into its relative motion
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
constexpr double kUnset = -9999.0;

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class GlfwSession
{
  public:
    GlfwSession()
    {
        glfwSetErrorCallback([](int code, const char* message)
        {
            std::fprintf(stderr, "GLFW %d: %s\n", code, message);
        });
        Check(glfwInit() == GLFW_TRUE, "glfwInit failed");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    }
    ~GlfwSession() { glfwTerminate(); }
};

// What a consumer that tracks the cursor callback sees at each button callback.
struct Observed
{
    double X = kUnset;
    double Y = kUnset;
    double ButtonX = kUnset;
    double ButtonY = kUnset;
    int Moves = 0;
    int Buttons = 0;
    int Button = -1;
    int Action = -1;
    bool MoveBeforeEveryButton = true;
};

void OnCursor(GLFWwindow* window, double x, double y)
{
    auto& observed = *static_cast<Observed*>(glfwGetWindowUserPointer(window));
    observed.X = x;
    observed.Y = y;
    ++observed.Moves;
}

void OnButton(GLFWwindow* window, int button, int action, int)
{
    auto& observed = *static_cast<Observed*>(glfwGetWindowUserPointer(window));
    observed.MoveBeforeEveryButton = observed.MoveBeforeEveryButton && observed.Moves > observed.Buttons;
    observed.ButtonX = observed.X;
    observed.ButtonY = observed.Y;
    observed.Button = button;
    observed.Action = action;
    ++observed.Buttons;
}

using Window = std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)>;
Window MakeWindow(Observed& observed)
{
    Window window(glfwCreateWindow(80, 80, "GLFW button coordinates regression (hidden)", nullptr, nullptr),
                  glfwDestroyWindow);
    Check(window != nullptr, "glfwCreateWindow failed");
    glfwSetWindowUserPointer(window.get(), &observed);
    glfwSetCursorPosCallback(window.get(), OnCursor);
    glfwSetMouseButtonCallback(window.get(), OnButton);
    return window;
}

void Deliver(GLFWwindow* window, UINT message, WPARAM wParam, int x, int y)
{
    const HWND handle = glfwGetWin32Window(window);
    const auto procedure = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(handle, GWLP_WNDPROC));
    ::CallWindowProcW(procedure, handle, message, wParam,
                      MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
}

struct NativeButton
{
    int Button;
    UINT Down;
    UINT Up;
    WPARAM DownFlags;
    WPARAM UpFlags;
    int X;
    int Y;
};

void Buttons()
{
    const NativeButton buttons[] = {
        {GLFW_MOUSE_BUTTON_LEFT, WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON, 0, 17, 23},
        {GLFW_MOUSE_BUTTON_RIGHT, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON, 0, 29, 11},
        {GLFW_MOUSE_BUTTON_MIDDLE, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON, 0, 5, 61},
        {GLFW_MOUSE_BUTTON_4, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON1, XBUTTON1),
         MAKEWPARAM(0, XBUTTON1), -31, -47},
        {GLFW_MOUSE_BUTTON_5, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON2, XBUTTON2),
         MAKEWPARAM(0, XBUTTON2), 43, -7},
    };
    for (const NativeButton& native : buttons)
    {
        Observed observed;
        auto window = MakeWindow(observed);
        Deliver(window.get(), native.Down, native.DownFlags, native.X, native.Y);
        Check(observed.Button == native.Button && observed.Action == GLFW_PRESS, "press did not arrive");
        Check(observed.MoveBeforeEveryButton, "press arrived before its position");
        Check(observed.ButtonX == native.X && observed.ButtonY == native.Y, "press reported another point");
        Deliver(window.get(), native.Up, native.UpFlags, native.X, native.Y);
        Check(observed.Button == native.Button && observed.Action == GLFW_RELEASE, "release did not arrive");
        Check(observed.MoveBeforeEveryButton, "release arrived before its position");
        Check(observed.ButtonX == native.X && observed.ButtonY == native.Y, "release reported another point");
    }
}

void ReleasePoint()
{
    Observed observed;
    auto window = MakeWindow(observed);
    Deliver(window.get(), WM_LBUTTONDOWN, MK_LBUTTON, 17, 23);
    Deliver(window.get(), WM_LBUTTONUP, 0, 41, 53);
    Check(observed.Action == GLFW_RELEASE, "release did not arrive");
    Check(observed.ButtonX == 41 && observed.ButtonY == 53, "release reported the press point");
}

void RepeatedPosition()
{
    Observed observed;
    auto window = MakeWindow(observed);
    Deliver(window.get(), WM_MOUSEMOVE, 0, 17, 23);
    Check(observed.X == 17 && observed.Y == 23, "move did not arrive");
    // The consumer forgets its position, as the engine's UI does on cursor leave.
    observed = Observed{};
    Deliver(window.get(), WM_LBUTTONDOWN, MK_LBUTTON, 17, 23);
    Deliver(window.get(), WM_LBUTTONUP, 0, 17, 23);
    Check(observed.Buttons == 2, "button callbacks missing");
    Check(observed.MoveBeforeEveryButton, "an unchanged point was not re-reported before the button");
    Check(observed.ButtonX == 17 && observed.ButtonY == 23, "button reported another point");
}

void RelativeModes()
{
    for (const bool raw : {false, true})
    {
        Observed observed;
        auto window = MakeWindow(observed);
        glfwSetInputMode(window.get(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (raw)
        {
            Check(glfwRawMouseMotionSupported() == GLFW_TRUE, "raw mouse motion unsupported");
            glfwSetInputMode(window.get(), GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
        observed = Observed{};
        Deliver(window.get(), WM_LBUTTONDOWN, MK_LBUTTON, 17, 23);
        Deliver(window.get(), WM_LBUTTONUP, 0, 41, 53);
        Check(observed.Buttons == 2, "button callbacks missing");
        Check(observed.Moves == 0, "an absolute point was injected into relative cursor motion");
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try
    {
        Check(argc == 3 && std::wstring(argv[1]) == L"--case", "expected --case <name>");
        GlfwSession session;
        const std::wstring name = argv[2];
        if (name == L"buttons") Buttons();
        else if (name == L"release_point") ReleasePoint();
        else if (name == L"repeated_position") RepeatedPosition();
        else if (name == L"relative_modes") RelativeModes();
        else throw std::runtime_error("unknown test case");
        std::printf("PASS: %ls\n", name.c_str());
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
