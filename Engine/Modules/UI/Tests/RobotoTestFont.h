#pragma once

// Loads the faces the build stages next to the test executable (Roboto and
// RobotoMono, from Apps/Editor/Assets/Fonts) — the same files the editor ships
// and the same Roboto the Chrome reference measurements in the text-geometry
// tests were taken against. Resolution is anchored to the executable directory,
// never to the working directory or a path back into the source tree: a
// cwd-relative lookup silently returns nothing when the cwd differs, and a
// test that skips on "font not found" then reads as green. Anchoring here
// also matches how UIManager's own font resolver finds the face, so an
// end-to-end test asking for `font-family: Roboto` and a direct FontAtlas load
// agree on which bytes they measured.

#include "Core/Application.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Text/FontAtlas.h"

#include <memory>
#include <string>
#include <vector>

namespace GameEngine::UITesting
{

// Matches kUiFontAtlasPx in UIManager_Internal.h, so glyph rasterisation here
// matches the running editor. Line metrics no longer depend on it: they come
// from the design-unit tables scaled to the render size, not from FreeType's
// ppem-rounded size metrics.
inline constexpr unsigned kUiAtlasPixelSize = 20u;

inline std::string StagedFontPath(const char* fileName)
{
    return (PathUtils::GetExecutableDirectory() / "Assets" / "Fonts" / fileName).string();
}

// Empty when the file is not staged.
inline std::vector<uint8_t> LoadStagedFontBytes(const char* fileName)
{
    const std::string path = StagedFontPath(fileName);
    if (!Rendering::Utils::FileExists(path.c_str()))
        return {};
    return Rendering::Utils::ReadFile(path.c_str());
}

inline std::unique_ptr<Rendering::Text::FontAtlas> LoadStagedFontAtlas(
    const char* fileName, unsigned atlasPixelSize = kUiAtlasPixelSize)
{
    const std::vector<uint8_t> bytes = LoadStagedFontBytes(fileName);
    if (bytes.empty())
        return nullptr;
    auto atlas = std::make_unique<Rendering::Text::FontAtlas>();
    if (!atlas->LoadFontBytes(bytes.data(), bytes.size(), atlasPixelSize))
        return nullptr;
    return atlas;
}

inline std::unique_ptr<Rendering::Text::FontAtlas> LoadRobotoAtlas(
    unsigned atlasPixelSize = kUiAtlasPixelSize)
{
    return LoadStagedFontAtlas("Roboto-Regular.ttf", atlasPixelSize);
}

} // namespace GameEngine::UITesting
