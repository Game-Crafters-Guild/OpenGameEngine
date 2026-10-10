#pragma once

#include <cstdint>

namespace GameEngine::Engine::Renderer
{
struct PostProcessSettings;
}

namespace GameEngine::Editor
{

// The Scene View toolbar's post-processing switches: the master post-processing
// button and the per-effect entries of its menu. An effect whose entry is off,
// or every listed effect while post processing is off, runs no pass in the view.
struct SceneViewPostProcessSwitches
{
    bool PostProcessing = true;
    bool Bloom = true;
    bool Tonemap = true;
    bool ColorFilter = true;
    bool ContrastAdaptiveSharpening = true;
    bool Crt = true;
};

/// Applies the switches to the view's blended settings. `offTonemapMode` is the
/// curve "tonemapping off" maps to on the current display (a Components::TonemapMode
/// value).
void ApplySceneViewPostProcessSwitches(const SceneViewPostProcessSwitches& switches,
                                       int32_t offTonemapMode,
                                       Engine::Renderer::PostProcessSettings& settings);

} // namespace GameEngine::Editor
