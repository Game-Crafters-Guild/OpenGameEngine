// Rasterizes spline ribbon triangles into one ocean cascade field. The including
// program defines OCEAN_RIBBON_FIELD (0 seabed depth, 1 flow, 2 clip, 3 albedo),
// which selects the storage format and how a ribbon combines into the field.
//
// Ribbon triangles extend past the true edge by an apron (see BuildOceanRibbon);
// the signed distance to the true edge is (1 - |cross coordinate|) * width, which
// is negative in the apron. Flow applies only inside the true edge. Clip and
// albedo ramp linearly across the edge over at least two texels, and the depth
// band's bank slope continues through the apron, so bilinear sampling puts each
// boundary on the true edge at every cascade instead of on the texel grid.
#ifndef OCEAN_RIBBON_FIELD
#error "define OCEAN_RIBBON_FIELD before including ocean_spline_raster.glsl"
#endif
#include "Ocean/ocean_cascade_common.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
struct Triangle
{
    vec4 a;
    vec4 b;
    vec4 c;
    vec4 flowA;
    vec4 flowB;
    vec4 flowC;
    vec4 color;
    vec4 values;
    uvec4 meta;
};
layout(std140, set = 0, binding = 0) uniform Params
{
    vec4 layers[7];
    uvec4 lodMeta;
    uvec4 grid;
};
layout(std430, set = 0, binding = 1) readonly buffer Triangles
{
    Triangle triangles[];
};
layout(std430, set = 0, binding = 2) readonly buffer Tiles
{
    uint tiles[];
};
#if OCEAN_RIBBON_FIELD == 0
layout(r16f, set = 0, binding = 3) uniform image2DArray field;
#elif OCEAN_RIBBON_FIELD == 1
layout(rg16f, set = 0, binding = 3) uniform image2DArray field;
#elif OCEAN_RIBBON_FIELD == 2
layout(r8, set = 0, binding = 3) uniform image2DArray field;
#else
layout(rgba8, set = 0, binding = 3) uniform image2DArray field;
#endif
struct RibbonHit
{
    uint Triangle;
    vec3 Weights;
    float Edge;
};

// Coverage across the true edge: 0.5 on it, a linear ramp at least two texels
// wide, never wider than the apron the triangles carry.
float RibbonEdgeCoverage(Triangle t, float edge, float texelSize)
{
    float halfRamp = min(0.5 * OceanCascadeFeather(t.values.z, texelSize), t.flowB.z);
    return clamp(0.5 + edge / max(2.0 * halfRamp, 1e-5), 0.0, 1.0);
}

void ApplyRibbon(RibbonHit hit, float texelSize, inout vec4 value)
{
    Triangle t = triangles[hit.Triangle];
    vec3 w = hit.Weights;
    float edge = hit.Edge;
#if OCEAN_RIBBON_FIELD == 0
    value.r = min(value.r, OceanDepthBandDepth(t.values.y, t.flowA.w, edge, OceanCascadeFeather(t.flowA.z, texelSize)));
#elif OCEAN_RIBBON_FIELD == 1
    if (edge < 0.0)
        return;
    float weight = t.values.z > 1e-5 ? smoothstep(0, t.values.z, edge) : 1;
    value.rg += (t.flowA.xy * w.x + t.flowB.xy * w.y + t.flowC.xy * w.z) * weight;
#elif OCEAN_RIBBON_FIELD == 2
    float weight = RibbonEdgeCoverage(t, edge, texelSize);
    value.r = (t.meta.x & 16u) != 0u ? min(value.r, 1 - weight) : max(value.r, weight);
#else
    float weight = RibbonEdgeCoverage(t, edge, texelSize);
    float alpha = clamp(t.color.a * weight, 0, 1), total = alpha + value.a * (1 - alpha);
    value.rgb = (t.color.rgb * alpha + value.rgb * value.a * (1 - alpha)) / max(total, 1e-5);
    value.a = total;
#endif
}

void main()
{
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (id.x >= int(grid.x) || id.y >= int(grid.x) || id.z >= int(lodMeta.x))
        return;
    uint tile = (gl_WorkGroupID.z * grid.y + gl_WorkGroupID.y) * grid.y + gl_WorkGroupID.x;
    uint begin = tiles[tile * 2], count = tiles[tile * 2 + 1];
    if (count == 0u)
        return;
    float texelSize = layers[id.z].z;
    vec2 xz = layers[id.z].xy + (vec2(id.xy) + 0.5) * texelSize;
    vec4 value = imageLoad(field, id);
    // Tile lists are in ascending triangle order and each ribbon's triangles are
    // contiguous, so one pass keeps the innermost triangle per ribbon: overlapping
    // triangles (shared diagonals, aprons of neighbouring segments) apply once.
    uint ribbon = 0xffffffffu;
    RibbonHit best = RibbonHit(0u, vec3(0), -1e30);
    for (uint i = 0u; i < count; ++i)
    {
        uint index = tiles[begin + i];
        Triangle t = triangles[index];
        if (t.meta.y != ribbon)
        {
            if (best.Edge > -1e29)
                ApplyRibbon(best, texelSize, value);
            ribbon = t.meta.y;
            best.Edge = -1e30;
        }
        vec2 b = t.b.xy - t.a.xy, c = t.c.xy - t.a.xy, p = xz - t.a.xy;
        float det = b.x * c.y - b.y * c.x;
        if (abs(det) < 1e-8)
            continue;
        vec3 w;
        w.y = (p.x * c.y - p.y * c.x) / det;
        w.z = (b.x * p.y - b.y * p.x) / det;
        w.x = 1 - w.y - w.z;
        if (any(lessThan(w, vec3(-1e-6))))
            continue;
        float edge = (1 - abs(dot(vec3(t.a.z, t.b.z, t.c.z), w))) * t.values.x;
        if (edge > best.Edge)
            best = RibbonHit(index, w, edge);
    }
    if (best.Edge > -1e29)
        ApplyRibbon(best, texelSize, value);
    imageStore(field, id, value);
}
