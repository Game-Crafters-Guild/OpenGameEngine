#ifndef GE_SKY_ATMOSPHERE_UBO_GLSL
#define GE_SKY_ATMOSPHERE_UBO_GLSL

// Canonical std140 AtmosphereUBO, declared once and shared by every sky pass.
// Byte-identical to AtmosphereParametersGPU (208 B) in
// Rendering/Sky/SkyRenderer.h, whose field offsets are all static_asserted --
// keep this block in lock-step with that struct. Every sky pass uploads the
// full 208 B AtmosphereParametersGPU (the capture packs it at
// kCaptureAtmoUboOffset), so a pass may declare the whole block here even when
// it only reads a prefix of it.
//
// The bind point differs per pass, so define GE_ATMOSPHERE_UBO_BINDING before
// including. It defaults to binding 2 (sky-view / render / multiscatter /
// capture); the transmittance pass overrides it to 1.
#ifndef GE_ATMOSPHERE_UBO_BINDING
#define GE_ATMOSPHERE_UBO_BINDING 2
#endif

layout(set = 0, binding = GE_ATMOSPHERE_UBO_BINDING) uniform AtmosphereUBO
{
    float planetRadius;
    float atmosphereRadius;
    float rayleighScaleHeight;
    float mieScaleHeight;
    vec3  betaRayleigh;   float pad0;
    vec3  betaMie;        float mieG;
    vec3  groundAlbedo;
    float groundBrightness;
    vec3  groundNightColor;
    vec3  groundHorizonColor;
    vec3  groundHorizonNightColor;
    vec3  nightSkyHorizonColor;
    float nightSkyHorizonPad;
    float groundHorizonDayCosWidth;
    float groundHorizonNightCosWidth;
    float belowHorizonBlendSharpness;
    float belowHorizonDarkness;
    vec2  belowHorizonDarkPadding;
    float belowHorizonDarkColorStd140PrePad0;
    float belowHorizonDarkColorStd140PrePad1;
    float belowHorizonDarkColorStd140PrePad2;
    vec3  belowHorizonDarkColor;
    uint  belowHorizonMode;
    float groundHazeStrength;
} uAtmos;

#endif
