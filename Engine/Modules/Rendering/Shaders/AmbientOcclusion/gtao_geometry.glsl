#ifndef GE_GTAO_GEOMETRY_GLSL
#define GE_GTAO_GEOMETRY_GLSL

#include "Includes/view_params.glsl"

bool GE_GtaoInside(vec2 uv)
{
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
}

vec3 GE_GtaoPosition(vec2 uv, float raw)
{
    vec4 p = ge_invProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, raw, 1.0);
    return p.xyz / p.w;
}

float GE_LinearizeDepth(float raw)
{
    // Projection-based conversion also covers orthographic cameras.
    return GE_GtaoPosition(vec2(0.5), raw).z;
}

float GE_GtaoRawDepth(float z)
{
    vec4 p = ge_proj * vec4(0.0, 0.0, z, 1.0);
    return p.z / p.w;
}

vec2 GE_GtaoDepthUV(sampler2D depth, vec2 uv)
{
    vec2 size = vec2(textureSize(depth, 0));
    return (clamp(floor(uv * size), vec2(0.0), size - 1.0) + 0.5) / size;
}

// The guide stores exact raw depth and a 0..3 index within a 2x2 source
// footprint. Full resolution uses index zero. Retaining the index avoids
// reconstructing a foreground depth at a different (background) pixel centre.
vec3 GE_GtaoGuide(sampler2D guide, ivec2 pix, ivec2 fullSize)
{
    ivec2 size = textureSize(guide, 0);
    pix = clamp(pix, ivec2(0), size - 1);
    vec2 data = texelFetch(guide, pix, 0).rg;
    int scale = all(equal(size, fullSize)) ? 1 : 2;
    int offset = int(data.y);
    ivec2 source = min(pix * scale + ivec2(offset & 1, offset >> 1), fullSize - 1);
    return vec3((vec2(source) + 0.5) / vec2(fullSize), data.x);
}

vec3 GE_GtaoViewVector(vec3 position)
{
    return abs(ge_proj[3][3]) > 0.5 ? vec3(0.0, 0.0, -1.0) : normalize(-position);
}

vec3 GE_GtaoDerivative(sampler2D depth, ivec2 pix, ivec2 axis, vec3 center)
{
    ivec2 size = textureSize(depth, 0);
    ivec2 a = pix - axis;
    ivec2 b = pix + axis;
    bool va = all(greaterThanEqual(a, ivec2(0))) && all(lessThan(a, size));
    bool vb = all(greaterThanEqual(b, ivec2(0))) && all(lessThan(b, size));
    float ra = va ? texelFetch(depth, a, 0).r : 0.0;
    float rb = vb ? texelFetch(depth, b, 0).r : 0.0;
    va = va && ra > 0.0;
    vb = vb && rb > 0.0;
    vec3 da = va ? center - GE_GtaoPosition((vec2(a) + 0.5) / vec2(size), ra) : vec3(0.0);
    vec3 db = vb ? GE_GtaoPosition((vec2(b) + 0.5) / vec2(size), rb) - center : vec3(0.0);
    if (va && vb) return abs(da.z) < abs(db.z) ? da : db;
    if (va) return da;
    if (vb) return db;
    // A one-pixel surface/extent has no usable neighbour on this axis.
    float raw = texelFetch(depth, pix, 0).r;
    return GE_GtaoPosition((vec2(pix + axis) + 0.5) / vec2(size), raw) - center;
}

vec3 GE_GtaoNormal(sampler2D depth, vec2 uv, vec3 position)
{
    ivec2 pix = clamp(ivec2(uv * vec2(textureSize(depth, 0))), ivec2(0), textureSize(depth, 0) - 1);
    vec3 dx = GE_GtaoDerivative(depth, pix, ivec2(1, 0), position);
    vec3 dy = GE_GtaoDerivative(depth, pix, ivec2(0, 1), position);
    vec3 n = cross(dx, dy);
    vec3 v = GE_GtaoViewVector(position);
    n = dot(n, n) > 1e-16 ? normalize(n) : v;
    return dot(n, v) < 0.0 ? -n : n;
}

#endif
