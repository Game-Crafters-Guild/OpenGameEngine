#include "Platform/Shell.h"
#if !defined(_WIN32) && !defined(__APPLE__)
#include "LinuxDialogPaths.h"
#endif

#include "Logger/Logger.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Windows headers must be included before shellapi/shlwapi so that
// macros like EXTERN_C and DECLSPEC_IMPORT are defined correctly.
#include <windows.h>

#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#else
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace GameEngine
{
namespace Platform
{

namespace
{

#if defined(_WIN32)
// Find a sensible owner HWND for native modal dialogs. GetActiveWindow()
// returns NULL when the calling thread doesn't own a window directly —
// common with GLFW-backed windows where the message thread is GLFW's,
// not the caller's. GetForegroundWindow() is also unreliable. As a
// fallback, enumerate top-level windows belonging to the current
// process and pick the largest visible one (the editor main window).
struct OwnerHwndScan
{
    HWND best = nullptr;
    LONG bestArea = 0;
    DWORD pid = 0;
};

BOOL CALLBACK PickLargestWindowProc(HWND hwnd, LPARAM param)
{
    auto* scan = reinterpret_cast<OwnerHwndScan*>(param);
    if (!IsWindowVisible(hwnd))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != scan->pid)
        return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect))
        return TRUE;
    const LONG w = rect.right - rect.left;
    const LONG h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0)
        return TRUE;
    const LONG area = w * h;
    if (area > scan->bestArea)
    {
        scan->bestArea = area;
        scan->best = hwnd;
    }
    return TRUE;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), wlen);
    return out;
}

HWND ResolveDialogOwnerHwnd()
{
    HWND hwnd = GetActiveWindow();
    const char* source = "GetActiveWindow";
    if (!hwnd)
    {
        hwnd = GetForegroundWindow();
        source = "GetForegroundWindow";
        if (hwnd)
        {
            // Only trust GetForegroundWindow if it belongs to us.
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId())
                hwnd = nullptr;
        }
    }
    if (!hwnd)
    {
        OwnerHwndScan scan;
        scan.pid = GetCurrentProcessId();
        EnumWindows(&PickLargestWindowProc, reinterpret_cast<LPARAM>(&scan));
        hwnd = scan.best;
        source = "EnumWindows";
    }
    Logger::Log::Info("Shell: dialog owner HWND={} via {}", reinterpret_cast<void*>(hwnd), source);
    return hwnd;
}

void PrepareDialogForeground(HWND ownerHwnd)
{
    if (!ownerHwnd)
        return;
    AllowSetForegroundWindow(ASFW_ANY);
    if (IsIconic(ownerHwnd))
        ShowWindow(ownerHwnd, SW_RESTORE);
    SetForegroundWindow(ownerHwnd);
    BringWindowToTop(ownerHwnd);
}

// Worker that runs while pfd->Show() is blocking the calling thread.
// Polls for the modal dialog window owned by our process (any new
// top-level window created after Show() begins) and forces it to the
// foreground. Without this the dialog can render behind the editor's
// Vulkan swapchain on some setups (occurs intermittently — likely a
// Z-order race between SetForegroundWindow and the swapchain present).
struct DialogPromoteContext
{
    DWORD pid = 0;
    HWND ownerHwnd = nullptr;
    std::atomic<bool> stop{false};
};

DWORD WINAPI PromoteDialogToForegroundThread(LPVOID lpParam)
{
    auto* ctx = static_cast<DialogPromoteContext*>(lpParam);

    // Snapshot of windows already owned by our process (and the owner)
    // so we can pick out the dialog when it appears as a *new* window.
    struct Snapshot
    {
        DWORD pid;
        std::vector<HWND> existing;
    };
    Snapshot before{ctx->pid, {}};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL
    {
        auto* s = reinterpret_cast<Snapshot*>(param);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == s->pid)
            s->existing.push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&before));

    HWND promoted = nullptr;
    int promoteAttemptMs = 0;
    // Try for up to ~3 seconds (60 * 50ms). The dialog usually appears
    // within 100-300ms of Show() being called.
    for (int attempt = 0; attempt < 60 && !ctx->stop.load(); ++attempt)
    {
        Sleep(50);
        promoteAttemptMs = (attempt + 1) * 50;
        struct Scan
        {
            DWORD pid;
            HWND owner;
            const std::vector<HWND>* existing;
            HWND found;
            wchar_t foundClassName[64];
        };
        Scan scan{ctx->pid, ctx->ownerHwnd, &before.existing, nullptr, {}};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL
        {
            auto* s = reinterpret_cast<Scan*>(param);
            if (hwnd == s->owner)
                return TRUE;
            DWORD windowPid = 0;
            GetWindowThreadProcessId(hwnd, &windowPid);
            if (windowPid != s->pid)
                return TRUE;
            if (!IsWindowVisible(hwnd))
                return TRUE;
            // Skip windows that already existed before Show() was called.
            for (HWND existing : *s->existing)
            {
                if (existing == hwnd)
                    return TRUE;
            }
            // Skip 0×0 / invisible windows.
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect))
                return TRUE;
            if (rect.right - rect.left <= 1 || rect.bottom - rect.top <= 1)
                return TRUE;
            GetClassNameW(hwnd, s->foundClassName, 64);
            s->found = hwnd;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&scan));
        if (scan.found)
        {
            promoted = scan.found;
            // Some IFileDialog implementations create the visible dialog
            // window with WS_EX_TOOLWINDOW or behind the swapchain; force
            // it to top + foreground.
            SetWindowPos(promoted, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
            SetForegroundWindow(promoted);
            BringWindowToTop(promoted);
            char classUtf8[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, scan.foundClassName, -1,
                                classUtf8, sizeof(classUtf8), nullptr, nullptr);
            Logger::Log::Info("Shell: promoted dialog HWND={} class='{}' after {}ms",
                              reinterpret_cast<void*>(promoted), classUtf8, promoteAttemptMs);
            break;
        }
    }
    if (!promoted)
        Logger::Log::Warning("Shell: dialog promote thread timed out after {}ms (no new visible window appeared in our process)", promoteAttemptMs);
    return 0;
}
#endif

#if !defined(_WIN32)
void ReapDetachedProcess(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }

    std::thread([pid]()
    {
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
        {
        }
    }).detach();
}

bool SpawnDetached(const char* const argv[])
{
    if (!argv || !argv[0])
    {
        return false;
    }
    
    pid_t pid = 0;
    int status = posix_spawnp(&pid, argv[0], nullptr, nullptr,
                              const_cast<char* const*>(argv),
                              environ);
    if (status != 0)
    {
        std::string cmd = argv[0];
        for (int i = 1; argv[i] != nullptr; ++i)
        {
            cmd += " ";
            cmd += argv[i];
        }
        Logger::Log::Warning("Platform: Failed to spawn '{}': error {}", cmd, status);
        return false;
    }

    ReapDetachedProcess(pid);
    return true;
}

#if !defined(__APPLE__)

std::string ShellQuote(const std::string& s)
{
    std::string result = "'";
    for (char c : s)
    {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }
    result += "'";
    return result;
}

enum class LinuxDialogTool
{
    None,
    Zenity,
    KDialog,
    Yad
};

LinuxDialogTool FindDialogTool()
{
    if (std::system("command -v zenity >/dev/null 2>&1") == 0)
        return LinuxDialogTool::Zenity;
    if (std::system("command -v kdialog >/dev/null 2>&1") == 0)
        return LinuxDialogTool::KDialog;
    if (std::system("command -v yad >/dev/null 2>&1") == 0)
        return LinuxDialogTool::Yad;
    return LinuxDialogTool::None;
}

std::string RunAndCapture(const std::string& command)
{
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
        return {};

    std::string output;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        output += buffer;

    int status = pclose(pipe);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {};

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();

    return output;
}

std::string ResolveInitialDir(const std::filesystem::path& initialPath)
{
    if (initialPath.empty())
        return {};

    std::error_code ec;
    if (std::filesystem::is_directory(initialPath, ec))
        return initialPath.string();
    if (std::filesystem::exists(initialPath, ec))
        return initialPath.parent_path().string();
    return {};
}

std::string ResolveInitialSavePath(const std::filesystem::path& initialPath)
{
    if (initialPath.empty())
        return {};

    std::error_code ec;
    if (std::filesystem::is_directory(initialPath, ec))
        return initialPath.string();

    const auto parent = initialPath.parent_path();
    if (!parent.empty() && std::filesystem::exists(parent, ec))
        return initialPath.string();

    const auto filename = initialPath.filename();
    return filename.empty() ? std::string{} : filename.string();
}

#endif // !defined(__APPLE__)

#else

enum class ScriptEditorKind
{
    Unknown,
    VisualStudio,
    VSCode,
    Rider
};

// Query the executable associated with a given file extension (e.g. L".csproj").
// Returns an empty string on failure.
std::wstring GetAssociatedExecutable(const wchar_t* extension)
{
    if (!extension || *extension == L'\0')
    {
        return std::wstring();
    }

    WCHAR buffer[1024] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(WCHAR));
    HRESULT hr = AssocQueryStringW(ASSOCF_NONE,
                                   ASSOCSTR_EXECUTABLE,
                                   extension,
                                   nullptr,
                                   buffer,
                                   &size);
    if (FAILED(hr) || size == 0 || buffer[0] == L'\0')
    {
        return std::wstring();
    }

    return std::wstring(buffer);
}

ScriptEditorKind ClassifyScriptEditorExecutable(const std::wstring& exePath)
{
    if (exePath.empty())
    {
        return ScriptEditorKind::Unknown;
    }

    std::wstring lower = exePath;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c)
                   { return static_cast<wchar_t>(std::towlower(c)); });

    if (lower.find(L"devenv.exe") != std::wstring::npos ||
        lower.find(L"vslauncher.exe") != std::wstring::npos)
    {
        return ScriptEditorKind::VisualStudio;
    }

    if (lower.find(L"code.exe") != std::wstring::npos ||
        lower.find(L"code.cmd") != std::wstring::npos)
    {
        return ScriptEditorKind::VSCode;
    }

    if (lower.find(L"rider64.exe") != std::wstring::npos ||
        lower.find(L"rider.exe") != std::wstring::npos)
    {
        return ScriptEditorKind::Rider;
    }

    return ScriptEditorKind::Unknown;
}

bool LaunchEditorProcess(const std::wstring& exe,
                         const std::wstring& parameters,
                         const std::filesystem::path& workingDirectory)
{
    if (exe.empty())
    {
        return false;
    }

    std::wstring workingDirW;
    const wchar_t* workingDirPtr = nullptr;
    if (!workingDirectory.empty())
    {
        workingDirW = workingDirectory.wstring();
        workingDirPtr = workingDirW.c_str();
    }

    HINSTANCE res = ::ShellExecuteW(nullptr,
                                    L"open",
                                    exe.c_str(),
                                    parameters.empty() ? nullptr : parameters.c_str(),
                                    workingDirPtr,
                                    SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(res) > 32;
}

#endif // !defined(_WIN32)

} // namespace

bool OpenUrl(const std::string& url)
{
    if (url.empty())
        return false;

#if defined(_WIN32)
    const std::wstring wUrl(url.begin(), url.end());
    HINSTANCE res = ::ShellExecuteW(nullptr, L"open", wUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(res) > 32;
#elif defined(__APPLE__)
    const char* argv[] = {"open", url.c_str(), nullptr};
    return SpawnDetached(argv);
#else
    const char* argv[] = {"xdg-open", url.c_str(), nullptr};
    return SpawnDetached(argv);
#endif
}

bool OpenPath(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

#if defined(_WIN32)
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return false;
    }

    const std::wstring wPath = path.wstring();
    HINSTANCE res = ::ShellExecuteW(nullptr,
                                    L"open",
                                    wPath.c_str(),
                                    nullptr,
                                    nullptr,
                                    SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(res) > 32;
#elif defined(__APPLE__)
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return false;
    }

    const std::string utf8 = path.string();
    const char* argv[] = {"open", utf8.c_str(), nullptr};
    return SpawnDetached(argv);
#else
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return false;
    }

    const std::string utf8 = path.string();
    const char* argv[] = {"xdg-open", utf8.c_str(), nullptr};
    return SpawnDetached(argv);
#endif
}

bool ShowInFileManager(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    std::error_code ec;

#if defined(_WIN32)
    if (!std::filesystem::exists(path, ec))
    {
        return false;
    }

    const bool isDirectory = std::filesystem::is_directory(path, ec);
    if (!isDirectory)
    {
        // Select the file in Explorer.
        const std::wstring param = L"/select,\"" + path.wstring() + L"\"";
        HINSTANCE res = ::ShellExecuteW(nullptr,
                                        nullptr,
                                        L"explorer.exe",
                                        param.c_str(),
                                        nullptr,
                                        SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(res) > 32;
    }

    // Directory case: open the directory itself.
    {
        const std::wstring folder = path.wstring();
        HINSTANCE res = ::ShellExecuteW(nullptr,
                                        L"open",
                                        folder.c_str(),
                                        nullptr,
                                        nullptr,
                                        SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(res) > 32;
    }

#elif defined(__APPLE__)
    if (!std::filesystem::exists(path, ec))
    {
        return false;
    }

    // Ensure the path is absolute and normalized for macOS
    std::filesystem::path absPath = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    if (ec)
    {
        return false;
    }
    
    // Normalize the path (resolve . and .., remove redundant separators)
    absPath = absPath.lexically_normal();
    
    // Convert to string - use native() to get the proper format for the OS
    // On macOS this gives us forward slashes which is what 'open' expects
    const std::string utf8 = absPath.native();
    const bool isDirectory = std::filesystem::is_directory(absPath, ec);

    if (!isDirectory)
    {
        // Reveal the file in Finder using full path to 'open' command
        const char* argv[] = {"/usr/bin/open", "-R", utf8.c_str(), nullptr};
        return SpawnDetached(argv);
    }

    // Directory case: open the directory itself
    {
        const char* argv[] = {"/usr/bin/open", utf8.c_str(), nullptr};
        return SpawnDetached(argv);
    }

#else
    // On Linux and other Unix-like systems, fall back to opening the
    // containing folder (or the path itself if already a directory).
    std::filesystem::path folder = path;
    if (!std::filesystem::is_directory(folder, ec))
    {
        folder = path.parent_path();
    }

    if (folder.empty() || !std::filesystem::exists(folder, ec))
    {
        return false;
    }

    const std::string utf8 = folder.string();
    const char* argv[] = {"xdg-open", utf8.c_str(), nullptr};
    return SpawnDetached(argv);
#endif
}

bool LaunchDetached(const std::filesystem::path& executable,
                    const std::vector<std::string>& arguments,
                    const std::filesystem::path& workingDirectory)
{
    if (executable.empty())
        return false;

#if defined(_WIN32)
    std::wstring parameters;
    auto quote = [](const std::wstring& value) {
        std::wstring out = L"\"";
        for (wchar_t c : value)
        {
            if (c == L'"')
                out += L"\\\"";
            else
                out += c;
        }
        out += L"\"";
        return out;
    };
    for (const std::string& arg : arguments)
    {
        if (!parameters.empty())
            parameters += L" ";
        parameters += quote(std::filesystem::path(arg).wstring());
    }

    const std::wstring exeW = executable.wstring();
    const std::wstring cwdW = workingDirectory.empty() ? std::wstring() : workingDirectory.wstring();
    HINSTANCE res = ::ShellExecuteW(nullptr,
                                    L"open",
                                    exeW.c_str(),
                                    parameters.empty() ? nullptr : parameters.c_str(),
                                    cwdW.empty() ? nullptr : cwdW.c_str(),
                                    SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(res) > 32;
#else
    std::error_code ec;
    const std::filesystem::path absExe = executable.is_absolute() ? executable : std::filesystem::absolute(executable, ec);
    const std::string exeUtf8 = ec ? executable.string() : absExe.string();

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1u);
    storage.push_back(exeUtf8);
    for (const auto& arg : arguments)
        storage.push_back(arg);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1u);
    for (auto& arg : storage)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    posix_spawnattr_setflags(&attr, flags);
#else
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#endif

    std::string cwdUtf8;
    if (!workingDirectory.empty())
    {
        cwdUtf8 = workingDirectory.string();
        posix_spawn_file_actions_addchdir_np(&actions, cwdUtf8.c_str());
    }

    pid_t pid = 0;
    const int status = posix_spawn(&pid, exeUtf8.c_str(), &actions, &attr, argv.data(), environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    if (status != 0)
    {
        Logger::Log::Warning("Platform: Failed to launch '{}': {}", exeUtf8, status);
        return false;
    }

    ReapDetachedProcess(pid);
    return true;
#endif
}

int LaunchDetachedGetPid(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments,
                         const std::filesystem::path& workingDirectory)
{
#if defined(_WIN32)
    LaunchDetached(executable, arguments, workingDirectory);
    return 0;
#else
    std::error_code ec;
    const std::filesystem::path absExe = executable.is_absolute() ? executable : std::filesystem::absolute(executable, ec);
    const std::string exeUtf8 = ec ? executable.string() : absExe.string();

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1u);
    storage.push_back(exeUtf8);
    for (const auto& arg : arguments)
    {
        storage.push_back(arg);
    }

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1u);
    for (auto& arg : storage)
    {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    std::string cwdUtf8;
    if (!workingDirectory.empty())
    {
        cwdUtf8 = workingDirectory.string();
        // Set the child's working directory without touching the parent's CWD.
        posix_spawn_file_actions_addchdir_np(&actions, cwdUtf8.c_str());
    }

#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    posix_spawnattr_setflags(&attr, flags);
#else
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#endif

    pid_t pid = 0;
    const int status = posix_spawn(&pid, exeUtf8.c_str(), &actions, &attr, argv.data(), environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    if (status != 0)
    {
        Logger::Log::Warning("Platform: Failed to launch '{}': {}", exeUtf8, status);
        return 0;
    }

    return static_cast<int>(pid);
#endif
}

bool IsProcessRunning(int pid)
{
    if (pid <= 0)
    {
        return false;
    }
#if defined(_WIN32)
    // An exited process stays openable while anyone holds a handle to it; its
    // handle is signaled from the moment it exits.
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process)
    {
        // Denied means the process exists and belongs to another user or session.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    // kill(pid, 0) probes the process without sending a signal: 0 when it exists and may be
    // signalled, EPERM when it exists and belongs to another user.
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

#if defined(_WIN32)
// True if a process with the given executable name (e.g. L"devenv.exe") is currently running.
static bool IsProcessRunning(const wchar_t* exeName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (lstrcmpiW(entry.szExeFile, exeName) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

// Run a command line and capture its stdout, with no console window. Used to query vswhere.
static std::string CaptureProcessStdout(const std::wstring& commandLine)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = commandLine; // CreateProcessW may write to its command-line buffer.
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok)
    {
        CloseHandle(readPipe);
        return {};
    }

    std::string out;
    char buf[512];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0)
        out.append(buf, bytesRead);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

// devenv.exe of the newest installed Visual Studio (including previews), via vswhere. Cached for the
// process; empty if vswhere or VS is not found. Lets us prefer e.g. VS 2026 over a 2022 that merely
// owns the file association.
static const std::wstring& LatestVisualStudioDevenv()
{
    static const std::wstring cached = []() -> std::wstring {
        wchar_t programFilesX86[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programFilesX86, MAX_PATH) == 0)
            return {};
        std::error_code ec;
        const std::filesystem::path vswhere =
            std::filesystem::path(programFilesX86) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
        if (!std::filesystem::exists(vswhere, ec))
            return {};
        const std::wstring cmd =
            L"\"" + vswhere.wstring() + L"\" -latest -prerelease -property productPath";
        std::string out = CaptureProcessStdout(cmd);
        out.erase(out.find_last_not_of(" \t\r\n") + 1);
        if (out.empty())
            return {};
        const std::filesystem::path devenv(out);
        if (!std::filesystem::exists(devenv, ec))
            return {};
        return devenv.wstring();
    }();
    return cached;
}

// If the resolved editor is Visual Studio, upgrade it to the newest installed VS (e.g. 2026) rather
// than whatever version owns the file association (often an older 2022).
static void PreferLatestVisualStudio(ScriptEditorKind kind, std::wstring& editorExe)
{
    if (kind == ScriptEditorKind::VisualStudio)
        if (const std::wstring& latest = LatestVisualStudioDevenv(); !latest.empty())
            editorExe = latest;
}
#endif

bool OpenScriptWithProject(const std::filesystem::path& scriptPath,
                           const std::filesystem::path& projectPath)
{
    if (scriptPath.empty())
    {
        return false;
    }

#if defined(_WIN32)
    std::error_code ec;
    if (!std::filesystem::exists(scriptPath, ec))
    {
        return false;
    }

    // If no project is provided or it does not exist, fall back to simple
    // shell open for the script file.
    if (projectPath.empty() || !std::filesystem::exists(projectPath, ec))
    {
        return OpenPath(scriptPath);
    }

    // Try associated executable in priority order: .csproj, .sln, .cs
    std::wstring editorExe = GetAssociatedExecutable(L".csproj");
    if (editorExe.empty())
    {
        editorExe = GetAssociatedExecutable(L".sln");
    }
    if (editorExe.empty())
    {
        editorExe = GetAssociatedExecutable(L".cs");
    }

    if (editorExe.empty())
    {
        // Unknown association: just open the script normally.
        return OpenPath(scriptPath);
    }

    ScriptEditorKind kind = ClassifyScriptEditorExecutable(editorExe);
    PreferLatestVisualStudio(kind, editorExe);
    if (kind == ScriptEditorKind::Unknown)
    {
        // Some other editor we don't know how to pass both project + file to;
        // delegate back to the OS for the script file only.
        return OpenPath(scriptPath);
    }

    const std::filesystem::path projectDir = projectPath.parent_path();
    const std::filesystem::path scriptDir = scriptPath.parent_path();
    const std::wstring projectW = projectPath.wstring();
    const std::wstring scriptW = scriptPath.wstring();
    const std::filesystem::path workingDir = !projectDir.empty() ? projectDir : scriptDir;

    std::wstring params;
    switch (kind)
    {
    case ScriptEditorKind::VisualStudio:
        // devenv.exe "<project>" "<file>"
        //
        // Using /Edit causes Visual Studio to open the file as a loose
        // document outside the solution, which means it is not associated
        // with the auto-generated GameEngine.Scripts.csproj. Passing the
        // project first and the file second opens the project/solution and
        // then opens the file within that context.
        params = L"\"" + projectW + L"\" \"" + scriptW + L"\"";
        break;

    case ScriptEditorKind::VSCode:
    {
        // code "<projectDir>" --goto "<file>:1:1"
        const std::wstring projectDirW = (!projectDir.empty() ? projectDir : scriptDir).wstring();
        params = L"\"" + projectDirW + L"\" --goto \"" + scriptW + L":1:1\"";
        break;
    }

    case ScriptEditorKind::Rider:
        // rider64.exe "<project>" --line 1 "<file>"
        params = L"\"" + projectW + L"\" --line 1 \"" + scriptW + L"\"";
        break;

    default:
        return OpenPath(scriptPath);
    }

    if (!LaunchEditorProcess(editorExe, params, workingDir))
    {
        // Best-effort: if the specialised launch fails, fall back to the
        // default shell open for the script file.
        return OpenPath(scriptPath);
    }

    return true;
#else
    // For non-Windows platforms we currently don't try to detect editor CLI
    // arguments. Fall back to the standard shell open behaviour for scripts.
    (void)projectPath;
    return OpenPath(scriptPath);
#endif
}

bool OpenSourceWithProject(const std::filesystem::path& sourcePath,
                           const std::filesystem::path& projectDir)
{
    if (sourcePath.empty())
    {
        return false;
    }

#if defined(_WIN32)
    std::error_code ec;
    if (!std::filesystem::exists(sourcePath, ec))
    {
        return false;
    }

    // Find the C/C++ IDE. Prefer native project associations, then the source file's own type,
    // then C# (the same IDE usually handles both).
    std::wstring editorExe = GetAssociatedExecutable(L".sln");
    if (editorExe.empty())
        editorExe = GetAssociatedExecutable(L".vcxproj");
    if (editorExe.empty())
        editorExe = GetAssociatedExecutable(sourcePath.extension().wstring().c_str());
    if (editorExe.empty())
        editorExe = GetAssociatedExecutable(L".cpp");
    if (editorExe.empty())
        editorExe = GetAssociatedExecutable(L".csproj");

    if (editorExe.empty())
    {
        return OpenPath(sourcePath);
    }

    const ScriptEditorKind kind = ClassifyScriptEditorExecutable(editorExe);
    PreferLatestVisualStudio(kind, editorExe);
    const std::wstring fileW = sourcePath.wstring();
    const std::filesystem::path folder = !projectDir.empty() ? projectDir : sourcePath.parent_path();
    const std::wstring folderW = folder.wstring();

    std::wstring params;
    switch (kind)
    {
    case ScriptEditorKind::VisualStudio:
        // VS can't open-folder + focus-file + reuse a window in one CLI call, so pick by state:
        //  - cold start (no VS running): open the project FOLDER and the file together — one new
        //    instance with the CMake Solution Explorer tree + IntelliSense set up.
        //  - VS already running: /Edit reuses that window and focuses the file (the cold open above
        //    loaded the folder, so the file lands in that workspace). Avoids a new window per open.
        if (IsProcessRunning(L"devenv.exe"))
            params = L"/Edit \"" + fileW + L"\"";
        else
            params = L"\"" + folderW + L"\" \"" + fileW + L"\"";
        break;

    case ScriptEditorKind::VSCode:
        // code -r "<folder>" --goto "<file>:1:1"  (reuse the window, open the folder, jump to file)
        params = L"-r \"" + folderW + L"\" --goto \"" + fileW + L":1:1\"";
        break;

    case ScriptEditorKind::Rider:
        // Rider reuses its running instance by default; open the file at line 1.
        params = L"--line 1 \"" + fileW + L"\"";
        break;

    default:
        return OpenPath(sourcePath);
    }

    if (!LaunchEditorProcess(editorExe, params, folder))
    {
        return OpenPath(sourcePath);
    }

    return true;
#else
    (void)projectDir;
    return OpenPath(sourcePath);
#endif
}

bool SupportsFolderPicker()
{
    return true;
}

std::filesystem::path SelectFolder(const std::filesystem::path& initialPath)
{
#if defined(_WIN32)
    // Use IFileDialog for folder selection on Windows
    std::filesystem::path result;
    
    // Initialize COM if needed
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInitialized = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE)
    {
        // COM already initialized with different mode, continue anyway
        comInitialized = false;
    }
    
    IFileDialog* pfd = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&pfd));
    
    if (SUCCEEDED(hr))
    {
        // Set options to select folders only and allow creating new folders
        DWORD dwOptions;
        hr = pfd->GetOptions(&dwOptions);
        if (SUCCEEDED(hr))
        {
            DWORD options = dwOptions | FOS_PICKFOLDERS;
#if defined(FOS_ALLOWCREATEPROMPT)
            options |= FOS_ALLOWCREATEPROMPT;
#endif
            hr = pfd->SetOptions(options);
        }
        
        // Set initial folder if provided
        if (SUCCEEDED(hr) && !initialPath.empty())
        {
            std::error_code ec;
            std::filesystem::path pathToUse = initialPath;
            if (std::filesystem::is_directory(pathToUse, ec))
            {
                // Use the directory directly
            }
            else if (std::filesystem::exists(pathToUse, ec))
            {
                // Use parent directory
                pathToUse = pathToUse.parent_path();
            }
            else
            {
                // Path doesn't exist, try to use parent if it exists
                pathToUse = pathToUse.parent_path();
                if (!std::filesystem::exists(pathToUse, ec))
                {
                    pathToUse.clear();
                }
            }
            
            if (!pathToUse.empty())
            {
                IShellItem* psi = nullptr;
                const std::wstring wPath = pathToUse.wstring();
                hr = SHCreateItemFromParsingName(wPath.c_str(), nullptr, IID_PPV_ARGS(&psi));
                if (SUCCEEDED(hr))
                {
                    pfd->SetFolder(psi);
                    psi->Release();
                }
            }
        }
        
        // Show the dialog. Pass an owner HWND so the dialog stays modal
        // and on top of the editor window — without this the dialog can
        // disappear behind the editor and the main thread hangs in Show().
        if (SUCCEEDED(hr))
        {
            HWND ownerHwnd = ResolveDialogOwnerHwnd();
            PrepareDialogForeground(ownerHwnd);
            DialogPromoteContext promoteCtx;
            promoteCtx.pid = GetCurrentProcessId();
            promoteCtx.ownerHwnd = ownerHwnd;
            HANDLE promoteThread = CreateThread(nullptr, 0,
                                                PromoteDialogToForegroundThread,
                                                &promoteCtx, 0, nullptr);
            hr = pfd->Show(ownerHwnd);
            promoteCtx.stop.store(true);
            if (promoteThread)
            {
                WaitForSingleObject(promoteThread, 1000);
                CloseHandle(promoteThread);
            }
            if (SUCCEEDED(hr))
            {
                IShellItem* psi = nullptr;
                hr = pfd->GetResult(&psi);
                if (SUCCEEDED(hr))
                {
                    PWSTR pszPath = nullptr;
                    hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                    if (SUCCEEDED(hr))
                    {
                        result = std::filesystem::path(pszPath);
                        CoTaskMemFree(pszPath);
                    }
                    psi->Release();
                }
            }
        }
        
        pfd->Release();
    }
    
    if (comInitialized)
    {
        CoUninitialize();
    }
    
    return result;
    
#elif defined(__APPLE__)
    // macOS implementation is in Shell_mac.mm
    // Forward declaration to avoid including Objective-C in C++ file
    extern std::filesystem::path SelectFolder_macOS(const std::filesystem::path& initialPath);
    return SelectFolder_macOS(initialPath);
    
#else
    std::string initialDir = ResolveInitialDir(initialPath);
    LinuxDialogTool tool = FindDialogTool();
    std::string output;

    switch (tool)
    {
    case LinuxDialogTool::Zenity:
    {
        std::string cmd = "zenity --file-selection --directory --title='Select Project Folder'";
        if (!initialDir.empty())
            cmd += " --filename=" + ShellQuote(initialDir + "/");
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::KDialog:
    {
        std::string cmd = "kdialog --getexistingdirectory";
        cmd += " " + ShellQuote(initialDir.empty() ? "." : initialDir);
        cmd += " --title 'Select Project Folder' 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::Yad:
    {
        std::string cmd = "yad --file --directory --title='Select Project Folder'";
        if (!initialDir.empty())
            cmd += " --filename=" + ShellQuote(initialDir + "/");
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::None:
        break;
    }

    if (!output.empty())
        return std::filesystem::path(output);
    return {};
#endif
}

namespace
{

#if defined(_WIN32)
std::vector<std::filesystem::path> OpenFileDialogWin32(const std::filesystem::path& initialPath,
                                                       const char* filterName,
                                                       const char* filterPattern,
                                                       bool allowMultiple)
{
    std::vector<std::filesystem::path> result;

    // IFileOpenDialog requires STA — if the calling thread was already
    // initialized in MTA (e.g. .NET CoreCLR scripting host), CoInitializeEx
    // returns RPC_E_CHANGED_MODE and the dialog can hang or render off-screen.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInitialized = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE)
    {
        Logger::Log::Warning("Shell::SelectFiles: COM already in MTA on this thread — dialog may misbehave");
        comInitialized = false;
    }

    IFileOpenDialog* pfd = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&pfd));
    Logger::Log::Info("Shell::SelectFiles: CoCreateInstance hr=0x{:x}", static_cast<unsigned>(hr));

    if (SUCCEEDED(hr))
    {
        DWORD dwOptions = 0;
        hr = pfd->GetOptions(&dwOptions);
        if (SUCCEEDED(hr))
        {
            DWORD options = dwOptions | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
            if (allowMultiple)
                options |= FOS_ALLOWMULTISELECT;
            hr = pfd->SetOptions(options);
        }

        std::wstring wFilterName;
        std::wstring wFilterPattern;
        COMDLG_FILTERSPEC spec{};
        if (SUCCEEDED(hr) && filterName && *filterName && filterPattern && *filterPattern)
        {
            auto widen = [](const char* s) -> std::wstring
            {
                if (!s || !*s)
                    return {};
                const int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
                if (wlen <= 0)
                    return {};
                std::wstring out;
                out.resize((size_t)wlen - 1);
                MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), wlen);
                return out;
            };
            wFilterName = widen(filterName);
            wFilterPattern = widen(filterPattern);
            if (!wFilterName.empty() && !wFilterPattern.empty())
            {
                spec.pszName = wFilterName.c_str();
                spec.pszSpec = wFilterPattern.c_str();
                (void)pfd->SetFileTypes(1, &spec);
                (void)pfd->SetFileTypeIndex(1);
            }
        }

        if (SUCCEEDED(hr) && !initialPath.empty())
        {
            std::error_code ec;
            std::filesystem::path pathToUse = initialPath;
            if (std::filesystem::is_directory(pathToUse, ec))
            {
                // Use directory directly.
            }
            else if (std::filesystem::exists(pathToUse, ec))
            {
                pathToUse = pathToUse.parent_path();
            }
            else
            {
                pathToUse = pathToUse.parent_path();
                if (!std::filesystem::exists(pathToUse, ec))
                    pathToUse.clear();
            }

            if (!pathToUse.empty())
            {
                IShellItem* psi = nullptr;
                const std::wstring wPath = pathToUse.wstring();
                hr = SHCreateItemFromParsingName(wPath.c_str(), nullptr, IID_PPV_ARGS(&psi));
                if (SUCCEEDED(hr))
                {
                    (void)pfd->SetFolder(psi);
                    psi->Release();
                }
            }
        }

        if (SUCCEEDED(hr))
        {
            HWND ownerHwnd = ResolveDialogOwnerHwnd();
            PrepareDialogForeground(ownerHwnd);
            DialogPromoteContext promoteCtx;
            promoteCtx.pid = GetCurrentProcessId();
            promoteCtx.ownerHwnd = ownerHwnd;
            HANDLE promoteThread = CreateThread(nullptr, 0,
                                                PromoteDialogToForegroundThread,
                                                &promoteCtx, 0, nullptr);
            hr = pfd->Show(ownerHwnd);
            promoteCtx.stop.store(true);
            if (promoteThread)
            {
                WaitForSingleObject(promoteThread, 1000);
                CloseHandle(promoteThread);
            }
            if (SUCCEEDED(hr))
            {
                IShellItemArray* items = nullptr;
                hr = pfd->GetResults(&items);
                if (SUCCEEDED(hr) && items)
                {
                    DWORD count = 0;
                    items->GetCount(&count);
                    for (DWORD i = 0; i < count; ++i)
                    {
                        IShellItem* item = nullptr;
                        if (FAILED(items->GetItemAt(i, &item)) || !item)
                            continue;
                        PWSTR pszPath = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
                        {
                            result.emplace_back(pszPath);
                            CoTaskMemFree(pszPath);
                        }
                        item->Release();
                    }
                    items->Release();
                }
            }
        }

        pfd->Release();
    }

    if (comInitialized)
        CoUninitialize();

    return result;
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
std::vector<std::filesystem::path> OpenFileDialogLinux(const std::filesystem::path& initialPath,
                                                       const char* filterName,
                                                       const char* filterPattern,
                                                       bool allowMultiple)
{
    std::string initialDir = ResolveInitialDir(initialPath);
    LinuxDialogTool tool = FindDialogTool();
    std::string output;

    switch (tool)
    {
    case LinuxDialogTool::Zenity:
    {
        std::string cmd = "zenity --file-selection --title='Select File'";
        if (allowMultiple)
            cmd += " --multiple --separator=" + ShellQuote("\n");
        if (!initialDir.empty())
            cmd += " --filename=" + ShellQuote(initialDir + "/");
        if (filterName && *filterName && filterPattern && *filterPattern)
            cmd += " --file-filter=" + ShellQuote(std::string(filterName) + " | " + filterPattern);
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::KDialog:
    {
        std::string cmd = "kdialog --getopenfilename";
        cmd += " " + ShellQuote(initialDir.empty() ? "." : initialDir);
        if (filterName && *filterName && filterPattern && *filterPattern)
            cmd += " " + ShellQuote(std::string(filterName) + " (" + filterPattern + ")");
        if (allowMultiple)
            cmd += " --multiple --separate-output";
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::Yad:
    {
        std::string cmd = "yad --file --title='Select File'";
        if (allowMultiple)
            cmd += " --multiple --separator=" + ShellQuote("\n");
        if (!initialDir.empty())
            cmd += " --filename=" + ShellQuote(initialDir + "/");
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::None:
        break;
    }

    if (output.empty())
        return {};
    if (!allowMultiple)
        return { std::filesystem::path(output) };
    return SplitLinuxDialogPaths(output, '\n');
}
#endif

} // namespace

std::filesystem::path SelectFile(const std::filesystem::path& initialPath,
                                 const char* filterName,
                                 const char* filterPattern)
{
#if defined(_WIN32)
    const auto picked = OpenFileDialogWin32(initialPath, filterName, filterPattern, false);
    return picked.empty() ? std::filesystem::path{} : picked.front();
#elif defined(__APPLE__)
    extern std::filesystem::path SelectFile_macOS(const std::filesystem::path& initialPath,
                                                  const char* filterName,
                                                  const char* filterPattern);
    return SelectFile_macOS(initialPath, filterName, filterPattern);
#else
    const auto picked = OpenFileDialogLinux(initialPath, filterName, filterPattern, false);
    return picked.empty() ? std::filesystem::path{} : picked.front();
#endif
}

std::vector<std::filesystem::path> SelectFiles(const std::filesystem::path& initialPath,
                                               const char* filterName,
                                               const char* filterPattern)
{
#if defined(_WIN32)
    return OpenFileDialogWin32(initialPath, filterName, filterPattern, true);
#elif defined(__APPLE__)
    extern std::vector<std::filesystem::path> SelectFiles_macOS(const std::filesystem::path& initialPath,
                                                                const char* filterName,
                                                                const char* filterPattern);
    return SelectFiles_macOS(initialPath, filterName, filterPattern);
#else
    return OpenFileDialogLinux(initialPath, filterName, filterPattern, true);
#endif
}

std::filesystem::path SaveFile(const std::filesystem::path& initialPath,
                               const char* filterName,
                               const char* filterPattern)
{
#if defined(_WIN32)
    std::filesystem::path result;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInitialized = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE)
        comInitialized = false;

    IFileDialog* pfd = nullptr;
    hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&pfd));

    if (SUCCEEDED(hr))
    {
        DWORD dwOptions = 0;
        hr = pfd->GetOptions(&dwOptions);
        if (SUCCEEDED(hr))
        {
            DWORD options = dwOptions | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST;
            hr = pfd->SetOptions(options);
        }

        // Best-effort filter.
        std::wstring wFilterName;
        std::wstring wFilterPattern;
        COMDLG_FILTERSPEC spec{};
        if (SUCCEEDED(hr) && filterName && *filterName && filterPattern && *filterPattern)
        {
            auto widen = [](const char* s) -> std::wstring
            {
                if (!s || !*s)
                    return {};
                const int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
                if (wlen <= 0)
                    return {};
                std::wstring out;
                out.resize((size_t)wlen - 1);
                MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), wlen);
                return out;
            };
            wFilterName = widen(filterName);
            wFilterPattern = widen(filterPattern);
            if (!wFilterName.empty() && !wFilterPattern.empty())
            {
                spec.pszName = wFilterName.c_str();
                spec.pszSpec = wFilterPattern.c_str();
                (void)pfd->SetFileTypes(1, &spec);
                (void)pfd->SetFileTypeIndex(1);
            }
        }

        if (SUCCEEDED(hr) && !initialPath.empty())
        {
            std::error_code ec;
            std::filesystem::path pathToUse = initialPath;
            std::filesystem::path filenameToUse;
            if (std::filesystem::is_directory(pathToUse, ec))
            {
                // Use directory directly.
            }
            else if (std::filesystem::exists(pathToUse, ec))
            {
                filenameToUse = pathToUse.filename();
                pathToUse = pathToUse.parent_path();
            }
            else
            {
                filenameToUse = pathToUse.filename();
                pathToUse = pathToUse.parent_path();
                if (!std::filesystem::exists(pathToUse, ec))
                    pathToUse.clear();
            }

            if (!pathToUse.empty())
            {
                IShellItem* psi = nullptr;
                const std::wstring wPath = pathToUse.wstring();
                hr = SHCreateItemFromParsingName(wPath.c_str(), nullptr, IID_PPV_ARGS(&psi));
                if (SUCCEEDED(hr))
                {
                    (void)pfd->SetFolder(psi);
                    psi->Release();
                }
            }

            if (!filenameToUse.empty())
                (void)pfd->SetFileName(filenameToUse.wstring().c_str());
        }

        if (SUCCEEDED(hr))
        {
            HWND ownerHwnd = ResolveDialogOwnerHwnd();
            PrepareDialogForeground(ownerHwnd);
            DialogPromoteContext promoteCtx;
            promoteCtx.pid = GetCurrentProcessId();
            promoteCtx.ownerHwnd = ownerHwnd;
            HANDLE promoteThread = CreateThread(nullptr, 0,
                                                PromoteDialogToForegroundThread,
                                                &promoteCtx, 0, nullptr);
            hr = pfd->Show(ownerHwnd);
            promoteCtx.stop.store(true);
            if (promoteThread)
            {
                WaitForSingleObject(promoteThread, 1000);
                CloseHandle(promoteThread);
            }
            if (SUCCEEDED(hr))
            {
                IShellItem* psi = nullptr;
                hr = pfd->GetResult(&psi);
                if (SUCCEEDED(hr))
                {
                    PWSTR pszPath = nullptr;
                    hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                    if (SUCCEEDED(hr))
                    {
                        result = std::filesystem::path(pszPath);
                        CoTaskMemFree(pszPath);
                    }
                    psi->Release();
                }
            }
        }

        pfd->Release();
    }

    if (comInitialized)
        CoUninitialize();

    return result;
#elif defined(__APPLE__)
    extern std::filesystem::path SaveFile_macOS(const std::filesystem::path& initialPath,
                                                const char* filterName,
                                                const char* filterPattern);
    return SaveFile_macOS(initialPath, filterName, filterPattern);
#else
    std::string initialDir = ResolveInitialDir(initialPath);
    std::string initialSavePath = ResolveInitialSavePath(initialPath);
    std::error_code initialPathEc;
    const bool initialPathIsDirectory = !initialPath.empty() && std::filesystem::is_directory(initialPath, initialPathEc);
    LinuxDialogTool tool = FindDialogTool();
    std::string output;

    switch (tool)
    {
    case LinuxDialogTool::Zenity:
    {
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title='Save File'";
        if (!initialSavePath.empty())
            cmd += " --filename=" + ShellQuote(initialSavePath + (initialPathIsDirectory ? "/" : ""));
        if (filterName && *filterName && filterPattern && *filterPattern)
            cmd += " --file-filter=" + ShellQuote(std::string(filterName) + " | " + filterPattern);
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::KDialog:
    {
        std::string cmd = "kdialog --getsavefilename";
        cmd += " " + ShellQuote(initialSavePath.empty() ? (initialDir.empty() ? "." : initialDir) : initialSavePath);
        if (filterName && *filterName && filterPattern && *filterPattern)
            cmd += " " + ShellQuote(std::string(filterName) + " (" + filterPattern + ")");
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::Yad:
    {
        std::string cmd = "yad --file --save --title='Save File'";
        if (!initialSavePath.empty())
            cmd += " --filename=" + ShellQuote(initialSavePath + (initialPathIsDirectory ? "/" : ""));
        cmd += " 2>/dev/null";
        output = RunAndCapture(cmd);
        break;
    }
    case LinuxDialogTool::None:
        break;
    }

    if (!output.empty())
        return std::filesystem::path(output);
    return {};
#endif
}

int ShowNativeChoiceDialog(const std::string& title, const std::string& message,
                           const std::vector<std::string>& buttons, int defaultButton)
{
    if (buttons.empty())
        return defaultButton;
#if defined(_WIN32)
    HWND owner = ResolveDialogOwnerHwnd();

    // MessageBox only. TaskDialogIndirect would give custom button captions, but it
    // lives in comctl32 v6 (WinSxS) — statically importing it forces a v6 SxS
    // dependency the editor does not declare, so the whole process fails to load
    // (LdrpSnapModule -> hard error) before any code runs. MessageBox is in user32,
    // always present with no SxS, so it cannot fail to load in this device-loss
    // emergency path. The three choices are folded into the body text since
    // MessageBox button captions are fixed.
    std::wstring wTitle = Utf8ToWide(title);
    std::wstring wMessage = Utf8ToWide(message);
    std::vector<std::wstring> wButtons;
    wButtons.reserve(buttons.size());
    for (const auto& b : buttons)
        wButtons.push_back(Utf8ToWide(b));

    const UINT defFlag = defaultButton == 1 ? MB_DEFBUTTON2
                       : defaultButton == 2 ? MB_DEFBUTTON3
                                            : MB_DEFBUTTON1;
    const UINT type = MB_ICONERROR | MB_TASKMODAL | MB_TOPMOST | defFlag;

    std::wstring body = wMessage + L"\n\n";
    if (buttons.size() >= 3)
    {
        body += L"[Yes] " + wButtons[0] + L"    [No] " + wButtons[1] + L"    [Cancel] " + wButtons[2];
        const int r = MessageBoxW(owner, body.c_str(), wTitle.c_str(), type | MB_YESNOCANCEL);
        return r == IDYES ? 0 : (r == IDNO ? 1 : 2);
    }
    if (buttons.size() == 2)
    {
        body += L"[OK] " + wButtons[0] + L"    [Cancel] " + wButtons[1];
        const int r = MessageBoxW(owner, body.c_str(), wTitle.c_str(), type | MB_OKCANCEL);
        return r == IDOK ? 0 : 1;
    }
    MessageBoxW(owner, wMessage.c_str(), wTitle.c_str(), type | MB_OK);
    return 0;
#else
    // No portable native dialog off Windows (the slice-6 real-TDR target is Windows/NVIDIA).
    // Log the options and take the default so the caller's terminal handling proceeds.
    Logger::Log::Error("ShowNativeChoiceDialog (no native dialog on this platform): {} — {}", title, message);
    for (size_t i = 0; i < buttons.size(); ++i)
        Logger::Log::Error("  [{}] {}", i, buttons[i]);
    return defaultButton;
#endif
}

std::filesystem::path GetExecutablePath()
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    return std::filesystem::path(std::wstring(buf, n));
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    std::error_code ec;
    auto canonical = std::filesystem::canonical(buf, ec);
    return ec ? std::filesystem::path(buf) : canonical;
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : p;
#endif
}

bool MoveToTrash(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return false;

#if defined(_WIN32)
    // SHFileOperationW requires a double-null-terminated path list.
    std::wstring from = path.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&op);
    return result == 0 && !op.fAnyOperationsAborted;
#elif defined(__APPLE__)
    extern bool MoveToTrash_macOS(const std::filesystem::path& path);
    return MoveToTrash_macOS(path);
#else
    // Prefer gio (GLib) which implements the XDG trash spec.
    const std::string utf8 = path.string();
    pid_t pid = 0;
    const char* argv[] = {"gio", "trash", "--", utf8.c_str(), nullptr};
    if (posix_spawnp(&pid, "gio", nullptr, nullptr, const_cast<char**>(argv), environ) != 0)
        return false;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

void ReleaseTransientFiles(const std::vector<std::filesystem::path>& /*paths*/)
{
    // Picked and dropped paths are the user's own files.
}

} // namespace Platform
} // namespace GameEngine
