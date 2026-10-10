#pragma once

#include <algorithm>

namespace GameEngine::Engine::Renderer
{
/// Persistent atlas contents only need refreshing outside the solve window when
/// their interpretation changes. Commit this snapshot after declaring uploads.
struct DDGIAtlasUploadState
{
    bool Valid = false;
    float GridMin[3] = {};
    float GridSize[3] = {};
    float FilterStrength = 0.0f;
    float FilterSmoothness = 0.0f;

    bool NeedsFullUpload(const float* gridMin, const float* gridSize,
                         float strength, float smoothness) const
    {
        return !Valid || !std::equal(GridMin, GridMin + 3, gridMin) ||
               !std::equal(GridSize, GridSize + 3, gridSize) ||
               FilterStrength != strength || FilterSmoothness != smoothness;
    }

    void Commit(const float* gridMin, const float* gridSize, float strength, float smoothness)
    {
        std::copy_n(gridMin, 3, GridMin);
        std::copy_n(gridSize, 3, GridSize);
        FilterStrength = strength;
        FilterSmoothness = smoothness;
        Valid = true;
    }
};
}  // namespace GameEngine::Engine::Renderer
