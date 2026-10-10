#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform ColorFilterPC {
    float colorFilterR;
    float colorFilterG;
    float colorFilterB;
    float colorFilterIntensity;
    int colorFilterBlendMode;
} pc;

vec3 ApplyColorFilterBlend(vec3 src, vec3 tint, int mode)
{
    if (mode == 1) {          // Add
        return src + tint;
    } else if (mode == 2) {   // Screen
        return 1.0 - (1.0 - src) * (1.0 - tint);
    } else if (mode == 3) {   // Soft Light
        vec3 low  = src - (1.0 - 2.0 * tint) * src * (1.0 - src);
        vec3 g    = mix(((16.0 * src - 12.0) * src + 4.0) * src, sqrt(max(src, vec3(0.0))), step(vec3(0.25), src));
        vec3 high = src + (2.0 * tint - 1.0) * (g - src);
        bvec3 useHigh = greaterThan(tint, vec3(0.5));
        return mix(low, high, useHigh);
    }
    return src * tint;        // Multiply (default)
}

void main()
{
    vec4 src = texture(uSceneColor, vUV);
    vec3 tint = vec3(pc.colorFilterR, pc.colorFilterG, pc.colorFilterB);
    vec3 tinted = ApplyColorFilterBlend(src.rgb, tint, pc.colorFilterBlendMode);
    vec3 outRgb = mix(src.rgb, tinted, clamp(pc.colorFilterIntensity, 0.0, 1.0));
    oColor = vec4(outRgb, src.a);
}
