#pragma once

#include "Types/Types.h"

namespace GameEngine::Engine::Renderer {

// std140 layout shared by volumetric_clouds_march.frag (CloudParamsBlock) and
// VolumetricCloudsParamsUploadNode. Scalar members only, so field order here
// IS the GPU ABI — keep in lockstep with the shader block.
struct VolumetricCloudsParamsUBO
{
    float radius = 0.0f;
    float altitude = 900.0f;
    float thickness = 0.0f;
    int32 numStepsLight = 8;
    float stepSize = 11.0f;
    float rayOffsetStrength = 10.0f;
    float cloudScale = 1.0f;
    float densityMultiplier = 0.0f;
    float densityOffset = 0.0f;
    float shapeOffsetX = 0.0f;
    float shapeOffsetY = 0.0f;
    float shapeOffsetZ = 0.0f;
    float shapeWeightR = 1.0f;
    float shapeWeightG = 0.48f;
    float shapeWeightB = 0.15f;
    float shapeWeightA = 0.0f;
    float detailNoiseScale = 10.0f;
    float detailNoiseWeight = 0.1f;
    float detailWeightR = 1.0f;
    float detailWeightG = 0.5f;
    float detailWeightB = 0.25f;
    float detailOffsetX = 0.0f;
    float detailOffsetY = 0.0f;
    float detailOffsetZ = 0.0f;
    float lightAbsorptionThroughCloud = 1.0f;
    float lightAbsorptionTowardSun = 1.0f;
    float darknessThreshold = 0.2f;
    float phaseForward = 0.83f;
    float phaseBack = 0.3f;
    float phaseBase = 0.8f;
    float phaseFactor = 0.15f;
    float timeScale = 1.0f;
    float baseSpeed = 1.0f;
    float detailSpeed = 2.0f;
    float sunDirX = 0.35f;
    float sunDirY = 0.65f;
    float sunDirZ = 0.68f;
    float sunColorR = 1.0f;
    float sunColorG = 0.9f;
    float sunColorB = 0.8f;
    float sunIntensity = 1.0f;
    float historyWeight = 0.85f;
    float _pad0 = 0.0f;
    float _pad1 = 0.0f;
};

static_assert(sizeof(VolumetricCloudsParamsUBO) == 176,
              "VolumetricCloudsParamsUBO must be 176 bytes");

void FillVolumetricCloudsParamsUBO(const struct PostProcessSettings& settings,
                                   VolumetricCloudsParamsUBO& out);

} // namespace GameEngine::Engine::Renderer
