#include "FileSystem/FileSystem.h"

#include "Logger/Logger.h"

#include <fstream>

// Copying and publishing bytes, with no thread pool and no platform layer
// behind them. That is what lets a test executable for a module that publishes
// files link these objects on their own (FileSystemObjects in the module's
// CMakeLists); the rest of this module cannot be linked that cheaply.
namespace GameEngine::FileSystem
{

namespace
{

bool StreamFile(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(from, ec) || ec)
        return false;
    ec.clear();
    if (std::filesystem::equivalent(from, to, ec))
        return false;
    const std::filesystem::path parent = to.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            LOG_WARNING("FileSystem: mkdir '{}' failed ({})", parent.string(), ec.message());
            return false;
        }
    }

    std::ifstream in(from, std::ios::binary);
    if (!in)
        return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LOG_WARNING("FileSystem: copy '{}' -> '{}' failed to open", from.string(), to.string());
        return false;
    }
    // Insert the source only when it has content. `out << in.rdbuf()` sets failbit
    // when the source stream is empty (nothing is extracted), which would misreport
    // a valid empty-file copy as a write failure; the truncated `out` is already the
    // correct empty result in that case.
    if (in.peek() != std::ifstream::traits_type::eof())
    {
        out << in.rdbuf();
        if (!out)
        {
            LOG_WARNING("FileSystem: copy '{}' -> '{}' failed to write", from.string(), to.string());
            return false;
        }
    }
    out.flush();
    return !in.bad() && out.good();
}

} // namespace

bool CopyFileContents(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (from.empty() || to.empty())
        return false;
    return StreamFile(from, to);
}

bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (from.empty() || to.empty())
        return false;

    std::error_code ec;
    if (std::filesystem::is_regular_file(from, ec))
        return StreamFile(from, to);
    if (!std::filesystem::is_directory(from, ec))
    {
        LOG_WARNING("FileSystem: copy source '{}' is missing", from.string());
        return false;
    }

    std::filesystem::create_directories(to, ec);
    if (ec)
    {
        LOG_WARNING("FileSystem: mkdir '{}' failed ({})", to.string(), ec.message());
        return false;
    }

    bool ok = true;
    for (const auto& entry : std::filesystem::directory_iterator(from, ec))
    {
        const std::filesystem::path target = to / entry.path().filename();
        if (entry.is_directory())
            ok = CopyTree(entry.path(), target) && ok;
        else
            ok = StreamFile(entry.path(), target) && ok;
    }
    if (ec)
    {
        LOG_WARNING("FileSystem: reading '{}' failed ({})", from.string(), ec.message());
        return false;
    }
    return ok;
}

// No rename: WASMFS's renameat is the one filesystem call that takes directory
// locks child before parent, so it deadlocks against any concurrent path lookup
// in the same tree (see the header). Copying the bytes and dropping the temp
// touches one directory lock at a time, which every other WASMFS call does too.
bool PublishFile(const std::filesystem::path& temp, const std::filesystem::path& target)
{
    std::error_code sameFileError;
    if (std::filesystem::equivalent(temp, target, sameFileError))
        return true; // A no-op publication must never remove its destination.
    const bool copied = CopyFileContents(temp, target);
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    if (!copied)
        LOG_WARNING("FileSystem: could not publish '{}' as '{}'", temp.string(), target.string());
    return copied;
}

} // namespace GameEngine::FileSystem
