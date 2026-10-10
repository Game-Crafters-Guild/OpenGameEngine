#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

// The spelling rule for asset source aliases (AssetSourceDesc::Alias): the form
// a mounted source is registered under and an "alias:path" reference names.
namespace GameEngine::AssetPathSyntax
{
/// The spelling a registered source keeps: surrounding whitespace trimmed,
/// lower-cased (AssetManager::RegisterSource stores this form).
inline std::string NormalizeAssetSourceAlias(std::string_view alias)
{
    size_t begin = 0;
    size_t end = alias.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(alias[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(alias[end - 1])))
        --end;
    std::string out(alias.substr(begin, end - begin));
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

/// True for a normalized alias that is non-empty and uses only letters,
/// digits, '-' and '_'; AssetManager::RegisterSource refuses any other.
inline bool IsValidAssetSourceAlias(std::string_view alias)
{
    if (alias.empty())
        return false;
    for (char c : alias)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-'))
            return false;
    }
    return true;
}
} // namespace GameEngine::AssetPathSyntax
