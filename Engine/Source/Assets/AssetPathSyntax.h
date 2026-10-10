#pragma once
#include "Assets/AssetSourceAlias.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

// Shared source-private spelling of authored asset source URLs.
namespace GameEngine::AssetPathSyntax
{
inline std::filesystem::path NormalizeRelativeAssetPath(std::filesystem::path relativeAssetPath)
{
    // On Windows a path beginning with a separator has a root-directory but no
    // root-name, so is_absolute() is false; joining it makes `source.Root /
    // relativePath` replace everything after the drive (C:/Package/Assets plus
    // /Textures/foo.png becomes C:/Textures/foo.png). Rooted spellings reach
    // this normalization from every resolution route (the inline `package:/x`
    // split and both ResolveAssetPath overloads), and genuinely absolute paths
    // early-return in the callers before getting here — so in this context any
    // root portion is source-relative noise. Strip it.
    if (relativeAssetPath.has_root_path())
        relativeAssetPath = relativeAssetPath.relative_path();
    if (relativeAssetPath.empty())
        return relativeAssetPath;

    // Accept either "Icons/Foo.png" or "Assets/Icons/Foo.png" for any Assets-root source.
    // This prevents accidentally resolving to "<...>/Assets/Assets/...".
    auto it = relativeAssetPath.begin();
    if (it == relativeAssetPath.end())
        return relativeAssetPath;

    std::string first = it->string();
    for (auto& c : first)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (first != "assets")
        return relativeAssetPath;

    std::filesystem::path stripped;
    ++it;
    for (; it != relativeAssetPath.end(); ++it)
    {
        stripped /= *it;
    }
    return stripped.empty() ? relativeAssetPath : stripped;
}

inline bool TrySplitAssetSourcePrefix(const std::filesystem::path& assetPath,
                                      std::string& outSourceAlias,
                                      std::filesystem::path& outRelativeAssetPath)
{
    outSourceAlias.clear();
    outRelativeAssetPath.clear();
    if (assetPath.empty() || assetPath.is_absolute())
        return false;

    const std::string ref = assetPath.generic_string();
    if (ref.empty())
        return false;

    const size_t colon = ref.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= ref.size())
        return false;

    // Ignore drive-letter forms such as "C:foo" (Windows).
    if (colon == 1 && std::isalpha(static_cast<unsigned char>(ref[0])))
        return false;

    outSourceAlias = NormalizeAssetSourceAlias(ref.substr(0, colon));
    // The remainder may be rooted (`package:/Textures/foo.png` is the canonical
    // alias URL spelling); NormalizeRelativeAssetPath strips the root portion.
    outRelativeAssetPath = NormalizeRelativeAssetPath(std::filesystem::path(ref.substr(colon + 1)));
    return IsValidAssetSourceAlias(outSourceAlias) && !outRelativeAssetPath.empty();
}
} // namespace GameEngine::AssetPathSyntax
