#pragma once

#include <filesystem>

namespace GameEngine
{

// `path` spelled relative to `root` when it lies strictly inside `root`, and an
// empty path otherwise: when `root` is empty, when `path` is `root` itself, when
// the relative spelling would walk out through `..`, or when the two share no
// root name. Purely lexical over both paths normalised — neither has to exist —
// and case-sensitive, as std::filesystem::path is. The walk-out test compares
// path elements, not characters, so a name that merely starts with two dots is
// inside, and the test reads the same on every platform whatever
// path::native() is spelled in.
inline std::filesystem::path RelativePathUnderRoot(const std::filesystem::path& path,
                                                   const std::filesystem::path& root)
{
    if (root.empty())
        return {};
    const std::filesystem::path rel =
        path.lexically_normal().lexically_relative(root.lexically_normal());
    if (rel.empty() || rel == "." || *rel.begin() == "..")
        return {};
    return rel;
}

} // namespace GameEngine
