// Slug text rendering functions for the UI fragment shader.
// Ported from Eric Lengyel's reference HLSL (MIT license).
// Evaluates quadratic Bezier curves per-pixel via horizontal/vertical
// ray casting with band-based spatial acceleration.
//
// Deviation from the reference: the per-crossing coverage ramp is the
// exact-area ramp convolved with the kTextFilterWidthPx box (see
// slugFilteredRamp), matching the reconstruction filter measured out of
// Chrome's text rasteriser.
//
// Requires ui_sdf_common.glsl to be included first.
// Curve texture: sampler2D (RGBA16F) — control points.
// Band texture: usampler2D (RG16UI) — spatial acceleration indices.

#ifndef SLUG_FUNCTIONS_GLSL
#define SLUG_FUNCTIONS_GLSL

// The curve and band textures use a fixed width of 4096 texels.
const int kLogBandTextureWidth = 12;

// Hard ceiling on a band's curve count.
//
// Both band loops take their trip count from texel data fetched out of a
// bindless utexture2D, so a descriptor that does not point at a live band
// texture supplies an arbitrary 32-bit integer. Unclamped that is a
// multi-billion iteration loop issuing three texelFetches each — a GPU hang,
// not a glitch. A fetched value must never be a loop bound; clamp at the fetch.
//
// The bound is the band texture width: a band's curve-index list is a
// contiguous run in a 4096-wide texture (slugCalcBandLoc wraps x into the next
// row), so a list longer than one full row has already outgrown the addressing
// scheme the packer and the shader agree on. Real glyphs sit orders of
// magnitude below it — a band holds one entry per curve whose extent crosses
// it, and a whole complex glyph is a few hundred curves — so the clamp cannot
// truncate correct data, only bound corrupt data.
const int kMaxCurvesPerBand = 1 << kLogBandTextureWidth;

// Antiderivative of the exact-area ramp clamp(t + 0.5, 0, 1).
float slugRampIntegral(float t)
{
    float s = clamp(t, -0.5, 0.5) + 0.5;
    return 0.5 * s * s + max(t - 0.5, 0.0);
}

// Coverage contribution of a curve crossing at signed distance r device px
// from the sample centre: the exact-area ramp convolved with the
// kTextFilterWidthPx box (ui_sdf_common.glsl), evaluated in closed form as
// the ramp's mean over the filter window. A box convolution of exact-area
// coverage is itself exact over the wider window, so the result stays
// analytic and mass-preserving; it saturates to 0/1 outside
// +-kTextFilterOuterRadiusPx exactly as the unfiltered ramp does at +-0.5.
float slugFilteredRamp(float r)
{
    const float halfWidth = 0.5 * kTextFilterWidthPx;
    return (slugRampIntegral(r + halfWidth) - slugRampIntegral(r - halfWidth))
           * (1.0 / kTextFilterWidthPx);
}

// Ray-reliability weight for a crossing: 1 at the sample centre, fading to 0
// at the edge of the crossing's widened influence window.
float slugCrossingWeight(float r)
{
    return clamp(1.0 - abs(r) * (1.0 / kTextFilterOuterRadiusPx), 0.0, 1.0);
}

uint slugCalcRootCode(float y1, float y2, float y3)
{
    // Calculate the root eligibility code for a sample-relative quadratic Bezier curve.
    // Extract the signs of the y coordinates of the three control points.

    uint i1 = floatBitsToUint(y1) >> 31u;
    uint i2 = floatBitsToUint(y2) >> 30u;
    uint i3 = floatBitsToUint(y3) >> 29u;

    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);

    // Eligibility is returned in bits 0 and 8.
    return ((0x2E74u >> shift) & 0x0101u);
}

vec2 slugSolveHorizPoly(vec4 p12, vec2 p3)
{
    // Solve for the values of t where the curve crosses y = 0.
    // Quadratic: a*t^2 - 2*b*t + c,
    // where a = p1.y - 2*p2.y + p3.y, b = p1.y - p2.y, c = p1.y.

    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;

    // Division moved inside branches to avoid speculative NaN on some GPUs.
    if (abs(a.y) < 1.0 / 65536.0)
    {
        // Nearly linear: solve -2*b*t + c = 0.
        float t = p12.y * 0.5 / b.y;
        float x = (a.x * t - b.x * 2.0) * t + p12.x;
        return vec2(x, x);
    }

    float ra = 1.0 / a.y;
    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 slugSolveVertPoly(vec4 p12, vec2 p3)
{
    // Solve for the values of t where the curve crosses x = 0.

    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;

    if (abs(a.x) < 1.0 / 65536.0)
    {
        float t = p12.x * 0.5 / b.x;
        float y = (a.y * t - b.y * 2.0) * t + p12.y;
        return vec2(y, y);
    }

    float ra = 1.0 / a.x;
    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y, (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

ivec2 slugCalcBandLoc(ivec2 glyphLoc, uint offset)
{
    // If the offset causes the x coordinate to exceed the texture width, wrap to the next line.
    ivec2 bandLoc = ivec2(glyphLoc.x + int(offset), glyphLoc.y);
    bandLoc.y += bandLoc.x >> kLogBandTextureWidth;
    bandLoc.x &= (1 << kLogBandTextureWidth) - 1;
    return bandLoc;
}

float slugCalcCoverage(float xcov, float ycov, float xwgt, float ywgt)
{
    // Combine coverages from the horizontal and vertical rays using their weights.
    float coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
                         min(abs(xcov), abs(ycov)));

    // Nonzero fill rule.
    return clamp(coverage, 0.0, 1.0);
}

float slugRender(texture2D curveData, utexture2D bandData,
                 sampler curveSampler, sampler bandSampler,
                 vec2 renderCoord, vec4 bandTransform, ivec4 glyphData)
{
    int curveIndex;

    // The effective pixel dimensions of the em square are computed
    // independently for x and y directions with texcoord derivatives.
    // renderCoord rides the vUV interpolant (offset by at most a subpixel
    // stripe step, itself a derivative), so the hoisted value the compat arm
    // reads is the same quantity — it is a relocation, not an approximation.
#if defined(GE_COMPAT_PROFILE)
    vec2 emsPerPixel = ge_UiUvFwidth();
#else
    vec2 emsPerPixel = fwidth(renderCoord);
#endif
    vec2 pixelsPerEm = 1.0 / emsPerPixel;

    ivec2 bandMax = glyphData.zw;
    bandMax.y &= 0x00FF;

    // Determine what bands the current pixel lies in.
    ivec2 bandIndex = clamp(ivec2(renderCoord * bandTransform.xy + bandTransform.zw),
                            ivec2(0, 0), bandMax);
    ivec2 glyphLoc = glyphData.xy;

    float xcov = 0.0;
    float xwgt = 0.0;

    // Fetch data for the horizontal band from the band texture.
    uvec2 hbandData = texelFetch(usampler2D(bandData, bandSampler), ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
    ivec2 hbandLoc = slugCalcBandLoc(glyphLoc, hbandData.y);

    // Loop over all curves in the horizontal band. The count is fetched data,
    // so it is clamped (kMaxCurvesPerBand) before it becomes a trip count.
    // The min runs in uint space: hbandData.x is unsigned, and narrowing to
    // int first would map the top half of the range onto negative values.
    int hbandCurveCount = int(min(hbandData.x, uint(kMaxCurvesPerBand)));
    for (curveIndex = 0; curveIndex < hbandCurveCount; curveIndex++)
    {
        // Fetch the location of the current curve.
        ivec2 curveLoc = ivec2(texelFetch(usampler2D(bandData, bandSampler), ivec2(hbandLoc.x + curveIndex, hbandLoc.y), 0).xy);

        // Fetch the three 2D control points. Subtract render coordinates to
        // make the curve relative to the sample position.
        vec4 p12 = texelFetch(sampler2D(curveData, curveSampler), curveLoc, 0) - vec4(renderCoord, renderCoord);
        vec2 p3 = texelFetch(sampler2D(curveData, curveSampler), ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

        // Early exit: if the largest x coordinate falls beyond the filter's
        // reach left of the current pixel, no more curves can influence the
        // result (sorted by descending max x).
        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -kTextFilterOuterRadiusPx) break;

        uint code = slugCalcRootCode(p12.y, p12.w, p3.y);
        if (code != 0u)
        {
            vec2 r = slugSolveHorizPoly(p12, p3) * pixelsPerEm.x;

            if ((code & 1u) != 0u)
            {
                xcov += slugFilteredRamp(r.x);
                xwgt = max(xwgt, slugCrossingWeight(r.x));
            }

            if (code > 1u)
            {
                xcov -= slugFilteredRamp(r.y);
                xwgt = max(xwgt, slugCrossingWeight(r.y));
            }
        }
    }

    float ycov = 0.0;
    float ywgt = 0.0;

    // Fetch data for the vertical band. This follows the horizontal bands,
    // so we offset by bandMax.y + 1.
    uvec2 vbandData = texelFetch(usampler2D(bandData, bandSampler), ivec2(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y), 0).xy;
    ivec2 vbandLoc = slugCalcBandLoc(glyphLoc, vbandData.y);

    // Loop over all curves in the vertical band, clamped as above.
    int vbandCurveCount = int(min(vbandData.x, uint(kMaxCurvesPerBand)));
    for (curveIndex = 0; curveIndex < vbandCurveCount; curveIndex++)
    {
        ivec2 curveLoc = ivec2(texelFetch(usampler2D(bandData, bandSampler), ivec2(vbandLoc.x + curveIndex, vbandLoc.y), 0).xy);
        vec4 p12 = texelFetch(sampler2D(curveData, curveSampler), curveLoc, 0) - vec4(renderCoord, renderCoord);
        vec2 p3 = texelFetch(sampler2D(curveData, curveSampler), ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

        // Early exit: if the largest y coordinate falls beyond the filter's
        // reach below the current pixel, no more curves can influence the
        // result (sorted by descending max y).
        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -kTextFilterOuterRadiusPx) break;

        uint code = slugCalcRootCode(p12.x, p12.z, p3.x);
        if (code != 0u)
        {
            vec2 r = slugSolveVertPoly(p12, p3) * pixelsPerEm.y;

            if ((code & 1u) != 0u)
            {
                ycov -= slugFilteredRamp(r.x);
                ywgt = max(ywgt, slugCrossingWeight(r.x));
            }

            if (code > 1u)
            {
                ycov += slugFilteredRamp(r.y);
                ywgt = max(ywgt, slugCrossingWeight(r.y));
            }
        }
    }

    return slugCalcCoverage(xcov, ycov, xwgt, ywgt);
}

// --- Distance to the glyph outline (text effects) ---

// A curve whose second difference b is below 1/32 of its first difference a
// deviates from its chord by |b| / 4, which the cubic below resolves only with
// catastrophic cancellation (its coefficients scale with |a| / |b|). Lines are
// stored as quadratics with the control point on the chord, so b is exactly the
// curve texture's half-float rounding there. Such a curve is measured as the two
// chords through its midpoint instead, within |b| / 16 of the curve.
const float kSlugNearLinearRatio = 32.0;

float slugSegmentDistance(vec2 a, vec2 b)
{
    vec2 ab = b - a;
    float t = clamp(-dot(a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
    return length(a + ab * t);
}

// Distance from the origin to the quadratic Bezier (p1, p2, p3), all in device
// pixels relative to the sample. The nearest point solves the cubic
// dB/dt . B(t) = 0 in closed form; of its up to three real roots the middle one
// is a maximum of the distance, so the outer two are compared.
float slugCurveDistance(vec2 p1, vec2 p2, vec2 p3)
{
    vec2 a = p2 - p1;
    vec2 b = p1 - 2.0 * p2 + p3;
    float bb = dot(b, b);
    if (bb * (kSlugNearLinearRatio * kSlugNearLinearRatio) <= dot(a, a))
    {
        vec2 mid = p1 + a + 0.25 * b;
        return min(slugSegmentDistance(p1, mid), slugSegmentDistance(mid, p3));
    }

    vec2 c = 2.0 * a;
    float kk = 1.0 / bb;
    float kx = kk * dot(a, b);
    float ky = kk * (2.0 * dot(a, a) + dot(p1, b)) / 3.0;
    float kz = kk * dot(p1, a);
    float p = ky - kx * kx;
    float q = kx * (2.0 * kx * kx - 3.0 * ky) + kz;
    float h = q * q + 4.0 * p * p * p;
    if (h >= 0.0)
    {
        h = sqrt(h);
        vec2 x = 0.5 * (vec2(h, -h) - q);
        vec2 uv = sign(x) * pow(abs(x), vec2(1.0 / 3.0));
        float t = clamp(uv.x + uv.y - kx, 0.0, 1.0);
        return length(p1 + (c + b * t) * t);
    }
    float z = sqrt(-p);
    float v = acos(clamp(q / (2.0 * p * z), -1.0, 1.0)) / 3.0;
    float m = cos(v);
    float n = sin(v) * 1.7320508;
    vec2 t = clamp(vec2(m + m, -n - m) * z - kx, 0.0, 1.0);
    return min(length(p1 + (c + b * t.x) * t.x), length(p1 + (c + b * t.y) * t.y));
}

// Unsigned distance, in device pixels, from renderCoord to the glyph outline,
// or reachPx when no outline point lies closer. The glyph's horizontal bands
// hold every curve whose y range meets them, so the bands spanning
// renderCoord.y +- reach hold every curve within reach; a curve listed in two
// of them is measured twice, which the minimum absorbs. Inside/outside is not
// known here: the caller signs the distance with the fill coverage.
float slugOutlineDistance(texture2D curveData, utexture2D bandData,
                          sampler curveSampler, sampler bandSampler,
                          vec2 renderCoord, vec4 bandTransform, ivec4 glyphData,
                          vec2 emsPerPixel, float reachPx)
{
    vec2 pixelsPerEm = 1.0 / max(emsPerPixel, vec2(1e-6));
    int hBandMax = glyphData.w & 0x00FF;
    float reachEm = reachPx * emsPerPixel.y;
    // Clamped twice. As floats, because a float-to-int conversion out of int range
    // is undefined and a degenerate derivative can scale reachEm without bound; as
    // ints, because under fast math a NaN survives the float clamp. The walk then
    // visits at most hBandMax + 1 <= 256 bands, each inside the glyph's band row.
    float bandLimit = float(hBandMax);
    int bandLow = clamp(int(clamp((renderCoord.y - reachEm) * bandTransform.y + bandTransform.w, 0.0, bandLimit)), 0, hBandMax);
    int bandHigh = clamp(int(clamp((renderCoord.y + reachEm) * bandTransform.y + bandTransform.w, 0.0, bandLimit)), 0, hBandMax);

    float best = reachPx;
    for (int band = min(bandLow, bandHigh); band <= max(bandLow, bandHigh); band++)
    {
        uvec2 hbandData = texelFetch(usampler2D(bandData, bandSampler), ivec2(glyphData.x + band, glyphData.y), 0).xy;
        ivec2 hbandLoc = slugCalcBandLoc(glyphData.xy, hbandData.y);
        // Fetched data, clamped before it becomes a trip count (kMaxCurvesPerBand).
        int curveCount = int(min(hbandData.x, uint(kMaxCurvesPerBand)));
        for (int curveIndex = 0; curveIndex < curveCount; curveIndex++)
        {
            ivec2 curveLoc = ivec2(texelFetch(usampler2D(bandData, bandSampler), ivec2(hbandLoc.x + curveIndex, hbandLoc.y), 0).xy);
            vec4 p12 = (texelFetch(sampler2D(curveData, curveSampler), curveLoc, 0) - vec4(renderCoord, renderCoord))
                       * pixelsPerEm.xyxy;
            vec2 p3 = (texelFetch(sampler2D(curveData, curveSampler), ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord)
                      * pixelsPerEm;

            // Curves are sorted by descending max x: once one lies wholly
            // further left than the best distance, the rest of the band does too.
            if (max(max(p12.x, p12.z), p3.x) < -best)
                break;

            // The control points bound the curve, so their box's distance is a
            // lower bound on the curve's.
            vec2 boxLow = min(min(p12.xy, p12.zw), p3);
            vec2 boxHigh = max(max(p12.xy, p12.zw), p3);
            vec2 gap = max(max(boxLow, -boxHigh), vec2(0.0));
            if (dot(gap, gap) >= best * best)
                continue;

            best = min(best, slugCurveDistance(p12.xy, p12.zw, p3));
        }
    }
    return best;
}

#endif // SLUG_FUNCTIONS_GLSL
