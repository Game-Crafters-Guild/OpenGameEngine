#include "Jobs/PipeTransport.h"
#include "Jobs/WideStringUtil.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstdlib>

#if defined(PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#include <chrono>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#endif

namespace GameEngine
{

// Shared helper to read integer tuning values from the environment
static int GetEnvInt(const char* k, int defv)
{
    const char* v = std::getenv(k);
    if (!v)
        return defv;
    try
    {
        return std::max(1, std::stoi(v));
    }
    catch (...)
    {
        return defv;
    }
}

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
// Compute the base directory for Unix domain sockets, honoring GE_PIPE_PATH and
// using XDG_RUNTIME_DIR (Linux) or TMPDIR (macOS) with sensible fallbacks.
static std::string GetPipeBaseDirectory()
{
    if (const char* overrideDir = std::getenv("GE_PIPE_PATH"))
    {
        if (overrideDir[0] != '\0')
            return std::string(overrideDir);
    }

#if defined(PLATFORM_LINUX)
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"))
    {
        if (xdg[0] != '\0')
        {
            std::string base(xdg);
            if (!base.empty() && base.back() != '/')
                base.push_back('/');
            base += "gameengine-compile-server";
            return base;
        }
    }

    // Fallback: per-user directory under /tmp
    uid_t uid = getuid();
    std::string base = "/tmp/gameengine-";
    base += std::to_string(static_cast<unsigned long long>(uid));
    base += "/compile_server";
    return base;
#elif defined(PLATFORM_MACOS)
    // NOTE: macOS enforces a strict ~104 byte limit on Unix domain socket paths.
    // TMPDIR is often a very long per-user path (e.g. /var/folders/...) which,
    // when combined with our pipe name, can exceed this limit and cause the
    // client to fail to connect.
    //
    // Prefer a short, stable base directory under /tmp. Callers can override
    // this via GE_PIPE_PATH if desired.
    return std::string("/tmp/gameengine-compile-server");
#endif
}
#endif // PLATFORM_LINUX || PLATFORM_MACOS

PipeTransport::PipeTransport()
{
#if defined(PLATFORM_WINDOWS)
    m_Handle = INVALID_HANDLE_VALUE;
    m_PipeName = L"\\\\.\\pipe\\GE_CompileServer_default";
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    m_SocketFd = -1;
    m_PipeNameUtf8 = "GE_CompileServer_default";
    m_SocketPath.clear();
#else
    // Unsupported platform; nothing to initialize.
#endif
}

PipeTransport::PipeTransport(const std::string& pipeNameUtf8)
{
#if defined(PLATFORM_WINDOWS)
    m_Handle = INVALID_HANDLE_VALUE;
    std::wstring pipeName = ToWide(pipeNameUtf8);
    if (pipeName.rfind(L"\\\\.\\pipe\\", 0) == 0)
        m_PipeName = pipeName;
    else
        m_PipeName = L"\\\\.\\pipe\\" + pipeName;
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    m_SocketFd = -1;
    m_PipeNameUtf8 = pipeNameUtf8;
    m_SocketPath.clear();
#else
    (void)pipeNameUtf8;
#endif
}

#if defined(PLATFORM_WINDOWS)
PipeTransport::PipeTransport(const std::wstring& pipeName)
{
    m_Handle = INVALID_HANDLE_VALUE;
    if (pipeName.rfind(L"\\\\.\\pipe\\", 0) == 0)
        m_PipeName = pipeName;
    else
        m_PipeName = L"\\\\.\\pipe\\" + pipeName;
}
#endif

PipeTransport::~PipeTransport()
{
    Close();
}

namespace
{
#if defined(PLATFORM_WINDOWS)
static bool ConnectWindowsImpl(void*& outHandle, const std::wstring& pipeName, int attempts, int waitMs, int sleepMs, bool logOnTimeout)
{
    const wchar_t* name = pipeName.empty() ? L"\\\\.\\pipe\\GE_CompileServer_default" : pipeName.c_str();

    DWORD firstErr = 0;
    for (int i = 0; i < attempts; ++i)
    {
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            outHandle = h;
            return true;
        }

        DWORD err = GetLastError();
        if (i == 0)
            firstErr = err;

        // Special-case: even for single-attempt probes, a busy pipe can become available
        // quickly. If we can wait briefly, retry CreateFile once after WaitNamedPipeW.
        if (err == ERROR_PIPE_BUSY && waitMs > 0)
        {
            if (WaitNamedPipeW(name, (DWORD)waitMs))
            {
                HANDLE h2 = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h2 != INVALID_HANDLE_VALUE)
                {
                    outHandle = h2;
                    return true;
                }
            }
        }

        if (i + 1 < attempts)
        {
            if (err != ERROR_PIPE_BUSY)
            {
                // For missing pipe / transient errors, sleep briefly to avoid busy-looping.
                if (sleepMs > 0)
                    Sleep((DWORD)sleepMs);
            }
        }
    }

    if (logOnTimeout)
        Logger::Log::Debug("[PipeTransport] Connect timeout (first error={})", (int)firstErr);
    return false;
}
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
static bool ConnectUnixImpl(int& outSocketFd,
                            std::string& inOutSocketPath,
                            const std::string& pipeNameUtf8,
                            int attempts,
                            int waitMs,
                            bool logOnTimeout)
{
    if (outSocketFd != -1)
    {
        ::close(outSocketFd);
        outSocketFd = -1;
    }

    std::string baseDir = GetPipeBaseDirectory();
    std::filesystem::path basePath(baseDir);
    std::error_code ec;
    std::filesystem::create_directories(basePath, ec);
    if (ec)
    {
        // This is primarily an infra convenience; failures shouldn't be fatal for a connect probe.
        Logger::Log::Debug("[PipeTransport] Failed to create socket directory '{}': {}", baseDir, ec.message());
    }

    std::filesystem::path socketPath = basePath / (pipeNameUtf8 + ".sock");
    inOutSocketPath = socketPath.string();

    if (inOutSocketPath.size() >= sizeof(sockaddr_un{}.sun_path))
    {
        const size_t maxLen = sizeof(sockaddr_un{}.sun_path) - 1; // reserve NUL terminator
        Logger::Log::Warning(
            "[PipeTransport] Socket path too long (len={} max={}): '{}'. "
            "Set GE_PIPE_PATH to a shorter directory (e.g. /tmp/gameengine-compile-server).",
            inOutSocketPath.size(),
            maxLen,
            inOutSocketPath);
        return false;
    }

    int lastErr = 0;
    for (int i = 0; i < attempts; ++i)
    {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == -1)
        {
            lastErr = errno;
            Logger::Log::Warning("[PipeTransport] socket(AF_UNIX) failed (errno={})", lastErr);
            break;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memset(addr.sun_path, 0, sizeof(addr.sun_path));
        std::memcpy(addr.sun_path, inOutSocketPath.c_str(), inOutSocketPath.size());

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
        {
            outSocketFd = fd;
            return true;
        }

        lastErr = errno;
        ::close(fd);

        if (i + 1 < attempts && waitMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        }
    }

    if (logOnTimeout)
        Logger::Log::Debug("[PipeTransport] Unix socket connect timeout (errno={})", lastErr);
    return false;
}
#endif
} // namespace

bool PipeTransport::Connect()
{
#if defined(PLATFORM_WINDOWS)
    const int kAttempts = GetEnvInt("GE_PIPE_CONNECT_ATTEMPTS", 50);
    const int kWaitMs = GetEnvInt("GE_PIPE_WAIT_MS", 200);
    // Preserve legacy behavior: bounded retry loop with small waits.
    Close();
    return ConnectWindowsImpl(m_Handle, m_PipeName, kAttempts, kWaitMs, /*sleepMs=*/100, /*logOnTimeout=*/true);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    const int kAttempts = GetEnvInt("GE_PIPE_CONNECT_ATTEMPTS", 50);
    const int kWaitMs = GetEnvInt("GE_PIPE_WAIT_MS", 200);
    return ConnectUnixImpl(m_SocketFd, m_SocketPath, m_PipeNameUtf8, kAttempts, kWaitMs, /*logOnTimeout=*/true);
#else
    return false;
#endif
}

bool PipeTransport::TryConnectOnce(int waitMs)
{
#if defined(PLATFORM_WINDOWS)
    Close();
    return ConnectWindowsImpl(m_Handle, m_PipeName, /*attempts=*/1, /*waitMs=*/waitMs, /*sleepMs=*/0, /*logOnTimeout=*/false);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    return ConnectUnixImpl(m_SocketFd, m_SocketPath, m_PipeNameUtf8, /*attempts=*/1, /*waitMs=*/std::max(0, waitMs), /*logOnTimeout=*/false);
#else
    (void)waitMs;
    return false;
#endif
}

bool PipeTransport::SendRequest(const std::string& requestJson, std::string& outResponse)
{
#if defined(PLATFORM_WINDOWS)
    if (m_Handle == INVALID_HANDLE_VALUE)
    {
        Logger::Log::Warning("[PipeTransport] SendRequest called with invalid handle");
        return false;
    }
    DWORD written = 0;
    if (!WriteFile((HANDLE)m_Handle, requestJson.data(), (DWORD)requestJson.size(), &written, nullptr))
    {
        DWORD err = GetLastError();
        Logger::Log::Warning("[PipeTransport] WriteFile(request) failed (err={})", (unsigned)err);
        return false;
    }
    char nl = '\n';
    if (!WriteFile((HANDLE)m_Handle, &nl, 1, &written, nullptr))
    {
        DWORD err = GetLastError();
        Logger::Log::Warning("[PipeTransport] WriteFile(newline) failed (err={})", (unsigned)err);
        return false;
    }

    // Read a single line response (up to a cap)
    char buffer[4096];
    DWORD read = 0;
    outResponse.clear();
    const int kRespTimeoutMs = GetEnvInt("GE_PIPE_RESPONSE_TIMEOUT_MS", 15000);
    const int kPollMs = GetEnvInt("GE_PIPE_RESPONSE_POLL_MS", 10);
    ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(kRespTimeoutMs);
    for (;;)
    {
        // Avoid indefinite blocking if the server wedges: poll for available bytes with a timeout.
        DWORD avail = 0;
        if (!PeekNamedPipe((HANDLE)m_Handle, nullptr, 0, nullptr, &avail, nullptr))
        {
            DWORD err = GetLastError();
            Logger::Log::Warning("[PipeTransport] PeekNamedPipe failed (err={})", (unsigned)err);
            break;
        }
        if (avail == 0)
        {
            if (GetTickCount64() >= deadline)
            {
                Logger::Log::Warning("[PipeTransport] Response timeout after {}ms", kRespTimeoutMs);
                break;
            }
            Sleep(static_cast<DWORD>(kPollMs));
            continue;
        }

        DWORD toRead = (DWORD)std::min<size_t>(sizeof(buffer), static_cast<size_t>(avail));
        if (!ReadFile((HANDLE)m_Handle, buffer, toRead, &read, nullptr))
        {
            DWORD err = GetLastError();
            Logger::Log::Warning("[PipeTransport] ReadFile failed (err={})", (unsigned)err);
            break;
        }
        if (read == 0)
            break;
        outResponse.append(buffer, buffer + read);
        if (!outResponse.empty() && outResponse.find('\n') != std::string::npos)
            break;
        if (outResponse.size() > 1'000'000)
        {
            Logger::Log::Warning("[PipeTransport] Response exceeded 1MB cap, truncating");
            break; // sanity
        }

        // We made progress; extend the deadline.
        deadline = GetTickCount64() + static_cast<ULONGLONG>(kRespTimeoutMs);
    }
    if (outResponse.empty())
    {
        Logger::Log::Warning("[PipeTransport] Empty response from server");
    }
    return !outResponse.empty();
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    if (m_SocketFd < 0)
    {
        Logger::Log::Warning("[PipeTransport] SendRequest called with invalid socket");
        return false;
    }

    std::string payload = requestJson;
    payload.push_back('\n');

    const char* data = payload.data();
    size_t remaining = payload.size();
    while (remaining > 0)
    {
        ssize_t n = ::send(m_SocketFd, data, remaining, 0);
        if (n <= 0)
        {
            Logger::Log::Warning("[PipeTransport] send(request) failed (errno={})", errno);
            return false;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }

    char buffer[4096];
    outResponse.clear();
    for (;;)
    {
        ssize_t n = ::recv(m_SocketFd, buffer, sizeof(buffer), 0);
        if (n < 0)
        {
            Logger::Log::Warning("[PipeTransport] recv(response) failed (errno={})", errno);
            return false;
        }
        if (n == 0)
            break;

        outResponse.append(buffer, buffer + n);
        if (!outResponse.empty() && outResponse.find('\n') != std::string::npos)
            break;
        if (outResponse.size() > 1'000'000)
        {
            Logger::Log::Warning("[PipeTransport] Response exceeded 1MB cap, truncating");
            break;
        }
    }
    if (outResponse.empty())
    {
        Logger::Log::Warning("[PipeTransport] Empty response from server");
    }
    return !outResponse.empty();
#else
    (void)requestJson;
    (void)outResponse;
    return false;
#endif
}

void PipeTransport::Close()
{
#if defined(PLATFORM_WINDOWS)
    if (m_Handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle((HANDLE)m_Handle);
        m_Handle = INVALID_HANDLE_VALUE;
    }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    if (m_SocketFd >= 0)
    {
        ::close(m_SocketFd);
        m_SocketFd = -1;
    }
#endif
}

} // namespace GameEngine
