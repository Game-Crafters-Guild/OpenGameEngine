#pragma once

#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <string_view>
#include <type_traits>

namespace GameEngine::Components
{

// Drives a per-entity video texture that overrides a texture slot on the entity's
// MeshRenderer material. VideoTextureSystem reads this component each frame, decodes
// the next video frame if needed, uploads it to a dynamic GPU texture, and binds that
// texture to the material slot named by uniformName.
struct VideoTextureComponent
{
    char videoPath[512] = {};   // Path to the .mp4 file; relative paths resolve against the project asset root
    // Material texture slot to override: a well-known slot name ("albedoMap",
    // "emissiveMap", ...) or a name the material's surface declares via
    // `// @texture`. Unresolvable names are skipped with a warning.
    char uniformName[64] = {"albedoMap"};

    bool loop = true;
    bool playOnStart = true;
    bool playing = false;   // Written by VideoTextureSystem; set playOnStart to auto-start

    float32 playbackSpeed = 1.0f;

    // The path and slot name up to their first null byte. Scene text and raw
    // component writes can fill either buffer with no terminator.
    [[nodiscard]] std::string_view VideoPath() const { return FixedStringView(videoPath); }
    [[nodiscard]] std::string_view UniformName() const { return FixedStringView(uniformName); }
};

static_assert(std::is_trivially_copyable_v<VideoTextureComponent>,
              "VideoTextureComponent must be trivially copyable");
static_assert(std::is_standard_layout_v<VideoTextureComponent>,
              "VideoTextureComponent must be standard layout");

} // namespace GameEngine::Components
