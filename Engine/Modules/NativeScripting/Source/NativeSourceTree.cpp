#include "NativeScripting/NativeSourceTree.h"

#include "AssetCore/AssetIgnoreRules.h"
#include "NativeScripting/BuildCacheRecord.h" // Fnv1aHash

#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace GameEngine
{
namespace NativeScripting
{

namespace
{

// The build pipeline copies its Player template sources to Assets/Source/ on
// every packaged build and compiles them into the Player EXE; the generated user
// project filters them out of the module the same way. They are not user code.
bool IsPlayerTemplateSource(const std::filesystem::path& relative)
{
    auto segment = relative.begin();
    if (segment == relative.end() || *segment != "Source")
        return false;
    ++segment;
    if (segment == relative.end() || std::next(segment) != relative.end())
        return false; // only the template files directly under Source/
    const std::string stem = relative.stem().string();
    return stem == "main" || stem == "PlayerApplication";
}

// Calls `visit(absolutePath, rootRelativePath)` for every watched native source
// under `sourceRoot`, skipping the ignored directories whole (with the Tests folder
// of `packageRoot`, when set). `visit` returns false to end the walk.
template <typename Visitor>
void ForEachWatchedNativeSource(const std::filesystem::path& sourceRoot, const std::filesystem::path& packageRoot,
                                Visitor&& visit)
{
    std::error_code rootEc;
    if (sourceRoot.empty() || !std::filesystem::is_directory(sourceRoot, rootEc))
        return;

    AssetIgnoreRules ignoreRules = AssetIgnoreRules::LoadForAssetRoot(sourceRoot);
    ignoreRules.IgnorePackageTestsFolder(sourceRoot, packageRoot);

    std::error_code walkEc;
    const std::filesystem::recursive_directory_iterator end;
    for (auto it = std::filesystem::recursive_directory_iterator(
             sourceRoot, std::filesystem::directory_options::skip_permission_denied, walkEc);
         !walkEc && it != end; it.increment(walkEc))
    {
        const std::filesystem::path& path = it->path();

        std::error_code entryEc;
        if (it->is_directory(entryEc))
        {
            if (ignoreRules.ShouldIgnoreDirectory(path, sourceRoot))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(entryEc) || !IsWatchedNativeSourceExtension(path))
            continue;

        // Lexical: the iterator built `path` by appending to `sourceRoot`, so the
        // remainder is exact and the file-level rules cost no filesystem call.
        const std::filesystem::path relative = path.lexically_relative(sourceRoot);
        if (ignoreRules.ShouldIgnoreCanonicalRelativePath(relative.generic_string()))
            continue;

        if (!visit(path, relative))
            return;
    }
}

} // namespace

bool IsWatchedNativeSourceExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c" ||
           extension == ".h" || extension == ".hpp" || extension == ".hxx" || extension == ".hh";
}

bool HasWatchedNativeSource(const std::filesystem::path& sourceRoot)
{
    bool found = false;
    ForEachWatchedNativeSource(
        sourceRoot, {}, [&found](const std::filesystem::path&, const std::filesystem::path& relative) {
            if (IsPlayerTemplateSource(relative))
                return true;
            found = true;
            return false;
        });
    return found;
}

std::uint64_t HashWatchedNativeSources(const std::filesystem::path& sourceRoot,
                                       const std::filesystem::path& packageRoot, std::uint64_t seed)
{
    std::vector<std::string> entries;
    ForEachWatchedNativeSource(
        sourceRoot, packageRoot, [&entries](const std::filesystem::path& path, const std::filesystem::path& relative) {
            // A per-file error_code so one bad stat does not abort the whole walk.
            std::error_code ec;
            const auto mtime = static_cast<long long>(
                std::filesystem::last_write_time(path, ec).time_since_epoch().count());
            const auto size = static_cast<std::uintmax_t>(std::filesystem::file_size(path, ec));
            std::ostringstream text;
            text << relative.generic_string() << ':' << mtime << ':' << size;
            entries.push_back(text.str());
            return true;
        });

    std::sort(entries.begin(), entries.end());
    std::uint64_t hash = seed;
    for (const std::string& entry : entries)
        hash = Fnv1aHash(entry, hash);
    return hash;
}

} // namespace NativeScripting
} // namespace GameEngine
