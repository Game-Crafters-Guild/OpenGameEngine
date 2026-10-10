#pragma once

#include "Types/Types.h"

#include <filesystem>
#include <string_view>

namespace GameEngine {

enum class TextureAlphaContent : uint8 {
    /// The container/format provably cannot produce a sampled alpha below 1.0
    /// (no alpha channel encoded). Sampling such a texture yields alpha == 1.
    NoAlphaSource = 0,
    /// The texture has (or may have) alpha data — or the format is unknown.
    MaybeAlpha = 1,
};

/**
 * @brief Header-metadata alpha probe. Never decodes texel data.
 *
 * Recognizes PNG (IHDR color type + tRNS chunk scan up to the first IDAT),
 * KTX2 (VkFormat or Basis DFD: ETC1S sample count / UASTC channel type),
 * JPEG (never carries alpha), and TGA (via the extension hint — TGA has no
 * magic). Anything unrecognized, truncated, or undecidable within the given
 * bytes reports MaybeAlpha, so callers can only ever act on a provable
 * absence of alpha.
 */
TextureAlphaContent ProbeTextureAlphaFromBytes(const uint8* bytes, size_t size,
                                               std::string_view extensionHint = {});

/// Reads a bounded prefix of the file (enough for every recognized header)
/// and forwards to ProbeTextureAlphaFromBytes. Unreadable files report
/// MaybeAlpha.
TextureAlphaContent ProbeTextureFileAlpha(const std::filesystem::path& path);

} // namespace GameEngine
