#pragma once

#include <filesystem>
#include <string>

namespace GameEngine
{
class AssetManager;

namespace Editor
{

/// An absolute asset path as an asset-root-relative string suitable for
/// settings serialization, or empty when the path lies outside every root.
/// Checks the primary asset root first, then each registered non-project
/// source, and rejects anything that escapes a root.
std::string TryMakeAssetRelativePathString(const AssetManager& assetManager,
                                           const std::filesystem::path& absolutePath);

} // namespace Editor
} // namespace GameEngine
