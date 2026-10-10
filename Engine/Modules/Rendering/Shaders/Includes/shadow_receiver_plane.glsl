#ifndef GE_SHADOW_RECEIVER_PLANE_GLSL
#define GE_SHADOW_RECEIVER_PLANE_GLSL

// Derivatives of (shadow UV, reverse-Z depth). Test conditioning relative
// to the UV footprint: a small, well-conditioned footprint is not singular.
vec2 GE_ShadowReceiverPlaneGradient(vec3 dx, vec3 dy)
{
    float determinant = dx.x * dy.y - dx.y * dy.x;
    float footprint = length(dx.xy) * length(dy.xy);
    if (footprint <= 1e-20 || abs(determinant) <= footprint * 1e-4)
        return vec2(0.0);
    vec2 gradient = vec2(dy.y * dx.z - dx.y * dy.z,
                         dx.x * dy.z - dy.x * dx.z) / determinant;
#if defined(GE_COMPAT_PROFILE)
    // An all-ones exponent marks both infinity and NaN; WGSL has no isnan/isinf.
    if (any(equal(floatBitsToUint(gradient) & uvec2(0x7f800000u), uvec2(0x7f800000u))))
#else
    if (any(isnan(gradient)) || any(isinf(gradient)))
#endif
        return vec2(0.0);
    return clamp(gradient, vec2(-32.0), vec2(32.0));
}

float GE_ShadowReceiverPlaneDepth(float center, vec2 offset, vec2 gradient, float texelSize)
{
    // The hardware comparison sampler bilinearly combines four texels,
    // each up to one texel from the lookup. Cover that residual footprint
    // after correcting the sample's center to this receiver's plane.
    return center + dot(offset, gradient) + dot(abs(gradient), vec2(texelSize));
}
#endif
