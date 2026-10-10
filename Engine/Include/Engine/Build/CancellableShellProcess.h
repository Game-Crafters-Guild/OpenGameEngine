#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine {

namespace Platform {
struct EnvironmentEdit;
}

// Result of running a shell command through CancellableShellProcess.
struct ShellProcessResult
{
    int         exitCode = -1;  // process exit code; -1 if the process never started
    std::string output;         // shell Run and RunProcessCaptured only: merged child
                                // output (stderr is included when the caller
                                // redirects with "2>&1"); the argv Run leaves it empty
    bool        cancelled = false;  // true if the child was terminated by cancellation
    std::string stderrTail;     // argv Run only: the last kProcessStderrTailBytes of stderr
    std::size_t overlongStdoutLines = 0;  // argv Run only: stdout lines dropped for
                                          // exceeding kMaxProcessStdoutLineBytes
};

/// The largest stdin payload the argv Run() accepts: 10 MiB. A larger payload is
/// refused before anything is spawned.
inline constexpr std::size_t kMaxProcessStdinBytes = 10u * 1024u * 1024u;
/// The longest stdout line the argv Run() hands to its line callback: 1 MiB. A longer
/// line is dropped whole and counted in ShellProcessResult::overlongStdoutLines, never
/// cut, so a line-framed parser never sees a truncated record.
inline constexpr std::size_t kMaxProcessStdoutLineBytes = 1024u * 1024u;
/// How much of the child's stderr the argv Run() keeps: the last 16 KiB.
inline constexpr std::size_t kProcessStderrTailBytes = 16u * 1024u;

/// What the argv overload of CancellableShellProcess::Run() starts.
struct ProcessLaunch
{
    /// The program to run. Without a path separator, Windows searches the editor's
    /// directory, the current directory, the system directories and then PATH, and
    /// appends only ".exe" (so an npm ".cmd" shim is not found); POSIX searches PATH.
    std::string Executable;
    /// Passed to the child verbatim, one argv entry each; no shell parses them.
    std::vector<std::string> Arguments;
    /// The child's working directory; empty inherits this process's.
    std::filesystem::path WorkingDirectory;
    /// Applied in order to this process's environment to form the child's; a name
    /// that is empty or holds '=' refuses the launch.
    std::vector<Platform::EnvironmentEdit> Environment;
    /// Written to the child's stdin, which is then closed; empty gives the child an
    /// immediate end of input. At most kMaxProcessStdinBytes.
    std::string StdinPayload;
};

// Runs an executable directly (no shell), captures merged stdout/stderr, and
// waits for it to exit. The executable is resolved by the platform when it does
// not contain a path separator.
//
// Arguments are passed to the child verbatim (no shell parsing), so paths and
// other values containing spaces or shell metacharacters need no quoting and
// cannot be interpreted as commands.
//
// If `timeout` is non-zero and the child has not exited by then, it is forcibly
// terminated; the returned result has `cancelled == true` and a non-zero
// `exitCode`. A zero timeout (the default) waits indefinitely.
ShellProcessResult RunProcessCaptured(const std::string& executable,
                                      const std::vector<std::string>& arguments,
                                      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

// Runs a child process with live output streaming and cooperative cancellation:
// a shell command (editor build steps, cmake/dotnet; one instance backs one
// BuildPipeline) or an executable with literal arguments, environment edits and
// a stdin payload (the AI Assistant's CLI sessions). Run() executes on the
// caller's worker thread and blocks until the child exits; Kill() may be called
// concurrently from any thread (e.g. a UI "Cancel" button) to terminate the
// in-flight child.
class CancellableShellProcess
{
public:
    CancellableShellProcess() = default;
    ~CancellableShellProcess();

    CancellableShellProcess(const CancellableShellProcess&) = delete;
    CancellableShellProcess& operator=(const CancellableShellProcess&) = delete;

    // Execute `command` via the platform shell (cmd /C on Windows, /bin/sh -c on
    // POSIX), capturing merged output. `onLine`, when set, is invoked once per
    // completed output line (without the trailing newline). `shouldCancel` is
    // polled periodically; when it returns true — or Kill() is called — the child
    // is terminated and the result holds whatever output was captured so far.
    ShellProcessResult Run(const std::string& command,
                           std::function<bool()> shouldCancel,
                           std::function<void(const std::string& line)> onLine);

    /// Runs `launch.Executable` directly (no shell) and blocks until it exits. Each
    /// completed stdout line, without its line ending ("\n" or "\r\n"), goes to
    /// `onLine` and is not kept; stderr never reaches `onLine` and is kept as the
    /// result's `stderrTail`. Lines longer than kMaxProcessStdoutLineBytes
    /// are dropped and counted. `shouldCancel` and Kill() stop the child as for the
    /// shell Run(). A payload above kMaxProcessStdinBytes, an invalid environment
    /// edit, or a failed spawn (a missing executable or working directory) returns
    /// `exitCode == -1` without running anything.
    ShellProcessResult Run(const ProcessLaunch& launch,
                           std::function<bool()> shouldCancel,
                           std::function<void(const std::string& line)> onLine);

    // Terminate the currently-running child, if any. Thread-safe; a no-op when
    // no child is running.
    void Kill();

private:
    void TerminateChildLocked();  // call with m_HandleMutex held

    std::mutex        m_HandleMutex;        // guards the live child handle/pid
    std::atomic<bool> m_KillRequested{false};

#if defined(_WIN32)
    void* m_ProcessHandle = nullptr;        // HANDLE; nullptr when idle
#else
    int   m_Pid = -1;                       // child pid; -1 when idle
#endif
};

} // namespace GameEngine
