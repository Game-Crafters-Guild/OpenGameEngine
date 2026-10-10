#include "AssetCore/SharedFileRead.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>

namespace GameEngine
{

SharedFileReader::SharedFileReader(const std::filesystem::path& path)
{
    Open(path);
}

SharedFileReader::~SharedFileReader()
{
    Close();
}

SharedFileReader::SharedFileReader(SharedFileReader&& other) noexcept
{
    *this = std::move(other);
}

SharedFileReader& SharedFileReader::operator=(SharedFileReader&& other) noexcept
{
    if (this != &other)
    {
        Close();
#ifdef _WIN32
        m_Handle = other.m_Handle;
        other.m_Handle = nullptr;
#else
        m_Fd = other.m_Fd;
        other.m_Fd = -1;
#endif
    }
    return *this;
}

#ifdef _WIN32

bool SharedFileReader::Open(const std::filesystem::path& path)
{
    Close();
    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    m_Handle = h;
    return true;
}

void SharedFileReader::Close()
{
    if (m_Handle)
    {
        CloseHandle(static_cast<HANDLE>(m_Handle));
        m_Handle = nullptr;
    }
}

bool SharedFileReader::IsOpen() const
{
    return m_Handle != nullptr;
}

int64 SharedFileReader::Size() const
{
    if (!m_Handle)
    {
        return -1;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(static_cast<HANDLE>(m_Handle), &size))
    {
        return -1;
    }
    return static_cast<int64>(size.QuadPart);
}

bool SharedFileReader::SeekTo(uint64 offset)
{
    if (!m_Handle)
    {
        return false;
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(static_cast<HANDLE>(m_Handle), pos, nullptr, FILE_BEGIN) != 0;
}

int64 SharedFileReader::Read(void* dst, uint64 bytes)
{
    if (!m_Handle)
    {
        return -1;
    }
    uint8* out = static_cast<uint8*>(dst);
    uint64 total = 0;
    while (total < bytes)
    {
        const DWORD chunk = static_cast<DWORD>(
            std::min<uint64>(bytes - total, 64ull * 1024ull * 1024ull));
        DWORD got = 0;
        if (!::ReadFile(static_cast<HANDLE>(m_Handle), out + total, chunk, &got, nullptr))
        {
            return -1;
        }
        if (got == 0)
        {
            break; // EOF
        }
        total += got;
    }
    return static_cast<int64>(total);
}

#else // POSIX

bool SharedFileReader::Open(const std::filesystem::path& path)
{
    Close();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        ::close(fd);
        return false;
    }
    m_Fd = fd;
    return true;
}

void SharedFileReader::Close()
{
    if (m_Fd >= 0)
    {
        ::close(m_Fd);
        m_Fd = -1;
    }
}

bool SharedFileReader::IsOpen() const
{
    return m_Fd >= 0;
}

int64 SharedFileReader::Size() const
{
    if (m_Fd < 0)
    {
        return -1;
    }
    struct stat st{};
    if (fstat(m_Fd, &st) != 0)
    {
        return -1;
    }
    return static_cast<int64>(st.st_size);
}

bool SharedFileReader::SeekTo(uint64 offset)
{
    if (m_Fd < 0)
    {
        return false;
    }
    return ::lseek(m_Fd, static_cast<off_t>(offset), SEEK_SET) >= 0;
}

int64 SharedFileReader::Read(void* dst, uint64 bytes)
{
    if (m_Fd < 0)
    {
        return -1;
    }
    uint8* out = static_cast<uint8*>(dst);
    uint64 total = 0;
    while (total < bytes)
    {
        const size_t chunk = static_cast<size_t>(
            std::min<uint64>(bytes - total, 64ull * 1024ull * 1024ull));
        const ssize_t got = ::read(m_Fd, out + total, chunk);
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (got == 0)
        {
            break; // EOF
        }
        total += static_cast<uint64>(got);
    }
    return static_cast<int64>(total);
}

#endif

namespace
{
template <typename Container>
bool ReadWholeFileShared(const std::filesystem::path& path, Container& out)
{
    out.clear();

    SharedFileReader reader(path);
    if (!reader.IsOpen())
    {
        return false;
    }

    const int64 size = reader.Size();
    if (size < 0)
    {
        return false;
    }
    if (size == 0)
    {
        return true;
    }

    // Size is a snapshot; the read is authoritative. A file replaced while we
    // hold the handle keeps serving the displaced bytes (see header contract),
    // but an in-place writer can still shrink or grow it under us.
    out.resize(static_cast<size_t>(size));
    const int64 got = reader.Read(out.data(), static_cast<uint64>(size));
    if (got < 0)
    {
        out.clear();
        return false;
    }
    if (got != size)
    {
        out.resize(static_cast<size_t>(got));
    }
    return true;
}
} // namespace

bool ReadFileBytesShared(const std::filesystem::path& path, Vector<uint8>& outBytes)
{
    return ReadWholeFileShared(path, outBytes);
}

bool ReadFileTextShared(const std::filesystem::path& path, String& outText)
{
    return ReadWholeFileShared(path, outText);
}

} // namespace GameEngine
