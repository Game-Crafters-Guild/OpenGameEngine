#version 450

// Folds the temporally-resolved cloud buffer (R=light energy,
// G=transmittance) over the scene: scene * transmittance + energy * sunColor
// (upstream Clouds.shader composite; the sun tint is applied here so the
// march/resolve stay scalar).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uClouds;

layout(set = 0, binding = 2, std140) uniform CloudCompositeParamsBlock
{
    float radius;
    float altitude;
    float thickness;
    int numStepsLight;
    float stepSize;
    float rayOffsetStrength;
    float cloudScale;
    float densityMultiplier;
    float densityOffset;
    float shapeOffsetX;
    float shapeOffsetY;
    float shapeOffsetZ;
    float shapeWeightR;
    float shapeWeightG;
    float shapeWeightB;
    float shapeWeightA;
    float detailNoiseScale;
    float detailNoiseWeight;
    float detailWeightR;
    float detailWeightG;
    float detailWeightB;
    float detailOffsetX;
    float detailOffsetY;
    float detailOffsetZ;
    float lightAbsorptionThroughCloud;
    float lightAbsorptionTowardSun;
    float darknessThreshold;
    float phaseForward;
    float phaseBack;
    float phaseBase;
    float phaseFactor;
    float timeScale;
    float baseSpeed;
    float detailSpeed;
    float sunDirX;
    float sunDirY;
    float sunDirZ;
    float sunColorR;
    float sunColorG;
    float sunColorB;
    float sunIntensity;
    float historyWeight;
} CloudParams;

void main()
{
    vec4 scene = texture(uSceneColor, vUV);
    vec4 clouds = texture(uClouds, vUV);
    vec3 sunColor = vec3(CloudParams.sunColorR, CloudParams.sunColorG, CloudParams.sunColorB) *
                    CloudParams.sunIntensity;
    vec3 rgb = scene.rgb * clamp(clouds.g, 0.0, 1.0) + clouds.r * sunColor;
    oColor = vec4(max(rgb, vec3(0.0)), scene.a);
}
