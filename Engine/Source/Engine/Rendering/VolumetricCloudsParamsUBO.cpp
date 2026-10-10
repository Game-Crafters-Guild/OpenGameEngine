#include "Engine/Rendering/VolumetricCloudsParamsUBO.h"

#include "Engine/Rendering/PostProcessSettings.h"

namespace GameEngine::Engine::Renderer {

void FillVolumetricCloudsParamsUBO(const PostProcessSettings& settings,
                                   VolumetricCloudsParamsUBO& out)
{
    out.radius = settings.CloudsRadius;
    out.altitude = settings.CloudsAltitude;
    out.thickness = settings.CloudsThickness;
    out.numStepsLight = settings.CloudsNumStepsLight;
    out.stepSize = settings.CloudsStepSize;
    out.rayOffsetStrength = settings.CloudsRayOffsetStrength;
    out.cloudScale = settings.CloudsScale;
    out.densityMultiplier = settings.CloudsDensityMultiplier;
    out.densityOffset = settings.CloudsDensityOffset;
    out.shapeOffsetX = settings.CloudsShapeOffsetX;
    out.shapeOffsetY = settings.CloudsShapeOffsetY;
    out.shapeOffsetZ = settings.CloudsShapeOffsetZ;
    out.shapeWeightR = settings.CloudsShapeWeightR;
    out.shapeWeightG = settings.CloudsShapeWeightG;
    out.shapeWeightB = settings.CloudsShapeWeightB;
    out.shapeWeightA = settings.CloudsShapeWeightA;
    out.detailNoiseScale = settings.CloudsDetailScale;
    out.detailNoiseWeight = settings.CloudsDetailWeight;
    out.detailWeightR = settings.CloudsDetailWeightR;
    out.detailWeightG = settings.CloudsDetailWeightG;
    out.detailWeightB = settings.CloudsDetailWeightB;
    out.detailOffsetX = settings.CloudsDetailOffsetX;
    out.detailOffsetY = settings.CloudsDetailOffsetY;
    out.detailOffsetZ = settings.CloudsDetailOffsetZ;
    out.lightAbsorptionThroughCloud = settings.CloudsAbsorptionThroughCloud;
    out.lightAbsorptionTowardSun = settings.CloudsAbsorptionTowardSun;
    out.darknessThreshold = settings.CloudsDarknessThreshold;
    out.phaseForward = settings.CloudsPhaseForward;
    out.phaseBack = settings.CloudsPhaseBack;
    out.phaseBase = settings.CloudsPhaseBase;
    out.phaseFactor = settings.CloudsPhaseFactor;
    out.timeScale = settings.CloudsTimeScale;
    out.baseSpeed = settings.CloudsBaseSpeed;
    out.detailSpeed = settings.CloudsDetailSpeed;
    out.historyWeight = settings.CloudsHistoryWeight;
}

} // namespace GameEngine::Engine::Renderer
