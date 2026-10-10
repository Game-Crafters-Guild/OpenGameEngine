#pragma once

#include <filesystem>

namespace GameEngine::AssetDatabase
{

// Cooked-texture directory under a source's cache root, and the directory the
// export publishes texture bakes into beside packaged content. The macOS bundle
// assembler also removes it from a Player template that ran before export.
inline constexpr char kTextureCacheDirectoryName[] = "Tex";

struct AssetDatabasePaths
{
    std::filesystem::path projectRoot;
    std::filesystem::path authoritativeFile; // <ProjectRoot>/AssetDatabase.assetdb

    // Derived cache root (safe to delete). Default: <ProjectRoot>/.Cache/AssetDatabase
    std::filesystem::path cacheRoot;
    // Default: <cacheRoot>/AssetDbCache.sqlite (derived)
    std::filesystem::path sqliteCacheFile;

    std::filesystem::path editorRoot;        // <ProjectRoot>/.Editor
    std::filesystem::path thumbnailsRoot;    // <ProjectRoot>/.Editor/Thumbnails (derived)
};

// Given the configured assetRoot (typically <ProjectRoot>/Assets), return default
// locations for the authoritative and derived files.
// Optional overrides allow decoupling DB/cache locations from assetRoot.
// If an override path is relative, it is resolved relative to the derived projectRoot.
AssetDatabasePaths GetDefaultPathsForAssetRoot(const std::filesystem::path& assetRoot,
                                              const std::filesystem::path& authoritativeFileOverride = {},
                                              const std::filesystem::path& cacheRootOverride = {});

} // namespace GameEngine::AssetDatabase


