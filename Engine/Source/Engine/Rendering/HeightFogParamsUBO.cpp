#include "Engine/Rendering/HeightFogParamsUBO.h"

#include "Engine/Rendering/PostProcessSettings.h"

namespace GameEngine::Engine::Renderer {

void FillHeightFogParamsUBO(const PostProcessSettings& settings, HeightFogParamsUBO& out)
{
    out.intensity = settings.HeightFogIntensity;
    out.density = settings.HeightFogDensity;
    out.maxOpacity = settings.HeightFogMaxOpacity;
    out.minDistance = settings.HeightFogMinDistance;
    out.smoothLength = settings.HeightFogSmoothLength;
    out.baseHeight = settings.HeightFogBaseHeight;
    out.transitionLength = settings.HeightFogTransitionLength;
    out.maxDistance = settings.HeightFogMaxDistance;
    out.gradientStrength = settings.HeightFogGradientStrength;
    out.emissiveR = settings.HeightFogEmissiveR;
    out.emissiveG = settings.HeightFogEmissiveG;
    out.emissiveB = settings.HeightFogEmissiveB;
    out.sunIntensity = settings.HeightFogSunIntensity;
    out.sunDirX = settings.HeightFogSunDirX;
    out.sunDirY = settings.HeightFogSunDirY;
    out.sunDirZ = settings.HeightFogSunDirZ;
    out.phase = settings.HeightFogPhase;
    out.sunColorR = settings.HeightFogSunColorR;
    out.sunColorG = settings.HeightFogSunColorG;
    out.sunColorB = settings.HeightFogSunColorB;
    out.phaseWeight0 = settings.HeightFogPhaseWeight0;
    out.phaseWeight1 = settings.HeightFogPhaseWeight1;
    out.skyPower = settings.HeightFogSkyPower;
    out.skyFillStart = settings.HeightFogSkyFillStart;
    out.skyFillEnd = settings.HeightFogSkyFillEnd;
    out.axisX = settings.HeightFogAxisX;
    out.axisY = settings.HeightFogAxisY;
    out.axisZ = settings.HeightFogAxisZ;
    out.noiseScale = settings.HeightFogNoiseScale;
    out.gradientLowR = settings.HeightFogGradientLowR;
    out.gradientLowG = settings.HeightFogGradientLowG;
    out.gradientLowB = settings.HeightFogGradientLowB;
    out.noiseStrength = settings.HeightFogNoiseStrength;
    out.gradientHighR = settings.HeightFogGradientHighR;
    out.gradientHighG = settings.HeightFogGradientHighG;
    out.gradientHighB = settings.HeightFogGradientHighB;
    out.noiseContrast = settings.HeightFogNoiseContrast;
    out.noiseVelX = settings.HeightFogNoiseVelX;
    out.noiseVelY = settings.HeightFogNoiseVelY;
    out.noiseVelZ = settings.HeightFogNoiseVelZ;

    out.flags = 0;
    if (settings.HeightFogDistanceFogEnabled)
        out.flags |= HeightFogFlagDistanceEnabled;
    if (settings.HeightFogHeightFogEnabled)
        out.flags |= HeightFogFlagHeightEnabled;
    if (settings.HeightFogNoiseEnabled)
        out.flags |= HeightFogFlagNoiseEnabled;
    if (settings.HeightFogSkyEnabled)
        out.flags |= HeightFogFlagSkyEnabled;

    out.axisMode = settings.HeightFogAxisMode;
    out.gradientMode = settings.HeightFogGradientMode;
    out.layerMode = settings.HeightFogLayerMode;
    out.horizonHeightOffset = settings.HeightFogHorizonHeightOffset;
    out.horizonHeightBlendStart = settings.HeightFogHorizonHeightBlendStart;
    out.horizonHeightBlendEnd = settings.HeightFogHorizonHeightBlendEnd;
    out.noiseMin = settings.HeightFogNoiseMin;
    out.noiseMax = settings.HeightFogNoiseMax;
    out.noiseFadeStart = settings.HeightFogNoiseFadeStart;
    out.noiseFadeEnd = settings.HeightFogNoiseFadeEnd;
    out.skyHorizonOffset = settings.HeightFogSkyHorizonOffset;
    out.skyBottomStrength = settings.HeightFogSkyBottomStrength;
}

} // namespace GameEngine::Engine::Renderer
