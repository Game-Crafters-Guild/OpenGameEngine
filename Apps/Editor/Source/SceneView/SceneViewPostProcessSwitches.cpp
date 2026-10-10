#include "SceneView/SceneViewPostProcessSwitches.h"

#include "Engine/Rendering/PostProcessSettings.h"

namespace GameEngine::Editor
{

void ApplySceneViewPostProcessSwitches(const SceneViewPostProcessSwitches& switches,
                                       int32_t offTonemapMode,
                                       Engine::Renderer::PostProcessSettings& settings)
{
    if (!switches.PostProcessing)
    {
        settings.DisableBloom();
        settings.ColorFilterIntensity = 0.0f;
        settings.CasStrength = 0.0f;
        settings.CrtIntensity = 0.0f;
        settings.CrtExposureCompensation = 0.0f;
        settings.VhsIntensity = 0.0f;
        settings.HeightFogIntensity = 0.0f;
        settings.HeightFogDensity = 0.0f;
        settings.VolumetricFogIntensity = 0.0f;
        settings.VolumetricFogTemporalEnabled = 0;
        settings.VolumetricFogTemporalBlend = 0.0f;
        settings.TonemapMode = offTonemapMode;
        return;
    }
    if (!switches.Bloom)
        settings.DisableBloom();
    if (!switches.Tonemap)
        settings.TonemapMode = offTonemapMode;
    if (!switches.ColorFilter)
        settings.ColorFilterIntensity = 0.0f;
    if (!switches.ContrastAdaptiveSharpening)
        settings.CasStrength = 0.0f;
    if (!switches.Crt)
    {
        settings.CrtIntensity = 0.0f;
        settings.CrtExposureCompensation = 0.0f;
    }
}

} // namespace GameEngine::Editor
