#include "FileSystem/FileSystem.h"

#include "FileSystem/RenameWithRetry.h"

#include "Logger/Logger.h"

// Copying and publishing bytes, with no thread pool and no platform layer
// behind them. That is what lets a test executable for a module that publishes
// files link these objects on their own (FileSystemObjects in the module's
// CMakeLists); the rest of this module cannot be linked that cheaply.
namespace GameEngine::FileSystem
{

bool CopyFileContents(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (from.empty() || to.empty())
        return false;
    std::error_code ec;
    const auto parent = to.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (!ec)
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        LOG_WARNING("FileSystem: copy '{}' -> '{}' failed ({})", from.string(), to.string(), ec.message());
    return !ec;
}

bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (from.empty() || to.empty())
        return false;
    std::error_code ec;
    const auto parent = to.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (!ec)
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive |
                                        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        LOG_WARNING("FileSystem: copy '{}' -> '{}' failed ({})", from.string(), to.string(), ec.message());
    return !ec;
}

bool PublishFile(const std::filesystem::path& temp, const std::filesystem::path& target)
{
    std::error_code ec;
    if (RenameWithRetry(temp, target, {}, ec))
        return true;

    // Publication must preserve the previous complete destination if the
    // replacement fails. Copying over it in place would truncate it before a
    // later write error; these callers already stage beside the destination,
    // so no cross-volume fallback is needed.
    std::error_code removeEc;
    std::filesystem::remove(temp, removeEc);
    LOG_WARNING("FileSystem: could not publish '{}' as '{}' ({})", temp.string(),
                target.string(), ec.message());
    return false;
}

} // namespace GameEngine::FileSystem
