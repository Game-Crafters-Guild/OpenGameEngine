#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Forward declare GLFW types to avoid exposing GLFW in public headers
struct GLFWwindow;
struct GLFWcursor;

namespace GameEngine
{
namespace Platform
{

struct WindowDesc
{
    std::string Title;
    int Width = 0;
    int Height = 0;
    GLFWwindow* Share = nullptr;
    std::string ClassName;

    // When true, the window is created with WS_EX_TOOLWINDOW applied before
    // it becomes visible, preventing shell zone managers (e.g. FancyZones)
    // from intercepting it. No-op on non-Windows platforms.
    bool ToolWindow = false;

    // When true, the window is created hidden and must be shown explicitly
    // via Show(). Use this to render one frame before making the window
    // visible, avoiding a flash of empty/gray content.
    bool StartHidden = false;

    // Request that the platform opens the window in its maximized state.
    bool StartMaximized = false;
};

class Window
{
  public:
    using MouseMoveHandler = std::function<void(float x, float y)>;
    using MouseButtonHandler = std::function<void(int button, bool pressed, int mods)>;
    using CursorEnterHandler = std::function<void(bool entered)>; // true when cursor enters window, false on leave
    using FocusHandler = std::function<void(bool focused)>;
    using CharHandler = std::function<void(unsigned int codepoint)>;
    using KeyHandler = std::function<void(int key, int action, int mods)>;
    using PositionHandler = std::function<void(int x, int y)>;
    using RefreshHandler = std::function<void()>;
    using FramebufferSizeHandler = std::function<void(int width, int height)>;
    using MonitorChangedHandler = std::function<void(int monitorIndex)>;
    using ScrollHandler = std::function<void(float dx, float dy, int mods)>;
    using FileDropHandler = std::function<void(const std::vector<std::filesystem::path>& paths)>;
    using TitleBarRightClickHandler = std::function<void(int screenX, int screenY)>;
    /// Invoked when the user requests window close (X button, Cmd+Q path, Alt+F4, etc.).
    /// Return **true** to **cancel** the close ( GLFW close flag is cleared).
    /// Return **false** to allow the platform default (window will close normally).
    using CloseRequestedHandler = std::function<bool()>;

    Window() = default;
    ~Window();

    // Create a window (no graphics context) suitable for external rendering APIs.
    bool Create(const WindowDesc& desc);
    void Destroy();

    // Native handle for swapchain/surface creation
    void* GetNativeHandle() const;     // returns GLFWwindow*
    GLFWwindow* GetGLFWHandle() const; // convenience

    // Window queries
    void GetFramebufferSize(int& outWidth, int& outHeight) const;   // pixels
    void GetWindowSize(int& outWidth, int& outHeight) const;        // screen coords (DIP)
    void SetWindowSize(int width, int height);                      // screen coords (DIP)
    bool IsFocused() const;
    bool IsKeyPressed(int key) const;
    void GetContentScale(float& outScaleX, float& outScaleY) const; // scale = framebuffer / window (cursor-to-pixel ratio)
    // UI content scale (physical px per CSS logical px), pushed by the UI layer.
    // Used by native popups (context menus) to map UI-logical coordinates to
    // window-client coordinates. Zero means "unset; treat UI coords as client coords."
    void SetUiContentScale(float scale) { m_UiContentScale = scale; }
    float GetUiContentScale() const { return m_UiContentScale; }
    // GLFW client-area cursor position (same space as the values delivered to the mouse-move handler).
    void GetCursorClientPosition(float& outX, float& outY) const;
    bool ShouldClose() const;
    void RequestClose();
    void SetTitle(const std::string& title);
    // Last title passed to SetTitle (cached — glfw has no getter). Empty until first set.
    const std::string& GetTitle() const { return m_CurrentTitle; }

    // Positioning and focus
    void GetPosition(int& outX, int& outY) const;
    void SetPosition(int x, int y);
    void GetFrameSize(int& left, int& top, int& right, int& bottom) const;
    void Focus();
    void Show();
    void Hide();

    // Window ownership/chrome
    void SetOwnedBy(Window* owner);    // no-op on non-Windows
    void SetDecorated(bool decorated); // wraps glfwSetWindowAttrib(GLFW_DECORATED,...)
    void SetResizable(bool resizable); // wraps glfwSetWindowAttrib(GLFW_RESIZABLE,...)

    // Windows-specific: make the window use a smaller tool-window nonclient style
    // (no-op on non-Windows platforms). Typically removes taskbar button and uses
    // a reduced-height caption, which is suitable for secondary/floating windows.
    void SetToolWindowStyle(bool enable);

    // Windows-specific: toggle whether this window shows in the taskbar (no-op elsewhere)
    void SetShowInTaskbar(bool show);

    // Keep window always on top of other windows (cross-platform)
    void SetAlwaysOnTop(bool enable);
    bool IsAlwaysOnTop() const { return m_AlwaysOnTop; }

    // Set window icon from RGBA pixel data. On Windows/macOS the embedded resource
    // icon is typically used, but this is needed for Linux where icons cannot be
    // embedded in ELF executables. Pass nullptr to reset to default.
    // Pixels must be RGBA 32-bit (4 bytes per pixel), row-major, top-to-bottom.
    void SetIcon(const unsigned char* pixels, int width, int height);

    // Input handlers (optional)
    void SetMouseMoveHandler(MouseMoveHandler h) { m_MouseMove = std::move(h); }
    void SetMouseButtonHandler(MouseButtonHandler h) { m_MouseButton = std::move(h); }
    void SetCursorEnterHandler(CursorEnterHandler h) { m_CursorEnter = std::move(h); }
    void SetFocusHandler(FocusHandler h) { m_Focus = std::move(h); }
    void SetCharHandler(CharHandler h) { m_Char = std::move(h); }
    void SetKeyHandler(KeyHandler h) { m_Key = std::move(h); }
    void SetPositionHandler(PositionHandler h) { m_Position = std::move(h); }
    // Dispatched during PollEvents when the OS requests repaint or changes framebuffer size.
    void SetRefreshHandler(RefreshHandler h) { m_Refresh = std::move(h); }
    void SetFramebufferSizeHandler(FramebufferSizeHandler h) { m_FramebufferSize = std::move(h); }
    void SetMonitorChangedHandler(MonitorChangedHandler h) { m_MonitorChanged = std::move(h); }
    void SetScrollHandler(ScrollHandler h) { m_Scroll = std::move(h); }
    void SetFileDropHandler(FileDropHandler h) { m_FileDrop = std::move(h); }
    // Fires when fullscreen state actually transitions. Only the web target
    // fires it today: there the browser owns the state (a request can be
    // deferred or denied, and Esc exits without consulting the engine), so
    // app chrome tracking fullscreen must follow this, not its own toggle.
    using FullscreenChangedHandler = std::function<void(bool isFullscreen)>;
    void SetFullscreenChangedHandler(FullscreenChangedHandler h)
    {
        m_FullscreenChangedHandler = std::move(h);
    }
#if defined(__EMSCRIPTEN__)
    // Called from the browser fullscreenchange event thunk.
    void NotifyWebFullscreenChanged(bool isFullscreen);
#endif
    void SetTitleBarRightClickHandler(TitleBarRightClickHandler h);

    /// Optional hook to intercept close (e.g. unsaved-document prompts).
    /// If unset, GLFW's default behavior applies.
    void SetCloseRequestedHandler(CloseRequestedHandler h) { m_CloseRequestedHandler = std::move(h); }
    void FireTitleBarRightClick(int screenX, int screenY);

    // User pointer for client (separate from the GLFW user pointer used internally)
    void SetUserPointer(void* p) { m_UserPtr = p; }
    void* GetUserPointer() const { return m_UserPtr; }

    // Global event pump
    static bool Initialize();
    static void PollEvents();
    static void Terminate();

    // Global cursor helpers (best-effort cross-platform; fully implemented on Win32)
    static void GetCursorScreenPosition(int& outX, int& outY);
    static bool IsLeftMouseButtonDown();
    static bool IsOptionKeyDown();
    /// Returns the modifiers physically held at the OS level using GLFW_MOD_* bits.
    /// Unlike IsKeyPressed(), this is not limited to key events reported to one window.
    static int GetModifierKeyMask();

    // Screen color sampling: returns ARGB (0xAARRGGBB) at given screen coordinates.
    // Best-effort cross-platform; returns 0 on unsupported platforms.
    static uint32_t GetScreenPixelColor(int screenX, int screenY);

    // Global click monitoring for eyedropper-style tools.
    // Callback receives screen coordinates when user clicks anywhere on screen.
    // Only one global monitor can be active at a time.
    using GlobalClickCallback = std::function<void(int screenX, int screenY)>;
    static void StartGlobalClickMonitor(GlobalClickCallback callback);
    static void StopGlobalClickMonitor();

    // Global mouse move monitoring for live preview (e.g., eyedropper)
    using GlobalMoveCallback = std::function<void(int screenX, int screenY)>;
    static void StartGlobalMoveMonitor(GlobalMoveCallback callback);
    static void StopGlobalMoveMonitor();

    // Global cursor for eyedropper mode (cursor persists even outside app windows)
    static void PushGlobalCrosshairCursor();
    static void PopGlobalCursor();

    // Cursor appearance
    enum class CursorType
    {
        Arrow,
        IBeam,
        Crosshair,
        Hand,
        HResize,
        VResize,
        NorthwestSoutheastResize,
        Move,
        NotAllowed,
        Grab,
        Grabbing,
        Hidden
    };
    void SetCursor(CursorType type);
    void ResetCursor(); // restore default arrow

    // True fullscreen toggle backed by glfwSetWindowMonitor.
    void SetFullscreen(bool enable);
    bool IsFullscreen() const { return m_IsFullscreen; }
    int GetActiveMonitorIndex() const { return m_LastMonitorIndex; }

  private:
    static Window* FromGLFW(GLFWwindow* win);
    static void CursorPosThunk(GLFWwindow* win, double x, double y);
    static void CursorEnterThunk(GLFWwindow* win, int entered);
    static void FocusThunk(GLFWwindow* win, int focused);
    static void ScrollThunk(GLFWwindow* win, double xoff, double yoff);
    static void MouseButtonThunk(GLFWwindow* win, int button, int action, int mods);
    static void CharThunk(GLFWwindow* win, unsigned int codepoint);
    static void KeyThunk(GLFWwindow* win, int key, int scancode, int action, int mods);
    static void WindowPosThunk(GLFWwindow* win, int x, int y);
    static void RefreshThunk(GLFWwindow* win);
    static void PollGlobalMouseMonitors();
    static void FramebufferSizeThunk(GLFWwindow* win, int width, int height);
    static void CloseThunk(GLFWwindow* win);
    static void DropThunk(GLFWwindow* win, int count, const char** paths);
    void CheckMonitorChanged();
#if defined(__EMSCRIPTEN__)
    void ApplyWebDragCursor();
    static std::filesystem::path ResolveWebDroppedPath(std::filesystem::path path);
#endif

    GLFWwindow* m_Handle = nullptr;
    void* m_UserPtr = nullptr;
    struct GLFWcursor* m_Cursor = nullptr; // custom cursor (if set)
    float m_UiContentScale = 0.0f;         // 0 = unset; otherwise pushed by UI layer

    // Fullscreen bookkeeping for SetFullscreen.
    bool m_IsFullscreen = false;
    bool m_AlwaysOnTop = false;
    int m_WindowedX = 0;
    int m_WindowedY = 0;
    int m_WindowedW = 0;
    int m_WindowedH = 0;
    int m_LastMonitorIndex = -1;

    MouseMoveHandler m_MouseMove;
    MouseButtonHandler m_MouseButton;
    CursorEnterHandler m_CursorEnter;
    FocusHandler m_Focus;
    CharHandler m_Char;
    KeyHandler m_Key;
    PositionHandler m_Position;
    RefreshHandler m_Refresh;
    FramebufferSizeHandler m_FramebufferSize;
    MonitorChangedHandler m_MonitorChanged;
    ScrollHandler m_Scroll;
    FileDropHandler m_FileDrop;
    FullscreenChangedHandler m_FullscreenChangedHandler;
    TitleBarRightClickHandler m_TitleBarRightClick;
    CloseRequestedHandler m_CloseRequestedHandler;
    void* m_TitleBarRightClickMonitor = nullptr; // platform-specific event monitor handle
    std::string m_CurrentTitle;                  // cached last SetTitle value (glfw has no getter)
#if defined(__EMSCRIPTEN__)
    // HTML5 file drags do not emit GLFW mousemove. Last dragover/drop client
    // position is used until a real cursor event arrives.
    bool m_HasWebDragCursor = false;
    float m_WebDragCursorX = 0.0f;
    float m_WebDragCursorY = 0.0f;
#endif
};

} // namespace Platform
} // namespace GameEngine
