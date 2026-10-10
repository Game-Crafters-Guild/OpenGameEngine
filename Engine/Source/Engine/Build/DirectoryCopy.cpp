#include "Engine/Build/DirectoryCopy.h"

namespace fs = std::filesystem;

namespace GameEngine {

bool CopyDirectoryKeepingLinks(const fs::path& from,
                               const fs::path& to,
                               std::error_code& ec,
                               const std::function<bool()>& shouldCancel,
                               const std::function<bool(const fs::path&)>& shouldSkip)
{
    ec.clear();
    fs::create_directories(to, ec);
    if (ec)
        return false;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec))
    {
        if (shouldCancel && shouldCancel())
            return false;
        // Lexical, not fs::relative: that resolves links, which maps a link
        // onto its target's path.
        const fs::path relative = it->path().lexically_relative(from);
        if (shouldSkip && shouldSkip(relative))
        {
            it.disable_recursion_pending();
            continue;
        }
        const fs::path target = to / relative;
        const fs::file_status status = it->symlink_status(ec);
        if (ec)
            return false;
        if (fs::is_symlink(status))
        {
            fs::copy_symlink(it->path(), target, ec);
            it.disable_recursion_pending();
        }
        else if (fs::is_directory(status))
        {
            fs::create_directory(target, ec);
        }
        else
        {
            fs::copy_file(it->path(), target, ec);
        }
        if (ec)
            return false;
    }
    return !ec;
}

} // namespace GameEngine
