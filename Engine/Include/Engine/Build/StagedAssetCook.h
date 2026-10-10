#pragma once

#include "Types/Types.h"

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
class AssetRegistry;
class ParserRegistry;
struct AssetManifest;
struct TexturePackageCookStats;
class TextureCookWorkers;
enum class TextureCookEncodeQuality : uint32;

/// The build's cook of the content it staged: strips the editor-only components from the staged
/// scenes (StripEditorOnlyComponentsFromStagedScenes), bakes the staged textures from the tree's
/// `.assetmanifest` (StagePackagedTextureCooks, at `textureQuality`, spread across `textureWorkers` when
/// given), then rewrites every asset whose
/// parser has a packaged form (CookStagedAssets). Returns false at the first failure, with `error`
/// naming it, and when `cancelRequested` returns true; the build then fails and publishes nothing.
bool CookStagedContent(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                       const AssetRegistry& registry, const ParserRegistry& parsers,
                       TextureCookEncodeQuality textureQuality, TextureCookWorkers* textureWorkers,
                       const std::function<bool()>& cancelRequested, TexturePackageCookStats& textureStats,
                       std::string& error);

/// Rewrites every staged asset whose parser has a packaged form (AssetParser::CookForPackage)
/// in place, under `contentRoot`, the directory the manifest's output paths are relative to.
/// An asset whose parser keeps the authored form, or that no parser reads, stays as staged.
/// Returns false at the first asset that cannot cook, with `error` naming the asset and the
/// reason, and when `cancelRequested` returns true, with `error` left empty; the build then
/// fails and publishes nothing.
bool CookStagedAssets(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                      const ParserRegistry& parsers, const std::function<bool()>& cancelRequested,
                      std::string& error);
} // namespace GameEngine
