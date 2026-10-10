#ifndef GE_TAA_HISTORY_GLSL
#define GE_TAA_HISTORY_GLSL

const float kTaaUniformHistorySpread = 0.05;

// Jitter moves the raster sample along a sloped surface even with a stationary
// camera. Estimate its depth footprint without treating a sky/surface jump as
// a slope. Prefer the smaller one-sided derivative at a depth discontinuity.
float TaaDepthAxisSlope(float d, float a, float b)
{
    float da = abs(a - d);
    float db = abs(b - d);
    bool validA = a > 0.0 && da <= max(a, d) * 0.1;
    bool validB = b > 0.0 && db <= max(b, d) * 0.1;
    if (validA && validB) return min(da, db);
    if (validA) return da;
    if (validB) return db;
    return 0.0;
}

// A min/max interval also contains depths belonging to neither side of a
// silhouette. Match a real sample with a local, slope-aware tolerance instead.
bool TaaDepthFootprintContains(sampler2D depthTexture, ivec2 center, ivec2 extent, float historyDepth)
{
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 p = clamp(center + ivec2(x, y), ivec2(0), extent - 1);
            float d = texelFetch(depthTexture, p, 0).r;
            float tolerance = max(d, historyDepth) * 0.002 + 1e-6;
            if (abs(d - historyDepth) <= tolerance)
                return true;
            if (d > 0.0 && historyDepth > 0.0)
            {
                float l = texelFetch(depthTexture, clamp(p + ivec2(-1, 0), ivec2(0), extent - 1), 0).r;
                float r = texelFetch(depthTexture, clamp(p + ivec2(1, 0), ivec2(0), extent - 1), 0).r;
                float u = texelFetch(depthTexture, clamp(p + ivec2(0, -1), ivec2(0), extent - 1), 0).r;
                float b = texelFetch(depthTexture, clamp(p + ivec2(0, 1), ivec2(0), extent - 1), 0).r;
                tolerance += TaaDepthAxisSlope(d, l, r) + TaaDepthAxisSlope(d, u, b);
            }
            if (abs(d - historyDepth) <= tolerance)
                return true;
        }
    return false;
}

// A grazing facet can disappear entirely between jitter phases. Its old point
// depth is then absent even from a slope-expanded current footprint. Corroborate
// it against nearby history from the same depth layer that still matches the
// current surface. A uniform depth replacement has no such surviving anchor.
bool TaaStaticHistoryMatches(sampler2D depthTexture, ivec2 center, ivec2 extent,
                            sampler2D historyTexture, ivec2 historyCenter, ivec2 historyExtent,
                            float historyDepth)
{
    if (TaaDepthFootprintContains(depthTexture, center, extent, historyDepth))
        return true;
    if (historyDepth <= 0.0)
        return false;
    // This fallback is specifically for disappearing silhouette coverage, not
    // a licence to borrow a neighbor's depth after a flat surface replacement.
    bool touchesSky = false;
    bool touchesSurface = false;
    // Include bilinear reconstruction support around the point-depth gather.
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            float d = texelFetch(depthTexture, clamp(center + ivec2(x, y), ivec2(0), extent - 1), 0).r;
            touchesSky = touchesSky || d == 0.0;
            touchesSurface = touchesSurface || d > 0.0;
        }
    if (!touchesSky || !touchesSurface)
        return false;
    vec2 historyStep = vec2(historyExtent) / vec2(extent);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0) continue;
            ivec2 p = clamp(historyCenter + ivec2(round(vec2(x, y) * historyStep)),
                            ivec2(0), historyExtent - 1);
            float h = texelFetch(historyTexture, p, 0).a;
            // A grazing facet can span more depth than one within-facet
            // derivative; keep the cross-facet layer bound separate (15%).
            if (h > 0.0 && abs(h - historyDepth) <= max(h, historyDepth) * 0.15 &&
                TaaDepthFootprintContains(depthTexture, center, extent, h))
                return true;
        }
    return false;
}

// Compare neighborhoods, not the current phase against one history pixel:
// missed subpixel coverage still overlaps the surrounding history background.
// Disjoint neighborhoods indicate a shading change and must not retain stale
// HDR color merely because the surface depth and camera stayed still.
bool TaaShadingChanged(sampler2D historyTexture, ivec2 center, ivec2 extent,
                       vec3 currentMin, vec3 currentMax, ivec2 radius)
{
    vec3 historyMin = vec3(3.402823e38);
    vec3 historyMax = vec3(0.0);
    for (int y = -radius.y; y <= radius.y; ++y)
        for (int x = -radius.x; x <= radius.x; ++x)
        {
            vec3 h = texelFetch(historyTexture, clamp(center + ivec2(x, y), ivec2(0), extent - 1), 0).rgb;
            historyMin = min(historyMin, h);
            historyMax = max(historyMax, h);
        }
    vec3 tolerance = max(historyMax, currentMax) * 0.002 + vec3(1e-5);
    return any(greaterThan(currentMin, historyMax + tolerance)) ||
           any(lessThan(currentMax, historyMin - tolerance));
}

bool TaaHistoryNeighborhoodUniform(sampler2D historyTexture, ivec2 center, ivec2 extent)
{
    vec3 historyMin = vec3(3.402823e38);
    vec3 historyMax = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec3 h = texelFetch(historyTexture, clamp(center + ivec2(x, y), ivec2(0), extent - 1), 0).rgb;
            historyMin = min(historyMin, h);
            historyMax = max(historyMax, h);
        }
    vec3 spread = historyMax - historyMin;
    return all(lessThanEqual(spread, max(historyMax, vec3(1.0)) * kTaaUniformHistorySpread + vec3(1e-5)));
}

bool TaaSampleOutsideCurrent(vec3 historyRgb, vec3 currentMin, vec3 currentMax)
{
    vec3 tolerance = max(historyRgb, currentMax) * 0.002 + vec3(1e-5);
    return any(greaterThan(historyRgb, currentMax + tolerance)) ||
           any(lessThan(historyRgb, currentMin - tolerance));
}
#endif
