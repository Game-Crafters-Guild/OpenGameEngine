#include "FileSystem/ScopedFileLock.h"

#include <cerrno>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace GameEngine::FileSystem
{

ScopedFileLock::ScopedFileLock(const std::filesystem::path& lockFile, Mode mode, bool wait)
{
#if defined(_WIN32)
    m_Handle = ::CreateFileW(lockFile.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_Handle == INVALID_HANDLE_VALUE)
    {
        m_FailureReason = "could not open '" + lockFile.string() + "': " +
                          std::system_category().message(static_cast<int>(::GetLastError()));
    }
    else
    {
        OVERLAPPED ov{};
        const DWORD flags = (mode == Mode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
                            (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
        if (!::LockFileEx(m_Handle, flags, 0, 1, 0, &ov))
        {
            m_FailureReason = "could not lock '" + lockFile.string() + "': " +
                              std::system_category().message(static_cast<int>(::GetLastError()));
            ::CloseHandle(m_Handle);
            m_Handle = INVALID_HANDLE_VALUE;
        }
    }
#else
    m_Fd = ::open(lockFile.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    const int flags = (mode == Mode::Exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB);
    if (m_Fd < 0)
    {
        m_FailureReason = "could not open '" + lockFile.string() + "': " + std::generic_category().message(errno);
    }
    else if (::flock(m_Fd, flags) != 0)
    {
        m_FailureReason = "could not lock '" + lockFile.string() + "': " + std::generic_category().message(errno);
        ::close(m_Fd);
        m_Fd = -1;
    }
#endif
#if defined(_WIN32)
    m_Locked = m_Handle != INVALID_HANDLE_VALUE;
#else
    m_Locked = m_Fd >= 0;
#endif
}

ScopedFileLock::~ScopedFileLock()
{
#if defined(_WIN32)
    if (m_Handle != INVALID_HANDLE_VALUE)
    {
        OVERLAPPED ov{};
        ::UnlockFileEx(m_Handle, 0, 1, 0, &ov);
        ::CloseHandle(m_Handle);
    }
#else
    if (m_Fd >= 0)
        ::close(m_Fd);
#endif
}

} // namespace GameEngine::FileSystem
