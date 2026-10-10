#pragma once

#include "Types/StringUtils.h"

#include <span>
#include <string_view>

namespace GameEngine::Platform
{

// A CSS generic family and the concrete font a platform ships for it.
struct GenericFontFamily
{
    std::string_view Generic;
    std::string_view Concrete;
};

// Maps a CSS generic family (sans-serif, serif, monospace, ...) to the
// backend's concrete OS font, so the OS lookup gets a real family name rather
// than a keyword it resolves to an arbitrary default. Other names pass through.
inline std::string_view MapGenericFontFamily(std::string_view family, std::span<const GenericFontFamily> table)
{
    const std::string_view key = TrimWhitespaceView(family);
    for (const GenericFontFamily& entry : table)
    {
        if (key.size() == entry.Generic.size() && StartsWithIgnoreCase(key, entry.Generic))
            return entry.Concrete;
    }
    return family;
}

} // namespace GameEngine::Platform
