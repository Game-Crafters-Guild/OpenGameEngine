#include "VCSIntegration/VCSCommandExecutor.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdlib>

#include "Platform/Capabilities.h"
#include "Platform/Process.h"

#if defined(_WIN32)
#include <windows.h>
#include <shlwapi.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace GameEngine
{
namespace
{

using Result = VCSCommandExecutor::Result;
using Environment = VCSCommandExecutor::Environment;

// The overrides as environment edits, each setting its variable.
std::vector<Platform::EnvironmentEdit> ToEnvironmentEdits(const Environment& overrides)
{
    std::vector<Platform::EnvironmentEdit> edits;
    edits.reserve(overrides.size());
    for (const auto& [name, value] : overrides)
        edits.push_back({name, value});
    return edits;
}

constexpr const char* kInvalidEnvironmentName = "An environment variable name is empty or contains '='";

#ifdef _WIN32
// The CreateProcessW command line: the quoted executable, then the arguments.
using ProcessCommand = std::wstring;

ProcessCommand BuildCommandLine(const std::filesystem::path& executable, const std::string& arguments)
{
    return std::wstring(L"\"") + executable.wstring() + L"\" " +
           std::wstring(arguments.begin(), arguments.end());
}

// Quotes one argument for a CreateProcessW command line so the child parses
// it back as a single argv entry under CommandLineToArgvW rules (no cmd.exe,
// so only whitespace and quotes need care, with backslashes doubled ahead of
// any quote).
std::string QuoteArgument(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;

    std::string quoted = "\"";
    size_t backslashes = 0;
    for (const char c : arg)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }
        if (c == '"')
        {
            quoted.append(backslashes * 2 + 1, '\\');
        }
        else
        {
            quoted.append(backslashes, '\\');
        }
        quoted += c;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

std::string JoinQuotedArguments(const std::vector<std::string>& args)
{
    std::string joined;
    for (const auto& arg : args)
    {
        if (!joined.empty())
            joined += " ";
        joined += QuoteArgument(arg);
    }
    return joined;
}

constexpr DWORD kPipeReadBufferSize = 4096;
constexpr DWORD kPipePollIntervalMs = 10;

// Appends what the pipe holds right now to sink without blocking. Returns
// false when the pipe is empty or closed.
bool ReadAvailable(HANDLE pipe, std::string& sink)
{
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
        return false;

    char buffer[kPipeReadBufferSize];
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, buffer, std::min(available, kPipeReadBufferSize), &bytesRead, nullptr) || bytesRead == 0)
        return false;
    sink.append(buffer, bytesRead);
    return true;
}

// Returns true when either pipe produced data.
bool ReadAvailablePipes(HANDLE outPipe, HANDLE errPipe, std::string& output, std::string& errorOutput)
{
    const bool readOutput = ReadAvailable(outPipe, output);
    const bool readError = ReadAvailable(errPipe, errorOutput);
    return readOutput || readError;
}

#else
// The argv handed to execve; the first entry is the program that runs.
using ProcessCommand = std::vector<std::string>;

constexpr size_t kPipeReadBufferSize = 4096;
constexpr int kPipePollTimeoutMs = 100;

// Appends one read of fd to sink. Returns false at end of file or on error.
bool ReadInto(int fd, std::string& sink)
{
    char buffer[kPipeReadBufferSize];
    const ssize_t bytesRead = read(fd, buffer, sizeof(buffer));
    if (bytesRead <= 0)
        return false;
    sink.append(buffer, static_cast<size_t>(bytesRead));
    return true;
}

// Waits up to timeoutMs for either pipe to become readable and reads what
// arrived. Returns true when either pipe produced data. poll(), not select():
// select() is undefined for descriptors at or above FD_SETSIZE, which a
// long-running editor process can reach.
bool ReadAvailablePipes(int outFd, int errFd, std::string& output, std::string& errorOutput, int timeoutMs)
{
    pollfd fds[] = {{outFd, POLLIN, 0}, {errFd, POLLIN, 0}};
    if (poll(fds, 2, timeoutMs) <= 0)
        return false;

    bool readAny = false;
    if (fds[0].revents & (POLLIN | POLLHUP))
        readAny = ReadInto(outFd, output) || readAny;
    if (fds[1].revents & (POLLIN | POLLHUP))
        readAny = ReadInto(errFd, errorOutput) || readAny;
    return readAny;
}

#endif

Result RunProcess(const std::filesystem::path& executable,
                  const std::filesystem::path& workingDir,
                  const ProcessCommand& command,
                  bool captureOutput,
                  const Environment& environment);

} // namespace

VCSCommandExecutor::Result VCSCommandExecutor::Execute(
    const std::filesystem::path& executable,
    const std::filesystem::path& workingDir,
    const std::vector<std::string>& args,
    bool captureOutput,
    const Environment& environment)
{
#ifdef _WIN32
    return RunProcess(executable, workingDir, BuildCommandLine(executable, JoinQuotedArguments(args)),
                      captureOutput, environment);
#else
    ProcessCommand argv;
    argv.reserve(args.size() + 1);
    argv.push_back(executable.string());
    argv.insert(argv.end(), args.begin(), args.end());
    return RunProcess(executable, workingDir, argv, captureOutput, environment);
#endif
}

namespace
{

Result RunProcess(const std::filesystem::path& executable,
                  const std::filesystem::path& workingDir,
                  const ProcessCommand& command,
                  bool captureOutput,
                  const Environment& environment)
{
    Result result;

    // The spawn calls exist and link everywhere; where the platform cannot
    // create a process they fail with an opaque errno, so refuse up front.
    if (!Platform::SupportsProcessCreation())
    {
        static bool s_reported = false;
        if (!s_reported)
        {
            s_reported = true;
            LOG_WARNING("VCS: version control is not available on this platform (no process "
                        "creation); every VCS command reports failure.");
        }
        result.success = false;
        result.exitCode = -1;
        result.error = "Version control is not available on this platform";
        return result;
    }
    if (executable.empty() || !std::filesystem::exists(executable))
    {
        result.success = false;
        result.error = "VCS executable not found";
        return result;
    }

    std::filesystem::path wd = workingDir.empty() ? std::filesystem::current_path() : workingDir;
    if (!std::filesystem::exists(wd))
    {
        result.success = false;
        result.error = "Working directory does not exist";
        return result;
    }

#ifdef _WIN32
    std::wstring wCommand = command;
    std::wstring wWorkingDir = wd.wstring();
    std::optional<std::wstring> environmentBlock = Platform::BuildChildEnvironmentBlock(ToEnvironmentEdits(environment));
    if (!environmentBlock)
    {
        result.success = false;
        result.error = kInvalidEnvironmentName;
        return result;
    }

    HANDLE hChildStdOutRd = nullptr;
    HANDLE hChildStdOutWr = nullptr;
    HANDLE hChildStdErrRd = nullptr;
    HANDLE hChildStdErrWr = nullptr;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    if (captureOutput)
    {
        if (!CreatePipe(&hChildStdOutRd, &hChildStdOutWr, &saAttr, 0) ||
            !CreatePipe(&hChildStdErrRd, &hChildStdErrWr, &saAttr, 0))
        {
            result.success = false;
            result.error = "Failed to create pipes";
            return result;
        }

        if (!SetHandleInformation(hChildStdOutRd, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(hChildStdErrRd, HANDLE_FLAG_INHERIT, 0))
        {
            CloseHandle(hChildStdOutRd);
            CloseHandle(hChildStdOutWr);
            CloseHandle(hChildStdErrRd);
            CloseHandle(hChildStdErrWr);
            result.success = false;
            result.error = "Failed to set handle information";
            return result;
        }
    }

    PROCESS_INFORMATION piProcInfo;
    STARTUPINFOW siStartInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFOW));

    siStartInfo.cb = sizeof(STARTUPINFOW);
    if (captureOutput)
    {
        siStartInfo.hStdError = hChildStdErrWr;
        siStartInfo.hStdOutput = hChildStdOutWr;
        siStartInfo.dwFlags |= STARTF_USESTDHANDLES;
    }

    // CREATE_NO_WINDOW: a console VCS binary spawned from the GUI editor would
    // otherwise flash a console window on every status poll.
    BOOL success = CreateProcessW(
        nullptr,
        wCommand.data(),
        nullptr,
        nullptr,
        captureOutput ? TRUE : FALSE,
        CREATE_NO_WINDOW | (environmentBlock->empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
        environmentBlock->empty() ? nullptr : environmentBlock->data(),
        wWorkingDir.c_str(),
        &siStartInfo,
        &piProcInfo);

    if (captureOutput)
    {
        CloseHandle(hChildStdOutWr);
        CloseHandle(hChildStdErrWr);
    }

    if (!success)
    {
        result.success = false;
        result.error = "Failed to create process: " + std::to_string(GetLastError());
        if (captureOutput)
        {
            CloseHandle(hChildStdOutRd);
            CloseHandle(hChildStdErrRd);
        }
        return result;
    }

    std::string output;
    std::string errorOutput;

    if (captureOutput)
    {
        // Both pipes are polled without blocking: a blocking read on stdout
        // while the child fills the stderr pipe (git progress output) would
        // deadlock both processes.
        while (WaitForSingleObject(piProcInfo.hProcess, 0) == WAIT_TIMEOUT)
        {
            if (!ReadAvailablePipes(hChildStdOutRd, hChildStdErrRd, output, errorOutput))
                Sleep(kPipePollIntervalMs);
        }
        // What the child wrote just before it exited is still in the pipes.
        while (ReadAvailablePipes(hChildStdOutRd, hChildStdErrRd, output, errorOutput))
        {
        }

        CloseHandle(hChildStdOutRd);
        CloseHandle(hChildStdErrRd);
    }

    WaitForSingleObject(piProcInfo.hProcess, INFINITE);
    DWORD exitCode = 0;
    result.exitCode = GetExitCodeProcess(piProcInfo.hProcess, &exitCode) ? static_cast<int>(exitCode) : -1;

    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);

    result.success = (result.exitCode == 0);
    result.output = output;
    if (!errorOutput.empty())
    {
        result.error = errorOutput;
    }

#else
    // Unix/Linux/macOS implementation
    // Built before fork(), and before the pipes so a refusal leaks nothing.
    const std::optional<std::vector<std::string>> environmentEntries =
        Platform::BuildChildEnvironment(ToEnvironmentEdits(environment));
    if (!environmentEntries)
    {
        result.success = false;
        result.error = kInvalidEnvironmentName;
        return result;
    }
    int pipeOut[2], pipeErr[2];
    if (captureOutput)
    {
        if (pipe(pipeOut) == -1 || pipe(pipeErr) == -1)
        {
            result.success = false;
            result.error = "Failed to create pipes";
            return result;
        }
    }

    // Everything the child needs is built before fork(): only async-signal-safe
    // calls are valid between fork() and exec in a multithreaded process.
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (const std::string& arg : command)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const std::string workingDirString = wd.string();
    std::vector<char*> envp;
    envp.reserve(environmentEntries->size() + 1);
    for (const std::string& entry : *environmentEntries)
        envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1)
    {
        result.success = false;
        result.error = "Failed to fork process";
        if (captureOutput)
        {
            close(pipeOut[0]);
            close(pipeOut[1]);
            close(pipeErr[0]);
            close(pipeErr[1]);
        }
        return result;
    }

    if (pid == 0)
    {
        // Child process
        if (captureOutput)
        {
            close(pipeOut[0]);
            close(pipeErr[0]);
            dup2(pipeOut[1], STDOUT_FILENO);
            dup2(pipeErr[1], STDERR_FILENO);
            close(pipeOut[1]);
            close(pipeErr[1]);
        }

        if (chdir(workingDirString.c_str()) != 0)
        {
            _exit(1);
        }

        execve(argv[0], argv.data(), envp.data());
        _exit(1);
    }
    else
    {
        // Parent process
        if (captureOutput)
        {
            close(pipeOut[1]);
            close(pipeErr[1]);
        }

        std::string output;
        std::string errorOutput;

        if (captureOutput)
        {
            int status = 0;
            pid_t waited = 0;
            while (true)
            {
                waited = waitpid(pid, &status, WNOHANG);
                if (waited == -1 && errno == EINTR)
                    continue;
                if (waited != 0)
                    break;
                ReadAvailablePipes(pipeOut[0], pipeErr[0], output, errorOutput, kPipePollTimeoutMs);
            }
            // What the child wrote just before it exited is still in the pipes.
            while (ReadAvailablePipes(pipeOut[0], pipeErr[0], output, errorOutput, 0))
            {
            }
            result.exitCode = (waited == pid && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;

            close(pipeOut[0]);
            close(pipeErr[0]);
        }
        else
        {
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status))
            {
                result.exitCode = WEXITSTATUS(status);
            }
            else
            {
                result.exitCode = -1;
            }
        }

        result.success = (result.exitCode == 0);
        result.output = output;
        if (!errorOutput.empty())
        {
            result.error = errorOutput;
        }
    }
#endif // _WIN32

    return result;
}

} // namespace

} // namespace GameEngine
