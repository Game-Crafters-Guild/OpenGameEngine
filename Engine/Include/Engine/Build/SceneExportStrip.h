#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
struct AssetManifest;

/// Rewrites every staged scene and blueprint of `manifest`, under `contentRoot`, without its
/// editor-only components: the lines of every component marked "// @ge-editor-only"
/// (ComponentFieldRegistry::IsComponentEditorOnly) and, on an entity that carries a mark-up,
/// its title and spline (SceneAssetParser::StripEditorOnlyComponents). Every entity stays,
/// with its parent link and its other components. Returns false when a staged scene cannot
/// be read or written, with `error` naming it, and when `cancelRequested` returns true, with
/// `error` left empty; the build then fails and publishes nothing.
bool StripEditorOnlyComponentsFromStagedScenes(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                                               const std::function<bool()>& cancelRequested, std::string& error);
} // namespace GameEngine
