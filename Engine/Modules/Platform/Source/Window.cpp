#include "Platform/Window.h"
#include "Platform/Display.h"
#include "Platform/SystemTheme.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <vector>

#include <GLFW/glfw3.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

#if defined(_WIN32)
#ifndef OEMRESOURCE
#define OEMRESOURCE
#endif
#include <windows.h>
#elif defined(__APPLE__)
// Forward declarations for macOS Objective-C++ implementations
extern "C" {
    uint32_t GE_MacOS_GetScreenPixelColor(int screenX, int screenY);
    void GE_MacOS_GetCursorScreenPosition(int* outX, int* outY);
    bool GE_MacOS_IsLeftMouseButtonDown();
    bool GE_MacOS_IsOptionKeyDown();
    int GE_MacOS_GetModifierKeyMask();
    
    // Global click monitoring
    typedef void (*GE_GlobalClickCallback)(int screenX, int screenY, void* userData);
    void GE_MacOS_StartGlobalClickMonitor(GE_GlobalClickCallback callback, void* userData);
    void GE_MacOS_StopGlobalClickMonitor();
    
    // Global mouse move monitoring
    typedef void (*GE_GlobalMoveCallback)(int screenX, int screenY, void* userData);
    void GE_MacOS_StartGlobalMoveMonitor(GE_GlobalMoveCallback callback, void* userData);
    void GE_MacOS_StopGlobalMoveMonitor();
    
    // Global cursor (for eyedropper)
    void GE_MacOS_PushCrosshairCursor();
    void GE_MacOS_PopCursor();
    
    // Window level
    void GE_MacOS_SetWindowAlwaysOnTop(void* nsWindow, bool enable);
    void GE_MacOS_FocusWindow(void* nsWindow);
    void* GE_MacOS_InstallTitleBarRightClickMonitor(void* nsWindow, void (*callback)(int, int, void*), void* userData);
    void GE_MacOS_RemoveTitleBarRightClickMonitor(void* monitor);
}
#elif defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#endif

namespace {
    // Storage for global click callback (cross-platform)
    static GameEngine::Platform::Window::GlobalClickCallback s_GlobalClickCallback;
    // Storage for global move callback (cross-platform)
    static GameEngine::Platform::Window::GlobalMoveCallback s_GlobalMoveCallback;
    static bool s_GlfwInitialized = false;
    static std::vector<GameEngine::Platform::Window*> s_Windows;

#if defined(__EMSCRIPTEN__)
    // The canvas is the only window emscripten's GLFW can produce; see the
    // refusal in Window::Create. Main-thread only, like all window creation.
    static bool s_WebWindowCreated = false;

    // Emscripten's GLFW 3 JS port has no glfwCreateStandardCursor / glfwSetCursor
    // (libglfw.js stubs neither). Splitters and text fields therefore never
    // change the OS cursor unless we write the canvas CSS cursor ourselves.
    const char* WebCanvasCssCursor(GameEngine::Platform::Window::CursorType type)
    {
        using CT = GameEngine::Platform::Window::CursorType;
        switch (type)
        {
        case CT::Hidden:
            return "none";
        case CT::IBeam:
            return "text";
        case CT::Crosshair:
            return "crosshair";
        case CT::Hand:
            return "pointer";
        case CT::HResize:
            return "ew-resize";
        case CT::VResize:
            return "ns-resize";
        case CT::NorthwestSoutheastResize:
            return "nwse-resize";
        case CT::Move:
            return "move";
        case CT::NotAllowed:
            return "not-allowed";
        case CT::Grab:
            return "grab";
        case CT::Grabbing:
            return "grabbing";
        case CT::Arrow:
        default:
            return "default";
        }
    }

    void ApplyWebCanvasCursor(const char* css)
    {
        MAIN_THREAD_EM_ASM(
            {
                var canvas = (typeof Module !== 'undefined' && Module['canvas'])
                                 ? Module['canvas']
                                 : document.getElementById('canvas');
                if (canvas)
                    canvas.style.cursor = UTF8ToString($0);
            },
            css);
    }

    // The browser owns the window geometry on this target: the canvas fills
    // the page, so the GLFW window tracks the viewport (CSS px). emscripten's
    // GLFW only pushes sizes canvas-ward (glfwSetWindowSize -> canvas), so
    // viewport changes have to be pushed back into GLFW here. The resize event
    // also fires on browser zoom, where the viewport's CSS size and
    // devicePixelRatio change together.
    bool WebViewportResizeThunk(int /*eventType*/, const EmscriptenUiEvent* event, void* userData)
    {
        auto* handle = static_cast<GLFWwindow*>(userData);
        if (handle && event && event->windowInnerWidth > 0 && event->windowInnerHeight > 0)
            glfwSetWindowSize(handle, event->windowInnerWidth, event->windowInnerHeight);
        return true;
    }

    // The browser is the authority on fullscreen state: a request can be
    // deferred (until the next user gesture) or denied, and the user can leave
    // at any time with Esc without asking the engine. m_IsFullscreen therefore
    // flips HERE, on the browser's transition, never in SetFullscreen — and
    // the app-level handler runs so editor state (play-fullscreen chrome)
    // unwinds on the browser's Esc exactly like on its own toggle.
    bool WebFullscreenChangeThunk(int /*eventType*/, const EmscriptenFullscreenChangeEvent* event,
                                  void* userData)
    {
        auto* handle = static_cast<GLFWwindow*>(userData);
        if (!handle || !event)
            return false;
        auto* self = static_cast<GameEngine::Platform::Window*>(glfwGetWindowUserPointer(handle));
        if (self)
            self->NotifyWebFullscreenChanged(event->isFullscreen == EM_TRUE);
        return true;
    }
#endif

    // emscripten's JS GLFW implements the 3.3 header only partially:
    // glfwGetError is declared but has no JS implementation, so a direct call
    // fails at wasm-ld. The error paths there report code 0 / no description.
    int GetGlfwError(const char** description)
    {
#if defined(__EMSCRIPTEN__)
        if (description)
            *description = nullptr;
        return 0;
#else
        return glfwGetError(description);
#endif
    }

    void MonitorConfigThunk(GLFWmonitor* monitor, int event)
    {
        const char* name = monitor ? glfwGetMonitorName(monitor) : nullptr;
        Logger::Log::Info("Window: monitor {} '{}'",
                          event == GLFW_CONNECTED ? "connected" : "disconnected",
                          name ? name : "");
    }

#if defined(_WIN32)
    // Tracks nesting of PushGlobalCrosshairCursor/PopGlobalCursor calls.
    static int s_Win32GlobalCursorDepth = 0;
#endif
    
#if defined(__APPLE__)
    // C callback wrapper for macOS
    void MacOS_GlobalClickCallbackWrapper(int screenX, int screenY, void* /*userData*/)
    {
        if (s_GlobalClickCallback)
            s_GlobalClickCallback(screenX, screenY);
    }
    
    void MacOS_GlobalMoveCallbackWrapper(int screenX, int screenY, void* /*userData*/)
    {
        if (s_GlobalMoveCallback)
            s_GlobalMoveCallback(screenX, screenY);
    }
#endif
}

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#endif

#if defined(_WIN32)

namespace
{
// On Windows, try to apply the main executable's icon resource to a GLFW window
// so the title bar and taskbar use the same icon as the .exe.
inline void ApplyExecutableIconToWindow(GLFWwindow* handle)
{
    if (!handle)
        return;

    HWND hwnd = glfwGetWin32Window(handle);
    if (!hwnd)
        return;

    // Use the primary application icon with numeric id 1. The Editor embeds
    // AppIcon.ico with this id in Apps/Editor/Icon/Editor.rc.
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!hInst)
        return;

    HICON hIcon = static_cast<HICON>(LoadImageW(
        hInst,
        MAKEINTRESOURCEW(1),
        IMAGE_ICON,
        0,
        0,
        LR_DEFAULTSIZE));
    if (!hIcon)
        return;

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
}
} // namespace
#endif

namespace GameEngine
{
namespace Platform
{

Window::~Window()
{
    Destroy();
}

bool Window::Initialize()
{
#if defined(__APPLE__)
    // Keep app-bundle launches from changing cwd into Contents/Resources, and
    // skip GLFW's auto-created Cocoa menubar so the host app owns its menu.
    // Both must be set before the GLFW instance in this module is initialized.
    if (!s_GlfwInitialized)
    {
        glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
        glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_FALSE);
    }
#endif
    if (s_GlfwInitialized)
        return true;

    if (!glfwInit())
    {
        const char* glfwError = nullptr;
        const int glfwErrorCode = GetGlfwError(&glfwError);
        Logger::Log::Error("Window::Initialize: glfwInit failed (errorCode={}, error='{}')",
                           glfwErrorCode,
                           glfwError ? glfwError : "");
        return false;
    }

    glfwSetMonitorCallback(&MonitorConfigThunk);
    s_GlfwInitialized = true;
    return true;
}

bool Window::Create(const WindowDesc& desc)
{
    if (m_Handle)
        return true;

    ApplySystemNativeAppTheme();

    if (!Initialize())
        return false;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_MAXIMIZED, desc.StartMaximized ? GLFW_TRUE : GLFW_FALSE);

    // Create window hidden when StartHidden is requested (so the caller can
    // render a frame before showing) or on Windows for ToolWindow style fixup.
    const bool createHidden = desc.StartHidden
#if defined(_WIN32)
        || desc.ToolWindow
#endif
        ;

    if (createHidden)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

#if defined(_WIN32)
#if defined(GLFW_WIN32_CLASSNAME)
    if (!desc.ClassName.empty())
        glfwWindowHintString(GLFW_WIN32_CLASSNAME, desc.ClassName.c_str());
#endif
#endif

#if defined(__EMSCRIPTEN__)
    // One canvas, one window. Emscripten's GLFW aborts the process on a second
    // glfwCreateWindow rather than returning null, so the refusal has to happen
    // before the call — a caller that undocks a panel gets the same false it
    // gets from any other creation failure, and the process stays alive.
    if (s_WebWindowCreated)
    {
        Logger::Log::Warning(
            "Window::Create: refusing a second OS window for '{}' — the web build has one "
            "canvas; dock the panel instead.",
            desc.Title);
        return false;
    }
    s_WebWindowCreated = true;

    // HiDPI-aware canvas: with GLFW_SCALE_TO_MONITOR, emscripten's GLFW sizes
    // the canvas backing store to window size (CSS px) x devicePixelRatio and
    // splits glfwGetWindowSize (CSS px) from glfwGetFramebufferSize (device
    // px), so GetContentScale() sees the DPR through the same
    // framebuffer/window ratio as a Retina desktop, and cursor coordinates
    // stay CSS px. devicePixelRatio changes (monitor move, browser zoom) are
    // tracked by emscripten's GLFW via matchMedia and re-fire the framebuffer
    // size callback; a page cannot opt out of zoom, so zoom rescales the UI.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

    m_Handle = glfwCreateWindow(desc.Width, desc.Height, desc.Title.c_str(), nullptr, desc.Share);
    if (!m_Handle)
    {
#if defined(__EMSCRIPTEN__)
        s_WebWindowCreated = false;
#endif
        const char* glfwError = nullptr;
        const int glfwErrorCode = GetGlfwError(&glfwError);
        Logger::Log::Error(
            "Window::Create: glfwCreateWindow failed for '{}' ({}x{}, hidden={}, errorCode={}, error='{}')",
            desc.Title,
            desc.Width,
            desc.Height,
            createHidden ? "true" : "false",
            glfwErrorCode,
            glfwError ? glfwError : "");

        if (createHidden)
            glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        return false;
    }

#if defined(_WIN32)
    if (desc.ToolWindow)
    {
        HWND hwnd = glfwGetWin32Window(m_Handle);
        if (hwnd)
        {
            LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            ex |= WS_EX_TOOLWINDOW;
            ex &= ~WS_EX_APPWINDOW;
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
        }
    }
#endif

    // Use GLFW user pointer to store this wrapper pointer
    glfwSetWindowUserPointer(m_Handle, this);
    s_Windows.push_back(this);

    // Wire callbacks to thunks
    glfwSetCursorPosCallback(m_Handle, &Window::CursorPosThunk);
    glfwSetCursorEnterCallback(m_Handle, &Window::CursorEnterThunk);
    glfwSetWindowFocusCallback(m_Handle, &Window::FocusThunk);
    glfwSetScrollCallback(m_Handle, &Window::ScrollThunk);
    glfwSetMouseButtonCallback(m_Handle, &Window::MouseButtonThunk);
    glfwSetCharCallback(m_Handle, &Window::CharThunk);
    glfwSetKeyCallback(m_Handle, &Window::KeyThunk);
    glfwSetWindowPosCallback(m_Handle, &Window::WindowPosThunk);
    glfwSetFramebufferSizeCallback(m_Handle, &Window::FramebufferSizeThunk);
    glfwSetWindowRefreshCallback(m_Handle, &Window::RefreshThunk);
    glfwSetWindowCloseCallback(m_Handle, &Window::CloseThunk);
    glfwSetDropCallback(m_Handle, &Window::DropThunk);
    CheckMonitorChanged();

#if defined(__EMSCRIPTEN__)
    // The requested size is a desktop concept; here the canvas fills the page,
    // so the window is the viewport — at creation and on every browser resize.
    {
        const int viewportW = EM_ASM_INT({ return window.innerWidth; });
        const int viewportH = EM_ASM_INT({ return window.innerHeight; });
        if (viewportW > 0 && viewportH > 0)
            glfwSetWindowSize(m_Handle, viewportW, viewportH);
        emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, m_Handle, false,
                                       &WebViewportResizeThunk);
        emscripten_set_fullscreenchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, m_Handle,
                                                 false, &WebFullscreenChangeThunk);
    }

    // The engine draws its own context menus, so the browser's must never open
    // over the canvas: right-mouse drives the scene-view camera and the
    // editor's menus, exactly as on desktop. Middle-mouse likewise cancels the
    // browser's autoscroll default. emscripten's GLFW cancels neither event
    // (its canvas listeners run regardless — preventDefault does not stop
    // propagation). Canvas-scoped so page UI outside the canvas keeps browser
    // behavior; installed once for the page's lifetime, since the canvas
    // outlives any Destroy/Create cycle of this single-window target.
    EM_ASM({
        var canvas = Module['canvas'];
        if (canvas && !canvas.geBrowserMouseDefaultsSuppressed)
        {
            canvas.geBrowserMouseDefaultsSuppressed = true;
            canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });
            canvas.addEventListener('mousedown', function(e) {
                if (e.button === 1) e.preventDefault();
            });
        }
        if (canvas && !canvas.geFileDropPointerInstalled)
        {
            canvas.geFileDropPointerInstalled = true;
            var record = function(e) {
                var rect = canvas.getBoundingClientRect();
                if (rect.width <= 0 || rect.height <= 0)
                    return;
                Module.__geDropCursorX = (e.clientX - rect.left) * (canvas.clientWidth / rect.width);
                Module.__geDropCursorY = (e.clientY - rect.top) * (canvas.clientHeight / rect.height);
                Module.__geDropCursorFresh = 1;
                Module.__geDropCursorValid = 1;
            };
            canvas.addEventListener('dragover', function(e) {
                e.preventDefault();
                if (e.dataTransfer)
                    e.dataTransfer.dropEffect = 'copy';
                record(e);
            }, true);
            canvas.addEventListener('drop', function(e) { record(e); }, true);
        }
        // Live modifier state for GetModifierKeyMask: the flags every DOM input
        // event carries are the browser's truth, while emscripten's GLFW derives
        // its mask from key events it saw, and a key-up lost to the OS (Cmd+Space
        // opening Spotlight, a modifier released during an OS drag) leaves that
        // mask stuck. Window capture phase runs ahead of the canvas listeners
        // GLFW installs, so the recorded mask is the current event's by the time
        // a GLFW callback reads it.
        //
        // The same listener repairs the lost key-up itself. It mirrors which
        // modifier keys the GLFW port holds (the port keys its state on these
        // same trusted window events, one GLFW key per modifier keyCode), and
        // when a later event reports a held modifier's flag clear it replays the
        // keyup through the port's own window listener. The port then releases
        // the key and fires the key callback exactly as the real keyup would, so
        // InputSystem sees a release instead of a mask it would have to trust
        // over its own key state. Window blur needs no replay: the port releases
        // every key it holds there (libglfw.js onBlur), so the mirror only
        // follows it.
        if (!Module.__geDomModifierListenerInstalled)
        {
            Module.__geDomModifierListenerInstalled = true;
            Module.__geDomModifierMask = 0;
            Module.__geHeldModifierKeys = 0;
            // Parenthesized: EM_ASM(code, ...) stringifies only its first macro
            // argument, so a comma outside every paren (an array/object literal's
            // own commas do not count — only () nesting protects a macro argument)
            // would truncate the JS this block compiles to right after "bit: 0x0001".
            var modifierKeys = ([
                { bit: 0x0001, keyCode: 16, key: 'Shift', code: 'ShiftLeft' },
                { bit: 0x0002, keyCode: 17, key: 'Control', code: 'ControlLeft' },
                { bit: 0x0004, keyCode: 18, key: 'Alt', code: 'AltLeft' },
                { bit: 0x0008, keyCode: 91, key: 'Meta', code: 'MetaLeft' }
            ]);
            var modifierBitOfKeyCode = function(keyCode) {
                switch (keyCode)
                {
                case 16: return 0x0001;
                case 17: return 0x0002;
                case 18: return 0x0004;
                case 91: case 93: case 224: return 0x0008;
                default: return 0;
                }
            };
            var replayLostModifierReleases = function(mask) {
                var lost = Module.__geHeldModifierKeys & ~mask;
                for (var i = 0; i < modifierKeys.length; ++i)
                {
                    var m = modifierKeys[i];
                    if ((lost & m.bit) === 0)
                        continue;
                    Module.__geHeldModifierKeys &= ~m.bit;
                    window.dispatchEvent(new KeyboardEvent('keyup', {
                        keyCode: m.keyCode, key: m.key, code: m.code,
                        shiftKey: (mask & 0x0001) !== 0, ctrlKey: (mask & 0x0002) !== 0,
                        altKey: (mask & 0x0004) !== 0, metaKey: (mask & 0x0008) !== 0,
                        bubbles: true, cancelable: true }));
                }
            };
            var recordMods = function(e) {
                if (!e.isTrusted)
                    return;
                var mask = 0;
                if (e.shiftKey) mask |= 0x0001;
                if (e.ctrlKey) mask |= 0x0002;
                if (e.altKey) mask |= 0x0004;
                if (e.metaKey) mask |= 0x0008;
                if (e.type === 'keydown')
                    Module.__geHeldModifierKeys |= modifierBitOfKeyCode(e.keyCode);
                else if (e.type === 'keyup')
                    Module.__geHeldModifierKeys &= ~modifierBitOfKeyCode(e.keyCode);
                Module.__geDomModifierMask = mask;
                replayLostModifierReleases(mask);
            };
            window.addEventListener('keydown', recordMods, true);
            window.addEventListener('keyup', recordMods, true);
            window.addEventListener('mousedown', recordMods, true);
            window.addEventListener('mouseup', recordMods, true);
            window.addEventListener('mousemove', recordMods, true);
            window.addEventListener('wheel', recordMods, true);
            window.addEventListener('dragover', recordMods, true);
            window.addEventListener('drop', recordMods, true);
            window.addEventListener('blur', function() {
                Module.__geHeldModifierKeys = 0;
                Module.__geDomModifierMask = 0;
            }, true);
        }
    });
#endif

    ApplySystemThemeToWindow(*this);
#if defined(_WIN32)
    ApplyExecutableIconToWindow(m_Handle);
#endif

    // Show the window now unless the caller wants to defer visibility
    // (e.g. to render a frame first and avoid a gray flash).
    if (createHidden && !desc.StartHidden)
    {
        glfwShowWindow(m_Handle);
    }

    if (createHidden)
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    return true;
}

void Window::Destroy()
{
    if (!m_Handle)
        return;
    // Remove title bar right-click monitor before destroying
#if defined(__APPLE__)
    if (m_TitleBarRightClickMonitor)
    {
        GE_MacOS_RemoveTitleBarRightClickMonitor(m_TitleBarRightClickMonitor);
        m_TitleBarRightClickMonitor = nullptr;
    }
#endif
    m_TitleBarRightClick = nullptr;
    s_Windows.erase(std::remove(s_Windows.begin(), s_Windows.end(), this), s_Windows.end());

    // Detach callbacks and user pointer to avoid dangling accesses during teardown
    glfwSetCursorPosCallback(m_Handle, nullptr);
    glfwSetCursorEnterCallback(m_Handle, nullptr);
    glfwSetWindowFocusCallback(m_Handle, nullptr);
    glfwSetScrollCallback(m_Handle, nullptr);
    glfwSetMouseButtonCallback(m_Handle, nullptr);
    glfwSetCharCallback(m_Handle, nullptr);
    glfwSetKeyCallback(m_Handle, nullptr);
    glfwSetWindowPosCallback(m_Handle, nullptr);
    glfwSetFramebufferSizeCallback(m_Handle, nullptr);
    glfwSetWindowRefreshCallback(m_Handle, nullptr);
    glfwSetWindowCloseCallback(m_Handle, nullptr);
    glfwSetDropCallback(m_Handle, nullptr);
    glfwSetWindowUserPointer(m_Handle, nullptr);
    // Clean up custom cursor
    if (m_Cursor)
    {
        glfwDestroyCursor(m_Cursor);
        m_Cursor = nullptr;
    }
    glfwDestroyWindow(m_Handle);
    m_Handle = nullptr;
#if defined(__EMSCRIPTEN__)
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
    s_WebWindowCreated = false;
#endif
}

void* Window::GetNativeHandle() const
{
    return (void*)m_Handle;
}
GLFWwindow* Window::GetGLFWHandle() const
{
    return m_Handle;
}

void Window::GetFramebufferSize(int& outWidth, int& outHeight) const
{
    outWidth = 0;
    outHeight = 0;
    if (!m_Handle)
        return;
    glfwGetFramebufferSize(m_Handle, &outWidth, &outHeight);
}

void Window::GetWindowSize(int& outWidth, int& outHeight) const
{
    outWidth = 0;
    outHeight = 0;
    if (!m_Handle)
        return;
    glfwGetWindowSize(m_Handle, &outWidth, &outHeight);
}

void Window::SetWindowSize(int width, int height)
{
    if (!m_Handle)
        return;
    glfwSetWindowSize(m_Handle, width, height);
}

bool Window::IsFocused() const
{
    return m_Handle && glfwGetWindowAttrib(m_Handle, GLFW_FOCUSED) == GLFW_TRUE;
}

bool Window::IsKeyPressed(int key) const
{
    return m_Handle && glfwGetKey(m_Handle, key) == GLFW_PRESS;
}

void Window::GetCursorClientPosition(float& outX, float& outY) const
{
    outX = 0.0f;
    outY = 0.0f;
    if (!m_Handle)
        return;
#if defined(__EMSCRIPTEN__)
    if (m_HasWebDragCursor)
    {
        outX = m_WebDragCursorX;
        outY = m_WebDragCursorY;
        return;
    }
#endif
    double dx = 0.0;
    double dy = 0.0;
    glfwGetCursorPos(m_Handle, &dx, &dy);
    outX = static_cast<float>(dx);
    outY = static_cast<float>(dy);
}

void Window::GetContentScale(float& outScaleX, float& outScaleY) const
{
    outScaleX = 1.0f;
    outScaleY = 1.0f;
    if (!m_Handle)
        return;
    // Use framebuffer/window ratio rather than glfwGetWindowContentScale.
    // GLFW 3.4 changed Windows cursor positions to physical pixels (matching
    // the framebuffer), but glfwGetWindowContentScale still returns DPI/96.
    // Using the DPI ratio would double-scale the cursor on HiDPI Windows displays.
    // The framebuffer/window ratio is correct on all platforms:
    //   - Windows GLFW 3.4: both are physical pixels → ratio = 1.0
    //   - macOS Retina: framebuffer is physical, window is logical → ratio = 2.0
    int fbW = 0, fbH = 0, winW = 0, winH = 0;
    GetFramebufferSize(fbW, fbH);
    GetWindowSize(winW, winH);
    outScaleX = (winW > 0) ? static_cast<float>(fbW) / static_cast<float>(winW) : 1.0f;
    outScaleY = (winH > 0) ? static_cast<float>(fbH) / static_cast<float>(winH) : 1.0f;
}

bool Window::ShouldClose() const
{
    return m_Handle ? glfwWindowShouldClose(m_Handle) == GLFW_TRUE : true;
}

void Window::RequestClose()
{
    if (m_Handle)
        glfwSetWindowShouldClose(m_Handle, GLFW_TRUE);
}

void Window::SetTitle(const std::string& title)
{
    m_CurrentTitle = title;
    if (m_Handle)
        glfwSetWindowTitle(m_Handle, title.c_str());
}

void Window::GetPosition(int& outX, int& outY) const
{
    outX = 0;
    outY = 0;
    if (m_Handle)
        glfwGetWindowPos(m_Handle, &outX, &outY);
}

void Window::SetPosition(int x, int y)
{
    if (m_Handle)
        glfwSetWindowPos(m_Handle, x, y);
}

void Window::GetFrameSize(int& left, int& top, int& right, int& bottom) const
{
    left = top = right = bottom = 0;
    if (!m_Handle)
        return;
#if defined(__EMSCRIPTEN__)
    // glfwGetWindowFrameSize aborts in emscripten's libglfw.js. The canvas
    // has no OS chrome, so the frame is the client rect.
    return;
#else
    glfwGetWindowFrameSize(m_Handle, &left, &top, &right, &bottom);
#endif
}

void Window::Focus()
{
    if (!m_Handle)
        return;

    glfwFocusWindow(m_Handle);
#if defined(__APPLE__)
    id nsWindow = glfwGetCocoaWindow(m_Handle);
    if (nsWindow)
        GE_MacOS_FocusWindow((void*)nsWindow);
#endif
}

void Window::Show()
{
    if (m_Handle)
        glfwShowWindow(m_Handle);
}

void Window::Hide()
{
    if (m_Handle)
        glfwHideWindow(m_Handle);
}

void Window::SetFullscreen(bool enable)
{
    if (!m_Handle || enable == m_IsFullscreen)
        return;

#if defined(__EMSCRIPTEN__)
    // The canvas already fills the page, so "fullscreen" here means the
    // browser's Fullscreen API on the canvas. Deferred-until-event-handler:
    // browsers only honor the request inside a user gesture, and the editor's
    // toggle runs from a deferred action queue — EM_TRUE parks the request
    // until the next input event instead of failing. m_IsFullscreen is NOT
    // set here: the fullscreenchange callback owns it (the request can be
    // deferred or denied, and Esc exits without consulting the engine).
    // The browser fires a window resize on the transition, which the resize
    // thunk already routes into glfwSetWindowSize.
    if (enable)
        emscripten_request_fullscreen("#canvas", EM_TRUE);
    else
        emscripten_exit_fullscreen();
    return;
#else
    if (enable)
    {
        // Save current windowed rect.
        glfwGetWindowPos(m_Handle, &m_WindowedX, &m_WindowedY);
        glfwGetWindowSize(m_Handle, &m_WindowedW, &m_WindowedH);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (!monitor || !mode)
            return;

        glfwSetWindowMonitor(m_Handle,
                             monitor,
                             0,
                             0,
                             mode->width,
                             mode->height,
                             mode->refreshRate);
        m_IsFullscreen = true;
    }
    else
    {
        // Restore to previous windowed rect.
        int w = m_WindowedW > 0 ? m_WindowedW : 1280;
        int h = m_WindowedH > 0 ? m_WindowedH : 720;
        glfwSetWindowMonitor(m_Handle,
                             nullptr,
                             m_WindowedX,
                             m_WindowedY,
                             w,
                             h,
                             0);
        m_IsFullscreen = false;
    }
#endif
}

#if defined(__EMSCRIPTEN__)
void Window::NotifyWebFullscreenChanged(bool isFullscreen)
{
    if (m_IsFullscreen == isFullscreen)
        return;
    m_IsFullscreen = isFullscreen;
    if (m_FullscreenChangedHandler)
        m_FullscreenChangedHandler(isFullscreen);
}
#endif

void Window::SetOwnedBy(Window* owner)
{
#if defined(_WIN32)
    if (!m_Handle || !owner)
        return;
    HWND hwndChild = glfwGetWin32Window(m_Handle);
    HWND hwndOwner = glfwGetWin32Window(owner->GetGLFWHandle());
    if (!hwndChild || !hwndOwner)
        return;
    SetWindowLongPtr(hwndChild, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hwndOwner));
#else
    (void)owner;
#endif
}

void Window::SetDecorated(bool decorated)
{
    if (!m_Handle)
        return;
    glfwSetWindowAttrib(m_Handle, GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
}

void Window::SetResizable(bool resizable)
{
    if (!m_Handle)
        return;
    glfwSetWindowAttrib(m_Handle, GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);
}

namespace {
bool pollingWindowEvents = false;
}

void Window::RefreshThunk(GLFWwindow* win)
{
    // Programmatic window/swapchain changes can notify outside the event pump.
    // Hosts render these on their next ordinary frame, not inside configuration.
    if (!pollingWindowEvents) return;
    if (auto* self = FromGLFW(win); self && self->m_Refresh)
        self->m_Refresh();
}

void Window::PollEvents()
{
    if (pollingWindowEvents) return;
    struct PollScope
    {
        PollScope() { pollingWindowEvents = true; }
        ~PollScope() { pollingWindowEvents = false; }
    } scope;
    glfwPollEvents();
    for (Window* window : s_Windows)
    {
        if (window)
            window->CheckMonitorChanged();
    }
#if defined(__EMSCRIPTEN__)
    for (Window* window : s_Windows)
    {
        if (window)
            window->ApplyWebDragCursor();
    }
#endif

    PollGlobalMouseMonitors();
}

void Window::PollGlobalMouseMonitors()
{
    // Cross-platform best-effort polling for "global" monitors on platforms where
    // we don't install native hooks. This allows eyedropper preview/click to work
    // even when the mouse is outside our window(s), as long as the app keeps polling.
#if defined(_WIN32) || defined(__linux__)
    // Global move monitoring: call every poll so clients get smooth updates.
    if (s_GlobalMoveCallback)
    {
        int sx = 0, sy = 0;
        Window::GetCursorScreenPosition(sx, sy);
        s_GlobalMoveCallback(sx, sy);
    }

    // Global click monitoring: fire on LMB down edge.
    static bool s_PrevLmb = false;
    const bool lmbDownNow = Window::IsLeftMouseButtonDown();
    if (s_GlobalClickCallback)
    {
        if (lmbDownNow && !s_PrevLmb)
        {
            int sx = 0, sy = 0;
            Window::GetCursorScreenPosition(sx, sy);
            s_GlobalClickCallback(sx, sy);
        }
    }
    s_PrevLmb = lmbDownNow;
#endif
}

void Window::Terminate()
{
    if (!s_GlfwInitialized)
        return;

    glfwTerminate();
    s_GlfwInitialized = false;
}

Window* Window::FromGLFW(GLFWwindow* win)
{
    return win ? reinterpret_cast<Window*>(glfwGetWindowUserPointer(win)) : nullptr;
}

void Window::CheckMonitorChanged()
{
    if (!m_Handle)
        return;
    const int monitorIndex = GetActiveMonitorIndexForWindow(m_Handle);
    if (monitorIndex == m_LastMonitorIndex)
        return;
    m_LastMonitorIndex = monitorIndex;
    if (m_MonitorChanged)
        m_MonitorChanged(m_LastMonitorIndex);
}

void Window::CursorEnterThunk(GLFWwindow* win, int entered)
{
    if (auto* self = FromGLFW(win))
    {
        if (self->m_CursorEnter)
            self->m_CursorEnter(entered == GLFW_TRUE);
    }
}

void Window::FocusThunk(GLFWwindow* win, int focused)
{
    if (auto* self = FromGLFW(win))
    {
        if (self->m_Focus)
            self->m_Focus(focused == GLFW_TRUE);
    }
}

void Window::GetCursorScreenPosition(int& outX, int& outY)
{
    outX = 0;
    outY = 0;
#if defined(_WIN32)
    POINT p{0, 0};
    if (GetCursorPos(&p))
    {
        outX = p.x;
        outY = p.y;
    }
#elif defined(__APPLE__)
    GE_MacOS_GetCursorScreenPosition(&outX, &outY);
#elif defined(__linux__)
    Display* display = XOpenDisplay(nullptr);
    if (display)
    {
        ::Window root = DefaultRootWindow(display);
        ::Window child;
        int rootX, rootY, winX, winY;
        unsigned int mask;
        if (XQueryPointer(display, root, &root, &child, &rootX, &rootY, &winX, &winY, &mask))
        {
            outX = rootX;
            outY = rootY;
        }
        XCloseDisplay(display);
    }
#endif
}

bool Window::IsLeftMouseButtonDown()
{
#if defined(_WIN32)
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#elif defined(__APPLE__)
    return GE_MacOS_IsLeftMouseButtonDown();
#elif defined(__linux__)
    Display* display = XOpenDisplay(nullptr);
    if (display)
    {
        ::Window root = DefaultRootWindow(display);
        ::Window child;
        int rootX, rootY, winX, winY;
        unsigned int mask;
        XQueryPointer(display, root, &root, &child, &rootX, &rootY, &winX, &winY, &mask);
        XCloseDisplay(display);
        return (mask & Button1Mask) != 0;
    }
    return false;
#else
    return false;
#endif
}

bool Window::IsOptionKeyDown()
{
#if defined(_WIN32)
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
#elif defined(__APPLE__)
    return GE_MacOS_IsOptionKeyDown();
#else
    return false;
#endif
}

int Window::GetModifierKeyMask()
{
#if defined(_WIN32)
    int mods = 0;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        mods |= GLFW_MOD_SHIFT;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        mods |= GLFW_MOD_CONTROL;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
        mods |= GLFW_MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
        mods |= GLFW_MOD_SUPER;
    return mods;
#elif defined(__APPLE__)
    return GE_MacOS_GetModifierKeyMask();
#elif defined(__linux__)
    // Scroll callbacks run on GLFW's event thread, so reuse its existing X11
    // connection instead of opening and closing a display for every wheel tick.
    Display* display = glfwGetX11Display();
    if (!display)
        return 0;

    ::Window root = DefaultRootWindow(display);
    ::Window rootReturn = 0;
    ::Window childReturn = 0;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int state = 0;
    const bool queried = XQueryPointer(
        display, root, &rootReturn, &childReturn,
        &rootX, &rootY, &winX, &winY, &state) != 0;
    if (!queried)
        return 0;

    int mods = 0;
    if ((state & ShiftMask) != 0)
        mods |= GLFW_MOD_SHIFT;
    if ((state & ControlMask) != 0)
        mods |= GLFW_MOD_CONTROL;
    if ((state & Mod1Mask) != 0)
        mods |= GLFW_MOD_ALT;
    if ((state & Mod4Mask) != 0)
        mods |= GLFW_MOD_SUPER;
    return mods;
#elif defined(__EMSCRIPTEN__)
    // The browser exposes no modifier query; the mask comes from the flags of
    // the most recent DOM input event, recorded by the window-capture listener
    // installed at window creation. It runs before emscripten's canvas
    // listeners, so a GLFW callback always sees the flags of its own event.
    return EM_ASM_INT({ return Module.__geDomModifierMask | 0; });
#else
    return 0;
#endif
}

void Window::FireTitleBarRightClick(int screenX, int screenY)
{
    if (m_TitleBarRightClick)
        m_TitleBarRightClick(screenX, screenY);
}

#ifdef __APPLE__
static void TitleBarRightClickTrampoline(int screenX, int screenY, void* userData)
{
    auto* self = static_cast<Window*>(userData);
    if (self)
        self->FireTitleBarRightClick(screenX, screenY);
}
#endif

void Window::SetTitleBarRightClickHandler(TitleBarRightClickHandler h)
{
    m_TitleBarRightClick = std::move(h);
#if defined(__APPLE__)
    // Remove any existing monitor
    if (m_TitleBarRightClickMonitor)
    {
        GE_MacOS_RemoveTitleBarRightClickMonitor(m_TitleBarRightClickMonitor);
        m_TitleBarRightClickMonitor = nullptr;
    }
    if (m_TitleBarRightClick && m_Handle)
    {
        id nsWindow = glfwGetCocoaWindow(m_Handle);
        if (nsWindow)
            m_TitleBarRightClickMonitor = GE_MacOS_InstallTitleBarRightClickMonitor(
                (void*)nsWindow, &TitleBarRightClickTrampoline, this);
    }
#elif defined(_WIN32)
    // TODO: WM_NCRBUTTONUP handling for Win32
#endif
}

void Window::StartGlobalClickMonitor(GlobalClickCallback callback)
{
    s_GlobalClickCallback = std::move(callback);
#if defined(_WIN32)
    // Windows: We'll poll using IsLeftMouseButtonDown() in the app update loop
    // The callback will be called from the ColorPickerWindow when it detects a click
    // This is handled differently - see ColorPickerWindow.cpp
#elif defined(__APPLE__)
    if (s_GlobalClickCallback)
        GE_MacOS_StartGlobalClickMonitor(MacOS_GlobalClickCallbackWrapper, nullptr);
    else
        GE_MacOS_StopGlobalClickMonitor();
#elif defined(__linux__)
    // Linux: With pointer grab (from PushGlobalCrosshairCursor), click events
    // are delivered through the normal event system. The callback is stored
    // and will be invoked from the ColorPickerWindow's mouse handler.
#endif
}

void Window::StopGlobalClickMonitor()
{
    s_GlobalClickCallback = nullptr;
#if defined(__APPLE__)
    GE_MacOS_StopGlobalClickMonitor();
#endif
}

void Window::StartGlobalMoveMonitor(GlobalMoveCallback callback)
{
    s_GlobalMoveCallback = std::move(callback);
#if defined(__APPLE__)
    if (s_GlobalMoveCallback)
        GE_MacOS_StartGlobalMoveMonitor(MacOS_GlobalMoveCallbackWrapper, nullptr);
    else
        GE_MacOS_StopGlobalMoveMonitor();
#endif
    // Windows/Linux: Handled via polling or window event handlers
}

void Window::StopGlobalMoveMonitor()
{
    s_GlobalMoveCallback = nullptr;
#if defined(__APPLE__)
    GE_MacOS_StopGlobalMoveMonitor();
#endif
}

// Linux: store grabbed display/cursor for cleanup
#if defined(__linux__)
static Display* s_LinuxGrabbedDisplay = nullptr;
static Cursor s_LinuxCrosshairCursor = None;
#endif

void Window::PushGlobalCrosshairCursor()
{
#if defined(__APPLE__)
    GE_MacOS_PushCrosshairCursor();
#elif defined(_WIN32)
    // NOTE: This changes system cursors globally (affects all apps).
    // We do it because Win32 doesn't provide a newer per-process "global cursor" API.
    // We restore defaults on Pop via SPI_SETCURSORS.
    ++s_Win32GlobalCursorDepth;
    if (s_Win32GlobalCursorDepth == 1)
    {
        SetSystemCursor(LoadCursor(NULL, IDC_CROSS), OCR_NORMAL);
    }
#elif defined(__linux__)
    s_LinuxGrabbedDisplay = XOpenDisplay(nullptr);
    if (s_LinuxGrabbedDisplay)
    {
        ::Window root = DefaultRootWindow(s_LinuxGrabbedDisplay);
        s_LinuxCrosshairCursor = XCreateFontCursor(s_LinuxGrabbedDisplay, XC_crosshair);
        XGrabPointer(s_LinuxGrabbedDisplay, root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, s_LinuxCrosshairCursor, CurrentTime);
        XFlush(s_LinuxGrabbedDisplay);
    }
#endif
}

void Window::PopGlobalCursor()
{
#if defined(__APPLE__)
    GE_MacOS_PopCursor();
#elif defined(_WIN32)
    if (s_Win32GlobalCursorDepth <= 0)
    {
        s_Win32GlobalCursorDepth = 0;
        return;
    }

    --s_Win32GlobalCursorDepth;
    if (s_Win32GlobalCursorDepth == 0)
    {
        // Restore system cursors to defaults (theme/user settings aware).
        SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
    }
#elif defined(__linux__)
    if (s_LinuxGrabbedDisplay)
    {
        XUngrabPointer(s_LinuxGrabbedDisplay, CurrentTime);
        if (s_LinuxCrosshairCursor != None)
        {
            XFreeCursor(s_LinuxGrabbedDisplay, s_LinuxCrosshairCursor);
            s_LinuxCrosshairCursor = None;
        }
        XFlush(s_LinuxGrabbedDisplay);
        XCloseDisplay(s_LinuxGrabbedDisplay);
        s_LinuxGrabbedDisplay = nullptr;
    }
#endif
}

void Window::SetToolWindowStyle(bool enable)
{
#if defined(_WIN32)
    if (!m_Handle)
        return;
    HWND hwnd = glfwGetWin32Window(m_Handle);
    if (!hwnd)
        return;
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (enable)
    {
        ex |= WS_EX_TOOLWINDOW;
        ex &= ~WS_EX_APPWINDOW;
    }
    else
    {
        ex &= ~WS_EX_TOOLWINDOW;
        // Do not automatically set APPWINDOW here; caller can decide
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
#else
    (void)enable;
#endif
}

void Window::SetShowInTaskbar(bool show)
{
#if defined(_WIN32)
    if (!m_Handle)
        return;
    HWND hwnd = glfwGetWin32Window(m_Handle);
    if (!hwnd)
        return;
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (show)
    {
        ex |= WS_EX_APPWINDOW;
    }
    else
    {
        ex &= ~WS_EX_APPWINDOW;
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
#else
    (void)show;
#endif
}

void Window::SetAlwaysOnTop(bool enable)
{
    m_AlwaysOnTop = enable;
    if (!m_Handle)
        return;
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(m_Handle);
    if (!hwnd)
        return;
    SetWindowPos(hwnd, enable ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#elif defined(__APPLE__)
    id nsWindow = glfwGetCocoaWindow(m_Handle);
    if (nsWindow)
        GE_MacOS_SetWindowAlwaysOnTop((void*)nsWindow, enable);
#elif defined(__linux__)
    Display* display = glfwGetX11Display();
    ::Window window = glfwGetX11Window(m_Handle);
    if (!display || !window)
        return;
    
    Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
    Atom wmStateAbove = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    
    XEvent event = {};
    event.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = wmState;
    event.xclient.format = 32;
    event.xclient.data.l[0] = enable ? 1 : 0; // _NET_WM_STATE_ADD or _NET_WM_STATE_REMOVE
    event.xclient.data.l[1] = (long)wmStateAbove;
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 1; // Source indication: normal application
    
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
#else
    (void)enable;
#endif
}

void Window::SetIcon(const unsigned char* pixels, int width, int height)
{
    if (!m_Handle)
        return;

    if (!pixels || width <= 0 || height <= 0)
    {
        // Reset to default icon
        glfwSetWindowIcon(m_Handle, 0, nullptr);
        return;
    }

    GLFWimage image;
    image.width = width;
    image.height = height;
    image.pixels = const_cast<unsigned char*>(pixels);
    glfwSetWindowIcon(m_Handle, 1, &image);
}

void Window::CursorPosThunk(GLFWwindow* win, double x, double y)
{
    if (auto* self = FromGLFW(win))
    {
#if defined(__EMSCRIPTEN__)
        self->m_HasWebDragCursor = false;
#endif
        if (self->m_MouseMove)
            self->m_MouseMove(static_cast<float>(x), static_cast<float>(y));
    }
}

void Window::ScrollThunk(GLFWwindow* win, double xoff, double yoff)
{
    if (auto* self = FromGLFW(win))
    {
        if (self->m_Scroll)
            self->m_Scroll(
                static_cast<float>(xoff), static_cast<float>(yoff),
                GetModifierKeyMask());
    }
}
void Window::WindowPosThunk(GLFWwindow* win, int x, int y)
{
    Window* self = FromGLFW(win);
    if (!self)
        return;
    if (self->m_Position)
        self->m_Position(x, y);
    self->CheckMonitorChanged();
}

void Window::FramebufferSizeThunk(GLFWwindow* win, int width, int height)
{
    if (auto* self = FromGLFW(win))
    {
        if (self->m_FramebufferSize)
            self->m_FramebufferSize(width, height);
        self->CheckMonitorChanged();
        // Cocoa can resize a layer without invalidating its contents. The
        // framebuffer notification is the authoritative live-size change.
        if (width > 0 && height > 0)
            RefreshThunk(win);
    }
}

void Window::CloseThunk(GLFWwindow* win)
{
    auto* self = FromGLFW(win);
    if (!self || !self->m_CloseRequestedHandler)
        return;
    // Handler returns true to cancel this close attempt.
    if (self->m_CloseRequestedHandler())
        glfwSetWindowShouldClose(win, GLFW_FALSE);
}

void Window::DropThunk(GLFWwindow* win, int count, const char** paths)
{
    auto* self = FromGLFW(win);
    if (!self || !self->m_FileDrop || count <= 0 || !paths)
        return;

#if defined(__EMSCRIPTEN__)
    // Finder/OS drags emit dragover/drop, not mousemove. Apply the last HTML5
    // pointer so the Assets panel hit-test matches where the files were released.
    if (EM_ASM_INT({ return Module.__geDropCursorValid ? 1 : 0; }))
    {
        const float x = static_cast<float>(EM_ASM_DOUBLE({ return Module.__geDropCursorX || 0; }));
        const float y = static_cast<float>(EM_ASM_DOUBLE({ return Module.__geDropCursorY || 0; }));
        self->m_HasWebDragCursor = true;
        self->m_WebDragCursorX = x;
        self->m_WebDragCursorY = y;
        if (self->m_MouseMove)
            self->m_MouseMove(x, y);
    }
#endif

    std::vector<std::filesystem::path> dropped;
    dropped.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        if (!paths[i] || paths[i][0] == '\0')
            continue;
        std::filesystem::path path{std::u8string_view{reinterpret_cast<const char8_t*>(paths[i])}};
#if defined(__EMSCRIPTEN__)
        path = ResolveWebDroppedPath(std::move(path));
        if (path.empty())
            continue;
#endif
        dropped.push_back(std::move(path));
    }
    if (!dropped.empty())
        self->m_FileDrop(dropped);
}

#if defined(__EMSCRIPTEN__)
void Window::ApplyWebDragCursor()
{
    if (!EM_ASM_INT({ return Module.__geDropCursorFresh ? 1 : 0; }))
        return;
    const float x = static_cast<float>(EM_ASM_DOUBLE({ return Module.__geDropCursorX || 0; }));
    const float y = static_cast<float>(EM_ASM_DOUBLE({ return Module.__geDropCursorY || 0; }));
    EM_ASM({ Module.__geDropCursorFresh = 0; });
    m_HasWebDragCursor = true;
    m_WebDragCursorX = x;
    m_WebDragCursorY = y;
    if (m_MouseMove)
        m_MouseMove(x, y);
}

std::filesystem::path Window::ResolveWebDroppedPath(std::filesystem::path path)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        return path.lexically_normal();

    const auto name = path.filename();
    if (name.empty())
        return {};

    const std::filesystem::path dropRoot{"/.glfw_dropped_files"};
    if (!std::filesystem::exists(dropRoot, ec))
        return {};

    for (auto it = std::filesystem::recursive_directory_iterator(dropRoot, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it)
    {
        if (it->path().filename() == name)
            return it->path().lexically_normal();
    }
    return {};
}
#endif

void Window::MouseButtonThunk(GLFWwindow* win, int button, int action, int mods)
{
    if (auto* self = FromGLFW(win))
    {
#if !defined(_WIN32)
        // Refresh the cursor position ahead of a press: macOS can emit a
        // spurious cursor-leave when a click focuses a text input (tracking
        // area churn), which sentinels the UI's cursor position — the press
        // would then hit-test against nothing and read as an outside click,
        // dismissing every popup under the pointer. Not on Win32: the patched
        // GLFW (cmake/ports/glfw3) reports the button message's own point
        // through the cursor callback just before this one, and querying the
        // OS cursor now would replace it with wherever the mouse moved since.
        if (action == GLFW_PRESS && self->m_MouseMove)
        {
            double cx = 0.0;
            double cy = 0.0;
            glfwGetCursorPos(win, &cx, &cy);
            self->m_MouseMove(static_cast<float>(cx), static_cast<float>(cy));
        }
#endif
        if (self->m_MouseButton)
            self->m_MouseButton(button, action == GLFW_PRESS, mods);
    }
}

void Window::CharThunk(GLFWwindow* win, unsigned int codepoint)
{
    if (auto* self = FromGLFW(win))
    {
        if (self->m_Char)
            self->m_Char(codepoint);
    }
}

void Window::KeyThunk(GLFWwindow* win, int key, int scancode, int action, int mods)
{
    (void)scancode;
    if (auto* self = FromGLFW(win))
    {
        if (self->m_Key)
            self->m_Key(key, action, mods);
    }
}

uint32_t Window::GetScreenPixelColor(int screenX, int screenY)
{
#if defined(_WIN32)
    HDC hdc = GetDC(NULL);
    if (!hdc)
        return 0;
    COLORREF color = GetPixel(hdc, screenX, screenY);
    ReleaseDC(NULL, hdc);
    if (color == CLR_INVALID)
        return 0;
    // COLORREF is 0x00BBGGRR, convert to ARGB 0xAARRGGBB
    uint8_t r = GetRValue(color);
    uint8_t g = GetGValue(color);
    uint8_t b = GetBValue(color);
    return (0xFFu << 24) | (r << 16) | (g << 8) | b;
#elif defined(__APPLE__)
    return GE_MacOS_GetScreenPixelColor(screenX, screenY);
#elif defined(__linux__)
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return 0;
    
    ::Window root = DefaultRootWindow(display);
    XImage* image = XGetImage(display, root, screenX, screenY, 1, 1, AllPlanes, ZPixmap);
    if (!image)
    {
        XCloseDisplay(display);
        return 0;
    }
    
    unsigned long pixel = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    XCloseDisplay(display);
    
    // Extract RGB from pixel (format depends on visual, assume 24/32-bit RGB)
    uint8_t r = (pixel >> 16) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t b = pixel & 0xFF;
    return (0xFFu << 24) | (r << 16) | (g << 8) | b;
#else
    (void)screenX;
    (void)screenY;
    return 0;
#endif
}

void Window::SetCursor(CursorType type)
{
    if (!m_Handle)
        return;

#if defined(__EMSCRIPTEN__)
    ApplyWebCanvasCursor(WebCanvasCssCursor(type));
    return;
#endif

    // Destroy previous custom cursor if any
    if (m_Cursor)
    {
        glfwDestroyCursor(m_Cursor);
        m_Cursor = nullptr;
    }

    if (type == CursorType::Hidden)
    {
        glfwSetInputMode(m_Handle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        return;
    }

    glfwSetInputMode(m_Handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    int glfwShape = GLFW_ARROW_CURSOR;
    switch (type)
    {
        case CursorType::Arrow:     glfwShape = GLFW_ARROW_CURSOR; break;
        case CursorType::IBeam:     glfwShape = GLFW_IBEAM_CURSOR; break;
        case CursorType::Crosshair: glfwShape = GLFW_CROSSHAIR_CURSOR; break;
        case CursorType::Hand:      glfwShape = GLFW_HAND_CURSOR; break;
        case CursorType::HResize:   glfwShape = GLFW_HRESIZE_CURSOR; break;
        case CursorType::VResize:   glfwShape = GLFW_VRESIZE_CURSOR; break;
        case CursorType::Grab:
        case CursorType::Grabbing:
            glfwShape = GLFW_HAND_CURSOR;
            break;
        case CursorType::Move:
        case CursorType::NotAllowed:
            glfwShape = GLFW_ARROW_CURSOR;
            break;
#if defined(GLFW_RESIZE_NWSE_CURSOR)
        // 3.4 name; emscripten's GLFW header is 3.3 (crosshair is the closest 3.3 shape).
        case CursorType::NorthwestSoutheastResize: glfwShape = GLFW_RESIZE_NWSE_CURSOR; break;
#else
        case CursorType::NorthwestSoutheastResize: glfwShape = GLFW_CROSSHAIR_CURSOR; break;
#endif
        default: break;
    }

    m_Cursor = glfwCreateStandardCursor(glfwShape);
    if (m_Cursor)
        glfwSetCursor(m_Handle, m_Cursor);
}

void Window::ResetCursor()
{
    if (!m_Handle)
        return;

#if defined(__EMSCRIPTEN__)
    ApplyWebCanvasCursor("default");
    return;
#endif

    if (m_Cursor)
    {
        glfwDestroyCursor(m_Cursor);
        m_Cursor = nullptr;
    }

    glfwSetInputMode(m_Handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    glfwSetCursor(m_Handle, nullptr); // reset to default
}

} // namespace Platform
} // namespace GameEngine
