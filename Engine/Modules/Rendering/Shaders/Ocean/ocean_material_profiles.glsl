#ifndef GE_OCEAN_MATERIAL_PROFILES
#define GE_OCEAN_MATERIAL_PROFILES
#ifndef OCEAN_MATERIAL_SET
#define OCEAN_MATERIAL_SET 2
#define OCEAN_MATERIAL_BINDING 30
#endif
struct OceanWaterMaterial
{
    vec4 bounds;
    vec4 points[8];
    float pointCount;
    float metaPad0, metaPad1, metaPad2; // keeps deep on its 16-byte boundary
    vec4 deep;
    vec4 diffuse;
    vec4 grazing;
    vec4 shadow;
    vec4 shallow;
    vec4 subsurface;
    vec4 foam;
    vec4 fog;
    vec4 surface;
    vec4 foamDetail;
    vec4 bubbles;
    vec4 optics;
    uint foamTexture;   // bindless index; 0xffffffff = procedural foam
    uint normalTexture; // bindless index; 0xffffffff = none
    uint entityId;
    int priority;
};
#if !defined(GE_COMPAT_PROFILE)
layout(std430, set = OCEAN_MATERIAL_SET, binding = OCEAN_MATERIAL_BINDING) readonly buffer OceanWaterMaterials
{
    uvec4 oceanMaterialMeta;
    OceanWaterMaterial oceanMaterials[];
};
#endif
bool OceanMaterialContains(OceanWaterMaterial m, vec2 p)
{
    if (any(lessThan(p, m.bounds.xy)) || any(greaterThan(p, m.bounds.zw)))
        return false;
    int count = clamp(int(m.pointCount), 0, 8);
    if (count < 3)
        return false;
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++)
    {
        vec2 a = m.points[i].xy, b = m.points[j].xy, edge = b - a;
        float t = clamp(dot(p - a, edge) / max(dot(edge, edge), 1e-12), 0, 1);
        if (length(p - a - edge * t) < 1e-5)
            return true;
        if ((a.y > p.y) != (b.y > p.y))
            if (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
    }
    return inside;
}
// Width (m) of the band over which a water-body material fades into the water
// around it, centered on the footprint edge.
const float GE_OCEAN_MATERIAL_BLEND_METERS = 4.0;
// The footprint edge is displaced by two incommensurate, domain-warped sine
// waves so a plume of sediment or algae meanders instead of following the
// authored polygon: wavelengths (m) and the largest displacement (m) of each.
// Sines rather than lattice noise: the warp sits in the surface shader's
// material loop, and hashed noise there cost the default ocean about 0.2 ms
// in register pressure even with no water body in view.
const vec2 GE_OCEAN_MATERIAL_EDGE_WAVELENGTHS = vec2(26.0, 9.0);
const vec2 GE_OCEAN_MATERIAL_EDGE_AMPLITUDES = vec2(3.0, 1.0);

// Signed displacement (m) of the footprint edge at p.
float OceanMaterialEdgeWarp(vec2 p)
{
    const float kTau = 6.2831853;
    vec2 phase = kTau * p / GE_OCEAN_MATERIAL_EDGE_WAVELENGTHS.x;
    float broad = sin(phase.x + 1.3 * sin(phase.y * 0.71));
    phase = kTau * p / GE_OCEAN_MATERIAL_EDGE_WAVELENGTHS.y;
    float fine = sin(phase.y * 0.83 + 1.1 * sin(phase.x));
    return broad * GE_OCEAN_MATERIAL_EDGE_AMPLITUDES.x + fine * GE_OCEAN_MATERIAL_EDGE_AMPLITUDES.y;
}

// How much of material m applies at p: 1 deeper than half the blend band inside
// the displaced footprint edge, 0 that far outside it, a smooth ramp across it.
float OceanMaterialWeight(OceanWaterMaterial m, vec2 p)
{
    const float halfBand = 0.5 * GE_OCEAN_MATERIAL_BLEND_METERS;
    const float reach = halfBand + GE_OCEAN_MATERIAL_EDGE_AMPLITUDES.x + GE_OCEAN_MATERIAL_EDGE_AMPLITUDES.y;
    if (any(lessThan(p, m.bounds.xy - reach)) || any(greaterThan(p, m.bounds.zw + reach)))
        return 0.0;
    int count = clamp(int(m.pointCount), 0, 8);
    if (count < 3)
        return 0.0;
    bool inside = false;
    float edgeDistance = 1e30;
    for (int i = 0, j = count - 1; i < count; j = i++)
    {
        vec2 a = m.points[i].xy, b = m.points[j].xy, edge = b - a;
        float t = clamp(dot(p - a, edge) / max(dot(edge, edge), 1e-12), 0, 1);
        edgeDistance = min(edgeDistance, length(p - a - edge * t));
        if ((a.y > p.y) != (b.y > p.y))
            if (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
    }
    float signedDistance = (inside ? edgeDistance : -edgeDistance) + OceanMaterialEdgeWarp(p);
    return smoothstep(-halfBand, halfBand, signedDistance);
}

int OceanFindMaterial(vec2 worldXZ)
{
    int result = -1;
#if !defined(GE_COMPAT_PROFILE)
    for (uint i = 0u; i < oceanMaterialMeta.x; ++i)
        if (OceanMaterialContains(oceanMaterials[i], worldXZ))
            result = int(i);
#endif
    return result;
}
#endif
