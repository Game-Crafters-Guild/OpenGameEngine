#ifndef GE_SSSR_REPROJECTION_MATH_GLSL
#define GE_SSSR_REPROJECTION_MATH_GLSL
vec2 GE_SssrPreviousRasterUv(vec2 uv, vec2 motion, vec2 currentJitterNdc, vec2 previousJitterNdc)
{
    return uv - currentJitterNdc * vec2(0.5, -0.5) - motion +
                previousJitterNdc * vec2(0.5, -0.5);
}

float GE_SssrMotionPixels(vec2 uv, vec2 previousUv, vec2 currentJitterNdc,
                          vec2 previousJitterNdc, vec2 extent)
{
    vec2 unjitteredDelta = previousUv - previousJitterNdc * vec2(0.5, -0.5) -
                          (uv - currentJitterNdc * vec2(0.5, -0.5));
    return length(unjitteredDelta * extent);
}

vec3 GE_SssrMirrorPoint(vec3 hitPoint, vec3 receiverPoint, vec3 planeNormal)
{
    return hitPoint - 2.0f * planeNormal * dot(hitPoint - receiverPoint, planeNormal);
}
#endif
