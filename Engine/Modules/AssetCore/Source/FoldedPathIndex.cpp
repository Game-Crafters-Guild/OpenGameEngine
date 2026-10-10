#include "AssetCore/FoldedPathIndex.h"

#include "Types/StringUtils.h"

#include <system_error>

namespace GameEngine::AssetPaths
{

std::unordered_map<std::string, std::filesystem::path> BuildFoldedPathIndex(
    const std::filesystem::path& root)
{
    std::unordered_map<std::string, std::filesystem::path> index;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, ec), end;
    for (; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const std::filesystem::path rel = std::filesystem::relative(it->path(), root, ec);
        if (ec || rel.empty())
            continue;
        index.emplace(ToLowerAscii(rel.generic_string()), it->path());
    }
    return index;
}

} // namespace GameEngine::AssetPaths
