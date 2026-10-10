#version 450

// MSM4 separable Gaussian blur — vertical pass.
// Reads from a 2D ping-pong target produced by msm_blur_h (single layer)
// and writes to one layer of the moments 2DArray (selected via the
// color attachment's TextureViewDesc baseLayer at the call site).
//
// blurMode (push constant):
//   0 = Linear5Tap   (fast, 5 bilinear samples, default)
//   1 = Discrete9Tap (sharper, 9 point samples, fallback for A/B)
// See msm_blur_h.frag for the kernel derivation.
//
// The horizontal pass uses sampler2DArray to read the per-cascade layer
// of the moments array. This vertical pass reads from the intermediate
// ping-pong which is a 2D texture, so it uses sampler2D — keeps the RG
// transient simple (no need for a 1-layer 2DArray view).

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(push_constant) uniform BlurPC
{
    float scale;
    int   cascadeIdx;  // ignored on V (ping-pong is single-layer); kept for
                       // pipeline-layout symmetry with msm_blur_h.
    int   blurMode;    // 0=Linear5Tap, 1=Discrete9Tap
    int   _pad;
} pc;

void main()
{
    ivec2 ts = textureSize(uTex, 0);
    vec2 texel = 1.0 / vec2(max(1, ts.x), max(1, ts.y));
    vec2 uv = gl_FragCoord.xy * texel;
    vec2 eps = 0.5 / max(vec2(ts), vec2(1.0));
    vec2 maxUV = vec2(1.0) - eps;

    vec4 sum = vec4(0.0);

    if (pc.blurMode == 1)
    {
        // 9-tap point-sampled Gaussian (sigma ~= 2).
        const float w0 = 0.2042;
        const float w1 = 0.1802;
        const float w2 = 0.1239;
        const float w3 = 0.0663;
        const float w4 = 0.0276;

        vec2 stepV = texel * vec2(0.0, pc.scale);
        sum += w4 * texture(uTex, clamp(uv + stepV * -4.0, eps, maxUV));
        sum += w3 * texture(uTex, clamp(uv + stepV * -3.0, eps, maxUV));
        sum += w2 * texture(uTex, clamp(uv + stepV * -2.0, eps, maxUV));
        sum += w1 * texture(uTex, clamp(uv + stepV * -1.0, eps, maxUV));
        sum += w0 * texture(uTex, clamp(uv,                eps, maxUV));
        sum += w1 * texture(uTex, clamp(uv + stepV *  1.0, eps, maxUV));
        sum += w2 * texture(uTex, clamp(uv + stepV *  2.0, eps, maxUV));
        sum += w3 * texture(uTex, clamp(uv + stepV *  3.0, eps, maxUV));
        sum += w4 * texture(uTex, clamp(uv + stepV *  4.0, eps, maxUV));
    }
    else
    {
        // 5-tap bilinear-sampled Gaussian (sigma ~= 2). See msm_blur_h.frag
        // for the offset/weight derivation.
        const float wCenter = 0.2042;
        const float wInner  = 0.3041;
        const float oInner  = 1.4076;
        const float wOuter  = 0.0939;
        const float oOuter  = 3.2940;

        vec2 stepV = texel * vec2(0.0, pc.scale);
        sum += wOuter  * texture(uTex, clamp(uv - stepV * oOuter, eps, maxUV));
        sum += wInner  * texture(uTex, clamp(uv - stepV * oInner, eps, maxUV));
        sum += wCenter * texture(uTex, clamp(uv,                  eps, maxUV));
        sum += wInner  * texture(uTex, clamp(uv + stepV * oInner, eps, maxUV));
        sum += wOuter  * texture(uTex, clamp(uv + stepV * oOuter, eps, maxUV));
    }

    oColor = sum;
}
