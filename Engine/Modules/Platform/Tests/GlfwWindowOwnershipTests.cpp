// Regression for the Win32 GLFW patch in cmake/ports/glfw3: modifier-release
// polling must resolve the active HWND through this GLFW instance's own window
// list, never through a foreign window's "GLFW" property.
//
// Links only GLFW and Win32: no Engine.dll, no graphics context, hidden windows
// only, no global input injection, and the desktop pointer is never moved (the
// disabled-cursor recentering path is deliberately not exercised). Each case
// runs in its own process and the cases are serialized because activation and
// attached input queues are under test. An interactive session is required;
// failing to establish the requested active HWND is a failure, not a skip.
//
// Cases (--case <name>):
//   local_modifiers    both an older and the newest local window repair stuck
//                      left/right Shift and Windows keys with exactly one
//                      release callback each
//   ctrl_keyup         Ctrl stays pressed until its ordinary native KEYUP
//   no_active          no active window is safe
//   non_glfw           a native non-GLFW active window is safe
//   property_sentinel  a native window carrying an unrelated "GLFW" property
//                      value must not be treated as an owned window
//   borrowed_property  a native HWND whose "GLFW" property equals a valid local
//                      window pointer must not repair that window's keys:
//                      pointer membership alone does not prove HWND ownership
//   foreign_glfw       a hidden helper process (--helper) creates a real GLFW
//                      window; the test attaches only its own two input threads,
//                      verifies the exact foreign active HWND, process, thread
//                      and property, polls, then detaches and polls locally again
//
// Test-only SEH turns an unpatched glfwPollEvents access violation into a
// failing exit status; the harness supplies no production exception handling.
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
constexpr DWORD kDeadlineMs = 10000;

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class Handle
{
  public:
    explicit Handle(HANDLE value = nullptr) : Value(value) {}
    ~Handle() { if (Value) ::CloseHandle(Value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return Value; }

  private:
    HANDLE Value;
};

struct Shared
{
    HWND Window = nullptr;
    DWORD Thread = 0;
    DWORD Process = 0;
    DWORD Parent = 0;
};

class MappingView
{
  public:
    explicit MappingView(HANDLE mapping)
        : Data(static_cast<Shared*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))))
    {
        Check(Data != nullptr, "MapViewOfFile failed");
    }
    ~MappingView() { ::UnmapViewOfFile(Data); }
    Shared* Data;
};

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

using Window = std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)>;
Window MakeWindow()
{
    Window window(glfwCreateWindow(80, 80, "GLFW ownership regression (hidden)", nullptr, nullptr),
                  glfwDestroyWindow);
    Check(window != nullptr, "glfwCreateWindow failed");
    return window;
}

// Keep SEH in a leaf with no C++ objects needing unwinding. Cleanup belongs to
// the caller's ordinary RAII scopes even when the unpatched dependency faults.
DWORD PollException()
{
    __try
    {
        glfwPollEvents();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return ::GetExceptionCode();
    }
}

void Poll()
{
    const DWORD fault = PollException();
    if (fault)
        std::fprintf(stderr, "glfwPollEvents exception=0x%08lx\n", fault);
    Check(fault == 0, "glfwPollEvents faulted");
}

void Activate(HWND window)
{
    ::SetActiveWindow(window);
    Check(::GetActiveWindow() == window, "test could not establish the requested active HWND");
}

struct KeyEvents
{
    ~KeyEvents()
    {
        if (Window)
        {
            glfwSetKeyCallback(Window, nullptr);
            glfwSetWindowUserPointer(Window, nullptr);
        }
    }
    GLFWwindow* Window = nullptr;
    std::array<unsigned, GLFW_KEY_LAST + 1> Press{};
    std::array<unsigned, GLFW_KEY_LAST + 1> Release{};
};

void CaptureKeys(GLFWwindow* window, KeyEvents& events)
{
    events.Window = window;
    glfwSetWindowUserPointer(window, &events);
    glfwSetKeyCallback(window, [](GLFWwindow* source, int key, int, int action, int)
    {
        auto& events = *static_cast<KeyEvents*>(glfwGetWindowUserPointer(source));
        if (key >= 0 && key <= GLFW_KEY_LAST)
        {
            if (action == GLFW_PRESS) ++events.Press[key];
            if (action == GLFW_RELEASE) ++events.Release[key];
        }
    });
}

class KeyboardState
{
  public:
    KeyboardState() { Check(::GetKeyboardState(Saved.data()) != FALSE, "GetKeyboardState failed"); }
    ~KeyboardState() { ::SetKeyboardState(Saved.data()); }
    void Set(std::initializer_list<int> pressed)
    {
        std::array<BYTE, 256> keys{};
        for (const int key : pressed) keys[key] = 0x80;
        Check(::SetKeyboardState(keys.data()) != FALSE, "SetKeyboardState failed");
    }

  private:
    std::array<BYTE, 256> Saved{};
};

void Key(HWND window, int glfwKey, UINT virtualKey, bool down)
{
    const int scan = glfwGetKeyScancode(glfwKey);
    Check(scan >= 0, "missing native key scancode");
    LPARAM bits = 1 | ((scan & 0xff) << 16) | ((scan & 0x100) << 16);
    if (!down) bits |= static_cast<LPARAM>(0xc0000000u);
    ::SendMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, virtualKey, bits);
}

void LocalModifiers()
{
    auto older = MakeWindow();
    auto newer = MakeWindow();
    KeyboardState keyboard;
    constexpr int keys[] = {GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT,
                            GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SUPER};
    constexpr UINT native[] = {VK_SHIFT, VK_SHIFT, VK_LWIN, VK_RWIN};
    for (GLFWwindow* window : {older.get(), newer.get()})
    {
        Activate(glfwGetWin32Window(window));
        Poll();
        KeyEvents events;
        CaptureKeys(window, events);
        keyboard.Set({VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN});
        for (unsigned i = 0; i < std::size(keys); ++i)
        {
            Key(glfwGetWin32Window(window), keys[i], native[i], true);
            Check(glfwGetKey(window, keys[i]) == GLFW_PRESS, "native modifier press was not delivered");
        }
        Poll();
        for (const int key : keys)
            Check(glfwGetKey(window, key) == GLFW_PRESS && events.Release[key] == 0,
                  "poll released a modifier whose native key state is still down");
        keyboard.Set({}); // Simulate the missing KEYUP that the poll workaround repairs.
        Poll();
        Poll();
        for (const int key : keys)
        {
            Check(glfwGetKey(window, key) == GLFW_RELEASE, "stuck modifier was not repaired");
            Check(events.Press[key] == 1 && events.Release[key] == 1,
                  "modifier repair did not deliver exactly one press and release");
        }
        glfwSetKeyCallback(window, nullptr);
        glfwSetWindowUserPointer(window, nullptr);
    }
}

void CtrlKeyUp()
{
    auto window = MakeWindow();
    Activate(glfwGetWin32Window(window.get()));
    Poll();
    KeyboardState keyboard;
    KeyEvents events;
    CaptureKeys(window.get(), events);
    keyboard.Set({VK_CONTROL, VK_LCONTROL});
    Key(glfwGetWin32Window(window.get()), GLFW_KEY_LEFT_CONTROL, VK_CONTROL, true);
    Check(glfwGetKey(window.get(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS, "Ctrl press missing");
    keyboard.Set({});
    Poll();
    Check(glfwGetKey(window.get(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS,
          "modifier workaround unexpectedly released Ctrl without KEYUP");
    Key(glfwGetWin32Window(window.get()), GLFW_KEY_LEFT_CONTROL, VK_CONTROL, false);
    Poll();
    Check(glfwGetKey(window.get(), GLFW_KEY_LEFT_CONTROL) == GLFW_RELEASE &&
          events.Press[GLFW_KEY_LEFT_CONTROL] == 1 && events.Release[GLFW_KEY_LEFT_CONTROL] == 1,
          "ordinary Ctrl KEYUP changed behavior");
    glfwSetKeyCallback(window.get(), nullptr);
}

class NativeWindow
{
  public:
    NativeWindow() : Value(::CreateWindowExW(0, L"STATIC", L"GLFW ownership native control",
                                           WS_OVERLAPPED, 0, 0, 80, 80, nullptr, nullptr,
                                           ::GetModuleHandleW(nullptr), nullptr))
    {
        Check(Value != nullptr, "native control window creation failed");
    }
    ~NativeWindow()
    {
        ::RemovePropW(Value, L"GLFW");
        ::DestroyWindow(Value);
    }
    HWND Value;
};

void NativeControl(bool sentinel)
{
    auto local = MakeWindow();
    NativeWindow control;
    Activate(control.Value);
    Check(::GetPropW(control.Value, L"GLFW") == nullptr, "native control already has a GLFW property");
    if (sentinel)
        Check(::SetPropW(control.Value, L"GLFW", reinterpret_cast<HANDLE>(1)) != FALSE,
              "could not install non-owned property sentinel");
    Poll();
    Check(::GetActiveWindow() == control.Value, "poll changed the native control's activation");
}

void BorrowedProperty()
{
    auto local = MakeWindow();
    NativeWindow control;
    Activate(control.Value);
    Poll();
    const HANDLE borrowed = ::GetPropW(glfwGetWin32Window(local.get()), L"GLFW");
    Check(borrowed != nullptr && ::SetPropW(control.Value, L"GLFW", borrowed) != FALSE,
          "could not install a valid borrowed GLFW property");
    KeyboardState keyboard;
    KeyEvents events;
    CaptureKeys(local.get(), events);
    keyboard.Set({VK_SHIFT, VK_LSHIFT});
    Key(glfwGetWin32Window(local.get()), GLFW_KEY_LEFT_SHIFT, VK_SHIFT, true);
    Check(glfwGetKey(local.get(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS, "borrowed control press missing");
    keyboard.Set({});
    Poll();
    Check(::GetActiveWindow() == control.Value, "borrowed control activation changed");
    Check(glfwGetKey(local.get(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS &&
          events.Release[GLFW_KEY_LEFT_SHIFT] == 0,
          "unowned active HWND repaired keys on the unrelated local GLFW window");
    Key(glfwGetWin32Window(local.get()), GLFW_KEY_LEFT_SHIFT, VK_SHIFT, false);
}

class ChildProcess
{
  public:
    explicit ChildProcess(const std::wstring& name)
    {
        std::array<wchar_t, 32768> executable{};
        const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(),
                                                 static_cast<DWORD>(executable.size()));
        Check(length && length < executable.size(), "could not resolve test executable");
        std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --helper " + name;
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        Check(::CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &Info) != FALSE,
              "helper process creation failed");
    }
    ~ChildProcess()
    {
        if (::WaitForSingleObject(Info.hProcess, 0) == WAIT_TIMEOUT)
            ::TerminateProcess(Info.hProcess, 18);
        ::WaitForSingleObject(Info.hProcess, kDeadlineMs);
        ::CloseHandle(Info.hThread);
        ::CloseHandle(Info.hProcess);
    }
    PROCESS_INFORMATION Info{};
};

class Attachment
{
  public:
    explicit Attachment(DWORD other) : Other(other)
    {
        Check(::AttachThreadInput(::GetCurrentThreadId(), Other, TRUE) != FALSE,
              "AttachThreadInput failed");
    }
    ~Attachment() { ::AttachThreadInput(::GetCurrentThreadId(), Other, FALSE); }

  private:
    DWORD Other;
};

int Helper(const std::wstring& name)
{
    Handle mapping(::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str()));
    Check(mapping.Get() != nullptr, "helper could not open mapping");
    MappingView shared(mapping.Get());
    Handle ready(::OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-ready").c_str()));
    Handle stop(::OpenEventW(SYNCHRONIZE, FALSE, (name + L"-stop").c_str()));
    Handle parent(::OpenProcess(SYNCHRONIZE, FALSE, shared.Data->Parent));
    Check(ready.Get() && stop.Get() && parent.Get(), "helper could not open lifetime handles");
    GlfwSession session;
    auto window = MakeWindow();
    shared.Data->Window = glfwGetWin32Window(window.get());
    shared.Data->Thread = ::GetCurrentThreadId();
    shared.Data->Process = ::GetCurrentProcessId();
    Check(::SetEvent(ready.Get()) != FALSE, "helper could not signal readiness");
    const HANDLE waits[] = {stop.Get(), parent.Get()};
    for (;;)
    {
        const DWORD result = ::MsgWaitForMultipleObjects(2, waits, FALSE, kDeadlineMs, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) return 0;
        Check(result == WAIT_OBJECT_0 + 2 || result == WAIT_TIMEOUT, "helper wait failed");
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
}

void ForeignGlfw()
{
    auto local = MakeWindow();
    Activate(glfwGetWin32Window(local.get()));
    Poll();
    const auto name = L"Local\\GlfwWindowOwnership-" + std::to_wstring(::GetCurrentProcessId()) +
                      L"-" + std::to_wstring(::GetTickCount64());
    Handle mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(Shared), name.c_str()));
    Check(mapping.Get() != nullptr, "shared mapping creation failed");
    MappingView shared(mapping.Get());
    *shared.Data = Shared{};
    shared.Data->Parent = ::GetCurrentProcessId();
    Handle ready(::CreateEventW(nullptr, TRUE, FALSE, (name + L"-ready").c_str()));
    Handle stop(::CreateEventW(nullptr, TRUE, FALSE, (name + L"-stop").c_str()));
    Check(ready.Get() && stop.Get(), "helper event creation failed");
    ChildProcess child(name);
    const HANDLE waits[] = {ready.Get(), child.Info.hProcess};
    Check(::WaitForMultipleObjects(2, waits, FALSE, kDeadlineMs) == WAIT_OBJECT_0,
          "helper did not become ready before timeout or exit");
    DWORD owner = 0;
    Check(shared.Data->Process == child.Info.dwProcessId &&
          ::GetWindowThreadProcessId(shared.Data->Window, &owner) == shared.Data->Thread &&
          owner == child.Info.dwProcessId, "helper HWND identity is not owned by its process/thread");
    {
        Attachment attached(shared.Data->Thread);
        Activate(shared.Data->Window);
        const HWND active = ::GetActiveWindow();
        const DWORD thread = ::GetWindowThreadProcessId(active, &owner);
        Check(active == shared.Data->Window && owner == child.Info.dwProcessId &&
              owner != ::GetCurrentProcessId() && thread == shared.Data->Thread &&
              ::GetPropW(active, L"GLFW") != nullptr,
              "attached control did not expose the foreign GLFW HWND/property");
        std::printf("attached active=%p owner=%lu caller=%lu property=%p\n", active, owner,
                    ::GetCurrentProcessId(), ::GetPropW(active, L"GLFW"));
        Poll();
        Check(::GetActiveWindow() == shared.Data->Window, "poll changed the foreign active HWND");
    }
    Activate(glfwGetWin32Window(local.get()));
    Poll();
    Check(::SetEvent(stop.Get()) != FALSE, "helper stop signal failed");
    Check(::WaitForSingleObject(child.Info.hProcess, kDeadlineMs) == WAIT_OBJECT_0,
          "helper did not stop before deadline");
    DWORD exit = 0;
    Check(::GetExitCodeProcess(child.Info.hProcess, &exit) != FALSE && exit == 0,
          "helper failed during cleanup");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try
    {
        if (argc == 3 && std::wstring(argv[1]) == L"--helper") return Helper(argv[2]);
        Check(argc == 3 && std::wstring(argv[1]) == L"--case", "expected --case <name>");
        GlfwSession session;
        const std::wstring name = argv[2];
        if (name == L"local_modifiers") LocalModifiers();
        else if (name == L"ctrl_keyup") CtrlKeyUp();
        else if (name == L"non_glfw") NativeControl(false);
        else if (name == L"property_sentinel") NativeControl(true);
        else if (name == L"borrowed_property") BorrowedProperty();
        else if (name == L"foreign_glfw") ForeignGlfw();
        else if (name == L"no_active")
        {
            auto window = MakeWindow();
            Activate(nullptr);
            Poll();
            Check(::GetActiveWindow() == nullptr, "poll created an active HWND");
        }
        else throw std::runtime_error("unknown test case");
        std::printf("PASS: %ls\n", name.c_str());
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s (Win32 error %lu)\n", error.what(), ::GetLastError());
        return 1;
    }
}
