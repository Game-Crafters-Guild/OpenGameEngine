#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace GameEngine::FileSystem
{

/// A process-shared advisory lock on a stable sidecar, held for the object's lifetime.
/// Exclusive/blocking is the default for read-merge-write callers. Shared locks
/// pin resources; nonblocking exclusive locks can claim them for eviction.
/// Check IsLocked() before relying on exclusion; acquisition can fail.
///
/// A platform whose process is the only one that can reach the file (the
/// browser origin) takes no lock at all: its filesystem's flock is not a
/// cross-agent lock, but it does block, and a sidecar left by a previous
/// session would then park the boot forever with no error.
class ScopedFileLock
{
  public:
    enum class Mode { Exclusive, Shared };
    explicit ScopedFileLock(const std::filesystem::path& lockFile,
                            Mode mode = Mode::Exclusive, bool wait = true);
    ~ScopedFileLock();
    /// False on unsupported platforms or acquisition failure; destructive callers must fail closed.
    bool IsLocked() const { return m_Locked; }
    /// Why acquisition failed, naming the step and the system's error; empty while locked.
    const std::string& FailureReason() const { return m_FailureReason; }

    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;

  private:
    bool m_Locked = false;
    std::string m_FailureReason;
#if defined(_WIN32)
    HANDLE m_Handle = INVALID_HANDLE_VALUE;
#else
    int m_Fd = -1;
#endif
};

} // namespace GameEngine::FileSystem
