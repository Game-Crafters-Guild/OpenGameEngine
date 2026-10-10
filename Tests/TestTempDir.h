#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <unistd.h>
#endif

namespace GameEngine::TestUtils
{

inline std::uint64_t GetProcessIdForTests()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

/// Unique, not-yet-created directory under a **canonical** temp root.
///
/// Asset mount roots keep the spelling they were registered with, while the
/// Engine resolves its workspace root through weakly_canonical; a test that
/// registered a raw temp path and compared an expectation built from it
/// against a resolved path would compare two spellings of one directory and
/// disagree. Handing out the canonical spelling keeps every such comparison
/// a plain string compare. On macOS this is the default, not an edge case —
/// temp_directory_path() reports /var/folders/…, a symlink to
/// /private/var/folders/….
inline std::filesystem::path MakeUniqueTempDirectory(std::string_view prefix)
{
    namespace fs = std::filesystem;
    const std::uint64_t pid = GetProcessIdForTests();
    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    std::string name;
    name.reserve(prefix.size() + 64);
    name.append(prefix.begin(), prefix.end());
    name.push_back('_');
    name += std::to_string(pid);
    name.push_back('_');
    name += std::to_string(stamp);

    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec)
        base = fs::path("/tmp");

    // canonical() on the existing temp root, not weakly_canonical() on the
    // full path: the leaf must stay uncreated for callers that expect to
    // create it themselves.
    ec.clear();
    if (fs::path resolved = fs::canonical(base, ec); !ec && !resolved.empty())
        base = std::move(resolved);

    return base / name;
}

/// A directory that exists for the lifetime of the object: created (with its parents) on
/// construction, removed recursively on destruction. Pair with MakeUniqueTempDirectory() for
/// a per-process scratch tree.
class ScopedTempDir
{
  public:
    explicit ScopedTempDir(std::filesystem::path path)
        : m_Path(std::move(path))
    {
        std::error_code ec;
        std::filesystem::create_directories(m_Path, ec);
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Path, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& Path() const { return m_Path; }

  private:
    std::filesystem::path m_Path;
};

} // namespace GameEngine::TestUtils
