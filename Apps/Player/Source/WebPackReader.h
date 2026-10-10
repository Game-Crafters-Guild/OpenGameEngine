#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace GameEngine::WebPlayer
{

/// Outcome of unpacking one .gepak image.
struct PackUnpackResult
{
    bool Success = false;
    std::size_t FileCount = 0;
    /// Names the offending entry when Success is false.
    std::string Error;
};

/// Write every file in a .gepak image into `root`, creating directories as
/// needed. The format is defined by Tools/Web/gepak.py, which writes the packs
/// this reads; the two change together.
///
/// Entry paths are relative to `root` and are rejected — the whole pack, not
/// just the entry — if they are absolute or contain a `..` component, so a
/// malformed or hostile pack cannot write outside the root.
PackUnpackResult UnpackWebPack(std::span<const std::uint8_t> image,
                               const std::filesystem::path& root);

} // namespace GameEngine::WebPlayer
