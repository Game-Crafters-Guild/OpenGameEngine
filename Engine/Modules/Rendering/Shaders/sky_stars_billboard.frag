#version 450

layout(location = 0) in vec2 vQuadPos;
layout(location = 1) flat in float vBrightness;
layout(location = 2) flat in float vTwinkle;
layout(location = 3) flat in float vRotAngle;
layout(location = 4) flat in float vSizeScale;
layout(location = 5) flat in float vColorTemp;

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 1) uniform SkyUBO
{
    vec3  cameraPositionWS;   float exposureEV;
    vec3  cameraRightWS;      float pad0;
    vec3  cameraUpWS;         float pad1;
    vec3  cameraForwardWS;    float pad2;
    vec3  sunDirectionWS;     float sunAngularRadius;
    vec3  moonDirectionWS;    float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    vec3  sunColor;           float sunIntensity;
    float timeOfDayHours;     float skyPad0;
    float viewportAspect;     float tanHalfFovY;
    vec2  viewportResolution; float nightSkyBlend; float skyTimeSeconds;
    vec4  starSizeShape;
    vec4  starTwinkleParams;
    vec4  starTwinkleAmp;
    vec4  starDensityHorizon;
    vec4  starSizeRange;
} uSky;

void main()
{
    float r = length(vQuadPos);
    if (r > 1.0)
        discard;

    float starRadiusFrac = clamp(uSky.starSizeShape.y, 0.001, 2.0) * vSizeScale;
    float diamondRadiusFrac = clamp(uSky.starSizeShape.z, 0.001, 2.0) * vSizeScale;
    float maxExtent = max(starRadiusFrac, diamondRadiusFrac);
    float radialFrac = starRadiusFrac / maxExtent;
    float diamondFrac = diamondRadiusFrac / maxExtent;

    float diamondAmount = clamp(uSky.starSizeShape.w, 0.0, 1.0);
    float glowFalloff = clamp(uSky.starTwinkleParams.w, 0.5, 16.0);
    float coreSize = clamp(uSky.starSizeRange.z, 0.0, 0.6);

    float normR = clamp(r / radialFrac, 0.0, 1.0);
    float adjR = (normR <= coreSize) ? 0.0
                 : (normR - coreSize) / (1.0 - coreSize);
    float radial = pow(1.0 - adjR, glowFalloff);

    float cs = cos(vRotAngle);
    float sn = sin(vRotAngle);
    vec2 rotPos = vec2(cs * vQuadPos.x - sn * vQuadPos.y,
                       sn * vQuadPos.x + cs * vQuadPos.y);
    float diamondMetric = abs(rotPos.x) + abs(rotPos.y);
    float normD = clamp(diamondMetric / diamondFrac, 0.0, 1.0);
    float adjD = (normD <= coreSize) ? 0.0
                 : (normD - coreSize) / (1.0 - coreSize);
    float diamond = pow(1.0 - adjD, glowFalloff);

    float shape = max(radial, diamond * diamondAmount);

    float starBrightnessSetting = clamp(uSky.starDensityHorizon.w, 0.0, 5.0);

    vec3 warmTint = vec3(1.0, 0.85, 0.7);
    vec3 coolTint = vec3(0.7, 0.85, 1.0);
    vec3 starColor = mix(warmTint, coolTint, vColorTemp);

    float intensity = vBrightness * vTwinkle * starBrightnessSetting * shape;

    oColor = vec4(starColor * intensity, 0.0);
}
