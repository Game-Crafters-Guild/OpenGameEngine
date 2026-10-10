struct SprayParticle
{
    vec4 positionAge;
    vec4 velocityLife;
    vec4 look;
    vec4 radiance; // rgb = light scattered toward the camera, lit by the simulation
};
layout(std140, set = 0, binding = 0) uniform SprayParams
{
    mat4 viewProjection;
    mat4 viewMatrix;
    vec4 camera;
    vec4 cameraRight;
    vec4 cameraUp;
    vec4 windTime;   // xz horizontal velocity; w dt
    vec4 emission;   // rate, radius, lifetime, up speed
    vec4 appearance; // start size, end size, opacity, sea level
    vec4 controls;   // capacity, pad, reset, emission threshold
    vec4 shift;      // floating-origin shift since last simulation
    vec4 lightColor;
    vec4 viewport;
    vec4 layers[7];
    uvec4 cascadeMeta;
    vec4 interaction;    // enabled, depth contact band, crest share, pad
    uvec4 resourceFlags; // clip available, flow available, frame seed, lighting inputs
};
// resourceFlags.w bits: the lighting inputs the simulation found bound.
const uint GE_OCEAN_SPRAY_SUN_SHADOWS = 1u;
const uint GE_OCEAN_SPRAY_IBL = 2u;
uint SprayHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float SprayRandom(uint x)
{
    return float(SprayHash(x) & 0x00ffffffu) / 16777216.0;
}
