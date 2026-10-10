#pragma once

#include <filesystem>
#include <string_view>

namespace GameEngine {

/// MIME type of a model's texture file, from its extension (case-insensitive).
/// Unknown extensions map to "image/png".
std::string_view MimeFromTextureExtension(const std::filesystem::path& path);

/// File extension (with the dot) for a texture MIME type; the inverse of
/// MimeFromTextureExtension. Empty or unknown MIME types map to ".png".
std::string_view TextureExtensionFromMime(std::string_view mimeType);

} // namespace GameEngine
