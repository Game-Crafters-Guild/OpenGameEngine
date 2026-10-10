#version 450

// MSM4 separable Gaussian blur — horizontal pass.
//
// Reads from a single layer of the moments 2DArray (selected via push
// constant cascadeIdx) and writes a layer of the ping-pong array. The
// vertical pass (msm_blur_v.frag) consumes this output and writes back
// into the moments array layer the read-side will sample.
//
// blurMode (push constant):
//   0 = Linear5Tap   (fast, 5 bilinear samples, default)
//   1 = Discrete9Tap (sharper, 9 point samples, fallback for A/B)
// Both target sigma=2 — the linear-sampled version pairs adjacent
// taps via offset bilinear sampling so the integrated Gaussian is
// equivalent to ~9 tap weights at half the texture-fetch cost.
//
// Edge-clamp to half-texel inset so wraparound doesn't pollute moments
// near cascade boundaries.

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2DArray uTex;
layout(push_constant) uniform BlurPC
{
    float scale;       // blur radius scale (1.0 = single texel step)
    int   cascadeIdx;  // which array layer to read/write
    int   blurMode;    // 0=Linear5Tap, 1=Discrete9Tap
    int   _pad;
} pc;

void main()
{
    ivec3 ts = textureSize(uTex, 0);
    vec2 texel = 1.0 / vec2(max(1, ts.x), max(1, ts.y));
    vec2 uv = gl_FragCoord.xy * texel;
    vec2 eps = 0.5 / max(vec2(ts.xy), vec2(1.0));
    vec2 maxUV = vec2(1.0) - eps;
    float layer = float(pc.cascadeIdx);

    vec4 sum = vec4(0.0);

    if (pc.blurMode == 1)
    {
        // 9-tap point-sampled Gaussian (sigma ~= 2).
        const float w0 = 0.2042;
        const float w1 = 0.1802;
        const float w2 = 0.1239;
        const float w3 = 0.0663;
        const float w4 = 0.0276;

        vec2 stepH = texel * vec2(pc.scale, 0.0);
        sum += w4 * texture(uTex, vec3(clamp(uv + stepH * -4.0, eps, maxUV), layer));
        sum += w3 * texture(uTex, vec3(clamp(uv + stepH * -3.0, eps, maxUV), layer));
        sum += w2 * texture(uTex, vec3(clamp(uv + stepH * -2.0, eps, maxUV), layer));
        sum += w1 * texture(uTex, vec3(clamp(uv + stepH * -1.0, eps, maxUV), layer));
        sum += w0 * texture(uTex, vec3(clamp(uv,                eps, maxUV), layer));
        sum += w1 * texture(uTex, vec3(clamp(uv + stepH *  1.0, eps, maxUV), layer));
        sum += w2 * texture(uTex, vec3(clamp(uv + stepH *  2.0, eps, maxUV), layer));
        sum += w3 * texture(uTex, vec3(clamp(uv + stepH *  3.0, eps, maxUV), layer));
        sum += w4 * texture(uTex, vec3(clamp(uv + stepH *  4.0, eps, maxUV), layer));
    }
    else
    {
        // 5-tap bilinear-sampled Gaussian (sigma ~= 2). Pairs of the
        // 9-tap weights are combined via a single bilinear fetch at a
        // pre-weighted offset:
        //   ±1,±2 -> weight 0.3041, offset 1.4076 texels (= (1*w1+2*w2)/(w1+w2))
        //   ±3,±4 -> weight 0.0939, offset 3.2940 texels (= (3*w3+4*w4)/(w3+w4))
        const float wCenter = 0.2042;
        const float wInner  = 0.3041;
        const float oInner  = 1.4076;
        const float wOuter  = 0.0939;
        const float oOuter  = 3.2940;

        vec2 stepH = texel * vec2(pc.scale, 0.0);
        sum += wOuter  * texture(uTex, vec3(clamp(uv - stepH * oOuter, eps, maxUV), layer));
        sum += wInner  * texture(uTex, vec3(clamp(uv - stepH * oInner, eps, maxUV), layer));
        sum += wCenter * texture(uTex, vec3(clamp(uv,                  eps, maxUV), layer));
        sum += wInner  * texture(uTex, vec3(clamp(uv + stepH * oInner, eps, maxUV), layer));
        sum += wOuter  * texture(uTex, vec3(clamp(uv + stepH * oOuter, eps, maxUV), layer));
    }

    oColor = sum;
}
