#include "FileSystem/FileSystem.h"

#include "Platform/Process.h"
#include "Platform/Shell.h"

#include <charconv>
#include <string>
#include <system_error>

namespace GameEngine::FileSystem
{
namespace
{

// The writing process of a temporary sibling named <target>.<pid>-<thread>-<counter>.tmp
// (MakeTemporarySiblingPath), or -1 when the name has another shape. `targetSuffix` must end the
// target's own name.
int64_t TemporaryWriterProcess(const std::string& name, std::string_view targetSuffix)
{
    constexpr std::string_view kTemporaryExtension = ".tmp";
    if (name.size() <= kTemporaryExtension.size() ||
        name.compare(name.size() - kTemporaryExtension.size(), kTemporaryExtension.size(), kTemporaryExtension) != 0)
        return -1;
    const std::string stem = name.substr(0, name.size() - kTemporaryExtension.size());
    const std::size_t dot = stem.rfind('.');
    if (dot == std::string::npos || dot < targetSuffix.size() ||
        stem.compare(dot - targetSuffix.size(), targetSuffix.size(), targetSuffix) != 0)
        return -1;
    int64_t pid = -1;
    const char* first = stem.data() + dot + 1;
    const char* last = stem.data() + stem.size();
    const auto [end, error] = std::from_chars(first, last, pid);
    if (error != std::errc{} || end == first || end == last || *end != '-')
        return -1;
    return pid;
}

} // namespace

std::size_t RemoveOrphanedTemporaryFiles(const std::filesystem::path& directory, std::string_view targetSuffix)
{
    const int64_t self = static_cast<int64_t>(Platform::GetCurrentProcessId());
    std::size_t removed = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        const int64_t writer = TemporaryWriterProcess(entry.path().filename().string(), targetSuffix);
        if (writer < 0 || writer == self || Platform::IsProcessRunning(static_cast<int>(writer)))
            continue;
        std::error_code removeEc;
        if (std::filesystem::remove(entry.path(), removeEc))
            ++removed;
    }
    return removed;
}

} // namespace GameEngine::FileSystem
