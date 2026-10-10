#pragma once

#include <cstdint>
#include <string_view>

namespace GameEngine::Engine::Renderer
{
// Medium is the cheapest preset: a fixed per-frame cost (prepare, depth mips,
// denoise, temporal) dominates below it, so fewer taps buy no meaningful savings.
enum class GtaoQuality : uint32_t
{
    Medium,
    High,
    Ultra
};

// Radial taps per slice direction, shared by every preset. The sweep marks the
// sectors its taps land in, so its horizon is a maximum over samples and the
// occlusion it reports RISES with the tap count without converging: three,
// four and six taps differ by up to 84 % of the occlusion on the same frame.
// The tap count is therefore an accuracy setting for the estimator, not a noise
// setting, and cannot be a preset dial - a preset that raised it would change
// how dark the frame is and silently retune every AmbientOcclusion volume.
// Presets buy resolution and slice directions, both measured neutral in amount.
constexpr uint32_t kGtaoSweepSteps = 3;

struct GtaoQualitySettings
{
    uint32_t ResolutionDivisor;
    uint32_t Directions;
    uint32_t MaxDepthMip;
};

constexpr GtaoQualitySettings GetGtaoQualitySettings(GtaoQuality quality)
{
    switch (quality)
    {
    case GtaoQuality::Medium:
        return {2, 5, 5};
    case GtaoQuality::High:
        return {1, 5, 5};
    case GtaoQuality::Ultra:
        return {1, 8, 5};
    }
    return {2, 5, 5};
}

inline bool ParseGtaoQuality(std::string_view name, GtaoQuality& out)
{
    if (name == "medium")
        out = GtaoQuality::Medium;
    else if (name == "high")
        out = GtaoQuality::High;
    else if (name == "ultra")
        out = GtaoQuality::Ultra;
    else
        return false;
    return true;
}
} // namespace GameEngine::Engine::Renderer
