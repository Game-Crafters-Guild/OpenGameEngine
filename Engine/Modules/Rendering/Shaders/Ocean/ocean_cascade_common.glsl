// Layout math for the camera-snapped ocean sim cascades (foam, seabed depth, flow,
// dynamic waves, wave mask, clip, albedo). Each LOD layer is a square world tile
// centered on the camera and snapped to its own texel grid, so a sim is temporally
// stable as the camera translates. Each cascade has its own layout, uploaded per
// frame and consumed by its sim's compute shader and the surface.
//
// A layout MUST match OceanCascadeLayoutGPU in Ocean/OceanTypes.h byte-for-byte.
// Per LOD layer (one vec4):
//   xy = layer origin (world XZ of texel (0,0))
//   z  = texel world size (meters per texel)
//   w  = lod scale (2^lod)
// The block itself is declared by each consumer at its own set/binding (the
// binding set differs between the compute sim and the surface draw), so this
// include only provides the constants + math, not the buffer declaration.

#ifndef GE_OCEAN_CASCADE_COMMON_GLSL
#define GE_OCEAN_CASCADE_COMMON_GLSL

// Must match kMaxOceanLodCascades in OceanTypes.h.
const int GE_OCEAN_LOD_CASCADES = 7;

// Map a world XZ position into [0,1] cascade UV for the given LOD layer, from
// that layer's origin + texel size. resolution is the per-axis texel count.
vec2 OceanCascadeUV(vec2 worldXZ, vec2 origin, float texelSize, float resolution)
{
    return (worldXZ - origin) / (texelSize * resolution);
}

// World XZ of a texel center for the given LOD layer (inverse of the UV map).
vec2 OceanCascadeTexelToWorld(ivec2 texel, vec2 origin, float texelSize)
{
    return origin + (vec2(texel) + 0.5) * texelSize;
}

// Bilinear sampling reconstructs an edge only when it ramps across at least a
// couple of texels. A sharper transition snaps to the texel grid: the shallow
// tint, shoreline foam and shallow wave attenuation that read the field then
// show stair steps and faceted crests along it. Writers widen an authored
// feather to this floor for the LOD being written, so a band that is narrow
// for a coarse LOD fades there instead of stepping.
const float GE_OCEAN_MIN_FEATHER_TEXELS = 2.0;

float OceanCascadeFeather(float authoredFeather, float texelSize)
{
    return max(authoredFeather, GE_OCEAN_MIN_FEATHER_TEXELS * texelSize);
}

// Returned where a depth band does not reach; above any depth the cascade holds.
const float GE_OCEAN_DEPTH_BAND_OUTSIDE = 1.0e6;

// Depth written by a feathered depth band (a contributor footprint or a ribbon)
// at signed distance `edge` inside its edge. Across the feather the depth ramps
// from the depth at which every shallow term has saturated down to the band
// depth; for one more feather width outside the edge the same bank slope
// continues downward. The field is then linear across the edge, so a bilinear
// read puts the shallow boundary on the true edge. Stepping to the deep-water
// sentinel at the edge instead snaps that boundary to the texel grid. A band no
// shallower than the saturation depth has no bank and ends at its edge. Mirror
// of OceanDepthBandDepth in Ocean/OceanTypes.h.
float OceanDepthBandDepth(float bandDepth, float saturationDepth, float edge, float feather)
{
    float outer = max(saturationDepth, bandDepth);
    feather = max(feather, 1e-3);
    if (edge < -feather || (edge < 0.0 && outer <= bandDepth))
        return GE_OCEAN_DEPTH_BAND_OUTSIDE;
    return outer + (bandDepth - outer) * min(edge / feather, 1.0);
}

#endif // GE_OCEAN_CASCADE_COMMON_GLSL
