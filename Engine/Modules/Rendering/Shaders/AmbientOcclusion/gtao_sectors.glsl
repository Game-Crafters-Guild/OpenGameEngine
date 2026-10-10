#ifndef GE_GTAO_SECTORS_GLSL
#define GE_GTAO_SECTORS_GLSL

const float GE_GTAO_PI = 3.141592653589793;
const float GE_GTAO_PI_HALF = 1.570796326794897;
const vec2 GE_GTAO_BOUNDARIES[33] = vec2[33](
    vec2(-1.0000000000, 0.0000000000),
    vec2(-0.9951847267, 0.0980171403),
    vec2(-0.9807852804, 0.1950903220),
    vec2(-0.9569403357, 0.2902846773),
    vec2(-0.9238795325, 0.3826834324),
    vec2(-0.8819212643, 0.4713967368),
    vec2(-0.8314696123, 0.5555702330),
    vec2(-0.7730104534, 0.6343932842),
    vec2(-0.7071067812, 0.7071067812),
    vec2(-0.6343932842, 0.7730104534),
    vec2(-0.5555702330, 0.8314696123),
    vec2(-0.4713967368, 0.8819212643),
    vec2(-0.3826834324, 0.9238795325),
    vec2(-0.2902846773, 0.9569403357),
    vec2(-0.1950903220, 0.9807852804),
    vec2(-0.0980171403, 0.9951847267),
    vec2(0.0000000000, 1.0000000000),
    vec2(0.0980171403, 0.9951847267),
    vec2(0.1950903220, 0.9807852804),
    vec2(0.2902846773, 0.9569403357),
    vec2(0.3826834324, 0.9238795325),
    vec2(0.4713967368, 0.8819212643),
    vec2(0.5555702330, 0.8314696123),
    vec2(0.6343932842, 0.7730104534),
    vec2(0.7071067812, 0.7071067812),
    vec2(0.7730104534, 0.6343932842),
    vec2(0.8314696123, 0.5555702330),
    vec2(0.8819212643, 0.4713967368),
    vec2(0.9238795325, 0.3826834324),
    vec2(0.9569403357, 0.2902846773),
    vec2(0.9807852804, 0.1950903220),
    vec2(0.9951847267, 0.0980171403),
    vec2(1.0000000000, 0.0000000000));

uint GE_GtaoSectorMask(vec2 span)
{
    if (span.y - span.x <= 1e-7) return 0u;
    uint first = uint(clamp(floor(span.x * 32.0), 0.0, 32.0));
    uint end = uint(clamp(ceil(span.y * 32.0), 0.0, 32.0));
    if (first >= end || first >= 32u) return 0u;
    uint count = end - first;
    return count == 32u ? 0xFFFFFFFFu : ((1u << count) - 1u) << first;
}

// Antiderivatives from zero of cosine-weighted spherical area and its first
// directional moment in a view-aligned slice. dOmega = abs(sin(theta)) dTheta
// dPhi; projected normal contributes cos(theta-n). The sign splits at theta=0.
// Returns (visibility, tangent moment, view moment). Subtract endpoints to
// integrate each open sector, with exactly the same visibility for AO and bent N.
vec3 GE_GtaoMomentPrimitive(float theta, float s, float c, float sn, float cn)
{
    float visibility = 0.5 * cn * s * s + sn * (0.5 * theta - 0.5 * s * c);
    float tangent = (cn * s * s * s + sn * (c * c * c - 3.0 * c + 2.0)) / 3.0;
    float view = (cn * (1.0 - c * c * c) + sn * s * s * s) / 3.0;
    return sign(theta) * vec3(visibility, tangent, view);
}

vec3 GE_GtaoBoundaryMoment(uint index, float n, float sn, float cn)
{
    vec2 b = GE_GTAO_BOUNDARIES[index];
    return GE_GtaoMomentPrimitive(n + (float(index) / 32.0 - 0.5) * GE_GTAO_PI,
                                  b.x * cn + b.y * sn, b.y * cn - b.x * sn, sn, cn);
}

vec3 GE_GtaoVisibleMoments(uint mask, float n, float sn, float cn)
{
    if (mask == 0xFFFFFFFFu) return vec3(0.0);
    vec3 sum = GE_GtaoBoundaryMoment(32u, n, sn, cn) - GE_GtaoBoundaryMoment(0u, n, sn, cn);
    // Integrate contiguous occluded runs instead of visiting all 32 sectors.
    // Typical silhouettes occupy one or two runs; the empty mask is free.
    while (mask != 0u)
    {
        uint first = uint(findLSB(mask));
        // Unsigned throughout: WGSL's firstTrailingBit keeps the operand type,
        // so a signed "not found" sentinel does not translate.
        uint remaining = ~(mask >> first);
        uint end = remaining == 0u ? 32u : first + uint(findLSB(remaining));
        sum -= GE_GtaoBoundaryMoment(end, n, sn, cn) - GE_GtaoBoundaryMoment(first, n, sn, cn);
        uint run = end == 32u ? (0xFFFFFFFFu << first) : (((1u << (end - first)) - 1u) << first);
        mask &= ~run;
    }
    return sum;
}
#endif
