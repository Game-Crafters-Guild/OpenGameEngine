// Packed light std430 field list — the single GLSL source for every GLSL
// mirror of the CPU LightUploadNode.cpp::GPULightPacked (144 B, static_assert
// -locked there). Included INSIDE a struct body:
//
//     struct LightPacked {                       // from Shaders/*.comp | *.frag
//     #include "Includes/light_packed_fields.glsl"
//     };
//     struct GE_LightPacked {                    // from Includes/clustered_lighting.glsl
//     #include "light_packed_fields.glsl"
//     };
//
// Consumers keep their own struct NAME (LightPacked in the standalone Forward+
// shaders; GE_LightPacked for the composed adapter path's shared BRDF), but
// share this field list so the layout can never drift between them.
//
// ---------------------------------------------------------------------------
// LANE MAP (9 x vec4 = 144 B; each lane maps 1:1 to a GPULightPacked field)
// ---------------------------------------------------------------------------
//   meta (uvec4)     x=type(0=dir,1=point,2=spot,4=area) y=castsShadows
//                    z=areaShape  w=castsLight
//   posRange         xyz=positionWS         w=range
//   dirIntensity     xyz=directionWS        w=intensity
//   colorAreaWidth   rgb=color              w=areaWidth
//   areaParams       x=areaHeight y=areaRadius z=decay   w=spotCosInner
//   spotParams       x=spotCosOuter | FOG: y=fogContribution z=fogDensityBoost
//                                          w=fogOriginFade
//   areaRight        xyz=area right axis    | FOG: w=fogAnisotropy
//   areaUp           xyz=area up axis       w=falloffMode
//   shadowSlots(ivec4) x=point-shadow atlas slot (-1 = unshadowed) yzw=reserved
//
// FOG LANES (repurposed spares): fog reuses the four otherwise-unused scalar
// slots left over once the light/area fields are packed —
//   spotParams.y = fogContribution, spotParams.z = fogDensityBoost,
//   spotParams.w = fogOriginFade,   areaRight.w   = fogAnisotropy.
// A non-fog consumer simply ignores them; the packer (LightUploadNode.cpp)
// always writes them, so they carry no separate binding.
//
// Field order and types are ABI: any change here must change GPULightPacked in
// LightUploadNode.cpp (and its size static_assert) in the same commit.
    uvec4 meta;          // x=type, y=castsShadows, z=areaShape, w=castsLight
    vec4 posRange;       // xyz=positionWS, w=range
    vec4 dirIntensity;   // xyz=directionWS, w=intensity
    vec4 colorAreaWidth; // rgb=color, w=areaWidth
    vec4 areaParams;     // x=areaHeight, y=areaRadius, z=decay, w=spotCosInner
    vec4 spotParams;     // x=spotCosOuter, y=fogContribution, z=fogDensityBoost, w=fogOriginFade
    vec4 areaRight;      // xyz=area right axis, w=fogAnisotropy
    vec4 areaUp;         // xyz=area up axis, w=falloffMode
    ivec4 shadowSlots;   // x=point-shadow atlas slot (-1 = unshadowed), yzw=reserved
