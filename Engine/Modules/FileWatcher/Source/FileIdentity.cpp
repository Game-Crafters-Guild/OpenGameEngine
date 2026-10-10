#include "FileWatcher/FileIdentity.h"

#include <cstdio>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace GameEngine
{

std::string FileIdentity::ToString() const
{
    if (!Valid)
        return {};
#ifdef PLATFORM_WINDOWS
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%08X-%016llX", static_cast<unsigned>(Volume),
                  static_cast<unsigned long long>(Index));
    return std::string(buf);
#else
    return std::to_string(Volume) + "-" + std::to_string(Index);
#endif
}

FileIdentity ReadFileIdentity(const std::filesystem::path& path)
{
    FileIdentity identity{};
#ifdef PLATFORM_WINDOWS
    HANDLE h = CreateFileW(
        path.wstring().c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return identity;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != 0;
    CloseHandle(h);
    if (!ok)
        return identity;

    identity.Volume = info.dwVolumeSerialNumber;
    identity.Index = (static_cast<uint64>(info.nFileIndexHigh) << 32) | static_cast<uint64>(info.nFileIndexLow);
#else
    struct stat st;
    if (stat(path.string().c_str(), &st) != 0)
        return identity;
    identity.Volume = static_cast<uint64>(st.st_dev);
    identity.Index = static_cast<uint64>(st.st_ino);
#endif
    identity.Valid = true;
    return identity;
}

} // namespace GameEngine
