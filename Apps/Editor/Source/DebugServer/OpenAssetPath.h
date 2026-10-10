#pragma once

#include <filesystem>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Editor
{

// The `path` parameter of the asset-addressed debug commands (open_asset,
// select_asset): absolute, or relative to the asset mounts, optionally spelled
// with a leading "Assets/" or a source prefix ("editor:Icons/foo.png").
//
// Resolution goes through the AssetManager mounts only, never the process
// working directory. The engine parks its CWD at the project root, so a
// CWD-relative probe accepts "Assets/Scenes/Main.scene" verbatim — and the
// registry, which treats every relative path as asset-root-relative, then
// prefixes the asset root to it a second time.
//
// Returns an absolute path, or empty when the request is empty or carries a
// source prefix no mount answers to.
std::filesystem::path ResolveOpenAssetPath(const AssetManager& assetManager, const std::filesystem::path& path);

} // namespace GameEngine::Editor
