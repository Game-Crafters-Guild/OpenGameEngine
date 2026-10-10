#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine::Platform
{

// Requested system font style (subset).
enum class SystemFontStyle : uint8_t
{
    Normal = 0,
    Italic = 1,
    Oblique = 2
};

// Best-effort resolved system font file reference.
// Note: on some platforms/fonts, a family may resolve to a collection (.ttc)
// and require a face index. Callers should preserve faceIndex even if they
// currently ignore it.
struct SystemFontFile
{
    std::filesystem::path path; // absolute path to font file on disk (best-effort)
    uint32_t faceIndex = 0;     // face index within a collection (.ttc); 0 for most .ttf/.otf
    std::string resolvedFamily; // best-effort resolved family name
};

// Resolve a font family name (e.g. "Segoe UI") to a local font file path.
// Returns false if the family is not available or cannot be resolved to a local file.
//
// weight is CSS-like numeric weight (100..900 typical; 400=normal, 700=bold).
bool TryResolveSystemFontFile(std::string_view family, int weight, SystemFontStyle style, SystemFontFile& out);

} // namespace GameEngine::Platform

