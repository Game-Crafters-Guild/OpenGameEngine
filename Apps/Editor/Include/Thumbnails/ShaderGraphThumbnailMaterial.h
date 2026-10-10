#pragma once

#include <filesystem>

namespace GameEngine
{
class AssetManager;

namespace ShaderGraphThumbnailMaterial
{

/** Derived .material path for a shader-graph .glsl asset (under Generated/ShaderGraphThumbnails/). */
std::filesystem::path ResolveMaterialPath(const std::filesystem::path& assetsRoot,
                                          const std::filesystem::path& graphGlslPath);

/** True for materials produced by {@link EnsureMaterial}. */
bool IsDerivedThumbnailMaterialPath(const std::filesystem::path& materialPath);

/** True for any generated shader-graph thumbnail artifact. */
bool IsDerivedThumbnailAssetPath(const std::filesystem::path& assetPath);

/**
 * Materializes {@p graphGlslPath} to a sibling .thumb.material + built surface .glsl
 * when missing or older than the graph. Registers the material asset. Returns empty on failure.
 */
std::filesystem::path EnsureMaterial(AssetManager& assets, const std::filesystem::path& graphGlslPath);

} // namespace ShaderGraphThumbnailMaterial
} // namespace GameEngine
