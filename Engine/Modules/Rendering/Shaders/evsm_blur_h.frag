#version 450

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(push_constant) uniform BlurPC { float scale; } pc;

void main()
{
    ivec2 ts = textureSize(uTex, 0);
    vec2 texel = 1.0 / vec2(max(1, ts.x), max(1, ts.y));
    vec2 uv = gl_FragCoord.xy * texel;

    vec2 eps = 0.5 / max(vec2(ts), vec2(1.0));

    // 9-tap 1D Gaussian blur horizontally (sigma ~= 2)
    float w0 = 0.2042; // center
    float w1 = 0.1802;
    float w2 = 0.1239;
    float w3 = 0.0663;
    float w4 = 0.0276;

    vec2 stepH = texel * vec2(pc.scale, 0.0);

    vec4 sum = vec4(0.0);
    sum += w4 * texture(uTex, clamp(uv + stepH * -4.0, eps, vec2(1.0) - eps));
    sum += w3 * texture(uTex, clamp(uv + stepH * -3.0, eps, vec2(1.0) - eps));
    sum += w2 * texture(uTex, clamp(uv + stepH * -2.0, eps, vec2(1.0) - eps));
    sum += w1 * texture(uTex, clamp(uv + stepH * -1.0, eps, vec2(1.0) - eps));
    sum += w0 * texture(uTex, clamp(uv, eps, vec2(1.0) - eps));
    sum += w1 * texture(uTex, clamp(uv + stepH * 1.0, eps, vec2(1.0) - eps));
    sum += w2 * texture(uTex, clamp(uv + stepH * 2.0, eps, vec2(1.0) - eps));
    sum += w3 * texture(uTex, clamp(uv + stepH * 3.0, eps, vec2(1.0) - eps));
    sum += w4 * texture(uTex, clamp(uv + stepH * 4.0, eps, vec2(1.0) - eps));

    oColor = sum;
}

