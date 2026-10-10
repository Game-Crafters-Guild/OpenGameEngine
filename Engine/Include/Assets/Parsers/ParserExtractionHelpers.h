#pragma once

// Internal helpers shared by per-parser ExtractDependencies implementations.
// Keep tiny and inline-able — these run on the asset-scan hot path.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace GameEngine::ParserExtraction
{

// 36-char canonical GUID shape ("XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX") with
// hex digits at every non-hyphen position. Cheaper than full parse-and-check;
// authoring layer always emits this exact shape.
inline bool LooksLikeGuid(std::string_view s) noexcept
{
    if (s.size() != 36)
        return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
                return false;
        }
        else
        {
            const bool isHex = (c >= '0' && c <= '9') ||
                               (c >= 'a' && c <= 'f') ||
                               (c >= 'A' && c <= 'F');
            if (!isHex)
                return false;
        }
    }
    return true;
}

// Slurp the entire file into a std::string. Returns empty string on I/O
// failure; the parser treats empty as "couldn't extract" and returns false.
inline std::string ReadFileBody(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Path-form targets are stored canonical mount-relative — same form as
// AssetMetadata::Path. Authored references frequently use platform-native
// separators (especially on Windows); normalize to forward-slash so the
// registry-side resolver sees the canonical form.
inline std::string NormalizeAuthoredPath(std::string_view raw)
{
    std::string out(raw);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

} // namespace GameEngine::ParserExtraction
