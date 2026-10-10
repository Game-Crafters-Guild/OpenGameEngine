#pragma once

#include "Engine/Rendering/AmbientOcclusion/GtaoQuality.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{
// History reuse policy for the GTAO temporal pass: what identifies a history
// texture's contents and when last frame's write may be read back.
struct GtaoHistoryKey
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Camera = 0;
    GtaoQuality Quality = GtaoQuality::Medium;
    float Radius = 0;
    float Thickness = 0;
    float Intensity = 0;
    uint64_t World = 0;
    bool operator==(const GtaoHistoryKey&) const = default;
};

inline bool CanReuseGtaoHistory(uint64_t writtenFrame, uint64_t frame,
                                const GtaoHistoryKey& previous, const GtaoHistoryKey& current,
                                bool freshAllocation, bool cameraContinuous)
{
    // Conservative adjacency: skipped/disabled AO and duplicate declarations
    // cannot read a texture written for a different camera-history interval.
    return writtenFrame != ~uint64_t{0} && writtenFrame + 1 == frame &&
           previous == current && !freshAllocation && cameraContinuous;
}
} // namespace GameEngine::Engine::Renderer
