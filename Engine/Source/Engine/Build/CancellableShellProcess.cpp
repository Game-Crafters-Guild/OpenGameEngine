#include "Engine/Build/CancellableShellProcess.h"

#include "Logger/Logger.h"
#include "Platform/Process.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <algorithm>
#  include <windows.h>
#else
#  include <algorithm>
#  include <cerrno>
#  include <fcntl.h>
#  include <limits>
#  include <poll.h>
#  include <pthread.h>
#  include <signal.h>
#  include <string_view>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <crt_externs.h>
#  else
extern char** environ;
#  endif
#endif

namespace GameEngine {

namespace {

// Splits incoming bytes into lines for the per-line callback while accumulating
// the raw text into `output`. Newlines/carriage-returns are preserved in
// `output` but stripped from the lines handed to `onLine`.
struct OutputSink
{
    std::string& output;
    const std::function<void(const std::string&)>& onLine;
    std::string pending;

    void Consume(const char* data, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            const char c = data[i];
            output.push_back(c);
            if (c == '\n')
            {
                if (onLine) onLine(pending);
                pending.clear();
            }
            else if (c != '\r')
            {
                pending.push_back(c);
            }
        }
    }

    void Flush()
    {
        if (!pending.empty() && onLine)
            onLine(pending);
        pending.clear();
    }
};

// Splits the argv Run's stdout into lines for `onLine`, each without its line
// ending ("\n" or "\r\n"; any other '\r' is part of the line). A line that grows
// past kMaxProcessStdoutLineBytes is dropped up to its newline and counted, so the
// callback only ever sees whole lines. Nothing is kept once a line is delivered.
struct CappedLineSink
{
    std::size_t& overlongLines;
    const std::function<void(const std::string&)>& onLine;
    std::string pending;
    bool overlong = false;
    bool carriageReturn = false;  // a '\r' held back until the next byte shows whether it ends the line

    void Consume(const char* data, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            const char c = data[i];
            if (c == '\n')
            {
                EndLine();
                continue;
            }
            if (carriageReturn)
            {
                carriageReturn = false;
                Append('\r');
            }
            if (c == '\r')
                carriageReturn = true;
            else
                Append(c);
        }
    }

    void Flush()
    {
        if (!pending.empty() || overlong)
            EndLine();
    }

private:
    void Append(char c)
    {
        if (overlong)
            return;
        if (pending.size() == kMaxProcessStdoutLineBytes)
        {
            overlong = true;
            pending.clear();
            return;
        }
        pending.push_back(c);
    }

    void EndLine()
    {
        if (overlong)
        {
            ++overlongLines;
            overlong = false;
        }
        else if (onLine)
        {
            onLine(pending);
        }
        pending.clear();
        carriageReturn = false;
    }
};

// Keeps the last kProcessStderrTailBytes of the child's stderr.
void AppendToTail(std::string& tail, const char* data, std::size_t count)
{
    tail.append(data, count);
    if (tail.size() > 2 * kProcessStderrTailBytes)
        tail.erase(0, tail.size() - kProcessStderrTailBytes);
}

void TrimTail(std::string& tail)
{
    if (tail.size() > kProcessStderrTailBytes)
        tail.erase(0, tail.size() - kProcessStderrTailBytes);
}

constexpr int kPollMs = 15;  // cancellation responsiveness while waiting on output
constexpr std::size_t kStdinChunkBytes = 64u * 1024u;

} // namespace

CancellableShellProcess::~CancellableShellProcess()
{
    Kill();
}

void CancellableShellProcess::Kill()
{
    m_KillRequested.store(true);
    std::lock_guard<std::mutex> lock(m_HandleMutex);
    TerminateChildLocked();
}

#if defined(_WIN32)

namespace {
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring QuoteWindowsArg(const std::wstring& arg)
{
    if (arg.empty())
        return L"\"\"";

    bool needsQuotes = false;
    for (wchar_t c : arg)
    {
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'"')
        {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes)
        return arg;

    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(c);
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// One anonymous pipe between this process and the child. Both ends are created
// non-inheritable and only the child's end is then marked, so the end this
// process keeps can never leak into a child another thread starts meanwhile.
struct ChildPipe
{
    HANDLE Read = nullptr;
    HANDLE Write = nullptr;

    bool Create(bool parentReads)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!::CreatePipe(&Read, &Write, &sa, 0))
            return false;
        return ::SetHandleInformation(parentReads ? Write : Read, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != 0;
    }

    void Close()
    {
        CloseEnd(Read);
        CloseEnd(Write);
    }

    static void CloseEnd(HANDLE& handle)
    {
        if (handle)
            ::CloseHandle(handle);
        handle = nullptr;
    }
};

// Reads what `pipe` holds without blocking. Clears `open` once the write end is
// closed and nothing is buffered. Returns the bytes read.
std::size_t ReadAvailable(HANDLE pipe, bool& open, char* buffer, std::size_t capacity)
{
    if (!open)
        return 0;
    DWORD avail = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr))
    {
        open = false;
        return 0;
    }
    if (avail == 0)
        return 0;
    const DWORD toRead = avail < capacity ? avail : static_cast<DWORD>(capacity);
    DWORD got = 0;
    if (!::ReadFile(pipe, buffer, toRead, &got, nullptr))
    {
        open = false;
        return 0;
    }
    return got;
}

// Writes the payload to the child's stdin, then closes it so the child sees the
// end of input. Stops early when the write fails or `childGone` is set: another
// process that holds the read end (a descendant that inherited the child's stdin,
// or a process another thread of this one started with CreateProcess(TRUE) while
// the handle was inheritable) keeps a write blocked after the child exits, so
// Run() sets the flag and cancels the blocked write.
void WriteStdin(HANDLE pipe, const std::string& payload, const std::atomic<bool>& childGone)
{
    std::size_t offset = 0;
    while (offset < payload.size() && !childGone.load())
    {
        const std::size_t remaining = payload.size() - offset;
        const DWORD chunk = static_cast<DWORD>(remaining < kStdinChunkBytes ? remaining : kStdinChunkBytes);
        DWORD written = 0;
        if (!::WriteFile(pipe, payload.data() + offset, chunk, &written, nullptr))
            break;
        offset += written;
    }
    ::CloseHandle(pipe);
}
} // namespace

ShellProcessResult RunProcessCaptured(const std::string& executable,
                                      const std::vector<std::string>& arguments,
                                      std::chrono::milliseconds timeout)
{
    ShellProcessResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        Logger::Log::Error("[RunProcessCaptured] CreatePipe failed (err={})", ::GetLastError());
        return result;
    }
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // Restrict inheritance to just the write pipe via an explicit handle list.
    // Without it, CreateProcess(bInheritHandles=TRUE) would also hand this pipe
    // to any unrelated child a different thread launches at the same moment,
    // which would keep the write end open and stall our read loop.
    STARTUPINFOEXW siex{};
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdOutput = writePipe;
    siex.StartupInfo.hStdError = writePipe;
    siex.StartupInfo.hStdInput = nullptr;

    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrStorage(attrSize ? attrSize : 1);
    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());

    bool attrInited = attrSize != 0 &&
                      ::InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    bool extended = false;
    HANDLE inheritList[1] = {writePipe};
    if (attrInited)
    {
        extended = ::UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inheritList, sizeof(inheritList), nullptr, nullptr);
    }
    siex.StartupInfo.cb = extended ? sizeof(siex) : sizeof(STARTUPINFOW);
    siex.lpAttributeList = extended ? attrList : nullptr;

    std::wstring commandLine = QuoteWindowsArg(Utf8ToWide(executable));
    for (const std::string& arg : arguments)
    {
        commandLine.push_back(L' ');
        commandLine += QuoteWindowsArg(Utf8ToWide(arg));
    }
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');

    const DWORD creationFlags = CREATE_NO_WINDOW | (extended ? EXTENDED_STARTUPINFO_PRESENT : 0u);
    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                                          /*inheritHandles=*/TRUE, creationFlags,
                                          nullptr, nullptr, &siex.StartupInfo, &pi);
    ::CloseHandle(writePipe);
    if (attrInited)
        ::DeleteProcThreadAttributeList(attrList);

    if (!created)
    {
        Logger::Log::Error("[RunProcessCaptured] CreateProcess failed (err={})", ::GetLastError());
        ::CloseHandle(readPipe);
        return result;
    }
    ::CloseHandle(pi.hThread);

    const bool useTimeout = timeout > std::chrono::milliseconds::zero();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timedOut = false;

    // Anonymous pipes don't support overlapped reads, so poll with PeekNamedPipe
    // (non-blocking) to keep the wait interruptible by the timeout instead of
    // blocking forever inside ReadFile.
    auto drainAvailable = [&]() {
        char buf[4096];
        for (;;)
        {
            DWORD avail = 0;
            if (!::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                break;
            DWORD toRead = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
            DWORD got = 0;
            if (!::ReadFile(readPipe, buf, toRead, &got, nullptr) || got == 0)
                break;
            result.output.append(buf, buf + got);
        }
    };

    constexpr DWORD kPollIntervalMs = 25;
    for (;;)
    {
        drainAvailable();

        DWORD waitMs = kPollIntervalMs;
        if (useTimeout)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                timedOut = true;
                break;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (remaining < waitMs)
                waitMs = static_cast<DWORD>(remaining);
        }

        const DWORD waited = ::WaitForSingleObject(pi.hProcess, waitMs);
        if (waited == WAIT_OBJECT_0)
        {
            drainAvailable();  // capture anything buffered after exit
            break;
        }
    }

    if (timedOut)
    {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        result.cancelled = true;
    }

    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    ::CloseHandle(pi.hProcess);
    ::CloseHandle(readPipe);
    return result;
}

void CancellableShellProcess::TerminateChildLocked()
{
    if (m_ProcessHandle)
        ::TerminateProcess(static_cast<HANDLE>(m_ProcessHandle), 1);
}

ShellProcessResult CancellableShellProcess::Run(const std::string& command,
                                                std::function<bool()> shouldCancel,
                                                std::function<void(const std::string& line)> onLine)
{
    ShellProcessResult result;
    m_KillRequested.store(false);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        Logger::Log::Error("[CancellableShellProcess] CreatePipe failed (err={})", ::GetLastError());
        return result;
    }
    // The parent's read end must not be inherited by the child.
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    // /S + outer quotes: cmd strips exactly the first and last quote and runs the
    // remainder verbatim, so complex commands (with their own quoting / "2>&1")
    // pass through unmangled.
    std::wstring fullCommand = L"cmd.exe /S /C \"" + Utf8ToWide(command) + L"\"";
    std::vector<wchar_t> cmdBuf(fullCommand.begin(), fullCommand.end());
    cmdBuf.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                                          /*inheritHandles=*/TRUE, CREATE_NO_WINDOW,
                                          nullptr, nullptr, &si, &pi);
    // The child holds its own write-end copy; close ours so ReadFile sees EOF on exit.
    ::CloseHandle(writePipe);

    if (!created)
    {
        Logger::Log::Error("[CancellableShellProcess] CreateProcess failed (err={})", ::GetLastError());
        ::CloseHandle(readPipe);
        return result;
    }
    ::CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_ProcessHandle = pi.hProcess;
        if (m_KillRequested.load())
        {
            result.cancelled = true;
            TerminateChildLocked();  // Kill() raced ahead of process creation
        }
    }

    OutputSink sink{ result.output, onLine, {} };
    bool exited = false;
    for (;;)
    {
        // Poll cancellation every iteration — including while output is streaming
        // — so a chatty child (cmake/compiler) can still be cancelled promptly.
        if (!result.cancelled && ((shouldCancel && shouldCancel()) || m_KillRequested.load()))
        {
            result.cancelled = true;
            std::lock_guard<std::mutex> lock(m_HandleMutex);
            TerminateChildLocked();
        }

        DWORD avail = 0;
        const BOOL peekOk = ::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr);
        if (peekOk && avail > 0)
        {
            char buf[4096];
            const DWORD toRead = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
            DWORD got = 0;
            if (::ReadFile(readPipe, buf, toRead, &got, nullptr) && got > 0)
            {
                sink.Consume(buf, got);
                continue;
            }
            break;  // read failed / broken pipe
        }
        if (!peekOk)
            break;  // write end closed and nothing buffered
        if (exited)
            break;  // drained everything after exit
        if (::WaitForSingleObject(pi.hProcess, kPollMs) == WAIT_OBJECT_0)
            exited = true;  // loop once more to drain any final bytes
    }
    sink.Flush();

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_ProcessHandle = nullptr;
    }
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(readPipe);
    return result;
}

ShellProcessResult CancellableShellProcess::Run(const ProcessLaunch& launch,
                                                std::function<bool()> shouldCancel,
                                                std::function<void(const std::string& line)> onLine)
{
    ShellProcessResult result;
    m_KillRequested.store(false);
    if (launch.StdinPayload.size() > kMaxProcessStdinBytes)
    {
        Logger::Log::Error("[CancellableShellProcess] stdin payload of {} bytes exceeds the {}-byte cap; {} not started",
                           launch.StdinPayload.size(), kMaxProcessStdinBytes, launch.Executable);
        return result;
    }
    std::optional<std::wstring> environmentBlock = Platform::BuildChildEnvironmentBlock(launch.Environment);
    if (!environmentBlock)
    {
        Logger::Log::Error("[CancellableShellProcess] an environment edit's name is empty or contains '='; {} not started",
                           launch.Executable);
        return result;
    }

    ChildPipe stdinPipe;
    ChildPipe stdoutPipe;
    ChildPipe stderrPipe;
    if (!stdinPipe.Create(/*parentReads=*/false) || !stdoutPipe.Create(true) || !stderrPipe.Create(true))
    {
        Logger::Log::Error("[CancellableShellProcess] CreatePipe failed (err={})", ::GetLastError());
        stdinPipe.Close();
        stdoutPipe.Close();
        stderrPipe.Close();
        return result;
    }

    // Only the child's three ends are inherited: the handle list below is attached
    // to the startup info (see RunProcessCaptured).
    STARTUPINFOEXW siex{};
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdInput = stdinPipe.Read;
    siex.StartupInfo.hStdOutput = stdoutPipe.Write;
    siex.StartupInfo.hStdError = stderrPipe.Write;

    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrStorage(attrSize ? attrSize : 1);
    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    const bool attrInited = attrSize != 0 && ::InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    bool extended = false;
    HANDLE inheritList[3] = {stdinPipe.Read, stdoutPipe.Write, stderrPipe.Write};
    if (attrInited)
    {
        extended = ::UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inheritList, sizeof(inheritList), nullptr, nullptr);
    }
    siex.StartupInfo.cb = extended ? sizeof(siex) : sizeof(STARTUPINFOW);
    siex.lpAttributeList = extended ? attrList : nullptr;

    std::wstring commandLine = QuoteWindowsArg(Utf8ToWide(launch.Executable));
    for (const std::string& arg : launch.Arguments)
    {
        commandLine.push_back(L' ');
        commandLine += QuoteWindowsArg(Utf8ToWide(arg));
    }
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');

    const std::wstring workingDirectory = launch.WorkingDirectory.wstring();

    const DWORD creationFlags = CREATE_NO_WINDOW | (extended ? EXTENDED_STARTUPINFO_PRESENT : 0u) |
                                (environmentBlock->empty() ? 0u : CREATE_UNICODE_ENVIRONMENT);
    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                                          /*inheritHandles=*/TRUE, creationFlags,
                                          environmentBlock->empty() ? nullptr : environmentBlock->data(),
                                          workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                          &siex.StartupInfo, &pi);
    if (attrInited)
        ::DeleteProcThreadAttributeList(attrList);
    // The child holds its own copies; close ours so reads end when the child exits.
    ChildPipe::CloseEnd(stdinPipe.Read);
    ChildPipe::CloseEnd(stdoutPipe.Write);
    ChildPipe::CloseEnd(stderrPipe.Write);

    if (!created)
    {
        Logger::Log::Error("[CancellableShellProcess] CreateProcess failed for {} (err={})",
                           launch.Executable, ::GetLastError());
        stdinPipe.Close();
        stdoutPipe.Close();
        stderrPipe.Close();
        return result;
    }
    ::CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_ProcessHandle = pi.hProcess;
        if (m_KillRequested.load())
        {
            result.cancelled = true;
            TerminateChildLocked();  // Kill() raced ahead of process creation
        }
    }

    // A writer thread, so a child that writes before it has read all of its input
    // cannot deadlock against a parent blocked on a full stdin pipe.
    std::atomic<bool> childGone{false};
    std::thread stdinWriter(WriteStdin, stdinPipe.Write, std::cref(launch.StdinPayload), std::cref(childGone));
    stdinPipe.Write = nullptr;  // owned and closed by the writer

    CappedLineSink sink{ result.overlongStdoutLines, onLine };
    bool stdoutOpen = true;
    bool stderrOpen = true;
    bool exited = false;
    char buf[4096];
    for (;;)
    {
        if (!result.cancelled && ((shouldCancel && shouldCancel()) || m_KillRequested.load()))
        {
            result.cancelled = true;
            std::lock_guard<std::mutex> lock(m_HandleMutex);
            TerminateChildLocked();
        }

        const std::size_t outBytes = ReadAvailable(stdoutPipe.Read, stdoutOpen, buf, sizeof(buf));
        sink.Consume(buf, outBytes);
        const std::size_t errBytes = ReadAvailable(stderrPipe.Read, stderrOpen, buf, sizeof(buf));
        AppendToTail(result.stderrTail, buf, errBytes);
        if (outBytes > 0 || errBytes > 0)
            continue;
        if (!stdoutOpen && !stderrOpen)
            break;  // both write ends closed and drained
        if (exited)
            break;  // drained everything after exit
        if (::WaitForSingleObject(pi.hProcess, kPollMs) == WAIT_OBJECT_0)
            exited = true;  // loop once more to drain any final bytes
    }
    sink.Flush();
    TrimTail(result.stderrTail);

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    // A write still pending now is held by another process with the read end (a
    // descendant of the child, or a process another thread started with
    // CreateProcess(TRUE)): cancel it rather than wait on that process.
    childGone.store(true);
    while (::WaitForSingleObject(stdinWriter.native_handle(), kPollMs) == WAIT_TIMEOUT)
        ::CancelSynchronousIo(stdinWriter.native_handle());
    stdinWriter.join();
    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_ProcessHandle = nullptr;
    }
    ::CloseHandle(pi.hProcess);
    stdoutPipe.Close();
    stderrPipe.Close();
    return result;
}

#else  // POSIX

ShellProcessResult RunProcessCaptured(const std::string& executable,
                                      const std::vector<std::string>& arguments,
                                      std::chrono::milliseconds timeout)
{
    ShellProcessResult result;

    // Create the pipe with the close-on-exec flag set so a child spawned
    // concurrently on another thread cannot inherit a copy of the write end and
    // keep it open after we fork here — which would stall our read() until that
    // unrelated process also exits. The child below dup2()s the write end onto
    // its stdout/stderr; dup2 clears FD_CLOEXEC on the new descriptors, so the
    // captured output still survives execvp().
    int fds[2];
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0)
    {
        Logger::Log::Error("[RunProcessCaptured] pipe2() failed (errno={})", errno);
        return result;
    }
#else
    if (::pipe(fds) != 0)
    {
        Logger::Log::Error("[RunProcessCaptured] pipe() failed (errno={})", errno);
        return result;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    const int readFd = fds[0];
    const int writeFd = fds[1];

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1u);
    storage.push_back(executable);
    for (const std::string& arg : arguments)
    {
        storage.push_back(arg);
    }

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1u);
    for (std::string& arg : storage)
    {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        Logger::Log::Error("[RunProcessCaptured] fork() failed (errno={})", errno);
        ::close(readFd);
        ::close(writeFd);
        return result;
    }

    if (pid == 0)
    {
        ::close(readFd);
        ::dup2(writeFd, STDOUT_FILENO);
        ::dup2(writeFd, STDERR_FILENO);
        ::close(writeFd);

        ::execvp(executable.c_str(), argv.data());
        _exit(127);
    }

    ::close(writeFd);

    const bool useTimeout = timeout > std::chrono::milliseconds::zero();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timedOut = false;

    char buf[4096];
    pollfd pfd{};
    pfd.fd = readFd;
    pfd.events = POLLIN;
    for (;;)
    {
        int waitMs = -1;
        if (useTimeout)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                timedOut = true;
                break;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            waitMs = remaining > std::numeric_limits<int>::max()
                         ? std::numeric_limits<int>::max()
                         : static_cast<int>(remaining);
        }

        const int ready = ::poll(&pfd, 1, waitMs);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
        {
            timedOut = true;
            break;
        }

        const ssize_t got = ::read(readFd, buf, sizeof(buf));
        if (got > 0)
        {
            result.output.append(buf, buf + got);
            continue;
        }
        if (got == 0)
            break;  // EOF: child closed its output
        if (errno == EINTR)
            continue;
        break;
    }
    ::close(readFd);

    if (timedOut)
    {
        ::kill(pid, SIGKILL);
        result.cancelled = true;
    }

    int status = 0;
    pid_t reaped = 0;
    do { reaped = ::waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
    if (reaped == pid)
    {
        if (WIFEXITED(status))
            result.exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            result.exitCode = 128 + WTERMSIG(status);
    }

    return result;
}

void CancellableShellProcess::TerminateChildLocked()
{
    if (m_Pid > 0)
        ::kill(m_Pid, SIGKILL);
}

ShellProcessResult CancellableShellProcess::Run(const std::string& command,
                                                std::function<bool()> shouldCancel,
                                                std::function<void(const std::string& line)> onLine)
{
    ShellProcessResult result;
    m_KillRequested.store(false);

    int fds[2];
    if (::pipe(fds) != 0)
    {
        Logger::Log::Error("[CancellableShellProcess] pipe() failed (errno={})", errno);
        return result;
    }
    const int readFd = fds[0];
    const int writeFd = fds[1];

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        Logger::Log::Error("[CancellableShellProcess] fork() failed (errno={})", errno);
        ::close(readFd);
        ::close(writeFd);
        return result;
    }

    if (pid == 0)
    {
        // Child: merge stdout+stderr into the pipe, then exec a shell.
        ::close(readFd);
        ::dup2(writeFd, STDOUT_FILENO);
        ::dup2(writeFd, STDERR_FILENO);
        ::close(writeFd);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);  // exec failed
    }

    ::close(writeFd);
    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_Pid = pid;
        if (m_KillRequested.load())
        {
            result.cancelled = true;
            TerminateChildLocked();
        }
    }

    OutputSink sink{ result.output, onLine, {} };
    for (;;)
    {
        // Poll cancellation every iteration — including while output is streaming
        // — so a chatty child (cmake/compiler) can still be cancelled promptly.
        if (!result.cancelled && ((shouldCancel && shouldCancel()) || m_KillRequested.load()))
        {
            result.cancelled = true;
            std::lock_guard<std::mutex> lock(m_HandleMutex);
            TerminateChildLocked();
        }

        pollfd pfd{};
        pfd.fd = readFd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, kPollMs);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
        {
            char buf[4096];
            const ssize_t got = ::read(readFd, buf, sizeof(buf));
            if (got > 0) { sink.Consume(buf, static_cast<std::size_t>(got)); continue; }
            if (got == 0) break;  // EOF: child closed its write end
            if (got < 0 && errno == EINTR) continue;
            break;
        }
        // poll timed out with no data: loop back; cancellation re-checked at top.
    }
    sink.Flush();

    int status = 0;
    pid_t reaped;
    do { reaped = ::waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
    if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exitCode = 128 + WTERMSIG(status);

    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_Pid = -1;
    }
    ::close(readFd);
    return result;
}

namespace {
// A pipe whose both ends are close-on-exec, so a child spawned concurrently on
// another thread cannot inherit them (see RunProcessCaptured); dup2() in our own
// child clears the flag on the descriptors it installs.
bool CreateCloexecPipe(int fds[2])
{
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    if (::pipe(fds) != 0)
        return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

void CloseFd(int& fd)
{
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

// Writes the payload to the child's stdin, then closes it so the child sees the
// end of input. SIGPIPE is blocked on this thread, so a child that exits early
// fails the write with EPIPE instead of killing the editor.
void WriteStdin(int fd, const std::string& payload)
{
    sigset_t pipeSignal;
    sigemptyset(&pipeSignal);
    sigaddset(&pipeSignal, SIGPIPE);
    ::pthread_sigmask(SIG_BLOCK, &pipeSignal, nullptr);

    std::size_t offset = 0;
    while (offset < payload.size())
    {
        const std::size_t remaining = payload.size() - offset;
        const ssize_t written = ::write(fd, payload.data() + offset, std::min(remaining, kStdinChunkBytes));
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        offset += static_cast<std::size_t>(written);
    }
    ::close(fd);
}

// In the child after fork(): sends errno through the close-on-exec status pipe
// and exits. Async-signal-safe.
[[noreturn]] void ReportSpawnFailure(int statusFd)
{
    const int error = errno;
    [[maybe_unused]] const ssize_t written = ::write(statusFd, &error, sizeof(error));
    _exit(127);
}

// In the parent: the child's errno when chdir or exec failed, or 0 once exec
// closed the status pipe's write end.
int ReadSpawnFailure(int statusFd)
{
    int error = 0;
    ssize_t got = 0;
    do { got = ::read(statusFd, &error, sizeof(error)); } while (got < 0 && errno == EINTR);
    return got == static_cast<ssize_t>(sizeof(error)) ? error : 0;
}

// One read from `fd` once poll() reported it. Clears `open` at end of file or on
// an error. Returns the bytes read.
std::size_t ReadReady(int fd, short revents, bool& open, char* buffer, std::size_t capacity)
{
    if (!open || !(revents & (POLLIN | POLLHUP | POLLERR)))
        return 0;
    const ssize_t got = ::read(fd, buffer, capacity);
    if (got > 0)
        return static_cast<std::size_t>(got);
    if (got < 0 && errno == EINTR)
        return 0;
    open = false;
    return 0;
}
} // namespace

ShellProcessResult CancellableShellProcess::Run(const ProcessLaunch& launch,
                                                std::function<bool()> shouldCancel,
                                                std::function<void(const std::string& line)> onLine)
{
    ShellProcessResult result;
    m_KillRequested.store(false);
    if (launch.StdinPayload.size() > kMaxProcessStdinBytes)
    {
        Logger::Log::Error("[CancellableShellProcess] stdin payload of {} bytes exceeds the {}-byte cap; {} not started",
                           launch.StdinPayload.size(), kMaxProcessStdinBytes, launch.Executable);
        return result;
    }
    // Built before fork(): the child may only make async-signal-safe calls before exec.
    std::optional<std::vector<std::string>> environmentEntries = Platform::BuildChildEnvironment(launch.Environment);
    if (!environmentEntries)
    {
        Logger::Log::Error("[CancellableShellProcess] an environment edit's name is empty or contains '='; {} not started",
                           launch.Executable);
        return result;
    }

    int stdinFds[2] = {-1, -1};
    int stdoutFds[2] = {-1, -1};
    int stderrFds[2] = {-1, -1};
    int statusFds[2] = {-1, -1};  // the child's errno when chdir or exec fails
    if (!CreateCloexecPipe(stdinFds) || !CreateCloexecPipe(stdoutFds) || !CreateCloexecPipe(stderrFds) ||
        !CreateCloexecPipe(statusFds))
    {
        Logger::Log::Error("[CancellableShellProcess] pipe() failed (errno={})", errno);
        for (int* fd : {&stdinFds[0], &stdinFds[1], &stdoutFds[0], &stdoutFds[1], &stderrFds[0], &stderrFds[1],
                        &statusFds[0], &statusFds[1]})
            CloseFd(*fd);
        return result;
    }

    // Everything the child needs is built before fork().
    std::vector<std::string> storage;
    storage.reserve(launch.Arguments.size() + 1u);
    storage.push_back(launch.Executable);
    storage.insert(storage.end(), launch.Arguments.begin(), launch.Arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1u);
    for (std::string& arg : storage)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<char*> envp;
    envp.reserve(environmentEntries->size() + 1u);
    for (std::string& entry : *environmentEntries)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    const std::string workingDirectory = launch.WorkingDirectory.string();

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        Logger::Log::Error("[CancellableShellProcess] fork() failed (errno={})", errno);
        for (int* fd : {&stdinFds[0], &stdinFds[1], &stdoutFds[0], &stdoutFds[1], &stderrFds[0], &stderrFds[1],
                        &statusFds[0], &statusFds[1]})
            CloseFd(*fd);
        return result;
    }

    if (pid == 0)
    {
        ::dup2(stdinFds[0], STDIN_FILENO);
        ::dup2(stdoutFds[1], STDOUT_FILENO);
        ::dup2(stderrFds[1], STDERR_FILENO);
        if (!workingDirectory.empty() && ::chdir(workingDirectory.c_str()) != 0)
            ReportSpawnFailure(statusFds[1]);
        // execvp resolves the executable through PATH and hands the child environ.
#if defined(__APPLE__)
        *_NSGetEnviron() = envp.data();
#else
        environ = envp.data();
#endif
        ::execvp(argv[0], argv.data());
        ReportSpawnFailure(statusFds[1]);
    }

    CloseFd(stdinFds[0]);
    CloseFd(stdoutFds[1]);
    CloseFd(stderrFds[1]);
    CloseFd(statusFds[1]);
    const int spawnError = ReadSpawnFailure(statusFds[0]);
    CloseFd(statusFds[0]);
    if (spawnError != 0)
    {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        Logger::Log::Error("[CancellableShellProcess] could not start {} in '{}' (errno={})", launch.Executable,
                           workingDirectory, spawnError);
        for (int* fd : {&stdinFds[1], &stdoutFds[0], &stderrFds[0]})
            CloseFd(*fd);
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_Pid = pid;
        if (m_KillRequested.load())
        {
            result.cancelled = true;
            TerminateChildLocked();
        }
    }

    std::thread stdinWriter(WriteStdin, stdinFds[1], std::cref(launch.StdinPayload));
    stdinFds[1] = -1;  // owned and closed by the writer

    CappedLineSink sink{ result.overlongStdoutLines, onLine };
    bool stdoutOpen = true;
    bool stderrOpen = true;
    char buf[4096];
    while (stdoutOpen || stderrOpen)
    {
        if (!result.cancelled && ((shouldCancel && shouldCancel()) || m_KillRequested.load()))
        {
            result.cancelled = true;
            std::lock_guard<std::mutex> lock(m_HandleMutex);
            TerminateChildLocked();
        }

        pollfd fds[2] = {{stdoutOpen ? stdoutFds[0] : -1, POLLIN, 0}, {stderrOpen ? stderrFds[0] : -1, POLLIN, 0}};
        const int ready = ::poll(fds, 2, kPollMs);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0)
            continue;  // cancellation re-checked at the top
        sink.Consume(buf, ReadReady(stdoutFds[0], fds[0].revents, stdoutOpen, buf, sizeof(buf)));
        AppendToTail(result.stderrTail, buf, ReadReady(stderrFds[0], fds[1].revents, stderrOpen, buf, sizeof(buf)));
    }
    sink.Flush();
    TrimTail(result.stderrTail);

    int status = 0;
    pid_t reaped;
    do { reaped = ::waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
    stdinWriter.join();  // the child is gone, so a pending write has failed
    if (reaped == pid)
    {
        if (WIFEXITED(status))
            result.exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            result.exitCode = 128 + WTERMSIG(status);
    }

    {
        std::lock_guard<std::mutex> lock(m_HandleMutex);
        m_Pid = -1;
    }
    CloseFd(stdoutFds[0]);
    CloseFd(stderrFds[0]);
    return result;
}

#endif

} // namespace GameEngine
