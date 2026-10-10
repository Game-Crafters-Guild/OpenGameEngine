#pragma once

#include "AssetCore/GUID.h"

#include <filesystem>
#include <optional>

namespace GameEngine
{
class AssetRegistry;

// Generated export metadata: filenames within the owning mount's Tex directory.
inline constexpr const char* kTexturePackagedPortableMetaKey = "assets.texture.packaged.portable";
inline constexpr const char* kTexturePackagedCompressedMetaKey = "assets.texture.packaged.compressed";

/// Resolve a finished package payload without reading or hashing the source image.
/// Nullopt means an authored source; an empty path means malformed package metadata.
/// CPU readers request the portable payload; GPU readers may prefer compression.
std::optional<std::filesystem::path> ResolvePackagedTexturePayload(
    const AssetRegistry& registry, const GUID& guid, bool preferCompressed);
} // namespace GameEngine
