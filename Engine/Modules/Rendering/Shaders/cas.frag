#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform CasPC {
    float casStrength; // 0 = bypass, 1 = strongest sharpening.
} pc;

float Luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    float w = clamp(pc.casStrength, 0.0, 1.0);
    vec4 center = texture(uSceneColor, vUV);
    if (w <= 1e-5)
    {
        oColor = center;
        return;
    }

    vec2 texel = 1.0 / vec2(textureSize(uSceneColor, 0));

    vec3 n = texture(uSceneColor, vUV + vec2(0.0, -texel.y)).rgb;
    vec3 s = texture(uSceneColor, vUV + vec2(0.0,  texel.y)).rgb;
    vec3 e = texture(uSceneColor, vUV + vec2( texel.x, 0.0)).rgb;
    vec3 wv = texture(uSceneColor, vUV + vec2(-texel.x, 0.0)).rgb;
    vec3 ne = texture(uSceneColor, vUV + vec2( texel.x, -texel.y)).rgb;
    vec3 nw = texture(uSceneColor, vUV + vec2(-texel.x, -texel.y)).rgb;
    vec3 se = texture(uSceneColor, vUV + vec2( texel.x,  texel.y)).rgb;
    vec3 sw = texture(uSceneColor, vUV + vec2(-texel.x,  texel.y)).rgb;

    vec3 c = center.rgb;
    vec3 avgCardinal = (n + s + e + wv) * 0.25;
    vec3 avgDiagonal = (ne + nw + se + sw) * 0.25;
    vec3 avg = mix(avgCardinal, avgDiagonal, 0.35);
    vec3 detail = c - avg;

    float lC = Luma(c);
    float lN = Luma(n);
    float lS = Luma(s);
    float lE = Luma(e);
    float lW = Luma(wv);
    float lNE = Luma(ne);
    float lNW = Luma(nw);
    float lSE = Luma(se);
    float lSW = Luma(sw);
    float lMin = min(min(lC, min(min(lN, lS), min(lE, lW))), min(min(lNE, lNW), min(lSE, lSW)));
    float lMax = max(max(lC, max(max(lN, lS), max(lE, lW))), max(max(lNE, lNW), max(lSE, lSW)));
    float contrast = max(lMax - lMin, 1e-4);

    // Adaptive suppression in high-contrast edges to reduce haloing.
    float adaptive = 1.0 - smoothstep(0.08, 0.40, contrast);
    float gain = w * mix(2.8, 0.75, 1.0 - adaptive);

    vec3 sharpened = c + detail * gain;
    oColor = vec4(max(sharpened, vec3(0.0)), center.a);
}
