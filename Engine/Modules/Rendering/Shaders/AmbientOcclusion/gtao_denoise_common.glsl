#ifndef GE_GTAO_DENOISE_COMMON_GLSL
#define GE_GTAO_DENOISE_COMMON_GLSL

#include "AmbientOcclusion/gtao_geometry.glsl"
const float kGtaoPlaneTolerance = 0.005;

// Raw device depth is affine across a projected plane. Choose the smaller
// one-sided difference to avoid fitting the gradient across a silhouette.
float GE_GtaoSlope(sampler2D depth, vec2 uv, vec2 stepUV, float center)
{
    vec2 a = uv - stepUV;
    vec2 b = uv + stepUV;
    float da = GE_GtaoInside(a) ? textureLod(depth, a, 0.0).r : 0.0;
    float db = GE_GtaoInside(b) ? textureLod(depth, b, 0.0).r : 0.0;
    if (da <= 0.0 && db <= 0.0) return 0.0;
    if (da <= 0.0) return db - center;
    if (db <= 0.0) return center - da;
    return abs(center - da) < abs(db - center) ? center - da : db - center;
}

vec2 GE_GtaoDepthSlope(sampler2D depth, vec2 uv, float center)
{
    vec2 texel = 1.0 / vec2(textureSize(depth, 0));
    return vec2(GE_GtaoSlope(depth, uv, vec2(texel.x, 0.0), center),
                GE_GtaoSlope(depth, uv, vec2(0.0, texel.y), center)) / texel;
}

float GE_GtaoDepthWeight(float raw, float predictedRaw, float centerDepth)
{
    if (raw <= 0.0 || predictedRaw <= 0.0 || predictedRaw > 1.0) return 0.0;
    float residual = abs(GE_LinearizeDepth(raw) - GE_LinearizeDepth(predictedRaw));
    float error = residual / max(centerDepth * kGtaoPlaneTolerance, 1e-5);
    // Compact support prevents tiny, unrelated weights becoming a full-strength
    // AO value after normalization when every candidate crosses an edge.
    return 1.0 - smoothstep(0.0, 3.0, error);
}

#endif
