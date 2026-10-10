#include "AssetDatabase/AssetDatabasePaths.h"

#include <algorithm>
#include <cctype>

namespace GameEngine::AssetDatabase
{

namespace
{
static std::filesystem::path TrimTrailingSeparators(std::filesystem::path p)
{
    std::filesystem::path prev;
    while (!p.empty() && p.filename().empty())
    {
        prev = p;
        p = p.parent_path();
        if (p == prev)
            break;
    }
    return p;
}
} // namespace

AssetDatabasePaths GetDefaultPathsForAssetRoot(const std::filesystem::path& assetRoot,
                                               const std::filesystem::path& authoritativeFileOverride,
                                               const std::filesystem::path& cacheRootOverride)
{
    AssetDatabasePaths out{};

    // Convention:
    // - If assetRoot is "<ProjectRoot>/Assets", place authoritative file at ProjectRoot.
    // - Otherwise, treat assetRoot itself as the project root (tests/tools often pass a temp root directly).
    const std::filesystem::path root = TrimTrailingSeparators(assetRoot.lexically_normal());

    std::filesystem::path projectRoot = root;
    if (!root.empty())
    {
        const std::string leaf = root.filename().string();
        std::string lower = leaf;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower == "assets")
        {
            projectRoot = root.parent_path();
        }
        else
        {
            projectRoot = root;
        }
    }

    out.projectRoot = projectRoot;
    if (!authoritativeFileOverride.empty())
    {
        out.authoritativeFile = authoritativeFileOverride.is_absolute()
                                    ? authoritativeFileOverride
                                    : (projectRoot / authoritativeFileOverride);
    }
    else
    {
        out.authoritativeFile = projectRoot / "AssetDatabase.assetdb";
    }

    if (!cacheRootOverride.empty())
    {
        out.cacheRoot = cacheRootOverride.is_absolute()
                            ? cacheRootOverride
                            : (projectRoot / cacheRootOverride);
    }
    else
    {
        // Align with the engine-wide cache convention.
        out.cacheRoot = projectRoot / ".Cache" / "AssetDatabase";
    }
    out.sqliteCacheFile = out.cacheRoot / "AssetDbCache.sqlite";

    out.editorRoot = projectRoot / ".Editor";
    out.thumbnailsRoot = out.editorRoot / "Thumbnails";

    return out;
}

} // namespace GameEngine::AssetDatabase


