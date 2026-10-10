#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uSrc;

layout(push_constant) uniform FogGlowDownsamplePC
{
    int fogGlowAntiFlicker;
    int antiFlickerStage;
} pc;

float Brightness(vec3 c) { return max(max(c.r, c.g), c.b); }

void main()
{
    if (pc.fogGlowAntiFlicker != 0 && pc.antiFlickerStage != 0)
    {
        // KinoBloom's first downsample Karis weighting follows the median
        // prefilter. See ThirdParty/KinoBloom/LICENSE.md and UPSTREAM.md.
        vec2 d = 1.0 / vec2(textureSize(uSrc, 0));
        vec3 s1 = texture(uSrc, vUV + vec2(-d.x, -d.y)).rgb;
        vec3 s2 = texture(uSrc, vUV + vec2( d.x, -d.y)).rgb;
        vec3 s3 = texture(uSrc, vUV + vec2(-d.x,  d.y)).rgb;
        vec3 s4 = texture(uSrc, vUV + vec2( d.x,  d.y)).rgb;
        float w1 = 1.0 / (Brightness(s1) + 1.0);
        float w2 = 1.0 / (Brightness(s2) + 1.0);
        float w3 = 1.0 / (Brightness(s3) + 1.0);
        float w4 = 1.0 / (Brightness(s4) + 1.0);
        float invWeight = 1.0 / max(w1 + w2 + w3 + w4, 1e-6);
        oColor = vec4((s1 * w1 + s2 * w2 + s3 * w3 + s4 * w4) * invWeight, 1.0);
        return;
    }

    vec2 hp = 0.5 / vec2(textureSize(uSrc, 0));
    vec3 sum = texture(uSrc, vUV).rgb * 4.0;
    sum += texture(uSrc, vUV + vec2( hp.x,  hp.y)).rgb;
    sum += texture(uSrc, vUV + vec2(-hp.x,  hp.y)).rgb;
    sum += texture(uSrc, vUV + vec2( hp.x, -hp.y)).rgb;
    sum += texture(uSrc, vUV + vec2(-hp.x, -hp.y)).rgb;
    oColor = vec4(sum * (1.0 / 8.0), 1.0);
}
