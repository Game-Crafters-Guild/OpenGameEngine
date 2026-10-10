#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uHDR;
layout(set = 0, binding = 1) uniform sampler2D uFogGlow;

layout(push_constant) uniform FogGlowCompositePC
{
    float fogGlowIntensity;
} pc;

void main()
{
    vec3 hdr = texture(uHDR, vUV).rgb;
    vec3 glow = max(texture(uFogGlow, vUV).rgb, vec3(0.0));
    oColor = vec4(hdr + glow * max(pc.fogGlowIntensity, 0.0), 1.0);
}
