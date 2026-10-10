#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine {
namespace Platform {

// Split a multi-select file-dialog capture on `separator`. Empty segments
// (leading, trailing, or doubled separators) are dropped. Callers that accept
// '|' inside a filename must pass newline, not '|'.
inline std::vector<std::filesystem::path> SplitLinuxDialogPaths(const std::string& output,
                                                                char separator)
{
    std::vector<std::filesystem::path> result;
    if (output.empty())
        return result;

    size_t start = 0;
    while (start <= output.size())
    {
        const size_t end = output.find(separator, start);
        const size_t stop = (end == std::string::npos) ? output.size() : end;
        if (stop > start)
            result.emplace_back(output.substr(start, stop - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return result;
}

} // namespace Platform
} // namespace GameEngine
