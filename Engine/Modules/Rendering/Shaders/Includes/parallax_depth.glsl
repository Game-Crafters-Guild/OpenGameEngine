// The relief's depth: the depth buffer value of a point farther along the view ray than the fragment,
// the relief hit the march found (parallax design, D7), and its inverse, which turns the depth the
// prepass wrote back into that hit. Included by the forward adapter under GE_PARALLAX_RELIEF_DEPTH only.
//
// Reverse-Z with a D32 buffer, compare GreaterOrEqual: nearer is larger, so a point farther along the
// ray has a depth no larger than the fragment's own, and every value written here is <= z, which is
// what layout(depth_less) promises the driver.
//
// The block between the GE_SHARED_PARALLAX_DEPTH markers is lifted verbatim into the host tests
// (ExtractShaderBlock.cmake -> Tests/ParallaxOcclusionTests.cpp) and compiled as C++ through GlslShim.h.
#ifndef GE_PARALLAX_DEPTH_GLSL
#define GE_PARALLAX_DEPTH_GLSL

// GE_SHARED_PARALLAX_DEPTH_BEGIN
// The tolerant colour pass's slack beyond one linear step, relative to the depth: the ulps by which two
// separately compiled marches can disagree. 2^-20.
const float kParallaxDepthRelativeTolerance = 9.5367431640625e-7f;

// The depth of the point offsetMetres (>= 0) farther along the view ray than a fragment of depth
// `depth` whose eye distance is eyeDistance metres. depthScale is the projection's P[2][2] (A);
// perspective is whether the projection divides by the view depth (P[2][3] != 0).
//   perspective   depth = A + B / z_view, and moving from distance D to D + d along the ray scales
//                 z_view by (D + d) / D, so (depth - A) scales by D / (D + d)
//   orthographic  depth = A z_view + B, and the ray runs along the view axis, so depth moves by A d
// No offset returns `depth` itself, so the region the relief fades out of keeps it to the bit.
float GE_ParallaxDepthAlongRay(float depth, float depthScale, bool perspective, float offsetMetres,
                               float eyeDistance)
{
    if (!(offsetMetres > 0.0f))
        return depth;
    precise float shifted = perspective
        ? depthScale + (depth - depthScale) * (eyeDistance / (eyeDistance + offsetMetres))
        : depth + depthScale * offsetMetres;
    return shifted;
}

// The depth the prepass writes, and the colour pass when it is the depth's sole writer: the hit's,
// clamp(z', 0, z).
float GE_ParallaxExactDepth(float depth, float depthScale, bool perspective, float offsetMetres,
                            float eyeDistance)
{
    return clamp(GE_ParallaxDepthAlongRay(depth, depthScale, perspective, offsetMetres, eyeDistance), 0.0f,
                 depth);
}

// The value a colour pass that cannot read the prepass's depth tests at: one linear step (stepMetres)
// nearer than its own hit, plus kParallaxDepthRelativeTolerance, never above the fragment's own depth. Its
// pipeline writes no depth, so the prepass's exact value stays in the buffer.
float GE_ParallaxToleratedDepth(float depth, float depthScale, bool perspective, float offsetMetres,
                                float stepMetres, float eyeDistance)
{
    float nearer =
        GE_ParallaxDepthAlongRay(depth, depthScale, perspective, max(offsetMetres - stepMetres, 0.0f), eyeDistance);
    return min(depth, nearer + nearer * kParallaxDepthRelativeTolerance);
}

// The inverse of GE_ParallaxDepthAlongRay: how far along the view ray, metres, behind a fragment of depth
// `depth` lies the point whose depth is storedDepth. 0 for a stored depth at or in front of the fragment
// (nothing behind the polygon to rebuild).
//   perspective   (storedDepth - A) = (depth - A) D / (D + d), so d = D (depth - storedDepth) / (storedDepth - A)
//   orthographic  storedDepth = depth + A d, so d = (storedDepth - depth) / A
float GE_ParallaxOffsetToDepth(float depth, float depthScale, bool perspective, float storedDepth, float eyeDistance)
{
    if (!(storedDepth < depth))
        return 0.0f;
    precise float offset = perspective ? eyeDistance * (depth - storedDepth) / (storedDepth - depthScale)
                                       : (storedDepth - depth) / depthScale;
    return offset;
}
// A bound on the error of GE_ParallaxOffsetToDepth, metres. Each of the two depths it reads carries the
// rounding of a 32-bit float, one ulp at most (2^-23 relative); the inverse divides their small
// difference, so the error grows with the eye distance and is largest where the stored depth is close to
// the fragment's. Twice the first-order bound covers the rounding of the write and of the inverse.
float GE_ParallaxOffsetError(float depth, float depthScale, bool perspective, float storedDepth, float eyeDistance)
{
    float storedUlp = abs(storedDepth) * 1.1920929e-7f;
    float depthUlp = abs(depth) * 1.1920929e-7f;
    if (!perspective)
        return 2.0f * (storedUlp + depthUlp) / abs(depthScale);
    float stored = max(storedDepth - depthScale, 1.0e-30f);
    return 2.0f * eyeDistance * ((depth - depthScale) * storedUlp / (stored * stored) + depthUlp / stored);
}
// GE_SHARED_PARALLAX_DEPTH_END

#endif // GE_PARALLAX_DEPTH_GLSL
