#pragma once

#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <string_view>

namespace GameEngine {
namespace Components {

struct Skybox {
    float32 HDRIIntensity  = 1.0f;
    float32 RotationDegrees = 0.0f;
    float32 IblIntensity   = 1.0f;
    float32 IblLowerHemisphereDarkness = 1.0f;
    char    Resolution[8]     = "1k";
    char    SourceSlug[64]    = {0};
    char    HDRIAssetGuid[37] = {0};
    char    HDRIPath[260]     = {0};

    // The text fields up to their first null byte. Scene text and raw component
    // writes can fill a buffer with no terminator, so never read them as C strings.
    std::string_view GetResolution() const { return FixedStringView(Resolution); }
    std::string_view GetSourceSlug() const { return FixedStringView(SourceSlug); }
    std::string_view GetHDRIAssetGuid() const { return FixedStringView(HDRIAssetGuid); }
    std::string_view GetHDRIPath() const { return FixedStringView(HDRIPath); }
};

} // namespace Components
} // namespace GameEngine
