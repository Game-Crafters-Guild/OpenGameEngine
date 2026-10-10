#pragma once

#include "Types/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering
{

// Parsed Resolve-style .cube LUT (1D, 3D, or 1D shaper + 3D). Comments (#) are
// skipped until header lines are found; LUT_*_INPUT_RANGE or scalar DOMAIN_MIN/
// DOMAIN_MAX is optional (defaults 0-1).
struct CubeLutParseResult
{
    bool Ok = false;
    std::string Error;

    bool Has1D = false;
    bool Has3D = false;
    uint32_t Size1D = 0;
    uint32_t Size3D = 0;
    float32 In1DMin = 0.0f;
    float32 In1DMax = 1.0f;
    float32 In3DMin = 0.0f;
    float32 In3DMax = 1.0f;

    // Interleaved RGB triples (Resolve order: red-major for 3D).
    std::vector<float32> Lut1DRgb;
    std::vector<float32> Lut3DRgb;
};

bool ParseCubeLutFromText(std::string_view text, CubeLutParseResult& out);

} // namespace GameEngine::Rendering
