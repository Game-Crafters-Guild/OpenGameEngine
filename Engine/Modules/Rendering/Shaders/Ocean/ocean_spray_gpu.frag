#version 450
#include "Ocean/ocean_spray_gpu_common.glsl"
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;
layout(location = 0) in vec2 uv;
layout(location = 1) in float opacity;
layout(location = 2) in float eyeDepth;
layout(location = 3) in float seed;
layout(location = 4) in vec3 radiance;
layout(location = 0) out vec4 outColor;
void main()
{
    vec2 screenUV = gl_FragCoord.xy / viewport.xy;
    float depth = texture(sceneDepth, screenUV).r;
    if (depth > gl_FragCoord.z + 1e-6)
        discard;
    float sceneEye = depth > 1e-6 ? viewport.z / max(depth + viewport.w, 1e-6) : 1e6;
    float soft = clamp((sceneEye - eyeDepth) / 0.4, 0, 1);
    vec2 p = uv + 0.07 * vec2(sin(uv.y * 5 + seed * 6.28), cos(uv.x * 5 + seed * 6.28));
    float r = dot(p, p);
    float a = opacity * exp(-4 * r) * (1 - smoothstep(0.6, 1, r)) * soft * 0.65;
    a *= smoothstep(0.05, 0.5, eyeDepth);
    if (a < 0.001)
        discard;
    outColor = vec4(radiance * a, a);
}
