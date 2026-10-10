#pragma once

#include <filesystem>

namespace GameEngine {

// True when `child` is a descendant of `parent` — not `parent` itself, and
// with no `..` walk-out. Callers that must refuse a symlink whose target
// leaves the assets tree should pass weakly-canonical paths.
inline bool IsAssetPathStrictlyUnder(const std::filesystem::path& child,
                                     const std::filesystem::path& parent)
{
    const auto rel = child.lexically_relative(parent);
    if (rel.empty() || rel == "." || rel == "..")
        return false;
    for (const auto& part : rel)
    {
        if (part == "..")
            return false;
    }
    return true;
}

} // namespace GameEngine
