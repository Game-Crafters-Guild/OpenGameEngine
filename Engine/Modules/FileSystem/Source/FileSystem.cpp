#include "FileSystem/FileSystem.h"

#include "Logger/Logger.h"
#include "Platform/Process.h"

#include <atomic>
#include <sstream>
#include <thread>

namespace GameEngine::FileSystem
{
std::filesystem::path MakeTemporarySiblingPath(const std::filesystem::path& target)
{
    static std::atomic<uint64_t> counter{0};
    std::ostringstream suffix;
    suffix << Platform::GetCurrentProcessId() << '-' << std::this_thread::get_id()
           << '-' << counter.fetch_add(1, std::memory_order_relaxed);
    auto filename = target.filename();
    filename += "." + suffix.str() + ".tmp";
    return target.parent_path() / filename;
}

std::vector<std::filesystem::path> ListDirectories(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> directories;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        std::error_code typeEc;
        if (entry.is_directory(typeEc))
            directories.push_back(entry.path());
    }
    if (ec)
        LOG_WARNING("FileSystem: listing '{}' failed ({})", root.string(), ec.message());
    return directories;
}

} // namespace GameEngine::FileSystem
