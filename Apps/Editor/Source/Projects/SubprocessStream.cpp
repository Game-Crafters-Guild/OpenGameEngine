#include "Projects/SubprocessStream.h"

#include <mutex>
#include <thread>

#include "Logger/Logger.h"
#include "Platform/Capabilities.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <spawn.h>
#include <cstring>
extern char** environ;
#endif

namespace GameEngine {

namespace {

constexpr size_t kStderrTailBytes = 8192;

void AppendTail(std::string& tail, const char* data, size_t count) {
    tail.append(data, count);
    if (tail.size() > kStderrTailBytes)
        tail.erase(0, tail.size() - kStderrTailBytes);
}

// Emit complete '\n'-terminated lines out of an accumulating buffer, keeping the
// trailing partial line. Strips a single trailing '\r' so Windows CRLF is clean.
void DrainLines(std::string& acc, const std::function<void(const std::string&)>& onLine) {
    size_t start = 0;
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i] != '\n')
            continue;
        size_t end = i;
        if (end > start && acc[end - 1] == '\r')
            --end;
        onLine(acc.substr(start, end - start));
        start = i + 1;
    }
    acc.erase(0, start);
}

#ifdef _WIN32

// args strings are UTF-8. Strings that aren't valid UTF-8 (legacy ANSI) fall
// back to byte widening, which is lossless for ASCII.
std::wstring WidenArg(const std::string& arg) {
    if (arg.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, arg.data(),
                                           static_cast<int>(arg.size()), nullptr, 0);
    if (length <= 0)
        return std::wstring(arg.begin(), arg.end());
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, arg.data(), static_cast<int>(arg.size()), wide.data(),
                        length);
    return wide;
}

// Quote a single argument per the CommandLineToArgvW rules so paths with spaces
// survive the CreateProcessW single-string command line.
std::wstring QuoteArg(const std::string& arg) {
    std::wstring w = WidenArg(arg);
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos)
        return w;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : w) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

#endif

} // namespace

#if defined(_WIN32)

static SubprocessResult RunSubprocessStreamingNative(
    const std::filesystem::path& exe,
    const std::vector<std::string>& args,
    const std::function<void(const std::string&)>& onStdoutLine) {
    SubprocessResult result;

    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const auto& a : args)
        cmd += L" " + QuoteArg(a);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRd = nullptr, outWr = nullptr, errRd = nullptr, errWr = nullptr;
    if (!CreatePipe(&outRd, &outWr, &sa, 0) || !CreatePipe(&errRd, &errWr, &sa, 0)) {
        if (outRd) CloseHandle(outRd);
        if (outWr) CloseHandle(outWr);
        if (errRd) CloseHandle(errRd);
        if (errWr) CloseHandle(errWr);
        return result;
    }
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWr;
    si.hStdError = errWr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Parent side closes its copies of the write ends so reads see EOF at exit.
    CloseHandle(outWr);
    CloseHandle(errWr);
    if (!ok) {
        CloseHandle(outRd);
        CloseHandle(errRd);
        return result;
    }
    result.Spawned = true;

    // Drain stderr on a helper thread so a full stderr pipe can never block the
    // child while we are reading stdout.
    std::string stderrTail;
    std::mutex stderrMutex;
    std::thread stderrThread([&]() {
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(errRd, buf, sizeof(buf), &read, nullptr) && read > 0) {
            std::lock_guard<std::mutex> lock(stderrMutex);
            AppendTail(stderrTail, buf, read);
        }
    });

    std::string acc;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(outRd, buf, sizeof(buf), &read, nullptr) && read > 0) {
        acc.append(buf, read);
        DrainLines(acc, onStdoutLine);
    }
    if (!acc.empty())
        onStdoutLine(acc); // trailing line without a newline

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result.ExitCode = static_cast<int>(exitCode);

    stderrThread.join();
    {
        std::lock_guard<std::mutex> lock(stderrMutex);
        result.StderrTail = std::move(stderrTail);
    }

    CloseHandle(outRd);
    CloseHandle(errRd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

#else

static SubprocessResult RunSubprocessStreamingNative(
    const std::filesystem::path& exe,
    const std::vector<std::string>& args,
    const std::function<void(const std::string&)>& onStdoutLine) {
    SubprocessResult result;

    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0)
        return result;

    std::vector<std::string> argvStore;
    argvStore.reserve(args.size() + 1);
    argvStore.push_back(exe.string());
    for (const auto& a : args)
        argvStore.push_back(a);
    std::vector<char*> argv;
    for (auto& s : argvStore)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errPipe[1]);

    pid_t pid = 0;
    const int spawnRc = posix_spawn(&pid, exe.string().c_str(), &actions, nullptr,
                                    argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    close(errPipe[1]);
    if (spawnRc != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        return result;
    }
    result.Spawned = true;

    std::string acc;
    std::string stderrTail;
    int openFds = 2;
    char buf[4096];
    while (openFds > 0) {
        fd_set fds;
        FD_ZERO(&fds);
        if (outPipe[0] >= 0) FD_SET(outPipe[0], &fds);
        if (errPipe[0] >= 0) FD_SET(errPipe[0], &fds);
        int maxFd = std::max(outPipe[0], errPipe[0]) + 1;
        if (select(maxFd, &fds, nullptr, nullptr, nullptr) <= 0)
            break;
        if (outPipe[0] >= 0 && FD_ISSET(outPipe[0], &fds)) {
            ssize_t n = read(outPipe[0], buf, sizeof(buf));
            if (n > 0) {
                acc.append(buf, n);
                DrainLines(acc, onStdoutLine);
            } else {
                close(outPipe[0]);
                outPipe[0] = -1;
                --openFds;
            }
        }
        if (errPipe[0] >= 0 && FD_ISSET(errPipe[0], &fds)) {
            ssize_t n = read(errPipe[0], buf, sizeof(buf));
            if (n > 0) {
                AppendTail(stderrTail, buf, n);
            } else {
                close(errPipe[0]);
                errPipe[0] = -1;
                --openFds;
            }
        }
    }
    if (outPipe[0] >= 0) close(outPipe[0]);
    if (errPipe[0] >= 0) close(errPipe[0]);
    if (!acc.empty())
        onStdoutLine(acc);

    int status = 0;
    waitpid(pid, &status, 0);
    result.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.StderrTail = std::move(stderrTail);
    return result;
}

#endif

SubprocessResult RunSubprocessStreaming(
    const std::filesystem::path& exe,
    const std::vector<std::string>& args,
    const std::function<void(const std::string&)>& onStdoutLine) {
    // The spawn calls exist and link on every platform; where the platform
    // cannot create a process they fail with an opaque errno, so refuse here
    // with a line the caller can show.
    if (!Platform::SupportsProcessCreation()) {
        SubprocessResult result;
        result.Spawned = false;
        result.ExitCode = -1;
        result.StderrTail = "Running external tools is not available on this platform";
        LOG_WARNING("Subprocess: cannot run '{}' (no process creation on this platform).",
                    exe.string());
        return result;
    }
    return RunSubprocessStreamingNative(exe, args, onStdoutLine);
}

} // namespace GameEngine
