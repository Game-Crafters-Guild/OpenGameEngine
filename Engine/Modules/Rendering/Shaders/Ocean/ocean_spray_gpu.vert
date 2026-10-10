#version 450
#include "Ocean/ocean_spray_gpu_common.glsl"
layout(std430, set = 0, binding = 1) readonly buffer Particles
{
    SprayParticle particles[];
};
layout(std430, set = 0, binding = 2) readonly buffer Visible
{
    uint visible[];
};
layout(location = 0) out vec2 uv;
layout(location = 1) out float opacity;
layout(location = 2) out float eyeDepth;
layout(location = 3) out float seed;
layout(location = 4) out vec3 radiance;
// Exposure (s) a droplet's streak covers: a sprite stretches along its screen
// motion by speed times this, so fast crest spray reads as streaks, not discs.
const float kSprayStreakSeconds = 0.035;
void main()
{
    const vec2 corners[6] =
        vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
    SprayParticle p = particles[visible[gl_InstanceIndex]];
    float age = clamp(p.positionAge.w / max(p.velocityLife.w, 0.001), 0, 1);
    vec2 corner = corners[gl_VertexIndex];
    float size = mix(p.look.x, p.look.y, age);
    // Orient the sprite along the velocity projected onto the view plane.
    vec2 screenVelocity = vec2(dot(p.velocityLife.xyz, cameraRight.xyz), dot(p.velocityLife.xyz, cameraUp.xyz));
    float screenSpeed = length(screenVelocity);
    vec2 along = screenSpeed > 1e-3 ? screenVelocity / screenSpeed : vec2(0, 1);
    vec3 alongWorld = cameraRight.xyz * along.x + cameraUp.xyz * along.y;
    vec3 acrossWorld = cameraUp.xyz * along.x - cameraRight.xyz * along.y;
    float halfLength = size + 0.5 * screenSpeed * kSprayStreakSeconds;
    vec3 world = p.positionAge.xyz + acrossWorld * (corner.x * size) + alongWorld * (corner.y * halfLength);
    gl_Position = viewProjection * vec4(world, 1);
    uv = corner;
    seed = p.look.w;
    radiance = p.radiance.rgb;
    // Fades in at birth, then thins steadily as the droplet cluster disperses.
    opacity = p.look.z * smoothstep(0, 0.08, age) * (1 - age) * (1 - age);
    eyeDepth = abs((viewMatrix * vec4(world, 1)).z);
}
