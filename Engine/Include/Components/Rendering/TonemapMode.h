#pragma once

#include <cstdint>

namespace GameEngine {
namespace Components {

enum class TonemapMode : int32_t
{
    ACES         = 0,
    Reinhard     = 1,
    AgX          = 2,
    Filmic       = 3,
    Neutral      = 4,
    Linear       = 5,
    GranTurismo7 = 6,
    ACES2        = 7,
};

static constexpr int32_t kTonemapModeCount = 8;

static constexpr const char* kTonemapModeNames[] = {
    "ACES", "Reinhard", "AgX", "Filmic", "Khronos PBR Neutral", "Linear",
    "ICtCp Tonemapper (2025, GT7)", "ACES 2"
};

constexpr bool IsValidTonemapModeValue(int32_t value)
{
    return (value >= static_cast<int32_t>(TonemapMode::ACES) &&
            value <= static_cast<int32_t>(TonemapMode::Linear)) ||
           value == static_cast<int32_t>(TonemapMode::GranTurismo7) ||
           value == static_cast<int32_t>(TonemapMode::ACES2);
}

} // namespace Components
} // namespace GameEngine
